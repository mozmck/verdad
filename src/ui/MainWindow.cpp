#include "ui/MainWindow.h"
#include "app/BuildConfig.h"
#include "app/VerdadApp.h"
#include "import/ImportedModuleManager.h"
#include "ui/LeftPane.h"
#include "ui/BiblePane.h"
#include "ui/FilterableChoiceWidget.h"
#include "ui/HtmlWidget.h"
#include "ui/RightPane.h"
#include "ui/ModuleManagerDialog.h"
#include "ui/StyledTabs.h"
#include "ui/UiFontUtils.h"
#include "ui/WrappingChoice.h"
#include "reading/DateUtils.h"
#include "sword/SwordManager.h"
#include "search/SearchIndexer.h"
#include "app/PerfTrace.h"
#include "tags/TagManager.h"

#include <sqlite3.h>

#include <FL/Fl.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_ask.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Spinner.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Browser_.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Multi_Browser.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Input_.H>
#include <FL/Fl_Menu_.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_PNG_Image.H>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace verdad {
namespace {

namespace fs = std::filesystem;
constexpr const char* kVerdadProjectUrl = "https://github.com/mozmck/verdad";
constexpr int kDefaultMaxCachedTabDocs = 4;
constexpr double kDailyDateCheckSeconds = 60.0 * 60.0;
constexpr double kDefaultUserDataSyncPollSeconds = 120.0;
constexpr double kDefaultUserDataSyncDebounceSeconds = 60.0;

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

void applyToggleButtonPressedColorRecursively(Fl_Widget* widget) {
    if (!widget) return;

    if (auto* button = dynamic_cast<Fl_Button*>(widget)) {
        if (button->type() == FL_TOGGLE_BUTTON) {
            button->selection_color(fl_darker(button->color()));
        }
    }

    if (auto* group = dynamic_cast<Fl_Group*>(widget)) {
        for (int i = 0; i < group->children(); ++i) {
            applyToggleButtonPressedColorRecursively(group->child(i));
        }
    }
}

int configuredMaxCachedTabDocs() {
    static const int value = []() {
        const char* env = std::getenv("VERDAD_MAX_CACHED_TAB_DOCS");
        if (!env || !*env) return kDefaultMaxCachedTabDocs;

        char* end = nullptr;
        long parsed = std::strtol(env, &end, 10);
        if (end == env) return kDefaultMaxCachedTabDocs;
        if (parsed < 0) return 0;
        if (parsed > 1024) return 1024;
        return static_cast<int>(parsed);
    }();
    return value;
}

double configuredSeconds(const char* envName,
                         double fallback,
                         double minValue,
                         double maxValue) {
    const char* env = std::getenv(envName);
    if (!env || !*env) return fallback;

    char* end = nullptr;
    double parsed = std::strtod(env, &end);
    if (end == env) return fallback;
    return std::clamp(parsed, minValue, maxValue);
}

double userDataSyncPollSeconds() {
    static const double value = configuredSeconds(
        "VERDAD_USER_DATA_SYNC_POLL_SECONDS",
        kDefaultUserDataSyncPollSeconds,
        5.0,
        60.0 * 60.0 * 12.0);
    return value;
}

double userDataSyncDebounceSeconds() {
    static const double value = configuredSeconds(
        "VERDAD_USER_DATA_SYNC_DEBOUNCE_SECONDS",
        kDefaultUserDataSyncDebounceSeconds,
        1.0,
        60.0 * 60.0);
    return value;
}

std::string htmlEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

std::string buildModuleRefLabel(VerdadApp* app,
                                const std::string& module,
                                const std::string& reference) {
    if (!app) return module + ":" + reference;
    std::string shortRef = app->swordManager().getShortReference(module, reference);
    if (shortRef.empty()) shortRef = reference;
    return module + ":" + shortRef;
}

std::string buildBibleReference(const std::string& book, int chapter, int verse) {
    std::string refBook = trimCopy(book);
    if (refBook.empty()) refBook = "Genesis";
    int refChapter = std::max(1, chapter);
    int refVerse = std::max(1, verse);
    return refBook + " " + std::to_string(refChapter) +
           ":" + std::to_string(refVerse);
}

std::string extractStrongsToken(const std::string& query) {
    static const std::regex strongRe(R"(([HhGg]\d+[A-Za-z]?))");
    std::smatch m;
    if (!std::regex_search(query, m, strongRe)) return "";
    std::string tok = m[1].str();
    for (char& c : tok) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }
    return tok;
}

const char* kFallbackHelpHtml = R"(
<div class="help-doc">
  <h2 id="overview">Overview</h2>
  <p>Verdad opens with the search, modules, and tags pane on the left, Bible text in the center, and commentary, dictionary, or general books on the right.</p>
  <h2 id="search">Search</h2>
  <p>The search box supports multi-word, exact-phrase, regex, and Strong's lookups. Select a result to update the preview pane and double-click to navigate.</p>
  <h2 id="regex">Regex</h2>
  <p>Regex uses ECMAScript syntax and is case-insensitive. Use <code>\\b</code> for word boundaries and <code>.*</code> to span text between terms.</p>
  <h2 id="features">Features</h2>
  <p>Use the Bible toolbar to switch modules, toggle paragraph mode, turn red-letter words on or off, manage parallel columns, and show or hide study markers such as Strong's, morphology, notes, and cross references.</p>
  <p>Right-clicking in the Bible pane can copy verses or selections with the reference first or last, and the Studypad editor supports copy, cut, paste, and verse-link insertion from the current Bible module.</p>
</div>
)";

std::string readFileOrFallback(const std::string& path,
                               const std::string& fallback) {
    std::ifstream in(path);
    if (!in.is_open()) return fallback;
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

std::vector<std::pair<std::string, std::string>> parseHelpTopics(
    const std::string& html) {
    std::vector<std::pair<std::string, std::string>> topics;
    static const std::regex headingRe(
        R"help(<h2[^>]*id\s*=\s*"([^"]+)"[^>]*>\s*([^<]+?)\s*</h2>)help",
        std::regex::icase);

    auto begin = std::sregex_iterator(html.begin(), html.end(), headingRe);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        topics.push_back({trimCopy((*it)[1].str()), trimCopy((*it)[2].str())});
    }
    return topics;
}

void appendEscapedTextLines(std::ostringstream& out,
                            const std::string& text,
                            const char* cls) {
    std::istringstream ss(text);
    std::string line;
    bool emitted = false;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out << "<span class=\"mag-line " << cls << "\">"
            << htmlEscape(line) << "</span>";
        emitted = true;
    }
    if (!emitted) {
        out << "<span class=\"mag-line " << cls << "\"></span>";
    }
}

std::vector<std::string> extractStrongsTokens(const std::string& strongs) {
    std::vector<std::string> prefixed;
    std::vector<std::string> numeric;
    std::unordered_set<std::string> seen;

    static const std::regex strongRe(
        R"((?:^|[|,;\s:])([HGhg]?\d+[A-Za-z]?)(?=$|[|,;\s]))");
    auto it = std::sregex_iterator(strongs.begin(), strongs.end(), strongRe);
    auto end = std::sregex_iterator();
    for (; it != end; ++it) {
        std::string tok = (*it)[1].str();
        for (char& c : tok) {
            if (std::isalpha(static_cast<unsigned char>(c))) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
        }
        if (tok.empty() || seen.count(tok) != 0) continue;

        if (std::isalpha(static_cast<unsigned char>(tok[0])) &&
            tok[0] != 'H' && tok[0] != 'G') {
            continue;
        }

        seen.insert(tok);
        if (std::isalpha(static_cast<unsigned char>(tok[0]))) {
            prefixed.push_back(tok);
        } else {
            numeric.push_back(tok);
        }
    }

    if (!prefixed.empty()) {
        return prefixed;
    }
    return numeric;
}

char strongPrefixFromToken(const std::string& token) {
    if (token.empty()) return 0;
    if (!std::isalpha(static_cast<unsigned char>(token[0]))) return 0;
    return static_cast<char>(
        std::toupper(static_cast<unsigned char>(token[0])));
}

std::string escapeChoiceLabel(const std::string& label) {
    std::string escaped = label;
    size_t pos = 0;
    while ((pos = escaped.find('/', pos)) != std::string::npos) {
        escaped.replace(pos, 1, "\\/");
        pos += 2;
    }
    return escaped;
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

bool isSupportedImportFile(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return ext == ".pdf" || ext == ".txt" || ext == ".md" || ext == ".markdown";
}

class ImportFilesDialog {
public:
    explicit ImportFilesDialog(MainWindow* owner)
        : owner_(owner)
        , app_(VerdadApp::instance()) {}

    bool runModal() {
        if (!owner_ || !app_) return false;

        window_ = new Fl_Double_Window(920, 560, "Import Files");
        window_->begin();

        int x = 14;
        int y = 12;
        auto* intro = new Fl_Box(x, y, 892, 36,
                                 "Import PDF, TXT, or Markdown files as searchable modules in General Books.");
        intro->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        y += 40;

        const int gap = 12;
        const int colW = 440;
        const int browserH = 240;

        auto* existingLabel = new Fl_Box(x, y, colW, 22, "Imported files");
        existingLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        auto* pendingLabel = new Fl_Box(x + colW + gap, y, colW, 22, "Pending import");
        pendingLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        y += 22;

        existingBrowser_ = new Fl_Multi_Browser(x, y, colW, browserH);
        pendingBrowser_ = new Fl_Hold_Browser(x + colW + gap, y, colW, browserH);
        y += browserH + 8;

        auto* removeImportButton = new Fl_Button(x, y, 130, 28, "Remove import");
        removeImportButton->callback(onRemoveImport, this);

        auto* addFilesButton = new Fl_Button(x + colW + gap, y, 96, 28, "Add files...");
        addFilesButton->callback(onAddFiles, this);
        auto* addFolderButton = new Fl_Button(x + colW + gap + 104, y, 102, 28, "Add folder...");
        addFolderButton->callback(onAddFolder, this);
        auto* clearPendingButton = new Fl_Button(x + colW + gap + 214, y, 112, 28, "Clear pending");
        clearPendingButton->callback(onClearPending, this);
        y += 42;

        copyCheck_ = new Fl_Check_Button(x, y, 892, 24,
                                         "Copy imported files to the user data directory");
        copyCheck_->value(1);
        y += 30;

        auto* tagsLabel = new Fl_Box(x, y, 60, 24, "Tags");
        tagsLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        tagsInput_ = new Fl_Input(x + 64, y, 842, 24);
        y += 32;

        auto* notesLabel = new Fl_Box(x, y, 60, 24, "Notes");
        notesLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        notesInput_ = new Fl_Multiline_Input(x + 64, y, 842, 72);
        y += 82;

        statusBox_ = new Fl_Box(x, y, 892, 48, "");
        statusBox_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);

        auto* closeButton = new Fl_Button(920 - 210, 560 - 42, 90, 28, "Close");
        closeButton->callback(onClose, this);
        auto* importButton = new Fl_Return_Button(920 - 110, 560 - 42, 90, 28, "Import");
        importButton->callback(onImport, this);

        window_->end();
        ui_font::applyCurrentAppUiFont(window_);

        refreshExisting();
        refreshPending();
        setStatus("Add individual files or an entire folder, then import them into the library.");

        window_->set_modal();
        window_->show();
        while (window_->shown()) {
            Fl::wait();
        }

        delete window_;
        window_ = nullptr;
        return changed_;
    }

private:
    void setStatus(const std::string& text) {
        if (statusBox_) statusBox_->copy_label(text.c_str());
    }

    void refreshExisting() {
        if (!existingBrowser_) return;
        existingRecords_ = app_->importedModuleManager().records();
        existingBrowser_->clear();
        for (const auto& record : existingRecords_) {
            std::string label = record.moduleName + " [" + record.fileType + "]";
            if (!trimCopy(record.tags).empty()) {
                label += " {" + trimCopy(record.tags) + "}";
            }
            existingBrowser_->add(label.c_str());
        }
    }

    void refreshPending() {
        if (!pendingBrowser_) return;
        pendingBrowser_->clear();
        for (const auto& path : pendingPaths_) {
            pendingBrowser_->add(path.c_str());
        }
    }

    void addPath(const std::string& path) {
        std::error_code ec;
        fs::path filePath(path);
        fs::path canonical = fs::weakly_canonical(filePath, ec);
        std::string normalized = (ec ? filePath.lexically_normal() : canonical).string();
        if (!isSupportedImportFile(fs::path(normalized))) return;
        if (std::find(pendingPaths_.begin(), pendingPaths_.end(), normalized) ==
            pendingPaths_.end()) {
            pendingPaths_.push_back(normalized);
        }
    }

    void addFiles() {
        Fl_Native_File_Chooser chooser;
        chooser.title("Import Files");
        chooser.type(Fl_Native_File_Chooser::BROWSE_MULTI_FILE);
        chooser.filter("Supported Files\t*.{pdf,PDF,txt,TXT,md,MD,markdown,MARKDOWN}");

        int result = chooser.show();
        if (result != 0) {
            if (result < 0) setStatus("Unable to open the file chooser.");
            return;
        }

        int count = chooser.count();
        if (count <= 0) {
            const char* filename = chooser.filename();
            if (filename) addPath(filename);
        } else {
            for (int i = 0; i < count; ++i) {
                const char* filename = chooser.filename(i);
                if (filename) addPath(filename);
            }
        }
        refreshPending();
        setStatus("Added files to the pending import list.");
    }

    void addFolder() {
        Fl_Native_File_Chooser chooser;
        chooser.title("Choose Folder to Import");
        chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);

        int result = chooser.show();
        if (result != 0) {
            if (result < 0) setStatus("Unable to open the folder chooser.");
            return;
        }

        std::string directory = chooser.filename() ? chooser.filename() : "";
        if (directory.empty()) return;

        std::error_code ec;
        for (fs::recursive_directory_iterator it(fs::path(directory), ec), end;
             !ec && it != end;
             it.increment(ec)) {
            if (it->is_regular_file() && isSupportedImportFile(it->path())) {
                addPath(it->path().string());
            }
        }

        refreshPending();
        setStatus("Scanned the selected folder for supported files.");
    }

    void clearPending() {
        pendingPaths_.clear();
        refreshPending();
        setStatus("Cleared the pending import list.");
    }

    void removeImport() {
        if (!existingBrowser_) {
            setStatus("Select one or more imported modules to remove.");
            return;
        }

        std::vector<std::string> moduleNames;
        for (int line = 1; line <= existingBrowser_->size(); ++line) {
            if (!existingBrowser_->selected(line)) continue;
            int index = line - 1;
            if (index >= 0 && index < static_cast<int>(existingRecords_.size())) {
                moduleNames.push_back(existingRecords_[static_cast<size_t>(index)].moduleName);
            }
        }

        if (moduleNames.empty()) {
            setStatus("Select one or more imported modules to remove.");
            return;
        }

        int removedCount = 0;
        std::string firstError;
        for (const auto& moduleName : moduleNames) {
            std::string error;
            if (!app_->importedModuleManager().removeModule(moduleName, &error)) {
                if (firstError.empty()) firstError = error.empty() ? "Failed to remove imported file." : error;
                continue;
            }
            ++removedCount;
        }

        if (removedCount == 0) {
            setStatus(firstError.empty() ? "Failed to remove imported file." : firstError);
            return;
        }

        changed_ = true;
        app_->refreshSearchIndexCatalog();
        owner_->refresh();
        refreshExisting();
        if (removedCount == 1) {
            setStatus("Removed imported file.");
        } else {
            std::ostringstream message;
            message << "Removed " << removedCount << " imported files.";
            setStatus(message.str());
        }
    }

    void importPending() {
        if (pendingPaths_.empty()) {
            setStatus("No files are queued for import.");
            return;
        }

        ImportedModuleManager::ImportOptions options;
        options.copyFiles = copyCheck_ && copyCheck_->value() != 0;
        options.tags = tagsInput_ ? trimCopy(tagsInput_->value()) : "";
        options.notes = notesInput_ ? trimCopy(notesInput_->value()) : "";

        auto result = app_->importedModuleManager().importPaths(pendingPaths_, options);
        if (result.importedCount() > 0 || result.updatedCount() > 0) {
            changed_ = true;
            app_->refreshSearchIndexCatalog();
            owner_->refresh();
        }

        std::ostringstream summary;
        summary << "Imported " << result.importedCount()
                << ", updated " << result.updatedCount()
                << ", skipped " << result.skippedCount()
                << ", failed " << result.failureCount() << ".";
        setStatus(summary.str());

        refreshExisting();
        if (result.failureCount() == 0) {
            pendingPaths_.clear();
            refreshPending();
        }
    }

    static void onAddFiles(Fl_Widget*, void* data) {
        auto* self = static_cast<ImportFilesDialog*>(data);
        if (self) self->addFiles();
    }

    static void onAddFolder(Fl_Widget*, void* data) {
        auto* self = static_cast<ImportFilesDialog*>(data);
        if (self) self->addFolder();
    }

    static void onClearPending(Fl_Widget*, void* data) {
        auto* self = static_cast<ImportFilesDialog*>(data);
        if (self) self->clearPending();
    }

    static void onRemoveImport(Fl_Widget*, void* data) {
        auto* self = static_cast<ImportFilesDialog*>(data);
        if (self) self->removeImport();
    }

    static void onImport(Fl_Widget*, void* data) {
        auto* self = static_cast<ImportFilesDialog*>(data);
        if (self) self->importPending();
    }

    static void onClose(Fl_Widget*, void* data) {
        auto* self = static_cast<ImportFilesDialog*>(data);
        if (self && self->window_) self->window_->hide();
    }

    MainWindow* owner_ = nullptr;
    VerdadApp* app_ = nullptr;
    Fl_Double_Window* window_ = nullptr;
    Fl_Multi_Browser* existingBrowser_ = nullptr;
    Fl_Hold_Browser* pendingBrowser_ = nullptr;
    Fl_Check_Button* copyCheck_ = nullptr;
    Fl_Input* tagsInput_ = nullptr;
    Fl_Multiline_Input* notesInput_ = nullptr;
    Fl_Box* statusBox_ = nullptr;
    std::vector<std::string> pendingPaths_;
    std::vector<ImportedModuleManager::ModuleRecord> existingRecords_;
    bool changed_ = false;
};

