#ifndef VERDAD_SMART_SEARCH_H
#define VERDAD_SMART_SEARCH_H

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace verdad {
namespace smart_search {

/// Compute Levenshtein edit distance between two strings (case-insensitive).
/// Returns the minimum number of single-character edits needed.
int editDistance(const std::string& a, const std::string& b);

/// Compute Damerau-Levenshtein distance (allows transpositions).
int damerauLevenshteinDistance(const std::string& a, const std::string& b);

/// Score how well `candidate` matches `query` (0.0 = no match, 1.0 = exact).
/// Considers edit distance, prefix matching, and substring containment.
double fuzzyScore(const std::string& query, const std::string& candidate);

/// Expand a query word into a set of synonym alternatives.
/// Returns the original word plus any known synonyms.
/// Language is an ISO code (e.g. "en", "es", "de").
std::vector<std::string> expandSynonyms(const std::string& word,
                                         const std::string& language = "en");

/// Check if a synonym database exists for the given language.
bool hasSynonymDatabase(const std::string& language);

/// Get all supported synonym languages.
std::vector<std::string> supportedSynonymLanguages();

/// Strip diacritical marks from a UTF-8 string, mapping accented characters
/// to their base ASCII equivalents.  Handles Latin-based scripts (covers the
/// languages used in Bible translations: Spanish, Portuguese, French, German).
std::string stripDiacritics(const std::string& text);

/// Generate phonetic key for approximate sound matching (simplified Metaphone).
std::string metaphoneKey(const std::string& word);

/// Generate common misspelling variants of a word for search expansion.
/// Returns candidate forms that are likely typos of the input.
std::vector<std::string> generateTypoVariants(const std::string& word);

/// Build an FTS5 query that covers the original terms plus fuzzy expansions.
/// Each query word is expanded with synonyms and phonetic variants, then
/// combined with OR within each word group and AND across word groups.
std::string buildSmartFtsQuery(const std::string& query,
                                const std::string& language = "en");

/// A match result with scoring metadata for smart search ranking.
struct ScoredMatch {
    int rowIndex = -1;          // Index into result set
    double relevanceScore = 0;  // BM25-like base relevance
    double fuzzyScore = 0;      // Fuzzy match quality (0-1)
    double combinedScore = 0;   // Weighted combination for final sort
    bool exactMatch = false;    // At least one query term matched exactly
    bool synonymMatch = false;  // Match came via synonym expansion
};

/// Score and rank results from a smart search. Takes the original query terms
/// and the text of each result, returning scored entries sorted best-first.
std::vector<ScoredMatch> scoreSmartResults(
    const std::vector<std::string>& queryTerms,
    const std::vector<std::string>& resultTexts,
    const std::string& language = "en");

} // namespace smart_search
} // namespace verdad

#endif // VERDAD_SMART_SEARCH_H
