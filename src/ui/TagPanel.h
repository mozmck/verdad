#ifndef VERDAD_TAG_PANEL_H
#define VERDAD_TAG_PANEL_H

#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Tree.H>

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "tags/TagManager.h"

namespace verdad {

class VerdadApp;
class TagFilterInput;
class TagItemBrowser;
class TagTree;

/// Panel showing the tag tree and tagged verses/resources in the left pane.
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

    /// Refresh the tag tree
    void refresh();

    /// Show dialog to add a tag to a verse or verse range.
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
    friend class TagTree;

    VerdadApp* app_;

    Fl_Input* filterInput_;
    Fl_Button* clearFilterButton_;
    Fl_Choice* resourceFilterChoice_;

    TagTree* tagTree_;
    Fl_Browser* itemBrowser_;

    Fl_Button* newTagButton_;
    Fl_Button* renameTagButton_;
    Fl_Button* moveTagButton_;
    Fl_Button* deleteTagButton_;
    Fl_Check_Button* includeSubtagsCheck_;
    Fl_Button* removeTagButton_;

    std::unordered_map<const Fl_Tree_Item*, std::string> itemTagNames_;
    std::set<std::string> expandedTags_;   // user-expanded nodes while unfiltered
    bool populatingTree_ = false;
    bool filterActive_ = false;

    std::vector<TagTarget> visibleTargets_;
    std::vector<std::string> visibleTargetTags_;  // tag each visible item came from
    std::string selectedTagName_;
    TagTarget selectedTarget_;
    bool hasSelectedTarget_ = false;
    bool filterTargetsByText_ = true;
    bool includeSubtags_ = false;
    ResourceFilter selectedResourceFilter_ = ResourceFilter::All;

    void layoutChildren();
    void refreshPreviewForSelection();
    std::string activeBibleModule() const;
    void updateTargetPreview(const TagTarget& target);
    void showItemContextMenu(int screenX, int screenY);
    void showTagContextMenu(int screenX, int screenY, bool onItem);
    void applyResourceFilterFromChoice();
    void updateFilterControls();
    void clearFilter(bool focusInput);
    void tagsChanged(bool refreshBible);
    void expandAncestors(const std::string& tagName);
    std::string treeLabelForTag(const std::string& tagName) const;
    std::string tagNameForItem(const Fl_Tree_Item* item) const;

    void populateTags();
    void populateTargets(const std::string& tagName);

    void createTagUnder(const std::string& parentName);
    void renameSelectedTag();
    void moveSelectedTag();
    void deleteSelectedTag();
    void setSelectedTagColor();
    void editSelectedVerseRange();
    void setAllExpanded(bool expanded);

    static void onFilterChange(Fl_Widget* w, void* data);
    static void onClearFilter(Fl_Widget* w, void* data);
    static void onResourceFilterChange(Fl_Widget* w, void* data);
    static void onTreeEvent(Fl_Widget* w, void* data);
    static void onItemSelect(Fl_Widget* w, void* data);
    static void onNewTag(Fl_Widget* w, void* data);
    static void onDeleteTag(Fl_Widget* w, void* data);
    static void onRenameTag(Fl_Widget* w, void* data);
    static void onMoveTag(Fl_Widget* w, void* data);
    static void onIncludeSubtags(Fl_Widget* w, void* data);
    static void onRemoveTag(Fl_Widget* w, void* data);
};

} // namespace verdad

#endif // VERDAD_TAG_PANEL_H
