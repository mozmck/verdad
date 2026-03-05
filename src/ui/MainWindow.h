#ifndef VERDAD_MAIN_WINDOW_H
#define VERDAD_MAIN_WINDOW_H

#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Tile.H>
#include <FL/Fl_Tabs.H>
#include <memory>
#include <string>
#include <vector>
#include <chrono>

class Fl_Box;
class Fl_Text_Buffer;

namespace verdad {

class VerdadApp;
class LeftPane;
class BiblePane;
class RightPane;

/// Main application window with left pane + tabbed Bible/Commentary workspaces.
class MainWindow : public Fl_Double_Window {
public:
    struct StudyTabState {
        std::string module;
        std::string book;
        int chapter = 1;
        int verse = 1;
        bool paragraphMode = false;
        bool parallelMode = false;
        std::vector<std::string> parallelModules;
        int biblePaneWidth = 0;
        int bibleScrollY = -1;

        std::string commentaryModule;
        std::string commentaryReference;
        int commentaryScrollY = -1;
        std::string dictionaryModule;
        std::string dictionaryKey;
        std::string generalBookModule;
        std::string generalBookKey;
        bool dictionaryActive = false;
        int dictionaryPaneHeight = 0;
    };

    struct SessionState {
        int windowX = -1;
        int windowY = -1;
        int windowW = 1200;
        int windowH = 800;
        int leftPaneWidth = 300;
        int leftPanePreviewHeight = 150;
        int activeStudyTab = 0;
        std::vector<StudyTabState> studyTabs;
    };

    MainWindow(VerdadApp* app, int W, int H, const char* title);
    ~MainWindow() override;

    /// Navigate to a verse reference in the active workspace tab
    void navigateTo(const std::string& reference);

    /// Navigate to a verse in a specific module in the active workspace tab
    void navigateTo(const std::string& module, const std::string& reference);

    /// Open a verse in a new study tab and activate it.
    void openInNewStudyTab(const std::string& module, const std::string& reference);

    /// Show commentary for a verse in the active workspace tab
    void showCommentary(const std::string& reference);

    /// Show dictionary entry in the active workspace tab
    void showDictionary(const std::string& key);

    /// Queue Mag viewer info update in the left pane preview.
    void showWordInfo(const std::string& word, const std::string& href,
                      const std::string& strong, const std::string& morph,
                      int screenX, int screenY);

    /// Cancel pending hover update (does not clear current MAG content).
    void hideWordInfo();

    /// Show search results in the left pane
    void showSearchResults(const std::string& query,
                           const std::string& moduleOverride = "");

    /// Show a short-lived status message in the bottom status bar.
    void showTransientStatus(const std::string& text, double seconds = 2.5);

    /// Called by BiblePane when module/book/chapter context changes.
    void updateActiveStudyTabLabel();

    /// Get left pane
    LeftPane* leftPane() { return leftPane_; }

    /// Get active Bible pane
    BiblePane* biblePane() { return biblePane_; }

    /// Get active right pane
    RightPane* rightPane() { return rightPane_; }

    /// Refresh all panes
    void refresh();

    /// Apply UI font and text rendering style settings.
    void applyAppearanceSettings(Fl_Font appFont,
                                 int appFontSize,
                                 const std::string& textCssOverride);

    /// Capture all session-restorable state.
    SessionState captureSessionState();

    /// Restore session state (window geometry, splitters, study tabs and pane state).
    void restoreSessionState(const SessionState& state);

protected:
    void resize(int X, int Y, int W, int H) override;
    int handle(int event) override;

private:
    struct HtmlDocBuffer {
        std::shared_ptr<void> doc;
        std::string html;
        std::string baseUrl;
        int scrollY = 0;
        int contentHeight = 0;
        int renderWidth = 0;
        bool scrollbarVisible = false;
        bool valid = false;
    };

    struct RightDocBuffers {
        HtmlDocBuffer commentary;
        HtmlDocBuffer dictionary;
        HtmlDocBuffer generalBook;
    };

    struct PendingWordInfo {
        std::string word;
        std::string href;
        std::string strong;
        std::string morph;
        int tabIndex = -1;
    };

    struct StudyContext {
        Fl_Group* tabGroup = nullptr;
        StudyTabState state;
        HtmlDocBuffer bibleBuffer;
        RightDocBuffers rightBuffer;
        bool hasBibleBuffer = false;
        bool hasRightBuffer = false;
        uint64_t lastUsed = 0;  ///< Monotonic counter for LRU eviction
    };

    VerdadApp* app_;

    // Menu bar
    Fl_Menu_Bar* menuBar_;
    Fl_Box* statusBar_;

