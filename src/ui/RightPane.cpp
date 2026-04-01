#include "ui/RightPane.h"
#include "app/VerdadApp.h"
#include "reading/ReadingPlanUtils.h"
#include "search/SearchIndexer.h"
#include "ui/MonthCalendarWidget.h"
#include "ui/ReadingPlanEditorDialog.h"
#include "ui/BiblePane.h"
#include "ui/FilterableChoiceWidget.h"
#include "ui/HtmlEditorWidget.h"
#include "ui/HtmlWidget.h"
#include "ui/LeftPane.h"
#include "ui/MainWindow.h"
#include "ui/ModuleChoiceUtils.h"
#include "ui/StyledTabs.h"
#include "ui/WrappingChoice.h"
#include "sword/SwordManager.h"
#include "app/PerfTrace.h"

#include <FL/Fl.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_SVG_Image.H>
#include <FL/fl_ask.H>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_set>

namespace verdad {
namespace {

namespace fs = std::filesystem;

constexpr int kStudypadToolbarButtonW = 32;
constexpr int kStudypadToolbarButtonH = 26;
constexpr int kStudypadToolbarIconSize = 22;
constexpr int kStudypadToolbarButtonGap = 2;
constexpr int kDictionaryNavButtonW = 28;
constexpr int kGeneralBookNavButtonW = 28;
constexpr int kGeneralBookContentsButtonW = 78;
constexpr int kGeneralBookOverlayHeaderH = 24;
constexpr int kGeneralBookOverlayMinW = 220;
constexpr int kGeneralBookOverlayMaxW = 360;
constexpr int kGeneralBookPageFillThresholdPx = 280;
constexpr int kGeneralBookMaxLoadedSections = 12;
constexpr int kDailyNavButtonW = 28;
constexpr int kDailyCalendarDrawerH = 252;
constexpr int kDailyActionButtonW = 112;
constexpr int kDailyReadingPlanBottomMinH = 180;
constexpr int kDailyReadingPlanBottomMaxH = 260;

std::string escapeCssString(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::string buildStudypadOdtHtml(VerdadApp* app, const std::string& bodyHtml) {
    std::string family = app ? app->appearanceSettings().textFontFamily : "Serif";
    int size = app ? app->appearanceSettings().textFontSize : 14;
    double lineHeight = app ? app->appearanceSettings().editorLineHeight : 1.2;

    std::ostringstream html;
    html << "<!DOCTYPE html>\n"
         << "<html>\n<head>\n"
         << "<meta charset=\"utf-8\" />\n"
         << "<style>\n"
         << "body { font-family: \"" << escapeCssString(family) << "\"; font-size: "
         << size << "pt; line-height: " << lineHeight << "; margin: 0.4in; }\n"
         << "p { margin: 0 0 0.65em 0; line-height: inherit; }\n"
         << "ul, ol { margin: 0 0 0.65em 1.4em; padding-left: 1.1em; }\n"
         << "li { margin: 0.15em 0; }\n"
         << "hr { border: 0; border-top: 1px solid #777; margin: 0.8em 0; }\n"
         << "</style>\n"
         << "</head>\n<body>\n"
         << bodyHtml
         << "\n</body>\n</html>\n";
    return html.str();
}

std::string buildStudypadVerseInsertHtml(VerdadApp* app,
                                         const std::string& verseReference) {
    if (!app || !app->mainWindow() || !app->mainWindow()->biblePane()) return "";

    std::string module = module_choice::trimCopy(
        app->mainWindow()->biblePane()->currentModule());
    if (module.empty()) return "";

    std::vector<std::string> refs = app->swordManager().verseReferencesFromLink(
        "sword://" + verseReference, "", module);
    if (refs.empty()) {
        refs.push_back(verseReference);
    }

    std::ostringstream html;
    bool first = true;
    for (const auto& ref : refs) {
        std::string verseHtml = app->swordManager().getVerseInsertText(module, ref);
        if (verseHtml.empty()) continue;
        if (!first) html << "<br>";
        html << verseHtml;
        first = false;
    }

    return html.str();
}

std::string commentaryVerseNumberElementId(int verse) {
    if (verse <= 0) return "";
    return "cv" + std::to_string(verse);
}

std::string commentaryVerseNumberSelectedStyle() {
    return "background-color:#e0e8f0;color:#1a5276;";
}

class ResizeAwareTabs : public StyledTabs {
public:
    using ResizeCallback = std::function<void(int, int, int, int)>;

    ResizeAwareTabs(int X, int Y, int W, int H, const char* L = nullptr)
        : StyledTabs(X, Y, W, H, L) {}

    void setResizeCallback(ResizeCallback cb) { resizeCb_ = std::move(cb); }

    void resize(int X, int Y, int W, int H) override {
        StyledTabs::resize(X, Y, W, H);
        if (resizeCb_) resizeCb_(X, Y, W, H);
    }

private:
    ResizeCallback resizeCb_;
};

class ResizeAwareGroup : public Fl_Group {
public:
    using ResizeCallback = std::function<void(int, int, int, int)>;

    ResizeAwareGroup(int X, int Y, int W, int H, const char* L = nullptr)
        : Fl_Group(X, Y, W, H, L) {}

    void setResizeCallback(ResizeCallback cb) { resizeCb_ = std::move(cb); }

    void resize(int X, int Y, int W, int H) override {
        Fl_Group::resize(X, Y, W, H);
        if (resizeCb_) resizeCb_(X, Y, W, H);
    }

private:
    ResizeCallback resizeCb_;
};

enum class StudypadToolbarIcon {
    New,
    Save,
    Export,
    Delete,
};

std::string studypadToolbarIconSvg(StudypadToolbarIcon icon) {
    switch (icon) {
    case StudypadToolbarIcon::New:
        return R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">
  <path d="M4 1.5h5l3 3v10H4z" fill="none" stroke="#2563eb" stroke-width="1.4" stroke-linejoin="round"/>
  <path d="M9 1.5V5h3" fill="none" stroke="#2563eb" stroke-width="1.4" stroke-linejoin="round"/>
  <path d="M8 7v4M6 9h4" fill="none" stroke="#2563eb" stroke-width="1.6" stroke-linecap="round"/>
</svg>
)SVG";
    case StudypadToolbarIcon::Save:
        return R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">
  <path d="M2.5 2.5h9l2 2v9h-11z" fill="none" stroke="#16a34a" stroke-width="1.4" stroke-linejoin="round"/>
  <path d="M5 2.5h5v3H5z" fill="none" stroke="#16a34a" stroke-width="1.2" stroke-linejoin="round"/>
  <path d="M5 10.5h6" fill="none" stroke="#16a34a" stroke-width="1.6" stroke-linecap="round"/>
</svg>
)SVG";
    case StudypadToolbarIcon::Export:
        return R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">
  <path d="M3 12.5h10" fill="none" stroke="#d97706" stroke-width="1.4" stroke-linecap="round"/>
  <path d="M8 3.5v6" fill="none" stroke="#d97706" stroke-width="1.6" stroke-linecap="round"/>
  <path d="M5.5 7l2.5 2.5L10.5 7" fill="none" stroke="#d97706" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>
  <path d="M3.5 2.5h9v8h-9z" fill="none" stroke="#d97706" stroke-width="1.2" stroke-linejoin="round" opacity="0.75"/>
</svg>
)SVG";
    case StudypadToolbarIcon::Delete:
        return R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">
  <path d="M4.2 4.2l7.6 7.6M11.8 4.2l-7.6 7.6" fill="none" stroke="#dc2626" stroke-width="2.1" stroke-linecap="round"/>
</svg>
)SVG";
    }
    return "";
}

Fl_SVG_Image* makeStudypadToolbarIcon(StudypadToolbarIcon icon) {
    std::string svg = studypadToolbarIconSvg(icon);
    auto* image = new Fl_SVG_Image(nullptr, svg.c_str());
    if (image->fail()) {
        delete image;
        return nullptr;
    }
    image->scale(kStudypadToolbarIconSize, kStudypadToolbarIconSize, 1, 1);
    return image;
}

void configureStudypadToolbarButton(Fl_Button* button, StudypadToolbarIcon icon) {
    if (!button) return;

    Fl_SVG_Image* image = makeStudypadToolbarIcon(icon);
    if (!image) return;

    button->bind_image(image);

    Fl_Image* inactiveImage = image->copy(kStudypadToolbarIconSize, kStudypadToolbarIconSize);
    if (inactiveImage) {
        inactiveImage->inactive();
        button->bind_deimage(inactiveImage);
    }
}

int studypadToolbarButtonsWidth() {
    return (kStudypadToolbarButtonW * 4) + (kStudypadToolbarButtonGap * 3);
}

void layoutDictionaryPaneContents(int paneX,
                                  int paneY,
                                  int paneW,
                                  int paneH,
                                  Fl_Button* dictionaryBackButton,
                                  FilterableChoiceWidget* dictionaryKeyInput,
                                  Fl_Button* dictionaryForwardButton,
                                  Fl_Choice* dictionaryChoice,
                                  HtmlWidget* dictionaryHtml) {
    if (!dictionaryBackButton || !dictionaryKeyInput || !dictionaryForwardButton ||
        !dictionaryChoice || !dictionaryHtml) {
        return;
    }

    const int choiceH = 25;

    int clampedPaneW = std::max(20, paneW);
    int clampedPaneH = std::max(20, paneH);

    int rowW = std::max(20, clampedPaneW - 4);
    int keyAreaW = std::clamp((rowW * 7) / 20,
                              150,
                              std::max(150, rowW - 140));
    int choiceW = std::max(20, rowW - keyAreaW - 2);
    int keyX = paneX + 2;
    dictionaryBackButton->resize(keyX, paneY + 2, kDictionaryNavButtonW, choiceH);
    dictionaryKeyInput->resize(keyX + kDictionaryNavButtonW,
                               paneY + 2,
                               std::max(40, keyAreaW - (kDictionaryNavButtonW * 2)),
                               choiceH);
    dictionaryForwardButton->resize(dictionaryKeyInput->x() + dictionaryKeyInput->w(),
                                    paneY + 2,
                                    kDictionaryNavButtonW,
                                    choiceH);
    dictionaryChoice->resize(keyX + keyAreaW + 2, paneY + 2, choiceW, choiceH);
    dictionaryHtml->resize(paneX + 2,
                           paneY + choiceH + 4,
                           rowW,
                           std::max(10, clampedPaneH - choiceH - 6));
}

void layoutDictionaryPane(int paneX,
                          int paneY,
                          int paneW,
                          int paneH,
                          Fl_Group* dictionaryPaneGroup,
                          Fl_Button* dictionaryBackButton,
                          FilterableChoiceWidget* dictionaryKeyInput,
                          Fl_Button* dictionaryForwardButton,
                          Fl_Choice* dictionaryChoice,
                          HtmlWidget* dictionaryHtml) {
    if (!dictionaryPaneGroup || !dictionaryBackButton || !dictionaryKeyInput ||
        !dictionaryForwardButton ||
        !dictionaryChoice || !dictionaryHtml) {
        return;
    }

    const int choiceH = 25;

    int clampedPaneW = std::max(20, paneW);
    int clampedPaneH = std::max(20, paneH);

    dictionaryPaneGroup->resize(paneX, paneY, clampedPaneW, clampedPaneH);
    layoutDictionaryPaneContents(paneX,
                                 paneY,
                                 clampedPaneW,
                                 clampedPaneH,
                                 dictionaryBackButton,
                                 dictionaryKeyInput,
                                 dictionaryForwardButton,
                                 dictionaryChoice,
                                 dictionaryHtml);
}

std::string initialDictionaryModuleForKey(VerdadApp* app,
                                          const std::string& key) {
    if (!app || key.empty()) return "";

    char prefix = static_cast<char>(
        std::toupper(static_cast<unsigned char>(key[0])));
    if (prefix != 'H' && prefix != 'G') return "";

    return app->preferredPreviewDictionary(prefix);
}

std::string moduleLanguageForName(VerdadApp* app,
                                  const std::string& moduleName) {
    if (!app || moduleName.empty()) return "";

    for (const auto& mod : app->swordManager().getModules()) {
        if (mod.name == moduleName) return mod.language;
    }

    return "";
}

std::string defaultDictionaryModuleForLookup(VerdadApp* app,
                                             const std::string& key,
                                             const std::string& contextModule) {
    if (!app || key.empty()) return "";

    std::string strongsModule = initialDictionaryModuleForKey(app, key);
    if (!strongsModule.empty()) return strongsModule;

    std::string language = moduleLanguageForName(app, contextModule);
    return app->preferredWordDictionary(language);
}

std::string trimCopy(const std::string& text) {
    size_t start = 0;
    while (start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }

    size_t end = text.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }

    return text.substr(start, end - start);
}

std::string htmlEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

std::string urlEncode(const std::string& text) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0F]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string urlDecode(const std::string& text) {
    auto hexValue = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };

    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            int hi = hexValue(text[i + 1]);
            int lo = hexValue(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (text[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
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
    std::string ref = trimCopy(reference);
    if (ref.empty()) return ref;

    size_t split = ref.find_first_of(",;");
    if (split == std::string::npos) return ref;

    std::string first = trimCopy(ref.substr(0, split));
    return first.empty() ? ref : first;
}

int readingPlanChoiceIndexForEditableId(
    const std::vector<DailyReadingPlanChoiceItem>& items,
    int planId) {
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].kind == DailyReadingPlanChoiceItem::Kind::EditablePlan &&
            items[i].planId == planId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int readingPlanChoiceIndexForSwordModule(
    const std::vector<DailyReadingPlanChoiceItem>& items,
    const std::string& moduleName) {
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].kind == DailyReadingPlanChoiceItem::Kind::SwordModule &&
            items[i].moduleName == moduleName) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string rewriteSwordReadingPlanLinks(const std::string& html,
                                         std::vector<std::string>* extractedRefs);
std::string extractDailyEntryBodyHtml(const std::string& html);
std::vector<std::string> extractSwordReadingPlanItemHtml(const std::string& html);
std::vector<std::string> extractSwordReadingPlanReferences(
    VerdadApp* app,
    const std::string& html,
    const std::string& moduleName,
    const std::string& dateIso,
    const std::string& verseModuleForRefs);

std::string preferredCalendarVerseModule(VerdadApp* app) {
    if (!app) return "";
    if (app->mainWindow() && app->mainWindow()->biblePane()) {
        std::string module = trimCopy(app->mainWindow()->biblePane()->currentModule());
        if (!module.empty()) return module;
    }

    auto bibles = app->swordManager().getBibleModules();
    return bibles.empty() ? std::string() : bibles.front().name;
}

std::string abbreviatedCalendarReference(VerdadApp* app,
                                         const std::string& verseModule,
                                         const std::string& reference) {
    std::string trimmed = trimCopy(reference);
    if (trimmed.empty()) return trimmed;

    SwordManager::VerseRef parsed = SwordManager::parseVerseRef(trimmed);
    if (parsed.book.empty() || parsed.chapter <= 0) {
        if (app && !verseModule.empty()) {
            std::string shortRef = trimCopy(
                app->swordManager().getShortReference(verseModule, trimmed));
            if (!shortRef.empty()) return shortRef;
        }
        return trimmed;
    }

    std::string abbreviatedBook = parsed.book;
    if (app && !verseModule.empty()) {
        std::ostringstream seed;
        seed << parsed.book << " " << parsed.chapter << ":1";
        std::string shortSeed = trimCopy(
            app->swordManager().getShortReference(verseModule, seed.str()));
        SwordManager::VerseRef shortParsed = SwordManager::parseVerseRef(shortSeed);
        if (!shortParsed.book.empty()) {
            abbreviatedBook = shortParsed.book;
        }
    }

    std::ostringstream out;
    out << abbreviatedBook << " " << parsed.chapter;
    if (parsed.verse > 0) {
        out << ":" << parsed.verse;
        if (parsed.verseEnd > parsed.verse) {
            out << "-" << parsed.verseEnd;
        }
    }
    return out.str();
}

std::string readingPlanDayCalendarSummary(VerdadApp* app, const ReadingPlanDay& day) {
    if (day.passages.empty()) return "(No passages)";

    const std::string verseModule = preferredCalendarVerseModule(app);
    std::ostringstream summary;
    for (size_t i = 0; i < day.passages.size(); ++i) {
        if (i) summary << "\n";
        summary << abbreviatedCalendarReference(app, verseModule,
                                                day.passages[i].reference);
    }
    return summary.str();
}

std::string decodeBasicHtmlEntities(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out.push_back(text[i]);
            continue;
        }

        size_t semi = text.find(';', i);
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
    return trimCopy(decodeBasicHtmlEntities(text));
}

std::string formatSwordMonthDayKey(int month, int day) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d.%02d", month, day);
    return std::string(buffer);
}

std::string swordReadingPlanCalendarSummary(VerdadApp* app,
                                            const std::string& moduleName,
                                            const std::string& lookupKey) {
    if (!app || moduleName.empty() || trimCopy(lookupKey).empty()) {
        return "";
    }

    std::string entryHtml = app->swordManager().getDailyDevotionEntry(moduleName, lookupKey);
    std::string rewrittenBody = extractDailyEntryBodyHtml(
        rewriteSwordReadingPlanLinks(entryHtml, nullptr));
    std::vector<std::string> refs = extractSwordReadingPlanReferences(
        app, rewrittenBody, moduleName, lookupKey, "");
    if (!refs.empty()) {
        std::ostringstream summary;
        for (size_t i = 0; i < refs.size(); ++i) {
            if (i) summary << "\n";
            summary << refs[i];
        }
        return summary.str();
    }

    std::vector<std::string> itemHtml = extractSwordReadingPlanItemHtml(rewrittenBody);
    if (itemHtml.empty()) return "";

    std::ostringstream summary;
    bool first = true;
    for (const auto& item : itemHtml) {
        std::string text = stripSimpleHtml(item);
        if (text.empty()) continue;
        if (!first) summary << "\n";
        summary << text;
        first = false;
    }
    return summary.str();
}

std::vector<std::string> extractSwordReadingPlanReferences(
    VerdadApp* app,
    const std::string& html,
    const std::string& moduleName,
    const std::string& dateIso,
    const std::string& verseModuleForRefs) {
    std::vector<std::string> refs;
    if (html.empty()) return refs;

    static const std::regex hrefRe(
        R"(href\s*=\s*["']([^"']+)["'])",
        std::regex::icase);

    std::unordered_set<std::string> seen;
    auto begin = std::sregex_iterator(html.begin(), html.end(), hrefRe);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::string url = (*it)[1].str();
        std::string compactRef;
        if (url.rfind("verdad-plan://open", 0) == 0) {
            compactRef = firstReadingListItem(extractQueryValue(url, "ref"));
        } else {
            compactRef = trimCopy(SwordManager::verseReferenceFromLink(url));
        }
        if (compactRef.empty()) {
            continue;
        }
        if (seen.insert(compactRef).second) {
            refs.push_back(std::move(compactRef));
        }
    }

    (void)app;
    (void)moduleName;
    (void)dateIso;
    (void)verseModuleForRefs;
    return refs;
}

std::string rewriteSwordReadingPlanLinks(const std::string& html,
                                         std::vector<std::string>* refsOut = nullptr) {
    if (refsOut) refsOut->clear();
    if (html.empty()) return html;

    static const std::regex hrefRe(
        R"(href\s*=\s*["']([^"']+)["'])",
        std::regex::icase);

    std::unordered_set<std::string> seen;
    std::string out;
    out.reserve(html.size() + 64);

    size_t cursor = 0;
    auto begin = std::sregex_iterator(html.begin(), html.end(), hrefRe);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const auto& match = *it;
        size_t pos = static_cast<size_t>(match.position());
        size_t len = static_cast<size_t>(match.length());
        out.append(html, cursor, pos - cursor);

        std::string original = match[1].str();
        std::string compactRef = trimCopy(SwordManager::verseReferenceFromLink(original));
        if (!compactRef.empty()) {
            if (refsOut && seen.insert(compactRef).second) {
                refsOut->push_back(compactRef);
            }
            out += "href=\"verdad-plan://open?ref=";
            out += urlEncode(compactRef);
            out += "\"";
        } else {
            out.append(html, pos, len);
        }

        cursor = pos + len;
    }

    out.append(html, cursor, std::string::npos);
    return out;
}

std::string extractDailyEntryBodyHtml(const std::string& html) {
    std::string body = html;

    size_t outerStart = body.find('>');
    if (outerStart != std::string::npos &&
        body.find("dictionary daily-devotion") != std::string::npos) {
        size_t outerEnd = body.rfind("</div>");
        if (outerEnd != std::string::npos && outerEnd > outerStart) {
            body = body.substr(outerStart + 1, outerEnd - (outerStart + 1));
        }
    }

    static const std::regex entryKeyRe(
        R"(<div[^>]*class\s*=\s*["'][^"']*\bentry-key\b[^"']*["'][^>]*>.*?</div>)",
        std::regex::icase);
    body = std::regex_replace(body, entryKeyRe, "");
    return trimCopy(body);
}

std::vector<std::string> extractSwordReadingPlanItemHtml(const std::string& html) {
    std::string flattened = std::regex_replace(
        html,
        std::regex(R"(<br\s*/?>)", std::regex::icase),
        "\n");
    flattened = std::regex_replace(
        flattened,
        std::regex(R"(</p\s*>)", std::regex::icase),
        "\n");
    flattened = std::regex_replace(
        flattened,
        std::regex(R"(<p\b[^>]*>)", std::regex::icase),
        "");
    flattened = std::regex_replace(
        flattened,
        std::regex(R"(</div\s*>)", std::regex::icase),
        "\n");
    flattened = std::regex_replace(
        flattened,
        std::regex(R"(<div\b[^>]*>)", std::regex::icase),
        "");

    std::vector<std::string> items;
    std::istringstream stream(flattened);
    std::string line;
    while (std::getline(stream, line)) {
        std::string trimmedLine = trimCopy(line);
        if (trimmedLine.empty()) continue;
        if (trimmedLine.find("verdad-plan://open") == std::string::npos) continue;
        items.push_back(std::move(trimmedLine));
    }
    return items;
}

std::string buildReadingPlanPassageLinkHtml(const std::string& reference) {
    std::string trimmedReference = trimCopy(reference);
    if (trimmedReference.empty()) return "";

    std::ostringstream html;
    html << "<a href=\"verdad-plan://open?ref="
         << urlEncode(trimmedReference) << "\">"
         << htmlEscape(trimmedReference) << "</a>";
    return html.str();
}

std::string joinHtmlItems(const std::vector<std::string>& items,
                          const std::string& separatorHtml) {
    std::ostringstream html;
    bool first = true;
    for (const auto& item : items) {
        if (trimCopy(item).empty()) continue;
        if (!first) html << separatorHtml;
        html << item;
        first = false;
    }
    return html.str();
}

const ReadingPlanDay* readingPlanDayForDate(const ReadingPlan& plan,
                                            const std::string& dateIso) {
    for (const auto& day : plan.days) {
        if (day.dateIso == dateIso) return &day;
    }
    return nullptr;
}

std::string buildReadingPlanDayHtml(const ReadingPlan& plan,
                                    const std::string& dateIso) {
    const ReadingPlanDay* day = readingPlanDayForDate(plan, dateIso);

    std::ostringstream html;
    html << "<div class=\"dictionary reading-plan-day\">\n";
    html << "<div class=\"entry-key\">" << htmlEscape(dateIso) << "</div>\n";
    html << "<h2>" << htmlEscape(plan.summary.name) << "</h2>\n";

    if (!trimCopy(plan.summary.description).empty()) {
        html << "<p>" << htmlEscape(plan.summary.description) << "</p>\n";
    }

    if (!day) {
        html << "<p><i>No readings are scheduled for this date.</i></p>\n";
        html << "</div>\n";
        return html.str();
    }

    if (!trimCopy(day->title).empty()) {
        html << "<h3>" << htmlEscape(day->title) << "</h3>\n";
    }

    html << "<p><b>Status:</b> "
         << (day->completed ? "Completed" : "Incomplete")
         << "</p>\n";

    if (day->passages.empty()) {
        html << "<p><i>No passages assigned.</i></p>\n";
    } else {
        html << "<ul>\n";
        for (const auto& passage : day->passages) {
            html << "<li><a href=\"verdad-plan://open?ref="
                 << urlEncode(passage.reference) << "\">"
                 << htmlEscape(passage.reference) << "</a></li>\n";
        }
        html << "</ul>\n";
    }

    html << "</div>\n";
    return html.str();
}

std::string buildSwordReadingPlanDayHtml(const std::string& moduleName,
                                         const std::string& description,
                                         const std::string& dateIso,
                                         const std::string& entryHtml) {
    std::ostringstream html;
    html << "<div class=\"dictionary reading-plan-day sword-reading-plan-day\">\n";
    html << "<div class=\"entry-key\">" << htmlEscape(dateIso) << "</div>\n";
    html << "<h2>" << htmlEscape(moduleName) << "</h2>\n";

    if (!trimCopy(description).empty()) {
        html << "<p>" << htmlEscape(description) << "</p>\n";
    }
    html << "<p><b>Source:</b> Read-only SWORD reading plan</p>\n";

    std::string rewrittenBody = extractDailyEntryBodyHtml(entryHtml);
    std::vector<std::string> itemHtml = extractSwordReadingPlanItemHtml(rewrittenBody);
    if (!itemHtml.empty()) {
        html << "<h3>Readings</h3>\n";
        html << "<ul>\n";
        for (const auto& item : itemHtml) {
            html << "<li>" << item << "</li>\n";
        }
        html << "</ul>\n";
    } else if (trimCopy(rewrittenBody).empty()) {
        html << "<p><i>No readings are scheduled for this date.</i></p>\n";
    } else {
        html << "<div class=\"reading-plan-source-entry\">\n";
        html << rewrittenBody << "\n";
        html << "</div>\n";
    }

    html << "</div>\n";
    return html.str();
}

std::string buildDailyDevotionPageHtml(const std::string& dateIso,
                                       const std::string& readingSummaryHtml,
                                       const std::string& headingHtml,
                                       const std::string& entryHtml) {
    std::string body = extractDailyEntryBodyHtml(entryHtml);
    if (body.empty()) return entryHtml;

    std::ostringstream html;
    html << "<div class=\"dictionary daily-devotion\">\n";
    html << "<div class=\"entry-key\">" << htmlEscape(dateIso) << "</div>\n";
    if (!trimCopy(readingSummaryHtml).empty()) {
        html << readingSummaryHtml << "\n";
    }
    html << "<hr class=\"daily-devotion-divider\" />\n";
    if (!trimCopy(headingHtml).empty()) {
        html << "<h2 class=\"daily-devotion-heading\">"
             << headingHtml
             << "</h2>\n";
    }
    html << "<div class=\"daily-devotion-body\">\n";
    html << body << "\n";
    html << "</div>\n";
    html << "</div>\n";
    return html.str();
}

struct ReadingPlanRescheduleRequest {
    std::string targetDateIso;
};

bool promptReadingPlanReschedule(const std::string& fromDateIso,
                                 ReadingPlanRescheduleRequest& out) {
    Fl_Double_Window dialog(430, 164, "Reschedule Reading Day");
    dialog.set_modal();

    Fl_Box info(16, 14, 398, 34,
                "Choose the new date to restart this reading day and everything after it.");
    info.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);

    Fl_Box fromLabel(16, 58, 74, 24, "Current");
    fromLabel.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Box fromValue(92, 58, 150, 24, fromDateIso.c_str());
    fromValue.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    Fl_Box targetLabel(16, 88, 74, 24, "New date");
    targetLabel.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Input targetInput(92, 86, 120, 26);
    targetInput.value(fromDateIso.c_str());

    Fl_Button cancelButton(250, 126, 76, 28, "Cancel");
    Fl_Return_Button okButton(338, 126, 76, 28, "OK");

    bool accepted = false;
    cancelButton.callback([](Fl_Widget*, void* data) {
        static_cast<Fl_Window*>(data)->hide();
    }, &dialog);
    okButton.callback([](Fl_Widget*, void* data) {
        *static_cast<bool*>(data) = true;
    }, &accepted);

    dialog.end();
    dialog.hotspot(&okButton);
    dialog.show();

    while (dialog.shown()) {
        Fl::wait();
        if (!accepted) continue;

        std::string target = trimCopy(targetInput.value() ? targetInput.value() : "");
        if (!reading::isIsoDateInRange(target)) {
            fl_alert("Enter a valid date in YYYY-MM-DD format.");
            accepted = false;
            continue;
        }

        out.targetDateIso = target;
        dialog.hide();
    }

    return accepted;
}

std::string toLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return text;
}

void selectFirstDictionaryModule(Fl_Choice* dictionaryChoice,
                                 const std::vector<std::string>& moduleNames,
                                 const std::vector<std::string>& displayLabels,
                                 std::string& currentDictionary) {
    if (!dictionaryChoice || dictionaryChoice->size() <= 0) return;
    currentDictionary = moduleNames.empty() ? "" : moduleNames.front();
    module_choice::applyChoiceValue(dictionaryChoice, moduleNames,
                                    displayLabels, currentDictionary);
}

