#include "ui/RightPane.h"
#include "app/VerdadApp.h"
#include "search/SearchIndexer.h"
#include "ui/BiblePane.h"
#include "ui/FilterableChoiceWidget.h"
#include "ui/HtmlEditorWidget.h"
#include "ui/HtmlWidget.h"
#include "ui/LeftPane.h"
#include "ui/MainWindow.h"
#include "ui/ModuleChoiceUtils.h"
#include "ui/StyledTabs.h"
#include "sword/SwordManager.h"
#include "app/PerfTrace.h"

#include <FL/Fl.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_SVG_Image.H>
#include <FL/fl_ask.H>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
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

std::string composeCss(const std::string& base, const std::string& extra) {
    if (base.empty()) return extra;
    if (extra.empty()) return base;
    return base + "\n" + extra;
}

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

std::string commentarySelectionCss(int verse) {
    if (verse <= 0) return "";

    std::ostringstream css;
    css << "div.commentary-verse#v" << verse
        << " > div.commentary-gutter > a.versenum-link > span.commentary-versenum {"
        << " display:inline-block;"
        << " padding:1px 1px;"
        << " border-radius:3px;"
        << " border:1px solid #c0d0e0;"
        << " background-color:#e0e8f0;"
        << " color:#1a5276;"
        << " line-height:1.1;"
        << " }\n";
    css << "div.commentary-verse#v" << verse
        << " > div.commentary-gutter > a.versenum-link:hover > span.commentary-versenum {"
        << " background-color:#c0d0e0;"
        << " }\n";
    return css.str();
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
    commentaryChoice_ = new Fl_Choice(tileX + 2, panelY + 2, tileW - 4, choiceH);
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
    generalBookChoice_ = new Fl_Choice(tileX + 2, panelY + 2, tileW - 4, choiceH);
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

    documentsGroup_ = new Fl_Group(tileX, panelY, tileW, panelH, "Studypad");
    documentsGroup_->begin();
    documentChoice_ = new Fl_Choice(tileX + 2,
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
    dictionaryChoice_ = new Fl_Choice(tileX + 2 + dictKeyAreaW + 2,
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
        }
    }

    populateCommentaryModules();
    populateDictionaryModules(false);
    populateGeneralBookModules(false);
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
        if (active == generalBooksGroup_) return TopTab::GeneralBooks;
        if (active == commentaryGroup_) return TopTab::Commentary;
    }
    return activeTopTab_;
}

void RightPane::resize(int X, int Y, int W, int H) {
    Fl_Group::resize(X, Y, W, H);

    if (!contentTile_ || !contentResizeBox_ || !tabs_ || !commentaryGroup_ ||
        !commentaryChoice_ || !commentaryHtml_ || !commentaryEditor_ ||
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

    if (tabs_ && visibleTopTab() != TopTab::Documents) {
        tabs_->value(commentaryGroup_);
        activeTopTab_ = TopTab::Commentary;
        secondaryTabIsGeneralBooks_ = false;
    }
}

void RightPane::updateCommentarySelection(int verse) {
    if (highlightedCommentaryVerse_ == verse) return;
    highlightedCommentaryVerse_ = verse;
    applyCommentaryStyleOverride();
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
    commentaryHtml_->setStyleOverrideCss(composeCss(
        htmlStyleOverrideCss_, commentarySelectionCss(highlightedCommentaryVerse_)));
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
    bool moduleChanged = (moduleName != currentDictionary_);
    currentDictionary_ = moduleName;
    currentDictKey_ = trimCopy(key);
    if (moduleChanged) {
        setDictionaryModule(moduleName);
    }

    std::string resolvedKey;
    std::string html = app_->swordManager().getDictionaryEntry(
        moduleName, currentDictKey_, &resolvedKey);
    if (!resolvedKey.empty()) {
        currentDictKey_ = resolvedKey;
    }
    if (dictionaryKeyInput_) {
        dictionaryKeyInput_->setDisplayedValue(currentDictKey_);
    }
    if (dictionaryHtml_) {
        dictionaryHtml_->setHtml(html);
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
    if (moduleName == currentDictionary_ &&
        dictionaryKeysModule_ == moduleName) {
        module_choice::applyChoiceValue(dictionaryChoice_,
                                        dictionaryChoiceModules_,
                                        dictionaryChoiceLabels_,
                                        moduleName);
        if (dictionaryKeyInput_) {
            dictionaryKeyInput_->setDisplayedValue(currentDictKey_);
        }
        updateDictionaryNavigationChrome();
        return;
    }

    currentDictionary_ = moduleName;

    module_choice::applyChoiceValue(dictionaryChoice_,
                                    dictionaryChoiceModules_,
                                    dictionaryChoiceLabels_,
                                    moduleName);
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
    if (!dictionaryActive) {
        showGeneralBookTocOverlay(false);
    }
    if (!tabs_) return;
    if (visibleTopTab() == TopTab::Documents) {
        activeTopTab_ = TopTab::Documents;
        return;
    }
    tabs_->value(dictionaryActive ? generalBooksGroup_ : commentaryGroup_);
    activeTopTab_ = dictionaryActive ? TopTab::GeneralBooks : TopTab::Commentary;
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

    if (!currentCommentary_.empty() && !currentCommentaryRef_.empty()) {
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

    if (keepTab == TopTab::Documents) {
        setDocumentsTabActive(true);
    } else {
        setDictionaryTabActive(keepTab == TopTab::GeneralBooks);
    }
    updateCommentaryEditorChrome();
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
    std::string key = self->dictionaryKeyInput_ && self->dictionaryKeyInput_->value()
        ? self->dictionaryKeyInput_->value()
        : "";
    key = trimCopy(key);
    if (key.empty()) key = self->currentDictKey_;
    if (!self->currentDictionary_.empty() && !key.empty()) {
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
        self->refreshDocumentChoices();
        self->updateDocumentChrome();
        return;
    }

    if (active == self->generalBooksGroup_) {
        self->tabs_->value(self->generalBooksGroup_);
        self->activeTopTab_ = TopTab::GeneralBooks;
        self->secondaryTabIsGeneralBooks_ = true;
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
