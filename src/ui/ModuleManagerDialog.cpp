#include "ui/ModuleManagerDialog.h"

#include "app/VerdadApp.h"
#include "search/SearchIndexer.h"
#include "sword/SwordManager.h"
#include "sword/SwordPaths.h"
#include "ui/FilterableChoiceWidget.h"
#include "ui/MainWindow.h"
#include "ui/UiFontUtils.h"
#include "ui/WrappingChoice.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Table_Row.H>
#include <FL/Fl_Tooltip.H>
#include <FL/Fl_Tree.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

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
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <set>
#include <unordered_map>

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
constexpr int kTimeoutButtonWidth = 110;
constexpr int kDefaultInstallTimeoutMillis = 180000;
constexpr int kMinInstallTimeoutMillis = 10000;
constexpr int kMaxInstallTimeoutMillis = 900000;
constexpr int kFilterLabelWidth = 82;
constexpr int kFilterFieldGap = 6;
constexpr int kFilterGroupGap = 14;
constexpr int kCompactLabelWidth = 54;
constexpr int kSourceManagerButtonWidth = 112;
constexpr int kSourceManagerCheckboxWidth = 46;
constexpr int kSourceManagerRefreshWidth = 92;

struct RemoteNetworkWarningDialogState {
    bool accepted = false;
};

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

std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return s;
}

std::string escapeChoiceLabel(const std::string& label) {
    std::string escaped;
    escaped.reserve(label.size() + 8);
    for (char c : label) {
        if (c == '\\' || c == '/') escaped.push_back('\\');
        escaped.push_back(c);
    }
    return escaped;
}

std::string unescapeChoiceLabel(const char* label) {
    if (!label) return "";

    std::string unescaped;
    for (size_t i = 0; label[i] != '\0'; ++i) {
        if (label[i] == '\\' && label[i + 1] != '\0') {
            unescaped.push_back(label[i + 1]);
            ++i;
            continue;
        }
        unescaped.push_back(label[i]);
    }
    return unescaped;
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

std::string stripTrailingSlashCopy(std::string text) {
    while (text.size() > 1 &&
           (text.back() == '/' || text.back() == '\\')) {
        text.pop_back();
    }
    return text;
}

std::string sourceSchemeFromType(const std::string& type) {
    const std::string normalized = lowerCopy(trimCopy(type));
    if (normalized == "ftp" ||
        normalized == "http" ||
        normalized == "https" ||
        normalized == "sftp") {
        return normalized;
    }
    return "https";
}

bool isSupportedRemoteScheme(const std::string& scheme) {
    return scheme == "ftp" ||
           scheme == "http" ||
           scheme == "https" ||
           scheme == "sftp";
}

std::string fallbackSourceCaptionFromPath(const std::string& path) {
    std::filesystem::path fsPath(path);
    std::string label = fsPath.filename().string();
    if (label.empty()) label = fsPath.parent_path().filename().string();
    if (label.empty()) label = path;
    return trimCopy(label);
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
    return defaultUserSwordDataPath();
}

bool ensureUserSwordDataDirs() {
    const std::string root = userSwordRootDir();
    return !root.empty() &&
           ensureDir(root) &&
           ensureDir(root + "/mods.d") &&
           ensureDir(root + "/modules");
}

int clampInstallTimeoutMillis(int timeoutMillis) {
    return std::clamp(timeoutMillis,
                      kMinInstallTimeoutMillis,
                      kMaxInstallTimeoutMillis);
}

std::string formatInstallTimeoutButtonLabel(int timeoutMillis) {
    const int seconds = std::max(1, timeoutMillis / 1000);
    if (seconds >= 60 && (seconds % 60) == 0) {
        return "Timeout " + std::to_string(seconds / 60) + "m";
    }
    return "Timeout " + std::to_string(seconds) + "s";
}

std::string formatInstallTimeoutDescription(int timeoutMillis) {
    const int seconds = std::max(1, timeoutMillis / 1000);
    if (seconds >= 60 && (seconds % 60) == 0) {
        const int minutes = seconds / 60;
        return std::to_string(minutes) + " minute" +
               (minutes == 1 ? "" : "s");
    }
    return std::to_string(seconds) + " second" +
           (seconds == 1 ? "" : "s");
}

void persistInstallMgrTimeoutMillis(const std::string& installMgrPath,
                                    int timeoutMillis) {
    if (installMgrPath.empty()) return;

    const std::string confFile = installMgrPath + "/InstallMgr.conf";
    sword::SWConfig conf(confFile.c_str());
    const std::string value =
        std::to_string(clampInstallTimeoutMillis(timeoutMillis));
    conf["General"]["TimeoutMillis"] = value.c_str();
    conf.save();
}

std::string versionOfModule(sword::SWModule* mod) {
    if (!mod) return "";
    const char* v = mod->getConfigEntry("Version");
    return v ? v : "";
}

struct LanguageCatalog {
    std::unordered_map<std::string, std::string> displayNames;
    std::unordered_map<std::string, std::string> aliases;
};

void addLanguageDisplay(LanguageCatalog& catalog,
                        const std::string& code,
                        const std::string& name,
                        bool overwrite = true) {
    const std::string normalizedCode = lowerCopy(trimCopy(code));
    const std::string trimmedName = trimCopy(name);
    if (normalizedCode.empty() || trimmedName.empty()) return;

    auto it = catalog.displayNames.find(normalizedCode);
    if (it == catalog.displayNames.end() || overwrite) {
        catalog.displayNames[normalizedCode] = trimmedName;
    }
}

void addLanguageAlias(LanguageCatalog& catalog,
                      const std::string& from,
                      const std::string& to,
                      bool overwrite = true) {
    const std::string normalizedFrom = lowerCopy(trimCopy(from));
    const std::string normalizedTo = lowerCopy(trimCopy(to));
    if (normalizedFrom.empty() || normalizedTo.empty() ||
        normalizedFrom == normalizedTo) {
        return;
    }

    auto it = catalog.aliases.find(normalizedFrom);
    if (it == catalog.aliases.end() || overwrite) {
        catalog.aliases[normalizedFrom] = normalizedTo;
    }
}

std::string parseJsonStringField(const std::string& line,
                                 const char* key) {
    if (!key || !*key) return "";

    const std::string quotedKey = std::string("\"") + key + "\"";
    size_t keyPos = line.find(quotedKey);
    if (keyPos == std::string::npos) return "";

    size_t colon = line.find(':', keyPos + quotedKey.size());
    if (colon == std::string::npos) return "";

    size_t firstQuote = line.find('"', colon + 1);
    if (firstQuote == std::string::npos) return "";

    std::string value;
    value.reserve(line.size() - firstQuote);
    bool escaped = false;
    for (size_t i = firstQuote + 1; i < line.size(); ++i) {
        const char c = line[i];
        if (escaped) {
            switch (c) {
            case '"':
            case '\\':
            case '/':
                value.push_back(c);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                value.push_back(c);
                break;
            }
            escaped = false;
            continue;
        }

        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            return value;
        }
        value.push_back(c);
    }

    return "";
}

void loadIsoLanguageCatalogFile(LanguageCatalog& catalog,
                                const std::string& path,
                                bool captureAliases) {
    std::ifstream in(path);
    if (!in) return;

    std::string line;
    std::string alpha2;
    std::string alpha3;
    std::string name;
    bool insideEntry = false;
    while (std::getline(in, line)) {
        const std::string trimmed = trimCopy(line);
        if (trimmed == "{") {
            insideEntry = true;
            alpha2.clear();
            alpha3.clear();
            name.clear();
            continue;
        }
        if (!insideEntry) continue;
        if (trimmed == "}," || trimmed == "}") {
            addLanguageDisplay(catalog, alpha2, name, false);
            addLanguageDisplay(catalog, alpha3, name, false);
            if (captureAliases && !alpha2.empty() && !alpha3.empty()) {
                addLanguageAlias(catalog, alpha3, alpha2);
            }
            insideEntry = false;
            continue;
        }

        std::string value = parseJsonStringField(trimmed, "alpha_2");
        if (!value.empty()) {
            alpha2 = value;
            continue;
        }

        value = parseJsonStringField(trimmed, "alpha_3");
        if (!value.empty()) {
            alpha3 = value;
            continue;
        }

        value = parseJsonStringField(trimmed, "name");
        if (!value.empty()) {
            name = value;
        }
    }
}

void loadXiphosLanguageCatalog(LanguageCatalog& catalog,
                               const std::string& path) {
    std::ifstream in(path);
    if (!in) return;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        addLanguageDisplay(catalog,
                           line.substr(0, tab),
                           line.substr(tab + 1));
    }
}

const LanguageCatalog& languageCatalog() {
    static const LanguageCatalog catalog = []() {
        LanguageCatalog built;

        loadIsoLanguageCatalogFile(
            built, "/usr/share/iso-codes/json/iso_639-2.json", true);
        loadIsoLanguageCatalogFile(
            built, "/usr/share/iso-codes/json/iso_639-3.json", false);
        loadXiphosLanguageCatalog(built, "/usr/share/xiphos/languages");

        addLanguageDisplay(built, "en", "English", false);
        addLanguageDisplay(built, "es", "Spanish", false);
        addLanguageDisplay(built, "fr", "French", false);
        addLanguageDisplay(built, "de", "German", false);
        addLanguageDisplay(built, "pt", "Portuguese", false);
        addLanguageDisplay(built, "it", "Italian", false);
        addLanguageDisplay(built, "ru", "Russian", false);
        addLanguageDisplay(built, "nl", "Dutch", false);
        addLanguageDisplay(built, "el", "Greek", false);
        addLanguageDisplay(built, "grc", "Greek", false);
        addLanguageDisplay(built, "he", "Hebrew", false);
        addLanguageDisplay(built, "la", "Latin", false);
        addLanguageDisplay(built, "ar", "Arabic", false);
        addLanguageDisplay(built, "zh", "Chinese", false);
        addLanguageAlias(built, "eng", "en", false);
        addLanguageAlias(built, "spa", "es", false);
        addLanguageAlias(built, "fra", "fr", false);
        addLanguageAlias(built, "fre", "fr", false);
        addLanguageAlias(built, "deu", "de", false);
        addLanguageAlias(built, "ger", "de", false);
        addLanguageAlias(built, "por", "pt", false);
        addLanguageAlias(built, "ita", "it", false);
        addLanguageAlias(built, "rus", "ru", false);
        addLanguageAlias(built, "nld", "nl", false);
        addLanguageAlias(built, "dut", "nl", false);
        addLanguageAlias(built, "ell", "el", false);
        addLanguageAlias(built, "gre", "el", false);
        addLanguageAlias(built, "heb", "he", false);
        addLanguageAlias(built, "hbo", "he", false);
        addLanguageAlias(built, "ara", "ar", false);
        addLanguageAlias(built, "zho", "zh", false);
        addLanguageAlias(built, "chi", "zh", false);
        addLanguageAlias(built, "lat", "la", false);

        return built;
    }();

    return catalog;
}

std::string normalizeLanguageCode(const std::string& languageCode) {
    std::string code = lowerCopy(trimCopy(languageCode));
    if (code.empty()) return "";

    size_t sep = code.find_first_of("-_");
    if (sep != std::string::npos) code = code.substr(0, sep);

    const auto& aliases = languageCatalog().aliases;
    auto it = aliases.find(code);
    if (it != aliases.end()) {
        return it->second;
    }
    return code;
}

