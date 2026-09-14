#include "import/ImportedModuleManager.h"
#include "search/SearchIndexer.h"
#include "search/SearchSnippet.h"
#include "search/SemanticSearch.h"
#include "search/SmartSearch.h"
#include "search/TopicSearch.h"
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
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <regex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace verdad {
namespace {

constexpr int kSearchSchemaVersion = 9;
constexpr size_t kMaxSpellingCandidatesPerToken = 360;
constexpr size_t kMaxGeneralBookTocEntries = 50000;
constexpr int kMaxGeneralBookTocDepth = 128;

struct CatalogSnapshot {
    ModuleInfo info;
    std::string resourceType;
    std::string moduleToken;
    std::string moduleSignature;
};

struct ModuleScanSource {
    std::string moduleName;
    std::string resourceType;
    std::string moduleToken;
    std::string moduleSignature;
    bool imported = false;
    std::vector<ImportedModuleManager::Entry> importedEntries;
    std::vector<std::string> keyedEntries;
    std::unordered_map<std::string, std::string> entryTitles;
    std::unique_ptr<sword::SWConfig> bundledSysConfig;
    std::unique_ptr<sword::SWMgr> mgr;
    sword::SWModule* mod = nullptr;
};

struct MetadataFilter {
    std::string sql;
    std::vector<std::string> values;
};

struct SpellingProbe {
    std::string term;
    bool orthographic = false;
};

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

std::string normalizeDirectSearchText(const std::string& text) {
    return lowerCopy(smart_search::stripDiacritics(text));
}

bool isDirectSearchWordByte(unsigned char c) {
    return std::isalnum(c) || c == '\'' || c == '-' || c >= 0x80;
}

bool containsWholeDirectSearchMatch(std::string_view haystack,
                                    std::string_view needle,
                                    bool prefix = false) {
    if (needle.empty()) return false;

    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
        const size_t end = pos + needle.size();
        const bool leftBoundary =
            (pos == 0) ||
            !isDirectSearchWordByte(static_cast<unsigned char>(haystack[pos - 1]));
        const bool rightBoundary =
            (end >= haystack.size()) ||
            !isDirectSearchWordByte(static_cast<unsigned char>(haystack[end]));
        if (leftBoundary && (prefix || rightBoundary)) return true;
        ++pos;
    }

    return false;
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

std::string regexFtsPrefilter(const std::string& pattern) {
    // FTS matches tokens, while regex can match inside a word. Only filter
    // when a start anchor guarantees a token start, and use a prefix
    // rather than requiring the regex's literal fragment to be a whole word.
    // std::regex's byte-oriented \b is not a unicode61 token boundary.
    if (hasUnsupportedRegexLiteralPrefilterSyntax(pattern)) return "";
    if (pattern.rfind("^", 0) != 0) return "";
    size_t start = pattern.rfind("^\\b", 0) == 0 ? 3 : 1;

    size_t end = start;
    while (end < pattern.size() &&
           static_cast<unsigned char>(pattern[end]) < 0x80 &&
           std::isalnum(static_cast<unsigned char>(pattern[end]))) ++end;
    std::string literal = pattern.substr(start, end - start);
    if (end < pattern.size()) {
        if (pattern[end] == '{') return "";
        if ((pattern[end] == '*' || pattern[end] == '?') && !literal.empty()) {
            literal.pop_back();
        }
    }
    return literal.empty() ? "" : "{title content}:" + quoteFtsToken(literal) + "*";
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

bool hasAsciiLetter(const std::string& text) {
    return std::any_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isalpha(c);
    });
}

std::string normalizeVocabularyTerm(const std::string& raw) {
    std::string token = normalizeWordToken(raw);
    if (token.empty()) return "";

    std::string stripped = lowerCopy(smart_search::stripDiacritics(token));
    std::string out;
    out.reserve(stripped.size());
    for (char c : stripped) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '\'' || c == '-') {
            out.push_back(static_cast<char>(std::tolower(uc)));
        }
    }

    if (out.size() < 3 || out.size() > 32 || !hasAsciiLetter(out)) {
        return "";
    }
    return out;
}

int spellingDistanceThreshold(size_t lhsLen, size_t rhsLen) {
    const size_t len = std::max(lhsLen, rhsLen);
    if (len <= 4) return 1;
    if (len <= 8) return 1;
    return 2;
}

int spellingCandidateLengthWindow(size_t len) {
    return len <= 4 ? 1 : 2;
}

size_t spellingPrefixProbeLength(size_t len) {
    if (len <= 4) return std::min<size_t>(3, len);
    if (len <= 7) return 3;
    return 4;
}

std::string prefixRangeUpperBound(const std::string& prefix) {
    if (prefix.empty()) return "";
    std::string upper = prefix;
    upper.push_back('\x7f');
    return upper;
}

std::vector<std::string> spellingPrefixProbes(const std::string& term) {
    std::vector<std::string> probes;
    std::unordered_set<std::string> seen;
    const size_t prefixLen = spellingPrefixProbeLength(term.size());
    if (prefixLen < 2 || term.size() < prefixLen) return probes;

    auto addProbe = [&](const std::string& value) {
        if (value.size() < 2) return;
        std::string prefix = value.substr(0, std::min(prefixLen, value.size()));
        if (prefix.size() < 2) return;
        if (seen.insert(prefix).second) {
            probes.push_back(std::move(prefix));
        }
    };

    addProbe(term);

    if (term.size() >= 5) {
        const size_t deleteProbeSpan = std::min(term.size(), prefixLen + 2);
        for (size_t i = 0; i < deleteProbeSpan; ++i) {
            std::string deleted = term.substr(0, i) + term.substr(i + 1);
            addProbe(deleted);
        }
    }

    return probes;
}

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void addSpellingProbe(std::vector<SpellingProbe>& probes,
                      std::unordered_set<std::string>& seen,
                      std::string value,
                      bool orthographic) {
    if (value.size() < 2) return;
    if (seen.insert(value).second) {
        probes.push_back({std::move(value), orthographic});
    }
}

void addSuffixSwapProbe(std::vector<SpellingProbe>& probes,
                        std::unordered_set<std::string>& seen,
                        const std::string& term,
                        std::string_view from,
                        std::string_view to) {
    if (!endsWith(term, from) || term.size() <= from.size() + 1) return;
    std::string value = term.substr(0, term.size() - from.size());
    value.append(to);
    addSpellingProbe(probes, seen, std::move(value), true);
}

smart_search::QueryExpansionOptions toQueryExpansionOptions(
    SearchIndexer::SmartSearchOptions options) {
    smart_search::QueryExpansionOptions queryOptions;
    queryOptions.includeSpelling = options.spellingCorrection;
    queryOptions.includeSynonyms = options.includeSynonyms;
    queryOptions.includePartialWords = options.partialWordMatching;
    return queryOptions;
}

std::vector<SpellingProbe> spellingProbeTerms(const std::string& term,
                                              bool includeTypoProbes) {
    std::vector<SpellingProbe> probes;
    std::unordered_set<std::string> seen;
    seen.insert(term);

    if (term.size() < 3 || term.size() > 24) return probes;

    auto addSuffixSwap = [&](std::string_view from, std::string_view to) {
        addSuffixSwapProbe(probes, seen, term, from, to);
    };

    // Common American/British spelling alternations. These are exact probes
    // against indexed terms, so they do not add persistent index rows.
    addSuffixSwap("our", "or");
    addSuffixSwap("ours", "ors");
    addSuffixSwap("oured", "ored");
    addSuffixSwap("ouring", "oring");
    addSuffixSwap("ourful", "orful");
    addSuffixSwap("or", "our");
    addSuffixSwap("ors", "ours");
    addSuffixSwap("ored", "oured");
    addSuffixSwap("oring", "ouring");
    addSuffixSwap("orful", "ourful");
    addSuffixSwap("isation", "ization");
    addSuffixSwap("isations", "izations");
    addSuffixSwap("ise", "ize");
    addSuffixSwap("ises", "izes");
    addSuffixSwap("ised", "ized");
    addSuffixSwap("ising", "izing");
    addSuffixSwap("ization", "isation");
    addSuffixSwap("izations", "isations");
    addSuffixSwap("ize", "ise");
    addSuffixSwap("izes", "ises");
    addSuffixSwap("ized", "ised");
    addSuffixSwap("izing", "ising");
    addSuffixSwap("yse", "yze");
    addSuffixSwap("yses", "yzes");
    addSuffixSwap("ysed", "yzed");
    addSuffixSwap("ysing", "yzing");
    addSuffixSwap("yze", "yse");
    addSuffixSwap("yzes", "yses");
    addSuffixSwap("yzed", "ysed");
    addSuffixSwap("yzing", "ysing");
    addSuffixSwap("re", "er");
    addSuffixSwap("res", "ers");
    addSuffixSwap("red", "ered");
    addSuffixSwap("er", "re");
    addSuffixSwap("ers", "res");
    addSuffixSwap("ered", "red");
    addSuffixSwap("ce", "se");
    addSuffixSwap("ces", "ses");
    addSuffixSwap("ced", "sed");
    addSuffixSwap("cing", "sing");
    addSuffixSwap("se", "ce");
    addSuffixSwap("ses", "ces");
    addSuffixSwap("sed", "ced");
    addSuffixSwap("sing", "cing");
    addSuffixSwap("ogue", "og");
    addSuffixSwap("ogues", "ogs");
    addSuffixSwap("og", "ogue");
    addSuffixSwap("ogs", "ogues");
    addSuffixSwap("lling", "ling");
    addSuffixSwap("lled", "led");
    addSuffixSwap("ller", "ler");
    addSuffixSwap("llor", "lor");
    addSuffixSwap("ling", "lling");
    addSuffixSwap("led", "lled");
    addSuffixSwap("ler", "ller");
    addSuffixSwap("lor", "llor");

    if (!includeTypoProbes) return probes;

    for (size_t i = 0; i + 1 < term.size(); ++i) {
        std::string transposed = term;
        std::swap(transposed[i], transposed[i + 1]);
        addSpellingProbe(probes, seen, std::move(transposed), false);
    }

    if (term.size() <= 10) {
        static const std::unordered_map<char, std::string> kQwertyNeighbors = {
            {'q', "wa"},    {'w', "qeas"},   {'e', "wrsd"},   {'r', "etdf"},
            {'t', "ryfg"},  {'y', "tugh"},   {'u', "yijh"},   {'i', "uokj"},
            {'o', "iplk"},  {'p', "ol"},     {'a', "qwsz"},   {'s', "awedxz"},
            {'d', "serfcx"}, {'f', "drtgvc"}, {'g', "ftyhbv"}, {'h', "gyujnb"},
            {'j', "huikmn"}, {'k', "jiolm"}, {'l', "kop"},    {'z', "asx"},
            {'x', "zsdc"},  {'c', "xdfv"},   {'v', "cfgb"},   {'b', "vghn"},
            {'n', "bhjm"},  {'m', "njk"},
        };

        for (size_t i = 0; i < term.size(); ++i) {
            auto it = kQwertyNeighbors.find(term[i]);
            if (it == kQwertyNeighbors.end()) continue;
            for (char replacement : it->second) {
                std::string value = term;
                value[i] = replacement;
                addSpellingProbe(probes, seen, std::move(value), false);
            }
        }
    }

    return probes;
}

struct VocabularyAggregate {
    std::string moduleName;
    std::string term;
    std::string phoneticKey;
    int docCount = 0;
    int totalCount = 0;
};

void collectVocabularyTerms(
    std::unordered_map<std::string, VocabularyAggregate>& vocabulary,
    const std::string& moduleName,
    const std::string& title,
    const std::string& content) {

    std::unordered_set<std::string> seenInEntry;
    for (const auto& raw : tokenizeWords(title + " " + content)) {
        std::string term = normalizeVocabularyTerm(raw);
        if (term.empty()) continue;

        auto [it, inserted] = vocabulary.emplace(term, VocabularyAggregate{});
        VocabularyAggregate& aggregate = it->second;
        if (inserted) {
            aggregate.moduleName = moduleName;
            aggregate.term = term;
            aggregate.phoneticKey = smart_search::metaphoneKey(term);
        }

        ++aggregate.totalCount;
        if (seenInEntry.insert(term).second) {
            ++aggregate.docCount;
        }
    }
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

std::string joinStrongsTokens(const std::vector<std::string>& tokens) {
    std::ostringstream out;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i) out << ' ';
        out << tokens[i];
    }
    return out.str();
}

void appendStrongsIndexTokens(const std::string& text,
                              std::vector<std::string>& tokens,
                              std::unordered_set<std::string>& seen) {
    for (const auto& token : extractStrongsTokens(text)) {
        if (token.empty()) continue;
        char prefix = static_cast<char>(std::toupper(
            static_cast<unsigned char>(token[0])));
        if (prefix != 'H' && prefix != 'G') continue;
        if (seen.insert(token).second) {
            tokens.push_back(token);
        }
    }
}