std::string pathLeaf(const std::string& path) {
    if (path.empty()) return "";
    size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

bool endsWithIgnoreCase(const std::string& text, const std::string& suffix) {
    if (suffix.size() > text.size()) return false;
    size_t offset = text.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        unsigned char a = static_cast<unsigned char>(text[offset + i]);
        unsigned char b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

std::string pathWithExtension(const std::string& path, const std::string& extension) {
    if (path.empty()) return path;
    fs::path result(path);
    result.replace_extension(extension);
    return result.string();
}

std::string normalizePath(const std::string& path) {
    if (path.empty()) return "";

    std::error_code ec;
    fs::path normalized = fs::path(path);
    fs::path absolute = fs::absolute(normalized, ec);
    if (!ec) normalized = absolute;
    normalized = normalized.lexically_normal();
    return normalized.string();
}

bool pathsEqual(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return a.empty() && b.empty();

    std::string left = normalizePath(a);
    std::string right = normalizePath(b);
#ifdef _WIN32
    left = toLowerAscii(left);
    right = toLowerAscii(right);
#endif
    return left == right;
}

std::string studypadDirectory(VerdadApp* app) {
    if (!app) return "";
    return normalizePath((fs::path(app->getConfigDir()) / "studypad").string());
}

bool ensureDirectoryExists(const std::string& path) {
    if (path.empty()) return false;

    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) return false;
    return fs::is_directory(fs::path(path), ec) && !ec;
}

bool isStudypadFilename(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) return false;
    return endsWithIgnoreCase(path.filename().string(), ".studypad");
}

std::string studypadDisplayName(const std::string& path) {
    if (path.empty()) return "";
    fs::path filePath(path);
    if (endsWithIgnoreCase(filePath.filename().string(), ".studypad")) {
        return filePath.stem().string();
    }
    std::string stem = filePath.stem().string();
    return stem.empty() ? filePath.filename().string() : stem;
}

std::string sanitizeStudypadName(const std::string& rawName) {
    std::string name = trimCopy(rawName);
    if (endsWithIgnoreCase(name, ".studypad")) {
        name.resize(name.size() - std::string(".studypad").size());
        name = trimCopy(name);
    }

    if (name.empty() || name == "." || name == "..") return "";
    if (name.find_first_of("/\\") != std::string::npos) return "";
    if (name.find_first_of("<>:\"|?*") != std::string::npos) return "";
    return name;
}

std::string shellQuote(const std::string& text) {
#ifdef _WIN32
    std::string escaped = "\"";
    for (char c : text) {
        if (c == '"') escaped += "\\\"";
        else escaped += c;
    }
    escaped += '"';
    return escaped;
#else
    std::string escaped = "'";
    for (char c : text) {
        if (c == '\'') escaped += "'\\''";
        else escaped += c;
    }
    escaped += "'";
    return escaped;
#endif
}

std::string fileUriForPath(const std::string& path) {
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
#ifdef _WIN32
    if (!normalized.empty() && normalized[0] != '/') normalized.insert(normalized.begin(), '/');
#endif
    return "file://" + normalized;
}

std::string makeUniqueTempDir(const std::string& prefix) {
    fs::path baseDir;
    try {
        baseDir = fs::temp_directory_path();
    } catch (...) {
#ifdef _WIN32
        baseDir = ".";
#else
        baseDir = "/tmp";
#endif
    }

    auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 128; ++attempt) {
        fs::path candidate = baseDir / (prefix + std::to_string(seed + attempt));
        std::error_code ec;
        if (fs::create_directory(candidate, ec)) {
            return candidate.string();
        }
    }

    return "";
}

bool writeTextFile(const std::string& path, const std::string& text) {
    std::ofstream out(path);
    if (!out.is_open()) return false;
    out << text;
    return static_cast<bool>(out);
}

