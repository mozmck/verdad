#include "import/ImportedModuleManager.h"
#include "search/SearchIndexer.h"
#include "search/SmartSearch.h"
#include "sword/SwordPaths.h"

#include <markupfiltmgr.h>
#include <swconfig.h>
#include <swmgr.h>
#include <swmodule.h>
#include <treekey.h>
#include <versekey.h>

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <regex>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace verdad {
namespace {

constexpr int kSmartSearchMinCandidateLimit = 200;
constexpr int kSmartSearchMaxCandidateLimit = 2000;

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

bool isAsciiText(std::string_view text) {
    return std::all_of(text.begin(), text.end(),
                       [](unsigned char c) { return c < 0x80; });
}

bool containsCaseInsensitiveAscii(std::string_view haystack,
                                  std::string_view needle) {
    if (needle.empty()) return true;

    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char lhs, char rhs) {
            return std::tolower(static_cast<unsigned char>(lhs)) ==
                   std::tolower(static_cast<unsigned char>(rhs));
        });
    return it != haystack.end();
}

bool hasUnsupportedRegexLiteralPrefilterSyntax(const std::string& pattern) {
    bool escaped = false;
    for (char c : pattern) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '[' || c == ']' || c == '(' || c == ')' || c == '|') {
            return true;
        }
    }
    return escaped;
}

std::string extractRegexLiteralPrefilter(const std::string& pattern) {
    if (pattern.empty() ||
        hasUnsupportedRegexLiteralPrefilterSyntax(pattern)) {
        return "";
    }

    std::string best;
    std::string current;
    bool escaped = false;

    auto commitCurrent = [&]() {
        std::string candidate = trimCopy(current);
        if (candidate.size() > best.size()) best = std::move(candidate);
        current.clear();
    };

    for (size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (escaped) {
            escaped = false;
            // Regex character classes/assertions such as \b or \d are not
            // reliable literal filters, but escaped punctuation remains literal.
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                commitCurrent();
            } else {
                current.push_back(c);
            }
            continue;
        }

        if (c == '\\') {
            escaped = true;
            continue;
        }

        if (c == '.' || c == '^' || c == '$') {
            commitCurrent();
            continue;
        }

        if (c == '*' || c == '?') {
            if (!current.empty()) current.pop_back();
            commitCurrent();
            continue;
        }

        if (c == '+') {
            commitCurrent();
            continue;
        }

        if (c == '{') {
            size_t close = pattern.find('}', i + 1);
            if (close == std::string::npos) {
                current.push_back(c);
                continue;
            }

            std::string body = pattern.substr(i + 1, close - i - 1);
            size_t comma = body.find(',');
            std::string minText = trimCopy(
                comma == std::string::npos ? body : body.substr(0, comma));
            if (minText == "0" && !current.empty()) {
                current.pop_back();
            }
            commitCurrent();
            i = close;
            continue;
        }

        current.push_back(c);
    }

    if (escaped) return "";

    commitCurrent();
    if (best.size() < 3) return "";
    return best;
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

std::string normalizeFilterToken(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            out.push_back(static_cast<char>(std::tolower(uc)));
        }
    }
    return out;
}

std::string makeBibleScopeToken(int testament, const std::string& bookName) {
    std::string scope = (testament == 2) ? "nt" : "ot";
    std::string bookToken = normalizeFilterToken(bookName);
    if (!bookToken.empty()) {
        scope += " ";
        scope += bookToken;
    }
    return scope;
}

std::string buildModuleSignature(const ModuleInfo& module,
                                 const std::string& resourceType,
                                 const std::string& moduleToken) {
    std::ostringstream out;
    out << module.name << '\n'
        << resourceType << '\n'
        << moduleToken << '\n'
        << module.type << '\n'
        << module.version << '\n'
        << module.language << '\n'
        << module.markup << '\n'
        << module.abbreviation << '\n'
        << module.description << '\n'
        << module.category << '\n'
        << module.distributionLicense << '\n'
        << module.textSource << '\n'
        << (module.hasStrongs ? '1' : '0')
        << (module.hasMorph ? '1' : '0');
    for (const auto& feature : module.featureLabels) {
        out << '\n' << feature;
    }
    return out.str();
}

