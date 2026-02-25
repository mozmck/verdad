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

        std::string commentaryModule;
        std::string commentaryReference;
        std::string dictionaryModule;
        std::string dictionaryKey;
        bool dictionaryActive = false;
    };

    struct SessionState {
        int windowX = -1;
        int windowY = -1;
        int windowW = 1200;
        int windowH = 800;
        int leftPaneWidth = 300;
        int activeStudyTab = 0;
        std::vector<StudyTabState> studyTabs;
    };

    MainWindow(VerdadApp* app, int W, int H, const char* title);
    ~MainWindow() override;

    /// Navigate to a verse reference in the active workspace tab
    void navigateTo(const std::string& reference);

    /// Navigate to a verse in a specific module in the active workspace tab
    void navigateTo(const std::string& module, const std::string& reference);

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
    void showSearchResults(const std::string& query);

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

    /// Capture all session-restorable state.
    SessionState captureSessionState() const;

    /// Restore session state (window geometry, splitters, study tabs and pane state).
    void restoreSessionState(const SessionState& state);

protected:
    int handle(int event) override;

private:
    struct PendingWordInfo {
        std::string word;
        std::string href;
        std::string strong;
        std::string morph;
    };

    struct StudyContext {
        Fl_Group* tabGroup = nullptr;
        Fl_Tile* contentTile = nullptr;
        BiblePane* biblePane = nullptr;
        RightPane* rightPane = nullptr;
    };

    VerdadApp* app_;

    // Menu bar
    Fl_Menu_Bar* menuBar_;

    // Top-level layout
    Fl_Tile* mainTile_;
    LeftPane* leftPane_;
    Fl_Group* studyArea_;
    Fl_Button* newStudyTabButton_;
    Fl_Tabs* studyTabsWidget_;

    // Active workspace panes
    BiblePane* biblePane_;
    RightPane* rightPane_;

    // All workspace tabs
    std::vector<StudyContext> studyTabs_;
    int activeStudyTab_ = -1;

    // Delayed hover state for MAG updates
    PendingWordInfo pendingWordInfo_;
    bool hoverDelayScheduled_ = false;

    /// Add a new Bible/Commentary workspace tab.
    void addStudyTab(const std::string& module,
                     const std::string& book,
                     int chapter,
                     int verse = 1);

    /// Duplicate the currently active workspace tab.
    void duplicateActiveStudyTab();

    /// Remove all study tabs and reset active pointers.
    void clearStudyTabs();

    /// Activate a workspace tab by index.
    void activateStudyTab(int index);

    /// Build a label like "KJV:Gen 1:1" for the given pane context.
    static std::string studyTabLabel(const BiblePane* pane);

    /// Apply the currently queued word info to the MAG viewer.
    void applyPendingWordInfo();

    /// FLTK timeout callback used for delayed MAG updates.
    static void onHoverDelayTimeout(void* data);

    /// Callback when study tabs selection changes.
    static void onStudyTabChange(Fl_Widget* w, void* data);

    /// Build the menu bar
    void buildMenu();

    // Menu callbacks
    static void onFileQuit(Fl_Widget* w, void* data);
    static void onNavigateGo(Fl_Widget* w, void* data);
    static void onViewParallel(Fl_Widget* w, void* data);
    static void onViewNewStudyTab(Fl_Widget* w, void* data);
    static void onHelpAbout(Fl_Widget* w, void* data);
};

} // namespace verdad

#endif // VERDAD_MAIN_WINDOW_H