std::string buildCompactStrongsIndexText(sword::SWModule* mod,
                                         const std::string& xhtml) {
    std::vector<std::string> tokens;
    std::unordered_set<std::string> seen;

    if (mod) {
        auto& attrs = mod->getEntryAttributes();
        auto wordIt = attrs.find("Word");
        if (wordIt != attrs.end()) {
            for (const auto& entry : wordIt->second) {
                const auto& wordAttrs = entry.second;
                auto lemmaIt = wordAttrs.find("Lemma");
                if (lemmaIt == wordAttrs.end()) continue;
                appendStrongsIndexTokens(lemmaIt->second.c_str(),
                                         tokens,
                                         seen);
            }
        }
    }

    static const std::regex kTypedStrongValueRe(
        R"(type\s*=\s*([A-Za-z]+).*?(?:&|&amp;)value\s*=\s*(\d+[A-Za-z]?))",
        std::regex::icase);
    auto valueIt = std::sregex_iterator(xhtml.begin(), xhtml.end(), kTypedStrongValueRe);
    const auto valueEnd = std::sregex_iterator();
    for (; valueIt != valueEnd; ++valueIt) {
        std::string type = lowerCopy((*valueIt)[1].str());
        char prefix = 0;
        if (type == "greek") prefix = 'G';
        if (type == "hebrew") prefix = 'H';
        if (!prefix) continue;
        appendStrongsIndexTokens(std::string(1, prefix) + (*valueIt)[2].str(),
                                 tokens,
                                 seen);
    }

    static const std::regex kPrefixedTokenRe(R"(([HhGg]\d+[A-Za-z]?))");

    auto it = std::sregex_iterator(xhtml.begin(), xhtml.end(), kPrefixedTokenRe);
    const auto end = std::sregex_iterator();
    for (; it != end; ++it) {
        appendStrongsIndexTokens((*it)[1].str(), tokens, seen);
    }

    return joinStrongsTokens(tokens);
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

std::string makeBibleTestamentToken(int testament) {
    return testament == 2 ? "nt" : "ot";
}

std::string normalizeDictionaryLookupText(const std::string& text) {
    return search_snippet::collapseWhitespace(
        lowerCopy(smart_search::stripDiacritics(trimCopy(text))));
}

MetadataFilter buildMetadataFilter(const SearchIndexer::SearchRequest& request,
                                   const std::string& alias) {
    MetadataFilter filter;
    const std::string prefix = alias.empty() ? "" : alias + ".";
    std::vector<std::string> clauses;

    if (!request.resourceTypes.empty()) {
        std::ostringstream clause;
        clause << prefix << "resource_type";
        if (request.resourceTypes.size() == 1) {
            clause << " = ?";
        } else {
            clause << " IN (";
            for (size_t i = 0; i < request.resourceTypes.size(); ++i) {
                if (i) clause << ", ";
                clause << "?";
            }
            clause << ")";
        }
        clauses.push_back(clause.str());
        for (const auto& type : request.resourceTypes) {
            filter.values.push_back(type);
        }
    }

    if (!request.moduleName.empty()) {
        clauses.push_back(prefix + "module_token = ?");
        filter.values.push_back(normalizeFilterToken(request.moduleName));
    }

    switch (request.bibleScope) {
    case SearchIndexer::SearchRequest::BibleScope::OldTestament:
        clauses.push_back(prefix + "scope_token = ?");
        filter.values.push_back("ot");
        break;
    case SearchIndexer::SearchRequest::BibleScope::NewTestament:
        clauses.push_back(prefix + "scope_token = ?");
        filter.values.push_back("nt");
        break;
    case SearchIndexer::SearchRequest::BibleScope::CurrentBook: {
        std::string bookToken = normalizeFilterToken(request.currentBook);
        if (!bookToken.empty()) {
            clauses.push_back(prefix + "book_token = ?");
            filter.values.push_back(std::move(bookToken));
        }
        break;
    }
    case SearchIndexer::SearchRequest::BibleScope::All:
        break;
    }

    for (size_t i = 0; i < clauses.size(); ++i) {
        if (i) filter.sql += " AND ";
        filter.sql += clauses[i];
    }
    return filter;
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
        << (module.hasMorph ? '1' : '0')
        << "\nsearch_schema=" << kSearchSchemaVersion;
    for (const auto& feature : module.featureLabels) {
        out << '\n' << feature;
    }
    return out.str();
}

int readSearchDatabaseUserVersion(const std::string& dbPath) {
    if (dbPath.empty() || !std::filesystem::exists(dbPath)) {
        return 0;
    }

    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(dbPath.c_str(), &db,
                             SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK || !db) {
        if (db) sqlite3_close(db);
        return 0;
    }

    int version = 0;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            version = sqlite3_column_int(stmt, 0);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(db);
    return version;
}

void removeSearchDatabaseCacheFiles(const std::string& dbPath) {
    std::error_code ec;
    std::filesystem::remove(dbPath + "-wal", ec);
    ec.clear();
    std::filesystem::remove(dbPath + "-shm", ec);
    ec.clear();
    std::filesystem::remove(dbPath, ec);
}

void appendGeneralBookTocEntriesBounded(sword::TreeKey* treeKey,
                                        int depth,
                                        std::vector<GeneralBookTocEntry>& out,
                                        size_t& visitedEntries) {
    if (!treeKey) return;
    if (depth > kMaxGeneralBookTocDepth) return;

    do {
        if (visitedEntries >= kMaxGeneralBookTocEntries) return;
        ++visitedEntries;

        GeneralBookTocEntry entry;
        entry.key = treeKey->getText();
        entry.label = treeKey->getLocalName();
        entry.depth = depth;
        entry.hasChildren = treeKey->hasChildren();
        out.push_back(std::move(entry));

        if (treeKey->hasChildren() && treeKey->firstChild()) {
            appendGeneralBookTocEntriesBounded(treeKey, depth + 1, out,
                                               visitedEntries);
            treeKey->parent();
        }
    } while (visitedEntries < kMaxGeneralBookTocEntries &&
             treeKey->nextSibling());
}

void appendGeneralBookTocEntries(sword::TreeKey* treeKey,
                                 int depth,
                                 std::vector<GeneralBookTocEntry>& out) {
    size_t visitedEntries = 0;
    appendGeneralBookTocEntriesBounded(treeKey, depth, out, visitedEntries);
}

std::unique_ptr<sword::SWMgr> createIsolatedSwordManager(
    std::unique_ptr<sword::SWConfig>& bundledSysConfigOut) {
    const std::string bundlePath = bundledSwordDataPath();
    if (!bundlePath.empty()) {
        bundledSysConfigOut = std::make_unique<sword::SWConfig>();
        bundledSysConfigOut->setValue("Install", "DataPath", bundlePath.c_str());

        auto& install = bundledSysConfigOut->getSection("Install");
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

        return std::unique_ptr<sword::SWMgr>(new sword::SWMgr(
            nullptr, bundledSysConfigOut.get(), true,
            new sword::MarkupFilterMgr(sword::FMT_XHTML, sword::ENC_UTF8)));
    }

    return std::unique_ptr<sword::SWMgr>(new sword::SWMgr(
        nullptr, nullptr, true,
        new sword::MarkupFilterMgr(sword::FMT_XHTML, sword::ENC_UTF8)));
}

void collectSequentialKeys(sword::SWModule* target,
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
}

bool prepareModuleScanSource(const std::string& moduleName,
                             const CatalogSnapshot& catalog,
                             const ImportedModuleManager* importedModuleMgr,
                             std::string& errorOut,
                             ModuleScanSource& out) {
    out = ModuleScanSource{};
    out.moduleName = moduleName;

    if (importedModuleMgr && importedModuleMgr->hasModule(moduleName)) {
        out.imported = true;
        out.resourceType = "general_book";
        out.moduleToken = !catalog.moduleToken.empty()
            ? catalog.moduleToken
            : normalizeFilterToken(moduleName);
        out.moduleSignature = !catalog.moduleSignature.empty()
            ? catalog.moduleSignature
            : buildModuleSignature(importedModuleMgr->moduleInfo(moduleName),
                                   out.resourceType,
                                   out.moduleToken);
        out.importedEntries = importedModuleMgr->indexEntries(moduleName);
        if (out.importedEntries.empty()) {
            errorOut = "Imported module has no extracted entries.";
            return false;
        }
        return true;
    }

    out.bundledSysConfig.reset();
    out.mgr = createIsolatedSwordManager(out.bundledSysConfig);
    if (out.mgr) {
        for (const auto& path : allUserSwordDataPaths()) {
            out.mgr->augmentModules(path.c_str());
        }
    }
    out.mod = out.mgr ? out.mgr->getModule(moduleName.c_str()) : nullptr;
    if (!out.mod) {
        errorOut = "Module not found while rebuilding index.";
        return false;
    }

    out.resourceType = !catalog.resourceType.empty()
                           ? catalog.resourceType
                           : searchResourceTypeTokenForModuleType(
                                 out.mod->getType() ? out.mod->getType() : "");
    if (!isSearchableResourceTypeToken(out.resourceType)) {
        errorOut = "Unsupported searchable module type.";
        return false;
    }

    out.moduleToken = !catalog.moduleToken.empty()
                          ? catalog.moduleToken
                          : normalizeFilterToken(moduleName);
    out.moduleSignature = !catalog.moduleSignature.empty()
                              ? catalog.moduleSignature
                              : buildModuleSignature(
                                    ModuleInfo{moduleName, "",
                                               out.mod->getType() ? out.mod->getType() : ""},
                                    out.resourceType,
                                    out.moduleToken);

    out.mgr->setGlobalOption("Strong's Numbers", "On");
    out.mgr->setGlobalOption("Morphological Tags", "On");
    out.mgr->setGlobalOption("Footnotes", "On");
    out.mgr->setGlobalOption("Cross-references", "On");
    out.mgr->setGlobalOption("Headings", "On");

    std::unique_ptr<sword::SWKey> createdKey(out.mod->createKey());
    if (out.resourceType == "general_book") {
        if (auto* treeKey = dynamic_cast<sword::TreeKey*>(createdKey.get())) {
            std::vector<GeneralBookTocEntry> toc;
            treeKey->root();
            if (treeKey->hasChildren() && treeKey->firstChild()) {
                appendGeneralBookTocEntries(treeKey, 0, toc);
            }
            for (const auto& entry : toc) {
                std::string key = trimCopy(entry.key);
                if (key.empty()) continue;
                out.keyedEntries.push_back(key);
                std::string label = trimCopy(entry.label);
                out.entryTitles.emplace(key, label.empty() ? key : label);
            }
        } else {
            collectSequentialKeys(out.mod, out.keyedEntries, out.entryTitles);
        }
    } else {
        collectSequentialKeys(out.mod, out.keyedEntries, out.entryTitles);
    }

    if (out.keyedEntries.empty()) {
        errorOut = out.resourceType == "bible"
                       ? "Bible module has no traversable entries."
                       : "Module has no traversable entries to index.";
        return false;
    }

    return true;
}

bool prepareModuleLookupSource(const std::string& moduleName,
                               const CatalogSnapshot& catalog,
                               const ImportedModuleManager* importedModuleMgr,
                               std::string& errorOut,
                               ModuleScanSource& out) {
    out = ModuleScanSource{};
    out.moduleName = moduleName;

    if (importedModuleMgr && importedModuleMgr->hasModule(moduleName)) {
        out.imported = true;
        out.resourceType = "general_book";
        out.moduleToken = !catalog.moduleToken.empty()
            ? catalog.moduleToken
            : normalizeFilterToken(moduleName);
        out.moduleSignature = !catalog.moduleSignature.empty()
            ? catalog.moduleSignature
            : buildModuleSignature(importedModuleMgr->moduleInfo(moduleName),
                                   out.resourceType,
                                   out.moduleToken);
        out.importedEntries = importedModuleMgr->indexEntries(moduleName);
        if (out.importedEntries.empty()) {
            errorOut = "Imported module has no extracted entries.";
            return false;
        }
        return true;
    }

    out.bundledSysConfig.reset();
    out.mgr = createIsolatedSwordManager(out.bundledSysConfig);
    if (out.mgr) {
        for (const auto& path : allUserSwordDataPaths()) {
            out.mgr->augmentModules(path.c_str());
        }
    }
    out.mod = out.mgr ? out.mgr->getModule(moduleName.c_str()) : nullptr;
    if (!out.mod) {
        errorOut = "Module not found while reading indexed search result.";
        return false;
    }

    out.resourceType = !catalog.resourceType.empty()
                           ? catalog.resourceType
                           : searchResourceTypeTokenForModuleType(
                                 out.mod->getType() ? out.mod->getType() : "");
    if (!isSearchableResourceTypeToken(out.resourceType)) {
        errorOut = "Unsupported searchable module type.";
        return false;
    }

    out.moduleToken = !catalog.moduleToken.empty()
                          ? catalog.moduleToken
                          : normalizeFilterToken(moduleName);
    out.moduleSignature = !catalog.moduleSignature.empty()
                              ? catalog.moduleSignature
                              : buildModuleSignature(
                                    ModuleInfo{moduleName, "",
                                               out.mod->getType() ? out.mod->getType() : ""},
                                    out.resourceType,
                                    out.moduleToken);
    return true;
}

bool matchesBibleScope(const SearchIndexer::SearchRequest& request,
                       sword::SWModule* mod) {
    if (!mod || request.bibleScope == SearchIndexer::SearchRequest::BibleScope::All) {
        return true;
    }

    auto* verseKey = dynamic_cast<sword::VerseKey*>(mod->getKey());
    if (!verseKey) return true;

    switch (request.bibleScope) {
    case SearchIndexer::SearchRequest::BibleScope::All:
        return true;
    case SearchIndexer::SearchRequest::BibleScope::OldTestament:
        return verseKey->getTestament() != 2;
    case SearchIndexer::SearchRequest::BibleScope::NewTestament:
        return verseKey->getTestament() == 2;
    case SearchIndexer::SearchRequest::BibleScope::CurrentBook:
        return normalizeFilterToken(verseKey->getBookName() ? verseKey->getBookName() : "") ==
               normalizeFilterToken(request.currentBook);
    }

    return true;
}

bool matchesBibleReferenceScope(const SearchIndexer::SearchRequest& request,
                                const std::string& reference) {
    if (request.bibleScope == SearchIndexer::SearchRequest::BibleScope::All) return true;
    sword::VerseKey key;
    key.setText(reference.c_str());
    if (key.popError()) return false;
    switch (request.bibleScope) {
    case SearchIndexer::SearchRequest::BibleScope::OldTestament:
        return key.getTestament() != 2;
    case SearchIndexer::SearchRequest::BibleScope::NewTestament:
        return key.getTestament() == 2;
    case SearchIndexer::SearchRequest::BibleScope::CurrentBook:
        return normalizeFilterToken(key.getBookName() ? key.getBookName() : "") ==
               normalizeFilterToken(request.currentBook);
    case SearchIndexer::SearchRequest::BibleScope::All:
    default:
        return true;
    }
}

bool bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    return sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

void bindMetadataFilterValues(sqlite3_stmt* stmt,
                              int startIndex,
                              const MetadataFilter& filter) {
    int bindIndex = startIndex;
    for (const auto& value : filter.values) {
        bindText(stmt, bindIndex++, value);
    }
}

bool removeModuleSpellTerms(sqlite3* db, const std::string& moduleName) {
    if (!db || moduleName.empty()) return false;

    sqlite3_stmt* selectStmt = nullptr;
    sqlite3_stmt* updateStmt = nullptr;
    sqlite3_stmt* deleteModuleStmt = nullptr;

    bool ok = true;
    if (sqlite3_prepare_v2(
            db,
            "SELECT term, doc_count, total_count "
            "FROM spell_module_terms WHERE module_name = ?",
            -1, &selectStmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(
            db,
            "UPDATE spell_terms "
            "SET doc_count = doc_count - ?, total_count = total_count - ? "
            "WHERE term = ?",
            -1, &updateStmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(
            db,
            "DELETE FROM spell_module_terms WHERE module_name = ?",
            -1, &deleteModuleStmt, nullptr) != SQLITE_OK) {
        ok = false;
    }

    if (ok) {
        bindText(selectStmt, 1, moduleName);
        while (sqlite3_step(selectStmt) == SQLITE_ROW) {
            const char* termText =
                reinterpret_cast<const char*>(sqlite3_column_text(selectStmt, 0));
            std::string term = termText ? termText : "";
            int docCount = sqlite3_column_int(selectStmt, 1);
            int totalCount = sqlite3_column_int(selectStmt, 2);

            sqlite3_bind_int(updateStmt, 1, docCount);
            sqlite3_bind_int(updateStmt, 2, totalCount);
            bindText(updateStmt, 3, term);
            if (sqlite3_step(updateStmt) != SQLITE_DONE) {
                ok = false;
                break;
            }
            sqlite3_reset(updateStmt);
            sqlite3_clear_bindings(updateStmt);
        }
    }

    if (ok) {
        bindText(deleteModuleStmt, 1, moduleName);
        ok = (sqlite3_step(deleteModuleStmt) == SQLITE_DONE);
    }

    if (selectStmt) sqlite3_finalize(selectStmt);
    if (updateStmt) sqlite3_finalize(updateStmt);
    if (deleteModuleStmt) sqlite3_finalize(deleteModuleStmt);
    if (!ok) return false;

    sqlite3_exec(db,
                 "DELETE FROM spell_terms WHERE total_count <= 0",
                 nullptr, nullptr, nullptr);
    return true;
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

std::string buildSnippetMarkup(const std::string& text,
                               const std::vector<bool>& mask,
                               size_t contextWords = search_snippet::kDefaultContextWords) {
    return search_snippet::buildHighlightedMarkup(text, mask, contextWords);
}

std::string buildPlainSnippet(const std::string& text,
                              const std::vector<bool>& mask,
                              size_t contextWords = search_snippet::kDefaultContextWords) {
    return search_snippet::buildPlainText(text, mask, contextWords);
}

std::string truncateSnippet(const std::string& text) {
    return search_snippet::truncateWords(text);
}

bool isLiteralWordByte(unsigned char c) {
    return std::isalnum(c) || c == '\'' || c == '-' || c >= 0x80;
}

std::vector<bool> buildLiteralMask(const std::string& text,
                                   const std::vector<std::string>& needles,
                                   bool requireWordBoundaries) {
    std::vector<bool> mask(text.size(), false);
    if (text.empty() || needles.empty()) return mask;

    std::string lowerText = lowerCopy(text);
    for (const auto& needleRaw : needles) {
        const bool prefix = !needleRaw.empty() && needleRaw.back() == '*';
        std::string needle = lowerCopy(normalizeWordToken(needleRaw));
        if (needle.empty()) continue;

        size_t pos = 0;
        while ((pos = lowerText.find(needle, pos)) != std::string::npos) {
            size_t end = pos + needle.size();
            if (prefix) {
                while (end < text.size() &&
                       isLiteralWordByte(static_cast<unsigned char>(text[end]))) ++end;
            }
            bool ok = true;
            if (requireWordBoundaries) {
                if (pos > 0 &&
                    isLiteralWordByte(static_cast<unsigned char>(text[pos - 1]))) {
                    ok = false;
                }
                if (end < text.size() &&
                    isLiteralWordByte(static_cast<unsigned char>(text[end]))) {
                    ok = false;
                }
            }
            if (ok) {
                for (size_t i = pos; i < end && i < mask.size(); ++i) {
                    mask[i] = true;
                }
                pos = end;
            } else {
                ++pos;
            }
        }
    }

    return mask;
}

std::string buildWordSnippetFromSourceText(const std::string& plainText,
                                           const std::string& title,
                                           const std::string& query,
                                           bool exactPhrase,
                                           bool includeHighlightMarkup = true) {
    std::vector<std::string> needles;
    if (exactPhrase) {
        std::string phrase = trimCopy(query);
        if (!phrase.empty()) needles.push_back(std::move(phrase));
    } else {
        needles = smart_search::queryTerms(query);
    }

    auto makeSnippet = [&](const std::string& text) {
        std::vector<bool> mask = buildLiteralMask(text, needles, true);
        if (std::find(mask.begin(), mask.end(), true) == mask.end()) {
            return truncateSnippet(text);
        }
        return includeHighlightMarkup
            ? buildSnippetMarkup(text, mask)
            : buildPlainSnippet(text, mask);
    };

    if (!plainText.empty()) {
        std::string snippet = makeSnippet(plainText);
        if (snippet.find("searchhit") != std::string::npos || title.empty()) {
            return snippet;
        }
    }

    if (!title.empty()) {
        return makeSnippet(title);
    }

    return "";
}

std::string buildRegexSnippet(const std::string& text,
                              const std::smatch& match) {
    if (text.empty()) return "";

    const size_t startPos = static_cast<size_t>(match.position());
    const size_t endPos = startPos + static_cast<size_t>(match.length());
    if (startPos >= text.size() || endPos <= startPos) {
        return truncateSnippet(text);
    }

    std::vector<bool> mask(text.size(), false);
    const size_t clippedEnd = std::min(endPos, text.size());
    for (size_t i = startPos; i < clippedEnd; ++i) {
        mask[i] = true;
    }
    return buildSnippetMarkup(text, mask);
}

std::string buildStrongsSnippet(const std::string& xhtml,
                                const std::string& strongsQuery,
                                const std::string& plainFallback) {
    auto truncateFallback = [](const std::string& text) {
        return truncateSnippet(text);
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

    std::string snippet = buildSnippetMarkup(plain, mask);
    return snippet.empty() ? truncateFallback(plainFallback) : snippet;
}

std::string buildSmartSnippetFromSourceText(
    const std::string& plainText,
    const std::string& title,
    const std::vector<std::string>& queryTerms,
    const std::string& language,
    const std::unordered_map<std::string, std::vector<std::string>>& spellingAlternatives,
    SearchIndexer::SmartSearchOptions options) {

    std::string plain = plainText.empty() ? title : plainText;
    if (plain.empty()) return "";

    std::vector<bool> mask(plain.size(), false);

    std::string lowerPlain;
    lowerPlain.resize(plain.size());
    std::transform(plain.begin(), plain.end(), lowerPlain.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string strippedPlain = smart_search::stripDiacritics(plain);
    std::string lowerStrippedPlain;
    lowerStrippedPlain.resize(strippedPlain.size());
    std::transform(strippedPlain.begin(), strippedPlain.end(),
                   lowerStrippedPlain.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

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
                continue;
            }

            size_t seqLen = 1;
            if ((ob & 0xE0) == 0xC0) seqLen = 2;
            else if ((ob & 0xF0) == 0xE0) seqLen = 3;
            else if ((ob & 0xF8) == 0xF0) seqLen = 4;

            unsigned char sb = static_cast<unsigned char>(strippedPlain[stripIdx]);
            if (sb < 0x80) {
                strippedToOrig.push_back(origIdx);
                origIdx += seqLen;
                ++stripIdx;
            } else {
                for (size_t k = 0; k < seqLen && stripIdx < strippedPlain.size(); ++k) {
                    strippedToOrig.push_back(origIdx + k);
                    ++stripIdx;
                }
                origIdx += seqLen;
            }
        }
    }

    auto markInLower = [&](const std::string& haystack,
                           const std::string& needle,
                           bool useStrippedMap,
                           bool prefix = false) {
        if (needle.empty()) return;
        size_t pos = 0;
        while ((pos = haystack.find(needle, pos)) != std::string::npos) {
            size_t end = pos + needle.size();
            const bool leftBoundary =
                (pos == 0) ||
                !isLiteralWordByte(static_cast<unsigned char>(haystack[pos - 1]));
            const bool rightBoundary =
                (end >= haystack.size()) ||
                !isLiteralWordByte(static_cast<unsigned char>(haystack[end]));
            if (!leftBoundary || (!prefix && !rightBoundary)) {
                ++pos;
                continue;
            }
            if (prefix) {
                while (end < haystack.size() &&
                       isLiteralWordByte(static_cast<unsigned char>(haystack[end]))) ++end;
            }
            if (useStrippedMap) {
                size_t origStart = (pos < strippedToOrig.size()) ? strippedToOrig[pos] : pos;
                size_t origEnd = (end > 0 && end - 1 < strippedToOrig.size())
                    ? strippedToOrig[end - 1] + 1
                    : end;
                if (origEnd < plain.size()) {
                    unsigned char b = static_cast<unsigned char>(plain[origEnd - 1]);
                    if (b >= 0x80) {
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
        std::string lowerTerm = lowerCopy(term);
        const bool explicitPrefix = lowerTerm.back() == '*';
        if (explicitPrefix) lowerTerm.pop_back();
        const bool prefix = explicitPrefix ||
            (options.partialWordMatching && lowerTerm.size() >= 4 &&
             lowerTerm.find(' ') == std::string::npos);
        std::string strippedTerm = smart_search::stripDiacritics(lowerTerm);
        std::string lowerStrippedTerm = lowerCopy(strippedTerm);

        markInLower(lowerPlain, lowerTerm, false, prefix);
        markInLower(lowerStrippedPlain, lowerTerm, true, prefix);
        if (lowerStrippedTerm != lowerTerm) {
            markInLower(lowerStrippedPlain, lowerStrippedTerm, true, prefix);
        }

        if (explicitPrefix) continue;
        if (options.includeSynonyms) {
            auto syns = smart_search::expandSynonyms(lowerTerm, language);
            for (const auto& syn : syns) {
                std::string lowerSyn = lowerCopy(syn);
                if (lowerSyn.empty() || lowerSyn == lowerTerm) continue;
                markInLower(lowerPlain, lowerSyn, false);
                std::string lowerStrippedSyn = lowerCopy(
                    smart_search::stripDiacritics(lowerSyn));
                markInLower(lowerStrippedPlain, lowerStrippedSyn, true);
            }
        }

        auto markSpellingAlternatives = [&](const std::string& key) {
            if (!options.spellingCorrection) return;
            auto it = spellingAlternatives.find(key);
            if (it == spellingAlternatives.end()) return;
            for (const auto& alt : it->second) {
                std::string lowerAlt = lowerCopy(alt);
                if (lowerAlt.empty() || lowerAlt == lowerTerm) continue;
                markInLower(lowerPlain, lowerAlt, false);

                std::string lowerStrippedAlt = lowerCopy(
                    smart_search::stripDiacritics(lowerAlt));
                markInLower(lowerStrippedPlain, lowerStrippedAlt, true);
            }
        };
        markSpellingAlternatives(lowerTerm);
        if (lowerStrippedTerm != lowerTerm) {
            markSpellingAlternatives(lowerStrippedTerm);
        }
    }

    std::string snippet = buildSnippetMarkup(plain, mask);
    if (snippet.empty()) snippet = truncateSnippet(plain);
    return snippet;
}

} // namespace

SearchIndexer::SearchIndexer(const std::string& dbPath,
                             const ImportedModuleManager* importedModuleMgr)
    : dbPath_(dbPath)
    , importedModuleMgr_(importedModuleMgr) {
    std::filesystem::path topicPath(dbPath_);
    topicPath.replace_filename("topic_index.db");
    topicSearchProvider_ = std::make_unique<TopicSearchProvider>(topicPath.string());
    int existingSchemaVersion = readSearchDatabaseUserVersion(dbPath_);
    if (existingSchemaVersion > 0 && existingSchemaVersion < kSearchSchemaVersion) {
        removeSearchDatabaseCacheFiles(dbPath_);
    }

    int rc = sqlite3_open_v2(
        dbPath_.c_str(), &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (rc != SQLITE_OK) {
        {
            std::lock_guard<std::mutex> statusLock(backendStatusMutex_);
            backendStatusMessage_ = std::string("Unable to open SQLite search database: ") +
                                    (db_ ? sqlite3_errmsg(db_) : "unknown error");
        }
        std::cerr << "SearchIndexer: unable to open database " << dbPath_
                  << " (" << (db_ ? sqlite3_errmsg(db_) : "unknown error") << ")\n";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        return;
    }

    sqlite3_busy_timeout(db_, 5000);
    applyPragmas(db_);
    if (!ensureSchema(db_)) {
        if (backendStatusMessage().empty()) {
            std::lock_guard<std::mutex> statusLock(backendStatusMutex_);
            backendStatusMessage_ = "Unable to initialize SQLite search schema.";
        }
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }

    backendAvailable_.store(true);
    {
        std::lock_guard<std::mutex> statusLock(backendStatusMutex_);
        backendStatusMessage_.clear();
    }

    workerThread_ = std::thread(&SearchIndexer::workerLoop, this);
}

bool SearchIndexer::topicIndexAvailable() const {
    if (!topicSearchProvider_) return false;
    std::lock_guard<std::mutex> lock(catalogMutex_);
    for (const auto& [name, entry] : moduleCatalog_) {
        if (lowerCopy(name) == "nave") {
            return topicSearchProvider_->readyForSignature(entry.signature);
        }
    }
    return false;
}

std::uint64_t SearchIndexer::topicIndexBytes() const {
    return topicSearchProvider_ ? topicSearchProvider_->databaseBytes() : 0;
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
    semanticAbortRequested_.store(true);
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

    // Constructor-level cache reset handles current schema upgrades cheaply by
    // deleting this generated database before it is opened. Keep this fallback
    // for very old opened databases.
    if (userVersion > 0 && userVersion < 4) {
        sqlite3_exec(db, "DROP TABLE IF EXISTS verse_index;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DROP TABLE IF EXISTS library_index;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DROP TABLE IF EXISTS library_entries;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DROP TABLE IF EXISTS dictionary_entries;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DROP TABLE IF EXISTS search_vocabulary;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DROP TABLE IF EXISTS search_vocabulary_deletes;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DROP TABLE IF EXISTS spell_module_terms;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DROP TABLE IF EXISTS spell_terms;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DROP TABLE IF EXISTS spell_deletes;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DROP TABLE IF EXISTS indexed_modules;", nullptr, nullptr, nullptr);
        sqlite3_exec(db, "DROP TABLE IF EXISTS module_index_errors;", nullptr, nullptr, nullptr);
    }

    sqlite3_exec(db, "DROP TABLE IF EXISTS spell_deletes;", nullptr, nullptr, nullptr);

    const char* tableSql = R"SQL(
        CREATE TABLE IF NOT EXISTS indexed_modules (
            module_name TEXT PRIMARY KEY,
            resource_type TEXT NOT NULL,
            module_signature TEXT NOT NULL,
            entry_count INTEGER NOT NULL DEFAULT 0,
            indexed_at TEXT DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS library_entries (
            entry_id INTEGER PRIMARY KEY AUTOINCREMENT,
            resource_type TEXT NOT NULL,
            module_token TEXT NOT NULL,
            scope_token TEXT NOT NULL DEFAULT '',
            book_token TEXT NOT NULL DEFAULT '',
            title TEXT NOT NULL,
            module_name TEXT NOT NULL,
            key_text TEXT NOT NULL
        );

        CREATE INDEX IF NOT EXISTS idx_library_entries_resource
            ON library_entries(resource_type);
        CREATE INDEX IF NOT EXISTS idx_library_entries_module_token
            ON library_entries(module_token);
        CREATE INDEX IF NOT EXISTS idx_library_entries_scope
            ON library_entries(scope_token, book_token);
        CREATE INDEX IF NOT EXISTS idx_library_entries_module_name
            ON library_entries(module_name);

        CREATE TABLE IF NOT EXISTS dictionary_entries (
            module_name TEXT NOT NULL,
            key_text TEXT NOT NULL,
            title TEXT NOT NULL,
            normalized_key TEXT NOT NULL,
            normalized_title TEXT NOT NULL,
            position INTEGER NOT NULL,
            PRIMARY KEY(module_name, key_text)
        );

        CREATE INDEX IF NOT EXISTS idx_dictionary_entries_module_position
            ON dictionary_entries(module_name, position);
        CREATE INDEX IF NOT EXISTS idx_dictionary_entries_module_key
            ON dictionary_entries(module_name, normalized_key);
        CREATE INDEX IF NOT EXISTS idx_dictionary_entries_module_title
            ON dictionary_entries(module_name, normalized_title);

        CREATE TABLE IF NOT EXISTS module_index_errors (
            module_name TEXT PRIMARY KEY,
            last_error TEXT NOT NULL,
            updated_at TEXT DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS spell_module_terms (
            module_name TEXT NOT NULL,
            term TEXT NOT NULL,
            phonetic_key TEXT NOT NULL DEFAULT '',
            doc_count INTEGER NOT NULL DEFAULT 0,
            total_count INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY(module_name, term)
        );

        CREATE TABLE IF NOT EXISTS spell_terms (
            term TEXT PRIMARY KEY,
            phonetic_key TEXT NOT NULL DEFAULT '',
            term_len INTEGER NOT NULL,
            doc_count INTEGER NOT NULL DEFAULT 0,
            total_count INTEGER NOT NULL DEFAULT 0
        );

        CREATE INDEX IF NOT EXISTS idx_spell_terms_phonetic
            ON spell_terms(phonetic_key, term_len, total_count);
        CREATE INDEX IF NOT EXISTS idx_spell_terms_len_term
            ON spell_terms(term_len, term);
    )SQL";

    char* err = nullptr;
    int rc = sqlite3_exec(db, tableSql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "SearchIndexer schema error: "
                  << (err ? err : "unknown") << "\n";
        if (err) sqlite3_free(err);
        return false;
    }
    if (err) sqlite3_free(err);

    const char* preferredFtsSql = R"SQL(
        CREATE VIRTUAL TABLE IF NOT EXISTS library_index USING fts5(
            title,
            content,
            strongs_text,
            content='',
            tokenize='unicode61 remove_diacritics 2'
        );
    )SQL";
    const char* fallbackFtsSql = R"SQL(
        CREATE VIRTUAL TABLE IF NOT EXISTS library_index USING fts5(
            title,
            content,
            strongs_text,
            content=''
        );
    )SQL";

    err = nullptr;
    rc = sqlite3_exec(db, preferredFtsSql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string preferredError = err ? err : "unknown";
        if (err) {
            sqlite3_free(err);
            err = nullptr;
        }

        std::cerr << "SearchIndexer preferred FTS tokenizer unavailable, "
                  << "retrying with default tokenizer: "
                  << preferredError << "\n";
        sqlite3_exec(db, "DROP TABLE IF EXISTS library_index;", nullptr, nullptr, nullptr);

        rc = sqlite3_exec(db, fallbackFtsSql, nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            std::cerr << "SearchIndexer schema error: "
                      << (err ? err : preferredError.c_str()) << "\n";
            if (err) sqlite3_free(err);
            return false;
        }
    }
    if (err) sqlite3_free(err);

    std::string schemaVersionPragma =
        "PRAGMA user_version = " + std::to_string(kSearchSchemaVersion) + ";";
    sqlite3_exec(db, schemaVersionPragma.c_str(), nullptr, nullptr, nullptr);
    return true;
}

std::string SearchIndexer::backendStatusMessage() const {
    std::lock_guard<std::mutex> lock(backendStatusMutex_);
    return backendStatusMessage_;
}

bool SearchIndexer::hasAnyIndexedData() const {
    if (!backendAvailable_.load() || !db_) return false;

    std::lock_guard<std::mutex> lock(dbMutex_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT 1 FROM indexed_modules LIMIT 1",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    const bool hasData = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return hasData;
}

void SearchIndexer::queueModuleIndex(const std::string& moduleName, bool force) {
    std::string normalized = trimCopy(moduleName);
    if (!db_ || normalized.empty()) return;
    if (!force &&
        (isModuleIndexed(normalized) || hasModuleIndexError(normalized))) {
        return;
    }

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

std::string SearchIndexer::semanticTaskKey(const std::string& language,
                                           const std::string& moduleName) {
    return language + "\x1f" + moduleName;
}

bool SearchIndexer::queueSemanticIndex(const std::string& language,
                                       const std::string& moduleName) {
    const std::string normalizedLanguage = lowerCopy(trimCopy(language));
    const std::string normalizedModule = trimCopy(moduleName);
    if (!semanticSearchService_ || normalizedLanguage.empty() ||
        normalizedModule.empty() ||
        semanticSearchService_->indexReady(normalizedLanguage, normalizedModule)) {
        return false;
    }

    const std::string key = semanticTaskKey(normalizedLanguage, normalizedModule);
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        semanticBuildErrors_.erase(key);
        incompleteSemanticBuilds_.erase(key);
    }
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        if (activeSemanticKey_ == key ||
            std::any_of(pendingSemanticIndexes_.begin(),
                        pendingSemanticIndexes_.end(),
                        [&](const SemanticIndexTask& pending) {
                            return semanticTaskKey(pending.language,
                                                   pending.moduleName) == key;
                        })) {
            return false;
        }
        pendingSemanticIndexes_.push_back(
            SemanticIndexTask{normalizedLanguage, normalizedModule});
    }
    workerCv_.notify_one();
    return true;
}

void SearchIndexer::cancelSemanticIndexBuild() {
    semanticUserCancelRequested_.store(true);
    semanticAbortRequested_.store(true);
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        pendingSemanticIndexes_.clear();
        if (!semanticTaskRunning_) semanticIdleCv_.notify_all();
    }
    workerCv_.notify_all();
}

void SearchIndexer::cancelSemanticIndexBuildAndWait() {
    cancelSemanticIndexBuild();
    std::unique_lock<std::mutex> lock(workerMutex_);
    semanticIdleCv_.wait(lock, [this]() {
        return !semanticTaskRunning_ && pendingSemanticIndexes_.empty();
    });
}

bool SearchIndexer::hasSemanticIndexWork() const {
    std::lock_guard<std::mutex> lock(workerMutex_);
    return semanticTaskRunning_ || !pendingSemanticIndexes_.empty();
}

bool SearchIndexer::activeSemanticIndexingTask(std::string& language,
                                               std::string& moduleName,
                                               int& percent) const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    if (activeSemanticLanguage_.empty() || activeSemanticModule_.empty()) {
        return false;
    }
    language = activeSemanticLanguage_;
    moduleName = activeSemanticModule_;
    percent = std::clamp(activeSemanticProgress_, 0, 100);
    return true;
}

SearchIndexer::SemanticIndexBuildStatus SearchIndexer::semanticIndexStatus(
    const std::string& language,
    const std::string& moduleName) const {
    SemanticIndexBuildStatus result;
    result.language = lowerCopy(trimCopy(language));
    result.moduleName = trimCopy(moduleName);
    if (result.language.empty() || result.moduleName.empty()) return result;
    const std::string key = semanticTaskKey(result.language, result.moduleName);

    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        if (activeSemanticLanguage_ == result.language &&
            activeSemanticModule_ == result.moduleName) {
            result.state = SemanticIndexBuildState::Building;
            result.percent = std::clamp(activeSemanticProgress_, 0, 100);
            return result;
        }
    }
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        if (std::any_of(pendingSemanticIndexes_.begin(),
                        pendingSemanticIndexes_.end(),
                        [&](const SemanticIndexTask& pending) {
                            return pending.language == result.language &&
                                   pending.moduleName == result.moduleName;
                        })) {
            result.state = SemanticIndexBuildState::Queued;
            return result;
        }
    }
    if (semanticSearchService_ &&
        semanticSearchService_->indexReady(result.language, result.moduleName)) {
        result.state = SemanticIndexBuildState::Ready;
        result.percent = 100;
        return result;
    }
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        auto error = semanticBuildErrors_.find(key);
        if (error != semanticBuildErrors_.end()) {
            result.state = SemanticIndexBuildState::Error;
            result.error = error->second;
            return result;
        }
        if (incompleteSemanticBuilds_.count(key) > 0) {
            result.state = SemanticIndexBuildState::Incomplete;
            return result;
        }
    }
    if (semanticSearchService_ &&
        semanticSearchService_->indexCheckpointAvailable(result.language,
                                                          result.moduleName)) {
        result.state = SemanticIndexBuildState::Incomplete;
    }
    return result;
}

void SearchIndexer::clearSemanticIndexBuildStatus() {
    std::lock_guard<std::mutex> lock(statusMutex_);
    semanticBuildErrors_.clear();
    incompleteSemanticBuilds_.clear();
}

void SearchIndexer::synchronizeModules(const std::vector<ModuleInfo>& modules) {
    std::unordered_map<std::string, ModuleCatalogEntry> nextCatalog;
    nextCatalog.reserve(modules.size());
    std::unordered_set<std::string> activeModules;

    std::string naveModule;
    std::string naveSignature;
    std::unordered_map<std::string, std::string> semanticModuleSignatures;
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
        semanticModuleSignatures[module.name] = entry.signature;
        if (lowerCopy(module.name) == "nave") {
            naveModule = module.name;
            naveSignature = entry.signature;
        }
        nextCatalog.emplace(module.name, std::move(entry));
    }

    {
        std::lock_guard<std::mutex> lock(catalogMutex_);
        moduleCatalog_ = std::move(nextCatalog);
    }

    if (!naveModule.empty() && topicSearchProvider_ &&
        !topicSearchProvider_->readyForSignature(naveSignature)) {
        queueModuleIndex(naveModule, true);
    }
    if (semanticSearchService_) {
        semanticSearchService_->setModuleSignatures(semanticModuleSignatures);
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

    sqlite3_stmt* deleteEntries = nullptr;
    sqlite3_stmt* deleteDictionaryEntries = nullptr;
    sqlite3_stmt* deleteStatus = nullptr;
    sqlite3_stmt* deleteErrors = nullptr;
    sqlite3_prepare_v2(db_,
                       "DELETE FROM library_entries WHERE module_name = ?",
                       -1, &deleteEntries, nullptr);
    sqlite3_prepare_v2(db_,
                       "DELETE FROM dictionary_entries WHERE module_name = ?",
                       -1, &deleteDictionaryEntries, nullptr);
    sqlite3_prepare_v2(db_,
                       "DELETE FROM indexed_modules WHERE module_name = ?",
                       -1, &deleteStatus, nullptr);
    sqlite3_prepare_v2(db_,
                       "DELETE FROM module_index_errors WHERE module_name = ?",
                       -1, &deleteErrors, nullptr);

    if (!deleteEntries || !deleteDictionaryEntries || !deleteStatus || !deleteErrors) {
        if (deleteEntries) sqlite3_finalize(deleteEntries);
        if (deleteDictionaryEntries) sqlite3_finalize(deleteDictionaryEntries);
        if (deleteStatus) sqlite3_finalize(deleteStatus);
        if (deleteErrors) sqlite3_finalize(deleteErrors);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return;
    }

    for (const auto& moduleName : staleModules) {
        bindText(deleteEntries, 1, moduleName);
        sqlite3_step(deleteEntries);
        sqlite3_reset(deleteEntries);
        sqlite3_clear_bindings(deleteEntries);

        bindText(deleteDictionaryEntries, 1, moduleName);
        sqlite3_step(deleteDictionaryEntries);
        sqlite3_reset(deleteDictionaryEntries);
        sqlite3_clear_bindings(deleteDictionaryEntries);

        removeModuleSpellTerms(db_, moduleName);

        bindText(deleteStatus, 1, moduleName);
        sqlite3_step(deleteStatus);
        sqlite3_reset(deleteStatus);
        sqlite3_clear_bindings(deleteStatus);

        bindText(deleteErrors, 1, moduleName);
        sqlite3_step(deleteErrors);
        sqlite3_reset(deleteErrors);
        sqlite3_clear_bindings(deleteErrors);
    }

    sqlite3_finalize(deleteEntries);
    sqlite3_finalize(deleteDictionaryEntries);
    sqlite3_finalize(deleteStatus);
    sqlite3_finalize(deleteErrors);
    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
}

std::vector<std::string> SearchIndexer::requestModules(
    const SearchRequest& request) const {
    std::vector<std::pair<std::string, std::string>> modules;
    {
        std::lock_guard<std::mutex> lock(catalogMutex_);
        modules.reserve(moduleCatalog_.size());
        for (const auto& [name, entry] : moduleCatalog_) {
            if (!request.moduleName.empty() && name != request.moduleName) continue;
            if (!request.resourceTypes.empty() &&
                std::find(request.resourceTypes.begin(),
                          request.resourceTypes.end(),
                          entry.resourceType) == request.resourceTypes.end()) {
                continue;
            }
            modules.emplace_back(name, entry.info.name);
        }
    }

    std::sort(modules.begin(), modules.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.second < rhs.second;
              });

    std::vector<std::string> names;
    names.reserve(modules.size());
    for (const auto& module : modules) {
        names.push_back(module.first);
    }
    return names;
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

bool SearchIndexer::canUseIndexedSearch(const SearchRequest& request) const {
    if (!backendAvailable_.load() || !db_) return false;

    const std::vector<std::string> modules = requestModules(request);
    if (modules.empty()) return false;

    auto isIndexed = [this](const std::string& moduleName) {
        return isModuleIndexed(moduleName);
    };

    if (!request.moduleName.empty()) {
        return std::all_of(modules.begin(), modules.end(), isIndexed);
    }

    return std::any_of(modules.begin(), modules.end(), isIndexed);
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

bool SearchIndexer::hasModuleIndexError(const std::string& moduleName) const {
    return !moduleIndexError(moduleName).empty();
}

std::vector<std::string> SearchIndexer::dictionaryKeys(
    const std::string& moduleName) const {

    std::vector<std::string> keys;
    if (!db_ || moduleName.empty()) return keys;

    std::lock_guard<std::mutex> lock(dbMutex_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT key_text FROM dictionary_entries "
            "WHERE module_name = ? "
            "ORDER BY position",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return keys;
    }

    bindText(stmt, 1, moduleName);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* key =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (key && *key) keys.emplace_back(key);
    }
    sqlite3_finalize(stmt);
    return keys;
}

SearchIndexer::ScopedSuspend SearchIndexer::suspendBackgroundIndexing() {
    if (!db_) return ScopedSuspend();

    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        ++suspendDepth_;
        semanticPauseRequested_.store(true);
        semanticAbortRequested_.store(true);
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
            if (shouldNotify) semanticPauseRequested_.store(false);
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

std::string SearchIndexer::buildWordFtsQuery(const SearchRequest& request,
                                             const std::string& query,
                                             bool exactPhrase) {
    (void)request;

    std::string text = trimCopy(query);
    if (text.empty()) return "";

    std::string contentClause;
    if (exactPhrase) {
        contentClause = "{title content}:" + quoteFtsToken(text);
    } else {
        std::vector<std::string> tokens = smart_search::queryTerms(text);
        if (tokens.empty()) return "";

        std::ostringstream content;
        content << "{title content}:(";
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (i) content << " AND ";
            const bool prefix = tokens[i].back() == '*';
            content << quoteFtsToken(prefix ? tokens[i].substr(0, tokens[i].size() - 1)
                                           : tokens[i]);
            if (prefix) content << "*";
        }
        content << ")";
        contentClause = content.str();
    }

    return contentClause;
}

std::string SearchIndexer::buildStrongsFtsQuery(const SearchRequest& request,
                                                const std::string& query) {
    (void)request;

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
    std::unordered_set<std::string> seen;
    char prefix = 0;
    if (langHint == StrongsLanguageHint::Greek) prefix = 'G';
    if (langHint == StrongsLanguageHint::Hebrew) prefix = 'H';
    for (const auto& t : terms) {
        if (t.empty()) continue;
        if (!std::isalpha(static_cast<unsigned char>(t[0]))) continue;
        char termPrefix = static_cast<char>(std::toupper(
            static_cast<unsigned char>(t[0])));
        if (termPrefix != 'G' && termPrefix != 'H') continue;
        if (prefix && termPrefix != prefix) continue;
        if (seen.insert(t).second) {
            prefixedTerms.push_back(t);
        }
    }
    if (prefixedTerms.empty()) return "";

    auto appendOrTerms = [](std::ostringstream& out,
                            const std::vector<std::string>& values) {
        for (size_t i = 0; i < values.size(); ++i) {
            if (i) out << " OR ";
            out << quoteFtsToken(values[i]);
        }
    };

    std::ostringstream content;
    content << "{strongs_text}:(";
    appendOrTerms(content, prefixedTerms);
    content << ")";
    return content.str();
}

std::vector<SearchResult> SearchIndexer::searchDirectInternal(
    const SearchRequest& request,
    const std::function<bool(const SearchResult&,
                             const std::string&,
                             std::string&)>& matcher,
    int maxResults,
    RegexProgressCallback progressCallback) const {

    std::vector<SearchResult> results;
    if (!matcher) return results;

    const int limit = (maxResults > 0) ? maxResults : request.maxResults;
    const std::vector<std::string> moduleNames = requestModules(request);
    if (moduleNames.empty()) return results;

    std::vector<ModuleScanSource> sources;
    sources.reserve(moduleNames.size());
    int total = 0;

    for (const auto& moduleName : moduleNames) {
        CatalogSnapshot catalog;
        {
            std::lock_guard<std::mutex> lock(catalogMutex_);
            auto it = moduleCatalog_.find(moduleName);
            if (it != moduleCatalog_.end()) {
                catalog.info = it->second.info;
                catalog.resourceType = it->second.resourceType;
                catalog.moduleToken = it->second.moduleToken;
                catalog.moduleSignature = it->second.signature;
            }
        }

        ModuleScanSource source;
        std::string error;
        if (!prepareModuleScanSource(moduleName, catalog, importedModuleMgr_, error, source)) {
            continue;
        }

        total += static_cast<int>(source.imported
                                      ? source.importedEntries.size()
                                      : source.keyedEntries.size());
        sources.push_back(std::move(source));
    }

    auto reportProgress = [&](int scanned, int matches) -> bool {
        if (!progressCallback) return true;
        RegexSearchProgress progress;
        progress.scanned = scanned;
        progress.total = total;
        progress.matches = matches;
        return progressCallback(progress);
    };

    if (!reportProgress(0, 0)) {
        return results;
    }

    int scanned = 0;
    for (auto& source : sources) {
        if (source.imported) {
            for (const auto& entry : source.importedEntries) {
                ++scanned;

                SearchResult result;
                result.resourceType = source.resourceType;
                result.module = source.moduleName;
                result.key = entry.key;
                result.title = entry.title.empty() ? entry.key : entry.title;

                std::string snippet;
                if (matcher(result, entry.plainText, snippet)) {
                    result.text = snippet.empty()
                                      ? truncateSnippet(entry.plainText.empty()
                                                            ? result.title
                                                            : entry.plainText)
                                      : snippet;
                    results.push_back(std::move(result));
                    if (limit > 0 && static_cast<int>(results.size()) >= limit) {
                        reportProgress(scanned,
                                       static_cast<int>(results.size()));
                        return results;
                    }
                }

                if ((scanned % 128) == 0 &&
                    !reportProgress(scanned, static_cast<int>(results.size()))) {
                    return results;
                }
            }
            continue;
        }

        for (const auto& key : source.keyedEntries) {
            ++scanned;

            source.mod->setKey(key.c_str());
            if (source.mod->popError()) {
                if ((scanned % 128) == 0 &&
                    !reportProgress(scanned, static_cast<int>(results.size()))) {
                    return results;
                }
                continue;
            }

            if (source.resourceType == "bible" &&
                !matchesBibleScope(request, source.mod)) {
                if ((scanned % 128) == 0 &&
                    !reportProgress(scanned, static_cast<int>(results.size()))) {
                    return results;
                }
                continue;
            }

            const char* plainRaw = source.mod->stripText();
            std::string plain = trimCopy(plainRaw ? plainRaw : "");

            std::string keyText = trimCopy(source.mod->getKeyText()
                                               ? source.mod->getKeyText()
                                               : "");
            if (keyText.empty()) keyText = key;

            std::string title = keyText;
            auto titleIt = source.entryTitles.find(key);
            if (titleIt != source.entryTitles.end() &&
                !trimCopy(titleIt->second).empty()) {
                title = titleIt->second;
            }

            SearchResult result;
            result.resourceType = source.resourceType;
            result.module = source.moduleName;
            result.key = keyText;
            result.title = title.empty() ? keyText : title;

            std::string snippet;
            if (matcher(result, plain, snippet)) {
                result.text = snippet.empty()
                                  ? truncateSnippet(plain.empty()
                                                        ? result.title
                                                        : plain)
                                  : snippet;
                results.push_back(std::move(result));
                if (limit > 0 && static_cast<int>(results.size()) >= limit) {
                    reportProgress(scanned,
                                   static_cast<int>(results.size()));
                    return results;
                }
            }

            if ((scanned % 128) == 0 &&
                !reportProgress(scanned, static_cast<int>(results.size()))) {
                return results;
            }
        }
    }

    reportProgress(scanned, static_cast<int>(results.size()));
    return results;
}

std::vector<SearchIndexer::SourceText> SearchIndexer::fetchSourceTextsForResults(
    const std::vector<SearchResult>& results,
    bool includeXhtml) const {

    std::vector<SourceText> texts(results.size());
    if (results.empty()) return texts;

    std::unordered_map<std::string, std::vector<size_t>> byModule;
    for (size_t i = 0; i < results.size(); ++i) {
        if (!results[i].module.empty() && !results[i].key.empty()) {
            byModule[results[i].module].push_back(i);
        }
    }

    auto setBibleOptions = [](ModuleScanSource& source, bool richText) {
        if (!source.mgr || source.resourceType != "bible") return;
        const char* value = richText ? "On" : "Off";
        source.mgr->setGlobalOption("Strong's Numbers", value);
        source.mgr->setGlobalOption("Morphological Tags", value);
        source.mgr->setGlobalOption("Footnotes", value);
        source.mgr->setGlobalOption("Cross-references", value);
        source.mgr->setGlobalOption("Headings", value);
    };

    auto fetchOne = [&](ModuleScanSource& source,
                        const SearchResult& result) -> SourceText {
        SourceText text;
        if (source.imported) {
            for (const auto& entry : source.importedEntries) {
                if (entry.key != result.key) continue;
                text.found = true;
                text.title = entry.title.empty() ? entry.key : entry.title;
                text.plainText = entry.plainText;
                return text;
            }
            return text;
        }

        if (!source.mod) return text;

        source.mod->setKey(result.key.c_str());
        if (source.mod->popError()) return text;

        std::string keyText = trimCopy(source.mod->getKeyText()
                                           ? source.mod->getKeyText()
                                           : "");
        if (keyText.empty()) keyText = result.key;
        text.title = result.title.empty() ? keyText : result.title;

        setBibleOptions(source, false);
        source.mod->setKey(result.key.c_str());
        if (!source.mod->popError()) {
            const char* plainRaw = source.mod->stripText();
            text.plainText = trimCopy(plainRaw ? plainRaw : "");
            text.found = true;
        }

        if (includeXhtml) {
            setBibleOptions(source, true);
            source.mod->setKey(result.key.c_str());
            if (!source.mod->popError()) {
                text.xhtml = std::string(source.mod->renderText().c_str());
                text.found = true;
            }
        }

        return text;
    };

    for (const auto& [moduleName, indices] : byModule) {
        CatalogSnapshot catalog;
        {
            std::lock_guard<std::mutex> lock(catalogMutex_);
            auto it = moduleCatalog_.find(moduleName);
            if (it != moduleCatalog_.end()) {
                catalog.info = it->second.info;
                catalog.resourceType = it->second.resourceType;
                catalog.moduleToken = it->second.moduleToken;
                catalog.moduleSignature = it->second.signature;
            }
        }

        ModuleScanSource source;
        std::string error;
        if (!prepareModuleLookupSource(moduleName, catalog, importedModuleMgr_,
                                       error, source)) {
            continue;
        }

        for (size_t index : indices) {
            texts[index] = fetchOne(source, results[index]);
        }
    }

    return texts;
}

std::vector<SearchResult> SearchIndexer::buildResultSnippets(
    const std::vector<SearchResult>& inputResults,
    SnippetKind kind,
    const std::string& query,
    const std::string& language,
    bool caseSensitive,
    SmartSearchOptions smartOptions) const {

    std::vector<SearchResult> results = inputResults;
    if (results.empty()) return results;

    std::regex regexPattern;
    bool regexValid = false;
    if (kind == SnippetKind::Regex) {
        try {
            std::regex::flag_type flags = std::regex::ECMAScript;
            if (!caseSensitive) flags |= std::regex::icase;
            regexPattern = std::regex(query, flags);
            regexValid = true;
        } catch (const std::regex_error&) {
            regexValid = false;
        }
    }

    std::vector<std::string> smartQueryTerms;
    std::unordered_map<std::string, std::vector<std::string>> spellingAlternatives;
    if (kind == SnippetKind::Smart) {
        smartQueryTerms = smart_search::queryTerms(query, language,
                                                   smartOptions.includeSynonyms);
        spellingAlternatives = buildSmartSpellingAlternatives(
            SearchRequest{}, query, language, smartOptions);
    }

    const bool includeXhtml = kind == SnippetKind::Strongs;
    std::vector<SourceText> sourceTexts =
        fetchSourceTextsForResults(results, includeXhtml);

    for (size_t i = 0; i < results.size(); ++i) {
        const SourceText& sourceText = sourceTexts[i];
        if (sourceText.found && !sourceText.title.empty()) {
            results[i].title = sourceText.title;
        }

        switch (kind) {
        case SnippetKind::Word:
        case SnippetKind::ExactPhrase:
            results[i].text = buildWordSnippetFromSourceText(
                sourceText.plainText, results[i].title, query,
                kind == SnippetKind::ExactPhrase);
            break;

        case SnippetKind::Strongs:
            results[i].text = buildStrongsSnippet(sourceText.xhtml,
                                                  query,
                                                  sourceText.plainText);
            break;

        case SnippetKind::Regex: {
            if (regexValid) {
                std::string searchableText = results[i].title.empty()
                    ? sourceText.plainText
                    : (results[i].title + " " + sourceText.plainText);
                std::smatch sourceMatch;
                if (!searchableText.empty() &&
                    std::regex_search(searchableText, sourceMatch, regexPattern)) {
                    results[i].text = buildRegexSnippet(searchableText,
                                                        sourceMatch);
                }
            }
            if (results[i].text.empty()) {
                results[i].text = truncateSnippet(sourceText.plainText.empty()
                                                      ? results[i].title
                                                      : sourceText.plainText);
            }
            break;
        }

        case SnippetKind::Smart:
            results[i].text = buildSmartSnippetFromSourceText(
                sourceText.plainText,
                results[i].title,
                smartQueryTerms,
                language,
                spellingAlternatives,
                smartOptions);
            break;
        }

        if (results[i].text.empty()) {
            results[i].text = results[i].title.empty() ? results[i].key : results[i].title;
        }
    }

    return results;
}

std::vector<SearchResult> SearchIndexer::searchWord(
    const SearchRequest& request,
    const std::string& query,
    bool exactPhrase,
    int maxResults,
    bool includeSnippets) const {

    std::vector<SearchResult> results;
    if (!db_ || query.empty()) return results;

    std::string ftsQuery = buildWordFtsQuery(request, query, exactPhrase);
    if (ftsQuery.empty()) return results;

    const int limit = (maxResults > 0) ? maxResults : request.maxResults;
    MetadataFilter filter = buildMetadataFilter(request, "e");
    std::string sql =
        "SELECT e.resource_type, e.module_name, e.key_text, e.title "
        "FROM library_index "
        "JOIN library_entries e ON e.entry_id = library_index.rowid "
        "WHERE library_index MATCH ? ";
    if (!filter.sql.empty()) {
        sql += "AND ";
        sql += filter.sql;
        sql += " ";
    }
    sql +=
        "ORDER BY bm25(library_index)";
    if (limit > 0) {
        sql += " LIMIT ?";
    }

    {
        std::lock_guard<std::mutex> lock(dbMutex_);

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return results;
        }

        bindText(stmt, 1, ftsQuery);
        bindMetadataFilterValues(stmt, 2, filter);
        if (limit > 0) {
            sqlite3_bind_int(stmt, static_cast<int>(filter.values.size()) + 2, limit);
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            SearchResult result;
            const char* type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* module = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const char* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            const char* title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            result.resourceType = type ? type : "";
            result.module = module ? module : request.moduleName;
            result.key = key ? key : "";
            result.title = title ? title : result.key;
            results.push_back(std::move(result));
        }

        sqlite3_finalize(stmt);
    }

    if (!includeSnippets) return results;

    return buildResultSnippets(results,
                               exactPhrase ? SnippetKind::ExactPhrase
                                           : SnippetKind::Word,
                               query);
}

std::vector<SearchResult> SearchIndexer::searchWordDirect(
    const SearchRequest& request,
    const std::string& query,
    bool exactPhrase,
    int maxResults) const {

    if (query.empty()) return {};

    if (exactPhrase) {
        const std::string normalizedPhrase =
            normalizeDirectSearchText(trimCopy(query));
        if (normalizedPhrase.empty()) return {};

        return searchDirectInternal(
            request,
            [normalizedPhrase, query](const SearchResult& result,
                                      const std::string& plainText,
                                      std::string& snippetOut) {
                const std::string searchable = normalizeDirectSearchText(
                    result.title.empty() ? plainText : (result.title + " " + plainText));
                if (!containsWholeDirectSearchMatch(searchable, normalizedPhrase)) {
                    return false;
                }
                snippetOut = buildWordSnippetFromSourceText(
                    plainText, result.title, query, true, false);
                return true;
            },
            maxResults);
    }

    std::vector<std::string> tokens = smart_search::queryTerms(query);
    if (tokens.empty()) return {};

    std::vector<std::string> normalizedTokens;
    normalizedTokens.reserve(tokens.size());
    for (const auto& token : tokens) {
        std::string normalized = normalizeDirectSearchText(token);
        if (!normalized.empty()) normalizedTokens.push_back(std::move(normalized));
    }
    if (normalizedTokens.empty()) return {};

    return searchDirectInternal(
        request,
        [normalizedTokens = std::move(normalizedTokens), query](
            const SearchResult& result,
            const std::string& plainText,
            std::string& snippetOut) {
            const std::string searchable = normalizeDirectSearchText(
                result.title.empty() ? plainText : (result.title + " " + plainText));
            const bool matched = std::all_of(
                normalizedTokens.begin(), normalizedTokens.end(),
                [&searchable](const std::string& token) {
                    std::string_view needle(token);
                    const bool prefix = needle.back() == '*';
                    if (prefix) needle.remove_suffix(1);
                    return containsWholeDirectSearchMatch(searchable, needle, prefix);
                });
            if (!matched) return false;
            snippetOut = buildWordSnippetFromSourceText(
                plainText, result.title, query, false, false);
            return true;
        },
        maxResults);
}

std::vector<SearchResult> SearchIndexer::searchStrongs(
    const SearchRequest& request,
    const std::string& strongsQuery,
    int maxResults,
    bool includeSnippets) const {

    std::vector<SearchResult> results;
    if (!db_ || strongsQuery.empty()) return results;

    std::string ftsQuery = buildStrongsFtsQuery(request, strongsQuery);
    if (ftsQuery.empty()) return results;

    const int limit = (maxResults > 0) ? maxResults : request.maxResults;
    MetadataFilter filter = buildMetadataFilter(request, "e");
    std::string sql =
        "SELECT e.resource_type, e.module_name, e.key_text, e.title "
        "FROM library_index "
        "JOIN library_entries e ON e.entry_id = library_index.rowid "
        "WHERE library_index MATCH ? ";
    if (!filter.sql.empty()) {
        sql += "AND ";
        sql += filter.sql;
        sql += " ";
    }
    sql +=
        "ORDER BY bm25(library_index)";
    if (limit > 0) {
        sql += " LIMIT ?";
    }

    {
        std::lock_guard<std::mutex> lock(dbMutex_);

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return results;
        }

        bindText(stmt, 1, ftsQuery);
        bindMetadataFilterValues(stmt, 2, filter);
        if (limit > 0) {
            sqlite3_bind_int(stmt, static_cast<int>(filter.values.size()) + 2, limit);
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            SearchResult result;
            const char* type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* module = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const char* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            const char* title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            result.resourceType = type ? type : "";
            result.module = module ? module : request.moduleName;
            result.key = key ? key : "";
            result.title = title ? title : result.key;
            results.push_back(std::move(result));
        }

        sqlite3_finalize(stmt);
    }

    if (!includeSnippets) return results;

    return buildResultSnippets(results, SnippetKind::Strongs, strongsQuery);
}

