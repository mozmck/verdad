#include "ui/ModuleManagerDialog.h"

#include "app/VerdadApp.h"
#include "search/SearchIndexer.h"
#include "sword/SwordManager.h"
#include "ui/MainWindow.h"
#include "ui/UiFontUtils.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/fl_ask.H>

#include <installmgr.h>
#include <markupfiltmgr.h>
#include <remotetrans.h>
#include <swconfig.h>
#include <swmgr.h>
#include <swmodule.h>
#include <swversion.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <sstream>
#include <set>

namespace verdad {
namespace {

constexpr int kMargin = 10;
constexpr int kWarningHeight = 70;
constexpr int kControlHeight = 26;
constexpr int kRowSpacing = 8;
constexpr int kBottomBarHeight = 42;
constexpr int kStatusBoxHeight = 26;
constexpr int kInstallButtonWidth = 120;
constexpr int kCloseButtonWidth = 80;
constexpr long kDefaultInstallTimeoutMillis = 40000;

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

std::string upperCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    return s;
}

std::string safeText(const char* s) {
    return s ? std::string(s) : std::string();
}

char lowerAsciiChar(char c) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
}

bool startsWithNoCase(const std::string& text, const std::string& prefix) {
    if (prefix.size() > text.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (lowerAsciiChar(text[i]) != lowerAsciiChar(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool containsNoCase(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;

    auto it = std::search(haystack.begin(), haystack.end(),
                          needle.begin(), needle.end(),
                          [](char left, char right) {
                              return lowerAsciiChar(left) == lowerAsciiChar(right);
                          });
    return it != haystack.end();
}

int compareNoCase(const std::string& left, const std::string& right) {
    const size_t common = std::min(left.size(), right.size());
    for (size_t i = 0; i < common; ++i) {
        const char a = lowerAsciiChar(left[i]);
        const char b = lowerAsciiChar(right[i]);
        if (a < b) return -1;
        if (a > b) return 1;
    }

    if (left.size() < right.size()) return -1;
    if (left.size() > right.size()) return 1;
    return 0;
}

struct NoCaseLess {
    bool operator()(const std::string& left,
                    const std::string& right) const {
        return compareNoCase(left, right) < 0;
    }
};

std::string collapseWhitespaceCopy(const std::string& text) {
    std::string out;
    out.reserve(text.size());

    bool inSpace = false;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!out.empty()) inSpace = true;
            continue;
        }

        if (inSpace) {
            out.push_back(' ');
            inSpace = false;
        }
        out.push_back(c);
    }

    return trimCopy(out);
}

std::string truncateWithEllipsis(const std::string& text, size_t maxLen) {
    if (text.size() <= maxLen || maxLen < 4) return text;
    return text.substr(0, maxLen - 3) + "...";
}

bool ensureDir(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return std::filesystem::is_directory(path, ec);
}

std::string userHomeDir() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    if (!home) home = std::getenv("HOME");
#else
    const char* home = std::getenv("HOME");
#endif
    return home ? std::string(home) : std::string();
}

std::string userSwordRootDir() {
    const std::string home = userHomeDir();
    return home.empty() ? std::string() : home + "/.sword";
}

bool ensureUserSwordDataDirs() {
    const std::string root = userSwordRootDir();
    return !root.empty() &&
           ensureDir(root) &&
           ensureDir(root + "/mods.d") &&
           ensureDir(root + "/modules");
}

std::string versionOfModule(sword::SWModule* mod) {
    if (!mod) return "";
    const char* v = mod->getConfigEntry("Version");
    return v ? v : "";
}

std::string descriptionOfModule(sword::SWModule* mod) {
    if (!mod) return "";

    std::string shortPromo = collapseWhitespaceCopy(
        safeText(mod->getConfigEntry("ShortPromo")));
    if (!shortPromo.empty()) return shortPromo;

    std::string description = collapseWhitespaceCopy(safeText(mod->getDescription()));
    if (!description.empty()) return description;

    return collapseWhitespaceCopy(safeText(mod->getConfigEntry("Description")));
}

bool isBibleType(const std::string& type) {
    return startsWithNoCase(type, "Biblical Text");
}

std::string selectedChoiceLabel(const Fl_Choice* choice) {
    if (!choice) return "";
    const Fl_Menu_Item* item = choice->mvalue();
    return (item && item->label()) ? std::string(item->label()) : std::string();
}

enum class VersionRelation {
    Older,
    Same,
    Newer,
    Unknown
};

VersionRelation compareVersions(const std::string& sourceVersion,
                                const std::string& installedVersion) {
    if (sourceVersion.empty() || installedVersion.empty()) {
        return (sourceVersion == installedVersion)
            ? VersionRelation::Same
            : VersionRelation::Unknown;
    }

    sword::SWVersion source(sourceVersion.c_str());
    sword::SWVersion installed(installedVersion.c_str());
    if (source > installed) return VersionRelation::Newer;
    if (source < installed) return VersionRelation::Older;
    return VersionRelation::Same;
}

class DialogInstallStatusReporter : public sword::StatusReporter {
public:
    explicit DialogInstallStatusReporter(Fl_Box* statusBox)
        : statusBox_(statusBox) {}

    void preStatus(long totalBytes, long completedBytes,
                   const char* message) override {
        lastMessage_ = collapseWhitespaceCopy(safeText(message));
        updateLabel(static_cast<unsigned long>(std::max<long>(0, totalBytes)),
                    static_cast<unsigned long>(std::max<long>(0, completedBytes)));
    }

    void update(unsigned long totalBytes,
                unsigned long completedBytes) override {
        updateLabel(totalBytes, completedBytes);
    }

private:
    void updateLabel(unsigned long totalBytes, unsigned long completedBytes) {
        if (!statusBox_) return;

        std::string label = lastMessage_.empty()
            ? std::string("Downloading module data...")
            : lastMessage_;

        if (totalBytes > 0) {
            const int pct = std::clamp(
                static_cast<int>((completedBytes * 100UL) / totalBytes), 0, 100);
            label += " (" + std::to_string(pct) + "%)";
        }

        statusBox_->copy_label(label.c_str());
        Fl::check();
    }

    Fl_Box* statusBox_ = nullptr;
    std::string lastMessage_;
};

class VerdadInstallMgr : public sword::InstallMgr {
public:
    using sword::InstallMgr::InstallMgr;

    void resetLastTransferState() {
        lastTransferResult_ = 0;
        lastTransferSourceCaption_.clear();
        lastTransferSourceHost_.clear();
        lastTransferSourceType_.clear();
        lastTransferPath_.clear();
        lastTransferWasDirectory_ = false;
    }

    int lastTransferResult() const {
        return lastTransferResult_;
    }

    const std::string& lastTransferSourceCaption() const {
        return lastTransferSourceCaption_;
    }

    const std::string& lastTransferSourceHost() const {
        return lastTransferSourceHost_;
    }

    const std::string& lastTransferSourceType() const {
        return lastTransferSourceType_;
    }

