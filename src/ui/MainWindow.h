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
class ToolTipWindow;

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

    /// Show Mag viewer info in the left pane preview and an optional tooltip,
    /// driven by strong/morph metadata extracted on hover.
    void showWordInfo(const std::string& word, const std::string& href,
                      const std::string& strong, const std::string& morph,
                      int screenX, int screenY);

    /// Hide the word info tooltip
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
    VerdadApp* app_;

    // Menu bar
    Fl_Menu_Bar* menuBar_;

    // Three-pane layout using Fl_Tile for resizable splits
    Fl_Tile* mainTile_;

    // Panes
    LeftPane* leftPane_;
    BiblePane* biblePane_;
    RightPane* rightPane_;

    // Tooltip window
    ToolTipWindow* tooltipWindow_;

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
