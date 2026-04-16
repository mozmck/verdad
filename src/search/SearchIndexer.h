#ifndef VERDAD_SEARCH_INDEXER_H
#define VERDAD_SEARCH_INDEXER_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "search/SmartSearch.h"
#include "sword/SwordManager.h"

struct sqlite3;

namespace verdad {

class ImportedModuleManager;

/// SQLite FTS5-backed search index for searchable SWORD modules.
/// The database only stores module index data (no tags/settings data).
class SearchIndexer {
public:
    class ScopedSuspend {
    public:
        ScopedSuspend() = default;
        ~ScopedSuspend();

        ScopedSuspend(const ScopedSuspend&) = delete;
        ScopedSuspend& operator=(const ScopedSuspend&) = delete;

        ScopedSuspend(ScopedSuspend&& other) noexcept;
        ScopedSuspend& operator=(ScopedSuspend&& other) noexcept;

    private:
        friend class SearchIndexer;

        explicit ScopedSuspend(SearchIndexer* owner) : owner_(owner) {}
        void release();

        SearchIndexer* owner_ = nullptr;
    };

    struct SearchRequest {
        enum class BibleScope {
            All,
            OldTestament,
            NewTestament,
            CurrentBook,
        };

        std::vector<std::string> resourceTypes;
        std::string moduleName;
        BibleScope bibleScope = BibleScope::All;
        std::string currentBook;
        int maxResults = 0;
    };

    struct RegexSearchProgress {
        int scanned = 0;
        int total = 0;
        int matches = 0;
    };

    using RegexProgressCallback = std::function<bool(const RegexSearchProgress&)>;

    explicit SearchIndexer(const std::string& dbPath,
                           const ImportedModuleManager* importedModuleMgr = nullptr);
    ~SearchIndexer();

    SearchIndexer(const SearchIndexer&) = delete;
    SearchIndexer& operator=(const SearchIndexer&) = delete;

    /// Queue one module for background indexing.
    void queueModuleIndex(const std::string& moduleName, bool force = false);

    /// Queue multiple modules for background indexing.
    void queueModuleIndex(const std::vector<std::string>& moduleNames);

    /// Refresh cached searchable-module metadata and prune removed modules.
    void synchronizeModules(const std::vector<ModuleInfo>& modules);

    /// True if module has a completed index entry.
    bool isModuleIndexed(const std::string& moduleName) const;

    /// True while a background indexing job is currently running.
    bool isIndexing() const { return indexing_.load(); }

    /// Get indexing progress (0-100) for a module.
    /// Returns 100 when indexed, 0 when queued/not started, and -1 when unknown.
    int moduleIndexProgress(const std::string& moduleName) const;

    /// Get currently active indexing task.
    /// Returns true if a module is actively indexing.
    bool activeIndexingTask(std::string& moduleName, int& percent) const;

    /// Return the last indexing error for a module, if any.
    std::string moduleIndexError(const std::string& moduleName) const;

    /// Pause background indexing until the returned guard is destroyed.
    [[nodiscard]] ScopedSuspend suspendBackgroundIndexing();

    /// Search plain indexed text (multi-word or exact phrase).
    /// maxResults <= 0 means no result limit.
    std::vector<SearchResult> searchWord(const SearchRequest& request,
                                         const std::string& query,
                                         bool exactPhrase = false,
                                         int maxResults = 0) const;

    /// Search for Strong's/Lemma references in indexed XHTML.
    /// maxResults <= 0 means no result limit.
    std::vector<SearchResult> searchStrongs(const SearchRequest& request,
                                            const std::string& strongsQuery,
                                            int maxResults = 0) const;

    /// Search indexed plain text using a regex pattern.
    /// maxResults <= 0 means no result limit.
    std::vector<SearchResult> searchRegex(const SearchRequest& request,
                                          const std::string& pattern,
                                          bool caseSensitive = false,
                                          int maxResults = 0,
                                          RegexProgressCallback progressCallback = {}) const;

    /// Smart/fuzzy search: expands query with synonyms, phonetic variants,
    /// and edit-distance matching.  Results are ranked by combined relevance.
    /// `language` is an ISO 639-1 code (e.g. "en") for synonym expansion.
    std::vector<SearchResult> searchSmart(const SearchRequest& request,
                                          const std::string& query,
                                          const std::string& language = "en",
                                          int maxResults = 0) const;

private:
    struct IndexTask {
        std::string moduleName;
        bool force = false;
    };

    struct ModuleCatalogEntry {
        ModuleInfo info;
        std::string resourceType;
        std::string moduleToken;
        std::string signature;
    };

    void workerLoop();
    void indexModuleNow(const std::string& moduleName);
    void resumeBackgroundIndexing();
    void waitForWorkerIdle();
    bool openOrRebuildDatabase();

    static std::string buildFilterFtsQuery(const SearchRequest& request);
    static std::string buildWordFtsQuery(const SearchRequest& request,
                                         const std::string& query,
                                         bool exactPhrase);
    static std::string buildStrongsFtsQuery(const SearchRequest& request,
                                            const std::string& query);

    static void applyPragmas(sqlite3* db);
    static bool ensureSchema(sqlite3* db);

    std::string dbPath_;
    sqlite3* db_ = nullptr;
    const ImportedModuleManager* importedModuleMgr_ = nullptr;

    mutable std::mutex dbMutex_;

    std::thread workerThread_;
    mutable std::mutex workerMutex_;
    std::condition_variable workerCv_;
    std::condition_variable workerIdleCv_;
    std::deque<IndexTask> pendingModules_;
    std::unordered_map<std::string, bool> pendingForces_;
    int suspendDepth_ = 0;
    bool workerTaskRunning_ = false;

    std::atomic<bool> indexing_{false};
    std::atomic<bool> stopRequested_{false};
    bool stopWorker_ = false;

    mutable std::mutex catalogMutex_;
    std::unordered_map<std::string, ModuleCatalogEntry> moduleCatalog_;

    mutable std::mutex statusMutex_;
    std::string activeModule_;
    int activeProgress_ = 0;
};

} // namespace verdad

#endif // VERDAD_SEARCH_INDEXER_H
