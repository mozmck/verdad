#include "app/VerdadApp.h"
#include "sword/SwordManager.h"
#include "search/SearchIndexer.h"
#include "tags/TagManager.h"
#include "ui/MainWindow.h"
#include "ui/BiblePane.h"

#include <FL/Fl.H>
#include <FL/Fl_File_Icon.H>
#include <FL/fl_ask.H>

#include <cstdlib>
#include <sys/stat.h>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#include <pwd.h>
#endif

namespace verdad {
namespace {

using PreferenceMap = std::unordered_map<std::string, std::string>;

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

int parseIntOr(const std::string& text, int fallback) {
    try {
        if (text.empty()) return fallback;
        return std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

bool parseBoolOr(const std::string& text, bool fallback) {
    std::string v = trimCopy(text);
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return fallback;
}

std::vector<std::string> splitCsv(const std::string& text) {
    std::vector<std::string> out;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trimCopy(item);
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

std::string joinCsv(const std::vector<std::string>& items) {
    std::ostringstream out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out << ',';
        out << items[i];
    }
    return out.str();
}

std::string normalizeAppFontName(const std::string& name) {
    std::string n = trimCopy(name);
    // Migrate legacy FLTK alias names to actual system font names
    if (n.empty() || n == "Helvetica") return "Sans";
    if (n == "Times") return "Serif";
    if (n == "Courier") return "Monospace";
    return n;
}

int clampFontSize(int size) {
    return std::clamp(size, 8, 36);
}

int clampHoverDelayMs(int ms) {
    return std::clamp(ms, 100, 5000);
}

int clampEditorIndentWidth(int width) {
    return std::clamp(width, 1, 8);
}

double parseDoubleOr(const std::string& text, double fallback) {
    try {
        if (text.empty()) return fallback;
        return std::stod(text);
    } catch (...) {
        return fallback;
    }
}

double clampLineHeight(double value) {
    return std::clamp(value, 1.0, 2.0);
}

std::string formatPreferenceDouble(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
}

std::string normalizePreviewDictionaryModule(const std::string& moduleName,
                                             const char* fallback) {
    std::string name = trimCopy(moduleName);
    if (!name.empty()) return name;
    return fallback ? std::string(fallback) : std::string();
}

std::string normalizeLanguageCode(const std::string& languageCode) {
    std::string code = trimCopy(languageCode);
    std::transform(code.begin(), code.end(), code.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });

    size_t sep = code.find_first_of("-_");
    if (sep != std::string::npos) code = code.substr(0, sep);

    if (code == "eng") return "en";
    if (code == "spa") return "es";
    if (code == "fra" || code == "fre") return "fr";
    if (code == "deu" || code == "ger") return "de";
    if (code == "por") return "pt";
    if (code == "ita") return "it";
    if (code == "rus") return "ru";
    if (code == "nld" || code == "dut") return "nl";
    if (code == "ell" || code == "gre") return "el";
    if (code == "heb" || code == "hbo") return "he";
    if (code == "ara") return "ar";
    if (code == "zho" || code == "chi") return "zh";
    if (code == "lat") return "la";

    return code;
}

bool containsNoCase(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;

    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

bool isGreekPreviewDictionary(const ModuleInfo& mod) {
    if (mod.name == "StrongsGreek" ||
        mod.name == "StrongsRealGreek" ||
        mod.name == "Thayer") {
        return true;
    }

    return containsNoCase(mod.name, "Greek") ||
           containsNoCase(mod.description, "Greek");
}

bool isHebrewPreviewDictionary(const ModuleInfo& mod) {
    if (mod.name == "StrongsHebrew" ||
        mod.name == "StrongsRealHebrew" ||
        mod.name == "TWOT") {
        return true;
    }

    return containsNoCase(mod.name, "Hebrew") ||
           containsNoCase(mod.description, "Hebrew");
}

bool isStrongsKeyedDictionary(const ModuleInfo& mod) {
    if (isGreekPreviewDictionary(mod) || isHebrewPreviewDictionary(mod)) {
        return true;
    }

    return containsNoCase(mod.name, "Strong") ||
           containsNoCase(mod.description, "Strong's") ||
           mod.name == "TWOT" ||
           mod.name == "Thayer";
}

bool hasDictionaryModuleName(const std::vector<ModuleInfo>& modules,
                             const std::string& moduleName) {
    if (moduleName.empty()) return false;
    for (const auto& mod : modules) {
        if (mod.name == moduleName) return true;
    }
    return false;
}

std::string escapeCssString(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

bool readPreferencesFile(const std::string& prefFile, PreferenceMap& prefsOut) {
    std::ifstream file(prefFile);
    if (!file.is_open()) return false;

    prefsOut.clear();
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trimCopy(line.substr(0, eq));
        std::string value = line.substr(eq + 1);
        prefsOut[key] = value;
    }
    return true;
}

MainWindow::SessionState sessionStateFromPreferences(const PreferenceMap& prefs) {
    auto lookup = [&](const std::string& key) -> std::string {
        auto it = prefs.find(key);
        return it != prefs.end() ? it->second : std::string();
    };

    MainWindow::SessionState state;
    state.windowX = parseIntOr(lookup("window_x"), -1);
    state.windowY = parseIntOr(lookup("window_y"), -1);
    state.windowW = parseIntOr(lookup("window_w"), 1200);
    state.windowH = parseIntOr(lookup("window_h"), 800);
    state.leftPaneWidth = parseIntOr(lookup("left_pane_w"), 300);
    state.leftPanePreviewHeight = parseIntOr(lookup("left_pane_preview_h"), 150);
    state.activeStudyTab = parseIntOr(lookup("active_study_tab"), 0);
    state.documentsTabActive = parseBoolOr(lookup("documents_tab_active"), false);
    state.documentPath = lookup("document_path");

    int tabCount = std::max(0, parseIntOr(lookup("study_tab_count"), 0));
    tabCount = std::min(tabCount, 64);
    for (int i = 0; i < tabCount; ++i) {
        std::string pfx = "study_tab_" + std::to_string(i) + "_";
        MainWindow::StudyTabState tab;
        auto lookup = [&](const std::string& key) -> std::string {
            auto it = prefs.find(pfx + key);
            return it != prefs.end() ? it->second : std::string();
        };

        tab.module = lookup("module");
        tab.book = lookup("book");
        tab.chapter = parseIntOr(lookup("chapter"), 1);
        tab.verse = parseIntOr(lookup("verse"), 1);
        tab.paragraphMode = parseBoolOr(lookup("paragraph_mode"), false);
        tab.parallelMode = parseBoolOr(lookup("parallel_mode"), false);
        tab.parallelModules = splitCsv(lookup("parallel_modules"));
        tab.biblePaneWidth = parseIntOr(lookup("bible_pane_w"), 0);
        tab.bibleScrollY = parseIntOr(lookup("bible_scroll_y"), -1);
        tab.commentaryModule = lookup("commentary_module");
        tab.commentaryReference = lookup("commentary_ref");
        tab.commentaryScrollY = parseIntOr(lookup("commentary_scroll_y"), -1);
        tab.dictionaryModule = lookup("dictionary_module");
        tab.dictionaryKey = lookup("dictionary_key");
        tab.generalBookModule = lookup("general_book_module");
        tab.generalBookKey = lookup("general_book_key");
        tab.dictionaryPaneHeight = parseIntOr(lookup("dictionary_pane_h"), 0);
        std::string activeRaw = lookup("general_book_active");
        if (activeRaw.empty()) activeRaw = lookup("dictionary_active");
        tab.dictionaryActive = parseBoolOr(activeRaw, false);
        state.studyTabs.push_back(std::move(tab));
    }

    return state;
}

void preserveCurrentLayout(MainWindow::SessionState& imported,
                           const MainWindow::SessionState& current) {
    imported.windowX = current.windowX;
    imported.windowY = current.windowY;
    imported.windowW = current.windowW;
    imported.windowH = current.windowH;
    imported.leftPaneWidth = current.leftPaneWidth;
    imported.leftPanePreviewHeight = current.leftPanePreviewHeight;

    for (auto& tab : imported.studyTabs) {
        tab.biblePaneWidth = 0;
        tab.dictionaryPaneHeight = 0;
        tab.bibleScrollY = -1;
        tab.commentaryScrollY = -1;
    }
}

} // namespace

VerdadApp* VerdadApp::instance_ = nullptr;

VerdadApp::VerdadApp()
    : swordMgr_(std::make_unique<SwordManager>())
    , searchIndexer_(nullptr)
    , tagMgr_(std::make_unique<TagManager>()) {
    instance_ = this;
}

VerdadApp::~VerdadApp() {
    savePreferences();
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

bool VerdadApp::initialize(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    // Ensure config directory exists
    ensureConfigDir();

    // Initialize SWORD
    if (!swordMgr_->initialize()) {
        fl_alert("Failed to initialize SWORD library.\n"
                 "Please ensure SWORD modules are installed.");
        // Continue anyway - app can still run without modules
    }

    // Load tags from the SQLite tag database.
    tagMgr_->load(getConfigDir() + "/tags.db");

    // Initialize FTS5 index database (separate from tags/settings data).
    searchIndexer_ = std::make_unique<SearchIndexer>(
        getConfigDir() + "/module_index.db");

    // Set up FLTK
    Fl::scheme("gtk+");
    Fl_File_Icon::load_system_icons();

    // Enumerate system fonts
    enumerateSystemFonts();

    // Create main window
    mainWindow_ = std::make_unique<MainWindow>(this, 1200, 800, "Verdad Bible Study");

    // Load user preferences
    loadPreferences();

    // Ensure appearance is applied even when no preference file exists.
    setAppearanceSettings(appearanceSettings_);

    // Index current Bible first, then queue remaining Bible modules.
    if (searchIndexer_) {
        std::string activeModule;
        if (mainWindow_ && mainWindow_->biblePane()) {
            activeModule = trimCopy(mainWindow_->biblePane()->currentModule());
        }
        if (!activeModule.empty()) {
            searchIndexer_->queueModuleIndex(activeModule);
        }

        std::vector<std::string> bibleModules;
        for (const auto& mod : swordMgr_->getBibleModules()) {
            if (!mod.name.empty()) bibleModules.push_back(mod.name);
        }
        searchIndexer_->queueModuleIndex(bibleModules);
    }

    return true;
}

int VerdadApp::run() {
    if (mainWindow_) {
        mainWindow_->show();
    }
    return Fl::run();
}

std::string VerdadApp::getDataDir() const {
    // Check for installed data
    std::string dataDir = "/usr/share/verdad";
    struct stat st;
    if (stat(dataDir.c_str(), &st) == 0) {
        return dataDir;
    }

    // Check local data directory
    dataDir = "./data";
    if (stat(dataDir.c_str(), &st) == 0) {
        return dataDir;
    }

    return dataDir;
}

std::string VerdadApp::getConfigDir() const {
#ifdef _WIN32
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, path))) {
        return std::string(path) + "\\Verdad";
    }
    return ".\\verdad_config";
#else
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (home) {
        return std::string(home) + "/.config/verdad";
    }
    return "./.config/verdad";
#endif
}

void VerdadApp::ensureConfigDir() {
    std::string dir = getConfigDir();

#ifdef _WIN32
    CreateDirectoryA(dir.c_str(), nullptr);
#else
    mkdir(dir.c_str(), 0755);
#endif
}

void VerdadApp::loadPreferences() {
    loadPreferencesFromFile(getConfigDir() + "/preferences.conf", false);
}

bool VerdadApp::loadPreferencesFromFile(const std::string& prefFile,
                                        bool preserveLayout) {
    PreferenceMap prefs;
    if (!readPreferencesFile(prefFile, prefs)) return false;
    return applyPreferencesMap(prefs, preserveLayout);
}

bool VerdadApp::applyPreferencesMap(const PreferenceMap& prefs,
                                    bool preserveLayout) {
    if (!mainWindow_) return false;

    auto lookup = [&](const std::string& key) -> std::string {
        auto it = prefs.find(key);
        return it != prefs.end() ? it->second : std::string();
    };

    AppearanceSettings importedAppearance = appearanceSettings_;
    importedAppearance.appFontName =
        normalizeAppFontName(lookup("app_font"));
    importedAppearance.appFontSize =
        clampFontSize(parseIntOr(lookup("app_font_size"),
                                 importedAppearance.appFontSize));
    if (!lookup("text_font_family").empty()) {
        importedAppearance.textFontFamily = lookup("text_font_family");
    }
    importedAppearance.textFontSize =
        clampFontSize(parseIntOr(lookup("text_font_size"),
                                 importedAppearance.textFontSize));
    importedAppearance.textLineHeight =
        clampLineHeight(parseDoubleOr(lookup("text_line_height"),
                                      importedAppearance.textLineHeight));
    importedAppearance.hoverDelayMs =
        clampHoverDelayMs(parseIntOr(lookup("hover_delay_ms"),
                                     importedAppearance.hoverDelayMs));
    importedAppearance.editorIndentWidth =
        clampEditorIndentWidth(parseIntOr(lookup("editor_indent_width"),
                                          importedAppearance.editorIndentWidth));
    importedAppearance.editorLineHeight =
        clampLineHeight(parseDoubleOr(lookup("editor_line_height"),
                                      importedAppearance.editorLineHeight));

    OptionDisplaySettings importedOptions = optionDisplaySettings_;
    importedOptions.showStrongsMarkers =
        parseBoolOr(lookup("show_strongs_markers"),
                    importedOptions.showStrongsMarkers);
    importedOptions.showMorphMarkers =
        parseBoolOr(lookup("show_morph_markers"),
                    importedOptions.showMorphMarkers);
    importedOptions.showFootnoteMarkers =
        parseBoolOr(lookup("show_footnote_markers"),
                    importedOptions.showFootnoteMarkers);
    importedOptions.showCrossReferenceMarkers =
        parseBoolOr(lookup("show_cross_reference_markers"),
                    importedOptions.showCrossReferenceMarkers);

    PreviewDictionarySettings importedPreview = previewDictionarySettings_;
    importedPreview.greekModule =
        normalizePreviewDictionaryModule(lookup("preview_dict_greek"),
                                         importedPreview.greekModule.c_str());
    importedPreview.hebrewModule =
        normalizePreviewDictionaryModule(lookup("preview_dict_hebrew"),
                                         importedPreview.hebrewModule.c_str());
    importedPreview.languageModules.clear();
    for (const auto& kv : prefs) {
        static const std::string prefix = "default_dict_lang_";
        if (kv.first.compare(0, prefix.size(), prefix) != 0) continue;

        std::string code = normalizeLanguageCode(kv.first.substr(prefix.size()));
        std::string module = trimCopy(kv.second);
        if (!code.empty() && !module.empty()) {
            importedPreview.languageModules[code] = module;
        }
    }
    setPreviewDictionarySettings(importedPreview);
    setOptionDisplaySettings(importedOptions);

    // New session format: restore full window/tabs/splitter state.
    if (prefs.find("study_tab_count") != prefs.end()) {
        MainWindow::SessionState state = sessionStateFromPreferences(prefs);
        if (preserveLayout && mainWindow_) {
            preserveCurrentLayout(state, mainWindow_->captureSessionState());
        }

        mainWindow_->restoreSessionState(state);
        setAppearanceSettings(importedAppearance);
        return true;
    }

    // Legacy format fallback.
    if (mainWindow_->biblePane()) {
        auto itMod = prefs.find("bible_module");
        if (itMod != prefs.end() && !itMod->second.empty()) {
            mainWindow_->biblePane()->setModule(itMod->second);
        }

        auto itBook = prefs.find("bible_book");
        if (itBook != prefs.end() && !itBook->second.empty()) {
            int chapter = 1;
            int verse = 1;
            auto itCh = prefs.find("bible_chapter");
            if (itCh != prefs.end()) chapter = parseIntOr(itCh->second, 1);
            auto itVs = prefs.find("bible_verse");
            if (itVs != prefs.end()) verse = parseIntOr(itVs->second, 1);
            mainWindow_->biblePane()->navigateTo(itBook->second, chapter, verse);
        }
    }

    setAppearanceSettings(importedAppearance);
    return true;
}

void VerdadApp::savePreferences() {
    std::string prefFile = getConfigDir() + "/preferences.conf";
    std::ofstream file(prefFile);
    if (!file.is_open()) return;

    file << "# Verdad preferences\n";

    if (mainWindow_) {
        file << "app_font=" << appearanceSettings_.appFontName << "\n";
        file << "app_font_size=" << appearanceSettings_.appFontSize << "\n";
        file << "text_font_family=" << appearanceSettings_.textFontFamily << "\n";
        file << "text_font_size=" << appearanceSettings_.textFontSize << "\n";
        file << "text_line_height="
             << formatPreferenceDouble(appearanceSettings_.textLineHeight) << "\n";
        file << "hover_delay_ms=" << appearanceSettings_.hoverDelayMs << "\n";
        file << "editor_indent_width=" << appearanceSettings_.editorIndentWidth << "\n";
        file << "editor_line_height="
             << formatPreferenceDouble(appearanceSettings_.editorLineHeight) << "\n";
        file << "show_strongs_markers=" << (optionDisplaySettings_.showStrongsMarkers ? 1 : 0) << "\n";
        file << "show_morph_markers=" << (optionDisplaySettings_.showMorphMarkers ? 1 : 0) << "\n";
        file << "show_footnote_markers=" << (optionDisplaySettings_.showFootnoteMarkers ? 1 : 0) << "\n";
        file << "show_cross_reference_markers=" << (optionDisplaySettings_.showCrossReferenceMarkers ? 1 : 0) << "\n";
        file << "preview_dict_greek=" << previewDictionarySettings_.greekModule << "\n";
        file << "preview_dict_hebrew=" << previewDictionarySettings_.hebrewModule << "\n";
        std::vector<std::string> languageCodes;
        languageCodes.reserve(previewDictionarySettings_.languageModules.size());
        for (const auto& kv : previewDictionarySettings_.languageModules) {
            if (kv.first.empty() || trimCopy(kv.second).empty()) continue;
            languageCodes.push_back(kv.first);
        }
        std::sort(languageCodes.begin(), languageCodes.end());
        for (const auto& code : languageCodes) {
            file << "default_dict_lang_" << code << "="
                 << previewDictionarySettings_.languageModules.at(code) << "\n";
        }

        MainWindow::SessionState state = mainWindow_->captureSessionState();
        file << "window_x=" << state.windowX << "\n";
        file << "window_y=" << state.windowY << "\n";
        file << "window_w=" << state.windowW << "\n";
        file << "window_h=" << state.windowH << "\n";
        file << "left_pane_w=" << state.leftPaneWidth << "\n";
        file << "left_pane_preview_h=" << state.leftPanePreviewHeight << "\n";
        file << "active_study_tab=" << state.activeStudyTab << "\n";
        file << "documents_tab_active=" << (state.documentsTabActive ? 1 : 0) << "\n";
        file << "document_path=" << state.documentPath << "\n";
        file << "study_tab_count=" << state.studyTabs.size() << "\n";

        for (size_t i = 0; i < state.studyTabs.size(); ++i) {
            const auto& t = state.studyTabs[i];
            std::string pfx = "study_tab_" + std::to_string(i) + "_";
            file << pfx << "module=" << t.module << "\n";
            file << pfx << "book=" << t.book << "\n";
            file << pfx << "chapter=" << t.chapter << "\n";
            file << pfx << "verse=" << t.verse << "\n";
            file << pfx << "paragraph_mode=" << (t.paragraphMode ? 1 : 0) << "\n";
            file << pfx << "parallel_mode=" << (t.parallelMode ? 1 : 0) << "\n";
            file << pfx << "parallel_modules=" << joinCsv(t.parallelModules) << "\n";
            file << pfx << "bible_pane_w=" << t.biblePaneWidth << "\n";
            file << pfx << "bible_scroll_y=" << t.bibleScrollY << "\n";
            file << pfx << "commentary_module=" << t.commentaryModule << "\n";
            file << pfx << "commentary_ref=" << t.commentaryReference << "\n";
            file << pfx << "commentary_scroll_y=" << t.commentaryScrollY << "\n";
            file << pfx << "dictionary_module=" << t.dictionaryModule << "\n";
            file << pfx << "dictionary_key=" << t.dictionaryKey << "\n";
            file << pfx << "general_book_module=" << t.generalBookModule << "\n";
            file << pfx << "general_book_key=" << t.generalBookKey << "\n";
            file << pfx << "dictionary_pane_h=" << t.dictionaryPaneHeight << "\n";
            file << pfx << "general_book_active=" << (t.dictionaryActive ? 1 : 0) << "\n";
            // Legacy compatibility for older builds.
            file << pfx << "dictionary_active=" << (t.dictionaryActive ? 1 : 0) << "\n";
        }

        // Also keep legacy keys for backwards compatibility with older builds.
        if (mainWindow_->biblePane()) {
            file << "bible_module=" << mainWindow_->biblePane()->currentModule() << "\n";
            file << "bible_book=" << mainWindow_->biblePane()->currentBook() << "\n";
            file << "bible_chapter=" << mainWindow_->biblePane()->currentChapter() << "\n";
            file << "bible_verse=" << mainWindow_->biblePane()->currentVerse() << "\n";
        }
    }
}

void VerdadApp::setAppearanceSettings(const AppearanceSettings& settings) {
    appearanceSettings_.appFontName = normalizeAppFontName(settings.appFontName);
    appearanceSettings_.appFontSize = clampFontSize(settings.appFontSize);
    appearanceSettings_.textFontFamily =
        trimCopy(settings.textFontFamily).empty()
            ? std::string("DejaVu Serif")
            : trimCopy(settings.textFontFamily);
    appearanceSettings_.textFontSize = clampFontSize(settings.textFontSize);
    appearanceSettings_.textLineHeight = clampLineHeight(settings.textLineHeight);
    appearanceSettings_.hoverDelayMs = clampHoverDelayMs(settings.hoverDelayMs);
    appearanceSettings_.editorIndentWidth =
        clampEditorIndentWidth(settings.editorIndentWidth);
    appearanceSettings_.editorLineHeight = clampLineHeight(settings.editorLineHeight);

    if (mainWindow_) {
        mainWindow_->applyAppearanceSettings(
            appFont(),
            appearanceSettings_.appFontSize,
            textStyleOverrideCss());
    }
}

void VerdadApp::setPreviewDictionarySettings(
    const PreviewDictionarySettings& settings) {
    PreviewDictionarySettings normalized = settings;
    normalized.greekModule =
        normalizePreviewDictionaryModule(normalized.greekModule,
                                         "StrongsRealGreek");
    normalized.hebrewModule =
        normalizePreviewDictionaryModule(normalized.hebrewModule,
                                         "StrongsRealHebrew");

    std::unordered_map<std::string, std::string> normalizedLanguageModules;
    for (const auto& kv : normalized.languageModules) {
        std::string code = normalizeLanguageCode(kv.first);
        std::string module = trimCopy(kv.second);
        if (!code.empty() && !module.empty()) {
            normalizedLanguageModules[code] = module;
        }
    }

    previewDictionarySettings_.greekModule = std::move(normalized.greekModule);
    previewDictionarySettings_.hebrewModule = std::move(normalized.hebrewModule);
    previewDictionarySettings_.languageModules = std::move(normalizedLanguageModules);
}

void VerdadApp::setOptionDisplaySettings(
    const OptionDisplaySettings& settings) {
    optionDisplaySettings_ = settings;

    if (mainWindow_) {
        mainWindow_->applyAppearanceSettings(
            appFont(),
            appearanceSettings_.appFontSize,
            textStyleOverrideCss());
    }
}

std::string VerdadApp::preferredPreviewDictionary(char strongPrefix) const {
    char prefix = static_cast<char>(
        std::toupper(static_cast<unsigned char>(strongPrefix)));
    std::vector<std::string> candidates = strongsDictionaryModules(prefix);

    std::string configured;
    std::vector<std::string> preferredNames;
    if (prefix == 'G') {
        configured = trimCopy(previewDictionarySettings_.greekModule);
        preferredNames = {"StrongsRealGreek", "StrongsGreek", "Thayer"};
    } else if (prefix == 'H') {
        configured = trimCopy(previewDictionarySettings_.hebrewModule);
        preferredNames = {"StrongsRealHebrew", "StrongsHebrew", "TWOT"};
    } else {
        return "";
    }

    if (!configured.empty() &&
        std::find(candidates.begin(), candidates.end(), configured) != candidates.end()) {
        return configured;
    }

    auto dicts = swordManager().getDictionaryModules();
    for (const auto& name : preferredNames) {
        if (hasDictionaryModuleName(dicts, name)) return name;
    }

    if (!candidates.empty()) return candidates.front();
    return "";
}

std::vector<std::string> VerdadApp::strongsDictionaryModules(char strongPrefix) const {
    char prefix = static_cast<char>(
        std::toupper(static_cast<unsigned char>(strongPrefix)));
    std::vector<std::string> names;

    for (const auto& mod : swordManager().getDictionaryModules()) {
        if (mod.name.empty()) continue;

        bool include = false;
        if (prefix == 'G') {
            include = isGreekPreviewDictionary(mod);
        } else if (prefix == 'H') {
            include = isHebrewPreviewDictionary(mod);
        }

        if (include) names.push_back(mod.name);
    }

    return names;
}

std::vector<std::string> VerdadApp::wordDictionaryModules(
    const std::string& languageCode) const {
    std::vector<std::string> names;
    std::string normalizedCode = normalizeLanguageCode(languageCode);

    for (const auto& mod : swordManager().getDictionaryModules()) {
        if (mod.name.empty() || isStrongsKeyedDictionary(mod)) continue;

        std::string modLanguage = normalizeLanguageCode(mod.language);
        if (!normalizedCode.empty() && modLanguage != normalizedCode) continue;

        names.push_back(mod.name);
    }

    return names;
}

std::string VerdadApp::preferredWordDictionary(
    const std::string& languageCode) const {
    std::string normalizedCode = normalizeLanguageCode(languageCode);
    auto installedDicts = swordManager().getDictionaryModules();

    auto configuredModule = [&](const std::string& code) -> std::string {
        if (code.empty()) return "";
        auto it = previewDictionarySettings_.languageModules.find(code);
        if (it == previewDictionarySettings_.languageModules.end()) return "";

        std::string module = trimCopy(it->second);
        if (module.empty()) return "";
        if (!hasDictionaryModuleName(installedDicts, module)) return "";
        return module;
    };

    std::string configured = configuredModule(normalizedCode);
    if (!configured.empty()) return configured;

    auto candidates = wordDictionaryModules(normalizedCode);
    if (!candidates.empty()) return candidates.front();

    if (!normalizedCode.empty()) return "";

    std::string englishConfigured = configuredModule("en");
    if (!englishConfigured.empty()) return englishConfigured;

    auto englishCandidates = wordDictionaryModules("en");
    if (!englishCandidates.empty()) return englishCandidates.front();

    auto anyCandidates = wordDictionaryModules("");
    if (!anyCandidates.empty()) return anyCandidates.front();

    return "";
}

Fl_Font VerdadApp::appFont() const {
    return fltkFontFromFamily(appearanceSettings_.appFontName);
}

Fl_Font VerdadApp::textEditorFont() const {
    return fltkFontFromFamily(appearanceSettings_.textFontFamily);
}

Fl_Font VerdadApp::boldTextEditorFont() const {
    return boldFltkFont(textEditorFont());
}

std::string VerdadApp::textStyleOverrideCss() const {
    std::string family = escapeCssString(appearanceSettings_.textFontFamily);
    int size = clampFontSize(appearanceSettings_.textFontSize);
    double lineHeight = clampLineHeight(appearanceSettings_.textLineHeight);
    const auto& options = optionDisplaySettings_;

    std::ostringstream css;
    css << "body,\n"
        << "div.chapter,\n"
        << "div.verse,\n"
        << "div.parallel-col,\n"
        << "div.parallel-col-last,\n"
        << "div.parallel-cell,\n"
        << "div.parallel-cell-last,\n"
        << "div.commentary-heading,\n"
        << "div.commentary,\n"
        << "div.commentary-text,\n"
        << "div.dictionary,\n"
        << "div.general-book,\n"
        << "div.mag-lite {\n"
        << "  font-family: \"" << family << "\" !important;\n"
        << "  font-size: " << size << "px !important;\n"
        << "  line-height: " << lineHeight << " !important;\n"
        << "}\n";

    css << "span.verdad-inline-marker { display: none; }\n";
    if (options.showStrongsMarkers) {
        css << "span.verdad-inline-marker.strongs-marker { display: inline; }\n";
    }
    if (options.showMorphMarkers) {
        css << "span.verdad-inline-marker.morph-marker { display: inline; }\n";
    }
    if (!options.showFootnoteMarkers) {
        css << "a.noteMarker:not(.crossReference), a.footnote:not(.crossReference) { display: none; }\n";
    }
    if (!options.showCrossReferenceMarkers) {
        css << "a.noteMarker.crossReference, a.footnote.crossReference { display: none; }\n";
    }

    return css.str();
}

/// Minimal suffix stripping — only remove suffixes that distinguish the regular
/// face of a family (where the bold variant uses a different naming convention).
/// E.g. "aakar medium" (regular) vs "aakar Bold", "CentSchbook BT Roman" vs
/// "CentSchbook BT Bold".  Weight/width variants like " Thin", " Narrow",
/// " Medium" are NOT stripped — they denote separate sub-families.
static std::string stripRegularSuffix(const char* name) {
    if (!name) return {};
    std::string s(name);
    static const char* suffixes[] = {
        " Regular", " medium", " Book", " Roman", nullptr
    };
    for (int i = 0; suffixes[i]; ++i) {
        size_t slen = std::strlen(suffixes[i]);
        if (s.size() > slen && s.compare(s.size() - slen, slen, suffixes[i]) == 0) {
            return s.substr(0, s.size() - slen);
        }
    }
    return s;
}

void VerdadApp::enumerateSystemFonts() {
    Fl_Font count = Fl::set_fonts("-*");

    std::set<std::string> families;

    // Build a name-based lookup: lowercase bold name → font index.
    std::unordered_map<std::string, Fl_Font> boldByName;
    for (Fl_Font f = 0; f < count; ++f) {
        int attrs = 0;
        const char* name = Fl::get_font_name(f, &attrs);
        if (!name || !name[0]) continue;
        // Only pure bold (attrs==1), not bold-italic (attrs==3).
        if (attrs != 1) continue;
        std::string lower(name);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (boldByName.find(lower) == boldByName.end())
            boldByName[lower] = f;
    }

    // Build fontFamilyMap_ and boldVariantMap_ from regular fonts.
    for (Fl_Font f = 0; f < count; ++f) {
        int attrs = 0;
        const char* name = Fl::get_font_name(f, &attrs);
        if (!name || !name[0] || attrs != 0) continue;

        std::string familyName(name);
        if (familyName[0] == ' ') continue;

        // Font family list and name→index map.
        std::string lower = familyName;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (fontFamilyMap_.find(lower) == fontFamilyMap_.end())
            fontFamilyMap_[lower] = f;
        families.insert(familyName);

        // Find the bold variant by name: look up "name Bold" first.
        std::string boldTarget = lower + " bold";
        auto it = boldByName.find(boldTarget);
        if (it != boldByName.end()) {
            boldVariantMap_[f] = it->second;
            continue;
        }

        // Fallback: strip regular-only suffixes and retry.
        // E.g. "aakar medium" → "aakar" → look up "aakar bold".
        std::string stripped = stripRegularSuffix(name);
        if (stripped != familyName) {
            std::string strippedLower = stripped;
            std::transform(strippedLower.begin(), strippedLower.end(),
                           strippedLower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            std::string target2 = strippedLower + " bold";
            auto it2 = boldByName.find(target2);
            if (it2 != boldByName.end())
                boldVariantMap_[f] = it2->second;
        }
    }

    systemFontFamilies_.assign(families.begin(), families.end());
}

Fl_Font VerdadApp::fltkFontFromFamily(const std::string& family) const {
    std::string lower = family;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Handle common aliases
    if (lower == "helvetica" || lower == "arial") lower = "sans";
    else if (lower == "times" || lower == "times new roman") lower = "serif";
    else if (lower == "courier" || lower == "courier new") lower = "monospace";

    auto it = fontFamilyMap_.find(lower);
    if (it != fontFamilyMap_.end()) return it->second;

    // Fallback: try partial match
    for (const auto& kv : fontFamilyMap_) {
        if (kv.first.find(lower) != std::string::npos ||
            lower.find(kv.first) != std::string::npos) {
            return kv.second;
        }
    }
    return FL_HELVETICA;
}

Fl_Font VerdadApp::boldFltkFont(Fl_Font regular) const {
    auto it = boldVariantMap_.find(regular);
    if (it != boldVariantMap_.end()) return it->second;
    return regular;  // no bold variant found; return font unchanged
}

} // namespace verdad
