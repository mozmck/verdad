#include "app/VerdadApp.h"
#include "app/PlatformPaths.h"
#include "reading/ReadingPlanManager.h"
#include "sword/SwordManager.h"
#include "search/SearchIndexer.h"
#include "tags/TagManager.h"
#include "ui/MainWindow.h"
#include "ui/BiblePane.h"

#include <FL/Fl.H>
#include <FL/Fl_File_Icon.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_ask.H>

#include <cstdlib>
#include <filesystem>
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

std::string joinPath(const std::string& base, const std::string& leaf) {
    return (std::filesystem::path(base) / leaf).string();
}

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

int clampBrowserLineSpacing(int pixels) {
    return std::clamp(pixels, 0, 16);
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

bool directoryExists(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

std::vector<std::string> candidateDataDirs() {
    std::vector<std::string> dirs;

    std::string exeDir = executableDir();
    if (!exeDir.empty()) {
        namespace fs = std::filesystem;
        fs::path dir(exeDir);
#if defined(__APPLE__)
        // Inside .app bundle: Contents/MacOS/../Resources
        dirs.push_back((dir.parent_path() / "Resources").string());
#endif
        // Build tree or portable install
        dirs.push_back((dir / "data").string());
        // FHS-like install
        dirs.push_back((dir.parent_path() / "share" / "verdad").string());
    }

#if defined(__APPLE__)
    dirs.push_back("/usr/local/share/verdad");
    dirs.push_back("/opt/homebrew/share/verdad");
#elif !defined(_WIN32)
    dirs.push_back("/usr/local/share/verdad");
    dirs.push_back("/usr/share/verdad");
#endif
    dirs.push_back("./data");
    return dirs;
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
    state.dictionaryPaneHeight = parseIntOr(lookup("dictionary_pane_h"), 0);
    state.activeStudyTab = parseIntOr(lookup("active_study_tab"), 0);
    state.generalBooksTabActive = parseBoolOr(lookup("general_book_active"), false);
    state.generalBookModule = lookup("general_book_module");
    state.generalBookKey = lookup("general_book_key");
    state.documentsTabActive = parseBoolOr(lookup("documents_tab_active"), false);
    state.documentPath = lookup("document_path");
    state.dailyWorkspace.tabActive =
        parseBoolOr(lookup("daily_workspace_active"), false);
    state.dailyWorkspace.mode = DailyWorkspaceMode::Devotionals;
    state.dailyWorkspace.devotionalModule = lookup("daily_workspace_devotional_module");
    state.dailyWorkspace.readingPlanSource =
        dailyReadingPlanSourceFromToken(trimCopy(lookup("daily_workspace_plan_source")));
    state.dailyWorkspace.readingPlanId =
        parseIntOr(lookup("daily_workspace_plan_id"), 0);
    state.dailyWorkspace.swordReadingPlanModule =
        lookup("daily_workspace_sword_plan_module");

    std::string activeLegacyGeneralBookModule;
    std::string activeLegacyGeneralBookKey;
    bool activeLegacyGeneralBooksTabActive = false;
    std::string firstActiveLegacyGeneralBookModule;
    std::string firstActiveLegacyGeneralBookKey;
    std::string firstLegacyGeneralBookModule;
    std::string firstLegacyGeneralBookKey;
    int activeLegacyDictionaryPaneHeight = 0;
    int firstLegacyDictionaryPaneHeight = 0;

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
        int historyCount = std::clamp(parseIntOr(lookup("history_count"), 0), 0, 100);
        for (int h = 0; h < historyCount; ++h) {
            MainWindow::StudyHistoryEntry entry;
            entry.module = lookup("history_" + std::to_string(h) + "_module");
            entry.reference = lookup("history_" + std::to_string(h) + "_ref");
            if (!trimCopy(entry.reference).empty()) {
                tab.history.push_back(std::move(entry));
            }
        }
        tab.historyIndex = parseIntOr(lookup("history_index"),
                                      tab.history.empty()
                                          ? -1
                                          : static_cast<int>(tab.history.size()) - 1);
        int legacyDictionaryPaneHeight = parseIntOr(lookup("dictionary_pane_h"), 0);
        std::string legacyGeneralBookModule = lookup("general_book_module");
        std::string legacyGeneralBookKey = lookup("general_book_key");
        std::string legacyActiveRaw = lookup("general_book_active");
        if (legacyActiveRaw.empty()) legacyActiveRaw = lookup("dictionary_active");
        bool legacyGeneralBooksTabActive = parseBoolOr(legacyActiveRaw, false);

        if (i == state.activeStudyTab) {
            activeLegacyGeneralBookModule = legacyGeneralBookModule;
            activeLegacyGeneralBookKey = legacyGeneralBookKey;
            activeLegacyGeneralBooksTabActive = legacyGeneralBooksTabActive;
            activeLegacyDictionaryPaneHeight = legacyDictionaryPaneHeight;
        }
        if (firstActiveLegacyGeneralBookModule.empty() &&
            legacyGeneralBooksTabActive &&
            !legacyGeneralBookModule.empty()) {
            firstActiveLegacyGeneralBookModule = legacyGeneralBookModule;
            firstActiveLegacyGeneralBookKey = legacyGeneralBookKey;
        }
        if (firstLegacyGeneralBookModule.empty() && !legacyGeneralBookModule.empty()) {
            firstLegacyGeneralBookModule = legacyGeneralBookModule;
            firstLegacyGeneralBookKey = legacyGeneralBookKey;
        }
        if (firstLegacyDictionaryPaneHeight <= 0 && legacyDictionaryPaneHeight > 0) {
            firstLegacyDictionaryPaneHeight = legacyDictionaryPaneHeight;
        }

        state.studyTabs.push_back(std::move(tab));
    }

    if (state.dictionaryPaneHeight <= 0) {
        if (activeLegacyDictionaryPaneHeight > 0) {
            state.dictionaryPaneHeight = activeLegacyDictionaryPaneHeight;
        } else if (firstLegacyDictionaryPaneHeight > 0) {
            state.dictionaryPaneHeight = firstLegacyDictionaryPaneHeight;
        }
    }

    if (state.generalBookModule.empty()) {
        if (!activeLegacyGeneralBookModule.empty()) {
            state.generalBookModule = activeLegacyGeneralBookModule;
            state.generalBookKey = activeLegacyGeneralBookKey;
            state.generalBooksTabActive = activeLegacyGeneralBooksTabActive;
        } else if (!firstActiveLegacyGeneralBookModule.empty()) {
            state.generalBookModule = firstActiveLegacyGeneralBookModule;
            state.generalBookKey = firstActiveLegacyGeneralBookKey;
            state.generalBooksTabActive = true;
        } else if (!firstLegacyGeneralBookModule.empty()) {
            state.generalBookModule = firstLegacyGeneralBookModule;
            state.generalBookKey = firstLegacyGeneralBookKey;
        }
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
    imported.dictionaryPaneHeight = current.dictionaryPaneHeight;

    for (auto& tab : imported.studyTabs) {
        tab.biblePaneWidth = 0;
        tab.bibleScrollY = -1;
        tab.commentaryScrollY = -1;
    }
}

} // namespace

VerdadApp* VerdadApp::instance_ = nullptr;

VerdadApp::VerdadApp()
    : swordMgr_(std::make_unique<SwordManager>())
    , searchIndexer_(nullptr)
    , tagMgr_(std::make_unique<TagManager>())
    , readingPlanMgr_(std::make_unique<ReadingPlanManager>()) {
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
    tagMgr_->load(joinPath(getConfigDir(), "tags.db"));
    readingPlanMgr_->load(joinPath(getConfigDir(), "reading_plans.db"));

    // Initialize FTS5 index database (separate from tags/settings data).
    searchIndexer_ = std::make_unique<SearchIndexer>(
        joinPath(getConfigDir(), "module_index.db"));

    // Set up FLTK
    Fl::scheme("gtk+");
    uchar backgroundRed = 0;
    uchar backgroundGreen = 0;
    uchar backgroundBlue = 0;
    Fl::get_color(FL_LIGHT2, backgroundRed, backgroundGreen, backgroundBlue);
    Fl::background(backgroundRed, backgroundGreen, backgroundBlue);
    Fl_File_Icon::load_system_icons();
    Fl::set_fonts("-*");

    // Create main window
    mainWindow_ = std::make_unique<MainWindow>(this, 1200, 800, "Verdad Bible Study");

    // Load user preferences
    bool loadedPreferences = loadPreferences();

    // Enumerate system fonts lazily when the settings dialog needs them.
    if (!loadedPreferences) {
        // Ensure appearance is applied even when no preference file exists.
        setAppearanceSettings(appearanceSettings_);
        if (mainWindow_) {
            mainWindow_->ensureDefaultStudyTab();
        }
    }

    refreshSearchIndexCatalog(true);

    return true;
}

int VerdadApp::run() {
    if (mainWindow_) {
        mainWindow_->show();
    }
    return Fl::run();
}

void VerdadApp::refreshSearchIndexCatalog(bool prioritizeActiveBible) {
    if (!searchIndexer_ || !swordMgr_) return;

    std::vector<ModuleInfo> modules = swordMgr_->getModules();
    searchIndexer_->synchronizeModules(modules);

    std::string activeModule;
    if (prioritizeActiveBible && mainWindow_ && mainWindow_->biblePane()) {
        activeModule = trimCopy(mainWindow_->biblePane()->currentModule());
        if (!activeModule.empty()) {
            searchIndexer_->queueModuleIndex(activeModule);
        }
    }

    std::vector<std::string> searchableModules;
    searchableModules.reserve(modules.size());
    for (const auto& module : modules) {
        if (module.name.empty()) continue;
        if (!isSearchableResourceTypeToken(
                searchResourceTypeTokenForModuleType(module.type))) {
            continue;
        }
        searchableModules.push_back(module.name);
    }
    searchIndexer_->queueModuleIndex(searchableModules);
}

std::string VerdadApp::getDataDir() const {
    for (const std::string& dataDir : candidateDataDirs()) {
        if (directoryExists(dataDir)) {
            return dataDir;
        }
    }

    return "./data";
}

std::string VerdadApp::getConfigDir() const {
#ifdef _WIN32
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, path))) {
        return (std::filesystem::path(path) / "Verdad").string();
    }
    return (std::filesystem::path(".") / "verdad_config").string();
