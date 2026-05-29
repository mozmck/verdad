#ifndef VERDAD_LEFT_PANE_H
#define VERDAD_LEFT_PANE_H

#include <FL/Fl_Group.H>
#include <FL/Fl_Tile.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Button.H>
#include <string>
#include <vector>

namespace verdad {

class VerdadApp;
class ModulePanel;
class SearchPanel;
class TagPanel;
class HtmlWidget;
enum class SearchAssistanceMode;

/// Left pane with search box, tabs (modules/search/tags),
/// and a preview area at the bottom.
class LeftPane : public Fl_Group {
public:
    enum class PreviewKind {
        Plain,
        Verse,
        Commentary,
        GeneralBook,
    };

    LeftPane(VerdadApp* app, int X, int Y, int W, int H);
    ~LeftPane() override;

    /// Execute a search
    void doSearch(const std::string& query,
                  const std::string& moduleOverride = "");

    /// Set selected module for the search tab dropdown.
    void setSearchModule(const std::string& moduleName);

    /// Switch to the search results tab
    void showSearchTab();

    /// Switch to the module list tab
    void showModuleTab();

    /// Switch to the tags tab
    void showTagTab();

    /// Update the preview area with text
    void setPreviewText(const std::string& html,
                        const std::string& sourceModule = "",
                        const std::string& sourceKey = "");

    /// Update the preview area for a tagged commentary/general-book item.
    void setResourcePreviewText(const std::string& html,
                                const std::string& sourceModule,
                                const std::string& sourceKey,
                                PreviewKind kind);

    /// Update the preview area with a verse preview and a clickable reference header.
    void setVersePreviewText(const std::string& html,
                             const std::string& verseModule,
                             const std::string& verseKey);

    /// Show a set of linked verse references in the Search tab.
    void showReferenceResults(const std::string& moduleName,
                              const std::vector<std::string>& references,
                              const std::string& statusSuffix = "");

    /// Apply runtime HTML style overrides to the preview widget.
    void setHtmlStyleOverride(const std::string& css);

    /// Set extra line spacing for search results and tagged-verse lists.
    void setBrowserLineSpacing(int pixels);

    /// Set the persisted search assistance mode selector.
    void setSearchAssistanceMode(SearchAssistanceMode mode);

    /// Current preview widget height in pixels.
    int previewHeight() const;

    /// Set preview widget height in pixels (clamped to valid splitter range).
    void setPreviewHeight(int height);

    /// Redraw top chrome (search + tabs header) during live layout changes.
    void redrawChrome();

    /// Refresh all tabs
    void refresh();

    /// Get module panel
    ModulePanel* modulePanel() { return modulePanel_; }

    /// Get search panel
    SearchPanel* searchPanel() { return searchPanel_; }

    /// Get tag panel
    TagPanel* tagPanel() { return tagPanel_; }

protected:
    void resize(int X, int Y, int W, int H) override;

private:
    VerdadApp* app_;

    // Search box (always visible at top)
    Fl_Group* searchGroup_;
    Fl_Input* searchInput_;
    Fl_Button* searchButton_;
    Fl_Group* searchModeGroup_;
    Fl_Button* exactSearchModeButton_;
    Fl_Button* spellingSearchModeButton_;
    Fl_Button* synonymsSearchModeButton_;
    Fl_Button* smartSearchModeButton_;

    // Splitter area (tabs + preview)
    Fl_Tile* contentTile_;
    Fl_Box* contentResizeBox_;

    // Tabbed area
    Fl_Tabs* tabs_;
    ModulePanel* modulePanel_;
    SearchPanel* searchPanel_;
    TagPanel* tagPanel_;

    // Preview area at bottom
    HtmlWidget* previewWidget_;
    std::string previewSourceModule_;
    std::string previewSourceKey_;
    PreviewKind previewKind_ = PreviewKind::Plain;

    /// Show only the active tab panel to avoid cross-tab redraw artifacts.
    void syncTabPanelVisibility();
    void syncSearchAssistanceButtons();

    void onPreviewLink(const std::string& url);

    // Callbacks
    static void onSearch(Fl_Widget* w, void* data);
    static void onSearchInput(Fl_Widget* w, void* data);
    static void onSearchMode(Fl_Widget* w, void* data);
    static void onTabChanged(Fl_Widget* w, void* data);
};

} // namespace verdad

#endif // VERDAD_LEFT_PANE_H
