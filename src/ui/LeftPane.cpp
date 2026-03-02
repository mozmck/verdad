#include "ui/LeftPane.h"
#include "app/VerdadApp.h"
#include "ui/ModulePanel.h"
#include "ui/SearchPanel.h"
#include "ui/TagPanel.h"
#include "ui/HtmlWidget.h"
#include "ui/MainWindow.h"
#include "ui/StyledTabs.h"

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <algorithm>
#include <fstream>

namespace verdad {

LeftPane::LeftPane(VerdadApp* app, int X, int Y, int W, int H)
    : Fl_Group(X, Y, W, H)
    , app_(app)
    , searchGroup_(nullptr)
    , searchInput_(nullptr)
    , searchButton_(nullptr)
    , contentTile_(nullptr)
    , contentResizeBox_(nullptr)
    , tabs_(nullptr)
    , modulePanel_(nullptr)
    , searchPanel_(nullptr)
    , tagPanel_(nullptr)
    , previewWidget_(nullptr) {
    box(FL_FLAT_BOX);
    //color(FL_BACKGROUND2_COLOR);

    begin();

    int padding = 2;
    int searchH = 30;
    int previewH = 150;
    int minTabH = 80;
    int minPreviewH = 60;
    int buttonW = 60;

    // Search box at top (always visible)
    searchGroup_ = new Fl_Group(X + padding, Y + padding,
                                W - 2 * padding, searchH);
    searchGroup_->begin();

    searchInput_ = new Fl_Input(X + padding, Y + padding,
                                 W - 2 * padding - buttonW - 2, searchH);
    searchInput_->box(FL_DOWN_BOX);
    searchInput_->color(FL_WHITE);
    searchInput_->when(FL_WHEN_ENTER_KEY);
    searchInput_->callback(onSearchInput, this);
    searchInput_->tooltip("Enter search text and press Enter");

    searchButton_ = new Fl_Button(X + W - padding - buttonW, Y + padding,
                                   buttonW, searchH, "Search");
    searchButton_->callback(onSearch, this);

    searchGroup_->end();
    searchGroup_->resizable(searchInput_);

    // Splitter area in the middle/bottom (tabs + preview)
    int contentY = Y + padding + searchH + padding;
    int contentW = std::max(20, W - 2 * padding);
    int contentH = std::max(minTabH + minPreviewH, H - (contentY - Y) - padding);
    contentTile_ = new Fl_Tile(X + padding, contentY, contentW, contentH);
    contentTile_->begin();

    // FLTK tile splitter logic expects an explicit resizable bounds widget.
    contentResizeBox_ = new Fl_Box(X + padding, contentY, contentW, contentH);
    contentResizeBox_->box(FL_NO_BOX);

    int previewInitH = std::min(previewH, contentH - minTabH);
    previewInitH = std::max(minPreviewH, previewInitH);
    int tabsInitH = std::max(minTabH, contentH - previewInitH);
    tabs_ = new StyledTabs(X + padding, contentY, contentW, tabsInitH);
    tabs_->selection_color(tabs_->color());
    tabs_->begin();

    // Modules tab
    modulePanel_ = new ModulePanel(app_, X + padding, contentY + 25,
                                   contentW, tabsInitH - 25);
    modulePanel_->label("Modules");

    // Search results tab
    searchPanel_ = new SearchPanel(app_, X + padding, contentY + 25,
                                   contentW, tabsInitH - 25);
    searchPanel_->label("Search");

    // Tags tab
    tagPanel_ = new TagPanel(app_, X + padding, contentY + 25,
                             contentW, tabsInitH - 25);
    tagPanel_->label("Tags");

    tabs_->end();
    tabs_->resizable(modulePanel_);
    tabs_->value(modulePanel_); // Default to modules tab
    tabs_->callback(onTabChanged, this);
    syncTabPanelVisibility();

    // Preview area at bottom
    int previewY = contentY + tabsInitH;
    previewWidget_ = new HtmlWidget(X + padding, previewY,
                                    contentW, std::max(minPreviewH, contentH - tabsInitH));

    // Use the same stylesheet as other HTML panes so MAG markup renders correctly.
    std::string cssFile = app_->getDataDir() + "/master.css";
    std::ifstream cssStream(cssFile);
    if (cssStream.is_open()) {
        std::string css((std::istreambuf_iterator<char>(cssStream)),
                         std::istreambuf_iterator<char>());
        previewWidget_->setMasterCSS(css);
    }

    contentTile_->end();
    contentTile_->resizable(contentResizeBox_);
    contentTile_->init_sizes();

    end();
    resizable(contentTile_);
}

LeftPane::~LeftPane() = default;

void LeftPane::resize(int X, int Y, int W, int H) {
    Fl_Group::resize(X, Y, W, H);

    if (!searchGroup_ || !searchInput_ || !searchButton_ || !contentTile_ ||
        !contentResizeBox_ ||
        !tabs_ ||
        !modulePanel_ || !searchPanel_ || !tagPanel_ ||
        !previewWidget_) {
        return;
    }

    const int padding = 2;
    const int searchH = 30;
    const int minTabH = 80;
    const int minPreviewH = 60;
    const int buttonW = 60;
    const int tabHeaderH = 25;

    searchGroup_->resize(X + padding, Y + padding,
                         std::max(20, W - 2 * padding), searchH);

    int searchW = std::max(10, W - 2 * padding - buttonW - 2);
    searchInput_->resize(X + padding, Y + padding, searchW, searchH);
    searchButton_->resize(X + W - padding - buttonW, Y + padding, buttonW, searchH);

    int oldTabsH = tabs_->h();
    int tileY = Y + padding + searchH + padding;
    int tileW = std::max(20, W - 2 * padding);
    int tileH = std::max(minTabH + minPreviewH, H - (tileY - Y) - padding);

    contentTile_->resize(X + padding, tileY, tileW, tileH);
    contentResizeBox_->resize(X + padding, tileY, tileW, tileH);

    int tabsH = std::clamp(oldTabsH, minTabH, std::max(minTabH, tileH - minPreviewH));
    tabs_->resize(X + padding, tileY, tileW, tabsH);
    previewWidget_->resize(X + padding, tileY + tabsH, tileW, tileH - tabsH);

    int panelY = tileY + tabHeaderH;
    int panelH = std::max(10, tabsH - tabHeaderH);
    modulePanel_->resize(X + padding, panelY, tileW, panelH);
    searchPanel_->resize(X + padding, panelY, tileW, panelH);
    tagPanel_->resize(X + padding, panelY, tileW, panelH);

    tabs_->redraw();
    contentTile_->init_sizes();
}

void LeftPane::doSearch(const std::string& query,
                        const std::string& moduleOverride) {
    searchInput_->value(query.c_str());
    if (searchPanel_) {
        searchPanel_->search(query, moduleOverride);
        showSearchTab();
    }
}

void LeftPane::setSearchModule(const std::string& moduleName) {
    if (searchPanel_) {
        searchPanel_->setSelectedModule(moduleName);
    }
}

void LeftPane::showSearchTab() {
    if (tabs_ && searchPanel_) {
        tabs_->value(searchPanel_);
        syncTabPanelVisibility();
        tabs_->redraw();
    }
}

void LeftPane::showModuleTab() {
    if (tabs_ && modulePanel_) {
        tabs_->value(modulePanel_);
        syncTabPanelVisibility();
        tabs_->redraw();
    }
}

void LeftPane::showTagTab() {
    if (tabs_ && tagPanel_) {
        tabs_->value(tagPanel_);
        syncTabPanelVisibility();
        tabs_->redraw();
    }
}

void LeftPane::syncTabPanelVisibility() {
    if (!tabs_ || !modulePanel_ || !searchPanel_ || !tagPanel_) return;

    Fl_Widget* active = tabs_->value();
    if (active == modulePanel_) {
        modulePanel_->show();
        searchPanel_->hide();
        tagPanel_->hide();
    } else if (active == searchPanel_) {
        modulePanel_->hide();
        searchPanel_->show();
        tagPanel_->hide();
    } else if (active == tagPanel_) {
        modulePanel_->hide();
        searchPanel_->hide();
        tagPanel_->show();
    } else {
        modulePanel_->show();
        searchPanel_->hide();
        tagPanel_->hide();
    }
}

void LeftPane::setPreviewText(const std::string& html) {
    if (previewWidget_) {
        previewWidget_->setHtml(html);
    }
}

void LeftPane::setHtmlStyleOverride(const std::string& css) {
    if (previewWidget_) {
        previewWidget_->setStyleOverrideCss(css);
    }
}

int LeftPane::previewHeight() const {
    return previewWidget_ ? previewWidget_->h() : 0;
}

void LeftPane::setPreviewHeight(int height) {
    if (!contentTile_ || !contentResizeBox_ || !tabs_ || !modulePanel_ ||
        !searchPanel_ || !tagPanel_ || !previewWidget_) {
        return;
    }

    const int minTabH = 80;
    const int minPreviewH = 60;
    const int tabHeaderH = 25;

    const int tileX = contentTile_->x();
    const int tileY = contentTile_->y();
    const int tileW = contentTile_->w();
    const int tileH = contentTile_->h();
    if (tileH <= 0) return;

    int previewH = std::clamp(height, minPreviewH,
                              std::max(minPreviewH, tileH - minTabH));
    int tabsH = std::max(minTabH, tileH - previewH);

    contentResizeBox_->resize(tileX, tileY, tileW, tileH);
    tabs_->resize(tileX, tileY, tileW, tabsH);
    previewWidget_->resize(tileX, tileY + tabsH, tileW, tileH - tabsH);

    int panelY = tileY + tabHeaderH;
    int panelH = std::max(10, tabsH - tabHeaderH);
    modulePanel_->resize(tileX, panelY, tileW, panelH);
    searchPanel_->resize(tileX, panelY, tileW, panelH);
    tagPanel_->resize(tileX, panelY, tileW, panelH);

    contentTile_->init_sizes();
    contentTile_->damage(FL_DAMAGE_ALL);
    contentTile_->redraw();
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

void LeftPane::onTabChanged(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<LeftPane*>(data);
    if (!self) return;
    self->syncTabPanelVisibility();
    self->redraw();
}

} // namespace verdad
