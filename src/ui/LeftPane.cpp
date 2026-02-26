#include "ui/LeftPane.h"
#include "app/VerdadApp.h"
#include "ui/ModulePanel.h"
#include "ui/SearchPanel.h"
#include "ui/TagPanel.h"
#include "ui/HtmlWidget.h"
#include "ui/MainWindow.h"

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <algorithm>
#include <fstream>

namespace verdad {

LeftPane::LeftPane(VerdadApp* app, int X, int Y, int W, int H)
    : Fl_Group(X, Y, W, H)
    , app_(app) {
    box(FL_FLAT_BOX);
    //color(FL_BACKGROUND2_COLOR);

    begin();

    int padding = 2;
    int searchH = 30;
    int previewH = 150;
    int buttonW = 60;

    // Search box at top (always visible)
    Fl_Group* searchGroup = new Fl_Group(X + padding, Y + padding,
                                          W - 2 * padding, searchH);
    searchGroup->begin();

    searchInput_ = new Fl_Input(X + padding, Y + padding,
                                 W - 2 * padding - buttonW - 2, searchH);
    searchInput_->when(FL_WHEN_ENTER_KEY);
    searchInput_->callback(onSearchInput, this);
    searchInput_->tooltip("Enter search text and press Enter");

    searchButton_ = new Fl_Button(X + W - padding - buttonW, Y + padding,
                                   buttonW, searchH, "Search");
    searchButton_->callback(onSearch, this);

    searchGroup->end();
    searchGroup->resizable(searchInput_);

    // Tabbed area in the middle
    int tabY = Y + padding + searchH + padding;
    int tabH = H - searchH - previewH - 4 * padding;

    tabs_ = new Fl_Tabs(X + padding, tabY, W - 2 * padding, tabH);
    tabs_->selection_color(tabs_->color());
    tabs_->begin();

    // Modules tab
    modulePanel_ = new ModulePanel(app_, X + padding, tabY + 25,
                                    W - 2 * padding, tabH - 25);
    modulePanel_->label("Modules");

    // Search results tab
    searchPanel_ = new SearchPanel(app_, X + padding, tabY + 25,
                                    W - 2 * padding, tabH - 25);
    searchPanel_->label("Search");

    // Tags tab
    tagPanel_ = new TagPanel(app_, X + padding, tabY + 25,
                              W - 2 * padding, tabH - 25);
    tagPanel_->label("Tags");

    tabs_->end();
    tabs_->value(modulePanel_); // Default to modules tab

    // Preview area at bottom
    int previewY = tabY + tabH + padding;
    previewWidget_ = new HtmlWidget(X + padding, previewY,
                                     W - 2 * padding, previewH);

    // Use the same stylesheet as other HTML panes so MAG markup renders correctly.
    std::string cssFile = app_->getDataDir() + "/master.css";
    std::ifstream cssStream(cssFile);
    if (cssStream.is_open()) {
        std::string css((std::istreambuf_iterator<char>(cssStream)),
                         std::istreambuf_iterator<char>());
        previewWidget_->setMasterCSS(css);
    }

    end();
    resizable(tabs_);
}

LeftPane::~LeftPane() = default;

void LeftPane::resize(int X, int Y, int W, int H) {
    Fl_Group::resize(X, Y, W, H);

    if (!searchInput_ || !searchButton_ || !tabs_ ||
        !modulePanel_ || !searchPanel_ || !tagPanel_ ||
        !previewWidget_) {
        return;
    }

    const int padding = 2;
    const int searchH = 30;
    const int previewH = 150;
    const int buttonW = 60;
    const int tabHeaderH = 25;

    int searchW = std::max(10, W - 2 * padding - buttonW - 2);
    searchInput_->resize(X + padding, Y + padding, searchW, searchH);
    searchButton_->resize(X + W - padding - buttonW, Y + padding, buttonW, searchH);

    int tabY = Y + padding + searchH + padding;
    int tabH = std::max(40, H - searchH - previewH - 4 * padding);
    tabs_->resize(X + padding, tabY, std::max(20, W - 2 * padding), tabH);

    int contentY = tabY + tabHeaderH;
    int contentH = std::max(10, tabH - tabHeaderH);
    int contentW = std::max(20, W - 2 * padding);
    modulePanel_->resize(X + padding, contentY, contentW, contentH);
    searchPanel_->resize(X + padding, contentY, contentW, contentH);
    tagPanel_->resize(X + padding, contentY, contentW, contentH);

    int previewY = tabY + tabH + padding;
    int previewW = std::max(20, W - 2 * padding);
    int previewActualH = std::max(40, H - (previewY - Y) - padding);
    previewWidget_->resize(X + padding, previewY, previewW, previewActualH);

    tabs_->redraw();
}

void LeftPane::doSearch(const std::string& query) {
    searchInput_->value(query.c_str());
    if (searchPanel_) {
        searchPanel_->search(query);
        showSearchTab();
    }
}

void LeftPane::showSearchTab() {
    if (tabs_ && searchPanel_) {
        tabs_->value(searchPanel_);
        tabs_->redraw();
    }
}

void LeftPane::showModuleTab() {
    if (tabs_ && modulePanel_) {
        tabs_->value(modulePanel_);
        tabs_->redraw();
    }
}

void LeftPane::showTagTab() {
    if (tabs_ && tagPanel_) {
        tabs_->value(tagPanel_);
        tabs_->redraw();
    }
}

void LeftPane::setPreviewText(const std::string& html) {
    if (previewWidget_) {
        previewWidget_->setHtml(html);
    }
}

void LeftPane::redrawChrome() {
    damage(FL_DAMAGE_ALL);
    if (tabs_) {
        tabs_->damage(FL_DAMAGE_ALL);
        tabs_->redraw();
    }
    if (searchInput_) searchInput_->redraw();
    if (searchButton_) searchButton_->redraw();
    if (tabs_) tabs_->redraw();
}

void LeftPane::refresh() {
    if (modulePanel_) modulePanel_->refresh();
    if (tagPanel_) tagPanel_->refresh();
}

void LeftPane::onSearch(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<LeftPane*>(data);
    const char* text = self->searchInput_->value();
    if (text && text[0]) {
        self->doSearch(text);
    }
}

void LeftPane::onSearchInput(Fl_Widget* /*w*/, void* data) {
    // Enter key pressed in search input
    auto* self = static_cast<LeftPane*>(data);
    const char* text = self->searchInput_->value();
    if (text && text[0]) {
        self->doSearch(text);
    }
}

} // namespace verdad
