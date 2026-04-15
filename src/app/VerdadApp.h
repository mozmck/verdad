#ifndef VERDAD_APP_H
#define VERDAD_APP_H

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <FL/Enumerations.H>

namespace verdad {

class SwordManager;
class SearchIndexer;
class TagManager;
class ReadingPlanManager;
class ImportedModuleManager;
class MainWindow;

/// Main application class - owns all managers and the main window
class VerdadApp {
public:
    enum class ThemeMode {
        Light,
        Dark,
    };

    struct ThemePalette {
        ThemeMode mode = ThemeMode::Light;
        Fl_Color appBackground = fl_rgb_color(0xec, 0xef, 0xf2);
        Fl_Color panelBackground = fl_rgb_color(0xfe, 0xfe, 0xfe);
        Fl_Color contentBackground = fl_rgb_color(0xfe, 0xfe, 0xfe);
        Fl_Color foreground = FL_BLACK;
        Fl_Color mutedForeground = fl_rgb_color(0x55, 0x55, 0x55);
        Fl_Color subtleForeground = fl_rgb_color(0x7b, 0x87, 0x92);
        Fl_Color border = fl_rgb_color(0xd3, 0xdd, 0xe6);
        Fl_Color selectionBackground = fl_rgb_color(0x4b, 0x7f, 0xb8);
        Fl_Color inactiveSelectionBackground = fl_rgb_color(0xa9, 0xc1, 0xde);
        Fl_Color accent = fl_rgb_color(0x1a, 0x52, 0x76);
        Fl_Color accentHover = fl_rgb_color(0x27, 0x4d, 0x68);
        Fl_Color link = fl_rgb_color(0x1a, 0x52, 0x76);
        Fl_Color linkHover = fl_rgb_color(0x27, 0x4d, 0x68);
        Fl_Color wordsOfJesus = fl_rgb_color(0xcc, 0x00, 0x00);
        Fl_Color success = fl_rgb_color(0x1f, 0x7f, 0x1f);
        Fl_Color successBackground = fl_rgb_color(0xe8, 0xf7, 0xec);
        Fl_Color warning = fl_rgb_color(0x7a, 0x5a, 0x0a);
        Fl_Color warningBackground = fl_rgb_color(0xff, 0xf3, 0xbf);
        Fl_Color danger = fl_rgb_color(0xb4, 0x50, 0x28);
        Fl_Color dangerBackground = fl_rgb_color(0xfc, 0xee, 0xe8);
        Fl_Color tagBackground = fl_rgb_color(0xe0, 0xe8, 0xf0);
        Fl_Color tagBorder = fl_rgb_color(0xc0, 0xd0, 0xe0);
        Fl_Color tagHoverBackground = fl_rgb_color(0xc0, 0xd0, 0xe0);
        Fl_Color codeBackground = fl_rgb_color(0xf2, 0xf5, 0xf7);
        Fl_Color highlightBackground = fl_rgb_color(0xff, 0xff, 0x80);
        Fl_Color highlightText = FL_BLACK;
        Fl_Color editorRule = fl_rgb_color(0x78, 0x78, 0x78);
        Fl_Color calendarCurrentMonthBackground = fl_rgb_color(0xfa, 0xfa, 0xfa);
        Fl_Color calendarOtherMonthBackground = fl_rgb_color(0xf0, 0xf0, 0xf0);
        Fl_Color calendarHeaderText = fl_rgb_color(0x55, 0x55, 0x55);
        Fl_Color calendarOtherMonthText = fl_rgb_color(0x8c, 0x8c, 0x8c);
        Fl_Color calendarGrid = fl_rgb_color(0xd2, 0xd2, 0xd2);
        Fl_Color calendarTodayOutline = fl_rgb_color(0x2e, 0x6f, 0xbf);
        Fl_Color calendarRangeOutline = fl_rgb_color(0x1e, 0x53, 0x99);
        Fl_Color calendarSelectedOutline = fl_rgb_color(0x0c, 0x3a, 0x71);
    };

    struct AppearanceSettings {
        ThemeMode themeMode = ThemeMode::Light;
        std::string appFontName = "Sans";
        int appFontSize = 14;
        std::string textFontFamily = "DejaVu Serif";
        int textFontSize = 14;
        double textLineHeight = 1.2;
        int browserLineSpacing = 0;
        int hoverDelayMs = 1000;
        int editorIndentWidth = 4;
        double editorLineHeight = 1.2;
    };

    struct PreviewDictionarySettings {
        std::string greekModule = "StrongsRealGreek";
        std::string hebrewModule = "StrongsRealHebrew";
        std::unordered_map<std::string, std::string> languageModules;
    };

    struct OptionDisplaySettings {
        bool showWordsOfChristRed = true;
        bool showStrongsMarkers = false;
        bool showMorphMarkers = false;
        bool showFootnoteMarkers = true;
        bool showCrossReferenceMarkers = true;
    };

    struct ModuleManagerSettings {
        std::string languageFilter = "en";
        int installTimeoutMillis = 0;
        bool showRemoteNetworkWarning = true;
        bool hasSelectedSources = false;
        std::vector<std::string> selectedSources;
    };

    VerdadApp();
    ~VerdadApp();

    /// Initialize the application. Returns true on success.
    bool initialize(int argc, char* argv[]);

    /// Run the application event loop. Returns exit code.
    int run();

    /// Get the SWORD manager
    SwordManager& swordManager() { return *swordMgr_; }
    const SwordManager& swordManager() const { return *swordMgr_; }

    /// Get the tag manager
    TagManager& tagManager() { return *tagMgr_; }

