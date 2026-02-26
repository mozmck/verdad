#include "ui/BiblePane.h"
#include "app/VerdadApp.h"
#include "ui/HtmlWidget.h"
#include "ui/MainWindow.h"
#include "ui/VerseContext.h"
#include "sword/SwordManager.h"

#include <FL/Fl.H>
#include <FL/fl_ask.H>

#include <algorithm>
#include <sstream>
#include <fstream>

namespace verdad {

BiblePane::BiblePane(VerdadApp* app, int X, int Y, int W, int H)
    : Fl_Group(X, Y, W, H)
    , app_(app)
    , navBar_(nullptr)
    , bookChoice_(nullptr)
    , chapterChoice_(nullptr)
    , moduleChoice_(nullptr)
    , refInput_(nullptr)
    , goButton_(nullptr)
    , prevButton_(nullptr)
    , nextButton_(nullptr)
    , parallelButton_(nullptr)
    , paragraphButton_(nullptr)
    , htmlWidget_(nullptr)
    , currentBook_("Genesis")
    , currentChapter_(1) {
    //box(FL_FLAT_BOX);
    //color(FL_BACKGROUND2_COLOR);

    begin();

    int navH = 30;
    int padding = 2;

    navBar_ = new Fl_Group(X, Y, W, navH);
    navBar_->begin();
    buildNavBar();
    navBar_->end();

    int contentY = Y + navH + padding;
    int contentH = H - navH - padding;

    htmlWidget_ = new HtmlWidget(X, contentY, W, contentH);

    std::string cssFile = app_->getDataDir() + "/master.css";
    std::ifstream cssStream(cssFile);
    if (cssStream.is_open()) {
        std::string css((std::istreambuf_iterator<char>(cssStream)),
                         std::istreambuf_iterator<char>());
        htmlWidget_->setMasterCSS(css);
    }

    htmlWidget_->setLinkCallback(
        [this](const std::string& url) { onLinkClicked(url); });
    htmlWidget_->setHoverCallback(
        [this](const std::string& word, const std::string& href,
               const std::string& strong, const std::string& morph,
               int x, int y) {
            onWordHover(word, href, strong, morph, x, y);
        });
    htmlWidget_->setContextCallback(
        [this](const std::string& word, const std::string& href, int x, int y) {
            onContextMenu(word, href, x, y);
        });

    end();
    resizable(htmlWidget_);

    // Select default module (if available). Initial navigation is performed
    // by MainWindow after this pane is attached to a context tab.
    auto bibles = app_->swordManager().getBibleModules();
    if (!bibles.empty()) {
        moduleName_ = bibles[0].name;
        setModule(moduleName_);
    }
}

BiblePane::~BiblePane() = default;

void BiblePane::resize(int X, int Y, int W, int H) {
    Fl_Group::resize(X, Y, W, H);

    if (!navBar_ || !bookChoice_ || !chapterChoice_ || !moduleChoice_ ||
        !refInput_ || !goButton_ || !prevButton_ || !nextButton_ ||
        !parallelButton_ || !paragraphButton_ || !htmlWidget_) {
        return;
    }

    const int navH = 30;
    const int padding = 2;
    const int spacing = 2;
    const int buttonW = 30;
    const int bookW = 120;
    const int chapW = 50;
    const int moduleW = 100;
    const int smallBtnW = 25;
    const int minRefW = 60;

    navBar_->resize(X, Y, W, navH);
    int cy = Y + 2;
    int nh = navH - 4;
    int cx = X + 2;

    prevButton_->resize(cx, cy, buttonW, nh);
    cx += buttonW + spacing;

    bookChoice_->resize(cx, cy, bookW, nh);
    cx += bookW + spacing;

    chapterChoice_->resize(cx, cy, chapW, nh);
    cx += chapW + spacing;

    nextButton_->resize(cx, cy, buttonW, nh);
    cx += buttonW + spacing;

    int rightFixed =
        buttonW + spacing + moduleW + spacing + smallBtnW + spacing + smallBtnW + 2;
    int refW = std::max(minRefW, (X + W) - cx - rightFixed);
    refInput_->resize(cx, cy, refW, nh);
    cx += refW + spacing;

    goButton_->resize(cx, cy, buttonW, nh);
    cx += buttonW + spacing;

    moduleChoice_->resize(cx, cy, moduleW, nh);
    cx += moduleW + spacing;

    parallelButton_->resize(cx, cy, smallBtnW, nh);
    cx += smallBtnW + spacing;

    paragraphButton_->resize(cx, cy, smallBtnW, nh);

    int contentY = Y + navH + padding;
    int contentH = std::max(20, H - navH - padding);
    htmlWidget_->resize(X, contentY, W, contentH);
}

void BiblePane::navigateTo(const std::string& book, int chapter, int verse) {
    if (moduleName_.empty()) return;

    currentBook_ = book;
    currentChapter_ = chapter;
    int maxVerse = app_->swordManager().getVerseCount(moduleName_, currentBook_, currentChapter_);
    if (maxVerse <= 0) maxVerse = 1;
    currentVerse_ = std::max(1, std::min(verse, maxVerse));

    if (bookChoice_) {
        for (int i = 0; i < bookChoice_->size(); ++i) {
            const Fl_Menu_Item& item = bookChoice_->menu()[i];
            if (item.label() && currentBook_ == item.label()) {
                bookChoice_->value(i);
                break;
            }
        }
    }

    updateDisplay();
    if (htmlWidget_ && currentVerse_ > 1) {
        htmlWidget_->scrollToAnchor("v" + std::to_string(currentVerse_));
    }
    populateChapters();
    notifyContextChanged();

    if (app_->mainWindow()) {
        std::string ref = currentBook_ + " " + std::to_string(currentChapter_) +
                          ":" + std::to_string(currentVerse_);
        app_->mainWindow()->showCommentary(ref);
    }
}

void BiblePane::navigateToReference(const std::string& reference) {
    auto ref = SwordManager::parseVerseRef(reference);
    if (!ref.book.empty() && ref.chapter > 0) {
        int verse = ref.verse > 0 ? ref.verse : 1;
        navigateTo(ref.book, ref.chapter, verse);
    }
}

void BiblePane::setModule(const std::string& moduleName) {
    if (moduleName.empty()) return;
    moduleName_ = moduleName;

    if (moduleChoice_) {
        for (int i = 0; i < moduleChoice_->size(); i++) {
            const Fl_Menu_Item& item = moduleChoice_->menu()[i];
            if (item.label() && moduleName_ == item.label()) {
                moduleChoice_->value(i);
                break;
            }
        }
    }

    populateBooks();
    updateDisplay();
    notifyContextChanged();
}

std::string BiblePane::currentModule() const {
    return moduleName_;
}

std::string BiblePane::currentBook() const {
    return currentBook_;
}

int BiblePane::currentChapter() const {
    return currentChapter_;
}

int BiblePane::currentVerse() const {
    return currentVerse_;
}

void BiblePane::toggleParallel() {
    parallelMode_ = !parallelMode_;

    if (parallelMode_ && parallelModules_.empty()) {
        auto bibles = app_->swordManager().getBibleModules();
        if (bibles.size() < 2) {
            fl_alert("Need at least 2 Bible modules for parallel view.");
            parallelMode_ = false;
            return;
        }

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
    if (moduleName_.empty() || currentBook_.empty()) return;

    int maxChapter = app_->swordManager().getChapterCount(moduleName_, currentBook_);

    if (currentChapter_ < maxChapter) {
        navigateTo(currentBook_, currentChapter_ + 1);
    } else {
        auto books = app_->swordManager().getBookNames(moduleName_);
        for (size_t i = 0; i + 1 < books.size(); i++) {
            if (books[i] == currentBook_) {
                navigateTo(books[i + 1], 1);
                populateBooks();
                break;
            }
        }
    }
}

void BiblePane::prevChapter() {
    if (moduleName_.empty() || currentBook_.empty()) return;

    if (currentChapter_ > 1) {
        navigateTo(currentBook_, currentChapter_ - 1);
    } else {
        auto books = app_->swordManager().getBookNames(moduleName_);
        for (size_t i = 1; i < books.size(); i++) {
            if (books[i] == currentBook_) {
                int lastCh = app_->swordManager().getChapterCount(moduleName_, books[i - 1]);
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

void BiblePane::redrawChrome() {
    if (navBar_) navBar_->redraw();
}

void BiblePane::selectVerse(int verse) {
    if (moduleName_.empty() || currentBook_.empty()) return;

    int maxVerse = app_->swordManager().getVerseCount(moduleName_, currentBook_, currentChapter_);
    if (maxVerse <= 0) maxVerse = 1;
    int clampedVerse = std::max(1, std::min(verse, maxVerse));
    if (clampedVerse == currentVerse_) return;

    currentVerse_ = clampedVerse;
    updateDisplay();
    if (htmlWidget_) {
        htmlWidget_->scrollToAnchor("v" + std::to_string(currentVerse_));
    }
    notifyContextChanged();

    if (app_->mainWindow()) {
        std::string ref = currentBook_ + " " + std::to_string(currentChapter_) +
                          ":" + std::to_string(currentVerse_);
        app_->mainWindow()->showCommentary(ref);
    }
}

void BiblePane::setStudyState(const std::string& module,
                              const std::string& book,
                              int chapter,
                              int verse,
                              bool paragraphMode,
                              bool parallelMode,
                              const std::vector<std::string>& parallelModules) {
    if (!module.empty()) {
        moduleName_ = module;
    } else if (moduleName_.empty()) {
        auto bibles = app_->swordManager().getBibleModules();
        if (!bibles.empty()) moduleName_ = bibles.front().name;
    }

    currentBook_ = book.empty() ? currentBook_ : book;
    if (currentBook_.empty()) currentBook_ = "Genesis";

    currentChapter_ = std::max(1, chapter);
    currentVerse_ = std::max(1, verse);
    paragraphMode_ = paragraphMode;
    parallelMode_ = parallelMode;
    parallelModules_ = parallelModules;

    if (parallelMode_ && parallelModules_.empty() && !moduleName_.empty()) {
        parallelModules_.push_back(moduleName_);
    }

    if (moduleChoice_) {
        for (int i = 0; i < moduleChoice_->size(); ++i) {
            const Fl_Menu_Item& item = moduleChoice_->menu()[i];
            if (item.label() && moduleName_ == item.label()) {
                moduleChoice_->value(i);
                break;
            }
        }
    }

    if (parallelButton_) {
        parallelButton_->value(parallelMode_ ? 1 : 0);
    }
    if (paragraphButton_) {
        paragraphButton_->value(paragraphMode_ ? 1 : 0);
    }

    populateBooks();
    populateChapters();

    int maxVerse = app_->swordManager().getVerseCount(moduleName_, currentBook_, currentChapter_);
    if (maxVerse <= 0) maxVerse = 1;
    currentVerse_ = std::max(1, std::min(currentVerse_, maxVerse));

    if (refInput_) {
        std::string ref = currentBook_ + " " + std::to_string(currentChapter_) +
                          ":" + std::to_string(currentVerse_);
        refInput_->value(ref.c_str());
    }
}

BiblePane::DisplayBuffer BiblePane::captureDisplayBuffer() const {
    DisplayBuffer buf;
    if (!htmlWidget_) return buf;

    buf.html = htmlWidget_->currentHtml();
    buf.scrollY = htmlWidget_->scrollY();
    buf.valid = !buf.html.empty();
    return buf;
}

void BiblePane::restoreDisplayBuffer(const DisplayBuffer& buffer) {
    if (!htmlWidget_ || !buffer.valid) return;
    htmlWidget_->setHtml(buffer.html);
    htmlWidget_->setScrollY(buffer.scrollY);
}

void BiblePane::buildNavBar() {
    int cx = navBar_->x() + 2;
    int cy = navBar_->y() + 2;
    int nh = navBar_->h() - 4;

    prevButton_ = new Fl_Button(cx, cy, 30, nh, "@<");
    prevButton_->callback(onPrev, this);
    prevButton_->tooltip("Previous chapter");
    cx += 32;

    bookChoice_ = new Fl_Choice(cx, cy, 120, nh);
    bookChoice_->callback(onBookChange, this);
    bookChoice_->tooltip("Select book");
    cx += 122;

    chapterChoice_ = new Fl_Choice(cx, cy, 50, nh);
    chapterChoice_->callback(onChapterChange, this);
    chapterChoice_->tooltip("Select chapter");
    cx += 52;

    nextButton_ = new Fl_Button(cx, cy, 30, nh, "@>");
    nextButton_->callback(onNext, this);
    nextButton_->tooltip("Next chapter");
    cx += 32;

    refInput_ = new Fl_Input(cx, cy, 120, nh);
    refInput_->tooltip("Type a reference (e.g. 'Gen 1:1')");
    cx += 122;

    goButton_ = new Fl_Button(cx, cy, 30, nh, "Go");
    goButton_->callback(onGo, this);
    cx += 32;

    moduleChoice_ = new Fl_Choice(cx, cy, 100, nh);
    moduleChoice_->callback(onModuleChange, this);
    moduleChoice_->tooltip("Select Bible module");

    auto bibles = app_->swordManager().getBibleModules();
    for (const auto& mod : bibles) {
        moduleChoice_->add(mod.name.c_str());
    }
    if (moduleChoice_->size() > 0) {
        moduleChoice_->value(0);
        const Fl_Menu_Item& item = moduleChoice_->menu()[0];
        if (item.label()) {
            moduleName_ = item.label();
        }
    }
    cx += 102;

    parallelButton_ = new Fl_Button(cx, cy, 25, nh, "||");
    parallelButton_->callback(onParallel, this);
    parallelButton_->tooltip("Toggle parallel Bible view");
    parallelButton_->type(FL_TOGGLE_BUTTON);
    cx += 27;

    paragraphButton_ = new Fl_Button(cx, cy, 25, nh, "\xC2\xB6");
    paragraphButton_->callback(onParagraphToggle, this);
    paragraphButton_->tooltip("Toggle paragraph / verse-per-line display");
    paragraphButton_->type(FL_TOGGLE_BUTTON);

    navBar_->resizable(refInput_);
}

void BiblePane::updateDisplay() {
    if (!htmlWidget_ || moduleName_.empty() || currentBook_.empty()) return;

    std::string html;

    if (parallelMode_ && !parallelModules_.empty()) {
        html = app_->swordManager().getParallelText(
            parallelModules_, currentBook_, currentChapter_, paragraphMode_,
            currentVerse_);
    } else {
        html = app_->swordManager().getChapterText(
            moduleName_, currentBook_, currentChapter_, paragraphMode_,
            currentVerse_);
    }

    htmlWidget_->setHtml(html);
    htmlWidget_->scrollToTop();
}

void BiblePane::populateBooks() {
    if (!bookChoice_ || moduleName_.empty()) return;

    bookChoice_->clear();
    auto books = app_->swordManager().getBookNames(moduleName_);
    for (const auto& book : books) {
        bookChoice_->add(book.c_str());
    }

    if (books.empty()) return;

    bool foundCurrent = false;
    for (int i = 0; i < bookChoice_->size(); i++) {
        const Fl_Menu_Item& item = bookChoice_->menu()[i];
        if (item.label() && currentBook_ == item.label()) {
            bookChoice_->value(i);
            foundCurrent = true;
            break;
        }
    }

    if (!foundCurrent) {
        currentBook_ = books.front();
        bookChoice_->value(0);
    }

    populateChapters();
}

void BiblePane::populateChapters() {
    if (!chapterChoice_ || moduleName_.empty() || currentBook_.empty()) return;

    chapterChoice_->clear();
    int count = app_->swordManager().getChapterCount(moduleName_, currentBook_);

    for (int i = 1; i <= count; i++) {
        chapterChoice_->add(std::to_string(i).c_str());
    }

    if (count <= 0) return;
    if (currentChapter_ < 1) currentChapter_ = 1;
    if (currentChapter_ > count) currentChapter_ = count;
    chapterChoice_->value(currentChapter_ - 1);
}

void BiblePane::notifyContextChanged() {
    if (app_->mainWindow()) {
        app_->mainWindow()->updateActiveStudyTabLabel();
    }
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
        self->navigateTo(self->currentBook_, idx + 1);
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

void BiblePane::onLinkClicked(const std::string& url) {
    if (url.find("verse:") == 0) {
        try {
            int verse = std::stoi(url.substr(6));
            selectVerse(verse);
        } catch (...) {
            // Ignore malformed verse links.
        }
    } else if (url.find("strongs:") == 0 || url.find("strong:") == 0) {
        size_t colonPos = url.find(':');
        std::string num = url.substr(colonPos + 1);
        if (app_->mainWindow()) {
            app_->mainWindow()->showDictionary(num);
        }
    } else if (url.find("sword://") == 0) {
        std::string ref = url.substr(8);
        navigateToReference(ref);
    } else if (url.find("morph:") == 0) {
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
    std::string verseKey = currentBook_ + " " + std::to_string(currentChapter_) +
                           ":" + std::to_string(currentVerse_);

    VerseContext ctx(app_);
    ctx.show(word, href, verseKey, x, y);
}

} // namespace verdad
