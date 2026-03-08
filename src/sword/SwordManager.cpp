#include "sword/SwordManager.h"
#include "app/PerfTrace.h"

#include <swmgr.h>
#include <swmodule.h>
#include <swkey.h>
#include <versekey.h>
#include <listkey.h>
#include <markupfiltmgr.h>
#include <swbuf.h>

#include <algorithm>
#include <chrono>
#include <regex>
#include <sstream>
#include <cstring>
#include <cctype>

namespace verdad {
namespace {

std::string trimCopy(const std::string& s) {
    size_t start = 0;
    while (start < s.size() &&
           std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

bool decodeNextUtf8(const std::string& s, size_t& i, uint32_t& cp) {
    if (i >= s.size()) return false;
    unsigned char c0 = static_cast<unsigned char>(s[i]);
    if (c0 < 0x80) {
        cp = c0;
        ++i;
        return true;
    }
    if ((c0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
        unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
        cp = (static_cast<uint32_t>(c0 & 0x1F) << 6) |
             static_cast<uint32_t>(c1 & 0x3F);
        i += 2;
        return true;
    }
    if ((c0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
        unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
        unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
        cp = (static_cast<uint32_t>(c0 & 0x0F) << 12) |
             (static_cast<uint32_t>(c1 & 0x3F) << 6) |
             static_cast<uint32_t>(c2 & 0x3F);
        i += 3;
        return true;
    }
    if ((c0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
        unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
        unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
        unsigned char c3 = static_cast<unsigned char>(s[i + 3]);
        cp = (static_cast<uint32_t>(c0 & 0x07) << 18) |
             (static_cast<uint32_t>(c1 & 0x3F) << 12) |
             (static_cast<uint32_t>(c2 & 0x3F) << 6) |
             static_cast<uint32_t>(c3 & 0x3F);
        i += 4;
        return true;
    }
    // Invalid leading byte; skip it.
    cp = c0;
    ++i;
    return true;
}

bool isCjkCodepoint(uint32_t cp) {
    // CJK Unified Ideographs + Extensions + Compatibility + Japanese/Korean ranges.
    return
        (cp >= 0x3400 && cp <= 0x4DBF) ||
        (cp >= 0x4E00 && cp <= 0x9FFF) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0x20000 && cp <= 0x2EBEF) ||
        (cp >= 0x30000 && cp <= 0x3134F) ||
        (cp >= 0x3040 && cp <= 0x30FF) ||
        (cp >= 0x31F0 && cp <= 0x31FF) ||
        (cp >= 0xAC00 && cp <= 0xD7AF);
}

bool containsCjkText(const std::string& s) {
    size_t i = 0;
    uint32_t cp = 0;
    while (decodeNextUtf8(s, i, cp)) {
        if (isCjkCodepoint(cp)) return true;
    }
    return false;
}

std::string stripCjkText(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        size_t start = i;
        uint32_t cp = 0;
        if (!decodeNextUtf8(s, i, cp)) break;
        if (!isCjkCodepoint(cp)) {
            out.append(s.substr(start, i - start));
        }
    }
    return trimCopy(out);
}

std::string cleanupLexiconText(const std::string& s) {
    // Preserve full lexicon payload; only trim outer whitespace.
    return trimCopy(s);
}

bool isLikelyMorphFragmentToken(const std::string& token) {
    return token.size() == 2 &&
           std::isdigit(static_cast<unsigned char>(token[0])) &&
           std::isalpha(static_cast<unsigned char>(token[1]));
}

std::string normalizeStrongsKey(const std::string& strongsNumber) {
    std::string key = trimCopy(strongsNumber);
    if (key.empty()) return key;

    static const std::regex keyRe(R"(([A-Za-z]?\d+[A-Za-z]?))");
    std::smatch match;
    if (std::regex_search(key, match, keyRe)) {
        key = match[1].str();
    }

    for (char& c : key) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }

    if (!key.empty() && std::isalpha(static_cast<unsigned char>(key[0]))) {
        // Strong's prefixes are H (Hebrew) or G (Greek). Reject unknown prefixes
        // to avoid lookups drifting to stale/incorrect lexicon keys.
        if (key[0] != 'H' && key[0] != 'G') return "";
    }
    return key;
}

std::string canonicalLexKey(const std::string& keyText) {
    std::string key = normalizeStrongsKey(keyText);
    if (key.empty()) return key;

    size_t i = 0;
    std::string prefix;
    if (std::isalpha(static_cast<unsigned char>(key[i]))) {
        prefix.push_back(key[i]);
        ++i;
    }

    size_t j = i;
    while (j < key.size() && std::isdigit(static_cast<unsigned char>(key[j]))) ++j;
    if (j == i) return key;

    std::string digits = key.substr(i, j - i);
    size_t nz = digits.find_first_not_of('0');
    digits = (nz == std::string::npos) ? "0" : digits.substr(nz);

    std::string suffix = key.substr(j);
    return prefix + digits + suffix;
}

bool keyMatchesRequest(const std::string& requestedKey, const std::string& resolvedKey) {
    std::string req = canonicalLexKey(requestedKey);
    std::string got = canonicalLexKey(resolvedKey);
    if (req.empty() || got.empty()) return false;
    if (req == got) return true;

    // Allow prefixed/unprefixed equivalent forms (e.g. H7225 vs 7225).
    if (req.size() > 1 && std::isalpha(static_cast<unsigned char>(req[0])) &&
        req.substr(1) == got) {
        return true;
    }
    if (got.size() > 1 && std::isalpha(static_cast<unsigned char>(got[0])) &&
        got.substr(1) == req) {
        return true;
    }
    return false;
}

char strongPrefixFromKey(const std::string& key) {
    if (!key.empty() && std::isalpha(static_cast<unsigned char>(key[0]))) {
        return static_cast<char>(
            std::toupper(static_cast<unsigned char>(key[0])));
    }
    return 0;
}

std::vector<std::string> strongLexiconsForPrefix(char prefix) {
    if (prefix == 'H') {
        return {"StrongsHebrew", "TWOT", "StrongsRealHebrew"};
    }
    if (prefix == 'G') {
        return {"StrongsGreek", "Thayer", "StrongsRealGreek"};
    }
    return {"StrongsHebrew", "TWOT", "StrongsRealHebrew",
            "StrongsGreek", "Thayer", "StrongsRealGreek"};
}

void appendUniqueLexicon(std::vector<std::string>& out,
                         const std::string& lexicon) {
    if (lexicon.empty()) return;
    if (std::find(out.begin(), out.end(), lexicon) != out.end()) return;
    out.push_back(lexicon);
}

std::vector<std::string> strongLexiconsForPrefix(
        char prefix,
        const std::vector<std::string>& preferredLexicons) {
    std::vector<std::string> lexicons;
    for (const auto& name : preferredLexicons) {
        appendUniqueLexicon(lexicons, name);
    }

    for (const auto& name : strongLexiconsForPrefix(prefix)) {
        appendUniqueLexicon(lexicons, name);
    }
    return lexicons;
}

std::vector<std::string> strongLookupKeys(const std::string& key, char prefix) {
    std::vector<std::string> keys;
    keys.push_back(key);
    if (prefix && key.length() > 1) {
        keys.push_back(key.substr(1));
    } else if (!key.empty() &&
               std::isdigit(static_cast<unsigned char>(key[0]))) {
        keys.push_back("H" + key);
        keys.push_back("G" + key);
    }
    return keys;
}

std::string readLexiconEntry(sword::SWModule* lex, const std::string& key) {
    if (!lex || key.empty()) return "";

    lex->setKey(key.c_str());
    if (lex->popError()) return "";

    // Ensure entry text is loaded for this key before stripping markup.
    (void)lex->renderText();
    if (lex->popError()) return "";

    const char* resolvedKey = lex->getKeyText();
    if (!resolvedKey || !keyMatchesRequest(key, resolvedKey)) return "";

    return cleanupLexiconText(lex->stripText());
}

char toLowerAsciiChar(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool equalsNoCase(char a, char b) {
    return toLowerAsciiChar(a) == toLowerAsciiChar(b);
}

std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = toLowerAsciiChar(c);
    return out;
}

bool lexiconEntryMatchesRequest(const std::string& requestedKey,
                                const std::string& resolvedKey) {
    std::string strongKey = normalizeStrongsKey(requestedKey);
    if (!strongKey.empty()) {
        return keyMatchesRequest(strongKey, resolvedKey);
    }

    std::string requested = toLowerAscii(trimCopy(requestedKey));
    std::string resolved = toLowerAscii(trimCopy(resolvedKey));
    if (requested.empty() || resolved.empty()) return false;
    return requested == resolved;
}

std::string renderLexiconEntryHtml(sword::SWModule* lex,
                                   const std::string& requestedKey,
                                   const std::string& lookupKey,
                                   std::string* resolvedKeyOut = nullptr) {
    if (!lex || lookupKey.empty()) return "";

    lex->setKey(lookupKey.c_str());
    if (lex->popError()) return "";

    std::string html = std::string(lex->renderText().c_str());
    if (lex->popError()) return "";

    const char* resolvedKey = lex->getKeyText();
    if (!resolvedKey || !lexiconEntryMatchesRequest(requestedKey, resolvedKey)) {
        return "";
    }

    if (resolvedKeyOut) *resolvedKeyOut = resolvedKey;
    return html;
}

bool containsNoCase(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;

    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (!equalsNoCase(haystack[i + j], needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

size_t findNoCase(const std::string& haystack,
                  const std::string& needle,
                  size_t start = 0) {
    if (needle.empty()) return start <= haystack.size() ? start : std::string::npos;
    if (needle.size() > haystack.size() || start >= haystack.size()) return std::string::npos;

    for (size_t i = start; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (!equalsNoCase(haystack[i + j], needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return i;
    }
    return std::string::npos;
}

size_t findTagEnd(const std::string& html, size_t tagStart) {
    if (tagStart >= html.size() || html[tagStart] != '<') return std::string::npos;
    bool inQuote = false;
    char quote = 0;
    for (size_t i = tagStart + 1; i < html.size(); ++i) {
        char c = html[i];
        if (inQuote) {
            if (c == quote) {
                inQuote = false;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            inQuote = true;
            quote = c;
            continue;
        }
        if (c == '>') return i;
    }
    return std::string::npos;
}

bool parseTag(const std::string& html,
              size_t tagStart,
              size_t& tagEnd,
              std::string& tagName,
              bool& isClosing,
              bool& isSelfClosing) {
    tagEnd = findTagEnd(html, tagStart);
    if (tagEnd == std::string::npos) return false;

    size_t i = tagStart + 1;
    while (i < tagEnd && std::isspace(static_cast<unsigned char>(html[i]))) ++i;

    isClosing = false;
    if (i < tagEnd && html[i] == '/') {
        isClosing = true;
        ++i;
        while (i < tagEnd && std::isspace(static_cast<unsigned char>(html[i]))) ++i;
    }

    size_t nameStart = i;
    while (i < tagEnd) {
        unsigned char c = static_cast<unsigned char>(html[i]);
        if (std::isalnum(c) || c == '_' || c == '-' || c == ':') {
            ++i;
            continue;
        }
        break;
    }
    if (i == nameStart) return false;

    tagName = toLowerAscii(html.substr(nameStart, i - nameStart));

    isSelfClosing = false;
    if (tagEnd > tagStart + 1) {
        size_t back = tagEnd;
        while (back > tagStart + 1 &&
               std::isspace(static_cast<unsigned char>(html[back - 1]))) {
            --back;
        }
        if (back > tagStart + 1 && html[back - 1] == '/') isSelfClosing = true;
    }

    return true;
}

bool extractAttributeValue(const std::string& tag,
                           const std::string& attrName,
                           std::string& valueOut) {
    valueOut.clear();
    if (tag.size() < 3) return false;

    std::string attrLower = toLowerAscii(attrName);

    size_t i = 1;
    while (i < tag.size() && tag[i] != '>' &&
           !std::isspace(static_cast<unsigned char>(tag[i]))) {
        ++i;
    }

    while (i < tag.size()) {
        while (i < tag.size() &&
               std::isspace(static_cast<unsigned char>(tag[i]))) {
            ++i;
        }
        if (i >= tag.size() || tag[i] == '>' || tag[i] == '/') break;

        size_t nameStart = i;
        while (i < tag.size()) {
            unsigned char c = static_cast<unsigned char>(tag[i]);
            if (std::isalnum(c) || c == '_' || c == '-' || c == ':') {
                ++i;
                continue;
            }
            break;
        }
        if (i == nameStart) {
            ++i;
            continue;
        }

        std::string name = toLowerAscii(tag.substr(nameStart, i - nameStart));

        while (i < tag.size() &&
               std::isspace(static_cast<unsigned char>(tag[i]))) {
            ++i;
        }

        std::string val;
        if (i < tag.size() && tag[i] == '=') {
            ++i;
            while (i < tag.size() &&
                   std::isspace(static_cast<unsigned char>(tag[i]))) {
                ++i;
            }

            if (i < tag.size() && (tag[i] == '"' || tag[i] == '\'')) {
                char q = tag[i++];
                size_t valStart = i;
                while (i < tag.size() && tag[i] != q) ++i;
                val = tag.substr(valStart, i - valStart);
                if (i < tag.size() && tag[i] == q) ++i;
            } else {
                size_t valStart = i;
                while (i < tag.size() && tag[i] != '>' &&
                       !std::isspace(static_cast<unsigned char>(tag[i]))) {
                    ++i;
                }
                val = tag.substr(valStart, i - valStart);
            }
        } else {
            val.clear();
        }

        if (name == attrLower) {
            valueOut = val;
            return true;
        }
    }

    return false;
}

std::string decodeHtmlEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] != '&') {
            out.push_back(s[i++]);
            continue;
        }
        if (i + 5 <= s.size() &&
            equalsNoCase(s[i + 1], 'a') &&
            equalsNoCase(s[i + 2], 'm') &&
            equalsNoCase(s[i + 3], 'p') &&
            s[i + 4] == ';') {
            out.push_back('&');
            i += 5;
            continue;
        }
        if (i + 4 <= s.size() &&
            equalsNoCase(s[i + 1], 'l') &&
            equalsNoCase(s[i + 2], 't') &&
            s[i + 3] == ';') {
            out.push_back('<');
            i += 4;
            continue;
        }
        if (i + 4 <= s.size() &&
            equalsNoCase(s[i + 1], 'g') &&
            equalsNoCase(s[i + 2], 't') &&
            s[i + 3] == ';') {
            out.push_back('>');
            i += 4;
            continue;
        }
        if (i + 6 <= s.size() &&
            equalsNoCase(s[i + 1], 'q') &&
            equalsNoCase(s[i + 2], 'u') &&
            equalsNoCase(s[i + 3], 'o') &&
            equalsNoCase(s[i + 4], 't') &&
            s[i + 5] == ';') {
            out.push_back('"');
            i += 6;
            continue;
        }
        if (i + 6 <= s.size() &&
            equalsNoCase(s[i + 1], 'a') &&
            equalsNoCase(s[i + 2], 'p') &&
            equalsNoCase(s[i + 3], 'o') &&
            equalsNoCase(s[i + 4], 's') &&
            s[i + 5] == ';') {
            out.push_back('\'');
            i += 6;
            continue;
        }
        if (i + 5 <= s.size() && s[i + 1] == '#' &&
            s[i + 2] == '3' && s[i + 3] == '9' && s[i + 4] == ';') {
            out.push_back('\'');
            i += 5;
            continue;
        }

        out.push_back(s[i++]);
    }
    return out;
}

bool isGreekCodepoint(uint32_t cp) {
    return (cp >= 0x0370 && cp <= 0x03FF) ||
           (cp >= 0x1F00 && cp <= 0x1FFF);
}

bool isHebrewCodepoint(uint32_t cp) {
    return (cp >= 0x0590 && cp <= 0x05FF) ||
           (cp >= 0xFB1D && cp <= 0xFB4F);
}

bool isUnicodeCombiningMark(uint32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036F) ||
           (cp >= 0x1DC0 && cp <= 0x1DFF) ||
           (cp >= 0x20D0 && cp <= 0x20FF) ||
           (cp >= 0xFE20 && cp <= 0xFE2F);
}

bool matchesStrongsScript(uint32_t cp, char prefix) {
    if (prefix == 'G') return isGreekCodepoint(cp);
    if (prefix == 'H') return isHebrewCodepoint(cp);
    return isGreekCodepoint(cp) || isHebrewCodepoint(cp);
}

std::string extractScriptToken(const std::string& text, char prefix) {
    size_t i = 0;
    while (i < text.size()) {
        size_t start = i;
        uint32_t cp = 0;
        if (!decodeNextUtf8(text, i, cp)) break;
        if (!matchesStrongsScript(cp, prefix)) continue;

        size_t end = i;
        while (end < text.size()) {
            size_t next = end;
            uint32_t cp2 = 0;
            if (!decodeNextUtf8(text, next, cp2)) break;
            if (matchesStrongsScript(cp2, prefix) ||
                isUnicodeCombiningMark(cp2)) {
                end = next;
                continue;
            }
            break;
        }

        std::string token = trimCopy(text.substr(start, end - start));
        if (!token.empty()) return token;
        i = end;
    }

    return "";
}

std::string extractBracketToken(const std::string& text, char open, char close) {
    size_t begin = text.find(open);
    if (begin == std::string::npos) return "";
    size_t end = text.find(close, begin + 1);
    if (end == std::string::npos || end <= begin + 1) return "";
    return trimCopy(text.substr(begin + 1, end - begin - 1));
}

std::string extractStrongsLemmaFromDefinition(const std::string& definition,
                                              char prefix) {
    std::string text = trimCopy(decodeHtmlEntities(definition));
    if (text.empty()) return "";

    static const std::regex leadKey(R"(^[HhGg]?\d+[A-Za-z]?\s+)");
    text = std::regex_replace(text, leadKey, "");
    text = trimCopy(text);
    if (text.empty()) return "";

    std::string lemma = extractScriptToken(text, prefix);
    if (!lemma.empty()) return lemma;

    // Fallback for lexicons without script text.
    return extractBracketToken(text, '{', '}');
}

bool isHexDigit(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) ||
           (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return 0;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '+') {
            out.push_back(' ');
            ++i;
            continue;
        }
        if (s[i] == '%' && i + 2 < s.size() &&
            isHexDigit(s[i + 1]) && isHexDigit(s[i + 2])) {
            int hi = hexValue(s[i + 1]);
            int lo = hexValue(s[i + 2]);
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 3;
            continue;
        }
        out.push_back(s[i++]);
    }
    return out;
}

std::string extractQueryValue(const std::string& url, const std::string& key) {
    std::string decoded = decodeHtmlEntities(url);
    size_t q = decoded.find('?');
    std::string query = (q == std::string::npos) ? decoded : decoded.substr(q + 1);
    std::string keyLower = toLowerAscii(key);

    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        std::string part = query.substr(
            pos, (amp == std::string::npos ? query.size() : amp) - pos);

        size_t eq = part.find('=');
        std::string name = (eq == std::string::npos) ? part : part.substr(0, eq);
        std::string value = (eq == std::string::npos) ? "" : part.substr(eq + 1);

        if (toLowerAscii(name) == keyLower) {
            return urlDecode(value);
        }

        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

bool isWordByte(unsigned char c) {
    return std::isalnum(c) || c == '\'' || c == '-' || c >= 0x80;
}

struct HoverMeta {
    std::string strong;
    std::string morph;

    bool empty() const {
        return strong.empty() && morph.empty();
    }
};

struct OutputTarget {
    size_t start = 0;
    size_t end = 0;
    bool valid = false;
};

void splitTokens(const std::string& src,
                 char delim,
                 std::vector<std::string>& out) {
    size_t i = 0;
    while (i < src.size()) {
        while (i < src.size() && (src[i] == delim ||
               std::isspace(static_cast<unsigned char>(src[i])))) {
            ++i;
        }
        if (i >= src.size()) break;

        size_t j = i;
        while (j < src.size() && src[j] != delim &&
               !std::isspace(static_cast<unsigned char>(src[j]))) {
            ++j;
        }

        std::string tok = trimCopy(src.substr(i, j - i));
        if (!tok.empty()) out.push_back(tok);
        i = j;
    }
}

void appendUniqueTokens(std::string& dst, const std::string& src, char delim) {
    std::vector<std::string> existing;
    splitTokens(dst, delim, existing);

    std::vector<std::string> incoming;
    splitTokens(src, delim, incoming);

    for (const auto& tok : incoming) {
        bool found = false;
        for (const auto& ex : existing) {
            if (ex == tok) {
                found = true;
                break;
            }
        }
        if (found) continue;
        if (!dst.empty()) dst.push_back(delim);
        dst += tok;
        existing.push_back(tok);
    }
}

void addStrongToken(HoverMeta& meta, const std::string& token) {
    std::string norm = normalizeStrongsKey(token);
    if (norm.empty()) return;
    if (isLikelyMorphFragmentToken(norm)) return;
    if (meta.strong.empty()) meta.strong = norm;
    else appendUniqueTokens(meta.strong, norm, '|');
}

std::string normalizeMorph(const std::string& morphRaw) {
    std::string morph = trimCopy(decodeHtmlEntities(morphRaw));
    while (!morph.empty() &&
           (morph.front() == '/' || morph.front() == '\\')) {
        morph.erase(morph.begin());
    }

    size_t colonPos = morph.find(':');
    if (colonPos != std::string::npos &&
        morph.substr(0, colonPos).find(' ') == std::string::npos) {
        morph = morph.substr(colonPos + 1);
    }

    return trimCopy(morph);
}

void addMorphToken(HoverMeta& meta, const std::string& morphRaw) {
    std::string norm = normalizeMorph(morphRaw);
    if (norm.empty()) return;
    if (meta.morph.empty()) meta.morph = norm;
    else appendUniqueTokens(meta.morph, norm, ' ');
}

std::string htmlEscapeAttr(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '"': out += "&quot;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

std::string buildWordSpanOpenTag(const HoverMeta& meta) {
    std::string out = R"(<span class="w")";
    if (!meta.strong.empty()) {
        out += R"( data-strong=")";
        out += htmlEscapeAttr(meta.strong);
        out += '"';
    }
    if (!meta.morph.empty()) {
        out += R"( data-morph=")";
        out += htmlEscapeAttr(meta.morph);
        out += '"';
    }
    out += '>';
    return out;
}

std::string stripTags(const std::string& html) {
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

std::string firstStrongTokenFromText(const std::string& text) {
    std::string t = trimCopy(decodeHtmlEntities(text));
    if (t.empty()) return "";

    for (size_t i = 0; i < t.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(t[i]);
        bool prefix = (c == 'H' || c == 'h' || c == 'G' || c == 'g');
        bool digitStart = std::isdigit(c);
        if (!prefix && !digitStart) continue;

        size_t j = i;
        if (prefix) ++j;
        size_t digitsStart = j;
        while (j < t.size() &&
               std::isdigit(static_cast<unsigned char>(t[j]))) {
            ++j;
        }
        if (j == digitsStart) continue;

        while (j < t.size() &&
               std::isalpha(static_cast<unsigned char>(t[j]))) {
            ++j;
        }

        std::string token = normalizeStrongsKey(t.substr(i, j - i));
        if (isLikelyMorphFragmentToken(token)) continue;
        if (!token.empty()) return token;
    }

    return "";
}

bool looksLikeStrongsDisplay(const std::string& text) {
    std::string t = trimCopy(decodeHtmlEntities(text));
    if (t.empty()) return true;

    while (!t.empty() && (t.front() == '<' || t.front() == '(' ||
           t.front() == '[' || std::isspace(static_cast<unsigned char>(t.front())))) {
        t.erase(t.begin());
    }
    while (!t.empty() && (t.back() == '>' || t.back() == ')' ||
           t.back() == ']' || std::isspace(static_cast<unsigned char>(t.back())))) {
        t.pop_back();
    }
    if (t.empty()) return true;

    std::string norm = normalizeStrongsKey(t);
    if (!norm.empty() && !isLikelyMorphFragmentToken(norm)) return true;
    if (isLikelyMorphFragmentToken(t)) return false;

    bool hasDigit = false;
    for (char c : t) {
        if (std::isdigit(static_cast<unsigned char>(c))) hasDigit = true;
        if (!std::isalnum(static_cast<unsigned char>(c))) return false;
    }
    return hasDigit;
}

bool looksLikeMorphDisplay(const std::string& text) {
    std::string t = trimCopy(decodeHtmlEntities(text));
    if (t.empty()) return true;

    if (t.front() == '(' && t.back() == ')') {
        t = trimCopy(t.substr(1, t.size() - 2));
    }
    if (t.empty()) return true;

    bool hasUpper = false;
    bool hasDash = false;
    for (char c : t) {
        if (std::isupper(static_cast<unsigned char>(c))) hasUpper = true;
        if (c == '-' || c == ':' || c == '/' || c == '+') hasDash = true;
        if (!(std::isalnum(static_cast<unsigned char>(c)) ||
              c == '-' || c == ':' || c == '/' || c == '+' || c == '.')) {
            return false;
        }
    }
    return hasUpper || hasDash;
}

void extractLemmaStrongs(const std::string& lemmaRaw, HoverMeta& meta) {
    std::string lemma = decodeHtmlEntities(lemmaRaw);
    std::string lower = toLowerAscii(lemma);

    size_t pos = 0;
    while (true) {
        size_t found = lower.find("strong:", pos);
        if (found == std::string::npos) break;

        size_t i = found + 7;
        while (i < lemma.size() &&
               (lemma[i] == '/' || std::isspace(static_cast<unsigned char>(lemma[i])))) {
            ++i;
        }

        size_t start = i;
        if (i < lemma.size() &&
            std::isalpha(static_cast<unsigned char>(lemma[i]))) {
            ++i;
        }
        size_t digitsStart = i;
        while (i < lemma.size() &&
               std::isdigit(static_cast<unsigned char>(lemma[i]))) {
            ++i;
        }
        if (i == digitsStart) {
            pos = found + 1;
            continue;
        }
        while (i < lemma.size() &&
               std::isalpha(static_cast<unsigned char>(lemma[i]))) {
            ++i;
        }

        addStrongToken(meta, lemma.substr(start, i - start));
        pos = i;
    }
}

void extractHrefMeta(const std::string& hrefRaw,
                     HoverMeta& meta,
                     bool* isStrongLink = nullptr,
                     bool* isMorphLink = nullptr) {
    if (isStrongLink) *isStrongLink = false;
    if (isMorphLink) *isMorphLink = false;

    std::string href = trimCopy(decodeHtmlEntities(hrefRaw));
    if (href.empty()) return;
    std::string lower = toLowerAscii(href);

    auto consumeSchemeValue = [&href](size_t prefixLen) -> std::string {
        if (href.size() <= prefixLen) return "";
        std::string value = href.substr(prefixLen);
        while (!value.empty() &&
               (value.front() == '/' || std::isspace(static_cast<unsigned char>(value.front())))) {
            value.erase(value.begin());
        }
        size_t stop = 0;
        while (stop < value.size() &&
               !std::isspace(static_cast<unsigned char>(value[stop])) &&
               value[stop] != '&' && value[stop] != '?' && value[stop] != '#') {
            ++stop;
        }
        return value.substr(0, stop);
    };

    if (lower.rfind("strongs:", 0) == 0) {
        if (isStrongLink) *isStrongLink = true;
        addStrongToken(meta, consumeSchemeValue(8));
        return;
    }
    if (lower.rfind("strong:", 0) == 0) {
        if (isStrongLink) *isStrongLink = true;
        addStrongToken(meta, consumeSchemeValue(7));
        return;
    }
    if (lower.rfind("morph:", 0) == 0) {
        if (isMorphLink) *isMorphLink = true;
        addMorphToken(meta, consumeSchemeValue(6));
        return;
    }

    if (!containsNoCase(lower, "passagestudy.jsp")) return;

    if (containsNoCase(lower, "showstrongs")) {
        if (isStrongLink) *isStrongLink = true;
        std::string value = extractQueryValue(href, "value");
        std::string type = toLowerAscii(extractQueryValue(href, "type"));
        std::string prefix;
        if (type.find("ebrew") != std::string::npos) prefix = "H";
        else if (type.find("reek") != std::string::npos) prefix = "G";
        addStrongToken(meta, prefix + value);
    }

    if (containsNoCase(lower, "showmorph")) {
        if (isMorphLink) *isMorphLink = true;
        addMorphToken(meta, extractQueryValue(href, "value"));
    }
}

HoverMeta extractMetaFromWTag(const std::string& openTag) {
    HoverMeta meta;

    std::string lemma;
    if (extractAttributeValue(openTag, "lemma", lemma)) {
        extractLemmaStrongs(lemma, meta);
    }

    std::string morph;
    if (extractAttributeValue(openTag, "morph", morph)) {
        addMorphToken(meta, morph);
    }

    return meta;
}

bool parseSmallBlockMeta(const std::string& block, HoverMeta& meta) {
    bool marker =
        containsNoCase(block, "showstrongs") ||
        containsNoCase(block, "showmorph") ||
        containsNoCase(block, "class=\"strongs\"") ||
        containsNoCase(block, "class='strongs'") ||
        containsNoCase(block, "class=\"morph\"") ||
        containsNoCase(block, "class='morph'") ||
        containsNoCase(block, "strongs:") ||
        containsNoCase(block, "morph:");

    size_t pos = 0;
    while (pos < block.size()) {
        size_t aPos = findNoCase(block, "<a", pos);
        if (aPos == std::string::npos) break;

        size_t aEnd = std::string::npos;
        std::string tagName;
        bool isClosing = false;
        bool isSelfClosing = false;
        if (!parseTag(block, aPos, aEnd, tagName, isClosing, isSelfClosing)) {
            pos = aPos + 2;
            continue;
        }

        if (!isClosing && tagName == "a") {
            std::string aTag = block.substr(aPos, aEnd - aPos + 1);
            std::string href;
            if (extractAttributeValue(aTag, "href", href)) {
                bool isStrong = false;
                bool isMorph = false;
                extractHrefMeta(href, meta, &isStrong, &isMorph);
                marker = marker || isStrong || isMorph;
            }
        }

        pos = aEnd + 1;
    }

    if (marker && meta.strong.empty()) {
        std::string token = firstStrongTokenFromText(stripTags(block));
        if (!token.empty()) addStrongToken(meta, token);
    }

    if (marker && meta.morph.empty() &&
        containsNoCase(block, "morph")) {
        std::string plain = trimCopy(stripTags(block));
        if (looksLikeMorphDisplay(plain)) addMorphToken(meta, plain);
    }

    return marker;
}

bool hoverMetaHasStrong(const HoverMeta& meta, const std::string& strongsNumber) {
    if (meta.strong.empty() || strongsNumber.empty()) return false;

    std::vector<std::string> tokens;
    splitTokens(meta.strong, '|', tokens);
    for (const auto& token : tokens) {
        if (normalizeStrongsKey(token) == strongsNumber) {
            return true;
        }
    }
    return false;
}

void updateSnippetLastWordRange(const std::string& text,
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

void appendSnippetVisibleText(const std::string& rawText,
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
    updateSnippetLastWordRange(decoded, base,
                               lastWordStart, lastWordEnd, lastWordValid);
}

struct SearchSnippetData {
    std::string text;
    std::vector<bool> mask;
};

SearchSnippetData collapseSnippetWhitespace(const std::string& text,
                                            const std::vector<bool>& mask) {
    SearchSnippetData collapsed;
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

std::string buildMaskedSnippetMarkup(const std::string& text,
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

std::string buildStrongsSearchSnippet(const std::string& html,
                                      const std::string& strongsNumber,
                                      size_t maxLen = 160) {
    std::string wanted = normalizeStrongsKey(strongsNumber);
    if (wanted.empty()) return trimCopy(stripTags(html));

    std::string plain;
    std::vector<bool> mask;
    size_t lastWordStart = 0;
    size_t lastWordEnd = 0;
    bool lastWordValid = false;

    size_t pos = 0;
    while (pos < html.size()) {
        if (html[pos] != '<') {
            size_t nextTag = html.find('<', pos);
            size_t end = (nextTag == std::string::npos) ? html.size() : nextTag;
            appendSnippetVisibleText(html.substr(pos, end - pos),
                                     plain, mask,
                                     lastWordStart, lastWordEnd, lastWordValid);
            pos = end;
            continue;
        }

        size_t tagEnd = std::string::npos;
        std::string tagName;
        bool isClosing = false;
        bool isSelfClosing = false;
        if (!parseTag(html, pos, tagEnd, tagName, isClosing, isSelfClosing)) {
            ++pos;
            continue;
        }

        std::string rawTag = html.substr(pos, tagEnd - pos + 1);
        if (!isClosing && tagName == "w") {
            size_t closePos = findNoCase(html, "</w", tagEnd + 1);
            if (closePos != std::string::npos) {
                size_t closeEnd = std::string::npos;
                std::string closeName;
                bool closeIsClosing = false;
                bool closeIsSelfClosing = false;
                if (parseTag(html, closePos, closeEnd, closeName,
                             closeIsClosing, closeIsSelfClosing) &&
                    closeIsClosing && closeName == "w") {
                    HoverMeta meta = extractMetaFromWTag(rawTag);
                    std::string inner = html.substr(tagEnd + 1, closePos - (tagEnd + 1));
                    std::string innerText = stripTags(inner);
                    size_t before = plain.size();
                    appendSnippetVisibleText(innerText, plain, mask,
                                             lastWordStart, lastWordEnd, lastWordValid);
                    if (hoverMetaHasStrong(meta, wanted)) {
                        for (size_t i = before; i < plain.size() && i < mask.size(); ++i) {
                            mask[i] = true;
                        }
                    }
                    pos = closeEnd + 1;
                    continue;
                }
            }
        }

        if (!isClosing && tagName == "small") {
            size_t closePos = findNoCase(html, "</small>", tagEnd + 1);
            if (closePos != std::string::npos) {
                size_t blockEnd = closePos + 8;
                std::string block = html.substr(pos, blockEnd - pos);
                HoverMeta meta;
                if (parseSmallBlockMeta(block, meta) &&
                    lastWordValid &&
                    hoverMetaHasStrong(meta, wanted)) {
                    for (size_t i = lastWordStart;
                         i < lastWordEnd && i < mask.size(); ++i) {
                        mask[i] = true;
                    }
                }
                pos = blockEnd;
                continue;
            }
        }

        if (!isClosing && tagName == "a") {
            size_t closePos = findNoCase(html, "</a>", tagEnd + 1);
            if (closePos != std::string::npos) {
                HoverMeta meta;
                std::string href;
                bool isStrong = false;
                bool isMorph = false;
                if (extractAttributeValue(rawTag, "href", href)) {
                    extractHrefMeta(href, meta, &isStrong, &isMorph);
                }

                std::string inner = html.substr(tagEnd + 1, closePos - (tagEnd + 1));
                std::string innerText = stripTags(inner);
                std::string plainInner = trimCopy(innerText);
                bool codeText = isStrong && looksLikeStrongsDisplay(plainInner);

                size_t before = plain.size();
                appendSnippetVisibleText(innerText, plain, mask,
                                         lastWordStart, lastWordEnd, lastWordValid);
                if (!codeText && hoverMetaHasStrong(meta, wanted)) {
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

    SearchSnippetData collapsed = collapseSnippetWhitespace(plain, mask);
    if (collapsed.text.empty()) return "";

    if (std::find(collapsed.mask.begin(), collapsed.mask.end(), true) ==
        collapsed.mask.end()) {
        if (collapsed.text.size() <= maxLen) return collapsed.text;
        return collapsed.text.substr(0, maxLen) + "...";
    }

    return buildMaskedSnippetMarkup(collapsed.text, collapsed.mask, maxLen);
}

bool isInlineStrongMarkerTag(const std::string& rawTag, const std::string& tagName) {
    if (rawTag.size() < 4 || rawTag.front() != '<' || rawTag.back() != '>') return false;
    if (rawTag.find(' ') != std::string::npos ||
        rawTag.find('\t') != std::string::npos ||
        rawTag.find('\n') != std::string::npos ||
        rawTag.find('\r') != std::string::npos) {
        return false;
    }

    char firstRaw = rawTag[1];
    bool prefixed = (firstRaw == 'H' || firstRaw == 'G');
    bool digitOnly = std::isdigit(static_cast<unsigned char>(firstRaw));
    if (!prefixed && !digitOnly) return false;

    size_t i = 0;
    if (!tagName.empty() && (tagName[0] == 'h' || tagName[0] == 'g')) i = 1;
    size_t digits = 0;
    while (i < tagName.size() &&
           std::isdigit(static_cast<unsigned char>(tagName[i]))) {
        ++i;
        ++digits;
    }
    return i == tagName.size() && digits >= 1 && digits <= 6;
}

bool tryConsumeEscapedStrongMarker(const std::string& text, size_t& pos) {
    if (pos + 4 > text.size()) return false;
    if (!(text[pos] == '&' && equalsNoCase(text[pos + 1], 'l') &&
          equalsNoCase(text[pos + 2], 't') && text[pos + 3] == ';')) {
        return false;
    }

    size_t i = pos + 4;
    if (i >= text.size()) return false;

    if (text[i] == 'H' || text[i] == 'h' || text[i] == 'G' || text[i] == 'g') {
        ++i;
    }

    size_t digitsStart = i;
    while (i < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[i])) &&
           (i - digitsStart) < 6) {
        ++i;
    }
    if (i == digitsStart) return false;

    if (i + 4 <= text.size() && text[i] == '&' &&
        equalsNoCase(text[i + 1], 'g') &&
        equalsNoCase(text[i + 2], 't') &&
        text[i + 3] == ';') {
        pos = i + 4;
        return true;
    }
    return false;
}

bool tryConsumeGbfMorphMarker(const std::string& text, size_t& pos) {
    if (pos >= text.size() || text[pos] != '(') return false;
    size_t i = pos + 1;
    size_t digits = 0;
    while (i < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[i])) &&
           digits < 5) {
        ++i;
        ++digits;
    }
    if (digits < 4 || digits > 5) return false;

    if (i < text.size() &&
        std::isalpha(static_cast<unsigned char>(text[i]))) {
        ++i;
    }
    if (i < text.size() && text[i] == ')') {
        pos = i + 1;
        return true;
    }
    return false;
}

void trackLastWordInPlainText(const std::string& text,
                              size_t outStart,
                              OutputTarget& target) {
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() &&
               !isWordByte(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        if (i >= text.size()) break;
        size_t start = i;
        while (i < text.size() &&
               isWordByte(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        target.start = outStart + start;
        target.end = outStart + i;
        target.valid = true;
    }
}

void trackLastWordInHtmlFragment(const std::string& html,
                                 size_t outStart,
                                 OutputTarget& target) {
    bool inTag = false;
    size_t wordStart = 0;
    bool inWord = false;

    for (size_t i = 0; i < html.size(); ++i) {
        char c = html[i];
        if (c == '<') {
            if (inWord) {
                target.start = outStart + wordStart;
                target.end = outStart + i;
                target.valid = true;
                inWord = false;
            }
            inTag = true;
            continue;
        }
        if (c == '>') {
            inTag = false;
            continue;
        }
        if (inTag) continue;

        if (isWordByte(static_cast<unsigned char>(c))) {
            if (!inWord) {
                inWord = true;
                wordStart = i;
            }
        } else if (inWord) {
            target.start = outStart + wordStart;
            target.end = outStart + i;
            target.valid = true;
            inWord = false;
        }
    }

    if (inWord) {
        target.start = outStart + wordStart;
        target.end = outStart + html.size();
        target.valid = true;
    }
}

void appendSanitizedText(std::string& out,
                         const std::string& text,
                         OutputTarget& target) {
    std::string cleaned;
    cleaned.reserve(text.size());

    for (size_t i = 0; i < text.size();) {
        size_t cursor = i;
        if (tryConsumeEscapedStrongMarker(text, cursor) ||
            tryConsumeGbfMorphMarker(text, cursor)) {
            cleaned.push_back(' ');
            i = cursor;
            continue;
        }
        cleaned.push_back(text[i]);
        ++i;
    }

    size_t outStart = out.size();
    out += cleaned;
    trackLastWordInPlainText(cleaned, outStart, target);
}

void mergeMeta(HoverMeta& base, const HoverMeta& update) {
    if (!update.strong.empty()) appendUniqueTokens(base.strong, update.strong, '|');
    if (!update.morph.empty()) appendUniqueTokens(base.morph, update.morph, ' ');
}

void applyMetaToTarget(std::string& out,
                       OutputTarget& target,
                       const HoverMeta& meta) {
    if (!target.valid || target.end <= target.start || meta.empty()) return;
    if (target.end > out.size()) {
        target.valid = false;
        return;
    }

    const std::string prefix = R"(<span class="w")";
    bool looksWrapped =
        target.end - target.start >= prefix.size() + 7 &&
        out.compare(target.start, prefix.size(), prefix) == 0 &&
        out.compare(target.end - 7, 7, "</span>") == 0;

    if (looksWrapped) {
        size_t openEnd = out.find('>', target.start);
        if (openEnd != std::string::npos && openEnd < target.end) {
            std::string openTag = out.substr(target.start, openEnd - target.start + 1);
            HoverMeta merged;
            std::string existingStrong;
            std::string existingMorph;
            if (extractAttributeValue(openTag, "data-strong", existingStrong)) {
                merged.strong = decodeHtmlEntities(existingStrong);
            }
            if (extractAttributeValue(openTag, "data-morph", existingMorph)) {
                merged.morph = decodeHtmlEntities(existingMorph);
            }
            mergeMeta(merged, meta);

            std::string newOpen = buildWordSpanOpenTag(merged);
            out.replace(target.start, openEnd - target.start + 1, newOpen);
            ptrdiff_t delta = static_cast<ptrdiff_t>(newOpen.size()) -
                              static_cast<ptrdiff_t>(openEnd - target.start + 1);
            target.end = static_cast<size_t>(
                static_cast<ptrdiff_t>(target.end) + delta);
            return;
        }
    }

    std::string inner = out.substr(target.start, target.end - target.start);
    std::string wrapped = buildWordSpanOpenTag(meta) + inner + "</span>";
    out.replace(target.start, target.end - target.start, wrapped);
    target.end = target.start + wrapped.size();
    target.valid = true;
}

std::string collapseSpacesOutsideTags(const std::string& html) {
    std::string out;
    out.reserve(html.size());
    bool inTag = false;
    bool prevSpace = false;
    for (char c : html) {
        if (c == '<') {
            inTag = true;
            prevSpace = false;
            out.push_back(c);
            continue;
        }
        if (c == '>') {
            inTag = false;
            prevSpace = false;
            out.push_back(c);
            continue;
        }
        if (!inTag && c == ' ') {
            if (prevSpace) continue;
            prevSpace = true;
            out.push_back(c);
            continue;
        }
        prevSpace = false;
        out.push_back(c);
    }
    return out;
}

bool mayContainMorphOrStrongsMarkup(const std::string& html) {
    return
        html.find("<small") != std::string::npos ||
        html.find("<SMALL") != std::string::npos ||
        html.find("passagestudy.jsp") != std::string::npos ||
        html.find("<w") != std::string::npos ||
        html.find("<W") != std::string::npos ||
        html.find("strongs:") != std::string::npos ||
        html.find("STRONGS:") != std::string::npos ||
        html.find("morph:") != std::string::npos ||
        html.find("MORPH:") != std::string::npos ||
        html.find("&lt;H") != std::string::npos ||
        html.find("&lt;G") != std::string::npos ||
        html.find("&lt;h") != std::string::npos ||
        html.find("&lt;g") != std::string::npos ||
        html.find("&lt;0") != std::string::npos ||
        html.find("&lt;1") != std::string::npos ||
        html.find("&lt;2") != std::string::npos ||
        html.find("&lt;3") != std::string::npos ||
        html.find("&lt;4") != std::string::npos ||
        html.find("&lt;5") != std::string::npos ||
        html.find("&lt;6") != std::string::npos ||
        html.find("&lt;7") != std::string::npos ||
        html.find("&lt;8") != std::string::npos ||
        html.find("&lt;9") != std::string::npos;
}

bool isParallelInlineTag(const std::string& tagName) {
    return
        tagName == "a" ||
        tagName == "span" ||
        tagName == "sup" ||
        tagName == "sub" ||
        tagName == "i" ||
        tagName == "b" ||
        tagName == "em" ||
        tagName == "strong" ||
        tagName == "small" ||
        tagName == "u" ||
        tagName == "font" ||
        tagName == "mark";
}

bool isParallelSpacerTag(const std::string& tagName) {
    return
        tagName == "div" ||
        tagName == "p" ||
        tagName == "li" ||
        tagName == "tr" ||
        tagName == "table" ||
        tagName == "tbody" ||
        tagName == "thead" ||
        tagName == "tfoot" ||
        tagName == "td" ||
        tagName == "th" ||
        tagName == "ul" ||
        tagName == "ol" ||
        tagName == "h1" ||
        tagName == "h2" ||
        tagName == "h3" ||
        tagName == "h4" ||
        tagName == "h5" ||
        tagName == "h6";
}

bool hasOpenTag(const std::vector<std::string>& openTags,
                const std::string& tagName) {
    for (auto it = openTags.rbegin(); it != openTags.rend(); ++it) {
        if (*it == tagName) return true;
    }
    return false;
}

void eraseLastOpenTag(std::vector<std::string>& openTags,
                      const std::string& tagName) {
    for (auto it = openTags.rbegin(); it != openTags.rend(); ++it) {
        if (*it == tagName) {
            openTags.erase(std::next(it).base());
            return;
        }
    }
}

std::string sanitizeParallelVerseHtml(const std::string& html) {
    if (html.empty()) return html;

    std::string out;
    out.reserve(html.size() + 16);
    std::vector<std::string> openTags;

    size_t pos = 0;
    while (pos < html.size()) {
        if (html[pos] != '<') {
            out.push_back(html[pos]);
            ++pos;
            continue;
        }

        size_t tagEnd = std::string::npos;
        std::string tagName;
        bool isClosing = false;
        bool isSelfClosing = false;
        if (!parseTag(html, pos, tagEnd, tagName, isClosing, isSelfClosing)) {
            out.push_back(html[pos]);
            ++pos;
            continue;
        }

        std::string rawTag = html.substr(pos, tagEnd - pos + 1);

        if (tagName == "br") {
            out += "<br/>";
            pos = tagEnd + 1;
            continue;
        }

        if (isParallelInlineTag(tagName)) {
            if (isClosing) {
                if (hasOpenTag(openTags, tagName)) {
                    out += rawTag;
                    eraseLastOpenTag(openTags, tagName);
                }
            } else {
                out += rawTag;
                if (!isSelfClosing) {
                    openTags.push_back(tagName);
                }
            }
            pos = tagEnd + 1;
            continue;
        }

        if (isParallelSpacerTag(tagName)) {
            out.push_back(' ');
        }

        pos = tagEnd + 1;
    }

    while (!openTags.empty()) {
        out += "</";
        out += openTags.back();
        out += ">";
        openTags.pop_back();
    }

    return collapseSpacesOutsideTags(out);
}

bool looksLikeHtmlMarkup(const std::string& text) {
    static const std::regex tagRe(
        R"(<\s*/?\s*(?:p|div|br|hr|ul|ol|li|strong|b|em|i|small|span|a)\b)",
        std::regex::icase);
    return std::regex_search(text, tagRe);
}

std::string plainTextToHtml(const std::string& text) {
    std::string trimmed = trimCopy(text);
    if (trimmed.empty()) return "";

    std::ostringstream html;
    std::istringstream input(text);
    std::string line;
    bool inParagraph = false;
    bool firstLineInParagraph = true;

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (trimCopy(line).empty()) {
            if (inParagraph) {
                html << "</p>\n";
                inParagraph = false;
            }
            firstLineInParagraph = true;
            continue;
        }

        if (!inParagraph) {
            html << "<p>";
            inParagraph = true;
            firstLineInParagraph = true;
        }

        if (!firstLineInParagraph) html << "<br/>";
        html << htmlEscapeAttr(line);
        firstLineInParagraph = false;
    }

    if (inParagraph) html << "</p>\n";
    return html.str();
}

std::string commentaryEntryHtml(sword::SWModule* mod) {
    if (!mod) return "";

    std::string raw;
    if (mod->isWritable()) {
        const char* rawEntry = mod->getRawEntry();
        raw = rawEntry ? rawEntry : "";
        if (looksLikeHtmlMarkup(raw)) {
            return raw;
        }
    }

    std::string rendered = std::string(mod->renderText().c_str());
    if (!trimCopy(rendered).empty()) return rendered;
    if (!trimCopy(raw).empty()) return plainTextToHtml(raw);
    return "";
}

} // namespace

SwordManager::SwordManager() = default;
SwordManager::~SwordManager() = default;

bool SwordManager::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        // Create SWORD manager with XHTML markup filter
        mgr_ = std::make_unique<sword::SWMgr>(
            new sword::MarkupFilterMgr(sword::FMT_XHTML));

        if (!mgr_) {
            return false;
        }

        // Enable global options for Strong's numbers and morphology
        mgr_->setGlobalOption("Strong's Numbers", "On");
        mgr_->setGlobalOption("Morphological Tags", "On");
        mgr_->setGlobalOption("Lemmas", "On");
        mgr_->setGlobalOption("Headings", "On");
        mgr_->setGlobalOption("Footnotes", "On");
        mgr_->setGlobalOption("Cross-references", "On");
        mgr_->setGlobalOption("Words of Christ in Red", "On");

        return true;
    } catch (...) {
        return false;
    }
}

std::vector<ModuleInfo> SwordManager::getModules() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ModuleInfo> modules;

    if (!mgr_) return modules;

    for (auto it = mgr_->Modules.begin(); it != mgr_->Modules.end(); ++it) {
        modules.push_back(buildModuleInfo(it->second));
    }

    std::sort(modules.begin(), modules.end(),
              [](const ModuleInfo& a, const ModuleInfo& b) {
                  return a.name < b.name;
              });

    return modules;
}

std::vector<ModuleInfo> SwordManager::getModulesByType(const std::string& type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ModuleInfo> modules;

    if (!mgr_) return modules;

    for (auto it = mgr_->Modules.begin(); it != mgr_->Modules.end(); ++it) {
        sword::SWModule* mod = it->second;
        if (mod->getType() == type) {
            modules.push_back(buildModuleInfo(mod));
        }
    }

    std::sort(modules.begin(), modules.end(),
              [](const ModuleInfo& a, const ModuleInfo& b) {
                  return a.name < b.name;
              });

    return modules;
}

std::vector<ModuleInfo> SwordManager::getBibleModules() const {
    return getModulesByType("Biblical Texts");
}

std::vector<ModuleInfo> SwordManager::getCommentaryModules() const {
    return getModulesByType("Commentaries");
}

std::vector<ModuleInfo> SwordManager::getDictionaryModules() const {
    return getModulesByType("Lexicons / Dictionaries");
}

std::vector<ModuleInfo> SwordManager::getGeneralBookModules() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ModuleInfo> modules;

    if (!mgr_) return modules;

    for (auto it = mgr_->Modules.begin(); it != mgr_->Modules.end(); ++it) {
        sword::SWModule* mod = it->second;
        if (!mod) continue;

        const char* typeRaw = mod->getType();
        std::string type = typeRaw ? typeRaw : "";
        std::string lower = toLowerAscii(type);
        bool isGeneralBook =
            lower == "genbook" ||
            containsNoCase(lower, "genbook") ||
            containsNoCase(lower, "general book") ||
            containsNoCase(lower, "generic book");
        if (isGeneralBook) {
            modules.push_back(buildModuleInfo(mod));
        }
    }

    std::sort(modules.begin(), modules.end(),
              [](const ModuleInfo& a, const ModuleInfo& b) {
                  return a.name < b.name;
              });

    return modules;
}

std::string SwordManager::getVerseText(const std::string& moduleName,
                                        const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return "<p><i>Module not found: " + moduleName + "</i></p>";

    mod->setKey(key.c_str());
    std::string text = std::string(mod->renderText().c_str());

    if (text.empty()) {
        return "<p><i>No text available for " + key + "</i></p>";
    }

    text = postProcessHtml(text);

    VerseRef ref;
    try {
        ref = parseVerseRef(key);
    } catch (...) {
        ref = VerseRef{};
    }

    std::ostringstream html;
    html << "<div class=\"chapter\">\n";
    if (ref.verse > 0) {
        html << "<div class=\"verse\" id=\"v" << ref.verse << "\">";
        html << "<a class=\"versenum-link\" href=\"verse:" << ref.verse << "\">"
             << "<sup class=\"versenum\">" << ref.verse << "</sup></a> ";
    } else {
        html << "<div class=\"verse\">";
    }
    html << text;
    html << "</div>\n</div>\n";

    return html.str();
}

std::string SwordManager::getChapterText(const std::string& moduleName,
                                          const std::string& book,
                                          int chapter,
                                          bool paragraphMode,
                                          int selectedVerse,
                                          VerseDecorationCallback verseDecorator) {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return "<p><i>Module not found: " + moduleName + "</i></p>";

    sword::VerseKey* vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    if (!vk) {
        // Clone a verse key
        sword::VerseKey tempKey;
        mod->setKey(tempKey);
        vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    }
    if (!vk) return "<p><i>Cannot create verse key</i></p>";

    std::ostringstream html;
    html << "<div class=\"chapter\">\n";

    // Set to the beginning of the chapter
    std::string ref = book + " " + std::to_string(chapter) + ":1";
    vk->setText(ref.c_str());

    int currentChapter = vk->getChapter();

    // Choose element tag based on display mode:
    // verse-per-line (default) uses <div>, paragraph mode uses <span>
    const char* verseTag = paragraphMode ? "span" : "div";

    auto tryGetVerseHtmlCache =
        [this](const std::string& key, std::string& valueOut) -> bool {
        auto it = verseHtmlCache_.find(key);
        if (it == verseHtmlCache_.end()) return false;

        verseHtmlLru_.splice(verseHtmlLru_.begin(),
                             verseHtmlLru_,
                             it->second.lruIt);
        valueOut = it->second.value;
        return true;
    };

    auto storeVerseHtmlCache =
        [this](const std::string& key, const std::string& value) {
        auto it = verseHtmlCache_.find(key);
        if (it != verseHtmlCache_.end()) {
            it->second.value = value;
            verseHtmlLru_.splice(verseHtmlLru_.begin(),
                                 verseHtmlLru_,
                                 it->second.lruIt);
            return;
        }

        verseHtmlLru_.push_front(key);
        verseHtmlCache_.emplace(key, VerseHtmlCacheEntry{
            value, verseHtmlLru_.begin()
        });

        if (verseHtmlCache_.size() > kVerseHtmlCacheLimit) {
            const std::string& evictKey = verseHtmlLru_.back();
            verseHtmlCache_.erase(evictKey);
            verseHtmlLru_.pop_back();
        }
    };

    while (!mod->popError() && vk->getChapter() == currentChapter) {
        int verse = vk->getVerse();
        std::string verseRef = book + " " + std::to_string(chapter) +
                               ":" + std::to_string(verse);
        std::string cacheKey = moduleName + "|" + verseRef;
        std::string verseText;

        if (!tryGetVerseHtmlCache(cacheKey, verseText)) {
            verseText = std::string(mod->renderText().c_str());
            if (!verseText.empty()) {
                verseText = postProcessHtml(verseText);
                storeVerseHtmlCache(cacheKey, verseText);
            }
        }

        if (!verseText.empty()) {
            std::string verseClass = "verse";
            if (selectedVerse > 0 && verse == selectedVerse) {
                verseClass += " verse-selected";
            }
            // In paragraph mode, a verse whose first visible content is ¶ (U+00B6,
            // UTF-8: 0xC2 0xB6) marks a paragraph boundary — whether the pilcrow is
            // at position 0 or after an opening HTML tag (e.g. <span class="wordsOfJesus">).
            // Emit a line break before the verse span so the paragraph starts on its own line.
            if (paragraphMode && verse > 1) {
                size_t pos = 0;
                // Skip any leading HTML tags and whitespace.
                while (pos < verseText.size()) {
                    if (verseText[pos] == '<') {
                        size_t close = verseText.find('>', pos);
                        if (close == std::string::npos) break;
                        pos = close + 1;
                    } else if (verseText[pos] == ' ' || verseText[pos] == '\t' ||
                               verseText[pos] == '\n' || verseText[pos] == '\r') {
                        ++pos;
                    } else {
                        break;
                    }
                }
                if (pos + 1 < verseText.size() &&
                    (unsigned char)verseText[pos]   == 0xC2 &&
                    (unsigned char)verseText[pos+1] == 0xB6) {
                    html << "<br><br>\n";
                }
            }
            html << "<" << verseTag << " class=\"" << verseClass
                 << "\" id=\"v" << verse << "\">";
            html << "<a class=\"versenum-link\" href=\"verse:" << verse << "\">"
                 << "<sup class=\"versenum\">" << verse << "</sup></a> ";
            html << verseText;
            if (verseDecorator) {
                html << verseDecorator(verseRef);
            }
            html << "</" << verseTag << ">\n";
        }

        (*mod)++;  // Advance to next verse
    }

    html << "</div>\n";
    return html.str();
}

std::string SwordManager::getParallelText(
    const std::vector<std::string>& moduleNames,
    const std::string& book, int chapter,
    bool paragraphMode,
    int selectedVerse,
    VerseDecorationCallback verseDecorator) {

    (void)paragraphMode; // Parallel view uses column layout; mode not applicable
    if (moduleNames.empty()) return "";

    std::lock_guard<std::mutex> lock(mutex_);

    // Determine verse count from first valid module
    int verseCount = 0;
    for (const auto& modName : moduleNames) {
        sword::SWModule* mod = getModule(modName);
        if (mod) {
            sword::VerseKey* vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
            if (!vk) {
                sword::VerseKey tempKey;
                mod->setKey(tempKey);
                vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
            }
            if (vk) {
                std::string ref = book + " " + std::to_string(chapter) + ":1";
                vk->setText(ref.c_str());
                if (!mod->popError()) {
                    // O(1) count; avoids per-verse cursor walk every render.
                    verseCount = vk->getVerseMax();
                }
            }
            break;
        }
    }

    if (verseCount == 0) verseCount = 31; // fallback

    // Column width split for inline-block parallel layout.
    int numCols = static_cast<int>(moduleNames.size());
    int colWidth = 100 / numCols;
    int lastColWidth = 100 - colWidth * (numCols - 1);

    std::ostringstream html;
    html << "<div class=\"parallel\">\n";

    auto tryGetVerseHtmlCache =
        [this](const std::string& key, std::string& valueOut) -> bool {
        auto it = verseHtmlCache_.find(key);
        if (it == verseHtmlCache_.end()) return false;

        verseHtmlLru_.splice(verseHtmlLru_.begin(),
                             verseHtmlLru_,
                             it->second.lruIt);
        valueOut = it->second.value;
        return true;
    };

    auto storeVerseHtmlCache =
        [this](const std::string& key, const std::string& value) {
        auto it = verseHtmlCache_.find(key);
        if (it != verseHtmlCache_.end()) {
            it->second.value = value;
            verseHtmlLru_.splice(verseHtmlLru_.begin(),
                                 verseHtmlLru_,
                                 it->second.lruIt);
            return;
        }

        verseHtmlLru_.push_front(key);
        verseHtmlCache_.emplace(key, VerseHtmlCacheEntry{
            value, verseHtmlLru_.begin()
        });

        if (verseHtmlCache_.size() > kVerseHtmlCacheLimit) {
            const std::string& evictKey = verseHtmlLru_.back();
            verseHtmlCache_.erase(evictKey);
            verseHtmlLru_.pop_back();
        }
    };

    // Verse rows
    for (int v = 1; v <= verseCount; ++v) {
        const std::string verseRef = book + " " + std::to_string(chapter)
                                     + ":" + std::to_string(v);
        html << "<div class=\"parallel-row\" id=\"v" << v << "\">\n";
        for (size_t i = 0; i < moduleNames.size(); ++i) {
            bool isLast = (i + 1 == moduleNames.size());
            int w = isLast ? lastColWidth : colWidth;
            const char* colClass = isLast ? "parallel-col-last" : "parallel-col";
            std::string cellClasses = isLast ? "parallel-cell-last" : "parallel-cell";
            if (selectedVerse > 0 && v == selectedVerse) {
                cellClasses += " verse-selected";
            }
            const std::string moduleAttr = htmlEscapeAttr(moduleNames[i]);
            sword::SWModule* mod = getModule(moduleNames[i]);
            html << "<div class=\"" << colClass << "\" data-module=\""
                 << moduleAttr << "\" style=\"width: " << w << "%;\">"
                 << "<div class=\"" << cellClasses << "\" data-module=\""
                 << moduleAttr << "\">";
            if (mod) {
                mod->setKey(verseRef.c_str());
                if (!mod->popError()) {
                    std::string cacheKey = moduleNames[i] + "|" + verseRef;
                    std::string verseText;
                    if (!tryGetVerseHtmlCache(cacheKey, verseText)) {
                        verseText = std::string(mod->renderText().c_str());
                        if (!verseText.empty()) {
                            verseText = postProcessHtml(verseText);
                            storeVerseHtmlCache(cacheKey, verseText);
                        }
                    }
                    verseText = sanitizeParallelVerseHtml(verseText);
                    html << "<a class=\"versenum-link\" href=\"verse:" << v << "\">"
                         << "<sup class=\"versenum\">" << v << "</sup></a> ";
                    html << verseText;
                }
            }
            if (isLast && verseDecorator) {
                html << verseDecorator(verseRef);
            }
            html << "</div></div>\n";
        }
        html << "</div>\n";
    }

    html << "</div>\n";
    return html.str();
}

std::string SwordManager::getCommentaryText(const std::string& moduleName,
                                             const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return "<p><i>Commentary module not found: " + moduleName + "</i></p>";

    VerseRef ref;
    bool hasChapterContext = false;
    try {
        ref = parseVerseRef(key);
        hasChapterContext = !ref.book.empty() && ref.chapter > 0;
    } catch (...) {
        ref = VerseRef{};
    }

    if (!hasChapterContext) {
        mod->setKey(key.c_str());
        std::string text = commentaryEntryHtml(mod);

        if (text.empty()) {
            return "<p><i>No commentary available for " + key + "</i></p>";
        }

        std::ostringstream html;
        html << "<div class=\"commentary\">\n";
        html << "<h3>" << key << "</h3>\n";
        html << text;
        html << "</div>\n";
        return html.str();
    }

    sword::VerseKey* vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    if (!vk) {
        sword::VerseKey tempKey;
        mod->setKey(tempKey);
        vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    }
    if (!vk) {
        mod->setKey(key.c_str());
        std::string text = commentaryEntryHtml(mod);
        if (text.empty()) {
            return "<p><i>No commentary available for " + key + "</i></p>";
        }
        std::ostringstream html;
        html << "<div class=\"commentary\">\n";
        html << "<h3>" << key << "</h3>\n";
        html << text;
        html << "</div>\n";
        return html.str();
    }

    std::string startRef = ref.book + " " + std::to_string(ref.chapter) + ":1";
    vk->setText(startRef.c_str());
    if (mod->popError()) {
        return "<p><i>No commentary available for " + key + "</i></p>";
    }

    int chapter = vk->getChapter();
    std::ostringstream html;
    html << "<div class=\"commentary\">\n";
    html << "<h3>" << htmlEscapeAttr(ref.book) << " " << ref.chapter << "</h3>\n";

    while (!mod->popError() && vk->getChapter() == chapter) {
        int verse = vk->getVerse();
        std::string verseText = commentaryEntryHtml(mod);
        if (verse > 1) {
            html << "<hr class=\"commentary-sep\"/>\n";
        }
        html << "<div class=\"commentary-verse\" id=\"v" << verse << "\">";
        html << "<div class=\"commentary-gutter\">"
             << "<a class=\"versenum-link\" href=\"verse:" << verse << "\">"
             << "<sup class=\"versenum\">" << verse << "</sup></a>"
             << "</div>";
        html << "<div class=\"commentary-text\">";
        if (!trimCopy(verseText).empty()) {
            html << verseText;
        } else {
            html << "<span class=\"commentary-empty\"></span>";
        }
        html << "</div>";
        html << "</div>\n";
        (*mod)++;
    }

    html << "</div>\n";
    return html.str();
}

bool SwordManager::moduleIsWritable(const std::string& moduleName) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    return mod && mod->isWritable();
}

std::string SwordManager::getRawEntry(const std::string& moduleName,
                                      const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return "";

    mod->setKey(key.c_str());
    if (mod->popError()) return "";

    const char* raw = mod->getRawEntry();
    return raw ? raw : "";
}

bool SwordManager::setRawEntry(const std::string& moduleName,
                               const std::string& key,
                               const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod || !mod->isWritable()) return false;

