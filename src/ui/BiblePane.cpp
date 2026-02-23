#include "ui/BiblePane.h"
#include "app/VerdadApp.h"
#include "ui/HtmlWidget.h"
#include "ui/MainWindow.h"
#include "ui/VerseContext.h"
#include "sword/SwordManager.h"

#include <FL/Fl.H>
#include <FL/fl_ask.H>

#include <sstream>
#include <fstream>

namespace verdad {

BiblePane::BiblePane(VerdadApp* app, int X, int Y, int W, int H)
    : Fl_Group(X, Y, W, H)
    , app_(app) {

    begin();

    int navH = 30;
    int padding = 2;

    // Build navigation bar
    navBar_ = new Fl_Group(X, Y, W, navH);
    navBar_->begin();
    buildNavBar();
    navBar_->end();

    // Tabs area for Bible text
    int tabY = Y + navH + padding;
    int tabH = H - navH - padding;

    tabs_ = new Fl_Tabs(X, tabY, W, tabH);
    tabs_->callback(onTabChange, this);
    tabs_->begin();

    // Create default tab
    BibleTab defaultTab;
    defaultTab.tabGroup = new Fl_Group(X, tabY + 25, W, tabH - 25, "Bible");
    defaultTab.tabGroup->begin();

    defaultTab.htmlWidget = new HtmlWidget(X, tabY + 25, W, tabH - 25);

    // Load master CSS
    std::string cssFile = app_->getDataDir() + "/master.css";
    std::ifstream cssStream(cssFile);
    if (cssStream.is_open()) {
        std::string css((std::istreambuf_iterator<char>(cssStream)),
                         std::istreambuf_iterator<char>());
        defaultTab.htmlWidget->setMasterCSS(css);
    }

    // Set up callbacks
    defaultTab.htmlWidget->setLinkCallback(
        [this](const std::string& url) { onLinkClicked(url); });
    defaultTab.htmlWidget->setHoverCallback(
        [this](const std::string& word, const std::string& href,
               const std::string& strong, const std::string& morph,
               int x, int y) {
            onWordHover(word, href, strong, morph, x, y);
        });
    defaultTab.htmlWidget->setContextCallback(
        [this](const std::string& word, const std::string& href, int x, int y) {
            onContextMenu(word, href, x, y);
        });

    defaultTab.tabGroup->end();
    defaultTab.tabGroup->resizable(defaultTab.htmlWidget);

    tabs_->end();

    bibleTabs_.push_back(defaultTab);
    activeTabIndex_ = 0;

    end();
    resizable(tabs_);

    // Initialize with first available Bible module
    auto bibles = app_->swordManager().getBibleModules();
    if (!bibles.empty()) {
        setModule(bibles[0].name);
        navigateTo("Genesis", 1);
    }
}

BiblePane::~BiblePane() = default;

void BiblePane::navigateTo(const std::string& book, int chapter) {
    BibleTab* tab = activeTab();
    if (!tab) return;

    tab->currentBook = book;
    tab->currentChapter = chapter;

    updateDisplay();

    // Update navigation controls
    populateChapters();

    // Also update commentary in right pane
    if (app_->mainWindow()) {
        std::string ref = book + " " + std::to_string(chapter) + ":1";
        app_->mainWindow()->showCommentary(ref);
    }
}

void BiblePane::navigateToReference(const std::string& reference) {
    auto ref = SwordManager::parseVerseRef(reference);
    if (!ref.book.empty() && ref.chapter > 0) {
        navigateTo(ref.book, ref.chapter);
    }
}

void BiblePane::setModule(const std::string& moduleName) {
    BibleTab* tab = activeTab();
    if (!tab) return;

    tab->moduleName = moduleName;

    // Update module choice widget
    if (moduleChoice_) {
        for (int i = 0; i < moduleChoice_->size(); i++) {
            const Fl_Menu_Item& item = moduleChoice_->menu()[i];
            if (item.label() && moduleName == item.label()) {
                moduleChoice_->value(i);
                break;
            }
        }
    }

    // Update tab label
    if (tab->tabGroup) {
        tab->tabGroup->copy_label(moduleName.c_str());
    }

    populateBooks();
    updateDisplay();
}

void BiblePane::addTab(const std::string& moduleName) {
    BibleTab newTab;
    newTab.moduleName = moduleName;

    int tabY = tabs_->y() + 25;
    int tabH = tabs_->h() - 25;

    tabs_->begin();

    newTab.tabGroup = new Fl_Group(tabs_->x(), tabY,
                                    tabs_->w(), tabH,
                                    moduleName.c_str());
    newTab.tabGroup->begin();

    newTab.htmlWidget = new HtmlWidget(tabs_->x(), tabY,
                                        tabs_->w(), tabH);

    // Load master CSS
    std::string cssFile = app_->getDataDir() + "/master.css";
    std::ifstream cssStream(cssFile);
    if (cssStream.is_open()) {
        std::string css((std::istreambuf_iterator<char>(cssStream)),
                         std::istreambuf_iterator<char>());
        newTab.htmlWidget->setMasterCSS(css);
    }

    newTab.htmlWidget->setLinkCallback(
        [this](const std::string& url) { onLinkClicked(url); });
    newTab.htmlWidget->setHoverCallback(
        [this](const std::string& word, const std::string& href,
               const std::string& strong, const std::string& morph,
               int x, int y) {
            onWordHover(word, href, strong, morph, x, y);
        });
    newTab.htmlWidget->setContextCallback(
        [this](const std::string& word, const std::string& href, int x, int y) {
            onContextMenu(word, href, x, y);
        });

    newTab.tabGroup->end();
    newTab.tabGroup->resizable(newTab.htmlWidget);

    tabs_->end();
    tabs_->redraw();

    bibleTabs_.push_back(newTab);

    // Switch to new tab
    activeTabIndex_ = static_cast<int>(bibleTabs_.size()) - 1;
    tabs_->value(newTab.tabGroup);

    // Navigate to same position as current tab
    if (bibleTabs_.size() > 1) {
        auto& prevTab = bibleTabs_[activeTabIndex_ - 1];
        newTab.currentBook = prevTab.currentBook;
        newTab.currentChapter = prevTab.currentChapter;
    } else {
        newTab.currentBook = "Genesis";
        newTab.currentChapter = 1;
    }

    setModule(moduleName);
}

void BiblePane::closeCurrentTab() {
    if (bibleTabs_.size() <= 1) return; // Don't close last tab

    BibleTab* tab = activeTab();
    if (!tab || !tab->tabGroup) return;

    tabs_->remove(tab->tabGroup);
    delete tab->tabGroup;

    bibleTabs_.erase(bibleTabs_.begin() + activeTabIndex_);
    if (activeTabIndex_ >= static_cast<int>(bibleTabs_.size())) {
        activeTabIndex_ = static_cast<int>(bibleTabs_.size()) - 1;
    }

    if (!bibleTabs_.empty()) {
        tabs_->value(bibleTabs_[activeTabIndex_].tabGroup);
    }

    tabs_->redraw();
}

std::string BiblePane::currentModule() const {
    const BibleTab* tab = activeTab();
    return tab ? tab->moduleName : "";
}

std::string BiblePane::currentBook() const {
    const BibleTab* tab = activeTab();
    return tab ? tab->currentBook : "";
}

int BiblePane::currentChapter() const {
    const BibleTab* tab = activeTab();
    return tab ? tab->currentChapter : 0;
}

void BiblePane::toggleParallel() {
    parallelMode_ = !parallelMode_;

    if (parallelMode_ && parallelModules_.empty()) {
        // Ask user to select modules for parallel view
        auto bibles = app_->swordManager().getBibleModules();
        if (bibles.size() < 2) {
            fl_alert("Need at least 2 Bible modules for parallel view.");
            parallelMode_ = false;
            return;
        }

        // Default: use current module plus next available
        parallelModules_.push_back(currentModule());
        for (const auto& mod : bibles) {
            if (mod.name != currentModule()) {
                parallelModules_.push_back(mod.name);
                if (parallelModules_.size() >= 3) break;
            }
        }
    }

    updateDisplay();
}

void BiblePane::toggleParagraphMode() {
    paragraphMode_ = !paragraphMode_;
    updateDisplay();
}

void BiblePane::setParallelModules(const std::vector<std::string>& modules) {
    parallelModules_ = modules;
    if (parallelMode_) {
        updateDisplay();
    }
}

void BiblePane::nextChapter() {
    BibleTab* tab = activeTab();
    if (!tab) return;

    int maxChapter = app_->swordManager().getChapterCount(
        tab->moduleName, tab->currentBook);

    if (tab->currentChapter < maxChapter) {
        navigateTo(tab->currentBook, tab->currentChapter + 1);
    } else {
        // Go to next book
        auto books = app_->swordManager().getBookNames(tab->moduleName);
        for (size_t i = 0; i < books.size() - 1; i++) {
            if (books[i] == tab->currentBook) {
                navigateTo(books[i + 1], 1);
                populateBooks();
                break;
            }
        }
    }
}

void BiblePane::prevChapter() {
    BibleTab* tab = activeTab();
    if (!tab) return;

    if (tab->currentChapter > 1) {
        navigateTo(tab->currentBook, tab->currentChapter - 1);
    } else {
        // Go to previous book's last chapter
        auto books = app_->swordManager().getBookNames(tab->moduleName);
        for (size_t i = 1; i < books.size(); i++) {
            if (books[i] == tab->currentBook) {
                int lastCh = app_->swordManager().getChapterCount(
                    tab->moduleName, books[i - 1]);
                navigateTo(books[i - 1], lastCh);
                populateBooks();
                break;
            }
        }
    }
}

void BiblePane::refresh() {
    updateDisplay();
}

void BiblePane::buildNavBar() {
    int cx = navBar_->x() + 2;
    int cy = navBar_->y() + 2;
    int nh = navBar_->h() - 4;

    // Previous button
    prevButton_ = new Fl_Button(cx, cy, 30, nh, "@<");
    prevButton_->callback(onPrev, this);
    prevButton_->tooltip("Previous chapter");
    cx += 32;

    // Book selector
    bookChoice_ = new Fl_Choice(cx, cy, 120, nh);
    bookChoice_->callback(onBookChange, this);
    bookChoice_->tooltip("Select book");
    cx += 122;

    // Chapter selector
    chapterChoice_ = new Fl_Choice(cx, cy, 50, nh);
    chapterChoice_->callback(onChapterChange, this);
    chapterChoice_->tooltip("Select chapter");
    cx += 52;

    // Next button
    nextButton_ = new Fl_Button(cx, cy, 30, nh, "@>");
    nextButton_->callback(onNext, this);
    nextButton_->tooltip("Next chapter");
    cx += 32;

    // Reference input
    refInput_ = new Fl_Input(cx, cy, 120, nh);
    refInput_->tooltip("Type a reference (e.g. 'Gen 1:1')");
    cx += 122;

    // Go button
    goButton_ = new Fl_Button(cx, cy, 30, nh, "Go");
    goButton_->callback(onGo, this);
    cx += 32;

    // Module selector
    moduleChoice_ = new Fl_Choice(cx, cy, 100, nh);
    moduleChoice_->callback(onModuleChange, this);
    moduleChoice_->tooltip("Select Bible module");

    // Populate module choices
    auto bibles = app_->swordManager().getBibleModules();
    for (const auto& mod : bibles) {
        moduleChoice_->add(mod.name.c_str());
    }
    if (moduleChoice_->size() > 0) {
        moduleChoice_->value(0);
    }
    cx += 102;

    // Parallel button
    parallelButton_ = new Fl_Button(cx, cy, 25, nh, "||");
    parallelButton_->callback(onParallel, this);
    parallelButton_->tooltip("Toggle parallel Bible view");
    parallelButton_->type(FL_TOGGLE_BUTTON);
    cx += 27;

    // Paragraph mode toggle button
    paragraphButton_ = new Fl_Button(cx, cy, 25, nh, "\xC2\xB6");
    paragraphButton_->callback(onParagraphToggle, this);
    paragraphButton_->tooltip("Toggle paragraph / verse-per-line display");
    paragraphButton_->type(FL_TOGGLE_BUTTON);
    cx += 27;

    // Add tab button
    addTabButton_ = new Fl_Button(cx, cy, 25, nh, "+");
    addTabButton_->callback(onAddTab, this);
    addTabButton_->tooltip("Open new Bible tab");

    navBar_->resizable(refInput_);
}

void BiblePane::updateDisplay() {
    BibleTab* tab = activeTab();
    if (!tab || !tab->htmlWidget) return;

    std::string html;

    if (parallelMode_ && !parallelModules_.empty()) {
        html = app_->swordManager().getParallelText(
            parallelModules_, tab->currentBook, tab->currentChapter,
            paragraphMode_);
    } else {
        html = app_->swordManager().getChapterText(
            tab->moduleName, tab->currentBook, tab->currentChapter,
            paragraphMode_);
    }

    tab->htmlWidget->setHtml(html);
    tab->htmlWidget->scrollToTop();
}

void BiblePane::populateBooks() {
    BibleTab* tab = activeTab();
    if (!tab || !bookChoice_) return;

    bookChoice_->clear();
    auto books = app_->swordManager().getBookNames(tab->moduleName);
    for (const auto& book : books) {
        bookChoice_->add(book.c_str());
    }

    // Select current book
    for (int i = 0; i < bookChoice_->size(); i++) {
        const Fl_Menu_Item& item = bookChoice_->menu()[i];
        if (item.label() && tab->currentBook == item.label()) {
            bookChoice_->value(i);
            break;
        }
    }

    populateChapters();
}

void BiblePane::populateChapters() {
    BibleTab* tab = activeTab();
    if (!tab || !chapterChoice_) return;

    chapterChoice_->clear();
    int count = app_->swordManager().getChapterCount(
        tab->moduleName, tab->currentBook);

    for (int i = 1; i <= count; i++) {
        chapterChoice_->add(std::to_string(i).c_str());
    }

    if (tab->currentChapter > 0 && tab->currentChapter <= count) {
        chapterChoice_->value(tab->currentChapter - 1);
    }
}

BibleTab* BiblePane::activeTab() {
    if (activeTabIndex_ >= 0 &&
        activeTabIndex_ < static_cast<int>(bibleTabs_.size())) {
        return &bibleTabs_[activeTabIndex_];
    }
    return nullptr;
}

const BibleTab* BiblePane::activeTab() const {
    if (activeTabIndex_ >= 0 &&
        activeTabIndex_ < static_cast<int>(bibleTabs_.size())) {
        return &bibleTabs_[activeTabIndex_];
    }
    return nullptr;
}

void BiblePane::onGo(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    const char* ref = self->refInput_->value();
    if (ref && ref[0]) {
        self->navigateToReference(ref);
    }
}

void BiblePane::onPrev(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    self->prevChapter();
}

void BiblePane::onNext(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    self->nextChapter();
}

void BiblePane::onBookChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    const Fl_Menu_Item* item = self->bookChoice_->mvalue();
    if (item && item->label()) {
        self->navigateTo(item->label(), 1);
    }
}

