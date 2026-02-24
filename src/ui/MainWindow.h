#ifndef VERDAD_MAIN_WINDOW_H
#define VERDAD_MAIN_WINDOW_H

#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Tile.H>
#include <memory>
#include <string>

namespace verdad {

class VerdadApp;
class LeftPane;
class BiblePane;
class RightPane;

/// Main application window with three-pane layout
class MainWindow : public Fl_Double_Window {
public:
    MainWindow(VerdadApp* app, int W, int H, const char* title);
    ~MainWindow() override;

    /// Navigate to a verse reference
    void navigateTo(const std::string& reference);

    /// Navigate to a verse in a specific module
    void navigateTo(const std::string& module, const std::string& reference);

    /// Show commentary for a verse
    void showCommentary(const std::string& reference);

    /// Show dictionary entry
    void showDictionary(const std::string& key);

    /// Queue Mag viewer info update in the left pane preview, driven by
    /// strong/morph metadata extracted on hover.
    void showWordInfo(const std::string& word, const std::string& href,
                      const std::string& strong, const std::string& morph,
                      int screenX, int screenY);

    /// Cancel pending hover update (does not clear current MAG content).
    void hideWordInfo();

    /// Show search results in the left pane
    void showSearchResults(const std::string& query);

    /// Get left pane
    LeftPane* leftPane() { return leftPane_; }

    /// Get Bible pane
    BiblePane* biblePane() { return biblePane_; }

    /// Get right pane
    RightPane* rightPane() { return rightPane_; }

    /// Refresh all panes
    void refresh();

protected:
    int handle(int event) override;

private:
    struct PendingWordInfo {
        std::string word;
        std::string href;
        std::string strong;
        std::string morph;
    };

    VerdadApp* app_;

    // Menu bar
    Fl_Menu_Bar* menuBar_;

    // Three-pane layout using Fl_Tile for resizable splits
    Fl_Tile* mainTile_;

    // Panes
    LeftPane* leftPane_;
    BiblePane* biblePane_;
    RightPane* rightPane_;

    // Delayed hover state for MAG updates
    PendingWordInfo pendingWordInfo_;
    bool hoverDelayScheduled_ = false;

    /// Apply the currently queued word info to the MAG viewer.
    void applyPendingWordInfo();

    /// FLTK timeout callback used for delayed MAG updates.
    static void onHoverDelayTimeout(void* data);

    /// Build the menu bar
    void buildMenu();

    // Menu callbacks
    static void onFileQuit(Fl_Widget* w, void* data);
    static void onNavigateGo(Fl_Widget* w, void* data);
    static void onViewParallel(Fl_Widget* w, void* data);
    static void onHelpAbout(Fl_Widget* w, void* data);
};

} // namespace verdad

#endif // VERDAD_MAIN_WINDOW_H
