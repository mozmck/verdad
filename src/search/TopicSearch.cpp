#include "search/TopicSearch.h"

#include "search/SmartSearch.h"

#include <listkey.h>
#include <sqlite3.h>
#include <versekey.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace verdad {
namespace {

namespace fs = std::filesystem;
constexpr std::uint64_t kMaxTopicDatabaseBytes = 15ULL * 1024ULL * 1024ULL;
constexpr int kTopicSchemaVersion = 1;

std::string trimCopy(const std::string& text) {
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string normalizeTopic(std::string text) {
    text = smart_search::stripDiacritics(text);
    std::string result;
    result.reserve(text.size());
    bool spacing = false;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c >= 0x80) {
            if (spacing && !result.empty()) result.push_back(' ');
            result.push_back(static_cast<char>(std::tolower(c)));
            spacing = false;
        } else {
            spacing = true;
        }
    }
    return trimCopy(result);
}

bool bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    return sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

std::string quoteFtsToken(const std::string& token) {
    std::string escaped;
    for (char c : token) escaped += c == '"' ? "\"\"" : std::string(1, c);
    return "\"" + escaped + "\"";
}

std::string topicFtsQuery(const std::string& query) {
    const std::vector<std::string> terms = smart_search::queryTerms(query);
    std::ostringstream out;
    bool first = true;
    for (const auto& raw : terms) {
        std::string term = normalizeTopic(raw);
        if (term.empty()) continue;
        if (!first) out << " AND ";
        first = false;
        out << quoteFtsToken(term);
    }
    return first ? std::string() : out.str();
}

std::string osisPartToHuman(const std::string& raw) {
    std::vector<std::string> parts;
    std::istringstream in(raw);
    std::string part;
    while (std::getline(in, part, '.')) parts.push_back(part);
    if (parts.size() < 3) return raw;
    return parts[0] + " " + parts[1] + ":" + parts[2];
}

std::string osisRangeToHuman(const std::string& raw) {
    size_t dash = raw.find('-');
    if (dash == std::string::npos) return osisPartToHuman(raw);
    return osisPartToHuman(raw.substr(0, dash)) + "-" +
           osisPartToHuman(raw.substr(dash + 1));
}

std::vector<std::string> expandOsisReference(const std::string& raw) {
    std::vector<std::string> references;
    sword::VerseKey parser;
    sword::ListKey list = parser.parseVerseList(osisRangeToHuman(raw).c_str(),
                                                 "Genesis 1:1", true);
    for (list = sword::TOP; !list.popError() && references.size() < 2000; list++) {
        std::string reference = trimCopy(list.getText());
        if (!reference.empty()) references.push_back(std::move(reference));
    }
    return references;
}

bool ensureTopicSchema(sqlite3* db) {
    if (!db) return false;
    const char* schema = R"SQL(
        CREATE TABLE IF NOT EXISTS metadata (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS topics (
            topic_id INTEGER PRIMARY KEY,
            title TEXT NOT NULL,
            normalized_title TEXT NOT NULL UNIQUE,
            normalized_redirect TEXT NOT NULL DEFAULT ''
        );
        CREATE VIRTUAL TABLE IF NOT EXISTS topic_index USING fts5(
            title,
            description,
            content='',
            tokenize='unicode61 remove_diacritics 2'
        );
        CREATE TABLE IF NOT EXISTS topic_references (
            topic_id INTEGER NOT NULL,
            position INTEGER NOT NULL,
            reference TEXT NOT NULL,
            PRIMARY KEY(topic_id, reference)
        );
        CREATE INDEX IF NOT EXISTS idx_topic_references_topic
            ON topic_references(topic_id, position);
    )SQL";
    if (sqlite3_exec(db, schema, nullptr, nullptr, nullptr) != SQLITE_OK) return false;
    return sqlite3_exec(db, "PRAGMA user_version=1;", nullptr, nullptr, nullptr) == SQLITE_OK;
}

std::vector<std::pair<sqlite3_int64, bool>> matchingTopics(
    sqlite3* db,
    const std::string& query) {
    std::vector<std::pair<sqlite3_int64, bool>> topics;
    const std::string normalized = normalizeTopic(query);
    sqlite3_stmt* exact = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT topic_id FROM topics WHERE normalized_title = ? LIMIT 1",
            -1, &exact, nullptr) == SQLITE_OK) {
        bindText(exact, 1, normalized);
        if (sqlite3_step(exact) == SQLITE_ROW) {
            topics.emplace_back(sqlite3_column_int64(exact, 0), true);
        }
    }
    if (exact) sqlite3_finalize(exact);
    if (!topics.empty()) return topics;

    const std::string fts = topicFtsQuery(query);
    if (fts.empty()) return topics;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT rowid FROM topic_index WHERE topic_index MATCH ? "
            "ORDER BY bm25(topic_index, 5.0, 1.0) LIMIT 20",
            -1, &stmt, nullptr) == SQLITE_OK) {
        bindText(stmt, 1, fts);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            topics.emplace_back(sqlite3_column_int64(stmt, 0), false);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    stmt = nullptr;
    return topics;
}

