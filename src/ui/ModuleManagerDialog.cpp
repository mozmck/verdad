#include "ui/ModuleManagerDialog.h"

#include "app/VerdadApp.h"
#include "search/SearchIndexer.h"
#include "sword/SwordManager.h"
#include "sword/SwordPaths.h"
#include "ui/FilterableChoiceWidget.h"
#include "ui/MainWindow.h"
#include "ui/UiFontUtils.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Browser.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Tooltip.H>
#include <FL/Fl_Tree.H>
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
constexpr long kDefaultInstallTimeoutMillis = 40000;
constexpr int kFilterLabelWidth = 82;
constexpr int kFilterFieldGap = 6;
constexpr int kFilterGroupGap = 14;
constexpr int kCompactLabelWidth = 54;

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

class SourceSelectionDialog : public Fl_Double_Window {
public:
    struct Row {
        std::string caption;
        bool checked = false;
    };

    SourceSelectionDialog(int W = 420, int H = 460)
        : Fl_Double_Window(W, H, "Visible Sources") {
        begin();

        filterInput_ = new Fl_Input(80, 12, W - 92, 26, "Filter:");
        filterInput_->when(FL_WHEN_CHANGED | FL_WHEN_ENTER_KEY_ALWAYS);
        filterInput_->callback(onFilterChanged, this);

        browser_ = new Fl_Check_Browser(12, 48, W - 24, H - 102);
        browser_->callback(onBrowserChanged, this);
        browser_->when(FL_WHEN_CHANGED);

        selectAllButton_ = new Fl_Button(12, H - 42, 70, 30, "All");
        selectAllButton_->callback(onSelectAll, this);

        selectNoneButton_ = new Fl_Button(88, H - 42, 70, 30, "None");
        selectNoneButton_->callback(onSelectNone, this);

        cancelButton_ = new Fl_Button(W - 168, H - 42, 70, 30, "Cancel");
        cancelButton_->callback(onCancel, this);

        okButton_ = new Fl_Button(W - 88, H - 42, 70, 30, "OK");
        okButton_->callback(onOk, this);

        end();
        size_range(340, 280);
        resizable(browser_);
    }

    void setRows(const std::vector<Row>& rows) {
        rows_ = rows;
        filterInput_->value("");
        refreshBrowser();
    }

    bool openModal() {
        accepted_ = false;
        set_modal();
        show();
        while (shown()) Fl::wait();
        return accepted_;
    }

    std::vector<std::string> selectedCaptions() const {
        std::vector<std::string> captions;
        for (const auto& row : rows_) {
            if (row.checked) captions.push_back(row.caption);
        }
        return captions;
    }

private:
    std::vector<Row> rows_;
    std::vector<int> visibleRows_;
    Fl_Input* filterInput_ = nullptr;
    Fl_Check_Browser* browser_ = nullptr;
    Fl_Button* selectAllButton_ = nullptr;
    Fl_Button* selectNoneButton_ = nullptr;
    Fl_Button* cancelButton_ = nullptr;
    Fl_Button* okButton_ = nullptr;
    bool accepted_ = false;

    void syncChecksFromBrowser() {
        if (!browser_) return;
        for (size_t i = 0; i < visibleRows_.size(); ++i) {
            const int line = static_cast<int>(i) + 1;
            const int rowIndex = visibleRows_[i];
            if (rowIndex < 0 || rowIndex >= static_cast<int>(rows_.size())) continue;
            rows_[static_cast<size_t>(rowIndex)].checked = browser_->checked(line) != 0;
        }
    }

    void refreshBrowser() {
        syncChecksFromBrowser();

        const std::string filter = trimCopy(
            filterInput_ && filterInput_->value() ? filterInput_->value() : "");

        visibleRows_.clear();
        browser_->clear();
        for (size_t i = 0; i < rows_.size(); ++i) {
            const auto& row = rows_[i];
            if (!filter.empty() && !containsNoCase(row.caption, filter)) continue;
            visibleRows_.push_back(static_cast<int>(i));
            browser_->add(row.caption.c_str(), row.checked ? 1 : 0);
        }
    }

