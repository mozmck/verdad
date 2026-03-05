#include "ui/MainWindow.h"
#include "app/VerdadApp.h"
#include "ui/LeftPane.h"
#include "ui/BiblePane.h"
#include "ui/RightPane.h"
#include "ui/ModuleManagerDialog.h"
#include "ui/StyledTabs.h"
#include "sword/SwordManager.h"
#include "search/SearchIndexer.h"
#include "app/PerfTrace.h"

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Spinner.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Browser_.H>
#include <FL/Fl_Input_.H>
#include <FL/Fl_Menu_.H>
#include <FL/Fl_Text_Display.H>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace verdad {
namespace {

std::string trimCopy(const std::string& s) {
    size_t start = 0;
    while (start < s.size() &&
           std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string htmlEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

std::string buildModuleRefLabel(VerdadApp* app,
                                const std::string& module,
                                const std::string& reference) {
    if (!app) return module + ":" + reference;
    std::string shortRef = app->swordManager().getShortReference(module, reference);
    if (shortRef.empty()) shortRef = reference;
    return module + ":" + shortRef;
}

std::string extractStrongsToken(const std::string& query) {
    static const std::regex strongRe(R"(([HhGg]\d+[A-Za-z]?))");
    std::smatch m;
    if (!std::regex_search(query, m, strongRe)) return "";
    std::string tok = m[1].str();
    for (char& c : tok) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }
    return tok;
}

const char* kSearchHelpText =
    "Search Help\n"
    "===========\n"
    "\n"
    "Search Module\n"
    "- The module dropdown in the Search tab controls which Bible module is searched.\n"
    "- If a search is started from a word right-click menu, that module is used automatically.\n"
    "\n"
    "Search Types\n"
    "1) Multi-word\n"
    "- Matches verses by terms (word-based search).\n"
    "- Example: faith hope\n"
    "\n"
    "2) Exact phrase\n"
    "- Words must appear together in order.\n"
    "- Example: in the beginning\n"
    "\n"
    "3) Regex\n"
    "- Uses ECMAScript regex syntax and is case-insensitive.\n"
    "- Regex search runs on indexed plain verse text.\n"
    "- If the module is not indexed yet, indexing is queued and regex returns no results until ready.\n"
    "- Examples:\n"
    "  ^In\\b\n"
    "  \\b(king|kingdom)\\b\n"
    "  grac(e|es)\n"
    "  God.*spirit\n"
    "\n"
    "Strong's / Lemma Search\n"
    "- Enter a Strong's key directly in the search box:\n"
    "  G3588\n"
    "  H7225\n"
    "- You can also use prefixes:\n"
    "  strongs:G3588\n"
    "  lemma:H7225\n"
    "- Right-click on a Bible word to use \"Search Strong's: ...\" menu items.\n"
    "\n"
    "Search Results\n"
    "- Left click: select result and update verse preview.\n"
    "- Left double-click: open verse in current study tab.\n"
    "- Middle click: open verse in a new study tab.\n"
    "- Results are sorted by book, chapter, and verse.\n"
    "\n"
    "Regex Tips\n"
    "- Escape special characters when searching literal punctuation.\n"
    "- Use \\b for word boundaries.\n"
    "- Use .* to allow any characters between terms.\n";

void appendEscapedTextLines(std::ostringstream& out,
                            const std::string& text,
                            const char* cls) {
    std::istringstream ss(text);
    std::string line;
    bool emitted = false;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out << "<span class=\"mag-line " << cls << "\">"
            << htmlEscape(line) << "</span>";
        emitted = true;
    }
    if (!emitted) {
        out << "<span class=\"mag-line " << cls << "\"></span>";
    }
}

std::vector<std::string> extractStrongsTokens(const std::string& strongs) {
    std::vector<std::string> prefixed;
    std::vector<std::string> numeric;
    std::unordered_set<std::string> seen;

    static const std::regex strongRe(
        R"((?:^|[|,;\s:])([HGhg]?\d+[A-Za-z]?)(?=$|[|,;\s]))");
    auto it = std::sregex_iterator(strongs.begin(), strongs.end(), strongRe);
    auto end = std::sregex_iterator();
    for (; it != end; ++it) {
        std::string tok = (*it)[1].str();
        for (char& c : tok) {
            if (std::isalpha(static_cast<unsigned char>(c))) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
        }
        if (tok.empty() || seen.count(tok) != 0) continue;

        if (std::isalpha(static_cast<unsigned char>(tok[0])) &&
            tok[0] != 'H' && tok[0] != 'G') {
            continue;
        }

        seen.insert(tok);
        if (std::isalpha(static_cast<unsigned char>(tok[0]))) {
            prefixed.push_back(tok);
        } else {
            numeric.push_back(tok);
        }
    }

    if (!prefixed.empty()) {
        return prefixed;
    }
    return numeric;
}

Fl_Font appFontFromName(const std::string& name) {
    if (name == "Times") return FL_TIMES;
    if (name == "Courier") return FL_COURIER;
    return FL_HELVETICA;
}

const char* appFontName(Fl_Font font) {
    switch (font) {
    case FL_TIMES:
    case FL_TIMES_BOLD:
    case FL_TIMES_ITALIC:
    case FL_TIMES_BOLD_ITALIC:
        return "Times";
    case FL_COURIER:
    case FL_COURIER_BOLD:
    case FL_COURIER_ITALIC:
    case FL_COURIER_BOLD_ITALIC:
        return "Courier";
    default:
        return "Helvetica";
    }
}

void applyUiFontRecursively(Fl_Widget* w, Fl_Font font, int size) {
    if (!w) return;
    int clampedSize = std::clamp(size, 8, 36);

    w->labelfont(font);
    w->labelsize(clampedSize);

    if (auto* input = dynamic_cast<Fl_Input_*>(w)) {
        input->textfont(font);
        input->textsize(clampedSize);
    } else if (auto* choice = dynamic_cast<Fl_Choice*>(w)) {
        choice->textfont(font);
        choice->textsize(clampedSize);
    } else if (auto* browser = dynamic_cast<Fl_Browser_*>(w)) {
        browser->textfont(font);
        browser->textsize(clampedSize);
    } else if (auto* menu = dynamic_cast<Fl_Menu_*>(w)) {
        menu->textfont(font);
        menu->textsize(clampedSize);
    }

    if (auto* group = dynamic_cast<Fl_Group*>(w)) {
        for (int i = 0; i < group->children(); ++i) {
            applyUiFontRecursively(group->child(i), font, clampedSize);
        }
    }
}

} // namespace

