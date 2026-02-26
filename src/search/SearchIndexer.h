#ifndef VERDAD_SEARCH_INDEXER_H
#define VERDAD_SEARCH_INDEXER_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "sword/SwordManager.h"

struct sqlite3;

namespace verdad {

/// SQLite FTS5-backed search index for Bible modules.
/// The database only stores module index data (no tags/settings data).
class SearchIndexer {
public:
    explicit SearchIndexer(const std::string& dbPath);
    ~SearchIndexer();

    SearchIndexer(const SearchIndexer&) = delete;
    SearchIndexer& operator=(const SearchIndexer&) = delete;

    /// Queue one module for background indexing.
    void queueModuleIndex(const std::string& moduleName);

    /// Queue multiple modules for background indexing.
    void queueModuleIndex(const std::vector<std::string>& moduleNames);

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

    /// Search plain verse text (multi-word or exact phrase).
    std::vector<SearchResult> searchWord(const std::string& moduleName,
                                         const std::string& query,
                                         bool exactPhrase = false,
                                         int maxResults = 500) const;

    /// Search for Strong's/Lemma references in indexed XHTML.
    std::vector<SearchResult> searchStrongs(const std::string& moduleName,
                                            const std::string& strongsQuery,
                                            int maxResults = 500) const;

private:
    void workerLoop();
    void indexModuleNow(const std::string& moduleName);

    static std::string buildWordFtsQuery(const std::string& query, bool exactPhrase);
    static std::string buildStrongsFtsQuery(const std::string& query);

    static void applyPragmas(sqlite3* db);
    static bool ensureSchema(sqlite3* db);

    std::string dbPath_;
    sqlite3* db_ = nullptr;

    mutable std::mutex dbMutex_;

    std::thread workerThread_;
    mutable std::mutex workerMutex_;
    std::condition_variable workerCv_;
    std::deque<std::string> pendingModules_;
    std::unordered_set<std::string> pendingSet_;

    std::atomic<bool> indexing_{false};
    std::atomic<bool> stopRequested_{false};
    bool stopWorker_ = false;

    mutable std::mutex statusMutex_;
    std::string activeModule_;
    int activeProgress_ = 0;
};

} // namespace verdad

#endif // VERDAD_SEARCH_INDEXER_H
