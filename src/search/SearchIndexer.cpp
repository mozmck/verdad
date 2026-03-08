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
    auto isAsciiWordChar = [](unsigned char c) -> bool {
        return std::isalnum(c) || c == '\'' || c == '-';
    };

    size_t start = 0;
    size_t end = raw.size();
    while (start < end) {
        unsigned char uc = static_cast<unsigned char>(raw[start]);
        if (uc >= 0x80 || isAsciiWordChar(uc)) break;
        ++start;
    }
    while (end > start) {
        unsigned char uc = static_cast<unsigned char>(raw[end - 1]);
        if (uc >= 0x80 || isAsciiWordChar(uc)) break;
        --end;
    }

    return raw.substr(start, end - start);
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
    auto addTerm = [&](const std::string& term) {
        std::string t = trimCopy(term);
        if (t.empty()) return;
        if (seen.insert(t).second) terms.push_back(t);
    };
    auto addNumericForms = [&](const std::string& digitsRaw,
                               const std::string& suffix) {
        std::string digits = digitsRaw;
        if (digits.empty()) return;

        addTerm(digits + suffix);

        size_t nz = digits.find_first_not_of('0');
        std::string baseDigits = (nz == std::string::npos) ? "0" : digits.substr(nz);
        addTerm(baseDigits + suffix);

        for (int width : {4, 5}) {
            if (static_cast<int>(baseDigits.size()) <= width) {
                addTerm(std::string(width - baseDigits.size(), '0') +
                        baseDigits + suffix);
            }
        }
    };

    static const std::regex kTokenRe(R"(([HhGg]?\d+[A-Za-z]?))");
    auto it = std::sregex_iterator(text.begin(), text.end(), kTokenRe);
    auto end = std::sregex_iterator();
    for (; it != end; ++it) {
        std::string tok = normalizeStrongsToken((*it)[1].str());
        if (tok.empty()) continue;

        char prefix = 0;
        size_t pos = 0;
        if (std::isalpha(static_cast<unsigned char>(tok[0]))) {
            prefix = tok[0];
            pos = 1;
        }

        size_t digitStart = pos;
        while (pos < tok.size() &&
               std::isdigit(static_cast<unsigned char>(tok[pos]))) {
            ++pos;
        }
        if (digitStart == pos) {
            addTerm(tok);
            continue;
        }

        std::string digits = tok.substr(digitStart, pos - digitStart);
        std::string suffix = tok.substr(pos);

        if (prefix == 'H' || prefix == 'G') {
            addTerm(std::string(1, prefix) + digits + suffix);
            addNumericForms(digits, suffix);
            // Some indexes include prefixed zero-padded forms.
            size_t nz = digits.find_first_not_of('0');
            std::string baseDigits = (nz == std::string::npos) ? "0" : digits.substr(nz);
            for (int width : {4, 5}) {
                if (static_cast<int>(baseDigits.size()) <= width) {
                    addTerm(std::string(1, prefix) +
                            std::string(width - baseDigits.size(), '0') +
                            baseDigits + suffix);
                }
            }
        } else {
            addNumericForms(digits, suffix);
            addTerm("H" + digits + suffix);
            addTerm("G" + digits + suffix);
        }
    }

    if (!terms.empty()) return terms;

    std::string fallback = normalizeStrongsToken(text);
    if (!fallback.empty()) addTerm(fallback);
    return terms;
}

enum class StrongsLanguageHint {
    Any,
    Greek,
    Hebrew
};

StrongsLanguageHint detectStrongsLanguageHint(const std::string& text) {
    static const std::regex kTokenRe(R"(([HhGg]?\d+[A-Za-z]?))");
    auto it = std::sregex_iterator(text.begin(), text.end(), kTokenRe);
    auto end = std::sregex_iterator();

    bool sawGreek = false;
    bool sawHebrew = false;
    for (; it != end; ++it) {
        std::string tok = normalizeStrongsToken((*it)[1].str());
        if (tok.empty()) continue;
        if (!std::isalpha(static_cast<unsigned char>(tok[0]))) continue;
        if (tok[0] == 'G') sawGreek = true;
        if (tok[0] == 'H') sawHebrew = true;
    }

    if (sawGreek && !sawHebrew) return StrongsLanguageHint::Greek;
    if (sawHebrew && !sawGreek) return StrongsLanguageHint::Hebrew;
    return StrongsLanguageHint::Any;
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

bool isWordByte(unsigned char c) {
    return std::isalnum(c) || c == '\'' || c == '-' || c >= 0x80;
}

std::string decodeHtmlEntities(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out.push_back(text[i]);
            continue;
        }

        if (text.compare(i, 5, "&amp;") == 0) {
            out.push_back('&');
            i += 4;
        } else if (text.compare(i, 4, "&lt;") == 0) {
            out.push_back('<');
            i += 3;
        } else if (text.compare(i, 4, "&gt;") == 0) {
            out.push_back('>');
            i += 3;
        } else if (text.compare(i, 6, "&quot;") == 0) {
            out.push_back('"');
            i += 5;
        } else if (text.compare(i, 6, "&apos;") == 0) {
            out.push_back('\'');
            i += 5;
        } else if (text.compare(i, 6, "&nbsp;") == 0) {
            out.push_back(' ');
            i += 5;
        } else {
            out.push_back(text[i]);
        }
    }

    return out;
}

