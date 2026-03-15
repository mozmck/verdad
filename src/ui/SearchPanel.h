#ifndef VERDAD_SEARCH_PANEL_H
#define VERDAD_SEARCH_PANEL_H

#include <FL/Fl_Group.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Progress.H>
#include <atomic>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>
#include "search/SearchIndexer.h"
#include "sword/SwordManager.h"

namespace verdad {

class VerdadApp;
class SearchResultBrowser;

/// Panel for displaying search results within the left pane tabs
class SearchPanel : public Fl_Group {
public:
    SearchPanel(VerdadApp* app, int X, int Y, int W, int H);
    ~SearchPanel() override;

    /// Refresh module/filter choices after library changes.
    void refresh();

    /// Execute a search and display results
    void search(const std::string& query,
                const std::string& moduleOverride = "");

    /// Show a supplied set of verse references in the results browser.
    void showReferenceResults(const std::string& moduleName,
                              const std::vector<std::string>& references,
                              const std::string& statusSuffix = "");

    /// Set the selected module in the search dropdown (if present).
    void setSelectedModule(const std::string& moduleName);

    /// Clear results
    void clear();

    /// Get the currently selected result
    const SearchResult* selectedResult() const;

    /// Get number of results
    int resultCount() const { return static_cast<int>(results_.size()); }

    /// Set extra line spacing between search result rows, in pixels.
    void setResultLineSpacing(int pixels);

private:
    friend class SearchResultBrowser;

    enum class SearchDomain {
        BibleOnly,
        LibraryOnly,
        AllResources
    };

    enum class ResultSort {
        Relevance,
        Canonical,
        Module
    };

    enum class HighlightMode {
        None,
        Terms,
        Phrase,
        Regex,
        Strongs
    };

    VerdadApp* app_;

    struct AsyncSearchState {
        bool active = false;
        bool completed = false;
        bool cancelled = false;
        bool usedIndexer = false;
        bool indexingPending = false;
        bool fallbackDeferred = false;
        std::string moduleName;
        int scanned = 0;
        int total = 0;
        int matches = 0;
        std::vector<SearchResult> results;
    };

    // Module to search in
    Fl_Choice* targetChoice_;
    Fl_Choice* moduleChoice_;
    Fl_Choice* resourceTypeChoice_;
    Fl_Choice* bibleScopeChoice_;
    std::vector<std::string> moduleChoiceModules_;
    std::vector<std::string> moduleChoiceLabels_;
    std::vector<std::string> resourceTypeChoiceTokens_;
    std::vector<std::string> resourceTypeChoiceLabels_;
    std::vector<ModuleInfo> searchableModules_;

    // Search type selector
    Fl_Choice* searchType_;
    Fl_Choice* sortChoice_;

    // Result status line
    Fl_Box* resultStatus_;
    Fl_Progress* searchProgress_;

    // Result list
    Fl_Browser* resultBrowser_;

    // Stored results
    std::vector<SearchResult> results_;
    std::vector<std::string> resultDisplayKeys_;
    std::vector<int> resultLineWidths_;
    int resultRefColumnWidth_ = 100;

    // Indexing indicator state
    bool indexingIndicatorActive_ = false;
    std::string indexingModule_;
    bool swordSearchInProgress_ = false;
    std::string statusSuffix_;
    std::string currentResultLabel_;
    bool previewUpdateScheduled_ = false;
    std::string pendingPreviewModule_;
    std::string pendingPreviewKey_;
    std::string pendingPreviewResourceType_;
    std::string pendingPreviewTitle_;
    std::string lastPreviewModule_;
    std::string lastPreviewKey_;
    HighlightMode highlightMode_ = HighlightMode::None;
    std::vector<std::string> highlightTerms_;
    std::vector<std::string> highlightStrongs_;
    std::string highlightPhrase_;
    std::regex highlightRegex_;
    bool highlightRegexValid_ = false;
    std::thread searchThread_;
    std::mutex asyncSearchMutex_;
    std::atomic<bool> cancelAsyncSearch_{false};
    AsyncSearchState asyncSearchState_;
    int statusResultCountOverride_ = -1;

    /// Populate module choices
    void populateModules();
    void populateResourceTypes();
    void populateBibleScopes();
    void updateFilterControls();
    SearchDomain searchDomain() const;
    ResultSort resultSort() const;
    std::vector<std::string> effectiveResourceTypes() const;
    std::string effectiveModuleSelection(const std::string& moduleOverride = "") const;
    std::string currentBibleScopeBook() const;
    bool bibleScopeActive() const;
    bool requestCanUseStrongs(const std::vector<std::string>& resourceTypes) const;
    bool resultIsBibleLike(const SearchResult& result) const;
    std::string resultLocationLabel(const SearchResult& result) const;
    std::string resultDisplayLabel(const SearchResult& result) const;
    std::string resultSummarySuffix() const;
    void sortResultsForDisplay(const std::string& canonicalModuleHint);
    void setResultCountLabel(const std::string& suffix = "");
    void resetResultView();
    void finalizeSearchResults(const std::string& moduleName,
                               bool usedIndexer,
                               bool indexingPending,
                               bool fallbackDeferred);
    void cancelActiveSearch();
    void startAsyncRegexSearch(const SearchIndexer::SearchRequest& request,
                               const std::string& moduleName,
                               const std::string& query,
                               bool indexingPending,
                               SearchIndexer* indexer);
    bool updateSearchProgressUi();
    void applyCompletedAsyncSearch();
    void startIndexingIndicator(const std::string& moduleName);
    void stopIndexingIndicator();
    void updateIndexingIndicator();
    bool isSearchTabActive() const;
    void cancelPendingPreviewUpdate();
    void schedulePreviewUpdate(const SearchResult& result);
    void applyPendingPreviewUpdate();
    void activateResultLine(int line, int mouseButton, bool isDoubleClick);
    void showVerseListContextMenu(int screenX, int screenY);
    void resetHighlightState();
    std::string applyPreviewHighlights(const std::string& html) const;
    void rebuildResultMetrics();
    void rebuildResultBrowserItems();
    int resultLineWidth(int line) const;

    // Callbacks
    static void onResultSelect(Fl_Widget* w, void* data);
    static void onResultDoubleClick(Fl_Widget* w, void* data);
    static void onIndexingPoll(void* data);
    static void onDeferredPreviewUpdate(void* data);
    static void onFilterChoiceChanged(Fl_Widget* w, void* data);
    static void onSortChoiceChanged(Fl_Widget* w, void* data);
};

} // namespace verdad

#endif // VERDAD_SEARCH_PANEL_H