sqlite3_int64 resolveRedirect(sqlite3* db, sqlite3_int64 topicId, bool& redirected) {
    std::unordered_set<sqlite3_int64> seen;
    for (int depth = 0; depth < 4 && seen.insert(topicId).second; ++depth) {
        sqlite3_stmt* stmt = nullptr;
        std::string redirect;
        if (sqlite3_prepare_v2(db,
                "SELECT normalized_redirect FROM topics WHERE topic_id = ?",
                -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, topicId);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                redirect = text ? text : "";
            }
        }
        if (stmt) sqlite3_finalize(stmt);
        if (redirect.empty()) return topicId;

        sqlite3_int64 target = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT topic_id FROM topics WHERE normalized_title = ? LIMIT 1",
                -1, &stmt, nullptr) == SQLITE_OK) {
            bindText(stmt, 1, redirect);
            if (sqlite3_step(stmt) == SQLITE_ROW) target = sqlite3_column_int64(stmt, 0);
        }
        if (stmt) sqlite3_finalize(stmt);
        if (!target) return topicId;
        redirected = true;
        topicId = target;
    }
    return topicId;
}

} // namespace

TopicSearchProvider::TopicSearchProvider(std::string dbPath)
    : dbPath_(std::move(dbPath)) {}

bool TopicSearchProvider::readyForSignature(const std::string& moduleSignature) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath_.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK || !db) {
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_stmt* stmt = nullptr;
    bool ready = false;
    if (sqlite3_prepare_v2(db,
            "SELECT value FROM metadata WHERE key='module_signature'",
            -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        ready = value && moduleSignature == value;
    }
    if (stmt) sqlite3_finalize(stmt);
    stmt = nullptr;
    if (ready && sqlite3_prepare_v2(db,
            "SELECT 1 FROM topic_references LIMIT 1", -1, &stmt, nullptr) == SQLITE_OK) {
        ready = sqlite3_step(stmt) == SQLITE_ROW;
    } else if (ready) {
        ready = false;
    }
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(db);
    return ready;
}

bool TopicSearchProvider::rebuild(
    const std::string& moduleSignature,
    const std::vector<TopicDocument>& documents,
    std::string& errorOut) {
    std::lock_guard<std::mutex> lock(mutex_);
    fs::path target(dbPath_);
    fs::path partial = target;
    partial += ".part";
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    fs::remove(partial, ec);
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(partial.string().c_str(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK || !db) {
        if (db) sqlite3_close(db);
        errorOut = "Unable to create the Bible topic index.";
        return false;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=OFF;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous=OFF;", nullptr, nullptr, nullptr);
    if (!ensureTopicSchema(db) ||
        sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        fs::remove(partial, ec);
        errorOut = "Unable to initialize the Bible topic index schema.";
        return false;
    }

    sqlite3_stmt* insertTopic = nullptr;
    sqlite3_stmt* selectTopic = nullptr;
    sqlite3_stmt* insertFts = nullptr;
    sqlite3_stmt* insertRef = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO topics(title, normalized_title, normalized_redirect) VALUES(?, ?, ?)",
        -1, &insertTopic, nullptr);
    sqlite3_prepare_v2(db,
        "SELECT topic_id FROM topics WHERE normalized_title = ? LIMIT 1",
        -1, &selectTopic, nullptr);
    sqlite3_prepare_v2(db,
        "INSERT INTO topic_index(rowid, title, description) VALUES(?, ?, ?)",
        -1, &insertFts, nullptr);
    sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO topic_references(topic_id, position, reference) VALUES(?, ?, ?)",
        -1, &insertRef, nullptr);
    bool ok = insertTopic && selectTopic && insertFts && insertRef;
    for (const auto& document : documents) {
        if (!ok || document.title.empty()) continue;
        const std::string normalizedTitle = normalizeTopic(document.title);
        if (normalizedTitle.empty()) continue;
        bindText(insertTopic, 1, document.title);
        bindText(insertTopic, 2, normalizedTitle);
        bindText(insertTopic, 3, normalizeTopic(document.redirectTitle));
        ok = sqlite3_step(insertTopic) == SQLITE_DONE;
        const bool inserted = ok && sqlite3_changes(db) > 0;
        sqlite3_reset(insertTopic);
        sqlite3_clear_bindings(insertTopic);
        if (!ok) break;

        bindText(selectTopic, 1, normalizedTitle);
        const sqlite3_int64 topicId = sqlite3_step(selectTopic) == SQLITE_ROW
            ? sqlite3_column_int64(selectTopic, 0)
            : 0;
        sqlite3_reset(selectTopic);
        sqlite3_clear_bindings(selectTopic);
        if (!topicId) continue;
        if (inserted) {
            sqlite3_bind_int64(insertFts, 1, topicId);
            bindText(insertFts, 2, document.title);
            bindText(insertFts, 3, document.description);
            ok = sqlite3_step(insertFts) == SQLITE_DONE;
            sqlite3_reset(insertFts);
            sqlite3_clear_bindings(insertFts);
        }
        for (size_t i = 0; ok && i < document.references.size(); ++i) {
            sqlite3_bind_int64(insertRef, 1, topicId);
            sqlite3_bind_int(insertRef, 2, static_cast<int>(i));
            bindText(insertRef, 3, document.references[i]);
            ok = sqlite3_step(insertRef) == SQLITE_DONE;
            sqlite3_reset(insertRef);
            sqlite3_clear_bindings(insertRef);
        }
    }
    if (ok) {
        sqlite3_stmt* metadata = nullptr;
        if (sqlite3_prepare_v2(db,
                "INSERT OR REPLACE INTO metadata(key, value) VALUES('module_signature', ?)",
                -1, &metadata, nullptr) == SQLITE_OK) {
            bindText(metadata, 1, moduleSignature);
            ok = sqlite3_step(metadata) == SQLITE_DONE;
        } else {
            ok = false;
        }
        if (metadata) sqlite3_finalize(metadata);
    }
    if (insertTopic) sqlite3_finalize(insertTopic);
    if (selectTopic) sqlite3_finalize(selectTopic);
    if (insertFts) sqlite3_finalize(insertFts);
    if (insertRef) sqlite3_finalize(insertRef);
    sqlite3_exec(db, ok ? "COMMIT;" : "ROLLBACK;", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    if (!ok || !fs::exists(partial, ec) || fs::file_size(partial, ec) > kMaxTopicDatabaseBytes) {
        fs::remove(partial, ec);
        errorOut = ok ? "The Bible topic index exceeded 15 MiB." :
                        "Writing the Bible topic index failed.";
        return false;
    }
    fs::path backup = target;
    backup += ".backup";
    fs::remove(backup, ec);
    ec.clear();
    const bool hadActiveIndex = fs::exists(target, ec) && !ec;
    if (hadActiveIndex) {
        fs::rename(target, backup, ec);
        if (ec) {
            fs::remove(partial, ec);
            errorOut = "Unable to stage the existing Bible topic index.";
            return false;
        }
    }
    ec.clear();
    fs::rename(partial, target, ec);
    if (ec) {
        std::error_code restoreError;
        if (hadActiveIndex) fs::rename(backup, target, restoreError);
        fs::remove(partial, restoreError);
        errorOut = "Unable to activate the Bible topic index.";
        return false;
    }
    fs::remove(backup, ec);
    return true;
}

std::vector<TopicHit> TopicSearchProvider::search(
    const std::string& query,
    size_t maxResults) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath_.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK || !db) {
        if (db) sqlite3_close(db);
        return {};
    }
    std::vector<TopicHit> hits;
    std::unordered_set<std::string> seenReferences;
    for (auto [topicId, exact] : matchingTopics(db, query)) {
        bool redirected = false;
        topicId = resolveRedirect(db, topicId, redirected);
        std::string title;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db,
                "SELECT title FROM topics WHERE topic_id = ?",
                -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, topicId);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                title = value ? value : "";
            }
        }
        if (stmt) sqlite3_finalize(stmt);
        if (sqlite3_prepare_v2(db,
                "SELECT reference FROM topic_references WHERE topic_id = ? ORDER BY position",
                -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, topicId);
            while ((maxResults == 0 || hits.size() < maxResults) &&
                   sqlite3_step(stmt) == SQLITE_ROW) {
                const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                std::string reference = value ? value : "";
                if (!reference.empty() && seenReferences.insert(reference).second) {
                    hits.push_back({std::move(reference), title, exact && !redirected});
                }
            }
        }
        if (stmt) sqlite3_finalize(stmt);
        if (maxResults > 0 && hits.size() >= maxResults) break;
    }
    sqlite3_close(db);
    return hits;
}