std::string pathLeaf(const std::string& path) {
    if (path.empty()) return "";
    size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

std::string normalizePath(const std::string& path) {
    if (path.empty()) return "";

    std::error_code ec;
    fs::path normalized(path);
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
    std::transform(left.begin(), left.end(), left.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    std::transform(right.begin(), right.end(), right.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
#endif
    return left == right;
}

bool pathIsInsideDirectory(const std::string& path, const std::string& directory) {
    if (path.empty() || directory.empty()) return false;

    fs::path target(normalizePath(path));
    fs::path base(normalizePath(directory));
    auto targetIt = target.begin();
    auto baseIt = base.begin();
    for (; baseIt != base.end(); ++baseIt, ++targetIt) {
        if (targetIt == target.end() || *targetIt != *baseIt) return false;
    }
    return true;
}

bool filenameLooksLikeSyncConflict(const fs::path& path) {
    return path.filename().string().find(".sync-conflict-") != std::string::npos;
}

std::string fileMetadataStamp(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) return "missing";

    std::string type = "other";
    uintmax_t size = 0;
    if (fs::is_directory(path, ec) && !ec) {
        type = "dir";
    } else if (fs::is_regular_file(path, ec) && !ec) {
        type = "file";
        size = fs::file_size(path, ec);
        if (ec) size = 0;
    }

    ec.clear();
    auto writeTime = fs::last_write_time(path, ec);
    long long writeTicks = ec ? 0 : writeTime.time_since_epoch().count();
    return type + ":" + std::to_string(size) + ":" + std::to_string(writeTicks);
}

void hashBytes(uint64_t& hash, const void* data, size_t size) {
    static constexpr uint64_t kFnvPrime = 1099511628211ULL;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= kFnvPrime;
    }
}

void hashText(uint64_t& hash, const char* text) {
    if (!text) {
        const char marker = '\0';
        hashBytes(hash, &marker, 1);
        return;
    }
    hashBytes(hash, text, std::strlen(text));
    const char marker = '\xff';
    hashBytes(hash, &marker, 1);
}

std::string sqliteContentStamp(const fs::path& path,
                               const std::vector<const char*>& queries) {
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) return "missing";

    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(path.string().c_str(),
                             &db,
                             SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK || !db) {
        if (db) sqlite3_close(db);
        return "db-unreadable:" + fileMetadataStamp(path);
    }

    uint64_t hash = 1469598103934665603ULL;
    int64_t rowCount = 0;
    bool ok = true;

    for (const char* sql : queries) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            ok = false;
            if (stmt) sqlite3_finalize(stmt);
            break;
        }

        hashText(hash, sql);
        int stepRc = SQLITE_ROW;
        while ((stepRc = sqlite3_step(stmt)) == SQLITE_ROW) {
            ++rowCount;
            const char rowMarker = '\x1e';
            hashBytes(hash, &rowMarker, 1);
            int columnCount = sqlite3_column_count(stmt);
            for (int i = 0; i < columnCount; ++i) {
                const char columnMarker = '\x1f';
                hashBytes(hash, &columnMarker, 1);
                if (sqlite3_column_type(stmt, i) == SQLITE_NULL) {
                    hashText(hash, nullptr);
                } else {
                    const auto* text =
                        reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
                    hashText(hash, text ? text : "");
                }
            }
        }
        if (stepRc != SQLITE_DONE) ok = false;
        sqlite3_finalize(stmt);
        if (!ok) break;
    }

    sqlite3_close(db);
    if (!ok) return "db-error:" + fileMetadataStamp(path);

    std::ostringstream out;
    out << "sqlite-content:" << rowCount << ':' << std::hex << hash;
    return out.str();
}

std::string tagsDbContentStamp(const fs::path& path) {
    return sqliteContentStamp(path, {
        "SELECT name, color FROM tags ORDER BY name;",
        "SELECT resource_kind, module_name, source_key, selection_text, tag_name "
        "FROM tag_items ORDER BY resource_kind, module_name, source_key, selection_text, tag_name;",
        "SELECT verse_key, tag_name FROM verse_tags ORDER BY verse_key, tag_name;",
    });
}

std::string readingPlansDbContentStamp(const fs::path& path) {
    return sqliteContentStamp(path, {
        "SELECT id, name, description, color FROM reading_plans ORDER BY id;",
        "SELECT id, plan_id, day_number, title, completed "
        "FROM reading_plan_days ORDER BY id;",
        "SELECT id, day_id, sort_order, reference "
        "FROM reading_plan_day_passages ORDER BY id;",
        "SELECT plan_id, start_date FROM reading_plan_schedules ORDER BY plan_id;",
        "SELECT id, plan_id, day_number, start_date "
        "FROM reading_plan_schedule_offsets ORDER BY id;",
        "SELECT module_name, start_date FROM sword_reading_plan_schedules ORDER BY module_name;",
        "SELECT id, module_name, day_number, start_date "
        "FROM sword_reading_plan_schedule_offsets ORDER BY id;",
        "SELECT module_name, day_number, completed "
        "FROM sword_reading_plan_progress ORDER BY module_name, day_number;",
    });
}

std::string pathStateStamp(const fs::path& path) {
    const std::string filename = path.filename().string();
    if (filename == "tags.db") {
        return tagsDbContentStamp(path);
    }
    if (filename == "reading_plans.db") {
        return readingPlansDbContentStamp(path);
    }

    return fileMetadataStamp(path);
}

void addRequiredSnapshotPath(std::unordered_map<std::string, std::string>& snapshot,
                             const fs::path& path) {
    snapshot[normalizePath(path.string())] = pathStateStamp(path);
}

void addOptionalSnapshotPath(std::unordered_map<std::string, std::string>& snapshot,
                             const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) return;
    snapshot[normalizePath(path.string())] = pathStateStamp(path);
}

std::unordered_map<std::string, std::string> buildUserDataSnapshot(VerdadApp* app) {
    std::unordered_map<std::string, std::string> snapshot;
    if (!app) return snapshot;

    fs::path userDataDir(app->getUserDataDir());
    fs::path studypadDir(app->getStudypadDir());

    addRequiredSnapshotPath(snapshot, userDataDir / "tags.db");
    addRequiredSnapshotPath(snapshot, userDataDir / "reading_plans.db");
    addRequiredSnapshotPath(snapshot, studypadDir);

    std::error_code ec;
    fs::directory_iterator studypadIt(studypadDir, ec);
    fs::directory_iterator end;
    while (!ec && studypadIt != end) {
        if (fs::is_regular_file(studypadIt->path(), ec) && !ec) {
            addOptionalSnapshotPath(snapshot, studypadIt->path());
        }
        ec.clear();
        studypadIt.increment(ec);
    }

    ec.clear();
    fs::directory_iterator dataIt(userDataDir, ec);
    while (!ec && dataIt != end) {
        if (fs::is_regular_file(dataIt->path(), ec) &&
            !ec &&
            filenameLooksLikeSyncConflict(dataIt->path())) {
            addOptionalSnapshotPath(snapshot, dataIt->path());
        }
        ec.clear();
        dataIt.increment(ec);
    }

    return snapshot;
}

std::vector<std::string> changedSnapshotPaths(
    const std::unordered_map<std::string, std::string>& oldSnapshot,
    const std::unordered_map<std::string, std::string>& newSnapshot) {
    std::vector<std::string> changed;
    for (const auto& kv : oldSnapshot) {
        auto it = newSnapshot.find(kv.first);
        if (it == newSnapshot.end() || it->second != kv.second) {
            changed.push_back(kv.first);
        }
    }
    for (const auto& kv : newSnapshot) {
        if (oldSnapshot.find(kv.first) == oldSnapshot.end()) {
            changed.push_back(kv.first);
        }
    }
    std::sort(changed.begin(), changed.end());
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
    return changed;
}

bool snapshotHasSyncConflict(
    const std::unordered_map<std::string, std::string>& snapshot) {
    return std::any_of(snapshot.begin(), snapshot.end(),
                       [](const auto& kv) {
                           return filenameLooksLikeSyncConflict(fs::path(kv.first));
                       });
}

bool ensureDirectoryExists(const std::string& path) {
    if (path.empty()) return false;

    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) return false;
    return fs::is_directory(fs::path(path), ec) && !ec;
}

std::string shellQuote(const std::string& text) {
#ifdef _WIN32
    std::string out = "\"";
    for (char c : text) {
        if (c == '"') out += "\\\"";
        else out.push_back(c);
    }
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (char c : text) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out += "'";
    return out;
#endif
}

std::string makeUniqueTempDir(const std::string& prefix) {
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec);
    if (ec) base = fs::path(".");

    for (int i = 0; i < 100; ++i) {
        auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        fs::path candidate = base / (prefix + std::to_string(now + i));
        if (fs::create_directory(candidate, ec)) {
            return candidate.string();
        }
        ec.clear();
    }
    return "";
}

bool copyBinaryFile(const std::string& fromPath, const std::string& toPath) {
    std::error_code ec;
    fs::copy_file(fromPath, toPath, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

bool copyDirectoryRecursive(const std::string& fromPath,
                            const std::string& toPath) {
    std::error_code ec;
    fs::remove_all(fs::path(toPath), ec);
    ec.clear();
    fs::copy(fs::path(fromPath),
             fs::path(toPath),
             fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             ec);
    return !ec;
}

bool runZipArchive(const std::string& workingDir,
                   const std::string& archivePath,
                   bool includeTagsDb,
                   bool includeReadingPlansDb) {
    std::string cmd = "cd " + shellQuote(workingDir) +
                      " && zip -rq " + shellQuote(archivePath) +
                      " preferences.conf";
    if (includeTagsDb) cmd += " tags.db";
    if (includeReadingPlansDb) cmd += " reading_plans.db";
    cmd += " studypad";
#ifdef _WIN32
    cmd += " >NUL 2>NUL";
#else
    cmd += " >/dev/null 2>&1";
#endif
    return std::system(cmd.c_str()) == 0;
}

bool runUnzipArchive(const std::string& archivePath,
                     const std::string& outputDir) {
    std::string cmd = "unzip -oq " + shellQuote(archivePath) +
                      " -d " + shellQuote(outputDir);
#ifdef _WIN32
    cmd += " >NUL 2>NUL";
#else
    cmd += " >/dev/null 2>&1";
#endif
    return std::system(cmd.c_str()) == 0;
}

bool openExternalUrl(const std::string& url) {
    if (url.empty()) return false;

#ifdef _WIN32
    std::string cmd = "cmd /c start \"\" " + shellQuote(url);
#elif defined(__APPLE__)
    std::string cmd = "open " + shellQuote(url) + " >/dev/null 2>&1";
#else
    std::string cmd = "xdg-open " + shellQuote(url) + " >/dev/null 2>&1";
#endif
    return std::system(cmd.c_str()) == 0;
}

std::string languageDisplayName(const std::string& languageCode) {
    std::string code = normalizeLanguageCode(languageCode);
    if (code == "en") return "English";
    if (code == "es") return "Spanish";
    if (code == "fr") return "French";
    if (code == "de") return "German";
    if (code == "pt") return "Portuguese";
    if (code == "it") return "Italian";
    if (code == "ru") return "Russian";
    if (code == "nl") return "Dutch";
    if (code == "el" || code == "grc") return "Greek";
    if (code == "he") return "Hebrew";
    if (code == "la") return "Latin";
    if (code == "ar") return "Arabic";
    if (code == "zh") return "Chinese";

    if (code.empty()) return "Default";

    std::string label = code;
    std::transform(label.begin(), label.end(), label.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    return label;
}

std::string offlineTranslationPairsLabel(const WikDictScanReport& report) {
    std::ostringstream label;
    label << "Detected: ";
    bool hasDetectedResource = false;
    for (const auto& pair : report.pairs) {
        if (hasDetectedResource) label << ", ";
        label << pair.sourceLanguage << " -> " << pair.targetLanguage;
        hasDetectedResource = true;
    }
    for (const auto& language : report.analysisLanguages) {
        if (hasDetectedResource) label << ", ";
        label << language << " analysis";
        hasDetectedResource = true;
    }
    if (!hasDetectedResource) label << "No WikDict resources";
    return label.str();
}

std::string joinDisplayValues(const std::vector<std::string>& values) {
    std::ostringstream joined;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) joined << ", ";
        joined << values[i];
    }
    return joined.str();
}

std::string offlineTranslationIssuesLabel(const WikDictScanReport& report) {
    if (report.issues.empty()) return "Errors: None";

    std::ostringstream label;
    label << "Errors: ";
    constexpr size_t kDisplayedIssues = 3;
    const size_t count = std::min(report.issues.size(), kDisplayedIssues);
    for (size_t i = 0; i < count; ++i) {
        if (i) label << "; ";
        if (!report.issues[i].fileName.empty()) {
            label << report.issues[i].fileName << ": ";
        }
        label << report.issues[i].message;
    }
    if (report.issues.size() > count) {
        label << "; plus " << (report.issues.size() - count) << " more";
    }
    return label.str();
}

void updateOfflineTranslationScanLabels(Fl_Box* detectedBox,
                                        Fl_Box* issuesBox,
                                        const WikDictScanReport& report) {
    if (detectedBox) {
        detectedBox->copy_label(offlineTranslationPairsLabel(report).c_str());
        detectedBox->redraw();
    }
    if (issuesBox) {
        issuesBox->copy_label(offlineTranslationIssuesLabel(report).c_str());
        issuesBox->redraw();
    }
}

std::vector<std::string> dictionaryLanguageCodes(
    VerdadApp* app,
    const VerdadApp::PreviewDictionarySettings& settings) {
    std::set<std::string> codes;

    if (app) {
        for (const auto& mod : app->swordManager().getBibleModules()) {
            std::string code = normalizeLanguageCode(mod.language);
            if (!code.empty()) codes.insert(code);
        }
        if (!app->wordDictionaryModules("en").empty()) codes.insert("en");
    }

    for (const auto& kv : settings.languageModules) {
        std::string code = normalizeLanguageCode(kv.first);
        if (!code.empty()) codes.insert(code);
    }

    std::vector<std::string> ordered(codes.begin(), codes.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const std::string& a, const std::string& b) {
                  return languageDisplayName(a) < languageDisplayName(b);
              });
    return ordered;
}

std::string unescapeChoiceLabel(const char* label) {
    if (!label) return "";

    std::string unescaped;
    for (size_t i = 0; label[i] != '\0'; ++i) {
        if (label[i] == '\\' && label[i + 1] == '/') {
            unescaped.push_back('/');
            ++i;
            continue;
        }
        unescaped.push_back(label[i]);
    }
    return unescaped;
}

int findChoiceIndexByLabel(Fl_Choice* choice, const std::string& label) {
    if (!choice || label.empty()) return -1;

    for (int i = 0; i < choice->size(); ++i) {
        const Fl_Menu_Item* item = choice->menu() ? &choice->menu()[i] : nullptr;
        if (!item || !item->label()) continue;

        if (label == item->label() ||
            label == unescapeChoiceLabel(item->label())) {
            return i;
        }
    }

    return -1;
}

bool populateChoiceWithItems(Fl_Choice* choice,
                             const std::vector<std::string>& items,
                             const std::string& selected,
                             const std::string& emptyLabel) {
    if (!choice) return false;
    choice->clear();

    if (items.empty()) {
        if (!emptyLabel.empty()) {
            choice->add(emptyLabel.c_str());
            choice->value(0);
        }
        choice->deactivate();
        return false;
    }

    choice->activate();
    for (const auto& item : items) {
        choice->add(escapeChoiceLabel(item).c_str());
    }

    int selectedIndex = selected.empty() ? -1 : findChoiceIndexByLabel(choice, selected);

    if (selectedIndex >= 0) {
        choice->value(selectedIndex);
    } else if (choice->size() > 0) {
        choice->value(0);
    }

    return true;
}

std::vector<std::string> preferredPreviewLexicons(VerdadApp* app,
                                                  const std::string& strongToken) {
    std::vector<std::string> preferred;
    if (!app) return preferred;

    char prefix = strongPrefixFromToken(strongToken);
    if (prefix == 'H' || prefix == 'G') {
        std::string module = app->preferredPreviewDictionary(prefix);
        if (!module.empty()) preferred.push_back(module);
        return preferred;
    }

    std::string hebrewModule = app->preferredPreviewDictionary('H');
    if (!hebrewModule.empty()) preferred.push_back(hebrewModule);
    std::string greekModule = app->preferredPreviewDictionary('G');
    if (!greekModule.empty() &&
        std::find(preferred.begin(), preferred.end(), greekModule) == preferred.end()) {
        preferred.push_back(greekModule);
    }
    return preferred;
}

} // namespace

