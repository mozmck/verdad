#include "ui/BiblePane.h"
#include "app/VerdadApp.h"
#include "ui/BibleBookChoice.h"
#include "ui/HtmlWidget.h"
#include "ui/MainWindow.h"
#include "ui/LeftPane.h"
#include "ui/ModuleChoiceUtils.h"
#include "ui/RightPane.h"
#include "ui/WrappingChoice.h"
#include "ui/WrappingInputChoice.h"
#include "ui/TagPanel.h"
#include "ui/VerseContext.h"
#include "reading/DateUtils.h"
#include "sword/SwordManager.h"
#include "tags/TagManager.h"
#include "app/PerfTrace.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>

namespace verdad {

namespace {
constexpr int kNavH = 30;
constexpr int kContentPadding = 2;
constexpr int kParallelHeaderH = 28;
constexpr int kParallelHeaderSpacing = 6;
constexpr int kDailyReadingBarH = 30;
constexpr int kDailyReadingCompleteW = 92;
constexpr int kDailyReadingPlanButtonW = 30;
constexpr int kHistoryChoiceWidth = 150; // Fits most refs while leaving room for history buttons.
constexpr int kParagraphButtonW = 25;
constexpr int kParallelButtonW = 25;
constexpr int kDisplayOptionsMenuButtonW = 30;
constexpr int kRedWordsButtonW = 34;
constexpr int kStrongsButtonW = 34;
constexpr int kMorphButtonW = 52;
constexpr int kFootnotesButtonW = 50;
constexpr int kCrossRefsButtonW = 46;

enum DisplayOptionMenuIndex {
    kDisplayOptionParagraph = 0,
    kDisplayOptionRedWords,
    kDisplayOptionStrongs,
    kDisplayOptionMorph,
    kDisplayOptionFootnotes,
    kDisplayOptionCrossRefs,
};

WrappingInputChoice* historyInputChoice(Fl_Input_Choice* choice) {
    return static_cast<WrappingInputChoice*>(choice);
}

void setToggleMenuItemValue(Fl_Menu_Button* button, int index, bool enabled) {
    if (!button || index < 0 || index >= button->size()) return;
    button->mode(index, FL_MENU_TOGGLE | (enabled ? FL_MENU_VALUE : 0));
}

int findChoiceIndexByLabel(const Fl_Choice* choice, const std::string& label) {
    if (!choice || label.empty()) return -1;
    const Fl_Menu_Item* menu = choice->menu();
    if (!menu) return -1;

    for (int i = 0; i < choice->size(); ++i) {
        const Fl_Menu_Item& item = menu[i];
        if (item.label() && label == item.label()) {
            return i;
        }
    }
    return -1;
}

std::string normalizeBookLookupKey(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            out.push_back(static_cast<char>(std::tolower(uc)));
        }
    }
    return out;
}

std::string canonicalBookLabelForModule(VerdadApp* app,
                                        const std::string& moduleName,
                                        const std::string& book) {
    const std::string trimmedBook = module_choice::trimCopy(book);
    if (!app || moduleName.empty() || trimmedBook.empty()) {
        return trimmedBook;
    }

    const std::string wanted = normalizeBookLookupKey(trimmedBook);
    if (wanted.empty()) return trimmedBook;

    std::vector<std::string> books = app->swordManager().getBookNames(moduleName);
    std::string uniquePrefixMatch;
    int prefixMatches = 0;

    for (const auto& candidate : books) {
        const std::string normalizedCandidate = normalizeBookLookupKey(candidate);
        if (normalizedCandidate == wanted) {
            return candidate;
        }

        std::string shortRef = app->swordManager().getShortReference(moduleName,
                                                                     candidate + " 1:1");
        SwordManager::VerseRef shortParsed = SwordManager::parseVerseRef(shortRef);
        const std::string normalizedShortBook = normalizeBookLookupKey(shortParsed.book);
        if (!normalizedShortBook.empty() && normalizedShortBook == wanted) {
            return candidate;
        }

        const bool prefixMatch =
            (!normalizedCandidate.empty() &&
             normalizedCandidate.rfind(wanted, 0) == 0) ||
            (!normalizedShortBook.empty() &&
             normalizedShortBook.rfind(wanted, 0) == 0);
        if (prefixMatch) {
            uniquePrefixMatch = candidate;
            ++prefixMatches;
        }
    }

    return prefixMatches == 1 ? uniquePrefixMatch : trimmedBook;
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

std::string buildVerseTagMarkersHtml(VerdadApp* app, const std::string& verseRef) {
    if (!app) return "";

    const auto tags = app->tagManager().getTagsForVerse(verseRef);
    if (tags.empty()) return "";

    const std::string escapedVerseRef = htmlEscape(verseRef);
    std::ostringstream tooltip;
    //tooltip << "Tags: ";
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i) tooltip << "\n";
        tooltip << tags[i].name;
    }

    std::ostringstream html;
    html << "<span class=\"verse-tags\">";
    html << "<a class=\"verse-tag-marker\" href=\"tags:" << escapedVerseRef << "\""
         << " title=\"" << htmlEscape(tooltip.str()) << "\"";
    if (!tags.front().color.empty()) {
        const std::string escapedColor = htmlEscape(tags.front().color);
        html << " style=\"border-color:" << escapedColor
             << ";color:" << escapedColor << ";\"";
    }
    html << ">Tag</a>";
    html << "</span>";
    return html.str();
}

std::string verseElementId(int verse) {
    return verse > 0 ? "v" + std::to_string(verse) : "";
}

std::string selectedVerseInlineStyleSnippet() {
    return "--verdad-selected-verse:1;color:#1f7f1f !important;";
}

std::string monthDayLabel(const std::string& isoDate) {
    reading::Date date{};
    if (!reading::parseIsoDate(isoDate, date)) {
        date = reading::today();
    }
    std::string month = reading::monthName(date.month);
    if (month.empty()) return isoDate;
    return month + " " + std::to_string(date.day);
}

std::string urlDecode(const std::string& text) {
    std::string decoded;
    decoded.reserve(text.size());

    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '%' && i + 2 < text.size()) {
            const std::string hex = text.substr(i + 1, 2);
            char* end = nullptr;
            long value = std::strtol(hex.c_str(), &end, 16);
            if (end && *end == '\0') {
                decoded.push_back(static_cast<char>(value));
                i += 2;
                continue;
            }
        }
        decoded.push_back(c == '+' ? ' ' : c);
    }

    return decoded;
}

std::string extractQueryValue(const std::string& url, const char* key) {
    if (!key || !*key) return "";
    const std::string marker = std::string(key) + "=";
    size_t queryPos = url.find('?');
    if (queryPos == std::string::npos) return "";
    size_t pos = url.find(marker, queryPos + 1);
    if (pos == std::string::npos) return "";
    pos += marker.size();
    size_t end = url.find('&', pos);
    return urlDecode(url.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
}

std::string firstReadingListItem(const std::string& reference) {
    std::string ref = module_choice::trimCopy(reference);
    if (ref.empty()) return ref;

    size_t split = ref.find_first_of(",;");
    if (split == std::string::npos) return ref;

    std::string first = module_choice::trimCopy(ref.substr(0, split));
    return first.empty() ? ref : first;
}

std::string decodeBasicHtmlEntities(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out.push_back(text[i]);
            continue;
        }

        size_t semi = text.find(';', i + 1);
        if (semi == std::string::npos) {
            out.push_back(text[i]);
            continue;
        }

        const std::string entity = text.substr(i, semi - i + 1);
        if (entity == "&amp;") out.push_back('&');
        else if (entity == "&lt;") out.push_back('<');
        else if (entity == "&gt;") out.push_back('>');
        else if (entity == "&quot;") out.push_back('"');
        else if (entity == "&#39;") out.push_back('\'');
        else if (entity == "&nbsp;") out.push_back(' ');
        else {
            out += entity;
            i = semi;
            continue;
        }
        i = semi;
    }

    return out;
}

std::string stripSimpleHtml(const std::string& html) {
    std::string text;
    text.reserve(html.size());
    bool inTag = false;
    for (char c : html) {
        if (c == '<') {
            inTag = true;
            continue;
        }
        if (c == '>') {
            inTag = false;
            continue;
        }
        if (!inTag) text.push_back(c);
    }
    return decodeBasicHtmlEntities(text);
}

class DailyReadingBarWidget : public Fl_Widget {
public:
    struct Chunk {
        std::string text;
        std::string link;
    };

    using LinkCallback = std::function<void(const std::string&)>;

