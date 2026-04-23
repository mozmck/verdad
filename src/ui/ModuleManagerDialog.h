#ifndef VERDAD_MODULE_MANAGER_DIALOG_H
#define VERDAD_MODULE_MANAGER_DIALOG_H

#include <FL/Fl_Double_Window.H>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sword {
class InstallMgr;
class InstallSource;
class StatusReporter;
}

class Fl_Choice;
class Fl_Hold_Browser;
class Fl_Box;
class Fl_Button;
class Fl_Input;
class Fl_Tree;
class Fl_Tree_Item;

namespace verdad {

class VerdadApp;
class FilterableChoiceWidget;
class SourceManagerDialog;
class SourceManagerTable;

/// Basic SWORD module manager UI (sources + install/update).
class ModuleManagerDialog : public Fl_Double_Window {
public:
    ModuleManagerDialog(VerdadApp* app, int W = 980, int H = 620);
    ~ModuleManagerDialog() override;

    /// Show dialog modally.
    void openModal();

private:
    struct SourceRow {
        std::string caption;
        std::string type;      // FTP/HTTP/HTTPS/SFTP/DIR
        std::string source;    // host or local dir
        std::string directory; // remote path
        std::string username;
        std::string password;
        std::string uid;
        bool isLocal = false;
        sword::InstallSource* remoteSource = nullptr; // owned by InstallMgr
    };

    struct ModuleRow {
        std::string sourceCaption;
        std::string sourceType;
        std::string sourcePath;    // local dir for DIR sources
        std::string moduleName;
        std::string shortDescription;
        std::string aboutText;
        std::string moduleType;
        std::string language;
        std::string normalizedLanguage;
        std::string displayLanguage;
        std::string installedVersion;
        std::string availableVersion;
        std::string statusText;
        std::string statusIcon;
        bool installed = false;
        bool updateAvailable = false;
        bool wouldDowngrade = false;
        bool isBible = false;
        bool checked = false;
        int statusSortRank = 0;
        sword::InstallSource* remoteSource = nullptr; // null for local
    };

    struct TreeFilter {
        enum class Scope {
            All,
            Type,
            TypeLanguage
        };

        Scope scope = Scope::All;
        std::string moduleType;
        std::string language;
    };

    VerdadApp* app_ = nullptr;
    std::string installMgrPath_;
    std::unique_ptr<sword::StatusReporter> installStatusReporter_;
    std::unique_ptr<sword::InstallMgr> installMgr_;

    Fl_Box* warningBox_ = nullptr;
    Fl_Button* sourceFilterButton_ = nullptr;
    Fl_Tree* filterTree_ = nullptr;
    Fl_Box* languageChoiceLabel_ = nullptr;
    FilterableChoiceWidget* languageChoice_ = nullptr;
    Fl_Box* sortChoiceLabel_ = nullptr;
    Fl_Choice* sortChoice_ = nullptr;
    Fl_Box* moduleFilterLabel_ = nullptr;
    Fl_Input* moduleFilterInput_ = nullptr;
    Fl_Box* descriptionFilterLabel_ = nullptr;
    Fl_Input* descriptionFilterInput_ = nullptr;
    Fl_Hold_Browser* moduleBrowser_ = nullptr;
    Fl_Box* statusBox_ = nullptr;
    Fl_Button* clearFiltersButton_ = nullptr;
    Fl_Button* installButton_ = nullptr;
    Fl_Button* closeButton_ = nullptr;

    int moduleBrowserColWidths_[10] = {36, 130, 260, 120, 130, 150, 90, 90, 55, 0};

    std::vector<SourceRow> sources_;
    std::vector<ModuleRow> modules_;
    std::vector<int> visibleModuleRows_;
    std::vector<std::string> languageChoiceLabels_;
    std::unordered_map<std::string, std::string> languageChoiceCodesByLabel_;
    std::vector<std::string> activeSourceCaptions_;
    bool hasExplicitSourceFilter_ = false;
    bool promptedToLoadInitialSources_ = false;
    std::unordered_map<const Fl_Tree_Item*, TreeFilter> treeFilters_;
    TreeFilter treeFilter_;

    void resize(int X, int Y, int W, int H) override;
    void buildUi();
    void initializeInstallMgr();
    void maybePromptToLoadInitialSources();
    void refreshSources(bool refreshRemoteContent);
    void refreshModules();
    void repopulateLanguageChoice();
    void repopulateFilterTree();
    void repopulateModuleBrowser();
    void updateModuleBrowserColumns();
    void updateStatusBox();
    void updateInstallButton();
    void updateSourceFilterButton();
    int selectedVisibleRow() const;
    std::vector<int> checkedModuleRows() const;
    std::vector<int> selectedOrCheckedModuleRows() const;
    std::string selectedLanguageCode() const;
    bool sourceCaptionSelected(const std::string& caption) const;
    std::string moduleTooltipText(int moduleIndex) const;
    void toggleVisibleModuleChecked(int browserLine);
    bool confirmInstallPlan(const std::vector<int>& moduleIndices) const;
    void openSourceManager();
    void persistModuleManagerSettings();
    int effectiveInstallTimeoutMillis() const;
    void syncInstallMgrTimeout();
    void setInstallTimeoutMillis(int timeoutMillis, bool rememberChoice);
    void promptForInstallTimeout();
    void normalizeActiveSourceCaptions();
    void applySourceFilterSelection();
    bool rewriteSourceConfiguration();
    std::string sourceUrl(const SourceRow& row) const;
    bool applySourceUrl(SourceRow& row,
                        const std::string& text,
                        std::string* errorMessage) const;
    bool updateSourceCaption(size_t index,
                             const std::string& text,
                             std::string* errorMessage);
    bool updateSourceUrl(size_t index,
                         const std::string& text,
                         std::string* errorMessage);
    void setSourceSelected(const std::string& caption, bool selected);
    void replaceSelectedSourceCaption(const std::string& oldCaption,
                                      const std::string& newCaption);
    std::string addRemoteSourceRow();
    std::string addLocalSourceRow();
    bool removeSourceRow(size_t index);
    bool refreshSourceRow(size_t index);
    bool refreshAllSources(bool skipNetworkConfirmation = false);

    void clearFilters();
    void installOrUpdateSelectedModules();
    bool confirmRemoteNetworkUse();

    static void onChooseSources(Fl_Widget* w, void* data);
    static void onTreeSelectionChanged(Fl_Widget* w, void* data);
    static void onFilterChanged(Fl_Widget* w, void* data);
    static void onClearFilters(Fl_Widget* w, void* data);
    static void onModuleSelectionChanged(Fl_Widget* w, void* data);
    static void onInstallUpdate(Fl_Widget* w, void* data);

    friend class SourceManagerDialog;
    friend class SourceManagerTable;
};

} // namespace verdad

#endif // VERDAD_MODULE_MANAGER_DIALOG_H