MainWindow::MainWindow(VerdadApp* app, int W, int H, const char* title)
    : Fl_Double_Window(W, H, title)
    , app_(app)
    , menuBar_(nullptr)
    , statusBar_(nullptr)
    , mainTile_(nullptr)
    , leftPane_(nullptr)
    , studyArea_(nullptr)
    , newStudyTabButton_(nullptr)
    , studyTabsWidget_(nullptr)
    , contentTile_(nullptr)
    , biblePane_(nullptr)
    , rightPane_(nullptr) {

    size_range(800, 600);

    const int menuH = 25;
    const int statusH = 22;

    menuBar_ = new Fl_Menu_Bar(0, 0, W, menuH);
    buildMenu();

    mainTile_ = new Fl_Tile(0, menuH, W, std::max(20, H - menuH - statusH));
    mainTile_->begin();

    int leftW = W * 25 / 100;
    leftPane_ = new LeftPane(app_, 0, menuH, leftW, mainTile_->h());

    int studyX = leftW;
    int studyW = W - leftW;
    const int newTabButtonW = 24;
    const int tabButtonPad = 2;
    const int tabsHeaderH = 25;
    studyArea_ = new Fl_Group(studyX, menuH, studyW, mainTile_->h());
    studyArea_->box(FL_FLAT_BOX);
    studyArea_->begin();

    newStudyTabButton_ = new Fl_Button(studyX + tabButtonPad,
                                       menuH + tabButtonPad,
                                       newTabButtonW, 21, "+");
    newStudyTabButton_->callback(onViewNewStudyTab, this);
    newStudyTabButton_->tooltip("Duplicate current study tab");

    studyTabsWidget_ = new StyledTabs(
        studyX + newTabButtonW + (tabButtonPad * 2),
        menuH,
        studyW - newTabButtonW - (tabButtonPad * 3),
        tabsHeaderH);
    //studyTabsWidget_->box(FL_FLAT_BOX);
    studyTabsWidget_->selection_color(studyTabsWidget_->color());
    studyTabsWidget_->setSelectionCallback([this](Fl_Widget* /*w*/) {
        syncStudyTabSelection();
    });
    studyTabsWidget_->setCloseCallback([this](Fl_Widget* w) {
        closeStudyTab(w);
    });
    // Close the tabs group so subsequent widgets are added to studyArea_,
    // not as tab page children.
    studyTabsWidget_->end();
    studyArea_->begin();

    int contentY = menuH + tabsHeaderH;
    int contentH = std::max(20, mainTile_->h() - tabsHeaderH);
    contentTile_ = new Fl_Tile(studyX, contentY, studyW, contentH);
    contentTile_->begin();

    int bibleW = studyW * 2 / 3;
    int commentaryW = studyW - bibleW;
    biblePane_ = new BiblePane(app_, studyX, contentY, bibleW, contentH);
    rightPane_ = new RightPane(app_, studyX + bibleW, contentY, commentaryW, contentH);

    contentTile_->end();
    studyArea_->end();
    studyArea_->resizable(contentTile_);

    mainTile_->end();

    statusBar_ = new Fl_Box(0, H - statusH, W, statusH);
    statusBar_->box(FL_THIN_UP_BOX);
    statusBar_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    statusBar_->copy_label("Ready");

    resizable(mainTile_);
    end();
    callback(onWindowClose, this);

    layoutStudyTabHeader();

    updateStatusBar();
    Fl::add_timeout(0.25, onStatusPoll, this);
    statusPollScheduled_ = true;
    lastDailyDateIso_ = reading::formatIsoDate(reading::today());
    Fl::add_timeout(kDailyDateCheckSeconds, onDailyDateCheck, this);
    dailyDateCheckScheduled_ = true;
    resetUserDataMonitorSnapshot();
    Fl::add_timeout(userDataSyncPollSeconds(), onUserDataSyncPoll, this);
    userDataSyncPollScheduled_ = true;
}

MainWindow::~MainWindow() {
    if (hoverDelayScheduled_) {
        Fl::remove_timeout(onHoverDelayTimeout, this);
        hoverDelayScheduled_ = false;
    }
    if (tabCacheEvictionScheduled_) {
        Fl::remove_timeout(onDeferredTabSnapshotEviction, this);
        tabCacheEvictionScheduled_ = false;
    }
    if (documentRestoreScheduled_) {
        Fl::remove_timeout(onDeferredDocumentRestore, this);
        documentRestoreScheduled_ = false;
    }
    if (statusPollScheduled_) {
        Fl::remove_timeout(onStatusPoll, this);
        statusPollScheduled_ = false;
    }
    if (dailyDateCheckScheduled_) {
        Fl::remove_timeout(onDailyDateCheck, this);
        dailyDateCheckScheduled_ = false;
    }
    if (userDataSyncPollScheduled_) {
        Fl::remove_timeout(onUserDataSyncPoll, this);
        userDataSyncPollScheduled_ = false;
    }
    if (searchHelpWindow_) {
        searchHelpWindow_->hide();
        delete searchHelpWindow_;
        searchHelpWindow_ = nullptr;
    }
    searchHelpTopicBrowser_ = nullptr;
    searchHelpHtml_ = nullptr;
}

void MainWindow::addStudyTab(const std::string& module,
                             const std::string& book,
                             int chapter,
                             int verse) {
    if (!studyTabsWidget_) return;

    captureActiveTabState();

    StudyContext ctx;
    std::string initModule = trimCopy(module);
    std::string initBook = trimCopy(book);
    int initChapter = chapter > 0 ? chapter : 1;
    int initVerse = verse > 0 ? verse : 1;

    if (initModule.empty() && biblePane_) {
        initModule = biblePane_->currentModule();
    }
    if (initModule.empty()) {
        auto bibles = app_->swordManager().getBibleModules();
        if (!bibles.empty()) initModule = bibles.front().name;
    }
    if (initBook.empty()) initBook = "Genesis";

    ctx.state.module = initModule;
    ctx.state.book = initBook;
    ctx.state.chapter = initChapter;
    ctx.state.verse = initVerse;
    ctx.state.paragraphMode = false;
    ctx.state.parallelMode = false;
    ctx.state.parallelModules.clear();
    ctx.state.biblePaneWidth = biblePane_ ? biblePane_->w() : 0;
    ensureStudyTabHistorySeeded(ctx.state);

    if (rightPane_) {
        ctx.state.commentaryModule = rightPane_->currentCommentaryModule();
        ctx.state.dictionaryModule = rightPane_->currentDictionaryModule();
    }
    if (!ctx.state.commentaryModule.empty()) {
        ctx.state.commentaryReference = initBook + " " + std::to_string(initChapter) +
                                        ":" + std::to_string(initVerse);
    }
    studyTabsWidget_->begin();
    ctx.tabGroup = new Fl_Group(studyTabsWidget_->x(),
                                studyTabsWidget_->y() + studyTabsWidget_->h(),
                                1, 1);
    ctx.tabGroup->copy_label(studyTabLabel(ctx.state).c_str());
    ctx.tabGroup->end();
    ui_font::applyCurrentAppUiFont(ctx.tabGroup);
    studyTabsWidget_->end();

    studyTabs_.push_back(std::move(ctx));
    int idx = static_cast<int>(studyTabs_.size()) - 1;
    studyTabsWidget_->value(studyTabs_[idx].tabGroup);
    activateStudyTab(idx);
}

void MainWindow::duplicateActiveStudyTab() {
    if (activeStudyTab_ < 0 || activeStudyTab_ >= static_cast<int>(studyTabs_.size())) {
        addStudyTab("", "Genesis", 1, 1);
        return;
    }

    captureActiveTabState();
    captureActiveTabDisplayBuffers();
    const StudyContext& src = studyTabs_[activeStudyTab_];

    StudyContext dst;
    dst.state = src.state;
    dst.bibleBuffer = src.bibleBuffer;
    dst.rightBuffer = src.rightBuffer;
    dst.hasBibleBuffer = src.hasBibleBuffer;
    dst.hasRightBuffer = src.hasRightBuffer;

    studyTabsWidget_->begin();
    dst.tabGroup = new Fl_Group(studyTabsWidget_->x(),
                                studyTabsWidget_->y() + studyTabsWidget_->h(),
                                1, 1);
    dst.tabGroup->copy_label(studyTabLabel(dst.state).c_str());
    dst.tabGroup->end();
    ui_font::applyCurrentAppUiFont(dst.tabGroup);
    studyTabsWidget_->end();

    studyTabs_.push_back(std::move(dst));
    int idx = static_cast<int>(studyTabs_.size()) - 1;
    studyTabsWidget_->value(studyTabs_[idx].tabGroup);
    activateStudyTab(idx);
    layoutStudyTabHeader();
}

void MainWindow::closeActiveStudyTab() {
    if (!studyTabsWidget_ || studyTabs_.size() <= 1) {
        layoutStudyTabHeader();
        return;
    }
    if (activeStudyTab_ < 0 || activeStudyTab_ >= static_cast<int>(studyTabs_.size())) {
        layoutStudyTabHeader();
        return;
    }

    captureActiveTabState();
    int closeIndex = activeStudyTab_;
    Fl_Group* doomedTabGroup = studyTabs_[closeIndex].tabGroup;
    if (appliedStudyTabGroup_ == doomedTabGroup) {
        appliedStudyTabGroup_ = nullptr;
    }

    if (doomedTabGroup) {
        studyTabsWidget_->remove(doomedTabGroup);
        delete doomedTabGroup;
    }
    studyTabs_.erase(studyTabs_.begin() + closeIndex);

    if (studyTabs_.empty()) {
        activeStudyTab_ = -1;
        addStudyTab("", "Genesis", 1, 1);
        layoutStudyTabHeader();
        return;
    }

    int nextIndex = std::min(closeIndex, static_cast<int>(studyTabs_.size()) - 1);
    studyTabsWidget_->value(studyTabs_[nextIndex].tabGroup);
    activeStudyTab_ = -1;
    activateStudyTab(nextIndex);
    layoutStudyTabHeader();
}

void MainWindow::closeStudyTab(Fl_Widget* tabGroup) {
    if (!studyTabsWidget_ || studyTabs_.size() <= 1 || !tabGroup) return;

    if (appliedStudyTabGroup_ == tabGroup) {
        appliedStudyTabGroup_ = nullptr;
    }

    // Find the index of the tab to close.
    int closeIndex = -1;
    for (size_t i = 0; i < studyTabs_.size(); ++i) {
        if (studyTabs_[i].tabGroup == tabGroup) {
            closeIndex = static_cast<int>(i);
            break;
        }
    }
    if (closeIndex < 0) return;

    // If closing the active tab, capture its state first.
    if (closeIndex == activeStudyTab_)
        captureActiveTabState();

    studyTabsWidget_->remove(tabGroup);
    delete tabGroup;
    studyTabs_.erase(studyTabs_.begin() + closeIndex);

    // Adjust activeStudyTab_ after erasing.
    if (activeStudyTab_ > closeIndex)
        --activeStudyTab_;
    else if (activeStudyTab_ == closeIndex)
        activeStudyTab_ = -1;

    if (studyTabs_.empty()) {
        activeStudyTab_ = -1;
        addStudyTab("", "Genesis", 1, 1);
        layoutStudyTabHeader();
        return;
    }

    int nextIndex = (activeStudyTab_ >= 0)
        ? activeStudyTab_
        : std::min(closeIndex, static_cast<int>(studyTabs_.size()) - 1);
    studyTabsWidget_->value(studyTabs_[nextIndex].tabGroup);
    if (activeStudyTab_ < 0)
        activateStudyTab(nextIndex);
    layoutStudyTabHeader();
}

void MainWindow::clearStudyTabs() {
    if (!studyTabsWidget_) return;

    for (auto& ctx : studyTabs_) {
        if (ctx.tabGroup) {
            studyTabsWidget_->remove(ctx.tabGroup);
            delete ctx.tabGroup;
        }
    }

    studyTabs_.clear();
    activeStudyTab_ = -1;
    appliedStudyTabGroup_ = nullptr;
    studyTabsWidget_->redraw();
    layoutStudyTabHeader();
}

void MainWindow::activateStudyTab(int index) {
    perf::ScopeTimer timer("MainWindow::activateStudyTab");
    if (index < 0 || index >= static_cast<int>(studyTabs_.size())) return;
    Fl_Widget* targetGroup = studyTabs_[index].tabGroup;
    if (studyTabsWidget_ && targetGroup && studyTabsWidget_->value() != targetGroup) {
        studyTabsWidget_->value(targetGroup);
    }
    if (activeStudyTab_ == index && appliedStudyTabGroup_ == targetGroup) {
        syncBibleHistoryUi();
        updateActiveStudyTabLabel();
        updateStatusBar();
        return;
    }

    perf::StepTimer step;
    int from = activeStudyTab_;
    captureStudyTabState(from);
    perf::logf("activateStudyTab from=%d to=%d captureActiveTabState: %.3f ms",
               from, index, step.elapsedMs());
    step.reset();
    const bool cacheDocs = configuredMaxCachedTabDocs() > 0;
    if (cacheDocs) {
        captureStudyTabDisplayBuffers(from);
        scheduleTabSnapshotEviction();
    } else {
        evictOldTabSnapshots();
    }
    perf::logf("activateStudyTab from=%d to=%d captureActiveTabDisplayBuffers: %.3f ms",
               from, index, step.elapsedMs());
    step.reset();
    hideWordInfo();
    perf::logf("activateStudyTab from=%d to=%d hideWordInfo: %.3f ms",
               from, index, step.elapsedMs());
    step.reset();
    activeStudyTab_ = index;
    studyTabs_[index].lastUsed = ++tabUseCounter_;
    applyTabState(index);
    perf::logf("activateStudyTab from=%d to=%d applyTabState: %.3f ms",
               from, index, step.elapsedMs());
    step.reset();
    syncBibleHistoryUi();
    perf::logf("activateStudyTab from=%d to=%d syncBibleHistoryUi: %.3f ms",
               from, index, step.elapsedMs());
    step.reset();
    updateActiveStudyTabLabel();
    perf::logf("activateStudyTab from=%d to=%d updateActiveStudyTabLabel: %.3f ms",
               from, index, step.elapsedMs());
    step.reset();
    layoutStudyTabHeader();
    perf::logf("activateStudyTab from=%d to=%d layoutStudyTabHeader: %.3f ms",
               from, index, step.elapsedMs());
    updateStatusBar();
}

void MainWindow::ensureDefaultStudyTab() {
    if (!studyTabs_.empty()) return;
    addStudyTab("", "Genesis", 1, 1);
}

void MainWindow::layoutStudyTabHeader() {
    if (!studyArea_ || !studyTabsWidget_ || !newStudyTabButton_) {
        return;
    }

    const int tabButtonPad = 2;
    const int newW = newStudyTabButton_->w();

    int headerY = studyArea_->y();
    int headerW = studyArea_->w();
    int tabsH = studyTabsWidget_->h();

    newStudyTabButton_->resize(studyArea_->x() + tabButtonPad,
                               headerY + tabButtonPad,
                               newW,
                               newStudyTabButton_->h());

    int tabsX = studyArea_->x() + newW + (tabButtonPad * 2);
    int tabsW = headerW - newW - (tabButtonPad * 3);
    studyTabsWidget_->resize(tabsX, headerY, std::max(40, tabsW), tabsH);

    studyTabsWidget_->updateCloseButtons();
    newStudyTabButton_->redraw();
    studyTabsWidget_->redraw();
}

std::string MainWindow::studyTabLabel(const StudyTabState& state) const {
    StudyHistoryEntry entry = historyEntryFromState(state);
    std::string label = historyEntryLabel(entry);
    if (!label.empty()) return label;
    return "Bible:" + buildBibleReference(state.book, state.chapter, state.verse);
}

std::string MainWindow::historyEntryLabel(const StudyHistoryEntry& entry) const {
    std::string module = trimCopy(entry.module);
    std::string reference = trimCopy(entry.reference);
    if (reference.empty()) return "";
    if (module.empty()) return reference;
    return buildModuleRefLabel(app_, module, reference);
}

MainWindow::StudyHistoryEntry MainWindow::historyEntryFromState(const StudyTabState& state) const {
    return StudyHistoryEntry{
        state.module,
        buildBibleReference(state.book, state.chapter, state.verse)
    };
}

MainWindow::StudyHistoryEntry MainWindow::currentBibleHistoryEntry() const {
    if (!biblePane_) return {};
    return StudyHistoryEntry{
        biblePane_->currentModule(),
        buildBibleReference(biblePane_->currentBook(),
                            biblePane_->currentChapter(),
                            biblePane_->currentVerse())
    };
}

void MainWindow::normalizeStudyHistory(StudyTabState& state) {
    state.history.erase(
        std::remove_if(state.history.begin(), state.history.end(),
                       [](const StudyHistoryEntry& entry) {
                           return trimCopy(entry.reference).empty();
                       }),
        state.history.end());

    if (state.history.empty()) {
        state.historyIndex = -1;
        return;
    }

    state.historyIndex = std::clamp(
        state.historyIndex, 0, static_cast<int>(state.history.size()) - 1);
}

void MainWindow::ensureStudyTabHistorySeeded(StudyTabState& state) {
    normalizeStudyHistory(state);
    if (!state.history.empty()) return;

    StudyHistoryEntry entry = historyEntryFromState(state);
    if (trimCopy(entry.reference).empty()) return;

    state.history.push_back(entry);
    state.historyIndex = 0;
}

void MainWindow::recordActiveStudyHistory() {
    if (activeStudyTab_ < 0 || activeStudyTab_ >= static_cast<int>(studyTabs_.size())) return;

    StudyTabState& state = studyTabs_[activeStudyTab_].state;
    normalizeStudyHistory(state);

    StudyHistoryEntry entry = currentBibleHistoryEntry();
    if (trimCopy(entry.reference).empty()) {
        syncBibleHistoryUi();
        return;
    }

    auto sameEntry = [](const StudyHistoryEntry& lhs, const StudyHistoryEntry& rhs) {
        return lhs.module == rhs.module && lhs.reference == rhs.reference;
    };

    if (state.history.empty()) {
        state.history.push_back(entry);
        state.historyIndex = 0;
        syncBibleHistoryUi();
        return;
    }

    if (state.historyIndex < 0 || state.historyIndex >= static_cast<int>(state.history.size())) {
        state.historyIndex = static_cast<int>(state.history.size()) - 1;
    }

    if (sameEntry(state.history[state.historyIndex], entry)) {
        syncBibleHistoryUi();
        return;
    }

    if (state.historyIndex + 1 < static_cast<int>(state.history.size())) {
        state.history.erase(state.history.begin() + state.historyIndex + 1,
                            state.history.end());
    }

    state.history.push_back(entry);
    state.historyIndex = static_cast<int>(state.history.size()) - 1;

    if (state.history.size() > static_cast<size_t>(kMaxStudyHistoryEntries)) {
        const size_t overflow = state.history.size() - kMaxStudyHistoryEntries;
        state.history.erase(state.history.begin(), state.history.begin() + overflow);
        state.historyIndex = std::max(0, state.historyIndex - static_cast<int>(overflow));
    }

    syncBibleHistoryUi();
}

void MainWindow::syncBibleHistoryUi() {
    if (!biblePane_) return;
    if (activeStudyTab_ < 0 || activeStudyTab_ >= static_cast<int>(studyTabs_.size())) {
        biblePane_->setNavigationHistory({}, -1, false, false);
        return;
    }

    StudyTabState& state = studyTabs_[activeStudyTab_].state;
    normalizeStudyHistory(state);
    if (state.history.empty()) {
        if (!suppressHistoryRecording_) {
            ensureStudyTabHistorySeeded(state);
        } else {
            biblePane_->setNavigationHistory({}, -1, false, false);
            return;
        }
    }

    std::vector<std::string> labels;
    labels.reserve(state.history.size());
    for (const auto& entry : state.history) {
        labels.push_back(historyEntryLabel(entry));
    }

    const bool canGoBack = state.historyIndex > 0;
    const bool canGoForward =
        state.historyIndex >= 0 &&
        state.historyIndex + 1 < static_cast<int>(state.history.size());
    biblePane_->setNavigationHistory(labels, state.historyIndex,
                                     canGoBack, canGoForward);
}