std::string languageDisplayName(const std::string& languageCode) {
    const std::string trimmed = lowerCopy(trimCopy(languageCode));
    const std::string normalized = normalizeLanguageCode(trimmed);
    const auto& displayNames = languageCatalog().displayNames;

    const auto exactIt = displayNames.find(trimmed);
    if (exactIt != displayNames.end()) return exactIt->second;

    size_t sep = trimmed.find_first_of("-_");
    if (sep != std::string::npos) {
        const std::string base = trimmed.substr(0, sep);
        const auto baseIt = displayNames.find(base);
        if (baseIt != displayNames.end()) return baseIt->second;
    }

    const auto normIt = displayNames.find(normalized);
    if (normIt != displayNames.end()) return normIt->second;

    if (normalized.empty()) return "";

    std::string label = normalized;
    std::transform(label.begin(), label.end(), label.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    return label;
}

std::string descriptionOfModule(sword::SWModule* mod) {
    if (!mod) return "";

    std::string description = collapseWhitespaceCopy(safeText(mod->getDescription()));
    if (!description.empty()) return description;

    description = collapseWhitespaceCopy(safeText(mod->getConfigEntry("Description")));
    if (!description.empty()) return description;

    return collapseWhitespaceCopy(safeText(mod->getConfigEntry("ShortPromo")));
}

std::string aboutTextOfModule(sword::SWModule* mod) {
    if (!mod) return "";

    std::string about = safeText(mod->getConfigEntry("About"));
    if (about.empty()) return "";

    std::string text;
    text.reserve(about.size());
    for (size_t i = 0; i < about.size(); ++i) {
        if (about[i] == '\\') {
            if (i + 4 < about.size() &&
                about.compare(i + 1, 4, "pard") == 0) {
                i += 4;
                continue;
            }
            if (i + 3 < about.size() &&
                about.compare(i + 1, 3, "par") == 0) {
                text.push_back('\n');
                i += 3;
                continue;
            }
            if (i + 2 < about.size() &&
                about.compare(i + 1, 2, "qc") == 0) {
                i += 2;
                continue;
            }
            if (i + 1 < about.size() &&
                (about[i + 1] == ' ' || about[i + 1] == '\n')) {
                ++i;
                continue;
            }
        }
        if (about[i] == '<') {
            while (i < about.size() && about[i] != '>') {
                ++i;
            }
            continue;
        }
        text.push_back(about[i]);
    }

    // Decode common HTML entities that may appear in SWORD config
    auto decodeEntities = [](std::string& s) {
        auto replaceAll = [&s](const std::string& from, const std::string& to) {
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
        };
        replaceAll("&amp;", "&");
        replaceAll("&lt;", "<");
        replaceAll("&gt;", ">");
        replaceAll("&quot;", "\"");
        replaceAll("&#39;", "'");
    };
    decodeEntities(text);

    return trimCopy(text);
}

bool isBibleType(const std::string& type) {
    return startsWithNoCase(type, "Biblical Text");
}

std::string selectedChoiceLabel(const Fl_Choice* choice) {
    if (!choice) return "";
    const Fl_Menu_Item* item = choice->mvalue();
    return (item && item->label()) ? unescapeChoiceLabel(item->label())
                                   : std::string();
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

class ModuleListBrowser : public Fl_Hold_Browser {
public:
    using ToggleCallback = std::function<void(int)>;
    using TooltipProvider = std::function<std::string(int)>;

    ModuleListBrowser(int X, int Y, int W, int H, const char* label = nullptr)
        : Fl_Hold_Browser(X, Y, W, H, label) {}

    void setCheckboxColumnWidth(int width) {
        checkboxColumnWidth_ = width;
    }

    void setToggleCallback(ToggleCallback cb) {
        toggleCallback_ = std::move(cb);
    }

    void setTooltipProvider(TooltipProvider provider) {
        tooltipProvider_ = std::move(provider);
    }

    void clearHoverTooltip() {
        hoveredLine_ = 0;
        tooltip(nullptr);
        Fl_Tooltip::current(nullptr);
    }

    int handle(int event) override {
        switch (event) {
        case FL_KEYDOWN:
            if (Fl::event_key() == ' ' || Fl::event_key() == FL_Enter) {
                const int line = value();
                if (line > 1 && toggleCallback_) {
                    toggleCallback_(line);
                    return 1;
                }
            }
            break;
        case FL_ENTER:
        case FL_MOVE:
            updateHoverTooltip();
            return 1;
        case FL_LEAVE:
            clearHoverTooltip();
            return 1;
        default:
            break;
        }

        const int result = Fl_Hold_Browser::handle(event);
        if (event == FL_RELEASE && Fl::event_button() == FL_LEFT_MOUSE) {
            const int localX = Fl::event_x() - x();
            const int line = value();
            if (line > 1 && localX >= 0 && localX < checkboxColumnWidth_) {
                if (toggleCallback_) toggleCallback_(line);
                return 1;
            }
        }

        return result;
    }

private:
    int lineAtEvent() const {
        // Fl_Browser_::find_item expects the window-relative event y-coordinate.
        void* item = const_cast<ModuleListBrowser*>(this)->find_item(Fl::event_y());
        return item ? lineno(item) : 0;
    }

    void updateHoverTooltip() {
        if (!tooltipProvider_) return;

        const int line = lineAtEvent();
        if (line == hoveredLine_) return;
        hoveredLine_ = line;

        const std::string tip = tooltipProvider_(line);
        if (tip.empty()) {
            tooltip(nullptr);
            Fl_Tooltip::current(nullptr);
            return;
        }

        copy_tooltip(tip.c_str());
        Fl_Tooltip::enter_area(this,
                               std::clamp(Fl::event_x() - x(), 0, std::max(0, w() - 1)),
                               std::clamp(Fl::event_y() - y(), 0, std::max(0, h() - 1)),
                               1,
                               1,
                               tooltip());
    }

    int checkboxColumnWidth_ = 36;
    int hoveredLine_ = 0;
    ToggleCallback toggleCallback_;
    TooltipProvider tooltipProvider_;
};

} // namespace

class SourceCellEditor : public Fl_Input {
public:
    SourceCellEditor(int X, int Y, int W, int H)
        : Fl_Input(X, Y, W, H) {}

    void setCancelCallback(std::function<void()> callback) {
        cancelCallback_ = std::move(callback);
    }

    int handle(int event) override {
        if ((event == FL_KEYDOWN || event == FL_SHORTCUT) &&
            Fl::event_key() == FL_Escape) {
            if (cancelCallback_) cancelCallback_();
            return 1;
        }
        return Fl_Input::handle(event);
    }

private:
    std::function<void()> cancelCallback_;
};

class SourceManagerTable;

class SourceManagerDialog : public Fl_Double_Window {
public:
    explicit SourceManagerDialog(ModuleManagerDialog* owner,
                                 int W = 860,
                                 int H = 430);

    void openModal();
    void refreshFromOwner(const std::string& preferredCaption = "");
    bool applyCellEdit(int row, int col, const std::string& text);
    void toggleFilter(int row);
    void activateRowRefresh(int row);
    void activateRemove();
    void activateAddRemote();
    void activateAddLocal();
    void activateRefreshAll();
    void activateTimeout();
    void activateClose();
    int rowIndexForCaption(const std::string& caption) const;
    void setSelectedCaption(const std::string& caption);
    void updateButtons();
    void updateTimeoutButton();
    const ModuleManagerDialog::SourceRow* rowAt(int row) const;
    std::string cellText(int row, int col) const;
    bool rowChecked(int row) const;

private:
    void resize(int X, int Y, int W, int H) override;
    void layoutControls();

    static void onAddRemote(Fl_Widget*, void* data);
    static void onAddLocal(Fl_Widget*, void* data);
    static void onRemove(Fl_Widget*, void* data);
    static void onRefreshAll(Fl_Widget*, void* data);
    static void onTimeout(Fl_Widget*, void* data);
    static void onClose(Fl_Widget*, void* data);

    ModuleManagerDialog* owner_ = nullptr;
    Fl_Button* addRemoteButton_ = nullptr;
    Fl_Button* addLocalButton_ = nullptr;
    Fl_Button* removeButton_ = nullptr;
    Fl_Button* refreshAllButton_ = nullptr;
    Fl_Button* timeoutButton_ = nullptr;
    SourceManagerTable* table_ = nullptr;
    Fl_Box* hintBox_ = nullptr;
    Fl_Button* closeButton_ = nullptr;
    std::string selectedCaption_;

    friend class SourceManagerTable;
};

class SourceManagerTable : public Fl_Table_Row {
public:
    SourceManagerTable(SourceManagerDialog* dialog, int X, int Y, int W, int H)
        : Fl_Table_Row(X, Y, W, H)
        , dialog_(dialog) {
        callback(onTableEvent, this);
        when(FL_WHEN_NOT_CHANGED | when());
        col_header(1);
        col_header_height(28);
        row_header(0);
        cols(4);
        rows(0);
        type(SELECT_SINGLE);
        row_resize(0);
        col_resize(1);
        row_height_all(28);
        col_width(0, kSourceManagerCheckboxWidth);
        col_width(1, 220);
        col_width(2, std::max(220, W - 220 - kSourceManagerCheckboxWidth -
                                       kSourceManagerRefreshWidth - 24));
        col_width(3, kSourceManagerRefreshWidth);

        editor_ = new SourceCellEditor(X, Y, 0, 0);
        editor_->hide();
        editor_->box(FL_BORDER_BOX);
        editor_->when(FL_WHEN_ENTER_KEY_ALWAYS);
        editor_->callback(onEditorCommit, this);
        editor_->setCancelCallback(
            [this]() {
                finishEditing(false);
            });
        end();
    }

    void reload() {
        if (editorVisible() && !committingEdit_) finishEditing(true);
        rows(dialog_ && dialog_->owner_
                 ? static_cast<int>(dialog_->owner_->sources_.size())
                 : 0);
        redraw();
    }

    void selectRow(int row) {
        select_all_rows(0);
        if (row >= 0 && row < rows()) {
            select_row(row, 1);
            row_position(row);
        }
        redraw();
    }

    int selectedRow() const {
        const int rowCount = const_cast<SourceManagerTable*>(this)->rows();
        for (int row = 0; row < rowCount; ++row) {
            if (const_cast<SourceManagerTable*>(this)->row_selected(row)) {
                return row;
            }
        }
        return -1;
    }

    void beginEditing(int row, int col) {
        if (!dialog_ || col < 1 || col > 2) return;
        if (!finishEditing(true)) return;

        editRow_ = row;
        editCol_ = col;
        editValue_ = dialog_->cellText(row, col);

        int X = 0;
        int Y = 0;
        int W = 0;
        int H = 0;
        if (find_cell(CONTEXT_CELL, row, col, X, Y, W, H) != 0) return;
        editor_->resize(X + 1, Y + 1, std::max(10, W - 2), std::max(10, H - 2));
        editor_->value(editValue_.c_str());
        editor_->insert_position(0, static_cast<int>(editValue_.size()));
        editor_->show();
        editor_->take_focus();
    }

    bool finishEditing(bool commit) {
        if (!editorVisible()) return true;
        if (commit && dialog_) {
            const std::string value = safeText(editor_->value());
            committingEdit_ = true;
            const bool applied = dialog_->applyCellEdit(editRow_, editCol_, value);
            committingEdit_ = false;
            if (!applied) {
                editor_->take_focus();
                return false;
            }
        }

        editor_->hide();
        editRow_ = -1;
        editCol_ = -1;
        editValue_.clear();
        return true;
    }

protected:
    void draw_cell(TableContext context,
                   int R = 0,
                   int C = 0,
                   int X = 0,
                   int Y = 0,
                   int W = 0,
                   int H = 0) override {
        switch (context) {
        case CONTEXT_STARTPAGE:
            if (editorVisible()) syncEditorBounds();
            return;

        case CONTEXT_COL_HEADER:
            drawHeaderCell(C, X, Y, W, H);
            return;

        case CONTEXT_CELL:
            if (editorVisible() && R == editRow_ && C == editCol_) return;
            drawBodyCell(R, C, X, Y, W, H);
            return;

        case CONTEXT_RC_RESIZE:
            if (editorVisible()) syncEditorBounds();
            return;

        default:
            return;
        }
    }

private:
    bool editorVisible() const {
        return editor_ && editor_->visible();
    }

    void drawHeaderCell(int col, int X, int Y, int W, int H) {
        static const char* kHeaders[] = {"Show", "Name", "URL", "Refresh"};
        fl_push_clip(X, Y, W, H);
        fl_draw_box(FL_THIN_UP_BOX, X, Y, W, H, col_header_color());
        fl_color(FL_BLACK);
        fl_font(FL_HELVETICA | FL_BOLD, 13);
        fl_draw(kHeaders[col], X + 4, Y, W - 8, H, FL_ALIGN_CENTER);
        fl_pop_clip();
    }

    void drawBodyCell(int row, int col, int X, int Y, int W, int H) {
        const auto* source = dialog_ ? dialog_->rowAt(row) : nullptr;
        const bool selected = row_selected(row) != 0;
        const Fl_Color background = selected
            ? selection_color()
            : ((row % 2) == 0 ? fl_rgb_color(247, 247, 247) : FL_WHITE);

        fl_push_clip(X, Y, W, H);
        fl_draw_box(FL_FLAT_BOX, X, Y, W, H, background);
        fl_color(fl_rgb_color(210, 210, 210));
        fl_rect(X, Y, W, H);

        if (col == 0) {
            const int boxSize = std::min(16, H - 8);
            const int boxX = X + (W - boxSize) / 2;
            const int boxY = Y + (H - boxSize) / 2;
            fl_draw_box(FL_DOWN_BOX, boxX, boxY, boxSize, boxSize, FL_WHITE);
            if (dialog_ && dialog_->rowChecked(row)) {
                fl_color(FL_DARK_GREEN);
                fl_font(FL_HELVETICA | FL_BOLD, 13);
                fl_draw("X", boxX, boxY, boxSize, boxSize, FL_ALIGN_CENTER);
            }
            fl_pop_clip();
            return;
        }

        if (col == 3) {
            const bool enabled = source && (!source->isLocal
                ? !trimCopy(source->source).empty()
                : !trimCopy(source->directory.empty()
                                ? source->source
                                : source->directory).empty());
            const Fl_Color buttonColor = enabled
                ? fl_rgb_color(230, 230, 230)
                : fl_rgb_color(240, 240, 240);
            fl_draw_box(FL_UP_BOX, X + 6, Y + 4, W - 12, H - 8, buttonColor);
            fl_color(enabled ? FL_BLACK : fl_rgb_color(140, 140, 140));
            fl_font(FL_HELVETICA, 12);
            fl_draw("Refresh", X + 6, Y + 4, W - 12, H - 8, FL_ALIGN_CENTER);
            fl_pop_clip();
            return;
        }

        const std::string text = dialog_ ? dialog_->cellText(row, col) : "";
        fl_color(FL_BLACK);
        fl_font(FL_HELVETICA, 13);
        fl_draw(text.c_str(),
                X + 6,
                Y,
                W - 12,
                H,
                FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
        fl_pop_clip();
    }

    void syncEditorBounds() {
        if (!editorVisible() || editRow_ < 0 || editCol_ < 0) return;
        int X = 0;
        int Y = 0;
        int W = 0;
        int H = 0;
        if (find_cell(CONTEXT_CELL, editRow_, editCol_, X, Y, W, H) != 0) return;
        editor_->resize(X + 1, Y + 1, std::max(10, W - 2), std::max(10, H - 2));
    }

    void handleTableEvent() {
        const int row = callback_row();
        const int col = callback_col();
        const TableContext context = callback_context();

        if (context != CONTEXT_CELL) {
            if (Fl::event() == FL_PUSH) finishEditing(true);
            return;
        }

        if (row >= 0 && dialog_ && dialog_->rowAt(row)) {
            selectRow(row);
            dialog_->setSelectedCaption(dialog_->rowAt(row)->caption);
        }

        if (Fl::event() != FL_PUSH) {
            if (dialog_) dialog_->updateButtons();
            return;
        }

        if (col == 0 && dialog_) {
            finishEditing(true);
            dialog_->toggleFilter(row);
            return;
        }

        if (col == 3 && dialog_) {
            finishEditing(true);
            dialog_->activateRowRefresh(row);
            return;
        }

        if ((col == 1 || col == 2) && Fl::event_clicks()) {
            beginEditing(row, col);
            return;
        }

        finishEditing(true);
        if (dialog_) dialog_->updateButtons();
    }

    static void onTableEvent(Fl_Widget*, void* data) {
        auto* self = static_cast<SourceManagerTable*>(data);
        if (!self) return;
        self->handleTableEvent();
    }

    static void onEditorCommit(Fl_Widget*, void* data) {
        auto* self = static_cast<SourceManagerTable*>(data);
        if (!self) return;
        self->finishEditing(true);
    }

    SourceManagerDialog* dialog_ = nullptr;
    SourceCellEditor* editor_ = nullptr;
    int editRow_ = -1;
    int editCol_ = -1;
    std::string editValue_;
    bool committingEdit_ = false;
};

SourceManagerDialog::SourceManagerDialog(ModuleManagerDialog* owner,
                                         int W,
                                         int H)
    : Fl_Double_Window(W, H, "Sources")
    , owner_(owner) {
    begin();

    addRemoteButton_ = new Fl_Button(0, 0, 0, 0, "Add Remote");
    addRemoteButton_->callback(onAddRemote, this);

    addLocalButton_ = new Fl_Button(0, 0, 0, 0, "Add Local");
    addLocalButton_->callback(onAddLocal, this);

    removeButton_ = new Fl_Button(0, 0, 0, 0, "Remove");
    removeButton_->callback(onRemove, this);

    refreshAllButton_ = new Fl_Button(0, 0, 0, 0, "Refresh All");
    refreshAllButton_->callback(onRefreshAll, this);

    timeoutButton_ = new Fl_Button(0, 0, 0, 0, "Timeout 3m");
    timeoutButton_->callback(onTimeout, this);

    table_ = new SourceManagerTable(this, 0, 0, 0, 0);

    hintBox_ = new Fl_Box(
        0, 0, 0, 0,
        "Double-click Name or URL to edit. Click a Refresh cell to reload that source.");
    hintBox_->box(FL_NO_BOX);
    hintBox_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    closeButton_ = new Fl_Button(0, 0, 0, 0, "Close");
    closeButton_->callback(onClose, this);

    end();

    size_range(620, 320);
    resizable(table_);
    layoutControls();
}

void SourceManagerDialog::openModal() {
    if (!owner_) return;
    ui_font::applyCurrentAppUiFont(this);
    refreshFromOwner(selectedCaption_);
    set_modal();
    show();
    while (shown()) {
        Fl::wait();
    }
}

void SourceManagerDialog::refreshFromOwner(const std::string& preferredCaption) {
    if (!preferredCaption.empty()) {
        selectedCaption_ = preferredCaption;
    }

    if (table_) {
        table_->reload();
        table_->selectRow(rowIndexForCaption(selectedCaption_));
    }

    updateTimeoutButton();
    updateButtons();
}

bool SourceManagerDialog::applyCellEdit(int row,
                                        int col,
                                        const std::string& text) {
    if (!owner_ || row < 0 || row >= static_cast<int>(owner_->sources_.size())) {
        return true;
    }

    std::string error;
    if (col == 1) {
        const std::string previousCaption = owner_->sources_[static_cast<size_t>(row)].caption;
        if (!owner_->updateSourceCaption(static_cast<size_t>(row), text, &error)) {
            fl_alert("%s", error.c_str());
            return false;
        }
        selectedCaption_ = trimCopy(text);
        if (selectedCaption_.empty()) selectedCaption_ = previousCaption;
        refreshFromOwner(selectedCaption_);
        return true;
    }

    if (col == 2) {
        const std::string caption = owner_->sources_[static_cast<size_t>(row)].caption;
        if (!owner_->updateSourceUrl(static_cast<size_t>(row), text, &error)) {
            fl_alert("%s", error.c_str());
            return false;
        }
        refreshFromOwner(caption);
        return true;
    }

    return true;
}

void SourceManagerDialog::toggleFilter(int row) {
    if (!owner_) return;
    const auto* source = rowAt(row);
    if (!source) return;

    owner_->setSourceSelected(source->caption,
                              !owner_->sourceCaptionSelected(source->caption));
    refreshFromOwner(source->caption);
}

void SourceManagerDialog::activateRowRefresh(int row) {
    if (!owner_ || !table_) return;
    if (!table_->finishEditing(true)) return;
    const auto* source = rowAt(row);
    if (!source) return;
    const std::string caption = source->caption;
    if (owner_->refreshSourceRow(static_cast<size_t>(row))) {
        refreshFromOwner(caption);
    }
}

void SourceManagerDialog::activateRemove() {
    if (!owner_ || !table_) return;
    if (!table_->finishEditing(true)) return;

    const int row = table_->selectedRow();
    if (row < 0) return;
    if (owner_->removeSourceRow(static_cast<size_t>(row))) {
        refreshFromOwner();
    }
}

void SourceManagerDialog::activateAddRemote() {
    if (!owner_ || !table_) return;
    if (!table_->finishEditing(true)) return;

    const std::string caption = owner_->addRemoteSourceRow();
    if (caption.empty()) return;
    refreshFromOwner(caption);
    const int row = rowIndexForCaption(caption);
    if (row >= 0) table_->beginEditing(row, 2);
}

void SourceManagerDialog::activateAddLocal() {
    if (!owner_ || !table_) return;
    if (!table_->finishEditing(true)) return;

    const std::string caption = owner_->addLocalSourceRow();
    if (caption.empty()) return;
    refreshFromOwner(caption);
}

void SourceManagerDialog::activateRefreshAll() {
    if (!owner_ || !table_) return;
    if (!table_->finishEditing(true)) return;
    if (owner_->refreshAllSources()) {
        refreshFromOwner(selectedCaption_);
    }
}

void SourceManagerDialog::activateTimeout() {
    if (!owner_ || !table_) return;
    if (!table_->finishEditing(true)) return;
    owner_->promptForInstallTimeout();
    updateTimeoutButton();
}

void SourceManagerDialog::activateClose() {
    if (!table_ || table_->finishEditing(true)) hide();
}

int SourceManagerDialog::rowIndexForCaption(const std::string& caption) const {
    if (!owner_) return -1;
    for (size_t i = 0; i < owner_->sources_.size(); ++i) {
        if (owner_->sources_[i].caption == caption) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void SourceManagerDialog::setSelectedCaption(const std::string& caption) {
    selectedCaption_ = caption;
    updateButtons();
}

void SourceManagerDialog::updateButtons() {
    if (!removeButton_ || !table_) return;
    if (table_->selectedRow() >= 0) {
        removeButton_->activate();
    } else {
        removeButton_->deactivate();
    }
}

void SourceManagerDialog::updateTimeoutButton() {
    if (!timeoutButton_ || !owner_) return;
    timeoutButton_->copy_label(
        formatInstallTimeoutButtonLabel(owner_->effectiveInstallTimeoutMillis()).c_str());
}

const ModuleManagerDialog::SourceRow* SourceManagerDialog::rowAt(int row) const {
    if (!owner_ ||
        row < 0 ||
        row >= static_cast<int>(owner_->sources_.size())) {
        return nullptr;
    }
    return &owner_->sources_[static_cast<size_t>(row)];
}

std::string SourceManagerDialog::cellText(int row, int col) const {
    const auto* source = rowAt(row);
    if (!source) return "";

    if (col == 1) return source->caption;
    if (col == 2) return owner_ ? owner_->sourceUrl(*source) : "";
    return "";
}

bool SourceManagerDialog::rowChecked(int row) const {
    const auto* source = rowAt(row);
    return source && owner_ && owner_->sourceCaptionSelected(source->caption);
}

void SourceManagerDialog::resize(int X, int Y, int W, int H) {
    Fl_Double_Window::resize(X, Y, W, H);
    layoutControls();
}

void SourceManagerDialog::layoutControls() {
    const int margin = 10;
    const int topY = margin;
    const int bottomY = h() - kBottomBarHeight;
    const int right = w() - margin;

    const int timeoutX = right - kTimeoutButtonWidth;
    const int refreshAllX = timeoutX - 6 - kSourceManagerButtonWidth;
    const int removeX = refreshAllX - 6 - 90;
    const int addLocalX = removeX - 6 - 100;
    const int addRemoteX = addLocalX - 6 - kSourceManagerButtonWidth;

    if (addRemoteButton_) addRemoteButton_->resize(addRemoteX, topY,
                                                   kSourceManagerButtonWidth,
                                                   kControlHeight);
    if (addLocalButton_) addLocalButton_->resize(addLocalX, topY, 100, kControlHeight);
    if (removeButton_) removeButton_->resize(removeX, topY, 90, kControlHeight);
    if (refreshAllButton_) refreshAllButton_->resize(refreshAllX,
                                                     topY,
                                                     kSourceManagerButtonWidth,
                                                     kControlHeight);
    if (timeoutButton_) timeoutButton_->resize(timeoutX,
                                               topY,
                                               kTimeoutButtonWidth,
                                               kControlHeight);

    const int tableY = topY + kControlHeight + kRowSpacing;
    const int tableH = std::max(120, bottomY - tableY - 8);
    if (table_) {
        table_->resize(margin, tableY, w() - (2 * margin), tableH);
        const int tableWidth = table_->w();
        const int nameWidth = std::clamp(tableWidth / 4, 170, 260);
        const int urlWidth = std::max(
            220,
            tableWidth - kSourceManagerCheckboxWidth -
                kSourceManagerRefreshWidth - nameWidth - 4);
        table_->col_width(0, kSourceManagerCheckboxWidth);
        table_->col_width(1, nameWidth);
        table_->col_width(2, urlWidth);
        table_->col_width(3, kSourceManagerRefreshWidth);
    }

    if (hintBox_) {
        hintBox_->resize(margin, bottomY, w() - (2 * margin) - kCloseButtonWidth - 10, 30);
    }
    if (closeButton_) {
        closeButton_->resize(w() - margin - kCloseButtonWidth, bottomY,
                             kCloseButtonWidth, 30);
    }
}

void SourceManagerDialog::onAddRemote(Fl_Widget*, void* data) {
    auto* self = static_cast<SourceManagerDialog*>(data);
    if (!self) return;
    self->activateAddRemote();
}

void SourceManagerDialog::onAddLocal(Fl_Widget*, void* data) {
    auto* self = static_cast<SourceManagerDialog*>(data);
    if (!self) return;
    self->activateAddLocal();
}

void SourceManagerDialog::onRemove(Fl_Widget*, void* data) {
    auto* self = static_cast<SourceManagerDialog*>(data);
    if (!self) return;
    self->activateRemove();
}

void SourceManagerDialog::onRefreshAll(Fl_Widget*, void* data) {
    auto* self = static_cast<SourceManagerDialog*>(data);
    if (!self) return;
    self->activateRefreshAll();
}

void SourceManagerDialog::onTimeout(Fl_Widget*, void* data) {
    auto* self = static_cast<SourceManagerDialog*>(data);
    if (!self) return;
    self->activateTimeout();
}

void SourceManagerDialog::onClose(Fl_Widget*, void* data) {
    auto* self = static_cast<SourceManagerDialog*>(data);
    if (!self) return;
    self->activateClose();
}

namespace {

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

bool showRemoteNetworkWarningDialog(bool* dontShowAgain) {
    if (dontShowAgain) *dontShowAgain = false;

    constexpr int kDialogW = 470;
    constexpr int kDialogH = 240;
    constexpr int kDialogPad = 16;
    constexpr int kWarningBoxH = 128;
    constexpr int kCheckboxY = 152;
    constexpr int kDialogButtonW = 90;
    constexpr int kDialogButtonH = 28;
    constexpr int kDialogButtonGap = 10;

    RemoteNetworkWarningDialogState state;
    Fl_Double_Window dialog(kDialogW, kDialogH, "Remote Download Warning");
    dialog.set_modal();
    dialog.begin();

    auto* warningBox = new Fl_Box(
        kDialogPad, kDialogPad, kDialogW - (2 * kDialogPad), kWarningBoxH,
        "Remote module sources can be monitored on the network.\n\n"
        "If you live in a persecuted country and do not want to risk detection,\n"
        "do not use remote sources.\n\n"
        "Continue with remote network access?");
    warningBox->box(FL_BORDER_BOX);
    warningBox->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);

    auto* dontShowButton = new Fl_Check_Button(
        kDialogPad, kCheckboxY, kDialogW - (2 * kDialogPad), 24,
        "Don't show this warning again");

    auto* cancelButton = new Fl_Button(
        kDialogW - (2 * kDialogPad) - (2 * kDialogButtonW) - kDialogButtonGap,
        kDialogH - kDialogPad - kDialogButtonH,
        kDialogButtonW,
        kDialogButtonH,
        "Cancel");
    cancelButton->callback(
        [](Fl_Widget* widget, void* data) {
            auto* state = static_cast<RemoteNetworkWarningDialogState*>(data);
            if (state) state->accepted = false;
            if (widget && widget->window()) widget->window()->hide();
        },
        &state);

    auto* continueButton = new Fl_Button(
        kDialogW - kDialogPad - kDialogButtonW,
        kDialogH - kDialogPad - kDialogButtonH,
        kDialogButtonW,
        kDialogButtonH,
        "Continue");
    continueButton->callback(
        [](Fl_Widget* widget, void* data) {
            auto* state = static_cast<RemoteNetworkWarningDialogState*>(data);
            if (state) state->accepted = true;
            if (widget && widget->window()) widget->window()->hide();
        },
        &state);

    dialog.end();
    ui_font::applyCurrentAppUiFont(&dialog);
    dialog.show();
    cancelButton->take_focus();
    while (dialog.shown()) {
        Fl::wait();
    }

    if (dontShowAgain && state.accepted && dontShowButton->value()) {
        *dontShowAgain = true;
    }
    return state.accepted;
}

bool showInitialSourceSetupDialog() {
    constexpr int kDialogW = 520;
    constexpr int kDialogH = 250;
    constexpr int kDialogPad = 16;
    constexpr int kMessageBoxH = 154;
    constexpr int kDialogButtonW = 130;
    constexpr int kDialogButtonH = 28;
    constexpr int kDialogButtonGap = 10;

    RemoteNetworkWarningDialogState state;
    Fl_Double_Window dialog(kDialogW, kDialogH, "Load Standard Sources");
    dialog.set_modal();
    dialog.begin();

    auto* messageBox = new Fl_Box(
        kDialogPad, kDialogPad, kDialogW - (2 * kDialogPad), kMessageBoxH,
        "No module sources are configured yet.\n\n"
        "Verdad can download the standard remote source list and refresh those "
        "sources now so the module manager is ready to use.\n\n"
        "Remote module sources can be monitored on the network. If you live in "
        "a persecuted country and do not want to risk detection, use local "
        "sources only.");
    messageBox->box(FL_BORDER_BOX);
    messageBox->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);

    auto* localOnlyButton = new Fl_Button(
        kDialogW - (2 * kDialogPad) - (2 * kDialogButtonW) - kDialogButtonGap,
        kDialogH - kDialogPad - kDialogButtonH,
        kDialogButtonW,
        kDialogButtonH,
        "Use Local Only");
    localOnlyButton->callback(
        [](Fl_Widget* widget, void* data) {
            auto* state = static_cast<RemoteNetworkWarningDialogState*>(data);
            if (state) state->accepted = false;
            if (widget && widget->window()) widget->window()->hide();
        },
        &state);

    auto* loadButton = new Fl_Button(
        kDialogW - kDialogPad - kDialogButtonW,
        kDialogH - kDialogPad - kDialogButtonH,
        kDialogButtonW,
        kDialogButtonH,
        "Load Sources");
    loadButton->callback(
        [](Fl_Widget* widget, void* data) {
            auto* state = static_cast<RemoteNetworkWarningDialogState*>(data);
            if (state) state->accepted = true;
            if (widget && widget->window()) widget->window()->hide();
        },
        &state);

    dialog.end();
    ui_font::applyCurrentAppUiFont(&dialog);
    dialog.show();
    localOnlyButton->take_focus();
    while (dialog.shown()) {
        Fl::wait();
    }

    return state.accepted;
}

} // namespace

ModuleManagerDialog::ModuleManagerDialog(VerdadApp* app, int W, int H)
    : Fl_Double_Window(W, H, "Module Manager")
    , app_(app) {
    const std::string swordRoot = userSwordRootDir();
    installMgrPath_ = swordRoot.empty() ? std::string("./InstallMgr")
                                        : swordRoot + "/InstallMgr";

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
    maybePromptToLoadInitialSources();
    while (shown()) {
        Fl::wait();
    }
}

void ModuleManagerDialog::resize(int X, int Y, int W, int H) {
    Fl_Double_Window::resize(X, Y, W, H);

    const int row1Y = kMargin + kWarningHeight + kRowSpacing;
    const int contentY = row1Y + kControlHeight + kRowSpacing;
    const int treeW = std::clamp((W - (2 * kMargin)) / 4, 210, 280);
    const int rightX = kMargin + treeW + kRowSpacing;
    const int rightW = std::max(320, W - rightX - kMargin);
    const int row2Y = contentY;
    const int row3Y = row2Y + kControlHeight + kRowSpacing;
    const int browserY = row3Y + kControlHeight + kRowSpacing;
    const int contentH = std::max(120, H - contentY - 74);

    if (warningBox_) {
        warningBox_->resize(kMargin, kMargin, W - (2 * kMargin), kWarningHeight);
    }
    if (sourceFilterButton_) {
        sourceFilterButton_->resize(kMargin, row1Y, 180, kControlHeight);
    }

    if (filterTree_) {
        filterTree_->resize(kMargin, contentY, treeW, contentH);
    }

    const int sortX = rightX + rightW - 160;
    const int sortLabelX = sortX - kCompactLabelWidth - kFilterFieldGap;
    const int languageLabelX = rightX;
    const int languageX = languageLabelX + kFilterLabelWidth + kFilterFieldGap;
    const int languageW = std::max(140, sortLabelX - kFilterGroupGap - languageX);
    if (languageChoiceLabel_) {
        languageChoiceLabel_->resize(languageLabelX,
                                     row2Y,
                                     kFilterLabelWidth,
                                     kControlHeight);
    }
    if (languageChoice_) {
        languageChoice_->resize(languageX, row2Y, languageW, kControlHeight);
    }
    if (sortChoiceLabel_) {
        sortChoiceLabel_->resize(sortLabelX, row2Y, kCompactLabelWidth, kControlHeight);
    }
    if (sortChoice_) sortChoice_->resize(sortX, row2Y, 160, kControlHeight);

    const int clearX = rightX + rightW - 85;
    const int moduleLabelX = rightX;
    const int moduleInputX = moduleLabelX + kFilterLabelWidth + kFilterFieldGap;
    const int moduleInputW = 100;
    const int descriptionLabelX = moduleInputX + moduleInputW + kFilterGroupGap;
    const int descriptionInputX =
        descriptionLabelX + kFilterLabelWidth + kFilterFieldGap;
    const int descriptionW = std::max(150, clearX - 5 - descriptionInputX);
    if (moduleFilterLabel_) {
        moduleFilterLabel_->resize(moduleLabelX, row3Y, kFilterLabelWidth, kControlHeight);
    }
    if (moduleFilterInput_) {
        moduleFilterInput_->resize(moduleInputX, row3Y, moduleInputW, kControlHeight);
    }
    if (descriptionFilterLabel_) {
        descriptionFilterLabel_->resize(descriptionLabelX,
                                        row3Y,
                                        kFilterLabelWidth,
                                        kControlHeight);
    }
    if (descriptionFilterInput_) {
        descriptionFilterInput_->resize(descriptionInputX,
                                        row3Y,
                                        descriptionW,
                                        kControlHeight);
    }
    if (clearFiltersButton_) clearFiltersButton_->resize(clearX, row3Y, 85, kControlHeight);

    if (moduleBrowser_) {
        moduleBrowser_->resize(rightX, browserY, rightW, H - browserY - 74);
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
    sourceFilterButton_ = new Fl_Button(kMargin, row1Y, 180, kControlHeight, "All Sources");
    sourceFilterButton_->callback(onChooseSources, this);

    const int contentY = row1Y + kControlHeight + kRowSpacing;
    filterTree_ = new Fl_Tree(kMargin, contentY, 230, h() - contentY - 74);
    filterTree_->showroot(0);
    filterTree_->selectmode(FL_TREE_SELECT_SINGLE);
    filterTree_->item_reselect_mode(FL_TREE_SELECTABLE_ALWAYS);
    filterTree_->callback(onTreeSelectionChanged, this);
    filterTree_->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);

    const int rightX = kMargin + 230 + kRowSpacing;
    const int row2Y = contentY;
    languageChoiceLabel_ = new Fl_Box(rightX, row2Y, kFilterLabelWidth, kControlHeight,
                                      "Language:");
    languageChoiceLabel_->box(FL_NO_BOX);
    languageChoiceLabel_->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);

    sortChoiceLabel_ = new Fl_Box(rightX + 494, row2Y, kCompactLabelWidth, kControlHeight,
                                  "Sort:");
    sortChoiceLabel_->box(FL_NO_BOX);
    sortChoiceLabel_->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);

    sortChoice_ = new WrappingChoice(rightX + 554, row2Y, 160, kControlHeight);
    sortChoice_->add("Module ID");
    sortChoice_->add("Description");
    sortChoice_->add("Language");
    sortChoice_->add("Module Type");
    sortChoice_->add("Source");
    sortChoice_->add("Status");
    sortChoice_->value(0);
    sortChoice_->callback(onFilterChanged, this);

    languageChoice_ = new FilterableChoiceWidget(rightX + 88, row2Y, 330, kControlHeight);
    languageChoice_->setShowAllWhenFilterEmpty(true);
    languageChoice_->setNoMatchesLabel("No language matches");
    languageChoice_->callback(onFilterChanged, this);

    const int row3Y = row2Y + kControlHeight + kRowSpacing;
    moduleFilterLabel_ = new Fl_Box(rightX, row3Y, kFilterLabelWidth, kControlHeight,
                                    "Module ID:");
    moduleFilterLabel_->box(FL_NO_BOX);
    moduleFilterLabel_->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);

    moduleFilterInput_ = new Fl_Input(rightX + kFilterLabelWidth + kFilterFieldGap,
                                      row3Y,
                                      100,
                                      kControlHeight);
    moduleFilterInput_->when(FL_WHEN_CHANGED | FL_WHEN_ENTER_KEY_ALWAYS);
    moduleFilterInput_->callback(onFilterChanged, this);

    const int descriptionLabelX =
        rightX + kFilterLabelWidth + kFilterFieldGap + 100 + kFilterGroupGap;
    descriptionFilterLabel_ = new Fl_Box(descriptionLabelX,
                                         row3Y,
                                         kFilterLabelWidth,
                                         kControlHeight,
                                         "Description:");
    descriptionFilterLabel_->box(FL_NO_BOX);
    descriptionFilterLabel_->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);

    descriptionFilterInput_ = new Fl_Input(
        descriptionLabelX + kFilterLabelWidth + kFilterFieldGap,
        row3Y,
        250,
        kControlHeight);
    descriptionFilterInput_->when(FL_WHEN_CHANGED | FL_WHEN_ENTER_KEY_ALWAYS);
    descriptionFilterInput_->callback(onFilterChanged, this);

    clearFiltersButton_ = new Fl_Button(rightX + 595, row3Y, 85, kControlHeight, "Clear");
    clearFiltersButton_->callback(onClearFilters, this);

    const int browserY = row3Y + kControlHeight + kRowSpacing;
    auto* moduleBrowser = new ModuleListBrowser(
        rightX, browserY, w() - rightX - kMargin, h() - browserY - 74);
    moduleBrowser_ = moduleBrowser;
    moduleBrowser_->callback(onModuleSelectionChanged, this);
    moduleBrowser->setCheckboxColumnWidth(moduleBrowserColWidths_[0]);
    moduleBrowser->setToggleCallback(
        [this](int line) {
            toggleVisibleModuleChecked(line);
        });
    moduleBrowser->setTooltipProvider(
        [this](int line) -> std::string {
            if (line <= 1) return "";
            const int index = line - 2;
            if (index < 0 ||
                index >= static_cast<int>(visibleModuleRows_.size())) {
                return "";
            }
            return moduleTooltipText(visibleModuleRows_[static_cast<size_t>(index)]);
        });
    updateModuleBrowserColumns();
    moduleBrowser_->add("@bSel\t@bModule ID\t@bDescription\t@bLanguage\t@bType\t@bSource\t@bInstalled\t@bAvailable\t@bStatus");

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
    resizable(this);
    updateSourceFilterButton();
    updateInstallButton();
    updateStatusBox();
}

