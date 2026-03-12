#ifndef VERDAD_VERSE_REFERENCE_SORT_H
#define VERDAD_VERSE_REFERENCE_SORT_H

#include "sword/SwordManager.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace verdad {
namespace verse_reference_sort {
namespace detail {

inline std::string trimReferenceText(const std::string& text) {
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

inline std::string normalizeBookKey(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            out.push_back(static_cast<char>(std::tolower(uc)));
        }
    }
    return out;
}

inline bool parseReferenceKey(const std::string& key, SwordManager::VerseRef& out) {
    out = SwordManager::VerseRef{};
    try {
        out = SwordManager::parseVerseRef(key);
    } catch (...) {
        out = SwordManager::VerseRef{};
    }

    if (!out.book.empty() && out.chapter > 0 && out.verse > 0) {
        return true;
    }

    static const std::regex fallbackRe(R"(^\s*(.+?)\s+(\d+):(\d+)(?:-\d+)?\s*$)");
    std::smatch match;
    if (!std::regex_match(key, match, fallbackRe)) return false;

    out.book = trimReferenceText(match[1].str());
    try {
        out.chapter = std::stoi(match[2].str());
        out.verse = std::stoi(match[3].str());
    } catch (...) {
        return false;
    }

    return !out.book.empty() && out.chapter > 0 && out.verse > 0;
}

struct SortKey {
    bool parsed = false;
    int bookRank = std::numeric_limits<int>::max();
    int chapter = std::numeric_limits<int>::max();
    int verse = std::numeric_limits<int>::max();
    std::string normBook;
};

template <typename Entry, typename KeyFn>
inline void sortEntriesCanonical(SwordManager& swordMgr,
                                 const std::string& moduleName,
                                 std::vector<Entry>& entries,
                                 KeyFn keyFn) {
    if (entries.size() < 2 || moduleName.empty()) return;

    std::unordered_map<std::string, int> bookOrder;
    const auto books = swordMgr.getBookNames(moduleName);
    for (size_t i = 0; i < books.size(); ++i) {
        std::string key = normalizeBookKey(books[i]);
        if (!key.empty() && bookOrder.find(key) == bookOrder.end()) {
            bookOrder.emplace(key, static_cast<int>(i));
        }
    }

    std::unordered_map<std::string, SortKey> cache;
    cache.reserve(entries.size());

    auto getSortKey = [&](const std::string& ref) -> const SortKey& {
        auto it = cache.find(ref);
        if (it != cache.end()) return it->second;

        SortKey sortKey;
        SwordManager::VerseRef parsed;
        if (parseReferenceKey(ref, parsed)) {
            sortKey.parsed = true;
            sortKey.chapter = parsed.chapter;
            sortKey.verse = parsed.verse;
            sortKey.normBook = normalizeBookKey(parsed.book);
            auto bookIt = bookOrder.find(sortKey.normBook);
            if (bookIt != bookOrder.end()) {
                sortKey.bookRank = bookIt->second;
            }
        }

        auto inserted = cache.emplace(ref, std::move(sortKey));
        return inserted.first->second;
    };

    std::stable_sort(entries.begin(), entries.end(),
                     [&](const Entry& a, const Entry& b) {
        const std::string& aKey = keyFn(a);
        const std::string& bKey = keyFn(b);
        const SortKey& ka = getSortKey(aKey);
        const SortKey& kb = getSortKey(bKey);

        if (ka.parsed != kb.parsed) return ka.parsed > kb.parsed;
        if (!ka.parsed && !kb.parsed) return aKey < bKey;

        if (ka.bookRank != kb.bookRank) return ka.bookRank < kb.bookRank;
        if (ka.bookRank == std::numeric_limits<int>::max() &&
            ka.normBook != kb.normBook) {
            return ka.normBook < kb.normBook;
        }
        if (ka.chapter != kb.chapter) return ka.chapter < kb.chapter;
        if (ka.verse != kb.verse) return ka.verse < kb.verse;
        return aKey < bKey;
    });
}

} // namespace detail

inline void sortSearchResultsCanonical(SwordManager& swordMgr,
                                       const std::string& moduleName,
                                       std::vector<SearchResult>& results) {
    detail::sortEntriesCanonical(
        swordMgr, moduleName, results,
        [](const SearchResult& result) -> const std::string& {
            return result.key;
        });
}

inline void sortVerseKeysCanonical(SwordManager& swordMgr,
                                   const std::string& moduleName,
                                   std::vector<std::string>& verseKeys) {
    detail::sortEntriesCanonical(
        swordMgr, moduleName, verseKeys,
        [](const std::string& verseKey) -> const std::string& {
            return verseKey;
        });
}

} // namespace verse_reference_sort
} // namespace verdad

#endif // VERDAD_VERSE_REFERENCE_SORT_H
