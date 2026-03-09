#ifndef VERDAD_TAG_PANEL_H
#define VERDAD_TAG_PANEL_H

#include <FL/Fl_Group.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Input.H>
#include <string>
#include <vector>

namespace verdad {

class VerdadApp;
class TagVerseBrowser;

/// Panel showing tags and tagged verses in the left pane
class TagPanel : public Fl_Group {
public:
    TagPanel(VerdadApp* app, int X, int Y, int W, int H);
    ~TagPanel() override;

    /// Refresh the tag list
    void refresh();

    /// Show dialog to add a tag to a verse
    void showAddTagDialog(const std::string& verseKey);

    /// Filter the panel to tags containing a specific verse.
    void showTagsForVerse(const std::string& verseKey);
    void resize(int X, int Y, int W, int H) override;

private:
    friend class TagVerseBrowser;

    VerdadApp* app_;

    Fl_Input* filterInput_;
    Fl_Button* clearFilterButton_;

    // Tag list at top
    Fl_Browser* tagBrowser_;

    // Verses for selected tag at bottom
    Fl_Browser* verseBrowser_;

    // Buttons
    Fl_Button* newTagButton_;
    Fl_Button* deleteTagButton_;
    Fl_Button* renameTagButton_;
    Fl_Button* removeVerseButton_;
    std::vector<std::string> visibleTags_;
    std::string selectedTagName_;
    std::string selectedVerseKey_;

    void layoutChildren();
    void refreshBiblePane();
    std::string activeBibleModule() const;
    void updateVersePreview(const std::string& verseKey);
    void activateVerseLine(int line, int mouseButton, bool isDoubleClick);
    void updateFilterControls();

    /// Populate tag list
    void populateTags();

    /// Populate verse list for selected tag
    void populateVerses(const std::string& tagName);

    // Callbacks
    static void onFilterChange(Fl_Widget* w, void* data);
    static void onClearFilter(Fl_Widget* w, void* data);
    static void onTagSelect(Fl_Widget* w, void* data);
    static void onVerseSelect(Fl_Widget* w, void* data);
    static void onNewTag(Fl_Widget* w, void* data);
    static void onDeleteTag(Fl_Widget* w, void* data);
    static void onRenameTag(Fl_Widget* w, void* data);
    static void onRemoveVerse(Fl_Widget* w, void* data);
};

} // namespace verdad

#endif // VERDAD_TAG_PANEL_H