bool copyBinaryFile(const std::string& fromPath, const std::string& toPath) {
    std::ifstream in(fromPath, std::ios::binary);
    if (!in.is_open()) return false;

    std::ofstream out(toPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    out << in.rdbuf();
    return static_cast<bool>(in) && static_cast<bool>(out);
}

bool runLibreOfficeOdtExport(const std::string& inputHtmlPath,
                             const std::string& outputDir,
                             const std::string& profileDir) {
    std::string profileUri = fileUriForPath(profileDir);
    for (const char* command : {"libreoffice", "soffice"}) {
        std::string cmd = std::string(command) +
                          " -env:UserInstallation=" + shellQuote(profileUri) +
                          " --headless --convert-to odt --outdir " + shellQuote(outputDir) +
                          " " + shellQuote(inputHtmlPath);
#ifdef _WIN32
        cmd += " >NUL 2>NUL";
#else
        cmd += " >/dev/null 2>&1";
#endif
        if (std::system(cmd.c_str()) == 0) return true;
    }
    return false;
}

int findGeneralBookTocIndex(
    const std::vector<GeneralBookTocEntry>& toc,
    const std::string& key) {
    std::string wanted = module_choice::trimCopy(key);
    if (wanted.empty()) return -1;
    for (size_t i = 0; i < toc.size(); ++i) {
        if (toc[i].key == wanted) return static_cast<int>(i);
    }
    for (size_t i = 0; i < toc.size(); ++i) {
        if (module_choice::equalsIgnoreCaseAscii(toc[i].label, wanted)) {
            return static_cast<int>(i);
        }
    }
    const std::string wantedPrefix = wanted + "/";
    for (size_t i = 0; i < toc.size(); ++i) {
        if (toc[i].key.rfind(wantedPrefix, 0) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string generalBookSectionAnchorId(int tocIndex) {
    return "gb-section-" + std::to_string(tocIndex);
}

void noteGeneralBookSectionCacheUse(std::deque<std::string>& order,
                                    const std::string& cacheKey) {
    auto it = std::find(order.begin(), order.end(), cacheKey);
    if (it != order.end()) {
        order.erase(it);
    }
    order.push_back(cacheKey);
}

void evictGeneralBookSectionCache(
        std::unordered_map<std::string, std::string>& cache,
        std::deque<std::string>& order,
        size_t limit) {
    while (order.size() > limit) {
        cache.erase(order.front());
        order.pop_front();
    }
}

int parseGeneralBookLoadedEdge(const std::string& html, bool wantLast) {
    constexpr const char* marker = "id=\"gb-section-";
    size_t pos = wantLast ? html.rfind(marker) : html.find(marker);
    if (pos == std::string::npos) return -1;
    pos += std::strlen(marker);

    int value = 0;
    bool sawDigit = false;
    while (pos < html.size() &&
           std::isdigit(static_cast<unsigned char>(html[pos]))) {
        sawDigit = true;
        value = (value * 10) + (html[pos] - '0');
        ++pos;
    }
    return sawDigit ? value : -1;
}

} // namespace

RightPane::RightPane(VerdadApp* app, int X, int Y, int W, int H)
    : Fl_Group(X, Y, W, H)
    , app_(app)
    , contentTile_(nullptr)
    , contentResizeBox_(nullptr)
    , tabs_(nullptr)
    , commentaryGroup_(nullptr)
    , commentaryChoice_(nullptr)
    , commentaryEditButton_(nullptr)
    , commentarySaveButton_(nullptr)
    , commentaryCancelButton_(nullptr)
    , commentaryHtml_(nullptr)
    , commentaryEditor_(nullptr)
    , currentCommentary_()
    , currentCommentaryRef_()
    , dictionaryPaneGroup_(nullptr)
    , dictionaryBackButton_(nullptr)
    , dictionaryKeyInput_(nullptr)
    , dictionaryForwardButton_(nullptr)
    , dictionaryChoice_(nullptr)
    , dictionaryHtml_(nullptr)
    , dictionaryKeysModule_()
    , currentDictionary_()
    , currentDictKey_()
    , generalBooksGroup_(nullptr)
    , generalBookChoice_(nullptr)
    , generalBookBackButton_(nullptr)
    , generalBookForwardButton_(nullptr)
    , generalBookContentsButton_(nullptr)
    , generalBookHtml_(nullptr)
    , generalBookTocPanel_(nullptr)
    , generalBookTocPanelHeader_(nullptr)
    , generalBookTocTree_(nullptr)
    , currentGeneralBook_()
    , currentGeneralBookKey_()
    , devotionsPlansGroup_(nullptr)
    , dailyModeButton_(nullptr)
    , dailyDevotionalChoice_(nullptr)
    , dailyReadingPlanChoice_(nullptr)
    , dailyPrevDayButton_(nullptr)
    , dailyDateButton_(nullptr)
    , dailyTodayButton_(nullptr)
    , dailyNextDayButton_(nullptr)
    , dailyNewPlanButton_(nullptr)
    , dailyEditPlanButton_(nullptr)
    , dailyDeletePlanButton_(nullptr)
    , dailyCompleteButton_(nullptr)
    , dailyCompleteThroughButton_(nullptr)
    , dailyRescheduleButton_(nullptr)
    , dailyCalendarGroup_(nullptr)
    , dailyPrevMonthButton_(nullptr)
    , dailyNextMonthButton_(nullptr)
    , dailyMonthLabel_(nullptr)
    , dailyCalendarWidget_(nullptr)
    , dailyHtml_(nullptr)
    , dailyPlanEditorGroup_(nullptr)
    , dailyPlanNameInput_(nullptr)
    , dailyPlanStartDateInput_(nullptr)
    , dailyPlanDescriptionInput_(nullptr)
    , dailyPlanDayBrowser_(nullptr)
    , dailyPlanDayDateInput_(nullptr)
    , dailyPlanDayTitleInput_(nullptr)
    , dailyPlanDayPassagesInput_(nullptr)
    , dailyPlanAddDayButton_(nullptr)
    , dailyPlanDuplicateDayButton_(nullptr)
    , dailyPlanRemoveDayButton_(nullptr)
    , dailyPlanSaveButton_(nullptr)
    , dailyPlanCancelButton_(nullptr)
    , dailyWorkspaceState_()
    , dailyPlanEditorWorkingPlan_()
    , dailyPlanEditorDirty_(false)
    , documentsGroup_(nullptr)
    , documentChoice_(nullptr)
    , documentNewButton_(nullptr)
    , documentSaveButton_(nullptr)
    , documentExportButton_(nullptr)
    , documentDeleteButton_(nullptr)
    , documentsEditor_(nullptr)
    , currentDocumentPath_() {
    box(FL_FLAT_BOX);

    begin();

    const int padding = 2;
    const int minTopH = 100;
    const int minBottomH = 90;
    const int tabsHeaderH = 25;
    const int choiceH = 25;
    int tileX = X + padding;
    int tileY = Y + padding;
    int tileW = std::max(20, W - 2 * padding);
    int tileH = std::max(minTopH + minBottomH, H - 2 * padding);

    contentTile_ = new Fl_Tile(tileX, tileY, tileW, tileH);
    contentTile_->begin();

    contentResizeBox_ = new Fl_Box(tileX, tileY, tileW, tileH);
    contentResizeBox_->box(FL_NO_BOX);

    int dictInitH = std::clamp(170, minBottomH,
                               std::max(minBottomH, tileH - minTopH));
    int tabsInitH = std::max(minTopH, tileH - dictInitH);

    auto* resizeTabs = new ResizeAwareTabs(tileX, tileY, tileW, tabsInitH);
    tabs_ = resizeTabs;
    tabs_->begin();

    int panelY = tileY + tabsHeaderH;
    int panelH = std::max(20, tabsInitH - tabsHeaderH);

    commentaryGroup_ = new Fl_Group(tileX, panelY, tileW, panelH, "Commentary");
    commentaryGroup_->begin();
    commentaryChoice_ = new WrappingChoice(tileX + 2, panelY + 2, tileW - 4, choiceH);
    commentaryChoice_->callback(onCommentaryModuleChange, this);
    commentaryEditButton_ = new Fl_Button(tileX + tileW - 154, panelY + 2, 48, choiceH, "Edit");
    commentaryEditButton_->callback(onCommentaryEdit, this);
    commentarySaveButton_ = new Fl_Button(tileX + tileW - 104, panelY + 2, 48, choiceH, "Save");
    commentarySaveButton_->callback(onCommentarySave, this);
    commentaryCancelButton_ = new Fl_Button(tileX + tileW - 54, panelY + 2, 52, choiceH, "Cancel");
    commentaryCancelButton_->callback(onCommentaryCancel, this);
    commentaryHtml_ = new HtmlWidget(tileX + 2,
                                     panelY + choiceH + 4,
                                     tileW - 4,
                                     panelH - choiceH - 6);
    commentaryHtml_->setLinkCallback(
        [this](const std::string& url) { onHtmlLink(url, true); });
    commentaryEditor_ = new HtmlEditorWidget(tileX + 2,
                                             panelY + choiceH + 4,
                                             tileW - 4,
                                             panelH - choiceH - 6);
    commentaryEditor_->setMode(HtmlEditorWidget::Mode::Commentary);
    commentaryEditor_->setIndentWidth(app_ ? app_->appearanceSettings().editorIndentWidth : 4);
    if (app_) {
        commentaryEditor_->setLineHeight(app_->appearanceSettings().editorLineHeight);
        commentaryEditor_->setTextFont(
            app_->textEditorFont(),
            app_->boldTextEditorFont(),
            app_->appearanceSettings().textFontSize);
    }
    commentaryEditor_->setChangeCallback([this]() { updateCommentaryEditorChrome(); });
    commentaryEditor_->hide();
    commentaryGroup_->end();
    commentaryGroup_->resizable(commentaryHtml_);

    generalBooksGroup_ = new Fl_Group(tileX, panelY, tileW, panelH, "General Books");
    generalBooksGroup_->begin();
    generalBookChoice_ = new WrappingChoice(tileX + 2, panelY + 2, tileW - 4, choiceH);
    generalBookChoice_->callback(onGeneralBookModuleChange, this);
    generalBookBackButton_ = new Fl_Button(tileX + 2, panelY + choiceH + 4,
                                           kGeneralBookNavButtonW, choiceH, "@<-");
    generalBookBackButton_->callback(onGeneralBookBack, this);
    generalBookBackButton_->tooltip("Show the previous general-book page");
    generalBookForwardButton_ = new Fl_Button(tileX + 2 + kGeneralBookNavButtonW,
                                              panelY + choiceH + 4,
                                              kGeneralBookNavButtonW, choiceH, "@->");
    generalBookForwardButton_->callback(onGeneralBookForward, this);
    generalBookForwardButton_->tooltip("Show the next general-book page");
    generalBookContentsButton_ = new Fl_Button(tileX + 2 + (kGeneralBookNavButtonW * 2) + 2,
                                               panelY + choiceH + 4,
                                               kGeneralBookContentsButtonW,
                                               choiceH,
                                               "Contents");
    generalBookContentsButton_->callback(onGeneralBookContents, this);
    generalBookContentsButton_->tooltip("Open the general-book contents tree");

    generalBookHtml_ = new HtmlWidget(tileX + 2,
                                      panelY + (choiceH * 2) + 6,
                                      tileW - 4,
                                      panelH - (choiceH * 2) - 8);
    generalBookHtml_->setLinkCallback(
        [this](const std::string& url) { onHtmlLink(url, false); });
    generalBookTocPanel_ = new Fl_Group(tileX + 10,
                                        panelY + (choiceH * 2) + 14,
                                        std::min(kGeneralBookOverlayMaxW,
                                                 std::max(kGeneralBookOverlayMinW,
                                                          (tileW * 2) / 5)),
                                        std::max(120, panelH - (choiceH * 2) - 28));
    generalBookTocPanel_->box(FL_THIN_UP_BOX);
    generalBookTocPanel_->color(FL_WHITE);
    generalBookTocPanel_->begin();
    generalBookTocPanelHeader_ = new Fl_Box(generalBookTocPanel_->x() + 8,
                                            generalBookTocPanel_->y() + 2,
                                            std::max(20, generalBookTocPanel_->w() - 16),
                                            kGeneralBookOverlayHeaderH,
                                            "Contents");
    generalBookTocPanelHeader_->labelfont(FL_HELVETICA_BOLD);
    generalBookTocPanelHeader_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    generalBookTocTree_ = new Fl_Tree(generalBookTocPanel_->x() + 4,
                                      generalBookTocPanel_->y() + kGeneralBookOverlayHeaderH + 2,
                                      std::max(20, generalBookTocPanel_->w() - 8),
                                      std::max(40, generalBookTocPanel_->h() - kGeneralBookOverlayHeaderH - 6));
    generalBookTocTree_->showroot(0);
    generalBookTocTree_->selectmode(FL_TREE_SELECT_SINGLE);
    generalBookTocTree_->item_reselect_mode(FL_TREE_SELECTABLE_ALWAYS);
    generalBookTocTree_->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);
    generalBookTocTree_->callback(onGeneralBookTreeSelect, this);
    generalBookTocPanel_->end();
    generalBookTocPanel_->hide();
    generalBooksGroup_->end();
    generalBooksGroup_->resizable(generalBookHtml_);

    devotionsPlansGroup_ = new Fl_Group(tileX, panelY, tileW, panelH, "Daily");
    devotionsPlansGroup_->begin();
    dailyModeButton_ = new Fl_Button(tileX + 2, panelY + 2, 128, choiceH, "Reading Plans...");
    dailyModeButton_->callback(onDailyModeChange, this);

    dailyDevotionalChoice_ = new WrappingChoice(tileX + 134, panelY + 2, 240, choiceH);
    dailyDevotionalChoice_->callback(onDailyDevotionalModuleChange, this);
    dailyReadingPlanChoice_ = new WrappingChoice(tileX + 134, panelY + 2, 240, choiceH);
    dailyReadingPlanChoice_->callback(onDailyReadingPlanChange, this);
    dailyReadingPlanChoice_->hide();

    dailyPrevDayButton_ = new Fl_Button(tileX + 378, panelY + 2, kDailyNavButtonW, choiceH, "@<-");
    dailyPrevDayButton_->callback(onDailyPrevDay, this);
    dailyDateButton_ = new Fl_Button(tileX + 408, panelY + 2, 136, choiceH, "");
    dailyDateButton_->callback(onDailyDateButton, this);
    dailyTodayButton_ = new Fl_Button(tileX + 546, panelY + 2, 56, choiceH, "Today");
    dailyTodayButton_->callback(onDailyToday, this);
    dailyNextDayButton_ = new Fl_Button(tileX + 604, panelY + 2, kDailyNavButtonW, choiceH, "@->");
    dailyNextDayButton_->callback(onDailyNextDay, this);
    dailyNewPlanButton_ = new Fl_Button(tileX + 636, panelY + 2, 52, choiceH, "New");
    dailyNewPlanButton_->callback(onDailyNewPlan, this);
    dailyEditPlanButton_ = new Fl_Button(tileX + 690, panelY + 2, 48, choiceH, "Edit");
    dailyEditPlanButton_->callback(onDailyEditPlan, this);
    dailyDeletePlanButton_ = new Fl_Button(tileX + 740, panelY + 2, 58, choiceH, "Delete");
    dailyDeletePlanButton_->callback(onDailyDeletePlan, this);

    dailyCompleteButton_ = new Fl_Button(tileX + 2,
                                         panelY + choiceH + 6,
                                         118,
                                         choiceH,
                                         "Mark Complete");
    dailyCompleteButton_->callback(onDailyToggleComplete, this);
    dailyCompleteThroughButton_ = new Fl_Button(tileX + 124,
                                                panelY + choiceH + 6,
                                                118,
                                                choiceH,
                                                "Mark Previous");
    dailyCompleteThroughButton_->callback(onDailyToggleCompleteThrough, this);
    dailyRescheduleButton_ = new Fl_Button(tileX + 246,
                                           panelY + choiceH + 6,
                                           96,
                                           choiceH,
                                           "Reschedule");
    dailyRescheduleButton_->callback(onDailyReschedule, this);

    dailyCalendarGroup_ = new Fl_Group(tileX + 2,
                                       panelY + (choiceH * 2) + 10,
                                       tileW - 4,
                                       kDailyCalendarDrawerH);
    dailyCalendarGroup_->box(FL_THIN_UP_BOX);
    dailyCalendarGroup_->color(FL_WHITE);
    dailyCalendarGroup_->begin();
    dailyPrevMonthButton_ = new Fl_Button(dailyCalendarGroup_->x() + 6,
                                          dailyCalendarGroup_->y() + 6,
                                          30,
                                          24,
                                          "@<-");
    dailyPrevMonthButton_->callback(onDailyPrevMonth, this);
    dailyNextMonthButton_ = new Fl_Button(dailyCalendarGroup_->x() + dailyCalendarGroup_->w() - 36,
                                          dailyCalendarGroup_->y() + 6,
                                          30,
                                          24,
                                          "@->");
    dailyNextMonthButton_->callback(onDailyNextMonth, this);
    dailyMonthLabel_ = new Fl_Box(dailyCalendarGroup_->x() + 42,
                                  dailyCalendarGroup_->y() + 6,
                                  std::max(20, dailyCalendarGroup_->w() - 84),
                                  24,
                                  "");
    dailyMonthLabel_->labelfont(FL_HELVETICA_BOLD);
    dailyMonthLabel_->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
    dailyCalendarWidget_ = new MonthCalendarWidget(
        dailyCalendarGroup_->x() + 6,
        dailyCalendarGroup_->y() + 34,
        std::max(20, dailyCalendarGroup_->w() - 12),
        std::max(80, dailyCalendarGroup_->h() - 40));
    dailyCalendarWidget_->setDateSelectCallback(
        [this](const reading::Date& date) {
            RightPane::onDailyCalendarDateSelected(date, this);
        });
    dailyCalendarGroup_->end();
    dailyCalendarGroup_->hide();

    dailyHtml_ = new HtmlWidget(tileX + 2,
                                panelY + (choiceH * 2) + 12,
                                tileW - 4,
                                panelH - (choiceH * 2) - 14);
    dailyHtml_->setLinkCallback(
        [this](const std::string& url) { onDailyContentLink(url); });
    dailyPlanEditorGroup_ = new Fl_Group(tileX + 2,
                                         panelY + (choiceH * 2) + 12,
                                         tileW - 4,
                                         panelH - (choiceH * 2) - 14);
    dailyPlanEditorGroup_->box(FL_NO_BOX);
    dailyPlanEditorGroup_->begin();
    auto* dailyPlanNameLabel = new Fl_Box(dailyPlanEditorGroup_->x(),
                                          dailyPlanEditorGroup_->y(),
                                          46,
                                          24,
                                          "Name");
    dailyPlanNameLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    dailyPlanNameInput_ = new Fl_Input(dailyPlanEditorGroup_->x() + 50,
                                       dailyPlanEditorGroup_->y(),
                                       238,
                                       24);
    dailyPlanNameInput_->when(FL_WHEN_CHANGED);
    dailyPlanNameInput_->callback(onDailyPlanEditorFieldChanged, this);

    auto* dailyPlanStartLabel = new Fl_Box(dailyPlanEditorGroup_->x() + 296,
                                           dailyPlanEditorGroup_->y(),
                                           40,
                                           24,
                                           "Start");
    dailyPlanStartLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    dailyPlanStartDateInput_ = new Fl_Input(dailyPlanEditorGroup_->x() + 340,
                                            dailyPlanEditorGroup_->y(),
                                            110,
                                            24);
    dailyPlanStartDateInput_->tooltip("YYYY-MM-DD");
    dailyPlanStartDateInput_->when(FL_WHEN_CHANGED);
    dailyPlanStartDateInput_->callback(onDailyPlanEditorFieldChanged, this);

    dailyPlanAddDayButton_ = new Fl_Button(dailyPlanEditorGroup_->x() + 458,
                                           dailyPlanEditorGroup_->y(),
                                           48,
                                           24,
                                           "Add");
    dailyPlanAddDayButton_->callback(onDailyPlanAddDay, this);
    dailyPlanDuplicateDayButton_ = new Fl_Button(dailyPlanEditorGroup_->x() + 510,
                                                 dailyPlanEditorGroup_->y(),
                                                 72,
                                                 24,
                                                 "Duplicate");
    dailyPlanDuplicateDayButton_->callback(onDailyPlanDuplicateDay, this);
    dailyPlanRemoveDayButton_ = new Fl_Button(dailyPlanEditorGroup_->x() + 586,
                                              dailyPlanEditorGroup_->y(),
                                              64,
                                              24,
                                              "Remove");
    dailyPlanRemoveDayButton_->callback(onDailyPlanRemoveDay, this);
    dailyPlanSaveButton_ = new Fl_Button(dailyPlanEditorGroup_->x() + 654,
                                         dailyPlanEditorGroup_->y(),
                                         42,
                                         24,
                                         "Save");
    dailyPlanSaveButton_->callback(onDailyPlanSave, this);
    dailyPlanCancelButton_ = new Fl_Button(dailyPlanEditorGroup_->x() + 700,
                                           dailyPlanEditorGroup_->y(),
                                           58,
                                           24,
                                           "Cancel");
    dailyPlanCancelButton_->callback(onDailyPlanCancel, this);

    auto* dailyPlanNotesLabel = new Fl_Box(dailyPlanEditorGroup_->x(),
                                           dailyPlanEditorGroup_->y() + 30,
                                           46,
                                           24,
                                           "Notes");
    dailyPlanNotesLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    dailyPlanDescriptionInput_ = new Fl_Multiline_Input(dailyPlanEditorGroup_->x() + 50,
                                                        dailyPlanEditorGroup_->y() + 30,
                                                        dailyPlanEditorGroup_->w() - 50,
                                                        48);
    dailyPlanDescriptionInput_->when(FL_WHEN_CHANGED);
    dailyPlanDescriptionInput_->callback(onDailyPlanEditorFieldChanged, this);

    dailyPlanDayBrowser_ = new Fl_Hold_Browser(dailyPlanEditorGroup_->x(),
                                               dailyPlanEditorGroup_->y() + 88,
                                               352,
                                               std::max(80, dailyPlanEditorGroup_->h() - 92));
    dailyPlanDayBrowser_->callback(onDailyPlanEditorSelection, this);

    auto* dailyPlanDayDateLabel = new Fl_Box(dailyPlanEditorGroup_->x() + 364,
                                             dailyPlanEditorGroup_->y() + 88,
                                             42,
                                             24,
                                             "Date");
    dailyPlanDayDateLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    dailyPlanDayDateInput_ = new Fl_Input(dailyPlanEditorGroup_->x() + 410,
                                          dailyPlanEditorGroup_->y() + 88,
                                          110,
                                          24);
    dailyPlanDayDateInput_->tooltip("YYYY-MM-DD");
    dailyPlanDayDateInput_->when(FL_WHEN_CHANGED);
    dailyPlanDayDateInput_->callback(onDailyPlanEditorFieldChanged, this);

    auto* dailyPlanDayTitleLabel = new Fl_Box(dailyPlanEditorGroup_->x() + 530,
                                              dailyPlanEditorGroup_->y() + 88,
                                              34,
                                              24,
                                              "Title");
    dailyPlanDayTitleLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    dailyPlanDayTitleInput_ = new Fl_Input(dailyPlanEditorGroup_->x() + 568,
                                           dailyPlanEditorGroup_->y() + 88,
                                           std::max(120, dailyPlanEditorGroup_->w() - 568),
                                           24);
    dailyPlanDayTitleInput_->when(FL_WHEN_CHANGED);
    dailyPlanDayTitleInput_->callback(onDailyPlanEditorFieldChanged, this);

    auto* dailyPlanPassagesLabel = new Fl_Box(dailyPlanEditorGroup_->x() + 364,
                                              dailyPlanEditorGroup_->y() + 120,
                                              320,
                                              24,
                                              "Passages (one reference per line)");
    dailyPlanPassagesLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    dailyPlanDayPassagesInput_ = new Fl_Multiline_Input(dailyPlanEditorGroup_->x() + 364,
                                                        dailyPlanEditorGroup_->y() + 144,
                                                        std::max(120, dailyPlanEditorGroup_->w() - 364),
                                                        std::max(80, dailyPlanEditorGroup_->h() - 148));
    dailyPlanDayPassagesInput_->when(FL_WHEN_CHANGED);
    dailyPlanDayPassagesInput_->callback(onDailyPlanEditorFieldChanged, this);
    dailyPlanEditorGroup_->end();
    dailyPlanEditorGroup_->hide();
    devotionsPlansGroup_->end();
    devotionsPlansGroup_->resizable(dailyHtml_);

    documentsGroup_ = new Fl_Group(tileX, panelY, tileW, panelH, "Studypad");
    documentsGroup_->begin();
    documentChoice_ = new WrappingChoice(tileX + 2,
                                         panelY + 2,
                                         std::max(20, (tileW - 4) - studypadToolbarButtonsWidth() - kStudypadToolbarButtonGap),
                                         choiceH);
    documentChoice_->callback(onDocumentChoiceChange, this);
    documentChoice_->tooltip("Select a studypad");
    auto* documentNewButton = new Fl_Button(tileX + tileW - studypadToolbarButtonsWidth() - 2,
                                            panelY + 4,
                                            kStudypadToolbarButtonW,
                                            kStudypadToolbarButtonH);
    configureStudypadToolbarButton(documentNewButton, StudypadToolbarIcon::New);
    documentNewButton_ = documentNewButton;
    documentNewButton_->callback(onDocumentNew, this);
    documentNewButton_->tooltip("Create a new studypad");
    auto* documentSaveButton = new Fl_Button(documentNewButton->x() + kStudypadToolbarButtonW + kStudypadToolbarButtonGap,
                                             panelY + 4,
                                             kStudypadToolbarButtonW,
                                             kStudypadToolbarButtonH);
    configureStudypadToolbarButton(documentSaveButton, StudypadToolbarIcon::Save);
    documentSaveButton_ = documentSaveButton;
    documentSaveButton_->callback(onDocumentSave, this);
    documentSaveButton_->tooltip("Save the current studypad");
    auto* documentExportButton = new Fl_Button(documentSaveButton->x() + kStudypadToolbarButtonW + kStudypadToolbarButtonGap,
                                               panelY + 4,
                                               kStudypadToolbarButtonW,
                                               kStudypadToolbarButtonH);
    configureStudypadToolbarButton(documentExportButton, StudypadToolbarIcon::Export);
    documentExportButton_ = documentExportButton;
    documentExportButton_->callback(onDocumentExportOdt, this);
    documentExportButton_->tooltip("Export the current studypad to ODT");
    auto* documentDeleteButton = new Fl_Button(documentExportButton->x() + kStudypadToolbarButtonW + kStudypadToolbarButtonGap,
                                               panelY + 4,
                                               kStudypadToolbarButtonW,
                                               kStudypadToolbarButtonH);
    configureStudypadToolbarButton(documentDeleteButton, StudypadToolbarIcon::Delete);
    documentDeleteButton_ = documentDeleteButton;
    documentDeleteButton_->callback(onDocumentDelete, this);
    documentDeleteButton_->tooltip("Delete the current studypad");
    documentsEditor_ = new HtmlEditorWidget(tileX + 2,
                                            panelY + choiceH + 4,
                                            tileW - 4,
                                            panelH - choiceH - 6);
    documentsEditor_->setMode(HtmlEditorWidget::Mode::Document);
    documentsEditor_->setIndentWidth(app_ ? app_->appearanceSettings().editorIndentWidth : 4);
    if (app_) {
        documentsEditor_->setLineHeight(app_->appearanceSettings().editorLineHeight);
        documentsEditor_->setTextFont(
            app_->textEditorFont(),
            app_->boldTextEditorFont(),
            app_->appearanceSettings().textFontSize);
    }
    documentsEditor_->setChangeCallback([this]() { updateDocumentChrome(); });
    documentsEditor_->setVerseTextProvider(
        [this](const std::string& verseReference) {
            return buildStudypadVerseInsertHtml(app_, verseReference);
        });
    documentsGroup_->end();
    documentsGroup_->resizable(documentsEditor_);

    tabs_->end();
    tabs_->value(commentaryGroup_);
    tabs_->callback(onTopTabChange, this);

    auto* resizeDictionaryPane = new ResizeAwareGroup(tileX,
                                                      tileY + tabsInitH,
                                                      tileW,
                                                      tileH - tabsInitH);
    dictionaryPaneGroup_ = resizeDictionaryPane;
    dictionaryPaneGroup_->begin();
    int dictRowW = std::max(20, tileW - 4);
    int dictKeyAreaW = std::clamp((dictRowW * 7) / 20,
                                  150,
                                  std::max(150, dictRowW - 140));
    int dictChoiceW = std::max(20, dictRowW - dictKeyAreaW - 2);
    dictionaryBackButton_ = new Fl_Button(tileX + 2,
                                          tileY + tabsInitH + 2,
                                          kDictionaryNavButtonW,
                                          choiceH,
                                          "@<-");
    dictionaryBackButton_->callback(onDictionaryBack, this);
    dictionaryBackButton_->tooltip("Show previous dictionary entry");
    dictionaryKeyInput_ = new FilterableChoiceWidget(tileX + 2 + kDictionaryNavButtonW,
                                                     tileY + tabsInitH + 2,
                                                     std::max(40, dictKeyAreaW - (kDictionaryNavButtonW * 2)),
                                                     choiceH);
    dictionaryKeyInput_->setNoMatchesLabel("No matching keys");
    dictionaryKeyInput_->setShowAllWhenFilterEmpty(false);
    dictionaryKeyInput_->setMaxVisibleItems(200);
    dictionaryKeyInput_->setEnsureItemsCallback([this]() {
        ensureDictionaryKeysLoaded();
    });
    dictionaryKeyInput_->callback(onDictionaryKeyInput, this);
    dictionaryKeyInput_->tooltip("Type a dictionary key or choose one from the list");
    dictionaryForwardButton_ = new Fl_Button(tileX + 2 + dictKeyAreaW - kDictionaryNavButtonW,
                                             tileY + tabsInitH + 2,
                                             kDictionaryNavButtonW,
                                             choiceH,
                                             "@->");
    dictionaryForwardButton_->callback(onDictionaryForward, this);
    dictionaryForwardButton_->tooltip("Show next dictionary entry");
    dictionaryChoice_ = new WrappingChoice(tileX + 2 + dictKeyAreaW + 2,
                                           tileY + tabsInitH + 2,
                                           dictChoiceW,
                                           choiceH);
    dictionaryChoice_->callback(onDictionaryModuleChange, this);
    dictionaryHtml_ = new HtmlWidget(tileX + 2,
                                     tileY + tabsInitH + choiceH + 4,
                                     dictRowW,
                                     (tileH - tabsInitH) - choiceH - 6);
    dictionaryHtml_->setLinkCallback(
        [this](const std::string& url) { onHtmlLink(url, false); });
    dictionaryPaneGroup_->end();
    dictionaryPaneGroup_->resizable(dictionaryHtml_);

    resizeTabs->setResizeCallback([this](int tabsX, int tabsY, int tabsW, int tabsH) {
        layoutTopTabContents(tabsX, tabsY, tabsW, tabsH);
    });

    resizeDictionaryPane->setResizeCallback(
        [this](int paneX, int paneY, int paneW, int paneH) {
            layoutDictionaryPaneContents(paneX,
                                         paneY,
                                         paneW,
                                         paneH,
                                         dictionaryBackButton_,
                                         dictionaryKeyInput_,
                                         dictionaryForwardButton_,
                                         dictionaryChoice_,
                                         dictionaryHtml_);
        });

    contentTile_->end();
    contentTile_->resizable(contentResizeBox_);
    contentTile_->init_sizes();

    end();
    resizable(contentTile_);

    // Load CSS once and apply to all HTML widgets.
    std::string cssFile = app_->getDataDir() + "/master.css";
    std::ifstream cssStream(cssFile);
    if (cssStream.is_open()) {
        std::string css((std::istreambuf_iterator<char>(cssStream)),
                        std::istreambuf_iterator<char>());
        if (!css.empty()) {
            if (commentaryHtml_) commentaryHtml_->setMasterCSS(css);
            if (dictionaryHtml_) dictionaryHtml_->setMasterCSS(css);
            if (generalBookHtml_) generalBookHtml_->setMasterCSS(css);
            if (dailyHtml_) dailyHtml_->setMasterCSS(css);
        }
    }

    populateCommentaryModules();
    populateDictionaryModules(false);
    populateGeneralBookModules(false);
    dailyWorkspaceState_.selectedDateIso = reading::formatIsoDate(reading::today());
    populateDailyDevotionModules();
    populateReadingPlanChoices();
    refreshDailyWorkspace(true);
    documentsEditor_->clearDocument();
    documentsEditor_->setModified(false);
    ensureDirectoryExists(studypadDirectory(app_));
    refreshDocumentChoices();
    updateCommentaryEditorChrome();
    updateDocumentChrome();
}

RightPane::~RightPane() = default;

void RightPane::layoutTopTabContents(int tabsX, int tabsY, int tabsW, int tabsH) {
    if (!commentaryGroup_ || !commentaryChoice_ || !commentaryEditButton_ ||
        !commentarySaveButton_ || !commentaryCancelButton_ || !commentaryHtml_ ||
        !commentaryEditor_ || !generalBooksGroup_ || !generalBookChoice_ ||
        !generalBookBackButton_ || !generalBookForwardButton_ ||
        !generalBookContentsButton_ || !generalBookHtml_ ||
        !generalBookTocPanel_ || !generalBookTocPanelHeader_ || !generalBookTocTree_ ||
        !devotionsPlansGroup_ || !dailyModeButton_ ||
        !dailyDevotionalChoice_ ||
        !dailyReadingPlanChoice_ || !dailyPrevDayButton_ || !dailyDateButton_ ||
        !dailyTodayButton_ || !dailyNextDayButton_ ||
        !dailyNewPlanButton_ || !dailyEditPlanButton_ || !dailyDeletePlanButton_ ||
        !dailyCompleteButton_ ||
        !dailyCompleteThroughButton_ ||
        !dailyRescheduleButton_ || !dailyCalendarGroup_ || !dailyPrevMonthButton_ ||
        !dailyNextMonthButton_ || !dailyMonthLabel_ || !dailyCalendarWidget_ ||
        !dailyHtml_ ||
        !documentsGroup_ || !documentChoice_ || !documentNewButton_ ||
        !documentSaveButton_ || !documentExportButton_ || !documentDeleteButton_ ||
        !documentsEditor_) {
        return;
    }

    const int tabsHeaderH = 25;
    const int rowH = 25;
    const int buttonGap = 2;

    int clampedTabsW = std::max(20, tabsW);
    int clampedTabsH = std::max(20, tabsH);
    int panelY = tabsY + tabsHeaderH;
    int panelH = std::max(20, clampedTabsH - tabsHeaderH);
    int contentX = tabsX + 2;
    int contentW = std::max(20, clampedTabsW - 4);

    commentaryGroup_->resize(tabsX, panelY, clampedTabsW, panelH);
    generalBooksGroup_->resize(tabsX, panelY, clampedTabsW, panelH);
    devotionsPlansGroup_->resize(tabsX, panelY, clampedTabsW, panelH);
    documentsGroup_->resize(tabsX, panelY, clampedTabsW, panelH);

    int commentaryButtonsW =
        commentaryEditButton_->w() + commentarySaveButton_->w() +
        commentaryCancelButton_->w() + (buttonGap * 2);
    int commentaryChoiceW = std::max(20, contentW - commentaryButtonsW - buttonGap);
    int commentaryButtonsX = contentX + commentaryChoiceW + buttonGap;
    commentaryChoice_->resize(contentX, panelY + 2, commentaryChoiceW, rowH);
    commentaryEditButton_->resize(commentaryButtonsX,
                                  panelY + 2,
                                  commentaryEditButton_->w(),
                                  rowH);
    commentarySaveButton_->resize(commentaryEditButton_->x() + commentaryEditButton_->w() + buttonGap,
                                  panelY + 2,
                                  commentarySaveButton_->w(),
                                  rowH);
    commentaryCancelButton_->resize(commentarySaveButton_->x() + commentarySaveButton_->w() + buttonGap,
                                    panelY + 2,
                                    commentaryCancelButton_->w(),
                                    rowH);
    int commentaryContentY = panelY + rowH + 4;
    int commentaryContentH = std::max(10, panelH - rowH - 6);
    commentaryHtml_->resize(contentX, commentaryContentY, contentW, commentaryContentH);
    static_cast<Fl_Widget*>(commentaryEditor_)
        ->resize(contentX, commentaryContentY, contentW, commentaryContentH);

    generalBookChoice_->resize(contentX, panelY + 2, contentW, rowH);
    int generalBookNavY = panelY + rowH + 4;
    generalBookBackButton_->resize(contentX,
                                   generalBookNavY,
                                   kGeneralBookNavButtonW,
                                   rowH);
    generalBookForwardButton_->resize(contentX + kGeneralBookNavButtonW,
                                      generalBookNavY,
                                      kGeneralBookNavButtonW,
                                      rowH);
    generalBookContentsButton_->resize(contentX + (kGeneralBookNavButtonW * 2) + buttonGap,
                                       generalBookNavY,
                                       std::min(kGeneralBookContentsButtonW,
                                                std::max(60,
                                                         contentW - (kGeneralBookNavButtonW * 2) - buttonGap)),
                                       rowH);
    int generalBookHtmlY = panelY + (rowH * 2) + 6;
    int generalBookHtmlH = std::max(10, panelH - (rowH * 2) - 8);
    generalBookHtml_->resize(contentX,
                             generalBookHtmlY,
                             contentW,
                             generalBookHtmlH);

    int overlayAvailW = std::max(120, contentW - 16);
    int overlayW = std::min(overlayAvailW,
                            std::clamp((contentW * 2) / 5,
                                       kGeneralBookOverlayMinW,
                                       kGeneralBookOverlayMaxW));
    int overlayH = std::max(120, generalBookHtmlH - 16);
    generalBookTocPanel_->resize(contentX + 8, generalBookHtmlY + 8, overlayW, overlayH);
    generalBookTocPanelHeader_->resize(generalBookTocPanel_->x() + 8,
                                       generalBookTocPanel_->y() + 2,
                                       std::max(20, overlayW - 16),
                                       kGeneralBookOverlayHeaderH);
    generalBookTocTree_->resize(generalBookTocPanel_->x() + 4,
                                generalBookTocPanel_->y() + kGeneralBookOverlayHeaderH + 2,
                                std::max(20, overlayW - 8),
                                std::max(40, overlayH - kGeneralBookOverlayHeaderH - 6));

    const bool readingPlansMode =
        dailyWorkspaceState_.mode == DailyWorkspaceMode::ReadingPlans;
    const bool editingPlan =
        readingPlansMode && dailyWorkspaceState_.readingPlanEditMode;
    const int dailyRow1Y = panelY + 2;
    const int dailyRow2Y = dailyRow1Y + rowH + 4;
    const int dailyModeButtonW = 128;
    const int dailyChoiceX = contentX + dailyModeButtonW + 4;
    const int dailyNavXBase = contentX + std::max(180, (contentW * 9) / 20);
    const int rightButtonsW =
        dailyNewPlanButton_->w() + dailyEditPlanButton_->w() +
        dailyDeletePlanButton_->w() + 8;
    const int dailyChoiceW = std::max(
        140,
        std::min(260,
                 std::max(140, contentW - (dailyNavXBase - contentX) - rightButtonsW - 240)));
    dailyModeButton_->resize(contentX, dailyRow1Y, dailyModeButtonW, rowH);
    dailyDevotionalChoice_->resize(dailyChoiceX, dailyRow1Y, dailyChoiceW, rowH);
    dailyReadingPlanChoice_->resize(dailyChoiceX, dailyRow1Y, dailyChoiceW, rowH);

    int dailyNavX = dailyChoiceX + dailyChoiceW + 6;
    dailyPrevDayButton_->resize(dailyNavX, dailyRow1Y, kDailyNavButtonW, rowH);
    dailyDateButton_->resize(dailyPrevDayButton_->x() + kDailyNavButtonW + 2,
                             dailyRow1Y,
                             136,
                             rowH);
    dailyTodayButton_->resize(dailyDateButton_->x() + dailyDateButton_->w() + 2,
                              dailyRow1Y,
                              56,
                              rowH);
    dailyNextDayButton_->resize(dailyTodayButton_->x() + dailyTodayButton_->w() + 2,
                                dailyRow1Y,
                                kDailyNavButtonW,
                                rowH);

    int planButtonsX = std::max(dailyNextDayButton_->x() + dailyNextDayButton_->w() + 8,
                                contentX + contentW - rightButtonsW);
    dailyNewPlanButton_->resize(planButtonsX, dailyRow1Y, 52, rowH);
    dailyEditPlanButton_->resize(dailyNewPlanButton_->x() + dailyNewPlanButton_->w() + 2,
                                 dailyRow1Y,
                                 48,
                                 rowH);
    dailyDeletePlanButton_->resize(dailyEditPlanButton_->x() + dailyEditPlanButton_->w() + 2,
                                   dailyRow1Y,
                                   58,
                                   rowH);

    dailyCompleteButton_->resize(contentX, dailyRow2Y, 118, rowH);
    dailyCompleteThroughButton_->resize(dailyCompleteButton_->x() + dailyCompleteButton_->w() + 4,
                                        dailyRow2Y,
                                        118,
                                        rowH);
    dailyRescheduleButton_->resize(
                                   dailyCompleteThroughButton_->x() +
                                       dailyCompleteThroughButton_->w() + 4,
                                   dailyRow2Y,
                                   98,
                                   rowH);

    const int dailyCalendarY = dailyRow2Y + rowH + 6;
    int dailyCalendarH = kDailyCalendarDrawerH;
    int dailyHtmlY = dailyRow2Y + rowH + 8;
    int dailyHtmlH = std::max(10, (panelY + panelH) - dailyHtmlY - 2);
    if (editingPlan) {
        dailyCalendarH = 0;
    } else if (readingPlansMode) {
        int requestedBottomH = std::clamp(panelH / 3,
                                          kDailyReadingPlanBottomMinH,
                                          kDailyReadingPlanBottomMaxH);
        int maxBottomH = std::max(10, (panelY + panelH) - (dailyCalendarY + 96) - 2);
        dailyHtmlH = std::min(requestedBottomH, maxBottomH);
        dailyHtmlY = (panelY + panelH) - dailyHtmlH - 2;
        dailyCalendarH = std::max(80, dailyHtmlY - dailyCalendarY - 8);
    } else if (dailyCalendarGroup_->visible()) {
        dailyHtmlY = dailyCalendarY + dailyCalendarH + 8;
        dailyHtmlH = std::max(10, (panelY + panelH) - dailyHtmlY - 2);
    }

    dailyCalendarGroup_->resize(contentX,
                                dailyCalendarY,
                                contentW,
                                dailyCalendarH);
    dailyPrevMonthButton_->resize(dailyCalendarGroup_->x() + 6,
                                  dailyCalendarGroup_->y() + 6,
                                  30,
                                  24);
    dailyNextMonthButton_->resize(dailyCalendarGroup_->x() + dailyCalendarGroup_->w() - 36,
                                  dailyCalendarGroup_->y() + 6,
                                  30,
                                  24);
    dailyMonthLabel_->resize(dailyCalendarGroup_->x() + 42,
                             dailyCalendarGroup_->y() + 6,
                             std::max(20, dailyCalendarGroup_->w() - 84),
                             24);
    dailyCalendarWidget_->resize(dailyCalendarGroup_->x() + 6,
                                 dailyCalendarGroup_->y() + 34,
                                 std::max(20, dailyCalendarGroup_->w() - 12),
                                 std::max(80, dailyCalendarGroup_->h() - 40));
    dailyHtml_->resize(contentX,
                       dailyHtmlY,
                       contentW,
                       dailyHtmlH);
    if (dailyPlanEditorGroup_ && dailyPlanNameInput_ && dailyPlanStartDateInput_ &&
        dailyPlanDescriptionInput_ && dailyPlanDayBrowser_ && dailyPlanDayDateInput_ &&
        dailyPlanDayTitleInput_ && dailyPlanDayPassagesInput_ && dailyPlanAddDayButton_ &&
        dailyPlanDuplicateDayButton_ && dailyPlanRemoveDayButton_ &&
        dailyPlanSaveButton_ && dailyPlanCancelButton_) {
        const int editorY = dailyRow2Y + rowH + 8;
        const int editorH = std::max(120, (panelY + panelH) - editorY - 2);
        dailyPlanEditorGroup_->resize(contentX, editorY, contentW, editorH);

        dailyPlanNameInput_->resize(contentX + 50, editorY, 238, 24);
        dailyPlanStartDateInput_->resize(contentX + 340, editorY, 110, 24);
        int editorButtonsX = contentX + contentW - 288;
        dailyPlanAddDayButton_->resize(editorButtonsX, editorY, 48, 24);
        dailyPlanDuplicateDayButton_->resize(editorButtonsX + 52, editorY, 72, 24);
        dailyPlanRemoveDayButton_->resize(editorButtonsX + 128, editorY, 64, 24);
        dailyPlanSaveButton_->resize(editorButtonsX + 196, editorY, 42, 24);
        dailyPlanCancelButton_->resize(editorButtonsX + 242, editorY, 58, 24);

        dailyPlanDescriptionInput_->resize(contentX + 50, editorY + 30, contentW - 50, 48);

        const int browserY = editorY + 88;
        const int browserW = std::clamp(contentW / 2, 280, 380);
        const int browserH = std::max(80, editorH - 92);
        dailyPlanDayBrowser_->resize(contentX, browserY, browserW, browserH);

        const int detailX = contentX + browserW + 12;
        const int detailW = std::max(120, contentW - browserW - 12);
        dailyPlanDayDateInput_->resize(detailX + 46, browserY, 110, 24);
        dailyPlanDayTitleInput_->resize(detailX + 204, browserY,
                                        std::max(120, detailW - 204),
                                        24);
        dailyPlanDayPassagesInput_->resize(detailX,
                                           browserY + 56,
                                           detailW,
                                           std::max(80, browserH - 56));
    }

    int docsButtonsW = studypadToolbarButtonsWidth();
    int documentChoiceW = std::max(20, contentW - docsButtonsW - kStudypadToolbarButtonGap);
    int docsButtonsX = contentX + documentChoiceW + kStudypadToolbarButtonGap;
    int docsButtonY = panelY + 2 + std::max(0, (rowH - kStudypadToolbarButtonH) / 2);
    documentChoice_->resize(contentX, panelY + 2, documentChoiceW, rowH);
    documentNewButton_->resize(docsButtonsX,
                               docsButtonY,
                               kStudypadToolbarButtonW,
                               kStudypadToolbarButtonH);
    documentSaveButton_->resize(documentNewButton_->x() + kStudypadToolbarButtonW + kStudypadToolbarButtonGap,
                                docsButtonY,
                                kStudypadToolbarButtonW,
                                kStudypadToolbarButtonH);
    documentExportButton_->resize(documentSaveButton_->x() + kStudypadToolbarButtonW + kStudypadToolbarButtonGap,
                                  docsButtonY,
                                  kStudypadToolbarButtonW,
                                  kStudypadToolbarButtonH);
    documentDeleteButton_->resize(documentExportButton_->x() + kStudypadToolbarButtonW + kStudypadToolbarButtonGap,
                                  docsButtonY,
                                  kStudypadToolbarButtonW,
                                  kStudypadToolbarButtonH);
    static_cast<Fl_Widget*>(documentsEditor_)
        ->resize(contentX,
                 panelY + rowH + 4,
                 contentW,
                 std::max(10, panelH - rowH - 6));
}

RightPane::TopTab RightPane::visibleTopTab() const {
    if (tabs_) {
        Fl_Widget* active = tabs_->value();
        if (active == documentsGroup_) return TopTab::Documents;
        if (active == devotionsPlansGroup_) return TopTab::DevotionsPlans;
        if (active == generalBooksGroup_) return TopTab::GeneralBooks;
        if (active == commentaryGroup_) return TopTab::Commentary;
    }
    return activeTopTab_;
}

void RightPane::resize(int X, int Y, int W, int H) {
    Fl_Group::resize(X, Y, W, H);

    if (!contentTile_ || !contentResizeBox_ || !tabs_ || !commentaryGroup_ ||
        !commentaryChoice_ || !commentaryHtml_ || !commentaryEditor_ ||
        !devotionsPlansGroup_ || !dailyModeButton_ ||
        !dailyDevotionalChoice_ ||
        !dailyReadingPlanChoice_ || !dailyPrevDayButton_ || !dailyDateButton_ ||
        !dailyTodayButton_ || !dailyNextDayButton_ ||
        !dailyNewPlanButton_ || !dailyEditPlanButton_ || !dailyDeletePlanButton_ ||
        !dailyCompleteButton_ ||
        !dailyCompleteThroughButton_ ||
        !dailyRescheduleButton_ || !dailyCalendarGroup_ || !dailyPrevMonthButton_ ||
        !dailyNextMonthButton_ || !dailyMonthLabel_ || !dailyCalendarWidget_ ||
        !dailyHtml_ || !dailyPlanEditorGroup_ || !dailyPlanNameInput_ ||
        !dailyPlanStartDateInput_ || !dailyPlanDescriptionInput_ ||
        !dailyPlanDayBrowser_ || !dailyPlanDayDateInput_ ||
        !dailyPlanDayTitleInput_ || !dailyPlanDayPassagesInput_ ||
        !dailyPlanAddDayButton_ || !dailyPlanDuplicateDayButton_ ||
        !dailyPlanRemoveDayButton_ || !dailyPlanSaveButton_ ||
        !dailyPlanCancelButton_ ||
        !documentsGroup_ || !documentChoice_ || !documentsEditor_ || !dictionaryPaneGroup_ ||
        !dictionaryBackButton_ || !dictionaryKeyInput_ || !dictionaryForwardButton_ ||
        !dictionaryChoice_ || !dictionaryHtml_ ||
        !generalBooksGroup_ ||
        !generalBookChoice_ || !generalBookBackButton_ ||
        !generalBookForwardButton_ || !generalBookContentsButton_ ||
        !generalBookTocPanel_ || !generalBookTocPanelHeader_ || !generalBookTocTree_ ||
        !generalBookHtml_) {
        return;
    }

    const int padding = 2;
    const int minTopH = 100;
    const int minBottomH = 90;

    int tileX = X + padding;
    int tileY = Y + padding;
    int tileW = std::max(20, W - 2 * padding);
    int tileH = std::max(minTopH + minBottomH, H - 2 * padding);

    contentTile_->resize(tileX, tileY, tileW, tileH);
    contentResizeBox_->resize(tileX, tileY, tileW, tileH);

    int oldTabsH = tabs_->h();
    if (oldTabsH <= 0) oldTabsH = tileH - minBottomH;
    int tabsH = std::clamp(oldTabsH,
                           minTopH,
                           std::max(minTopH, tileH - minBottomH));

    tabs_->resize(tileX, tileY, tileW, tabsH);
    layoutTopTabContents(tileX, tileY, tileW, tabsH);

    int dictY = tileY + tabsH;
    int dictH = tileH - tabsH;
    layoutDictionaryPane(tileX,
                         dictY,
                         tileW,
                         dictH,
                         dictionaryPaneGroup_,
                         dictionaryBackButton_,
                         dictionaryKeyInput_,
                         dictionaryForwardButton_,
                         dictionaryChoice_,
                         dictionaryHtml_);

    contentTile_->init_sizes();

    if (contentTile_) {
        contentTile_->damage(FL_DAMAGE_ALL);
        contentTile_->redraw();
    }
    if (tabs_) {
        tabs_->damage(FL_DAMAGE_ALL);
        tabs_->redraw();
    }
    damage(FL_DAMAGE_ALL);
    redraw();
}

int RightPane::handle(int event) {
    if (generalBookTocVisible_ && visibleTopTab() == TopTab::GeneralBooks) {
        if (event == FL_PUSH) {
            bool insidePanel = generalBookTocPanel_ && Fl::event_inside(generalBookTocPanel_);
            bool insideButton = generalBookContentsButton_ &&
                                Fl::event_inside(generalBookContentsButton_);
            if (!insidePanel && !insideButton) {
                showGeneralBookTocOverlay(false);
            }
        } else if ((event == FL_KEYDOWN || event == FL_SHORTCUT) &&
                   Fl::event_key() == FL_Escape) {
            showGeneralBookTocOverlay(false);
            return 1;
        }
    }

    if (dailyWorkspaceState_.mode != DailyWorkspaceMode::ReadingPlans &&
        dailyWorkspaceState_.calendarVisible &&
        visibleTopTab() == TopTab::DevotionsPlans &&
        (event == FL_KEYDOWN || event == FL_SHORTCUT) &&
        Fl::event_key() == FL_Escape) {
        dailyWorkspaceState_.calendarVisible = false;
        updateDailyWorkspaceControls();
        return 1;
    }

    return Fl_Group::handle(event);
}

void RightPane::showCommentary(const std::string& reference) {
    if (currentCommentary_.empty()) return;
    showCommentary(currentCommentary_, reference);
}

void RightPane::showCommentary(const std::string& moduleName,
                               const std::string& reference) {
    perf::ScopeTimer timer("RightPane::showCommentary");

    if (commentaryEditing_ &&
        (!commentaryEditModule_.empty() || !commentaryEditReference_.empty()) &&
        (moduleName != commentaryEditModule_ ||
         reference != commentaryEditReference_)) {
        if (!saveCommentaryEdit(false)) {
            return;
        }
    }

    currentCommentary_ = moduleName;
    currentCommentaryRef_ = reference;

    int verse = 0;
    std::string chapterKey = commentaryChapterKeyForReference(reference, &verse);
    bool haveChapterKey = !chapterKey.empty();

    bool needReload = true;
    if (commentaryHtml_ && haveChapterKey) {
        needReload = (loadedCommentaryModule_ != moduleName ||
                      loadedCommentaryChapterKey_ != chapterKey ||
                      commentaryHtml_->currentHtml().empty());
    }

    if (commentaryHtml_ && needReload) {
        perf::StepTimer step;
        std::string html;
        if (haveChapterKey) {
            std::string cacheKey = moduleName + "|" + chapterKey;
            if (!lookupCommentaryCache(cacheKey, html)) {
                html = app_->swordManager().getCommentaryText(moduleName, reference);
                storeCommentaryCache(cacheKey, html);
                perf::logf("RightPane::showCommentary getCommentaryText miss %s %s: %.3f ms",
                           moduleName.c_str(), reference.c_str(), step.elapsedMs());
                step.reset();
            } else {
                perf::logf("RightPane::showCommentary cache hit %s %s: %.3f ms",
                           moduleName.c_str(), reference.c_str(), step.elapsedMs());
                step.reset();
            }
            loadedCommentaryModule_ = moduleName;
            loadedCommentaryChapterKey_ = chapterKey;
        } else {
            html = app_->swordManager().getCommentaryText(moduleName, reference);
            loadedCommentaryModule_.clear();
            loadedCommentaryChapterKey_.clear();
            perf::logf("RightPane::showCommentary getCommentaryText direct %s %s: %.3f ms",
                       moduleName.c_str(), reference.c_str(), step.elapsedMs());
            step.reset();
        }
        commentaryHtml_->setHtml(html);
        highlightedCommentaryVerse_ = 0;
        perf::logf("RightPane::showCommentary commentaryHtml_->setHtml: %.3f ms",
                   step.elapsedMs());
    }

    updateCommentarySelection(verse);

    if (commentaryHtml_) {
        if (verse > 0) {
            commentaryHtml_->scrollToAnchor("vpos" + std::to_string(verse));
        } else {
            commentaryHtml_->scrollToTop();
        }
    }

    if (commentaryEditing_) {
        loadCommentaryEditorForCurrentEntry();
    }

    updateCommentaryEditorChrome();

    if (tabs_ &&
        visibleTopTab() != TopTab::Documents &&
        visibleTopTab() != TopTab::DevotionsPlans) {
        tabs_->value(commentaryGroup_);
        activeTopTab_ = TopTab::Commentary;
        secondaryTabIsGeneralBooks_ = false;
        dailyWorkspaceState_.tabActive = false;
    }
}

void RightPane::updateCommentarySelection(int verse) {
    if (highlightedCommentaryVerse_ == verse) return;
    int oldVerse = highlightedCommentaryVerse_;
    highlightedCommentaryVerse_ = verse;
    syncCommentarySelectionClass(oldVerse, verse);
}

std::string RightPane::activeBibleReference() const {
    if (!app_ || !app_->mainWindow() || !app_->mainWindow()->biblePane()) {
        return "";
    }

    BiblePane* biblePane = app_->mainWindow()->biblePane();
    std::string book = biblePane->currentBook();
    int chapter = biblePane->currentChapter();
    int verse = biblePane->currentVerse();
    if (book.empty() || chapter <= 0 || verse <= 0) return "";

    std::ostringstream ref;
    ref << book << " " << chapter << ":" << verse;
    return ref.str();
}

void RightPane::applyCommentaryStyleOverride() {
    if (!commentaryHtml_) return;
    commentaryHtml_->setStyleOverrideCss(htmlStyleOverrideCss_);
    syncCommentarySelectionClass(0, highlightedCommentaryVerse_);
}

void RightPane::syncCommentarySelectionClass(int oldVerse, int newVerse) {
    if (!commentaryHtml_) return;
    commentaryHtml_->updateElementStyleById(
        commentaryVerseNumberElementId(oldVerse),
        "",
        commentaryVerseNumberElementId(newVerse),
        commentaryVerseNumberSelectedStyle(),
        false);
}

void RightPane::showDictionaryEntry(const std::string& key) {
    std::string moduleName = initialDictionaryModuleForKey(app_, key);
    if (!moduleName.empty()) {
        setDictionaryModule(moduleName);
    } else if (currentDictionary_.empty()) {
        selectFirstDictionaryModule(dictionaryChoice_,
                                    dictionaryChoiceModules_,
                                    dictionaryChoiceLabels_,
                                    currentDictionary_);
    }

    if (!currentDictionary_.empty()) {
        showDictionaryEntryInternal(currentDictionary_, key);
    }
}

void RightPane::showDictionaryLookup(const std::string& key,
                                     const std::string& contextModule) {
    std::string moduleName =
        defaultDictionaryModuleForLookup(app_, key, contextModule);
    if (!moduleName.empty()) {
        setDictionaryModule(moduleName);
    } else if (currentDictionary_.empty()) {
        selectFirstDictionaryModule(dictionaryChoice_,
                                    dictionaryChoiceModules_,
                                    dictionaryChoiceLabels_,
                                    currentDictionary_);
    }

    if (!currentDictionary_.empty()) {
        showDictionaryEntryInternal(currentDictionary_, key);
    }
}

void RightPane::showDictionaryEntry(const std::string& moduleName,
                                    const std::string& key) {
    showDictionaryEntryInternal(moduleName, key);
}

void RightPane::showDictionaryEntryInternal(const std::string& moduleName,
                                            const std::string& key) {
    perf::ScopeTimer timer("RightPane::showDictionaryEntryInternal");
    bool moduleChanged = (moduleName != currentDictionary_);
    currentDictionary_ = moduleName;
    currentDictKey_ = trimCopy(key);
    if (moduleChanged) {
        setDictionaryModule(moduleName);
    }

    perf::StepTimer step;
    std::string resolvedKey;
    std::string html = app_->swordManager().getDictionaryEntry(
        moduleName, currentDictKey_, &resolvedKey);
    perf::logf("RightPane::showDictionaryEntryInternal getDictionaryEntry %s %s: %.3f ms",
               moduleName.c_str(), currentDictKey_.c_str(), step.elapsedMs());
    step.reset();
    if (!resolvedKey.empty()) {
        currentDictKey_ = resolvedKey;
    }
    if (dictionaryKeyInput_) {
        dictionaryKeyInput_->setDisplayedValue(currentDictKey_);
    }
    if (dictionaryHtml_) {
        dictionaryHtml_->setHtml(html);
        perf::logf("RightPane::showDictionaryEntryInternal dictionaryHtml_->setHtml: %.3f ms",
                   step.elapsedMs());
    }
    updateDictionaryNavigationChrome();
}

void RightPane::showGeneralBookEntry(const std::string& key) {
    if (currentGeneralBook_.empty()) {
        auto books = app_->swordManager().getGeneralBookModules();
        if (!books.empty()) {
            setGeneralBookModule(books.front().name);
        }
    }

    if (!currentGeneralBook_.empty()) {
        showGeneralBookEntry(currentGeneralBook_, key);
    }
}

void RightPane::showGeneralBookEntry(const std::string& moduleName,
                                     const std::string& key) {
    bool moduleChanged = (moduleName != currentGeneralBook_);
    currentGeneralBook_ = moduleName;
    module_choice::applyChoiceValue(generalBookChoice_,
                                    generalBookChoiceModules_,
                                    generalBookChoiceLabels_,
                                    currentGeneralBook_);
    if (moduleChanged || generalBookToc_.empty()) {
        populateGeneralBookToc();
    }

    if (generalBookToc_.empty()) {
        currentGeneralBookKey_.clear();
        generalBookLoadedStart_ = -1;
        generalBookLoadedEnd_ = -1;
        showGeneralBookTocOverlay(false);
        if (generalBookHtml_) {
            generalBookHtml_->setHtml(app_->swordManager().getGeneralBookEntry(moduleName, ""));
        }
        updateGeneralBookNavigationChrome();
        return;
    }

    int targetIndex = findGeneralBookTocIndex(generalBookToc_, trimCopy(key));
    if (targetIndex < 0) targetIndex = 0;

    currentGeneralBookKey_ = generalBookToc_[static_cast<size_t>(targetIndex)].key;
    generalBookLoadedStart_ = targetIndex;
    generalBookLoadedEnd_ = targetIndex;
    updateGeneralBookNavigationChrome();
    syncGeneralBookTreeSelection();
    rebuildGeneralBookWindow(targetIndex, true);
    ensureGeneralBookViewportFilled();
}

void RightPane::setCommentaryModule(const std::string& moduleName,
                                    bool activateCurrentVerse) {
    if (moduleName != currentCommentary_) {
        loadedCommentaryModule_.clear();
        loadedCommentaryChapterKey_.clear();
    }
    currentCommentary_ = moduleName;

    module_choice::applyChoiceValue(commentaryChoice_,
                                    commentaryChoiceModules_,
                                    commentaryChoiceLabels_,
                                    moduleName);

    updateCommentaryEditorChrome();

    if (!activateCurrentVerse) return;

    std::string reference = activeBibleReference();
    if (reference.empty()) reference = currentCommentaryRef_;
    if (!reference.empty()) {
        showCommentary(moduleName, reference);
    }
}

void RightPane::setDictionaryModule(const std::string& moduleName,
                                    bool loadKeys) {
    std::string selectedModule = moduleName;
    if (!selectedModule.empty() &&
        std::find(dictionaryChoiceModules_.begin(),
                  dictionaryChoiceModules_.end(),
                  selectedModule) == dictionaryChoiceModules_.end()) {
        selectedModule = dictionaryChoiceModules_.empty()
            ? std::string()
            : dictionaryChoiceModules_.front();
    }

    if (selectedModule == currentDictionary_ &&
        dictionaryKeysModule_ == selectedModule) {
        module_choice::applyChoiceValue(dictionaryChoice_,
                                        dictionaryChoiceModules_,
                                        dictionaryChoiceLabels_,
                                        selectedModule);
        if (dictionaryKeyInput_) {
            dictionaryKeyInput_->setDisplayedValue(currentDictKey_);
        }
        updateDictionaryNavigationChrome();
        return;
    }

    currentDictionary_ = selectedModule;

    module_choice::applyChoiceValue(dictionaryChoice_,
                                    dictionaryChoiceModules_,
                                    dictionaryChoiceLabels_,
                                    selectedModule);
    if (loadKeys) {
        populateDictionaryKeyChoices();
    } else {
        dictionaryKeys_.reset();
        dictionaryKeysModule_.clear();
        if (dictionaryKeyInput_) {
            dictionaryKeyInput_->setItems({});
            dictionaryKeyInput_->setDisplayedValue(currentDictKey_);
        }
    }
    updateDictionaryNavigationChrome();
}

void RightPane::setGeneralBookModule(const std::string& moduleName,
                                     bool loadToc) {
    currentGeneralBook_ = moduleName;

    module_choice::applyChoiceValue(generalBookChoice_,
                                    generalBookChoiceModules_,
                                    generalBookChoiceLabels_,
                                    moduleName);

    if (loadToc) {
        populateGeneralBookToc();
    } else {
        generalBookToc_.clear();
        generalBookTreeItemIndices_.clear();
        generalBookLoadedStart_ = -1;
        generalBookLoadedEnd_ = -1;
        showGeneralBookTocOverlay(false);
        if (generalBookTocTree_) generalBookTocTree_->clear();
    }
    updateGeneralBookNavigationChrome();
}

int RightPane::currentGeneralBookTocIndex() const {
    return findGeneralBookTocIndex(generalBookToc_, currentGeneralBookKey_);
}

std::string RightPane::generalBookSectionHtml(int tocIndex) {
    if (!app_ || currentGeneralBook_.empty() ||
        tocIndex < 0 || tocIndex >= static_cast<int>(generalBookToc_.size())) {
        return "";
    }

    const std::string& key = generalBookToc_[static_cast<size_t>(tocIndex)].key;
    std::string cacheKey = currentGeneralBook_ + "|" + key;
    auto it = generalBookSectionCache_.find(cacheKey);
    if (it != generalBookSectionCache_.end()) {
        noteGeneralBookSectionCacheUse(generalBookSectionCacheOrder_, cacheKey);
        return it->second;
    }

    std::string html = app_->swordManager().getGeneralBookEntry(currentGeneralBook_, key);
    generalBookSectionCache_[cacheKey] = html;
    noteGeneralBookSectionCacheUse(generalBookSectionCacheOrder_, cacheKey);
    evictGeneralBookSectionCache(generalBookSectionCache_,
                                 generalBookSectionCacheOrder_,
                                 kGeneralBookSectionCacheLimit);
    return html;
}

std::string RightPane::buildGeneralBookWindowHtml() {
    if (generalBookLoadedStart_ < 0 || generalBookLoadedEnd_ < generalBookLoadedStart_ ||
        generalBookLoadedStart_ >= static_cast<int>(generalBookToc_.size())) {
        return "";
    }

    std::ostringstream html;
    html << "<div class=\"general-book-stream\">\n";
    for (int tocIndex = generalBookLoadedStart_;
         tocIndex <= generalBookLoadedEnd_ &&
         tocIndex < static_cast<int>(generalBookToc_.size());
         ++tocIndex) {
        html << "<div class=\"general-book-section\" id=\""
             << generalBookSectionAnchorId(tocIndex) << "\" data-gb-index=\""
             << tocIndex << "\">\n";
        html << generalBookSectionHtml(tocIndex) << "\n";
        html << "</div>\n";
    }
    html << "</div>\n";
    return html.str();
}

void RightPane::rebuildGeneralBookWindow(int preserveIndex,
                                         bool alignPreserveToTop) {
    if (!generalBookHtml_) return;

    const std::string preserveAnchor =
        generalBookSectionAnchorId(std::clamp(preserveIndex,
                                              generalBookLoadedStart_,
                                              std::max(generalBookLoadedStart_,
                                                       generalBookLoadedEnd_)));
    int oldScrollY = generalBookHtml_->scrollY();
    int oldAnchorTop = generalBookHtml_->elementTopById(preserveAnchor);

    generalBookHtml_->setHtml(buildGeneralBookWindowHtml());

    if (alignPreserveToTop) {
        generalBookHtml_->scrollToAnchor(preserveAnchor);
    } else if (oldAnchorTop >= 0) {
        int newAnchorTop = generalBookHtml_->elementTopById(preserveAnchor);
        if (newAnchorTop >= 0) {
            generalBookHtml_->setScrollY(oldScrollY + (newAnchorTop - oldAnchorTop));
        }
    }
    updateGeneralBookNavigationChrome();
    syncGeneralBookTreeSelection();
}

void RightPane::ensureGeneralBookViewportFilled() {
    if (!generalBookHtml_ || generalBookToc_.empty() ||
        generalBookLoadedStart_ < 0 || generalBookLoadedEnd_ < generalBookLoadedStart_) {
        return;
    }

    int guard = 0;
    while (guard++ < kGeneralBookMaxLoadedSections &&
           generalBookLoadedEnd_ + 1 < static_cast<int>(generalBookToc_.size()) &&
           generalBookHtml_->contentHeight() <=
               generalBookHtml_->viewportHeightPixels() + kGeneralBookPageFillThresholdPx &&
           (generalBookLoadedEnd_ - generalBookLoadedStart_ + 1) < kGeneralBookMaxLoadedSections) {
        int preserveIndex = currentGeneralBookTocIndex();
        if (preserveIndex < 0) preserveIndex = generalBookLoadedStart_;
        ++generalBookLoadedEnd_;
        rebuildGeneralBookWindow(preserveIndex, false);
    }
}

void RightPane::showGeneralBookTocOverlay(bool show) {
    generalBookTocVisible_ = show && generalBookTocPanel_ &&
                             generalBookTocTree_ &&
                             !generalBookTreeItemIndices_.empty();
    if (generalBookTocPanel_) {
        if (generalBookTocVisible_) {
            syncGeneralBookTreeSelection();
            generalBookTocPanel_->show();
        } else {
            generalBookTocPanel_->hide();
        }
        generalBookTocPanel_->redraw();
    }
    updateGeneralBookNavigationChrome();
}

void RightPane::toggleGeneralBookTocOverlay() {
    showGeneralBookTocOverlay(!generalBookTocVisible_);
}

void RightPane::updateGeneralBookNavigationChrome() {
    int pageStart = generalBookLoadedStart_;
    int pageEnd = generalBookLoadedEnd_;
    if (pageStart < 0 || pageEnd < pageStart) {
        pageStart = currentGeneralBookTocIndex();
        pageEnd = pageStart;
    }

    bool canGoBack = pageStart > 0;
    bool canGoForward =
        pageEnd >= 0 && pageEnd + 1 < static_cast<int>(generalBookToc_.size());
    bool hasToc = !generalBookToc_.empty();

    if (generalBookBackButton_) {
        if (canGoBack) generalBookBackButton_->activate();
        else generalBookBackButton_->deactivate();
        generalBookBackButton_->redraw();
    }
    if (generalBookForwardButton_) {
        if (canGoForward) generalBookForwardButton_->activate();
        else generalBookForwardButton_->deactivate();
        generalBookForwardButton_->redraw();
    }
    if (generalBookContentsButton_) {
        if (hasToc) generalBookContentsButton_->activate();
        else generalBookContentsButton_->deactivate();
        generalBookContentsButton_->copy_label(generalBookTocVisible_ ? "Hide" : "Contents");
        generalBookContentsButton_->tooltip(
            generalBookTocVisible_
                ? "Hide the general-book contents tree"
                : "Open the general-book contents tree");
        generalBookContentsButton_->redraw();
    }
}

void RightPane::showAdjacentGeneralBookEntry(int delta) {
    int pageStart = generalBookLoadedStart_;
    int pageEnd = generalBookLoadedEnd_;
    if (pageStart < 0 || pageEnd < pageStart) {
        pageStart = currentGeneralBookTocIndex();
        pageEnd = pageStart;
    }
    if (pageStart < 0) return;

    int newIndex = delta < 0 ? pageStart - 1 : pageEnd + 1;
    if (newIndex < 0 || newIndex >= static_cast<int>(generalBookToc_.size())) {
        updateGeneralBookNavigationChrome();
        return;
    }

    showGeneralBookEntry(currentGeneralBook_,
                         generalBookToc_[static_cast<size_t>(newIndex)].key);
}

void RightPane::rebuildGeneralBookTocTree() {
    if (!generalBookTocTree_) return;

    generalBookTreeSyncing_ = true;
    generalBookTocTree_->clear();
    generalBookTreeItemIndices_.clear();

    std::vector<Fl_Tree_Item*> depthItems;
    depthItems.reserve(16);
    for (size_t i = 0; i < generalBookToc_.size(); ++i) {
        const auto& entry = generalBookToc_[i];
        Fl_Tree_Item* item = nullptr;
        if (entry.depth <= 0 || depthItems.empty()) {
            item = generalBookTocTree_->add(entry.label.c_str());
        } else {
            int parentDepth = std::min<int>(entry.depth - 1,
                                            static_cast<int>(depthItems.size()) - 1);
            item = generalBookTocTree_->add(depthItems[static_cast<size_t>(parentDepth)],
                                            entry.label.c_str());
        }
        if (!item) continue;

        generalBookTreeItemIndices_[item] = static_cast<int>(i);

        if (entry.depth <= 0) {
            depthItems.assign(1, item);
        } else {
            if (static_cast<int>(depthItems.size()) <= entry.depth) {
                depthItems.resize(static_cast<size_t>(entry.depth + 1), nullptr);
            }
            depthItems[static_cast<size_t>(entry.depth)] = item;
        }
    }
    generalBookTreeSyncing_ = false;
    syncGeneralBookTreeSelection();
}

void RightPane::syncGeneralBookTreeSelection() {
    if (!generalBookTocTree_ || generalBookTreeItemIndices_.empty()) return;

    int tocIndex = currentGeneralBookTocIndex();
    if (tocIndex < 0) return;

    Fl_Tree_Item* selected = nullptr;
    for (const auto& [item, index] : generalBookTreeItemIndices_) {
        if (index != tocIndex) continue;
        selected = const_cast<Fl_Tree_Item*>(item);
        break;
    }
    if (!selected) return;

    std::unordered_set<const Fl_Tree_Item*> openPath;
    openPath.reserve(static_cast<size_t>(selected->depth() + 2));
    for (Fl_Tree_Item* item = selected; item; item = item->parent()) {
        openPath.insert(item);
    }

    generalBookTreeSyncing_ = true;
    for (const auto& [item, index] : generalBookTreeItemIndices_) {
        (void)index;
        auto* treeItem = const_cast<Fl_Tree_Item*>(item);
        if (!treeItem || treeItem->children() <= 0) continue;

        if (openPath.find(treeItem) != openPath.end()) {
            generalBookTocTree_->open(treeItem, 0);
        } else {
            generalBookTocTree_->close(treeItem, 0);
        }
    }
    generalBookTocTree_->select_only(selected, 0);
    generalBookTocTree_->show_item_middle(selected);
    generalBookTreeSyncing_ = false;
}

void RightPane::restoreGeneralBookLoadedRangeFromHtml(const std::string& html) {
    int parsedStart = parseGeneralBookLoadedEdge(html, false);
    int parsedEnd = parseGeneralBookLoadedEdge(html, true);
    if (parsedStart >= 0 && parsedEnd >= parsedStart &&
        parsedEnd < static_cast<int>(generalBookToc_.size())) {
        generalBookLoadedStart_ = parsedStart;
        generalBookLoadedEnd_ = parsedEnd;
        currentGeneralBookKey_ = generalBookToc_[static_cast<size_t>(parsedStart)].key;
    } else {
        int tocIndex = currentGeneralBookTocIndex();
        generalBookLoadedStart_ = tocIndex;
        generalBookLoadedEnd_ = tocIndex;
    }
    updateGeneralBookNavigationChrome();
    syncGeneralBookTreeSelection();
}

bool RightPane::isDictionaryTabActive() const {
    return secondaryTabIsGeneralBooks_;
}

void RightPane::setDictionaryTabActive(bool dictionaryActive) {
    secondaryTabIsGeneralBooks_ = dictionaryActive;
    dailyWorkspaceState_.tabActive = false;
    if (!dictionaryActive) {
        showGeneralBookTocOverlay(false);
    }
    if (!tabs_) return;
    if (visibleTopTab() == TopTab::Documents) {
        activeTopTab_ = TopTab::Documents;
        return;
    }
    if (visibleTopTab() == TopTab::DevotionsPlans) {
        activeTopTab_ = TopTab::DevotionsPlans;
        return;
    }
    tabs_->value(dictionaryActive ? generalBooksGroup_ : commentaryGroup_);
    activeTopTab_ = dictionaryActive ? TopTab::GeneralBooks : TopTab::Commentary;
    tabs_->redraw();
}

bool RightPane::isDevotionsPlansTabActive() const {
    return visibleTopTab() == TopTab::DevotionsPlans;
}

void RightPane::setDevotionsPlansTabActive(bool active) {
    if (!tabs_) return;

    if (active) {
        showGeneralBookTocOverlay(false);
        tabs_->value(devotionsPlansGroup_);
        activeTopTab_ = TopTab::DevotionsPlans;
        dailyWorkspaceState_.tabActive = true;
        refreshDailyWorkspace(true);
    } else {
        dailyWorkspaceState_.tabActive = false;
        tabs_->value(secondaryTabIsGeneralBooks_ ? generalBooksGroup_ : commentaryGroup_);
        activeTopTab_ = secondaryTabIsGeneralBooks_
                            ? TopTab::GeneralBooks
                            : TopTab::Commentary;
    }
    tabs_->redraw();
}

bool RightPane::isDocumentsTabActive() const {
    return visibleTopTab() == TopTab::Documents;
}

void RightPane::setDocumentsTabActive(bool active) {
    if (!tabs_) return;
    if (active) {
        showGeneralBookTocOverlay(false);
        tabs_->value(documentsGroup_);
        activeTopTab_ = TopTab::Documents;
        dailyWorkspaceState_.tabActive = false;
        refreshDocumentChoices();
        updateDocumentChrome();
    } else {
        tabs_->value(secondaryTabIsGeneralBooks_ ? generalBooksGroup_ : commentaryGroup_);
        activeTopTab_ = secondaryTabIsGeneralBooks_
                            ? TopTab::GeneralBooks
                            : TopTab::Commentary;
    }
    tabs_->redraw();
}

void RightPane::setDailyDevotionModule(const std::string& moduleName,
                                       bool activateTab) {
    dailyWorkspaceState_.mode = DailyWorkspaceMode::Devotionals;
    dailyWorkspaceState_.devotionalModule = moduleName;
    if (dailyDevotionalChoice_) {
        module_choice::applyChoiceValue(dailyDevotionalChoice_,
                                        dailyDevotionalModules_,
                                        dailyDevotionalLabels_,
                                        dailyWorkspaceState_.devotionalModule);
    }
    refreshDailyWorkspace(true);
    if (activateTab) {
        setDevotionsPlansTabActive(true);
    }
}

void RightPane::setDailyReadingPlanModule(const std::string& moduleName,
                                          bool activateTab) {
    dailyWorkspaceState_.mode = DailyWorkspaceMode::ReadingPlans;
    dailyWorkspaceState_.readingPlanSource = DailyReadingPlanSource::SwordModule;
    dailyWorkspaceState_.readingPlanId = 0;
    dailyWorkspaceState_.swordReadingPlanModule = moduleName;
    dailyWorkspaceState_.readingPlanSelectedDateIso.clear();
    if (dailyReadingPlanChoice_) {
        int index = readingPlanChoiceIndexForSwordModule(dailyReadingPlanChoices_, moduleName);
        dailyReadingPlanChoice_->value(index);
    }
    refreshDailyWorkspace(true);
    if (activateTab) {
        setDevotionsPlansTabActive(true);
    }
}

DailyWorkspaceState RightPane::currentDailyWorkspaceState() const {
    DailyWorkspaceState state = dailyWorkspaceState_;
    state.tabActive = (visibleTopTab() == TopTab::DevotionsPlans);
    return state;
}

void RightPane::setDailyWorkspaceState(const DailyWorkspaceState& state) {
    dailyWorkspaceState_ = state;
    populateDailyDevotionModules();
    populateReadingPlanChoices();
    ensureDailyWorkspaceDates();
    refreshDailyWorkspace(true);
}

std::string RightPane::selectedDailyReadingPlanLabel() const {
    if (!app_) return "";

    if (dailyWorkspaceState_.readingPlanSource == DailyReadingPlanSource::SwordModule) {
        return dailyDevotionalHeadingLabel(dailyWorkspaceState_.swordReadingPlanModule);
    }

    if (dailyWorkspaceState_.readingPlanId <= 0) return "";

    ReadingPlan plan;
    if (!app_->readingPlanManager().getPlan(dailyWorkspaceState_.readingPlanId, plan)) {
        return "";
    }
    return plan.summary.name;
}

int RightPane::dictionaryPaneHeight() const {
    return dictionaryPaneGroup_ ? dictionaryPaneGroup_->h() : 0;
}

int RightPane::commentaryScrollY() const {
    return commentaryHtml_ ? commentaryHtml_->scrollY() : 0;
}

void RightPane::setCommentaryScrollY(int y) {
    if (commentaryHtml_) {
        commentaryHtml_->setScrollY(y);
    }
}

void RightPane::setDictionaryPaneHeight(int height) {
    if (!contentTile_ || !tabs_ || !commentaryGroup_ || !commentaryChoice_ ||
        !commentaryHtml_ || !commentaryEditor_ ||
        !generalBooksGroup_ || !generalBookChoice_ ||
        !generalBookBackButton_ || !generalBookForwardButton_ ||
        !generalBookContentsButton_ || !generalBookTocPanel_ ||
        !generalBookTocPanelHeader_ || !generalBookTocTree_ ||
        !generalBookHtml_ ||
        !documentsGroup_ || !documentsEditor_ ||
        !dictionaryPaneGroup_ || !dictionaryBackButton_ || !dictionaryKeyInput_ ||
        !dictionaryForwardButton_ ||
        !dictionaryChoice_ || !dictionaryHtml_) {
        return;
    }

    const int minTopH = 100;
    const int minBottomH = 90;

    int tileX = contentTile_->x();
    int tileY = contentTile_->y();
    int tileW = contentTile_->w();
    int tileH = contentTile_->h();
    if (tileH < (minTopH + minBottomH)) return;

    int bottomH = std::clamp(height,
                             minBottomH,
                             std::max(minBottomH, tileH - minTopH));
    int tabsH = tileH - bottomH;

    tabs_->resize(tileX, tileY, tileW, tabsH);
    layoutTopTabContents(tileX, tileY, tileW, tabsH);

    layoutDictionaryPane(tileX,
                         tileY + tabsH,
                         tileW,
                         bottomH,
                         dictionaryPaneGroup_,
                         dictionaryBackButton_,
                         dictionaryKeyInput_,
                         dictionaryForwardButton_,
                         dictionaryChoice_,
                         dictionaryHtml_);

    contentTile_->init_sizes();
    contentTile_->damage(FL_DAMAGE_ALL);
    contentTile_->redraw();
}

void RightPane::setStudyState(const std::string& commentaryModule,
                              const std::string& commentaryReference,
                              const std::string& dictionaryModule,
                              const std::string& dictionaryKey) {
    perf::ScopeTimer timer("RightPane::setStudyState");
    showGeneralBookTocOverlay(false);
    if (!commentaryModule.empty()) {
        setCommentaryModule(commentaryModule);
    }
    currentDictKey_ = dictionaryKey;
    if (!dictionaryModule.empty()) {
        setDictionaryModule(dictionaryModule, false);
    }

    currentCommentaryRef_ = commentaryReference;
    if (dictionaryModule.empty() && dictionaryKeyInput_) {
        dictionaryKeyInput_->setDisplayedValue(currentDictKey_);
    }
    updateCommentaryEditorChrome();
}

RightPane::DisplayBuffer RightPane::captureDisplayBuffer() const {
    DisplayBuffer buf;
    if (commentaryHtml_) {
        HtmlWidget::Snapshot snap = commentaryHtml_->captureSnapshot();
        buf.commentary.doc = snap.doc;
        buf.commentary.html = std::move(snap.html);
        buf.commentary.baseUrl = std::move(snap.baseUrl);
        buf.commentary.scrollY = snap.scrollY;
        buf.commentary.contentHeight = snap.contentHeight;
        buf.commentary.renderWidth = snap.renderWidth;
        buf.commentary.scrollbarVisible = snap.scrollbarVisible;
        buf.commentary.valid = snap.valid;
    }
    if (dictionaryHtml_) {
        HtmlWidget::Snapshot snap = dictionaryHtml_->captureSnapshot();
        buf.dictionary.doc = snap.doc;
        buf.dictionary.html = std::move(snap.html);
        buf.dictionary.baseUrl = std::move(snap.baseUrl);
        buf.dictionary.scrollY = snap.scrollY;
        buf.dictionary.contentHeight = snap.contentHeight;
        buf.dictionary.renderWidth = snap.renderWidth;
        buf.dictionary.scrollbarVisible = snap.scrollbarVisible;
        buf.dictionary.valid = snap.valid;
    }
    return buf;
}

RightPane::DisplayBuffer RightPane::takeDisplayBuffer() {
    DisplayBuffer buf;
    if (commentaryHtml_) {
        HtmlWidget::Snapshot snap = commentaryHtml_->takeSnapshot();
        buf.commentary.doc = std::move(snap.doc);
        buf.commentary.html = std::move(snap.html);
        buf.commentary.baseUrl = std::move(snap.baseUrl);
        buf.commentary.scrollY = snap.scrollY;
        buf.commentary.contentHeight = snap.contentHeight;
        buf.commentary.renderWidth = snap.renderWidth;
        buf.commentary.scrollbarVisible = snap.scrollbarVisible;
        buf.commentary.valid = snap.valid;
    }
    if (dictionaryHtml_) {
        HtmlWidget::Snapshot snap = dictionaryHtml_->takeSnapshot();
        buf.dictionary.doc = std::move(snap.doc);
        buf.dictionary.html = std::move(snap.html);
        buf.dictionary.baseUrl = std::move(snap.baseUrl);
        buf.dictionary.scrollY = snap.scrollY;
        buf.dictionary.contentHeight = snap.contentHeight;
        buf.dictionary.renderWidth = snap.renderWidth;
        buf.dictionary.scrollbarVisible = snap.scrollbarVisible;
        buf.dictionary.valid = snap.valid;
    }
    return buf;
}

void RightPane::restoreDisplayBuffer(const DisplayBuffer& buffer) {
    perf::ScopeTimer timer("RightPane::restoreDisplayBuffer(copy)");
    if (commentaryHtml_ && buffer.commentary.valid) {
        HtmlWidget::Snapshot snap;
        snap.doc = buffer.commentary.doc;
        snap.html = buffer.commentary.html;
        snap.baseUrl = buffer.commentary.baseUrl;
        snap.scrollY = buffer.commentary.scrollY;
        snap.contentHeight = buffer.commentary.contentHeight;
        snap.renderWidth = buffer.commentary.renderWidth;
        snap.scrollbarVisible = buffer.commentary.scrollbarVisible;
        snap.valid = buffer.commentary.valid;
        commentaryHtml_->restoreSnapshot(snap);
    }
    if (dictionaryHtml_ && buffer.dictionary.valid) {
        HtmlWidget::Snapshot snap;
        snap.doc = buffer.dictionary.doc;
        snap.html = buffer.dictionary.html;
        snap.baseUrl = buffer.dictionary.baseUrl;
        snap.scrollY = buffer.dictionary.scrollY;
        snap.contentHeight = buffer.dictionary.contentHeight;
        snap.renderWidth = buffer.dictionary.renderWidth;
        snap.scrollbarVisible = buffer.dictionary.scrollbarVisible;
        snap.valid = buffer.dictionary.valid;
        dictionaryHtml_->restoreSnapshot(snap);
    }
}

void RightPane::restoreDisplayBuffer(DisplayBuffer&& buffer) {
    perf::ScopeTimer timer("RightPane::restoreDisplayBuffer(move)");
    if (commentaryHtml_ && buffer.commentary.valid) {
        HtmlWidget::Snapshot snap;
        snap.doc = std::move(buffer.commentary.doc);
        snap.html = std::move(buffer.commentary.html);
        snap.baseUrl = std::move(buffer.commentary.baseUrl);
        snap.scrollY = buffer.commentary.scrollY;
        snap.contentHeight = buffer.commentary.contentHeight;
        snap.renderWidth = buffer.commentary.renderWidth;
        snap.scrollbarVisible = buffer.commentary.scrollbarVisible;
        snap.valid = buffer.commentary.valid;
        buffer.commentary.valid = false;
        commentaryHtml_->restoreSnapshot(std::move(snap));
    }
    if (dictionaryHtml_ && buffer.dictionary.valid) {
        HtmlWidget::Snapshot snap;
        snap.doc = std::move(buffer.dictionary.doc);
        snap.html = std::move(buffer.dictionary.html);
        snap.baseUrl = std::move(buffer.dictionary.baseUrl);
        snap.scrollY = buffer.dictionary.scrollY;
        snap.contentHeight = buffer.dictionary.contentHeight;
        snap.renderWidth = buffer.dictionary.renderWidth;
        snap.scrollbarVisible = buffer.dictionary.scrollbarVisible;
        snap.valid = buffer.dictionary.valid;
        buffer.dictionary.valid = false;
        dictionaryHtml_->restoreSnapshot(std::move(snap));
    }
}

void RightPane::redrawChrome() {
    damage(FL_DAMAGE_ALL);
    if (tabs_) {
        tabs_->damage(FL_DAMAGE_ALL);
        tabs_->redraw();
    }
    if (commentaryChoice_) commentaryChoice_->redraw();
    if (commentaryEditButton_) commentaryEditButton_->redraw();
    if (commentarySaveButton_) commentarySaveButton_->redraw();
    if (commentaryCancelButton_) commentaryCancelButton_->redraw();
    if (dictionaryBackButton_) dictionaryBackButton_->redraw();
    if (dictionaryKeyInput_) dictionaryKeyInput_->redraw();
    if (dictionaryForwardButton_) dictionaryForwardButton_->redraw();
    if (dictionaryChoice_) dictionaryChoice_->redraw();
    if (generalBookChoice_) generalBookChoice_->redraw();
    if (generalBookBackButton_) generalBookBackButton_->redraw();
    if (generalBookForwardButton_) generalBookForwardButton_->redraw();
    if (generalBookContentsButton_) generalBookContentsButton_->redraw();
    if (generalBookTocPanel_) generalBookTocPanel_->redraw();
    if (generalBookTocTree_) generalBookTocTree_->redraw();
    if (dailyModeButton_) dailyModeButton_->redraw();
    if (dailyDevotionalChoice_) dailyDevotionalChoice_->redraw();
    if (dailyReadingPlanChoice_) dailyReadingPlanChoice_->redraw();
    if (dailyPrevDayButton_) dailyPrevDayButton_->redraw();
    if (dailyDateButton_) dailyDateButton_->redraw();
    if (dailyTodayButton_) dailyTodayButton_->redraw();
    if (dailyNextDayButton_) dailyNextDayButton_->redraw();
    if (dailyNewPlanButton_) dailyNewPlanButton_->redraw();
    if (dailyEditPlanButton_) dailyEditPlanButton_->redraw();
    if (dailyDeletePlanButton_) dailyDeletePlanButton_->redraw();
    if (dailyCompleteButton_) dailyCompleteButton_->redraw();
    if (dailyCompleteThroughButton_) dailyCompleteThroughButton_->redraw();
    if (dailyRescheduleButton_) dailyRescheduleButton_->redraw();
    if (dailyCalendarGroup_) dailyCalendarGroup_->redraw();
    if (dailyMonthLabel_) dailyMonthLabel_->redraw();
    if (dailyCalendarWidget_) dailyCalendarWidget_->redraw();
    if (documentChoice_) documentChoice_->redraw();
    if (documentNewButton_) documentNewButton_->redraw();
    if (documentSaveButton_) documentSaveButton_->redraw();
    if (documentExportButton_) documentExportButton_->redraw();
    if (documentDeleteButton_) documentDeleteButton_->redraw();
    redraw();
}

void RightPane::refresh() {
    perf::ScopeTimer timer("RightPane::refresh");
    TopTab keepTab = visibleTopTab();

    bool commentaryVisible = (keepTab == TopTab::Commentary);
    if (!currentCommentary_.empty() &&
        !currentCommentaryRef_.empty() &&
        (commentaryVisible || commentaryEditing_)) {
        showCommentary(currentCommentary_, currentCommentaryRef_);
    }
    if (!currentDictionary_.empty() && !currentDictKey_.empty()) {
        showDictionaryEntryInternal(currentDictionary_, currentDictKey_);
    }
    // Lazy-load general books to avoid paying parse/render cost on every cold
    // tab activation when user is reading commentary.
    if (!currentGeneralBook_.empty() && secondaryTabIsGeneralBooks_) {
        showGeneralBookEntry(currentGeneralBook_, currentGeneralBookKey_);
    }
    if (keepTab == TopTab::DevotionsPlans || dailyWorkspaceState_.tabActive) {
        refreshDailyWorkspace(true);
    }

    if (keepTab == TopTab::Documents) {
        setDocumentsTabActive(true);
    } else if (keepTab == TopTab::DevotionsPlans) {
        setDevotionsPlansTabActive(true);
    } else {
        setDictionaryTabActive(keepTab == TopTab::GeneralBooks);
    }
    updateCommentaryEditorChrome();
    updateDailyWorkspaceControls();
    updateDocumentChrome();
}

std::string RightPane::commentaryChapterKeyForReference(const std::string& reference,
                                                        int* verseOut) const {
    if (verseOut) *verseOut = 0;

    SwordManager::VerseRef ref;
    try {
        ref = SwordManager::parseVerseRef(reference);
    } catch (...) {
        return "";
    }

    if (ref.book.empty() || ref.chapter <= 0) return "";
    if (verseOut) {
        *verseOut = ref.verse > 0 ? ref.verse : 0;
    }
    return ref.book + " " + std::to_string(ref.chapter);
}

bool RightPane::lookupCommentaryCache(const std::string& cacheKey,
                                      std::string& htmlOut) {
    auto it = commentaryChapterCache_.find(cacheKey);
    if (it == commentaryChapterCache_.end()) return false;
    htmlOut = it->second;

    auto ordIt = std::find(commentaryChapterCacheOrder_.begin(),
                           commentaryChapterCacheOrder_.end(),
                           cacheKey);
    if (ordIt != commentaryChapterCacheOrder_.end()) {
        commentaryChapterCacheOrder_.erase(ordIt);
    }
    commentaryChapterCacheOrder_.push_back(cacheKey);
    return true;
}

void RightPane::storeCommentaryCache(const std::string& cacheKey,
                                     const std::string& html) {
    auto cachedBytes = [](const std::string& key, const std::string& value) {
        return key.size() + value.size();
    };

    auto existing = commentaryChapterCache_.find(cacheKey);
    if (existing != commentaryChapterCache_.end()) {
        commentaryChapterCacheBytes_ -= cachedBytes(existing->first, existing->second);
        existing->second = html;
    } else {
        commentaryChapterCache_.emplace(cacheKey, html);
    }
    commentaryChapterCacheBytes_ += cachedBytes(cacheKey, html);

    auto ordIt = std::find(commentaryChapterCacheOrder_.begin(),
                           commentaryChapterCacheOrder_.end(),
                           cacheKey);
    if (ordIt != commentaryChapterCacheOrder_.end()) {
        commentaryChapterCacheOrder_.erase(ordIt);
    }
    commentaryChapterCacheOrder_.push_back(cacheKey);

    while (commentaryChapterCacheOrder_.size() > kCommentaryChapterCacheLimit ||
           commentaryChapterCacheBytes_ > kCommentaryChapterCacheByteLimit) {
        const std::string evict = commentaryChapterCacheOrder_.front();
        commentaryChapterCacheOrder_.pop_front();
        auto it = commentaryChapterCache_.find(evict);
        if (it == commentaryChapterCache_.end()) continue;
        commentaryChapterCacheBytes_ -= cachedBytes(it->first, it->second);
        commentaryChapterCache_.erase(it);
    }
}

void RightPane::invalidateCommentaryCache(const std::string& moduleName,
                                          const std::string& reference) {
    auto cachedBytes = [](const std::string& key, const std::string& value) {
        return key.size() + value.size();
    };

    int verse = 0;
    std::string chapterKey = commentaryChapterKeyForReference(reference, &verse);
    if (chapterKey.empty()) {
        commentaryChapterCache_.clear();
        commentaryChapterCacheOrder_.clear();
        commentaryChapterCacheBytes_ = 0;
    } else {
        std::string cacheKey = moduleName + "|" + chapterKey;
        auto cacheIt = commentaryChapterCache_.find(cacheKey);
        if (cacheIt != commentaryChapterCache_.end()) {
            commentaryChapterCacheBytes_ -=
                cachedBytes(cacheIt->first, cacheIt->second);
            commentaryChapterCache_.erase(cacheIt);
        }
        auto it = std::find(commentaryChapterCacheOrder_.begin(),
                            commentaryChapterCacheOrder_.end(),
                            cacheKey);
        if (it != commentaryChapterCacheOrder_.end()) {
            commentaryChapterCacheOrder_.erase(it);
        }
    }

    if (loadedCommentaryModule_ == moduleName &&
        loadedCommentaryChapterKey_ == chapterKey) {
        loadedCommentaryModule_.clear();
        loadedCommentaryChapterKey_.clear();
    }
}

void RightPane::setHtmlStyleOverride(const std::string& css) {
    htmlStyleOverrideCss_ = css;
    applyCommentaryStyleOverride();
    if (dictionaryHtml_) dictionaryHtml_->setStyleOverrideCss(css);
    if (generalBookHtml_) generalBookHtml_->setStyleOverrideCss(css);
    if (dailyHtml_) dailyHtml_->setStyleOverrideCss(css);
}

void RightPane::setEditorIndentWidth(int width) {
    if (commentaryEditor_) commentaryEditor_->setIndentWidth(width);
    if (documentsEditor_) documentsEditor_->setIndentWidth(width);
}

void RightPane::setEditorLineHeight(double lineHeight) {
    if (commentaryEditor_) commentaryEditor_->setLineHeight(lineHeight);
    if (documentsEditor_) documentsEditor_->setLineHeight(lineHeight);
}

void RightPane::setEditorTextFont(Fl_Font regularFont, Fl_Font boldFont, int size) {
    if (commentaryEditor_) commentaryEditor_->setTextFont(regularFont, boldFont, size);
    if (documentsEditor_) documentsEditor_->setTextFont(regularFont, boldFont, size);
}

void RightPane::populateCommentaryModules() {
    if (!commentaryChoice_) return;

    auto mods = app_->swordManager().getCommentaryModules();
    module_choice::populateChoice(commentaryChoice_, mods,
                                  commentaryChoiceModules_,
                                  commentaryChoiceLabels_);

    if (commentaryChoice_->size() > 0) {
        currentCommentary_ = commentaryChoiceModules_.front();
    }

    updateCommentaryEditorChrome();
}

void RightPane::populateDictionaryModules(bool eagerKeyLoad) {
    if (!dictionaryChoice_) return;

    auto mods = app_->swordManager().getDictionaryModules();
    module_choice::populateChoice(dictionaryChoice_, mods,
                                  dictionaryChoiceModules_,
                                  dictionaryChoiceLabels_);

    if (dictionaryChoice_->size() > 0) {
        currentDictionary_ = dictionaryChoiceModules_.front();
        if (eagerKeyLoad) {
            populateDictionaryKeyChoices();
        } else {
            dictionaryKeys_.reset();
            dictionaryKeysModule_.clear();
            if (dictionaryKeyInput_) {
                dictionaryKeyInput_->setItems({});
                dictionaryKeyInput_->setDisplayedValue(currentDictKey_);
            }
        }
    } else {
        dictionaryKeys_.reset();
        dictionaryKeysModule_.clear();
        currentDictionary_.clear();
        currentDictKey_.clear();
        if (dictionaryKeyInput_) {
            dictionaryKeyInput_->setItems({});
            dictionaryKeyInput_->setDisplayedValue("");
        }
        if (dictionaryHtml_) {
            dictionaryHtml_->setHtml(
                "<p><i>No dictionary modules installed.</i></p>");
        }
    }
    updateDictionaryNavigationChrome();
}

void RightPane::populateDictionaryKeyChoices() {
    if (!dictionaryKeyInput_ || !app_) return;

    if (currentDictionary_.empty()) {
        dictionaryKeys_.reset();
        dictionaryKeysModule_.clear();
        dictionaryKeyInput_->setItems({});
        dictionaryKeyInput_->setDisplayedValue(currentDictKey_);
        return;
    }

    ensureDictionaryKeysLoaded();
    dictionaryKeyInput_->setDisplayedValue(currentDictKey_);
}

void RightPane::ensureDictionaryKeysLoaded() {
    if (!dictionaryKeyInput_ || !app_ || currentDictionary_.empty()) return;
    if (dictionaryKeysModule_ == currentDictionary_ && dictionaryKeys_) return;

    dictionaryKeys_ = app_->swordManager().getDictionaryKeys(currentDictionary_);
    dictionaryKeysModule_ = currentDictionary_;
    dictionaryKeyInput_->setItemsView(dictionaryKeys_.get());
    updateDictionaryNavigationChrome();
}

void RightPane::updateDictionaryNavigationChrome() {
    const int keyIndex = currentDictionaryKeyIndex();
    const bool canGoBack = keyIndex > 0;
    const bool canGoForward =
        keyIndex >= 0 &&
        dictionaryKeys_ &&
        keyIndex + 1 < static_cast<int>(dictionaryKeys_->size());

    if (dictionaryBackButton_) {
        if (canGoBack) dictionaryBackButton_->activate();
        else dictionaryBackButton_->deactivate();
        dictionaryBackButton_->redraw();
    }
    if (dictionaryForwardButton_) {
        if (canGoForward) dictionaryForwardButton_->activate();
        else dictionaryForwardButton_->deactivate();
        dictionaryForwardButton_->redraw();
    }
}

int RightPane::currentDictionaryKeyIndex() const {
    if (currentDictKey_.empty() || !dictionaryKeys_ || dictionaryKeys_->empty()) {
        return -1;
    }

    auto it = std::find(dictionaryKeys_->begin(), dictionaryKeys_->end(),
                        currentDictKey_);
    if (it == dictionaryKeys_->end()) {
        std::string selected = dictionaryKeyInput_
            ? dictionaryKeyInput_->selectedValue()
            : "";
        if (!selected.empty()) {
            it = std::find(dictionaryKeys_->begin(), dictionaryKeys_->end(),
                           selected);
        }
    }
    if (it == dictionaryKeys_->end()) return -1;
    return static_cast<int>(it - dictionaryKeys_->begin());
}

void RightPane::showAdjacentDictionaryEntry(int delta) {
    if (currentDictionary_.empty() || !dictionaryKeys_ || dictionaryKeys_->empty()) {
        return;
    }

    int keyIndex = currentDictionaryKeyIndex();
    if (keyIndex < 0) return;

    int newIndex = keyIndex + delta;
    if (newIndex < 0 || newIndex >= static_cast<int>(dictionaryKeys_->size())) {
        updateDictionaryNavigationChrome();
        return;
    }

    showDictionaryEntryInternal(currentDictionary_,
                                (*dictionaryKeys_)[static_cast<size_t>(newIndex)]);
}

void RightPane::populateGeneralBookModules(bool eagerLoad) {
    if (!generalBookChoice_) return;

    auto mods = app_->swordManager().getGeneralBookModules();
    module_choice::populateChoice(generalBookChoice_, mods,
                                  generalBookChoiceModules_,
                                  generalBookChoiceLabels_);

    if (generalBookChoice_->size() > 0) {
        std::string selectedModule = currentGeneralBook_;
        if (!module_choice::applyChoiceValue(generalBookChoice_,
                                            generalBookChoiceModules_,
                                            generalBookChoiceLabels_,
                                            selectedModule)) {
            selectedModule = generalBookChoiceModules_.front();
            module_choice::applyChoiceValue(generalBookChoice_,
                                            generalBookChoiceModules_,
                                            generalBookChoiceLabels_,
                                            selectedModule);
        }
        currentGeneralBook_ = selectedModule;
        if (eagerLoad) {
            populateGeneralBookToc();
            showGeneralBookEntry(currentGeneralBook_, currentGeneralBookKey_);
        } else {
            generalBookToc_.clear();
            generalBookTreeItemIndices_.clear();
            generalBookLoadedStart_ = -1;
            generalBookLoadedEnd_ = -1;
            if (generalBookTocTree_) generalBookTocTree_->clear();
            showGeneralBookTocOverlay(false);
        }
    } else {
        currentGeneralBook_.clear();
        currentGeneralBookKey_.clear();
        generalBookToc_.clear();
        generalBookTreeItemIndices_.clear();
        generalBookLoadedStart_ = -1;
        generalBookLoadedEnd_ = -1;
        if (generalBookTocTree_) generalBookTocTree_->clear();
        showGeneralBookTocOverlay(false);
        if (generalBookHtml_) {
            generalBookHtml_->setHtml(
                "<p><i>No general book modules installed.</i></p>");
        }
    }
    updateGeneralBookNavigationChrome();
}

void RightPane::populateGeneralBookToc() {
    generalBookToc_.clear();
    generalBookTreeItemIndices_.clear();
    generalBookLoadedStart_ = -1;
    generalBookLoadedEnd_ = -1;
    showGeneralBookTocOverlay(false);
    if (generalBookTocTree_) {
        generalBookTocTree_->clear();
    }
    if (currentGeneralBook_.empty()) return;

    generalBookToc_ = app_->swordManager().getGeneralBookToc(currentGeneralBook_);
    if (!generalBookToc_.empty()) {
        int selectedIndex = findGeneralBookTocIndex(generalBookToc_,
                                                    currentGeneralBookKey_);
        if (selectedIndex < 0) {
            selectedIndex = 0;
            currentGeneralBookKey_ = generalBookToc_.front().key;
        }
        rebuildGeneralBookTocTree();
    } else {
        currentGeneralBookKey_.clear();
    }
    updateGeneralBookNavigationChrome();
}

void RightPane::populateDailyDevotionModules() {
    if (!dailyDevotionalChoice_ || !app_) return;

    auto mods = app_->swordManager().getDailyDevotionModules();
    module_choice::populateChoice(dailyDevotionalChoice_, mods,
                                  dailyDevotionalModules_,
                                  dailyDevotionalLabels_);

    if (!dailyDevotionalModules_.empty()) {
        if (std::find(dailyDevotionalModules_.begin(),
                      dailyDevotionalModules_.end(),
                      dailyWorkspaceState_.devotionalModule) ==
            dailyDevotionalModules_.end()) {
            dailyWorkspaceState_.devotionalModule = dailyDevotionalModules_.front();
        }
        module_choice::applyChoiceValue(dailyDevotionalChoice_,
                                        dailyDevotionalModules_,
                                        dailyDevotionalLabels_,
                                        dailyWorkspaceState_.devotionalModule);
    } else {
        dailyWorkspaceState_.devotionalModule.clear();
        dailyDevotionalChoice_->clear();
    }
}

void RightPane::populateReadingPlanChoices() {
    if (!dailyReadingPlanChoice_ || !app_) return;

    const DailyReadingPlanSource previousSource = dailyWorkspaceState_.readingPlanSource;
    const int previousPlanId = dailyWorkspaceState_.readingPlanId;
    const std::string previousSwordModule = dailyWorkspaceState_.swordReadingPlanModule;

    auto plans = app_->readingPlanManager().listPlans();
    auto swordPlans = app_->swordManager().getDailyReadingPlanModules();
    dailyReadingPlanChoice_->clear();
    dailyReadingPlanChoices_.clear();

    for (const auto& plan : plans) {
        std::ostringstream label;
        label << plan.name;
        if (plan.totalDays > 0) {
            label << " (" << plan.completedDays << "/" << plan.totalDays << ")";
        }
        dailyReadingPlanChoice_->add(module_choice::escapeMenuLabel(label.str()).c_str());
        DailyReadingPlanChoiceItem item;
        item.kind = DailyReadingPlanChoiceItem::Kind::EditablePlan;
        item.planId = plan.id;
        dailyReadingPlanChoices_.push_back(std::move(item));
    }

    for (const auto& module : swordPlans) {
        std::string label = module_choice::formatLabel(module) + " (read-only)";
        dailyReadingPlanChoice_->add(module_choice::escapeMenuLabel(label).c_str());
        DailyReadingPlanChoiceItem item;
        item.kind = DailyReadingPlanChoiceItem::Kind::SwordModule;
        item.moduleName = module.name;
        dailyReadingPlanChoices_.push_back(std::move(item));
    }

    int selectedIndex = -1;
    if (dailyWorkspaceState_.readingPlanSource == DailyReadingPlanSource::SwordModule) {
        selectedIndex = readingPlanChoiceIndexForSwordModule(dailyReadingPlanChoices_,
                                                             dailyWorkspaceState_.swordReadingPlanModule);
    } else {
        selectedIndex = readingPlanChoiceIndexForEditableId(dailyReadingPlanChoices_,
                                                            dailyWorkspaceState_.readingPlanId);
    }

    if (selectedIndex < 0 && !dailyReadingPlanChoices_.empty()) {
        const auto& first = dailyReadingPlanChoices_.front();
        if (first.kind == DailyReadingPlanChoiceItem::Kind::SwordModule) {
            dailyWorkspaceState_.readingPlanSource = DailyReadingPlanSource::SwordModule;
            dailyWorkspaceState_.readingPlanId = 0;
            dailyWorkspaceState_.swordReadingPlanModule = first.moduleName;
        } else {
            dailyWorkspaceState_.readingPlanSource = DailyReadingPlanSource::Editable;
            dailyWorkspaceState_.readingPlanId = first.planId;
            dailyWorkspaceState_.swordReadingPlanModule.clear();
        }
        selectedIndex = 0;
    }

    if (selectedIndex >= 0) {
        dailyReadingPlanChoice_->value(selectedIndex);
    } else {
        dailyWorkspaceState_.readingPlanSource = DailyReadingPlanSource::Editable;
        dailyWorkspaceState_.readingPlanId = 0;
        dailyWorkspaceState_.swordReadingPlanModule.clear();
        dailyReadingPlanChoice_->value(-1);
    }

    if (dailyWorkspaceState_.readingPlanSource != previousSource ||
        dailyWorkspaceState_.readingPlanId != previousPlanId ||
        dailyWorkspaceState_.swordReadingPlanModule != previousSwordModule) {
        dailyWorkspaceState_.readingPlanSelectedDateIso.clear();
    }
}

void RightPane::ensureDailyWorkspaceDates() {
    if (!reading::isIsoDateInRange(dailyWorkspaceState_.selectedDateIso)) {
        dailyWorkspaceState_.selectedDateIso = reading::formatIsoDate(reading::today());
    }
    if (app_ &&
        dailyWorkspaceState_.readingPlanSource == DailyReadingPlanSource::SwordModule &&
        !dailyWorkspaceState_.swordReadingPlanModule.empty()) {
        app_->readingPlanManager().ensureSwordScheduleInitialized(
            dailyWorkspaceState_.swordReadingPlanModule);
    }
    if (!reading::isIsoDateInRange(dailyWorkspaceState_.readingPlanSelectedDateIso)) {
        dailyWorkspaceState_.readingPlanSelectedDateIso = defaultReadingPlanDateIso();
    }
}

std::string RightPane::currentDailyDateIso() const {
    if (dailyWorkspaceState_.mode == DailyWorkspaceMode::ReadingPlans) {
        if (reading::isIsoDateInRange(dailyWorkspaceState_.readingPlanSelectedDateIso)) {
            return dailyWorkspaceState_.readingPlanSelectedDateIso;
        }
    } else if (reading::isIsoDateInRange(dailyWorkspaceState_.selectedDateIso)) {
        return dailyWorkspaceState_.selectedDateIso;
    }
    return reading::formatIsoDate(reading::today());
}

void RightPane::setCurrentDailyDateIso(const std::string& dateIso) {
    if (!reading::isIsoDateInRange(dateIso)) return;
    if (dailyWorkspaceState_.mode == DailyWorkspaceMode::ReadingPlans) {
        dailyWorkspaceState_.readingPlanSelectedDateIso = dateIso;
    } else {
        dailyWorkspaceState_.selectedDateIso = dateIso;
    }
}

std::vector<std::string> RightPane::selectedDailyCalendarDateIsos() const {
    if (!dailyCalendarWidget_) {
        return {currentDailyDateIso()};
    }

    std::vector<std::string> dateIsos = dailyCalendarWidget_->selectedDateIsos();
    if (dateIsos.empty()) {
        dateIsos.push_back(currentDailyDateIso());
    }
    return dateIsos;
}

std::vector<std::string> RightPane::actionableSelectedReadingPlanDateIsos(
    bool* allCompleted) const {
    std::vector<std::string> actionable;
    if (allCompleted) *allCompleted = false;
    if (!app_) return actionable;

    const std::vector<std::string> selectedDateIsos = selectedDailyCalendarDateIsos();
    bool anyCompletedState = false;
    bool everyCompleted = true;

    if (dailyWorkspaceState_.readingPlanSource == DailyReadingPlanSource::SwordModule) {
        for (const auto& dateIso : selectedDateIsos) {
            if (!swordReadingPlanTemplateDayForDate(dailyWorkspaceState_.swordReadingPlanModule,
                                                    dateIso)) {
                continue;
            }
            actionable.push_back(dateIso);
            const bool completed = app_->readingPlanManager().swordDayCompleted(
                dailyWorkspaceState_.swordReadingPlanModule, dateIso);
            anyCompletedState = true;
            everyCompleted = everyCompleted && completed;
        }
    } else if (dailyWorkspaceState_.readingPlanId > 0) {
        ReadingPlan plan;
        if (!app_->readingPlanManager().getPlan(dailyWorkspaceState_.readingPlanId, plan)) {
            return actionable;
        }

        for (const auto& dateIso : selectedDateIsos) {
            const ReadingPlanDay* day = readingPlanDayForDate(plan, dateIso);
            if (!day) continue;
            actionable.push_back(dateIso);
            anyCompletedState = true;
            everyCompleted = everyCompleted && day->completed;
        }
    }

    if (allCompleted) {
        *allCompleted = anyCompletedState && everyCompleted;
    }
    return actionable;
}

std::vector<std::string> RightPane::actionableReadingPlanDatesThroughCurrent(
    bool* allCompleted) const {
    std::vector<std::string> actionable;
    if (allCompleted) *allCompleted = false;
    if (!app_) return actionable;

    const std::string currentDateIso = dailyWorkspaceState_.readingPlanSelectedDateIso;
    if (!reading::isIsoDateInRange(currentDateIso)) return actionable;

    bool anyCompletedState = false;
    bool everyCompleted = true;

    if (dailyWorkspaceState_.readingPlanSource == DailyReadingPlanSource::SwordModule) {
        const std::string& moduleName = dailyWorkspaceState_.swordReadingPlanModule;
        if (moduleName.empty()) return actionable;

        for (const auto& day : swordReadingPlanTemplateDays(moduleName)) {
            const std::string dateIso =
                app_->readingPlanManager().swordScheduledDateForDay(moduleName,
                                                                    day.sequenceNumber);
            if (!reading::isIsoDateInRange(dateIso) || dateIso > currentDateIso) {
                continue;
            }
            actionable.push_back(dateIso);
            const bool completed = app_->readingPlanManager().swordDayCompleted(
                moduleName, dateIso);
            anyCompletedState = true;
            everyCompleted = everyCompleted && completed;
        }
    } else if (dailyWorkspaceState_.readingPlanId > 0) {
        ReadingPlan plan;
        if (!app_->readingPlanManager().getPlan(dailyWorkspaceState_.readingPlanId, plan)) {
            return actionable;
        }

        for (const auto& day : plan.days) {
            if (day.dateIso > currentDateIso) break;
            actionable.push_back(day.dateIso);
            anyCompletedState = true;
            everyCompleted = everyCompleted && day.completed;
        }
    }

    if (allCompleted) {
        *allCompleted = anyCompletedState && everyCompleted;
    }
    return actionable;
}

std::string RightPane::defaultReadingPlanDateIso() const {
    const std::string todayIso = reading::formatIsoDate(reading::today());
    if (!app_) return todayIso;

    if (dailyWorkspaceState_.readingPlanSource == DailyReadingPlanSource::SwordModule) {
        return defaultSwordReadingPlanDateIso(dailyWorkspaceState_.swordReadingPlanModule);
    }
    return defaultEditableReadingPlanDateIso(dailyWorkspaceState_.readingPlanId);
}

std::string RightPane::defaultEditableReadingPlanDateIso(int planId) const {
    const std::string todayIso = reading::formatIsoDate(reading::today());
    if (!app_ || planId <= 0) return todayIso;

    ReadingPlan plan;
    if (!app_->readingPlanManager().getPlan(planId, plan) || plan.days.empty()) {
        return todayIso;
    }

    for (const auto& day : plan.days) {
        if (!day.completed && day.dateIso >= todayIso) {
            return day.dateIso;
        }
    }
    for (const auto& day : plan.days) {
        if (!day.completed) return day.dateIso;
    }
    return plan.days.back().dateIso;
}

std::string RightPane::defaultSwordReadingPlanDateIso(const std::string& moduleName) const {
    const std::string todayIso = reading::formatIsoDate(reading::today());
    if (!app_ || moduleName.empty()) return todayIso;

    const auto& days = swordReadingPlanTemplateDays(moduleName);
    if (days.empty()) return todayIso;

    for (const auto& day : days) {
        const std::string dateIso =
            app_->readingPlanManager().swordScheduledDateForDay(moduleName,
                                                                day.sequenceNumber);
        if (!reading::isIsoDateInRange(dateIso)) continue;
        if (!app_->readingPlanManager().swordDayCompleted(moduleName, dateIso) &&
            dateIso >= todayIso) {
            return dateIso;
        }
    }
    for (const auto& day : days) {
        const std::string dateIso =
            app_->readingPlanManager().swordScheduledDateForDay(moduleName,
                                                                day.sequenceNumber);
        if (!reading::isIsoDateInRange(dateIso)) continue;
        if (!app_->readingPlanManager().swordDayCompleted(moduleName, dateIso)) {
            return dateIso;
        }
    }
    const std::string lastDateIso = app_->readingPlanManager().swordScheduledDateForDay(
        moduleName, days.back().sequenceNumber);
    return reading::isIsoDateInRange(lastDateIso) ? lastDateIso : todayIso;
}

const std::vector<SwordReadingPlanTemplateDay>& RightPane::swordReadingPlanTemplateDays(
    const std::string& moduleName) const {
    static const std::vector<SwordReadingPlanTemplateDay> kEmptyDays;
    if (!app_ || moduleName.empty()) return kEmptyDays;

    auto found = swordReadingPlanTemplateCache_.find(moduleName);
    if (found != swordReadingPlanTemplateCache_.end()) {
        return found->second;
    }

    std::vector<SwordReadingPlanTemplateDay> days;
    const int referenceYear = 2000;
    for (int month = 1; month <= 12; ++month) {
        const int dayCount = reading::daysInMonth(referenceYear, month);
        for (int day = 1; day <= dayCount; ++day) {
            const std::string moduleKey = formatSwordMonthDayKey(month, day);
            std::string entryHtml =
                app_->swordManager().getDailyDevotionEntry(moduleName, moduleKey);
            std::string rewrittenBody = extractDailyEntryBodyHtml(
                rewriteSwordReadingPlanLinks(entryHtml, nullptr));
            std::vector<std::string> itemHtml = extractSwordReadingPlanItemHtml(rewrittenBody);
            if (itemHtml.empty() && trimCopy(rewrittenBody).empty()) continue;

            SwordReadingPlanTemplateDay templateDay;
            templateDay.sequenceNumber = static_cast<int>(days.size()) + 1;
            templateDay.moduleKey = moduleKey;
            days.push_back(std::move(templateDay));
        }
    }

    auto inserted = swordReadingPlanTemplateCache_.emplace(moduleName, std::move(days));
    return inserted.first->second;
}

const SwordReadingPlanTemplateDay* RightPane::swordReadingPlanTemplateDayForDate(
    const std::string& moduleName,
    const std::string& dateIso,
    int* dayNumberOut) const {
    if (dayNumberOut) *dayNumberOut = -1;
    if (!app_ || moduleName.empty() || !reading::isIsoDateInRange(dateIso)) {
        return nullptr;
    }

    const auto& days = swordReadingPlanTemplateDays(moduleName);
    if (days.empty()) return nullptr;

    const int dayNumber = app_->readingPlanManager().swordDayNumberForDate(moduleName, dateIso);
    if (dayNumber <= 0 || dayNumber > static_cast<int>(days.size())) {
        return nullptr;
    }
    if (dayNumberOut) *dayNumberOut = dayNumber;
    return &days[static_cast<size_t>(dayNumber - 1)];
}

bool RightPane::swordReadingPlanHasContentForDate(const std::string& moduleName,
                                                  const std::string& dateIso) const {
    return swordReadingPlanTemplateDayForDate(moduleName, dateIso) != nullptr;
}

void RightPane::updateDailyCalendarHeader() {
    if (!dailyCalendarWidget_ || !dailyMonthLabel_) return;
    reading::Date month = dailyCalendarWidget_->displayedMonth();
    std::string label = reading::monthName(month.month);
    if (label.empty()) label = "Calendar";
    else label += " " + std::to_string(month.year);
    dailyMonthLabel_->copy_label(label.c_str());
}

void RightPane::updateDailyCalendarMeta() {
    if (!dailyCalendarWidget_ || !app_) return;

    std::unordered_map<std::string, CalendarDayMeta> meta;
    reading::Date month = dailyCalendarWidget_->displayedMonth();
    if (!month.valid()) {
        month = reading::today();
        dailyCalendarWidget_->setDisplayedMonth(month);
    }
    reading::Date today = reading::today();

    if (dailyWorkspaceState_.mode == DailyWorkspaceMode::Devotionals) {
        if (!dailyWorkspaceState_.devotionalModule.empty()) {
            auto summaries = app_->swordManager().getDailyDevotionMonthSummaries(
                dailyWorkspaceState_.devotionalModule, month.year, month.month);
            for (auto& entry : summaries) {
                CalendarDayMeta dayMeta;
                dayMeta.summary = std::move(entry.second);
                dayMeta.hasContent = true;
                meta.emplace(entry.first, std::move(dayMeta));
            }
        }
    } else if (dailyWorkspaceState_.readingPlanSource == DailyReadingPlanSource::SwordModule) {
        if (!dailyWorkspaceState_.swordReadingPlanModule.empty()) {
            const std::string& moduleName = dailyWorkspaceState_.swordReadingPlanModule;
            for (const auto& templateDay : swordReadingPlanTemplateDays(moduleName)) {
                const std::string scheduledDateIso =
                    app_->readingPlanManager().swordScheduledDateForDay(
                        moduleName, templateDay.sequenceNumber);
                reading::Date date{};
                if (!reading::parseIsoDate(scheduledDateIso, date)) continue;
                if (date.year != month.year || date.month != month.month) continue;

                CalendarDayMeta dayMeta;
                dayMeta.summary = swordReadingPlanCalendarSummary(
                    app_, moduleName, templateDay.moduleKey);
                dayMeta.hasContent = true;
                dayMeta.completed = app_->readingPlanManager().swordDayCompleted(
                    moduleName, scheduledDateIso);
                dayMeta.overdue = !dayMeta.completed &&
                                  reading::compareDates(date, today) < 0;
                meta.emplace(scheduledDateIso, std::move(dayMeta));
            }
        }
    } else if (dailyWorkspaceState_.readingPlanId > 0) {
        ReadingPlan plan;
        if (app_->readingPlanManager().getPlan(dailyWorkspaceState_.readingPlanId, plan)) {
            for (const auto& day : plan.days) {
                reading::Date date{};
                if (!reading::parseIsoDate(day.dateIso, date)) continue;
                if (date.year != month.year || date.month != month.month) continue;

                CalendarDayMeta dayMeta;
                dayMeta.summary = readingPlanDayCalendarSummary(app_, day);
                dayMeta.hasContent = true;
                dayMeta.completed = day.completed;
                dayMeta.overdue = !day.completed &&
                                  reading::compareDates(date, today) < 0;
                meta.emplace(day.dateIso, std::move(dayMeta));
            }
        }
    }

    dailyCalendarWidget_->setDayMeta(meta);
    updateDailyCalendarHeader();
}

void RightPane::updateDailyWorkspaceControls() {
    if (!dailyModeButton_ ||
        !dailyDevotionalChoice_ || !dailyReadingPlanChoice_ ||
        !dailyDateButton_ || !dailyCalendarGroup_ ||
        !dailyCompleteButton_ || !dailyCompleteThroughButton_ || !dailyRescheduleButton_ ||
        !dailyNewPlanButton_ || !dailyEditPlanButton_ || !dailyDeletePlanButton_ ||
        !dailyPlanEditorGroup_) {
        return;
    }

    const bool readingPlansMode =
        dailyWorkspaceState_.mode == DailyWorkspaceMode::ReadingPlans;
    const bool editingPlan =
        readingPlansMode && dailyWorkspaceState_.readingPlanEditMode;
    const std::string activeDateIso = currentDailyDateIso();
    bool selectedDaysCompleted = false;
    const std::vector<std::string> actionableSelectedDates =
        (readingPlansMode && !editingPlan)
            ? actionableSelectedReadingPlanDateIsos(&selectedDaysCompleted)
                         : std::vector<std::string>{};
    dailyModeButton_->copy_label(
        readingPlansMode ? "Back to Daily" : "Reading Plans...");
    if (dailyDevotionalChoice_) {
        module_choice::applyChoiceValue(dailyDevotionalChoice_,
                                        dailyDevotionalModules_,
                                        dailyDevotionalLabels_,
                                        dailyWorkspaceState_.devotionalModule);
    }
    if (dailyReadingPlanChoice_) {
        int selectedIndex = -1;
        if (dailyWorkspaceState_.readingPlanSource == DailyReadingPlanSource::SwordModule) {
            selectedIndex = readingPlanChoiceIndexForSwordModule(
                dailyReadingPlanChoices_, dailyWorkspaceState_.swordReadingPlanModule);
        } else {
            selectedIndex = readingPlanChoiceIndexForEditableId(
                dailyReadingPlanChoices_, dailyWorkspaceState_.readingPlanId);
        }
        dailyReadingPlanChoice_->value(selectedIndex);
    }

    if (readingPlansMode) {
        dailyReadingPlanChoice_->show();
        dailyDevotionalChoice_->hide();
        dailyNewPlanButton_->show();
        dailyEditPlanButton_->show();
        dailyDeletePlanButton_->show();
        if (editingPlan) {
            dailyCompleteButton_->hide();
            dailyCompleteThroughButton_->hide();
            dailyRescheduleButton_->hide();
        } else {
            dailyCompleteButton_->show();
            dailyCompleteThroughButton_->show();
            dailyRescheduleButton_->show();
        }
    } else {
        dailyDevotionalChoice_->show();
        dailyReadingPlanChoice_->hide();
        dailyNewPlanButton_->hide();
        dailyEditPlanButton_->hide();
        dailyDeletePlanButton_->hide();
        dailyCompleteButton_->hide();
        dailyCompleteThroughButton_->hide();
        dailyRescheduleButton_->hide();
    }

    if (reading::isIsoDateInRange(activeDateIso)) {
        reading::Date date{};
        reading::parseIsoDate(activeDateIso, date);
        dailyDateButton_->copy_label(reading::formatLongDate(date).c_str());
    } else {
        dailyDateButton_->copy_label("Select Date");
    }

    if (!editingPlan && (readingPlansMode || dailyWorkspaceState_.calendarVisible)) {
        dailyCalendarGroup_->show();
    } else {
        dailyCalendarGroup_->hide();
    }
    if (editingPlan) dailyPlanEditorGroup_->show();
    else dailyPlanEditorGroup_->hide();
    if (editingPlan) dailyHtml_->hide();
    else dailyHtml_->show();

    bool hasPlan = false;
    bool readOnlyPlan = false;
    bool hasDay = false;
    bool dayCompleted = false;
    bool throughCompleted = false;
    const std::vector<std::string> actionableThroughDates =
        (readingPlansMode && !editingPlan)
            ? actionableReadingPlanDatesThroughCurrent(&throughCompleted)
                         : std::vector<std::string>{};
    if (readingPlansMode &&
        dailyWorkspaceState_.readingPlanSource == DailyReadingPlanSource::SwordModule &&
        !dailyWorkspaceState_.swordReadingPlanModule.empty()) {
        hasPlan = true;
        readOnlyPlan = true;
        hasDay = swordReadingPlanHasContentForDate(
            dailyWorkspaceState_.swordReadingPlanModule, activeDateIso);
        dayCompleted = app_->readingPlanManager().swordDayCompleted(
            dailyWorkspaceState_.swordReadingPlanModule, activeDateIso);
    } else if (readingPlansMode && dailyWorkspaceState_.readingPlanId > 0 && app_) {
        ReadingPlan plan;
        if (app_->readingPlanManager().getPlan(dailyWorkspaceState_.readingPlanId, plan)) {
            hasPlan = true;
            if (const ReadingPlanDay* day =
                    readingPlanDayForDate(plan, activeDateIso)) {
                hasDay = true;
                dayCompleted = day->completed;
            }
        }
    }

    if (dailyEditPlanButton_) {
        dailyEditPlanButton_->copy_label(editingPlan ? "Editing" : "Edit");
        if (readingPlansMode && hasPlan && !readOnlyPlan && !editingPlan) {
            dailyEditPlanButton_->activate();
        } else {
            dailyEditPlanButton_->deactivate();
        }
    }
    if (dailyDeletePlanButton_) {
        if (readingPlansMode && hasPlan && !readOnlyPlan && !editingPlan) {
            dailyDeletePlanButton_->activate();
        }
        else dailyDeletePlanButton_->deactivate();
    }
    if (dailyReadingPlanChoice_) {
        if (editingPlan) dailyReadingPlanChoice_->deactivate();
        else dailyReadingPlanChoice_->activate();
    }
    if (dailyPrevDayButton_) {
        if (editingPlan) dailyPrevDayButton_->deactivate();
        else dailyPrevDayButton_->activate();
    }
    if (dailyDateButton_) {
        if (editingPlan) dailyDateButton_->deactivate();
        else dailyDateButton_->activate();
    }
    if (dailyTodayButton_) {
        if (editingPlan) dailyTodayButton_->deactivate();
        else dailyTodayButton_->activate();
    }
    if (dailyNextDayButton_) {
        if (editingPlan) dailyNextDayButton_->deactivate();
        else dailyNextDayButton_->activate();
    }
    if (dailyNewPlanButton_) {
        if (editingPlan) dailyNewPlanButton_->deactivate();
        else dailyNewPlanButton_->activate();
    }
    if (dailyCompleteButton_) {
        const bool useSelectionState = !actionableSelectedDates.empty();
        dailyCompleteButton_->copy_label(
            (useSelectionState ? selectedDaysCompleted : dayCompleted)
                ? "Mark Incomplete"
                : "Mark Complete");
        if (readingPlansMode && !editingPlan && (useSelectionState || hasDay)) {
            dailyCompleteButton_->activate();
        }
        else dailyCompleteButton_->deactivate();
    }
    if (dailyCompleteThroughButton_) {
        dailyCompleteThroughButton_->copy_label(
            throughCompleted ? "Unmark Previous" : "Mark Previous");
        if (readingPlansMode && !editingPlan && !actionableThroughDates.empty()) {
            dailyCompleteThroughButton_->activate();
        } else {
            dailyCompleteThroughButton_->deactivate();
        }
    }
    if (dailyRescheduleButton_) {
        if (readingPlansMode && !editingPlan && hasDay) {
            dailyRescheduleButton_->activate();
        }
        else dailyRescheduleButton_->deactivate();
    }

    updateDailyPlanEditorState();

    if (tabs_) {
        layoutTopTabContents(tabs_->x(), tabs_->y(), tabs_->w(), tabs_->h());
    }
}

std::string RightPane::dailyDevotionalHeadingLabel(const std::string& moduleName) const {
    if (moduleName.empty()) return "";

    int index = module_choice::findChoiceIndexByModuleName(dailyDevotionalModules_, moduleName);
    if (index >= 0 && index < static_cast<int>(dailyDevotionalLabels_.size())) {
        return dailyDevotionalLabels_[static_cast<size_t>(index)];
    }

    if (!app_) return moduleName;
    std::string description = module_choice::trimCopy(
        app_->swordManager().getModuleDescription(moduleName));
    if (description.empty() ||
        module_choice::equalsIgnoreCaseAscii(description, moduleName)) {
        return moduleName;
    }
    return moduleName + ": " + description;
}

int RightPane::selectedDailyPlanEditorIndex() const {
    if (!dailyPlanDayBrowser_) return -1;
    int line = dailyPlanDayBrowser_->value();
    if (line <= 0 || line > static_cast<int>(dailyPlanEditorWorkingPlan_.days.size())) {
        return -1;
    }
    return line - 1;
}

void RightPane::applyDailyPlanEditorSummaryFields() {
    if (!dailyPlanNameInput_ || !dailyPlanStartDateInput_ || !dailyPlanDescriptionInput_) return;
    dailyPlanEditorWorkingPlan_.summary.name = reading::trimCopy(
        dailyPlanNameInput_->value() ? dailyPlanNameInput_->value() : "");
    dailyPlanEditorWorkingPlan_.summary.startDateIso = reading::trimCopy(
        dailyPlanStartDateInput_->value() ? dailyPlanStartDateInput_->value() : "");
    dailyPlanEditorWorkingPlan_.summary.description = reading::trimCopy(
        dailyPlanDescriptionInput_->value() ? dailyPlanDescriptionInput_->value() : "");
}

void RightPane::updateDailyPlanEditorSummaryFields() {
    if (!dailyPlanNameInput_ || !dailyPlanStartDateInput_ || !dailyPlanDescriptionInput_) return;
    dailyPlanNameInput_->value(dailyPlanEditorWorkingPlan_.summary.name.c_str());
    dailyPlanStartDateInput_->value(dailyPlanEditorWorkingPlan_.summary.startDateIso.c_str());
    dailyPlanDescriptionInput_->value(dailyPlanEditorWorkingPlan_.summary.description.c_str());
}

void RightPane::applyDailyPlanEditorSelectionFields() {
    int index = selectedDailyPlanEditorIndex();
    if (index < 0 ||
        !dailyPlanDayDateInput_ || !dailyPlanDayTitleInput_ || !dailyPlanDayPassagesInput_) {
        return;
    }

    ReadingPlanDay& day = dailyPlanEditorWorkingPlan_.days[static_cast<size_t>(index)];
    day.dateIso = reading::trimCopy(
        dailyPlanDayDateInput_->value() ? dailyPlanDayDateInput_->value() : "");
    day.title = reading::trimCopy(
        dailyPlanDayTitleInput_->value() ? dailyPlanDayTitleInput_->value() : "");
    day.passages.clear();
    for (const auto& ref : reading::splitPlanLines(
             dailyPlanDayPassagesInput_->value() ? dailyPlanDayPassagesInput_->value() : "")) {
        day.passages.push_back(ReadingPlanPassage{0, ref});
    }
}

void RightPane::loadDailyPlanEditorSelection() {
    if (!dailyPlanDayDateInput_ || !dailyPlanDayTitleInput_ || !dailyPlanDayPassagesInput_) return;

    int index = selectedDailyPlanEditorIndex();
    if (index < 0) {
        dailyPlanDayDateInput_->value("");
        dailyPlanDayTitleInput_->value("");
        dailyPlanDayPassagesInput_->value("");
        updateDailyPlanEditorState();
        return;
    }

    const ReadingPlanDay& day = dailyPlanEditorWorkingPlan_.days[static_cast<size_t>(index)];
    dailyPlanDayDateInput_->value(day.dateIso.c_str());
    dailyPlanDayTitleInput_->value(day.title.c_str());
    dailyPlanDayPassagesInput_->value(reading::joinPlanPassages(day.passages, "\n").c_str());
    dailyWorkspaceState_.readingPlanSelectedDateIso = day.dateIso;
    updateDailyPlanEditorState();
}

void RightPane::rebuildDailyPlanDayBrowser() {
    if (!dailyPlanDayBrowser_) return;

    int selectedIndex = selectedDailyPlanEditorIndex();
    dailyPlanDayBrowser_->clear();
    for (const auto& day : dailyPlanEditorWorkingPlan_.days) {
        dailyPlanDayBrowser_->add(reading::formatReadingPlanDayLabel(day).c_str());
    }

    if (!dailyPlanEditorWorkingPlan_.days.empty()) {
        if (selectedIndex < 0 ||
            selectedIndex >= static_cast<int>(dailyPlanEditorWorkingPlan_.days.size())) {
            selectedIndex = std::min<int>(
                static_cast<int>(dailyPlanEditorWorkingPlan_.days.size()) - 1, 0);
        }
        dailyPlanDayBrowser_->select(selectedIndex + 1);
    }

    loadDailyPlanEditorSelection();
}

void RightPane::updateDailyPlanEditorState() {
    const bool hasSelection = selectedDailyPlanEditorIndex() >= 0;
    if (dailyPlanDuplicateDayButton_) {
        hasSelection ? dailyPlanDuplicateDayButton_->activate()
                     : dailyPlanDuplicateDayButton_->deactivate();
    }
    if (dailyPlanRemoveDayButton_) {
        hasSelection ? dailyPlanRemoveDayButton_->activate()
                     : dailyPlanRemoveDayButton_->deactivate();
    }
    if (dailyPlanDayDateInput_) {
        if (hasSelection) dailyPlanDayDateInput_->activate();
        else dailyPlanDayDateInput_->deactivate();
    }
    if (dailyPlanDayTitleInput_) {
        if (hasSelection) dailyPlanDayTitleInput_->activate();
        else dailyPlanDayTitleInput_->deactivate();
    }
    if (dailyPlanDayPassagesInput_) {
        if (hasSelection) dailyPlanDayPassagesInput_->activate();
        else dailyPlanDayPassagesInput_->deactivate();
    }
}

bool RightPane::validateDailyPlanEditorPlan(ReadingPlan& out,
                                            std::string& errorMessage) {
    applyDailyPlanEditorSummaryFields();
    applyDailyPlanEditorSelectionFields();

    ReadingPlan updated = dailyPlanEditorWorkingPlan_;
    if (updated.summary.name.empty()) {
        errorMessage = "Enter a plan name.";
        return false;
    }
    if (!reading::isIsoDateInRange(updated.summary.startDateIso)) {
        errorMessage = "Enter a valid start date in YYYY-MM-DD format.";
        return false;
    }

    reading::normalizeReadingPlanDays(updated.days);
    if (updated.days.empty()) {
        errorMessage = "Add at least one reading day.";
        return false;
    }

    for (const auto& day : updated.days) {
        if (!reading::isIsoDateInRange(day.dateIso)) {
            errorMessage = "Each reading day needs a valid date.";
            return false;
        }
        if (day.passages.empty()) {
            errorMessage = "Each reading day needs at least one passage.";
            return false;
        }
    }

    updated.summary.totalDays = static_cast<int>(updated.days.size());
    updated.summary.completedDays = static_cast<int>(
        std::count_if(updated.days.begin(), updated.days.end(),
                      [](const ReadingPlanDay& day) { return day.completed; }));
    out = std::move(updated);
    return true;
}

bool RightPane::maybeDiscardDailyPlanEditorChanges() {
    if (!dailyWorkspaceState_.readingPlanEditMode || !dailyPlanEditorDirty_) return true;
    int choice = fl_choice("Discard the unsaved reading-plan changes?",
                           "Cancel",
                           "Discard",
                           nullptr);
    return choice == 1;
}

void RightPane::loadDailyPlanEditor() {
    if (!app_ || dailyWorkspaceState_.readingPlanId <= 0) return;

    ReadingPlan plan;
    if (!app_->readingPlanManager().getPlan(dailyWorkspaceState_.readingPlanId, plan)) {
        fl_alert("Failed to load the selected reading plan.");
        dailyWorkspaceState_.readingPlanEditMode = false;
        dailyPlanEditorDirty_ = false;
        return;
    }

    dailyPlanEditorWorkingPlan_ = std::move(plan);
    reading::normalizeReadingPlanDays(dailyPlanEditorWorkingPlan_.days);
    updateDailyPlanEditorSummaryFields();
    dailyPlanEditorDirty_ = false;

    int targetIndex = 0;
    for (size_t i = 0; i < dailyPlanEditorWorkingPlan_.days.size(); ++i) {
        if (dailyPlanEditorWorkingPlan_.days[i].dateIso ==
            dailyWorkspaceState_.readingPlanSelectedDateIso) {
            targetIndex = static_cast<int>(i);
            break;
        }
    }

    rebuildDailyPlanDayBrowser();
    if (dailyPlanDayBrowser_ && !dailyPlanEditorWorkingPlan_.days.empty()) {
        dailyPlanDayBrowser_->select(targetIndex + 1);
        loadDailyPlanEditorSelection();
    }
}

void RightPane::enterDailyPlanEditMode() {
    if (!app_ ||
        dailyWorkspaceState_.readingPlanSource != DailyReadingPlanSource::Editable ||
        dailyWorkspaceState_.readingPlanId <= 0) {
        return;
    }

    dailyWorkspaceState_.readingPlanEditMode = true;
    loadDailyPlanEditor();
    refreshDailyWorkspace(true);
}

void RightPane::exitDailyPlanEditMode(bool discardChanges) {
    if (!discardChanges && !maybeDiscardDailyPlanEditorChanges()) {
        return;
    }

    dailyWorkspaceState_.readingPlanEditMode = false;
    dailyPlanEditorWorkingPlan_ = ReadingPlan{};
    dailyPlanEditorDirty_ = false;
    refreshDailyWorkspace(true);
}

std::string RightPane::selectedDailyReadingPlanSummaryHtml(const std::string& dateIso,
                                                           bool includePlanLabel) const {
    if (!app_) return "";

    std::string planLabel = selectedDailyReadingPlanLabel();
    std::vector<std::string> itemsHtml;

    if (dailyWorkspaceState_.readingPlanSource == DailyReadingPlanSource::SwordModule) {
        const std::string moduleName = dailyWorkspaceState_.swordReadingPlanModule;
        if (moduleName.empty()) return "";

        const SwordReadingPlanTemplateDay* templateDay =
            swordReadingPlanTemplateDayForDate(moduleName, dateIso);
        if (!templateDay) return "";

        std::string entryHtml =
            app_->swordManager().getDailyDevotionEntry(moduleName, templateDay->moduleKey);
        std::string rewrittenBody = extractDailyEntryBodyHtml(
            rewriteSwordReadingPlanLinks(entryHtml, nullptr));
        itemsHtml = extractSwordReadingPlanItemHtml(rewrittenBody);
    } else if (dailyWorkspaceState_.readingPlanId > 0) {
        ReadingPlan plan;
        if (!app_->readingPlanManager().getPlan(dailyWorkspaceState_.readingPlanId, plan)) {
            return "";
        }

        planLabel = plan.summary.name;
        const ReadingPlanDay* day = readingPlanDayForDate(plan, dateIso);
        if (day) {
            std::vector<std::string> passageLinks;
            passageLinks.reserve(day->passages.size());
            for (const auto& passage : day->passages) {
                std::string linkHtml = buildReadingPlanPassageLinkHtml(passage.reference);
                if (!linkHtml.empty()) passageLinks.push_back(std::move(linkHtml));
            }

            std::string joinedLinks = joinHtmlItems(
                passageLinks,
                "<span class=\"daily-reading-summary-separator\">, </span>");
            if (!module_choice::trimCopy(day->title).empty()) {
                std::ostringstream item;
                item << "<span class=\"daily-reading-summary-item-label\">"
                     << htmlEscape(day->title)
                     << "</span>";
                if (!joinedLinks.empty()) {
                    item << " " << joinedLinks;
                }
                itemsHtml.push_back(item.str());
            } else if (!joinedLinks.empty()) {
                itemsHtml.push_back(joinedLinks);
            }
        }
    }

    if (planLabel.empty() && itemsHtml.empty()) return "";

    std::ostringstream html;
    html << "<div class=\"daily-reading-summary\">";
    if (includePlanLabel && !planLabel.empty()) {
        html << "<span class=\"daily-reading-summary-plan\">"
             << htmlEscape(planLabel)
             << "</span>";
        html << "<span class=\"daily-reading-summary-separator\">: </span>";
    }
    if (!itemsHtml.empty()) {
        html << joinHtmlItems(itemsHtml,
                              "<span class=\"daily-reading-summary-separator\"> | </span>");
    } else {
        html << "<span class=\"daily-reading-summary-empty\">No readings scheduled.</span>";
    }
    html << "</div>";
    return html.str();
}

void RightPane::showDailyDevotionEntry(const std::string& moduleName,
                                       const std::string& dateIso) {
    if (!dailyHtml_ || !app_) return;

    if (moduleName.empty()) {
        dailyHtml_->setHtml("<p><i>No daily devotion modules installed.</i></p>");
        return;
    }

    std::string entryHtml = app_->swordManager().getDailyDevotionEntry(moduleName, dateIso);
    dailyHtml_->setHtml(buildDailyDevotionPageHtml(
        dateIso,
        "",
        htmlEscape(dailyDevotionalHeadingLabel(moduleName)),
        entryHtml));
}

void RightPane::showReadingPlanDay(int planId, const std::string& dateIso) {
    if (!dailyHtml_ || !app_) return;

    if (planId <= 0) {
        dailyHtml_->setHtml(
            "<p><i>No reading plans yet. Use New to create your first plan.</i></p>");
        return;
    }

    ReadingPlan plan;
    if (!app_->readingPlanManager().getPlan(planId, plan)) {
        dailyHtml_->setHtml(
            "<p><i>The selected reading plan could not be loaded.</i></p>");
        return;
    }

    dailyHtml_->setHtml(buildReadingPlanDayHtml(plan, dateIso));
}

void RightPane::showSwordReadingPlanDay(const std::string& moduleName,
                                        const std::string& dateIso) {
    if (!dailyHtml_ || !app_) return;

    if (moduleName.empty()) {
        dailyHtml_->setHtml(
            "<p><i>No SWORD reading-plan modules are installed.</i></p>");
        return;
    }

    const SwordReadingPlanTemplateDay* templateDay =
        swordReadingPlanTemplateDayForDate(moduleName, dateIso);
    if (!templateDay) {
        dailyHtml_->setHtml(buildSwordReadingPlanDayHtml(moduleName,
                                                         app_->swordManager().getModuleDescription(
                                                             moduleName),
                                                         dateIso,
                                                         ""));
        return;
    }

    std::string entryHtml =
        app_->swordManager().getDailyDevotionEntry(moduleName, templateDay->moduleKey);
    std::string rewrittenEntryHtml =
        rewriteSwordReadingPlanLinks(entryHtml, nullptr);

    std::string description = app_->swordManager().getModuleDescription(moduleName);
    dailyHtml_->setHtml(buildSwordReadingPlanDayHtml(moduleName,
                                                     description,
                                                     dateIso,
                                                     rewrittenEntryHtml));
}

void RightPane::refreshDailyWorkspace(bool /*forceCalendarReload*/) {
    if (!app_ || !dailyHtml_ || !dailyCalendarWidget_) return;

    if (dailyWorkspaceState_.mode == DailyWorkspaceMode::ReadingPlans) {
        if (dailyWorkspaceState_.readingPlanSource == DailyReadingPlanSource::SwordModule) {
            dailyWorkspaceState_.readingPlanEditMode = false;
            if (dailyWorkspaceState_.swordReadingPlanModule.empty() &&
                !dailyReadingPlanChoices_.empty()) {
                for (const auto& item : dailyReadingPlanChoices_) {
                    if (item.kind == DailyReadingPlanChoiceItem::Kind::SwordModule) {
                        dailyWorkspaceState_.swordReadingPlanModule = item.moduleName;
                        break;
                    }
                }
            }
        } else if (dailyWorkspaceState_.readingPlanId <= 0 && !dailyReadingPlanChoices_.empty()) {
            for (const auto& item : dailyReadingPlanChoices_) {
                if (item.kind == DailyReadingPlanChoiceItem::Kind::EditablePlan) {
                    dailyWorkspaceState_.readingPlanId = item.planId;
                    break;
                }
            }
        } else if (dailyWorkspaceState_.readingPlanId <= 0) {
            dailyWorkspaceState_.readingPlanEditMode = false;
        }
    } else {
        dailyWorkspaceState_.readingPlanEditMode = false;
    }

    ensureDailyWorkspaceDates();

    reading::Date selectedDate{};
    reading::parseIsoDate(currentDailyDateIso(), selectedDate);
    if (dailyWorkspaceState_.mode == DailyWorkspaceMode::ReadingPlans) {
        if (!dailyCalendarWidget_->hasSelectedDateIso(currentDailyDateIso())) {
            dailyCalendarWidget_->setSelectedDate(selectedDate);
            dailyCalendarWidget_->setSelectedDates({currentDailyDateIso()});
        }
    } else {
        if (!reading::sameDate(dailyCalendarWidget_->selectedDate(), selectedDate)) {
            dailyCalendarWidget_->setSelectedDate(selectedDate);
        }
        dailyCalendarWidget_->setSelectedDates({dailyWorkspaceState_.selectedDateIso});
    }
    const bool calendarShouldTrackSelection =
        dailyWorkspaceState_.mode == DailyWorkspaceMode::ReadingPlans ||
        dailyWorkspaceState_.calendarVisible;
    if (calendarShouldTrackSelection) {
        reading::Date displayedMonth = dailyCalendarWidget_->displayedMonth();
        if (!displayedMonth.valid() ||
            displayedMonth.year != selectedDate.year ||
            displayedMonth.month != selectedDate.month) {
            dailyCalendarWidget_->setDisplayedMonth(selectedDate);
        }
    }

    if (dailyWorkspaceState_.mode == DailyWorkspaceMode::Devotionals) {
        if (dailyWorkspaceState_.devotionalModule.empty() &&
            !dailyDevotionalModules_.empty()) {
            dailyWorkspaceState_.devotionalModule = dailyDevotionalModules_.front();
        }
        showDailyDevotionEntry(dailyWorkspaceState_.devotionalModule,
                               dailyWorkspaceState_.selectedDateIso);
    } else {
        if (dailyWorkspaceState_.readingPlanEditMode &&
            dailyWorkspaceState_.readingPlanSource == DailyReadingPlanSource::Editable) {
            if (dailyPlanEditorWorkingPlan_.summary.id != dailyWorkspaceState_.readingPlanId) {
                loadDailyPlanEditor();
            }
        } else if (dailyWorkspaceState_.readingPlanSource == DailyReadingPlanSource::SwordModule) {
            showSwordReadingPlanDay(dailyWorkspaceState_.swordReadingPlanModule,
                                    dailyWorkspaceState_.readingPlanSelectedDateIso);
        } else {
            showReadingPlanDay(dailyWorkspaceState_.readingPlanId,
                               dailyWorkspaceState_.readingPlanSelectedDateIso);
        }
    }

    updateDailyCalendarMeta();
    updateDailyWorkspaceControls();
    if (app_->mainWindow() && app_->mainWindow()->biblePane()) {
        app_->mainWindow()->biblePane()->refreshDailyReadingPlanBar();
    }
}

void RightPane::openReadingPlanPassage(const std::string& reference) {
    if (!app_ || !app_->mainWindow()) return;
    std::string ref = firstReadingListItem(reference);
    if (ref.empty()) return;
    app_->mainWindow()->navigateTo(ref);
}

void RightPane::onDailyContentLink(const std::string& url) {
    if (!app_ || !app_->mainWindow()) return;

    if (url.rfind("verdad-plan://open", 0) == 0) {
        openReadingPlanPassage(extractQueryValue(url, "ref"));
        return;
    }

    std::string sourceModule =
        (dailyWorkspaceState_.mode == DailyWorkspaceMode::Devotionals)
            ? dailyWorkspaceState_.devotionalModule
            : (dailyWorkspaceState_.readingPlanSource == DailyReadingPlanSource::SwordModule
                   ? dailyWorkspaceState_.swordReadingPlanModule
                   : std::string());
    std::string sourceKey = currentDailyDateIso();
    std::string previewModule;
    if (BiblePane* biblePane = app_->mainWindow()->biblePane()) {
        previewModule = biblePane->currentModule();
    }

    std::vector<std::string> refs = app_->swordManager().verseReferencesFromLink(
        url, sourceKey, previewModule);
    if (!refs.empty() && app_->mainWindow()->leftPane()) {
        app_->mainWindow()->leftPane()->showReferenceResults(
            previewModule, refs, "(linked verses)");
        return;
    }

    std::string previewHtml = app_->swordManager().buildLinkPreviewHtml(
        sourceModule, sourceKey, url, previewModule);
    if (!previewHtml.empty() && app_->mainWindow()->leftPane()) {
        app_->mainWindow()->leftPane()->setPreviewText(
            previewHtml, sourceModule, sourceKey);
    }
}

void RightPane::updateCommentaryEditorChrome() {
    bool writable = !currentCommentary_.empty() &&
                    !currentCommentaryRef_.empty() &&
                    app_ &&
                    app_->swordManager().moduleIsWritable(currentCommentary_);

    if (!writable && commentaryEditing_) {
        commentaryEditing_ = false;
        commentaryEditModule_.clear();
        commentaryEditReference_.clear();
    }

    if (commentaryEditButton_) {
        if (writable && !commentaryEditing_) commentaryEditButton_->activate();
        else commentaryEditButton_->deactivate();
    }
    if (commentarySaveButton_) {
        if (commentaryEditing_ && writable) commentarySaveButton_->activate();
        else commentarySaveButton_->deactivate();
    }
    if (commentaryCancelButton_) {
        if (commentaryEditing_) commentaryCancelButton_->activate();
        else commentaryCancelButton_->deactivate();
    }

    if (commentaryHtml_ && commentaryEditor_) {
        if (commentaryEditing_) {
            commentaryHtml_->hide();
            commentaryEditor_->show();
        } else {
            commentaryEditor_->hide();
            commentaryHtml_->show();
        }
    }
}

void RightPane::updateDocumentChrome() {
    if (documentChoice_) {
        std::string tooltip;
        if (currentDocumentPath_.empty()) {
            tooltip = "Studypads in " + studypadDirectory(app_);
        } else {
            tooltip = currentDocumentPath_;
        }
        if (documentsEditor_ && documentsEditor_->isModified()) {
            tooltip += "\nModified";
        }
        documentChoice_->copy_tooltip(tooltip.c_str());
    }

    bool hasContent = documentsEditor_ &&
                      (documentsEditor_->isModified() ||
                       !documentsEditor_->html().empty() ||
                       !currentDocumentPath_.empty());
    bool canDelete = false;
    if (!currentDocumentPath_.empty() && isManagedStudypadPath(currentDocumentPath_)) {
        std::error_code ec;
        canDelete = fs::exists(fs::path(currentDocumentPath_), ec) && !ec;
    }
    if (documentSaveButton_) {
        if (documentsEditor_ && documentsEditor_->isModified()) {
            documentSaveButton_->activate();
        } else {
            documentSaveButton_->deactivate();
        }
    }
    if (documentNewButton_) documentNewButton_->activate();
    if (documentExportButton_) {
        if (hasContent) documentExportButton_->activate();
        else documentExportButton_->deactivate();
    }
    if (documentDeleteButton_) {
        if (canDelete) documentDeleteButton_->activate();
        else documentDeleteButton_->deactivate();
    }
}

void RightPane::refreshDocumentChoices() {
    if (!documentChoice_) return;

    const std::string directory = studypadDirectory(app_);
    if (!directory.empty()) {
        ensureDirectoryExists(directory);
    }

    struct DocumentChoiceEntry {
        std::string label;
        std::string path;
    };

    std::vector<DocumentChoiceEntry> fileEntries;
    if (!directory.empty()) {
        std::error_code ec;
        fs::directory_iterator it(fs::path(directory), ec);
        fs::directory_iterator end;
        while (!ec && it != end) {
            if (isStudypadFilename(it->path())) {
                fileEntries.push_back({
                    studypadDisplayName(it->path().string()),
                    normalizePath(it->path().string()),
                });
            }
            it.increment(ec);
        }
    }

    std::sort(fileEntries.begin(), fileEntries.end(),
              [](const DocumentChoiceEntry& a, const DocumentChoiceEntry& b) {
                  std::string left = toLowerAscii(a.label);
                  std::string right = toLowerAscii(b.label);
                  if (left != right) return left < right;
                  return a.label < b.label;
              });

    std::vector<DocumentChoiceEntry> entries;
    if (currentDocumentPath_.empty()) {
        entries.push_back({"Untitled", ""});
    }
    entries.insert(entries.end(), fileEntries.begin(), fileEntries.end());

    bool foundCurrent = currentDocumentPath_.empty();
    for (const auto& entry : fileEntries) {
        if (pathsEqual(entry.path, currentDocumentPath_)) {
            foundCurrent = true;
            break;
        }
    }
    if (!foundCurrent && !currentDocumentPath_.empty()) {
        entries.push_back({
            studypadDisplayName(currentDocumentPath_),
            normalizePath(currentDocumentPath_),
        });
    }
    if (entries.empty()) {
        entries.push_back({"Untitled", ""});
    }

    int selectedIndex = 0;
    if (!currentDocumentPath_.empty()) {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (pathsEqual(entries[i].path, currentDocumentPath_)) {
                selectedIndex = static_cast<int>(i);
                break;
            }
        }
    }

    documentChoiceSyncing_ = true;
    documentChoice_->clear();
    documentChoicePaths_.clear();
    for (const auto& entry : entries) {
        documentChoice_->add(entry.label.c_str());
        documentChoicePaths_.push_back(entry.path);
    }
    if (!documentChoicePaths_.empty()) {
        documentChoice_->value(std::clamp(selectedIndex,
                                          0,
                                          static_cast<int>(documentChoicePaths_.size()) - 1));
    }
    documentChoiceSyncing_ = false;
}

bool RightPane::isManagedStudypadPath(const std::string& path) const {
    if (path.empty()) return false;

    fs::path filePath(normalizePath(path));
    if (!endsWithIgnoreCase(filePath.filename().string(), ".studypad")) {
        return false;
    }

    return pathsEqual(filePath.parent_path().string(), studypadDirectory(app_));
}

void RightPane::loadCommentaryEditorForCurrentEntry() {
    if (!commentaryEditor_) return;

    commentaryEditModule_ = currentCommentary_;
    commentaryEditReference_ = currentCommentaryRef_;
    std::string rawHtml;
    if (app_ && !commentaryEditModule_.empty() && !commentaryEditReference_.empty()) {
        rawHtml = app_->swordManager().getRawEntry(commentaryEditModule_,
                                                   commentaryEditReference_);
    }
    commentaryEditor_->setHtml(rawHtml);
    commentaryEditor_->setModified(false);
    commentaryEditor_->focusEditor();
    updateCommentaryEditorChrome();
}

bool RightPane::beginCommentaryEdit() {
    if (!app_ || currentCommentary_.empty() || currentCommentaryRef_.empty()) {
        return false;
    }
    if (!app_->swordManager().moduleIsWritable(currentCommentary_)) {
        return false;
    }

    commentaryEditing_ = true;
    activeTopTab_ = TopTab::Commentary;
    secondaryTabIsGeneralBooks_ = false;
    dailyWorkspaceState_.tabActive = false;
    if (tabs_) tabs_->value(commentaryGroup_);
    loadCommentaryEditorForCurrentEntry();
    return true;
}

bool RightPane::saveCommentaryEdit(bool exitEditMode) {
    if (!commentaryEditor_ || !app_) return false;

    std::string module = !commentaryEditModule_.empty()
                             ? commentaryEditModule_
                             : currentCommentary_;
    std::string reference = !commentaryEditReference_.empty()
                                ? commentaryEditReference_
                                : currentCommentaryRef_;
    if (module.empty() || reference.empty()) return true;

    if (!app_->swordManager().setRawEntry(module, reference, commentaryEditor_->html())) {
        fl_alert("Failed to save commentary note for %s.",
                 reference.c_str());
        return false;
    }

    commentaryEditor_->setModified(false);
    invalidateCommentaryCache(module, reference);
    if (app_ && app_->searchIndexer()) {
        app_->searchIndexer()->queueModuleIndex(module, true);
    }

    if (exitEditMode) {
        commentaryEditing_ = false;
        commentaryEditModule_.clear();
        commentaryEditReference_.clear();
        if (!currentCommentary_.empty() && !currentCommentaryRef_.empty()) {
            showCommentary(currentCommentary_, currentCommentaryRef_);
        }
    }

    updateCommentaryEditorChrome();
    return true;
}

void RightPane::cancelCommentaryEdit() {
    if (!commentaryEditing_) return;
    commentaryEditing_ = false;
    commentaryEditModule_.clear();
    commentaryEditReference_.clear();
    if (commentaryEditor_) commentaryEditor_->setModified(false);
    if (!currentCommentary_.empty() && !currentCommentaryRef_.empty()) {
        showCommentary(currentCommentary_, currentCommentaryRef_);
    } else {
        updateCommentaryEditorChrome();
    }
}

bool RightPane::maybeSaveDocumentChanges() {
    if (!documentsEditor_ || !documentsEditor_->isModified()) return true;

    int choice = fl_choice("Save changes to the current studypad?",
                           "Cancel",
                           "Discard",
                           "Save");
    if (choice == 0) return false;
    if (choice == 2) return saveDocument();
    return true;
}

bool RightPane::saveDocumentToPath(const std::string& path) {
    if (!documentsEditor_ || path.empty()) return false;

    std::string normalizedPath = normalizePath(path);
    fs::path targetPath(normalizedPath);
    if (!targetPath.parent_path().empty() &&
        !ensureDirectoryExists(targetPath.parent_path().string())) {
        fl_alert("Failed to prepare the studypad directory:\n%s",
                 targetPath.parent_path().string().c_str());
        return false;
    }

    std::ofstream out(normalizedPath);
    if (!out.is_open()) {
        fl_alert("Failed to open studypad for writing:\n%s", normalizedPath.c_str());
        return false;
    }

    out << documentsEditor_->html();
    out.close();
    if (!out) {
        fl_alert("Failed to save studypad:\n%s", normalizedPath.c_str());
        return false;
    }

    currentDocumentPath_ = normalizedPath;
    documentsEditor_->setModified(false);
    refreshDocumentChoices();
    updateDocumentChrome();
    return true;
}

bool RightPane::saveDocumentAs() {
    const std::string directory = studypadDirectory(app_);
    if (!ensureDirectoryExists(directory)) {
        fl_alert("Failed to create the studypad directory:\n%s", directory.c_str());
        return false;
    }

    std::string suggestedName =
        currentDocumentPath_.empty() ? "Untitled" : studypadDisplayName(currentDocumentPath_);
    const char* entered = fl_input("Studypad name:", suggestedName.c_str());
    if (!entered) return false;

    std::string name = sanitizeStudypadName(entered);
    if (name.empty()) {
        fl_alert("Enter a valid studypad name.");
        return false;
    }

    std::string path = normalizePath((fs::path(directory) / (name + ".studypad")).string());
    if (fs::exists(fs::path(path)) && !pathsEqual(path, currentDocumentPath_)) {
        int overwrite = fl_choice("A studypad named \"%s\" already exists.",
                                  "Cancel",
                                  "Overwrite",
                                  nullptr,
                                  name.c_str());
        if (overwrite != 1) return false;
    }
    return saveDocumentToPath(path);
}

bool RightPane::deleteCurrentDocument() {
    if (!documentsEditor_ || currentDocumentPath_.empty() ||
        !isManagedStudypadPath(currentDocumentPath_)) {
        return false;
    }

    std::string normalizedPath = normalizePath(currentDocumentPath_);
    std::string name = studypadDisplayName(normalizedPath);
    std::error_code existsError;
    bool exists = fs::exists(fs::path(normalizedPath), existsError) && !existsError;
    if (!exists) {
        fl_alert("The current studypad file is no longer available:\n%s",
                 normalizedPath.c_str());
        updateDocumentChrome();
        return false;
    }

    std::string prompt = "Delete studypad \"%s\"?\n\n"
                         "This permanently removes the file.";
    if (documentsEditor_->isModified()) {
        prompt += "\nUnsaved edits in the editor will also be lost.";
    }

    int confirm = fl_choice(prompt.c_str(), "Cancel", "Delete", nullptr, name.c_str());
    if (confirm != 1) return false;

    std::error_code removeError;
    bool removed = fs::remove(fs::path(normalizedPath), removeError);
    if (removeError || !removed) {
        fl_alert("Failed to delete studypad:\n%s", normalizedPath.c_str());
        return false;
    }

    currentDocumentPath_.clear();
    documentsEditor_->clearDocument();
    documentsEditor_->setModified(false);
    refreshDocumentChoices();
    updateDocumentChrome();
    setDocumentsTabActive(true);
    documentsEditor_->focusEditor();
    if (app_ && app_->mainWindow()) {
        app_->mainWindow()->showTransientStatus("Deleted " + name, 2.8);
    }
    return true;
}

bool RightPane::exportDocumentToOdtPath(const std::string& path) {
    if (!documentsEditor_ || path.empty()) return false;

    std::string tempDir = makeUniqueTempDir("verdad-odt-export-");
    if (tempDir.empty()) {
        fl_alert("Failed to create a temporary export directory.");
        return false;
    }

    auto cleanup = [&]() {
        std::error_code ec;
        fs::remove_all(tempDir, ec);
    };

    fs::path exportDir(tempDir);
    fs::path inputPath = exportDir / fs::path(path).filename();
    inputPath.replace_extension(".html");
    fs::path outputPath = inputPath;
    outputPath.replace_extension(".odt");
    fs::path profileDir = exportDir / "lo-profile";

    std::error_code fsError;
    fs::create_directory(profileDir, fsError);
    if (fsError) {
        cleanup();
        fl_alert("Failed to prepare the LibreOffice export profile.");
        return false;
    }

    if (!writeTextFile(inputPath.string(),
                       buildStudypadOdtHtml(app_, documentsEditor_->odtHtml()))) {
        cleanup();
        fl_alert("Failed to write the temporary HTML export.");
        return false;
    }

    if (!runLibreOfficeOdtExport(inputPath.string(),
                                 exportDir.string(),
                                 profileDir.string()) ||
        !fs::exists(outputPath)) {
        cleanup();
        fl_alert("LibreOffice could not export this studypad to ODT.\n"
                 "Make sure libreoffice or soffice is installed and available in PATH.");
        return false;
    }

    if (!copyBinaryFile(outputPath.string(), path)) {
        cleanup();
        fl_alert("Failed to write the ODT file:\n%s", path.c_str());
        return false;
    }

    cleanup();
    if (app_ && app_->mainWindow()) {
        app_->mainWindow()->showTransientStatus("Exported " + pathLeaf(path), 2.8);
    }
    return true;
}

bool RightPane::exportDocumentToOdt() {
    if (!documentsEditor_) return false;

    Fl_Native_File_Chooser chooser;
    chooser.title("Export Studypad to ODT");
    chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM |
                    Fl_Native_File_Chooser::USE_FILTER_EXT);
    chooser.filter("ODT Files\t*.{odt,ODT}\nAll Files\t*");

    if (!currentDocumentPath_.empty()) {
        std::string suggested = pathWithExtension(currentDocumentPath_, ".odt");
        chooser.preset_file(suggested.c_str());
    } else {
        chooser.preset_file("studypad.odt");
    }

    int result = chooser.show();
    if (result != 0) {
        if (result < 0) {
            fl_alert("Unable to open the file chooser.");
        }
        return false;
    }

    std::string path = chooser.filename() ? chooser.filename() : "";
    if (path.empty()) return false;
    if (!endsWithIgnoreCase(path, ".odt")) {
        path = pathWithExtension(path, ".odt");
    }
    return exportDocumentToOdtPath(path);
}