    DailyReadingBarWidget(int X, int Y, int W, int H, const char* label = nullptr)
        : Fl_Widget(X, Y, W, H, label) {
        box(FL_FLAT_BOX);
        color(FL_BACKGROUND_COLOR);
        labelfont(FL_HELVETICA);
        labelsize(12);
    }

    void setChunks(std::vector<Chunk> chunks) {
        chunks_ = std::move(chunks);
        redraw();
    }

    void setLinkCallback(LinkCallback callback) {
        linkCallback_ = std::move(callback);
    }

    int handle(int event) override {
        switch (event) {
        case FL_MOVE:
        case FL_ENTER: {
            updateHover(linkIndexAt(Fl::event_x(), Fl::event_y()));
            return 1;
        }
        case FL_LEAVE:
            updateHover(-1);
            return 1;
        case FL_PUSH:
            if (Fl::event_button() == FL_LEFT_MOUSE) {
                int index = linkIndexAt(Fl::event_x(), Fl::event_y());
                if (index >= 0 && index < static_cast<int>(hitRects_.size()) &&
                    linkCallback_) {
                    linkCallback_(hitRects_[static_cast<size_t>(index)].link);
                    return 1;
                }
            }
            break;
        default:
            break;
        }
        return Fl_Widget::handle(event);
    }

    void draw() override {
        fl_push_clip(x(), y(), w(), h());
        fl_color(color());
        fl_rectf(x(), y(), w(), h());

        hitRects_.clear();

        fl_font(labelfont(), labelsize());
        const int baseline = y() + ((h() + fl_height()) / 2) - fl_descent();
        int cursorX = x() + 4;
        const int maxX = x() + w() - 4;

        for (const auto& chunk : chunks_) {
            if (chunk.text.empty() || cursorX >= maxX) continue;

            int chunkW = 0;
            int chunkH = 0;
            fl_measure(chunk.text.c_str(), chunkW, chunkH, 0);
            if (chunkW <= 0) continue;

            const int drawW = std::min(chunkW, maxX - cursorX);
            if (drawW <= 0) break;

            const bool isLink = !chunk.link.empty();
            fl_color(isLink ? fl_rgb_color(0, 50, 200) : fl_rgb_color(0, 0, 0));
            fl_draw(chunk.text.c_str(),
                    cursorX,
                    y(),
                    drawW,
                    h(),
                    FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);

            if (isLink) {
                HitRect rect;
                rect.x = cursorX;
                rect.y = y();
                rect.w = drawW;
                rect.h = h();
                rect.link = chunk.link;
                hitRects_.push_back(std::move(rect));

                if (hoveredLink_ == static_cast<int>(hitRects_.size()) - 1) {
                    fl_color(fl_rgb_color(0, 50, 200));
                    fl_line(cursorX, baseline + 1, cursorX + drawW, baseline + 1);
                }
            }

            cursorX += drawW;
        }

        fl_pop_clip();
    }

private:
    struct HitRect {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        std::string link;
    };

    int linkIndexAt(int mouseX, int mouseY) const {
        for (size_t i = 0; i < hitRects_.size(); ++i) {
            const auto& rect = hitRects_[i];
            if (mouseX >= rect.x && mouseX < rect.x + rect.w &&
                mouseY >= rect.y && mouseY < rect.y + rect.h) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void updateHover(int hoveredLink) {
        if (hoveredLink_ == hoveredLink) return;
        hoveredLink_ = hoveredLink;
        fl_cursor(hoveredLink_ >= 0 ? FL_CURSOR_HAND : FL_CURSOR_DEFAULT);
        redraw();
    }

    std::vector<Chunk> chunks_;
    std::vector<HitRect> hitRects_;
    LinkCallback linkCallback_;
    int hoveredLink_ = -1;
};

std::vector<DailyReadingBarWidget::Chunk> chunksFromSummaryHtml(const std::string& html) {
    std::vector<DailyReadingBarWidget::Chunk> chunks;
    size_t cursor = 0;

    while (cursor < html.size()) {
        size_t anchorStart = html.find("<a", cursor);
        if (anchorStart == std::string::npos) {
            std::string text = stripSimpleHtml(html.substr(cursor));
            if (!text.empty()) chunks.push_back({text, ""});
            break;
        }

        if (anchorStart > cursor) {
            std::string text = stripSimpleHtml(html.substr(cursor, anchorStart - cursor));
            if (!text.empty()) chunks.push_back({text, ""});
        }

        size_t hrefStart = html.find("href=\"", anchorStart);
        if (hrefStart == std::string::npos) break;
        hrefStart += 6;
        size_t hrefEnd = html.find('"', hrefStart);
        if (hrefEnd == std::string::npos) break;

        size_t contentStart = html.find('>', hrefEnd);
        if (contentStart == std::string::npos) break;
        ++contentStart;

        size_t anchorEnd = html.find("</a>", contentStart);
        if (anchorEnd == std::string::npos) break;

        std::string link = html.substr(hrefStart, hrefEnd - hrefStart);
        std::string text = stripSimpleHtml(html.substr(contentStart, anchorEnd - contentStart));
        if (!text.empty()) chunks.push_back({text, link});

        cursor = anchorEnd + 4;
    }

    return chunks;
}

bool summaryHtmlHasReadingLinks(const std::string& html) {
    return html.find("<a ") != std::string::npos ||
           html.find("<a href=") != std::string::npos;
}
} // namespace

BiblePane::BiblePane(VerdadApp* app, int X, int Y, int W, int H)
    : Fl_Group(X, Y, W, H)
    , app_(app)
    , navBar_(nullptr)
    , bookChoice_(nullptr)
    , chapterChoice_(nullptr)
    , moduleChoice_(nullptr)
    , prevButton_(nullptr)
    , nextButton_(nullptr)
    , historyLeftSeparator_(nullptr)
    , historyBackButton_(nullptr)
    , historyChoice_(nullptr)
    , historyForwardButton_(nullptr)
    , historyRightSeparator_(nullptr)
    , moduleRightSeparator_(nullptr)
    , parallelButton_(nullptr)
    , paragraphButton_(nullptr)
    , redWordsToggleButton_(nullptr)
    , parallelAddButton_(nullptr)
    , strongsToggleButton_(nullptr)
    , morphToggleButton_(nullptr)
    , footnotesToggleButton_(nullptr)
    , crossRefsToggleButton_(nullptr)
    , displayOptionsMenuButton_(nullptr)
    , crossRefsRightSeparator_(nullptr)
    , navSpacer_(nullptr)
    , parallelHeader_(nullptr)
    , dailyReadingBar_(nullptr)
    , dailyReadingBarWidget_(nullptr)
    , dailyReadingCompleteButton_(nullptr)
    , dailyReadingPlanButton_(nullptr)
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

    dailyReadingBar_ = new Fl_Group(X, Y + H - kDailyReadingBarH, W, kDailyReadingBarH);
    dailyReadingBar_->box(FL_THIN_UP_BOX);
    dailyReadingBar_->color(FL_BACKGROUND_COLOR);
    dailyReadingBar_->begin();
    dailyReadingBarWidget_ = new DailyReadingBarWidget(
        X + 4,
        dailyReadingBar_->y() + 2,
        std::max(20, W - kDailyReadingCompleteW - kDailyReadingPlanButtonW - 16),
        std::max(20, kDailyReadingBarH - 4));
    static_cast<DailyReadingBarWidget*>(dailyReadingBarWidget_)->setLinkCallback(
        [this](const std::string& url) { onDailyReadingBarLink(url); });
    dailyReadingCompleteButton_ = new Fl_Check_Button(
        X + W - kDailyReadingPlanButtonW - kDailyReadingCompleteW - 6,
        dailyReadingBar_->y() + 5,
        kDailyReadingCompleteW,
        std::max(20, kDailyReadingBarH - 10),
        "Completed");
    dailyReadingCompleteButton_->callback(onDailyReadingComplete, this);
    dailyReadingPlanButton_ = new Fl_Button(X + W - kDailyReadingPlanButtonW - 4,
                                            dailyReadingBar_->y() + 3,
                                            kDailyReadingPlanButtonW,
                                            std::max(20, kDailyReadingBarH - 6),
                                            u8"☰");
    dailyReadingPlanButton_->callback(onDailyReadingPlanButton, this);
    dailyReadingPlanButton_->tooltip("Reading Plan");
    dailyReadingBar_->end();

