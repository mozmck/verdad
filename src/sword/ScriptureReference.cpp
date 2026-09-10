#include "sword/ScriptureReference.h"

#include "sword/SwordManager.h"

#include <versificationmgr.h>

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <unordered_map>
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

// Use SWORD's explicit aliases (Mt, Mk, Lk, Jn, etc.) before trying
// unambiguous prefixes of canonical names. Do not use fuzzy matching in prose.
const sword::VersificationMgr::Book* referenceBook(const std::string& name) {
    static const auto* system =
        sword::VersificationMgr::getSystemVersificationMgr()->getVersificationSystem("KJV");
    static const auto aliases = [] {
        std::unordered_map<std::string, int> result;
        for (const sword::abbrev* alias = sword::builtin_abbrevs;
             *alias->ab; ++alias) {
            int number = system->getBookNumberByOSISName(alias->osis);
            if (number > 0) {
                for (const auto& key : bookLookupKeys(alias->ab)) result[key] = number - 1;
            }
        }
        for (int i = 0; i < system->getBookCount(); ++i) {
            const auto* book = system->getBook(i);
            for (const char* label : {book->getLongName(), book->getOSISName(),
                                      book->getPreferredAbbreviation()}) {
                for (const auto& key : bookLookupKeys(label)) result[key] = i;
            }
        }
        // Preserve the application's established interpretation of Jud.
        result["jud"] = system->getBookNumberByOSISName("Judg") - 1;
        return result;
    }();
    const auto keys = bookLookupKeys(name);
    for (const auto& key : keys) {
        auto it = aliases.find(key);
        if (it != aliases.end()) return system->getBook(it->second);
    }
    const sword::VersificationMgr::Book* match = nullptr;
    for (int i = 0; i < system->getBookCount(); ++i) {
        const auto* book = system->getBook(i);
        for (const char* label : {book->getLongName(), book->getOSISName(),
                                  book->getPreferredAbbreviation()}) {
            const std::string candidate = normalizeBookLookupKey(label);
            for (const auto& key : keys) {
                if (key.size() < 2 || candidate.rfind(key, 0) != 0) continue;
                if (match && match != book) return nullptr;
                match = book;
            }
        }
    }
    return match;
}

bool referenceWordChar(char c) {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_' || uc >= 128;
}

bool listSeparator(const std::string& text, int start, int end) {
    bool separator = false;
    for (int i = start; i < end; ++i) {
        if (text[i] == ',' || text[i] == ';') {
            if (separator) return false;
            separator = true;
        } else if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r') {
            return false;
        }
    }
    return separator;
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

std::vector<VerseReference> verseReferences(const std::string& text) {
    // A token may name a book or inherit it from the preceding list item.
    // Keep newlines out of the grammar so context never leaks into a new paragraph.
    static const std::regex tokenRe(
        R"(((?:(?:[1-3]|III|II|I)[ \t]*)?[A-Za-z]+\.?(?:[ \t]+[A-Za-z]+\.?){0,4}[ \t]*)?([0-9]+)(?:[ \t]*:[ \t]*([0-9]+))?(?:[ \t]*(?:-|–|—)[ \t]*([0-9]+)(?:[ \t]*:[ \t]*([0-9]+))?)?)",
        std::regex::icase);

    std::vector<VerseReference> references;
    const sword::VersificationMgr::Book* lastBook = nullptr;
    int lastChapter = 0;
    int lastEnd = 0;
    std::smatch token;
    size_t searchStart = 0;
    while (std::regex_search(text.begin() + searchStart, text.end(), token, tokenRe)) {
        int start = static_cast<int>(searchStart + token.position());
        const int end = start + static_cast<int>(token.length());
        // If a prose prefix consumed an ordinal ("see 1 John"), retry from
        // that number when validation fails, rather than losing the book number.
        searchStart = token[1].matched ? searchStart + token.position(2) : end;
        if ((start > 0 && referenceWordChar(text[start - 1])) ||
            (end < static_cast<int>(text.size()) &&
             (referenceWordChar(text[end]) || text[end] == ':'))) continue;

        const sword::VersificationMgr::Book* book = nullptr;
        if (token[1].matched) {
            const std::string name = token[1].str();
            size_t offset = 0;
            while (offset < name.size()) {
                book = referenceBook(trimCopy(name.substr(offset)));
                if (book) {
                    start += static_cast<int>(offset);
                    break;
                }
                offset = name.find_first_of(" \t", offset);
                if (offset == std::string::npos) break;
                offset = name.find_first_not_of(" \t", offset);
            }
        } else if (lastBook && listSeparator(text, lastEnd, start)) {
            book = lastBook;
        }
        if (!book) continue;

        int first, chapter, verse, endChapter, endVerse;
        try {
            first = std::stoi(token[2].str());
            if (token[3].matched) {
                chapter = first;
                verse = std::stoi(token[3].str());
            } else {
                // Bare numbers mean verses only in a list or a single-chapter book.
                if (token[1].matched && book->getChapterMax() != 1) continue;
                chapter = token[1].matched ? 1 : lastChapter;
                verse = first;
            }
            endChapter = token[5].matched ? std::stoi(token[4].str()) : chapter;
            endVerse = token[5].matched ? std::stoi(token[5].str()) :
                       token[4].matched ? std::stoi(token[4].str()) : verse;
        } catch (...) {
            continue;
        }
        if (chapter <= 0 || chapter > book->getChapterMax() ||
            endChapter < chapter || endChapter > book->getChapterMax() ||
            verse <= 0 || verse > book->getVerseMax(chapter) ||
            endVerse <= 0 || endVerse > book->getVerseMax(endChapter) ||
            (endChapter == chapter && endVerse < verse)) continue;

        std::string target = std::string(book->getLongName()) + " " +
                             std::to_string(chapter) + ":" + std::to_string(verse);
        if (token[4].matched) {
            target += "-";
            if (endChapter != chapter) target += std::to_string(endChapter) + ":";
            target += std::to_string(endVerse);
        }
        references.push_back({start, end, std::move(target)});
        lastBook = book;
        lastChapter = endChapter;
        lastEnd = end;
        searchStart = end;
    }
    return references;
}

std::vector<std::pair<int, int>> verseReferenceRanges(const std::string& text) {
    std::vector<std::pair<int, int>> ranges;
    for (const auto& ref : verseReferences(text)) {
        ranges.emplace_back(ref.start, ref.end);
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
    for (const auto& range : verseReferences(text)) {
        bool inside = (pos >= range.start && pos < range.end);
        bool onRightEdge =
            (pos > 0 && (pos - 1) >= range.start && (pos - 1) < range.end);
        if (!inside && !onRightEdge) continue;
        if (startOut) *startOut = range.start;
        if (endOut) *endOut = range.end;
        return range.reference;
    }
    return "";
}

} // namespace scripture
} // namespace verdad