    const std::string& lastTransferPath() const {
        return lastTransferPath_;
    }

    bool lastTransferWasDirectory() const {
        return lastTransferWasDirectory_;
    }

    int remoteCopy(sword::InstallSource* is,
                   const char* src,
                   const char* dest,
                   bool dirTransfer = false,
                   const char* suffix = "") override {
        resetLastTransferState();
        if (is) {
            lastTransferSourceCaption_ = safeText(is->caption.c_str());
            lastTransferSourceHost_ = safeText(is->source.c_str());
            lastTransferSourceType_ = safeText(is->type.c_str());
        }
        lastTransferPath_ = safeText(src);
        lastTransferWasDirectory_ = dirTransfer;
        lastTransferResult_ =
            sword::InstallMgr::remoteCopy(is, src, dest, dirTransfer, suffix);
        return lastTransferResult_;
    }

protected:
    sword::RemoteTransport* createFTPTransport(
        const char* host,
        sword::StatusReporter* statusReporter) override {
        sword::RemoteTransport* transport =
            sword::InstallMgr::createFTPTransport(host, statusReporter);
        if (transport) {
            transport->setTimeoutMillis(getTimeoutMillis());
        }
        return transport;
    }

    sword::RemoteTransport* createHTTPTransport(
        const char* host,
        sword::StatusReporter* statusReporter) override {
        sword::RemoteTransport* transport =
            sword::InstallMgr::createHTTPTransport(host, statusReporter);
        if (transport) {
            transport->setTimeoutMillis(getTimeoutMillis());
        }
        return transport;
    }

private:
    int lastTransferResult_ = 0;
    std::string lastTransferSourceCaption_;
    std::string lastTransferSourceHost_;
    std::string lastTransferSourceType_;
    std::string lastTransferPath_;
    bool lastTransferWasDirectory_ = false;
};

VerdadInstallMgr* verdadInstallMgr(sword::InstallMgr* installMgr) {
    return static_cast<VerdadInstallMgr*>(installMgr);
}

std::string joinMessageBlocks(const std::vector<std::string>& blocks) {
    std::ostringstream out;
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i > 0) out << "\n\n";
        out << blocks[i];
    }
    return out.str();
}

std::string transferErrorSummary(int rc) {
    switch (rc) {
    case -1:
        return "The requested item could not be copied from the remote source.";
    case -2:
        return "The connection failed or timed out while contacting the remote source.";
    case -3:
        return "The remote source denied access to the requested item.";
    case -4:
        return "The requested file was not found on the remote source.";
    case -5:
        return "The remote source returned a protocol error response.";
    case -9:
        return "The remote transfer failed for an unspecified network reason.";
    default:
        return "The remote transfer failed with error code " + std::to_string(rc) + ".";
    }
}

std::string transferErrorBrief(int rc) {
    switch (rc) {
    case -1:
        return "remote copy failed";
    case -2:
        return "connection failed or timed out";
    case -3:
        return "access denied";
    case -4:
        return "remote file not found";
    case -5:
        return "server returned an error";
    case -9:
        return "remote transfer failed";
    default:
        return "error " + std::to_string(rc);
    }
}

std::string installErrorBrief(int installRc,
                              const VerdadInstallMgr* installMgr) {
    const int transferRc =
        installMgr ? installMgr->lastTransferResult() : 0;
    if (transferRc != 0) {
        return transferErrorBrief(transferRc);
    }

    switch (installRc) {
    case 1:
        return "module not found in source";
    case -1:
        return "local .sword copy failed";
    case -9:
        return "install aborted";
    default:
        return installRc < 0 ? "install failed" : "local file copy failed";
    }
}

std::string refreshErrorBrief(int refreshRc,
                              const VerdadInstallMgr* installMgr) {
    const int transferRc =
        installMgr ? installMgr->lastTransferResult() : 0;
    if (transferRc != 0) {
        return transferErrorBrief(transferRc);
    }
    return refreshRc == -1
        ? std::string("source metadata could not be updated")
        : "error " + std::to_string(refreshRc);
}

std::string describeInstallFailure(const std::string& moduleName,
                                   const std::string& sourceCaption,
                                   int installRc,
                                   const VerdadInstallMgr* installMgr) {
    std::ostringstream message;
    message << "Installing " << moduleName;
    if (!sourceCaption.empty()) {
        message << " from " << sourceCaption;
    }
    message << " failed.\n\n";

    const int transferRc =
        installMgr ? installMgr->lastTransferResult() : 0;
    if (transferRc != 0) {
        message << transferErrorSummary(transferRc);
        const std::string sourceName =
            !installMgr->lastTransferSourceCaption().empty()
                ? installMgr->lastTransferSourceCaption()
                : installMgr->lastTransferSourceHost();
        if (!sourceName.empty()) {
            message << "\nSource: " << sourceName;
        }
        if (!installMgr->lastTransferSourceType().empty()) {
            message << "\nProtocol: " << installMgr->lastTransferSourceType();
        }
        if (!installMgr->lastTransferPath().empty()) {
            message << "\nRemote path: " << installMgr->lastTransferPath();
        }
        message << "\nTransfer error code: " << transferRc;
        message << "\nInstall error code: " << installRc;
        return message.str();
    }

    switch (installRc) {
    case 1:
        message << "The selected module was not found in the source metadata.";
        break;
    case -1:
        message << "The download completed, but copying the module into "
                << userSwordRootDir() << " failed.";
        break;
    case -9:
        message << "The install was aborted before completion.";
        break;
    default:
        if (installRc < 0) {
            message << "The installer failed after the download step.";
        } else {
            message << "A local file operation failed while installing the module.";
        }
        break;
    }

    message << "\nInstall error code: " << installRc;
    return message.str();
}

std::string describeRefreshFailure(const std::string& caption,
                                   int refreshRc,
                                   const VerdadInstallMgr* installMgr) {
    std::ostringstream message;
    message << "Refreshing " << caption << " failed.\n\n";

    const int transferRc =
        installMgr ? installMgr->lastTransferResult() : 0;
    if (transferRc != 0) {
        message << transferErrorSummary(transferRc);
        const std::string sourceName =
            !installMgr->lastTransferSourceCaption().empty()
                ? installMgr->lastTransferSourceCaption()
                : installMgr->lastTransferSourceHost();
        if (!sourceName.empty()) {
            message << "\nSource: " << sourceName;
        }
        if (!installMgr->lastTransferSourceType().empty()) {
            message << "\nProtocol: " << installMgr->lastTransferSourceType();
        }
        if (!installMgr->lastTransferPath().empty()) {
            message << "\nRemote path: " << installMgr->lastTransferPath();
        }
        message << "\nRefresh error code: " << refreshRc;
        return message.str();
    }

    if (refreshRc == -1) {
        message << "The source metadata could not be updated.";
    } else {
        message << "The source refresh failed with error code "
                << refreshRc << ".";
    }

    return message.str();
}