    mod->setKey(key.c_str());
    if (mod->popError()) return false;

    if (trimCopy(text).empty()) {
        mod->deleteEntry();
    } else {
        mod->setEntry(text.c_str());
    }
    return !mod->popError();
}

bool SwordManager::deleteEntry(const std::string& moduleName,
                               const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod || !mod->isWritable()) return false;

    mod->setKey(key.c_str());
    if (mod->popError()) return false;

    mod->deleteEntry();
    return !mod->popError();
}

std::string SwordManager::getDictionaryEntry(const std::string& moduleName,
                                              const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return "<p><i>Dictionary module not found: " + moduleName + "</i></p>";

    std::string requestedKey = trimCopy(key);
    std::string displayKey = requestedKey;
    std::string text;

    std::string normalizedStrong = normalizeStrongsKey(requestedKey);
    std::vector<std::string> lookupKeys;
    if (!normalizedStrong.empty()) {
        displayKey = normalizedStrong;
        char prefix = strongPrefixFromKey(normalizedStrong);
        lookupKeys = strongLookupKeys(normalizedStrong, prefix);
    } else if (!requestedKey.empty()) {
        lookupKeys.push_back(requestedKey);
    }

    std::string resolvedKey;
    for (const auto& lookupKey : lookupKeys) {
        text = renderLexiconEntryHtml(mod, requestedKey, lookupKey, &resolvedKey);
        if (!text.empty()) break;
    }

    if (text.empty() && !requestedKey.empty() && lookupKeys.empty()) {
        text = renderLexiconEntryHtml(mod, requestedKey, requestedKey, &resolvedKey);
    }

    if (text.empty() && !resolvedKey.empty()) {
        displayKey = resolvedKey;
    }

    if (text.empty()) {
        return "<p><i>No entry found for: " + htmlEscapeAttr(requestedKey) + "</i></p>";
    }

    std::ostringstream html;
    html << "<div class=\"dictionary\">\n";
    html << "<div class=\"entry-key\">" << htmlEscapeAttr(displayKey) << "</div>\n";
    html << text;
    html << "</div>\n";
    return html.str();
}