bool RightPane::newDocument() {
    if (!maybeSaveDocumentChanges() || !documentsEditor_) return false;
    currentDocumentPath_.clear();
    documentsEditor_->clearDocument();
    documentsEditor_->setModified(false);
    refreshDocumentChoices();
    updateDocumentChrome();
    setDocumentsTabActive(true);
    documentsEditor_->focusEditor();
    return true;
}

bool RightPane::openDocument(const std::string& path, bool activateTab) {
    if (path.empty() || !documentsEditor_) return false;
    if (!maybeSaveDocumentChanges()) return false;

    std::string normalizedPath = normalizePath(path);
    std::ifstream in(normalizedPath);
    if (!in.is_open()) {
        fl_alert("Failed to open studypad:\n%s", normalizedPath.c_str());
        return false;
    }

    std::string html((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    documentsEditor_->setHtml(html);
    documentsEditor_->setModified(false);
    currentDocumentPath_ = normalizedPath;
    refreshDocumentChoices();
    updateDocumentChrome();

    if (activateTab) {
        setDocumentsTabActive(true);
        documentsEditor_->focusEditor();
    }
    return true;
}

bool RightPane::saveDocument() {
    if (!documentsEditor_) return false;
    if (currentDocumentPath_.empty() || !isManagedStudypadPath(currentDocumentPath_)) {
        return saveDocumentAs();
    }
    return saveDocumentToPath(currentDocumentPath_);
}

void RightPane::onHtmlLink(const std::string& url, bool commentarySource) {
    if (!app_ || !app_->mainWindow()) return;

    HtmlWidget* sourceWidget = commentarySource ? commentaryHtml_ : generalBookHtml_;
    std::string sourceModule = commentarySource ? currentCommentary_ : currentGeneralBook_;
    std::string sourceKey = commentarySource ? currentCommentaryRef_ : currentGeneralBookKey_;
    BiblePane* biblePane = app_->mainWindow()->biblePane();

    if (!url.empty() && url[0] == '#') {
        if (sourceWidget) sourceWidget->scrollToAnchor(url.substr(1));
        return;
    }

    if (url.rfind("strongs:", 0) == 0 || url.rfind("strong:", 0) == 0) {
        app_->mainWindow()->showWordInfoNow("", url, "", "");
        return;
    }
    if (url.rfind("morph:", 0) == 0) {
        app_->mainWindow()->showWordInfoNow("", url, "", "");
        return;
    }

    std::string previewModule;
    if (biblePane) {
        previewModule = biblePane->currentModule();
    }

    if (commentarySource && url.rfind("bible-verse:", 0) == 0) {
        try {
            int verse = std::stoi(url.substr(12));
            SwordManager::VerseRef ref = SwordManager::parseVerseRef(currentCommentaryRef_);
            if (!ref.book.empty() && ref.chapter > 0 && verse > 0) {
                if (biblePane &&
                    biblePane->currentBook() == ref.book &&
                    biblePane->currentChapter() == ref.chapter) {
                    biblePane->selectVerse(verse);
                } else {
                    std::ostringstream target;
                    target << ref.book << " " << ref.chapter << ":" << verse;
                    app_->mainWindow()->navigateTo(target.str());
                }
                return;
            }
        } catch (...) {
        }
    }

    std::vector<std::string> refs;
    if (commentarySource && url.rfind("verse:", 0) == 0) {
        try {
            int verse = std::stoi(url.substr(6));
            SwordManager::VerseRef ref = SwordManager::parseVerseRef(currentCommentaryRef_);
            if (!ref.book.empty() && ref.chapter > 0 && verse > 0) {
                std::ostringstream target;
                target << ref.book << " " << ref.chapter << ":" << verse;
                refs.push_back(target.str());
            }
        } catch (...) {
        }
    } else {
        refs = app_->swordManager().verseReferencesFromLink(
            url, sourceKey, previewModule);
    }

    if (!refs.empty() && app_->mainWindow()->leftPane()) {
        app_->mainWindow()->leftPane()->showReferenceResults(
            previewModule, refs, "(linked verses)");
        return;
    }

    std::string previewHtml = app_->swordManager().buildLinkPreviewHtml(
        sourceModule, sourceKey, url, previewModule);
    if (!previewHtml.empty() &&
        app_->mainWindow()->leftPane()) {
        app_->mainWindow()->leftPane()->setPreviewText(
            previewHtml, sourceModule, sourceKey);
        return;
    }
}

void RightPane::onCommentaryModuleChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->commentaryChoice_) return;
    std::string module = module_choice::selectedModuleName(
        self->commentaryChoice_, self->commentaryChoiceModules_);
    if (!module.empty()) self->setCommentaryModule(module, true);
}


