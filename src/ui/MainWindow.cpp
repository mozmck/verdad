#include "ui/MainWindow.h"
#include "app/VerdadApp.h"
#include "ui/LeftPane.h"
#include "ui/BiblePane.h"
#include "ui/RightPane.h"
#include "sword/SwordManager.h"

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <FL/Fl_Menu_Item.H>

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

    void resize(int X, int Y, int W, int H) override {
        Fl_Tabs::resize(X, Y, W, H);
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
    studyArea_ = new Fl_Group(studyX, menuH, studyW, H - menuH);
    //studyArea_->box(FL_FLAT_BOX);
    //studyArea_->color(FL_BACKGROUND_COLOR);
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
        H - menuH);
    studyTabsWidget_->selection_color(studyTabsWidget_->color());
    studyTabsWidget_->callback(onStudyTabChange, this);
    studyArea_->end();
    studyArea_->resizable(studyTabsWidget_);

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

    const int tabX = studyTabsWidget_->x();
    const int tabY = studyTabsWidget_->y() + 25;
    const int tabW = studyTabsWidget_->w();
    const int tabH = studyTabsWidget_->h() - 25;

    studyTabsWidget_->begin();

    StudyContext ctx;
    ctx.tabGroup = new Fl_Group(tabX, tabY, tabW, tabH, "Loading...");
    //ctx.tabGroup->box(FL_FLAT_BOX);
    //ctx.tabGroup->color(FL_BACKGROUND2_COLOR);
    ctx.tabGroup->begin();

    ctx.contentTile = new Fl_Tile(tabX, tabY, tabW, tabH);
    ctx.contentTile->begin();

    int bibleW = tabW * 2 / 3;
    int commentaryW = tabW - bibleW;
    ctx.biblePane = new BiblePane(app_, tabX, tabY, bibleW, tabH);
    ctx.rightPane = new RightPane(app_, tabX + bibleW, tabY, commentaryW, tabH);

    ctx.contentTile->end();
    ctx.tabGroup->end();
    ctx.tabGroup->resizable(ctx.contentTile);

    studyTabsWidget_->end();

    studyTabs_.push_back(ctx);
    int idx = static_cast<int>(studyTabs_.size()) - 1;
    studyTabsWidget_->value(studyTabs_[idx].tabGroup);
    activateStudyTab(idx);

    std::string initModule = module;
    if (initModule.empty() && biblePane_) {
        initModule = biblePane_->currentModule();
    }
    if (initModule.empty()) {
        auto bibles = app_->swordManager().getBibleModules();
        if (!bibles.empty()) initModule = bibles[0].name;
    }

    std::string initBook = book.empty() ? "Genesis" : book;
    int initChapter = chapter > 0 ? chapter : 1;
    int initVerse = verse > 0 ? verse : 1;

    if (biblePane_) {
        if (!initModule.empty()) biblePane_->setModule(initModule);
        biblePane_->navigateTo(initBook, initChapter, initVerse);
    }
    updateActiveStudyTabLabel();
}