std::string SwordManager::getGeneralBookEntry(const std::string& moduleName,
                                              const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return "<p><i>General book module not found: " + moduleName + "</i></p>";

    std::string lookupKey = trimCopy(key);
    if (lookupKey.empty()) {
        std::ostringstream html;
        html << "<div class=\"general-book\">\n";
        html << "<div class=\"entry-key\">" << htmlEscapeAttr(moduleName) << "</div>\n";
        const char* desc = mod->getDescription();
        if (desc && *desc) {
            html << "<p>" << desc << "</p>\n";
        }
        html << "<p><i>Enter a key and press Go to open an entry.</i></p>\n";
        html << "</div>\n";
        return html.str();
    }

    mod->setKey(lookupKey.c_str());
    std::string text = std::string(mod->renderText().c_str());
    if (text.empty()) {
        return "<p><i>No entry found for: " + lookupKey + "</i></p>";
    }

    std::ostringstream html;
    html << "<div class=\"general-book\">\n";
    html << "<div class=\"entry-key\">" << htmlEscapeAttr(lookupKey) << "</div>\n";
    html << text;
    html << "</div>\n";
    return html.str();
}

std::vector<SearchResult> SwordManager::search(
    const std::string& moduleName,
    const std::string& searchText,
    int searchType,
    const std::string& scope,
    std::function<void(float)> callback) {

    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SearchResult> results;

    if (moduleName.empty() || searchText.empty()) return results;

    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return results;

    // Set up scope key if provided
    sword::ListKey scopeKey;
    sword::SWKey* scopePtr = nullptr;
    if (!scope.empty()) {
        sword::VerseKey vk;
        scopeKey = vk.parseVerseList(scope.c_str(), "", true);
        scopePtr = &scopeKey;
    }

    // Perform search
    sword::ListKey& resultKeys = mod->search(
        searchText.c_str(), searchType, 0, scopePtr,
        nullptr,  // percent update function
        nullptr   // percent user data
    );

    // Collect results
    for (resultKeys = sword::TOP; !resultKeys.popError(); resultKeys++) {
        const char* keyText = resultKeys.getText();
        if (!keyText || !*keyText) {
            continue;
        }

        SearchResult result;
        result.key = keyText;
        result.module = moduleName;

        // Get preview text
        mod->setKey(keyText);
        const char* preview = mod->stripText();
        result.text = preview ? preview : "";

        // Truncate long preview
        if (result.text.length() > 200) {
            result.text = result.text.substr(0, 200) + "...";
        }

        results.push_back(result);
    }

    return results;
}

