#include "ui/MainWindow.h"
#include "app/VerdadApp.h"
#include "ui/LeftPane.h"
#include "ui/BiblePane.h"
#include "ui/RightPane.h"
#include "sword/SwordManager.h"

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace verdad {
namespace {

class AutoRedrawTabs : public Fl_Tabs {
public:
    AutoRedrawTabs(int X, int Y, int W, int H, const char* L = nullptr)
        : Fl_Tabs(X, Y, W, H, L) {}

    void draw() override {
        fl_push_clip(x(), y(), w(), h());
        Fl_Color bg = parent() ? parent()->color() : FL_BACKGROUND_COLOR;
        fl_color(bg);
        fl_rectf(x(), y(), w(), h());
        fl_pop_clip();
        Fl_Tabs::draw();
    }

    void resize(int X, int Y, int W, int H) override {
        Fl_Tabs::resize(X, Y, W, H);
        damage(FL_DAMAGE_ALL);
        redraw();
    }
};

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

std::string abbreviateBookName(const std::string& bookName) {
    static const std::unordered_map<std::string, std::string> kBookAbbrev = {
        {"Genesis", "Gen"}, {"Exodus", "Exod"}, {"Leviticus", "Lev"},
        {"Numbers", "Num"}, {"Deuteronomy", "Deut"}, {"Joshua", "Josh"},
        {"Judges", "Judg"}, {"Ruth", "Ruth"}, {"1 Samuel", "1Sam"},
        {"2 Samuel", "2Sam"}, {"1 Kings", "1Kgs"}, {"2 Kings", "2Kgs"},
        {"1 Chronicles", "1Chr"}, {"2 Chronicles", "2Chr"}, {"Ezra", "Ezra"},
        {"Nehemiah", "Neh"}, {"Esther", "Esth"}, {"Job", "Job"},
        {"Psalms", "Ps"}, {"Psalm", "Ps"}, {"Proverbs", "Prov"},
        {"Ecclesiastes", "Eccl"}, {"Song of Solomon", "Song"},
        {"Song of Songs", "Song"}, {"Isaiah", "Isa"}, {"Jeremiah", "Jer"},
        {"Lamentations", "Lam"}, {"Ezekiel", "Ezek"}, {"Daniel", "Dan"},
        {"Hosea", "Hos"}, {"Joel", "Joel"}, {"Amos", "Amos"},
        {"Obadiah", "Obad"}, {"Jonah", "Jonah"}, {"Micah", "Mic"},
        {"Nahum", "Nah"}, {"Habakkuk", "Hab"}, {"Zephaniah", "Zeph"},
        {"Haggai", "Hag"}, {"Zechariah", "Zech"}, {"Malachi", "Mal"},
        {"Matthew", "Matt"}, {"Mark", "Mark"}, {"Luke", "Luke"},
        {"John", "John"}, {"Acts", "Acts"}, {"Romans", "Rom"},
        {"1 Corinthians", "1Cor"}, {"2 Corinthians", "2Cor"},
        {"Galatians", "Gal"}, {"Ephesians", "Eph"}, {"Philippians", "Phil"},
        {"Colossians", "Col"}, {"1 Thessalonians", "1Thess"},
        {"2 Thessalonians", "2Thess"}, {"1 Timothy", "1Tim"},
        {"2 Timothy", "2Tim"}, {"Titus", "Titus"}, {"Philemon", "Phlm"},
        {"Hebrews", "Heb"}, {"James", "Jas"}, {"1 Peter", "1Pet"},
        {"2 Peter", "2Pet"}, {"1 John", "1John"}, {"2 John", "2John"},
        {"3 John", "3John"}, {"Jude", "Jude"}, {"Revelation", "Rev"}
    };

    auto it = kBookAbbrev.find(bookName);
    if (it != kBookAbbrev.end()) return it->second;

    // Fallback for non-canonical names: keep numeric prefix and first 3 letters.
    std::istringstream ss(bookName);
    std::string first;
    std::string second;
    ss >> first;
    ss >> second;
    if (first.empty()) return "Gen";

    if (!first.empty() && std::isdigit(static_cast<unsigned char>(first[0])) &&
        !second.empty()) {
        std::string out = first;
        std::string stem = second.substr(0, std::min<size_t>(3, second.size()));
        if (!stem.empty()) {
            stem[0] = static_cast<char>(std::toupper(
                static_cast<unsigned char>(stem[0])));
            for (size_t i = 1; i < stem.size(); ++i) {
                stem[i] = static_cast<char>(std::tolower(
                    static_cast<unsigned char>(stem[i])));
            }
        }
        return out + stem;
    }

    if (first.size() <= 4) return first;
    return first.substr(0, 4);
}

} // namespace

MainWindow::MainWindow(VerdadApp* app, int W, int H, const char* title)
    : Fl_Double_Window(W, H, title)
    , app_(app)
    , menuBar_(nullptr)
    , mainTile_(nullptr)
    , leftPane_(nullptr)
    , studyArea_(nullptr)
    , newStudyTabButton_(nullptr)
    , studyTabsWidget_(nullptr)
    , contentTile_(nullptr)
    , biblePane_(nullptr)
    , rightPane_(nullptr) {

    size_range(800, 600);

    int menuH = 25;

    menuBar_ = new Fl_Menu_Bar(0, 0, W, menuH);
    buildMenu();

    mainTile_ = new Fl_Tile(0, menuH, W, H - menuH);
    mainTile_->begin();

    int leftW = W * 25 / 100;
    leftPane_ = new LeftPane(app_, 0, menuH, leftW, H - menuH);

    int studyX = leftW;
    int studyW = W - leftW;
    const int newTabButtonW = 24;
    const int newTabButtonPad = 2;
    const int tabsHeaderH = 25;
    studyArea_ = new Fl_Group(studyX, menuH, studyW, H - menuH);
    studyArea_->box(FL_FLAT_BOX);
    studyArea_->begin();

    newStudyTabButton_ = new Fl_Button(studyX + newTabButtonPad,
                                       menuH + newTabButtonPad,
                                       newTabButtonW, 21, "+");
    newStudyTabButton_->callback(onViewNewStudyTab, this);
    newStudyTabButton_->tooltip("Duplicate current study tab");

    studyTabsWidget_ = new AutoRedrawTabs(
        studyX + newTabButtonW + (newTabButtonPad * 2),
        menuH,
        studyW - newTabButtonW - (newTabButtonPad * 2),
        tabsHeaderH);
    //studyTabsWidget_->box(FL_FLAT_BOX);
    studyTabsWidget_->selection_color(studyTabsWidget_->color());
    studyTabsWidget_->callback(onStudyTabChange, this);
    // Close the tabs group so subsequent widgets are added to studyArea_,
    // not as tab page children.
    studyTabsWidget_->end();
    studyArea_->begin();

    int contentY = menuH + tabsHeaderH;
    int contentH = std::max(20, H - contentY);
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
    resizable(mainTile_);
    end();

    addStudyTab("", "Genesis", 1, 1);
}

MainWindow::~MainWindow() {
    if (hoverDelayScheduled_) {
        Fl::remove_timeout(onHoverDelayTimeout, this);
        hoverDelayScheduled_ = false;
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
    const StudyContext& src = studyTabs_[activeStudyTab_];

    StudyContext dst;
    dst.state = src.state;

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
}

void MainWindow::activateStudyTab(int index) {
    if (index < 0 || index >= static_cast<int>(studyTabs_.size())) return;
    if (activeStudyTab_ == index) {
        updateActiveStudyTabLabel();
        return;
    }

    captureActiveTabState();
    hideWordInfo();
    activeStudyTab_ = index;
    applyTabState(index);
    updateActiveStudyTabLabel();
}

std::string MainWindow::studyTabLabel(const StudyTabState& state) {
    std::string module = state.module;
    if (module.empty()) module = "Bible";
    std::string book = state.book;
    if (book.empty()) book = "Genesis";
    int chapter = std::max(1, state.chapter);
    int verse = std::max(1, state.verse);
    return module + ":" + abbreviateBookName(book) + " " +
           std::to_string(chapter) + ":" + std::to_string(verse);
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
    }

    if (rightPane_) {
        ctx.state.commentaryModule = rightPane_->currentCommentaryModule();
        ctx.state.commentaryReference = rightPane_->currentCommentaryReference();
        ctx.state.dictionaryModule = rightPane_->currentDictionaryModule();
        ctx.state.dictionaryKey = rightPane_->currentDictionaryKey();
        ctx.state.dictionaryActive = rightPane_->isDictionaryTabActive();
    }
}

void MainWindow::applyTabState(int index) {
    if (index < 0 || index >= static_cast<int>(studyTabs_.size())) return;
    if (!biblePane_ || !rightPane_) return;

    StudyContext& ctx = studyTabs_[index];
    applyingTabState_ = true;

    biblePane_->setStudyState(
        ctx.state.module,
        ctx.state.book,
        ctx.state.chapter,
        ctx.state.verse,
        ctx.state.paragraphMode,
        ctx.state.parallelMode,
        ctx.state.parallelModules);
    biblePane_->refresh();

    rightPane_->setStudyState(
        ctx.state.commentaryModule,
        ctx.state.commentaryReference,
        ctx.state.dictionaryModule,
        ctx.state.dictionaryKey,
        ctx.state.dictionaryActive);
    rightPane_->refresh();
    rightPane_->setDictionaryTabActive(ctx.state.dictionaryActive);
    biblePane_->redrawChrome();
    rightPane_->redrawChrome();
    if (studyTabsWidget_) {
        studyTabsWidget_->damage(FL_DAMAGE_ALL);
        studyTabsWidget_->redraw();
    }

    applyingTabState_ = false;
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
    Fl::add_timeout(1.0, onHoverDelayTimeout, this);
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
    captureActiveTabState();
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
        int totalW = w();
        int totalH = h() - menuH;
        int minLeft = 180;
        int maxLeft = std::max(minLeft, totalW - 220);
        int leftW = std::clamp(state.leftPaneWidth, minLeft, maxLeft);

        static_cast<Fl_Widget*>(leftPane_)->resize(0, menuH, leftW, totalH);
        studyArea_->resize(leftW, menuH, totalW - leftW, totalH);

        if (newStudyTabButton_ && studyTabsWidget_) {
            const int newTabButtonPad = 2;
            const int newTabButtonW = newStudyTabButton_->w();
            const int tabsHeaderH = studyTabsWidget_->h();
            newStudyTabButton_->resize(studyArea_->x() + newTabButtonPad,
                                       studyArea_->y() + newTabButtonPad,
                                       newTabButtonW,
                                       newStudyTabButton_->h());
            studyTabsWidget_->resize(studyArea_->x() + newTabButtonW + (newTabButtonPad * 2),
                                     studyArea_->y(),
                                     std::max(40, studyArea_->w() - newTabButtonW - (newTabButtonPad * 2)),
                                     tabsHeaderH);
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
}

int MainWindow::handle(int event) {
    if (event == FL_DRAG || event == FL_RELEASE) {
        if (newStudyTabButton_) newStudyTabButton_->redraw();
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

    if (event == FL_SHORTCUT) {
        if (Fl::event_key() == FL_Escape) {
            hideWordInfo();
            return 1;
        }
    }
    return Fl_Double_Window::handle(event);
}

void MainWindow::onStudyTabChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self || !self->studyTabsWidget_) return;

    Fl_Widget* active = self->studyTabsWidget_->value();
    if (!active) return;

    for (size_t i = 0; i < self->studyTabs_.size(); ++i) {
        if (self->studyTabs_[i].tabGroup == active) {
            self->activateStudyTab(static_cast<int>(i));
            break;
        }
    }
}

void MainWindow::buildMenu() {
    menuBar_->add("&File/&Quit", FL_CTRL + 'q', onFileQuit, this);
    menuBar_->add("&Navigate/&Go to Verse...", FL_CTRL + 'g', onNavigateGo, this);
    menuBar_->add("&View/&Parallel Bibles", FL_CTRL + 'p', onViewParallel, this);
    menuBar_->add("&View/&New Study Tab", FL_CTRL + 't', onViewNewStudyTab, this);
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
        self->biblePane_->redrawChrome();
    }
    if (self->rightPane_) {
        self->rightPane_->redrawChrome();
    }
}

void MainWindow::onViewNewStudyTab(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;
    self->duplicateActiveStudyTab();
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