void MainWindow::onBibleStudyContextChanged() {
    if (activeStudyTab_ < 0 || activeStudyTab_ >= static_cast<int>(studyTabs_.size())) return;
    if (applyingTabState_) return;

    captureActiveTabState();
    if (!suppressHistoryRecording_) {
        recordActiveStudyHistory();
    } else {
        syncBibleHistoryUi();
    }
    updateActiveStudyTabLabel();
    updateStatusBar();
}

void MainWindow::navigateHistoryBack() {
    if (activeStudyTab_ < 0 || activeStudyTab_ >= static_cast<int>(studyTabs_.size())) return;
    StudyTabState& state = studyTabs_[activeStudyTab_].state;
    normalizeStudyHistory(state);
    navigateToHistoryIndex(state.historyIndex - 1);
}

void MainWindow::navigateHistoryForward() {
    if (activeStudyTab_ < 0 || activeStudyTab_ >= static_cast<int>(studyTabs_.size())) return;
    StudyTabState& state = studyTabs_[activeStudyTab_].state;
    normalizeStudyHistory(state);
    navigateToHistoryIndex(state.historyIndex + 1);
}

void MainWindow::navigateToHistoryMenuIndex(int menuIndex) {
    navigateToHistoryIndex(menuIndex);
}

void MainWindow::navigateToHistoryIndex(int index) {
    if (activeStudyTab_ < 0 || activeStudyTab_ >= static_cast<int>(studyTabs_.size())) return;

    StudyTabState& state = studyTabs_[activeStudyTab_].state;
    normalizeStudyHistory(state);
    if (index < 0 || index >= static_cast<int>(state.history.size())) {
        syncBibleHistoryUi();
        return;
    }
    if (index == state.historyIndex) {
        syncBibleHistoryUi();
        return;
    }

    StudyHistoryEntry entry = state.history[index];
    std::string reference = trimCopy(entry.reference);
    if (reference.empty()) {
        syncBibleHistoryUi();
        return;
    }

    state.historyIndex = index;
    bool wasSuppressing = suppressHistoryRecording_;
    suppressHistoryRecording_ = true;
    if (!trimCopy(entry.module).empty()) {
        navigateTo(entry.module, reference);
    } else {
        navigateTo(reference);
    }
    suppressHistoryRecording_ = wasSuppressing;

    captureActiveTabState();
    syncBibleHistoryUi();
    updateActiveStudyTabLabel();
}

void MainWindow::updateActiveStudyTabLabel() {
    if (activeStudyTab_ < 0 ||
        activeStudyTab_ >= static_cast<int>(studyTabs_.size()) ||
        !studyTabsWidget_) {
        return;
    }

    if (!applyingTabState_) captureActiveTabState();

    StudyContext& ctx = studyTabs_[activeStudyTab_];
    if (!ctx.tabGroup) return;

    std::string label = studyTabLabel(ctx.state);
    ctx.tabGroup->copy_label(label.c_str());
    studyTabsWidget_->redraw();
}

void MainWindow::captureStudyTabState(int index) {
    if (applyingTabState_) return;
    if (index < 0 || index >= static_cast<int>(studyTabs_.size())) return;

    StudyContext& ctx = studyTabs_[index];
    if (biblePane_) {
        ctx.state.module = biblePane_->currentModule();
        ctx.state.book = biblePane_->currentBook();
        ctx.state.chapter = biblePane_->currentChapter();
        ctx.state.verse = biblePane_->currentVerse();
        ctx.state.paragraphMode = biblePane_->isParagraphMode();
        ctx.state.parallelMode = biblePane_->isParallel();
        ctx.state.parallelModules = biblePane_->parallelModules();
        ctx.state.biblePaneWidth = biblePane_->w();
        ctx.state.bibleScrollY = biblePane_->scrollY();
    }

    if (rightPane_) {
        ctx.state.commentaryModule = rightPane_->currentCommentaryModule();
        ctx.state.commentaryReference = rightPane_->currentCommentaryReference();
        ctx.state.commentaryScrollY = rightPane_->commentaryScrollY();
        ctx.state.dictionaryModule = rightPane_->currentDictionaryModule();
        ctx.state.dictionaryKey = rightPane_->currentDictionaryKey();
    }

    if (!suppressHistoryRecording_) {
        ensureStudyTabHistorySeeded(ctx.state);
    }
}

void MainWindow::captureActiveTabState() {
    captureStudyTabState(activeStudyTab_);
}

void MainWindow::captureStudyTabDisplayBuffers(int index) {
    perf::ScopeTimer timer("MainWindow::captureActiveTabDisplayBuffers");
    if (applyingTabState_) return;
    if (index < 0 || index >= static_cast<int>(studyTabs_.size())) return;

    StudyContext& ctx = studyTabs_[index];
    perf::StepTimer step;
    if (biblePane_) {
        BiblePane::DisplayBuffer b = biblePane_->takeDisplayBuffer();
        ctx.bibleBuffer.doc = std::move(b.doc);
        ctx.bibleBuffer.html = std::move(b.html);
        ctx.bibleBuffer.baseUrl = std::move(b.baseUrl);
        ctx.bibleBuffer.scrollY = b.scrollY;
        ctx.bibleBuffer.contentHeight = b.contentHeight;
        ctx.bibleBuffer.renderWidth = b.renderWidth;
        ctx.bibleBuffer.storedSelectedVerse = b.storedSelectedVerse;
        ctx.bibleBuffer.scrollbarVisible = b.scrollbarVisible;
        ctx.bibleBuffer.valid = b.valid;
        ctx.hasBibleBuffer = b.valid;
        perf::logf("captureActiveTabDisplayBuffers tab=%d biblePane_->takeDisplayBuffer: %.3f ms (valid=%d)",
                   index, step.elapsedMs(), b.valid ? 1 : 0);
        step.reset();
    }
    if (rightPane_) {
        RightPane::DisplayBuffer r = rightPane_->takeDisplayBuffer();

        ctx.rightBuffer.commentary.doc = std::move(r.commentary.doc);
        ctx.rightBuffer.commentary.html = std::move(r.commentary.html);
        ctx.rightBuffer.commentary.baseUrl = std::move(r.commentary.baseUrl);
        ctx.rightBuffer.commentary.scrollY = r.commentary.scrollY;
        ctx.rightBuffer.commentary.contentHeight = r.commentary.contentHeight;
        ctx.rightBuffer.commentary.renderWidth = r.commentary.renderWidth;
        ctx.rightBuffer.commentary.scrollbarVisible = r.commentary.scrollbarVisible;
        ctx.rightBuffer.commentary.valid = r.commentary.valid;

        ctx.rightBuffer.dictionary.doc = std::move(r.dictionary.doc);
        ctx.rightBuffer.dictionary.html = std::move(r.dictionary.html);
        ctx.rightBuffer.dictionary.baseUrl = std::move(r.dictionary.baseUrl);
        ctx.rightBuffer.dictionary.scrollY = r.dictionary.scrollY;
        ctx.rightBuffer.dictionary.contentHeight = r.dictionary.contentHeight;
        ctx.rightBuffer.dictionary.renderWidth = r.dictionary.renderWidth;
        ctx.rightBuffer.dictionary.scrollbarVisible = r.dictionary.scrollbarVisible;
        ctx.rightBuffer.dictionary.valid = r.dictionary.valid;

        ctx.hasRightBuffer = ctx.rightBuffer.commentary.valid ||
                             ctx.rightBuffer.dictionary.valid;
        perf::logf("captureActiveTabDisplayBuffers tab=%d rightPane_->takeDisplayBuffer: %.3f ms (c=%d d=%d)",
                   index,
                   step.elapsedMs(),
                   ctx.rightBuffer.commentary.valid ? 1 : 0,
                   ctx.rightBuffer.dictionary.valid ? 1 : 0);
    }
}

void MainWindow::captureActiveTabDisplayBuffers() {
    captureStudyTabDisplayBuffers(activeStudyTab_);
}

void MainWindow::evictOldTabSnapshots() {
    const int maxCachedDocs = configuredMaxCachedTabDocs();
    auto clearBuffer = [](auto& buf) {
        buf.doc.reset();
        buf.html.clear();
        buf.baseUrl.clear();
        buf.scrollY = 0;
        buf.contentHeight = 0;
        buf.renderWidth = 0;
        buf.scrollbarVisible = false;
        buf.valid = false;
    };

    if (maxCachedDocs <= 0) {
        for (int i = 0; i < static_cast<int>(studyTabs_.size()); ++i) {
            if (i == activeStudyTab_) continue;
            auto& t = studyTabs_[i];
            if (t.hasBibleBuffer) {
                clearBuffer(t.bibleBuffer);
                t.hasBibleBuffer = false;
            }
            if (t.hasRightBuffer) {
                clearBuffer(t.rightBuffer.commentary);
                clearBuffer(t.rightBuffer.dictionary);
                t.hasRightBuffer = false;
            }
        }
        return;
    }

    // Count tabs that hold a cached litehtml doc (bible or right pane).
    int cached = 0;
    for (const auto& t : studyTabs_) {
        if (t.hasBibleBuffer || t.hasRightBuffer) ++cached;
    }
    if (cached <= maxCachedDocs) return;

    // Build list of candidate tabs sorted by lastUsed (oldest first).
    std::vector<int> candidates;
    for (int i = 0; i < static_cast<int>(studyTabs_.size()); ++i) {
        if (i == activeStudyTab_) continue;
        auto& t = studyTabs_[i];
        if (t.hasBibleBuffer || t.hasRightBuffer)
            candidates.push_back(i);
    }
    std::sort(candidates.begin(), candidates.end(),
              [this](int a, int b) {
                  return studyTabs_[a].lastUsed < studyTabs_[b].lastUsed;
              });

    // Evict docs from oldest tabs until we're within budget.
    // The uncached restore path rebuilds from study state, so once a tab falls
    // out of the doc cache budget we can drop the stored HTML as well.
    for (int idx : candidates) {
        if (cached <= maxCachedDocs) break;
        auto& t = studyTabs_[idx];
        if (t.hasBibleBuffer) {
            clearBuffer(t.bibleBuffer);
            t.hasBibleBuffer = false;
        }
        if (t.hasRightBuffer) {
            clearBuffer(t.rightBuffer.commentary);
            clearBuffer(t.rightBuffer.dictionary);
            t.hasRightBuffer = false;
        }
        --cached;
        perf::logf("evictOldTabSnapshots: evicted tab %d (lastUsed=%llu)",
                   idx, static_cast<unsigned long long>(t.lastUsed));
    }
}

void MainWindow::applyTabState(int index) {
    perf::ScopeTimer timer("MainWindow::applyTabState");
    if (index < 0 || index >= static_cast<int>(studyTabs_.size())) return;
    if (!biblePane_ || !rightPane_) return;

    StudyContext& ctx = studyTabs_[index];
    applyingTabState_ = true;
    perf::StepTimer step;
    bool restoredBible = false;

    biblePane_->setStudyState(
        ctx.state.module,
        ctx.state.book,
        ctx.state.chapter,
        ctx.state.verse,
        ctx.state.paragraphMode,
        ctx.state.parallelMode,
        ctx.state.parallelModules);
    perf::logf("applyTabState tab=%d biblePane_->setStudyState: %.3f ms",
               index, step.elapsedMs());
    step.reset();
    if (ctx.hasBibleBuffer) {
        restoredBible = true;
        BiblePane::DisplayBuffer b;
        b.doc = std::move(ctx.bibleBuffer.doc);
        b.html = std::move(ctx.bibleBuffer.html);
        b.baseUrl = std::move(ctx.bibleBuffer.baseUrl);
        b.scrollY = ctx.bibleBuffer.scrollY;
        b.contentHeight = ctx.bibleBuffer.contentHeight;
        b.renderWidth = ctx.bibleBuffer.renderWidth;
        b.storedSelectedVerse = ctx.bibleBuffer.storedSelectedVerse;
        b.scrollbarVisible = ctx.bibleBuffer.scrollbarVisible;
        b.valid = ctx.bibleBuffer.valid;
        biblePane_->restoreDisplayBuffer(std::move(b));
        ctx.bibleBuffer = HtmlDocBuffer{};
        ctx.hasBibleBuffer = false;
        perf::logf("applyTabState tab=%d biblePane_->restoreDisplayBuffer: %.3f ms",
                   index, step.elapsedMs());
        step.reset();
    } else {
        biblePane_->refresh();
        perf::logf("applyTabState tab=%d biblePane_->refresh: %.3f ms",
                   index, step.elapsedMs());
        step.reset();
    }
    if (leftPane_ && biblePane_) {
        leftPane_->setSearchModule(biblePane_->currentModule());
        perf::logf("applyTabState tab=%d leftPane_->setSearchModule: %.3f ms",
                   index, step.elapsedMs());
        step.reset();
    }

    rightPane_->setStudyState(
        ctx.state.commentaryModule,
        ctx.state.commentaryReference,
        ctx.state.dictionaryModule,
        ctx.state.dictionaryKey);
    perf::logf("applyTabState tab=%d rightPane_->setStudyState: %.3f ms",
               index, step.elapsedMs());
    step.reset();
    bool restoredRight = ctx.hasRightBuffer;
    if (restoredRight) {
        RightPane::DisplayBuffer r;
        r.commentary.doc = std::move(ctx.rightBuffer.commentary.doc);
        r.commentary.html = std::move(ctx.rightBuffer.commentary.html);
        r.commentary.baseUrl = std::move(ctx.rightBuffer.commentary.baseUrl);
        r.commentary.scrollY = ctx.rightBuffer.commentary.scrollY;
        r.commentary.contentHeight = ctx.rightBuffer.commentary.contentHeight;
        r.commentary.renderWidth = ctx.rightBuffer.commentary.renderWidth;
        r.commentary.scrollbarVisible = ctx.rightBuffer.commentary.scrollbarVisible;
        r.commentary.valid = ctx.rightBuffer.commentary.valid;

        r.dictionary.doc = std::move(ctx.rightBuffer.dictionary.doc);
        r.dictionary.html = std::move(ctx.rightBuffer.dictionary.html);
        r.dictionary.baseUrl = std::move(ctx.rightBuffer.dictionary.baseUrl);
        r.dictionary.scrollY = ctx.rightBuffer.dictionary.scrollY;
        r.dictionary.contentHeight = ctx.rightBuffer.dictionary.contentHeight;
        r.dictionary.renderWidth = ctx.rightBuffer.dictionary.renderWidth;
        r.dictionary.scrollbarVisible = ctx.rightBuffer.dictionary.scrollbarVisible;
        r.dictionary.valid = ctx.rightBuffer.dictionary.valid;

        rightPane_->restoreDisplayBuffer(std::move(r));
        ctx.rightBuffer = RightDocBuffers{};
        ctx.hasRightBuffer = false;
        perf::logf("applyTabState tab=%d rightPane_->restoreDisplayBuffer: %.3f ms",
                   index, step.elapsedMs());
        step.reset();
    } else {
        rightPane_->refresh();
        perf::logf("applyTabState tab=%d rightPane_->refresh: %.3f ms",
                   index, step.elapsedMs());
        step.reset();
    }
    if (!restoredBible && ctx.state.bibleScrollY >= 0) {
        biblePane_->setScrollY(ctx.state.bibleScrollY);
        perf::logf("applyTabState tab=%d biblePane_->setScrollY: %.3f ms",
                   index, step.elapsedMs());
        step.reset();
    }
    if (!restoredRight && ctx.state.commentaryScrollY >= 0) {
        rightPane_->setCommentaryScrollY(ctx.state.commentaryScrollY);
        perf::logf("applyTabState tab=%d rightPane_->setCommentaryScrollY: %.3f ms",
                   index, step.elapsedMs());
        step.reset();
    }
    biblePane_->redrawChrome();
    rightPane_->redrawChrome();
    perf::logf("applyTabState tab=%d redrawChrome panes: %.3f ms",
               index, step.elapsedMs());
    step.reset();
    if (studyTabsWidget_) {
        studyTabsWidget_->damage(FL_DAMAGE_ALL);
        studyTabsWidget_->redraw();
        perf::logf("applyTabState tab=%d studyTabsWidget redraw: %.3f ms",
                   index, step.elapsedMs());
        step.reset();
    }

    appliedStudyTabGroup_ = ctx.tabGroup;
    applyingTabState_ = false;
}

void MainWindow::navigateTo(const std::string& reference) {
    if (biblePane_) {
        biblePane_->navigateToReference(reference);
        std::string module = trimCopy(biblePane_->currentModule());
        std::string ref = trimCopy(reference);
        if (!module.empty() && !ref.empty()) {
            showTransientStatus("Opened " + buildModuleRefLabel(app_, module, ref), 2.2);
        }
    }
}

void MainWindow::navigateTo(const std::string& module, const std::string& reference) {
    if (biblePane_) {
        const bool alreadySuppressing = suppressHistoryRecording_;
        if (!alreadySuppressing) {
            suppressHistoryRecording_ = true;
        }

        std::string mod = trimCopy(module);
        if (!mod.empty() && mod != trimCopy(biblePane_->currentModule())) {
            biblePane_->setModule(mod);
        }
        biblePane_->navigateToReference(reference);

        if (!alreadySuppressing) {
            suppressHistoryRecording_ = false;
            captureActiveTabState();
            recordActiveStudyHistory();
            updateActiveStudyTabLabel();
            updateStatusBar();
        }

        std::string ref = trimCopy(reference);
        if (!mod.empty() && !ref.empty()) {
            showTransientStatus("Opened " + buildModuleRefLabel(app_, mod, ref), 2.2);
        }
    }
}

void MainWindow::openInNewStudyTab(const std::string& module,
                                   const std::string& reference) {
    std::string refText = trimCopy(reference);
    std::string mod = trimCopy(module);
    if (refText.empty()) return;

    SwordManager::VerseRef parsed;
    bool parsedOk = false;
    try {
        parsed = SwordManager::parseVerseRef(refText);
        parsedOk = !parsed.book.empty();
    } catch (...) {
        parsed = SwordManager::VerseRef{};
    }

    std::string book = parsedOk ? parsed.book : "Genesis";
    int chapter = (parsedOk && parsed.chapter > 0) ? parsed.chapter : 1;
    int verse = (parsedOk && parsed.verse > 0) ? parsed.verse : 1;

    addStudyTab(mod, book, chapter, verse);
    if (!mod.empty()) {
        navigateTo(mod, refText);
    } else {
        navigateTo(refText);
    }
}