MainWindow::MainWindow(VerdadApp* app, int W, int H, const char* title)
    : Fl_Double_Window(W, H, title)
    , app_(app)
    , menuBar_(nullptr)
    , statusBar_(nullptr)
    , mainTile_(nullptr)
    , leftPane_(nullptr)
    , studyArea_(nullptr)
    , newStudyTabButton_(nullptr)
    , closeStudyTabButton_(nullptr)
    , studyTabsWidget_(nullptr)
    , contentTile_(nullptr)
    , biblePane_(nullptr)
    , rightPane_(nullptr) {

    size_range(800, 600);

    const int menuH = 25;
    const int statusH = 22;

    menuBar_ = new Fl_Menu_Bar(0, 0, W, menuH);
    buildMenu();

    mainTile_ = new Fl_Tile(0, menuH, W, std::max(20, H - menuH - statusH));
    mainTile_->begin();

    int leftW = W * 25 / 100;
    leftPane_ = new LeftPane(app_, 0, menuH, leftW, mainTile_->h());

    int studyX = leftW;
    int studyW = W - leftW;
    const int newTabButtonW = 24;
    const int closeTabButtonW = 24;
    const int tabButtonPad = 2;
    const int tabsHeaderH = 25;
    studyArea_ = new Fl_Group(studyX, menuH, studyW, mainTile_->h());
    studyArea_->box(FL_FLAT_BOX);
    studyArea_->begin();

    newStudyTabButton_ = new Fl_Button(studyX + tabButtonPad,
                                       menuH + tabButtonPad,
                                       newTabButtonW, 21, "+");
    newStudyTabButton_->callback(onViewNewStudyTab, this);
    newStudyTabButton_->tooltip("Duplicate current study tab");

    closeStudyTabButton_ = new Fl_Button(studyX + studyW - closeTabButtonW - tabButtonPad,
                                         menuH + tabButtonPad,
                                         closeTabButtonW, 21, "x");
    closeStudyTabButton_->callback(onViewCloseStudyTab, this);
    closeStudyTabButton_->tooltip("Close current study tab");

    studyTabsWidget_ = new StyledTabs(
        studyX + newTabButtonW + (tabButtonPad * 2),
        menuH,
        studyW - newTabButtonW - closeTabButtonW - (tabButtonPad * 4),
        tabsHeaderH);
    //studyTabsWidget_->box(FL_FLAT_BOX);
    studyTabsWidget_->selection_color(studyTabsWidget_->color());
    studyTabsWidget_->callback(onStudyTabChange, this);
    // Close the tabs group so subsequent widgets are added to studyArea_,
    // not as tab page children.
    studyTabsWidget_->end();
    studyArea_->begin();

    int contentY = menuH + tabsHeaderH;
    int contentH = std::max(20, mainTile_->h() - tabsHeaderH);
    contentTile_ = new Fl_Tile(studyX, contentY, studyW, contentH);
    contentTile_->begin();

    int bibleW = studyW * 2 / 3;
    int commentaryW = studyW - bibleW;
    biblePane_ = new BiblePane(app_, studyX, contentY, bibleW, contentH);
    rightPane_ = new RightPane(app_, studyX + bibleW, contentY, commentaryW, contentH);

    contentTile_->end();
    studyArea_->end();
    studyArea_->resizable(contentTile_);

    mainTile_->end();

    statusBar_ = new Fl_Box(0, H - statusH, W, statusH);
    statusBar_->box(FL_THIN_UP_BOX);
    statusBar_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    statusBar_->copy_label("Ready");

    resizable(mainTile_);
    end();

    addStudyTab("", "Genesis", 1, 1);
    layoutStudyTabHeader();

    lastUserInteraction_ = std::chrono::steady_clock::now();
    updateStatusBar();
    Fl::add_timeout(0.25, onStatusPoll, this);
    statusPollScheduled_ = true;
}

MainWindow::~MainWindow() {
    if (hoverDelayScheduled_) {
        Fl::remove_timeout(onHoverDelayTimeout, this);
        hoverDelayScheduled_ = false;
    }
    if (prewarmScheduled_) {
        Fl::remove_timeout(onDeferredPrewarm, this);
        prewarmScheduled_ = false;
    }
    if (statusPollScheduled_) {
        Fl::remove_timeout(onStatusPoll, this);
        statusPollScheduled_ = false;
    }
    if (searchHelpWindow_) {
        searchHelpWindow_->hide();
        delete searchHelpWindow_;
        searchHelpWindow_ = nullptr;
    }
    if (searchHelpTextBuffer_) {
        delete searchHelpTextBuffer_;
        searchHelpTextBuffer_ = nullptr;
    }
}

void MainWindow::addStudyTab(const std::string& module,
                             const std::string& book,
                             int chapter,
                             int verse) {
    if (!studyTabsWidget_) return;

    captureActiveTabState();

    StudyContext ctx;
    std::string initModule = trimCopy(module);
    std::string initBook = trimCopy(book);
    int initChapter = chapter > 0 ? chapter : 1;
    int initVerse = verse > 0 ? verse : 1;

    if (initModule.empty() && biblePane_) {
        initModule = biblePane_->currentModule();
    }
    if (initModule.empty()) {
        auto bibles = app_->swordManager().getBibleModules();
        if (!bibles.empty()) initModule = bibles.front().name;
    }
    if (initBook.empty()) initBook = "Genesis";

    ctx.state.module = initModule;
    ctx.state.book = initBook;
    ctx.state.chapter = initChapter;
    ctx.state.verse = initVerse;
    ctx.state.paragraphMode = false;
    ctx.state.parallelMode = false;
    ctx.state.parallelModules.clear();
    ctx.state.biblePaneWidth = biblePane_ ? biblePane_->w() : 0;

    if (rightPane_) {
        ctx.state.commentaryModule = rightPane_->currentCommentaryModule();
        ctx.state.dictionaryModule = rightPane_->currentDictionaryModule();
        ctx.state.generalBookModule = rightPane_->currentGeneralBookModule();
        ctx.state.generalBookKey = rightPane_->currentGeneralBookKey();
        ctx.state.dictionaryPaneHeight = rightPane_->dictionaryPaneHeight();
    }
    if (!ctx.state.commentaryModule.empty()) {
        ctx.state.commentaryReference = initBook + " " + std::to_string(initChapter) +
                                        ":" + std::to_string(initVerse);
    }
    ctx.state.dictionaryActive = false;

    studyTabsWidget_->begin();
    ctx.tabGroup = new Fl_Group(studyTabsWidget_->x(),
                                studyTabsWidget_->y() + studyTabsWidget_->h(),
                                1, 1);
    ctx.tabGroup->copy_label(studyTabLabel(ctx.state).c_str());
    ctx.tabGroup->end();
    studyTabsWidget_->end();

    studyTabs_.push_back(std::move(ctx));
    int idx = static_cast<int>(studyTabs_.size()) - 1;
    studyTabsWidget_->value(studyTabs_[idx].tabGroup);
    activateStudyTab(idx);
}

void MainWindow::duplicateActiveStudyTab() {
    if (activeStudyTab_ < 0 || activeStudyTab_ >= static_cast<int>(studyTabs_.size())) {
        addStudyTab("", "Genesis", 1, 1);
        return;
    }

    captureActiveTabState();
    captureActiveTabDisplayBuffers();
    const StudyContext& src = studyTabs_[activeStudyTab_];

    StudyContext dst;
    dst.state = src.state;
    dst.bibleBuffer = src.bibleBuffer;
    dst.rightBuffer = src.rightBuffer;
    dst.hasBibleBuffer = src.hasBibleBuffer;
    dst.hasRightBuffer = src.hasRightBuffer;

    studyTabsWidget_->begin();
    dst.tabGroup = new Fl_Group(studyTabsWidget_->x(),
                                studyTabsWidget_->y() + studyTabsWidget_->h(),
                                1, 1);
    dst.tabGroup->copy_label(studyTabLabel(dst.state).c_str());
    dst.tabGroup->end();
    studyTabsWidget_->end();

    studyTabs_.push_back(std::move(dst));
    int idx = static_cast<int>(studyTabs_.size()) - 1;
    studyTabsWidget_->value(studyTabs_[idx].tabGroup);
    activateStudyTab(idx);
    layoutStudyTabHeader();
}

void MainWindow::closeActiveStudyTab() {
    if (!studyTabsWidget_ || studyTabs_.size() <= 1) {
        layoutStudyTabHeader();
        return;
    }
    if (activeStudyTab_ < 0 || activeStudyTab_ >= static_cast<int>(studyTabs_.size())) {
        layoutStudyTabHeader();
        return;
    }

    captureActiveTabState();
    int closeIndex = activeStudyTab_;
    Fl_Group* doomedTabGroup = studyTabs_[closeIndex].tabGroup;

    if (doomedTabGroup) {
        studyTabsWidget_->remove(doomedTabGroup);
        delete doomedTabGroup;
    }
    studyTabs_.erase(studyTabs_.begin() + closeIndex);

    if (studyTabs_.empty()) {
        activeStudyTab_ = -1;
        addStudyTab("", "Genesis", 1, 1);
        layoutStudyTabHeader();
        return;
    }

    int nextIndex = std::min(closeIndex, static_cast<int>(studyTabs_.size()) - 1);
    studyTabsWidget_->value(studyTabs_[nextIndex].tabGroup);
    activeStudyTab_ = -1;
    activateStudyTab(nextIndex);
    layoutStudyTabHeader();
}

void MainWindow::clearStudyTabs() {
    if (!studyTabsWidget_) return;

    for (auto& ctx : studyTabs_) {
        if (ctx.tabGroup) {
            studyTabsWidget_->remove(ctx.tabGroup);
            delete ctx.tabGroup;
        }
    }

    studyTabs_.clear();
    activeStudyTab_ = -1;
    studyTabsWidget_->redraw();
    layoutStudyTabHeader();
}