    htmlWidget_ = new HtmlWidget(X, contentY, W, std::max(20, contentH - kDailyReadingBarH));

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
               const std::string& module,
               int x, int y) {
            onWordHover(word, href, strong, morph, module, x, y);
        });
    htmlWidget_->setContextCallback(
        [this](const std::string& word, const std::string& href,
               const std::string& strong, const std::string& morph,
               const std::string& module,
               int x, int y) {
            onContextMenu(word, href, strong, morph, module, x, y);
        });

    end();
    resizable(htmlWidget_);

    if (parallelAddButton_) {
        parallelAddButton_->hide();
    }
    syncOptionButtons();
    layoutNavBarControls();
    refreshDailyReadingPlanBar();

}

BiblePane::~BiblePane() {
    clearParallelHeader();
}

void BiblePane::resize(int X, int Y, int W, int H) {
    Fl_Group::resize(X, Y, W, H);

    if (!navBar_ || !bookChoice_ || !chapterChoice_ || !moduleChoice_ ||
        !prevButton_ || !nextButton_ || !historyLeftSeparator_ ||
        !historyBackButton_ || !historyChoice_ || !historyForwardButton_ ||
        !historyRightSeparator_ || !moduleRightSeparator_ ||
        !parallelButton_ || !paragraphButton_ || !redWordsToggleButton_ ||
        !parallelAddButton_ ||
        !strongsToggleButton_ || !morphToggleButton_ ||
        !footnotesToggleButton_ || !crossRefsToggleButton_ ||
        !displayOptionsMenuButton_ || !crossRefsRightSeparator_ ||
        !navSpacer_ || !parallelHeader_ || !dailyReadingBar_ ||
        !dailyReadingBarWidget_ || !dailyReadingCompleteButton_ ||
        !dailyReadingPlanButton_ || !htmlWidget_) {
        return;
    }

    const int navH = kNavH;
    const int padding = kContentPadding;
    layoutNavBarControls();

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

    layoutDailyReadingBar();
    int textY = contentY + headerH;
    int textH = std::max(20, H - navH - padding - headerH - kDailyReadingBarH);
    htmlWidget_->resize(X, textY, W, textH);
}

void BiblePane::layoutNavBarControls() {
    if (!navBar_ || !bookChoice_ || !chapterChoice_ || !moduleChoice_ ||
        !prevButton_ || !nextButton_ || !historyLeftSeparator_ ||
        !historyBackButton_ || !historyChoice_ || !historyForwardButton_ ||
        !historyRightSeparator_ || !moduleRightSeparator_ ||
        !parallelButton_ || !paragraphButton_ || !redWordsToggleButton_ ||
        !parallelAddButton_ || !strongsToggleButton_ || !morphToggleButton_ ||
        !footnotesToggleButton_ || !crossRefsToggleButton_ ||
        !displayOptionsMenuButton_ || !crossRefsRightSeparator_ ||
        !navSpacer_) {
        return;
    }

    const int spacing = 2;
    const int buttonW = 30;
    const int historyBtnW = 24;
    const int separatorW = 4;
    const int bookW = 120;
    const int chapW = 50;
    const int moduleW = 100;
    const int historyW = kHistoryChoiceWidth;

    navBar_->resize(x(), y(), w(), kNavH);
    const int cy = y() + 2;
    const int nh = kNavH - 4;
    int cx = x() + 2;

    prevButton_->resize(cx, cy, buttonW, nh);
    cx += buttonW + spacing;

    bookChoice_->resize(cx, cy, bookW, nh);
    cx += bookW + spacing;

    chapterChoice_->resize(cx, cy, chapW, nh);
    cx += chapW + spacing;

    nextButton_->resize(cx, cy, buttonW, nh);
    cx += buttonW + spacing;

    historyLeftSeparator_->resize(cx, cy + 4, separatorW, std::max(10, nh - 8));
    cx += separatorW + spacing;

    historyBackButton_->resize(cx, cy, historyBtnW, nh);
    cx += historyBtnW + spacing;

    historyChoice_->resize(cx, cy, historyW, nh);
    cx += historyW + spacing;

    historyForwardButton_->resize(cx, cy, historyBtnW, nh);
    cx += historyBtnW + spacing;

    historyRightSeparator_->resize(cx, cy + 4, separatorW, std::max(10, nh - 8));
    cx += separatorW + spacing;

    moduleChoice_->resize(cx, cy, moduleW, nh);
    cx += moduleW + spacing;

    moduleRightSeparator_->resize(cx, cy + 4, separatorW, std::max(10, nh - 8));
    cx += separatorW + spacing;

    const int visibleParallelControlsW = kParallelButtonW + (parallelMode_ ? spacing + kParallelButtonW : 0);
    const int expandedDisplayOptionsW =
        kParagraphButtonW + spacing +
        kRedWordsButtonW + spacing +
        kStrongsButtonW + spacing +
        kMorphButtonW + spacing +
        kFootnotesButtonW + spacing +
        kCrossRefsButtonW +
        spacing + separatorW + spacing +
        visibleParallelControlsW;
    const bool collapseDisplayOptions = cx + expandedDisplayOptionsW > x() + w() - 2;

    if (collapseDisplayOptions) {
        displayOptionsMenuButton_->show();
        displayOptionsMenuButton_->resize(cx, cy, kDisplayOptionsMenuButtonW, nh);
        paragraphButton_->hide();
        redWordsToggleButton_->hide();
        strongsToggleButton_->hide();
        morphToggleButton_->hide();
        footnotesToggleButton_->hide();
        crossRefsToggleButton_->hide();
        cx += kDisplayOptionsMenuButtonW + spacing;
    } else {
        displayOptionsMenuButton_->hide();

        paragraphButton_->show();
        paragraphButton_->resize(cx, cy, kParagraphButtonW, nh);
        cx += kParagraphButtonW + spacing;

        redWordsToggleButton_->show();
        redWordsToggleButton_->resize(cx, cy, kRedWordsButtonW, nh);
        cx += kRedWordsButtonW + spacing;

        strongsToggleButton_->show();
        strongsToggleButton_->resize(cx, cy, kStrongsButtonW, nh);
        cx += kStrongsButtonW + spacing;

        morphToggleButton_->show();
        morphToggleButton_->resize(cx, cy, kMorphButtonW, nh);
        cx += kMorphButtonW + spacing;

        footnotesToggleButton_->show();
        footnotesToggleButton_->resize(cx, cy, kFootnotesButtonW, nh);
        cx += kFootnotesButtonW + spacing;

        crossRefsToggleButton_->show();
        crossRefsToggleButton_->resize(cx, cy, kCrossRefsButtonW, nh);
        cx += kCrossRefsButtonW + spacing;
    }

    crossRefsRightSeparator_->resize(cx, cy + 4, separatorW, std::max(10, nh - 8));
    cx += separatorW + spacing;

    parallelButton_->resize(cx, cy, kParallelButtonW, nh);
    cx += kParallelButtonW + spacing;

    parallelAddButton_->resize(cx, cy, kParallelButtonW, nh);
    if (parallelMode_) {
        parallelAddButton_->show();
        cx += kParallelButtonW + spacing;
    } else {
        parallelAddButton_->hide();
    }

    int spacerW = std::max(0, (x() + w() - 2) - cx);
    navSpacer_->resize(cx, cy, spacerW, nh);
}

void BiblePane::navigateTo(const std::string& book, int chapter, int verse) {
    if (moduleName_.empty()) return;
    const std::string canonicalBook = canonicalBookLabelForModule(app_, moduleName_, book);
    if (canonicalBook.empty()) return;

    if (canonicalBook == currentBook_ && chapter == currentChapter_) {
        selectVerse(verse);
        return;
    }

    currentBook_ = canonicalBook;
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
    syncReferenceInput();
    if (htmlWidget_ && currentVerse_ > 0) {
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
    if (!ref.book.empty()) {
        int chapter = ref.chapter > 0 ? ref.chapter : 1;
        int verse = ref.verse > 0 ? ref.verse : 1;
        navigateTo(ref.book, chapter, verse);
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

    applyModuleChoiceValue(moduleChoice_, moduleName_);

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

int BiblePane::scrollY() const {
    return htmlWidget_ ? htmlWidget_->scrollY() : 0;
}

void BiblePane::setScrollY(int y) {
    if (htmlWidget_) {
        htmlWidget_->setScrollY(y);
    }
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
    layoutNavBarControls();
    updateDisplay();
    notifyContextChanged();
}

void BiblePane::toggleParagraphMode() {
    paragraphMode_ = !paragraphMode_;
    syncOptionButtons();
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
                break;
            }
        }
    }
}