std::uint64_t TopicSearchProvider::databaseBytes() const {
    std::error_code ec;
    return fs::exists(dbPath_, ec) && !ec ? fs::file_size(dbPath_, ec) : 0;
}

TopicDocument TopicSearchProvider::documentFromSwordEntry(
    const std::string& title,
    const std::string& description,
    const std::string& rawEntry) {
    TopicDocument document;
    document.title = title;
    document.description = description;
    static const std::regex osisPattern(R"(osisRef\s*=\s*["']([^"']+)["'])",
                                        std::regex::icase);
    std::unordered_set<std::string> seen;
    for (std::sregex_iterator it(rawEntry.begin(), rawEntry.end(), osisPattern), end;
         it != end; ++it) {
        std::istringstream values((*it)[1].str());
        std::string value;
        while (values >> value) {
            for (auto reference : expandOsisReference(value)) {
                if (seen.insert(reference).second) {
                    document.references.push_back(std::move(reference));
                }
            }
        }
    }
    if (document.references.empty()) {
        static const std::regex redirectPattern(
            R"(target\s*=\s*["']Nave:([^"']+)["'])", std::regex::icase);
        std::smatch match;
        if (std::regex_search(rawEntry, match, redirectPattern)) {
            document.redirectTitle = trimCopy(match[1].str());
        }
    }
    return document;
}

} // namespace verdad
