#include "app/VerdadApp.h"
#include "sword/SwordManager.h"
#include "tags/TagManager.h"
#include "bookmarks/BookmarkManager.h"
#include "ui/MainWindow.h"
#include "ui/BiblePane.h"

#include <FL/Fl.H>
#include <FL/Fl_File_Icon.H>
#include <FL/fl_ask.H>

#include <cstdlib>
#include <sys/stat.h>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#include <pwd.h>
#endif

namespace verdad {

VerdadApp* VerdadApp::instance_ = nullptr;

VerdadApp::VerdadApp()
    : swordMgr_(std::make_unique<SwordManager>())
    , tagMgr_(std::make_unique<TagManager>())
    , bookmarkMgr_(std::make_unique<BookmarkManager>()) {
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

    // Load bookmarks
    bookmarkMgr_->load(getConfigDir() + "/bookmarks.dat");

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

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        // Apply preferences
        if (key == "bible_module" && mainWindow_) {
            mainWindow_->biblePane()->setModule(value);
        }
    }
}

void VerdadApp::savePreferences() {
    std::string prefFile = getConfigDir() + "/preferences.conf";
    std::ofstream file(prefFile);
    if (!file.is_open()) return;

    file << "# Verdad preferences\n";

    if (mainWindow_ && mainWindow_->biblePane()) {
        file << "bible_module=" << mainWindow_->biblePane()->currentModule() << "\n";
        file << "bible_book=" << mainWindow_->biblePane()->currentBook() << "\n";
        file << "bible_chapter=" << mainWindow_->biblePane()->currentChapter() << "\n";
    }
}

} // namespace verdad
