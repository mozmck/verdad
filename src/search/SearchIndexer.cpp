#include "search/SearchIndexer.h"

#include <markupfiltmgr.h>
#include <swmgr.h>
#include <swmodule.h>
#include <versekey.h>

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace verdad {
namespace {

std::string trimCopy(const std::string& text) {
    size_t start = 0;
    while (start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }

    size_t end = text.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }

    return text.substr(start, end - start);
}

std::string lowerCopy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string quoteFtsToken(const std::string& token) {
    std::string escaped;
    escaped.reserve(token.size() + 4);
    for (char c : token) {
        if (c == '"') {
            escaped += "\"\"";
        } else {
            escaped.push_back(c);
        }
    }
    return "\"" + escaped + "\"";
}

std::string normalizeWordToken(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '\'') {
            out.push_back(c);
        }
    }
    return out;
}

std::vector<std::string> tokenizeWords(const std::string& text) {
    std::vector<std::string> tokens;
    std::istringstream ss(text);
    std::string raw;
    while (ss >> raw) {
        std::string tok = normalizeWordToken(raw);
        if (!tok.empty()) tokens.push_back(tok);
    }
    return tokens;
}

std::string normalizeStrongsToken(std::string token) {
    token = trimCopy(token);
    if (token.empty()) return "";
    for (char& c : token) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }
    return token;
}

std::vector<std::string> extractStrongsTokens(const std::string& text) {
    std::vector<std::string> terms;
    std::unordered_set<std::string> seen;

    static const std::regex kTokenRe(R"(([HhGg]?\d+[A-Za-z]?))");
    auto it = std::sregex_iterator(text.begin(), text.end(), kTokenRe);
    auto end = std::sregex_iterator();
    for (; it != end; ++it) {
        std::string tok = normalizeStrongsToken((*it)[1].str());
        if (tok.empty()) continue;

        if (std::isdigit(static_cast<unsigned char>(tok[0]))) {
            std::string gtok = "G" + tok;
            std::string htok = "H" + tok;
            if (seen.insert(gtok).second) terms.push_back(gtok);
            if (seen.insert(htok).second) terms.push_back(htok);
        } else {
            if (seen.insert(tok).second) terms.push_back(tok);
        }
    }

    if (!terms.empty()) return terms;

    std::string fallback = normalizeStrongsToken(text);
    if (!fallback.empty()) terms.push_back(fallback);
    return terms;
}

std::vector<std::string> collectBookNames() {
    std::vector<std::string> books;

    sword::VerseKey vk;
    vk.setAutoNormalize(true);

    for (int testament = 1; testament <= 2; ++testament) {
        vk.setTestament(testament);
        int maxBook = vk.getBookMax();
        for (int book = 1; book <= maxBook; ++book) {
            vk.setBook(book);
            const char* name = vk.getBookName();
            if (name && *name) books.emplace_back(name);
        }
    }

    return books;
}

bool bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    return sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

} // namespace

SearchIndexer::SearchIndexer(const std::string& dbPath)
    : dbPath_(dbPath) {
    int rc = sqlite3_open_v2(
        dbPath_.c_str(), &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "SearchIndexer: unable to open database " << dbPath_
                  << " (" << sqlite3_errmsg(db_) << ")\n";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        return;
    }

    sqlite3_busy_timeout(db_, 5000);
    applyPragmas(db_);
    if (!ensureSchema(db_)) {
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }

    workerThread_ = std::thread(&SearchIndexer::workerLoop, this);
}

SearchIndexer::~SearchIndexer() {
    stopRequested_.store(true);
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        stopWorker_ = true;
    }
    workerCv_.notify_all();

    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void SearchIndexer::applyPragmas(sqlite3* db) {
    if (!db) return;
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA temp_store=MEMORY;", nullptr, nullptr, nullptr);
}

bool SearchIndexer::ensureSchema(sqlite3* db) {
    if (!db) return false;

    const char* sql = R"SQL(
        CREATE TABLE IF NOT EXISTS indexed_modules (
            module_name TEXT PRIMARY KEY,
            indexed_at TEXT DEFAULT (datetime('now'))
        );

        CREATE VIRTUAL TABLE IF NOT EXISTS verse_index USING fts5(
            module_name UNINDEXED,
            key_text UNINDEXED,
            book UNINDEXED,
            chapter UNINDEXED,
            verse UNINDEXED,
            plain_text,
            strongs_text,
            tokenize='unicode61 remove_diacritics 2'
        );
    )SQL";

    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "SearchIndexer schema error: "
                  << (err ? err : "unknown") << "\n";
        if (err) sqlite3_free(err);
        return false;
    }
    if (err) sqlite3_free(err);
    return true;
}

