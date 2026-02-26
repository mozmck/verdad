#include "app/VerdadApp.h"
#include "sword/SwordManager.h"
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

} // namespace

VerdadApp* VerdadApp::instance_ = nullptr;

VerdadApp::VerdadApp()
    : swordMgr_(std::make_unique<SwordManager>())
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

    // Load tags
    tagMgr_->load(getConfigDir() + "/tags.dat");

    // Set up FLTK
    Fl::scheme("gtk+");
    Fl_File_Icon::load_system_icons();

    // Create main window
    mainWindow_ = std::make_unique<MainWindow>(this, 1200, 800, "Verdad Bible Study");

    // Load user preferences
    loadPreferences();

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
    std::string prefFile = getConfigDir() + "/preferences.conf";
    std::ifstream file(prefFile);
    if (!file.is_open()) return;

    std::unordered_map<std::string, std::string> prefs;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trimCopy(line.substr(0, eq));
        std::string value = line.substr(eq + 1);
        prefs[key] = value;
    }

    if (!mainWindow_) return;

    // New session format: restore full window/tabs/splitter state.
    if (prefs.find("study_tab_count") != prefs.end()) {
        MainWindow::SessionState state;
        state.windowX = parseIntOr(prefs["window_x"], -1);
        state.windowY = parseIntOr(prefs["window_y"], -1);
        state.windowW = parseIntOr(prefs["window_w"], 1200);
        state.windowH = parseIntOr(prefs["window_h"], 800);
        state.leftPaneWidth = parseIntOr(prefs["left_pane_w"], 300);
        state.leftPanePreviewHeight = parseIntOr(prefs["left_pane_preview_h"], 150);
        state.activeStudyTab = parseIntOr(prefs["active_study_tab"], 0);

        int tabCount = std::max(0, parseIntOr(prefs["study_tab_count"], 0));
        tabCount = std::min(tabCount, 64);
        for (int i = 0; i < tabCount; ++i) {
            std::string pfx = "study_tab_" + std::to_string(i) + "_";
            MainWindow::StudyTabState tab;
            tab.module = prefs[pfx + "module"];
            tab.book = prefs[pfx + "book"];
            tab.chapter = parseIntOr(prefs[pfx + "chapter"], 1);
            tab.verse = parseIntOr(prefs[pfx + "verse"], 1);
            tab.paragraphMode = parseBoolOr(prefs[pfx + "paragraph_mode"], false);
            tab.parallelMode = parseBoolOr(prefs[pfx + "parallel_mode"], false);
            tab.parallelModules = splitCsv(prefs[pfx + "parallel_modules"]);
            tab.biblePaneWidth = parseIntOr(prefs[pfx + "bible_pane_w"], 0);
            tab.commentaryModule = prefs[pfx + "commentary_module"];
            tab.commentaryReference = prefs[pfx + "commentary_ref"];
            tab.dictionaryModule = prefs[pfx + "dictionary_module"];
            tab.dictionaryKey = prefs[pfx + "dictionary_key"];
            tab.dictionaryActive = parseBoolOr(prefs[pfx + "dictionary_active"], false);
            state.studyTabs.push_back(tab);
        }

        mainWindow_->restoreSessionState(state);
        return;
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
}

void VerdadApp::savePreferences() {
    std::string prefFile = getConfigDir() + "/preferences.conf";
    std::ofstream file(prefFile);
    if (!file.is_open()) return;

    file << "# Verdad preferences\n";

    if (mainWindow_) {
        MainWindow::SessionState state = mainWindow_->captureSessionState();
        file << "window_x=" << state.windowX << "\n";
        file << "window_y=" << state.windowY << "\n";
        file << "window_w=" << state.windowW << "\n";
        file << "window_h=" << state.windowH << "\n";
        file << "left_pane_w=" << state.leftPaneWidth << "\n";
        file << "left_pane_preview_h=" << state.leftPanePreviewHeight << "\n";
        file << "active_study_tab=" << state.activeStudyTab << "\n";
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
            file << pfx << "commentary_module=" << t.commentaryModule << "\n";
            file << pfx << "commentary_ref=" << t.commentaryReference << "\n";
            file << pfx << "dictionary_module=" << t.dictionaryModule << "\n";
            file << pfx << "dictionary_key=" << t.dictionaryKey << "\n";
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

} // namespace verdad