std::vector<SearchResult> SearchIndexer::searchRegex(
    const SearchRequest& request,
    const std::string& pattern,
    bool caseSensitive,
    int maxResults,
    RegexProgressCallback progressCallback,
    bool includeSnippets) const {

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

    MetadataFilter filter = buildMetadataFilter(request, "e");
    std::string prefilterQuery = regexFtsPrefilter(pattern);

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
    std::string countSql;
    if (prefilterQuery.empty()) {
        countSql = "SELECT count(*) FROM library_entries e ";
        if (!filter.sql.empty()) {
            countSql += "WHERE ";
            countSql += filter.sql;
        }
    } else {
        countSql =
            "SELECT count(*) "
            "FROM library_index "
            "JOIN library_entries e ON e.entry_id = library_index.rowid "
            "WHERE library_index MATCH ? ";
        if (!filter.sql.empty()) {
            countSql += "AND ";
            countSql += filter.sql;
        }
    }
    if (sqlite3_prepare_v2(readDb, countSql.c_str(), -1,
                           &countStmt, nullptr) == SQLITE_OK) {
        int bindIndex = 1;
        if (!prefilterQuery.empty()) {
            bindText(countStmt, bindIndex++, prefilterQuery);
        }
        bindMetadataFilterValues(countStmt, bindIndex, filter);
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
    std::string sql;
    if (prefilterQuery.empty()) {
        sql =
            "SELECT e.resource_type, e.module_name, e.key_text, e.title "
            "FROM library_entries e ";
        if (!filter.sql.empty()) {
            sql += "WHERE ";
            sql += filter.sql;
            sql += " ";
        }
        sql += "ORDER BY e.entry_id";
    } else {
        sql =
            "SELECT e.resource_type, e.module_name, e.key_text, e.title "
            "FROM library_index "
            "JOIN library_entries e ON e.entry_id = library_index.rowid "
            "WHERE library_index MATCH ? ";
        if (!filter.sql.empty()) {
            sql += "AND ";
            sql += filter.sql;
            sql += " ";
        }
        sql += "ORDER BY library_index.rowid";
    }

    if (sqlite3_prepare_v2(readDb, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(readDb);
        return results;
    }

    {
        int bindIndex = 1;
        if (!prefilterQuery.empty()) {
            bindText(stmt, bindIndex++, prefilterQuery);
        }
        bindMetadataFilterValues(stmt, bindIndex, filter);
    }

    int scanned = 0;
    int matches = 0;
    bool cancelled = false;
    const int limit = (maxResults > 0) ? maxResults : request.maxResults;
    std::vector<SearchResult> batch;
    batch.reserve(128);

    auto flushBatch = [&]() -> bool {
        if (batch.empty()) return true;

        std::vector<SourceText> sourceTexts = fetchSourceTextsForResults(batch, false);
        for (size_t i = 0; i < batch.size(); ++i) {
            ++scanned;
            const SourceText& sourceText = sourceTexts[i];
            std::string titleText = sourceText.title.empty()
                ? batch[i].title
                : sourceText.title;
            std::string searchableText = titleText.empty()
                ? sourceText.plainText
                : (titleText + " " + sourceText.plainText);

            std::smatch match;
            if (searchableText.empty() || !std::regex_search(searchableText, match, re)) {
                if ((scanned % 128) == 0 && !reportProgress(scanned, matches)) {
                    cancelled = true;
                    batch.clear();
                    return false;
                }
                continue;
            }

            SearchResult result = batch[i];
            result.title = titleText.empty() ? result.key : titleText;
            if (includeSnippets) {
                result.text = buildRegexSnippet(searchableText, match);
            }
            results.push_back(std::move(result));
            matches = static_cast<int>(results.size());

            if (!reportProgress(scanned, matches)) {
                cancelled = true;
                batch.clear();
                return false;
            }

            if (limit > 0 && static_cast<int>(results.size()) >= limit) {
                batch.clear();
                return false;
            }
        }

        batch.clear();
        return true;
    };

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* module = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));

        SearchResult result;
        result.resourceType = type ? type : "";
        result.module = module ? module : request.moduleName;
        result.key = key ? key : "";
        result.title = title ? title : result.key;
        batch.push_back(std::move(result));
        if (batch.size() >= 128 && !flushBatch()) {
            break;
        }
    }

    if (!cancelled && (limit <= 0 || static_cast<int>(results.size()) < limit)) {
        flushBatch();
    }
    sqlite3_finalize(stmt);
    if (!cancelled) {
        reportProgress(scanned, matches);
    }
    sqlite3_close(readDb);

    return results;
}