void BiblePane::refresh() {
    perf::ScopeTimer timer("BiblePane::refresh");
    syncOptionButtons();
    updateDisplay();
    refreshDailyReadingPlanBar();
}

void BiblePane::setHtmlStyleOverride(const std::string& css) {
    if (htmlWidget_) {
        htmlWidget_->setStyleOverrideCss(css);
        if (currentVerse_ != htmlStoredSelectedVerse_ &&
            canUpdateVerseSelectionInPlace()) {
            syncVerseSelectionInPlace(htmlStoredSelectedVerse_, currentVerse_);
        }
    }
}

void BiblePane::setBrowserLineSpacing(int pixels) {
    if (bookChoice_) {
        bookChoice_->setBrowserLineSpacing(pixels);
    }
}

void BiblePane::layoutDailyReadingBar() {
    if (!dailyReadingBar_ || !dailyReadingBarWidget_ ||
        !dailyReadingCompleteButton_ || !dailyReadingPlanButton_) {
        return;
    }

    dailyReadingBar_->resize(x(), y() + h() - kDailyReadingBarH, w(), kDailyReadingBarH);

    const int buttonH = std::max(20, dailyReadingBar_->h() - 6);
    const int checkboxH = std::max(20, dailyReadingBar_->h() - 10);
    const int buttonY = dailyReadingBar_->y() + std::max(2, (dailyReadingBar_->h() - buttonH) / 2);
    const int checkboxY = dailyReadingBar_->y() + std::max(2, (dailyReadingBar_->h() - checkboxH) / 2);
    const int buttonX = dailyReadingBar_->x() + dailyReadingBar_->w() - kDailyReadingPlanButtonW - 4;
    const int checkboxX = buttonX - kDailyReadingCompleteW - 8;

    dailyReadingPlanButton_->resize(buttonX,
                                    buttonY,
                                    kDailyReadingPlanButtonW,
                                    buttonH);
    dailyReadingCompleteButton_->resize(checkboxX,
                                        checkboxY,
                                        kDailyReadingCompleteW,
                                        checkboxH);
    dailyReadingBarWidget_->resize(dailyReadingBar_->x() + 6,
                                   dailyReadingBar_->y() + 2,
                                   std::max(20, checkboxX - dailyReadingBar_->x() - 12),
                                   std::max(20, dailyReadingBar_->h() - 4));
}

void BiblePane::syncOptionButtons() {
    if (!app_) return;
    const auto& options = app_->optionDisplaySettings();
    if (paragraphButton_) {
        paragraphButton_->value(paragraphMode_ ? 1 : 0);
    }
    if (strongsToggleButton_) {
        strongsToggleButton_->value(options.showStrongsMarkers ? 1 : 0);
    }
    if (redWordsToggleButton_) {
        redWordsToggleButton_->value(options.showWordsOfChristRed ? 1 : 0);
    }
    if (morphToggleButton_) {
        morphToggleButton_->value(options.showMorphMarkers ? 1 : 0);
    }
    if (footnotesToggleButton_) {
        footnotesToggleButton_->value(options.showFootnoteMarkers ? 1 : 0);
    }
    if (crossRefsToggleButton_) {
        crossRefsToggleButton_->value(options.showCrossReferenceMarkers ? 1 : 0);
    }
    setToggleMenuItemValue(displayOptionsMenuButton_, kDisplayOptionParagraph, paragraphMode_);
    setToggleMenuItemValue(displayOptionsMenuButton_, kDisplayOptionRedWords,
                           options.showWordsOfChristRed);
    setToggleMenuItemValue(displayOptionsMenuButton_, kDisplayOptionStrongs,
                           options.showStrongsMarkers);
    setToggleMenuItemValue(displayOptionsMenuButton_, kDisplayOptionMorph,
                           options.showMorphMarkers);
    setToggleMenuItemValue(displayOptionsMenuButton_, kDisplayOptionFootnotes,
                           options.showFootnoteMarkers);
    setToggleMenuItemValue(displayOptionsMenuButton_, kDisplayOptionCrossRefs,
                           options.showCrossReferenceMarkers);
}

void BiblePane::setNavigationHistory(const std::vector<std::string>& labels,
                                     int currentIndex,
                                     bool canGoBack,
                                     bool canGoForward) {
    if (!historyChoice_ || !historyBackButton_ || !historyForwardButton_) return;

    historyChoice_->clear();
    for (const auto& label : labels) {
        historyChoice_->add(label.c_str());
    }

    if (currentIndex >= 0 && currentIndex < static_cast<int>(labels.size())) {
        historyChoice_->menubutton()->value(currentIndex);
        historyInputChoice(historyChoice_)->setPopupInitialIndex(currentIndex);
    } else {
        historyChoice_->menubutton()->value(-1);
        historyInputChoice(historyChoice_)->setPopupInitialIndex(-1);
    }

    historyBackButton_->activate();
    if (!canGoBack) historyBackButton_->deactivate();

    historyForwardButton_->activate();
    if (!canGoForward) historyForwardButton_->deactivate();

    syncReferenceInput();
    historyChoice_->clear_changed();
    historyChoice_->input()->clear_changed();
    historyChoice_->menubutton()->clear_changed();
}

void BiblePane::redrawChrome() {
    damage(FL_DAMAGE_ALL);
    if (navBar_) {
        navBar_->damage(FL_DAMAGE_ALL);
        navBar_->redraw();
    }
    if (parallelAddButton_) parallelAddButton_->redraw();
    if (paragraphButton_) paragraphButton_->redraw();
    if (redWordsToggleButton_) redWordsToggleButton_->redraw();
    if (strongsToggleButton_) strongsToggleButton_->redraw();
    if (morphToggleButton_) morphToggleButton_->redraw();
    if (footnotesToggleButton_) footnotesToggleButton_->redraw();
    if (crossRefsToggleButton_) crossRefsToggleButton_->redraw();
    if (displayOptionsMenuButton_) displayOptionsMenuButton_->redraw();
    if (parallelHeader_) {
        parallelHeader_->damage(FL_DAMAGE_ALL);
        parallelHeader_->redraw();
    }
    if (dailyReadingBar_) {
        dailyReadingBar_->damage(FL_DAMAGE_ALL);
        dailyReadingBar_->redraw();
    }
    if (dailyReadingBarWidget_) dailyReadingBarWidget_->redraw();
    if (dailyReadingCompleteButton_) dailyReadingCompleteButton_->redraw();
    if (dailyReadingPlanButton_) dailyReadingPlanButton_->redraw();
    for (auto& col : parallelHeaderColumns_) {
        if (col.moduleChoice) col.moduleChoice->redraw();
        if (col.removeButton) col.removeButton->redraw();
    }
    redraw();
}

void BiblePane::refreshDailyReadingPlanBar() {
    if (!dailyReadingBarWidget_ || !dailyReadingCompleteButton_ || !dailyReadingPlanButton_) {
        return;
    }

    layoutDailyReadingBar();

    RightPane* rightPane =
        (app_ && app_->mainWindow()) ? app_->mainWindow()->rightPane() : nullptr;
    DailyWorkspaceState state;
    if (rightPane) {
        state = rightPane->currentDailyWorkspaceState();
    }
    if (!reading::isIsoDateInRange(state.readingPlanSelectedDateIso)) {
        state.readingPlanSelectedDateIso = reading::formatIsoDate(reading::today());
    }

    const std::string planLabel =
        rightPane ? rightPane->selectedDailyReadingPlanLabel() : std::string();
    std::string summaryHtml =
        rightPane ? rightPane->selectedDailyReadingPlanSummaryHtml(
                        state.readingPlanSelectedDateIso, false)
                  : std::string();
    std::vector<DailyReadingBarWidget::Chunk> chunks;
    chunks.push_back({monthDayLabel(state.readingPlanSelectedDateIso), ""});
    chunks.push_back({": ", ""});
    if (!summaryHtml.empty()) {
        auto summaryChunks = chunksFromSummaryHtml(summaryHtml);
        chunks.insert(chunks.end(), summaryChunks.begin(), summaryChunks.end());
    } else if (!planLabel.empty()) {
        chunks.push_back({"No readings scheduled.", ""});
    } else {
        chunks.push_back({"No reading plan selected.", ""});
    }

    dailyReadingPlanButton_->copy_label(u8"☰");
    dailyReadingPlanButton_->tooltip("Reading Plan");

    bool canComplete = false;
    bool completed = false;
    if (app_ &&
        state.readingPlanSource == DailyReadingPlanSource::Editable &&
        state.readingPlanId > 0) {
        ReadingPlan plan;
        if (app_->readingPlanManager().getPlan(state.readingPlanId, plan)) {
            for (const auto& day : plan.days) {
                if (day.dateIso == state.readingPlanSelectedDateIso) {
                    canComplete = true;
                    completed = day.completed;
                    break;
                }
            }
        }
    } else if (app_ &&
               state.readingPlanSource == DailyReadingPlanSource::SwordModule &&
               !state.swordReadingPlanModule.empty() &&
               summaryHtmlHasReadingLinks(summaryHtml)) {
        canComplete = true;
        completed = app_->readingPlanManager().swordDayCompleted(
            state.swordReadingPlanModule, state.readingPlanSelectedDateIso);
    }

    dailyReadingCompleteButton_->value(completed ? 1 : 0);
    if (canComplete) dailyReadingCompleteButton_->activate();
    else dailyReadingCompleteButton_->deactivate();

    static_cast<DailyReadingBarWidget*>(dailyReadingBarWidget_)->setChunks(std::move(chunks));
}