void MainWindow::activateStudyTab(int index) {
    perf::ScopeTimer timer("MainWindow::activateStudyTab");
    if (index < 0 || index >= static_cast<int>(studyTabs_.size())) return;
    if (activeStudyTab_ == index) {
        updateActiveStudyTabLabel();
        updateStatusBar();
        return;
    }

    perf::StepTimer step;
    int from = activeStudyTab_;
    captureActiveTabState();
    perf::logf("activateStudyTab from=%d to=%d captureActiveTabState: %.3f ms",
               from, index, step.elapsedMs());
    step.reset();
    captureActiveTabDisplayBuffers();
    evictOldTabSnapshots();
    perf::logf("activateStudyTab from=%d to=%d captureActiveTabDisplayBuffers: %.3f ms",
               from, index, step.elapsedMs());
    step.reset();
    hideWordInfo();
    perf::logf("activateStudyTab from=%d to=%d hideWordInfo: %.3f ms",
               from, index, step.elapsedMs());
    step.reset();
    activeStudyTab_ = index;
    studyTabs_[index].lastUsed = ++tabUseCounter_;
    applyTabState(index);
    perf::logf("activateStudyTab from=%d to=%d applyTabState: %.3f ms",
               from, index, step.elapsedMs());
    step.reset();
    updateActiveStudyTabLabel();
    perf::logf("activateStudyTab from=%d to=%d updateActiveStudyTabLabel: %.3f ms",
               from, index, step.elapsedMs());
    step.reset();
    layoutStudyTabHeader();
    perf::logf("activateStudyTab from=%d to=%d layoutStudyTabHeader: %.3f ms",
               from, index, step.elapsedMs());
    updateStatusBar();

    scheduleBackgroundPrewarm(0.2);
}

void MainWindow::layoutStudyTabHeader() {
    if (!studyArea_ || !studyTabsWidget_ || !newStudyTabButton_ || !closeStudyTabButton_) {
        return;
    }

    const int tabButtonPad = 2;
    const int newW = newStudyTabButton_->w();
    const int closeW = closeStudyTabButton_->w();

    int headerY = studyArea_->y();
    int headerW = studyArea_->w();
    int tabsH = studyTabsWidget_->h();

    newStudyTabButton_->resize(studyArea_->x() + tabButtonPad,
                               headerY + tabButtonPad,
                               newW,
                               newStudyTabButton_->h());

    closeStudyTabButton_->resize(studyArea_->x() + headerW - closeW - tabButtonPad,
                                 headerY + tabButtonPad,
                                 closeW,
                                 closeStudyTabButton_->h());

    int tabsX = studyArea_->x() + newW + (tabButtonPad * 2);
    int tabsW = headerW - newW - closeW - (tabButtonPad * 4);
    studyTabsWidget_->resize(tabsX, headerY, std::max(40, tabsW), tabsH);

    if (studyTabs_.size() > 1 && activeStudyTab_ >= 0 &&
        activeStudyTab_ < static_cast<int>(studyTabs_.size())) {
        closeStudyTabButton_->activate();
    } else {
        closeStudyTabButton_->deactivate();
    }
    closeStudyTabButton_->redraw();
    newStudyTabButton_->redraw();
    studyTabsWidget_->redraw();
}

std::string MainWindow::studyTabLabel(const StudyTabState& state) const {
    std::string module = state.module;
    if (module.empty()) module = "Bible";
    std::string book = state.book;
    if (book.empty()) book = "Genesis";
    int chapter = std::max(1, state.chapter);
    int verse = std::max(1, state.verse);

    std::string reference = book + " " + std::to_string(chapter) +
                            ":" + std::to_string(verse);
    std::string shortRef = app_ ? app_->swordManager().getShortReference(module, reference)
                                : reference;
    if (shortRef.empty()) {
        shortRef = reference;
    }

    return module + ":" + shortRef;
}

void MainWindow::updateActiveStudyTabLabel() {
    if (activeStudyTab_ < 0 ||
        activeStudyTab_ >= static_cast<int>(studyTabs_.size()) ||
        !studyTabsWidget_) {
        return;
    }

    if (!applyingTabState_) captureActiveTabState();

    StudyContext& ctx = studyTabs_[activeStudyTab_];
    if (!ctx.tabGroup) return;

    std::string label = studyTabLabel(ctx.state);
    ctx.tabGroup->copy_label(label.c_str());
    studyTabsWidget_->redraw();
}

void MainWindow::captureActiveTabState() {
    if (applyingTabState_) return;
    if (activeStudyTab_ < 0 || activeStudyTab_ >= static_cast<int>(studyTabs_.size())) return;

    StudyContext& ctx = studyTabs_[activeStudyTab_];
    if (biblePane_) {
        ctx.state.module = biblePane_->currentModule();
        ctx.state.book = biblePane_->currentBook();
        ctx.state.chapter = biblePane_->currentChapter();
        ctx.state.verse = biblePane_->currentVerse();
        ctx.state.paragraphMode = biblePane_->isParagraphMode();
        ctx.state.parallelMode = biblePane_->isParallel();
        ctx.state.parallelModules = biblePane_->parallelModules();
        ctx.state.biblePaneWidth = biblePane_->w();
        ctx.state.bibleScrollY = biblePane_->scrollY();
    }

    if (rightPane_) {
        ctx.state.commentaryModule = rightPane_->currentCommentaryModule();
        ctx.state.commentaryReference = rightPane_->currentCommentaryReference();
        ctx.state.commentaryScrollY = rightPane_->commentaryScrollY();
        ctx.state.dictionaryModule = rightPane_->currentDictionaryModule();
        ctx.state.dictionaryKey = rightPane_->currentDictionaryKey();
        ctx.state.generalBookModule = rightPane_->currentGeneralBookModule();
        ctx.state.generalBookKey = rightPane_->currentGeneralBookKey();
        ctx.state.dictionaryActive = rightPane_->isDictionaryTabActive();
        ctx.state.dictionaryPaneHeight = rightPane_->dictionaryPaneHeight();
    }
}

void MainWindow::captureActiveTabDisplayBuffers() {
    perf::ScopeTimer timer("MainWindow::captureActiveTabDisplayBuffers");
    if (applyingTabState_) return;
    if (activeStudyTab_ < 0 || activeStudyTab_ >= static_cast<int>(studyTabs_.size())) return;

    StudyContext& ctx = studyTabs_[activeStudyTab_];
    perf::StepTimer step;
    if (biblePane_) {
        BiblePane::DisplayBuffer b = biblePane_->takeDisplayBuffer();
        ctx.bibleBuffer.doc = std::move(b.doc);
        ctx.bibleBuffer.html = std::move(b.html);
        ctx.bibleBuffer.baseUrl = std::move(b.baseUrl);
        ctx.bibleBuffer.scrollY = b.scrollY;
        ctx.bibleBuffer.contentHeight = b.contentHeight;
        ctx.bibleBuffer.renderWidth = b.renderWidth;
        ctx.bibleBuffer.scrollbarVisible = b.scrollbarVisible;
        ctx.bibleBuffer.valid = b.valid;
        ctx.hasBibleBuffer = b.valid;
        perf::logf("captureActiveTabDisplayBuffers tab=%d biblePane_->takeDisplayBuffer: %.3f ms (valid=%d)",
                   activeStudyTab_, step.elapsedMs(), b.valid ? 1 : 0);
        step.reset();
    }
    if (rightPane_) {
        RightPane::DisplayBuffer r = rightPane_->takeDisplayBuffer();

        ctx.rightBuffer.commentary.doc = std::move(r.commentary.doc);
        ctx.rightBuffer.commentary.html = std::move(r.commentary.html);
        ctx.rightBuffer.commentary.baseUrl = std::move(r.commentary.baseUrl);
        ctx.rightBuffer.commentary.scrollY = r.commentary.scrollY;
        ctx.rightBuffer.commentary.contentHeight = r.commentary.contentHeight;
        ctx.rightBuffer.commentary.renderWidth = r.commentary.renderWidth;
        ctx.rightBuffer.commentary.scrollbarVisible = r.commentary.scrollbarVisible;
        ctx.rightBuffer.commentary.valid = r.commentary.valid;

        ctx.rightBuffer.dictionary.doc = std::move(r.dictionary.doc);
        ctx.rightBuffer.dictionary.html = std::move(r.dictionary.html);
        ctx.rightBuffer.dictionary.baseUrl = std::move(r.dictionary.baseUrl);
        ctx.rightBuffer.dictionary.scrollY = r.dictionary.scrollY;
        ctx.rightBuffer.dictionary.contentHeight = r.dictionary.contentHeight;
        ctx.rightBuffer.dictionary.renderWidth = r.dictionary.renderWidth;
        ctx.rightBuffer.dictionary.scrollbarVisible = r.dictionary.scrollbarVisible;
        ctx.rightBuffer.dictionary.valid = r.dictionary.valid;

        ctx.rightBuffer.generalBook.doc = std::move(r.generalBook.doc);
        ctx.rightBuffer.generalBook.html = std::move(r.generalBook.html);
        ctx.rightBuffer.generalBook.baseUrl = std::move(r.generalBook.baseUrl);
        ctx.rightBuffer.generalBook.scrollY = r.generalBook.scrollY;
        ctx.rightBuffer.generalBook.contentHeight = r.generalBook.contentHeight;
        ctx.rightBuffer.generalBook.renderWidth = r.generalBook.renderWidth;
        ctx.rightBuffer.generalBook.scrollbarVisible = r.generalBook.scrollbarVisible;
        ctx.rightBuffer.generalBook.valid = r.generalBook.valid;

        ctx.hasRightBuffer = ctx.rightBuffer.commentary.valid ||
                             ctx.rightBuffer.dictionary.valid ||
                             ctx.rightBuffer.generalBook.valid;
        perf::logf("captureActiveTabDisplayBuffers tab=%d rightPane_->takeDisplayBuffer: %.3f ms (c=%d d=%d g=%d)",
                   activeStudyTab_,
                   step.elapsedMs(),
                   ctx.rightBuffer.commentary.valid ? 1 : 0,
                   ctx.rightBuffer.dictionary.valid ? 1 : 0,
                   ctx.rightBuffer.generalBook.valid ? 1 : 0);
    }
}