void MainWindow::duplicateActiveStudyTab() {
    if (activeStudyTab_ < 0 || activeStudyTab_ >= static_cast<int>(studyTabs_.size())) {
        addStudyTab("", "Genesis", 1, 1);
        return;
    }

    const StudyContext& src = studyTabs_[activeStudyTab_];
    if (!src.biblePane) {
        addStudyTab("", "Genesis", 1, 1);
        return;
    }

    const std::string module = src.biblePane->currentModule();
    const std::string book = src.biblePane->currentBook();
    const int chapter = std::max(1, src.biblePane->currentChapter());
    const int verse = std::max(1, src.biblePane->currentVerse());

    const bool srcParagraphMode = src.biblePane->isParagraphMode();
    const bool srcParallelMode = src.biblePane->isParallel();
    const std::vector<std::string> srcParallelModules = src.biblePane->parallelModules();

    std::string srcCommentaryModule;
    std::string srcCommentaryRef;
    std::string srcDictionaryModule;
    std::string srcDictionaryKey;
    bool srcDictionaryActive = false;
    if (src.rightPane) {
        srcCommentaryModule = src.rightPane->currentCommentaryModule();
        srcCommentaryRef = src.rightPane->currentCommentaryReference();
        srcDictionaryModule = src.rightPane->currentDictionaryModule();
        srcDictionaryKey = src.rightPane->currentDictionaryKey();
        srcDictionaryActive = src.rightPane->isDictionaryTabActive();
    }

    addStudyTab(module, book, chapter, verse);
    if (!biblePane_) return;

    if (srcParallelMode) {
        biblePane_->setParallelModules(srcParallelModules);
        if (!biblePane_->isParallel()) {
            biblePane_->toggleParallel();
        }
    }

    if (srcParagraphMode != biblePane_->isParagraphMode()) {
        biblePane_->toggleParagraphMode();
    }

    if (rightPane_) {
        if (!srcCommentaryModule.empty() && !srcCommentaryRef.empty()) {
            rightPane_->showCommentary(srcCommentaryModule, srcCommentaryRef);
        }
        if (!srcDictionaryModule.empty() && !srcDictionaryKey.empty()) {
            rightPane_->showDictionaryEntry(srcDictionaryModule, srcDictionaryKey);
        }
        rightPane_->setDictionaryTabActive(srcDictionaryActive);
    }

    updateActiveStudyTabLabel();
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
    biblePane_ = nullptr;
    rightPane_ = nullptr;
    studyTabsWidget_->redraw();
}

void MainWindow::activateStudyTab(int index) {
    if (index < 0 || index >= static_cast<int>(studyTabs_.size())) return;

    activeStudyTab_ = index;
    biblePane_ = studyTabs_[index].biblePane;
    rightPane_ = studyTabs_[index].rightPane;
    updateActiveStudyTabLabel();
}

std::string MainWindow::studyTabLabel(const BiblePane* pane) {
    if (!pane) return "Study";

    std::string module = pane->currentModule();
    if (module.empty()) module = "Bible";
    std::string book = pane->currentBook();
    if (book.empty()) book = "Genesis";
    int chapter = std::max(1, pane->currentChapter());
    int verse = std::max(1, pane->currentVerse());
    return module + ":" + abbreviateBookName(book) + " " +
           std::to_string(chapter) + ":" + std::to_string(verse);
}

void MainWindow::updateActiveStudyTabLabel() {
    if (activeStudyTab_ < 0 ||
        activeStudyTab_ >= static_cast<int>(studyTabs_.size()) ||
        !studyTabsWidget_) {
        return;
    }

    StudyContext& ctx = studyTabs_[activeStudyTab_];
    if (!ctx.tabGroup) return;

    std::string label = studyTabLabel(ctx.biblePane);
    ctx.tabGroup->copy_label(label.c_str());
    studyTabsWidget_->redraw();
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
                               int /*screenX*/, int /*screenY*/) {
    pendingWordInfo_.word = word;
    pendingWordInfo_.href = href;
    pendingWordInfo_.strong = strong;
    pendingWordInfo_.morph = morph;

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
    for (auto& ctx : studyTabs_) {
        if (ctx.biblePane) ctx.biblePane->refresh();
        if (ctx.rightPane) ctx.rightPane->refresh();
    }
}

