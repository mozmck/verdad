#ifndef VERDAD_BIBLE_PANE_H
#define VERDAD_BIBLE_PANE_H

#include <FL/Fl_Group.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Button.H>
#include <string>
#include <vector>

namespace verdad {

class VerdadApp;
class HtmlWidget;

/// A single Bible view tab
struct BibleTab {
    std::string moduleName;
    std::string currentBook;
    int currentChapter = 1;
    Fl_Group* tabGroup = nullptr;
    HtmlWidget* htmlWidget = nullptr;
};

/// Center pane for Bible text display.
/// Supports tabbed browsing and parallel Bible view.
class BiblePane : public Fl_Group {
public:
    BiblePane(VerdadApp* app, int X, int Y, int W, int H);
    ~BiblePane() override;

    /// Navigate to a chapter in the current tab
    void navigateTo(const std::string& book, int chapter);

    /// Navigate using a full reference string (e.g. "Genesis 1:5")
    void navigateToReference(const std::string& reference);

    /// Set the active Bible module for the current tab
    void setModule(const std::string& moduleName);

    /// Add a new Bible tab
    void addTab(const std::string& moduleName);

    /// Close the current tab
    void closeCurrentTab();

    /// Get current module name
    std::string currentModule() const;

    /// Get current book
    std::string currentBook() const;

    /// Get current chapter
    int currentChapter() const;

    /// Toggle parallel Bible view
    void toggleParallel();

    /// Toggle paragraph display mode
    void toggleParagraphMode();

    /// Check if paragraph mode is active
    bool isParagraphMode() const { return paragraphMode_; }

    /// Set modules for parallel view
    void setParallelModules(const std::vector<std::string>& modules);

    /// Check if parallel view is active
    bool isParallel() const { return parallelMode_; }

    /// Go to next chapter
    void nextChapter();

    /// Go to previous chapter
    void prevChapter();

    /// Refresh the current view
    void refresh();

private:
    VerdadApp* app_;

    // Navigation bar
    Fl_Group* navBar_;
    Fl_Choice* bookChoice_;
    Fl_Choice* chapterChoice_;
    Fl_Choice* moduleChoice_;
    Fl_Input* refInput_;
    Fl_Button* goButton_;
    Fl_Button* prevButton_;
    Fl_Button* nextButton_;
    Fl_Button* parallelButton_;
    Fl_Button* paragraphButton_;
    Fl_Button* addTabButton_;

    // Tab group for multiple Bible views
    Fl_Tabs* tabs_;
    std::vector<BibleTab> bibleTabs_;
    int activeTabIndex_ = 0;

    // Parallel mode
    bool parallelMode_ = false;
    std::vector<std::string> parallelModules_;

    // Display mode: false = verse-per-line (default), true = paragraph style
    bool paragraphMode_ = false;

    /// Build the navigation bar
    void buildNavBar();

    /// Update the chapter text display
    void updateDisplay();

    /// Populate book choices for current module
    void populateBooks();

    /// Populate chapter choices for current book
    void populateChapters();

    /// Get the currently active tab
    BibleTab* activeTab();
    const BibleTab* activeTab() const;

    // Callbacks
    static void onGo(Fl_Widget* w, void* data);
    static void onPrev(Fl_Widget* w, void* data);
    static void onNext(Fl_Widget* w, void* data);
    static void onBookChange(Fl_Widget* w, void* data);
    static void onChapterChange(Fl_Widget* w, void* data);
    static void onModuleChange(Fl_Widget* w, void* data);
    static void onParallel(Fl_Widget* w, void* data);
    static void onParagraphToggle(Fl_Widget* w, void* data);
    static void onAddTab(Fl_Widget* w, void* data);
    static void onTabChange(Fl_Widget* w, void* data);

    // HTML widget callbacks
    void onLinkClicked(const std::string& url);
    void onWordHover(const std::string& word, const std::string& href,
                     const std::string& strong, const std::string& morph,
                     int x, int y);
    void onContextMenu(const std::string& word, const std::string& href,
                       int x, int y);
};

} // namespace verdad

#endif // VERDAD_BIBLE_PANE_H