std::vector<SearchResult> SwordManager::searchStrongs(
    const std::string& moduleName,
    const std::string& strongsNumber) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SearchResult> results;

    std::string normalizedStrong = normalizeStrongsKey(strongsNumber);
    if (moduleName.empty() || normalizedStrong.empty()) return results;

    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return results;

    std::string query = "Word//Lemma./" + normalizedStrong;
    sword::ListKey& resultKeys = mod->search(query.c_str(), -2, 0, nullptr, nullptr, nullptr);

    for (resultKeys = sword::TOP; !resultKeys.popError(); resultKeys++) {
        const char* keyText = resultKeys.getText();
        if (!keyText || !*keyText) continue;

        SearchResult result;
        result.key = keyText;
        result.module = moduleName;

        mod->setKey(keyText);
        std::string rendered = std::string(mod->renderText().c_str());
        result.text = buildStrongsSearchSnippet(rendered, normalizedStrong);
        if (result.text.empty()) {
            const char* preview = mod->stripText();
            result.text = preview ? preview : "";
            if (result.text.size() > 200) {
                result.text = result.text.substr(0, 200) + "...";
            }
        }

        results.push_back(std::move(result));
    }

    return results;
}

WordInfo SwordManager::getWordInfo(const std::string& moduleName,
                                    const std::string& verseKey,
                                    const std::string& word) {
    WordInfo info;
    info.word = word;

    // First phase: extract Strong's/morph codes under the lock
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sword::SWModule* mod = getModule(moduleName);
        if (!mod) return info;

        // Render the verse to populate entry attributes
        mod->setKey(verseKey.c_str());
        mod->renderText();

        // Search through word attributes to find matching word
        auto& attrs = mod->getEntryAttributes();
        auto wordIt = attrs.find("Word");
        if (wordIt != attrs.end()) {
            for (auto& entry : wordIt->second) {
                auto& wordAttrs = entry.second;

                auto textIt = wordAttrs.find("Text");
                if (textIt != wordAttrs.end()) {
                    std::string attrWord = textIt->second.c_str();
                    std::string wordLower = word;
                    std::string attrLower = attrWord;
                    std::transform(wordLower.begin(), wordLower.end(),
                                   wordLower.begin(), ::tolower);
                    std::transform(attrLower.begin(), attrLower.end(),
                                   attrLower.begin(), ::tolower);

                    if (wordLower == attrLower) {
                        auto lemmaIt = wordAttrs.find("Lemma");
                        if (lemmaIt != wordAttrs.end()) {
                            info.strongsNumber = lemmaIt->second.c_str();
                            auto colonPos = info.strongsNumber.find(':');
                            if (colonPos != std::string::npos) {
                                info.strongsNumber = info.strongsNumber.substr(colonPos + 1);
                            }
                        }

                        auto morphIt = wordAttrs.find("Morph");
                        if (morphIt != wordAttrs.end()) {
                            info.morphCode = morphIt->second.c_str();
                            auto colonPos = info.morphCode.find(':');
                            if (colonPos != std::string::npos) {
                                info.morphCode = info.morphCode.substr(colonPos + 1);
                            }
                        }

                        break;
                    }
                }
            }
        }
    } // lock released here

    // Second phase: look up definitions without holding the lock
    if (!info.strongsNumber.empty()) {
        info.strongsDef = getStrongsDefinition(info.strongsNumber);
    }

    if (!info.morphCode.empty()) {
        info.morphDef = getMorphDefinition(info.morphCode);
    }

    return info;
}