std::string stripTagsSimple(const std::string& html) {
    std::string out;
    out.reserve(html.size());
    bool inTag = false;
    for (char c : html) {
        if (c == '<') {
            inTag = true;
            continue;
        }
        if (c == '>') {
            inTag = false;
            continue;
        }
        if (!inTag) out.push_back(c);
    }
    return decodeHtmlEntities(out);
}

bool extractTagAttribute(const std::string& tag,
                         const std::string& name,
                         std::string& valueOut) {
    std::string lowerTag = lowerCopy(tag);
    std::string needle = lowerCopy(name) + "=";
    size_t pos = lowerTag.find(needle);
    if (pos == std::string::npos) return false;

    size_t valueStart = pos + needle.size();
    if (valueStart >= tag.size()) return false;

    char quote = tag[valueStart];
    if (quote == '"' || quote == '\'') {
        ++valueStart;
        size_t end = tag.find(quote, valueStart);
        if (end == std::string::npos) return false;
        valueOut = tag.substr(valueStart, end - valueStart);
        return true;
    }

    size_t end = valueStart;
    while (end < tag.size() &&
           !std::isspace(static_cast<unsigned char>(tag[end])) &&
           tag[end] != '>') {
        ++end;
    }
    valueOut = tag.substr(valueStart, end - valueStart);
    return !valueOut.empty();
}