void applyInstallMgrDefaults(VerdadInstallMgr* installMgr) {
    if (!installMgr) return;
    if (installMgr->getTimeoutMillis() >= kDefaultInstallTimeoutMillis) {
        return;
    }

    installMgr->setTimeoutMillis(kDefaultInstallTimeoutMillis);
}

} // namespace

ModuleManagerDialog::ModuleManagerDialog(VerdadApp* app, int W, int H)
    : Fl_Double_Window(W, H, "Module Manager")
    , app_(app) {
    std::string home = userHomeDir();
    installMgrPath_ = home.empty() ? std::string("./InstallMgr")
                                   : home + "/.sword/InstallMgr";

    buildUi();
    initializeInstallMgr();
    refreshSources(false);
    refreshModules();
}

ModuleManagerDialog::~ModuleManagerDialog() = default;

void ModuleManagerDialog::openModal() {
    ui_font::applyCurrentAppUiFont(this);
    set_modal();
    show();
    while (shown()) {
        Fl::wait();
    }
}

void ModuleManagerDialog::resize(int X, int Y, int W, int H) {
    Fl_Double_Window::resize(X, Y, W, H);

    const int row1Y = kMargin + kWarningHeight + kRowSpacing;
    const int row2Y = row1Y + kControlHeight + kRowSpacing;
    const int row3Y = row2Y + kControlHeight + kRowSpacing;
    const int browserY = row3Y + kControlHeight + kRowSpacing;

    const int right = W - kMargin;
    const int refreshAllX = right - 110;
    const int refreshX = refreshAllX - 5 - 95;
    const int removeX = refreshX - 5 - 95;
    const int addLocalX = removeX - 5 - 100;
    const int addRemoteX = addLocalX - 5 - 110;
    const int sourceW = std::max(170, addRemoteX - 5 - 90);

    if (warningBox_) {
        warningBox_->resize(kMargin, kMargin, W - (2 * kMargin), kWarningHeight);
    }
    if (sourceChoice_) sourceChoice_->resize(90, row1Y, sourceW, kControlHeight);
    if (addRemoteButton_) addRemoteButton_->resize(addRemoteX, row1Y, 110, kControlHeight);
    if (addLocalButton_) addLocalButton_->resize(addLocalX, row1Y, 100, kControlHeight);
    if (removeButton_) removeButton_->resize(removeX, row1Y, 95, kControlHeight);
    if (refreshSourceButton_) refreshSourceButton_->resize(refreshX, row1Y, 95, kControlHeight);
    if (refreshAllButton_) refreshAllButton_->resize(refreshAllX, row1Y, 110, kControlHeight);

    if (sortChoice_) sortChoice_->resize(60, row2Y, 160, kControlHeight);
    if (languageChoice_) languageChoice_->resize(320, row2Y, 150, kControlHeight);
    if (typeChoice_) typeChoice_->resize(540, row2Y, 180, kControlHeight);

    const int clearX = W - kMargin - 85;
    const int descriptionW = std::max(180, clearX - 5 - 395);
    if (moduleFilterInput_) moduleFilterInput_->resize(85, row3Y, 220, kControlHeight);
    if (descriptionFilterInput_) {
        descriptionFilterInput_->resize(395, row3Y, descriptionW, kControlHeight);
    }
    if (clearFiltersButton_) clearFiltersButton_->resize(clearX, row3Y, 85, kControlHeight);

    if (moduleBrowser_) {
        moduleBrowser_->resize(kMargin, browserY, W - (2 * kMargin),
                               H - browserY - 74);
        updateModuleBrowserColumns();
    }

    if (statusBox_) {
        statusBox_->resize(kMargin, H - 38,
                           W - (2 * kMargin) - kInstallButtonWidth -
                               kCloseButtonWidth - 20,
                           kStatusBoxHeight);
    }
    if (installButton_) {
        installButton_->resize(W - kMargin - kCloseButtonWidth - 10 - kInstallButtonWidth,
                               H - kBottomBarHeight,
                               kInstallButtonWidth, 30);
    }
    if (closeButton_) {
        closeButton_->resize(W - kMargin - kCloseButtonWidth,
                             H - kBottomBarHeight,
                             kCloseButtonWidth, 30);
    }
}

void ModuleManagerDialog::buildUi() {
    begin();

    warningBox_ = new Fl_Box(
        kMargin, kMargin, w() - (2 * kMargin), kWarningHeight,
        "Warning: Module download sites can reveal user activity over the network.\n"
        "If you live in a persecuted country and do not want to risk detection,\n"
        "do not use remote sources. Use local module sources only.");
    warningBox_->box(FL_BORDER_BOX);
    warningBox_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);

    const int row1Y = kMargin + kWarningHeight + kRowSpacing;
    sourceChoice_ = new Fl_Choice(90, row1Y, 250, kControlHeight, "Source:");
    sourceChoice_->callback(onSourceChanged, this);

    addRemoteButton_ = new Fl_Button(350, row1Y, 110, kControlHeight, "Add Remote...");
    addRemoteButton_->callback(onAddRemote, this);

    addLocalButton_ = new Fl_Button(465, row1Y, 100, kControlHeight, "Add Local...");
    addLocalButton_->callback(onAddLocal, this);

    removeButton_ = new Fl_Button(570, row1Y, 95, kControlHeight, "Remove");
    removeButton_->callback(onRemoveSource, this);

    refreshSourceButton_ = new Fl_Button(670, row1Y, 95, kControlHeight, "Refresh");
    refreshSourceButton_->callback(onRefreshSource, this);

    refreshAllButton_ = new Fl_Button(770, row1Y, 110, kControlHeight, "Refresh All");
    refreshAllButton_->callback(onRefreshAll, this);

    const int row2Y = row1Y + kControlHeight + kRowSpacing;
    sortChoice_ = new Fl_Choice(60, row2Y, 160, kControlHeight, "Sort:");
    sortChoice_->add("Module ID");
    sortChoice_->add("Description");
    sortChoice_->add("Language");
    sortChoice_->add("Module Type");
    sortChoice_->add("Source");
    sortChoice_->add("Status");
    sortChoice_->value(0);
    sortChoice_->callback(onFilterChanged, this);

    languageChoice_ = new Fl_Choice(320, row2Y, 150, kControlHeight, "Language:");
    languageChoice_->callback(onFilterChanged, this);

    typeChoice_ = new Fl_Choice(540, row2Y, 180, kControlHeight, "Type:");
    typeChoice_->callback(onFilterChanged, this);

    const int row3Y = row2Y + kControlHeight + kRowSpacing;
    moduleFilterInput_ = new Fl_Input(85, row3Y, 220, kControlHeight, "Module ID:");
    moduleFilterInput_->when(FL_WHEN_CHANGED | FL_WHEN_ENTER_KEY_ALWAYS);
    moduleFilterInput_->callback(onFilterChanged, this);

    descriptionFilterInput_ = new Fl_Input(395, row3Y, 490, kControlHeight, "Description:");
    descriptionFilterInput_->when(FL_WHEN_CHANGED | FL_WHEN_ENTER_KEY_ALWAYS);
    descriptionFilterInput_->callback(onFilterChanged, this);

    clearFiltersButton_ = new Fl_Button(w() - 95, row3Y, 85, kControlHeight, "Clear");
    clearFiltersButton_->callback(onClearFilters, this);

    const int browserY = row3Y + kControlHeight + kRowSpacing;
    moduleBrowser_ = new Fl_Hold_Browser(10, browserY, w() - 20, h() - browserY - 74);
    moduleBrowser_->callback(onModuleSelectionChanged, this);
    updateModuleBrowserColumns();
    moduleBrowser_->add("Module ID\tDescription\tLanguage\tType\tSource\tInstalled\tAvailable\tStatus");

    statusBox_ = new Fl_Box(10, h() - 38, w() - 250, 26, "");
    statusBox_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    installButton_ = new Fl_Button(w() - 220, h() - 42, kInstallButtonWidth, 30,
                                   "Install/Update");
    installButton_->callback(onInstallUpdate, this);

    closeButton_ = new Fl_Button(w() - 90, h() - 42, kCloseButtonWidth, 30, "Close");
    closeButton_->callback(
        [](Fl_Widget* widget, void*) {
            if (widget && widget->window()) widget->window()->hide();
        }, nullptr);

    end();

    size_range(900, 520);
    resizable(moduleBrowser_);
    updateInstallButton();
    updateStatusBox();
}