std::string SwordManager::getStrongsDefinition(
        const std::string& strongsNumber,
        const std::vector<std::string>& preferredLexicons) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mgr_) return "";

    std::string key = normalizeStrongsKey(strongsNumber);
    if (key.empty()) return "";

    char prefix = strongPrefixFromKey(key);
    std::vector<std::string> lexicons =
        strongLexiconsForPrefix(prefix, preferredLexicons);
    std::vector<std::string> lookupKeys = strongLookupKeys(key, prefix);

    std::string fallbackWithoutCjk;
    for (const auto& lexName : lexicons) {
        sword::SWModule* lex = getModule(lexName);
        if (!lex) continue;
        for (const auto& lookupKey : lookupKeys) {
            std::string text = readLexiconEntry(lex, lookupKey);
            if (text.empty()) continue;
            if (!containsCjkText(text)) return text;
            if (fallbackWithoutCjk.empty()) {
                std::string stripped = stripCjkText(text);
                if (!stripped.empty()) fallbackWithoutCjk = stripped;
            }
        }
    }

    return fallbackWithoutCjk;
}

std::string SwordManager::getStrongsLemma(
        const std::string& strongsNumber,
        const std::vector<std::string>& preferredLexicons) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mgr_) return "";

    std::string key = normalizeStrongsKey(strongsNumber);
    if (key.empty()) return "";

    char prefix = strongPrefixFromKey(key);
    std::vector<std::string> lexicons =
        strongLexiconsForPrefix(prefix, preferredLexicons);
    std::vector<std::string> lookupKeys = strongLookupKeys(key, prefix);

    for (const auto& lexName : lexicons) {
        sword::SWModule* lex = getModule(lexName);
        if (!lex) continue;
        for (const auto& lookupKey : lookupKeys) {
            std::string text = readLexiconEntry(lex, lookupKey);
            if (text.empty()) continue;
            if (containsCjkText(text)) {
                text = stripCjkText(text);
                if (text.empty()) continue;
            }
            std::string lemma = extractStrongsLemmaFromDefinition(text, prefix);
            if (!lemma.empty()) return lemma;
        }
    }

    return "";
}