void MainWindow::showCommentary(const std::string& reference) {
    if (rightPane_) {
        rightPane_->showCommentary(reference);
        if (!applyingTabState_) captureActiveTabState();
    }
}

void MainWindow::showCommentary(const std::string& module,
                                const std::string& reference) {
    if (rightPane_) {
        rightPane_->showCommentary(module, reference);
        if (!applyingTabState_) captureActiveTabState();
    }
}

void MainWindow::showCommentary(const std::string& module,
                                const std::string& reference,
                                const std::string& searchHighlight) {
    if (rightPane_) {
        rightPane_->showCommentary(module, reference, searchHighlight);
        if (!applyingTabState_) captureActiveTabState();
    }
}

void MainWindow::showDictionary(const std::string& key) {
    if (rightPane_) {
        rightPane_->showDictionaryEntry(key);
        if (!applyingTabState_) captureActiveTabState();
    }
}

void MainWindow::showDictionaryEntry(const std::string& module,
                                     const std::string& key) {
    if (rightPane_) {
        rightPane_->showDictionaryEntry(module, key);
        if (!applyingTabState_) captureActiveTabState();
    }
}

void MainWindow::showDictionary(const std::string& key,
                                const std::string& contextModule) {
    if (rightPane_) {
        rightPane_->showDictionaryLookup(key, contextModule);
        if (!applyingTabState_) captureActiveTabState();
    }
}

void MainWindow::showGeneralBookEntry(const std::string& module,
                                      const std::string& key) {
    if (rightPane_) {
        rightPane_->showGeneralBookEntry(module, key);
        if (!applyingTabState_) captureActiveTabState();
    }
}

void MainWindow::showGeneralBookEntry(const std::string& module,
                                      const std::string& key,
                                      const std::string& searchHighlight) {
    if (rightPane_) {
        rightPane_->showGeneralBookEntry(module, key, searchHighlight);
        if (!applyingTabState_) captureActiveTabState();
    }
}

void MainWindow::showWordInfo(const std::string& word, const std::string& href,
                               const std::string& strong, const std::string& morph,
                               int /*screenX*/, int /*screenY*/,
                               const std::string& sourceModule) {
    pendingWordInfo_.word = word;
    pendingWordInfo_.href = href;
    pendingWordInfo_.strong = strong;
    pendingWordInfo_.morph = morph;
    pendingWordInfo_.sourceModule = sourceModule;
    pendingWordInfo_.tabIndex = activeStudyTab_;

    if (hoverDelayScheduled_) {
        Fl::remove_timeout(onHoverDelayTimeout, this);
    }
    double hoverDelaySec = 1.0;
    if (app_) {
        hoverDelaySec = std::clamp(
            app_->appearanceSettings().hoverDelayMs / 1000.0,
            0.1, 5.0);
    }
    Fl::add_timeout(hoverDelaySec, onHoverDelayTimeout, this);
    hoverDelayScheduled_ = true;
}

void MainWindow::showWordInfoNow(const std::string& word,
                                 const std::string& href,
                                 const std::string& strong,
                                 const std::string& morph,
                                 const std::string& sourceModule) {
    if (hoverDelayScheduled_) {
        Fl::remove_timeout(onHoverDelayTimeout, this);
        hoverDelayScheduled_ = false;
    }

    pendingWordInfo_.word = word;
    pendingWordInfo_.href = href;
    pendingWordInfo_.strong = strong;
    pendingWordInfo_.morph = morph;
    pendingWordInfo_.sourceModule = sourceModule;
    pendingWordInfo_.tabIndex = activeStudyTab_;
    applyPendingWordInfo();
}

void MainWindow::hideWordInfo() {
    if (hoverDelayScheduled_) {
        Fl::remove_timeout(onHoverDelayTimeout, this);
        hoverDelayScheduled_ = false;
    }
    pendingWordInfo_ = PendingWordInfo{};
}

void MainWindow::onHoverDelayTimeout(void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;
    self->hoverDelayScheduled_ = false;
    self->applyPendingWordInfo();
}

void MainWindow::applyPendingWordInfo() {
    if (pendingWordInfo_.tabIndex != activeStudyTab_) return;

    std::string strongNum = pendingWordInfo_.strong;
    if (strongNum.empty() && pendingWordInfo_.href.find("strongs:") == 0) {
        strongNum = pendingWordInfo_.href.substr(8);
    } else if (strongNum.empty() && pendingWordInfo_.href.find("strong:") == 0) {
        strongNum = pendingWordInfo_.href.substr(7);
    }
    while (!strongNum.empty() &&
           (strongNum.front() == '/' ||
            std::isspace(static_cast<unsigned char>(strongNum.front())))) {
        strongNum.erase(strongNum.begin());
    }

    std::string morphCode = pendingWordInfo_.morph;
    if (morphCode.empty() && pendingWordInfo_.href.find("morph:") == 0) {
        morphCode = pendingWordInfo_.href.substr(6);
    }

    std::vector<std::string> strongTokens = extractStrongsTokens(strongNum);
    std::string morphKey = trimCopy(morphCode);
    if (strongTokens.empty() && morphKey.empty()) {
        if (!app_ || !app_->offlineTranslationSettings().enabled ||
            !pendingWordInfo_.href.empty()) {
            return;
        }

        std::string sourceLanguage = app_->sourceLanguageForModule(
            pendingWordInfo_.sourceModule);
        std::optional<OfflineTranslationResult> translation =
            app_->wikDictManager().lookup(
                sourceLanguage, "en", pendingWordInfo_.word);
        if (!translation) return;

        std::ostringstream translationHtml;
        translationHtml << "<div class=\"mag-lite mag-translation\">";
        translationHtml << "<span class=\"mag-line mag-wordline\">"
                        << htmlEscape(translation->sourceWord) << "</span>";
        translationHtml << "<span class=\"mag-line mag-translation-label\">"
                        << htmlEscape(languageDisplayName(
                               translation->targetLanguage))
                        << "</span>";
        for (const auto& gloss : translation->glosses) {
            translationHtml
                << "<span class=\"mag-line mag-translation-gloss\">"
                << htmlEscape(gloss) << "</span>";
        }
        if (!translation->lemmas.empty()) {
            translationHtml
                << "<span class=\"mag-line mag-translation-metadata\"><b>"
                << (translation->inferredAnalysis ? "Lemma (inferred): "
                                                  : "Lemma: ")
                << "</b>"
                << htmlEscape(joinDisplayValues(translation->lemmas))
                << "</span>";
        }
        if (!translation->partsOfSpeech.empty()) {
            translationHtml
                << "<span class=\"mag-line mag-translation-metadata\"><b>"
                << "Part of speech: </b>"
                << htmlEscape(joinDisplayValues(translation->partsOfSpeech))
                << "</span>";
        }
        for (const auto& detail : translation->grammaticalDetails) {
            translationHtml
                << "<span class=\"mag-line mag-translation-metadata\">"
                << htmlEscape(detail)
                << (translation->inferredAnalysis &&
                            detail.rfind("Form:", 0) == 0
                        ? " (inferred)"
                        : "")
                << "</span>";
        }
        translationHtml
            << "<span class=\"mag-line mag-translation-attribution\">"
            << htmlEscape(translation->attribution) << "</span></div>";

        if (leftPane_) {
            leftPane_->setPreviewText(translationHtml.str());
        }
        return;
    }

    std::ostringstream html;
    html << "<div class=\"mag-lite\">";
    bool hasDefinition = false;
    std::string displayWord = trimCopy(pendingWordInfo_.word);
    if (!displayWord.empty()) {
        html << "<span class=\"mag-line mag-wordline\">"
             << htmlEscape(displayWord) << "</span>";
        html << "<span class=\"mag-line mag-gap\"></span>";
    }

    for (const auto& tok : strongTokens) {
        html << "<span class=\"mag-line mag-label\">Strong's "
             << htmlEscape(tok) << "</span>";
        std::string def = app_->swordManager().getStrongsDefinition(
            tok, preferredPreviewLexicons(app_, tok));
        if (!def.empty()) {
            hasDefinition = true;
            appendEscapedTextLines(html, def, "mag-defline");
            html << "<span class=\"mag-line mag-gap\"></span>";
        }
    }

    if (!morphKey.empty()) {
        html << "<span class=\"mag-line mag-morph-label\">Morph: "
             << htmlEscape(morphKey) << "</span>";
        std::string def = app_->swordManager().getMorphDefinition(morphKey);
        if (!def.empty()) {
            hasDefinition = true;
            appendEscapedTextLines(html, def, "mag-defline");
        }
    }

    if (!hasDefinition) return;
    html << "</div>";

    if (leftPane_) {
        leftPane_->setPreviewText(html.str());
    }
}

void MainWindow::showSearchResults(const std::string& query,
                                   const std::string& moduleOverride) {
    if (leftPane_) {
        std::string strongTok = extractStrongsToken(query);
        if (!strongTok.empty()) {
            showTransientStatus("Strong's lookup: " + strongTok, 2.8);
        } else {
            std::string q = trimCopy(query);
            if (!q.empty()) {
                if (q.size() > 40) q = q.substr(0, 40) + "...";
                showTransientStatus("Search: " + q, 2.0);
            }
        }
        leftPane_->doSearch(query, moduleOverride);
        leftPane_->showSearchTab();
    }
}

void MainWindow::showTransientStatus(const std::string& text, double seconds) {
    std::string msg = trimCopy(text);
    if (msg.empty()) return;

    transientStatusText_ = msg;
    transientStatusUntil_ = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(
                                static_cast<int>(std::clamp(seconds, 0.4, 10.0) * 1000.0));
    updateStatusBar();
}

void MainWindow::refresh() {
    if (leftPane_) leftPane_->refresh();
    if (biblePane_) biblePane_->refresh();
    if (rightPane_) rightPane_->refresh();
    captureActiveTabState();
}

void MainWindow::applyAppearanceSettings(Fl_Font appFont,
                                         int appFontSize,
                                         const std::string& textCssOverride) {
    const int clampedSize = std::clamp(appFontSize, 8, 36);
    const bool cssChanged =
        appearanceApplied_ && (textCssOverride != lastAppliedTextCss_);
    const bool uiFontChanged =
        appearanceApplied_ &&
        (appFont != lastAppliedAppFont_ ||
         clampedSize != lastAppliedAppFontSize_);

    // Re-apply widget fonts only when needed.
    if (!appearanceApplied_ || uiFontChanged) {
        Fl_Font boldFont = app_ ? app_->boldFltkFont(appFont) : appFont;
        ui_font::applyUiFontRecursively(this, appFont, boldFont, clampedSize);
        if (searchHelpWindow_) {
            ui_font::applyUiFontRecursively(
                searchHelpWindow_, appFont, boldFont, clampedSize);
        }
    }

    applyToggleButtonPressedColorRecursively(this);

    // Re-apply HTML CSS only when needed.
    if (!appearanceApplied_ || cssChanged) {
        if (leftPane_) leftPane_->setHtmlStyleOverride(textCssOverride);
        if (biblePane_) biblePane_->setHtmlStyleOverride(textCssOverride);
        if (rightPane_) rightPane_->setHtmlStyleOverride(textCssOverride);
        if (searchHelpHtml_) searchHelpHtml_->setStyleOverrideCss(textCssOverride);
    }
    if (leftPane_ && app_) {
        leftPane_->setBrowserLineSpacing(app_->appearanceSettings().browserLineSpacing);
    }
    if (biblePane_ && app_) {
        biblePane_->setBrowserLineSpacing(app_->appearanceSettings().browserLineSpacing);
    }
    if (rightPane_ && app_) {
        rightPane_->setEditorIndentWidth(app_->appearanceSettings().editorIndentWidth);
        rightPane_->setEditorLineHeight(app_->appearanceSettings().editorLineHeight);
        rightPane_->setEditorTextFont(
            app_->textEditorFont(),
            app_->boldTextEditorFont(),
            app_->appearanceSettings().textFontSize);
    }
    if (biblePane_) biblePane_->syncOptionButtons();
    if (leftPane_) leftPane_->redrawChrome();
    if (biblePane_) biblePane_->redrawChrome();
    if (rightPane_) rightPane_->redrawChrome();

    // Only drop inactive tab buffers when text rendering style actually changed.
    if (cssChanged) {
        for (size_t i = 0; i < studyTabs_.size(); ++i) {
            if (static_cast<int>(i) == activeStudyTab_) continue;
            studyTabs_[i].bibleBuffer = HtmlDocBuffer{};
            studyTabs_[i].rightBuffer = RightDocBuffers{};
            studyTabs_[i].hasBibleBuffer = false;
            studyTabs_[i].hasRightBuffer = false;
        }
    }

    lastAppliedAppFont_ = appFont;
    lastAppliedAppFontSize_ = clampedSize;
    lastAppliedTextCss_ = textCssOverride;
    appearanceApplied_ = true;

    if (searchHelpWindow_) {
        searchHelpWindow_->redraw();
    }

    redraw();
}

MainWindow::SessionState MainWindow::captureSessionState() {
    captureActiveTabState();

    SessionState state;
    state.windowX = x();
    state.windowY = y();
    state.windowW = w();
    state.windowH = h();
    if (leftPane_) {
        state.leftPaneWidth = leftPane_->w();
        state.leftPanePreviewHeight = leftPane_->previewHeight();
    }
    state.activeStudyTab = activeStudyTab_;
    if (rightPane_) {
        state.dictionaryPaneHeight = rightPane_->dictionaryPaneHeight();
        switch (rightPane_->currentTopTab()) {
        case RightPane::TopTab::GeneralBooks:
            state.rightPaneTab = RightPaneTab::GeneralBooks;
            break;
        case RightPane::TopTab::DevotionsPlans:
            state.rightPaneTab = RightPaneTab::DevotionsPlans;
            break;
        case RightPane::TopTab::Documents:
            state.rightPaneTab = RightPaneTab::Documents;
            break;
        case RightPane::TopTab::Commentary:
        default:
            state.rightPaneTab = RightPaneTab::Commentary;
            break;
        }
        state.generalBookModule = rightPane_->currentGeneralBookModule();
        state.generalBookKey = rightPane_->currentGeneralBookKey();
        state.documentPath = rightPane_->currentDocumentPath();
        state.dailyWorkspace = rightPane_->currentDailyWorkspaceState();
    }
    if (documentRestoreScheduled_) {
        if (pendingDocumentsTabActive_) {
            state.rightPaneTab = RightPaneTab::Documents;
        }
        if (!pendingDocumentRestorePath_.empty()) {
            state.documentPath = pendingDocumentRestorePath_;
        }
    }

    int sharedBiblePaneWidth = biblePane_ ? biblePane_->w() : 0;
    for (const auto& ctx : studyTabs_) {
        StudyTabState tab = ctx.state;
        ensureStudyTabHistorySeeded(tab);
        if (tab.biblePaneWidth <= 0) {
            tab.biblePaneWidth = sharedBiblePaneWidth;
        }
        state.studyTabs.push_back(tab);
    }

    return state;
}