    // Top-level layout
    Fl_Tile* mainTile_;
    LeftPane* leftPane_;
    Fl_Group* studyArea_;
    Fl_Button* newStudyTabButton_;
    Fl_Button* closeStudyTabButton_;
    Fl_Tabs* studyTabsWidget_;
    Fl_Tile* contentTile_;

    // Active workspace panes
    BiblePane* biblePane_;
    RightPane* rightPane_;

    // All workspace tabs
    std::vector<StudyContext> studyTabs_;
    int activeStudyTab_ = -1;
    uint64_t tabUseCounter_ = 0;
    static constexpr int kMaxCachedTabDocs = 4;
    bool applyingTabState_ = false;
    bool appearanceApplied_ = false;
    Fl_Font lastAppliedAppFont_ = FL_HELVETICA;
    int lastAppliedAppFontSize_ = 12;
    std::string lastAppliedTextCss_;

    // Delayed hover state for MAG updates
    PendingWordInfo pendingWordInfo_;
    bool hoverDelayScheduled_ = false;

    // Deferred startup prewarm state.
    bool prewarmScheduled_ = false;
    int prewarmCursor_ = 0;
    int prewarmAnchorTab_ = -1;
    std::chrono::steady_clock::time_point lastUserInteraction_;
    bool statusPollScheduled_ = false;
    std::string lastStatusBarText_;
    std::string transientStatusText_;
    std::chrono::steady_clock::time_point transientStatusUntil_{};

    /// Add a new Bible/Commentary workspace tab.
    void addStudyTab(const std::string& module,
                     const std::string& book,
                     int chapter,
                     int verse = 1);

    /// Duplicate the currently active workspace tab.
    void duplicateActiveStudyTab();

    /// Close the currently active workspace tab.
    void closeActiveStudyTab();

    /// Remove all study tabs and reset active pointers.
    void clearStudyTabs();

    /// Activate a workspace tab by index.
    void activateStudyTab(int index);

    /// Build a label like "KJV:Gen 1:1" for the given tab state.
    std::string studyTabLabel(const StudyTabState& state) const;

    /// Capture current shared pane state into the active tab context.
    void captureActiveTabState();

    /// Move rendered pane buffers out of the active tab for fast tab restore.
    void captureActiveTabDisplayBuffers();

    /// Evict litehtml doc from least-recently-used tabs to limit memory.
    void evictOldTabSnapshots();

    /// Apply a tab context into the shared Bible/Right panes.
    void applyTabState(int index);

    /// Pre-render inactive tabs once (used during startup restore to avoid cold switch spikes).
    void prewarmInactiveTabs();

    /// Schedule non-blocking incremental prewarm while keeping the active tab visible.
    void scheduleBackgroundPrewarm(double delaySec = 0.05);

    /// Run a single background prewarm step. Returns true when more work remains.
    bool runOneBackgroundPrewarmStep();

    /// FLTK timeout callback for deferred incremental prewarm.
    static void onDeferredPrewarm(void* data);

    /// Mark user interaction to throttle background prewarm while UI is active.
    void noteUserInteraction();

    /// Update bottom status bar text.
    void updateStatusBar();

    /// Periodic status poll callback.
    static void onStatusPoll(void* data);

    /// Apply the currently queued word info to the MAG viewer.
    void applyPendingWordInfo();

    /// FLTK timeout callback used for delayed MAG updates.
    static void onHoverDelayTimeout(void* data);

    /// Callback when study tabs selection changes.
    static void onStudyTabChange(Fl_Widget* w, void* data);

    /// Layout tab header controls (+, tabs, close).
    void layoutStudyTabHeader();

    /// Build the menu bar
    void buildMenu();

    // Menu callbacks
    static void onFileQuit(Fl_Widget* w, void* data);
    static void onFileModuleManager(Fl_Widget* w, void* data);
    static void onNavigateGo(Fl_Widget* w, void* data);
    static void onViewParallel(Fl_Widget* w, void* data);
    static void onViewSettings(Fl_Widget* w, void* data);
    static void onViewNewStudyTab(Fl_Widget* w, void* data);
    static void onViewCloseStudyTab(Fl_Widget* w, void* data);
    static void onHelpSearch(Fl_Widget* w, void* data);
    static void onHelpAbout(Fl_Widget* w, void* data);

    /// Open/search help window with search mode examples and regex tips.
    void showSearchHelpWindow();

    Fl_Double_Window* searchHelpWindow_ = nullptr;
    Fl_Text_Buffer* searchHelpTextBuffer_ = nullptr;
};

} // namespace verdad

#endif // VERDAD_MAIN_WINDOW_H