void appendGeneralBookTocEntries(sword::TreeKey* treeKey,
                                 int depth,
                                 std::vector<GeneralBookTocEntry>& out) {
    if (!treeKey) return;

    do {
        GeneralBookTocEntry entry;
        entry.key = treeKey->getText();
        entry.label = treeKey->getLocalName();
        entry.depth = depth;
        entry.hasChildren = treeKey->hasChildren();
        out.push_back(std::move(entry));

        if (treeKey->hasChildren() && treeKey->firstChild()) {
            appendGeneralBookTocEntries(treeKey, depth + 1, out);
            treeKey->parent();
        }
    } while (treeKey->nextSibling());
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
    {
        size_t start = 0;
        while (start < collapsed.text.size() && collapsed.text[start] == ' ') ++start;
        if (start > 0) {
            collapsed.text.erase(0, start);
            if (collapsed.mask.size() >= start)
                collapsed.mask.erase(collapsed.mask.begin(),
                                     collapsed.mask.begin() + static_cast<ptrdiff_t>(start));
        }
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

SearchIndexer::SearchIndexer(const std::string& dbPath,
                             const ImportedModuleManager* importedModuleMgr)
    : dbPath_(dbPath)
    , importedModuleMgr_(importedModuleMgr) {
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

SearchIndexer::ScopedSuspend::~ScopedSuspend() {
    release();
}

SearchIndexer::ScopedSuspend::ScopedSuspend(ScopedSuspend&& other) noexcept
    : owner_(other.owner_) {
    other.owner_ = nullptr;
}

SearchIndexer::ScopedSuspend&
SearchIndexer::ScopedSuspend::operator=(ScopedSuspend&& other) noexcept {
    if (this == &other) return *this;

    release();
    owner_ = other.owner_;
    other.owner_ = nullptr;
    return *this;
}

void SearchIndexer::ScopedSuspend::release() {
    if (!owner_) return;
    owner_->resumeBackgroundIndexing();
    owner_ = nullptr;
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

    // Rebuild older schemas into the unified library index.
    if (userVersion < 4) {
        sqlite3_exec(db, "DROP TABLE IF EXISTS verse_index;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DROP TABLE IF EXISTS library_index;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DROP TABLE IF EXISTS indexed_modules;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DROP TABLE IF EXISTS module_index_errors;", nullptr, nullptr, nullptr);
    }

    const char* sql = R"SQL(
        CREATE TABLE IF NOT EXISTS indexed_modules (
            module_name TEXT PRIMARY KEY,
            resource_type TEXT NOT NULL,
            module_signature TEXT NOT NULL,
            entry_count INTEGER NOT NULL DEFAULT 0,
            indexed_at TEXT DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS module_index_errors (
            module_name TEXT PRIMARY KEY,
            last_error TEXT NOT NULL,
            updated_at TEXT DEFAULT (datetime('now'))
        );

        CREATE VIRTUAL TABLE IF NOT EXISTS library_index USING fts5(
            resource_type,
            module_token,
            scope_token,
            title,
            content,
            strongs_text,
            module_name UNINDEXED,
            key_text UNINDEXED,
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

    sqlite3_exec(db, "PRAGMA user_version = 4;", nullptr, nullptr, nullptr);
    return true;
}

void SearchIndexer::queueModuleIndex(const std::string& moduleName, bool force) {
    std::string normalized = trimCopy(moduleName);
    if (!db_ || normalized.empty()) return;
    if (!force && isModuleIndexed(normalized)) return;

    std::lock_guard<std::mutex> lock(workerMutex_);
    auto it = pendingForces_.find(normalized);
    if (it == pendingForces_.end()) {
        pendingModules_.push_back(IndexTask{normalized, force});
        pendingForces_.emplace(normalized, force);
        workerCv_.notify_one();
        return;
    }
    if (force) it->second = true;
}

void SearchIndexer::queueModuleIndex(const std::vector<std::string>& moduleNames) {
    for (const auto& module : moduleNames) {
        queueModuleIndex(module, false);
    }
}

void SearchIndexer::synchronizeModules(const std::vector<ModuleInfo>& modules) {
    std::unordered_map<std::string, ModuleCatalogEntry> nextCatalog;
    nextCatalog.reserve(modules.size());
    std::unordered_set<std::string> activeModules;

    for (const auto& module : modules) {
        std::string resourceType = searchResourceTypeTokenForModuleType(module.type);
        if (!isSearchableResourceTypeToken(resourceType) || module.name.empty()) {
            continue;
        }

        ModuleCatalogEntry entry;
        entry.info = module;
        entry.resourceType = resourceType;
        entry.moduleToken = normalizeFilterToken(module.name);
        entry.signature = buildModuleSignature(module, resourceType, entry.moduleToken);
        activeModules.insert(module.name);
        nextCatalog.emplace(module.name, std::move(entry));
    }

    {
        std::lock_guard<std::mutex> lock(catalogMutex_);
        moduleCatalog_ = std::move(nextCatalog);
    }

    if (!db_) return;

    std::lock_guard<std::mutex> lock(dbMutex_);

    sqlite3_stmt* selectStmt = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT module_name FROM indexed_modules",
                           -1, &selectStmt, nullptr) != SQLITE_OK) {
        return;
    }

    std::vector<std::string> staleModules;
    while (sqlite3_step(selectStmt) == SQLITE_ROW) {
        const char* moduleText =
            reinterpret_cast<const char*>(sqlite3_column_text(selectStmt, 0));
        std::string moduleName = moduleText ? moduleText : "";
        if (!moduleName.empty() && activeModules.count(moduleName) == 0) {
            staleModules.push_back(std::move(moduleName));
        }
    }
    sqlite3_finalize(selectStmt);

    if (staleModules.empty()) return;

    sqlite3_exec(db_, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, nullptr);

    sqlite3_stmt* deleteIndex = nullptr;
    sqlite3_stmt* deleteStatus = nullptr;
    sqlite3_stmt* deleteErrors = nullptr;
    sqlite3_prepare_v2(db_,
                       "DELETE FROM library_index WHERE module_name = ?",
                       -1, &deleteIndex, nullptr);
    sqlite3_prepare_v2(db_,
                       "DELETE FROM indexed_modules WHERE module_name = ?",
                       -1, &deleteStatus, nullptr);
    sqlite3_prepare_v2(db_,
                       "DELETE FROM module_index_errors WHERE module_name = ?",
                       -1, &deleteErrors, nullptr);

    if (!deleteIndex || !deleteStatus || !deleteErrors) {
        if (deleteIndex) sqlite3_finalize(deleteIndex);
        if (deleteStatus) sqlite3_finalize(deleteStatus);
        if (deleteErrors) sqlite3_finalize(deleteErrors);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return;
    }

    for (const auto& moduleName : staleModules) {
        bindText(deleteIndex, 1, moduleName);
        sqlite3_step(deleteIndex);
        sqlite3_reset(deleteIndex);
        sqlite3_clear_bindings(deleteIndex);

        bindText(deleteStatus, 1, moduleName);
        sqlite3_step(deleteStatus);
        sqlite3_reset(deleteStatus);
        sqlite3_clear_bindings(deleteStatus);

        bindText(deleteErrors, 1, moduleName);
        sqlite3_step(deleteErrors);
        sqlite3_reset(deleteErrors);
        sqlite3_clear_bindings(deleteErrors);
    }

    sqlite3_finalize(deleteIndex);
    sqlite3_finalize(deleteStatus);
    sqlite3_finalize(deleteErrors);
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
}

bool SearchIndexer::isModuleIndexed(const std::string& moduleName) const {
    if (!db_ || moduleName.empty()) return false;

    std::string expectedSignature;
    {
        std::lock_guard<std::mutex> catalogLock(catalogMutex_);
        auto it = moduleCatalog_.find(moduleName);
        if (it != moduleCatalog_.end()) {
            expectedSignature = it->second.signature;
        }
    }

    std::lock_guard<std::mutex> lock(dbMutex_);

    const char* sql =
        "SELECT module_signature FROM indexed_modules WHERE module_name = ? LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    bindText(stmt, 1, moduleName);
    bool indexed = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* signatureText =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string storedSignature = signatureText ? signatureText : "";
        indexed = expectedSignature.empty() || storedSignature == expectedSignature;
    }
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
        if (pendingForces_.count(normalized) > 0) {
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

std::string SearchIndexer::moduleIndexError(const std::string& moduleName) const {
    if (!db_ || moduleName.empty()) return "";

    std::lock_guard<std::mutex> lock(dbMutex_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT last_error FROM module_index_errors WHERE module_name = ? LIMIT 1",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return "";
    }

    bindText(stmt, 1, moduleName);
    std::string error;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* text =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        error = text ? text : "";
    }
    sqlite3_finalize(stmt);
    return error;
}

SearchIndexer::ScopedSuspend SearchIndexer::suspendBackgroundIndexing() {
    if (!db_) return ScopedSuspend();

    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        ++suspendDepth_;
    }
    workerCv_.notify_all();
    waitForWorkerIdle();
    return ScopedSuspend(this);
}

void SearchIndexer::resumeBackgroundIndexing() {
    bool shouldNotify = false;
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        if (suspendDepth_ > 0) {
            --suspendDepth_;
            shouldNotify = (suspendDepth_ == 0);
        }
    }

    if (shouldNotify) workerCv_.notify_all();
}

void SearchIndexer::waitForWorkerIdle() {
    std::unique_lock<std::mutex> lock(workerMutex_);
    workerIdleCv_.wait(lock, [this]() {
        return !workerTaskRunning_;
    });
}

std::string SearchIndexer::buildFilterFtsQuery(const SearchRequest& request) {
    std::vector<std::string> clauses;

    if (!request.resourceTypes.empty()) {
        std::ostringstream types;
        if (request.resourceTypes.size() > 1) types << "(";
        for (size_t i = 0; i < request.resourceTypes.size(); ++i) {
            if (i) types << " OR ";
            types << "{resource_type}:" << quoteFtsToken(request.resourceTypes[i]);
        }
        if (request.resourceTypes.size() > 1) types << ")";
        clauses.push_back(types.str());
    }

    if (!request.moduleName.empty()) {
        clauses.push_back(
            "{module_token}:" + quoteFtsToken(normalizeFilterToken(request.moduleName)));
    }

    std::string scopeToken;
    switch (request.bibleScope) {
    case SearchRequest::BibleScope::OldTestament:
        scopeToken = "ot";
        break;
    case SearchRequest::BibleScope::NewTestament:
        scopeToken = "nt";
        break;
    case SearchRequest::BibleScope::CurrentBook:
        scopeToken = normalizeFilterToken(request.currentBook);
        break;
    case SearchRequest::BibleScope::All:
        break;
    }
    if (!scopeToken.empty()) {
        clauses.push_back("{scope_token}:" + quoteFtsToken(scopeToken));
    }

    if (clauses.empty()) return "";

    std::ostringstream out;
    for (size_t i = 0; i < clauses.size(); ++i) {
        if (i) out << " AND ";
        out << clauses[i];
    }
    return out.str();
}

std::string SearchIndexer::buildWordFtsQuery(const SearchRequest& request,
                                             const std::string& query,
                                             bool exactPhrase) {
    std::string text = trimCopy(query);
    std::string filterQuery = buildFilterFtsQuery(request);
    if (text.empty()) return filterQuery;

    std::string contentClause;
    if (exactPhrase) {
        contentClause = "{title content}:" + quoteFtsToken(text);
    } else {
        std::vector<std::string> tokens = tokenizeWords(text);
        if (tokens.empty()) return filterQuery;

        std::ostringstream content;
        content << "{title content}:(";
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (i) content << " AND ";
            content << quoteFtsToken(tokens[i]);
        }
        content << ")";
        contentClause = content.str();
    }

    if (filterQuery.empty()) return contentClause;
    return filterQuery + " AND " + contentClause;
}