void MainWindow::evictOldTabSnapshots() {
    // Count tabs that hold a cached litehtml doc (bible or right pane).
    int cached = 0;
    for (const auto& t : studyTabs_) {
        if (t.hasBibleBuffer || t.hasRightBuffer) ++cached;
    }
    if (cached <= kMaxCachedTabDocs) return;

    // Build list of candidate tabs sorted by lastUsed (oldest first).
    std::vector<int> candidates;
    for (int i = 0; i < static_cast<int>(studyTabs_.size()); ++i) {
        if (i == activeStudyTab_) continue;
        auto& t = studyTabs_[i];
        if (t.hasBibleBuffer || t.hasRightBuffer)
            candidates.push_back(i);
    }
    std::sort(candidates.begin(), candidates.end(),
              [this](int a, int b) {
                  return studyTabs_[a].lastUsed < studyTabs_[b].lastUsed;
              });

    // Evict docs from oldest tabs until we're within budget.
    // Keep HTML + scroll position so re-render on switch is possible.
    for (int idx : candidates) {
        if (cached <= kMaxCachedTabDocs) break;
        auto& t = studyTabs_[idx];
        auto clearDoc = [](HtmlDocBuffer& buf) {
            buf.doc.reset();
        };
        if (t.hasBibleBuffer) {
            clearDoc(t.bibleBuffer);
            t.hasBibleBuffer = false;
        }
        if (t.hasRightBuffer) {
            clearDoc(t.rightBuffer.commentary);
            clearDoc(t.rightBuffer.dictionary);
            clearDoc(t.rightBuffer.generalBook);
            t.hasRightBuffer = false;
        }
        --cached;
        perf::logf("evictOldTabSnapshots: evicted tab %d (lastUsed=%llu)",
                   idx, static_cast<unsigned long long>(t.lastUsed));
    }
}

void MainWindow::applyTabState(int index) {
    perf::ScopeTimer timer("MainWindow::applyTabState");
    if (index < 0 || index >= static_cast<int>(studyTabs_.size())) return;
    if (!biblePane_ || !rightPane_) return;

    StudyContext& ctx = studyTabs_[index];
    applyingTabState_ = true;
    perf::StepTimer step;

    biblePane_->setStudyState(
        ctx.state.module,
        ctx.state.book,
        ctx.state.chapter,
        ctx.state.verse,
        ctx.state.paragraphMode,
        ctx.state.parallelMode,
        ctx.state.parallelModules);
    perf::logf("applyTabState tab=%d biblePane_->setStudyState: %.3f ms",
               index, step.elapsedMs());
    step.reset();
    if (ctx.hasBibleBuffer) {
        BiblePane::DisplayBuffer b;
        b.doc = std::move(ctx.bibleBuffer.doc);
        b.html = std::move(ctx.bibleBuffer.html);
        b.baseUrl = std::move(ctx.bibleBuffer.baseUrl);
        b.scrollY = ctx.bibleBuffer.scrollY;
        b.contentHeight = ctx.bibleBuffer.contentHeight;
        b.renderWidth = ctx.bibleBuffer.renderWidth;
        b.scrollbarVisible = ctx.bibleBuffer.scrollbarVisible;
        b.valid = ctx.bibleBuffer.valid;
        biblePane_->restoreDisplayBuffer(std::move(b));
        ctx.bibleBuffer = HtmlDocBuffer{};
        ctx.hasBibleBuffer = false;
        perf::logf("applyTabState tab=%d biblePane_->restoreDisplayBuffer: %.3f ms",
                   index, step.elapsedMs());
        step.reset();
    } else {
        biblePane_->refresh();
        perf::logf("applyTabState tab=%d biblePane_->refresh: %.3f ms",
                   index, step.elapsedMs());
        step.reset();
    }
    if (leftPane_ && biblePane_) {
        leftPane_->setSearchModule(biblePane_->currentModule());
        perf::logf("applyTabState tab=%d leftPane_->setSearchModule: %.3f ms",
                   index, step.elapsedMs());
        step.reset();
    }

    rightPane_->setStudyState(
        ctx.state.commentaryModule,
        ctx.state.commentaryReference,
        ctx.state.dictionaryModule,
        ctx.state.dictionaryKey,
        ctx.state.generalBookModule,
        ctx.state.generalBookKey,
        ctx.state.dictionaryActive);
    perf::logf("applyTabState tab=%d rightPane_->setStudyState: %.3f ms",
               index, step.elapsedMs());
    step.reset();
    if (ctx.state.dictionaryPaneHeight > 0) {
        rightPane_->setDictionaryPaneHeight(ctx.state.dictionaryPaneHeight);
        perf::logf("applyTabState tab=%d rightPane_->setDictionaryPaneHeight: %.3f ms",
                   index, step.elapsedMs());
        step.reset();
    }
    bool restoredRight = ctx.hasRightBuffer;
    if (restoredRight) {
        RightPane::DisplayBuffer r;
        r.commentary.doc = std::move(ctx.rightBuffer.commentary.doc);
        r.commentary.html = std::move(ctx.rightBuffer.commentary.html);
        r.commentary.baseUrl = std::move(ctx.rightBuffer.commentary.baseUrl);
        r.commentary.scrollY = ctx.rightBuffer.commentary.scrollY;
        r.commentary.contentHeight = ctx.rightBuffer.commentary.contentHeight;
        r.commentary.renderWidth = ctx.rightBuffer.commentary.renderWidth;
        r.commentary.scrollbarVisible = ctx.rightBuffer.commentary.scrollbarVisible;
        r.commentary.valid = ctx.rightBuffer.commentary.valid;

        r.dictionary.doc = std::move(ctx.rightBuffer.dictionary.doc);
        r.dictionary.html = std::move(ctx.rightBuffer.dictionary.html);
        r.dictionary.baseUrl = std::move(ctx.rightBuffer.dictionary.baseUrl);
        r.dictionary.scrollY = ctx.rightBuffer.dictionary.scrollY;
        r.dictionary.contentHeight = ctx.rightBuffer.dictionary.contentHeight;
        r.dictionary.renderWidth = ctx.rightBuffer.dictionary.renderWidth;
        r.dictionary.scrollbarVisible = ctx.rightBuffer.dictionary.scrollbarVisible;
        r.dictionary.valid = ctx.rightBuffer.dictionary.valid;

        r.generalBook.doc = std::move(ctx.rightBuffer.generalBook.doc);
        r.generalBook.html = std::move(ctx.rightBuffer.generalBook.html);
        r.generalBook.baseUrl = std::move(ctx.rightBuffer.generalBook.baseUrl);
        r.generalBook.scrollY = ctx.rightBuffer.generalBook.scrollY;
        r.generalBook.contentHeight = ctx.rightBuffer.generalBook.contentHeight;
        r.generalBook.renderWidth = ctx.rightBuffer.generalBook.renderWidth;
        r.generalBook.scrollbarVisible = ctx.rightBuffer.generalBook.scrollbarVisible;
        r.generalBook.valid = ctx.rightBuffer.generalBook.valid;

        rightPane_->restoreDisplayBuffer(std::move(r), ctx.state.dictionaryActive);
        ctx.rightBuffer = RightDocBuffers{};
        ctx.hasRightBuffer = false;
        perf::logf("applyTabState tab=%d rightPane_->restoreDisplayBuffer: %.3f ms",
                   index, step.elapsedMs());
        step.reset();
    } else {
        rightPane_->refresh();
        rightPane_->setDictionaryTabActive(ctx.state.dictionaryActive);
        perf::logf("applyTabState tab=%d rightPane_->refresh+setDictionaryTabActive: %.3f ms",
                   index, step.elapsedMs());
        step.reset();
    }
    if (ctx.state.bibleScrollY >= 0) {
        biblePane_->setScrollY(ctx.state.bibleScrollY);
        perf::logf("applyTabState tab=%d biblePane_->setScrollY: %.3f ms",
                   index, step.elapsedMs());
        step.reset();
    }
    if (ctx.state.commentaryScrollY >= 0) {
        rightPane_->setCommentaryScrollY(ctx.state.commentaryScrollY);
        perf::logf("applyTabState tab=%d rightPane_->setCommentaryScrollY: %.3f ms",
                   index, step.elapsedMs());
        step.reset();
    }
    biblePane_->redrawChrome();
    rightPane_->redrawChrome();
    perf::logf("applyTabState tab=%d redrawChrome panes: %.3f ms",
               index, step.elapsedMs());
    step.reset();
    if (studyTabsWidget_) {
        studyTabsWidget_->damage(FL_DAMAGE_ALL);
        studyTabsWidget_->redraw();
        perf::logf("applyTabState tab=%d studyTabsWidget redraw: %.3f ms",
                   index, step.elapsedMs());
        step.reset();
    }

    applyingTabState_ = false;
}