std::string SwordManager::getMorphDefinition(const std::string& morphCode) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mgr_) return "";

    std::string key = trimCopy(morphCode);
    if (key.empty()) return "";

    std::string fallbackWithoutCjk;
    // Try morphology lexicons
    std::vector<std::string> morphLexicons = {
        "Robinson", "Packard", "OSHM"
    };

    for (const auto& lexName : morphLexicons) {
        sword::SWModule* lex = getModule(lexName);
        if (lex) {
            std::string text = readLexiconEntry(lex, key);
            if (text.empty()) continue;
            if (!containsCjkText(text)) return text;
            if (fallbackWithoutCjk.empty()) {
                std::string stripped = stripCjkText(text);
                if (!stripped.empty()) fallbackWithoutCjk = stripped;
            }
        }
    }

    return fallbackWithoutCjk;
}

std::vector<std::string> SwordManager::getBookNames(const std::string& moduleName) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> books;

    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return books;

    sword::VerseKey* vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    if (!vk) {
        sword::VerseKey tempKey;
        mod->setKey(tempKey);
        vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    }
    if (!vk) return books;

    for (int t = 0; t < 2; t++) {  // Old and New Testament
        vk->setTestament(t + 1);
        for (int b = 1; b <= vk->getBookMax(); b++) {
            vk->setBook(b);
            books.push_back(vk->getBookName());
        }
    }

    return books;
}