void RightPane::onCommentaryEdit(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    self->beginCommentaryEdit();
}

void RightPane::onCommentarySave(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    self->saveCommentaryEdit(true);
}

void RightPane::onCommentaryCancel(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    self->cancelCommentaryEdit();
}

void RightPane::onDictionaryModuleChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->dictionaryChoice_) return;
    self->currentDictionary_ = module_choice::selectedModuleName(
        self->dictionaryChoice_, self->dictionaryChoiceModules_);
    self->populateDictionaryKeyChoices();
    const bool isDailyDevotion =
        self->app_ &&
        self->app_->swordManager().moduleIsDailyDevotion(self->currentDictionary_);
    std::string key = self->dictionaryKeyInput_ && self->dictionaryKeyInput_->value()
        ? self->dictionaryKeyInput_->value()
        : "";
    key = trimCopy(key);
    if (isDailyDevotion) {
        if (!key.empty() && self->dictionaryKeys_) {
            auto it = std::find(self->dictionaryKeys_->begin(),
                                self->dictionaryKeys_->end(),
                                key);
            if (it == self->dictionaryKeys_->end()) {
                key.clear();
            }
        }
    } else if (key.empty()) {
        key = self->currentDictKey_;
    }
    if (!self->currentDictionary_.empty() &&
        (!key.empty() || isDailyDevotion)) {
        self->showDictionaryEntryInternal(self->currentDictionary_, key);
    } else {
        self->updateDictionaryNavigationChrome();
    }
}

