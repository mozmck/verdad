#ifndef VERDAD_BIBLE_PANE_H
#define VERDAD_BIBLE_PANE_H

#include <FL/Fl_Group.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input_Choice.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Box.H>
#include <memory>
#include <string>
#include <vector>

namespace verdad {

class VerdadApp;
class HtmlWidget;
class BibleBookChoice;

/// Center pane for Bible text display.
/// Single-view (no internal tabs). Context tabs are managed by MainWindow.
class BiblePane : public Fl_Group {
public:
    struct DisplayBuffer {
        std::shared_ptr<void> doc;
        std::string html;
        std::string baseUrl;
        int scrollY = 0;
        int contentHeight = 0;
        int renderWidth = 0;
        bool scrollbarVisible = false;
        bool valid = false;
    };

    BiblePane(VerdadApp* app, int X, int Y, int W, int H);
    ~BiblePane() override;

    /// Navigate to a chapter in the current module
    void navigateTo(const std::string& book, int chapter, int verse = 1);

    /// Navigate using a full reference string (e.g. "Genesis 1:5")
    void navigateToReference(const std::string& reference);

    /// Set the active Bible module
    void setModule(const std::string& moduleName);

    /// Get current module name
    std::string currentModule() const;

    /// Get current book
    std::string currentBook() const;

    /// Get current chapter
    int currentChapter() const;

    /// Get currently selected verse in the chapter
    int currentVerse() const;

    /// Current vertical scroll position of the Bible HTML view.
    int scrollY() const;

    /// Set vertical scroll position of the Bible HTML view.
    void setScrollY(int y);

    /// Toggle parallel Bible view
    void toggleParallel();

    /// Toggle paragraph display mode
    void toggleParagraphMode();

    /// Check if paragraph mode is active
    bool isParagraphMode() const { return paragraphMode_; }

    /// Set modules for parallel view
    void setParallelModules(const std::vector<std::string>& modules);

    /// Get modules currently configured for parallel view
    const std::vector<std::string>& parallelModules() const { return parallelModules_; }

    /// Check if parallel view is active
    bool isParallel() const { return parallelMode_; }

    /// Go to next chapter
    void nextChapter();

    /// Go to previous chapter
    void prevChapter();

    /// Refresh the current view
    void refresh();

    /// Apply runtime HTML style overrides to the Bible view.
    void setHtmlStyleOverride(const std::string& css);

    /// Apply configured list spacing to browser-style controls in the Bible pane.
    void setBrowserLineSpacing(int pixels);

    /// Select a verse in the current chapter
    void selectVerse(int verse);

    /// Apply Bible view state without fetching new text.
    void setStudyState(const std::string& module,
                       const std::string& book,
                       int chapter,
                       int verse,
                       bool paragraphMode,
                       bool parallelMode,
                       const std::vector<std::string>& parallelModules);

    /// Capture current rendered text/scroll for fast restore.
    DisplayBuffer captureDisplayBuffer() const;

    /// Move current rendered text/scroll out of the widget for zero-copy tab switching.
    DisplayBuffer takeDisplayBuffer();

    /// Restore previously captured rendered text/scroll.
    void restoreDisplayBuffer(const DisplayBuffer& buffer);

    /// Restore previously captured rendered text/scroll (move).
    void restoreDisplayBuffer(DisplayBuffer&& buffer);

    /// Redraw toolbar chrome during live layout changes.
    void redrawChrome();

    /// Sync option button state from the application settings.
    void syncOptionButtons();

    /// Update the per-tab history controls in the Bible toolbar.
    void setNavigationHistory(const std::vector<std::string>& labels,
                              int currentIndex,
                              bool canGoBack,
                              bool canGoForward);

protected:
    void resize(int X, int Y, int W, int H) override;

private:
    VerdadApp* app_;