std::vector<SearchResult> SearchIndexer::searchRegexDirect(
    const SearchRequest& request,
    const std::string& pattern,
    bool caseSensitive,
    int maxResults,
    RegexProgressCallback progressCallback) const {

    if (pattern.empty()) return {};

    std::regex re;
    try {
        std::regex::flag_type flags = std::regex::ECMAScript;
        if (!caseSensitive) flags |= std::regex::icase;
        re = std::regex(pattern, flags);
    } catch (const std::regex_error&) {
        return {};
    }

    std::string literalPrefilter = extractRegexLiteralPrefilter(pattern);
    if (!caseSensitive && !isAsciiText(literalPrefilter)) {
        literalPrefilter.clear();
    }

    return searchDirectInternal(
        request,
        [re, literalPrefilter = std::move(literalPrefilter), caseSensitive](
            const SearchResult& result,
            const std::string& plainText,
            std::string& snippetOut) {
            const std::string searchableText = result.title.empty()
                                                   ? plainText
                                                   : (result.title + " " + plainText);

            if (!literalPrefilter.empty()) {
                const bool literalMatch = caseSensitive
                                              ? (searchableText.find(literalPrefilter) !=
                                                 std::string::npos)
                                              : containsCaseInsensitiveAscii(
                                                    searchableText, literalPrefilter);
                if (!literalMatch) return false;
            }

            std::smatch match;
            if (searchableText.empty() ||
                !std::regex_search(searchableText, match, re)) {
                return false;
            }

            snippetOut = buildRegexSnippet(searchableText, match);
            return true;
        },
        maxResults,
        std::move(progressCallback));
}