void RightPane::onDictionaryKeyInput(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->dictionaryKeyInput_) return;

    std::string key = self->dictionaryKeyInput_->value()
                          ? self->dictionaryKeyInput_->value()
                          : "";
    if (key.empty()) return;

    if (self->currentDictionary_.empty()) {
        selectFirstDictionaryModule(self->dictionaryChoice_,
                                    self->dictionaryChoiceModules_,
                                    self->dictionaryChoiceLabels_,
                                    self->currentDictionary_);
    }
    if (self->currentDictionary_.empty()) return;

    self->showDictionaryEntryInternal(self->currentDictionary_, key);
}

void RightPane::onDictionaryBack(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    self->showAdjacentDictionaryEntry(-1);
}

void RightPane::onDictionaryForward(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    self->showAdjacentDictionaryEntry(1);
}

void RightPane::onTopTabChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->tabs_) return;

    Fl_Widget* active = self->tabs_->value();
    if (!active) return;
    self->activeTopTab_ = self->visibleTopTab();

    if (self->commentaryEditing_ && active != self->commentaryGroup_) {
        if (!self->saveCommentaryEdit(true)) {
            self->tabs_->value(self->commentaryGroup_);
            self->activeTopTab_ = TopTab::Commentary;
            self->secondaryTabIsGeneralBooks_ = false;
            return;
        }
    }

    if (active == self->documentsGroup_) {
        self->showGeneralBookTocOverlay(false);
        self->tabs_->value(self->documentsGroup_);
        self->activeTopTab_ = TopTab::Documents;
        self->dailyWorkspaceState_.tabActive = false;
        self->refreshDocumentChoices();
        self->updateDocumentChrome();
        return;
    }

    if (active == self->devotionsPlansGroup_) {
        self->showGeneralBookTocOverlay(false);
        self->tabs_->value(self->devotionsPlansGroup_);
        self->activeTopTab_ = TopTab::DevotionsPlans;
        self->dailyWorkspaceState_.tabActive = true;
        self->refreshDailyWorkspace(true);
    } else if (active == self->generalBooksGroup_) {
        self->tabs_->value(self->generalBooksGroup_);
        self->activeTopTab_ = TopTab::GeneralBooks;
        self->secondaryTabIsGeneralBooks_ = true;
        self->dailyWorkspaceState_.tabActive = false;
        if (self->generalBookHtml_ &&
            self->generalBookHtml_->currentHtml().empty() &&
            !self->currentGeneralBook_.empty()) {
            self->showGeneralBookEntry(self->currentGeneralBook_,
                                       self->currentGeneralBookKey_);
        }
    } else if (active == self->commentaryGroup_) {
        self->showGeneralBookTocOverlay(false);
        self->tabs_->value(self->commentaryGroup_);
        self->activeTopTab_ = TopTab::Commentary;
        self->secondaryTabIsGeneralBooks_ = false;
        self->dailyWorkspaceState_.tabActive = false;
        if (self->commentaryHtml_ &&
            self->commentaryHtml_->currentHtml().empty() &&
            !self->currentCommentary_.empty() &&
            !self->currentCommentaryRef_.empty()) {
            self->showCommentary(self->currentCommentary_,
                                 self->currentCommentaryRef_);
        }
    }
}