void MainWindow::restoreSessionState(const SessionState& state) {
    // Restore window geometry first.
    int rx = state.windowX >= 0 ? state.windowX : x();
    int ry = state.windowY >= 0 ? state.windowY : y();
    int rw = state.windowW > 100 ? state.windowW : w();
    int rh = state.windowH > 100 ? state.windowH : h();
    resize(rx, ry, rw, rh);

    // Restore top-level splitter width.
    if (mainTile_ && leftPane_ && studyArea_) {
        int menuH = menuBar_ ? menuBar_->h() : 25;
        int statusH = statusBar_ ? statusBar_->h() : 22;
        int totalW = w();
        int totalH = std::max(20, h() - menuH - statusH);
        int minLeft = 180;
        int maxLeft = std::max(minLeft, totalW - 220);
        int leftW = std::clamp(state.leftPaneWidth, minLeft, maxLeft);

        static_cast<Fl_Widget*>(leftPane_)->resize(0, menuH, leftW, totalH);
        studyArea_->resize(leftW, menuH, totalW - leftW, totalH);
        if (state.leftPanePreviewHeight > 0) {
            leftPane_->setPreviewHeight(state.leftPanePreviewHeight);
        }

        if (studyTabsWidget_) {
            const int tabsHeaderH = studyTabsWidget_->h();
            layoutStudyTabHeader();
            if (contentTile_) {
                int contentY = studyArea_->y() + tabsHeaderH;
                int contentH = std::max(20, studyArea_->h() - tabsHeaderH);
                contentTile_->resize(studyArea_->x(), contentY, studyArea_->w(), contentH);
            }
        }
    }

    clearStudyTabs();

    if (state.studyTabs.empty() || !studyTabsWidget_) {
        addStudyTab("", "Genesis", 1, 1);
        if (rightPane_) {
            DailyWorkspaceState dailyState = state.dailyWorkspace;
            dailyState.calendarVisible = false;
            rightPane_->setDailyWorkspaceState(dailyState);
            if (state.dictionaryPaneHeight > 0) {
                rightPane_->setDictionaryPaneHeight(state.dictionaryPaneHeight);
            }
            if (!state.generalBookModule.empty()) {
                rightPane_->showGeneralBookEntry(state.generalBookModule,
                                                 state.generalBookKey);
            } else if (state.rightPaneTab == RightPaneTab::GeneralBooks &&
                       !rightPane_->currentGeneralBookModule().empty()) {
                rightPane_->showGeneralBookEntry(rightPane_->currentGeneralBookModule(),
                                                 rightPane_->currentGeneralBookKey());
            }
            switch (state.rightPaneTab) {
            case RightPaneTab::Documents:
                rightPane_->setDocumentsTabActive(true);
                break;
            case RightPaneTab::DevotionsPlans:
                rightPane_->setDevotionsPlansTabActive(true);
                break;
            case RightPaneTab::GeneralBooks:
                rightPane_->setDictionaryTabActive(true);
                break;
            case RightPaneTab::Commentary:
            default:
                rightPane_->setDictionaryTabActive(false);
                break;
            }
            scheduleDeferredDocumentRestore(state.documentPath,
                                            state.rightPaneTab == RightPaneTab::Documents);
        }
        redraw();
        return;
    }

    for (const auto& tabState : state.studyTabs) {
        StudyContext ctx;
        ctx.state = tabState;
        normalizeStudyHistory(ctx.state);
        if (ctx.state.history.empty()) {
            ensureStudyTabHistorySeeded(ctx.state);
        }

        studyTabsWidget_->begin();
        ctx.tabGroup = new Fl_Group(studyTabsWidget_->x(),
                                    studyTabsWidget_->y() + studyTabsWidget_->h(),
                                    1, 1);
        ctx.tabGroup->copy_label(studyTabLabel(ctx.state).c_str());
        ctx.tabGroup->end();
        ui_font::applyCurrentAppUiFont(ctx.tabGroup);
        studyTabsWidget_->end();

        studyTabs_.push_back(std::move(ctx));
    }

    if (contentTile_ && biblePane_ && rightPane_) {
        int desired = 0;
        if (!state.studyTabs.empty()) {
            int idx = std::clamp(state.activeStudyTab, 0,
                                 static_cast<int>(state.studyTabs.size()) - 1);
            desired = state.studyTabs[idx].biblePaneWidth;
        }
        if (desired <= 0) {
            for (const auto& t : state.studyTabs) {
                if (t.biblePaneWidth > 0) {
                    desired = t.biblePaneWidth;
                    break;
                }
            }
        }
        if (desired > 0) {
            int tileX = contentTile_->x();
            int tileY = contentTile_->y();
            int tileW = contentTile_->w();
            int tileH = contentTile_->h();
            int bibleW = std::clamp(desired, 140, std::max(140, tileW - 140));
            static_cast<Fl_Widget*>(biblePane_)->resize(tileX, tileY, bibleW, tileH);
            static_cast<Fl_Widget*>(rightPane_)
                ->resize(tileX + bibleW, tileY, tileW - bibleW, tileH);
            contentTile_->redraw();
        }
    }

    if (rightPane_) {
        DailyWorkspaceState dailyState = state.dailyWorkspace;
        dailyState.calendarVisible = false;
        rightPane_->setDailyWorkspaceState(dailyState);
        if (state.dictionaryPaneHeight > 0) {
            rightPane_->setDictionaryPaneHeight(state.dictionaryPaneHeight);
        }
        if (!state.generalBookModule.empty()) {
            rightPane_->showGeneralBookEntry(state.generalBookModule,
                                             state.generalBookKey);
        } else if (state.rightPaneTab == RightPaneTab::GeneralBooks &&
                   !rightPane_->currentGeneralBookModule().empty()) {
            rightPane_->showGeneralBookEntry(rightPane_->currentGeneralBookModule(),
                                             rightPane_->currentGeneralBookKey());
        }
        switch (state.rightPaneTab) {
        case RightPaneTab::Documents:
            rightPane_->setDocumentsTabActive(true);
            break;
        case RightPaneTab::DevotionsPlans:
            rightPane_->setDevotionsPlansTabActive(true);
            break;
        case RightPaneTab::GeneralBooks:
            rightPane_->setDictionaryTabActive(true);
            break;
        case RightPaneTab::Commentary:
        default:
            rightPane_->setDictionaryTabActive(false);
            break;
        }
    }

    if (!studyTabs_.empty()) {
        int idx = std::clamp(state.activeStudyTab, 0,
                             static_cast<int>(studyTabs_.size()) - 1);
        studyTabsWidget_->value(studyTabs_[idx].tabGroup);
        activeStudyTab_ = -1;
        activateStudyTab(idx);
    }

    if (rightPane_) {
        scheduleDeferredDocumentRestore(state.documentPath,
                                        state.rightPaneTab == RightPaneTab::Documents);
    }

    redraw();
    updateStatusBar();
}

void MainWindow::resize(int X, int Y, int W, int H) {
    Fl_Double_Window::resize(X, Y, W, H);

    const int menuH = menuBar_ ? menuBar_->h() : 25;
    const int statusH = statusBar_ ? statusBar_->h() : 22;
    const int bodyH = std::max(20, H - menuH - statusH);

    if (menuBar_) {
        menuBar_->resize(0, 0, W, menuH);
    }
    if (mainTile_) {
        mainTile_->resize(0, menuH, W, bodyH);
    }
    if (statusBar_) {
        statusBar_->resize(0, menuH + bodyH, W, statusH);
    }
}

void MainWindow::updateStatusBar() {
    if (!statusBar_) return;

    std::string text;
    auto now = std::chrono::steady_clock::now();

    if (!transientStatusText_.empty() && now < transientStatusUntil_) {
        text = transientStatusText_;
    } else if (!transientStatusText_.empty()) {
        transientStatusText_.clear();
    }

    if (text.empty() && app_ && app_->searchIndexer()) {
        std::string module;
        int pct = 0;
        if (app_->searchIndexer()->activeIndexingTask(module, pct)) {
            text = "Indexing " + module + ": " + std::to_string(pct) + "%";
        } else if (app_->searchIndexer()->isIndexing()) {
            text = "Indexing modules...";
        }
    }

    if (text.empty()) {
        std::string module;
        std::string reference;
        if (biblePane_) {
            module = trimCopy(biblePane_->currentModule());
            std::string book = trimCopy(biblePane_->currentBook());
            int chapter = std::max(1, biblePane_->currentChapter());
            int verse = std::max(1, biblePane_->currentVerse());
            if (!book.empty()) {
                reference = book + " " + std::to_string(chapter) + ":" +
                            std::to_string(verse);
            }
        }

        if (!module.empty() && !reference.empty() && app_) {
            text = "Ready | " + buildModuleRefLabel(app_, module, reference);
        } else {
            text = "Ready";
        }
    }

    if (text != lastStatusBarText_) {
        lastStatusBarText_ = text;
        statusBar_->copy_label(lastStatusBarText_.c_str());
        statusBar_->redraw();
    }
}

void MainWindow::onStatusPoll(void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;
    if (!self->shown()) return;

    self->updateStatusBar();
    Fl::repeat_timeout(0.25, onStatusPoll, self);
    self->statusPollScheduled_ = true;
}

void MainWindow::checkDailyDateRollover() {
    const std::string todayIso = reading::formatIsoDate(reading::today());
    if (lastDailyDateIso_.empty()) {
        lastDailyDateIso_ = todayIso;
        return;
    }
    if (todayIso == lastDailyDateIso_) return;

    lastDailyDateIso_ = todayIso;
    if (rightPane_) {
        rightPane_->refreshDailyDatesForToday();
    }
}

void MainWindow::onDailyDateCheck(void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;

    self->dailyDateCheckScheduled_ = false;
    self->checkDailyDateRollover();
    Fl::repeat_timeout(kDailyDateCheckSeconds, onDailyDateCheck, self);
    self->dailyDateCheckScheduled_ = true;
}

void MainWindow::resetUserDataMonitorSnapshot() {
    userDataSnapshot_ = buildUserDataSnapshot(app_);
    pendingUserDataSnapshot_.clear();
    pendingUserDataChangedPaths_.clear();
    pendingUserDataChange_ = false;
}

void MainWindow::pollUserDataSyncChanges() {
    auto currentSnapshot = buildUserDataSnapshot(app_);
    std::vector<std::string> changed =
        changedSnapshotPaths(userDataSnapshot_, currentSnapshot);
    if (changed.empty()) {
        pendingUserDataSnapshot_.clear();
        pendingUserDataChangedPaths_.clear();
        pendingUserDataChange_ = false;
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (!pendingUserDataChange_ ||
        pendingUserDataSnapshot_ != currentSnapshot) {
        pendingUserDataSnapshot_ = std::move(currentSnapshot);
        pendingUserDataChangedPaths_ = std::move(changed);
        pendingUserDataChangeSince_ = now;
        pendingUserDataChange_ = true;
        return;
    }

    auto quietFor = std::chrono::duration<double>(now - pendingUserDataChangeSince_).count();
    if (quietFor < userDataSyncDebounceSeconds()) return;

    applyUserDataSyncChanges();
}

void MainWindow::applyUserDataSyncChanges() {
    if (!app_ || !pendingUserDataChange_) return;

    bool tagsChanged = false;
    bool readingPlansChanged = false;
    bool studypadChanged = false;
    bool currentDocumentChanged = false;

    fs::path userDataDir(app_->getUserDataDir());
    fs::path studypadDir(app_->getStudypadDir());
    std::string currentDocumentPath =
        rightPane_ ? rightPane_->currentDocumentPath() : std::string();
    currentDocumentPath = normalizePath(currentDocumentPath);

    for (const std::string& pathText : pendingUserDataChangedPaths_) {
        fs::path path(pathText);
        if (pathsEqual(pathText, (userDataDir / "tags.db").string())) {
            tagsChanged = true;
        } else if (pathsEqual(pathText, (userDataDir / "reading_plans.db").string())) {
            readingPlansChanged = true;
        }

        if (pathIsInsideDirectory(pathText, studypadDir.string())) {
            studypadChanged = true;
            if (!currentDocumentPath.empty() && pathsEqual(pathText, currentDocumentPath)) {
                currentDocumentChanged = true;
            }
        }

        if (filenameLooksLikeSyncConflict(path)) {
            studypadChanged = studypadChanged ||
                              pathIsInsideDirectory(pathText, studypadDir.string());
        }
    }

    bool conflictDetected = snapshotHasSyncConflict(pendingUserDataSnapshot_);
    bool dirtyDocumentConflict = false;
    std::string conflictCopyPath;
    if (currentDocumentChanged && rightPane_ &&
        rightPane_->currentDocumentHasUnsavedChanges()) {
        dirtyDocumentConflict = true;
        conflictCopyPath = rightPane_->saveCurrentDocumentConflictCopy();
    }

    if (tagsChanged) {
        if (!app_->tagManager().load((userDataDir / "tags.db").string())) {
            showTransientStatus("Failed to reload synced tags.", 8.0);
        }
    }
    if (readingPlansChanged) {
        if (!app_->readingPlanManager().load((userDataDir / "reading_plans.db").string())) {
            showTransientStatus("Failed to reload synced reading plans.", 8.0);
        }
    }

    if (tagsChanged || readingPlansChanged) {
        refresh();
    }
    if (studypadChanged && rightPane_) {
        rightPane_->refreshDocumentsForExternalChange(
            currentDocumentChanged && !dirtyDocumentConflict);
    }

    if (dirtyDocumentConflict) {
        if (!conflictCopyPath.empty()) {
            showTransientStatus("Synced studypad changed; local edits were saved as a conflict copy.",
                                8.0);
        } else {
            showTransientStatus("Synced studypad changed; unsaved local edits were kept.",
                                8.0);
        }
    } else if (conflictDetected) {
        showTransientStatus("Syncthing conflict file detected in user data folder.", 8.0);
    } else if (tagsChanged || readingPlansChanged || studypadChanged) {
        showTransientStatus("Reloaded synced user data.", 3.5);
    }

    userDataSnapshot_ = buildUserDataSnapshot(app_);
    pendingUserDataSnapshot_.clear();
    pendingUserDataChangedPaths_.clear();
    pendingUserDataChange_ = false;
}

void MainWindow::onUserDataSyncPoll(void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;

    self->userDataSyncPollScheduled_ = false;
    if (!self->shown()) return;

    self->pollUserDataSyncChanges();
    Fl::repeat_timeout(userDataSyncPollSeconds(), onUserDataSyncPoll, self);
    self->userDataSyncPollScheduled_ = true;
}

void MainWindow::scheduleTabSnapshotEviction() {
    if (tabCacheEvictionScheduled_) return;
    Fl::add_timeout(0.0, onDeferredTabSnapshotEviction, this);
    tabCacheEvictionScheduled_ = true;
}

void MainWindow::onDeferredTabSnapshotEviction(void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;

    self->tabCacheEvictionScheduled_ = false;
    self->evictOldTabSnapshots();
}

int MainWindow::handle(int event) {
    if (event == FL_RELEASE) {
        // Refresh layout chrome after a potential splitter drag completes
        if (newStudyTabButton_) newStudyTabButton_->redraw();
        if (studyTabsWidget_) {
            studyTabsWidget_->damage(FL_DAMAGE_ALL);
            studyTabsWidget_->redraw();
        }
        if (leftPane_) leftPane_->redrawChrome();
        if (biblePane_) biblePane_->redrawChrome();
        if (rightPane_) rightPane_->redrawChrome();
        if (studyArea_) {
            studyArea_->damage(FL_DAMAGE_ALL);
            studyArea_->redraw();
        }
        if (contentTile_) {
            contentTile_->damage(FL_DAMAGE_ALL);
            contentTile_->redraw();
        }
        damage(FL_DAMAGE_ALL);
        redraw();
    }

    if (event == FL_PUSH || event == FL_RELEASE || event == FL_MOUSEWHEEL) {
        updateStatusBar();
    }

    if (event == FL_SHORTCUT) {
        const bool ctrl = (Fl::event_state() & FL_CTRL) != 0;
        const int key = Fl::event_key();
        if (ctrl && (key == 'p' || key == 'P')) {
            onViewParallel(nullptr, this);
            return 1;
        }
        if (ctrl && (key == 't' || key == 'T')) {
            onViewNewStudyTab(nullptr, this);
            return 1;
        }
        if (Fl::event_key() == FL_Escape) {
            hideWordInfo();
            return 1;
        }
    }

    int handled = Fl_Double_Window::handle(event);

    if ((event == FL_PUSH || event == FL_RELEASE ||
         event == FL_KEYDOWN || event == FL_SHORTCUT) &&
        !applyingTabState_) {
        syncStudyTabSelection();
    }

    return handled;
}

void MainWindow::syncStudyTabSelection() {
    if (!studyTabsWidget_) return;

    Fl_Widget* active = studyTabsWidget_->value();
    if (!active) return;

    for (size_t i = 0; i < studyTabs_.size(); ++i) {
        if (studyTabs_[i].tabGroup != active) continue;
        if (static_cast<int>(i) != activeStudyTab_ ||
            appliedStudyTabGroup_ != active) {
            perf::logf("syncStudyTabSelection target index=%zu", i);
            activateStudyTab(static_cast<int>(i));
        }
        return;
    }
}

void MainWindow::scheduleDeferredDocumentRestore(const std::string& path,
                                                 bool documentsTabActive) {
    pendingDocumentRestorePath_ =
        endsWithIgnoreCase(path, ".studypad") ? path : std::string();
    pendingDocumentsTabActive_ = documentsTabActive;
    Fl::remove_timeout(onDeferredDocumentRestore, this);
    if (pendingDocumentRestorePath_.empty() && !pendingDocumentsTabActive_) {
        documentRestoreScheduled_ = false;
        return;
    }
    Fl::add_timeout(0.15, onDeferredDocumentRestore, this);
    documentRestoreScheduled_ = true;
}

void MainWindow::restoreDeferredDocumentSession() {
    documentRestoreScheduled_ = false;
    if (!rightPane_) return;

    const bool keepDocumentsActive = pendingDocumentsTabActive_;
    if (!pendingDocumentRestorePath_.empty()) {
        rightPane_->openDocument(pendingDocumentRestorePath_, keepDocumentsActive);
    }
    if (keepDocumentsActive) {
        rightPane_->setDocumentsTabActive(true);
    }

    rightPane_->redrawChrome();

    pendingDocumentRestorePath_.clear();
    pendingDocumentsTabActive_ = false;
}

void MainWindow::onStudyTabChange(Fl_Widget* /*w*/, void* data) {
    perf::ScopeTimer timer("MainWindow::onStudyTabChange");
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;
    self->syncStudyTabSelection();
}

void MainWindow::onDeferredDocumentRestore(void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;
    self->restoreDeferredDocumentSession();
}

bool MainWindow::exportSettingsArchive(const std::string& archivePath,
                                       std::string& errorMessage) {
    errorMessage.clear();
    if (!app_) {
        errorMessage = "Application state is not available.";
        return false;
    }

    fs::path destination(archivePath);
    if (destination.empty()) {
        errorMessage = "Choose a destination for the settings archive.";
        return false;
    }

    if (!destination.parent_path().empty() &&
        !ensureDirectoryExists(destination.parent_path().string())) {
        errorMessage = "Failed to prepare the destination directory.";
        return false;
    }

    app_->tagManager().save();
    app_->readingPlanManager().save();
    app_->tagManager().checkpoint();
    app_->readingPlanManager().checkpoint();
    app_->savePreferences();

    fs::path configDir(app_->getConfigDir());
    fs::path userDataDir(app_->getUserDataDir());
    fs::path prefsPath = configDir / "preferences.conf";
    fs::path tagsPath = userDataDir / "tags.db";
    fs::path readingPlansPath = userDataDir / "reading_plans.db";
    fs::path studypadPath = userDataDir / "studypad";

    std::error_code ec;
    if (!fs::exists(prefsPath, ec) || ec) {
        errorMessage = "preferences.conf was not found in the config directory.";
        return false;
    }

    std::string tempDir = makeUniqueTempDir("verdad-settings-export-");
    if (tempDir.empty()) {
        errorMessage = "Failed to create a temporary export directory.";
        return false;
    }

    auto cleanup = [&]() {
        std::error_code cleanupError;
        fs::remove_all(fs::path(tempDir), cleanupError);
    };

    fs::path stagingDir(tempDir);
    if (!copyBinaryFile(prefsPath.string(), (stagingDir / "preferences.conf").string())) {
        cleanup();
        errorMessage = "Failed to stage preferences.conf for export.";
        return false;
    }

    bool includeTagsDb = fs::exists(tagsPath, ec) && !ec;
    if (includeTagsDb &&
        !copyBinaryFile(tagsPath.string(), (stagingDir / "tags.db").string())) {
        cleanup();
        errorMessage = "Failed to stage tags.db for export.";
        return false;
    }

    bool includeReadingPlansDb = fs::exists(readingPlansPath, ec) && !ec;
    if (includeReadingPlansDb &&
        !copyBinaryFile(readingPlansPath.string(),
                        (stagingDir / "reading_plans.db").string())) {
        cleanup();
        errorMessage = "Failed to stage reading_plans.db for export.";
        return false;
    }

    fs::path stagingStudypad = stagingDir / "studypad";
    if (fs::exists(studypadPath, ec) && !ec) {
        if (!copyDirectoryRecursive(studypadPath.string(), stagingStudypad.string())) {
            cleanup();
            errorMessage = "Failed to stage the studypad directory for export.";
            return false;
        }
    } else if (!ensureDirectoryExists(stagingStudypad.string())) {
        cleanup();
        errorMessage = "Failed to create an empty studypad staging directory.";
        return false;
    }

    std::error_code removeError;
    fs::remove(destination, removeError);

    if (!runZipArchive(stagingDir.string(),
                       destination.string(),
                       includeTagsDb,
                       includeReadingPlansDb)) {
        cleanup();
        errorMessage = "zip failed to create the settings archive.";
        return false;
    }

    cleanup();
    return true;
}

bool MainWindow::importSettingsArchive(const std::string& archivePath,
                                       std::string& errorMessage) {
    errorMessage.clear();
    if (!app_) {
        errorMessage = "Application state is not available.";
        return false;
    }

    fs::path archive(archivePath);
    std::error_code ec;
    if (!fs::exists(archive, ec) || ec) {
        errorMessage = "The selected settings archive does not exist.";
        return false;
    }

    std::string tempDir = makeUniqueTempDir("verdad-settings-import-");
    if (tempDir.empty()) {
        errorMessage = "Failed to create a temporary import directory.";
        return false;
    }

    auto cleanup = [&]() {
        std::error_code cleanupError;
        fs::remove_all(fs::path(tempDir), cleanupError);
    };

    if (!runUnzipArchive(archive.string(), tempDir)) {
        cleanup();
        errorMessage = "unzip failed to extract the selected archive.";
        return false;
    }

    fs::path extractedDir(tempDir);
    fs::path extractedPrefs = extractedDir / "preferences.conf";
    fs::path extractedTags = extractedDir / "tags.db";
    fs::path extractedReadingPlans = extractedDir / "reading_plans.db";
    fs::path extractedStudypad = extractedDir / "studypad";
    if (!fs::exists(extractedPrefs, ec) || ec) {
        cleanup();
        errorMessage = "The archive does not contain preferences.conf.";
        return false;
    }

    fs::path configDir(app_->getConfigDir());
    fs::path userDataDir(app_->getUserDataDir());
    if (!ensureDirectoryExists(configDir.string())) {
        cleanup();
        errorMessage = "Failed to prepare the application config directory.";
        return false;
    }
    if (!ensureDirectoryExists(userDataDir.string())) {
        cleanup();
        errorMessage = "Failed to prepare the user data directory.";
        return false;
    }

    if (!copyBinaryFile(extractedPrefs.string(),
                        (configDir / "preferences.conf").string())) {
        cleanup();
        errorMessage = "Failed to copy preferences.conf into the config directory.";
        return false;
    }

    if (fs::exists(extractedTags, ec) && !ec) {
        if (!copyBinaryFile(extractedTags.string(),
                            (userDataDir / "tags.db").string())) {
            cleanup();
            errorMessage = "Failed to copy tags.db into the user data directory.";
            return false;
        }
    }

    if (fs::exists(extractedReadingPlans, ec) && !ec) {
        if (!copyBinaryFile(extractedReadingPlans.string(),
                            (userDataDir / "reading_plans.db").string())) {
            cleanup();
            errorMessage = "Failed to copy reading_plans.db into the user data directory.";
            return false;
        }
    }

    if (fs::exists(extractedStudypad, ec) && !ec) {
        if (!copyDirectoryRecursive(extractedStudypad.string(),
                                    (userDataDir / "studypad").string())) {
            cleanup();
            errorMessage = "Failed to copy the studypad directory into the user data directory.";
            return false;
        }
    }

    cleanup();

    fs::path tagsPath = userDataDir / "tags.db";
    if (fs::exists(tagsPath, ec) && !ec) {
        app_->tagManager().load(tagsPath.string());
    }
    fs::path readingPlansPath = userDataDir / "reading_plans.db";
    if (fs::exists(readingPlansPath, ec) && !ec) {
        app_->readingPlanManager().load(readingPlansPath.string());
    }

    if (!app_->loadPreferencesFromFile((configDir / "preferences.conf").string(), true)) {
        errorMessage = "Failed to apply the imported preferences.";
        return false;
    }

    refresh();
    app_->savePreferences();
    resetUserDataMonitorSnapshot();
    return true;
}

bool MainWindow::requestClose() {
    if (rightPane_ && !rightPane_->maybeSaveDocumentChanges()) return false;
    Fl_Double_Window::hide();
    return true;
}

void MainWindow::buildMenu() {
    menuBar_->add("&File/&New Studypad", FL_CTRL + 'n', onFileNewDocument, this);
    menuBar_->add("&File/&Save Studypad", FL_CTRL + 's', onFileSaveDocument, this);
    menuBar_->add("&File/&Export Studypad to ODT...", 0, onFileExportDocumentOdt, this);
    menuBar_->add("&File/&Quit", FL_CTRL + 'q', onFileQuit, this);
    menuBar_->add("&Tools/&Module Manager...", 0, onFileModuleManager, this);
    menuBar_->add("&Tools/&Import Files...", 0, onToolsImportFiles, this);
    menuBar_->add("&Tools/&Settings...", 0, onViewSettings, this);
    menuBar_->add("&Tools/&Import Settings...", 0, onToolsImportSettings, this);
    menuBar_->add("&Tools/&Export Settings...", 0, onToolsExportSettings, this);
    menuBar_->add("&Help/&Help Topics", FL_F + 1, onHelpSearch, this);
    menuBar_->add("&Help/&About Verdad", 0, onHelpAbout, this);
}

void MainWindow::onWindowClose(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;
    self->requestClose();
}

void MainWindow::onFileQuit(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;
    self->requestClose();
}

void MainWindow::onFileModuleManager(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self || !self->app_) return;

    ModuleManagerDialog dlg(self->app_);
    dlg.openModal();
}