void ModuleManagerDialog::initializeInstallMgr() {
    ensureUserSwordDataDirs();
    ensureDir(installMgrPath_);

    installStatusReporter_ = std::make_unique<DialogInstallStatusReporter>(statusBox_);
    installMgr_ = std::make_unique<VerdadInstallMgr>(
        installMgrPath_.c_str(), installStatusReporter_.get());
    installMgr_->setFTPPassive(true);
    // We present our own explicit warning in the UI before remote operations.
    installMgr_->setUserDisclaimerConfirmed(true);
    installMgr_->readInstallConf();
    applyInstallMgrDefaults(verdadInstallMgr(installMgr_.get()));
}

void ModuleManagerDialog::refreshSources(bool refreshRemoteContent) {
    if (!installMgr_) initializeInstallMgr();
    if (!installMgr_) return;

    sources_.clear();
    installMgr_->readInstallConf();

    for (auto it = installMgr_->sources.begin(); it != installMgr_->sources.end(); ++it) {
        sword::InstallSource* src = it->second;
        if (!src) continue;
        SourceRow row;
        row.caption = safeText(src->caption);
        row.type = safeText(src->type);
        row.source = safeText(src->source);
        row.directory = safeText(src->directory);
        row.isLocal = false;
        row.remoteSource = src;
        sources_.push_back(std::move(row));
    }

    // Include local DIR sources from InstallMgr.conf.
    std::string confFile = installMgrPath_ + "/InstallMgr.conf";
    sword::SWConfig conf(confFile.c_str());
    auto& sections = conf.getSections();
    auto secIt = sections.find("Sources");
    if (secIt != sections.end()) {
        auto begin = secIt->second.lower_bound("DIRSource");
        auto endIt = secIt->second.upper_bound("DIRSource");
        for (auto it = begin; it != endIt; ++it) {
            sword::InstallSource src("DIR", it->second.c_str());
            SourceRow row;
            row.caption = safeText(src.caption);
            row.type = "DIR";
            row.source = safeText(src.source);
            row.directory = safeText(src.directory);
            if (row.directory.empty()) row.directory = row.source;
            if (row.source.empty()) row.source = row.directory;
            row.isLocal = true;
            row.remoteSource = nullptr;
            sources_.push_back(std::move(row));
        }
    }

    std::sort(sources_.begin(), sources_.end(),
              [](const SourceRow& a, const SourceRow& b) {
                  return compareNoCase(a.caption, b.caption) < 0;
              });

    repopulateSourceChoice();

    if (refreshRemoteContent) {
        std::vector<std::string> refreshFailures;
        for (const auto& src : sources_) {
            if (src.isLocal || !src.remoteSource) continue;
            statusBox_->copy_label(("Refreshing " + src.caption + "...").c_str());
            Fl::check();
            if (auto* manager = verdadInstallMgr(installMgr_.get())) {
                manager->resetLastTransferState();
            }
            const int rc = installMgr_->refreshRemoteSource(src.remoteSource);
            if (rc != 0) {
                refreshFailures.push_back(
                    describeRefreshFailure(
                        src.caption,
                        rc,
                        verdadInstallMgr(installMgr_.get())));
            }
        }
        if (!refreshFailures.empty()) {
            fl_alert("%s", joinMessageBlocks(refreshFailures).c_str());
            statusBox_->copy_label("One or more sources failed to refresh.");
        } else {
            statusBox_->copy_label("Sources refreshed.");
        }
    }
}