void RightPane::onGeneralBookModuleChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->generalBookChoice_) return;
    self->currentGeneralBook_ = module_choice::selectedModuleName(
        self->generalBookChoice_, self->generalBookChoiceModules_);
    if (self->currentGeneralBook_.empty()) return;
    self->populateGeneralBookToc();
    self->showGeneralBookEntry(self->currentGeneralBook_,
                               self->currentGeneralBookKey_);
}

void RightPane::onGeneralBookBack(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    self->showAdjacentGeneralBookEntry(-1);
}

void RightPane::onGeneralBookForward(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    self->showAdjacentGeneralBookEntry(1);
}

void RightPane::onGeneralBookContents(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    self->toggleGeneralBookTocOverlay();
}

void RightPane::onGeneralBookTreeSelect(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || self->generalBookTreeSyncing_ || !self->generalBookTocTree_) return;

    Fl_Tree_Reason reason = self->generalBookTocTree_->callback_reason();
    if (reason != FL_TREE_REASON_SELECTED &&
        reason != FL_TREE_REASON_RESELECTED) {
        return;
    }

    Fl_Tree_Item* item = self->generalBookTocTree_->callback_item();
    if (!item) return;

    auto it = self->generalBookTreeItemIndices_.find(item);
    if (it == self->generalBookTreeItemIndices_.end()) return;

    int index = it->second;
    if (index < 0 || index >= static_cast<int>(self->generalBookToc_.size())) return;

    self->showGeneralBookTocOverlay(false);
    self->showGeneralBookEntry(self->currentGeneralBook_,
                               self->generalBookToc_[static_cast<size_t>(index)].key);
}