void updateLastWordRange(const std::string& text,
                         size_t baseOffset,
                         size_t& startOut,
                         size_t& endOut,
                         bool& validOut) {
    validOut = false;
    if (text.empty()) return;

    size_t end = text.size();
    while (end > 0 &&
           !isWordByte(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    if (end == 0) return;

    size_t start = end;
    while (start > 0 &&
           isWordByte(static_cast<unsigned char>(text[start - 1]))) {
        --start;
    }

    startOut = baseOffset + start;
    endOut = baseOffset + end;
    validOut = (end > start);
}

void appendVisibleText(const std::string& rawText,
                       std::string& plainOut,
                       std::vector<bool>& maskOut,
                       size_t& lastWordStart,
                       size_t& lastWordEnd,
                       bool& lastWordValid) {
    std::string decoded = decodeHtmlEntities(rawText);
    if (decoded.empty()) return;

    size_t base = plainOut.size();
    plainOut += decoded;
    maskOut.insert(maskOut.end(), decoded.size(), false);
    updateLastWordRange(decoded, base, lastWordStart, lastWordEnd, lastWordValid);
}

bool containsMatchingStrongs(const std::string& text,
                             const std::unordered_set<std::string>& wanted) {
    if (wanted.empty()) return false;
    for (const auto& token : extractStrongsTokens(text)) {
        if (wanted.count(token) != 0) return true;
    }
    return false;
}

struct MaskedText {
    std::string text;
    std::vector<bool> mask;
};

MaskedText collapseWhitespaceWithMask(const std::string& text,
                                      const std::vector<bool>& mask) {
    MaskedText collapsed;
    collapsed.text.reserve(text.size());
    collapsed.mask.reserve(mask.size());

    bool lastWasSpace = true;
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char uc = static_cast<unsigned char>(text[i]);
        if (std::isspace(uc)) {
            if (!lastWasSpace) {
                collapsed.text.push_back(' ');
                collapsed.mask.push_back(false);
                lastWasSpace = true;
            }
            continue;
        }

        collapsed.text.push_back(text[i]);
        collapsed.mask.push_back(i < mask.size() ? mask[i] : false);
        lastWasSpace = false;
    }

    while (!collapsed.text.empty() && collapsed.text.back() == ' ') {
        collapsed.text.pop_back();
        if (!collapsed.mask.empty()) collapsed.mask.pop_back();
    }
    while (!collapsed.text.empty() && collapsed.text.front() == ' ') {
        collapsed.text.erase(collapsed.text.begin());
        if (!collapsed.mask.empty()) collapsed.mask.erase(collapsed.mask.begin());
    }

    return collapsed;
}

std::string buildSnippetMarkup(const std::string& text,
                               const std::vector<bool>& mask,
                               size_t maxLen = 160) {
    if (text.empty()) return "";

    size_t hitStart = std::string::npos;
    size_t hitEnd = std::string::npos;
    for (size_t i = 0; i < mask.size() && i < text.size(); ++i) {
        if (!mask[i]) continue;
        hitStart = i;
        hitEnd = i + 1;
        while (hitEnd < mask.size() && hitEnd < text.size() && mask[hitEnd]) {
            ++hitEnd;
        }
        break;
    }

    size_t left = 0;
    size_t right = text.size();
    if (text.size() > maxLen) {
        if (hitStart != std::string::npos) {
            left = (hitStart > maxLen / 2) ? (hitStart - maxLen / 2) : 0;
            if (hitEnd > left + maxLen) {
                left = hitEnd - maxLen;
            }
        }
        if (left > text.size()) left = text.size();
        right = std::min(text.size(), left + maxLen);
    }

    std::string out;
    out.reserve((right - left) + 32);
    if (left > 0) out += "... ";

    bool inHighlight = false;
    for (size_t i = left; i < right; ++i) {
        bool highlight = (i < mask.size()) && mask[i];
        if (highlight && !inHighlight) {
            out += "<span class=\"searchhit\">";
            inHighlight = true;
        } else if (!highlight && inHighlight) {
            out += "</span>";
            inHighlight = false;
        }
        out.push_back(text[i]);
    }

    if (inHighlight) out += "</span>";
    if (right < text.size()) out += " ...";
    return out;
}

std::string buildRegexSnippet(const std::string& text,
                              const std::smatch& match,
                              const std::regex& re,
                              size_t maxLen = 160) {
    if (text.empty()) return "";

    const size_t startPos = static_cast<size_t>(match.position());
    const size_t endPos = startPos + static_cast<size_t>(match.length());

    size_t left = 0;
    if (startPos > maxLen / 2) {
        left = startPos - (maxLen / 2);
    }
    if (endPos > left + maxLen) {
        left = endPos - maxLen;
    }
    if (left > text.size()) left = text.size();

    size_t right = std::min(text.size(), left + maxLen);
    std::string snippet = text.substr(left, right - left);
    std::vector<bool> mask(snippet.size(), false);
    auto begin = std::sregex_iterator(snippet.begin(), snippet.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        size_t pos = static_cast<size_t>((*it).position());
        size_t len = static_cast<size_t>((*it).length());
        if (len == 0) continue;
        for (size_t i = pos; i < pos + len && i < mask.size(); ++i) {
            mask[i] = true;
        }
    }

    std::string out;
    if (left > 0) out += "... ";
    out += buildSnippetMarkup(snippet, mask, snippet.size());
    if (right < text.size()) out += " ...";
    return out;
}

std::string buildStrongsSnippet(const std::string& xhtml,
                                const std::string& strongsQuery,
                                const std::string& plainFallback,
                                size_t maxLen = 160) {
    auto truncateFallback = [maxLen](const std::string& text) {
        std::string collapsed = trimCopy(text);
        if (collapsed.size() <= maxLen) return collapsed;
        return collapsed.substr(0, maxLen) + "...";
    };

    if (xhtml.empty()) return truncateFallback(plainFallback);

    std::unordered_set<std::string> wanted;
    for (const auto& token : extractStrongsTokens(strongsQuery)) {
        wanted.insert(token);
    }
    if (wanted.empty()) return truncateFallback(plainFallback);

    std::string plain;
    std::vector<bool> mask;
    size_t lastWordStart = 0;
    size_t lastWordEnd = 0;
    bool lastWordValid = false;

    for (size_t pos = 0; pos < xhtml.size();) {
        if (xhtml[pos] != '<') {
            size_t nextTag = xhtml.find('<', pos);
            size_t end = (nextTag == std::string::npos) ? xhtml.size() : nextTag;
            appendVisibleText(xhtml.substr(pos, end - pos),
                              plain, mask,
                              lastWordStart, lastWordEnd, lastWordValid);
            pos = end;
            continue;
        }

        size_t tagEnd = xhtml.find('>', pos);
        if (tagEnd == std::string::npos) break;

        std::string tag = xhtml.substr(pos, tagEnd - pos + 1);
        std::string lowerTag = lowerCopy(tag);

        if (lowerTag.rfind("<small", 0) == 0) {
            size_t closePos = xhtml.find("</small>", tagEnd + 1);
            if (closePos != std::string::npos) {
                size_t blockEnd = closePos + 8;
                std::string block = xhtml.substr(pos, blockEnd - pos);
                if (lastWordValid && containsMatchingStrongs(block, wanted)) {
                    for (size_t i = lastWordStart;
                         i < lastWordEnd && i < mask.size(); ++i) {
                        mask[i] = true;
                    }
                }
                pos = blockEnd;
                continue;
            }
        }

        if (lowerTag.rfind("<w", 0) == 0) {
            size_t closePos = xhtml.find("</w>", tagEnd + 1);
            if (closePos != std::string::npos) {
                std::string inner = xhtml.substr(tagEnd + 1, closePos - (tagEnd + 1));
                size_t before = plain.size();
                appendVisibleText(stripTagsSimple(inner),
                                  plain, mask,
                                  lastWordStart, lastWordEnd, lastWordValid);
                if (containsMatchingStrongs(tag, wanted)) {
                    for (size_t i = before; i < plain.size() && i < mask.size(); ++i) {
                        mask[i] = true;
                    }
                }
                pos = closePos + 4;
                continue;
            }
        }

        if (lowerTag.rfind("<a", 0) == 0) {
            size_t closePos = xhtml.find("</a>", tagEnd + 1);
            if (closePos != std::string::npos) {
                std::string inner = xhtml.substr(tagEnd + 1, closePos - (tagEnd + 1));
                std::string innerText = stripTagsSimple(inner);
                size_t before = plain.size();
                appendVisibleText(innerText,
                                  plain, mask,
                                  lastWordStart, lastWordEnd, lastWordValid);
                std::string href;
                if (extractTagAttribute(tag, "href", href) &&
                    containsMatchingStrongs(href, wanted)) {
                    for (size_t i = before; i < plain.size() && i < mask.size(); ++i) {
                        mask[i] = true;
                    }
                }
                pos = closePos + 4;
                continue;
            }
        }

        pos = tagEnd + 1;
    }

    MaskedText collapsed = collapseWhitespaceWithMask(plain, mask);
    if (collapsed.text.empty()) {
        return plainFallback;
    }

    if (std::find(collapsed.mask.begin(), collapsed.mask.end(), true) ==
        collapsed.mask.end()) {
        if (collapsed.text.size() <= maxLen) return collapsed.text;
        return collapsed.text.substr(0, maxLen) + "...";
    }

    return buildSnippetMarkup(collapsed.text, collapsed.mask, maxLen);
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

    int userVersion = 0;
    {
        sqlite3_stmt* verStmt = nullptr;
        if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &verStmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(verStmt) == SQLITE_ROW) {
                userVersion = sqlite3_column_int(verStmt, 0);
            }
        }
        if (verStmt) sqlite3_finalize(verStmt);
    }

    // Rebuild old indexes so plain_text no longer contains legacy Strong's/notes artifacts.
    if (userVersion < 3) {
        sqlite3_exec(db, "DROP TABLE IF EXISTS verse_index;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DROP TABLE IF EXISTS indexed_modules;", nullptr, nullptr, nullptr);
    }

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

    sqlite3_exec(db, "PRAGMA user_version = 3;", nullptr, nullptr, nullptr);
    return true;
}