std::unordered_map<std::string, std::vector<std::string>>
SearchIndexer::buildSmartSpellingAlternatives(
    const SearchRequest& request,
    const std::string& query,
    const std::string& language,
    SmartSearchOptions options) const {

    std::unordered_map<std::string, std::vector<std::string>> alternatives;
    std::vector<std::string> tokens = smart_search::queryTerms(query);
    if (!db_ || tokens.empty() || !options.spellingCorrection) return alternatives;

    struct Candidate {
        std::string term;
        std::string strippedTerm;
        std::string phoneticKey;
        int totalCount = 0;
        bool exactHit = false;
        bool prefixHit = false;
        bool phoneticHit = false;
        bool spellingProbeHit = false;
        bool orthographicHit = false;
        double score = 0.0;
    };

    auto addCandidate = [](std::unordered_map<std::string, Candidate>& candidates,
                           const std::string& term,
                           const std::string& strippedTerm,
                           const std::string& phoneticKey,
                           int totalCount,
                           bool exactHit,
                           bool prefixHit,
                           bool phoneticHit,
                           bool spellingProbeHit,
                           bool orthographicHit) {
        if (term.empty()) return;
        auto [it, inserted] = candidates.emplace(term, Candidate{});
        Candidate& candidate = it->second;
        if (inserted) {
            candidate.term = term;
            candidate.strippedTerm = strippedTerm.empty() ? term : strippedTerm;
            candidate.phoneticKey = phoneticKey;
        }
        candidate.totalCount = std::max(candidate.totalCount, totalCount);
        candidate.exactHit = candidate.exactHit || exactHit;
        candidate.prefixHit = candidate.prefixHit || prefixHit;
        candidate.phoneticHit = candidate.phoneticHit || phoneticHit;
        candidate.spellingProbeHit = candidate.spellingProbeHit || spellingProbeHit;
        candidate.orthographicHit = candidate.orthographicHit || orthographicHit;
    };

    std::vector<std::string> vocabularyModules;
    if (!request.moduleName.empty()) {
        vocabularyModules.push_back(request.moduleName);
    } else if (!language.empty()) {
        const std::string wantedLanguage = lowerCopy(language);
        std::lock_guard<std::mutex> catalogLock(catalogMutex_);
        for (const auto& [name, entry] : moduleCatalog_) {
            if (lowerCopy(entry.info.language) != wantedLanguage) continue;
            if (!request.resourceTypes.empty() &&
                std::find(request.resourceTypes.begin(), request.resourceTypes.end(),
                          entry.resourceType) == request.resourceTypes.end()) {
                continue;
            }
            vocabularyModules.push_back(name);
        }
    }

    std::lock_guard<std::mutex> lock(dbMutex_);
    sqlite3_stmt* scopedTermStmt = nullptr;
    if (!vocabularyModules.empty()) {
        std::ostringstream scopedSql;
        scopedSql << "SELECT 1 FROM spell_module_terms "
                     "WHERE term = ? AND module_name IN (";
        for (size_t i = 0; i < vocabularyModules.size(); ++i) {
            if (i) scopedSql << ",";
            scopedSql << "?";
        }
        scopedSql << ") LIMIT 1";
        if (sqlite3_prepare_v2(db_, scopedSql.str().c_str(), -1,
                               &scopedTermStmt, nullptr) != SQLITE_OK) {
            scopedTermStmt = nullptr;
        }
    }

    auto termAllowedByScope = [&](const std::string& term) {
        if (!scopedTermStmt) return true;
        sqlite3_reset(scopedTermStmt);
        sqlite3_clear_bindings(scopedTermStmt);
        bindText(scopedTermStmt, 1, term);
        for (size_t i = 0; i < vocabularyModules.size(); ++i) {
            bindText(scopedTermStmt, static_cast<int>(i) + 2,
                     vocabularyModules[i]);
        }
        return sqlite3_step(scopedTermStmt) == SQLITE_ROW;
    };

    auto readCandidates = [&](sqlite3_stmt* stmt,
                              std::unordered_map<std::string, Candidate>& candidates,
                              bool exactHit,
                              bool prefixHit,
                              bool phoneticHit,
                              bool spellingProbeHit,
                              bool orthographicHit) {
        int rows = 0;
        while (candidates.size() < kMaxSpellingCandidatesPerToken &&
               sqlite3_step(stmt) == SQLITE_ROW) {
            ++rows;
            const char* term = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const std::string candidateTerm = term ? term : "";
            if (!termAllowedByScope(candidateTerm)) continue;
            const char* stripped = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const char* phonetic = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            int totalCount = sqlite3_column_int(stmt, 3);
            addCandidate(candidates,
                         candidateTerm,
                         stripped ? stripped : "",
                         phonetic ? phonetic : "",
                         totalCount,
                         exactHit,
                         prefixHit,
                         phoneticHit,
                         spellingProbeHit,
                         orthographicHit);
        }
        return rows;
    };

    for (const auto& rawToken : tokens) {
        // A wildcard is an explicit request, never a misspelling.
        if (rawToken.back() == '*') continue;
        std::string lowerToken = lowerCopy(normalizeWordToken(rawToken));
        std::string normalized = normalizeVocabularyTerm(rawToken);
        if (lowerToken.empty() || normalized.empty()) continue;

        alternatives.try_emplace(lowerToken);
        alternatives.try_emplace(normalized);

        std::unordered_map<std::string, Candidate> candidates;
        const int lengthWindow = spellingCandidateLengthWindow(normalized.size());
        const int minLen = std::max<int>(3, static_cast<int>(normalized.size()) - lengthWindow);
        const int maxLen = static_cast<int>(normalized.size()) + lengthWindow;

        {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(
                    db_,
                    "SELECT term, term, phonetic_key, total_count "
                    "FROM spell_terms WHERE term = ? LIMIT 1",
                    -1, &stmt, nullptr) == SQLITE_OK) {
                bindText(stmt, 1, normalized);
                readCandidates(stmt, candidates, true, false, false, false, false);
            }
            if (stmt) sqlite3_finalize(stmt);
        }

        const auto exactIt = candidates.find(normalized);
        const bool exactTermExists = exactIt != candidates.end();
        // A rare valid word is still valid. Never "correct" railer to ruler.
        if (exactTermExists) continue;
        const bool allowPartialCandidateProbe =
            options.partialWordMatching || options.fuzzyExpansion;

        for (const auto& probe : spellingProbeTerms(
                 normalized, true)) {
            if (candidates.size() >= kMaxSpellingCandidatesPerToken) break;
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(
                    db_,
                    "SELECT term, term, phonetic_key, total_count "
                    "FROM spell_terms WHERE term = ? LIMIT 1",
                    -1, &stmt, nullptr) == SQLITE_OK) {
                bindText(stmt, 1, probe.term);
                readCandidates(stmt, candidates, false, false, false, true,
                               probe.orthographic);
            }
            if (stmt) sqlite3_finalize(stmt);
        }

        std::string phoneticKey = smart_search::metaphoneKey(normalized);
        if (!phoneticKey.empty() && normalized.size() >= 4 &&
            candidates.size() < kMaxSpellingCandidatesPerToken) {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(
                    db_,
                    "SELECT term, term, phonetic_key, total_count "
                    "FROM spell_terms "
                    "WHERE phonetic_key = ? AND term_len BETWEEN ? AND ? "
                    "ORDER BY total_count DESC "
                    "LIMIT 160",
                    -1, &stmt, nullptr) == SQLITE_OK) {
                bindText(stmt, 1, phoneticKey);
                sqlite3_bind_int(stmt, 2, minLen);
                sqlite3_bind_int(stmt, 3, maxLen);
                readCandidates(stmt, candidates, false, false, true, false, false);
            }
            if (stmt) sqlite3_finalize(stmt);
        }

        if (allowPartialCandidateProbe) {
            for (const auto& prefix : spellingPrefixProbes(normalized)) {
                if (candidates.size() >= kMaxSpellingCandidatesPerToken) break;
                std::string upperBound = prefixRangeUpperBound(prefix);
                if (upperBound.empty()) continue;

                sqlite3_stmt* stmt = nullptr;
                if (sqlite3_prepare_v2(
                        db_,
                        "SELECT term, term, phonetic_key, total_count "
                        "FROM spell_terms "
                        "WHERE term >= ? AND term < ? "
                        "AND term_len BETWEEN ? AND ? "
                        "ORDER BY total_count DESC "
                        "LIMIT 96",
                        -1, &stmt, nullptr) == SQLITE_OK) {
                    bindText(stmt, 1, prefix);
                    bindText(stmt, 2, upperBound);
                    sqlite3_bind_int(stmt, 3, minLen);
                    sqlite3_bind_int(stmt, 4, maxLen);
                    readCandidates(stmt, candidates, false, true, false, false, false);
                }
                if (stmt) sqlite3_finalize(stmt);
            }
        }

        std::vector<Candidate> ranked;
        ranked.reserve(candidates.size());
        for (auto& [term, candidate] : candidates) {
            if (term == normalized) continue;
            if (candidate.prefixHit && !allowPartialCandidateProbe &&
                !candidate.spellingProbeHit && !candidate.orthographicHit &&
                !candidate.phoneticHit) {
                continue;
            }

            const std::string& candidateTerm = candidate.strippedTerm.empty()
                ? candidate.term
                : candidate.strippedTerm;
            int distance = smart_search::damerauLevenshteinDistance(normalized,
                                                                    candidateTerm);
            int allowedDistance = spellingDistanceThreshold(normalized.size(),
                                                            candidateTerm.size());
            double fuzzy = smart_search::fuzzyScore(normalized, candidateTerm);

            bool acceptable = false;
            if (distance > 0 && distance <= allowedDistance) {
                candidate.score = std::max(fuzzy, 0.92 - 0.08 * distance);
                acceptable = true;
            } else if (candidate.orthographicHit) {
                candidate.score = std::max(fuzzy, 0.76);
                acceptable = true;
            }

            if (!acceptable) continue;

            candidate.score += std::min(0.08,
                                        std::log(candidate.totalCount + 1.0) * 0.01);
            ranked.push_back(candidate);
        }

        std::sort(ranked.begin(), ranked.end(), [](const Candidate& lhs,
                                                   const Candidate& rhs) {
            if (lhs.score != rhs.score) return lhs.score > rhs.score;
            if (lhs.totalCount != rhs.totalCount) return lhs.totalCount > rhs.totalCount;
            return lhs.term < rhs.term;
        });

        auto storeAlternatives = [&](const std::string& key) {
            auto& values = alternatives[key];
            std::unordered_set<std::string> seen(values.begin(), values.end());
            for (const auto& candidate : ranked) {
                if (values.size() >= 3) break;
                if (candidate.score < ranked.front().score - 0.05) break;
                if (candidate.term == normalized) continue;
                if (seen.insert(candidate.term).second) {
                    values.push_back(candidate.term);
                }
            }
        };

        storeAlternatives(lowerToken);
        if (normalized != lowerToken) {
            storeAlternatives(normalized);
        }
    }

    if (scopedTermStmt) sqlite3_finalize(scopedTermStmt);
    return alternatives;
}