int SwordManager::getChapterCount(const std::string& moduleName,
                                   const std::string& book) {
    std::lock_guard<std::mutex> lock(mutex_);

    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return 0;

    sword::VerseKey* vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    if (!vk) {
        sword::VerseKey tempKey;
        mod->setKey(tempKey);
        vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    }
    if (!vk) return 0;

    std::string ref = book + " 1:1";
    vk->setText(ref.c_str());

    return vk->getChapterMax();
}

int SwordManager::getVerseCount(const std::string& moduleName,
                                 const std::string& book, int chapter) {
    std::lock_guard<std::mutex> lock(mutex_);

    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return 0;

    sword::VerseKey* vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    if (!vk) {
        sword::VerseKey tempKey;
        mod->setKey(tempKey);
        vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    }
    if (!vk) return 0;

    std::string ref = book + " " + std::to_string(chapter) + ":1";
    vk->setText(ref.c_str());

    return vk->getVerseMax();
}

std::string SwordManager::getModuleDescription(const std::string& moduleName) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return "";
    return mod->getDescription();
}

bool SwordManager::moduleHasStrongs(const std::string& moduleName) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return false;
    auto feat = mod->getConfigEntry("Feature");
    if (feat) {
        std::string features(feat);
        return features.find("StrongsNumbers") != std::string::npos;
    }
    // Also check GlobalOptionFilter
    auto filter = mod->getConfigEntry("GlobalOptionFilter");
    if (filter) {
        std::string filters(filter);
        return filters.find("Strongs") != std::string::npos;
    }
    return false;
}