void MainWindow::prewarmInactiveTabs() {
    if (!biblePane_ || !rightPane_) return;
    if (studyTabs_.size() <= 1) return;
    if (activeStudyTab_ < 0 || activeStudyTab_ >= static_cast<int>(studyTabs_.size())) {
        return;
    }

    perf::ScopeTimer timer("MainWindow::prewarmInactiveTabs");

    const int original = activeStudyTab_;
    captureActiveTabState();
    captureActiveTabDisplayBuffers();

    for (int i = 0; i < static_cast<int>(studyTabs_.size()); ++i) {
        if (i == original) continue;
        StudyContext& ctx = studyTabs_[i];
        if (ctx.hasBibleBuffer && ctx.hasRightBuffer) continue;

        perf::logf("prewarmInactiveTabs warm tab=%d (hasBible=%d hasRight=%d)",
                   i, ctx.hasBibleBuffer ? 1 : 0, ctx.hasRightBuffer ? 1 : 0);

        activeStudyTab_ = i;
        applyTabState(i);
        captureActiveTabState();
        captureActiveTabDisplayBuffers();
    }

    activeStudyTab_ = original;
    applyTabState(original);
    if (studyTabsWidget_ &&
        original >= 0 &&
        original < static_cast<int>(studyTabs_.size()) &&
        studyTabs_[original].tabGroup) {
        studyTabsWidget_->value(studyTabs_[original].tabGroup);
    }
    updateActiveStudyTabLabel();
    layoutStudyTabHeader();
}

void MainWindow::scheduleBackgroundPrewarm(double delaySec) {
    if (studyTabs_.size() <= 1) return;
    if (activeStudyTab_ < 0 || activeStudyTab_ >= static_cast<int>(studyTabs_.size())) {
        return;
    }

    prewarmAnchorTab_ = activeStudyTab_;
    prewarmCursor_ = 0;

    if (prewarmScheduled_) {
        Fl::remove_timeout(onDeferredPrewarm, this);
        prewarmScheduled_ = false;
    }

    Fl::add_timeout(std::max(0.0, delaySec), onDeferredPrewarm, this);
    prewarmScheduled_ = true;
}

bool MainWindow::runOneBackgroundPrewarmStep() {
    if (!biblePane_ || !rightPane_) return false;
    if (studyTabs_.size() <= 1) return false;
    if (activeStudyTab_ < 0 || activeStudyTab_ >= static_cast<int>(studyTabs_.size())) {
        return false;
    }

    const int original = activeStudyTab_;
    const int tabCount = static_cast<int>(studyTabs_.size());

    int target = -1;
    for (int n = 0; n < tabCount; ++n) {
        const int idx = (prewarmCursor_ + n) % tabCount;
        if (idx == original) continue;

        const StudyContext& ctx = studyTabs_[idx];
        if (ctx.hasBibleBuffer && ctx.hasRightBuffer) continue;

        target = idx;
        prewarmCursor_ = (idx + 1) % tabCount;
        break;
    }

    if (target < 0) return false;

    perf::logf("backgroundPrewarm warm tab=%d (hasBible=%d hasRight=%d)",
               target,
               studyTabs_[target].hasBibleBuffer ? 1 : 0,
               studyTabs_[target].hasRightBuffer ? 1 : 0);

    captureActiveTabState();
    captureActiveTabDisplayBuffers();

    activeStudyTab_ = target;
    applyTabState(target);
    captureActiveTabState();
    captureActiveTabDisplayBuffers();

    activeStudyTab_ = original;
    applyTabState(original);
    if (studyTabsWidget_ &&
        original >= 0 &&
        original < static_cast<int>(studyTabs_.size()) &&
        studyTabs_[original].tabGroup) {
        studyTabsWidget_->value(studyTabs_[original].tabGroup);
    }
    updateActiveStudyTabLabel();
    layoutStudyTabHeader();

    for (int i = 0; i < tabCount; ++i) {
        if (i == original) continue;
        if (!(studyTabs_[i].hasBibleBuffer && studyTabs_[i].hasRightBuffer)) {
            return true;
        }
    }
    return false;
}

void MainWindow::onDeferredPrewarm(void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;
    self->prewarmScheduled_ = false;

    if (!self->shown()) {
        self->scheduleBackgroundPrewarm(0.15);
        return;
    }
    if (self->applyingTabState_) {
        self->scheduleBackgroundPrewarm(0.08);
        return;
    }
    if (self->studyTabs_.size() <= 1) return;
    if (self->activeStudyTab_ < 0 ||
        self->activeStudyTab_ >= static_cast<int>(self->studyTabs_.size())) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto sinceInput = now - self->lastUserInteraction_;
    if (sinceInput < std::chrono::milliseconds(350)) {
        self->scheduleBackgroundPrewarm(0.2);
        return;
    }

    if (self->activeStudyTab_ != self->prewarmAnchorTab_) {
        self->prewarmAnchorTab_ = self->activeStudyTab_;
        self->prewarmCursor_ = 0;
    }

    if (self->runOneBackgroundPrewarmStep()) {
        self->scheduleBackgroundPrewarm(0.03);
    }
}

void MainWindow::noteUserInteraction() {
    lastUserInteraction_ = std::chrono::steady_clock::now();
}

void MainWindow::navigateTo(const std::string& reference) {
    if (biblePane_) {
        biblePane_->navigateToReference(reference);
        std::string module = trimCopy(biblePane_->currentModule());
        std::string ref = trimCopy(reference);
        if (!module.empty() && !ref.empty()) {
            showTransientStatus("Opened " + buildModuleRefLabel(app_, module, ref), 2.2);
        }
    }
}

