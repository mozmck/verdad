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
class MainWindow;

/// Main application class - owns all managers and the main window
class VerdadApp {
public:
    struct AppearanceSettings {
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
        bool showStrongsMarkers = false;
        bool showMorphMarkers = false;
        bool showFootnoteMarkers = true;
        bool showCrossReferenceMarkers = true;
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

    /// Current inline marker display settings.
    const OptionDisplaySettings& optionDisplaySettings() const {
        return optionDisplaySettings_;
    }

    /// Update inline marker display settings and apply them immediately.
    void setOptionDisplaySettings(const OptionDisplaySettings& settings);

    /// Build runtime CSS overrides for HTML-rendered text panes.
    std::string textStyleOverrideCss() const;

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
    std::unique_ptr<MainWindow> mainWindow_;
    AppearanceSettings appearanceSettings_;
    PreviewDictionarySettings previewDictionarySettings_;
    OptionDisplaySettings optionDisplaySettings_;
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

    /// Apply parsed preference data to the running app.
    bool applyPreferencesMap(const std::unordered_map<std::string, std::string>& prefs,
                             bool preserveLayout);
};

} // namespace verdad

#endif // VERDAD_APP_H