#elif defined(__APPLE__)
    const char* home = getenv("HOME");
    if (home) {
        return (std::filesystem::path(home) / "Library" / "Application Support" / "Verdad").string();
    }
    return (std::filesystem::path(".") / "verdad_config").string();
#else
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (home) {
        return (std::filesystem::path(home) / ".config" / "verdad").string();
    }
    return (std::filesystem::path(".") / ".config" / "verdad").string();
#endif
}

void VerdadApp::ensureConfigDir() {
    std::string dir = getConfigDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
}

bool VerdadApp::loadPreferences() {
    return loadPreferencesFromFile(joinPath(getConfigDir(), "preferences.conf"), false);
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
    importedAppearance.browserLineSpacing =
        clampBrowserLineSpacing(parseIntOr(lookup("browser_line_spacing"),
                                           importedAppearance.browserLineSpacing));
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
    importedOptions.showWordsOfChristRed =
        parseBoolOr(lookup("show_words_of_christ_red"),
                    importedOptions.showWordsOfChristRed);
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

    ModuleManagerSettings importedModuleManager = moduleManagerSettings_;
    {
        std::string languageFilter = trimCopy(lookup("module_manager_language"));
        if (!languageFilter.empty()) {
            importedModuleManager.languageFilter = normalizeLanguageCode(languageFilter);
        }

        auto sourceCountIt = prefs.find("module_manager_source_count");
        importedModuleManager.selectedSources.clear();
        importedModuleManager.hasSelectedSources =
            (sourceCountIt != prefs.end());
        if (sourceCountIt != prefs.end()) {
            int sourceCount = std::max(0, parseIntOr(sourceCountIt->second, 0));
            sourceCount = std::min(sourceCount, 512);
            for (int i = 0; i < sourceCount; ++i) {
                std::string value = trimCopy(
                    lookup("module_manager_source_" + std::to_string(i)));
                if (!value.empty()) {
                    importedModuleManager.selectedSources.push_back(value);
                }
            }
        }
    }

    setPreviewDictionarySettings(importedPreview);
    setOptionDisplaySettings(importedOptions);
    setAppearanceSettings(importedAppearance);
    setModuleManagerSettings(importedModuleManager);

    // New session format: restore full window/tabs/splitter state.
    if (prefs.find("study_tab_count") != prefs.end()) {
        MainWindow::SessionState state = sessionStateFromPreferences(prefs);
        if (preserveLayout && mainWindow_) {
            preserveCurrentLayout(state, mainWindow_->captureSessionState());
        }

        mainWindow_->restoreSessionState(state);
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

    return true;
}

void VerdadApp::savePreferences() {
    std::string prefFile = joinPath(getConfigDir(), "preferences.conf");
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
        file << "browser_line_spacing=" << appearanceSettings_.browserLineSpacing << "\n";
        file << "hover_delay_ms=" << appearanceSettings_.hoverDelayMs << "\n";
        file << "editor_indent_width=" << appearanceSettings_.editorIndentWidth << "\n";
        file << "editor_line_height="
             << formatPreferenceDouble(appearanceSettings_.editorLineHeight) << "\n";
        file << "show_words_of_christ_red="
             << (optionDisplaySettings_.showWordsOfChristRed ? 1 : 0) << "\n";
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
        if (!trimCopy(moduleManagerSettings_.languageFilter).empty()) {
            file << "module_manager_language="
                 << moduleManagerSettings_.languageFilter << "\n";
        }
        if (moduleManagerSettings_.hasSelectedSources) {
            file << "module_manager_source_count="
                 << moduleManagerSettings_.selectedSources.size() << "\n";
            for (size_t i = 0; i < moduleManagerSettings_.selectedSources.size(); ++i) {
                file << "module_manager_source_" << i << "="
                     << moduleManagerSettings_.selectedSources[i] << "\n";
            }
        }

        MainWindow::SessionState state = mainWindow_->captureSessionState();
        file << "window_x=" << state.windowX << "\n";
        file << "window_y=" << state.windowY << "\n";
        file << "window_w=" << state.windowW << "\n";
        file << "window_h=" << state.windowH << "\n";
        file << "left_pane_w=" << state.leftPaneWidth << "\n";
        file << "left_pane_preview_h=" << state.leftPanePreviewHeight << "\n";
        file << "dictionary_pane_h=" << state.dictionaryPaneHeight << "\n";
        file << "active_study_tab=" << state.activeStudyTab << "\n";
        file << "general_book_active=" << (state.generalBooksTabActive ? 1 : 0) << "\n";
        file << "general_book_module=" << state.generalBookModule << "\n";
        file << "general_book_key=" << state.generalBookKey << "\n";
        file << "documents_tab_active=" << (state.documentsTabActive ? 1 : 0) << "\n";
        file << "document_path=" << state.documentPath << "\n";
        file << "daily_workspace_active=" << (state.dailyWorkspace.tabActive ? 1 : 0) << "\n";
        file << "daily_workspace_mode="
             << dailyWorkspaceModeToken(state.dailyWorkspace.mode) << "\n";
        file << "daily_workspace_devotional_module="
             << state.dailyWorkspace.devotionalModule << "\n";
        file << "daily_workspace_plan_source="
             << dailyReadingPlanSourceToken(state.dailyWorkspace.readingPlanSource) << "\n";
        file << "daily_workspace_plan_id="
             << state.dailyWorkspace.readingPlanId << "\n";
        file << "daily_workspace_sword_plan_module="
             << state.dailyWorkspace.swordReadingPlanModule << "\n";
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
            file << pfx << "history_count=" << t.history.size() << "\n";
            file << pfx << "history_index=" << t.historyIndex << "\n";
            for (size_t h = 0; h < t.history.size(); ++h) {
                const auto& entry = t.history[h];
                file << pfx << "history_" << h << "_module=" << entry.module << "\n";
                file << pfx << "history_" << h << "_ref=" << entry.reference << "\n";
            }
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
    appearanceSettings_.browserLineSpacing =
        clampBrowserLineSpacing(settings.browserLineSpacing);
    appearanceSettings_.hoverDelayMs = clampHoverDelayMs(settings.hoverDelayMs);
    appearanceSettings_.editorIndentWidth =
        clampEditorIndentWidth(settings.editorIndentWidth);
    appearanceSettings_.editorLineHeight = clampLineHeight(settings.editorLineHeight);

    const Fl_Font uiFont = appFont();
    const Fl_Fontsize uiFontSize = static_cast<Fl_Fontsize>(appearanceSettings_.appFontSize);
    fl_message_font(uiFont, uiFontSize);
    Fl_Tooltip::font(uiFont);
    Fl_Tooltip::size(uiFontSize);

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

void VerdadApp::setModuleManagerSettings(
    const ModuleManagerSettings& settings) {
    ModuleManagerSettings normalized = settings;
    normalized.languageFilter = normalizeLanguageCode(normalized.languageFilter);

    std::vector<std::string> cleanedSources;
    cleanedSources.reserve(normalized.selectedSources.size());
    for (const auto& source : normalized.selectedSources) {
        std::string trimmed = trimCopy(source);
        if (trimmed.empty()) continue;
        if (std::find(cleanedSources.begin(), cleanedSources.end(), trimmed) ==
            cleanedSources.end()) {
            cleanedSources.push_back(trimmed);
        }
    }
    normalized.selectedSources = std::move(cleanedSources);

    moduleManagerSettings_ = std::move(normalized);
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

const std::vector<std::string>& VerdadApp::systemFontFamilies() const {
    if (systemFontFamilies_.empty()) {
        const_cast<VerdadApp*>(this)->enumerateSystemFonts();
    }
    return systemFontFamilies_;
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
    if (!options.showWordsOfChristRed) {
        css << ".wordsOfJesus,\n"
            << ".wordsOfJesus a,\n"
            << "span.wordsofchrist,\n"
            << "span.wordsofchrist a,\n"
            << ".wordsofchrist,\n"
            << ".wordsofchrist a,\n"
            << ".jesusWords,\n"
            << ".jesusWords a {\n"
            << "  color: inherit !important;\n"
            << "}\n";
    }
    if (options.showStrongsMarkers || options.showMorphMarkers) {
        css << "span.w { display: inline-table; vertical-align: top; text-align: center; white-space: nowrap; }\n";
        css << "span.w > span.wt { display: table-row; }\n";
    }
    if (options.showStrongsMarkers) {
        css << "span.w > span.verdad-inline-marker.strongs-marker { display: table-row; margin-left: 0; line-height: 1.05; text-align: center; }\n";
    }
    if (options.showMorphMarkers) {
        css << "span.w > span.verdad-inline-marker.morph-marker { display: table-row; margin-left: 0; line-height: 1.05; text-align: center; }\n";
        css << "span.w > span.verdad-inline-marker.morph-marker em.morph { display: inline; }\n";
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
    if (!systemFontFamilies_.empty() || !fontFamilyMap_.empty()) return;

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
    if (fontFamilyMap_.empty()) {
        const_cast<VerdadApp*>(this)->enumerateSystemFonts();
    }

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
    if (boldVariantMap_.empty() && fontFamilyMap_.empty()) {
        const_cast<VerdadApp*>(this)->enumerateSystemFonts();
    }

    auto it = boldVariantMap_.find(regular);
    if (it != boldVariantMap_.end()) return it->second;
    return regular;  // no bold variant found; return font unchanged
}

} // namespace verdad