void MainWindow::navigateTo(const std::string& module, const std::string& reference) {
    if (biblePane_) {
        biblePane_->setModule(module);
        biblePane_->navigateToReference(reference);
        std::string mod = trimCopy(module);
        std::string ref = trimCopy(reference);
        if (!mod.empty() && !ref.empty()) {
            showTransientStatus("Opened " + buildModuleRefLabel(app_, mod, ref), 2.2);
        }
    }
}

void MainWindow::openInNewStudyTab(const std::string& module,
                                   const std::string& reference) {
    std::string refText = trimCopy(reference);
    std::string mod = trimCopy(module);
    if (refText.empty()) return;

    SwordManager::VerseRef parsed;
    bool parsedOk = false;
    try {
        parsed = SwordManager::parseVerseRef(refText);
        parsedOk = (!parsed.book.empty() && parsed.chapter > 0 && parsed.verse > 0);
    } catch (...) {
        parsed = SwordManager::VerseRef{};
    }

    std::string book = parsedOk ? parsed.book : "Genesis";
    int chapter = parsedOk ? parsed.chapter : 1;
    int verse = parsedOk ? parsed.verse : 1;

    addStudyTab(mod, book, chapter, verse);
    if (!mod.empty()) {
        navigateTo(mod, refText);
    } else {
        navigateTo(refText);
    }
}

void MainWindow::showCommentary(const std::string& reference) {
    if (rightPane_) {
        rightPane_->showCommentary(reference);
        if (!applyingTabState_) captureActiveTabState();
    }
}

void MainWindow::showDictionary(const std::string& key) {
    if (rightPane_) {
        rightPane_->showDictionaryEntry(key);
        if (!applyingTabState_) captureActiveTabState();
    }
}

void MainWindow::showWordInfo(const std::string& word, const std::string& href,
                               const std::string& strong, const std::string& morph,
                               int /*screenX*/, int /*screenY*/) {
    pendingWordInfo_.word = word;
    pendingWordInfo_.href = href;
    pendingWordInfo_.strong = strong;
    pendingWordInfo_.morph = morph;
    pendingWordInfo_.tabIndex = activeStudyTab_;

    if (hoverDelayScheduled_) {
        Fl::remove_timeout(onHoverDelayTimeout, this);
    }
    double hoverDelaySec = 1.0;
    if (app_) {
        hoverDelaySec = std::clamp(
            app_->appearanceSettings().hoverDelayMs / 1000.0,
            0.1, 5.0);
    }
    Fl::add_timeout(hoverDelaySec, onHoverDelayTimeout, this);
    hoverDelayScheduled_ = true;
}

void MainWindow::hideWordInfo() {
    if (hoverDelayScheduled_) {
        Fl::remove_timeout(onHoverDelayTimeout, this);
        hoverDelayScheduled_ = false;
    }
    pendingWordInfo_ = PendingWordInfo{};
}

void MainWindow::onHoverDelayTimeout(void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;
    self->hoverDelayScheduled_ = false;
    self->applyPendingWordInfo();
}

void MainWindow::applyPendingWordInfo() {
    if (pendingWordInfo_.tabIndex != activeStudyTab_) return;

    std::string strongNum = pendingWordInfo_.strong;
    if (strongNum.empty() && pendingWordInfo_.href.find("strongs:") == 0) {
        strongNum = pendingWordInfo_.href.substr(8);
    } else if (strongNum.empty() && pendingWordInfo_.href.find("strong:") == 0) {
        strongNum = pendingWordInfo_.href.substr(7);
    }
    while (!strongNum.empty() &&
           (strongNum.front() == '/' ||
            std::isspace(static_cast<unsigned char>(strongNum.front())))) {
        strongNum.erase(strongNum.begin());
    }

    std::string morphCode = pendingWordInfo_.morph;
    if (morphCode.empty() && pendingWordInfo_.href.find("morph:") == 0) {
        morphCode = pendingWordInfo_.href.substr(6);
    }

    std::vector<std::string> strongTokens = extractStrongsTokens(strongNum);
    std::string morphKey = trimCopy(morphCode);
    if (strongTokens.empty() && morphKey.empty()) return;

    std::ostringstream html;
    html << "<div class=\"mag-lite\">";
    bool hasDefinition = false;
    std::string displayWord = trimCopy(pendingWordInfo_.word);
    if (!displayWord.empty()) {
        html << "<span class=\"mag-line mag-wordline\">"
             << htmlEscape(displayWord) << "</span>";
        html << "<span class=\"mag-line mag-gap\"></span>";
    }

    for (const auto& tok : strongTokens) {
        html << "<span class=\"mag-line mag-label\">Strong's "
             << htmlEscape(tok) << "</span>";
        std::string def = app_->swordManager().getStrongsDefinition(tok);
        if (!def.empty()) {
            hasDefinition = true;
            appendEscapedTextLines(html, def, "mag-defline");
            html << "<span class=\"mag-line mag-gap\"></span>";
        }
    }

    if (!morphKey.empty()) {
        html << "<span class=\"mag-line mag-morph-label\">Morph: "
             << htmlEscape(morphKey) << "</span>";
        std::string def = app_->swordManager().getMorphDefinition(morphKey);
        if (!def.empty()) {
            hasDefinition = true;
            appendEscapedTextLines(html, def, "mag-defline");
        }
    }

    if (!hasDefinition) return;
    html << "</div>";

    if (leftPane_) {
        leftPane_->setPreviewText(html.str());
    }
}

void MainWindow::showSearchResults(const std::string& query,
                                   const std::string& moduleOverride) {
    if (leftPane_) {
        std::string strongTok = extractStrongsToken(query);
        if (!strongTok.empty()) {
            showTransientStatus("Strong's lookup: " + strongTok, 2.8);
        } else {
            std::string q = trimCopy(query);
            if (!q.empty()) {
                if (q.size() > 40) q = q.substr(0, 40) + "...";
                showTransientStatus("Search: " + q, 2.0);
            }
        }
        leftPane_->doSearch(query, moduleOverride);
        leftPane_->showSearchTab();
    }
}

void MainWindow::showTransientStatus(const std::string& text, double seconds) {
    std::string msg = trimCopy(text);
    if (msg.empty()) return;

    transientStatusText_ = msg;
    transientStatusUntil_ = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(
                                static_cast<int>(std::clamp(seconds, 0.4, 10.0) * 1000.0));
    updateStatusBar();
}

void MainWindow::refresh() {
    if (leftPane_) leftPane_->refresh();
    if (biblePane_) biblePane_->refresh();
    if (rightPane_) rightPane_->refresh();
    captureActiveTabState();
}

void MainWindow::applyAppearanceSettings(Fl_Font appFont,
                                         int appFontSize,
                                         const std::string& textCssOverride) {
    const int clampedSize = std::clamp(appFontSize, 8, 36);
    const bool cssChanged =
        appearanceApplied_ && (textCssOverride != lastAppliedTextCss_);
    const bool uiFontChanged =
        appearanceApplied_ &&
        (appFont != lastAppliedAppFont_ ||
         clampedSize != lastAppliedAppFontSize_);

    // Re-apply widget fonts only when needed.
    if (!appearanceApplied_ || uiFontChanged) {
        applyUiFontRecursively(this, appFont, clampedSize);
    }

    // Re-apply HTML CSS only when needed.
    if (!appearanceApplied_ || cssChanged) {
        if (leftPane_) leftPane_->setHtmlStyleOverride(textCssOverride);
        if (biblePane_) biblePane_->setHtmlStyleOverride(textCssOverride);
        if (rightPane_) rightPane_->setHtmlStyleOverride(textCssOverride);
    }

    // Only drop inactive tab buffers when text rendering style actually changed.
    if (cssChanged) {
        for (size_t i = 0; i < studyTabs_.size(); ++i) {
            if (static_cast<int>(i) == activeStudyTab_) continue;
            studyTabs_[i].bibleBuffer = HtmlDocBuffer{};
            studyTabs_[i].rightBuffer = RightDocBuffers{};
            studyTabs_[i].hasBibleBuffer = false;
            studyTabs_[i].hasRightBuffer = false;
        }
    }

    lastAppliedAppFont_ = appFont;
    lastAppliedAppFontSize_ = clampedSize;
    lastAppliedTextCss_ = textCssOverride;
    appearanceApplied_ = true;

    redraw();
}