void SearchIndexer::queueModuleIndex(const std::string& moduleName) {
    std::string normalized = trimCopy(moduleName);
    if (!db_ || normalized.empty()) return;
    if (isModuleIndexed(normalized)) return;

    std::lock_guard<std::mutex> lock(workerMutex_);
    if (pendingSet_.insert(normalized).second) {
        pendingModules_.push_back(normalized);
        workerCv_.notify_one();
    }
}

void SearchIndexer::queueModuleIndex(const std::vector<std::string>& moduleNames) {
    for (const auto& module : moduleNames) {
        queueModuleIndex(module);
    }
}

bool SearchIndexer::isModuleIndexed(const std::string& moduleName) const {
    if (!db_ || moduleName.empty()) return false;

    std::lock_guard<std::mutex> lock(dbMutex_);

    const char* sql =
        "SELECT 1 FROM indexed_modules WHERE module_name = ? LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    bindText(stmt, 1, moduleName);
    bool indexed = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return indexed;
}

int SearchIndexer::moduleIndexProgress(const std::string& moduleName) const {
    std::string normalized = trimCopy(moduleName);
    if (normalized.empty()) return -1;

    if (isModuleIndexed(normalized)) return 100;

    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        if (activeModule_ == normalized && indexing_.load()) {
            return std::clamp(activeProgress_, 0, 100);
        }
    }

    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        if (pendingSet_.count(normalized) > 0) {
            return 0;
        }
    }

    return -1;
}

bool SearchIndexer::activeIndexingTask(std::string& moduleName, int& percent) const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    if (!indexing_.load() || activeModule_.empty()) return false;

    moduleName = activeModule_;
    percent = std::clamp(activeProgress_, 0, 100);
    return true;
}

std::string SearchIndexer::buildWordFtsQuery(const std::string& query, bool exactPhrase) {
    std::string text = trimCopy(query);
    if (text.empty()) return "";

    if (exactPhrase) {
        return "{plain_text}:" + quoteFtsToken(text);
    }

    std::vector<std::string> tokens = tokenizeWords(text);
    if (tokens.empty()) return "";

    std::ostringstream out;
    out << "{plain_text}:(";
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i) out << " AND ";
        out << quoteFtsToken(tokens[i]);
    }
    out << ")";
    return out.str();
}

std::string SearchIndexer::buildStrongsFtsQuery(const std::string& query) {
    std::string text = trimCopy(query);
    if (text.empty()) return "";

    std::string lower = lowerCopy(text);
    if (lower.rfind("strong's:", 0) == 0) {
        text = trimCopy(text.substr(9));
    } else if (lower.rfind("strongs:", 0) == 0) {
        text = trimCopy(text.substr(8));
    } else if (lower.rfind("lemma:", 0) == 0) {
        text = trimCopy(text.substr(6));
    }

    std::vector<std::string> terms = extractStrongsTokens(text);
    if (terms.empty()) return "";

    std::ostringstream out;
    out << "{strongs_text}:(";
    for (size_t i = 0; i < terms.size(); ++i) {
        if (i) out << " OR ";
        out << quoteFtsToken(terms[i]);
    }
    out << ")";
    return out.str();
}

std::vector<SearchResult> SearchIndexer::searchWord(
    const std::string& moduleName,
    const std::string& query,
    bool exactPhrase,
    int maxResults) const {

    std::vector<SearchResult> results;
    if (!db_ || moduleName.empty() || query.empty()) return results;

    std::string ftsQuery = buildWordFtsQuery(query, exactPhrase);
    if (ftsQuery.empty()) return results;

    std::lock_guard<std::mutex> lock(dbMutex_);

    const char* sql =
        "SELECT module_name, key_text, "
        "snippet(verse_index, 5, '', '', ' ... ', 18) "
        "FROM verse_index "
        "WHERE verse_index MATCH ? AND module_name = ? "
        "ORDER BY bm25(verse_index) "
        "LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }

    bindText(stmt, 1, ftsQuery);
    bindText(stmt, 2, moduleName);
    sqlite3_bind_int(stmt, 3, std::max(1, maxResults));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SearchResult result;
        const char* module = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* snippet = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        result.module = module ? module : moduleName;
        result.key = key ? key : "";
        result.text = snippet ? snippet : "";
        results.push_back(std::move(result));
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<SearchResult> SearchIndexer::searchStrongs(
    const std::string& moduleName,
    const std::string& strongsQuery,
    int maxResults) const {

    std::vector<SearchResult> results;
    if (!db_ || moduleName.empty() || strongsQuery.empty()) return results;

    std::string ftsQuery = buildStrongsFtsQuery(strongsQuery);
    if (ftsQuery.empty()) return results;

    std::lock_guard<std::mutex> lock(dbMutex_);

    const char* sql =
        "SELECT module_name, key_text, "
        "snippet(verse_index, 5, '', '', ' ... ', 18) "
        "FROM verse_index "
        "WHERE verse_index MATCH ? AND module_name = ? "
        "ORDER BY bm25(verse_index) "
        "LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }

    bindText(stmt, 1, ftsQuery);
    bindText(stmt, 2, moduleName);
    sqlite3_bind_int(stmt, 3, std::max(1, maxResults));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SearchResult result;
        const char* module = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* snippet = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        result.module = module ? module : moduleName;
        result.key = key ? key : "";
        result.text = snippet ? snippet : "";
        results.push_back(std::move(result));
    }

    sqlite3_finalize(stmt);
    return results;
}