void MainWindow::onFileNewDocument(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self || !self->rightPane_) return;
    self->rightPane_->newDocument();
}

void MainWindow::onFileSaveDocument(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self || !self->rightPane_) return;
    self->rightPane_->saveDocument();
}

void MainWindow::onFileExportDocumentOdt(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self || !self->rightPane_) return;
    self->rightPane_->exportDocumentToOdt();
}

void MainWindow::onToolsImportFiles(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self || !self->app_) return;

    ImportFilesDialog dialog(self);
    bool changed = dialog.runModal();
    if (changed) {
        self->showTransientStatus("Updated imported files", 2.8);
    }
}

void MainWindow::onToolsImportSettings(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;

    Fl_Native_File_Chooser chooser;
    chooser.title("Import Settings");
    chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
    chooser.filter("ZIP Files\t*.{zip,ZIP}\nAll Files\t*");

    int result = chooser.show();
    if (result != 0) {
        if (result < 0) fl_alert("Unable to open the file chooser.");
        return;
    }

    std::string path = chooser.filename() ? chooser.filename() : "";
    if (path.empty()) return;

    int confirm = fl_choice("Import settings from \"%s\"?\n\n"
                            "This will replace the current preferences, tags, and studypads, "
                            "then reopen the imported study tabs.\n"
                            "Unsaved studypad edits will be lost.",
                            "Cancel",
                            "Import",
                            nullptr,
                            pathLeaf(path).c_str());
    if (confirm != 1) return;

    std::string errorMessage;
    if (!self->importSettingsArchive(path, errorMessage)) {
        fl_alert("Failed to import settings:\n%s", errorMessage.c_str());
        return;
    }

    self->showTransientStatus("Imported settings", 2.8);
}

void MainWindow::onToolsExportSettings(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;

    Fl_Native_File_Chooser chooser;
    chooser.title("Export Settings Directory");
    chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);

    int result = chooser.show();
    if (result != 0) {
        if (result < 0) fl_alert("Unable to open the file chooser.");
        return;
    }

    std::string directory = chooser.filename() ? chooser.filename() : "";
    if (directory.empty()) return;

    fs::path archivePath = fs::path(directory) / "verdad_settings.zip";
    std::error_code ec;
    if (fs::exists(archivePath, ec) && !ec) {
        int overwrite = fl_choice("Overwrite existing file \"%s\"?",
                                  "Cancel",
                                  "Overwrite",
                                  nullptr,
                                  archivePath.filename().string().c_str());
        if (overwrite != 1) return;
    }

    std::string errorMessage;
    if (!self->exportSettingsArchive(archivePath.string(), errorMessage)) {
        fl_alert("Failed to export settings:\n%s", errorMessage.c_str());
        return;
    }

    self->showTransientStatus("Exported " + archivePath.filename().string(), 2.8);
}

void MainWindow::onViewParallel(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (self->biblePane_) {
        self->biblePane_->toggleParallel();
        self->biblePane_->redrawChrome();
    }
    if (self->rightPane_) {
        self->rightPane_->redrawChrome();
    }
}