void BiblePane::onChapterChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    int idx = self->chapterChoice_->value();
    if (idx >= 0) {
        BibleTab* tab = self->activeTab();
        if (tab) {
            self->navigateTo(tab->currentBook, idx + 1);
        }
    }
}

void BiblePane::onModuleChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    const Fl_Menu_Item* item = self->moduleChoice_->mvalue();
    if (item && item->label()) {
        self->setModule(item->label());
    }
}

void BiblePane::onParallel(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    self->toggleParallel();
}

void BiblePane::onParagraphToggle(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    self->toggleParagraphMode();
}

void BiblePane::onAddTab(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);

    // Show dialog to pick a module
    auto bibles = self->app_->swordManager().getBibleModules();
    if (bibles.empty()) {
        fl_alert("No Bible modules available.");
        return;
    }

    const char* name = fl_input("Enter Bible module name:", "KJV");
    if (name && name[0]) {
        self->addTab(name);
    }
}

void BiblePane::onTabChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);

    // Find which tab is now active
    Fl_Widget* val = self->tabs_->value();
    for (size_t i = 0; i < self->bibleTabs_.size(); i++) {
        if (self->bibleTabs_[i].tabGroup == val) {
            self->activeTabIndex_ = static_cast<int>(i);
            self->populateBooks();
            break;
        }
    }
}