int ModuleManagerDialog::effectiveInstallTimeoutMillis() const {
    if (app_) {
        const int configured = app_->moduleManagerSettings().installTimeoutMillis;
        if (configured > 0) return clampInstallTimeoutMillis(configured);
    }

    const int currentTimeout = installMgr_
        ? clampInstallTimeoutMillis(static_cast<int>(installMgr_->getTimeoutMillis()))
        : 0;
    return std::max(currentTimeout, kDefaultInstallTimeoutMillis);
}

void ModuleManagerDialog::syncInstallMgrTimeout() {
    if (installMgr_) {
        installMgr_->setTimeoutMillis(effectiveInstallTimeoutMillis());
    }
}

void ModuleManagerDialog::setInstallTimeoutMillis(int timeoutMillis,
                                                  bool rememberChoice) {
    const int effectiveTimeout = rememberChoice
        ? clampInstallTimeoutMillis(timeoutMillis)
        : kDefaultInstallTimeoutMillis;

    if (app_) {
        VerdadApp::ModuleManagerSettings settings = app_->moduleManagerSettings();
        settings.installTimeoutMillis = rememberChoice ? effectiveTimeout : 0;
        app_->setModuleManagerSettings(settings);
        app_->savePreferences();
    }

    if (installMgr_ && installMgr_->installConf) {
        const std::string value = std::to_string(effectiveTimeout);
        (*installMgr_->installConf)["General"]["TimeoutMillis"] = value.c_str();
        installMgr_->saveInstallConf();
        installMgr_->setTimeoutMillis(effectiveTimeout);
    } else {
        persistInstallMgrTimeoutMillis(installMgrPath_, effectiveTimeout);
    }
    if (statusBox_) {
        statusBox_->copy_label(
            ("Module download timeout set to " +
             formatInstallTimeoutDescription(effectiveTimeout) + ".").c_str());
    }
}