std::string SearchIndexer::buildStrongsFtsQuery(const SearchRequest& request,
                                                const std::string& query) {
    std::string text = trimCopy(query);
    std::string filterQuery = buildFilterFtsQuery(request);
    if (text.empty()) return filterQuery;

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

    std::ostringstream content;
    content << "{strongs_text}:((";
    appendOrTerms(content, terms);
    content << ")";

    if (langHint != StrongsLanguageHint::Any) {
        content << " AND (";
        bool wrote = false;
        if (!prefixedTerms.empty()) {
            appendOrTerms(content, prefixedTerms);
            wrote = true;
        }
        std::string langToken = (langHint == StrongsLanguageHint::Greek)
                                    ? "Greek"
                                    : "Hebrew";
        if (wrote) content << " OR ";
        content << quoteFtsToken(langToken);
        content << ")";
    }

    content << ")";

    std::string contentClause = content.str();
    if (filterQuery.empty()) return contentClause;
    return filterQuery + " AND " + contentClause;
}

std::vector<SearchResult> SearchIndexer::searchWord(
    const SearchRequest& request,
    const std::string& query,
    bool exactPhrase,
    int maxResults) const {

    std::vector<SearchResult> results;
    if (!db_ || query.empty()) return results;

    std::string ftsQuery = buildWordFtsQuery(request, query, exactPhrase);
    if (ftsQuery.empty()) return results;

    std::lock_guard<std::mutex> lock(dbMutex_);

    const int limit = (maxResults > 0) ? maxResults : request.maxResults;
    std::string sql =
        "SELECT resource_type, module_name, key_text, title, "
        "snippet(library_index, 4, '<span class=\"searchhit\">', '</span>', ' ... ', 18) "
        "FROM library_index "
        "WHERE library_index MATCH ? "
        "ORDER BY bm25(library_index)";
    if (limit > 0) {
        sql += " LIMIT ?";
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }

    bindText(stmt, 1, ftsQuery);
    if (limit > 0) {
        sqlite3_bind_int(stmt, 2, limit);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SearchResult result;
        const char* type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* module = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* snippet = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        result.resourceType = type ? type : "";
        result.module = module ? module : request.moduleName;
        result.key = key ? key : "";
        result.title = title ? title : result.key;
        result.text = snippet ? snippet : "";
        if (result.text.empty()) {
            result.text = result.title.empty() ? result.key : result.title;
        }
        results.push_back(std::move(result));
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<SearchResult> SearchIndexer::searchStrongs(
    const SearchRequest& request,
    const std::string& strongsQuery,
    int maxResults) const {

    std::vector<SearchResult> results;
    if (!db_ || strongsQuery.empty()) return results;

    std::string ftsQuery = buildStrongsFtsQuery(request, strongsQuery);
    if (ftsQuery.empty()) return results;

    std::lock_guard<std::mutex> lock(dbMutex_);

    const int limit = (maxResults > 0) ? maxResults : request.maxResults;
    std::string sql =
        "SELECT resource_type, module_name, key_text, title, strongs_text, content "
        "FROM library_index "
        "WHERE library_index MATCH ? "
        "ORDER BY bm25(library_index)";
    if (limit > 0) {
        sql += " LIMIT ?";
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }

    bindText(stmt, 1, ftsQuery);
    if (limit > 0) {
        sqlite3_bind_int(stmt, 2, limit);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SearchResult result;
        const char* type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* module = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* strongs = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        const char* plain = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        result.resourceType = type ? type : "";
        result.module = module ? module : request.moduleName;
        result.key = key ? key : "";
        result.title = title ? title : result.key;
        result.text = buildStrongsSnippet(strongs ? strongs : "",
                                          strongsQuery,
                                          plain ? plain : "");
        if (result.text.empty()) {
            result.text = result.title.empty() ? result.key : result.title;
        }
        results.push_back(std::move(result));
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<SearchResult> SearchIndexer::searchRegex(
    const SearchRequest& request,
    const std::string& pattern,
    bool caseSensitive,
    int maxResults,
    RegexProgressCallback progressCallback) const {

    std::vector<SearchResult> results;
    if (!db_ || pattern.empty()) return results;

    std::regex re;
    try {
        std::regex::flag_type flags = std::regex::ECMAScript;
        if (!caseSensitive) flags |= std::regex::icase;
        re = std::regex(pattern, flags);
    } catch (const std::regex_error&) {
        return results;
    }

    std::string literalPrefilter = extractRegexLiteralPrefilter(pattern);
    if (!caseSensitive && !isAsciiText(literalPrefilter)) {
        literalPrefilter.clear();
    }

    std::string filterQuery = buildFilterFtsQuery(request);

    sqlite3* readDb = nullptr;
    int rc = sqlite3_open_v2(
        dbPath_.c_str(), &readDb,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (rc != SQLITE_OK || !readDb) {
        if (readDb) sqlite3_close(readDb);
        return results;
    }
    sqlite3_busy_timeout(readDb, 5000);

    int total = 0;
    sqlite3_stmt* countStmt = nullptr;
    if (filterQuery.empty()) {
        if (sqlite3_prepare_v2(
                readDb,
                "SELECT count(*) FROM library_index",
                -1, &countStmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(countStmt) == SQLITE_ROW) {
                total = sqlite3_column_int(countStmt, 0);
            }
        }
    } else if (sqlite3_prepare_v2(
                   readDb,
                   "SELECT count(*) FROM library_index WHERE library_index MATCH ?",
                   -1, &countStmt, nullptr) == SQLITE_OK) {
        bindText(countStmt, 1, filterQuery);
        if (sqlite3_step(countStmt) == SQLITE_ROW) {
            total = sqlite3_column_int(countStmt, 0);
        }
    }
    if (countStmt) sqlite3_finalize(countStmt);

    auto reportProgress = [&](int scanned, int matches) -> bool {
        if (!progressCallback) return true;
        RegexSearchProgress progress;
        progress.scanned = scanned;
        progress.total = total;
        progress.matches = matches;
        return progressCallback(progress);
    };

    if (!reportProgress(0, 0)) {
        sqlite3_close(readDb);
        return results;
    }

    sqlite3_stmt* stmt = nullptr;
    std::string sql =
        "SELECT resource_type, module_name, key_text, title, content "
        "FROM library_index ";
    if (!filterQuery.empty()) {
        sql += "WHERE library_index MATCH ? ";
    }
    sql += "ORDER BY rowid";

    if (sqlite3_prepare_v2(readDb, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(readDb);
        return results;
    }

    if (!filterQuery.empty()) {
        bindText(stmt, 1, filterQuery);
    }

    int scanned = 0;
    int matches = 0;
    bool cancelled = false;
    const int limit = (maxResults > 0) ? maxResults : request.maxResults;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ++scanned;

        const char* type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* module = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* plain = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        std::string titleText = title ? title : "";
        std::string plainText = plain ? plain : "";
        std::string searchableText = titleText.empty()
                                         ? plainText
                                         : (titleText + " " + plainText);

        if (!literalPrefilter.empty()) {
            const bool literalMatch = caseSensitive
                                          ? (searchableText.find(literalPrefilter) != std::string::npos)
                                          : containsCaseInsensitiveAscii(searchableText, literalPrefilter);
            if (!literalMatch) {
                if ((scanned % 128) == 0 && !reportProgress(scanned, matches)) {
                    cancelled = true;
                    break;
                }
                continue;
            }
        }

        std::smatch match;
        if (searchableText.empty() || !std::regex_search(searchableText, match, re)) {
            if ((scanned % 128) == 0 && !reportProgress(scanned, matches)) {
                cancelled = true;
                break;
            }
            continue;
        }

        SearchResult result;
        result.resourceType = type ? type : "";
        result.module = module ? module : request.moduleName;
        result.key = key ? key : "";
        result.title = titleText.empty() ? result.key : titleText;
        result.text = buildRegexSnippet(searchableText, match, re);
        results.push_back(std::move(result));
        matches = static_cast<int>(results.size());

        if (!reportProgress(scanned, matches)) {
            cancelled = true;
            break;
        }

        if (limit > 0 && static_cast<int>(results.size()) >= limit) {
            break;
        }
    }

    sqlite3_finalize(stmt);
    if (!cancelled) {
        reportProgress(scanned, matches);
    }
    sqlite3_close(readDb);
    return results;
}

std::vector<SearchResult> SearchIndexer::searchSmart(
    const SearchRequest& request,
    const std::string& query,
    const std::string& language,
    int maxResults) const {

    std::vector<SearchResult> results;
    if (!db_ || query.empty()) return results;

    // Build the expanded FTS query (synonyms + prefix matching)
    std::string smartFtsContent = smart_search::buildSmartFtsQuery(query, language);
    if (smartFtsContent.empty()) return results;

    std::string filterQuery = buildFilterFtsQuery(request);
    std::string ftsQuery = filterQuery.empty()
        ? smartFtsContent
        : filterQuery + " AND " + smartFtsContent;

    // Phase 1: Run the expanded FTS query to get candidate results.
    const int limit = (maxResults > 0) ? maxResults : request.maxResults;

    sqlite3_stmt* stmt = nullptr;
    struct CandidateRow {
        SearchResult result;
        std::string plainText;
        int ftsRank = 0;
    };
    std::vector<CandidateRow> candidates;
    {
        std::lock_guard<std::mutex> lock(dbMutex_);

        std::string sql =
            "SELECT resource_type, module_name, key_text, title, content "
            "FROM library_index "
            "WHERE library_index MATCH ? "
            "ORDER BY bm25(library_index) "
            "LIMIT ?";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return results;
        }

        int candidateLimit = (limit > 0)
            ? std::max(limit * 8, kSmartSearchMinCandidateLimit)
            : kSmartSearchMaxCandidateLimit;
        candidateLimit = std::min(candidateLimit, kSmartSearchMaxCandidateLimit);

        bindText(stmt, 1, ftsQuery);
        sqlite3_bind_int(stmt, 2, candidateLimit);

        int rank = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            CandidateRow row;
            const char* type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* module = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const char* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            const char* title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            const char* plain = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));

            row.result.resourceType = type ? type : "";
            row.result.module = module ? module : request.moduleName;
            row.result.key = key ? key : "";
            row.result.title = title ? title : row.result.key;
            row.plainText = plain ? plain : "";
            row.ftsRank = rank++;

            // Pre-populate text with a truncated snippet; will be replaced
            // with highlighted version below.
            if (row.plainText.size() > 160) {
                row.result.text = row.plainText.substr(0, 160) + "...";
            } else {
                row.result.text = row.plainText;
            }

            candidates.push_back(std::move(row));
        }
        sqlite3_finalize(stmt);
    }

    if (candidates.empty()) return results;

    // Phase 2: Score and re-rank using fuzzy/synonym/phonetic matching.
    std::vector<std::string> queryTerms;
    {
        std::istringstream ss(query);
        std::string word;
        while (ss >> word) {
            // Strip punctuation
            size_t s = 0, e = word.size();
            while (s < e && !std::isalnum(static_cast<unsigned char>(word[s])) &&
                   static_cast<unsigned char>(word[s]) < 0x80) ++s;
            while (e > s && !std::isalnum(static_cast<unsigned char>(word[e - 1])) &&
                   static_cast<unsigned char>(word[e - 1]) < 0x80) --e;
            if (s < e) queryTerms.push_back(word.substr(s, e - s));
        }
    }

    std::vector<std::string> candidateTexts;
    candidateTexts.reserve(candidates.size());
    for (const auto& c : candidates) {
        std::string searchable = c.result.title.empty()
            ? c.plainText
            : (c.result.title + " " + c.plainText);
        candidateTexts.push_back(std::move(searchable));
    }

    auto scored = smart_search::scoreSmartResults(queryTerms, candidateTexts, language);

    // Phase 3: Build final results in scored order with highlighted snippets.
    // Combine the FTS rank and fuzzy score for final ordering.
    // FTS BM25 rank is already good for exact/synonym hits; the fuzzy score
    // helps promote phonetic and near-miss matches.
    for (auto& s : scored) {
        // Blend: 60% fuzzy quality, 40% FTS rank order (normalized).
        double ftsNorm = 1.0 - (static_cast<double>(candidates[s.rowIndex].ftsRank) /
                                std::max(1.0, static_cast<double>(candidates.size())));
        s.combinedScore = 0.6 * s.combinedScore + 0.4 * ftsNorm;
    }
    std::sort(scored.begin(), scored.end(),
              [](const smart_search::ScoredMatch& a,
                 const smart_search::ScoredMatch& b) {
                  return a.combinedScore > b.combinedScore;
              });

    int count = 0;
    for (const auto& s : scored) {
        if (limit > 0 && count >= limit) break;

        auto& candidate = candidates[s.rowIndex];

        // Build a highlighted snippet showing where matches occur.
        // Highlight query terms, their synonyms, and fuzzy matches.
        std::string plain = candidate.plainText;
        std::string titleText = candidate.result.title;
        std::string searchable = titleText.empty()
            ? plain : (titleText + " " + plain);

        // Build a highlight mask over the plain text content.
        // We build two versions of the plain text for matching:
        // 1. ASCII-lowered (for basic case-insensitive matching)
        // 2. Accent-stripped + lowered (for accent-insensitive matching)
        // Both are used to find highlight positions.
        std::vector<bool> mask(plain.size(), false);
        std::string lowerPlain;
        lowerPlain.resize(plain.size());
        std::transform(plain.begin(), plain.end(), lowerPlain.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        // Build a position map from stripped→original so we can highlight
        // the right bytes even when accented chars are multi-byte UTF-8.
        std::string strippedPlain = smart_search::stripDiacritics(plain);
        std::string lowerStrippedPlain;
        lowerStrippedPlain.resize(strippedPlain.size());
        std::transform(strippedPlain.begin(), strippedPlain.end(),
                       lowerStrippedPlain.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        // Map from stripped-string byte index → original-string byte index.
        // When a multi-byte accented char maps to a single ASCII char, all
        // original bytes in that sequence should be highlighted.
        std::vector<size_t> strippedToOrig;
        strippedToOrig.reserve(strippedPlain.size());
        {
            size_t origIdx = 0;
            size_t stripIdx = 0;
            while (origIdx < plain.size() && stripIdx < strippedPlain.size()) {
                unsigned char ob = static_cast<unsigned char>(plain[origIdx]);
                if (ob < 0x80) {
                    strippedToOrig.push_back(origIdx);
                    ++origIdx;
                    ++stripIdx;
                } else {
                    // Multi-byte UTF-8 in original, possibly 1 byte in stripped
                    size_t seqLen = 1;
                    if ((ob & 0xE0) == 0xC0) seqLen = 2;
                    else if ((ob & 0xF0) == 0xE0) seqLen = 3;
                    else if ((ob & 0xF8) == 0xF0) seqLen = 4;

                    unsigned char sb = static_cast<unsigned char>(strippedPlain[stripIdx]);
                    if (sb < 0x80) {
                        // Was mapped to ASCII — 1 stripped byte covers seqLen orig bytes
                        strippedToOrig.push_back(origIdx);
                        origIdx += seqLen;
                        ++stripIdx;
                    } else {
                        // Was kept as-is — copy same number of bytes
                        for (size_t k = 0; k < seqLen && stripIdx < strippedPlain.size(); ++k) {
                            strippedToOrig.push_back(origIdx + k);
                            ++stripIdx;
                        }
                        origIdx += seqLen;
                    }
                }
            }
        }

        auto markInLower = [&](const std::string& haystack, const std::string& needle,
                               bool useStrippedMap) {
            if (needle.empty()) return;
            size_t pos = 0;
            while ((pos = haystack.find(needle, pos)) != std::string::npos) {
                size_t end = pos + needle.size();
                if (useStrippedMap) {
                    // Map stripped positions back to original positions
                    size_t origStart = (pos < strippedToOrig.size())
                        ? strippedToOrig[pos] : pos;
                    size_t origEnd = (end > 0 && end - 1 < strippedToOrig.size())
                        ? strippedToOrig[end - 1] + 1 : end;
                    // Extend origEnd to cover the full UTF-8 sequence of the last char
                    if (origEnd < plain.size()) {
                        unsigned char b = static_cast<unsigned char>(plain[origEnd - 1]);
                        if (b >= 0x80) {
                            // Walk forward to end of UTF-8 sequence
                            while (origEnd < plain.size() &&
                                   (static_cast<unsigned char>(plain[origEnd]) & 0xC0) == 0x80) {
                                ++origEnd;
                            }
                        }
                    }
                    for (size_t i = origStart; i < origEnd && i < mask.size(); ++i) {
                        mask[i] = true;
                    }
                } else {
                    for (size_t i = pos; i < end && i < mask.size(); ++i) {
                        mask[i] = true;
                    }
                }
                pos += needle.size();
            }
        };

        for (const auto& term : queryTerms) {
            std::string lowerTerm;
            lowerTerm.resize(term.size());
            std::transform(term.begin(), term.end(), lowerTerm.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::string strippedTerm = smart_search::stripDiacritics(lowerTerm);
            std::string lowerStrippedTerm;
            lowerStrippedTerm.resize(strippedTerm.size());
            std::transform(strippedTerm.begin(), strippedTerm.end(),
                           lowerStrippedTerm.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            // Mark exact term matches (case-insensitive)
            markInLower(lowerPlain, lowerTerm, false);
            // Also try accent-stripped matching
            if (lowerStrippedTerm != lowerTerm) {
                markInLower(lowerStrippedPlain, lowerStrippedTerm, true);
            }
            // And match stripped term against unstripped text (covers user typing
            // unaccented query against accented content)
            markInLower(lowerStrippedPlain, lowerTerm, true);

            // Mark synonym matches
            auto syns = smart_search::expandSynonyms(lowerTerm, language);
            for (const auto& syn : syns) {
                if (syn == lowerTerm) continue;
                std::string lowerSyn;
                lowerSyn.resize(syn.size());
                std::transform(syn.begin(), syn.end(), lowerSyn.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                markInLower(lowerPlain, lowerSyn, false);
                // Also match stripped synonym against stripped text
                std::string strippedSyn = smart_search::stripDiacritics(lowerSyn);
                std::string lowerStrippedSyn;
                lowerStrippedSyn.resize(strippedSyn.size());
                std::transform(strippedSyn.begin(), strippedSyn.end(),
                               lowerStrippedSyn.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lowerStrippedSyn != lowerSyn) {
                    markInLower(lowerStrippedPlain, lowerStrippedSyn, true);
                }
            }
        }

        candidate.result.text = buildSnippetMarkup(plain, mask);

        if (candidate.result.text.empty()) {
            candidate.result.text = candidate.result.title.empty()
                ? candidate.result.key : candidate.result.title;
        }

        results.push_back(std::move(candidate.result));
        ++count;
    }

    return results;
}

void SearchIndexer::workerLoop() {
    while (true) {
        IndexTask task;
        auto finishTask = [this]() {
            {
                std::lock_guard<std::mutex> lock(workerMutex_);
                workerTaskRunning_ = false;
            }
            workerIdleCv_.notify_all();
        };
        {
            std::unique_lock<std::mutex> lock(workerMutex_);
            workerCv_.wait(lock, [this]() {
                return stopWorker_ ||
                       (suspendDepth_ == 0 && !pendingModules_.empty());
            });
            if (stopWorker_) break;
            task = pendingModules_.front();
            pendingModules_.pop_front();
            auto forceIt = pendingForces_.find(task.moduleName);
            if (forceIt != pendingForces_.end()) {
                task.force = task.force || forceIt->second;
                pendingForces_.erase(forceIt);
            }
            workerTaskRunning_ = true;
        }

        if (task.moduleName.empty()) {
            finishTask();
            continue;
        }
        if (!task.force && isModuleIndexed(task.moduleName)) {
            finishTask();
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            activeModule_ = task.moduleName;
            activeProgress_ = 0;
        }
        indexing_.store(true);
        indexModuleNow(task.moduleName);
        indexing_.store(false);
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            activeModule_.clear();
            activeProgress_ = 0;
        }
        finishTask();
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
    auto writeError = [&](const std::string& errorText) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(
                writeDb,
                "INSERT OR REPLACE INTO module_index_errors(module_name, last_error, updated_at) "
                "VALUES (?, ?, datetime('now'))",
                -1, &stmt, nullptr) == SQLITE_OK) {
            bindText(stmt, 1, moduleName);
            bindText(stmt, 2, errorText);
            sqlite3_step(stmt);
        }
        if (stmt) sqlite3_finalize(stmt);
    };

    ModuleCatalogEntry catalogEntry;
    {
        std::lock_guard<std::mutex> lock(catalogMutex_);
        auto it = moduleCatalog_.find(moduleName);
        if (it != moduleCatalog_.end()) {
            catalogEntry = it->second;
        }
    }

    if (importedModuleMgr_ && importedModuleMgr_->hasModule(moduleName)) {
        auto importedEntries = importedModuleMgr_->indexEntries(moduleName);
        if (importedEntries.empty()) {
            writeError("Imported module has no extracted entries.");
            sqlite3_close(writeDb);
            return;
        }

        const std::string resourceType = "general_book";
        const std::string moduleToken = !catalogEntry.moduleToken.empty()
            ? catalogEntry.moduleToken
            : normalizeFilterToken(moduleName);
        const std::string moduleSignature = !catalogEntry.signature.empty()
            ? catalogEntry.signature
            : buildModuleSignature(
                importedModuleMgr_->moduleInfo(moduleName),
                resourceType,
                moduleToken);

        sqlite3_exec(writeDb, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, nullptr);

        sqlite3_stmt* deleteIndex = nullptr;
        sqlite3_stmt* deleteStatus = nullptr;
        sqlite3_stmt* insertRow = nullptr;
        sqlite3_stmt* markIndexed = nullptr;
        sqlite3_stmt* deleteError = nullptr;

        sqlite3_prepare_v2(
            writeDb,
            "DELETE FROM library_index WHERE module_name = ?",
            -1, &deleteIndex, nullptr);
        sqlite3_prepare_v2(
            writeDb,
            "DELETE FROM indexed_modules WHERE module_name = ?",
            -1, &deleteStatus, nullptr);
        sqlite3_prepare_v2(
            writeDb,
            "INSERT INTO library_index(resource_type, module_token, scope_token, title, content, strongs_text, module_name, key_text) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            -1, &insertRow, nullptr);
        sqlite3_prepare_v2(
            writeDb,
            "INSERT OR REPLACE INTO indexed_modules(module_name, resource_type, module_signature, entry_count, indexed_at) "
            "VALUES (?, ?, ?, ?, datetime('now'))",
            -1, &markIndexed, nullptr);
        sqlite3_prepare_v2(
            writeDb,
            "DELETE FROM module_index_errors WHERE module_name = ?",
            -1, &deleteError, nullptr);

        if (!deleteIndex || !deleteStatus || !insertRow || !markIndexed || !deleteError) {
            if (deleteIndex) sqlite3_finalize(deleteIndex);
            if (deleteStatus) sqlite3_finalize(deleteStatus);
            if (insertRow) sqlite3_finalize(insertRow);
            if (markIndexed) sqlite3_finalize(markIndexed);
            if (deleteError) sqlite3_finalize(deleteError);
            sqlite3_exec(writeDb, "ROLLBACK;", nullptr, nullptr, nullptr);
            writeError("Failed to prepare imported-module index statements.");
            sqlite3_close(writeDb);
            return;
        }

        bindText(deleteIndex, 1, moduleName);
        sqlite3_step(deleteIndex);
        bindText(deleteStatus, 1, moduleName);
        sqlite3_step(deleteStatus);

        int insertedEntries = 0;
        for (size_t i = 0; i < importedEntries.size(); ++i) {
            const auto& entry = importedEntries[i];
            bindText(insertRow, 1, resourceType);
            bindText(insertRow, 2, moduleToken);
            bindText(insertRow, 3, "");
            bindText(insertRow, 4, entry.title);
            bindText(insertRow, 5, entry.plainText);
            bindText(insertRow, 6, "");
            bindText(insertRow, 7, moduleName);
            bindText(insertRow, 8, entry.key);
            if (sqlite3_step(insertRow) != SQLITE_DONE) {
                sqlite3_reset(insertRow);
                sqlite3_clear_bindings(insertRow);
                sqlite3_exec(writeDb, "ROLLBACK;", nullptr, nullptr, nullptr);
                sqlite3_finalize(deleteIndex);
                sqlite3_finalize(deleteStatus);
                sqlite3_finalize(insertRow);
                sqlite3_finalize(markIndexed);
                sqlite3_finalize(deleteError);
                writeError("Failed to insert imported module index entry.");
                sqlite3_close(writeDb);
                return;
            }
            ++insertedEntries;
            sqlite3_reset(insertRow);
            sqlite3_clear_bindings(insertRow);
            setProgress(static_cast<int>(((i + 1) * 100) /
                                         std::max<size_t>(importedEntries.size(), 1)));
        }

        bindText(markIndexed, 1, moduleName);
        bindText(markIndexed, 2, resourceType);
        bindText(markIndexed, 3, moduleSignature);
        sqlite3_bind_int(markIndexed, 4, insertedEntries);
        bool marked = sqlite3_step(markIndexed) == SQLITE_DONE;

        bindText(deleteError, 1, moduleName);
        sqlite3_step(deleteError);

        sqlite3_finalize(deleteIndex);
        sqlite3_finalize(deleteStatus);
        sqlite3_finalize(insertRow);
        sqlite3_finalize(markIndexed);
        sqlite3_finalize(deleteError);

        if (marked) {
            sqlite3_exec(writeDb, "COMMIT;", nullptr, nullptr, nullptr);
            setProgress(100);
        } else {
            sqlite3_exec(writeDb, "ROLLBACK;", nullptr, nullptr, nullptr);
            writeError("Failed to finalize imported module index metadata.");
        }
        sqlite3_close(writeDb);
        return;
    }

    std::unique_ptr<sword::SWConfig> bundledSysConfig;
    std::unique_ptr<sword::SWMgr> mgr;
    const std::string bundlePath = bundledSwordDataPath();
    if (!bundlePath.empty()) {
        bundledSysConfig = std::make_unique<sword::SWConfig>();
        bundledSysConfig->setValue("Install", "DataPath", bundlePath.c_str());

        auto& install = bundledSysConfig->getSection("Install");
#if defined(__APPLE__)
        if (bundlePath != "/usr/local/share/sword") {
            install.insert({sword::SWBuf("AugmentPath"),
                            sword::SWBuf("/usr/local/share/sword")});
        }
        if (bundlePath != "/opt/homebrew/share/sword") {
            install.insert({sword::SWBuf("AugmentPath"),
                            sword::SWBuf("/opt/homebrew/share/sword")});
        }
#elif !defined(_WIN32)
        if (bundlePath != "/usr/share/sword") {
            install.insert({sword::SWBuf("AugmentPath"),
                            sword::SWBuf("/usr/share/sword")});
        }
        if (bundlePath != "/usr/local/share/sword") {
            install.insert({sword::SWBuf("AugmentPath"),
                            sword::SWBuf("/usr/local/share/sword")});
        }
#endif

        mgr = std::unique_ptr<sword::SWMgr>(new sword::SWMgr(
            nullptr, bundledSysConfig.get(), true,
            new sword::MarkupFilterMgr(sword::FMT_XHTML, sword::ENC_UTF8)));
    } else {
        mgr = std::unique_ptr<sword::SWMgr>(new sword::SWMgr(
            nullptr, nullptr, true,
            new sword::MarkupFilterMgr(sword::FMT_XHTML, sword::ENC_UTF8)));
    }
    if (mgr) {
        for (const auto& path : allUserSwordDataPaths()) {
            mgr->augmentModules(path.c_str());
        }
    }
    sword::SWModule* mod = mgr ? mgr->getModule(moduleName.c_str()) : nullptr;
    if (!mod) {
        writeError("Module not found while rebuilding index.");
        sqlite3_close(writeDb);
        return;
    }

    std::string resourceType = !catalogEntry.resourceType.empty()
                                   ? catalogEntry.resourceType
                                   : searchResourceTypeTokenForModuleType(
                                         mod->getType() ? mod->getType() : "");
    if (!isSearchableResourceTypeToken(resourceType)) {
        writeError("Unsupported searchable module type.");
        sqlite3_close(writeDb);
        return;
    }

    std::string moduleToken = !catalogEntry.moduleToken.empty()
                                  ? catalogEntry.moduleToken
                                  : normalizeFilterToken(moduleName);
    std::string moduleSignature = !catalogEntry.signature.empty()
                                      ? catalogEntry.signature
                                      : buildModuleSignature(
                                            ModuleInfo{moduleName, "", mod->getType() ? mod->getType() : ""},
                                            resourceType,
                                            moduleToken);

    mgr->setGlobalOption("Strong's Numbers", "On");
    mgr->setGlobalOption("Morphological Tags", "On");
    mgr->setGlobalOption("Footnotes", "On");
    mgr->setGlobalOption("Cross-references", "On");
    mgr->setGlobalOption("Headings", "On");

    auto collectSequentialKeys = [](sword::SWModule* target,
                                    std::vector<std::string>& keysOut,
                                    std::unordered_map<std::string, std::string>& titlesOut) {
        if (!target) return;
        std::unique_ptr<sword::SWKey> restoreKey(
            target->getKey() ? target->getKey()->clone() : nullptr);
        const bool hadSkipConsecutiveLinks = target->isSkipConsecutiveLinks();
        target->setSkipConsecutiveLinks(true);
        target->setPosition(sword::TOP);
        if (!target->popError()) {
            std::string lastKey;
            for (size_t count = 0; count < 500000; ++count) {
                std::string key = trimCopy(target->getKeyText());
                if (!key.empty() && key != lastKey) {
                    keysOut.push_back(key);
                    titlesOut.emplace(key, key);
                    lastKey = key;
                }
                (*target)++;
                if (target->popError()) break;
            }
        }
        target->setSkipConsecutiveLinks(hadSkipConsecutiveLinks);
        if (restoreKey) {
            target->setKey(*restoreKey);
            target->popError();
        }
    };

    std::vector<std::string> keyedEntries;
    std::unordered_map<std::string, std::string> entryTitles;

    std::unique_ptr<sword::SWKey> createdKey(mod->createKey());
    if (resourceType == "general_book") {
        if (auto* treeKey = dynamic_cast<sword::TreeKey*>(createdKey.get())) {
            std::vector<GeneralBookTocEntry> toc;
            treeKey->root();
            if (treeKey->hasChildren() && treeKey->firstChild()) {
                appendGeneralBookTocEntries(treeKey, 0, toc);
            }
            for (const auto& entry : toc) {
                std::string key = trimCopy(entry.key);
                if (key.empty()) continue;
                keyedEntries.push_back(key);
                std::string label = trimCopy(entry.label);
                entryTitles.emplace(key, label.empty() ? key : label);
            }
        } else {
            collectSequentialKeys(mod, keyedEntries, entryTitles);
        }
    } else {
        collectSequentialKeys(mod, keyedEntries, entryTitles);
    }

    if (keyedEntries.empty()) {
        writeError(resourceType == "bible"
                       ? "Bible module has no traversable entries."
                       : "Module has no traversable entries to index.");
        sqlite3_close(writeDb);
        return;
    }

    int totalEntries = static_cast<int>(keyedEntries.size());
    if (totalEntries <= 0) totalEntries = 1;

    int processedEntries = 0;
    int insertedEntries = 0;
    int lastProgress = -1;
    auto bumpProgress = [&]() {
        int percent = (processedEntries * 100) / totalEntries;
        if (percent != lastProgress) {
            lastProgress = percent;
            setProgress(percent);
        }
    };

    sqlite3_exec(writeDb, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, nullptr);

    sqlite3_stmt* deleteIndex = nullptr;
    sqlite3_stmt* deleteStatus = nullptr;
    sqlite3_stmt* insertRow = nullptr;
    sqlite3_stmt* markIndexed = nullptr;
    sqlite3_stmt* deleteError = nullptr;

    sqlite3_prepare_v2(
        writeDb,
        "DELETE FROM library_index WHERE module_name = ?",
        -1, &deleteIndex, nullptr);
    sqlite3_prepare_v2(
        writeDb,
        "DELETE FROM indexed_modules WHERE module_name = ?",
        -1, &deleteStatus, nullptr);
    sqlite3_prepare_v2(
        writeDb,
        "INSERT INTO library_index(resource_type, module_token, scope_token, title, content, strongs_text, module_name, key_text) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        -1, &insertRow, nullptr);
    sqlite3_prepare_v2(
        writeDb,
        "INSERT OR REPLACE INTO indexed_modules(module_name, resource_type, module_signature, entry_count, indexed_at) "
        "VALUES (?, ?, ?, ?, datetime('now'))",
        -1, &markIndexed, nullptr);
    sqlite3_prepare_v2(
        writeDb,
        "DELETE FROM module_index_errors WHERE module_name = ?",
        -1, &deleteError, nullptr);

    if (!deleteIndex || !deleteStatus || !insertRow || !markIndexed || !deleteError) {
        if (deleteIndex) sqlite3_finalize(deleteIndex);
        if (deleteStatus) sqlite3_finalize(deleteStatus);
        if (insertRow) sqlite3_finalize(insertRow);
        if (markIndexed) sqlite3_finalize(markIndexed);
        if (deleteError) sqlite3_finalize(deleteError);
        sqlite3_exec(writeDb, "ROLLBACK;", nullptr, nullptr, nullptr);
        writeError("Failed to prepare indexing statements.");
        sqlite3_close(writeDb);
        return;
    }

    bindText(deleteIndex, 1, moduleName);
    sqlite3_step(deleteIndex);
    sqlite3_reset(deleteIndex);
    sqlite3_clear_bindings(deleteIndex);

    bindText(deleteStatus, 1, moduleName);
    sqlite3_step(deleteStatus);
    sqlite3_reset(deleteStatus);
    sqlite3_clear_bindings(deleteStatus);

    auto insertCurrentRow = [&](const std::string& scopeToken,
                                const std::string& title,
                                const std::string& content,
                                const std::string& strongsText,
                                const std::string& keyText) -> bool {
        bindText(insertRow, 1, resourceType);
        bindText(insertRow, 2, moduleToken);
        bindText(insertRow, 3, scopeToken);
        bindText(insertRow, 4, title);
        bindText(insertRow, 5, content);
        bindText(insertRow, 6, strongsText);
        bindText(insertRow, 7, moduleName);
        bindText(insertRow, 8, keyText);
        bool ok = (sqlite3_step(insertRow) == SQLITE_DONE);
        sqlite3_reset(insertRow);
        sqlite3_clear_bindings(insertRow);
        if (ok) ++insertedEntries;
        return ok;
    };

    bool cancelled = false;
    bool failed = false;
    std::string failureMessage;

    if (resourceType == "bible") {
        // First pass: collect plain text with options off
        mgr->setGlobalOption("Strong's Numbers", "Off");
        mgr->setGlobalOption("Morphological Tags", "Off");
        mgr->setGlobalOption("Footnotes", "Off");
        mgr->setGlobalOption("Cross-references", "Off");
        mgr->setGlobalOption("Headings", "Off");

        std::vector<std::string> plainTexts;
        plainTexts.reserve(keyedEntries.size());
        for (const auto& key : keyedEntries) {
            if (stopRequested_.load()) {
                cancelled = true;
                break;
            }
            mod->setKey(key.c_str());
            if (mod->popError()) {
                plainTexts.emplace_back();
                continue;
            }
            const char* plainRaw = mod->stripText();
            plainTexts.push_back(trimCopy(plainRaw ? plainRaw : ""));
        }

        // Second pass: collect XHTML with options on, then insert
        mgr->setGlobalOption("Strong's Numbers", "On");
        mgr->setGlobalOption("Morphological Tags", "On");
        mgr->setGlobalOption("Footnotes", "On");
        mgr->setGlobalOption("Cross-references", "On");
        mgr->setGlobalOption("Headings", "On");

        for (size_t ki = 0; ki < keyedEntries.size() && !cancelled; ++ki) {
            if (stopRequested_.load()) {
                cancelled = true;
                break;
            }

            ++processedEntries;
            if ((processedEntries % 128) == 0) bumpProgress();

            const auto& key = keyedEntries[ki];
            mod->setKey(key.c_str());
            if (mod->popError()) continue;

            std::string plain = ki < plainTexts.size() ? plainTexts[ki] : "";
            std::string xhtml = std::string(mod->renderText().c_str());

            std::string keyText = trimCopy(mod->getKeyText() ? mod->getKeyText() : "");
            if (keyText.empty()) keyText = key;

            std::string title = keyText;
            std::string scopeToken;
            if (auto* vk = dynamic_cast<sword::VerseKey*>(mod->getKey())) {
                const char* bookName = vk->getBookName();
                scopeToken = makeBibleScopeToken(
                    vk->getTestament(), bookName ? bookName : "");
            }
            if (!insertCurrentRow(scopeToken, title, plain, xhtml, keyText)) {
                failed = true;
                failureMessage = "Failed to insert indexed Bible verse.";
                break;
            }
        }
    } else {
        for (const auto& key : keyedEntries) {
            if (stopRequested_.load()) {
                cancelled = true;
                break;
            }

            ++processedEntries;
            if ((processedEntries % 64) == 0) bumpProgress();

            mod->setKey(key.c_str());
            if (mod->popError()) continue;

            const char* plainRaw = mod->stripText();
            std::string plain = trimCopy(plainRaw ? plainRaw : "");
            std::string title = key;
            auto titleIt = entryTitles.find(key);
            if (titleIt != entryTitles.end() && !trimCopy(titleIt->second).empty()) {
                title = titleIt->second;
            }

            if (plain.empty() && title.empty()) continue;
            if (!insertCurrentRow("", title, plain, "", key)) {
                failed = true;
                failureMessage = "Failed to insert indexed module entry.";
                break;
            }
        }
    }

    if (!cancelled && !failed) {
        bindText(markIndexed, 1, moduleName);
        bindText(markIndexed, 2, resourceType);
        bindText(markIndexed, 3, moduleSignature);
        sqlite3_bind_int(markIndexed, 4, insertedEntries);
        const bool marked = (sqlite3_step(markIndexed) == SQLITE_DONE);
        sqlite3_reset(markIndexed);
        sqlite3_clear_bindings(markIndexed);

        bindText(deleteError, 1, moduleName);
        sqlite3_step(deleteError);
        sqlite3_reset(deleteError);
        sqlite3_clear_bindings(deleteError);

        if (marked) {
            sqlite3_exec(writeDb, "COMMIT;", nullptr, nullptr, nullptr);
            setProgress(100);
        } else {
            sqlite3_exec(writeDb, "ROLLBACK;", nullptr, nullptr, nullptr);
            writeError("Failed to finalize module index metadata.");
        }
    } else {
        sqlite3_exec(writeDb, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (failed) {
            writeError(failureMessage.empty() ? "Module indexing failed." : failureMessage);
        }
    }

    sqlite3_finalize(deleteIndex);
    sqlite3_finalize(deleteStatus);
    sqlite3_finalize(insertRow);
    sqlite3_finalize(markIndexed);
    sqlite3_finalize(deleteError);
    sqlite3_close(writeDb);
}

} // namespace verdad