MainWindow::SessionState MainWindow::captureSessionState() {
    captureActiveTabState();

    SessionState state;
    state.windowX = x();
    state.windowY = y();
    state.windowW = w();
    state.windowH = h();
    if (leftPane_) {
        state.leftPaneWidth = leftPane_->w();
        state.leftPanePreviewHeight = leftPane_->previewHeight();
    }
    state.activeStudyTab = activeStudyTab_;

    int sharedBiblePaneWidth = biblePane_ ? biblePane_->w() : 0;
    for (const auto& ctx : studyTabs_) {
        StudyTabState tab = ctx.state;
        if (tab.biblePaneWidth <= 0) {
            tab.biblePaneWidth = sharedBiblePaneWidth;
        }
        state.studyTabs.push_back(tab);
    }

    return state;
}

void MainWindow::restoreSessionState(const SessionState& state) {
    // Restore window geometry first.
    int rx = state.windowX >= 0 ? state.windowX : x();
    int ry = state.windowY >= 0 ? state.windowY : y();
    int rw = state.windowW > 100 ? state.windowW : w();
    int rh = state.windowH > 100 ? state.windowH : h();
    resize(rx, ry, rw, rh);

    // Restore top-level splitter width.
    if (mainTile_ && leftPane_ && studyArea_) {
        int menuH = menuBar_ ? menuBar_->h() : 25;
        int statusH = statusBar_ ? statusBar_->h() : 22;
        int totalW = w();
        int totalH = std::max(20, h() - menuH - statusH);
        int minLeft = 180;
        int maxLeft = std::max(minLeft, totalW - 220);
        int leftW = std::clamp(state.leftPaneWidth, minLeft, maxLeft);

        static_cast<Fl_Widget*>(leftPane_)->resize(0, menuH, leftW, totalH);
        studyArea_->resize(leftW, menuH, totalW - leftW, totalH);
        if (state.leftPanePreviewHeight > 0) {
            leftPane_->setPreviewHeight(state.leftPanePreviewHeight);
        }

        if (studyTabsWidget_) {
            const int tabsHeaderH = studyTabsWidget_->h();
            layoutStudyTabHeader();
            if (contentTile_) {
                int contentY = studyArea_->y() + tabsHeaderH;
                int contentH = std::max(20, studyArea_->h() - tabsHeaderH);
                contentTile_->resize(studyArea_->x(), contentY, studyArea_->w(), contentH);
            }
        }
    }

    clearStudyTabs();

    if (state.studyTabs.empty() || !studyTabsWidget_) {
        addStudyTab("", "Genesis", 1, 1);
        redraw();
        return;
    }

    for (const auto& tabState : state.studyTabs) {
        StudyContext ctx;
        ctx.state = tabState;

        studyTabsWidget_->begin();
        ctx.tabGroup = new Fl_Group(studyTabsWidget_->x(),
                                    studyTabsWidget_->y() + studyTabsWidget_->h(),
                                    1, 1);
        ctx.tabGroup->copy_label(studyTabLabel(ctx.state).c_str());
        ctx.tabGroup->end();
        studyTabsWidget_->end();

        studyTabs_.push_back(std::move(ctx));
    }

    if (contentTile_ && biblePane_ && rightPane_) {
        int desired = 0;
        if (!state.studyTabs.empty()) {
            int idx = std::clamp(state.activeStudyTab, 0,
                                 static_cast<int>(state.studyTabs.size()) - 1);
            desired = state.studyTabs[idx].biblePaneWidth;
        }
        if (desired <= 0) {
            for (const auto& t : state.studyTabs) {
                if (t.biblePaneWidth > 0) {
                    desired = t.biblePaneWidth;
                    break;
                }
            }
        }
        if (desired > 0) {
            int tileX = contentTile_->x();
            int tileY = contentTile_->y();
            int tileW = contentTile_->w();
            int tileH = contentTile_->h();
            int bibleW = std::clamp(desired, 140, std::max(140, tileW - 140));
            static_cast<Fl_Widget*>(biblePane_)->resize(tileX, tileY, bibleW, tileH);
            static_cast<Fl_Widget*>(rightPane_)
                ->resize(tileX + bibleW, tileY, tileW - bibleW, tileH);
            contentTile_->redraw();
        }
    }

    if (!studyTabs_.empty()) {
        int idx = std::clamp(state.activeStudyTab, 0,
                             static_cast<int>(studyTabs_.size()) - 1);
        studyTabsWidget_->value(studyTabs_[idx].tabGroup);
        activeStudyTab_ = -1;
        activateStudyTab(idx);
    }

    redraw();
    scheduleBackgroundPrewarm(0.1);
    updateStatusBar();
}

void MainWindow::resize(int X, int Y, int W, int H) {
    Fl_Double_Window::resize(X, Y, W, H);

    const int menuH = menuBar_ ? menuBar_->h() : 25;
    const int statusH = statusBar_ ? statusBar_->h() : 22;
    const int bodyH = std::max(20, H - menuH - statusH);

    if (menuBar_) {
        menuBar_->resize(0, 0, W, menuH);
    }
    if (mainTile_) {
        mainTile_->resize(0, menuH, W, bodyH);
    }
    if (statusBar_) {
        statusBar_->resize(0, menuH + bodyH, W, statusH);
    }
}

void MainWindow::updateStatusBar() {
    if (!statusBar_) return;

    std::string text;
    auto now = std::chrono::steady_clock::now();

    if (!transientStatusText_.empty() && now < transientStatusUntil_) {
        text = transientStatusText_;
    } else if (!transientStatusText_.empty()) {
        transientStatusText_.clear();
    }

    if (text.empty() && app_ && app_->searchIndexer()) {
        std::string module;
        int pct = 0;
        if (app_->searchIndexer()->activeIndexingTask(module, pct)) {
            text = "Indexing " + module + ": " + std::to_string(pct) + "%";
        } else if (app_->searchIndexer()->isIndexing()) {
            text = "Indexing modules...";
        }
    }

    if (text.empty()) {
        std::string module;
        std::string reference;
        if (biblePane_) {
            module = trimCopy(biblePane_->currentModule());
            std::string book = trimCopy(biblePane_->currentBook());
            int chapter = std::max(1, biblePane_->currentChapter());
            int verse = std::max(1, biblePane_->currentVerse());
            if (!book.empty()) {
                reference = book + " " + std::to_string(chapter) + ":" +
                            std::to_string(verse);
            }
        }

        if (!module.empty() && !reference.empty() && app_) {
            text = "Ready | " + buildModuleRefLabel(app_, module, reference);
        } else {
            text = "Ready";
        }
    }

    if (text != lastStatusBarText_) {
        lastStatusBarText_ = text;
        statusBar_->copy_label(lastStatusBarText_.c_str());
        statusBar_->redraw();
    }
}

void MainWindow::onStatusPoll(void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;
    if (!self->shown()) return;

    self->updateStatusBar();
    Fl::repeat_timeout(0.25, onStatusPoll, self);
    self->statusPollScheduled_ = true;
}

int MainWindow::handle(int event) {
    switch (event) {
    case FL_PUSH:
    case FL_RELEASE:
    case FL_DRAG:
    case FL_MOUSEWHEEL:
    case FL_SHORTCUT:
    case FL_KEYDOWN:
    case FL_KEYUP:
        noteUserInteraction();
        break;
    default:
        break;
    }

    if (event == FL_DRAG || event == FL_RELEASE) {
        if (newStudyTabButton_) newStudyTabButton_->redraw();
        if (closeStudyTabButton_) closeStudyTabButton_->redraw();
        if (studyTabsWidget_) {
            studyTabsWidget_->damage(FL_DAMAGE_ALL);
            studyTabsWidget_->redraw();
        }
        if (leftPane_) leftPane_->redrawChrome();
        if (biblePane_) biblePane_->redrawChrome();
        if (rightPane_) rightPane_->redrawChrome();
        if (studyArea_) {
            studyArea_->damage(FL_DAMAGE_ALL);
            studyArea_->redraw();
        }
        if (contentTile_) {
            contentTile_->damage(FL_DAMAGE_ALL);
            contentTile_->redraw();
        }
        damage(FL_DAMAGE_ALL);
        redraw();
    }

    if (event == FL_PUSH || event == FL_RELEASE || event == FL_MOUSEWHEEL) {
        updateStatusBar();
    }

    if (event == FL_SHORTCUT) {
        if (Fl::event_key() == FL_Escape) {
            hideWordInfo();
            return 1;
        }
    }
    return Fl_Double_Window::handle(event);
}

