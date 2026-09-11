#ifndef VERDAD_SMART_SEARCH_H
#define VERDAD_SMART_SEARCH_H

#include <cstdint>
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

struct QueryExpansionOptions {
    bool includeSpelling = true;
    bool includeSynonyms = true;
    bool includePartialWords = true;
};

enum class RetrievalSource {
    ExactPhrase,
    OriginalTerms,
    NaveExactTopic,
    Semantic,
    Synonyms,
    Spelling,
    NaveExpandedTopic,
    AutomaticPrefix,
    TskCorroboration
};

struct SmartQuerySource {
    RetrievalSource source = RetrievalSource::OriginalTerms;
    std::string ftsQuery;
    double weight = 1.0;
};

/// A decomposed Smart query.  Each source is retrieved and ranked separately,
/// so an automatic prefix or spelling correction cannot outrank the words the
/// user actually entered merely because it has a better FTS score.
struct SmartQueryPlan {
    std::vector<std::string> originalTerms;
    std::vector<std::string> coreTerms;
    std::vector<std::string> softTerms;
    std::vector<std::string> quotedTerms;
    std::vector<std::string> excludedTerms;
    std::vector<SmartQuerySource> lexicalSources;
};

struct WeightedRankList {
    RetrievalSource source = RetrievalSource::OriginalTerms;
    double weight = 1.0;
    std::vector<std::string> resultIds;
    size_t originalTermCoverage = 0;
};

struct RankedCandidate {
    std::string id;
    double fusedScore = 0.0;
    size_t originalTermCoverage = 0;
    size_t bestLexicalRank = static_cast<size_t>(-1);
    size_t semanticRank = static_cast<size_t>(-1);
    std::int64_t canonicalOrder = INT64_MAX;
};

/// Build independently ranked lexical retrieval sources for Smart search.
SmartQueryPlan buildSmartQueryPlan(
    const std::string& query,
    const std::string& language = "en",
    const std::unordered_map<std::string, std::vector<std::string>>& spellingAlternatives = {},
    QueryExpansionOptions options = {});

/// Weighted reciprocal-rank fusion.  Returned IDs are ordered best first;
/// ties are resolved by best source rank and then stable lexical ID order.
std::vector<std::string> fuseRankedResults(
    const std::vector<WeightedRankList>& rankedLists,
    size_t maxResults = 0,
    double rankConstant = 60.0,
    const std::unordered_map<std::string, std::int64_t>& canonicalOrders = {});

/// Split search input, preserving quoted phrases and a trailing '*' as an
/// explicit word prefix. Unary '-' exclusions are intentionally omitted from
/// this positive-term view; SmartQueryPlan records them separately.
/// With synonyms enabled, recognize known multi-word expressions as phrases.
std::vector<std::string> queryTerms(const std::string& query,
                                  const std::string& language = "en",
                                  bool includeSynonyms = false);

/// Build an FTS5 query using synonyms and vocabulary-validated corrections.
/// Each query term is expanded with enabled alternatives, then
/// combined with OR within each word group and AND across word groups.
std::string buildSmartFtsQuery(const std::string& query,
                               const std::string& language = "en",
                               const std::unordered_map<std::string, std::vector<std::string>>& spellingAlternatives = {},
                               QueryExpansionOptions options = {});

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