void ModuleManagerDialog::refreshModules() {
    modules_.clear();

    auto assignStatus = [](ModuleRow& row, bool notInstalled, bool updateAvailable,
                           bool installed, bool wouldDowngrade,
                           const std::string& fallbackDifferentVersion) {
        row.installed = installed;
        row.updateAvailable = updateAvailable;
        row.wouldDowngrade = wouldDowngrade;

        if (notInstalled) {
            row.statusText = "Not installed";
            row.statusIcon = "+";
            row.statusSortRank = 0;
            return;
        }

        if (updateAvailable) {
            row.statusText = fallbackDifferentVersion.empty()
                ? "Update available"
                : fallbackDifferentVersion;
            row.statusIcon = "!";
            row.statusSortRank = 1;
            return;
        }

        if (wouldDowngrade) {
            row.statusText = "Installed (local newer)";
            row.statusIcon = "<";
            row.statusSortRank = 3;
            return;
        }

        row.statusText = "Installed";
        row.statusIcon = "=";
        row.statusSortRank = 2;
    };

    sword::SWMgr localMgr(new sword::MarkupFilterMgr(sword::FMT_XHTML));
    std::map<std::string, sword::SWModule*> installed;
    for (auto it = localMgr.Modules.begin(); it != localMgr.Modules.end(); ++it) {
        if (!it->second) continue;
        installed[safeText(it->first)] = it->second;
    }

    for (const auto& src : sources_) {
        if (src.isLocal) {
            std::string localPath = !src.directory.empty() ? src.directory : src.source;
            if (localPath.empty()) continue;

            sword::SWMgr sourceMgr(localPath.c_str());
            for (auto it = sourceMgr.Modules.begin(); it != sourceMgr.Modules.end(); ++it) {
                sword::SWModule* mod = it->second;
                if (!mod) continue;

                ModuleRow row;
                row.sourceCaption = src.caption;
                row.sourceType = src.type;
                row.sourcePath = localPath;
                row.moduleName = safeText(it->first);
                row.description = descriptionOfModule(mod);
                row.moduleType = safeText(mod->getType());
                row.language = safeText(mod->getLanguage());
                row.remoteSource = nullptr;
                row.isBible = isBibleType(row.moduleType);

                auto localIt = installed.find(row.moduleName);
                row.installedVersion = localIt != installed.end()
                    ? versionOfModule(localIt->second)
                    : "";
                row.availableVersion = versionOfModule(mod);

                const bool isInstalled = (localIt != installed.end());
                const VersionRelation relation = compareVersions(
                    row.availableVersion, row.installedVersion);
                const bool versionStringsDiffer =
                    !row.availableVersion.empty() &&
                    !row.installedVersion.empty() &&
                    row.availableVersion != row.installedVersion;

                assignStatus(row,
                             !isInstalled,
                             isInstalled &&
                                 (relation == VersionRelation::Newer ||
                                  (relation == VersionRelation::Unknown &&
                                   versionStringsDiffer)),
                             isInstalled,
                             isInstalled && relation == VersionRelation::Older,
                             (relation == VersionRelation::Unknown &&
                              versionStringsDiffer)
                                 ? std::string("Different version")
                                 : std::string());

                modules_.push_back(std::move(row));
            }
            continue;
        }

        if (!src.remoteSource) continue;
        sword::SWMgr* sourceMgr = src.remoteSource->getMgr();
        if (!sourceMgr) continue;

        auto statusMap = sword::InstallMgr::getModuleStatus(localMgr, *sourceMgr, false);
        for (auto it = sourceMgr->Modules.begin(); it != sourceMgr->Modules.end(); ++it) {
            sword::SWModule* mod = it->second;
            if (!mod) continue;

            ModuleRow row;
            row.sourceCaption = src.caption;
            row.sourceType = src.type;
            row.moduleName = safeText(it->first);
            row.description = descriptionOfModule(mod);
            row.moduleType = safeText(mod->getType());
            row.language = safeText(mod->getLanguage());
            row.remoteSource = src.remoteSource;
            row.isBible = isBibleType(row.moduleType);

            auto localIt = installed.find(row.moduleName);
            row.installedVersion = localIt != installed.end()
                ? versionOfModule(localIt->second)
                : "";
            row.availableVersion = versionOfModule(mod);

            int flags = 0;
            auto st = statusMap.find(mod);
            if (st != statusMap.end()) {
                flags = st->second;
            }

            const bool isNew = (flags & sword::InstallMgr::MODSTAT_NEW) != 0;
            const bool isUpdated = (flags & sword::InstallMgr::MODSTAT_UPDATED) != 0;
            const bool isSame = (flags & sword::InstallMgr::MODSTAT_SAMEVERSION) != 0;
            const bool isOlder = (flags & sword::InstallMgr::MODSTAT_OLDER) != 0;
            const VersionRelation relation = compareVersions(
                row.availableVersion, row.installedVersion);
            const bool versionStringsDiffer =
                !row.availableVersion.empty() &&
                !row.installedVersion.empty() &&
                row.availableVersion != row.installedVersion;

            assignStatus(row,
                         isNew,
                         isUpdated ||
                             (!isNew && !isSame && !isOlder &&
                              relation == VersionRelation::Newer) ||
                             (!isNew && !isSame && !isOlder &&
                              relation == VersionRelation::Unknown &&
                              versionStringsDiffer),
                         !isNew,
                         isOlder ||
                             (!isNew && !isSame && !isUpdated &&
                              relation == VersionRelation::Older),
                         (!isNew && !isSame && !isUpdated && !isOlder &&
                          relation == VersionRelation::Unknown &&
                          versionStringsDiffer)
                             ? std::string("Different version")
                             : std::string());

            modules_.push_back(std::move(row));
        }
    }

    repopulateFilterChoices();
    repopulateModuleBrowser();
}

void ModuleManagerDialog::repopulateSourceChoice() {
    if (!sourceChoice_) return;

    const std::string selected = selectedChoiceLabel(sourceChoice_);
    sourceChoice_->clear();
    sourceChoice_->add("All Sources");

    int selectedIndex = 0;
    int index = 1;
    for (const auto& src : sources_) {
        sourceChoice_->add(src.caption.c_str());
        if (!selected.empty() && src.caption == selected) {
            selectedIndex = index;
        }
        ++index;
    }

    sourceChoice_->value(selectedIndex);
    sourceChoice_->redraw();
}

void ModuleManagerDialog::repopulateFilterChoices() {
    const std::string selectedLanguage = selectedChoiceLabel(languageChoice_);
    const std::string selectedType = selectedChoiceLabel(typeChoice_);

    std::set<std::string, NoCaseLess> languages;
    std::set<std::string, NoCaseLess> types;

    for (const auto& row : modules_) {
        if (!row.language.empty()) languages.insert(row.language);
        if (!row.moduleType.empty()) types.insert(row.moduleType);
    }

    if (languageChoice_) {
        languageChoice_->clear();
        languageChoice_->add("All Languages");
        int selectedIndex = 0;
        int index = 1;
        for (const auto& language : languages) {
            languageChoice_->add(language.c_str());
            if (!selectedLanguage.empty() && language == selectedLanguage) {
                selectedIndex = index;
            }
            ++index;
        }
        languageChoice_->value(selectedIndex);
    }

    if (typeChoice_) {
        typeChoice_->clear();
        typeChoice_->add("All Types");
        int selectedIndex = 0;
        int index = 1;
        for (const auto& type : types) {
            typeChoice_->add(type.c_str());
            if (!selectedType.empty() && type == selectedType) {
                selectedIndex = index;
            }
            ++index;
        }
        typeChoice_->value(selectedIndex);
    }
}

void ModuleManagerDialog::updateModuleBrowserColumns() {
    if (!moduleBrowser_) return;

    const int tableWidth = std::max(0, moduleBrowser_->w());
    const int fixedWidth = 130 + 90 + 120 + 150 + 90 + 90 + 55;
    const int descriptionWidth = std::max(220, tableWidth - fixedWidth - 35);

    moduleBrowserColWidths_[0] = 130;
    moduleBrowserColWidths_[1] = descriptionWidth;
    moduleBrowserColWidths_[2] = 90;
    moduleBrowserColWidths_[3] = 120;
    moduleBrowserColWidths_[4] = 150;
    moduleBrowserColWidths_[5] = 90;
    moduleBrowserColWidths_[6] = 90;
    moduleBrowserColWidths_[7] = 55;
    moduleBrowserColWidths_[8] = 0;

    moduleBrowser_->column_widths(moduleBrowserColWidths_);
}