MainWindow::SessionState MainWindow::captureSessionState() const {
    SessionState state;
    state.windowX = x();
    state.windowY = y();
    state.windowW = w();
    state.windowH = h();
    if (leftPane_) {
        state.leftPaneWidth = leftPane_->w();
    }
    state.activeStudyTab = activeStudyTab_;

    for (const auto& ctx : studyTabs_) {
        StudyTabState tab;
        if (ctx.biblePane) {
            tab.module = ctx.biblePane->currentModule();
            tab.book = ctx.biblePane->currentBook();
            tab.chapter = ctx.biblePane->currentChapter();
            tab.verse = ctx.biblePane->currentVerse();
            tab.paragraphMode = ctx.biblePane->isParagraphMode();
            tab.parallelMode = ctx.biblePane->isParallel();
            tab.parallelModules = ctx.biblePane->parallelModules();
            tab.biblePaneWidth = ctx.biblePane->w();
        }
        if (ctx.rightPane) {
            tab.commentaryModule = ctx.rightPane->currentCommentaryModule();
            tab.commentaryReference = ctx.rightPane->currentCommentaryReference();
            tab.dictionaryModule = ctx.rightPane->currentDictionaryModule();
            tab.dictionaryKey = ctx.rightPane->currentDictionaryKey();
            tab.dictionaryActive = ctx.rightPane->isDictionaryTabActive();
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
            newStudyTabButton_->resize(studyArea_->x() + newTabButtonPad,
                                       studyArea_->y() + newTabButtonPad,
                                       newTabButtonW,
                                       newStudyTabButton_->h());
            studyTabsWidget_->resize(studyArea_->x() + newTabButtonW + (newTabButtonPad * 2),
                                     studyArea_->y(),
                                     std::max(40, studyArea_->w() - newTabButtonW - (newTabButtonPad * 2)),
                                     studyArea_->h());
        }
    }

    if (state.studyTabs.empty()) {
        redraw();
        return;
    }

    clearStudyTabs();

    for (const auto& tabState : state.studyTabs) {
        addStudyTab(tabState.module, tabState.book, tabState.chapter, tabState.verse);

        if (biblePane_) {
            if (tabState.parallelMode) {
                if (!tabState.parallelModules.empty()) {
                    biblePane_->setParallelModules(tabState.parallelModules);
                }
                if (!biblePane_->isParallel()) {
                    biblePane_->toggleParallel();
                }
            } else if (biblePane_->isParallel()) {
                biblePane_->toggleParallel();
            }

            if (tabState.paragraphMode != biblePane_->isParagraphMode()) {
                biblePane_->toggleParagraphMode();
            }
        }

        if (rightPane_) {
            if (!tabState.commentaryModule.empty()) {
                rightPane_->setCommentaryModule(tabState.commentaryModule);
            }
            if (!tabState.dictionaryModule.empty()) {
                rightPane_->setDictionaryModule(tabState.dictionaryModule);
            }

            if (!tabState.commentaryReference.empty()) {
                std::string mod = tabState.commentaryModule.empty()
                    ? rightPane_->currentCommentaryModule()
                    : tabState.commentaryModule;
                if (!mod.empty()) {
                    rightPane_->showCommentary(mod, tabState.commentaryReference);
                }
            }

            if (!tabState.dictionaryKey.empty()) {
                std::string mod = tabState.dictionaryModule.empty()
                    ? rightPane_->currentDictionaryModule()
                    : tabState.dictionaryModule;
                if (!mod.empty()) {
                    rightPane_->showDictionaryEntry(mod, tabState.dictionaryKey);
                }
            }

            rightPane_->setDictionaryTabActive(tabState.dictionaryActive);
        }

        if (activeStudyTab_ >= 0 &&
            activeStudyTab_ < static_cast<int>(studyTabs_.size())) {
            StudyContext& ctx = studyTabs_[activeStudyTab_];
            if (ctx.contentTile && ctx.biblePane && ctx.rightPane &&
                tabState.biblePaneWidth > 0) {
                int tileX = ctx.contentTile->x();
                int tileY = ctx.contentTile->y();
                int tileW = ctx.contentTile->w();
                int tileH = ctx.contentTile->h();
                int bibleW = std::clamp(tabState.biblePaneWidth, 140, std::max(140, tileW - 140));
                static_cast<Fl_Widget*>(ctx.biblePane)
                    ->resize(tileX, tileY, bibleW, tileH);
                static_cast<Fl_Widget*>(ctx.rightPane)
                    ->resize(tileX + bibleW, tileY, tileW - bibleW, tileH);
                ctx.contentTile->redraw();
            }
        }
    }

    if (!studyTabs_.empty() && studyTabsWidget_) {
        int idx = std::clamp(state.activeStudyTab, 0,
                             static_cast<int>(studyTabs_.size()) - 1);
        studyTabsWidget_->value(studyTabs_[idx].tabGroup);
        activateStudyTab(idx);
    }

    redraw();
}

int MainWindow::handle(int event) {
    if (event == FL_DRAG || event == FL_RELEASE) {
        if (newStudyTabButton_) newStudyTabButton_->redraw();
        if (studyTabsWidget_) studyTabsWidget_->redraw();
        if (leftPane_) leftPane_->redrawChrome();
        if (biblePane_) biblePane_->redrawChrome();
        if (rightPane_) rightPane_->redrawChrome();
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