void SearchIndexer::workerLoop() {
    while (true) {
        std::string module;
        {
            std::unique_lock<std::mutex> lock(workerMutex_);
            workerCv_.wait(lock, [this]() {
                return stopWorker_ || !pendingModules_.empty();
            });
            if (stopWorker_) break;
            module = pendingModules_.front();
            pendingModules_.pop_front();
            pendingSet_.erase(module);
        }

        if (module.empty()) continue;
        if (isModuleIndexed(module)) continue;

        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            activeModule_ = module;
            activeProgress_ = 0;
        }
        indexing_.store(true);
        indexModuleNow(module);
        indexing_.store(false);
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            activeModule_.clear();
            activeProgress_ = 0;
        }
    }
}

void SearchIndexer::indexModuleNow(const std::string& moduleName) {
    auto setProgress = [this, &moduleName](int percent) {
        percent = std::clamp(percent, 0, 100);
        std::lock_guard<std::mutex> lock(statusMutex_);
        if (activeModule_ == moduleName) {
            activeProgress_ = percent;
        }
    };

    setProgress(0);

    sqlite3* writeDb = nullptr;
    int rc = sqlite3_open_v2(
        dbPath_.c_str(), &writeDb,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (rc != SQLITE_OK || !writeDb) {
        if (writeDb) sqlite3_close(writeDb);
        return;
    }

    sqlite3_busy_timeout(writeDb, 5000);
    applyPragmas(writeDb);
    if (!ensureSchema(writeDb)) {
        sqlite3_close(writeDb);
        return;
    }

    auto mgr = std::unique_ptr<sword::SWMgr>(new sword::SWMgr(
        nullptr, nullptr, true,
        new sword::MarkupFilterMgr(sword::FMT_XHTML, sword::ENC_UTF8)));
    sword::SWModule* mod = mgr ? mgr->getModule(moduleName.c_str()) : nullptr;
    if (!mod) {
        sqlite3_close(writeDb);
        return;
    }

    mgr->setGlobalOption("Strong's Numbers", "On");
    mgr->setGlobalOption("Morphological Tags", "On");
    mgr->setGlobalOption("Footnotes", "On");
    mgr->setGlobalOption("Cross-references", "On");
    mgr->setGlobalOption("Headings", "On");

    std::vector<std::string> books = collectBookNames();
    if (books.empty()) {
        sqlite3_close(writeDb);
        return;
    }

    int totalVerses = 0;
    sword::VerseKey countVk;
    countVk.setAutoNormalize(true);
    for (const auto& book : books) {
        std::string chapterRef = book + " 1:1";
        countVk.setText(chapterRef.c_str());
        if (countVk.popError()) continue;

        int maxChapter = countVk.getChapterMax();
        for (int chapter = 1; chapter <= maxChapter; ++chapter) {
            std::string verseRef = book + " " + std::to_string(chapter) + ":1";
            countVk.setText(verseRef.c_str());
            if (countVk.popError()) continue;
            totalVerses += std::max(1, countVk.getVerseMax());
        }
    }
    if (totalVerses <= 0) totalVerses = 1;
    int processedVerses = 0;
    int lastProgress = -1;
    auto bumpProgress = [&]() {
        int percent = (processedVerses * 100) / totalVerses;
        if (percent != lastProgress) {
            lastProgress = percent;
            setProgress(percent);
        }
    };

    sqlite3_exec(writeDb, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, nullptr);

    sqlite3_stmt* deleteVerses = nullptr;
    sqlite3_stmt* deleteIndexed = nullptr;
    sqlite3_stmt* insertVerse = nullptr;
    sqlite3_stmt* markIndexed = nullptr;

    sqlite3_prepare_v2(
        writeDb,
        "DELETE FROM verse_index WHERE module_name = ?",
        -1, &deleteVerses, nullptr);
    sqlite3_prepare_v2(
        writeDb,
        "DELETE FROM indexed_modules WHERE module_name = ?",
        -1, &deleteIndexed, nullptr);
    sqlite3_prepare_v2(
        writeDb,
        "INSERT INTO verse_index(module_name, key_text, book, chapter, verse, plain_text, strongs_text) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)",
        -1, &insertVerse, nullptr);
    sqlite3_prepare_v2(
        writeDb,
        "INSERT OR REPLACE INTO indexed_modules(module_name, indexed_at) "
        "VALUES (?, datetime('now'))",
        -1, &markIndexed, nullptr);

    if (!deleteVerses || !deleteIndexed || !insertVerse || !markIndexed) {
        if (deleteVerses) sqlite3_finalize(deleteVerses);
        if (deleteIndexed) sqlite3_finalize(deleteIndexed);
        if (insertVerse) sqlite3_finalize(insertVerse);
        if (markIndexed) sqlite3_finalize(markIndexed);
        sqlite3_exec(writeDb, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(writeDb);
        return;
    }

    bindText(deleteVerses, 1, moduleName);
    sqlite3_step(deleteVerses);
    sqlite3_reset(deleteVerses);

    bindText(deleteIndexed, 1, moduleName);
    sqlite3_step(deleteIndexed);
    sqlite3_reset(deleteIndexed);

    bool cancelled = false;
    sword::VerseKey vk;
    vk.setAutoNormalize(true);

    for (const auto& book : books) {
        if (stopRequested_.load()) {
            cancelled = true;
            break;
        }

        std::string chapterRef = book + " 1:1";
        vk.setText(chapterRef.c_str());
        if (vk.popError()) continue;

        int maxChapter = vk.getChapterMax();
        for (int chapter = 1; chapter <= maxChapter; ++chapter) {
            if (stopRequested_.load()) {
                cancelled = true;
                break;
            }

            std::string verseRef = book + " " + std::to_string(chapter) + ":1";
            vk.setText(verseRef.c_str());
            if (vk.popError()) continue;

            int maxVerse = vk.getVerseMax();
            for (int verse = 1; verse <= maxVerse; ++verse) {
                if (stopRequested_.load()) {
                    cancelled = true;
                    break;
                }

                ++processedVerses;
                if ((processedVerses % 128) == 0) bumpProgress();

                vk.setVerse(verse);
                mod->setKey(vk);
                if (mod->popError()) continue;

                std::string plain = std::string(mod->stripText());
                std::string xhtml = std::string(mod->renderText().c_str());

                if (plain.empty() && xhtml.empty()) continue;

                std::string keyText = std::string(vk.getBookName()) + " " +
                                      std::to_string(chapter) + ":" +
                                      std::to_string(verse);
                std::string bookName = vk.getBookName() ? vk.getBookName() : "";

                sqlite3_reset(insertVerse);
                sqlite3_clear_bindings(insertVerse);
                bindText(insertVerse, 1, moduleName);
                bindText(insertVerse, 2, keyText);
                bindText(insertVerse, 3, bookName);
                sqlite3_bind_int(insertVerse, 4, chapter);
                sqlite3_bind_int(insertVerse, 5, verse);
                bindText(insertVerse, 6, plain);
                bindText(insertVerse, 7, xhtml);
                sqlite3_step(insertVerse);
            }
            if (cancelled) break;
        }
        if (cancelled) break;
    }

    if (!cancelled) {
        sqlite3_reset(markIndexed);
        sqlite3_clear_bindings(markIndexed);
        bindText(markIndexed, 1, moduleName);
        sqlite3_step(markIndexed);
        sqlite3_exec(writeDb, "COMMIT;", nullptr, nullptr, nullptr);
        setProgress(100);
    } else {
        sqlite3_exec(writeDb, "ROLLBACK;", nullptr, nullptr, nullptr);
    }

    sqlite3_finalize(deleteVerses);
    sqlite3_finalize(deleteIndexed);
    sqlite3_finalize(insertVerse);
    sqlite3_finalize(markIndexed);

    sqlite3_close(writeDb);
}

} // namespace verdad