void ModuleManagerDialog::repopulateModuleBrowser() {
    if (!moduleBrowser_) return;

    const int currentIndex = selectedVisibleRow();
    std::string selectedModule;
    std::string selectedSource;
    if (currentIndex >= 0 &&
        currentIndex < static_cast<int>(modules_.size())) {
        selectedModule = modules_[currentIndex].moduleName;
        selectedSource = modules_[currentIndex].sourceCaption;
    }

    moduleBrowser_->clear();
    visibleModuleRows_.clear();
    moduleBrowser_->add(
        "Module ID\tDescription\tLanguage\tType\tSource\tInstalled\tAvailable\tStatus");

    updateModuleBrowserColumns();

    const std::string sourceFilter = selectedChoiceLabel(sourceChoice_);
    const std::string languageFilter = selectedChoiceLabel(languageChoice_);
    const std::string typeFilter = selectedChoiceLabel(typeChoice_);
    const std::string sortField = selectedChoiceLabel(sortChoice_);
    const std::string moduleFilter = trimCopy(
        moduleFilterInput_ && moduleFilterInput_->value()
            ? moduleFilterInput_->value()
            : "");
    const std::string descriptionFilter = trimCopy(
        descriptionFilterInput_ && descriptionFilterInput_->value()
            ? descriptionFilterInput_->value()
            : "");

    const bool allSources = sourceFilter.empty() || sourceFilter == "All Sources";
    const bool allLanguages = languageFilter.empty() || languageFilter == "All Languages";
    const bool allTypes = typeFilter.empty() || typeFilter == "All Types";

    std::vector<int> filteredRows;
    filteredRows.reserve(modules_.size());

    for (size_t i = 0; i < modules_.size(); ++i) {
        const ModuleRow& row = modules_[i];
        if (!allSources && row.sourceCaption != sourceFilter) continue;
        if (!allLanguages && row.language != languageFilter) continue;
        if (!allTypes && row.moduleType != typeFilter) continue;
        if (!moduleFilter.empty() && !containsNoCase(row.moduleName, moduleFilter)) continue;
        if (!descriptionFilter.empty() &&
            !containsNoCase(row.description, descriptionFilter)) {
            continue;
        }

        filteredRows.push_back(static_cast<int>(i));
    }

    std::stable_sort(filteredRows.begin(), filteredRows.end(),
                     [&](int lhs, int rhs) {
                         const ModuleRow& a = modules_[lhs];
                         const ModuleRow& b = modules_[rhs];

                         auto compareField = [](const std::string& left,
                                                const std::string& right) {
                             return compareNoCase(left, right);
                         };

                         int cmp = 0;
                         if (sortField == "Description") {
                             cmp = compareField(a.description, b.description);
                             if (cmp != 0) return cmp < 0;
                             cmp = compareField(a.moduleName, b.moduleName);
                             if (cmp != 0) return cmp < 0;
                         } else if (sortField == "Language") {
                             cmp = compareField(a.language, b.language);
                             if (cmp != 0) return cmp < 0;
                             cmp = compareField(a.moduleName, b.moduleName);
                             if (cmp != 0) return cmp < 0;
                         } else if (sortField == "Module Type") {
                             cmp = compareField(a.moduleType, b.moduleType);
                             if (cmp != 0) return cmp < 0;
                             cmp = compareField(a.language, b.language);
                             if (cmp != 0) return cmp < 0;
                             cmp = compareField(a.moduleName, b.moduleName);
                             if (cmp != 0) return cmp < 0;
                         } else if (sortField == "Source") {
                             cmp = compareField(a.sourceCaption, b.sourceCaption);
                             if (cmp != 0) return cmp < 0;
                             cmp = compareField(a.moduleName, b.moduleName);
                             if (cmp != 0) return cmp < 0;
                         } else if (sortField == "Status") {
                             if (a.statusSortRank != b.statusSortRank) {
                                 return a.statusSortRank < b.statusSortRank;
                             }
                             cmp = compareField(a.moduleName, b.moduleName);
                             if (cmp != 0) return cmp < 0;
                         } else {
                             cmp = compareField(a.moduleName, b.moduleName);
                             if (cmp != 0) return cmp < 0;
                             cmp = compareField(a.sourceCaption, b.sourceCaption);
                             if (cmp != 0) return cmp < 0;
                         }

                         return lhs < rhs;
                     });

    int selectedLine = 0;
    for (size_t i = 0; i < filteredRows.size(); ++i) {
        const int moduleIndex = filteredRows[i];
        const ModuleRow& row = modules_[moduleIndex];

        const std::string description = row.description.empty()
            ? std::string("-")
            : truncateWithEllipsis(row.description, 96);
        const std::string line = row.moduleName + "\t" +
                                 description + "\t" +
                                 (row.language.empty() ? "-" : row.language) + "\t" +
                                 (row.moduleType.empty() ? "-" : row.moduleType) + "\t" +
                                 (row.sourceCaption.empty() ? "-" : row.sourceCaption) + "\t" +
                                 (row.installedVersion.empty() ? "-" : row.installedVersion) + "\t" +
                                 (row.availableVersion.empty() ? "-" : row.availableVersion) + "\t" +
                                 row.statusIcon;
        moduleBrowser_->add(line.c_str());
        visibleModuleRows_.push_back(moduleIndex);

        if (!selectedModule.empty() &&
            row.moduleName == selectedModule &&
            row.sourceCaption == selectedSource) {
            selectedLine = static_cast<int>(i) + 2;
        }
    }

    moduleBrowser_->value(selectedLine);
    updateStatusBox();
    updateInstallButton();
    moduleBrowser_->redraw();
}

void ModuleManagerDialog::updateStatusBox() {
    if (!statusBox_) return;

    const int moduleIndex = selectedVisibleRow();
    if (moduleIndex >= 0 &&
        moduleIndex < static_cast<int>(modules_.size())) {
        const ModuleRow& row = modules_[moduleIndex];

        std::string label = row.moduleName + " | " + row.statusText;
        if (!row.language.empty()) label += " | " + row.language;
        if (!row.moduleType.empty()) label += " | " + row.moduleType;
        if (!row.installedVersion.empty() || !row.availableVersion.empty()) {
            label += " | local ";
            label += row.installedVersion.empty() ? "-" : row.installedVersion;
            label += " / source ";
            label += row.availableVersion.empty() ? "-" : row.availableVersion;
        }

        statusBox_->copy_label(label.c_str());
        return;
    }

    std::string summary = std::to_string(visibleModuleRows_.size()) +
                          " modules listed. Status: + install  ! update  = installed  < local newer";
    statusBox_->copy_label(summary.c_str());
}

void ModuleManagerDialog::updateInstallButton() {
    if (!installButton_) return;

    const int moduleIndex = selectedVisibleRow();
    if (moduleIndex < 0 ||
        moduleIndex >= static_cast<int>(modules_.size())) {
        installButton_->copy_label("Install/Update");
        installButton_->deactivate();
        return;
    }

    const ModuleRow& row = modules_[moduleIndex];
    if (row.wouldDowngrade) {
        installButton_->copy_label("Install Older");
    } else if (row.updateAvailable) {
        installButton_->copy_label("Update");
    } else if (row.installed) {
        installButton_->copy_label("Reinstall");
    } else {
        installButton_->copy_label("Install");
    }
    installButton_->activate();
}