void MainWindow::onStudyTabChange(Fl_Widget* /*w*/, void* data) {
    perf::ScopeTimer timer("MainWindow::onStudyTabChange");
    auto* self = static_cast<MainWindow*>(data);
    if (!self || !self->studyTabsWidget_) return;

    Fl_Widget* active = self->studyTabsWidget_->value();
    if (!active) return;

    for (size_t i = 0; i < self->studyTabs_.size(); ++i) {
        if (self->studyTabs_[i].tabGroup == active) {
            perf::logf("onStudyTabChange target index=%zu", i);
            self->activateStudyTab(static_cast<int>(i));
            break;
        }
    }
}

void MainWindow::buildMenu() {
    menuBar_->add("&File/&Module Manager...", 0, onFileModuleManager, this);
    menuBar_->add("&File/&Quit", FL_CTRL + 'q', onFileQuit, this);
    menuBar_->add("&Navigate/&Go to Verse...", FL_CTRL + 'g', onNavigateGo, this);
    menuBar_->add("&View/&Parallel Bibles", FL_CTRL + 'p', onViewParallel, this);
    menuBar_->add("&View/&Settings...", 0, onViewSettings, this);
    menuBar_->add("&View/&New Study Tab", FL_CTRL + 't', onViewNewStudyTab, this);
    menuBar_->add("&Help/&Search Help", FL_F + 1, onHelpSearch, this);
    menuBar_->add("&Help/&About Verdad", 0, onHelpAbout, this);
}

void MainWindow::onFileQuit(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    self->hide();
}

void MainWindow::onFileModuleManager(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self || !self->app_) return;

    ModuleManagerDialog dlg(self->app_);
    dlg.openModal();
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
        self->biblePane_->redrawChrome();
    }
    if (self->rightPane_) {
        self->rightPane_->redrawChrome();
    }
}

void MainWindow::onViewSettings(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self || !self->app_) return;

    auto current = self->app_->appearanceSettings();

    Fl_Double_Window* dlg = new Fl_Double_Window(420, 285, "Settings");
    dlg->set_modal();
    dlg->begin();

    Fl_Box* appFontLabel = new Fl_Box(20, 20, 140, 24, "Application font:");
    appFontLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Choice* appFontChoice = new Fl_Choice(170, 20, 220, 24);
    appFontChoice->add("Helvetica");
    appFontChoice->add("Times");
    appFontChoice->add("Courier");
    int appFontIdx = appFontChoice->find_index(current.appFontName.c_str());
    appFontChoice->value(appFontIdx >= 0 ? appFontIdx : 0);

    Fl_Box* appSizeLabel = new Fl_Box(20, 54, 140, 24, "Application size:");
    appSizeLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Spinner* appSizeSpinner = new Fl_Spinner(170, 54, 80, 24);
    appSizeSpinner->minimum(8);
    appSizeSpinner->maximum(36);
    appSizeSpinner->step(1);
    appSizeSpinner->value(current.appFontSize);

    Fl_Box* textFontLabel = new Fl_Box(20, 96, 140, 24, "Text font:");
    textFontLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Choice* textFontChoice = new Fl_Choice(170, 96, 220, 24);
    textFontChoice->add("DejaVu Serif");
    textFontChoice->add("DejaVu Sans");
    textFontChoice->add("Liberation Serif");
    textFontChoice->add("Liberation Sans");
    textFontChoice->add("Times New Roman");
    textFontChoice->add("Arial");
    int textFontIdx = textFontChoice->find_index(current.textFontFamily.c_str());
    textFontChoice->value(textFontIdx >= 0 ? textFontIdx : 0);

    Fl_Box* textSizeLabel = new Fl_Box(20, 130, 140, 24, "Text size:");
    textSizeLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Spinner* textSizeSpinner = new Fl_Spinner(170, 130, 80, 24);
    textSizeSpinner->minimum(8);
    textSizeSpinner->maximum(36);
    textSizeSpinner->step(1);
    textSizeSpinner->value(current.textFontSize);

    Fl_Box* hoverLabel = new Fl_Box(20, 164, 140, 24, "Hover delay (ms):");
    hoverLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Spinner* hoverDelaySpinner = new Fl_Spinner(170, 164, 90, 24);
    hoverDelaySpinner->minimum(100);
    hoverDelaySpinner->maximum(5000);
    hoverDelaySpinner->step(100);
    hoverDelaySpinner->value(current.hoverDelayMs);

    Fl_Button* cancelBtn = new Fl_Button(220, 235, 80, 28, "Cancel");
    Fl_Return_Button* applyBtn = new Fl_Return_Button(310, 235, 80, 28, "Apply");

    struct DialogState {
        bool accepted = false;
    };
    auto* state = new DialogState();

    cancelBtn->callback(
        [](Fl_Widget* w, void* data) {
            auto* st = static_cast<DialogState*>(data);
            st->accepted = false;
            if (w && w->window()) w->window()->hide();
        },
        state);

    applyBtn->callback(
        [](Fl_Widget* w, void* data) {
            auto* st = static_cast<DialogState*>(data);
            st->accepted = true;
            if (w && w->window()) w->window()->hide();
        },
        state);

    dlg->end();
    dlg->show();
    while (dlg->shown()) {
        Fl::wait();
    }

    if (state->accepted) {
        VerdadApp::AppearanceSettings updated = current;

        const Fl_Menu_Item* appFontItem = appFontChoice->mvalue();
        if (appFontItem && appFontItem->label()) {
            updated.appFontName = appFontItem->label();
        } else {
            updated.appFontName = appFontName(appFontFromName(current.appFontName));
        }
        updated.appFontSize = static_cast<int>(appSizeSpinner->value());

        const Fl_Menu_Item* textFontItem = textFontChoice->mvalue();
        if (textFontItem && textFontItem->label()) {
            updated.textFontFamily = textFontItem->label();
        }
        updated.textFontSize = static_cast<int>(textSizeSpinner->value());
        updated.hoverDelayMs = static_cast<int>(hoverDelaySpinner->value());

        self->app_->setAppearanceSettings(updated);
    }

    delete state;
    delete dlg;
}

void MainWindow::onViewNewStudyTab(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;
    self->duplicateActiveStudyTab();
}

void MainWindow::onViewCloseStudyTab(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;
    self->closeActiveStudyTab();
}

void MainWindow::onHelpSearch(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;
    self->showSearchHelpWindow();
}

void MainWindow::showSearchHelpWindow() {
    if (!searchHelpWindow_) {
        searchHelpWindow_ = new Fl_Double_Window(780, 600, "Search Help");
        searchHelpWindow_->begin();

        auto* helpText = new Fl_Text_Display(10, 10, 760, 542);
        helpText->box(FL_DOWN_BOX);
        helpText->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
        helpText->textfont(FL_HELVETICA);
        helpText->textsize(14);

        searchHelpTextBuffer_ = new Fl_Text_Buffer();
        helpText->buffer(searchHelpTextBuffer_);

        auto* closeBtn = new Fl_Return_Button(680, 560, 90, 28, "Close");
        closeBtn->callback(
            [](Fl_Widget* w, void* /*data*/) {
                if (w && w->window()) w->window()->hide();
            },
            nullptr);

        searchHelpWindow_->callback(
            [](Fl_Widget* w, void* /*data*/) {
                if (w) w->hide();
            },
            nullptr);
        searchHelpWindow_->resizable(helpText);
        searchHelpWindow_->end();
    }

    if (searchHelpTextBuffer_) {
        searchHelpTextBuffer_->text(kSearchHelpText);
    }

    searchHelpWindow_->show();
    searchHelpWindow_->take_focus();
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