void BiblePane::selectVerse(int verse) {
    if (moduleName_.empty() || currentBook_.empty()) return;

    int maxVerse = app_->swordManager().getVerseCount(moduleName_, currentBook_, currentChapter_);
    if (maxVerse <= 0) maxVerse = 1;
    int clampedVerse = std::max(1, std::min(verse, maxVerse));
    int oldVerse = currentVerse_;
    bool changed = (clampedVerse != currentVerse_);

    currentVerse_ = clampedVerse;
    if (changed) {
        if (canUpdateVerseSelectionInPlace()) {
            syncVerseSelectionInPlace(oldVerse, currentVerse_);
        } else {
            updateDisplay();
        }
    }
    syncReferenceInput();
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
    perf::ScopeTimer timer("BiblePane::setStudyState");
    if (!module.empty()) {
        moduleName_ = module;
    } else if (moduleName_.empty()) {
        auto bibles = app_->swordManager().getBibleModules();
        if (!bibles.empty()) moduleName_ = bibles.front().name;
    }

    currentBook_ = book.empty()
        ? currentBook_
        : canonicalBookLabelForModule(app_, moduleName_, book);
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

    applyModuleChoiceValue(moduleChoice_, moduleName_);

    if (parallelButton_) {
        parallelButton_->value(parallelMode_ ? 1 : 0);
    }
    if (paragraphButton_) {
        paragraphButton_->value(paragraphMode_ ? 1 : 0);
    }
    syncOptionButtons();
    layoutNavBarControls();

    populateBooks();

    int maxVerse = app_->swordManager().getVerseCount(moduleName_, currentBook_, currentChapter_);
    if (maxVerse <= 0) maxVerse = 1;
    currentVerse_ = std::max(1, std::min(currentVerse_, maxVerse));

    syncReferenceInput();

    // Ensure parallel header visibility/layout matches restored state even when
    // caller restores a pre-rendered HTML snapshot instead of calling updateDisplay().
    int contentY = y() + kNavH + kContentPadding;
    int headerH = (parallelMode_ ? kParallelHeaderH : 0);
    if (parallelHeader_) {
        if (parallelMode_) {
            syncParallelHeader();
            parallelHeader_->show();
        } else {
            parallelHeader_->hide();
        }
        parallelHeader_->resize(x(), contentY, w(), headerH);
        parallelHeader_->damage(FL_DAMAGE_ALL);
        layoutParallelHeader();
    }
    if (htmlWidget_) {
        int textY = contentY + headerH;
        int textH = std::max(20, h() - kNavH - kContentPadding - headerH - kDailyReadingBarH);
        htmlWidget_->resize(x(), textY, w(), textH);
    }
    layoutDailyReadingBar();
    refreshDailyReadingPlanBar();
}

BiblePane::DisplayBuffer BiblePane::captureDisplayBuffer() const {
    DisplayBuffer buf;
    if (!htmlWidget_) return buf;

    HtmlWidget::Snapshot snap = htmlWidget_->captureSnapshot();
    buf.doc = snap.doc;
    buf.html = std::move(snap.html);
    buf.baseUrl = std::move(snap.baseUrl);
    buf.scrollY = snap.scrollY;
    buf.contentHeight = snap.contentHeight;
    buf.renderWidth = snap.renderWidth;
    buf.storedSelectedVerse = htmlStoredSelectedVerse_;
    buf.scrollbarVisible = snap.scrollbarVisible;
    buf.valid = snap.valid;
    return buf;
}

BiblePane::DisplayBuffer BiblePane::takeDisplayBuffer() {
    DisplayBuffer buf;
    if (!htmlWidget_) return buf;

    HtmlWidget::Snapshot snap = htmlWidget_->takeSnapshot();
    buf.doc = std::move(snap.doc);
    buf.html = std::move(snap.html);
    buf.baseUrl = std::move(snap.baseUrl);
    buf.scrollY = snap.scrollY;
    buf.contentHeight = snap.contentHeight;
    buf.renderWidth = snap.renderWidth;
    buf.storedSelectedVerse = htmlStoredSelectedVerse_;
    buf.scrollbarVisible = snap.scrollbarVisible;
    buf.valid = snap.valid;
    return buf;
}

void BiblePane::restoreDisplayBuffer(const DisplayBuffer& buffer) {
    perf::ScopeTimer timer("BiblePane::restoreDisplayBuffer(copy)");
    if (!htmlWidget_ || !buffer.valid) return;
    HtmlWidget::Snapshot snap;
    snap.doc = buffer.doc;
    snap.html = buffer.html;
    snap.baseUrl = buffer.baseUrl;
    snap.scrollY = buffer.scrollY;
    snap.contentHeight = buffer.contentHeight;
    snap.renderWidth = buffer.renderWidth;
    snap.scrollbarVisible = buffer.scrollbarVisible;
    snap.valid = buffer.valid;
    htmlStoredSelectedVerse_ = buffer.storedSelectedVerse;
    htmlWidget_->restoreSnapshot(snap);
    if (currentVerse_ != htmlStoredSelectedVerse_ &&
        canUpdateVerseSelectionInPlace()) {
        syncVerseSelectionInPlace(htmlStoredSelectedVerse_, currentVerse_);
    }
}

void BiblePane::restoreDisplayBuffer(DisplayBuffer&& buffer) {
    perf::ScopeTimer timer("BiblePane::restoreDisplayBuffer(move)");
    if (!htmlWidget_ || !buffer.valid) return;
    HtmlWidget::Snapshot snap;
    snap.doc = std::move(buffer.doc);
    snap.html = std::move(buffer.html);
    snap.baseUrl = std::move(buffer.baseUrl);
    snap.scrollY = buffer.scrollY;
    snap.contentHeight = buffer.contentHeight;
    snap.renderWidth = buffer.renderWidth;
    snap.scrollbarVisible = buffer.scrollbarVisible;
    snap.valid = buffer.valid;
    htmlStoredSelectedVerse_ = buffer.storedSelectedVerse;
    buffer.valid = false;
    htmlWidget_->restoreSnapshot(std::move(snap));
    if (currentVerse_ != htmlStoredSelectedVerse_ &&
        canUpdateVerseSelectionInPlace()) {
        syncVerseSelectionInPlace(htmlStoredSelectedVerse_, currentVerse_);
    }
}