std::vector<SearchResult> SearchIndexer::searchSmart(
    const SearchRequest& request,
    const std::string& query,
    const std::string& language,
    int maxResults,
    bool includeSnippets,
    SmartSearchOptions options) const {

    std::vector<SearchResult> results;
    if (!db_ || query.empty()) return results;

    auto spellingAlternatives = buildSmartSpellingAlternatives(
        request, query, language, options);
    smart_search::SmartQueryPlan queryPlan = smart_search::buildSmartQueryPlan(
        query, language, spellingAlternatives, toQueryExpansionOptions(options));
    if (queryPlan.lexicalSources.empty()) return results;

    const int requestedLimit = (maxResults > 0) ? maxResults : request.maxResults;
    constexpr int kCandidateLimitPerSource = 250;
    MetadataFilter filter = buildMetadataFilter(request, "e");

    auto resultId = [](const SearchResult& result) {
        return result.resourceType + "\x1f" + result.module + "\x1f" + result.key;
    };

    std::unordered_map<std::string, SearchResult> candidates;
    std::vector<smart_search::WeightedRankList> rankedLists;
    rankedLists.reserve(queryPlan.lexicalSources.size());

    bool bibleOnly = !request.resourceTypes.empty();
    for (const auto& type : request.resourceTypes) {
        if (type != "bible") bibleOnly = false;
    }
    const std::string bm25Expression = bibleOnly
        ? "bm25(library_index, 0.0, 1.0, 0.0)"
        : "bm25(library_index, 5.0, 1.0, 0.25)";

    {
        std::lock_guard<std::mutex> lock(dbMutex_);

        for (const auto& source : queryPlan.lexicalSources) {
            std::string sql =
                "SELECT e.resource_type, e.module_name, e.key_text, e.title "
                "FROM library_index "
                "JOIN library_entries e ON e.entry_id = library_index.rowid "
                "WHERE library_index MATCH ? ";
            if (!filter.sql.empty()) {
                sql += "AND ";
                sql += filter.sql;
                sql += " ";
            }
            sql += "ORDER BY " + bm25Expression + " LIMIT ?";

            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
                continue;
            }
            bindText(stmt, 1, source.ftsQuery);
            bindMetadataFilterValues(stmt, 2, filter);
            sqlite3_bind_int(stmt, static_cast<int>(filter.values.size()) + 2,
                             kCandidateLimitPerSource);

            smart_search::WeightedRankList ranked;
            ranked.source = source.source;
            ranked.weight = source.weight;
            if (source.source == smart_search::RetrievalSource::ExactPhrase ||
                source.source == smart_search::RetrievalSource::OriginalTerms) {
                ranked.originalTermCoverage = queryPlan.coreTerms.size();
            }
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                SearchResult result;
                const char* type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                const char* module = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                const char* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                const char* title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));

                result.resourceType = type ? type : "";
                result.module = module ? module : request.moduleName;
                result.key = key ? key : "";
                result.title = title ? title : result.key;
                std::string id = resultId(result);
                candidates.emplace(id, result);
                ranked.resultIds.push_back(std::move(id));
            }
            sqlite3_finalize(stmt);
            if (!ranked.resultIds.empty()) rankedLists.push_back(std::move(ranked));
        }
    }

    if (bibleOnly && !request.moduleName.empty() && topicSearchProvider_ &&
        queryPlan.excludedTerms.empty()) {
        std::ostringstream topicQuery;
        for (const auto& term : queryPlan.coreTerms) {
            std::string clean = term;
            if (!clean.empty() && clean.back() == '*') clean.pop_back();
            if (clean.empty()) continue;
            if (topicQuery.tellp() > 0) topicQuery << ' ';
            topicQuery << clean;
        }
        const std::vector<TopicHit> topicHits = topicSearchProvider_->search(
            topicQuery.str(), kCandidateLimitPerSource);
        smart_search::WeightedRankList exactTopics;
        exactTopics.source = smart_search::RetrievalSource::NaveExactTopic;
        exactTopics.weight = 2.25;
        smart_search::WeightedRankList expandedTopics;
        expandedTopics.source = smart_search::RetrievalSource::NaveExpandedTopic;
        expandedTopics.weight = 1.0;
        for (const auto& hit : topicHits) {
            if (!matchesBibleReferenceScope(request, hit.reference)) continue;
            SearchResult result;
            result.resourceType = "bible";
            result.module = request.moduleName;
            result.key = hit.reference;
            result.title = hit.reference;
            std::string id = resultId(result);
            candidates.emplace(id, result);
            (hit.exactTopic ? exactTopics : expandedTopics).resultIds.push_back(
                std::move(id));
        }
        if (!exactTopics.resultIds.empty()) rankedLists.push_back(std::move(exactTopics));
        if (!expandedTopics.resultIds.empty()) rankedLists.push_back(std::move(expandedTopics));
    }

    if (bibleOnly && !request.moduleName.empty() && queryPlan.excludedTerms.empty() &&
        semanticSearchService_ &&
        semanticSearchService_->available()) {
        std::vector<SemanticHit> semanticHits = semanticSearchService_->search(
            language, request.moduleName, query, kCandidateLimitPerSource);
        smart_search::WeightedRankList ranked;
        ranked.source = smart_search::RetrievalSource::Semantic;
        ranked.weight = 1.75;
        for (const auto& hit : semanticHits) {
            SearchResult result;
            result.resourceType = "bible";
            result.module = request.moduleName;
            result.key = hit.reference;
            result.title = hit.reference;
            std::string id = resultId(result);
            candidates.emplace(id, result);
            ranked.resultIds.push_back(std::move(id));
        }
        if (!ranked.resultIds.empty()) rankedLists.push_back(std::move(ranked));
    }

    // TSK is corroboration only: it may strengthen an already retrieved
    // verse, but it never expands the candidate set.
    if (bibleOnly && !request.moduleName.empty() && !rankedLists.empty()) {
        CatalogSnapshot tskCatalog;
        std::string tskModule;
        {
            std::lock_guard<std::mutex> catalogLock(catalogMutex_);
            for (const auto& [name, entry] : moduleCatalog_) {
                if (lowerCopy(name) != "tsk") continue;
                tskModule = name;
                tskCatalog.info = entry.info;
                tskCatalog.resourceType = entry.resourceType;
                tskCatalog.moduleToken = entry.moduleToken;
                tskCatalog.moduleSignature = entry.signature;
                break;
            }
        }
        if (!tskModule.empty()) {
            const std::vector<std::string> seeds =
                smart_search::fuseRankedResults(rankedLists, 50);
            std::unordered_map<std::string, std::string> candidateByReference;
            for (const auto& [id, result] : candidates) {
                if (result.resourceType == "bible" && result.module == request.moduleName) {
                    candidateByReference[lowerCopy(trimCopy(result.key))] = id;
                }
            }

            ModuleScanSource tskSource;
            std::string tskError;
            if (prepareModuleLookupSource(tskModule, tskCatalog,
                                          importedModuleMgr_, tskError, tskSource) &&
                tskSource.mod) {
                smart_search::WeightedRankList corroborated;
                corroborated.source = smart_search::RetrievalSource::TskCorroboration;
                corroborated.weight = 0.25;
                std::unordered_set<std::string> seen;
                for (const auto& seedId : seeds) {
                    auto seed = candidates.find(seedId);
                    if (seed == candidates.end() || seed->second.resourceType != "bible") continue;
                    tskSource.mod->setKey(seed->second.key.c_str());
                    if (tskSource.mod->popError()) continue;
                    const char* raw = tskSource.mod->getRawEntry();
                    TopicDocument links = TopicSearchProvider::documentFromSwordEntry(
                        "", "", raw ? raw : "");
                    for (const auto& reference : links.references) {
                        auto target = candidateByReference.find(
                            lowerCopy(trimCopy(reference)));
                        if (target != candidateByReference.end() &&
                            target->second != seedId && seen.insert(target->second).second) {
                            corroborated.resultIds.push_back(target->second);
                        }
                    }
                }
                if (!corroborated.resultIds.empty()) {
                    rankedLists.push_back(std::move(corroborated));
                }
            }
        }
    }

    const size_t finalLimit = requestedLimit > 0
        ? static_cast<size_t>(requestedLimit)
        : 0;
    std::unordered_map<std::string, std::int64_t> canonicalOrders;
    if (bibleOnly) {
        for (const auto& [id, result] : candidates) {
            sword::VerseKey key(result.key.c_str());
            if (!key.popError()) canonicalOrders[id] = key.getIndex();
        }
    }
    std::vector<std::string> orderedIds =
        smart_search::fuseRankedResults(rankedLists, finalLimit, 60.0,
                                        canonicalOrders);
    results.reserve(orderedIds.size());
    for (const auto& id : orderedIds) {
        auto it = candidates.find(id);
        if (it != candidates.end()) results.push_back(std::move(it->second));
    }

    if (!includeSnippets) return results;

    return buildResultSnippets(results,
                               SnippetKind::Smart,
                               query,
                               language,
                               false,
                               options);
}