void ModuleManagerDialog::promptForInstallTimeout() {
    const int currentSeconds = std::max(1, effectiveInstallTimeoutMillis() / 1000);
    const int defaultSeconds = kDefaultInstallTimeoutMillis / 1000;

    std::ostringstream prompt;
    prompt << "Download timeout in seconds.\n"
           << "Leave blank to use the default (" << defaultSeconds << " seconds).";

    const std::string currentValue = std::to_string(currentSeconds);
    const char* raw = fl_input("%s",
                               currentValue.c_str(),
                               prompt.str().c_str());
    if (!raw) return;

    std::string text = trimCopy(raw);
    if (text.empty()) {
        setInstallTimeoutMillis(kDefaultInstallTimeoutMillis, false);
        return;
    }

    char* end = nullptr;
    const long seconds = std::strtol(text.c_str(), &end, 10);
    while (end && *end != '\0' &&
           std::isspace(static_cast<unsigned char>(*end))) {
        ++end;
    }

    const long minSeconds = kMinInstallTimeoutMillis / 1000;
    const long maxSeconds = kMaxInstallTimeoutMillis / 1000;
    if (!end || end == text.c_str() || *end != '\0' ||
        seconds < minSeconds || seconds > maxSeconds) {
        fl_alert("Enter a timeout between %ld and %ld seconds.",
                 minSeconds,
                 maxSeconds);
        return;
    }

    setInstallTimeoutMillis(static_cast<int>(seconds * 1000), true);
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
    syncInstallMgrTimeout();
}