void BiblePane::buildNavBar() {
    int cx = navBar_->x() + 2;
    int cy = navBar_->y() + 2;
    int nh = navBar_->h() - 4;

    prevButton_ = new Fl_Button(cx, cy, 30, nh, "@<");
    prevButton_->callback(onPrev, this);
    prevButton_->tooltip("Previous chapter");
    cx += 32;

    bookChoice_ = new BibleBookChoice(cx, cy, 120, nh);
    bookChoice_->callback(onBookChange, this);
    bookChoice_->tooltip("Select book");
    bookChoice_->setBrowserLineSpacing(app_ ? app_->appearanceSettings().browserLineSpacing : 0);
    cx += 122;

    chapterChoice_ = new WrappingChoice(cx, cy, 50, nh);
    chapterChoice_->callback(onChapterChange, this);
    chapterChoice_->tooltip("Select chapter");
    cx += 52;

    nextButton_ = new Fl_Button(cx, cy, 30, nh, "@>");
    nextButton_->callback(onNext, this);
    nextButton_->tooltip("Next chapter");
    cx += 32;

    historyLeftSeparator_ = new Fl_Box(cx, cy + 4, 4, std::max(10, nh - 8));
    historyLeftSeparator_->box(FL_THIN_DOWN_BOX);
    cx += historyLeftSeparator_->w() + 2;

    historyBackButton_ = new Fl_Button(cx, cy, 24, nh, "@undo");
    historyBackButton_->callback(onHistoryBack, this);
    historyBackButton_->tooltip("Back in this study tab's history");
    historyBackButton_->box(FL_THIN_UP_BOX);
    historyBackButton_->deactivate();
    cx += historyBackButton_->w() + 2;

    historyChoice_ = new WrappingInputChoice(cx, cy, kHistoryChoiceWidth, nh);
    historyChoice_->callback(onHistoryChoice, this);
    historyChoice_->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);
    historyChoice_->input()->when(FL_WHEN_ENTER_KEY_ALWAYS);
    historyChoice_->tooltip("Type a reference and press Enter, or pick from this tab's history");
    historyChoice_->input()->tooltip("Type a reference and press Enter, or pick from this tab's history");
    cx += kHistoryChoiceWidth + 2;

    historyForwardButton_ = new Fl_Button(cx, cy, 24, nh, "@redo");
    historyForwardButton_->callback(onHistoryForward, this);
    historyForwardButton_->tooltip("Forward in this study tab's history");
    historyForwardButton_->box(FL_THIN_UP_BOX);
    historyForwardButton_->deactivate();
    cx += historyForwardButton_->w() + 2;

    historyRightSeparator_ = new Fl_Box(cx, cy + 4, 4, std::max(10, nh - 8));
    historyRightSeparator_->box(FL_THIN_DOWN_BOX);
    cx += historyRightSeparator_->w() + 2;

    moduleChoice_ = new WrappingChoice(cx, cy, 100, nh);
    moduleChoice_->callback(onModuleChange, this);
    moduleChoice_->tooltip("Select Bible module");

    auto bibles = app_->swordManager().getBibleModules();
    module_choice::populateChoice(moduleChoice_, bibles,
                                  bibleChoiceModules_, bibleChoiceLabels_);
    if (moduleChoice_->size() > 0) {
        moduleName_ = bibleChoiceModules_.front();
    }
    cx += 102;

    moduleRightSeparator_ = new Fl_Box(cx, cy + 4, 4, std::max(10, nh - 8));
    moduleRightSeparator_->box(FL_THIN_DOWN_BOX);
    cx += moduleRightSeparator_->w() + 2;

    displayOptionsMenuButton_ = new Fl_Menu_Button(cx, cy, kDisplayOptionsMenuButtonW, nh, u8"☰");
    displayOptionsMenuButton_->tooltip("Display options");
    displayOptionsMenuButton_->add("Paragraph mode", 0, onParagraphToggle, this, FL_MENU_TOGGLE);
    displayOptionsMenuButton_->add("Words of Jesus in red", 0, onRedWordsToggle, this, FL_MENU_TOGGLE);
    displayOptionsMenuButton_->add("Strong's markers", 0, onStrongsToggle, this, FL_MENU_TOGGLE);
    displayOptionsMenuButton_->add("Morphology markers", 0, onMorphToggle, this, FL_MENU_TOGGLE);
    displayOptionsMenuButton_->add("Footnotes", 0, onFootnotesToggle, this, FL_MENU_TOGGLE);
    displayOptionsMenuButton_->add("Cross references", 0, onCrossRefsToggle, this, FL_MENU_TOGGLE);
    displayOptionsMenuButton_->hide();

    paragraphButton_ = new Fl_Button(cx, cy, kParagraphButtonW, nh, "\xC2\xB6");
    paragraphButton_->callback(onParagraphToggle, this);
    paragraphButton_->tooltip("Toggle paragraph / verse-per-line display");
    paragraphButton_->type(FL_TOGGLE_BUTTON);
    cx += paragraphButton_->w() + 2;

    redWordsToggleButton_ = new Fl_Button(cx, cy, kRedWordsButtonW, nh, "Red");
    redWordsToggleButton_->callback(onRedWordsToggle, this);
    redWordsToggleButton_->tooltip("Highlight or unhighlight words of Jesus in red");
    redWordsToggleButton_->type(FL_TOGGLE_BUTTON);
    redWordsToggleButton_->labelcolor(fl_rgb_color(170, 0, 0));
    cx += redWordsToggleButton_->w() + 2;

    strongsToggleButton_ = new Fl_Button(cx, cy, kStrongsButtonW, nh, u8"αא");
    strongsToggleButton_->callback(onStrongsToggle, this);
    strongsToggleButton_->tooltip("Show or hide inline Strong's markers");
    strongsToggleButton_->type(FL_TOGGLE_BUTTON);
    cx += strongsToggleButton_->w() + 2;

    morphToggleButton_ = new Fl_Button(cx, cy, kMorphButtonW, nh, "Morph");
    morphToggleButton_->callback(onMorphToggle, this);
    morphToggleButton_->tooltip("Show or hide inline morphology markers");
    morphToggleButton_->type(FL_TOGGLE_BUTTON);
    cx += morphToggleButton_->w() + 2;

    footnotesToggleButton_ = new Fl_Button(cx, cy, kFootnotesButtonW, nh, "Notes");
    footnotesToggleButton_->callback(onFootnotesToggle, this);
    footnotesToggleButton_->tooltip("Show or hide inline footnote markers");
    footnotesToggleButton_->type(FL_TOGGLE_BUTTON);
    cx += footnotesToggleButton_->w() + 2;

    crossRefsToggleButton_ = new Fl_Button(cx, cy, kCrossRefsButtonW, nh, "Xref");
    crossRefsToggleButton_->callback(onCrossRefsToggle, this);
    crossRefsToggleButton_->tooltip("Show or hide inline cross-reference markers");
    crossRefsToggleButton_->type(FL_TOGGLE_BUTTON);
    cx += crossRefsToggleButton_->w() + 2;

    crossRefsRightSeparator_ = new Fl_Box(cx, cy + 4, 4, std::max(10, nh - 8));
    crossRefsRightSeparator_->box(FL_THIN_DOWN_BOX);
    cx += crossRefsRightSeparator_->w() + 2;

    parallelButton_ = new Fl_Button(cx, cy, kParallelButtonW, nh, "||");
    parallelButton_->callback(onParallel, this);
    parallelButton_->tooltip("Toggle parallel Bible view");
    parallelButton_->type(FL_TOGGLE_BUTTON);
    cx += parallelButton_->w() + 2;

    parallelAddButton_ = new Fl_Button(cx, cy, kParallelButtonW, nh, "+");
    parallelAddButton_->callback(onParallelAdd, this);
    parallelAddButton_->tooltip("Add parallel Bible column (up to 7)");
    parallelAddButton_->hide();
    cx += parallelAddButton_->w() + 2;

    navSpacer_ = new Fl_Box(cx, cy, 0, nh);
}

void BiblePane::updateDisplay() {
    perf::ScopeTimer timer("BiblePane::updateDisplay");
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
        htmlStoredSelectedVerse_ = 0;
        htmlWidget_->scrollToTop();
        return;
    }

    if (currentBook_.empty()) {
        populateBooks();
    }
    if (currentBook_.empty()) {
        htmlWidget_->setHtml("<div class=\"chapter\"><p><i>No books available in selected module.</i></p></div>");
        htmlStoredSelectedVerse_ = 0;
        htmlWidget_->scrollToTop();
        return;
    }

    std::string html;
    bool hasVerseMarkup = true;
    perf::StepTimer step;
    auto verseDecorator = [this](const std::string& verseRef) {
        return buildVerseTagMarkersHtml(app_, verseRef);
    };

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
            currentVerse_, verseDecorator);
        perf::logf("BiblePane::updateDisplay getParallelText (%zu cols): %.3f ms",
                   parallelModules_.size(), step.elapsedMs());
        step.reset();
    } else {
        parallelHeader_->hide();
        if (parallelAddButton_) {
            parallelAddButton_->hide();
        }
        html = app_->swordManager().getChapterText(
            moduleName_, currentBook_, currentChapter_, paragraphMode_,
            currentVerse_, verseDecorator);
        perf::logf("BiblePane::updateDisplay getChapterText %s %s %d: %.3f ms",
                   moduleName_.c_str(), currentBook_.c_str(), currentChapter_,
                   step.elapsedMs());
        step.reset();
    }

    if (html.empty()) {
        hasVerseMarkup = false;
        html = "<div class=\"chapter\"><p><i>No text available for current reference.</i></p></div>";
    }

    syncReferenceInput();

    int contentY = y() + kNavH + kContentPadding;
    int headerH = parallelMode_ ? kParallelHeaderH : 0;
    parallelHeader_->resize(x(), contentY, w(), headerH);
    parallelHeader_->damage(FL_DAMAGE_ALL);
    layoutParallelHeader();
    int textY = contentY + headerH;
    int textH = std::max(20, h() - kNavH - kContentPadding - headerH - kDailyReadingBarH);
    htmlWidget_->resize(x(), textY, w(), textH);

    htmlWidget_->setHtml(html);
    htmlStoredSelectedVerse_ = hasVerseMarkup ? currentVerse_ : 0;
    perf::logf("BiblePane::updateDisplay htmlWidget_->setHtml: %.3f ms", step.elapsedMs());
    step.reset();
    if (currentVerse_ > 0) {
        htmlWidget_->scrollToAnchor("v" + std::to_string(currentVerse_));
    } else {
        htmlWidget_->scrollToTop();
    }
    htmlWidget_->redraw();
    refreshDailyReadingPlanBar();
    redrawChrome();
    perf::logf("BiblePane::updateDisplay scroll+redraw: %.3f ms", step.elapsedMs());
}

