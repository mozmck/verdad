#include "sword/ScriptureReference.h"

#include "sword/SwordManager.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <utility>

namespace verdad {
namespace scripture {
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

std::vector<std::string> splitList(const std::string& text, char delim) {
    std::vector<std::string> items;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(delim, start);
        std::string item = trimCopy(text.substr(
            start,
            (end == std::string::npos ? text.size() : end) - start));
        if (!item.empty()) items.push_back(std::move(item));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return items;
}

bool anyBookKeyEquals(const std::vector<std::string>& lhs,
                      const std::vector<std::string>& rhs) {
    for (const auto& left : lhs) {
        for (const auto& right : rhs) {
            if (left == right) return true;
        }
    }
    return false;
}

bool anyBookKeyPrefixMatches(const std::vector<std::string>& queryKeys,
                             const std::vector<std::string>& candidateKeys) {
    for (const auto& query : queryKeys) {
        for (const auto& candidate : candidateKeys) {
            if (query.empty() || candidate.empty()) continue;
            if (candidate.rfind(query, 0) == 0 ||
                query.rfind(candidate, 0) == 0) {
                return true;
            }
        }
    }
    return false;
}

struct VerseReferenceRange {
    int start = 0;
    int end = 0;
};

bool findValidVerseReferenceSuffix(const std::string& candidate,
                                   int basePos,
                                   VerseReferenceRange& rangeOut) {
    size_t pos = 0;
    while (pos < candidate.size()) {
        while (pos < candidate.size() &&
               std::isspace(static_cast<unsigned char>(candidate[pos]))) {
            ++pos;
        }
        if (pos >= candidate.size()) break;

        std::string suffix = candidate.substr(pos);
        if (SwordManager::isValidVerseRef(suffix)) {
            rangeOut.start = basePos + static_cast<int>(pos);
            rangeOut.end = basePos + static_cast<int>(candidate.size());
            return true;
        }

        while (pos < candidate.size() &&
               !std::isspace(static_cast<unsigned char>(candidate[pos]))) {
            ++pos;
        }
    }
    return false;
}

} // namespace

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

std::string ordinalBookLookupKey(const std::string& text) {
    std::string trimmed = trimCopy(text);
    if (trimmed.empty()) return "";

    auto skipSeparators = [&trimmed](size_t pos) {
        while (pos < trimmed.size()) {
            unsigned char c = static_cast<unsigned char>(trimmed[pos]);
            if (std::isalnum(c)) break;
            ++pos;
        }
        return pos;
    };

    unsigned char first = static_cast<unsigned char>(trimmed.front());
    if (first >= '1' && first <= '3') {
        size_t restPos = skipSeparators(1);
        std::string rest = normalizeBookLookupKey(trimmed.substr(restPos));
        if (rest.empty()) return "";

        const char* roman = first == '1' ? "i" : (first == '2' ? "ii" : "iii");
        return std::string(roman) + rest;
    }

    size_t romanLen = 0;
    while (romanLen < trimmed.size() && romanLen < 3) {
        char c = static_cast<char>(
            std::tolower(static_cast<unsigned char>(trimmed[romanLen])));
        if (c != 'i') break;
        ++romanLen;
    }
    if (romanLen == 0) return "";

    size_t afterRoman = romanLen;
    if (afterRoman < trimmed.size() &&
        std::isalnum(static_cast<unsigned char>(trimmed[afterRoman]))) {
        return "";
    }

    size_t restPos = skipSeparators(afterRoman);
    std::string rest = normalizeBookLookupKey(trimmed.substr(restPos));
    if (rest.empty()) return "";

    return std::to_string(static_cast<int>(romanLen)) + rest;
}

std::vector<std::string> bookLookupKeys(const std::string& text) {
    std::vector<std::string> keys;
    std::string normalized = normalizeBookLookupKey(text);
    if (!normalized.empty()) keys.push_back(normalized);

    std::string ordinal = ordinalBookLookupKey(text);
    if (!ordinal.empty() &&
        std::find(keys.begin(), keys.end(), ordinal) == keys.end()) {
        keys.push_back(std::move(ordinal));
    }
    return keys;
}

std::string canonicalBookLabelForModule(SwordManager& manager,
                                        const std::string& moduleName,
                                        const std::string& book) {
    const std::string trimmedBook = trimCopy(book);
    if (moduleName.empty() || trimmedBook.empty()) {
        return trimmedBook;
    }

    const std::vector<std::string> wantedKeys = bookLookupKeys(trimmedBook);
    if (wantedKeys.empty()) return trimmedBook;

    std::vector<std::string> books = manager.getBookNames(moduleName);
    std::string uniquePrefixMatch;
    int prefixMatches = 0;

    for (const auto& candidate : books) {
        std::vector<std::string> candidateKeys = bookLookupKeys(candidate);
        if (anyBookKeyEquals(wantedKeys, candidateKeys)) {
            return candidate;
        }

        std::string shortRef = manager.getShortReference(moduleName,
                                                         candidate + " 1:1");
        SwordManager::VerseRef shortParsed = SwordManager::parseVerseRef(shortRef);
        std::vector<std::string> shortKeys = bookLookupKeys(shortParsed.book);
        if (anyBookKeyEquals(wantedKeys, shortKeys)) {
            return candidate;
        }

        if (anyBookKeyPrefixMatches(wantedKeys, candidateKeys) ||
            anyBookKeyPrefixMatches(wantedKeys, shortKeys)) {
            uniquePrefixMatch = candidate;
            ++prefixMatches;
        }
    }

    return prefixMatches == 1 ? uniquePrefixMatch : trimmedBook;
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

std::vector<std::pair<int, int>> verseReferenceRanges(const std::string& text) {
    static const std::regex refRe(
        R"(((?:[1-3]\s+)?[A-Za-z]+(?:\s+[A-Za-z]+)*\s+\d+:\d+(?:-\d+)?))");

    std::vector<std::pair<int, int>> ranges;
    auto begin = std::sregex_iterator(text.begin(), text.end(), refRe);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::string candidate = (*it)[1].str();
        VerseReferenceRange range;
        if (findValidVerseReferenceSuffix(candidate,
                                          static_cast<int>((*it).position(1)),
                                          range)) {
            ranges.emplace_back(range.start, range.end);
        }
    }
    return ranges;
}

std::string verseReferenceAtPosition(const std::string& text,
                                     int pos,
                                     int* startOut,
                                     int* endOut) {
    if (startOut) *startOut = 0;
    if (endOut) *endOut = 0;
    if (text.empty()) return "";

    pos = std::clamp(pos, 0, static_cast<int>(text.size()));
    for (const auto& range : verseReferenceRanges(text)) {
        bool inside = (pos >= range.first && pos < range.second);
        bool onRightEdge =
            (pos > 0 && (pos - 1) >= range.first && (pos - 1) < range.second);
        if (!inside && !onRightEdge) continue;
        if (startOut) *startOut = range.first;
        if (endOut) *endOut = range.second;
        return text.substr(static_cast<size_t>(range.first),
                           static_cast<size_t>(range.second - range.first));
    }
    return "";
}

} // namespace scripture
} // namespace verdad