void MainWindow::onViewSettings(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self || !self->app_) return;

    auto current = self->app_->appearanceSettings();
    auto currentPreview = self->app_->previewDictionarySettings();
    auto currentOfflineTranslation =
        self->app_->offlineTranslationSettings();
    std::string defaultOfflineTranslationDirectory =
        (fs::path(self->app_->getConfigDir()) / "dictionaries").string();
    std::string currentOfflineTranslationDirectory =
        self->app_->effectiveOfflineTranslationDictionaryDirectory();
    std::string currentUserDataDir = self->app_->getUserDataDir();
    std::vector<std::string> greekDictionaryModules =
        self->app_->strongsDictionaryModules('G');
    std::vector<std::string> hebrewDictionaryModules =
        self->app_->strongsDictionaryModules('H');
    std::vector<std::string> languageCodes =
        dictionaryLanguageCodes(self->app_, currentPreview);

    constexpr int dialogW = 720;
    constexpr int rowStep = 34;
    constexpr int tabsX = 12;
    constexpr int tabsY = 12;
    constexpr int tabsHeaderH = 28;
    constexpr int groupPadX = 20;
    constexpr int groupPadY = 18;
    constexpr int labelW = 170;
    constexpr int fieldW = 470;
    constexpr int fieldXOffset = 190;
    constexpr int spinnerW = 90;

    int appearanceRowCount = 8;
    int dictionaryRowCount = 7 + static_cast<int>(languageCodes.size());
    int editorRowCount = 2;
    int dataRowCount = 1;
    int maxRowCount = std::max({appearanceRowCount,
                                dictionaryRowCount,
                                editorRowCount,
                                dataRowCount});
    int tabsH = tabsHeaderH + (groupPadY * 2) + (maxRowCount * rowStep);
    int buttonsY = tabsY + tabsH + 12;
    int dialogH = buttonsY + 44;

    Fl_Double_Window* dlg = new Fl_Double_Window(dialogW, dialogH, "Settings");
    dlg->set_modal();
    dlg->begin();

    Fl_Tabs* tabs = new Fl_Tabs(tabsX, tabsY, dialogW - (tabsX * 2), tabsH);
    tabs->begin();

    int groupX = tabsX;
    int groupY = tabsY + tabsHeaderH;
    int groupW = tabs->w();
    int groupH = tabsH - tabsHeaderH;
    int labelX = groupX + groupPadX;
    int fieldX = groupX + fieldXOffset;

    const std::vector<std::string> fonts = self->app_->systemFontFamilies();

    Fl_Group* appearanceTab =
        new Fl_Group(groupX, groupY, groupW, groupH, "Appearance");
    appearanceTab->begin();

    int rowY = groupY + groupPadY;
    Fl_Box* themeLabel = new Fl_Box(labelX, rowY, labelW, 24, "Theme:");
    themeLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Choice* themeChoice = new Fl_Choice(fieldX, rowY, spinnerW + 40, 24);
    themeChoice->add("Light");
    themeChoice->add("Dark");
    themeChoice->value(current.themeMode == VerdadApp::ThemeMode::Dark ? 1 : 0);
    rowY += rowStep;

    Fl_Box* appFontLabel = new Fl_Box(labelX, rowY, labelW, 24, "Application font:");
    appFontLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    auto* appFontChoice = new FilterableChoiceWidget(fieldX, rowY, fieldW, 24);
    appFontChoice->setNoMatchesLabel("No matching fonts");
    appFontChoice->setItems(fonts);
    appFontChoice->setSelectedValue(current.appFontName);
    rowY += rowStep;

    Fl_Box* appSizeLabel = new Fl_Box(labelX, rowY, labelW, 24, "Application size:");
    appSizeLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Spinner* appSizeSpinner = new Fl_Spinner(fieldX, rowY, spinnerW, 24);
    appSizeSpinner->minimum(8);
    appSizeSpinner->maximum(36);
    appSizeSpinner->step(1);
    appSizeSpinner->value(current.appFontSize);
    rowY += rowStep;

    Fl_Box* textFontLabel = new Fl_Box(labelX, rowY, labelW, 24, "Text font:");
    textFontLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    auto* textFontChoice = new FilterableChoiceWidget(fieldX, rowY, fieldW, 24);
    textFontChoice->setNoMatchesLabel("No matching fonts");
    textFontChoice->setItems(fonts);
    textFontChoice->setSelectedValue(current.textFontFamily);
    rowY += rowStep;

    Fl_Box* textSizeLabel = new Fl_Box(labelX, rowY, labelW, 24, "Text size:");
    textSizeLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Spinner* textSizeSpinner = new Fl_Spinner(fieldX, rowY, spinnerW, 24);
    textSizeSpinner->minimum(8);
    textSizeSpinner->maximum(36);
    textSizeSpinner->step(1);
    textSizeSpinner->value(current.textFontSize);
    rowY += rowStep;

    Fl_Box* textLineHeightLabel =
        new Fl_Box(labelX, rowY, labelW, 24, "Pane line height:");
    textLineHeightLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Spinner* textLineHeightSpinner = new Fl_Spinner(fieldX, rowY, spinnerW, 24);
    textLineHeightSpinner->minimum(1.0);
    textLineHeightSpinner->maximum(2.0);
    textLineHeightSpinner->step(0.05);
    textLineHeightSpinner->value(current.textLineHeight);
    rowY += rowStep;

    Fl_Box* browserSpacingLabel =
        new Fl_Box(labelX, rowY, labelW, 24, "Browser line spacing:");
    browserSpacingLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Spinner* browserSpacingSpinner = new Fl_Spinner(fieldX, rowY, spinnerW, 24);
    browserSpacingSpinner->minimum(0);
    browserSpacingSpinner->maximum(16);
    browserSpacingSpinner->step(1);
    browserSpacingSpinner->value(current.browserLineSpacing);
    rowY += rowStep;

    Fl_Box* hoverLabel = new Fl_Box(labelX, rowY, labelW, 24, "Hover delay (ms):");
    hoverLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Spinner* hoverDelaySpinner = new Fl_Spinner(fieldX, rowY, spinnerW, 24);
    hoverDelaySpinner->minimum(100);
    hoverDelaySpinner->maximum(5000);
    hoverDelaySpinner->step(100);
    hoverDelaySpinner->value(current.hoverDelayMs);

    appearanceTab->end();

    Fl_Group* dictionariesTab =
        new Fl_Group(groupX, groupY, groupW, groupH, "Dictionaries");
    dictionariesTab->begin();

    rowY = groupY + groupPadY;
    Fl_Box* greekDictLabel = new Fl_Box(labelX, rowY, labelW, 24, "Greek Strong's dict:");
    greekDictLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Choice* greekDictChoice = new WrappingChoice(fieldX, rowY, fieldW, 24);
    bool hasGreekPreviewDictionaries = populateChoiceWithItems(
        greekDictChoice,
        greekDictionaryModules,
        self->app_->preferredPreviewDictionary('G'),
        "No Greek dictionaries installed");
    rowY += rowStep;

    Fl_Box* hebrewDictLabel = new Fl_Box(labelX, rowY, labelW, 24, "Hebrew Strong's dict:");
    hebrewDictLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Choice* hebrewDictChoice = new WrappingChoice(fieldX, rowY, fieldW, 24);
    bool hasHebrewPreviewDictionaries = populateChoiceWithItems(
        hebrewDictChoice,
        hebrewDictionaryModules,
        self->app_->preferredPreviewDictionary('H'),
        "No Hebrew dictionaries installed");
    rowY += rowStep;

    struct LanguageRow {
        std::string languageCode;
        Fl_Choice* choice = nullptr;
        bool hasChoices = false;
    };
    std::vector<LanguageRow> languageRows;
    languageRows.reserve(languageCodes.size());

    for (const auto& code : languageCodes) {
        std::string labelText = languageDisplayName(code) + " dictionary:";
        auto* label = new Fl_Box(labelX, rowY, labelW, 24);
        label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        label->copy_label(labelText.c_str());

        auto* choice = new WrappingChoice(fieldX, rowY, fieldW, 24);
        std::vector<std::string> modules = self->app_->wordDictionaryModules(code);
        std::string emptyLabel = "No " + languageDisplayName(code) +
                                 " dictionaries installed";
        bool hasChoices = populateChoiceWithItems(
            choice,
            modules,
            self->app_->preferredWordDictionary(code),
            emptyLabel.c_str());

        languageRows.push_back(LanguageRow{code, choice, hasChoices});
        rowY += rowStep;
    }

    auto* offlineTranslationCheck = new Fl_Check_Button(
        labelX,
        rowY,
        groupW - (groupPadX * 2),
        24,
        "Show offline English translations on word hover");
    offlineTranslationCheck->value(currentOfflineTranslation.enabled ? 1 : 0);
    rowY += rowStep;

    constexpr int dictionaryButtonW = 70;
    constexpr int dictionaryButtonGap = 8;
    Fl_Box* offlineDirectoryLabel =
        new Fl_Box(labelX, rowY, labelW, 24, "Dictionary folder:");
    offlineDirectoryLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    const int dictionaryInputW = fieldW - (dictionaryButtonW * 2) -
                                 (dictionaryButtonGap * 2);
    auto* offlineDirectoryInput =
        new Fl_Input(fieldX, rowY, dictionaryInputW, 24);
    offlineDirectoryInput->value(currentOfflineTranslationDirectory.c_str());
    auto* offlineBrowseButton = new Fl_Button(
        fieldX + dictionaryInputW + dictionaryButtonGap,
        rowY,
        dictionaryButtonW,
        24,
        "Browse");
    auto* offlineRescanButton = new Fl_Button(
        fieldX + dictionaryInputW + dictionaryButtonGap + dictionaryButtonW +
            dictionaryButtonGap,
        rowY,
        dictionaryButtonW,
        24,
        "Rescan");
    rowY += rowStep;

    auto* offlineDetectedBox = new Fl_Box(
        labelX, rowY, groupW - (groupPadX * 2), 24);
    offlineDetectedBox->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    rowY += rowStep;

    auto* offlineIssuesBox = new Fl_Box(
        labelX, rowY, groupW - (groupPadX * 2), (rowStep * 2) - 8);
    offlineIssuesBox->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);

    struct OfflineTranslationUiState {
        Fl_Input* directoryInput = nullptr;
        Fl_Box* detectedBox = nullptr;
        Fl_Box* issuesBox = nullptr;
        std::string defaultDirectory;
        WikDictManager manager;
    };
    OfflineTranslationUiState offlineTranslationUiState{
        offlineDirectoryInput,
        offlineDetectedBox,
        offlineIssuesBox,
        defaultOfflineTranslationDirectory,
        WikDictManager{}};
    WikDictScanReport initialOfflineReport =
        offlineTranslationUiState.manager.scan(
            currentOfflineTranslationDirectory);
    updateOfflineTranslationScanLabels(
        offlineDetectedBox, offlineIssuesBox, initialOfflineReport);

    offlineBrowseButton->callback(
        [](Fl_Widget*, void* data) {
            auto* state = static_cast<OfflineTranslationUiState*>(data);
            if (!state || !state->directoryInput) return;

            Fl_Native_File_Chooser chooser;
            chooser.title("Choose Offline Dictionary Folder");
            chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
            const char* currentValue = state->directoryInput->value();
            if (currentValue && currentValue[0]) {
                chooser.directory(currentValue);
            }
            if (chooser.show() != 0 || !chooser.filename()) return;
            state->directoryInput->value(chooser.filename());
        },
        &offlineTranslationUiState);

    offlineRescanButton->callback(
        [](Fl_Widget*, void* data) {
            auto* state = static_cast<OfflineTranslationUiState*>(data);
            if (!state || !state->directoryInput) return;

            std::string directory = trimCopy(
                state->directoryInput->value()
                    ? state->directoryInput->value()
                    : "");
            if (directory.empty()) directory = state->defaultDirectory;
            WikDictScanReport report = state->manager.scan(directory);
            updateOfflineTranslationScanLabels(
                state->detectedBox, state->issuesBox, report);
        },
        &offlineTranslationUiState);

    dictionariesTab->end();

    Fl_Group* editorTab =
        new Fl_Group(groupX, groupY, groupW, groupH, "Editor");
    editorTab->begin();

    rowY = groupY + groupPadY;
    Fl_Box* indentLabel = new Fl_Box(labelX, rowY, labelW, 24, "Editor tab width:");
    indentLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Spinner* indentSpinner = new Fl_Spinner(fieldX, rowY, spinnerW, 24);
    indentSpinner->minimum(1);
    indentSpinner->maximum(8);
    indentSpinner->step(1);
    indentSpinner->value(current.editorIndentWidth);
    rowY += rowStep;

    Fl_Box* editorLineHeightLabel =
        new Fl_Box(labelX, rowY, labelW, 24, "Studypad line height:");
    editorLineHeightLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Spinner* editorLineHeightSpinner =
        new Fl_Spinner(fieldX, rowY, spinnerW, 24);
    editorLineHeightSpinner->minimum(1.0);
    editorLineHeightSpinner->maximum(2.0);
    editorLineHeightSpinner->step(0.05);
    editorLineHeightSpinner->value(current.editorLineHeight);

    editorTab->end();

    Fl_Group* dataTab =
        new Fl_Group(groupX, groupY, groupW, groupH, "Data");
    dataTab->begin();

    rowY = groupY + groupPadY;
    Fl_Box* userDataDirLabel = new Fl_Box(labelX, rowY, labelW, 24, "User data folder:");
    userDataDirLabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    constexpr int browseButtonW = 68;
    Fl_Input* userDataDirInput =
        new Fl_Input(fieldX, rowY, fieldW - browseButtonW - 8, 24);
    userDataDirInput->value(currentUserDataDir.c_str());
    Fl_Button* userDataBrowseButton =
        new Fl_Button(fieldX + fieldW - browseButtonW, rowY, browseButtonW, 24, "Browse");

    dataTab->end();

    tabs->end();
    tabs->value(appearanceTab);

    Fl_Button* cancelBtn = new Fl_Button(dialogW - 170, buttonsY, 80, 28, "Cancel");
    Fl_Return_Button* applyBtn =
        new Fl_Return_Button(dialogW - 80, buttonsY, 60, 28, "Apply");

    struct DialogState {
        bool accepted = false;
    };
    auto* state = new DialogState();

    struct BrowseState {
        Fl_Input* input = nullptr;
    };
    auto* browseState = new BrowseState{userDataDirInput};

    cancelBtn->callback(
        [](Fl_Widget* w, void* data) {
            auto* st = static_cast<DialogState*>(data);
            st->accepted = false;
            if (w && w->window()) w->window()->hide();
        },
        state);

    applyBtn->callback(
        [](Fl_Widget* w, void* data) {
            auto* st = static_cast<DialogState*>(data);
            st->accepted = true;
            if (w && w->window()) w->window()->hide();
        },
        state);

    userDataBrowseButton->callback(
        [](Fl_Widget*, void* data) {
            auto* st = static_cast<BrowseState*>(data);
            if (!st || !st->input) return;

            Fl_Native_File_Chooser chooser;
            chooser.title("Choose User Data Folder");
            chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
            const char* currentValue = st->input->value();
            if (currentValue && currentValue[0]) {
                chooser.directory(currentValue);
            }
            if (chooser.show() != 0 || !chooser.filename()) return;
            st->input->value(chooser.filename());
        },
        browseState);

    dlg->end();
    ui_font::applyCurrentAppUiFont(dlg);
    dlg->show();
    while (dlg->shown()) {
        Fl::wait();
    }

    if (state->accepted) {
        VerdadApp::AppearanceSettings updated = current;
        VerdadApp::PreviewDictionarySettings updatedPreview = currentPreview;
        OfflineTranslationSettings updatedOfflineTranslation =
            currentOfflineTranslation;
        std::string requestedUserDataDir =
            trimCopy(userDataDirInput->value() ? userDataDirInput->value() : "");

        if (requestedUserDataDir.empty()) {
            fl_alert("Choose a user data folder.");
            delete browseState;
            delete state;
            delete dlg;
            return;
        }

        updated.themeMode =
            (themeChoice->value() == 1)
                ? VerdadApp::ThemeMode::Dark
                : VerdadApp::ThemeMode::Light;
        std::string appFontName = appFontChoice->selectedValue();
        if (!appFontName.empty()) {
            updated.appFontName = appFontName;
        }
        updated.appFontSize = static_cast<int>(appSizeSpinner->value());

        std::string textFontName = textFontChoice->selectedValue();
        if (!textFontName.empty()) {
            updated.textFontFamily = textFontName;
        }
        updated.textFontSize = static_cast<int>(textSizeSpinner->value());
        updated.textLineHeight = textLineHeightSpinner->value();
        updated.browserLineSpacing = static_cast<int>(browserSpacingSpinner->value());
        updated.hoverDelayMs = static_cast<int>(hoverDelaySpinner->value());
        updated.editorIndentWidth = static_cast<int>(indentSpinner->value());
        updated.editorLineHeight = editorLineHeightSpinner->value();

        const Fl_Menu_Item* greekDictItem = greekDictChoice->mvalue();
        if (hasGreekPreviewDictionaries && greekDictItem && greekDictItem->label()) {
            updatedPreview.greekModule = greekDictItem->label();
        }

        const Fl_Menu_Item* hebrewDictItem = hebrewDictChoice->mvalue();
        if (hasHebrewPreviewDictionaries && hebrewDictItem && hebrewDictItem->label()) {
            updatedPreview.hebrewModule = hebrewDictItem->label();
        }

        for (const auto& row : languageRows) {
            if (!row.hasChoices || !row.choice) continue;

            const Fl_Menu_Item* item = row.choice->mvalue();
            if (item && item->label()) {
                updatedPreview.languageModules[row.languageCode] = item->label();
            }
        }

        updatedOfflineTranslation.enabled =
            offlineTranslationCheck->value() != 0;
        std::string requestedOfflineDirectory = trimCopy(
            offlineDirectoryInput->value()
                ? offlineDirectoryInput->value()
                : "");
        if (requestedOfflineDirectory.empty() ||
            (currentOfflineTranslation.dictionaryDirectory.empty() &&
             pathsEqual(requestedOfflineDirectory,
                        currentOfflineTranslationDirectory))) {
            updatedOfflineTranslation.dictionaryDirectory.clear();
        } else {
            updatedOfflineTranslation.dictionaryDirectory =
                requestedOfflineDirectory;
        }

        if (!pathsEqual(requestedUserDataDir, currentUserDataDir)) {
            if (self->rightPane_ && !self->rightPane_->maybeSaveDocumentChanges()) {
                delete browseState;
                delete state;
                delete dlg;
                return;
            }

            VerdadApp::UserDataDirectoryMode dataMode =
                VerdadApp::UserDataDirectoryMode::CopyCurrentData;
            if (self->app_->userDataDirectoryHasData(requestedUserDataDir)) {
                int choice = fl_choice(
                    "The selected folder already contains Verdad user data.",
                    "Cancel",
                    "Use Existing",
                    "Replace");
                if (choice == 0) {
                    delete browseState;
                    delete state;
                    delete dlg;
                    return;
                }
                dataMode = (choice == 1)
                               ? VerdadApp::UserDataDirectoryMode::UseExisting
                               : VerdadApp::UserDataDirectoryMode::CopyCurrentData;
            }

            std::string oldUserDataDir = self->app_->getUserDataDir();
            std::string errorMessage;
            if (!self->app_->setUserDataDir(requestedUserDataDir,
                                            dataMode,
                                            errorMessage)) {
                fl_alert("%s", errorMessage.c_str());
                delete browseState;
                delete state;
                delete dlg;
                return;
            }

            if (self->rightPane_) {
                self->rightPane_->onUserDataDirectoryChanged(
                    oldUserDataDir,
                    self->app_->getUserDataDir());
            }
            self->refresh();
            self->resetUserDataMonitorSnapshot();
            self->showTransientStatus("User data folder updated.", 3.0);
        }

        self->app_->setPreviewDictionarySettings(updatedPreview);
        self->app_->setOfflineTranslationSettings(updatedOfflineTranslation);
        self->app_->setAppearanceSettings(updated);
        self->app_->savePreferences();
    }

    delete browseState;
    delete state;
    delete dlg;
}

void MainWindow::onViewNewStudyTab(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;
    self->duplicateActiveStudyTab();
}

void MainWindow::onViewCloseStudyTab(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;
    self->closeActiveStudyTab();
}

void MainWindow::onHelpSearch(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;
    self->showSearchHelpWindow();
}

void MainWindow::selectHelpTopic(int index) {
    if (!searchHelpHtml_ ||
        index < 0 ||
        index >= static_cast<int>(searchHelpTopics_.size())) {
        return;
    }

    if (searchHelpTopicBrowser_ &&
        searchHelpTopicBrowser_->value() != index + 1) {
        searchHelpTopicBrowser_->value(index + 1);
    }
    searchHelpHtml_->scrollToAnchor(searchHelpTopics_[static_cast<size_t>(index)].first);
}

void MainWindow::onHelpTopicSelect(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self || !self->searchHelpTopicBrowser_) return;

    int line = self->searchHelpTopicBrowser_->value();
    if (line > 0) {
        self->selectHelpTopic(line - 1);
    }
}

void MainWindow::showSearchHelpWindow() {
    if (!searchHelpWindow_) {
        searchHelpWindow_ = new Fl_Double_Window(980, 680, "Help Topics");
        searchHelpWindow_->begin();

        searchHelpTopicBrowser_ = new Fl_Hold_Browser(10, 10, 220, 626);
        searchHelpTopicBrowser_->box(FL_DOWN_BOX);
        searchHelpTopicBrowser_->callback(onHelpTopicSelect, this);

        searchHelpHtml_ = new HtmlWidget(236, 10, 734, 626);
        std::string cssFile = app_->getDataDir() + "/master.css";
        std::ifstream cssStream(cssFile);
        if (cssStream.is_open()) {
            std::string css((std::istreambuf_iterator<char>(cssStream)),
                            std::istreambuf_iterator<char>());
            searchHelpHtml_->setMasterCSS(css);
        }
        searchHelpHtml_->setStyleOverrideCss(lastAppliedTextCss_);
        searchHelpHtml_->setLinkCallback(
            [this](const std::string& url) {
                if (searchHelpHtml_ && !url.empty() && url[0] == '#') {
                    searchHelpHtml_->scrollToAnchor(url.substr(1));
                }
            });

        auto* closeBtn = new Fl_Return_Button(880, 646, 90, 24, "Close");
        closeBtn->callback(
            [](Fl_Widget* w, void* /*data*/) {
                if (w && w->window()) w->window()->hide();
            },
            nullptr);

        searchHelpWindow_->callback(
            [](Fl_Widget* w, void* /*data*/) {
                if (w) w->hide();
            },
            nullptr);
        searchHelpWindow_->resizable(searchHelpHtml_);
        searchHelpWindow_->end();
    }

    searchHelpDocumentHtml_ = readFileOrFallback(
        app_->getDataDir() + "/help.html",
        kFallbackHelpHtml);
    searchHelpTopics_ = parseHelpTopics(searchHelpDocumentHtml_);

    if (searchHelpTopicBrowser_) {
        searchHelpTopicBrowser_->clear();
        for (const auto& topic : searchHelpTopics_) {
            searchHelpTopicBrowser_->add(topic.second.c_str());
        }
    }
    if (searchHelpHtml_) {
        searchHelpHtml_->setHtml(searchHelpDocumentHtml_);
    }
    if (!searchHelpTopics_.empty()) {
        selectHelpTopic(0);
    }
    if (appearanceApplied_ && searchHelpWindow_) {
        Fl_Font boldFont = app_ ? app_->boldFltkFont(lastAppliedAppFont_)
                                : lastAppliedAppFont_;
        ui_font::applyUiFontRecursively(searchHelpWindow_,
                                        lastAppliedAppFont_,
                                        boldFont,
                                        lastAppliedAppFontSize_);
        if (searchHelpHtml_) searchHelpHtml_->setStyleOverrideCss(lastAppliedTextCss_);
    }

    searchHelpWindow_->show();
    searchHelpWindow_->take_focus();
}

void MainWindow::onHelpAbout(Fl_Widget* /*w*/, void* data) {
    auto* self = static_cast<MainWindow*>(data);
    if (!self) return;

    constexpr int kDialogW = 420;
    constexpr int kDialogH = 446;
    constexpr int kIconSize = 192;

    Fl_Double_Window dialog(kDialogW, kDialogH, "About Verdad");
    dialog.set_modal();
    dialog.begin();

    Fl_Box iconBox((kDialogW - kIconSize) / 2, 18, kIconSize, kIconSize);
    iconBox.box(FL_NO_BOX);

    std::unique_ptr<Fl_PNG_Image> aboutIcon;
    std::string aboutIconPath;
    if (self->app_) {
        aboutIconPath = self->app_->getDataDir() + "/verdad_icon.png";
        aboutIcon = std::make_unique<Fl_PNG_Image>(aboutIconPath.c_str());
        if (aboutIcon && aboutIcon->w() > 0 && aboutIcon->h() > 0) {
            iconBox.image(aboutIcon.get());
        }
    }

    Fl_Box titleBox(20, 220, kDialogW - 40, 30, "Verdad Bible Study");
    titleBox.box(FL_NO_BOX);
    titleBox.align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
    titleBox.labelsize(24);

    const std::string versionLabel = std::string("Version ") + build_config::kVersion;
    Fl_Box versionBox(20, 252, kDialogW - 40, 22, versionLabel.c_str());
    versionBox.box(FL_NO_BOX);
    versionBox.align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
    versionBox.labelsize(14);

    Fl_Box detailsBox(
        44, 286, kDialogW - 88, 68,
        "A Bible study application using:\n"
        "FLTK for the user interface\n"
        "litehtml for XHTML rendering\n"
        "SWORD library for Bible modules");
    detailsBox.box(FL_NO_BOX);
    detailsBox.align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    detailsBox.labelsize(14);

    Fl_Button projectLinkButton(44, 360, kDialogW - 88, 24, kVerdadProjectUrl);
    projectLinkButton.box(FL_NO_BOX);
    projectLinkButton.down_box(FL_NO_BOX);
    projectLinkButton.clear_visible_focus();
    projectLinkButton.align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
    projectLinkButton.labelcolor(self->app_ ? self->app_->themePalette().link
                                            : fl_rgb_color(0, 102, 204));
    projectLinkButton.tooltip("Open the Verdad GitHub project page.");
    projectLinkButton.callback(
        [](Fl_Widget* /*w*/, void* /*data*/) {
            if (!openExternalUrl(kVerdadProjectUrl)) {
                fl_alert("Failed to open:\n%s", kVerdadProjectUrl);
            }
        },
        nullptr);

    Fl_Return_Button closeButton((kDialogW - 96) / 2, 394, 96, 28, "Close");
    auto hideDialog = [](Fl_Widget* /*w*/, void* data) {
        auto* win = static_cast<Fl_Window*>(data);
        if (win) win->hide();
    };
    closeButton.callback(hideDialog, &dialog);
    dialog.callback(hideDialog, &dialog);

    dialog.end();

    if (self->appearanceApplied_) {
        Fl_Font appFont = self->lastAppliedAppFont_;
        Fl_Font boldFont = self->app_ ? self->app_->boldFltkFont(appFont) : appFont;
        ui_font::applyUiFontRecursively(
            &dialog, appFont, boldFont, self->lastAppliedAppFontSize_);
        titleBox.labelfont(boldFont);
        titleBox.labelsize(std::max(24, self->lastAppliedAppFontSize_ + 10));
        versionBox.labelsize(std::max(14, self->lastAppliedAppFontSize_));
        detailsBox.labelsize(std::max(14, self->lastAppliedAppFontSize_));
        projectLinkButton.labelsize(std::max(13, self->lastAppliedAppFontSize_));
    } else {
        titleBox.labelfont(FL_HELVETICA_BOLD);
        projectLinkButton.labelsize(13);
    }

    dialog.show();
    closeButton.take_focus();
    while (dialog.shown()) {
        Fl::wait();
    }
}

} // namespace verdad