    /// Get the reading plan manager.
    ReadingPlanManager& readingPlanManager() { return *readingPlanMgr_; }
    const ReadingPlanManager& readingPlanManager() const { return *readingPlanMgr_; }

    /// Get the imported file/module manager.
    ImportedModuleManager& importedModuleManager() { return *importedModuleMgr_; }
    const ImportedModuleManager& importedModuleManager() const { return *importedModuleMgr_; }

    /// Get the search indexer (may be nullptr if initialization failed)
    SearchIndexer* searchIndexer() { return searchIndexer_.get(); }
    const SearchIndexer* searchIndexer() const { return searchIndexer_.get(); }

    /// Get the main window
    MainWindow* mainWindow() { return mainWindow_.get(); }

    /// Get application data directory
    std::string getDataDir() const;

    /// Get application config directory
    std::string getConfigDir() const;

    /// Get the singleton instance
    static VerdadApp* instance() { return instance_; }

    /// Refresh searchable-module metadata and queue background indexing.
    void refreshSearchIndexCatalog(bool prioritizeActiveBible = false);

    /// Current appearance settings.
    const AppearanceSettings& appearanceSettings() const { return appearanceSettings_; }

    /// Current application theme palette.
    const ThemePalette& themePalette() const { return themePalette_; }

    /// Return true when the current appearance theme is dark.
    bool isDarkTheme() const { return appearanceSettings_.themeMode == ThemeMode::Dark; }

    /// Update appearance settings and apply them immediately.
    void setAppearanceSettings(const AppearanceSettings& settings);

    /// Current Strong's preview dictionary preferences.
    const PreviewDictionarySettings& previewDictionarySettings() const {
        return previewDictionarySettings_;
    }

    /// Update default Greek/Hebrew Strong's preview dictionaries.
    void setPreviewDictionarySettings(const PreviewDictionarySettings& settings);

    /// Return the effective preview dictionary for a Strong's language prefix.
    std::string preferredPreviewDictionary(char strongPrefix) const;

    /// List installed Strong's-capable lexicons for a Greek/Hebrew prefix.
    std::vector<std::string> strongsDictionaryModules(char strongPrefix) const;

    /// List installed word dictionaries for the given source language code.
    std::vector<std::string> wordDictionaryModules(
        const std::string& languageCode) const;

    /// Return the effective default dictionary for plain-word lookups.
    std::string preferredWordDictionary(const std::string& languageCode) const;

    /// Resolve configured UI font to FLTK font enum.
    Fl_Font appFont() const;
    Fl_Font textEditorFont() const;
    Fl_Font boldTextEditorFont() const;

    /// Current Bible display option settings.
    const OptionDisplaySettings& optionDisplaySettings() const {
        return optionDisplaySettings_;
    }

    /// Update Bible display option settings and apply them immediately.
    void setOptionDisplaySettings(const OptionDisplaySettings& settings);

    /// Build runtime CSS overrides for HTML-rendered text panes.
    std::string textStyleOverrideCss() const;

    /// Current Module Manager filter preferences.
    const ModuleManagerSettings& moduleManagerSettings() const {
        return moduleManagerSettings_;
    }

    /// Update Module Manager filter preferences.
    void setModuleManagerSettings(const ModuleManagerSettings& settings);

    /// Load preferences from a specific file. When preserveLayout is true,
    /// imported window geometry, splitter sizes, and pane scroll positions
    /// are ignored in favor of the current session layout.
    bool loadPreferencesFromFile(const std::string& prefFile,
                                 bool preserveLayout = false);

    /// Save current preferences and session state to disk.
    void savePreferences();

    /// Get sorted list of available system font family names.
    const std::vector<std::string>& systemFontFamilies() const;

    /// Look up an FLTK font index by family name.  Returns FL_HELVETICA if not found.
    Fl_Font fltkFontFromFamily(const std::string& family) const;

    /// Return the bold variant of a given FLTK font index.
    Fl_Font boldFltkFont(Fl_Font regular) const;

private:
    static VerdadApp* instance_;

    std::unique_ptr<SwordManager> swordMgr_;
    std::unique_ptr<SearchIndexer> searchIndexer_;
    std::unique_ptr<TagManager> tagMgr_;
    std::unique_ptr<ReadingPlanManager> readingPlanMgr_;
    std::unique_ptr<ImportedModuleManager> importedModuleMgr_;
    std::unique_ptr<MainWindow> mainWindow_;
    AppearanceSettings appearanceSettings_;
    PreviewDictionarySettings previewDictionarySettings_;
    OptionDisplaySettings optionDisplaySettings_;
    ModuleManagerSettings moduleManagerSettings_;
    ThemePalette themePalette_;
    std::vector<std::string> systemFontFamilies_;
    /// Map from family name (lowercase) to FLTK font index
    std::unordered_map<std::string, Fl_Font> fontFamilyMap_;
    /// Map from regular font index to bold font index
    std::unordered_map<Fl_Font, Fl_Font> boldVariantMap_;

    /// Ensure config directory exists
    void ensureConfigDir();

    /// Enumerate all system fonts (called once during init)
    void enumerateSystemFonts();

    /// Load user preferences
    bool loadPreferences();

    /// Apply the configured light/dark palette to FLTK and cache theme colors.
    void applyThemePalette(ThemeMode mode);

    /// Apply parsed preference data to the running app.
    bool applyPreferencesMap(const std::unordered_map<std::string, std::string>& prefs,
                             bool preserveLayout);
};

} // namespace verdad

#endif // VERDAD_APP_H