    static void onFilterChanged(Fl_Widget*, void* data) {
        auto* self = static_cast<SourceSelectionDialog*>(data);
        if (!self) return;
        self->refreshBrowser();
    }

    static void onBrowserChanged(Fl_Widget*, void* data) {
        auto* self = static_cast<SourceSelectionDialog*>(data);
        if (!self) return;
        self->syncChecksFromBrowser();
    }

    static void onSelectAll(Fl_Widget*, void* data) {
        auto* self = static_cast<SourceSelectionDialog*>(data);
        if (!self) return;
        for (auto& row : self->rows_) row.checked = true;
        self->refreshBrowser();
    }

    static void onSelectNone(Fl_Widget*, void* data) {
        auto* self = static_cast<SourceSelectionDialog*>(data);
        if (!self) return;
        for (auto& row : self->rows_) row.checked = false;
        self->refreshBrowser();
    }

    static void onCancel(Fl_Widget*, void* data) {
        auto* self = static_cast<SourceSelectionDialog*>(data);
        if (!self) return;
        self->accepted_ = false;
        self->hide();
    }

    static void onOk(Fl_Widget*, void* data) {
        auto* self = static_cast<SourceSelectionDialog*>(data);
        if (!self) return;
        self->syncChecksFromBrowser();
        self->accepted_ = true;
        self->hide();
    }
};

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

    if (filterTree_) {
        filterTree_->resize(kMargin, contentY, treeW, contentH);
    }

    const int sortX = rightX + rightW - 160;
    const int sortLabelX = sortX - kCompactLabelWidth - kFilterFieldGap;
    const int sourceButtonX = rightX + 28;
    const int sourceButtonW = 150;
    const int languageLabelX = sourceButtonX + sourceButtonW + 18;
    const int languageX = languageLabelX + kFilterLabelWidth + kFilterFieldGap;
    const int languageW = std::max(120, sortLabelX - kFilterGroupGap - languageX);
    if (sourceFilterButton_) {
        sourceFilterButton_->resize(sourceButtonX, row2Y, sourceButtonW, kControlHeight);
    }
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

    const int contentY = row1Y + kControlHeight + kRowSpacing;
    filterTree_ = new Fl_Tree(kMargin, contentY, 230, h() - contentY - 74);
    filterTree_->showroot(0);
    filterTree_->selectmode(FL_TREE_SELECT_SINGLE);
    filterTree_->item_reselect_mode(FL_TREE_SELECTABLE_ALWAYS);
    filterTree_->callback(onTreeSelectionChanged, this);
    filterTree_->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);

    const int rightX = kMargin + 230 + kRowSpacing;
    const int row2Y = contentY;
    sourceFilterButton_ = new Fl_Button(rightX + 28, row2Y, 150, kControlHeight, "All Sources");
    sourceFilterButton_->callback(onChooseSources, this);

    languageChoiceLabel_ = new Fl_Box(rightX + 194, row2Y, kFilterLabelWidth, kControlHeight,
                                      "Language:");
    languageChoiceLabel_->box(FL_NO_BOX);
    languageChoiceLabel_->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);

    sortChoiceLabel_ = new Fl_Box(rightX + 518, row2Y, kCompactLabelWidth, kControlHeight,
                                  "Sort:");
    sortChoiceLabel_->box(FL_NO_BOX);
    sortChoiceLabel_->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);

    sortChoice_ = new Fl_Choice(rightX + 578, row2Y, 160, kControlHeight);
    sortChoice_->add("Module ID");
    sortChoice_->add("Description");
    sortChoice_->add("Language");
    sortChoice_->add("Module Type");
    sortChoice_->add("Source");
    sortChoice_->add("Status");
    sortChoice_->value(0);
    sortChoice_->callback(onFilterChanged, this);

    languageChoice_ = new FilterableChoiceWidget(rightX + 282, row2Y, 150, kControlHeight);
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

    if (app_) {
        const auto& settings = app_->moduleManagerSettings();
        if (activeSourceCaptions_.empty()) {
            activeSourceCaptions_ = settings.selectedSources;
            hasExplicitSourceFilter_ = settings.hasSelectedSources;
        }
    }

    if (!hasExplicitSourceFilter_) {
        activeSourceCaptions_.clear();
        for (const auto& source : sources_) {
            activeSourceCaptions_.push_back(source.caption);
        }
    } else {
        std::vector<std::string> validSelections;
        for (const auto& caption : activeSourceCaptions_) {
            auto it = std::find_if(sources_.begin(), sources_.end(),
                                   [&](const SourceRow& row) {
                                       return row.caption == caption;
                                   });
            if (it != sources_.end()) validSelections.push_back(caption);
        }
        activeSourceCaptions_ = std::move(validSelections);
    }

    repopulateSourceChoice();
    updateSourceFilterButton();

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
    for (const auto& path : supplementalUserSwordDataPaths()) {
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

void ModuleManagerDialog::repopulateSourceChoice() {
    if (!sourceChoice_) return;

    const std::string selected = selectedChoiceLabel(sourceChoice_);
    sourceChoice_->clear();
    sourceChoice_->add("All Sources");

    int selectedIndex = 0;
    int index = 1;
    for (const auto& src : sources_) {
        sourceChoice_->add(escapeChoiceLabel(src.caption).c_str());
        if (!selected.empty() && src.caption == selected) {
            selectedIndex = index;
        }
        ++index;
    }

    sourceChoice_->value(selectedIndex);
    sourceChoice_->redraw();
}

void ModuleManagerDialog::updateSourceFilterButton() {
    if (!sourceFilterButton_) return;

    if (sources_.empty()) {
        sourceFilterButton_->copy_label("No Sources");
        sourceFilterButton_->tooltip("No sources are available.");
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
        tooltip << "No sources selected for the module list.";
    } else {
        tooltip << "Visible sources:\n";
        for (size_t i = 0; i < selected.size(); ++i) {
            if (i) tooltip << '\n';
            tooltip << selected[i];
        }
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

void ModuleManagerDialog::chooseVisibleSources() {
    SourceSelectionDialog dialog;
    std::vector<SourceSelectionDialog::Row> rows;
    rows.reserve(sources_.size());
    for (const auto& source : sources_) {
        rows.push_back({source.caption, sourceCaptionSelected(source.caption)});
    }

    dialog.setRows(rows);
    if (!dialog.openModal()) return;

    activeSourceCaptions_ = dialog.selectedCaptions();
    hasExplicitSourceFilter_ =
        (activeSourceCaptions_.size() != sources_.size());
    if (!hasExplicitSourceFilter_) {
        activeSourceCaptions_.clear();
        for (const auto& source : sources_) {
            activeSourceCaptions_.push_back(source.caption);
        }
    }

    updateSourceFilterButton();
    repopulateLanguageChoice();
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

    const std::string localSwordRoot = userSwordRootDir();
    sword::SWMgr destMgr(localSwordRoot.c_str(),
                         true,
                         new sword::MarkupFilterMgr(sword::FMT_XHTML),
                         false,
                         false);

    std::vector<int> successfulRows;
    std::vector<std::string> failureMessages;
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

void ModuleManagerDialog::onSourceChanged(Fl_Widget* /*widget*/, void* data) {
    auto* self = static_cast<ModuleManagerDialog*>(data);
    if (!self) return;
    // TODO: implement source change handling if needed
}

void ModuleManagerDialog::onChooseSources(Fl_Widget* /*widget*/, void* data) {
    auto* self = static_cast<ModuleManagerDialog*>(data);
    if (!self) return;
    self->chooseVisibleSources();
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
    self->installOrUpdateSelectedModules();
}

} // namespace verdad