void RightPane::onDailyModeChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    if (!self->maybeDiscardDailyPlanEditorChanges()) {
        self->updateDailyWorkspaceControls();
        return;
    }
    self->dailyWorkspaceState_.mode =
        (self->dailyWorkspaceState_.mode == DailyWorkspaceMode::ReadingPlans)
            ? DailyWorkspaceMode::Devotionals
            : DailyWorkspaceMode::ReadingPlans;
    self->ensureDailyWorkspaceDates();
    self->refreshDailyWorkspace(true);
}

void RightPane::onDailyDevotionalModuleChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->dailyDevotionalChoice_) return;
    if (!self->maybeDiscardDailyPlanEditorChanges()) {
        self->updateDailyWorkspaceControls();
        return;
    }
    self->dailyWorkspaceState_.mode = DailyWorkspaceMode::Devotionals;
    self->dailyWorkspaceState_.devotionalModule = module_choice::selectedModuleName(
        self->dailyDevotionalChoice_, self->dailyDevotionalModules_);
    self->refreshDailyWorkspace(true);
}

void RightPane::onDailyReadingPlanChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->dailyReadingPlanChoice_) return;
    if (!self->maybeDiscardDailyPlanEditorChanges()) {
        self->updateDailyWorkspaceControls();
        return;
    }
    self->dailyWorkspaceState_.mode = DailyWorkspaceMode::ReadingPlans;
    int index = self->dailyReadingPlanChoice_->value();
    if (index >= 0 && index < static_cast<int>(self->dailyReadingPlanChoices_.size())) {
        const auto& item = self->dailyReadingPlanChoices_[static_cast<size_t>(index)];
        if (item.kind == DailyReadingPlanChoiceItem::Kind::SwordModule) {
            self->dailyWorkspaceState_.readingPlanSource = DailyReadingPlanSource::SwordModule;
            self->dailyWorkspaceState_.readingPlanId = 0;
            self->dailyWorkspaceState_.swordReadingPlanModule = item.moduleName;
        } else {
            self->dailyWorkspaceState_.readingPlanSource = DailyReadingPlanSource::Editable;
            self->dailyWorkspaceState_.readingPlanId = item.planId;
            self->dailyWorkspaceState_.swordReadingPlanModule.clear();
        }
    } else {
        self->dailyWorkspaceState_.readingPlanSource = DailyReadingPlanSource::Editable;
        self->dailyWorkspaceState_.readingPlanId = 0;
        self->dailyWorkspaceState_.swordReadingPlanModule.clear();
    }
    self->dailyWorkspaceState_.readingPlanSelectedDateIso.clear();
    self->dailyWorkspaceState_.readingPlanEditMode = false;
    self->refreshDailyWorkspace(true);
}

void RightPane::onDailyPrevDay(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    reading::Date date{};
    if (!reading::parseIsoDate(self->currentDailyDateIso(), date)) {
        date = reading::today();
    }
    self->setCurrentDailyDateIso(reading::formatIsoDate(reading::addDays(date, -1)));
    self->refreshDailyWorkspace(true);
}

void RightPane::onDailyDateButton(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    if (self->dailyWorkspaceState_.mode == DailyWorkspaceMode::ReadingPlans) {
        reading::Date date{};
        if (reading::parseIsoDate(self->currentDailyDateIso(), date) &&
            self->dailyCalendarWidget_) {
            self->dailyCalendarWidget_->setDisplayedMonth(date);
            self->updateDailyCalendarMeta();
        }
        return;
    }
    self->dailyWorkspaceState_.calendarVisible =
        !self->dailyWorkspaceState_.calendarVisible;
    self->updateDailyWorkspaceControls();
    self->updateDailyCalendarMeta();
}

void RightPane::onDailyToday(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    self->setCurrentDailyDateIso(reading::formatIsoDate(reading::today()));
    self->refreshDailyWorkspace(true);
}

void RightPane::onDailyNextDay(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    reading::Date date{};
    if (!reading::parseIsoDate(self->currentDailyDateIso(), date)) {
        date = reading::today();
    }
    self->setCurrentDailyDateIso(reading::formatIsoDate(reading::addDays(date, 1)));
    self->refreshDailyWorkspace(true);
}

void RightPane::onDailyPrevMonth(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->dailyCalendarWidget_) return;
    self->dailyCalendarWidget_->setDisplayedMonth(
        reading::addMonths(self->dailyCalendarWidget_->displayedMonth(), -1));
    self->updateDailyCalendarMeta();
}

void RightPane::onDailyNextMonth(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->dailyCalendarWidget_) return;
    self->dailyCalendarWidget_->setDisplayedMonth(
        reading::addMonths(self->dailyCalendarWidget_->displayedMonth(), 1));
    self->updateDailyCalendarMeta();
}

void RightPane::onDailyCalendarDateSelected(const reading::Date& date, RightPane* self) {
    if (!self) return;
    self->setCurrentDailyDateIso(reading::formatIsoDate(date));
    self->refreshDailyWorkspace(true);
}

void RightPane::onDailyNewPlan(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->app_) return;
    if (!self->maybeDiscardDailyPlanEditorChanges()) {
        self->updateDailyWorkspaceControls();
        return;
    }

    ReadingPlan plan;
    plan.summary.startDateIso = reading::formatIsoDate(reading::today());
    if (!ReadingPlanEditorDialog::createPlan(self->app_, plan)) return;

    int createdId = 0;
    if (!self->app_->readingPlanManager().createPlan(plan, &createdId)) {
        fl_alert("Failed to create the reading plan.");
        return;
    }

    self->dailyWorkspaceState_.mode = DailyWorkspaceMode::ReadingPlans;
    self->dailyWorkspaceState_.readingPlanSource = DailyReadingPlanSource::Editable;
    self->dailyWorkspaceState_.readingPlanId = createdId;
    self->dailyWorkspaceState_.swordReadingPlanModule.clear();
    self->dailyWorkspaceState_.readingPlanSelectedDateIso.clear();
    self->dailyWorkspaceState_.readingPlanEditMode = false;
    self->populateReadingPlanChoices();
    self->refreshDailyWorkspace(true);
}

void RightPane::onDailyEditPlan(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->app_ ||
        self->dailyWorkspaceState_.readingPlanSource != DailyReadingPlanSource::Editable ||
        self->dailyWorkspaceState_.readingPlanId <= 0) {
        return;
    }
    self->enterDailyPlanEditMode();
}

void RightPane::onDailyDeletePlan(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->app_ ||
        self->dailyWorkspaceState_.readingPlanSource != DailyReadingPlanSource::Editable ||
        self->dailyWorkspaceState_.readingPlanId <= 0) {
        return;
    }

    ReadingPlan plan;
    if (!self->app_->readingPlanManager().getPlan(self->dailyWorkspaceState_.readingPlanId,
                                                  plan)) {
        return;
    }

    int confirm = fl_choice("Delete reading plan \"%s\"?",
                            "Cancel",
                            "Delete",
                            nullptr,
                            plan.summary.name.c_str());
    if (confirm != 1) return;

    if (!self->app_->readingPlanManager().deletePlan(self->dailyWorkspaceState_.readingPlanId)) {
        fl_alert("Failed to delete the reading plan.");
        return;
    }

    self->dailyWorkspaceState_.readingPlanId = 0;
    self->dailyWorkspaceState_.readingPlanSelectedDateIso.clear();
    self->populateReadingPlanChoices();
    self->refreshDailyWorkspace(true);
}

void RightPane::onDailyToggleComplete(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->app_) {
        return;
    }

    bool allCompleted = false;
    const std::vector<std::string> selectedDates =
        self->actionableSelectedReadingPlanDateIsos(&allCompleted);
    if (selectedDates.empty()) {
        return;
    }

    const bool targetCompleted = !allCompleted;
    bool ok = false;
    if (self->dailyWorkspaceState_.readingPlanSource == DailyReadingPlanSource::SwordModule) {
        if (self->dailyWorkspaceState_.swordReadingPlanModule.empty()) {
            return;
        }
        ok = true;
        for (const auto& dateIso : selectedDates) {
            if (!self->app_->readingPlanManager().setSwordDayCompleted(
                    self->dailyWorkspaceState_.swordReadingPlanModule,
                    dateIso,
                    targetCompleted)) {
                ok = false;
                break;
            }
        }
    } else if (self->dailyWorkspaceState_.readingPlanId > 0) {
        ok = true;
        for (const auto& dateIso : selectedDates) {
            if (!self->app_->readingPlanManager().setDayCompleted(
                    self->dailyWorkspaceState_.readingPlanId,
                    dateIso,
                    targetCompleted)) {
                ok = false;
                break;
            }
        }
    }

    if (!ok) {
        fl_alert("Failed to update the reading-day status.");
        return;
    }

    self->populateReadingPlanChoices();
    self->refreshDailyWorkspace(true);
}

void RightPane::onDailyToggleCompleteThrough(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->app_) {
        return;
    }

    bool allCompleted = false;
    const std::vector<std::string> targetDates =
        self->actionableReadingPlanDatesThroughCurrent(&allCompleted);
    if (targetDates.empty()) {
        return;
    }

    const bool targetCompleted = !allCompleted;
    bool ok = false;
    if (self->dailyWorkspaceState_.readingPlanSource == DailyReadingPlanSource::SwordModule) {
        if (self->dailyWorkspaceState_.swordReadingPlanModule.empty()) {
            return;
        }
        ok = true;
        for (const auto& dateIso : targetDates) {
            if (!self->app_->readingPlanManager().setSwordDayCompleted(
                    self->dailyWorkspaceState_.swordReadingPlanModule,
                    dateIso,
                    targetCompleted)) {
                ok = false;
                break;
            }
        }
    } else if (self->dailyWorkspaceState_.readingPlanId > 0) {
        ok = true;
        for (const auto& dateIso : targetDates) {
            if (!self->app_->readingPlanManager().setDayCompleted(
                    self->dailyWorkspaceState_.readingPlanId,
                    dateIso,
                    targetCompleted)) {
                ok = false;
                break;
            }
        }
    }

    if (!ok) {
        fl_alert("Failed to update the reading-day status.");
        return;
    }

    self->populateReadingPlanChoices();
    self->refreshDailyWorkspace(true);
}

void RightPane::onDailyReschedule(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->app_ ||
        self->dailyWorkspaceState_.readingPlanEditMode) {
        return;
    }

    if (self->dailyWorkspaceState_.readingPlanSource == DailyReadingPlanSource::Editable) {
        if (self->dailyWorkspaceState_.readingPlanId <= 0) return;

        ReadingPlan plan;
        if (!self->app_->readingPlanManager().getPlan(self->dailyWorkspaceState_.readingPlanId,
                                                      plan)) {
            return;
        }
        if (!readingPlanDayForDate(plan, self->dailyWorkspaceState_.readingPlanSelectedDateIso)) {
            return;
        }
    } else if (self->dailyWorkspaceState_.readingPlanSource ==
               DailyReadingPlanSource::SwordModule) {
        if (self->dailyWorkspaceState_.swordReadingPlanModule.empty() ||
            !self->swordReadingPlanHasContentForDate(
                self->dailyWorkspaceState_.swordReadingPlanModule,
                self->dailyWorkspaceState_.readingPlanSelectedDateIso)) {
            return;
        }
    } else {
        return;
    }

    ReadingPlanRescheduleRequest request;
    if (!promptReadingPlanReschedule(self->dailyWorkspaceState_.readingPlanSelectedDateIso,
                                     request)) {
        return;
    }

    bool ok = false;
    if (self->dailyWorkspaceState_.readingPlanSource == DailyReadingPlanSource::SwordModule) {
        ok = self->app_->readingPlanManager().rescheduleSwordDay(
            self->dailyWorkspaceState_.swordReadingPlanModule,
            self->dailyWorkspaceState_.readingPlanSelectedDateIso,
            request.targetDateIso);
    } else {
        ok = self->app_->readingPlanManager().rescheduleDay(
            self->dailyWorkspaceState_.readingPlanId,
            self->dailyWorkspaceState_.readingPlanSelectedDateIso,
            request.targetDateIso);
    }

    if (!ok) {
        fl_alert("Failed to reschedule the reading day.");
        return;
    }

    self->dailyWorkspaceState_.readingPlanSelectedDateIso = request.targetDateIso;
    self->populateReadingPlanChoices();
    self->refreshDailyWorkspace(true);
}

void RightPane::onDailyPlanEditorSelection(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->dailyWorkspaceState_.readingPlanEditMode) return;
    self->loadDailyPlanEditorSelection();
}

void RightPane::onDailyPlanEditorFieldChanged(Fl_Widget* w, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->dailyWorkspaceState_.readingPlanEditMode) return;

    if (w == self->dailyPlanNameInput_ ||
        w == self->dailyPlanStartDateInput_ ||
        w == self->dailyPlanDescriptionInput_) {
        self->applyDailyPlanEditorSummaryFields();
    } else {
        self->applyDailyPlanEditorSelectionFields();
        int index = self->selectedDailyPlanEditorIndex();
        if (index >= 0 && self->dailyPlanDayBrowser_) {
            const ReadingPlanDay& day =
                self->dailyPlanEditorWorkingPlan_.days[static_cast<size_t>(index)];
            self->dailyPlanDayBrowser_->text(index + 1,
                                             reading::formatReadingPlanDayLabel(day).c_str());
            self->dailyWorkspaceState_.readingPlanSelectedDateIso = day.dateIso;
        }
    }

    self->dailyPlanEditorDirty_ = true;
    self->updateDailyPlanEditorState();
}

void RightPane::onDailyPlanAddDay(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->dailyWorkspaceState_.readingPlanEditMode) return;

    self->applyDailyPlanEditorSummaryFields();
    self->applyDailyPlanEditorSelectionFields();

    ReadingPlanDay day;
    reading::Date baseDate{};
    bool haveBaseDate = false;
    int index = self->selectedDailyPlanEditorIndex();
    if (index >= 0 &&
        reading::parseIsoDate(self->dailyPlanEditorWorkingPlan_.days[static_cast<size_t>(index)].dateIso,
                              baseDate)) {
        haveBaseDate = true;
    } else if (!self->dailyPlanEditorWorkingPlan_.days.empty() &&
               reading::parseIsoDate(self->dailyPlanEditorWorkingPlan_.days.back().dateIso,
                                     baseDate)) {
        haveBaseDate = true;
    } else if (reading::parseIsoDate(self->dailyPlanEditorWorkingPlan_.summary.startDateIso,
                                     baseDate)) {
        haveBaseDate = true;
    }
    day.sequenceNumber = static_cast<int>(self->dailyPlanEditorWorkingPlan_.days.size()) + 1;
    day.dateIso = reading::formatIsoDate(
        haveBaseDate ? reading::addDays(baseDate, 1) : reading::today());
    self->dailyPlanEditorWorkingPlan_.days.push_back(std::move(day));
    reading::normalizeReadingPlanDays(self->dailyPlanEditorWorkingPlan_.days);
    self->dailyPlanEditorDirty_ = true;
    self->rebuildDailyPlanDayBrowser();
    if (self->dailyPlanDayBrowser_) {
        self->dailyPlanDayBrowser_->select(self->dailyPlanDayBrowser_->size());
        self->loadDailyPlanEditorSelection();
    }
}

void RightPane::onDailyPlanDuplicateDay(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->dailyWorkspaceState_.readingPlanEditMode) return;

    self->applyDailyPlanEditorSelectionFields();
    int index = self->selectedDailyPlanEditorIndex();
    if (index < 0) return;

    ReadingPlanDay copy = self->dailyPlanEditorWorkingPlan_.days[static_cast<size_t>(index)];
    copy.id = 0;
    copy.completed = false;
    copy.sequenceNumber = static_cast<int>(self->dailyPlanEditorWorkingPlan_.days.size()) + 1;
    for (auto& passage : copy.passages) {
        passage.id = 0;
    }
    reading::Date date{};
    if (reading::parseIsoDate(copy.dateIso, date)) {
        copy.dateIso = reading::formatIsoDate(reading::addDays(date, 1));
    }
    self->dailyPlanEditorWorkingPlan_.days.push_back(std::move(copy));
    reading::normalizeReadingPlanDays(self->dailyPlanEditorWorkingPlan_.days);
    self->dailyPlanEditorDirty_ = true;
    self->rebuildDailyPlanDayBrowser();
}

void RightPane::onDailyPlanRemoveDay(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->dailyWorkspaceState_.readingPlanEditMode) return;

    int index = self->selectedDailyPlanEditorIndex();
    if (index < 0) return;

    self->dailyPlanEditorWorkingPlan_.days.erase(
        self->dailyPlanEditorWorkingPlan_.days.begin() + index);
    self->dailyPlanEditorDirty_ = true;
    self->rebuildDailyPlanDayBrowser();
}

void RightPane::onDailyPlanSave(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->app_ || !self->dailyWorkspaceState_.readingPlanEditMode) return;

    ReadingPlan updated;
    std::string errorMessage;
    if (!self->validateDailyPlanEditorPlan(updated, errorMessage)) {
        fl_alert("%s", errorMessage.c_str());
        return;
    }

    if (!self->app_->readingPlanManager().updatePlan(updated)) {
        fl_alert("Failed to save the reading plan changes.");
        return;
    }

    if (!updated.days.empty()) {
        int index = self->selectedDailyPlanEditorIndex();
        if (index < 0) index = 0;
        index = std::min(index, static_cast<int>(updated.days.size()) - 1);
        self->dailyWorkspaceState_.readingPlanSelectedDateIso =
            updated.days[static_cast<size_t>(index)].dateIso;
    }

    self->dailyWorkspaceState_.readingPlanEditMode = false;
    self->dailyPlanEditorWorkingPlan_ = ReadingPlan{};
    self->dailyPlanEditorDirty_ = false;
    self->populateReadingPlanChoices();
    self->refreshDailyWorkspace(true);
}

void RightPane::onDailyPlanCancel(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    self->exitDailyPlanEditMode(false);
}

void RightPane::onDocumentChoiceChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || !self->documentChoice_ || self->documentChoiceSyncing_) return;

    int index = self->documentChoice_->value();
    if (index < 0 ||
        index >= static_cast<int>(self->documentChoicePaths_.size())) {
        self->refreshDocumentChoices();
        return;
    }

    const std::string& path = self->documentChoicePaths_[static_cast<size_t>(index)];
    if (path.empty() || pathsEqual(path, self->currentDocumentPath_)) {
        self->refreshDocumentChoices();
        self->updateDocumentChrome();
        return;
    }

    if (!self->openDocument(path, true)) {
        self->refreshDocumentChoices();
        self->updateDocumentChrome();
    }
}

void RightPane::onDocumentNew(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    self->newDocument();
}

void RightPane::onDocumentSave(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    self->saveDocument();
}

void RightPane::onDocumentExportOdt(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    self->exportDocumentToOdt();
}

void RightPane::onDocumentDelete(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    self->deleteCurrentDocument();
}

} // namespace verdad
