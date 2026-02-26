#include "ui/BiblePane.h"
#include "app/VerdadApp.h"
#include "ui/HtmlWidget.h"
#include "ui/MainWindow.h"
#include "ui/VerseContext.h"
#include "sword/SwordManager.h"

#include <FL/Fl.H>

#include <algorithm>
#include <sstream>
#include <fstream>

namespace verdad {

namespace {
constexpr int kNavH = 30;
constexpr int kContentPadding = 2;
constexpr int kParallelHeaderH = 28;
constexpr int kParallelHeaderSpacing = 6;
constexpr int kRefInputWidth = 190; // Fits: "2 Thessalonians xx:xxx"
} // namespace

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
    , parallelAddButton_(nullptr)
    , navSpacer_(nullptr)
    , parallelHeader_(nullptr)
    , htmlWidget_(nullptr)
    , currentBook_("Genesis")
    , currentChapter_(1) {
    box(FL_FLAT_BOX);
    //color(FL_BACKGROUND2_COLOR);

    begin();

    navBar_ = new Fl_Group(X, Y, W, kNavH);
    //navBar_->box(FL_FLAT_BOX);
    //navBar_->color(FL_BACKGROUND2_COLOR);
    navBar_->begin();
    buildNavBar();
    navBar_->end();

    int contentY = Y + kNavH + kContentPadding;
    int contentH = H - kNavH - kContentPadding;

    parallelHeader_ = new Fl_Group(X, contentY, W, kParallelHeaderH);
    //parallelHeader_->box(FL_FLAT_BOX);
    //parallelHeader_->color(FL_BACKGROUND2_COLOR);
    parallelHeader_->end();
    parallelHeader_->hide();

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

    if (parallelAddButton_) {
        parallelAddButton_->hide();
    }

    // Select default module (if available). Initial navigation is performed
    // by MainWindow after this pane is attached to a context tab.
    auto bibles = app_->swordManager().getBibleModules();
    if (!bibles.empty()) {
        moduleName_ = bibles[0].name;
        setModule(moduleName_);
    }
}

BiblePane::~BiblePane() {
    clearParallelHeader();
}

void BiblePane::resize(int X, int Y, int W, int H) {
    Fl_Group::resize(X, Y, W, H);

    if (!navBar_ || !bookChoice_ || !chapterChoice_ || !moduleChoice_ ||
        !refInput_ || !goButton_ || !prevButton_ || !nextButton_ ||
        !parallelButton_ || !paragraphButton_ || !parallelAddButton_ ||
        !navSpacer_ || !parallelHeader_ || !htmlWidget_) {
        return;
    }

    const int navH = kNavH;
    const int padding = kContentPadding;
    const int spacing = 2;
    const int buttonW = 30;
    const int bookW = 120;
    const int chapW = 50;
    const int moduleW = 100;
    const int smallBtnW = 25;
    const int refW = kRefInputWidth;

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

    refInput_->resize(cx, cy, refW, nh);
    cx += refW + spacing;

    goButton_->resize(cx, cy, buttonW, nh);
    cx += buttonW + spacing;

    moduleChoice_->resize(cx, cy, moduleW, nh);
    cx += moduleW + spacing;

    parallelButton_->resize(cx, cy, smallBtnW, nh);
    cx += smallBtnW + spacing;

    paragraphButton_->resize(cx, cy, smallBtnW, nh);
    cx += smallBtnW + spacing;

    parallelAddButton_->resize(cx, cy, smallBtnW, nh);
    cx += smallBtnW + spacing;

    int spacerW = std::max(0, (X + W - 2) - cx);
    navSpacer_->resize(cx, cy, spacerW, nh);

    int contentY = Y + navH + padding;
    int headerH = (parallelMode_ ? kParallelHeaderH : 0);
    parallelHeader_->resize(X, contentY, W, headerH);
    if (parallelMode_) {
        parallelHeader_->show();
    } else {
        parallelHeader_->hide();
    }
    parallelHeader_->damage(FL_DAMAGE_ALL);
    layoutParallelHeader();

    int textY = contentY + headerH;
    int textH = std::max(20, H - navH - padding - headerH);
    htmlWidget_->resize(X, textY, W, textH);
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

    if (parallelMode_) {
        normalizeParallelModules();
        if (parallelModules_.empty()) {
            parallelModules_.push_back(moduleName_);
        } else {
            parallelModules_.front() = moduleName_;
        }
    }

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

    if (parallelMode_) {
        normalizeParallelModules();
        if (parallelModules_.empty()) {
            parallelModules_.push_back(currentModule());
        }
        if (parallelModules_.size() == 1) {
            auto bibles = app_->swordManager().getBibleModules();
            for (const auto& mod : bibles) {
                if (mod.name != parallelModules_.front()) {
                    parallelModules_.push_back(mod.name);
                    break;
                }
            }
        }
    }

    if (parallelButton_) {
        parallelButton_->value(parallelMode_ ? 1 : 0);
    }
    updateDisplay();
    notifyContextChanged();
}

void BiblePane::toggleParagraphMode() {
    paragraphMode_ = !paragraphMode_;
    updateDisplay();
}

void BiblePane::setParallelModules(const std::vector<std::string>& modules) {
    parallelModules_ = modules;
    normalizeParallelModules();
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
    damage(FL_DAMAGE_ALL);
    if (navBar_) {
        navBar_->damage(FL_DAMAGE_ALL);
        navBar_->redraw();
    }
    if (parallelAddButton_) parallelAddButton_->redraw();
    if (parallelHeader_) {
        parallelHeader_->damage(FL_DAMAGE_ALL);
        parallelHeader_->redraw();
    }
    for (auto& col : parallelHeaderColumns_) {
        if (col.moduleChoice) col.moduleChoice->redraw();
        if (col.removeButton) col.removeButton->redraw();
    }
    redraw();
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
    normalizeParallelModules();

    if (parallelMode_ && parallelModules_.empty() && !moduleName_.empty()) {
        parallelModules_.push_back(moduleName_);
    }
    if (parallelMode_ && !parallelModules_.empty()) {
        moduleName_ = parallelModules_.front();
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
    if (parallelAddButton_) {
        if (parallelMode_) parallelAddButton_->show();
        else parallelAddButton_->hide();
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

    refInput_ = new Fl_Input(cx, cy, kRefInputWidth, nh);
    refInput_->tooltip("Type a reference (e.g. 'Gen 1:1')");
    cx += kRefInputWidth + 2;

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
    cx += 27;

    parallelAddButton_ = new Fl_Button(cx, cy, 25, nh, "+");
    parallelAddButton_->callback(onParallelAdd, this);
    parallelAddButton_->tooltip("Add parallel Bible column (up to 7)");
    parallelAddButton_->hide();
    cx += 27;

    navSpacer_ = new Fl_Box(cx, cy, 0, nh);
    navBar_->resizable(navSpacer_);
}

void BiblePane::updateDisplay() {
    if (!htmlWidget_ || !parallelHeader_) return;
    htmlWidget_->show();

    if (moduleName_.empty()) {
        auto bibles = app_->swordManager().getBibleModules();
        if (!bibles.empty()) {
            moduleName_ = bibles.front().name;
            applyModuleChoiceValue(moduleChoice_, moduleName_);
        }
    }
    if (moduleName_.empty()) {
        htmlWidget_->setHtml("<div class=\"chapter\"><p><i>No Bible module selected.</i></p></div>");
        htmlWidget_->scrollToTop();
        return;
    }

    if (currentBook_.empty()) {
        auto books = app_->swordManager().getBookNames(moduleName_);
        if (!books.empty()) {
            currentBook_ = books.front();
            if (bookChoice_) {
                for (int i = 0; i < bookChoice_->size(); ++i) {
                    const Fl_Menu_Item& item = bookChoice_->menu()[i];
                    if (item.label() && currentBook_ == item.label()) {
                        bookChoice_->value(i);
                        break;
                    }
                }
            }
            populateChapters();
        }
    }
    if (currentBook_.empty()) {
        htmlWidget_->setHtml("<div class=\"chapter\"><p><i>No books available in selected module.</i></p></div>");
        htmlWidget_->scrollToTop();
        return;
    }

    std::string html;

    if (parallelMode_) {
        normalizeParallelModules();
        if (parallelModules_.empty()) {
            parallelModules_.push_back(moduleName_);
        }
        if (moduleName_ != parallelModules_.front()) {
            moduleName_ = parallelModules_.front();
            applyModuleChoiceValue(moduleChoice_, moduleName_);
            populateBooks();
            int maxVerse = app_->swordManager().getVerseCount(moduleName_, currentBook_, currentChapter_);
            if (maxVerse <= 0) maxVerse = 1;
            currentVerse_ = std::max(1, std::min(currentVerse_, maxVerse));
            populateChapters();
        }

        syncParallelHeader();
        parallelHeader_->show();
        if (parallelAddButton_) {
            parallelAddButton_->show();
            if (parallelModules_.size() >= static_cast<size_t>(kMaxParallelColumns))
                parallelAddButton_->deactivate();
            else
                parallelAddButton_->activate();
        }
        html = app_->swordManager().getParallelText(
            parallelModules_, currentBook_, currentChapter_, paragraphMode_,
            currentVerse_);
    } else {
        parallelHeader_->hide();
        if (parallelAddButton_) {
            parallelAddButton_->hide();
        }
        html = app_->swordManager().getChapterText(
            moduleName_, currentBook_, currentChapter_, paragraphMode_,
            currentVerse_);
    }

    if (html.empty()) {
        html = "<div class=\"chapter\"><p><i>No text available for current reference.</i></p></div>";
    }

    int contentY = y() + kNavH + kContentPadding;
    int headerH = parallelMode_ ? kParallelHeaderH : 0;
    parallelHeader_->resize(x(), contentY, w(), headerH);
    parallelHeader_->damage(FL_DAMAGE_ALL);
    layoutParallelHeader();
    int textY = contentY + headerH;
    int textH = std::max(20, h() - kNavH - kContentPadding - headerH);
    htmlWidget_->resize(x(), textY, w(), textH);

    htmlWidget_->setHtml(html);
    htmlWidget_->scrollToTop();
    htmlWidget_->redraw();
    redrawChrome();
}

void BiblePane::normalizeParallelModules() {
    std::vector<std::string> normalized;
    normalized.reserve(parallelModules_.size());
    for (const auto& module : parallelModules_) {
        if (module.empty()) continue;
        normalized.push_back(module);
        if (normalized.size() >= static_cast<size_t>(kMaxParallelColumns)) break;
    }
    parallelModules_.swap(normalized);
}

void BiblePane::clearParallelHeader() {
    if (!parallelHeader_) {
        parallelHeaderColumns_.clear();
        return;
    }

    for (auto& col : parallelHeaderColumns_) {
        if (col.group) {
            parallelHeader_->remove(col.group);
            delete col.group;
        }
    }
    parallelHeaderColumns_.clear();
}

void BiblePane::populateParallelChoice(Fl_Choice* choice) const {
    if (!choice) return;
    choice->clear();
    auto bibles = app_->swordManager().getBibleModules();
    for (const auto& mod : bibles) {
        choice->add(mod.name.c_str());
    }
    if (choice->size() > 0) {
        choice->value(0);
    }
}

void BiblePane::applyModuleChoiceValue(Fl_Choice* choice, const std::string& module) const {
    if (!choice) return;
    for (int i = 0; i < choice->size(); ++i) {
        const Fl_Menu_Item& item = choice->menu()[i];
        if (item.label() && module == item.label()) {
            choice->value(i);
            return;
        }
    }
}

int BiblePane::parallelColumnIndexForWidget(Fl_Widget* w) const {
    if (!w) return -1;
    for (size_t i = 0; i < parallelHeaderColumns_.size(); ++i) {
        const auto& col = parallelHeaderColumns_[i];
        if (w == col.moduleChoice || w == col.removeButton || w == col.group) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void BiblePane::syncParallelHeader() {
    if (!parallelHeader_) return;

    normalizeParallelModules();
    if (parallelModules_.empty()) {
        clearParallelHeader();
        return;
    }

    while (parallelHeaderColumns_.size() < parallelModules_.size()) {
        parallelHeader_->begin();

        ParallelHeaderColumn col;
        col.group = new Fl_Group(parallelHeader_->x(), parallelHeader_->y(),
                                 parallelHeader_->w(), parallelHeader_->h());
        col.group->box(FL_FLAT_BOX);
        //col.group->color(FL_BACKGROUND2_COLOR);
        col.group->begin();

        col.moduleChoice = new Fl_Choice(col.group->x() + 2, col.group->y() + 2,
                                         std::max(40, col.group->w() - 28), 24);
        populateParallelChoice(col.moduleChoice);
        col.moduleChoice->callback(onParallelModuleChange, this);
        col.moduleChoice->tooltip("Parallel Bible module");

        col.removeButton = new Fl_Button(col.group->x() + col.group->w() - 24,
                                         col.group->y() + 2, 22, 24, "-");
        col.removeButton->callback(onParallelRemove, this);
        col.removeButton->tooltip("Remove this parallel Bible");

        col.group->end();
        parallelHeader_->end();
        parallelHeaderColumns_.push_back(col);
    }

    while (parallelHeaderColumns_.size() > parallelModules_.size()) {
        ParallelHeaderColumn col = parallelHeaderColumns_.back();
        parallelHeaderColumns_.pop_back();
        if (col.group) {
            parallelHeader_->remove(col.group);
            delete col.group;
        }
    }

    layoutParallelHeader();

    for (size_t i = 0; i < parallelHeaderColumns_.size() && i < parallelModules_.size(); ++i) {
        auto& col = parallelHeaderColumns_[i];
        if (col.moduleChoice) {
            applyModuleChoiceValue(col.moduleChoice, parallelModules_[i]);
        }
        if (col.removeButton) {
            if (parallelModules_.size() > 1) col.removeButton->activate();
            else col.removeButton->deactivate();
        }
    }
}

void BiblePane::layoutParallelHeader() {
    if (!parallelHeader_ || parallelHeaderColumns_.empty()) return;

    const int count = static_cast<int>(parallelHeaderColumns_.size());
    const int spacing = kParallelHeaderSpacing;
    const int contentX = parallelHeader_->x();
    const int contentY = parallelHeader_->y();
    const int contentW = std::max(20, parallelHeader_->w());
    const int contentH = std::max(20, parallelHeader_->h());

    int availableW = std::max(20, contentW - (spacing * (count - 1)));
    int baseW = availableW / count;
    int remainder = availableW % count;

    int x = contentX;
    for (int i = 0; i < count; ++i) {
        int colW = baseW + (i < remainder ? 1 : 0);
        auto& col = parallelHeaderColumns_[i];
        if (!col.group) continue;

        col.group->resize(x, contentY, colW, contentH);
        if (col.moduleChoice) {
            int choiceW = std::max(40, colW - 30);
            col.moduleChoice->resize(x + 2, contentY + 2, choiceW, std::max(20, contentH - 4));
        }
        if (col.removeButton) {
            col.removeButton->resize(x + colW - 24, contentY + 2, 22, std::max(20, contentH - 4));
        }

        x += colW + spacing;
    }
    parallelHeader_->damage(FL_DAMAGE_ALL);
}

void BiblePane::addParallelModule() {
    if (!parallelMode_) {
        parallelMode_ = true;
    }
    normalizeParallelModules();
    if (parallelModules_.size() >= static_cast<size_t>(kMaxParallelColumns)) return;

    auto bibles = app_->swordManager().getBibleModules();
    if (bibles.empty()) return;

    std::string candidate = bibles.front().name;
    for (const auto& mod : bibles) {
        bool used = false;
        for (const auto& existing : parallelModules_) {
            if (existing == mod.name) {
                used = true;
                break;
            }
        }
        if (!used) {
            candidate = mod.name;
            break;
        }
    }

    parallelModules_.push_back(candidate);
    normalizeParallelModules();
    if (parallelButton_) parallelButton_->value(1);
    updateDisplay();
    notifyContextChanged();
}

void BiblePane::removeParallelModuleAt(int index) {
    if (index < 0 || index >= static_cast<int>(parallelModules_.size())) return;

    parallelModules_.erase(parallelModules_.begin() + index);
    normalizeParallelModules();

    if (parallelModules_.empty()) {
        parallelMode_ = false;
        if (parallelButton_) parallelButton_->value(0);
    } else if (moduleName_ != parallelModules_.front()) {
        moduleName_ = parallelModules_.front();
        applyModuleChoiceValue(moduleChoice_, moduleName_);
        populateBooks();
        int maxVerse = app_->swordManager().getVerseCount(moduleName_, currentBook_, currentChapter_);
        if (maxVerse <= 0) maxVerse = 1;
        currentVerse_ = std::max(1, std::min(currentVerse_, maxVerse));
        populateChapters();
    }

    updateDisplay();
    notifyContextChanged();
}

void BiblePane::setParallelModuleAt(int index, const std::string& module) {
    if (module.empty()) return;
    if (index < 0 || index >= static_cast<int>(parallelModules_.size())) return;
    parallelModules_[index] = module;

    if (index == 0 && moduleName_ != module) {
        moduleName_ = module;
        applyModuleChoiceValue(moduleChoice_, moduleName_);
        populateBooks();
        int maxVerse = app_->swordManager().getVerseCount(moduleName_, currentBook_, currentChapter_);
        if (maxVerse <= 0) maxVerse = 1;
        currentVerse_ = std::max(1, std::min(currentVerse_, maxVerse));
        populateChapters();
    }

    updateDisplay();
    notifyContextChanged();
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

void BiblePane::onParallelAdd(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    if (!self) return;
    self->addParallelModule();
}

void BiblePane::onParallelRemove(Fl_Widget* w, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    if (!self) return;
    const int index = self->parallelColumnIndexForWidget(w);
    if (index < 0) return;
    self->removeParallelModuleAt(index);
}

void BiblePane::onParallelModuleChange(Fl_Widget* w, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    if (!self) return;
    const int index = self->parallelColumnIndexForWidget(w);
    if (index < 0 || index >= static_cast<int>(self->parallelHeaderColumns_.size())) return;

    Fl_Choice* choice = self->parallelHeaderColumns_[index].moduleChoice;
    if (!choice) return;
    const Fl_Menu_Item* item = choice->mvalue();
    if (item && item->label()) {
        self->setParallelModuleAt(index, item->label());
    }
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