    // Navigation bar
    Fl_Group* navBar_;
    BibleBookChoice* bookChoice_;
    Fl_Choice* chapterChoice_;
    Fl_Choice* moduleChoice_;
    Fl_Button* prevButton_;
    Fl_Button* nextButton_;
    Fl_Box* historyLeftSeparator_;
    Fl_Button* historyBackButton_;
    Fl_Input_Choice* historyChoice_;
    Fl_Button* historyForwardButton_;
    Fl_Box* historyRightSeparator_;
    Fl_Box* moduleRightSeparator_;
    Fl_Button* parallelButton_;
    Fl_Button* paragraphButton_;
    Fl_Button* parallelAddButton_;
    Fl_Button* strongsToggleButton_;
    Fl_Button* morphToggleButton_;
    Fl_Button* footnotesToggleButton_;
    Fl_Button* crossRefsToggleButton_;
    Fl_Box* crossRefsRightSeparator_;
    Fl_Box* navSpacer_;
    Fl_Group* parallelHeader_;
    std::vector<std::string> bibleChoiceModules_;
    std::vector<std::string> bibleChoiceLabels_;
    std::string populatedBookModule_;
    int populatedChapterCount_ = 0;

    // Content
    HtmlWidget* htmlWidget_;
    std::string moduleName_;
    std::string currentBook_;
    int currentChapter_ = 1;
    int currentVerse_ = 1;

    // Parallel mode
    struct ParallelHeaderColumn {
        Fl_Group* group = nullptr;
        Fl_Choice* moduleChoice = nullptr;
        Fl_Button* removeButton = nullptr;
    };

    static constexpr int kMaxParallelColumns = 7;
    bool parallelMode_ = false;
    std::vector<std::string> parallelModules_;
    std::vector<ParallelHeaderColumn> parallelHeaderColumns_;

    // Display mode: false = verse-per-line (default), true = paragraph style
    bool paragraphMode_ = false;

    /// Build the navigation bar
    void buildNavBar();

    /// Update the chapter text display
    void updateDisplay();
    void normalizeParallelModules();
    void syncParallelHeader();
    void clearParallelHeader();
    void layoutParallelHeader();
    void populateParallelChoice(Fl_Choice* choice);
    void applyModuleChoiceValue(Fl_Choice* choice, const std::string& module) const;
    int parallelColumnIndexForWidget(Fl_Widget* w) const;
    void addParallelModule();
    void removeParallelModuleAt(int index);
    void setParallelModuleAt(int index, const std::string& module);

    /// Populate book choices for current module
    void populateBooks(bool force = false);

    /// Populate chapter choices for current book
    void populateChapters(bool force = false);

    /// Keep the reference entry field aligned with the current book/chapter/verse.
    void syncReferenceInput();

    /// Notify main window that module/book/chapter changed (for context tab label sync)
    void notifyContextChanged();

    // Callbacks
    static void onPrev(Fl_Widget* w, void* data);
    static void onNext(Fl_Widget* w, void* data);
    static void onHistoryBack(Fl_Widget* w, void* data);
    static void onHistoryChoice(Fl_Widget* w, void* data);
    static void onHistoryForward(Fl_Widget* w, void* data);
    static void onBookChange(Fl_Widget* w, void* data);
    static void onChapterChange(Fl_Widget* w, void* data);
    static void onModuleChange(Fl_Widget* w, void* data);
    static void onParallel(Fl_Widget* w, void* data);
    static void onParagraphToggle(Fl_Widget* w, void* data);
    static void onStrongsToggle(Fl_Widget* w, void* data);
    static void onMorphToggle(Fl_Widget* w, void* data);
    static void onFootnotesToggle(Fl_Widget* w, void* data);
    static void onCrossRefsToggle(Fl_Widget* w, void* data);
    static void onParallelAdd(Fl_Widget* w, void* data);
    static void onParallelRemove(Fl_Widget* w, void* data);
    static void onParallelModuleChange(Fl_Widget* w, void* data);

    // HTML widget callbacks
    void onLinkClicked(const std::string& url);
    void onWordHover(const std::string& word, const std::string& href,
                     const std::string& strong, const std::string& morph,
                     const std::string& module,
                     int x, int y);
    void onContextMenu(const std::string& word, const std::string& href,
                       const std::string& strong, const std::string& morph,
                       const std::string& module,
                       int x, int y);
};

} // namespace verdad

#endif // VERDAD_BIBLE_PANE_H