bool SwordManager::moduleHasMorph(const std::string& moduleName) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return false;
    auto filter = mod->getConfigEntry("GlobalOptionFilter");
    if (filter) {
        std::string filters(filter);
        return filters.find("Morph") != std::string::npos;
    }
    return false;
}

SwordManager::VerseRef SwordManager::parseVerseRef(const std::string& ref) {
    VerseRef result;

    // Simple parser: "Book Chapter:Verse" or "Book Chapter:Verse-EndVerse"
    // Examples: "Genesis 1:1", "1 John 3:16", "Gen 1:1-5"
    size_t lastSpace = ref.rfind(' ');
    if (lastSpace == std::string::npos) {
        result.book = ref;
        return result;
    }

    std::string afterBook = ref.substr(lastSpace + 1);
    result.book = ref.substr(0, lastSpace);

    size_t colonPos = afterBook.find(':');
    if (colonPos != std::string::npos) {
        result.chapter = std::stoi(afterBook.substr(0, colonPos));
        std::string verseStr = afterBook.substr(colonPos + 1);

        size_t dashPos = verseStr.find('-');
        if (dashPos != std::string::npos) {
            result.verse = std::stoi(verseStr.substr(0, dashPos));
            result.verseEnd = std::stoi(verseStr.substr(dashPos + 1));
        } else {
            result.verse = std::stoi(verseStr);
        }
    } else {
        // Just chapter number
        try {
            result.chapter = std::stoi(afterBook);
        } catch (...) {
            // Might be part of the book name (e.g., "1 John")
            result.book = ref;
        }
    }

    return result;
}

std::string SwordManager::getShortReference(const std::string& moduleName,
                                            const std::string& reference) const {
    std::string ref = trimCopy(reference);
    if (ref.empty()) return "";

    std::lock_guard<std::mutex> lock(mutex_);

    sword::VerseKey vk;
    vk.setAutoNormalize(true);

    sword::SWModule* mod = moduleName.empty() ? nullptr : getModule(moduleName);
    if (mod) {
        const char* v11n = mod->getConfigEntry("Versification");
        if (v11n && *v11n) {
            vk.setVersificationSystem(v11n);
        }
    }

    vk.setText(ref.c_str());
    if (vk.popError()) {
        return ref;
    }

    const char* shortText = vk.getShortText();
    if (shortText && *shortText) return shortText;

    const char* longText = vk.getText();
    if (longText && *longText) return longText;

    return ref;
}

std::map<std::string, std::map<std::string, std::string>>
SwordManager::getEntryAttributes(const std::string& moduleName) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, std::map<std::string, std::string>> result;

    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return result;

    auto& attrs = mod->getEntryAttributes();
    auto wordIt = attrs.find("Word");
    if (wordIt != attrs.end()) {
        for (auto& entry : wordIt->second) {
            std::map<std::string, std::string> wordAttrs;
            for (auto& attr : entry.second) {
                wordAttrs[attr.first.c_str()] = attr.second.c_str();
            }
            result[entry.first.c_str()] = wordAttrs;
        }
    }

    return result;
}

sword::SWModule* SwordManager::getModule(const std::string& name) const {
    if (!mgr_) return nullptr;
    auto it = mgr_->Modules.find(name.c_str());
    if (it != mgr_->Modules.end()) {
        return it->second;
    }
    return nullptr;
}

void SwordManager::configureFilters(sword::SWModule* mod) {
    // Filters are configured globally in the manager
    // This method can be used for per-module filter adjustments if needed
    (void)mod;
}

std::string SwordManager::postProcessHtml(const std::string& html) const {
    auto ppStart = std::chrono::steady_clock::now();
    bool cacheHit = false;
    bool bypassed = false;

    auto cacheIt = postProcessCache_.find(html);
    if (cacheIt != postProcessCache_.end()) {
        cacheHit = true;
        postProcessLru_.splice(postProcessLru_.begin(),
                               postProcessLru_,
                               cacheIt->second.lruIt);
        if (perf::enabled()) {
            double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - ppStart).count();
            if (ms > 1.0) {
                perf::logf("SwordManager::postProcessHtml hit len=%zu: %.3f ms",
                           html.size(), ms);
            }
        }
        return cacheIt->second.value;
    }

    auto cacheStore = [this](const std::string& key, const std::string& value) {
        auto it = postProcessCache_.find(key);
        if (it != postProcessCache_.end()) {
            it->second.value = value;
            postProcessLru_.splice(postProcessLru_.begin(),
                                   postProcessLru_,
                                   it->second.lruIt);
            return;
        }

        postProcessLru_.push_front(key);
        postProcessCache_.emplace(key, PostProcessCacheEntry{
            value, postProcessLru_.begin()
        });

        if (postProcessCache_.size() > kPostProcessCacheLimit) {
            const std::string& evictKey = postProcessLru_.back();
            postProcessCache_.erase(evictKey);
            postProcessLru_.pop_back();
        }
    };

    if (!mayContainMorphOrStrongsMarkup(html)) {
        bypassed = true;
        cacheStore(html, html);
        if (perf::enabled()) {
            double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - ppStart).count();
            if (ms > 1.0) {
                perf::logf("SwordManager::postProcessHtml bypass len=%zu: %.3f ms",
                           html.size(), ms);
            }
        }
        return html;
    }

    std::string out;
    out.reserve(html.size() + 32);
    OutputTarget lastTarget;

    size_t pos = 0;
    while (pos < html.size()) {
        if (html[pos] != '<') {
            size_t nextTag = html.find('<', pos);
            size_t end = (nextTag == std::string::npos) ? html.size() : nextTag;
            appendSanitizedText(out, html.substr(pos, end - pos), lastTarget);
            pos = end;
            continue;
        }

        size_t tagEnd = std::string::npos;
        std::string tagName;
        bool isClosing = false;
        bool isSelfClosing = false;
        if (!parseTag(html, pos, tagEnd, tagName, isClosing, isSelfClosing)) {
            appendSanitizedText(out, html.substr(pos, 1), lastTarget);
            ++pos;
            continue;
        }

        std::string rawTag = html.substr(pos, tagEnd - pos + 1);
        if (!isClosing && isInlineStrongMarkerTag(rawTag, tagName)) {
            pos = tagEnd + 1;
            continue;
        }

        if (!isClosing && tagName == "w") {
            size_t closePos = findNoCase(html, "</w", tagEnd + 1);
            if (closePos != std::string::npos) {
                size_t closeEnd = std::string::npos;
                std::string closeName;
                bool closeIsClosing = false;
                bool closeIsSelfClosing = false;
                if (parseTag(html, closePos, closeEnd, closeName,
                             closeIsClosing, closeIsSelfClosing) &&
                    closeIsClosing && closeName == "w") {
                    std::string content =
                        html.substr(tagEnd + 1, closePos - (tagEnd + 1));
                    HoverMeta meta = extractMetaFromWTag(rawTag);

                    if (!meta.empty()) {
                        size_t start = out.size();
                        out += buildWordSpanOpenTag(meta);
                        out += content;
                        out += "</span>";
                        lastTarget.start = start;
                        lastTarget.end = out.size();
                        lastTarget.valid = true;
                    } else {
                        size_t outStart = out.size();
                        out += content;
                        trackLastWordInHtmlFragment(content, outStart, lastTarget);
                    }

                    pos = closeEnd + 1;
                    continue;
                }
            }

            out += rawTag;
            pos = tagEnd + 1;
            continue;
        }

        if (!isClosing && tagName == "small") {
            size_t closePos = findNoCase(html, "</small>", tagEnd + 1);
            if (closePos != std::string::npos) {
                size_t blockEnd = closePos + 8; // strlen("</small>")
                std::string block = html.substr(pos, blockEnd - pos);

                HoverMeta blockMeta;
                bool isMarkerBlock = parseSmallBlockMeta(block, blockMeta);
                if (isMarkerBlock) {
                    applyMetaToTarget(out, lastTarget, blockMeta);
                } else {
                    size_t outStart = out.size();
                    out += block;
                    trackLastWordInHtmlFragment(block, outStart, lastTarget);
                }

                pos = blockEnd;
                continue;
            }

            out += rawTag;
            pos = tagEnd + 1;
            continue;
        }

        if (!isClosing && tagName == "a") {
            std::string href;
            bool hasHref = extractAttributeValue(rawTag, "href", href);
            bool isStrongLink = false;
            bool isMorphLink = false;
            HoverMeta linkMeta;
            if (hasHref) {
                extractHrefMeta(href, linkMeta, &isStrongLink, &isMorphLink);
            }

            bool isSpecialLink = isStrongLink || isMorphLink;
            if (isSpecialLink) {
                size_t closePos = findNoCase(html, "</a>", tagEnd + 1);
                if (closePos != std::string::npos) {
                    size_t afterClose = closePos + 4; // strlen("</a>")
                    std::string inner =
                        html.substr(tagEnd + 1, closePos - (tagEnd + 1));
                    std::string plainInner = trimCopy(stripTags(inner));
                    bool codeText =
                        (isStrongLink && looksLikeStrongsDisplay(plainInner)) ||
                        (isMorphLink && looksLikeMorphDisplay(plainInner));

                    if (!linkMeta.empty() && !inner.empty() && !codeText) {
                        size_t start = out.size();
                        out += buildWordSpanOpenTag(linkMeta);
                        out += inner;
                        out += "</span>";
                        lastTarget.start = start;
                        lastTarget.end = out.size();
                        lastTarget.valid = true;
                    } else {
                        if (!linkMeta.empty()) {
                            applyMetaToTarget(out, lastTarget, linkMeta);
                        } else if (!inner.empty() && !codeText) {
                            size_t outStart = out.size();
                            out += inner;
                            trackLastWordInHtmlFragment(inner, outStart, lastTarget);
                        }
                    }

                    pos = afterClose;
                    continue;
                }
            }
        }

        out += rawTag;
        pos = tagEnd + 1;
    }

    std::string result = collapseSpacesOutsideTags(out);
    cacheStore(html, result);
    if (perf::enabled()) {
        double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - ppStart).count();
        if (ms > 1.0) {
            perf::logf("SwordManager::postProcessHtml miss len=%zu out=%zu cacheHit=%d bypass=%d: %.3f ms",
                       html.size(), result.size(), cacheHit ? 1 : 0, bypassed ? 1 : 0, ms);
        }
    }
    return result;
}

ModuleInfo SwordManager::buildModuleInfo(sword::SWModule* mod) const {
    ModuleInfo info;
    info.name = mod->getName();
    info.description = mod->getDescription();
    info.type = mod->getType();

    auto lang = mod->getLanguage();
    info.language = lang ? lang : "en";

    // Check for Strong's
    auto feat = mod->getConfigEntry("Feature");
    if (feat) {
        std::string features(feat);
        info.hasStrongs = features.find("StrongsNumbers") != std::string::npos;
    }
    auto filter = mod->getConfigEntry("GlobalOptionFilter");
    if (filter) {
        std::string filters(filter);
        if (!info.hasStrongs) {
            info.hasStrongs = filters.find("Strongs") != std::string::npos;
        }
        info.hasMorph = filters.find("Morph") != std::string::npos;
    }

    return info;
}

} // namespace verdad
