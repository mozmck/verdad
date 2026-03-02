#ifndef VERDAD_SEARCH_PANEL_H
#define VERDAD_SEARCH_PANEL_H

#include <FL/Fl_Group.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Check_Button.H>
#include <string>
#include <vector>
#include "sword/SwordManager.h"

namespace verdad {

class VerdadApp;

/// Panel for displaying search results within the left pane tabs
class SearchPanel : public Fl_Group {
public:
    SearchPanel(VerdadApp* app, int X, int Y, int W, int H);
    ~SearchPanel() override;

    /// Execute a search and display results
    void search(const std::string& query,
                const std::string& moduleOverride = "");

    /// Set the selected module in the search dropdown (if present).
    void setSelectedModule(const std::string& moduleName);

    /// Clear results
    void clear();

    /// Get the currently selected result
    const SearchResult* selectedResult() const;

    /// Get number of results
    int resultCount() const { return static_cast<int>(results_.size()); }

private:
    VerdadApp* app_;

    // Module to search in
    Fl_Choice* moduleChoice_;

    // Search type selector
    Fl_Choice* searchType_;

    // Result list
    Fl_Browser* resultBrowser_;

    // Stored results
    std::vector<SearchResult> results_;

    // Indexing indicator state
    bool indexingIndicatorActive_ = false;
    std::string indexingModule_;
    bool swordSearchInProgress_ = false;
    std::string statusSuffix_;

    /// Populate module choices
    void populateModules();
    void setResultCountLabel(const std::string& suffix = "");
    void startIndexingIndicator(const std::string& moduleName);
    void stopIndexingIndicator();
    void updateIndexingIndicator();
    bool isSearchTabActive() const;

    // Callbacks
    static void onResultSelect(Fl_Widget* w, void* data);
    static void onResultDoubleClick(Fl_Widget* w, void* data);
    static void onIndexingPoll(void* data);
};

} // namespace verdad

#endif // VERDAD_SEARCH_PANEL_H
