#include "sword/SwordManager.h"
#include "sword/SwordPaths.h"
#include "app/PerfTrace.h"

#include <swmgr.h>
#include <swconfig.h>
#include <swmodule.h>
#include <swkey.h>
#include <treekey.h>
#include <versekey.h>
#include <listkey.h>
#include <markupfiltmgr.h>
#include <rtfhtml.h>
#include <swbuf.h>

#include <algorithm>
#include <chrono>
#include <regex>
#include <sstream>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <string_view>

namespace verdad {
namespace {

std::string htmlEscapeAttr(const std::string& s);

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

bool containsNonWhitespace(std::string_view text) {
    for (char ch : text) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            return true;
        }
    }
    return false;
}

size_t countCharOccurrences(const std::string& text, char needle) {
    return static_cast<size_t>(
        std::count(text.begin(), text.end(), needle));
}

size_t countSubstringOccurrences(const std::string& text,
                                 const char* needle) {
    if (!needle || !*needle) return 0;

    size_t count = 0;
    size_t pos = 0;
    const std::string match(needle);
    while ((pos = text.find(match, pos)) != std::string::npos) {
        ++count;
        pos += match.size();
    }
    return count;
}

std::string safeConfigEntry(const char* value) {
    return value ? std::string(value) : std::string();
}

std::string moduleMetadataLocaleSuffix() {
    const char* vars[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    for (const char* var : vars) {
        const char* raw = std::getenv(var);
        if (!raw || !*raw) continue;

        std::string locale(raw);
        size_t cut = locale.find_first_of(".@");
        if (cut != std::string::npos) locale.erase(cut);
        cut = locale.find_first_of("-_");
        if (cut != std::string::npos) locale.erase(cut);
        if (locale.size() < 2) continue;
        if (!std::isalpha(static_cast<unsigned char>(locale[0])) ||
            !std::isalpha(static_cast<unsigned char>(locale[1]))) {
            continue;
        }

        std::string suffix = locale.substr(0, 2);
        std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        if (suffix == "c" || suffix == "po") continue;
        return suffix;
    }

    return "";
}

std::string moduleConfigEntry(sword::SWModule* mod, const char* key) {
    if (!mod || !key || !*key) return "";
    return safeConfigEntry(mod->getConfigEntry(key));
}

std::string localizedModuleConfigEntry(sword::SWModule* mod, const char* key) {
    if (!mod || !key || !*key) return "";

    const std::string localeSuffix = moduleMetadataLocaleSuffix();
    if (!localeSuffix.empty()) {
        const std::string localizedKey = std::string(key) + "_" + localeSuffix;
        std::string localizedValue = moduleConfigEntry(mod, localizedKey.c_str());
        if (!localizedValue.empty()) return localizedValue;
    }

    return moduleConfigEntry(mod, key);
}

std::string formattedModuleConfigEntryHtml(sword::SWModule* mod, const char* key) {
    const std::string raw = localizedModuleConfigEntry(mod, key);
    if (raw.empty()) return "";

    sword::SWBuf buffer(raw.c_str());
    sword::RTFHTML filter;
    filter.processText(buffer, nullptr, nullptr);
    return std::string(buffer.c_str());
}

bool moduleHasConfigValue(sword::SWModule* mod,
                          const char* section,
                          const char* value) {
    return mod && section && value && *value &&
           mod->getConfig().has(section, value);
}

template <size_t N>
bool moduleHasAnyGlobalOption(sword::SWModule* mod,
                              const char* const (&values)[N]) {
    for (const char* value : values) {
        if (moduleHasConfigValue(mod, "GlobalOptionFilter", value)) {
            return true;
        }
    }
    return false;
}

bool moduleHasFeature(sword::SWModule* mod, const char* value) {
    return moduleHasConfigValue(mod, "Feature", value);
}

bool sameBookChapter(const sword::VerseKey* vk,
                     char testament,
                     char book,
                     int chapter) {
    return vk &&
           vk->getTestament() == testament &&
           vk->getBook() == book &&
           vk->getChapter() == chapter;
}

sword::VerseKey* verseKeyForModule(sword::SWModule* mod) {
    if (!mod) return nullptr;

    sword::VerseKey* vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    if (!vk) {
        sword::VerseKey tempKey;
        mod->setKey(tempKey);
        vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    }
    return vk;
}

class ScopedGlobalOptionOverride {
public:
    ScopedGlobalOptionOverride(sword::SWMgr* mgr,
                               std::initializer_list<std::pair<const char*, const char*>> values)
        : mgr_(mgr) {
        if (!mgr_) return;
        overrides_.reserve(values.size());
        for (const auto& entry : values) {
            if (!entry.first || !entry.second) continue;
            const char* current = mgr_->getGlobalOption(entry.first);
            overrides_.push_back(OptionState{
                entry.first,
                current ? std::string(current) : std::string()
            });
            mgr_->setGlobalOption(entry.first, entry.second);
        }
    }

    ~ScopedGlobalOptionOverride() {
        if (!mgr_) return;
        for (auto it = overrides_.rbegin(); it != overrides_.rend(); ++it) {
            mgr_->setGlobalOption(it->name.c_str(), it->value.c_str());
        }
    }

private:
    struct OptionState {
        std::string name;
        std::string value;
    };

    sword::SWMgr* mgr_ = nullptr;
    std::vector<OptionState> overrides_;
};

void appendFeatureLabel(std::vector<std::string>& labels,
                        const std::string& label) {
    if (label.empty()) return;
    if (std::find(labels.begin(), labels.end(), label) == labels.end()) {
        labels.push_back(label);
    }
}

std::vector<std::string> moduleFeatureLabels(sword::SWModule* mod) {
    std::vector<std::string> labels;

    static const char* kHeadings[] = {"ThMLHeadings", "OSISHeadings"};
    static const char* kFootnotes[] = {"GBFFootnotes", "ThMLFootnotes", "OSISFootnotes"};
    static const char* kCrossRefs[] = {"ThMLScripref", "OSISScripref"};
    static const char* kStrongs[] = {"GBFStrongs", "ThMLStrongs", "OSISStrongs"};
    static const char* kMorph[] = {"GBFMorph", "ThMLMorph", "OSISMorph"};
    static const char* kLemmas[] = {"ThMLLemma", "OSISLemma"};
    static const char* kRedLetter[] = {"GBFRedLetterWords", "OSISRedLetterWords"};
    static const char* kGreekAccents[] = {"UTF8GreekAccents"};
    static const char* kHebrewPoints[] = {"UTF8HebrewPoints"};
    static const char* kCantillation[] = {"UTF8Cantillation"};
    static const char* kVariants[] = {"ThMLVariants", "OSISVariants"};
    static const char* kXlit[] = {"OSISXlit"};
    static const char* kEnum[] = {"OSISEnum"};
    static const char* kGlosses[] = {"OSISGlosses", "OSISRuby"};
    static const char* kMorphemeSeg[] = {"OSISMorphSegmentation"};

    if (moduleHasAnyGlobalOption(mod, kHeadings)) {
        appendFeatureLabel(labels, "Headings");
    }
    if (moduleHasAnyGlobalOption(mod, kFootnotes)) {
        appendFeatureLabel(labels, "Footnotes");
    }
    if (moduleHasAnyGlobalOption(mod, kCrossRefs)) {
        appendFeatureLabel(labels, "Cross references");
    }
    if (moduleHasFeature(mod, "StrongsNumbers") ||
        moduleHasAnyGlobalOption(mod, kStrongs)) {
        appendFeatureLabel(labels, "Strong's numbers");
    }
    if (moduleHasAnyGlobalOption(mod, kMorph) ||
        moduleHasFeature(mod, "GreekParse") ||
        moduleHasFeature(mod, "HebrewParse")) {
        appendFeatureLabel(labels, "Morphological tags");
    }
    if (moduleHasAnyGlobalOption(mod, kLemmas)) {
        appendFeatureLabel(labels, "Lemmas");
    }
    if (moduleHasAnyGlobalOption(mod, kRedLetter)) {
        appendFeatureLabel(labels, "Words of Christ in red");
    }
    if (moduleHasAnyGlobalOption(mod, kGreekAccents)) {
        appendFeatureLabel(labels, "Greek accents");
    }
    if (moduleHasAnyGlobalOption(mod, kHebrewPoints)) {
        appendFeatureLabel(labels, "Hebrew vowel points");
    }
    if (moduleHasAnyGlobalOption(mod, kCantillation)) {
        appendFeatureLabel(labels, "Hebrew cantillation");
    }
    if (moduleHasAnyGlobalOption(mod, kVariants)) {
        appendFeatureLabel(labels, "Variant readings");
    }
    if (moduleHasAnyGlobalOption(mod, kXlit)) {
        appendFeatureLabel(labels, "Transliteration forms");
    }
    if (moduleHasAnyGlobalOption(mod, kEnum)) {
        appendFeatureLabel(labels, "Enumerations");
    }
    if (moduleHasAnyGlobalOption(mod, kGlosses)) {
        appendFeatureLabel(labels, "Glosses");
    }
    if (moduleHasAnyGlobalOption(mod, kMorphemeSeg)) {
        appendFeatureLabel(labels, "Morpheme segmentation");
    }
    if (moduleHasFeature(mod, "GreekDef")) {
        appendFeatureLabel(labels, "Greek definitions");
    }
    if (moduleHasFeature(mod, "HebrewDef")) {
        appendFeatureLabel(labels, "Hebrew definitions");
    }

    return labels;
}

std::string extractChapterHeadingHtml(const std::string& raw) {
    static const std::regex chapterTitleRe(
        R"(<title\b[^>]*\btype\s*=\s*["']chapter["'][^>]*>([\s\S]*?)</title>)",
        std::regex::icase);
    static const std::regex anyTitleRe(
        R"(<title\b[^>]*>([\s\S]*?)</title>)",
        std::regex::icase);
    static const std::regex chapterAttrRe(
        R"chapter(<chapter\b[^>]*\bchapterTitle\s*=\s*"([^"]+)")chapter",
        std::regex::icase);

    std::smatch match;
    if (std::regex_search(raw, match, chapterTitleRe)) {
        return trimCopy(match[1].str());
    }
    if (std::regex_search(raw, match, anyTitleRe)) {
        return trimCopy(match[1].str());
    }
    if (std::regex_search(raw, match, chapterAttrRe)) {
        return htmlEscapeAttr(trimCopy(match[1].str()));
    }
    return "";
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

    const char* stripped = lex->stripText();
    return cleanupLexiconText(stripped ? stripped : "");
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

std::vector<std::string> splitList(const std::string& text, char delim) {
    std::vector<std::string> items;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(delim, start);
        std::string item = trimCopy(text.substr(
            start,
            (end == std::string::npos ? text.size() : end) - start));
        if (!item.empty()) items.push_back(item);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return items;
}

std::string normalizeSingleLinkedVerseRef(const std::string& rawRef) {
    std::string ref = trimCopy(rawRef);
    if (ref.empty()) return "";

    std::vector<std::string> parts = splitList(ref, '.');
    if (parts.size() >= 3) {
        std::ostringstream out;
        for (size_t i = 0; i + 2 < parts.size(); ++i) {
            if (i) out << ' ';
            out << parts[i];
        }
        out << ' ' << parts[parts.size() - 2];
        out << ':' << parts.back();
        ref = out.str();
    }

    if (!ref.empty() &&
        std::isdigit(static_cast<unsigned char>(ref[0]))) {
        size_t pos = 1;
        while (pos < ref.size() &&
               std::isdigit(static_cast<unsigned char>(ref[pos]))) {
            ++pos;
        }
        if (pos < ref.size() &&
            std::isalpha(static_cast<unsigned char>(ref[pos])) &&
            ref[pos - 1] != ' ') {
            ref.insert(pos, " ");
        }
    }

    return trimCopy(ref);
}

std::string normalizeLinkedVerseRef(const std::string& rawRef) {
    std::vector<std::string> parts = splitList(rawRef, '-');
    if (parts.size() <= 1) return normalizeSingleLinkedVerseRef(rawRef);

    std::ostringstream out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out << '-';
        out << normalizeSingleLinkedVerseRef(parts[i]);
    }
    return trimCopy(out.str());
}