int ModuleManagerDialog::selectedVisibleRow() const {
    if (!moduleBrowser_) return -1;
    const int line = moduleBrowser_->value();
    if (line <= 1) return -1; // Header row or no selection.
    const int idx = line - 2;
    if (idx < 0 || idx >= static_cast<int>(visibleModuleRows_.size())) return -1;
    return visibleModuleRows_[idx];
}

void ModuleManagerDialog::clearFilters() {
    if (languageChoice_) languageChoice_->value(0);
    if (typeChoice_) typeChoice_->value(0);
    if (moduleFilterInput_) moduleFilterInput_->value("");
    if (descriptionFilterInput_) descriptionFilterInput_->value("");
    repopulateModuleBrowser();
}

bool ModuleManagerDialog::confirmRemoteNetworkUse() {
    int answer = fl_choice(
        "Remote module sources can be monitored on the network.\n\n"
        "If you live in a persecuted country and do not want to risk detection,\n"
        "do not use remote sources.\n\n"
        "Continue with remote network access?",
        "Cancel", "Continue", nullptr);
    return answer == 1;
}

void ModuleManagerDialog::addRemoteSource() {
    if (!installMgr_) initializeInstallMgr();
    if (!installMgr_) return;

    const char* captionRaw = fl_input("Remote source name:", "");
    if (!captionRaw || !*captionRaw) return;
    std::string caption = trimCopy(captionRaw);
    if (caption.empty()) return;

    const char* hostRaw = fl_input("Host (example: ftp.crosswire.org):", "");
    if (!hostRaw || !*hostRaw) return;
    std::string host = trimCopy(hostRaw);
    if (host.empty()) return;

    const char* dirRaw = fl_input("Remote directory (example: /pub/sword/raw):", "/");
    if (!dirRaw || !*dirRaw) return;
    std::string dir = trimCopy(dirRaw);

    const char* protoRaw = fl_input("Protocol (FTP/HTTP/HTTPS/SFTP):", "FTP");
    if (!protoRaw || !*protoRaw) return;
    std::string proto = upperCopy(trimCopy(protoRaw));
    if (proto != "FTP" && proto != "HTTP" &&
        proto != "HTTPS" && proto != "SFTP") {
        fl_alert("Unsupported protocol.");
        return;
    }

    auto existing = installMgr_->sources.find(caption.c_str());
    if (existing != installMgr_->sources.end()) {
        delete existing->second;
        installMgr_->sources.erase(existing);
    }

    sword::InstallSource* src = new sword::InstallSource(proto.c_str());
    src->caption = caption.c_str();
    src->source = host.c_str();
    src->directory = dir.c_str();
    installMgr_->sources[src->caption] = src;
    installMgr_->saveInstallConf();

    refreshSources(false);
    refreshModules();
}

void ModuleManagerDialog::addLocalSource() {
    std::string startDir = userHomeDir().empty() ? "." : userHomeDir();
    const char* dirRaw = fl_dir_chooser("Select local module source directory",
                                        startDir.c_str());
    if (!dirRaw || !*dirRaw) return;

    std::string dir = trimCopy(dirRaw);
    if (dir.empty()) return;

    const char* captionRaw = fl_input("Local source name:", dir.c_str());
    if (!captionRaw || !*captionRaw) return;
    std::string caption = trimCopy(captionRaw);
    if (caption.empty()) return;

    std::string confFile = installMgrPath_ + "/InstallMgr.conf";
    sword::SWConfig conf(confFile.c_str());

    // Remove any existing local source with same caption.
    auto& sections = conf.getSections();
    auto secIt = sections.find("Sources");
    if (secIt != sections.end()) {
        for (auto it = secIt->second.begin(); it != secIt->second.end(); ) {
            if (safeText(it->first).find("DIRSource") != 0) {
                ++it;
                continue;
            }
            sword::InstallSource src("DIR", it->second.c_str());
            if (caption == safeText(src.caption)) {
                it = secIt->second.erase(it);
            } else {
                ++it;
            }
        }
    }

    sword::InstallSource local("DIR");
    local.caption = caption.c_str();
    local.source = dir.c_str();
    local.directory = dir.c_str();
    conf["Sources"].insert(
        std::make_pair(sword::SWBuf("DIRSource"), local.getConfEnt()));
    conf.save();

    refreshSources(false);
    refreshModules();
}

void ModuleManagerDialog::removeSelectedSource() {
    if (!sourceChoice_) return;
    const std::string caption = selectedChoiceLabel(sourceChoice_);
    if (caption.empty() || caption == "All Sources") return;

    auto srcIt = std::find_if(sources_.begin(), sources_.end(),
                              [&](const SourceRow& source) {
                                  return source.caption == caption;
                              });
    if (srcIt == sources_.end()) return;

    const std::string prompt = "Remove source '" + caption + "'?";
    if (fl_choice("%s", "Cancel", "Remove", nullptr, prompt.c_str()) != 1) {
        return;
    }

    if (srcIt->isLocal) {
        std::string confFile = installMgrPath_ + "/InstallMgr.conf";
        sword::SWConfig conf(confFile.c_str());
        auto& sections = conf.getSections();
        auto secIt = sections.find("Sources");
        if (secIt != sections.end()) {
            for (auto it = secIt->second.begin(); it != secIt->second.end(); ) {
                if (safeText(it->first).find("DIRSource") != 0) {
                    ++it;
                    continue;
                }
                sword::InstallSource src("DIR", it->second.c_str());
                if (caption == safeText(src.caption)) {
                    it = secIt->second.erase(it);
                } else {
                    ++it;
                }
            }
        }
        conf.save();
    } else if (installMgr_) {
        auto it = installMgr_->sources.find(caption.c_str());
        if (it != installMgr_->sources.end()) {
            delete it->second;
            installMgr_->sources.erase(it);
            installMgr_->saveInstallConf();
        }
    }

    refreshSources(false);
    refreshModules();
}

void ModuleManagerDialog::refreshSelectedSource() {
    if (!sourceChoice_) return;

    const std::string caption = selectedChoiceLabel(sourceChoice_);
    if (caption.empty() || caption == "All Sources") {
        refreshSources(false);
        refreshModules();
        return;
    }

    auto srcIt = std::find_if(sources_.begin(), sources_.end(),
                              [&](const SourceRow& source) {
                                  return source.caption == caption;
                              });
    if (srcIt == sources_.end()) return;

    if (!srcIt->isLocal && srcIt->remoteSource) {
        if (!confirmRemoteNetworkUse()) return;
        statusBox_->copy_label(("Refreshing " + srcIt->caption + "...").c_str());
        Fl::check();
        if (auto* manager = verdadInstallMgr(installMgr_.get())) {
            manager->resetLastTransferState();
        }
        const int rc = installMgr_->refreshRemoteSource(srcIt->remoteSource);
        if (rc != 0) {
            const std::string message =
                describeRefreshFailure(
                    srcIt->caption,
                    rc,
                    verdadInstallMgr(installMgr_.get()));
            fl_alert("%s", message.c_str());
            statusBox_->copy_label(
                ("Refresh failed: " +
                 refreshErrorBrief(rc, verdadInstallMgr(installMgr_.get()))).c_str());
        }
    }

    refreshSources(false);
    refreshModules();
}

