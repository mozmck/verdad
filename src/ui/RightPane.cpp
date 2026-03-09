#include "ui/RightPane.h"
#include "app/VerdadApp.h"
#include "ui/BiblePane.h"
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
#include <FL/fl_ask.H>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <system_error>

namespace verdad {
namespace {

namespace fs = std::filesystem;

std::string composeCss(const std::string& base, const std::string& extra) {
    if (base.empty()) return extra;
    if (extra.empty()) return base;
    return base + "\n" + extra;
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

void layoutDictionaryPaneContents(int paneX,
                                  int paneY,
                                  int paneW,
                                  int paneH,
                                  Fl_Input* dictionaryKeyInput,
                                  Fl_Choice* dictionaryChoice,
                                  HtmlWidget* dictionaryHtml) {
    if (!dictionaryKeyInput || !dictionaryChoice || !dictionaryHtml) {
        return;
    }

    const int choiceH = 25;

    int clampedPaneW = std::max(20, paneW);
    int clampedPaneH = std::max(20, paneH);

    int rowW = std::max(20, clampedPaneW - 4);
    int inputW = std::clamp(rowW / 3, 110, std::max(110, rowW - 150));
    int choiceW = std::max(20, rowW - inputW - 2);
    dictionaryKeyInput->resize(paneX + 2, paneY + 2, inputW, choiceH);
    dictionaryChoice->resize(paneX + 2 + inputW + 2, paneY + 2, choiceW, choiceH);
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
                          Fl_Input* dictionaryKeyInput,
                          Fl_Choice* dictionaryChoice,
                          HtmlWidget* dictionaryHtml) {
    if (!dictionaryPaneGroup || !dictionaryKeyInput ||
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
                                 dictionaryKeyInput,
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
    if (key.empty()) return -1;
    for (size_t i = 0; i < toc.size(); ++i) {
        if (toc[i].key == key) return static_cast<int>(i);
    }
    return -1;
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
    , dictionaryKeyInput_(nullptr)
    , dictionaryChoice_(nullptr)
    , dictionaryHtml_(nullptr)
    , currentDictionary_()
    , currentDictKey_()
    , generalBooksGroup_(nullptr)
    , generalBookChoice_(nullptr)
    , generalBookTocChoice_(nullptr)
    , generalBookHtml_(nullptr)
    , currentGeneralBook_()
    , currentGeneralBookKey_()
    , documentsGroup_(nullptr)
    , documentNewButton_(nullptr)
    , documentOpenButton_(nullptr)
    , documentSaveButton_(nullptr)
    , documentExportButton_(nullptr)
    , documentCloseButton_(nullptr)
    , documentPathLabel_(nullptr)
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
    commentaryEditor_->setChangeCallback([this]() { updateCommentaryEditorChrome(); });
    commentaryEditor_->hide();
    commentaryGroup_->end();
    commentaryGroup_->resizable(commentaryHtml_);

    generalBooksGroup_ = new Fl_Group(tileX, panelY, tileW, panelH, "General Books");
    generalBooksGroup_->begin();
    generalBookChoice_ = new Fl_Choice(tileX + 2, panelY + 2, tileW - 4, choiceH);
    generalBookChoice_->callback(onGeneralBookModuleChange, this);
    generalBookTocChoice_ = new Fl_Choice(tileX + 2,
                                          panelY + choiceH + 4,
                                          tileW - 4,
                                          choiceH);
    generalBookTocChoice_->callback(onGeneralBookTocChange, this);

    generalBookHtml_ = new HtmlWidget(tileX + 2,
                                      panelY + (choiceH * 2) + 6,
                                      tileW - 4,
                                      panelH - (choiceH * 2) - 8);
    generalBookHtml_->setLinkCallback(
        [this](const std::string& url) { onHtmlLink(url, false); });
    generalBooksGroup_->end();
    generalBooksGroup_->resizable(generalBookHtml_);

    documentsGroup_ = new Fl_Group(tileX, panelY, tileW, panelH, "Documents");
    documentsGroup_->begin();
    documentNewButton_ = new Fl_Button(tileX + 2, panelY + 2, 48, choiceH, "New");
    documentNewButton_->callback(onDocumentNew, this);
    documentOpenButton_ = new Fl_Button(tileX + 52, panelY + 2, 52, choiceH, "Open");
    documentOpenButton_->callback(onDocumentOpen, this);
    documentSaveButton_ = new Fl_Button(tileX + 106, panelY + 2, 52, choiceH, "Save");
    documentSaveButton_->callback(onDocumentSave, this);
    documentExportButton_ = new Fl_Button(tileX + 160, panelY + 2, 60, choiceH, "Export");
    documentExportButton_->callback(onDocumentExportOdt, this);
    documentCloseButton_ = new Fl_Button(tileX + 222, panelY + 2, 52, choiceH, "Close");
    documentCloseButton_->callback(onDocumentClose, this);
    documentPathLabel_ = new Fl_Box(tileX + 278, panelY + 2, tileW - 280, choiceH);
    documentPathLabel_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    documentsEditor_ = new HtmlEditorWidget(tileX + 2,
                                            panelY + choiceH + 4,
                                            tileW - 4,
                                            panelH - choiceH - 6);
    documentsEditor_->setMode(HtmlEditorWidget::Mode::Document);
    documentsEditor_->setIndentWidth(app_ ? app_->appearanceSettings().editorIndentWidth : 4);
    documentsEditor_->setChangeCallback([this]() { updateDocumentChrome(); });
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
    int dictInputW = std::clamp(dictRowW / 3, 110, std::max(110, dictRowW - 150));
    int dictChoiceW = std::max(20, dictRowW - dictInputW - 2);
    dictionaryKeyInput_ = new Fl_Input(tileX + 2,
                                       tileY + tabsInitH + 2,
                                       dictInputW,
                                       choiceH);
    dictionaryKeyInput_->tooltip("Type a dictionary key and press Enter");
    dictionaryKeyInput_->when(FL_WHEN_ENTER_KEY);
    dictionaryKeyInput_->callback(onDictionaryKeyInput, this);
    dictionaryChoice_ = new Fl_Choice(tileX + 2 + dictInputW + 2,
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
                                         dictionaryKeyInput_,
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
    populateDictionaryModules();
    populateGeneralBookModules();
    documentsEditor_->clearDocument();
    documentsEditor_->setModified(false);
    updateCommentaryEditorChrome();
    updateDocumentChrome();
}

RightPane::~RightPane() = default;

void RightPane::layoutTopTabContents(int tabsX, int tabsY, int tabsW, int tabsH) {
    if (!commentaryGroup_ || !commentaryChoice_ || !commentaryEditButton_ ||
        !commentarySaveButton_ || !commentaryCancelButton_ || !commentaryHtml_ ||
        !commentaryEditor_ || !generalBooksGroup_ || !generalBookChoice_ ||
        !generalBookTocChoice_ || !generalBookHtml_ ||
        !documentsGroup_ || !documentNewButton_ || !documentOpenButton_ ||
        !documentSaveButton_ || !documentExportButton_ || !documentCloseButton_ || !documentPathLabel_ ||
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
    generalBookTocChoice_->resize(contentX, panelY + rowH + 4, contentW, rowH);
    generalBookHtml_->resize(contentX,
                             panelY + (rowH * 2) + 6,
                             contentW,
                             std::max(10, panelH - (rowH * 2) - 8));

    int docsButtonsW = documentNewButton_->w() + documentOpenButton_->w() +
                       documentSaveButton_->w() + documentExportButton_->w() +
                       documentCloseButton_->w() + (buttonGap * 4);
    int pathX = contentX + docsButtonsW + buttonGap;
    documentNewButton_->resize(contentX, panelY + 2, documentNewButton_->w(), rowH);
    documentOpenButton_->resize(documentNewButton_->x() + documentNewButton_->w() + buttonGap,
                                panelY + 2,
                                documentOpenButton_->w(),
                                rowH);
    documentSaveButton_->resize(documentOpenButton_->x() + documentOpenButton_->w() + buttonGap,
                                panelY + 2,
                                documentSaveButton_->w(),
                                rowH);
    documentExportButton_->resize(documentSaveButton_->x() + documentSaveButton_->w() + buttonGap,
                                  panelY + 2,
                                  documentExportButton_->w(),
                                  rowH);
    documentCloseButton_->resize(documentExportButton_->x() + documentExportButton_->w() + buttonGap,
                                 panelY + 2,
                                 documentCloseButton_->w(),
                                 rowH);
    documentPathLabel_->resize(pathX,
                               panelY + 2,
                               std::max(20, contentX + contentW - pathX),
                               rowH);
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
        !documentsGroup_ || !documentsEditor_ || !dictionaryPaneGroup_ ||
        !dictionaryKeyInput_ || !dictionaryChoice_ || !dictionaryHtml_ ||
        !generalBooksGroup_ ||
        !generalBookChoice_ || !generalBookTocChoice_ ||
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
                         dictionaryKeyInput_,
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
        showDictionaryEntry(currentDictionary_, key);
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
        showDictionaryEntry(currentDictionary_, key);
    }
}

void RightPane::showDictionaryEntry(const std::string& moduleName,
                                    const std::string& key) {
    currentDictionary_ = moduleName;
    currentDictKey_ = key;
    if (dictionaryKeyInput_) {
        dictionaryKeyInput_->value(currentDictKey_.c_str());
    }

    std::string html = app_->swordManager().getDictionaryEntry(moduleName, key);
    if (dictionaryHtml_) {
        dictionaryHtml_->setHtml(html);
    }
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
    if (moduleChanged || generalBookToc_.empty()) {
        populateGeneralBookToc();
    }
    currentGeneralBookKey_ = key;
    if (currentGeneralBookKey_.empty() && !generalBookToc_.empty()) {
        currentGeneralBookKey_ = generalBookToc_.front().key;
    }
    if (generalBookTocChoice_) {
        for (int i = 0; i < generalBookTocChoice_->size(); ++i) {
            const Fl_Menu_Item& item = generalBookTocChoice_->menu()[i];
            if (item.label() && i < static_cast<int>(generalBookToc_.size()) &&
                generalBookToc_[static_cast<size_t>(i)].key == currentGeneralBookKey_) {
                generalBookTocChoice_->value(i);
                break;
            }
        }
    }

    std::string html = app_->swordManager().getGeneralBookEntry(
        moduleName, currentGeneralBookKey_);
    if (generalBookHtml_) {
        generalBookHtml_->setHtml(html);
    }
}

void RightPane::setCommentaryModule(const std::string& moduleName,
                                    bool activateCurrentVerse) {
    currentCommentary_ = moduleName;
    loadedCommentaryModule_.clear();
    loadedCommentaryChapterKey_.clear();

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

void RightPane::setDictionaryModule(const std::string& moduleName) {
    currentDictionary_ = moduleName;

    module_choice::applyChoiceValue(dictionaryChoice_,
                                    dictionaryChoiceModules_,
                                    dictionaryChoiceLabels_,
                                    moduleName);
}

void RightPane::setGeneralBookModule(const std::string& moduleName) {
    currentGeneralBook_ = moduleName;

    module_choice::applyChoiceValue(generalBookChoice_,
                                    generalBookChoiceModules_,
                                    generalBookChoiceLabels_,
                                    moduleName);

    populateGeneralBookToc();
}

bool RightPane::isDictionaryTabActive() const {
    return secondaryTabIsGeneralBooks_;
}

void RightPane::setDictionaryTabActive(bool dictionaryActive) {
    secondaryTabIsGeneralBooks_ = dictionaryActive;
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
        tabs_->value(documentsGroup_);
        activeTopTab_ = TopTab::Documents;
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
        !generalBookTocChoice_ || !generalBookHtml_ ||
        !documentsGroup_ || !documentsEditor_ ||
        !dictionaryPaneGroup_ || !dictionaryKeyInput_ ||
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
                         dictionaryKeyInput_,
                         dictionaryChoice_,
                         dictionaryHtml_);

    contentTile_->init_sizes();
    contentTile_->damage(FL_DAMAGE_ALL);
    contentTile_->redraw();
}

void RightPane::setStudyState(const std::string& commentaryModule,
                              const std::string& commentaryReference,
                              const std::string& dictionaryModule,
                              const std::string& dictionaryKey,
                              const std::string& generalBookModule,
                              const std::string& generalBookKey,
                              bool dictionaryActive) {
    perf::ScopeTimer timer("RightPane::setStudyState");
    if (!commentaryModule.empty()) {
        setCommentaryModule(commentaryModule);
    }
    if (!dictionaryModule.empty()) {
        setDictionaryModule(dictionaryModule);
    }
    if (!generalBookModule.empty()) {
        setGeneralBookModule(generalBookModule);
    }

    currentCommentaryRef_ = commentaryReference;
    currentDictKey_ = dictionaryKey;
    currentGeneralBookKey_ = generalBookKey;
    if (dictionaryKeyInput_) {
        dictionaryKeyInput_->value(currentDictKey_.c_str());
    }
    populateGeneralBookToc();

    setDictionaryTabActive(dictionaryActive);
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
    if (generalBookHtml_) {
        HtmlWidget::Snapshot snap = generalBookHtml_->captureSnapshot();
        buf.generalBook.doc = snap.doc;
        buf.generalBook.html = std::move(snap.html);
        buf.generalBook.baseUrl = std::move(snap.baseUrl);
        buf.generalBook.scrollY = snap.scrollY;
        buf.generalBook.contentHeight = snap.contentHeight;
        buf.generalBook.renderWidth = snap.renderWidth;
        buf.generalBook.scrollbarVisible = snap.scrollbarVisible;
        buf.generalBook.valid = snap.valid;
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
    if (generalBookHtml_) {
        HtmlWidget::Snapshot snap = generalBookHtml_->takeSnapshot();
        buf.generalBook.doc = std::move(snap.doc);
        buf.generalBook.html = std::move(snap.html);
        buf.generalBook.baseUrl = std::move(snap.baseUrl);
        buf.generalBook.scrollY = snap.scrollY;
        buf.generalBook.contentHeight = snap.contentHeight;
        buf.generalBook.renderWidth = snap.renderWidth;
        buf.generalBook.scrollbarVisible = snap.scrollbarVisible;
        buf.generalBook.valid = snap.valid;
    }
    return buf;
}

void RightPane::restoreDisplayBuffer(const DisplayBuffer& buffer, bool dictionaryActive) {
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
    if (generalBookHtml_ && buffer.generalBook.valid) {
        HtmlWidget::Snapshot snap;
        snap.doc = buffer.generalBook.doc;
        snap.html = buffer.generalBook.html;
        snap.baseUrl = buffer.generalBook.baseUrl;
        snap.scrollY = buffer.generalBook.scrollY;
        snap.contentHeight = buffer.generalBook.contentHeight;
        snap.renderWidth = buffer.generalBook.renderWidth;
        snap.scrollbarVisible = buffer.generalBook.scrollbarVisible;
        snap.valid = buffer.generalBook.valid;
        generalBookHtml_->restoreSnapshot(snap);
    }
    setDictionaryTabActive(dictionaryActive);
}

void RightPane::restoreDisplayBuffer(DisplayBuffer&& buffer, bool dictionaryActive) {
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
    if (generalBookHtml_ && buffer.generalBook.valid) {
        HtmlWidget::Snapshot snap;
        snap.doc = std::move(buffer.generalBook.doc);
        snap.html = std::move(buffer.generalBook.html);
        snap.baseUrl = std::move(buffer.generalBook.baseUrl);
        snap.scrollY = buffer.generalBook.scrollY;
        snap.contentHeight = buffer.generalBook.contentHeight;
        snap.renderWidth = buffer.generalBook.renderWidth;
        snap.scrollbarVisible = buffer.generalBook.scrollbarVisible;
        snap.valid = buffer.generalBook.valid;
        buffer.generalBook.valid = false;
        generalBookHtml_->restoreSnapshot(std::move(snap));
    }
    setDictionaryTabActive(dictionaryActive);
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
    if (dictionaryKeyInput_) dictionaryKeyInput_->redraw();
    if (dictionaryChoice_) dictionaryChoice_->redraw();
    if (generalBookChoice_) generalBookChoice_->redraw();
    if (generalBookTocChoice_) generalBookTocChoice_->redraw();
    if (documentNewButton_) documentNewButton_->redraw();
    if (documentOpenButton_) documentOpenButton_->redraw();
    if (documentSaveButton_) documentSaveButton_->redraw();
    if (documentExportButton_) documentExportButton_->redraw();
    if (documentCloseButton_) documentCloseButton_->redraw();
    if (documentPathLabel_) documentPathLabel_->redraw();
    redraw();
}

void RightPane::refresh() {
    perf::ScopeTimer timer("RightPane::refresh");
    TopTab keepTab = visibleTopTab();

    if (!currentCommentary_.empty() && !currentCommentaryRef_.empty()) {
        showCommentary(currentCommentary_, currentCommentaryRef_);
    }
    if (!currentDictionary_.empty() && !currentDictKey_.empty()) {
        showDictionaryEntry(currentDictionary_, currentDictKey_);
    }
    // Lazy-load general books to avoid paying parse/render cost on every cold
    // tab activation when user is reading commentary.
    if (!currentGeneralBook_.empty() &&
        (secondaryTabIsGeneralBooks_ || !currentGeneralBookKey_.empty())) {
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
    commentaryChapterCache_[cacheKey] = html;

    auto ordIt = std::find(commentaryChapterCacheOrder_.begin(),
                           commentaryChapterCacheOrder_.end(),
                           cacheKey);
    if (ordIt != commentaryChapterCacheOrder_.end()) {
        commentaryChapterCacheOrder_.erase(ordIt);
    }
    commentaryChapterCacheOrder_.push_back(cacheKey);

    while (commentaryChapterCacheOrder_.size() > kCommentaryChapterCacheLimit) {
        const std::string evict = commentaryChapterCacheOrder_.front();
        commentaryChapterCacheOrder_.pop_front();
        commentaryChapterCache_.erase(evict);
    }
}

void RightPane::invalidateCommentaryCache(const std::string& moduleName,
                                          const std::string& reference) {
    int verse = 0;
    std::string chapterKey = commentaryChapterKeyForReference(reference, &verse);
    if (chapterKey.empty()) {
        commentaryChapterCache_.clear();
        commentaryChapterCacheOrder_.clear();
    } else {
        std::string cacheKey = moduleName + "|" + chapterKey;
        commentaryChapterCache_.erase(cacheKey);
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

void RightPane::populateDictionaryModules() {
    if (!dictionaryChoice_) return;

    auto mods = app_->swordManager().getDictionaryModules();
    module_choice::populateChoice(dictionaryChoice_, mods,
                                  dictionaryChoiceModules_,
                                  dictionaryChoiceLabels_);

    if (dictionaryChoice_->size() > 0) {
        currentDictionary_ = dictionaryChoiceModules_.front();
    } else {
        currentDictionary_.clear();
        currentDictKey_.clear();
        if (dictionaryKeyInput_) {
            dictionaryKeyInput_->value("");
        }
        if (dictionaryHtml_) {
            dictionaryHtml_->setHtml(
                "<p><i>No dictionary modules installed.</i></p>");
        }
    }
}

void RightPane::populateGeneralBookModules() {
    if (!generalBookChoice_) return;

    auto mods = app_->swordManager().getGeneralBookModules();
    module_choice::populateChoice(generalBookChoice_, mods,
                                  generalBookChoiceModules_,
                                  generalBookChoiceLabels_);

    if (generalBookChoice_->size() > 0) {
        currentGeneralBook_ = generalBookChoiceModules_.front();
        populateGeneralBookToc();
        showGeneralBookEntry(currentGeneralBook_, currentGeneralBookKey_);
    } else {
        currentGeneralBook_.clear();
        currentGeneralBookKey_.clear();
        generalBookToc_.clear();
        if (generalBookTocChoice_) {
            generalBookTocChoice_->clear();
        }
        if (generalBookHtml_) {
            generalBookHtml_->setHtml(
                "<p><i>No general book modules installed.</i></p>");
        }
    }
}

void RightPane::populateGeneralBookToc() {
    generalBookToc_.clear();
    if (!generalBookTocChoice_) return;

    generalBookTocChoice_->clear();
    if (currentGeneralBook_.empty()) return;

    generalBookToc_ = app_->swordManager().getGeneralBookToc(currentGeneralBook_);
    for (const auto& entry : generalBookToc_) {
        std::string label(static_cast<size_t>(entry.depth) * 2, ' ');
        label += entry.label;
        if (entry.hasChildren) label += " ...";
        generalBookTocChoice_->add(label.c_str());
    }

    if (!generalBookToc_.empty()) {
        int selectedIndex = findGeneralBookTocIndex(generalBookToc_,
                                                    currentGeneralBookKey_);
        if (selectedIndex < 0) {
            selectedIndex = 0;
            currentGeneralBookKey_ = generalBookToc_.front().key;
        }
        generalBookTocChoice_->value(selectedIndex);
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
    if (documentPathLabel_) {
        std::string label;
        if (currentDocumentPath_.empty()) {
            label = "Untitled";
        } else {
            label = pathLeaf(currentDocumentPath_);
        }
        if (documentsEditor_ && documentsEditor_->isModified()) {
            label += " *";
        }
        documentPathLabel_->copy_label(label.c_str());
    }

    bool hasContent = documentsEditor_ &&
                      (documentsEditor_->isModified() ||
                       !documentsEditor_->html().empty() ||
                       !currentDocumentPath_.empty());
    if (documentSaveButton_) documentSaveButton_->activate();
    if (documentNewButton_) documentNewButton_->activate();
    if (documentOpenButton_) documentOpenButton_->activate();
    if (documentExportButton_) {
        if (hasContent) documentExportButton_->activate();
        else documentExportButton_->deactivate();
    }
    if (documentCloseButton_) {
        if (hasContent) documentCloseButton_->activate();
        else documentCloseButton_->deactivate();
    }
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

    int choice = fl_choice("Save changes to the current document?",
                           "Cancel",
                           "Discard",
                           "Save");
    if (choice == 0) return false;
    if (choice == 2) return saveDocument();
    return true;
}

bool RightPane::saveDocumentToPath(const std::string& path) {
    if (!documentsEditor_ || path.empty()) return false;

    std::ofstream out(path);
    if (!out.is_open()) {
        fl_alert("Failed to open document for writing:\n%s", path.c_str());
        return false;
    }

    out << documentsEditor_->html();
    out.close();
    if (!out) {
        fl_alert("Failed to save document:\n%s", path.c_str());
        return false;
    }

    currentDocumentPath_ = path;
    documentsEditor_->setModified(false);
    updateDocumentChrome();
    return true;
}

bool RightPane::saveDocumentAs() {
    Fl_Native_File_Chooser chooser;
    chooser.title("Save Document");
    chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    chooser.filter("HTML Files\t*.html\nAll Files\t*");
    if (!currentDocumentPath_.empty()) {
        chooser.preset_file(currentDocumentPath_.c_str());
    } else {
        chooser.preset_file("notes.html");
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
    if (!endsWithIgnoreCase(path, ".html") && !endsWithIgnoreCase(path, ".htm")) {
        path = pathWithExtension(path, ".html");
    }
    return saveDocumentToPath(path);
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

    if (!writeTextFile(inputPath.string(), documentsEditor_->html())) {
        cleanup();
        fl_alert("Failed to write the temporary HTML export.");
        return false;
    }

    if (!runLibreOfficeOdtExport(inputPath.string(),
                                 exportDir.string(),
                                 profileDir.string()) ||
        !fs::exists(outputPath)) {
        cleanup();
        fl_alert("LibreOffice could not export this document to ODT.\n"
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
    chooser.title("Export Document to ODT");
    chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    chooser.filter("ODT Files\t*.odt\nAll Files\t*");

    if (!currentDocumentPath_.empty()) {
        std::string suggested = pathWithExtension(currentDocumentPath_, ".odt");
        chooser.preset_file(suggested.c_str());
    } else {
        chooser.preset_file("notes.odt");
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
    updateDocumentChrome();
    setDocumentsTabActive(true);
    documentsEditor_->focusEditor();
    return true;
}

bool RightPane::openDocument() {
    Fl_Native_File_Chooser chooser;
    chooser.title("Open Document");
    chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
    chooser.filter("HTML Files\t*.html;*.htm\nAll Files\t*");
    if (!currentDocumentPath_.empty()) {
        chooser.preset_file(currentDocumentPath_.c_str());
    }

    int result = chooser.show();
    if (result != 0) {
        if (result < 0) {
            fl_alert("Unable to open the file chooser.");
        }
        return false;
    }

    std::string path = chooser.filename() ? chooser.filename() : "";
    return openDocument(path, true);
}

bool RightPane::openDocument(const std::string& path, bool activateTab) {
    if (path.empty() || !documentsEditor_) return false;
    if (!maybeSaveDocumentChanges()) return false;

    std::ifstream in(path);
    if (!in.is_open()) {
        fl_alert("Failed to open document:\n%s", path.c_str());
        return false;
    }

    std::string html((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    documentsEditor_->setHtml(html);
    documentsEditor_->setModified(false);
    currentDocumentPath_ = path;
    updateDocumentChrome();

    if (activateTab) {
        setDocumentsTabActive(true);
        documentsEditor_->focusEditor();
    }
    return true;
}

bool RightPane::saveDocument() {
    if (!documentsEditor_) return false;
    if (currentDocumentPath_.empty()) return saveDocumentAs();
    return saveDocumentToPath(currentDocumentPath_);
}

bool RightPane::closeDocument() {
    if (!maybeSaveDocumentChanges() || !documentsEditor_) return false;

    currentDocumentPath_.clear();
    documentsEditor_->clearDocument();
    documentsEditor_->setModified(false);
    updateDocumentChrome();
    return true;
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
    if (!self->currentDictionary_.empty() && !self->currentDictKey_.empty()) {
        self->showDictionaryEntry(self->currentDictionary_,
                                  self->currentDictKey_);
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

    self->showDictionaryEntry(self->currentDictionary_, key);
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
        self->tabs_->value(self->documentsGroup_);
        self->activeTopTab_ = TopTab::Documents;
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

void RightPane::onGeneralBookTocChange(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self || self->currentGeneralBook_.empty() || !self->generalBookTocChoice_) {
        return;
    }

    int index = self->generalBookTocChoice_->value();
    if (index < 0 ||
        index >= static_cast<int>(self->generalBookToc_.size())) {
        return;
    }
    self->currentGeneralBookKey_ =
        self->generalBookToc_[static_cast<size_t>(index)].key;
    self->showGeneralBookEntry(self->currentGeneralBook_,
                               self->currentGeneralBookKey_);
}

void RightPane::onDocumentNew(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    self->newDocument();
}

void RightPane::onDocumentOpen(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    self->openDocument();
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

void RightPane::onDocumentClose(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<RightPane*>(data);
    if (!self) return;
    self->closeDocument();
}

} // namespace verdad