std::string normalizeBookLookupKey(const std::string& text) {
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

const sword::VersificationMgr::System* versificationSystemForName(
        const std::string& versificationName) {
    sword::VersificationMgr* mgr =
        sword::VersificationMgr::getSystemVersificationMgr();
    if (!mgr) return nullptr;

    std::string name = trimCopy(versificationName);
    if (!name.empty()) {
        if (const auto* system = mgr->getVersificationSystem(name.c_str())) {
            return system;
        }
    }
    return mgr->getVersificationSystem("KJV");
}

template <typename Callback>
void forEachBookAlias(const sword::VersificationMgr::System* system,
                      Callback&& callback) {
    if (!system) return;

    const int bookCount = system->getBookCount();
    for (int bookIndex = 0; bookIndex < bookCount; ++bookIndex) {
        const sword::VersificationMgr::Book* book = system->getBook(bookIndex);
        if (!book) continue;

        const char* aliases[] = {
            book->getLongName(),
            book->getOSISName(),
            book->getPreferredAbbreviation()
        };
        for (const char* alias : aliases) {
            if (!alias || !*alias) continue;
            callback(bookIndex + 1, alias, book);
        }
    }
}

int resolveBookNumberExact(const std::string& bookName,
                           const sword::VersificationMgr::System* system) {
    const std::string wanted = normalizeBookLookupKey(bookName);
    if (wanted.empty()) return -1;

    int resolvedBook = -1;
    forEachBookAlias(system,
                     [&](int bookNumber, const char* alias,
                         const sword::VersificationMgr::Book* /*book*/) {
        if (resolvedBook >= 0) return;
        if (normalizeBookLookupKey(alias) == wanted) {
            resolvedBook = bookNumber;
        }
    });
    return resolvedBook;
}

int boundedLevenshteinDistance(const std::string& lhs,
                               const std::string& rhs,
                               int maxDistance) {
    if (lhs == rhs) return 0;
    const int lhsLen = static_cast<int>(lhs.size());
    const int rhsLen = static_cast<int>(rhs.size());
    if (std::abs(lhsLen - rhsLen) > maxDistance) {
        return maxDistance + 1;
    }

    std::vector<int> prev(rhsLen + 1);
    std::vector<int> cur(rhsLen + 1);
    for (int j = 0; j <= rhsLen; ++j) prev[j] = j;

    for (int i = 1; i <= lhsLen; ++i) {
        cur[0] = i;
        int rowBest = cur[0];
        for (int j = 1; j <= rhsLen; ++j) {
            int cost = (lhs[i - 1] == rhs[j - 1]) ? 0 : 1;
            cur[j] = std::min({
                prev[j] + 1,
                cur[j - 1] + 1,
                prev[j - 1] + cost
            });
            rowBest = std::min(rowBest, cur[j]);
        }
        if (rowBest > maxDistance) return maxDistance + 1;
        prev.swap(cur);
    }

    return prev[rhsLen];
}

std::string leadingDigits(const std::string& text) {
    size_t pos = 0;
    while (pos < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
    return text.substr(0, pos);
}

std::string canonicalBookTokenForNumber(
        int bookNumber,
        const sword::VersificationMgr::System* system) {
    if (!system || bookNumber <= 0) return "";
    const sword::VersificationMgr::Book* book = system->getBook(bookNumber - 1);
    if (!book) return "";

    const char* candidates[] = {
        book->getOSISName(),
        book->getPreferredAbbreviation(),
        book->getLongName()
    };
    for (const char* candidate : candidates) {
        if (candidate && *candidate) return candidate;
    }
    return "";
}

std::string linkedVerseBookOverride(const std::string& bookName) {
    const std::string wanted = normalizeBookLookupKey(bookName);
    if (wanted == "jud") return "Judges";
    return "";
}

bool bookSupportsChapter(int bookNumber,
                         int chapter,
                         const sword::VersificationMgr::System* system) {
    if (!system || bookNumber <= 0 || chapter <= 0) return true;
    const sword::VersificationMgr::Book* book = system->getBook(bookNumber - 1);
    return book && book->getChapterMax() >= chapter;
}

void appendBookCandidate(std::vector<int>& candidates, int bookNumber) {
    if (bookNumber <= 0) return;
    if (std::find(candidates.begin(), candidates.end(), bookNumber) ==
        candidates.end()) {
        candidates.push_back(bookNumber);
    }
}

int resolveBookNumberByPrefix(const std::string& bookName,
                              int chapter,
                              const sword::VersificationMgr::System* system) {
    const std::string wanted = normalizeBookLookupKey(bookName);
    if (wanted.empty() || !system) return -1;

    const std::string wantedDigits = leadingDigits(wanted);
    std::vector<int> candidates;
    forEachBookAlias(system,
                     [&](int bookNumber, const char* alias,
                         const sword::VersificationMgr::Book* /*book*/) {
        std::string aliasKey = normalizeBookLookupKey(alias);
        if (aliasKey.empty()) return;
        if (!wantedDigits.empty() && leadingDigits(aliasKey) != wantedDigits) {
            return;
        }
        if (aliasKey.rfind(wanted, 0) != 0 &&
            wanted.rfind(aliasKey, 0) != 0) {
            return;
        }
        if (!bookSupportsChapter(bookNumber, chapter, system)) {
            return;
        }
        appendBookCandidate(candidates, bookNumber);
    });

    return (candidates.size() == 1) ? candidates.front() : -1;
}

std::string repairSingleLinkedVerseRef(const std::string& rawRef,
                                       const std::string& versificationName) {
    std::string ref = normalizeSingleLinkedVerseRef(rawRef);
    size_t lastSpace = ref.rfind(' ');
    if (lastSpace == std::string::npos) return ref;

    std::string bookPart = trimCopy(ref.substr(0, lastSpace));
    std::string tail = trimCopy(ref.substr(lastSpace + 1));
    if (bookPart.empty() || tail.empty()) return ref;

    SwordManager::VerseRef parsed;
    try {
        parsed = SwordManager::parseVerseRef(ref);
    } catch (...) {
        return ref;
    }
    if (parsed.book.empty() || parsed.chapter <= 0) return ref;

    const auto* system = versificationSystemForName(versificationName);
    if (!system) return ref;

    if (std::string overrideBook = linkedVerseBookOverride(parsed.book);
        !overrideBook.empty()) {
        return normalizeSingleLinkedVerseRef(overrideBook + " " + tail);
    }

    if (resolveBookNumberExact(parsed.book, system) >= 0) return ref;

    if (int prefixBook = resolveBookNumberByPrefix(parsed.book,
                                                   parsed.chapter,
                                                   system);
        prefixBook > 0) {
        std::string repairedBook = canonicalBookTokenForNumber(prefixBook, system);
        if (!repairedBook.empty()) {
            return normalizeSingleLinkedVerseRef(repairedBook + " " + tail);
        }
    }

    const std::string wanted = normalizeBookLookupKey(parsed.book);
    if (wanted.empty()) return ref;

    const std::string wantedDigits = leadingDigits(wanted);
    const int maxDistance =
        (wanted.size() <= 5) ? 1 : (wanted.size() <= 9 ? 2 : 3);

    int bestBook = -1;
    int bestDistance = maxDistance + 1;
    bool ambiguous = false;

    forEachBookAlias(system,
                     [&](int bookNumber, const char* alias,
                         const sword::VersificationMgr::Book* /*book*/) {
        std::string aliasKey = normalizeBookLookupKey(alias);
        if (aliasKey.empty()) return;
        if (!wantedDigits.empty() && leadingDigits(aliasKey) != wantedDigits) {
            return;
        }
        if (!bookSupportsChapter(bookNumber, parsed.chapter, system)) {
            return;
        }

        int distance = boundedLevenshteinDistance(wanted, aliasKey, maxDistance);
        if (distance > maxDistance) return;

        if (distance < bestDistance) {
            bestDistance = distance;
            bestBook = bookNumber;
            ambiguous = false;
            return;
        }
        if (distance == bestDistance && bookNumber != bestBook) {
            ambiguous = true;
        }
    });

    if (bestBook <= 0 || ambiguous) return ref;

    std::string repairedBook = canonicalBookTokenForNumber(bestBook, system);
    if (repairedBook.empty()) return ref;
    return normalizeSingleLinkedVerseRef(repairedBook + " " + tail);
}

std::string repairLinkedVerseRef(const std::string& rawRef,
                                 const std::string& versificationName) {
    std::vector<std::string> parts = splitList(rawRef, '-');
    if (parts.size() <= 1) {
        return repairSingleLinkedVerseRef(rawRef, versificationName);
    }

    std::ostringstream out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out << '-';
        out << repairSingleLinkedVerseRef(parts[i], versificationName);
    }
    return trimCopy(out.str());
}

bool resolvedRefsMatchExpected(const std::vector<std::string>& resolvedRefs,
                               const std::string& expectedRef,
                               const std::string& versificationName) {
    if (resolvedRefs.empty()) return false;

    std::string expected = trimCopy(expectedRef);
    if (expected.empty()) return true;

    std::vector<std::string> rangeParts = splitList(expected, '-');
    if (!rangeParts.empty()) expected = rangeParts.front();

    SwordManager::VerseRef wanted;
    SwordManager::VerseRef got;
    try {
        wanted = SwordManager::parseVerseRef(expected);
        got = SwordManager::parseVerseRef(resolvedRefs.front());
    } catch (...) {
        return true;
    }

    if (wanted.book.empty() || wanted.chapter <= 0) return true;
    if (got.book.empty() || got.chapter <= 0) return false;
    if (got.chapter != wanted.chapter) return false;
    if (wanted.verse > 0 && got.verse != wanted.verse) return false;

    const auto* system = versificationSystemForName(versificationName);
    if (!system) return true;

    int wantedBook = resolveBookNumberExact(wanted.book, system);
    int gotBook = resolveBookNumberExact(got.book, system);
    if (wantedBook >= 0 && gotBook >= 0) {
        return wantedBook == gotBook;
    }

    return normalizeBookLookupKey(wanted.book) ==
           normalizeBookLookupKey(got.book);
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

int findGeneralBookTocIndex(const std::vector<GeneralBookTocEntry>& toc,
                            const std::string& key) {
    std::string wanted = trimCopy(key);
    if (toc.empty() || wanted.empty()) return -1;

    for (size_t i = 0; i < toc.size(); ++i) {
        if (toc[i].key == wanted) return static_cast<int>(i);
    }

    std::string wantedLower = toLowerAscii(wanted);
    for (size_t i = 0; i < toc.size(); ++i) {
        if (toLowerAscii(toc[i].label) == wantedLower) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

std::string renderGeneralBookSectionHtml(const GeneralBookTocEntry& entry,
                                         const std::string& text) {
    if (trimCopy(text).empty()) return "";

    std::ostringstream html;
    html << "<div class=\"general-book\">\n";
    html << "<div class=\"entry-key\">" << htmlEscapeAttr(entry.label)
         << "</div>\n";
    html << text << "\n";
    html << "</div>\n";
    return html.str();
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
    {
        size_t start = 0;
        while (start < morph.size() &&
               (morph[start] == '/' || morph[start] == '\\')) {
            ++start;
        }
        if (start > 0) morph.erase(0, start);
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

std::string buildInlineMarkerHtml(const std::string& innerHtml,
                                  const HoverMeta& meta) {
    if (trimCopy(innerHtml).empty() || meta.empty()) return "";

    std::string classes = "verdad-inline-marker";
    if (!meta.strong.empty()) classes += " strongs-marker";
    if (!meta.morph.empty()) classes += " morph-marker";

    std::string out = "<span class=\"" + classes + "\">";
    out += innerHtml;
    out += "</span>";
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

struct CommentaryBuildPerf {
    size_t verses = 0;
    size_t emptyEntries = 0;
    size_t cacheHits = 0;
    size_t cacheMisses = 0;
    size_t entryBytes = 0;
    double entryBuildMs = 0.0;
    double renderTextMs = 0.0;
    double normalizeMs = 0.0;
    double closeTagsMs = 0.0;
    double plainTextMs = 0.0;
};

void logCommentaryBuildPerf(const std::string& moduleName,
                            const std::string& key,
                            size_t htmlBytes,
                            double assembleMs,
                            const CommentaryBuildPerf& perfStats) {
    if (!perf::enabled()) return;

    perf::logf(
        "SwordManager::getCommentaryText build %s %s: html=%zu verses=%zu empty=%zu cacheHits=%zu cacheMisses=%zu entryBytes=%zu entryBuild=%.3f ms renderText=%.3f ms normalize=%.3f ms closeTags=%.3f ms plainText=%.3f ms assemble=%.3f ms",
        moduleName.c_str(),
        key.c_str(),
        htmlBytes,
        perfStats.verses,
        perfStats.emptyEntries,
        perfStats.cacheHits,
        perfStats.cacheMisses,
        perfStats.entryBytes,
        perfStats.entryBuildMs,
        perfStats.renderTextMs,
        perfStats.normalizeMs,
        perfStats.closeTagsMs,
        perfStats.plainTextMs,
        assembleMs);
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

    {
        size_t start = 0;
        while (start < t.size() && (t[start] == '<' || t[start] == '(' ||
               t[start] == '[' || std::isspace(static_cast<unsigned char>(t[start])))) {
            ++start;
        }
        if (start > 0) t.erase(0, start);
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

bool tryConsumeEscapedStrongMarker(std::string_view text, size_t& pos) {
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

bool tryConsumeGbfMorphMarker(std::string_view text, size_t& pos) {
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

void trackLastWordInPlainText(std::string_view text,
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

void trackLastWordInHtmlFragment(std::string_view html,
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
                         std::string_view text,
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

std::string closeDanglingInlineTags(std::string html) {
    if (html.empty()) return html;

    std::vector<std::string> openTags;
    size_t pos = 0;
    while (pos < html.size()) {
        if (html[pos] != '<') {
            ++pos;
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

        if (isParallelInlineTag(tagName)) {
            if (isClosing) {
                eraseLastOpenTag(openTags, tagName);
            } else if (!isSelfClosing) {
                openTags.push_back(tagName);
            }
        }

        pos = tagEnd + 1;
    }

    if (openTags.empty()) return html;

    while (!openTags.empty()) {
        html += "</";
        html += openTags.back();
        html += ">";
        openTags.pop_back();
    }
    return html;
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

namespace {

// Case-insensitive search for a tag pattern starting at pos.
// Returns position or std::string::npos.
size_t findTagCI(const std::string& s, const std::string& tagLower, size_t pos = 0) {
    for (; pos + tagLower.size() <= s.size(); ++pos) {
        bool match = true;
        for (size_t j = 0; j < tagLower.size() && match; ++j) {
            match = (std::tolower(static_cast<unsigned char>(s[pos + j])) ==
                     static_cast<unsigned char>(tagLower[j]));
        }
        if (match) return pos;
    }
    return std::string::npos;
}

size_t skipWhitespace(const std::string& input, size_t pos) {
    while (pos < input.size() &&
           std::isspace(static_cast<unsigned char>(input[pos]))) {
        ++pos;
    }
    return pos;
}

// Replace self-closing <p /> and empty <p></p> tags with gap div
std::string replaceSelfClosingAndEmptyParagraphs(const std::string& input) {
    static const std::string gap = "<div class=\"commentary-gap\">&nbsp;</div>";
    if (input.empty()) return input;

    std::string result;
    result.reserve(input.size());
    size_t i = 0;
    while (i < input.size()) {
        if (input[i] != '<') {
            result.push_back(input[i]);
            ++i;
            continue;
        }

        size_t tagEnd = std::string::npos;
        std::string tagName;
        bool isClosing = false;
        bool isSelfClosing = false;
        if (!parseTag(input, i, tagEnd, tagName, isClosing, isSelfClosing) ||
            isClosing || tagName != "p") {
            result.push_back(input[i]);
            ++i;
            continue;
        }

        if (isSelfClosing) {
            result += gap;
            i = tagEnd + 1;
            continue;
        }

        size_t next = skipWhitespace(input, tagEnd + 1);
        if (next < input.size() && input[next] == '<') {
            size_t closeEnd = std::string::npos;
            std::string closeName;
            bool closeIsClosing = false;
            bool closeIsSelfClosing = false;
            if (parseTag(input, next, closeEnd, closeName, closeIsClosing, closeIsSelfClosing) &&
                closeIsClosing && closeName == "p") {
                result += gap;
                i = closeEnd + 1;
                continue;
            }
        }

        result.append(input, i, tagEnd + 1 - i);
        i = tagEnd + 1;
    }
    return result;
}

// Collapse runs of 2+ <br> tags into a single gap div
std::string collapseMultipleBreaks(const std::string& input) {
    static const std::string gap = "<div class=\"commentary-gap\">&nbsp;</div>";
    if (input.empty()) return input;

    std::string result;
    result.reserve(input.size());
    size_t i = 0;
    while (i < input.size()) {
        if (input[i] != '<') {
            result.push_back(input[i]);
            ++i;
            continue;
        }

        size_t tagEnd = std::string::npos;
        std::string tagName;
        bool isClosing = false;
        bool isSelfClosing = false;
        if (!parseTag(input, i, tagEnd, tagName, isClosing, isSelfClosing) ||
            isClosing || tagName != "br") {
            result.push_back(input[i]);
            ++i;
            continue;
        }

        int brCount = 0;
        size_t pos = i;
        size_t sequenceEnd = i;
        while (pos < input.size()) {
            if (input[pos] != '<') {
                size_t next = skipWhitespace(input, pos);
                if (next == pos) break;
                pos = next;
                sequenceEnd = pos;
                continue;
            }

            size_t close = std::string::npos;
            std::string closeName;
            bool closeIsClosing = false;
            bool closeIsSelfClosing = false;
            if (!parseTag(input, pos, close, closeName, closeIsClosing, closeIsSelfClosing) ||
                closeIsClosing || closeName != "br") {
                break;
            }
            ++brCount;
            pos = close + 1;
            sequenceEnd = pos;
        }

        if (brCount >= 2) {
            result += gap;
        } else {
            result.append(input, i, tagEnd + 1 - i);
        }
        i = sequenceEnd;
    }
    return result;
}

// Collapse repeated gap divs into one
std::string collapseRepeatedGaps(const std::string& input) {
    static const std::string gapTag = "<div class=\"commentary-gap\">&nbsp;</div>";
    if (input.empty()) return input;

    std::string result;
    result.reserve(input.size());
    size_t i = 0;
    while (i < input.size()) {
        if (input.compare(i, gapTag.size(), gapTag) != 0) {
            result.push_back(input[i]);
            ++i;
            continue;
        }

        result += gapTag;
        i += gapTag.size();
        while (true) {
            size_t next = skipWhitespace(input, i);
            if (input.compare(next, gapTag.size(), gapTag) == 0) {
                i = next + gapTag.size();
            } else {
                break;
            }
        }
    }
    return result;
}

} // anonymous namespace

std::string normalizeCommentaryMarkup(std::string text) {
    if (!containsNonWhitespace(text)) return text;
    if (findTagCI(text, "<p") == std::string::npos &&
        findTagCI(text, "<br") == std::string::npos) {
        return text;
    }

    text = replaceSelfClosingAndEmptyParagraphs(text);
    text = collapseMultipleBreaks(text);
    text = collapseRepeatedGaps(text);
    return text;
}

std::string commentaryEntryHtml(sword::SWModule* mod,
                                CommentaryBuildPerf* perfStats = nullptr) {
    if (!mod) {
        return "";
    }

    std::string raw;
    if (mod->isWritable()) {
        const char* rawEntry = mod->getRawEntry();
        raw = rawEntry ? rawEntry : "";
        if (looksLikeHtmlMarkup(raw)) {
            perf::StepTimer step;
            raw = normalizeCommentaryMarkup(std::move(raw));
            if (perfStats) perfStats->normalizeMs += step.elapsedMs();
            step.reset();
            raw = closeDanglingInlineTags(std::move(raw));
            if (perfStats) {
                perfStats->closeTagsMs += step.elapsedMs();
            }
            return raw;
        }
    }

    perf::StepTimer step;
    std::string rendered = std::string(mod->renderText().c_str());
    if (perfStats) perfStats->renderTextMs += step.elapsedMs();
    if (containsNonWhitespace(rendered)) {
        step.reset();
        rendered = normalizeCommentaryMarkup(std::move(rendered));
        if (perfStats) perfStats->normalizeMs += step.elapsedMs();
        step.reset();
        rendered = closeDanglingInlineTags(std::move(rendered));
        if (perfStats) {
            perfStats->closeTagsMs += step.elapsedMs();
        }
        return rendered;
    }
    if (containsNonWhitespace(raw)) {
        step.reset();
        std::string plain = plainTextToHtml(raw);
        if (perfStats) {
            perfStats->plainTextMs += step.elapsedMs();
        }
        return plain;
    }
    return "";
}

} // namespace

SwordManager::SwordManager() = default;
SwordManager::~SwordManager() = default;

bool SwordManager::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        mgr_.reset();
        bundledSysConfig_.reset();
        postProcessCache_.clear();
        postProcessLru_.clear();
        verseHtmlCache_.clear();
        verseHtmlLru_.clear();
        commentaryVerseHtmlCache_.clear();
        commentaryVerseHtmlLru_.clear();
        dictionaryKeyCache_.clear();

        // Create SWORD manager with XHTML markup filter
        auto* filterMgr = new sword::MarkupFilterMgr(sword::FMT_XHTML);
        const std::string bundlePath = bundledSwordDataPath();
        if (!bundlePath.empty()) {
            bundledSysConfig_ = std::make_unique<sword::SWConfig>();
            bundledSysConfig_->setValue("Install", "DataPath", bundlePath.c_str());

            auto& install = bundledSysConfig_->getSection("Install");
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

            mgr_ = std::make_unique<sword::SWMgr>(
                nullptr, bundledSysConfig_.get(), true, filterMgr);
        } else {
            mgr_ = std::make_unique<sword::SWMgr>(filterMgr);
        }

        if (!mgr_) {
            return false;
        }

        for (const auto& path : supplementalUserSwordDataPaths()) {
            mgr_->augmentModules(path.c_str());
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

std::vector<GeneralBookTocEntry> SwordManager::getGeneralBookToc(
    const std::string& moduleName) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<GeneralBookTocEntry> toc;

    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return toc;

    std::unique_ptr<sword::SWKey> key(mod->createKey());
    auto* treeKey = dynamic_cast<sword::TreeKey*>(key.get());
    if (!treeKey) return toc;

    treeKey->root();
    if (treeKey->hasChildren() && treeKey->firstChild()) {
        appendGeneralBookTocEntries(treeKey, 0, toc);
    }

    return toc;
}

std::string SwordManager::getVerseText(const std::string& moduleName,
                                        const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return "<p><i>Module not found: " + htmlEscapeAttr(moduleName) + "</i></p>";

    mod->setKey(key.c_str());
    std::string text = std::string(mod->renderText().c_str());

    if (text.empty()) {
        return "<p><i>No text available for " + htmlEscapeAttr(key) + "</i></p>";
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

std::string SwordManager::getVersePlainText(const std::string& moduleName,
                                            const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod || !mgr_) return "";

    mod->setKey(key.c_str());
    if (mod->popError()) return "";

    ScopedGlobalOptionOverride options(mgr_.get(), {
        {"Strong's Numbers", "Off"},
        {"Morphological Tags", "Off"},
        {"Lemmas", "Off"},
        {"Footnotes", "Off"},
        {"Cross-references", "Off"},
        {"Headings", "Off"},
    });

    const char* plainRaw = mod->stripText();
    std::string plain = plainRaw ? plainRaw : "";

    return plain;
}

std::string SwordManager::getVerseInsertText(const std::string& moduleName,
                                             const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod || !mgr_) return "";

    mod->setKey(key.c_str());
    if (mod->popError()) return "";

    ScopedGlobalOptionOverride options(mgr_.get(), {
        {"Strong's Numbers", "Off"},
        {"Morphological Tags", "Off"},
        {"Lemmas", "Off"},
        {"Footnotes", "Off"},
        {"Cross-references", "Off"},
        {"Headings", "Off"},
    });

    std::string text = std::string(mod->renderText().c_str());
    if (text.empty()) return "";
    text = postProcessHtml(text);

    VerseRef ref;
    try {
        ref = parseVerseRef(key);
    } catch (...) {
        ref = VerseRef{};
    }

    std::ostringstream html;
    if (ref.verse > 0) {
        html << "<small><strong>" << ref.verse << "</strong></small> ";
    }
    html << text;
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
    if (!mod) return "<p><i>Module not found: " + htmlEscapeAttr(moduleName) + "</i></p>";

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

    std::string chapterHeadingHtml =
        renderedChapterHeadingLocked(mod, book, chapter);
    vk->setText(ref.c_str());

    const char currentTestament = vk->getTestament();
    const char currentBook = vk->getBook();
    const int currentChapter = vk->getChapter();
    if (chapterHeadingHtml.empty()) {
        html << "<div class=\"chapter-heading\">CHAPTER "
             << chapter << ".</div>\n";
    } else {
        html << chapterHeadingHtml;
    }

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

    while (!mod->popError() &&
           sameBookChapter(vk, currentTestament, currentBook, currentChapter)) {
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

    std::string chapterHeadingHtml;
    for (const auto& modName : moduleNames) {
        sword::SWModule* mod = getModule(modName);
        if (!mod) continue;
        chapterHeadingHtml = renderedChapterHeadingLocked(mod, book, chapter);
        if (!chapterHeadingHtml.empty()) break;
    }
    if (chapterHeadingHtml.empty()) {
        html << "<div class=\"chapter-heading\">CHAPTER "
             << chapter << ".</div>\n";
    } else {
        html << chapterHeadingHtml;
    }

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
        std::string rowClasses = "parallel-row";
        if (selectedVerse > 0 && v == selectedVerse) {
            rowClasses += " verse-selected";
        }
        html << "<div class=\"" << rowClasses << "\" id=\"v" << v << "\">\n";
        for (size_t i = 0; i < moduleNames.size(); ++i) {
            bool isLast = (i + 1 == moduleNames.size());
            int w = isLast ? lastColWidth : colWidth;
            const char* colClass = isLast ? "parallel-col-last" : "parallel-col";
            std::string cellClasses = isLast ? "parallel-cell-last" : "parallel-cell";
            const std::string columnAttr = std::to_string(i);
            const std::string moduleAttr = htmlEscapeAttr(moduleNames[i]);
            sword::SWModule* mod = getModule(moduleNames[i]);
            html << "<div class=\"" << colClass << "\" data-module=\""
                 << moduleAttr << "\" data-parallel-col=\"" << columnAttr
                 << "\" style=\"width: " << w << "%;\">"
                 << "<div class=\"" << cellClasses << "\" data-module=\""
                 << moduleAttr << "\" data-parallel-col=\"" << columnAttr
                 << "\">";
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
    if (!mod) return "<p><i>Commentary module not found: " + htmlEscapeAttr(moduleName) + "</i></p>";

    VerseRef ref;
    bool hasChapterContext = false;
    try {
        ref = parseVerseRef(key);
        hasChapterContext = !ref.book.empty() && ref.chapter > 0;
    } catch (...) {
        ref = VerseRef{};
    }

    CommentaryBuildPerf perfStats;
    auto finalizeHtml = [&](std::string html, double assembleMs) {
        logCommentaryBuildPerf(moduleName, key, html.size(), assembleMs, perfStats);
        return html;
    };

    if (!hasChapterContext) {
        mod->setKey(key.c_str());
        perf::StepTimer entryTimer;
        std::string text = commentaryEntryHtml(mod, &perfStats);
        perfStats.entryBuildMs += entryTimer.elapsedMs();
        perfStats.cacheMisses = 1;
        perfStats.verses = 1;

        if (text.empty()) {
            perfStats.emptyEntries = 1;
            return finalizeHtml("<p><i>No commentary available for " + key + "</i></p>", 0.0);
        }
        perfStats.entryBytes += text.size();

        perf::StepTimer assembleTimer;
        std::string html;
        html.reserve(text.size() + key.size() + 64);
        html += "<div class=\"commentary\">\n";
        html += "<h3>";
        html += key;
        html += "</h3>\n";
        html += text;
        html += "</div>\n";
        return finalizeHtml(std::move(html), assembleTimer.elapsedMs());
    }

    sword::VerseKey* vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    if (!vk) {
        sword::VerseKey tempKey;
        mod->setKey(tempKey);
        vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    }
    if (!vk) {
        mod->setKey(key.c_str());
        perf::StepTimer entryTimer;
        std::string text = commentaryEntryHtml(mod, &perfStats);
        perfStats.entryBuildMs += entryTimer.elapsedMs();
        perfStats.cacheMisses = 1;
        perfStats.verses = 1;
        if (text.empty()) {
            perfStats.emptyEntries = 1;
            return finalizeHtml("<p><i>No commentary available for " + key + "</i></p>", 0.0);
        }
        perfStats.entryBytes += text.size();
        perf::StepTimer assembleTimer;
        std::string html;
        std::string escapedKey = htmlEscapeAttr(key);
        html.reserve(text.size() + escapedKey.size() + 96);
        html += "<div class=\"commentary-heading\"><h3>";
        html += escapedKey;
        html += "</h3></div>\n";
        html += "<div class=\"commentary\">\n";
        html += text;
        html += "</div>\n";
        return finalizeHtml(std::move(html), assembleTimer.elapsedMs());
    }

    std::string startRef = ref.book + " " + std::to_string(ref.chapter) + ":1";
    vk->setText(startRef.c_str());
    if (mod->popError()) {
        return finalizeHtml("<p><i>No commentary available for " + key + "</i></p>", 0.0);
    }

    auto tryGetCommentaryVerseHtmlCache =
        [this](const std::string& cacheKey, std::string& valueOut) -> bool {
        auto it = commentaryVerseHtmlCache_.find(cacheKey);
        if (it == commentaryVerseHtmlCache_.end()) return false;

        commentaryVerseHtmlLru_.splice(commentaryVerseHtmlLru_.begin(),
                                       commentaryVerseHtmlLru_,
                                       it->second.lruIt);
        valueOut = it->second.value;
        return true;
    };

    auto storeCommentaryVerseHtmlCache =
        [this](const std::string& cacheKey, const std::string& value) {
        auto it = commentaryVerseHtmlCache_.find(cacheKey);
        if (it != commentaryVerseHtmlCache_.end()) {
            it->second.value = value;
            commentaryVerseHtmlLru_.splice(commentaryVerseHtmlLru_.begin(),
                                           commentaryVerseHtmlLru_,
                                           it->second.lruIt);
            return;
        }

        commentaryVerseHtmlLru_.push_front(cacheKey);
        commentaryVerseHtmlCache_.emplace(cacheKey, VerseHtmlCacheEntry{
            value, commentaryVerseHtmlLru_.begin()
        });

        if (commentaryVerseHtmlCache_.size() > kCommentaryVerseHtmlCacheLimit) {
            const std::string& evictKey = commentaryVerseHtmlLru_.back();
            commentaryVerseHtmlCache_.erase(evictKey);
            commentaryVerseHtmlLru_.pop_back();
        }
    };

    perf::StepTimer assembleTimer;
    const char testament = vk->getTestament();
    const char book = vk->getBook();
    const int chapter = vk->getChapter();
    std::string html;
    std::string escapedBook = htmlEscapeAttr(ref.book);
    html.reserve(4096);
    html += "<div class=\"commentary-heading\"><h3>";
    html += escapedBook;
    html += " ";
    html += std::to_string(ref.chapter);
    html += "</h3></div>\n";
    html += "<div class=\"commentary\">\n";

    while (!mod->popError() && sameBookChapter(vk, testament, book, chapter)) {
        int verse = vk->getVerse();
        std::string verseRef =
            ref.book + " " + std::to_string(ref.chapter) + ":" + std::to_string(verse);
        std::string cacheKey = moduleName + "|" + verseRef;
        std::string verseText;
        if (tryGetCommentaryVerseHtmlCache(cacheKey, verseText)) {
            ++perfStats.cacheHits;
        } else {
            perf::StepTimer entryTimer;
            verseText = commentaryEntryHtml(mod, &perfStats);
            perfStats.entryBuildMs += entryTimer.elapsedMs();
            ++perfStats.cacheMisses;
            storeCommentaryVerseHtmlCache(cacheKey, verseText);
        }

        ++perfStats.verses;
        perfStats.entryBytes += verseText.size();
        bool hasVerseText = containsNonWhitespace(verseText);
        if (!hasVerseText) {
            ++perfStats.emptyEntries;
        }

        bool separated = verse > 1;
        html += "<div class=\"commentary-verse";
        if (separated) {
            html += " commentary-separated";
        }
        html += "\" id=\"v";
        html += std::to_string(verse);
        html += "\">";
        html += "<div class=\"commentary-gutter\"><a class=\"versenum-link\" href=\"bible-verse:";
        html += std::to_string(verse);
        html += "\"><span class=\"commentary-versenum\" id=\"cv";
        html += std::to_string(verse);
        html += "\">";
        html += std::to_string(verse);
        html += "</span></a></div>";
        html += "<div class=\"commentary-text\">";
        html += "<div class=\"commentary-scroll-anchor\" id=\"vpos";
        html += std::to_string(verse);
        html += "\"></div>";
        if (separated) {
            html += "<div class=\"commentary-separator\"></div>";
        }
        html += "<div class=\"commentary-entry\">";
        if (hasVerseText) {
            html += verseText;
        } else {
            html += "<span class=\"commentary-empty\"></span>";
        }
        html += "</div></div>";
        html += "</div>\n";
        (*mod)++;
    }

    html += "</div>\n";
    return finalizeHtml(std::move(html), assembleTimer.elapsedMs());
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
    bool ok = !mod->popError();
    if (ok) {
        dictionaryKeyCache_.erase(moduleName);
        commentaryVerseHtmlCache_.clear();
        commentaryVerseHtmlLru_.clear();
    }
    return ok;
}

bool SwordManager::deleteEntry(const std::string& moduleName,
                               const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod || !mod->isWritable()) return false;

    mod->setKey(key.c_str());
    if (mod->popError()) return false;

    mod->deleteEntry();
    bool ok = !mod->popError();
    if (ok) {
        dictionaryKeyCache_.erase(moduleName);
        commentaryVerseHtmlCache_.clear();
        commentaryVerseHtmlLru_.clear();
    }
    return ok;
}

std::shared_ptr<const std::vector<std::string>> SwordManager::getDictionaryKeys(
        const std::string& moduleName) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto cached = dictionaryKeyCache_.find(moduleName);
    if (cached != dictionaryKeyCache_.end()) {
        return cached->second;
    }

    std::vector<std::string> keys;
    sword::SWModule* mod = getModule(moduleName);
    if (!mod) {
        return std::make_shared<const std::vector<std::string>>();
    }

    if (std::unique_ptr<sword::SWKey> createdKey(mod->createKey());
        createdKey) {
        if (auto* treeKey = dynamic_cast<sword::TreeKey*>(createdKey.get())) {
            std::vector<GeneralBookTocEntry> toc;
            treeKey->root();
            if (treeKey->hasChildren() && treeKey->firstChild()) {
                appendGeneralBookTocEntries(treeKey, 0, toc);
            }
            keys.reserve(toc.size());
            for (const auto& entry : toc) {
                std::string key = trimCopy(entry.key);
                if (!key.empty()) keys.push_back(std::move(key));
            }
        } else {
            std::unique_ptr<sword::SWKey> restoreKey(
                mod->getKey() ? mod->getKey()->clone() : nullptr);
            mod->setPosition(sword::TOP);
            if (!mod->popError()) {
                for (size_t count = 0; count < 50000; ++count) {
                    std::string key = trimCopy(mod->getKeyText());
                    if (!key.empty() &&
                        (keys.empty() || keys.back() != key)) {
                        keys.push_back(std::move(key));
                    }
                    (*mod)++;
                    if (mod->popError()) break;
                }
            }
            if (restoreKey) {
                mod->setKey(*restoreKey);
                mod->popError();
            }
        }
    }

    auto sharedKeys =
        std::shared_ptr<const std::vector<std::string>>(
            std::make_shared<std::vector<std::string>>(std::move(keys)));
    auto [it, inserted] =
        dictionaryKeyCache_.emplace(moduleName, std::move(sharedKeys));
    (void)inserted;
    return it->second;
}

std::string SwordManager::getDictionaryEntry(const std::string& moduleName,
                                             const std::string& key,
                                             std::string* resolvedKeyOut) {
    std::lock_guard<std::mutex> lock(mutex_);
    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return "<p><i>Dictionary module not found: " + htmlEscapeAttr(moduleName) + "</i></p>";
    if (resolvedKeyOut) resolvedKeyOut->clear();

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

    if (!resolvedKey.empty()) {
        displayKey = resolvedKey;
    }

    if (text.empty()) {
        return "<p><i>No entry found for: " + htmlEscapeAttr(requestedKey) + "</i></p>";
    }
    if (resolvedKeyOut) {
        *resolvedKeyOut = resolvedKey.empty() ? displayKey : resolvedKey;
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
    if (!mod) return "<p><i>General book module not found: " + htmlEscapeAttr(moduleName) + "</i></p>";

    std::vector<GeneralBookTocEntry> toc;
    {
        std::unique_ptr<sword::SWKey> tocKey(mod->createKey());
        auto* treeKey = dynamic_cast<sword::TreeKey*>(tocKey.get());
        if (treeKey) {
            treeKey->root();
            if (treeKey->hasChildren() && treeKey->firstChild()) {
                appendGeneralBookTocEntries(treeKey, 0, toc);
            }
        }
    }

    std::string lookupKey = trimCopy(key);
    if (lookupKey.empty() && !toc.empty()) {
        lookupKey = toc.front().key;
    }

    if (lookupKey.empty()) {
        std::ostringstream html;
        html << "<div class=\"general-book\">\n";
        html << "<div class=\"entry-key\">" << htmlEscapeAttr(moduleName) << "</div>\n";
        const char* desc = mod->getDescription();
        if (desc && *desc) {
            html << "<p>" << desc << "</p>\n";
        }
        html << "<p><i>Select a table-of-contents item to open a section.</i></p>\n";
        html << "</div>\n";
        return html.str();
    }

    int tocIndex = findGeneralBookTocIndex(toc, lookupKey);
    if (tocIndex >= 0) {
        const GeneralBookTocEntry& entry = toc[static_cast<size_t>(tocIndex)];
        mod->setKey(entry.key.c_str());
        if (mod->popError()) {
            return "<p><i>No entry found for: " + htmlEscapeAttr(lookupKey) + "</i></p>";
        }
        std::string text = std::string(mod->renderText().c_str());
        text = this->postProcessHtml(text);
        std::string html = renderGeneralBookSectionHtml(entry, text);
        if (!trimCopy(html).empty()) {
            return html;
        }
        if (entry.hasChildren) {
            std::ostringstream placeholder;
            placeholder << "<div class=\"general-book\">\n";
            placeholder << "<div class=\"entry-key\">"
                        << htmlEscapeAttr(entry.label) << "</div>\n";
            placeholder << "<p><i>No direct text for this heading. "
                        << "Subsections appear below or can be opened from Contents.</i></p>\n";
            placeholder << "</div>\n";
            return placeholder.str();
        }
        return "<p><i>No entry found for: " + htmlEscapeAttr(lookupKey) + "</i></p>";
    }

    mod->setKey(lookupKey.c_str());
    if (!mod->popError()) {
        std::string text = std::string(mod->renderText().c_str());
        text = this->postProcessHtml(text);
        if (!trimCopy(text).empty()) {
            std::ostringstream html;
            html << "<div class=\"general-book\">\n";
            html << "<div class=\"entry-key\">" << htmlEscapeAttr(lookupKey) << "</div>\n";
            html << text;
            html << "</div>\n";
            return html.str();
        }
    }
    return "<p><i>No entry found for: " + htmlEscapeAttr(lookupKey) + "</i></p>";
}

std::string SwordManager::verseReferenceFromLink(const std::string& url) {
    std::string decoded = trimCopy(decodeHtmlEntities(url));
    if (decoded.empty()) return "";

    std::string lower = toLowerAscii(decoded);
    if (lower.rfind("sword://", 0) == 0) {
        return trimCopy(decoded.substr(8));
    }

    if (!containsNoCase(lower, "passagestudy.jsp")) return "";
    std::string action = toLowerAscii(extractQueryValue(decoded, "action"));
    std::string type = toLowerAscii(extractQueryValue(decoded, "type"));
    if (action == "showref" && (type.empty() || type == "scripref")) {
        return normalizeLinkedVerseRef(extractQueryValue(decoded, "value"));
    }

    return "";
}

std::vector<std::string> SwordManager::verseReferencesFromLink(
    const std::string& url,
    const std::string& defaultKey,
    const std::string& verseModuleForRefs) const {
    std::string decoded = trimCopy(decodeHtmlEntities(url));
    if (decoded.empty()) return {};

    std::string rawRefs;
    std::string lower = toLowerAscii(decoded);
    if (lower.rfind("sword://", 0) == 0) {
        rawRefs = trimCopy(decoded.substr(8));
    } else if (containsNoCase(lower, "passagestudy.jsp")) {
        std::string action = toLowerAscii(extractQueryValue(decoded, "action"));
        std::string type = toLowerAscii(extractQueryValue(decoded, "type"));
        if (action == "showref" && (type.empty() || type == "scripref")) {
            rawRefs = trimCopy(extractQueryValue(decoded, "value"));
        }
    }

    if (rawRefs.empty()) return {};

    sword::VerseKey vk;
    vk.setAutoNormalize(true);

    std::string verseModule = trimCopy(verseModuleForRefs);
    std::string versificationName;
    if (!verseModule.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        sword::SWModule* mod = getModule(verseModule);
        if (mod) {
            const char* v11n = mod->getConfigEntry("Versification");
            if (v11n && *v11n) {
                versificationName = v11n;
                vk.setVersificationSystem(v11n);
            }
        }
    }

    std::string defaultRef = trimCopy(defaultKey);
    auto parseRefs = [&](const std::string& refText) {
        std::vector<std::string> parsed;
        sword::ListKey refs = vk.parseVerseList(
            refText.c_str(),
            defaultRef.empty() ? nullptr : defaultRef.c_str(),
            true);
        for (refs = sword::TOP; !refs.popError(); refs++) {
            const char* current = refs.getText();
            if (!current || !*current) continue;
            std::string normalized = trimCopy(current);
            if (!normalized.empty()) parsed.push_back(std::move(normalized));
        }
        return parsed;
    };

    auto parseClauseRefs = [&](const std::string& refText,
                               const std::string& clauseDefaultRef) {
        auto parseWithDefault = [&](const std::string& candidate,
                                    const std::string& currentDefaultRef) {
            std::vector<std::string> parsed;
            sword::ListKey refs = vk.parseVerseList(
                candidate.c_str(),
                currentDefaultRef.empty() ? nullptr : currentDefaultRef.c_str(),
                true);
            for (refs = sword::TOP; !refs.popError(); refs++) {
                const char* current = refs.getText();
                if (!current || !*current) continue;
                std::string normalized = trimCopy(current);
                if (!normalized.empty()) parsed.push_back(std::move(normalized));
            }
            return parsed;
        };

        std::vector<std::string> out;
        std::string itemDefaultRef = clauseDefaultRef;
        std::vector<std::string> items = splitList(refText, ',');
        if (items.empty()) items.push_back(refText);

        for (const auto& item : items) {
            std::string normalizedItem = normalizeLinkedVerseRef(item);
            if (normalizedItem.empty()) continue;

            std::string repairedItem =
                repairLinkedVerseRef(normalizedItem, versificationName);

            std::vector<std::string> parsed =
                parseWithDefault(normalizedItem, itemDefaultRef);
            if (!parsed.empty() &&
                resolvedRefsMatchExpected(parsed, repairedItem, versificationName)) {
                itemDefaultRef = parsed.back();
                out.insert(out.end(), parsed.begin(), parsed.end());
                continue;
            }

            if (!repairedItem.empty() && repairedItem != normalizedItem) {
                std::vector<std::string> repairedParsed =
                    parseWithDefault(repairedItem, itemDefaultRef);
                if (!repairedParsed.empty() &&
                    resolvedRefsMatchExpected(repairedParsed,
                                              repairedItem,
                                              versificationName)) {
                    itemDefaultRef = repairedParsed.back();
                    out.insert(out.end(),
                               repairedParsed.begin(),
                               repairedParsed.end());
                    continue;
                }
                if (!repairedParsed.empty()) {
                    itemDefaultRef = repairedParsed.back();
                    out.insert(out.end(),
                               repairedParsed.begin(),
                               repairedParsed.end());
                    continue;
                }
            }

            if (!parsed.empty()) {
                itemDefaultRef = parsed.back();
                out.insert(out.end(), parsed.begin(), parsed.end());
                continue;
            }

            if (!repairedItem.empty()) {
                itemDefaultRef = repairedItem;
                out.push_back(repairedItem);
                continue;
            }

            itemDefaultRef = normalizedItem;
            out.push_back(normalizedItem);
        }

        return out;
    };

    std::string normalizedRawRefs = normalizeLinkedVerseRef(rawRefs);
    std::string repairedRefs =
        repairLinkedVerseRef(normalizedRawRefs, versificationName);

    if (rawRefs.find(';') != std::string::npos ||
        rawRefs.find(',') != std::string::npos) {
        std::vector<std::string> out;
        std::string clauseDefaultRef = defaultRef;
        std::vector<std::string> clauses = splitList(rawRefs, ';');
        for (const auto& clause : clauses) {
            std::vector<std::string> clauseRefs =
                parseClauseRefs(clause, clauseDefaultRef);
            if (clauseRefs.empty()) continue;
            clauseDefaultRef = clauseRefs.back();
            out.insert(out.end(), clauseRefs.begin(), clauseRefs.end());
        }
        if (!out.empty()) return out;
    }

    std::vector<std::string> out = parseRefs(rawRefs);
    if (!out.empty() &&
        resolvedRefsMatchExpected(out, repairedRefs, versificationName)) {
        return out;
    }

    if (!repairedRefs.empty() && repairedRefs != rawRefs) {
        std::vector<std::string> repairedOut = parseRefs(repairedRefs);
        if (!repairedOut.empty() &&
            resolvedRefsMatchExpected(repairedOut,
                                      repairedRefs,
                                      versificationName)) {
            return repairedOut;
        }
    }

    if (!repairedRefs.empty()) {
        return {repairedRefs};
    }
    if (!normalizedRawRefs.empty()) {
        return {normalizedRawRefs};
    }

    return {};
}

std::string SwordManager::buildLinkPreviewHtml(const std::string& sourceModule,
                                               const std::string& sourceKey,
                                               const std::string& url,
                                               const std::string& verseModuleForRefs) {
    auto resolveVerseModule = [&]() -> std::string {
        std::string moduleName = trimCopy(verseModuleForRefs);
        if (!moduleName.empty()) return moduleName;

        if (!trimCopy(sourceModule).empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            sword::SWModule* source = getModule(sourceModule);
            if (source && source->getType() &&
                std::string(source->getType()) == "Biblical Texts") {
                return sourceModule;
            }
        }

        auto bibles = getBibleModules();
        if (!bibles.empty()) return bibles.front().name;
        return "";
    };

    std::vector<std::string> verseRefs = verseReferencesFromLink(
        url, sourceKey, verseModuleForRefs);
    if (verseRefs.size() == 1) {
        std::string verseModule = resolveVerseModule();
        if (verseModule.empty()) return "";
        return getVerseText(verseModule, verseRefs.front());
    }
    if (verseRefs.size() > 1) {
        return "";
    }

    std::string decoded = trimCopy(decodeHtmlEntities(url));
    if (decoded.empty() || !containsNoCase(decoded, "passagestudy.jsp")) {
        return "";
    }

    std::string action = toLowerAscii(extractQueryValue(decoded, "action"));
    if (action != "shownote") return "";

    std::string noteModule = trimCopy(extractQueryValue(decoded, "module"));
    if (noteModule.empty()) noteModule = trimCopy(sourceModule);
    std::string noteValue = trimCopy(extractQueryValue(decoded, "value"));
    std::string noteType = toLowerAscii(extractQueryValue(decoded, "type"));
    std::string passage = trimCopy(extractQueryValue(decoded, "passage"));
    if (passage.empty()) passage = trimCopy(sourceKey);
    if (noteModule.empty() || noteValue.empty() || passage.empty()) return "";

    std::string renderedBody;
    std::string refList;
    std::string noteLabel;
    std::string noteKind;
    std::string noteOsisRef;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sword::SWModule* mod = getModule(noteModule);
        if (!mod) return "";

        mod->setProcessEntryAttributes(true);
        mod->setKey(passage.c_str());
        (void)mod->renderText();
        auto& attrs = mod->getEntryAttributes();
        auto typeIt = attrs.find("Footnote");
        if (typeIt == attrs.end()) return "";
        auto entryIt = typeIt->second.find(noteValue.c_str());
        if (entryIt == typeIt->second.end()) return "";

        auto bodyIt = entryIt->second.find("body");
        if (bodyIt != entryIt->second.end()) {
            renderedBody = std::string(
                mod->renderText(bodyIt->second.c_str(), -1, true).c_str());
            renderedBody = postProcessHtml(renderedBody);
        }
        auto refListIt = entryIt->second.find("refList");
        if (refListIt != entryIt->second.end()) {
            refList = refListIt->second.c_str();
        }
        auto labelIt = entryIt->second.find("n");
        if (labelIt != entryIt->second.end()) {
            noteLabel = labelIt->second.c_str();
        }
        auto kindIt = entryIt->second.find("type");
        if (kindIt != entryIt->second.end()) {
            noteKind = toLowerAscii(kindIt->second.c_str());
        }
        auto osisIt = entryIt->second.find("osisRef");
        if (osisIt != entryIt->second.end()) {
            noteOsisRef = osisIt->second.c_str();
        }
    }

    if (noteKind.empty()) noteKind = noteType;

    if ((noteKind == "crossreference" || noteType == "x") && !refList.empty()) {
        std::string verseModule = resolveVerseModule();
        if (verseModule.empty()) return "";

        std::vector<std::string> refs = splitList(refList, ';');
        constexpr size_t kMaxPreviewRefs = 8;
        const size_t shown = std::min(refs.size(), kMaxPreviewRefs);

        std::ostringstream html;
        html << "<div class=\"link-preview crossref-preview\">\n";
        html << "<div class=\"entry-key\">Cross references";
        if (!noteOsisRef.empty()) {
            html << " for " << htmlEscapeAttr(normalizeLinkedVerseRef(noteOsisRef));
        }
        html << "</div>\n";
        for (size_t i = 0; i < shown; ++i) {
            std::string ref = normalizeLinkedVerseRef(refs[i]);
            if (ref.empty()) continue;
            html << "<div class=\"crossref-item\">\n";
            html << getVerseText(verseModule, ref);
            html << "</div>\n";
        }
        if (refs.size() > shown) {
            html << "<p><i>Showing " << shown << " of " << refs.size()
                 << " cross references.</i></p>\n";
        }
        html << "</div>\n";
        return html.str();
    }

    if (!renderedBody.empty()) {
        std::ostringstream html;
        html << "<div class=\"link-preview note-preview\">\n";
        html << "<div class=\"entry-key\">Footnote";
        if (!noteLabel.empty()) {
            html << " " << htmlEscapeAttr(noteLabel);
        }
        if (!noteOsisRef.empty()) {
            html << " on " << htmlEscapeAttr(normalizeLinkedVerseRef(noteOsisRef));
        }
        html << "</div>\n";
        html << renderedBody << "\n";
        html << "</div>\n";
        return html.str();
    }

    return "";
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

    // Perform search with optional progress callback
    void (*percentUpdate)(char, void*) = nullptr;
    void* percentUserData = nullptr;
    if (callback) {
        percentUpdate = [](char percent, void* userData) {
            auto* cb = static_cast<std::function<void(float)>*>(userData);
            (*cb)(static_cast<float>(percent) / 100.0f);
        };
        percentUserData = &callback;
    }
    sword::ListKey& resultKeys = mod->search(
        searchText.c_str(), searchType, 0, scopePtr,
        nullptr, percentUpdate, percentUserData
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
        result.title = keyText;
        result.resourceType = searchResourceTypeTokenForModuleType(
            mod->getType() ? mod->getType() : "");

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
        result.title = keyText;
        result.resourceType = searchResourceTypeTokenForModuleType(
            mod->getType() ? mod->getType() : "");

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
                                   wordLower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    std::transform(attrLower.begin(), attrLower.end(),
                                   attrLower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });

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

    sword::VerseKey* vk = verseKeyForModule(mod);
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

std::vector<std::string> SwordManager::getBookNamesForTestament(
    const std::string& moduleName,
    int testament) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> books;

    if (testament != 1 && testament != 2) return books;

    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return books;

    sword::VerseKey* vk = verseKeyForModule(mod);
    if (!vk) return books;

    vk->setTestament(testament);
    for (int b = 1; b <= vk->getBookMax(); ++b) {
        vk->setBook(b);
        books.push_back(vk->getBookName());
    }

    return books;
}

int SwordManager::getChapterCount(const std::string& moduleName,
                                   const std::string& book) {
    std::lock_guard<std::mutex> lock(mutex_);

    sword::SWModule* mod = getModule(moduleName);
    if (!mod) return 0;

    sword::VerseKey* vk = verseKeyForModule(mod);
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
        try {
            result.chapter = std::stoi(afterBook.substr(0, colonPos));
            std::string verseStr = afterBook.substr(colonPos + 1);

            size_t dashPos = verseStr.find('-');
            if (dashPos != std::string::npos) {
                result.verse = std::stoi(verseStr.substr(0, dashPos));
                result.verseEnd = std::stoi(verseStr.substr(dashPos + 1));
            } else {
                result.verse = std::stoi(verseStr);
            }
        } catch (...) {
            result.book = ref;
            result.chapter = 0;
            result.verse = 0;
            result.verseEnd = 0;
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

std::string SwordManager::renderedChapterHeadingLocked(sword::SWModule* mod,
                                                       const std::string& book,
                                                       int chapter) const {
    if (!mod || book.empty() || chapter <= 0) return "";

    sword::VerseKey* vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    if (!vk) {
        sword::VerseKey tempKey;
        mod->setKey(tempKey);
        vk = dynamic_cast<sword::VerseKey*>(mod->getKey());
    }
    if (!vk) return "";

    const std::string oldKey = mod->getKeyText();
    const bool oldIntros = vk->isIntros();
    const bool oldProcessAttrs = mod->isProcessEntryAttributes();

    auto restoreState = [&]() {
        mod->setProcessEntryAttributes(oldProcessAttrs);
        vk->setIntros(oldIntros);
        if (!oldKey.empty()) {
            vk->setText(oldKey.c_str());
        }
    };

    vk->setIntros(true);
    mod->setProcessEntryAttributes(true);

    std::string raw;
    try {
        std::ostringstream introKey;
        introKey << book << " " << chapter << ":0";
        vk->setText(introKey.str().c_str());
        raw = std::string(mod->renderText().c_str());
    } catch (...) {
        restoreState();
        return "";
    }
    restoreState();

    std::string heading = extractChapterHeadingHtml(raw);
    if (trimCopy(stripTags(heading)).empty()) return "";

    return "<div class=\"chapter-heading\">" + postProcessHtml(heading) + "</div>\n";
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

        auto [inserted, ok] = postProcessCache_.emplace(key, PostProcessCacheEntry{
            value, {}
        });
        postProcessLru_.push_front(&inserted->first);
        inserted->second.lruIt = postProcessLru_.begin();

        if (postProcessCache_.size() > kPostProcessCacheLimit) {
            const std::string* evictKey = postProcessLru_.back();
            postProcessCache_.erase(*evictKey);
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
            appendSanitizedText(out,
                                std::string_view(html).substr(pos, end - pos),
                                lastTarget);
            pos = end;
            continue;
        }

        size_t tagEnd = std::string::npos;
        std::string tagName;
        bool isClosing = false;
        bool isSelfClosing = false;
        if (!parseTag(html, pos, tagEnd, tagName, isClosing, isSelfClosing)) {
            appendSanitizedText(out,
                                std::string_view(html).substr(pos, 1),
                                lastTarget);
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
                    out += buildInlineMarkerHtml(block, blockMeta);
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
                            if (codeText) {
                                out += buildInlineMarkerHtml(inner, linkMeta);
                            }
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
    info.description = localizedModuleConfigEntry(mod, "Description");
    if (info.description.empty()) {
        info.description = safeConfigEntry(mod->getDescription());
    }
    info.type = mod->getType();

    auto lang = mod->getLanguage();
    info.language = lang ? lang : "en";
    info.abbreviation = moduleConfigEntry(mod, "Abbreviation");
    info.version = moduleConfigEntry(mod, "Version");
    info.markup = moduleConfigEntry(mod, "SourceType");
    info.category = moduleConfigEntry(mod, "Category");
    info.aboutHtml = formattedModuleConfigEntryHtml(mod, "About");
    info.distributionLicense = moduleConfigEntry(mod, "DistributionLicense");
    info.textSource = moduleConfigEntry(mod, "TextSource");
    info.featureLabels = moduleFeatureLabels(mod);

    info.hasStrongs = moduleHasFeature(mod, "StrongsNumbers") ||
                      std::find(info.featureLabels.begin(),
                                info.featureLabels.end(),
                                "Strong's numbers") != info.featureLabels.end();
    info.hasMorph = std::find(info.featureLabels.begin(),
                              info.featureLabels.end(),
                              "Morphological tags") != info.featureLabels.end();

    return info;
}

} // namespace verdad