void ModuleManagerDialog::installOrUpdateSelectedModule() {
    if (!installMgr_) initializeInstallMgr();
    if (!installMgr_) return;

    const int moduleIndex = selectedVisibleRow();
    if (moduleIndex < 0 || moduleIndex >= static_cast<int>(modules_.size())) {
        fl_alert("Select a module first.");
        return;
    }

    const ModuleRow& row = modules_[moduleIndex];
    if (row.wouldDowngrade) {
        std::string message = "Installing " + row.moduleName +
            " from " + row.sourceCaption + " will replace local version " +
            (row.installedVersion.empty() ? std::string("(unknown)") : row.installedVersion) +
            " with older version " +
            (row.availableVersion.empty() ? std::string("(unknown)") : row.availableVersion) +
            ".\n\nContinue?";
        if (fl_choice("%s", "Cancel", "Install older", nullptr, message.c_str()) != 1) {
            return;
        }
    }

    if (row.sourceType == "DIR") {
        if (row.sourcePath.empty()) {
            fl_alert("Local source path is missing.");
            return;
        }
    } else {
        if (!row.remoteSource) {
            fl_alert("Remote source is not available. Refresh the source first.");
            return;
        }
        if (!confirmRemoteNetworkUse()) return;
    }

    statusBox_->copy_label(("Installing " + row.moduleName + "...").c_str());
    Fl::check();

    if (auto* manager = verdadInstallMgr(installMgr_.get())) {
        manager->resetLastTransferState();
    }

    if (!ensureUserSwordDataDirs()) {
        fl_alert("The local SWORD folder %s could not be created.",
                 userSwordRootDir().c_str());
        statusBox_->copy_label("Install failed: local .sword folder unavailable");
        return;
    }

    const std::string localSwordRoot = userSwordRootDir();
    sword::SWMgr destMgr(localSwordRoot.c_str(),
                         true,
                         new sword::MarkupFilterMgr(sword::FMT_XHTML),
                         false,
                         false);
    int rc = 0;
    if (row.sourceType == "DIR") {
        rc = installMgr_->installModule(
            &destMgr,
            row.sourcePath.c_str(),
            row.moduleName.c_str(),
            nullptr);
    } else {
        rc = installMgr_->installModule(
            &destMgr,
            nullptr,
            row.moduleName.c_str(),
            row.remoteSource);
    }

    if (rc != 0) {
        const std::string message =
            describeInstallFailure(
                row.moduleName,
                row.sourceCaption,
                rc,
                verdadInstallMgr(installMgr_.get()));
        fl_alert("%s", message.c_str());
        statusBox_->copy_label(
            ("Install failed: " +
             installErrorBrief(rc, verdadInstallMgr(installMgr_.get()))).c_str());
        return;
    }

    // Reload installed module registry and UI panes.
    app_->swordManager().initialize();
    if (app_->mainWindow()) {
        app_->mainWindow()->refresh();
    }

    if (app_) {
        app_->refreshSearchIndexCatalog(false);
    }

    // Rebuild indexed searchable module content when needed.
    if (app_->searchIndexer() &&
        isSearchableResourceTypeToken(
            searchResourceTypeTokenForModuleType(row.moduleType))) {
        app_->searchIndexer()->queueModuleIndex(row.moduleName, true);
    }

    refreshSources(false);
    refreshModules();

    const std::string done = "Installed/updated " + row.moduleName;
    statusBox_->copy_label(done.c_str());
}

void ModuleManagerDialog::onSourceChanged(Fl_Widget* /*widget*/, void* data) {
    auto* self = static_cast<ModuleManagerDialog*>(data);
    if (!self) return;
    self->repopulateModuleBrowser();
}

void ModuleManagerDialog::onFilterChanged(Fl_Widget* /*widget*/, void* data) {
    auto* self = static_cast<ModuleManagerDialog*>(data);
    if (!self) return;
    self->repopulateModuleBrowser();
}

void ModuleManagerDialog::onClearFilters(Fl_Widget* /*widget*/, void* data) {
    auto* self = static_cast<ModuleManagerDialog*>(data);
    if (!self) return;
    self->clearFilters();
}

void ModuleManagerDialog::onModuleSelectionChanged(Fl_Widget* /*widget*/, void* data) {
    auto* self = static_cast<ModuleManagerDialog*>(data);
    if (!self) return;
    self->updateStatusBox();
    self->updateInstallButton();
}

void ModuleManagerDialog::onRefreshSource(Fl_Widget* /*widget*/, void* data) {
    auto* self = static_cast<ModuleManagerDialog*>(data);
    if (!self) return;
    self->refreshSelectedSource();
}

void ModuleManagerDialog::onRefreshAll(Fl_Widget* /*widget*/, void* data) {
    auto* self = static_cast<ModuleManagerDialog*>(data);
    if (!self) return;
    if (!self->confirmRemoteNetworkUse()) return;
    if (self->installMgr_) {
        if (auto* manager = verdadInstallMgr(self->installMgr_.get())) {
            manager->resetLastTransferState();
        }
        const int rc = self->installMgr_->refreshRemoteSourceConfiguration();
        if (rc != 0) {
            const std::string message =
                describeRefreshFailure(
                    "the remote source list",
                    rc,
                    verdadInstallMgr(self->installMgr_.get()));
            fl_alert("%s", message.c_str());
            self->statusBox_->copy_label(
                ("Refresh failed: " +
                 refreshErrorBrief(
                     rc,
                     verdadInstallMgr(self->installMgr_.get()))).c_str());
            self->refreshSources(false);
            self->refreshModules();
            return;
        }
    }
    self->refreshSources(true);
    self->refreshModules();
}

void ModuleManagerDialog::onAddRemote(Fl_Widget* /*widget*/, void* data) {
    auto* self = static_cast<ModuleManagerDialog*>(data);
    if (!self) return;
    self->addRemoteSource();
}

void ModuleManagerDialog::onAddLocal(Fl_Widget* /*widget*/, void* data) {
    auto* self = static_cast<ModuleManagerDialog*>(data);
    if (!self) return;
    self->addLocalSource();
}

void ModuleManagerDialog::onRemoveSource(Fl_Widget* /*widget*/, void* data) {
    auto* self = static_cast<ModuleManagerDialog*>(data);
    if (!self) return;
    self->removeSelectedSource();
}

void ModuleManagerDialog::onInstallUpdate(Fl_Widget* /*widget*/, void* data) {
    auto* self = static_cast<ModuleManagerDialog*>(data);
    if (!self) return;
    self->installOrUpdateSelectedModule();
}

} // namespace verdad