bool BiblePane::canUpdateVerseSelectionInPlace() const {
    return htmlWidget_ &&
           !moduleName_.empty() &&
           !currentBook_.empty() &&
           htmlStoredSelectedVerse_ > 0 &&
           !htmlWidget_->currentHtml().empty();
}

void BiblePane::syncVerseSelectionInPlace(int oldVerse, int newVerse) {
    perf::ScopeTimer timer("BiblePane::syncVerseSelectionInPlace");
    if (!htmlWidget_) return;
    htmlWidget_->updateElementClassById(verseElementId(oldVerse),
                                        verseElementId(newVerse),
                                        "verse-selected",
                                        false);
    htmlWidget_->updateElementTreeStyleSnippetById(
        verseElementId(oldVerse),
        verseElementId(newVerse),
        selectedVerseInlineStyleSnippet(),
        false);
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

void BiblePane::syncReferenceInput() {
    if (!historyChoice_) return;
    if (currentBook_.empty() || currentChapter_ <= 0) {
        historyChoice_->value("");
        return;
    }

    std::string ref = currentBook_ + " " + std::to_string(currentChapter_) +
                      ":" + std::to_string(std::max(1, currentVerse_));
    historyChoice_->value(ref.c_str());
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

void BiblePane::populateParallelChoice(Fl_Choice* choice) {
    if (!choice) return;
    auto bibles = app_->swordManager().getBibleModules();
    module_choice::populateChoice(choice, bibles,
                                  bibleChoiceModules_, bibleChoiceLabels_);
}

void BiblePane::applyModuleChoiceValue(Fl_Choice* choice, const std::string& module) const {
    module_choice::applyChoiceValue(choice, bibleChoiceModules_,
                                    bibleChoiceLabels_, module);
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

        col.moduleChoice = new WrappingChoice(col.group->x() + 2, col.group->y() + 2,
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

    if (parallelModules_.size() <= 1) {
        if (!parallelModules_.empty() && moduleName_ != parallelModules_.front()) {
            moduleName_ = parallelModules_.front();
            applyModuleChoiceValue(moduleChoice_, moduleName_);
            populateBooks();
            int maxVerse = app_->swordManager().getVerseCount(moduleName_, currentBook_, currentChapter_);
            if (maxVerse <= 0) maxVerse = 1;
            currentVerse_ = std::max(1, std::min(currentVerse_, maxVerse));
        }
        parallelMode_ = false;
        if (parallelButton_) parallelButton_->value(0);
    } else if (moduleName_ != parallelModules_.front()) {
        moduleName_ = parallelModules_.front();
        applyModuleChoiceValue(moduleChoice_, moduleName_);
        populateBooks();
        int maxVerse = app_->swordManager().getVerseCount(moduleName_, currentBook_, currentChapter_);
        if (maxVerse <= 0) maxVerse = 1;
        currentVerse_ = std::max(1, std::min(currentVerse_, maxVerse));
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
    }

    updateDisplay();
    notifyContextChanged();
}

void BiblePane::populateBooks(bool force) {
    if (!bookChoice_ || moduleName_.empty()) return;

    bool needRebuild = force || populatedBookModule_ != moduleName_ || bookChoice_->size() <= 0;
    if (!needRebuild) {
        int selectedIndex = findChoiceIndexByLabel(bookChoice_, currentBook_);
        if (selectedIndex >= 0) {
            bookChoice_->value(selectedIndex);
            populateChapters(false);
            return;
        }
        needRebuild = true;
    }

    if (needRebuild) {
        bookChoice_->clear();
        auto oldTestamentBooks = app_->swordManager().getBookNamesForTestament(moduleName_, 1);
        auto newTestamentBooks = app_->swordManager().getBookNamesForTestament(moduleName_, 2);
        std::vector<std::string> books = oldTestamentBooks;
        books.insert(books.end(), newTestamentBooks.begin(), newTestamentBooks.end());
        for (const auto& book : books) {
            bookChoice_->add(book.c_str());
        }
        bookChoice_->setBookColumns(oldTestamentBooks, newTestamentBooks);
        populatedBookModule_ = moduleName_;

        if (books.empty()) {
            currentBook_.clear();
            populatedChapterCount_ = 0;
            if (chapterChoice_) chapterChoice_->clear();
            return;
        }

        int selectedIndex = findChoiceIndexByLabel(bookChoice_, currentBook_);
        if (selectedIndex >= 0) {
            bookChoice_->value(selectedIndex);
        } else {
            currentBook_ = books.front();
            bookChoice_->value(0);
        }

        populateChapters(true);
    }
}

void BiblePane::populateChapters(bool force) {
    if (!chapterChoice_ || moduleName_.empty() || currentBook_.empty()) return;

    int count = app_->swordManager().getChapterCount(moduleName_, currentBook_);
    if (count <= 0) {
        currentChapter_ = 1;
        populatedChapterCount_ = 0;
        if (chapterChoice_->size() > 0) {
            chapterChoice_->clear();
        }
        return;
    }

    bool needRebuild = force || populatedChapterCount_ != count || chapterChoice_->size() != count;
    if (needRebuild) {
        chapterChoice_->clear();
        for (int i = 1; i <= count; i++) {
            chapterChoice_->add(std::to_string(i).c_str());
        }
        populatedChapterCount_ = count;
    }

    if (currentChapter_ < 1) currentChapter_ = 1;
    if (currentChapter_ > count) currentChapter_ = count;
    chapterChoice_->value(currentChapter_ - 1);
}

void BiblePane::openDailyReadingPlanWorkspace() {
    if (!app_ || !app_->mainWindow() || !app_->mainWindow()->rightPane()) return;

    RightPane* rightPane = app_->mainWindow()->rightPane();
    DailyWorkspaceState state = rightPane->currentDailyWorkspaceState();
    state.mode = DailyWorkspaceMode::ReadingPlans;
    if (!reading::isIsoDateInRange(state.readingPlanSelectedDateIso)) {
        state.readingPlanSelectedDateIso.clear();
    }
    rightPane->setDailyWorkspaceState(state);
    rightPane->setDevotionsPlansTabActive(true);
}

void BiblePane::onDailyReadingBarLink(const std::string& url) {
    if (url.rfind("verdad-plan://open", 0) == 0) {
        const std::string ref = firstReadingListItem(extractQueryValue(url, "ref"));
        if (!ref.empty()) {
            navigateToReference(ref);
        }
        return;
    }

    onLinkClicked(url);
}

void BiblePane::updateDailyReadingPlanCompleted(bool completed) {
    if (!app_ || !app_->mainWindow() || !app_->mainWindow()->rightPane()) {
        refreshDailyReadingPlanBar();
        return;
    }

    RightPane* rightPane = app_->mainWindow()->rightPane();
    DailyWorkspaceState state = rightPane->currentDailyWorkspaceState();
    if (!reading::isIsoDateInRange(state.readingPlanSelectedDateIso)) {
        refreshDailyReadingPlanBar();
        return;
    }

    bool ok = false;
    if (state.readingPlanSource == DailyReadingPlanSource::Editable &&
        state.readingPlanId > 0) {
        ReadingPlan plan;
        if (!app_->readingPlanManager().getPlan(state.readingPlanId, plan)) {
            refreshDailyReadingPlanBar();
            return;
        }

        bool hasDay = false;
        for (const auto& day : plan.days) {
            if (day.dateIso == state.readingPlanSelectedDateIso) {
                hasDay = true;
                break;
            }
        }
        if (!hasDay) {
            refreshDailyReadingPlanBar();
            return;
        }

        ok = app_->readingPlanManager().setDayCompleted(
            state.readingPlanId, state.readingPlanSelectedDateIso, completed);
    } else if (state.readingPlanSource == DailyReadingPlanSource::SwordModule &&
               !state.swordReadingPlanModule.empty()) {
        const std::string summaryHtml =
            rightPane->selectedDailyReadingPlanSummaryHtml(state.readingPlanSelectedDateIso,
                                                           false);
        if (!summaryHtmlHasReadingLinks(summaryHtml)) {
            refreshDailyReadingPlanBar();
            return;
        }
        ok = app_->readingPlanManager().setSwordDayCompleted(
            state.swordReadingPlanModule, state.readingPlanSelectedDateIso, completed);
    }

    if (!ok) {
        app_->mainWindow()->showTransientStatus("Failed to update reading plan completion.");
        refreshDailyReadingPlanBar();
        return;
    }

    rightPane->setDailyWorkspaceState(state);
}

void BiblePane::notifyContextChanged() {
    if (app_->mainWindow()) {
        app_->mainWindow()->onBibleStudyContextChanged();
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

void BiblePane::onHistoryBack(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    if (!self || !self->app_ || !self->app_->mainWindow()) return;
    self->app_->mainWindow()->navigateHistoryBack();
}

void BiblePane::onHistoryChoice(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    if (!self || !self->historyChoice_) return;

    if (historyInputChoice(self->historyChoice_)->consumeMenuSelectionCallback() ||
        self->historyChoice_->menubutton()->changed()) {
        if (self->app_ && self->app_->mainWindow()) {
            self->app_->mainWindow()->navigateToHistoryMenuIndex(
                self->historyChoice_->menubutton()->value());
        }
    } else {
        const char* ref = self->historyChoice_->value();
        if (ref && ref[0]) {
            self->navigateToReference(ref);
        }
    }

    self->historyChoice_->clear_changed();
    self->historyChoice_->input()->clear_changed();
    self->historyChoice_->menubutton()->clear_changed();
}

void BiblePane::onHistoryForward(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    if (!self || !self->app_ || !self->app_->mainWindow()) return;
    self->app_->mainWindow()->navigateHistoryForward();
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
    std::string module = module_choice::selectedModuleName(
        self->moduleChoice_, self->bibleChoiceModules_);
    if (!module.empty()) self->setModule(module);
}

void BiblePane::onParallel(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    self->toggleParallel();
}

void BiblePane::onParagraphToggle(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    self->toggleParagraphMode();
}

void BiblePane::onRedWordsToggle(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    if (!self || !self->app_) return;

    auto options = self->app_->optionDisplaySettings();
    options.showWordsOfChristRed = !options.showWordsOfChristRed;
    self->app_->setOptionDisplaySettings(options);
    self->syncOptionButtons();
}

void BiblePane::onStrongsToggle(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    if (!self || !self->app_) return;

    auto options = self->app_->optionDisplaySettings();
    options.showStrongsMarkers = !options.showStrongsMarkers;
    self->app_->setOptionDisplaySettings(options);
    self->syncOptionButtons();
}

void BiblePane::onMorphToggle(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    if (!self || !self->app_) return;

    auto options = self->app_->optionDisplaySettings();
    options.showMorphMarkers = !options.showMorphMarkers;
    self->app_->setOptionDisplaySettings(options);
    self->syncOptionButtons();
}

void BiblePane::onFootnotesToggle(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    if (!self || !self->app_) return;

    auto options = self->app_->optionDisplaySettings();
    options.showFootnoteMarkers = !options.showFootnoteMarkers;
    self->app_->setOptionDisplaySettings(options);
    self->syncOptionButtons();
}

void BiblePane::onCrossRefsToggle(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    if (!self || !self->app_) return;

    auto options = self->app_->optionDisplaySettings();
    options.showCrossReferenceMarkers = !options.showCrossReferenceMarkers;
    self->app_->setOptionDisplaySettings(options);
    self->syncOptionButtons();
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
    std::string module = module_choice::selectedModuleName(
        choice, self->bibleChoiceModules_);
    if (!module.empty()) self->setParallelModuleAt(index, module);
}

void BiblePane::onDailyReadingComplete(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    if (!self || !self->dailyReadingCompleteButton_) return;
    self->updateDailyReadingPlanCompleted(self->dailyReadingCompleteButton_->value() != 0);
}

void BiblePane::onDailyReadingPlanButton(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<BiblePane*>(data);
    if (!self) return;
    self->openDailyReadingPlanWorkspace();
}

void BiblePane::onLinkClicked(const std::string& url) {
    if (!app_ || !app_->mainWindow()) return;

    if (url.find("verse:") == 0) {
        try {
            int verse = std::stoi(url.substr(6));
            selectVerse(verse);
        } catch (...) {
            // Ignore malformed verse links.
        }
    } else if (url.find("tags:") == 0) {
        std::string verseRef = url.substr(5);
        if (app_->mainWindow() && app_->mainWindow()->leftPane() &&
            app_->mainWindow()->leftPane()->tagPanel()) {
            app_->mainWindow()->leftPane()->showTagTab();
            app_->mainWindow()->leftPane()->tagPanel()->showTagsForVerse(verseRef);
        }
    } else if (url.find("strongs:") == 0 || url.find("strong:") == 0) {
        app_->mainWindow()->showWordInfoNow("", url, "", "");
    } else if (url.find("morph:") == 0) {
        app_->mainWindow()->showWordInfoNow("", url, "", "");
    } else {
        std::string contextKey = currentBook_ + " " + std::to_string(currentChapter_);
        std::string verseModule = currentModule();
        auto refs = app_->swordManager().verseReferencesFromLink(
            url, contextKey, verseModule);

        if (refs.size() > 1) {
            if (app_->mainWindow()->leftPane()) {
                app_->mainWindow()->leftPane()->showReferenceResults(
                    verseModule, refs, "(linked verses)");
            }
            return;
        }
        if (refs.size() == 1) {
            if (app_->mainWindow()->leftPane()) {
                app_->mainWindow()->leftPane()->setVersePreviewText(
                    app_->swordManager().getVerseText(verseModule, refs.front()),
                    verseModule,
                    refs.front());
            }
            return;
        }

        std::string previewHtml = app_->swordManager().buildLinkPreviewHtml(
            verseModule, contextKey, url, verseModule);
        if (!previewHtml.empty()) {
            if (app_->mainWindow()->leftPane()) {
                app_->mainWindow()->leftPane()->setPreviewText(
                    previewHtml, verseModule, contextKey);
            }
            return;
        }

        if (url.find("sword://") == 0) {
            std::string ref = url.substr(8);
            navigateToReference(ref);
        }
    }
}

void BiblePane::onWordHover(const std::string& word, const std::string& href,
                             const std::string& strong, const std::string& morph,
                             const std::string& /*module*/,
                             int x, int y) {
    if (app_->mainWindow()) {
        if (href.rfind("tags:", 0) == 0) {
            app_->mainWindow()->hideWordInfo();
        } else if (!strong.empty() || !morph.empty() || !href.empty()) {
            app_->mainWindow()->showWordInfo(word, href, strong, morph, x, y);
        } else {
            app_->mainWindow()->hideWordInfo();
        }
    }
}

void BiblePane::onContextMenu(const std::string& word, const std::string& href,
                               const std::string& strong, const std::string& morph,
                               const std::string& module,
                               int x, int y) {
    VerseContext::SelectionContext selection;
    if (htmlWidget_) {
        HtmlWidget::SelectionInfo info = htmlWidget_->selectionInfo();
        selection.hasSelection = info.hasSelection;
        selection.text = info.text;
        selection.wholeWordText = info.wholeWordText;
        selection.startsInsideWord = info.startsInsideWord;
        selection.endsInsideWord = info.endsInsideWord;
        selection.startVerse = info.startVerse;
        selection.endVerse = info.endVerse;
    }

    int clickedVerse = htmlWidget_ ? htmlWidget_->verseAtScreenPoint(x, y) : 0;
    if (clickedVerse <= 0 && href.rfind("verse:", 0) == 0) {
        try {
            clickedVerse = std::stoi(href.substr(6));
        } catch (...) {
            clickedVerse = 0;
        }
    }
    if (clickedVerse <= 0) {
        clickedVerse = currentVerse_;
    }

    VerseContext ctx(app_);
    ctx.show(word, href, strong, morph, module,
             currentBook_, currentChapter_, clickedVerse, paragraphMode_,
             selection, x, y);
}

} // namespace verdad
