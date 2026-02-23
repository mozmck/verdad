#include "ui/MainWindow.h"
#include "app/VerdadApp.h"
#include "ui/LeftPane.h"
#include "ui/BiblePane.h"
#include "ui/RightPane.h"
#include "ui/ToolTipWindow.h"
#include "sword/SwordManager.h"

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <FL/Fl_Menu_Item.H>

#include <sstream>

namespace verdad {

MainWindow::MainWindow(VerdadApp* app, int W, int H, const char* title)
    : Fl_Double_Window(W, H, title)
    , app_(app)
    , tooltipWindow_(nullptr) {

    // Set minimum size
    size_range(800, 600);

    int menuH = 25;

    // Menu bar
    menuBar_ = new Fl_Menu_Bar(0, 0, W, menuH);
    buildMenu();

    // Three-pane layout using Fl_Tile
    mainTile_ = new Fl_Tile(0, menuH, W, H - menuH);
    mainTile_->begin();

    // Left pane (about 25% width)
    int leftW = W * 25 / 100;
    leftPane_ = new LeftPane(app_, 0, menuH, leftW, H - menuH);

    // Bible pane (about 50% width)
    int bibleW = W * 50 / 100;
    biblePane_ = new BiblePane(app_, leftW, menuH, bibleW, H - menuH);

    // Right pane (remaining width)
    int rightX = leftW + bibleW;
    int rightW = W - rightX;
    rightPane_ = new RightPane(app_, rightX, menuH, rightW, H - menuH);

    mainTile_->end();

    // Create tooltip window (floating, not part of layout)
    tooltipWindow_ = new ToolTipWindow();

    // Set resizable
    resizable(mainTile_);

    end();
}

MainWindow::~MainWindow() {
    delete tooltipWindow_;
}

void MainWindow::navigateTo(const std::string& reference) {
    if (biblePane_) {
        biblePane_->navigateToReference(reference);
    }
}

void MainWindow::navigateTo(const std::string& module, const std::string& reference) {
    if (biblePane_) {
        biblePane_->setModule(module);
        biblePane_->navigateToReference(reference);
    }
}

void MainWindow::showCommentary(const std::string& reference) {
    if (rightPane_) {
        rightPane_->showCommentary(reference);
    }
}

void MainWindow::showDictionary(const std::string& key) {
    if (rightPane_) {
        rightPane_->showDictionaryEntry(key);
    }
}

void MainWindow::showWordInfo(const std::string& word, const std::string& href,
                               const std::string& strong, const std::string& morph,
                               int screenX, int screenY) {
    // Resolve strong number: prefer the data attribute, fall back to href parsing
    std::string strongNum = strong;
    if (strongNum.empty() && href.find("strongs:") == 0) {
        strongNum = href.substr(8);
    } else if (strongNum.empty() && href.find("strong:") == 0) {
        strongNum = href.substr(7);
    }

    // Resolve morph code: prefer the data attribute, fall back to href parsing
    std::string morphCode = morph;
    if (morphCode.empty() && href.find("morph:") == 0) {
        morphCode = href.substr(6);
    }

    if (strongNum.empty() && morphCode.empty()) return;

    // Build Mag HTML (shown in left-pane preview and optionally as tooltip)
    std::ostringstream html;
    html << "<div class=\"mag\">";

    if (!word.empty()) {
        html << "<div class=\"mag-word\">" << word << "</div>";
    }

    // Strong's: handle pipe-separated multiple numbers (e.g. "H7225|H1254")
    if (!strongNum.empty()) {
        std::istringstream ss(strongNum);
        std::string tok;
        while (std::getline(ss, tok, '|')) {
            if (tok.empty()) continue;
            html << "<div class=\"mag-strongs\"><b>Strong's " << tok << "</b></div>";
            std::string def = app_->swordManager().getStrongsDefinition(tok);
            if (!def.empty()) {
                html << "<div class=\"mag-def\">" << def << "</div>";
            }
        }
    }

    // Morphology
    if (!morphCode.empty()) {
        html << "<div class=\"mag-morph\"><b>Morph: " << morphCode << "</b></div>";
        std::string def = app_->swordManager().getMorphDefinition(morphCode);
        if (!def.empty()) {
            html << "<div class=\"mag-def\">" << def << "</div>";
        }
    }

    html << "</div>";
    std::string magHtml = html.str();

    // Show in left-pane Mag viewer (bottom preview area)
    if (leftPane_) {
        leftPane_->setPreviewText(magHtml);
    }

    // Also show as tooltip
    if (tooltipWindow_) {
        tooltipWindow_->showAt(screenX + 15, screenY + 15, magHtml);
    }
}

void MainWindow::hideWordInfo() {
    if (tooltipWindow_) {
        tooltipWindow_->hideTooltip();
    }
    if (leftPane_) {
        leftPane_->setPreviewText("");
    }
}

void MainWindow::showSearchResults(const std::string& query) {
    if (leftPane_) {
        leftPane_->doSearch(query);
        leftPane_->showSearchTab();
    }
}

void MainWindow::refresh() {
    if (leftPane_) leftPane_->refresh();
    if (biblePane_) biblePane_->refresh();
    if (rightPane_) rightPane_->refresh();
}

int MainWindow::handle(int event) {
    // Global keyboard shortcuts
    if (event == FL_SHORTCUT) {
        if (Fl::event_key() == FL_Escape) {
            hideWordInfo();
            return 1;
        }
    }
    return Fl_Double_Window::handle(event);
}

void MainWindow::buildMenu() {
    menuBar_->add("&File/&Quit", FL_CTRL + 'q', onFileQuit, this);
    menuBar_->add("&Navigate/&Go to Verse...", FL_CTRL + 'g', onNavigateGo, this);
    menuBar_->add("&View/&Parallel Bibles", FL_CTRL + 'p', onViewParallel, this);
    menuBar_->add("&Help/&About Verdad", 0, onHelpAbout, this);
}

void MainWindow::onFileQuit(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    self->hide();
}

void MainWindow::onNavigateGo(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    const char* ref = fl_input("Go to verse:", "Genesis 1:1");
    if (ref && ref[0]) {
        self->navigateTo(ref);
    }
}

void MainWindow::onViewParallel(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (self->biblePane_) {
        self->biblePane_->toggleParallel();
    }
}

void MainWindow::onHelpAbout(Fl_Widget* /*w*/, void* /*data*/) {
    fl_message("Verdad Bible Study\n\n"
               "A Bible study application using:\n"
               "- FLTK for the user interface\n"
               "- litehtml for XHTML rendering\n"
               "- SWORD library for Bible modules\n\n"
               "Version 0.1.0");
}

} // namespace verdad