void SearchIndexer::workerLoop() {
    while (true) {
        IndexTask task;
        SemanticIndexTask semanticTask;
        bool isSemanticTask = false;
        auto finishTask = [this](bool semanticFinished) {
            {
                std::lock_guard<std::mutex> lock(workerMutex_);
                if (semanticFinished) {
                    semanticTaskRunning_ = false;
                    activeSemanticKey_.clear();
                }
                workerTaskRunning_ = false;
            }
            workerIdleCv_.notify_all();
            if (semanticFinished) semanticIdleCv_.notify_all();
        };
        {
            std::unique_lock<std::mutex> lock(workerMutex_);
            workerCv_.wait(lock, [this]() {
                return stopWorker_ ||
                       (suspendDepth_ == 0 &&
                        (!pendingSemanticIndexes_.empty() ||
                         !pendingModules_.empty()));
            });
            if (stopWorker_) break;
            if (!pendingSemanticIndexes_.empty()) {
                semanticTask = pendingSemanticIndexes_.front();
                pendingSemanticIndexes_.pop_front();
                isSemanticTask = true;
                semanticTaskRunning_ = true;
                activeSemanticKey_ = semanticTaskKey(
                    semanticTask.language, semanticTask.moduleName);
                semanticAbortRequested_.store(false);
                semanticUserCancelRequested_.store(false);
            } else {
                task = pendingModules_.front();
                pendingModules_.pop_front();
                auto forceIt = pendingForces_.find(task.moduleName);
                if (forceIt != pendingForces_.end()) {
                    task.force = task.force || forceIt->second;
                    pendingForces_.erase(forceIt);
                }
            }
            workerTaskRunning_ = true;
        }

        if (isSemanticTask) {
            runSemanticIndexTask(semanticTask);

            const bool semanticReady = semanticSearchService_ &&
                semanticSearchService_->indexReady(
                    semanticTask.language, semanticTask.moduleName);
            bool requeue = false;
            {
                std::lock_guard<std::mutex> lock(workerMutex_);
                requeue = semanticPauseRequested_.load() &&
                          !semanticUserCancelRequested_.load() &&
                          !stopWorker_ && !semanticReady;
                if (requeue) pendingSemanticIndexes_.push_front(semanticTask);
            }
            finishTask(true);
            continue;
        }

        if (task.moduleName.empty()) {
            finishTask(false);
            continue;
        }
        if (!task.force && isModuleIndexed(task.moduleName)) {
            finishTask(false);
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
        finishTask(false);
    }
}

void SearchIndexer::runSemanticIndexTask(const SemanticIndexTask& task) {
    const std::string key = semanticTaskKey(task.language, task.moduleName);
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        activeSemanticLanguage_ = task.language;
        activeSemanticModule_ = task.moduleName;
        activeSemanticProgress_ = 0;
        semanticBuildErrors_.erase(key);
        incompleteSemanticBuilds_.erase(key);
    }

    std::string error;
    const bool built = buildSemanticIndex(
        task.language, task.moduleName,
        [this, &task](size_t completed, size_t total) {
            const int percent = total == 0
                                    ? 0
                                    : static_cast<int>((completed * 100) / total);
            {
                std::lock_guard<std::mutex> lock(statusMutex_);
                if (activeSemanticLanguage_ == task.language &&
                    activeSemanticModule_ == task.moduleName) {
                    activeSemanticProgress_ = std::clamp(percent, 0, 100);
                }
            }
            return !stopRequested_.load() &&
                   !semanticAbortRequested_.load();
        },
        &semanticAbortRequested_, error);

    const bool cancelled = semanticAbortRequested_.load() ||
                           error.find("cancelled") != std::string::npos;
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        activeSemanticLanguage_.clear();
        activeSemanticModule_.clear();
        activeSemanticProgress_ = 0;
        if (built) {
            semanticBuildErrors_.erase(key);
            incompleteSemanticBuilds_.erase(key);
        } else if (cancelled) {
            semanticBuildErrors_.erase(key);
            incompleteSemanticBuilds_.insert(key);
        } else {
            semanticBuildErrors_[key] = error.empty()
                                            ? "Semantic index build failed."
                                            : error;
            incompleteSemanticBuilds_.erase(key);
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
        backendAvailable_.store(false);
        {
            std::lock_guard<std::mutex> statusLock(backendStatusMutex_);
            if (backendStatusMessage_.empty()) {
                backendStatusMessage_ = "Unable to initialize SQLite search schema.";
            }
        }
        sqlite3_close(writeDb);
        return;
    }
    backendAvailable_.store(true);
    {
        std::lock_guard<std::mutex> statusLock(backendStatusMutex_);
        backendStatusMessage_.clear();
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
    CatalogSnapshot catalog;
    catalog.info = catalogEntry.info;
    catalog.resourceType = catalogEntry.resourceType;
    catalog.moduleToken = catalogEntry.moduleToken;
    catalog.moduleSignature = catalogEntry.signature;

    ModuleScanSource source;
    std::string sourceError;
    if (!prepareModuleScanSource(moduleName, catalog, importedModuleMgr_, sourceError, source)) {
        writeError(sourceError);
        sqlite3_close(writeDb);
        return;
    }

    int totalEntries = static_cast<int>(source.imported
                                            ? source.importedEntries.size()
                                            : source.keyedEntries.size());
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

    sqlite3_stmt* deleteEntries = nullptr;
    sqlite3_stmt* deleteDictionaryEntries = nullptr;
    sqlite3_stmt* deleteStatus = nullptr;
    sqlite3_stmt* insertEntry = nullptr;
    sqlite3_stmt* insertFtsRow = nullptr;
    sqlite3_stmt* insertDictionaryEntry = nullptr;
    sqlite3_stmt* insertModuleTerm = nullptr;
    sqlite3_stmt* upsertSpellTerm = nullptr;
    sqlite3_stmt* markIndexed = nullptr;
    sqlite3_stmt* deleteError = nullptr;

    sqlite3_prepare_v2(
        writeDb,
        "DELETE FROM library_entries WHERE module_name = ?",
        -1, &deleteEntries, nullptr);
    sqlite3_prepare_v2(
        writeDb,
        "DELETE FROM dictionary_entries WHERE module_name = ?",
        -1, &deleteDictionaryEntries, nullptr);
    sqlite3_prepare_v2(
        writeDb,
        "DELETE FROM indexed_modules WHERE module_name = ?",
        -1, &deleteStatus, nullptr);
    sqlite3_prepare_v2(
        writeDb,
        "INSERT INTO library_entries(resource_type, module_token, scope_token, book_token, title, module_name, key_text) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)",
        -1, &insertEntry, nullptr);
    sqlite3_prepare_v2(
        writeDb,
        "INSERT INTO library_index(rowid, title, content, strongs_text) "
        "VALUES (?, ?, ?, ?)",
        -1, &insertFtsRow, nullptr);
    sqlite3_prepare_v2(
        writeDb,
        "INSERT OR REPLACE INTO dictionary_entries(module_name, key_text, title, normalized_key, normalized_title, position) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        -1, &insertDictionaryEntry, nullptr);
    sqlite3_prepare_v2(
        writeDb,
        "INSERT OR REPLACE INTO spell_module_terms(module_name, term, phonetic_key, doc_count, total_count) "
        "VALUES (?, ?, ?, ?, ?)",
        -1, &insertModuleTerm, nullptr);
    sqlite3_prepare_v2(
        writeDb,
        "INSERT INTO spell_terms(term, phonetic_key, term_len, doc_count, total_count) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(term) DO UPDATE SET "
        "phonetic_key = excluded.phonetic_key, "
        "term_len = excluded.term_len, "
        "doc_count = spell_terms.doc_count + excluded.doc_count, "
        "total_count = spell_terms.total_count + excluded.total_count",
        -1, &upsertSpellTerm, nullptr);
    sqlite3_prepare_v2(
        writeDb,
        "INSERT OR REPLACE INTO indexed_modules(module_name, resource_type, module_signature, entry_count, indexed_at) "
        "VALUES (?, ?, ?, ?, datetime('now'))",
        -1, &markIndexed, nullptr);
    sqlite3_prepare_v2(
        writeDb,
        "DELETE FROM module_index_errors WHERE module_name = ?",
        -1, &deleteError, nullptr);

    if (!deleteEntries || !deleteDictionaryEntries || !deleteStatus ||
        !insertEntry || !insertFtsRow || !insertDictionaryEntry ||
        !insertModuleTerm || !upsertSpellTerm || !markIndexed || !deleteError) {
        if (deleteEntries) sqlite3_finalize(deleteEntries);
        if (deleteDictionaryEntries) sqlite3_finalize(deleteDictionaryEntries);
        if (deleteStatus) sqlite3_finalize(deleteStatus);
        if (insertEntry) sqlite3_finalize(insertEntry);
        if (insertFtsRow) sqlite3_finalize(insertFtsRow);
        if (insertDictionaryEntry) sqlite3_finalize(insertDictionaryEntry);
        if (insertModuleTerm) sqlite3_finalize(insertModuleTerm);
        if (upsertSpellTerm) sqlite3_finalize(upsertSpellTerm);
        if (markIndexed) sqlite3_finalize(markIndexed);
        if (deleteError) sqlite3_finalize(deleteError);
        sqlite3_exec(writeDb, "ROLLBACK;", nullptr, nullptr, nullptr);
        writeError("Failed to prepare indexing statements.");
        sqlite3_close(writeDb);
        return;
    }

    bindText(deleteEntries, 1, moduleName);
    sqlite3_step(deleteEntries);
    sqlite3_reset(deleteEntries);
    sqlite3_clear_bindings(deleteEntries);

    bindText(deleteDictionaryEntries, 1, moduleName);
    sqlite3_step(deleteDictionaryEntries);
    sqlite3_reset(deleteDictionaryEntries);
    sqlite3_clear_bindings(deleteDictionaryEntries);

    removeModuleSpellTerms(writeDb, moduleName);

    bindText(deleteStatus, 1, moduleName);
    sqlite3_step(deleteStatus);
    sqlite3_reset(deleteStatus);
    sqlite3_clear_bindings(deleteStatus);

    std::unordered_map<std::string, VocabularyAggregate> vocabulary;

    auto insertCurrentRow = [&](const std::string& scopeToken,
                                const std::string& bookToken,
                                const std::string& title,
                                const std::string& content,
                                const std::string& strongsText,
                                const std::string& keyText) -> bool {
        bindText(insertEntry, 1, source.resourceType);
        bindText(insertEntry, 2, source.moduleToken);
        bindText(insertEntry, 3, scopeToken);
        bindText(insertEntry, 4, bookToken);
        bindText(insertEntry, 5, title);
        bindText(insertEntry, 6, moduleName);
        bindText(insertEntry, 7, keyText);
        bool ok = (sqlite3_step(insertEntry) == SQLITE_DONE);
        sqlite3_reset(insertEntry);
        sqlite3_clear_bindings(insertEntry);
        if (!ok) return false;

        sqlite3_int64 entryId = sqlite3_last_insert_rowid(writeDb);
        sqlite3_bind_int64(insertFtsRow, 1, entryId);
        bindText(insertFtsRow, 2, title);
        bindText(insertFtsRow, 3, content);
        bindText(insertFtsRow, 4, strongsText);
        ok = (sqlite3_step(insertFtsRow) == SQLITE_DONE);
        sqlite3_reset(insertFtsRow);
        sqlite3_clear_bindings(insertFtsRow);
        if (ok) {
            ++insertedEntries;
            collectVocabularyTerms(vocabulary, moduleName, title, content);
        }
        return ok;
    };

    auto insertDictionaryKey = [&](const std::string& keyText,
                                   const std::string& title,
                                   int position) -> bool {
        bindText(insertDictionaryEntry, 1, moduleName);
        bindText(insertDictionaryEntry, 2, keyText);
        bindText(insertDictionaryEntry, 3, title);
        bindText(insertDictionaryEntry, 4, normalizeDictionaryLookupText(keyText));
        bindText(insertDictionaryEntry, 5, normalizeDictionaryLookupText(title));
        sqlite3_bind_int(insertDictionaryEntry, 6, position);
        bool ok = (sqlite3_step(insertDictionaryEntry) == SQLITE_DONE);
        sqlite3_reset(insertDictionaryEntry);
        sqlite3_clear_bindings(insertDictionaryEntry);
        if (ok) ++insertedEntries;
        return ok;
    };

    auto insertVocabularyRows = [&]() -> bool {
        for (const auto& [key, aggregate] : vocabulary) {
            if (stopRequested_.load()) return false;

            (void)key;
            bindText(insertModuleTerm, 1, aggregate.moduleName);
            bindText(insertModuleTerm, 2, aggregate.term);
            bindText(insertModuleTerm, 3, aggregate.phoneticKey);
            sqlite3_bind_int(insertModuleTerm, 4, aggregate.docCount);
            sqlite3_bind_int(insertModuleTerm, 5, aggregate.totalCount);

            bool ok = (sqlite3_step(insertModuleTerm) == SQLITE_DONE);
            sqlite3_reset(insertModuleTerm);
            sqlite3_clear_bindings(insertModuleTerm);
            if (!ok) return false;

            bindText(upsertSpellTerm, 1, aggregate.term);
            bindText(upsertSpellTerm, 2, aggregate.phoneticKey);
            sqlite3_bind_int(upsertSpellTerm, 3,
                             static_cast<int>(aggregate.term.size()));
            sqlite3_bind_int(upsertSpellTerm, 4, aggregate.docCount);
            sqlite3_bind_int(upsertSpellTerm, 5, aggregate.totalCount);

            ok = (sqlite3_step(upsertSpellTerm) == SQLITE_DONE);
            sqlite3_reset(upsertSpellTerm);
            sqlite3_clear_bindings(upsertSpellTerm);
            if (!ok) return false;
        }

        return true;
    };

    bool cancelled = false;
    bool failed = false;
    bool committed = false;
    std::string failureMessage;
    const bool buildNaveTopics = lowerCopy(moduleName) == "nave";
    std::vector<TopicDocument> topicDocuments;
    if (buildNaveTopics) topicDocuments.reserve(source.keyedEntries.size());

    if (source.imported) {
        for (size_t i = 0; i < source.importedEntries.size(); ++i) {
            const auto& entry = source.importedEntries[i];
            if (!insertCurrentRow("",
                                  "",
                                  entry.title.empty() ? entry.key : entry.title,
                                  entry.plainText,
                                  "",
                                  entry.key)) {
                failed = true;
                failureMessage = "Failed to insert imported module index entry.";
                break;
            }
            setProgress(static_cast<int>(((i + 1) * 100) /
                                         std::max<size_t>(source.importedEntries.size(), 1)));
        }
    } else if (source.resourceType == "dictionary") {
        int position = 0;
        for (const auto& key : source.keyedEntries) {
            if (stopRequested_.load()) {
                cancelled = true;
                break;
            }

            ++processedEntries;
            if ((processedEntries % 128) == 0) bumpProgress();

            std::string title = key;
            auto titleIt = source.entryTitles.find(key);
            if (titleIt != source.entryTitles.end()) {
                std::string trimmedTitle = trimCopy(titleIt->second);
                if (!trimmedTitle.empty()) title = std::move(trimmedTitle);
            }

            if (buildNaveTopics && source.mod) {
                source.mod->setKey(key.c_str());
                if (!source.mod->popError()) {
                    const char* raw = source.mod->getRawEntry();
                    const std::string rawEntry = raw ? raw : "";
                    const char* plain = source.mod->stripText();
                    topicDocuments.push_back(
                        TopicSearchProvider::documentFromSwordEntry(
                            title,
                            trimCopy(plain ? plain : ""),
                            rawEntry));
                }
            }

            if (!insertDictionaryKey(key, title, position++)) {
                failed = true;
                failureMessage = "Failed to insert dictionary key index entry.";
                break;
            }
        }
    } else if (source.resourceType == "bible") {
        // First pass: collect plain text with options off
        source.mgr->setGlobalOption("Strong's Numbers", "Off");
        source.mgr->setGlobalOption("Morphological Tags", "Off");
        source.mgr->setGlobalOption("Footnotes", "Off");
        source.mgr->setGlobalOption("Cross-references", "Off");
        source.mgr->setGlobalOption("Headings", "Off");

        std::vector<std::string> plainTexts;
        plainTexts.reserve(source.keyedEntries.size());
        for (const auto& key : source.keyedEntries) {
            if (stopRequested_.load()) {
                cancelled = true;
                break;
            }
            source.mod->setKey(key.c_str());
            if (source.mod->popError()) {
                plainTexts.emplace_back();
                continue;
            }
            const char* plainRaw = source.mod->stripText();
            plainTexts.push_back(trimCopy(plainRaw ? plainRaw : ""));
        }

        // Second pass: collect XHTML with options on, then insert
        source.mgr->setGlobalOption("Strong's Numbers", "On");
        source.mgr->setGlobalOption("Morphological Tags", "On");
        source.mgr->setGlobalOption("Footnotes", "On");
        source.mgr->setGlobalOption("Cross-references", "On");
        source.mgr->setGlobalOption("Headings", "On");
        source.mod->setProcessEntryAttributes(true);

        for (size_t ki = 0; ki < source.keyedEntries.size() && !cancelled; ++ki) {
            if (stopRequested_.load()) {
                cancelled = true;
                break;
            }

            ++processedEntries;
            if ((processedEntries % 128) == 0) bumpProgress();

            const auto& key = source.keyedEntries[ki];
            source.mod->setKey(key.c_str());
            if (source.mod->popError()) continue;

            std::string plain = ki < plainTexts.size() ? plainTexts[ki] : "";
            std::string xhtml = std::string(source.mod->renderText().c_str());

            std::string keyText = trimCopy(source.mod->getKeyText()
                                               ? source.mod->getKeyText()
                                               : "");
            if (keyText.empty()) keyText = key;

            std::string title = keyText;
            std::string scopeToken;
            std::string bookToken;
            if (auto* vk = dynamic_cast<sword::VerseKey*>(source.mod->getKey())) {
                const char* bookName = vk->getBookName();
                scopeToken = makeBibleTestamentToken(vk->getTestament());
                bookToken = normalizeFilterToken(bookName ? bookName : "");
            }
            std::string strongsIndexText = buildCompactStrongsIndexText(source.mod, xhtml);
            if (!insertCurrentRow(scopeToken,
                                  bookToken,
                                  title,
                                  plain,
                                  strongsIndexText,
                                  keyText)) {
                failed = true;
                failureMessage = "Failed to insert indexed Bible verse.";
                break;
            }
        }
    } else {
        for (const auto& key : source.keyedEntries) {
            if (stopRequested_.load()) {
                cancelled = true;
                break;
            }

            ++processedEntries;
            if ((processedEntries % 64) == 0) bumpProgress();

            source.mod->setKey(key.c_str());
            if (source.mod->popError()) continue;

            std::string rawEntry;
            if (buildNaveTopics) {
                const char* raw = source.mod->getRawEntry();
                rawEntry = raw ? raw : "";
            }
            const char* plainRaw = source.mod->stripText();
            std::string plain = trimCopy(plainRaw ? plainRaw : "");
            std::string title = key;
            auto titleIt = source.entryTitles.find(key);
            if (titleIt != source.entryTitles.end() && !trimCopy(titleIt->second).empty()) {
                title = titleIt->second;
            }

            if (plain.empty() && title.empty()) continue;
            if (!insertCurrentRow("", "", title, plain, "", key)) {
                failed = true;
                failureMessage = "Failed to insert indexed module entry.";
                break;
            }
            if (buildNaveTopics) {
                topicDocuments.push_back(TopicSearchProvider::documentFromSwordEntry(
                    title, plain, rawEntry));
            }
        }
    }

    if (!cancelled && !failed) {
        if (stopRequested_.load()) {
            cancelled = true;
        } else if (!insertVocabularyRows()) {
            if (stopRequested_.load()) {
                cancelled = true;
            } else {
                failed = true;
                failureMessage = "Failed to insert spelling vocabulary index.";
            }
        }
    }

    if (!cancelled && !failed) {
        bindText(markIndexed, 1, moduleName);
        bindText(markIndexed, 2, source.resourceType);
        bindText(markIndexed, 3, source.moduleSignature);
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
            committed = true;
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

    sqlite3_finalize(deleteEntries);
    sqlite3_finalize(deleteDictionaryEntries);
    sqlite3_finalize(deleteStatus);
    sqlite3_finalize(insertEntry);
    sqlite3_finalize(insertFtsRow);
    sqlite3_finalize(insertDictionaryEntry);
    sqlite3_finalize(insertModuleTerm);
    sqlite3_finalize(upsertSpellTerm);
    sqlite3_finalize(markIndexed);
    sqlite3_finalize(deleteError);
    sqlite3_close(writeDb);

    if (committed && buildNaveTopics && topicSearchProvider_) {
        std::string topicError;
        if (!topicSearchProvider_->rebuild(source.moduleSignature,
                                           topicDocuments, topicError)) {
            std::cerr << "SearchIndexer: unable to rebuild Nave topic index ("
                      << topicError << ")\n";
        }
    }
}

bool SearchIndexer::buildSemanticIndex(
    const std::string& language,
    const std::string& moduleName,
    SemanticProgressCallback progress,
    std::atomic<bool>* cancel,
    std::string& errorOut) const {
    if (!semanticSearchService_) {
        errorOut = "Semantic search is not initialized.";
        return false;
    }
    if (language.empty() || moduleName.empty()) {
        errorOut = "Choose a language and reference Bible first.";
        return false;
    }

    ModuleCatalogEntry catalogEntry;
    {
        std::lock_guard<std::mutex> lock(catalogMutex_);
        auto it = moduleCatalog_.find(moduleName);
        if (it == moduleCatalog_.end()) {
            errorOut = "The selected reference Bible is no longer installed.";
            return false;
        }
        catalogEntry = it->second;
    }
    if (catalogEntry.resourceType != "bible") {
        errorOut = "Semantic indexes can only be built for Bible modules.";
        return false;
    }

    CatalogSnapshot catalog;
    catalog.info = catalogEntry.info;
    catalog.resourceType = catalogEntry.resourceType;
    catalog.moduleToken = catalogEntry.moduleToken;
    catalog.moduleSignature = catalogEntry.signature;

    ModuleScanSource source;
    if (!prepareModuleScanSource(moduleName, catalog, importedModuleMgr_,
                                 errorOut, source)) {
        return false;
    }
    if (!source.mod || source.resourceType != "bible") {
        errorOut = "The selected module is not a readable Bible.";
        return false;
    }

    source.mgr->setGlobalOption("Strong's Numbers", "Off");
    source.mgr->setGlobalOption("Morphological Tags", "Off");
    source.mgr->setGlobalOption("Footnotes", "Off");
    source.mgr->setGlobalOption("Cross-references", "Off");
    source.mgr->setGlobalOption("Headings", "Off");

    std::vector<SemanticPassage> passages;
    passages.reserve(source.keyedEntries.size());
    const size_t scanTotal = source.keyedEntries.size();
    const size_t combinedTotal = scanTotal * 2;
    for (size_t i = 0; i < source.keyedEntries.size(); ++i) {
        if ((cancel && cancel->load()) ||
            (progress && !progress(i, combinedTotal))) {
            errorOut = "Semantic index build cancelled.";
            return false;
        }
        source.mod->setKey(source.keyedEntries[i].c_str());
        if (source.mod->popError()) continue;
        const char* plainRaw = source.mod->stripText();
        std::string plain = trimCopy(plainRaw ? plainRaw : "");
        std::string reference = trimCopy(source.mod->getKeyText()
                                             ? source.mod->getKeyText()
                                             : "");
        if (!reference.empty() && !plain.empty()) {
            passages.push_back({std::move(reference), std::move(plain)});
        }
    }
    if (passages.empty()) {
        errorOut = "The selected Bible did not provide any verse text.";
        return false;
    }

    auto encodeProgress = [&](size_t completed, size_t total) {
        if (!progress) return true;
        const size_t scaled = total == 0
                                  ? scanTotal
                                  : (completed * scanTotal) / total;
        return progress(scanTotal + scaled, combinedTotal);
    };
    return semanticSearchService_->buildIndex(
        language, moduleName, source.moduleSignature, passages,
        encodeProgress, cancel, errorOut);
}

} // namespace verdad
