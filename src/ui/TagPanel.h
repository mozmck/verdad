#ifndef VERDAD_TAG_PANEL_H
#define VERDAD_TAG_PANEL_H

#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>

#include <string>
#include <vector>

#include "tags/TagManager.h"

namespace verdad {

class VerdadApp;
class TagFilterInput;
class TagItemBrowser;

/// Panel showing tags and tagged verses/resources in the left pane.
class TagPanel : public Fl_Group {
public:
    enum class ResourceFilter {
        All,
        Verse,
        Commentary,
        GeneralBook,
    };

    TagPanel(VerdadApp* app, int X, int Y, int W, int H);
    ~TagPanel() override;

    /// Refresh the tag list
    void refresh();

    /// Show dialog to add a tag to a verse.
    void showAddTagDialog(const std::string& verseKey);

    /// Show dialog to add a tag to any tagged item.
    void showAddTagDialog(const TagTarget& target);

    /// Filter the panel to tags containing a specific verse.
    void showTagsForVerse(const std::string& verseKey);

    void resize(int X, int Y, int W, int H) override;

    /// Set extra line spacing between tagged-item rows, in pixels.
    void setVerseListLineSpacing(int pixels);

private:
    friend class TagFilterInput;
    friend class TagItemBrowser;

    VerdadApp* app_;

    Fl_Input* filterInput_;
    Fl_Button* clearFilterButton_;
    Fl_Choice* resourceFilterChoice_;

    Fl_Browser* tagBrowser_;
    Fl_Browser* itemBrowser_;

    Fl_Button* newTagButton_;
    Fl_Button* deleteTagButton_;
    Fl_Button* renameTagButton_;
    Fl_Button* removeTagButton_;

    std::vector<std::string> visibleTags_;
    std::vector<TagTarget> visibleTargets_;
    std::string selectedTagName_;
    TagTarget selectedTarget_;
    bool hasSelectedTarget_ = false;
    bool filterTargetsByText_ = true;
    ResourceFilter selectedResourceFilter_ = ResourceFilter::All;

    void layoutChildren();
    void refreshPreviewForSelection();
    std::string activeBibleModule() const;
    void updateTargetPreview(const TagTarget& target);
    void activateTargetLine(int line, int mouseButton, bool isDoubleClick);
    void showItemContextMenu(int screenX, int screenY);
    void applyResourceFilterFromChoice();
    void updateFilterControls();
    void clearFilter(bool focusInput);

    void populateTags();
    void populateTargets(const std::string& tagName);
    bool targetMatchesResourceFilter(const TagTarget& target) const;
    bool tagMatchesResourceFilter(const std::string& tagName) const;

    static void onFilterChange(Fl_Widget* w, void* data);
    static void onClearFilter(Fl_Widget* w, void* data);
    static void onResourceFilterChange(Fl_Widget* w, void* data);
    static void onTagSelect(Fl_Widget* w, void* data);
    static void onItemSelect(Fl_Widget* w, void* data);
    static void onNewTag(Fl_Widget* w, void* data);
    static void onDeleteTag(Fl_Widget* w, void* data);
    static void onRenameTag(Fl_Widget* w, void* data);
    static void onRemoveTag(Fl_Widget* w, void* data);
};

} // namespace verdad

#endif // VERDAD_TAG_PANEL_H
