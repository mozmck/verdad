#ifndef VERDAD_SEARCH_PANEL_H
#define VERDAD_SEARCH_PANEL_H

#include <FL/Fl_Group.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Check_Button.H>
#include <regex>
#include <string>
#include <vector>
#include "sword/SwordManager.h"

namespace verdad {

class VerdadApp;
class SearchResultBrowser;

/// Panel for displaying search results within the left pane tabs
class SearchPanel : public Fl_Group {
public:
    SearchPanel(VerdadApp* app, int X, int Y, int W, int H);
    ~SearchPanel() override;

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

private:
    friend class SearchResultBrowser;

    enum class HighlightMode {
        None,
        Terms,
        Phrase,
        Regex,
        Strongs
    };

    VerdadApp* app_;

    // Module to search in
    Fl_Choice* moduleChoice_;

    // Search type selector
    Fl_Choice* searchType_;

    // Result status line
    Fl_Box* resultStatus_;

    // Result list
    Fl_Browser* resultBrowser_;

    // Stored results
    std::vector<SearchResult> results_;
    std::vector<std::string> resultDisplayKeys_;

    // Indexing indicator state
    bool indexingIndicatorActive_ = false;
    std::string indexingModule_;
    bool swordSearchInProgress_ = false;
    std::string statusSuffix_;
    std::string currentResultLabel_;
    bool previewUpdateScheduled_ = false;
    std::string pendingPreviewModule_;
    std::string pendingPreviewKey_;
    std::string lastPreviewModule_;
    std::string lastPreviewKey_;
    HighlightMode highlightMode_ = HighlightMode::None;
    std::vector<std::string> highlightTerms_;
    std::vector<std::string> highlightStrongs_;
    std::string highlightPhrase_;
    std::regex highlightRegex_;
    bool highlightRegexValid_ = false;

    /// Populate module choices
    void populateModules();
    void setResultCountLabel(const std::string& suffix = "");
    void startIndexingIndicator(const std::string& moduleName);
    void stopIndexingIndicator();
    void updateIndexingIndicator();
    bool isSearchTabActive() const;
    void cancelPendingPreviewUpdate();
    void schedulePreviewUpdate(const SearchResult& result);
    void applyPendingPreviewUpdate();
    void activateResultLine(int line, int mouseButton, bool isDoubleClick);
    void resetHighlightState();
    std::string applyPreviewHighlights(const std::string& html) const;

    // Callbacks
    static void onResultSelect(Fl_Widget* w, void* data);
    static void onResultDoubleClick(Fl_Widget* w, void* data);
    static void onIndexingPoll(void* data);
    static void onDeferredPreviewUpdate(void* data);
};

} // namespace verdad

#endif // VERDAD_SEARCH_PANEL_H