void ModuleManagerDialog::maybePromptToLoadInitialSources() {
    if (promptedToLoadInitialSources_) return;
    promptedToLoadInitialSources_ = true;

    if (!sources_.empty()) return;

    if (!showInitialSourceSetupDialog()) {
        if (statusBox_) {
            statusBox_->copy_label(
                "No sources configured. Open All Sources to add a local source or load the standard remote sources.");
        }
        return;
    }

    if (!refreshAllSources(true) && sources_.empty() && statusBox_) {
        statusBox_->copy_label(
            "Loading standard remote sources failed. Open All Sources to retry or add a local source.");
    }
}

void ModuleManagerDialog::refreshSources(bool refreshRemoteContent) {
    if (!installMgr_) initializeInstallMgr();
    if (!installMgr_) return;

    sources_.clear();
    installMgr_->readInstallConf();
    syncInstallMgrTimeout();

    for (auto it = installMgr_->sources.begin(); it != installMgr_->sources.end(); ++it) {
        sword::InstallSource* src = it->second;
        if (!src) continue;
        SourceRow row;
        row.caption = safeText(src->caption);
        row.type = safeText(src->type);
        row.source = safeText(src->source);
        row.directory = safeText(src->directory);
        row.username = safeText(src->u);
        row.password = safeText(src->p);
        row.uid = safeText(src->uid);
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
            row.username = safeText(src.u);
            row.password = safeText(src.p);
            row.uid = safeText(src.uid);
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

    if (app_) {
        const auto& settings = app_->moduleManagerSettings();
        if (activeSourceCaptions_.empty() && !hasExplicitSourceFilter_) {
            activeSourceCaptions_ = settings.selectedSources;
            hasExplicitSourceFilter_ = settings.hasSelectedSources;
        }
    }

    normalizeActiveSourceCaptions();
    updateSourceFilterButton();

    if (refreshRemoteContent) {
        std::vector<std::string> refreshFailures;
        for (const auto& src : sources_) {
            if (src.isLocal || !src.remoteSource || trimCopy(src.source).empty()) continue;
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
    for (const auto& path : allUserSwordDataPaths()) {
        localMgr.augmentModules(path.c_str());
    }
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
                row.shortDescription = descriptionOfModule(mod);
                row.aboutText = aboutTextOfModule(mod);
                row.moduleType = safeText(mod->getType());
                row.language = safeText(mod->getLanguage());
                row.normalizedLanguage = normalizeLanguageCode(row.language);
                row.displayLanguage = languageDisplayName(row.language);
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
            row.shortDescription = descriptionOfModule(mod);
            row.aboutText = aboutTextOfModule(mod);
            row.moduleType = safeText(mod->getType());
            row.language = safeText(mod->getLanguage());
            row.normalizedLanguage = normalizeLanguageCode(row.language);
            row.displayLanguage = languageDisplayName(row.language);
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

    repopulateLanguageChoice();
    repopulateFilterTree();
    repopulateModuleBrowser();
}

void ModuleManagerDialog::normalizeActiveSourceCaptions() {
    std::vector<std::string> validSelections;
    validSelections.reserve(sources_.size());
    for (const auto& source : sources_) {
        if (!hasExplicitSourceFilter_ ||
            std::find(activeSourceCaptions_.begin(),
                      activeSourceCaptions_.end(),
                      source.caption) != activeSourceCaptions_.end()) {
            validSelections.push_back(source.caption);
        }
    }

    if (!hasExplicitSourceFilter_ ||
        validSelections.size() == sources_.size()) {
        hasExplicitSourceFilter_ = false;
        activeSourceCaptions_.clear();
        for (const auto& source : sources_) {
            activeSourceCaptions_.push_back(source.caption);
        }
        return;
    }

    activeSourceCaptions_ = std::move(validSelections);
}

void ModuleManagerDialog::applySourceFilterSelection() {
    normalizeActiveSourceCaptions();
    updateSourceFilterButton();
    repopulateLanguageChoice();
    repopulateFilterTree();
    repopulateModuleBrowser();
    persistModuleManagerSettings();
}

void ModuleManagerDialog::updateSourceFilterButton() {
    if (!sourceFilterButton_) return;

    if (sources_.empty()) {
        sourceFilterButton_->copy_label("No Sources");
        sourceFilterButton_->tooltip(
            "No sources are configured yet.\n\n"
            "Open this menu to add a local source or load the standard remote sources.");
        return;
    }

    std::vector<std::string> selected;
    for (const auto& source : sources_) {
        if (sourceCaptionSelected(source.caption)) {
            selected.push_back(source.caption);
        }
    }

    std::string label;
    if (selected.empty()) {
        label = "No Sources";
    } else if (selected.size() == sources_.size()) {
        label = "All Sources";
    } else if (selected.size() == 1) {
        label = selected.front();
    } else {
        label = std::to_string(selected.size()) + " Sources";
    }
    sourceFilterButton_->copy_label(label.c_str());

    std::ostringstream tooltip;
    if (selected.empty()) {
        tooltip << "No sources selected for the module list.\n\nManage sources and filtering.";
    } else {
        tooltip << "Visible sources:\n";
        for (size_t i = 0; i < selected.size(); ++i) {
            if (i) tooltip << '\n';
            tooltip << selected[i];
        }
        tooltip << "\n\nManage sources and filtering.";
    }
    sourceFilterButton_->copy_tooltip(tooltip.str().c_str());
}

void ModuleManagerDialog::repopulateLanguageChoice() {
    if (!languageChoice_) return;

    const std::string currentLabel = trimCopy(languageChoice_->selectedValue());
    std::string desiredCode;
    if (currentLabel.empty()) {
        if (app_) desiredCode = trimCopy(app_->moduleManagerSettings().languageFilter);
        if (desiredCode.empty()) desiredCode = "en";
    } else if (currentLabel != "All Languages") {
        desiredCode = selectedLanguageCode();
    }

    std::map<std::string, std::string, NoCaseLess> labelsToCodes;
    for (const auto& row : modules_) {
        if (!sourceCaptionSelected(row.sourceCaption)) continue;
        if (row.normalizedLanguage.empty() || row.displayLanguage.empty()) continue;
        labelsToCodes.emplace(row.displayLanguage, row.normalizedLanguage);
    }

    languageChoiceLabels_.clear();
    languageChoiceCodesByLabel_.clear();
    languageChoiceLabels_.push_back("All Languages");
    languageChoiceCodesByLabel_["All Languages"] = "";

    std::string selectedLabel = "All Languages";
    for (const auto& entry : labelsToCodes) {
        languageChoiceLabels_.push_back(entry.first);
        languageChoiceCodesByLabel_[entry.first] = entry.second;
        if (!desiredCode.empty() && entry.second == desiredCode) {
            selectedLabel = entry.first;
        }
    }

    languageChoice_->setItems(languageChoiceLabels_);
    languageChoice_->setSelectedValue(selectedLabel);
}

void ModuleManagerDialog::repopulateFilterTree() {
    if (!filterTree_) return;

    const TreeFilter desiredFilter = treeFilter_;
    const std::string languageFilter = selectedLanguageCode();

    std::map<std::string,
             std::map<std::string, std::string, NoCaseLess>,
             NoCaseLess> typeLanguages;
    for (const auto& row : modules_) {
        if (!sourceCaptionSelected(row.sourceCaption)) continue;
        if (!languageFilter.empty() && row.normalizedLanguage != languageFilter) continue;
        if (row.moduleType.empty()) continue;

        if (!row.displayLanguage.empty() && !row.normalizedLanguage.empty()) {
            typeLanguages[row.moduleType].emplace(row.displayLanguage,
                                                  row.normalizedLanguage);
        } else {
            typeLanguages.try_emplace(row.moduleType);
        }
    }

    filterTree_->clear();
    treeFilters_.clear();

    Fl_Tree_Item* allItem = filterTree_->add("All");
    if (!allItem) return;
    allItem->open();
    treeFilters_[allItem] = TreeFilter{};

    for (const auto& entry : typeLanguages) {
        Fl_Tree_Item* typeItem = filterTree_->add(allItem, entry.first.c_str());
        if (!typeItem) continue;
        treeFilters_[typeItem] = TreeFilter{TreeFilter::Scope::Type,
                                            entry.first,
                                            ""};

        for (const auto& language : entry.second) {
            Fl_Tree_Item* languageItem = filterTree_->add(typeItem,
                                                          language.first.c_str());
            if (!languageItem) continue;
            treeFilters_[languageItem] = TreeFilter{
                TreeFilter::Scope::TypeLanguage,
                entry.first,
                language.second};
        }
    }

    auto filtersEqual = [](const TreeFilter& left, const TreeFilter& right) {
        return left.scope == right.scope &&
               left.moduleType == right.moduleType &&
               left.language == right.language;
    };

    Fl_Tree_Item* selectedItem = allItem;
    for (const auto& entry : treeFilters_) {
        if (filtersEqual(entry.second, desiredFilter)) {
            selectedItem = const_cast<Fl_Tree_Item*>(entry.first);
            break;
        }
    }

    filterTree_->select_only(selectedItem, 0);
    auto selectedFilter = treeFilters_.find(selectedItem);
    treeFilter_ = (selectedFilter != treeFilters_.end())
        ? selectedFilter->second
        : TreeFilter{};
    for (Fl_Tree_Item* item = selectedItem; item; item = item->parent()) {
        item->open();
    }
    filterTree_->redraw();
}

void ModuleManagerDialog::updateModuleBrowserColumns() {
    if (!moduleBrowser_) return;

    const int tableWidth = std::max(0, moduleBrowser_->w());
    const int fixedWidth = 36 + 130 + 120 + 130 + 150 + 90 + 90 + 55;
    const int descriptionWidth = std::max(180, tableWidth - fixedWidth - 30);

    moduleBrowserColWidths_[0] = 36;
    moduleBrowserColWidths_[1] = 130;
    moduleBrowserColWidths_[2] = descriptionWidth;
    moduleBrowserColWidths_[3] = 120;
    moduleBrowserColWidths_[4] = 130;
    moduleBrowserColWidths_[5] = 150;
    moduleBrowserColWidths_[6] = 90;
    moduleBrowserColWidths_[7] = 90;
    moduleBrowserColWidths_[8] = 55;
    moduleBrowserColWidths_[9] = 0;

    moduleBrowser_->column_widths(moduleBrowserColWidths_);
    if (auto* browser = dynamic_cast<ModuleListBrowser*>(moduleBrowser_)) {
        browser->setCheckboxColumnWidth(moduleBrowserColWidths_[0]);
    }
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
        "@bSel\t@bModule ID\t@bDescription\t@bLanguage\t@bType\t@bSource\t@bInstalled\t@bAvailable\t@bStatus");

    updateModuleBrowserColumns();

    const std::string languageFilter = selectedLanguageCode();
    const std::string sortField = selectedChoiceLabel(sortChoice_);
    const std::string moduleFilter = trimCopy(
        moduleFilterInput_ && moduleFilterInput_->value()
            ? moduleFilterInput_->value()
            : "");
    const std::string descriptionFilter = trimCopy(
        descriptionFilterInput_ && descriptionFilterInput_->value()
            ? descriptionFilterInput_->value()
            : "");

    std::vector<int> filteredRows;
    filteredRows.reserve(modules_.size());

    for (size_t i = 0; i < modules_.size(); ++i) {
        const ModuleRow& row = modules_[i];
        if (!sourceCaptionSelected(row.sourceCaption)) continue;
        if (!languageFilter.empty() && row.normalizedLanguage != languageFilter) continue;
        if (treeFilter_.scope == TreeFilter::Scope::Type &&
            row.moduleType != treeFilter_.moduleType) {
            continue;
        }
        if (treeFilter_.scope == TreeFilter::Scope::TypeLanguage &&
            (row.moduleType != treeFilter_.moduleType ||
             row.normalizedLanguage != treeFilter_.language)) {
            continue;
        }
        if (!moduleFilter.empty() && !containsNoCase(row.moduleName, moduleFilter)) continue;
        if (!descriptionFilter.empty() &&
            !containsNoCase(row.shortDescription, descriptionFilter)) {
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
                             cmp = compareField(a.shortDescription, b.shortDescription);
                             if (cmp != 0) return cmp < 0;
                             cmp = compareField(a.moduleName, b.moduleName);
                             if (cmp != 0) return cmp < 0;
                         } else if (sortField == "Language") {
                             cmp = compareField(a.displayLanguage, b.displayLanguage);
                             if (cmp != 0) return cmp < 0;
                             cmp = compareField(a.moduleName, b.moduleName);
                             if (cmp != 0) return cmp < 0;
                         } else if (sortField == "Module Type") {
                             cmp = compareField(a.moduleType, b.moduleType);
                             if (cmp != 0) return cmp < 0;
                             cmp = compareField(a.displayLanguage, b.displayLanguage);
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

        const std::string description = row.shortDescription.empty()
            ? std::string("-")
            : truncateWithEllipsis(row.shortDescription, 96);
        const std::string line =
                                 std::string(row.checked ? "[x]" : "[ ]") + "\t" +
                                 row.moduleName + "\t" +
                                 description + "\t" +
                                 (row.displayLanguage.empty() ? "-" : row.displayLanguage) + "\t" +
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
    if (auto* browser = dynamic_cast<ModuleListBrowser*>(moduleBrowser_)) {
        browser->clearHoverTooltip();
    }
    updateStatusBox();
    updateInstallButton();
    moduleBrowser_->redraw();
}

void ModuleManagerDialog::updateStatusBox() {
    if (!statusBox_) return;

    const auto checkedRows = checkedModuleRows();
    if (!checkedRows.empty()) {
        std::string label = std::to_string(checkedRows.size()) + " module";
        if (checkedRows.size() != 1) label += "s";
        label += " checked";
        if (!visibleModuleRows_.empty()) {
            label += " | " + std::to_string(visibleModuleRows_.size()) + " visible";
        }
        label += " | Continue to install/update or adjust the checked list.";
        statusBox_->copy_label(label.c_str());
        return;
    }

    if (sources_.empty()) {
        statusBox_->copy_label(
            "No sources configured. Open All Sources to add a local source or load the standard remote sources.");
        return;
    }

    if (modules_.empty()) {
        statusBox_->copy_label(
            "0 modules listed. Refresh a source to load module metadata.");
        return;
    }

    const int moduleIndex = selectedVisibleRow();
    if (moduleIndex >= 0 &&
        moduleIndex < static_cast<int>(modules_.size())) {
        const ModuleRow& row = modules_[moduleIndex];

        std::string label = row.moduleName + " | " + row.statusText;
        if (!row.displayLanguage.empty()) label += " | " + row.displayLanguage;
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

    const auto checkedRows = checkedModuleRows();
    if (!checkedRows.empty()) {
        std::string label = "Install/Update " + std::to_string(checkedRows.size());
        installButton_->copy_label(label.c_str());
        installButton_->activate();
        return;
    }

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

std::vector<int> ModuleManagerDialog::checkedModuleRows() const {
    std::vector<int> rows;
    for (size_t i = 0; i < modules_.size(); ++i) {
        if (modules_[i].checked) {
            rows.push_back(static_cast<int>(i));
        }
    }
    return rows;
}

std::vector<int> ModuleManagerDialog::selectedOrCheckedModuleRows() const {
    std::vector<int> rows = checkedModuleRows();
    if (!rows.empty()) return rows;

    const int selected = selectedVisibleRow();
    if (selected >= 0) rows.push_back(selected);
    return rows;
}

std::string ModuleManagerDialog::selectedLanguageCode() const {
    if (!languageChoice_) return "";
    const std::string label = trimCopy(languageChoice_->selectedValue());
    if (label.empty() || label == "All Languages") return "";

    auto it = languageChoiceCodesByLabel_.find(label);
    return (it != languageChoiceCodesByLabel_.end()) ? it->second
                                                     : std::string();
}

bool ModuleManagerDialog::sourceCaptionSelected(const std::string& caption) const {
    if (caption.empty()) return false;
    if (!hasExplicitSourceFilter_) return true;

    return std::find(activeSourceCaptions_.begin(),
                     activeSourceCaptions_.end(),
                     caption) != activeSourceCaptions_.end();
}

std::string ModuleManagerDialog::moduleTooltipText(int moduleIndex) const {
    if (moduleIndex < 0 ||
        moduleIndex >= static_cast<int>(modules_.size())) {
        return "";
    }

    const ModuleRow& row = modules_[static_cast<size_t>(moduleIndex)];
    std::ostringstream out;
    out << (row.shortDescription.empty()
                ? row.moduleName
                : row.shortDescription);
    if (!row.availableVersion.empty()) {
        out << "\nSword module version " << row.availableVersion;
    }
    out << "\n\n";
    out << (row.aboutText.empty()
                ? "The module has no About information."
                : row.aboutText);

    std::string text = trimCopy(out.str());
    if (text.size() > 1200) {
        text = text.substr(0, 1196) + " ...";
    }
    return text;
}

void ModuleManagerDialog::toggleVisibleModuleChecked(int browserLine) {
    if (browserLine <= 1) return;

    const int visibleIndex = browserLine - 2;
    if (visibleIndex < 0 ||
        visibleIndex >= static_cast<int>(visibleModuleRows_.size())) {
        return;
    }

    moduleBrowser_->value(browserLine);
    const int moduleIndex = visibleModuleRows_[static_cast<size_t>(visibleIndex)];
    modules_[static_cast<size_t>(moduleIndex)].checked =
        !modules_[static_cast<size_t>(moduleIndex)].checked;
    repopulateModuleBrowser();
}

bool ModuleManagerDialog::confirmInstallPlan(
    const std::vector<int>& moduleIndices) const {
    if (moduleIndices.empty()) return false;

    std::ostringstream message;
    message << "Install/update " << moduleIndices.size() << " module";
    if (moduleIndices.size() != 1) message << "s";
    message << "?\n\n";

    size_t downgradeCount = 0;
    const size_t limit = 14;
    for (size_t i = 0; i < moduleIndices.size(); ++i) {
        if (i == limit) {
            message << "... and " << (moduleIndices.size() - limit)
                    << " more.\n";
            break;
        }

        const int moduleIndex = moduleIndices[i];
        if (moduleIndex < 0 ||
            moduleIndex >= static_cast<int>(modules_.size())) {
            continue;
        }

        const ModuleRow& row = modules_[static_cast<size_t>(moduleIndex)];
        if (row.wouldDowngrade) ++downgradeCount;

        message << row.moduleName;
        if (!row.shortDescription.empty()) {
            message << " - " << row.shortDescription;
        }
        if (!row.sourceCaption.empty()) {
            message << " [" << row.sourceCaption << "]";
        }
        message << "\n";
    }

    if (downgradeCount > 0) {
        message << "\nWarning: " << downgradeCount << " selected module";
        if (downgradeCount != 1) message << "s";
        message << " will replace a newer local version with an older source version.\n";
    }

    message << "\nContinue?";

    return fl_choice("%s", "Cancel", "Continue", nullptr,
                     message.str().c_str()) == 1;
}

void ModuleManagerDialog::clearFilters() {
    if (languageChoice_) {
        if (languageChoiceCodesByLabel_.find("English") !=
            languageChoiceCodesByLabel_.end()) {
            languageChoice_->setSelectedValue("English");
        } else {
            languageChoice_->setSelectedValue("All Languages");
        }
    }
    if (moduleFilterInput_) moduleFilterInput_->value("");
    if (descriptionFilterInput_) descriptionFilterInput_->value("");
    treeFilter_ = TreeFilter{};
    repopulateFilterTree();
    repopulateModuleBrowser();
    persistModuleManagerSettings();
}

void ModuleManagerDialog::persistModuleManagerSettings() {
    if (!app_) return;

    VerdadApp::ModuleManagerSettings settings = app_->moduleManagerSettings();
    VerdadApp::ModuleManagerSettings updated = settings;
    updated.languageFilter = selectedLanguageCode();
    updated.hasSelectedSources = hasExplicitSourceFilter_;
    updated.selectedSources = hasExplicitSourceFilter_
        ? activeSourceCaptions_
        : std::vector<std::string>();

    if (updated.languageFilter == settings.languageFilter &&
        updated.hasSelectedSources == settings.hasSelectedSources &&
        updated.selectedSources == settings.selectedSources) {
        return;
    }

    app_->setModuleManagerSettings(updated);
    app_->savePreferences();
}

void ModuleManagerDialog::openSourceManager() {
    SourceManagerDialog dialog(this);
    dialog.openModal();
}

bool ModuleManagerDialog::confirmRemoteNetworkUse() {
    if (app_ && !app_->moduleManagerSettings().showRemoteNetworkWarning) {
        return true;
    }

    bool dontShowAgain = false;
    const bool confirmed = showRemoteNetworkWarningDialog(&dontShowAgain);
    if (confirmed && dontShowAgain && app_) {
        VerdadApp::ModuleManagerSettings settings = app_->moduleManagerSettings();
        settings.showRemoteNetworkWarning = false;
        app_->setModuleManagerSettings(settings);
        app_->savePreferences();
    }

    return confirmed;
}

std::string ModuleManagerDialog::sourceUrl(const SourceRow& row) const {
    if (row.isLocal) {
        return !row.directory.empty() ? row.directory : row.source;
    }

    const std::string host = trimCopy(row.source);
    if (host.empty()) return "";

    std::string url = sourceSchemeFromType(row.type) + "://" + host;
    std::string path = trimCopy(row.directory);
    if (!path.empty()) {
        if (path.front() != '/') url.push_back('/');
        url += path;
    }
    return url;
}

bool ModuleManagerDialog::applySourceUrl(SourceRow& row,
                                         const std::string& text,
                                         std::string* errorMessage) const {
    const std::string trimmed = trimCopy(text);
    if (row.isLocal) {
        if (trimmed.empty()) {
            if (errorMessage) {
                *errorMessage = "Local sources need a directory path.";
            }
            return false;
        }
        row.type = "DIR";
        row.source = trimmed;
        row.directory = trimmed;
        return true;
    }

    if (trimmed.empty()) {
        row.source.clear();
        row.directory.clear();
        if (row.type.empty()) row.type = "HTTPS";
        return true;
    }

    const size_t schemePos = trimmed.find("://");
    if (schemePos == std::string::npos) {
        if (errorMessage) {
            *errorMessage =
                "Remote URLs must include ftp://, http://, https://, or sftp://.";
        }
        return false;
    }

    const std::string scheme = lowerCopy(trimCopy(trimmed.substr(0, schemePos)));
    if (!isSupportedRemoteScheme(scheme)) {
        if (errorMessage) {
            *errorMessage =
                "Unsupported remote URL scheme. Use ftp://, http://, https://, or sftp://.";
        }
        return false;
    }

    const std::string remainder = trimmed.substr(schemePos + 3);
    std::string host = remainder;
    std::string path = "/";
    const size_t slashPos = remainder.find('/');
    if (slashPos != std::string::npos) {
        host = remainder.substr(0, slashPos);
        path = remainder.substr(slashPos);
    }

    host = trimCopy(host);
    if (host.empty()) {
        if (errorMessage) {
            *errorMessage = "Remote URLs must include a host name.";
        }
        return false;
    }

    path = trimCopy(path);
    if (path.empty()) path = "/";
    if (path.size() > 1) path = stripTrailingSlashCopy(path);

    row.isLocal = false;
    row.type = upperCopy(scheme);
    row.source = host;
    row.directory = path;
    return true;
}

bool ModuleManagerDialog::rewriteSourceConfiguration() {
    if (!installMgr_) initializeInstallMgr();
    if (!installMgr_) return false;

    installMgr_->readInstallConf();
    installMgr_->clearSources();
    syncInstallMgrTimeout();

    for (const auto& row : sources_) {
        if (row.isLocal) continue;

        auto* source = new sword::InstallSource(
            (row.type.empty() ? "HTTPS" : row.type).c_str());
        source->caption = row.caption.c_str();
        source->source = row.source.c_str();
        source->directory = row.directory.c_str();
        source->u = row.username.c_str();
        source->p = row.password.c_str();
        source->uid = row.uid.c_str();
        installMgr_->sources[source->caption] = source;
    }

    installMgr_->saveInstallConf();

    const std::string confFile = installMgrPath_ + "/InstallMgr.conf";
    sword::SWConfig conf(confFile.c_str());
    auto& sourceSection = conf.getSection("Sources");
    for (const auto& row : sources_) {
        if (!row.isLocal) continue;

        sword::InstallSource local("DIR");
        local.caption = row.caption.c_str();
        local.source = row.source.c_str();
        local.directory = row.directory.c_str();
        local.u = row.username.c_str();
        local.p = row.password.c_str();
        local.uid = row.uid.c_str();
        sourceSection.insert(
            std::make_pair(sword::SWBuf("DIRSource"), local.getConfEnt()));
    }
    conf.save();

    refreshSources(false);
    refreshModules();
    persistModuleManagerSettings();
    return true;
}

bool ModuleManagerDialog::updateSourceCaption(size_t index,
                                              const std::string& text,
                                              std::string* errorMessage) {
    if (index >= sources_.size()) return false;

    const std::string caption = trimCopy(text);
    if (caption.empty()) {
        if (errorMessage) *errorMessage = "Source names cannot be empty.";
        return false;
    }

    for (size_t i = 0; i < sources_.size(); ++i) {
        if (i == index) continue;
        if (compareNoCase(sources_[i].caption, caption) == 0) {
            if (errorMessage) *errorMessage = "Source names must be unique.";
            return false;
        }
    }

    const std::vector<SourceRow> previousSources = sources_;
    const std::vector<std::string> previousSelection = activeSourceCaptions_;
    const bool previousExplicit = hasExplicitSourceFilter_;

    const std::string oldCaption = sources_[index].caption;
    sources_[index].caption = caption;
    replaceSelectedSourceCaption(oldCaption, caption);

    if (!rewriteSourceConfiguration()) {
        sources_ = previousSources;
        activeSourceCaptions_ = previousSelection;
        hasExplicitSourceFilter_ = previousExplicit;
        if (errorMessage) {
            *errorMessage = "The updated source list could not be saved.";
        }
        return false;
    }
    return true;
}

bool ModuleManagerDialog::updateSourceUrl(size_t index,
                                          const std::string& text,
                                          std::string* errorMessage) {
    if (index >= sources_.size()) return false;

    const std::vector<SourceRow> previousSources = sources_;
    const std::vector<std::string> previousSelection = activeSourceCaptions_;
    const bool previousExplicit = hasExplicitSourceFilter_;

    SourceRow updated = sources_[index];
    if (!applySourceUrl(updated, text, errorMessage)) {
        return false;
    }

    sources_[index] = std::move(updated);
    if (!rewriteSourceConfiguration()) {
        sources_ = previousSources;
        activeSourceCaptions_ = previousSelection;
        hasExplicitSourceFilter_ = previousExplicit;
        if (errorMessage) {
            *errorMessage = "The updated source list could not be saved.";
        }
        return false;
    }
    return true;
}

void ModuleManagerDialog::setSourceSelected(const std::string& caption, bool selected) {
    if (caption.empty()) return;

    if (!hasExplicitSourceFilter_) {
        activeSourceCaptions_.clear();
        for (const auto& source : sources_) {
            activeSourceCaptions_.push_back(source.caption);
        }
        hasExplicitSourceFilter_ = true;
    }

    auto it = std::find(activeSourceCaptions_.begin(),
                        activeSourceCaptions_.end(),
                        caption);
    if (selected) {
        if (it == activeSourceCaptions_.end()) {
            activeSourceCaptions_.push_back(caption);
        }
    } else if (it != activeSourceCaptions_.end()) {
        activeSourceCaptions_.erase(it);
    }

    applySourceFilterSelection();
}

void ModuleManagerDialog::replaceSelectedSourceCaption(const std::string& oldCaption,
                                                       const std::string& newCaption) {
    if (!hasExplicitSourceFilter_) return;

    auto it = std::find(activeSourceCaptions_.begin(),
                        activeSourceCaptions_.end(),
                        oldCaption);
    if (it != activeSourceCaptions_.end()) {
        *it = newCaption;
    }
}

std::string ModuleManagerDialog::addRemoteSourceRow() {
    auto makeUniqueCaption = [&](const std::string& base) {
        std::string candidate = base;
        int suffix = 2;
        while (std::any_of(sources_.begin(), sources_.end(),
                           [&](const SourceRow& row) {
                               return compareNoCase(row.caption, candidate) == 0;
                           })) {
            candidate = base + " " + std::to_string(suffix++);
        }
        return candidate;
    };
    auto makeUniqueUid = [&](const std::string& base) {
        std::string candidate = base;
        int suffix = 2;
        while (std::any_of(sources_.begin(), sources_.end(),
                           [&](const SourceRow& row) {
                               return row.uid == candidate;
                           })) {
            candidate = base + "-" + std::to_string(suffix++);
        }
        return candidate;
    };

    const std::vector<SourceRow> previousSources = sources_;
    const std::vector<std::string> previousSelection = activeSourceCaptions_;
    const bool previousExplicit = hasExplicitSourceFilter_;

    SourceRow row;
    row.caption = makeUniqueCaption("New Source");
    row.type = "HTTPS";
    row.directory = "/";
    row.uid = makeUniqueUid("manual-source");
    row.isLocal = false;
    sources_.push_back(row);

    if (hasExplicitSourceFilter_) {
        activeSourceCaptions_.push_back(row.caption);
    }

    if (!rewriteSourceConfiguration()) {
        sources_ = previousSources;
        activeSourceCaptions_ = previousSelection;
        hasExplicitSourceFilter_ = previousExplicit;
        return "";
    }
    return row.caption;
}

std::string ModuleManagerDialog::addLocalSourceRow() {
    std::string startDir = userHomeDir().empty() ? "." : userHomeDir();
    const char* dirRaw = fl_dir_chooser("Select local module source directory",
                                        startDir.c_str());
    if (!dirRaw || !*dirRaw) return "";

    const std::string dir = trimCopy(dirRaw);
    if (dir.empty()) return "";

    auto makeUniqueCaption = [&](const std::string& base) {
        std::string candidate = base;
        int suffix = 2;
        while (std::any_of(sources_.begin(), sources_.end(),
                           [&](const SourceRow& row) {
                               return compareNoCase(row.caption, candidate) == 0;
                           })) {
            candidate = base + " " + std::to_string(suffix++);
        }
        return candidate;
    };
    auto makeUniqueUid = [&](const std::string& base) {
        std::string candidate = base;
        int suffix = 2;
        while (std::any_of(sources_.begin(), sources_.end(),
                           [&](const SourceRow& row) {
                               return row.uid == candidate;
                           })) {
            candidate = base + "-" + std::to_string(suffix++);
        }
        return candidate;
    };

    const std::vector<SourceRow> previousSources = sources_;
    const std::vector<std::string> previousSelection = activeSourceCaptions_;
    const bool previousExplicit = hasExplicitSourceFilter_;

    SourceRow row;
    row.caption = makeUniqueCaption(fallbackSourceCaptionFromPath(dir));
    row.type = "DIR";
    row.source = dir;
    row.directory = dir;
    row.uid = makeUniqueUid("local-source");
    row.isLocal = true;
    sources_.push_back(row);

    if (hasExplicitSourceFilter_) {
        activeSourceCaptions_.push_back(row.caption);
    }

    if (!rewriteSourceConfiguration()) {
        sources_ = previousSources;
        activeSourceCaptions_ = previousSelection;
        hasExplicitSourceFilter_ = previousExplicit;
        return "";
    }
    return row.caption;
}

bool ModuleManagerDialog::removeSourceRow(size_t index) {
    if (index >= sources_.size()) return false;

    const std::string caption = sources_[index].caption;
    const std::string prompt = "Remove source '" + caption + "'?";
    if (fl_choice("%s", "Cancel", "Remove", nullptr, prompt.c_str()) != 1) {
        return false;
    }

    const std::vector<SourceRow> previousSources = sources_;
    const std::vector<std::string> previousSelection = activeSourceCaptions_;
    const bool previousExplicit = hasExplicitSourceFilter_;

    sources_.erase(sources_.begin() + index);
    activeSourceCaptions_.erase(
        std::remove(activeSourceCaptions_.begin(),
                    activeSourceCaptions_.end(),
                    caption),
        activeSourceCaptions_.end());

    if (!rewriteSourceConfiguration()) {
        sources_ = previousSources;
        activeSourceCaptions_ = previousSelection;
        hasExplicitSourceFilter_ = previousExplicit;
        return false;
    }
    return true;
}

bool ModuleManagerDialog::refreshSourceRow(size_t index) {
    if (!installMgr_) initializeInstallMgr();
    if (!installMgr_) return false;
    if (index >= sources_.size()) return false;

    const SourceRow& source = sources_[index];
    if (source.isLocal) {
        refreshSources(false);
        refreshModules();
        if (statusBox_) statusBox_->copy_label("Sources refreshed.");
        return true;
    }

    if (trimCopy(source.source).empty()) {
        fl_alert("Enter a remote URL for %s first.", source.caption.c_str());
        return false;
    }

    if (!source.remoteSource) {
        fl_alert("Remote source is not available for %s yet.", source.caption.c_str());
        return false;
    }

    if (!confirmRemoteNetworkUse()) return false;
    syncInstallMgrTimeout();
    statusBox_->copy_label(("Refreshing " + source.caption + "...").c_str());
    Fl::check();
    if (auto* manager = verdadInstallMgr(installMgr_.get())) {
        manager->resetLastTransferState();
    }
    const int rc = installMgr_->refreshRemoteSource(source.remoteSource);
    if (rc != 0) {
        const std::string message =
            describeRefreshFailure(
                source.caption,
                rc,
                verdadInstallMgr(installMgr_.get()));
        fl_alert("%s", message.c_str());
        statusBox_->copy_label(
            ("Refresh failed: " +
             refreshErrorBrief(rc, verdadInstallMgr(installMgr_.get()))).c_str());
        refreshSources(false);
        refreshModules();
        return false;
    }

    statusBox_->copy_label("Sources refreshed.");
    refreshSources(false);
    refreshModules();
    return true;
}

bool ModuleManagerDialog::refreshAllSources(bool skipNetworkConfirmation) {
    if (!installMgr_) initializeInstallMgr();
    if (!installMgr_) return false;
    if (!skipNetworkConfirmation && !confirmRemoteNetworkUse()) return false;
    if (installMgr_) {
        syncInstallMgrTimeout();
        if (auto* manager = verdadInstallMgr(installMgr_.get())) {
            manager->resetLastTransferState();
        }
        const int rc = installMgr_->refreshRemoteSourceConfiguration();
        if (rc != 0) {
            const std::string message =
                describeRefreshFailure(
                    "the remote source list",
                    rc,
                    verdadInstallMgr(installMgr_.get()));
            fl_alert("%s", message.c_str());
            statusBox_->copy_label(
                ("Refresh failed: " +
                 refreshErrorBrief(
                     rc,
                     verdadInstallMgr(installMgr_.get()))).c_str());
            refreshSources(false);
            refreshModules();
            return false;
        }
    }
    refreshSources(true);
    refreshModules();
    return true;
}

void ModuleManagerDialog::installOrUpdateSelectedModules() {
    if (!installMgr_) initializeInstallMgr();
    if (!installMgr_) return;

    const std::vector<int> moduleIndices = selectedOrCheckedModuleRows();
    if (moduleIndices.empty()) {
        fl_alert("Select at least one module first.");
        return;
    }

    bool requiresRemoteAccess = false;
    for (int moduleIndex : moduleIndices) {
        if (moduleIndex < 0 ||
            moduleIndex >= static_cast<int>(modules_.size())) {
            continue;
        }

        const ModuleRow& row = modules_[static_cast<size_t>(moduleIndex)];
        if (row.sourceType == "DIR") {
            if (row.sourcePath.empty()) {
                fl_alert("Local source path is missing for %s.",
                         row.moduleName.c_str());
                return;
            }
            continue;
        }

        if (!row.remoteSource) {
            fl_alert("Remote source is not available for %s. Refresh the source first.",
                     row.moduleName.c_str());
            return;
        }
        requiresRemoteAccess = true;
    }

    if (!confirmInstallPlan(moduleIndices)) return;
    if (requiresRemoteAccess && !confirmRemoteNetworkUse()) return;

    if (!ensureUserSwordDataDirs()) {
        fl_alert("The local SWORD folder %s could not be created.",
                 userSwordRootDir().c_str());
        statusBox_->copy_label("Install failed: local .sword folder unavailable");
        return;
    }

    // Installing mutates ~/.sword while the indexer builds fresh SWMgr
    // instances in the background, so pause indexing until the module set is
    // stable again.
    SearchIndexer::ScopedSuspend suspendedIndexing =
        (app_ && app_->searchIndexer())
            ? app_->searchIndexer()->suspendBackgroundIndexing()
            : SearchIndexer::ScopedSuspend();

    const std::string localSwordRoot = userSwordRootDir();
    sword::SWMgr destMgr(localSwordRoot.c_str(),
                         true,
                         new sword::MarkupFilterMgr(sword::FMT_XHTML),
                         false,
                         false);

    std::vector<int> successfulRows;
    std::vector<std::string> failureMessages;
    syncInstallMgrTimeout();
    for (size_t i = 0; i < moduleIndices.size(); ++i) {
        const int moduleIndex = moduleIndices[i];
        if (moduleIndex < 0 ||
            moduleIndex >= static_cast<int>(modules_.size())) {
            continue;
        }

        const ModuleRow& row = modules_[static_cast<size_t>(moduleIndex)];
        statusBox_->copy_label(
            ("Installing " + std::to_string(i + 1) + "/" +
             std::to_string(moduleIndices.size()) + ": " +
             row.moduleName + "...")
                .c_str());
        Fl::check();

        if (auto* manager = verdadInstallMgr(installMgr_.get())) {
            manager->resetLastTransferState();
        }

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
            failureMessages.push_back(
                describeInstallFailure(
                    row.moduleName,
                    row.sourceCaption,
                    rc,
                    verdadInstallMgr(installMgr_.get())));
            continue;
        }

        successfulRows.push_back(moduleIndex);
    }

    if (!successfulRows.empty()) {
        app_->swordManager().initialize();
        if (app_->mainWindow()) {
            app_->mainWindow()->refresh();
        }

        if (app_) {
            app_->refreshSearchIndexCatalog(false);
        }

        if (app_->searchIndexer()) {
            for (int moduleIndex : successfulRows) {
                if (moduleIndex < 0 ||
                    moduleIndex >= static_cast<int>(modules_.size())) {
                    continue;
                }

                const ModuleRow& row = modules_[static_cast<size_t>(moduleIndex)];
                if (isSearchableResourceTypeToken(
                        searchResourceTypeTokenForModuleType(row.moduleType))) {
                    app_->searchIndexer()->queueModuleIndex(row.moduleName, true);
                }
            }
        }
    }

    refreshSources(false);
    refreshModules();

    if (failureMessages.empty()) {
        std::string done = "Installed/updated " + std::to_string(successfulRows.size()) +
                           " module";
        if (successfulRows.size() != 1) done += "s";
        statusBox_->copy_label(done.c_str());
        return;
    }

    std::ostringstream summary;
    if (!successfulRows.empty()) {
        summary << "Installed/updated " << successfulRows.size() << " module";
        if (successfulRows.size() != 1) summary << "s";
        summary << ".\n\n";
    }
    summary << joinMessageBlocks(failureMessages);
    fl_alert("%s", summary.str().c_str());
    statusBox_->copy_label(
        successfulRows.empty()
            ? "Install failed."
            : "Install completed with errors.");
}

void ModuleManagerDialog::onChooseSources(Fl_Widget* /*widget*/, void* data) {
    auto* self = static_cast<ModuleManagerDialog*>(data);
    if (!self) return;
    self->openSourceManager();
}

void ModuleManagerDialog::onTreeSelectionChanged(Fl_Widget* /*widget*/, void* data) {
    auto* self = static_cast<ModuleManagerDialog*>(data);
    if (!self || !self->filterTree_) return;

    Fl_Tree_Reason reason = self->filterTree_->callback_reason();
    if (reason != FL_TREE_REASON_SELECTED &&
        reason != FL_TREE_REASON_RESELECTED) {
        return;
    }

    Fl_Tree_Item* item = self->filterTree_->callback_item();
    auto it = self->treeFilters_.find(item);
    self->treeFilter_ = (it != self->treeFilters_.end())
        ? it->second
        : TreeFilter{};
    self->repopulateModuleBrowser();
}

void ModuleManagerDialog::onFilterChanged(Fl_Widget* widget, void* data) {
    auto* self = static_cast<ModuleManagerDialog*>(data);
    if (!self) return;
    if (widget == self->languageChoice_) {
        self->repopulateFilterTree();
        self->persistModuleManagerSettings();
    }
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

void ModuleManagerDialog::onInstallUpdate(Fl_Widget* /*widget*/, void* data) {
    auto* self = static_cast<ModuleManagerDialog*>(data);
    if (!self) return;
    self->installOrUpdateSelectedModules();
}

} // namespace verdad