void BiblePane::onLinkClicked(const std::string& url) {
    // Handle various link types
    if (url.find("strongs:") == 0 || url.find("strong:") == 0) {
        size_t colonPos = url.find(':');
        std::string num = url.substr(colonPos + 1);
        if (app_->mainWindow()) {
            app_->mainWindow()->showDictionary(num);
        }
    } else if (url.find("sword://") == 0) {
        // SWORD protocol link
        std::string ref = url.substr(8);
        navigateToReference(ref);
    } else if (url.find("morph:") == 0) {
        // Morphology link
        std::string code = url.substr(6);
        if (app_->mainWindow()) {
            app_->mainWindow()->showDictionary(code);
        }
    }
}

void BiblePane::onWordHover(const std::string& word, const std::string& href,
                             const std::string& strong, const std::string& morph,
                             int x, int y) {
    if (app_->mainWindow()) {
        if (!strong.empty() || !morph.empty() || !href.empty()) {
            app_->mainWindow()->showWordInfo(word, href, strong, morph, x, y);
        } else {
            app_->mainWindow()->hideWordInfo();
        }
    }
}

void BiblePane::onContextMenu(const std::string& word, const std::string& href,
                               int x, int y) {
    BibleTab* tab = activeTab();
    if (!tab) return;

    std::string verseKey = tab->currentBook + " "
                           + std::to_string(tab->currentChapter);

    VerseContext ctx(app_);
    ctx.show(word, href, verseKey, x, y);
}

} // namespace verdad