void SearchIndexer::queueModuleIndex(const std::string& moduleName, bool force) {
    std::string normalized = trimCopy(moduleName);
    if (!db_ || normalized.empty()) return;
    if (!force && isModuleIndexed(normalized)) return;

    std::lock_guard<std::mutex> lock(workerMutex_);
    if (pendingSet_.insert(normalized).second) {
        pendingModules_.push_back(normalized);
        workerCv_.notify_one();
    }
}

void SearchIndexer::queueModuleIndex(const std::vector<std::string>& moduleNames) {
    for (const auto& module : moduleNames) {
        queueModuleIndex(module, false);
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

    StrongsLanguageHint langHint = detectStrongsLanguageHint(text);
    std::vector<std::string> terms = extractStrongsTokens(text);
    if (terms.empty()) return "";

    std::vector<std::string> prefixedTerms;
    char prefix = 0;
    if (langHint == StrongsLanguageHint::Greek) prefix = 'G';
    if (langHint == StrongsLanguageHint::Hebrew) prefix = 'H';
    if (prefix) {
        for (const auto& t : terms) {
            if (!t.empty() && std::isalpha(static_cast<unsigned char>(t[0])) &&
                static_cast<char>(std::toupper(static_cast<unsigned char>(t[0]))) == prefix) {
                prefixedTerms.push_back(t);
            }
        }
    }

    auto appendOrTerms = [](std::ostringstream& out,
                            const std::vector<std::string>& values) {
        for (size_t i = 0; i < values.size(); ++i) {
            if (i) out << " OR ";
            out << quoteFtsToken(values[i]);
        }
    };

    std::ostringstream out;
    out << "{strongs_text}:((";
    appendOrTerms(out, terms);
    out << ")";

    if (langHint != StrongsLanguageHint::Any) {
        out << " AND (";
        bool wrote = false;
        if (!prefixedTerms.empty()) {
            appendOrTerms(out, prefixedTerms);
            wrote = true;
        }
        std::string langToken = (langHint == StrongsLanguageHint::Greek)
                                    ? "Greek"
                                    : "Hebrew";
        if (wrote) out << " OR ";
        out << quoteFtsToken(langToken);
        out << ")";
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

    std::string sql =
        "SELECT module_name, key_text, "
        "snippet(verse_index, 5, '<span class=\"searchhit\">', '</span>', ' ... ', 18) "
        "FROM verse_index "
        "WHERE verse_index MATCH ? AND module_name = ? "
        "ORDER BY bm25(verse_index)";
    if (maxResults > 0) {
        sql += " LIMIT ?";
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }

    bindText(stmt, 1, ftsQuery);
    bindText(stmt, 2, moduleName);
    if (maxResults > 0) {
        sqlite3_bind_int(stmt, 3, maxResults);
    }

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

    std::string sql =
        "SELECT module_name, key_text, strongs_text, plain_text "
        "FROM verse_index "
        "WHERE verse_index MATCH ? AND module_name = ? "
        "ORDER BY bm25(verse_index)";
    if (maxResults > 0) {
        sql += " LIMIT ?";
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }

    bindText(stmt, 1, ftsQuery);
    bindText(stmt, 2, moduleName);
    if (maxResults > 0) {
        sqlite3_bind_int(stmt, 3, maxResults);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SearchResult result;
        const char* module = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* strongs = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* plain = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        result.module = module ? module : moduleName;
        result.key = key ? key : "";
        result.text = buildStrongsSnippet(strongs ? strongs : "",
                                          strongsQuery,
                                          plain ? plain : "");
        results.push_back(std::move(result));
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<SearchResult> SearchIndexer::searchRegex(
    const std::string& moduleName,
    const std::string& pattern,
    bool caseSensitive,
    int maxResults) const {

    std::vector<SearchResult> results;
    if (!db_ || moduleName.empty() || pattern.empty()) return results;

    std::regex re;
    try {
        std::regex::flag_type flags = std::regex::ECMAScript;
        if (!caseSensitive) flags |= std::regex::icase;
        re = std::regex(pattern, flags);
    } catch (const std::regex_error&) {
        return results;
    }

    std::lock_guard<std::mutex> lock(dbMutex_);

    const char* sql =
        "SELECT module_name, key_text, plain_text "
        "FROM verse_index "
        "WHERE module_name = ? "
        "ORDER BY rowid";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }

    bindText(stmt, 1, moduleName);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* module = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* plain = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        std::string plainText = plain ? plain : "";

        std::smatch match;
        if (plainText.empty() || !std::regex_search(plainText, match, re)) {
            continue;
        }

        SearchResult result;
        result.module = module ? module : moduleName;
        result.key = key ? key : "";
        result.text = buildRegexSnippet(plainText, match, re);
        results.push_back(std::move(result));

        if (maxResults > 0 && static_cast<int>(results.size()) >= maxResults) {
            break;
        }
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

                // Keep plain_text index free of Strong's/morph tags and notes/cross refs.
                mgr->setGlobalOption("Strong's Numbers", "Off");
                mgr->setGlobalOption("Morphological Tags", "Off");
                mgr->setGlobalOption("Footnotes", "Off");
                mgr->setGlobalOption("Cross-references", "Off");
                mgr->setGlobalOption("Headings", "Off");
                const char* plainRaw = mod->stripText();
                std::string plain = plainRaw ? plainRaw : "";

                // Keep strongs_text index with tag-rich XHTML for lemma searches.
                mgr->setGlobalOption("Strong's Numbers", "On");
                mgr->setGlobalOption("Morphological Tags", "On");
                mgr->setGlobalOption("Footnotes", "On");
                mgr->setGlobalOption("Cross-references", "On");
                mgr->setGlobalOption("Headings", "On");
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
