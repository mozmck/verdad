#include "sword/SwordPaths.h"
#include "app/PlatformPaths.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace verdad {
namespace {

namespace fs = std::filesystem;

bool looksLikeBundledSwordData(const fs::path& path) {
    if (path.empty()) return false;

    std::error_code ec;
    return fs::is_directory(path / "locales.d", ec) &&
           fs::exists(path / "mods.d" / "globals.conf", ec);
}

std::string envPath(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

std::string normalizedPathKey(const fs::path& path) {
    std::string key = path.lexically_normal().string();
#if defined(_WIN32)
    for (char& c : key) {
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
#endif
    return key;
}

void appendUniquePath(std::vector<fs::path>& paths, const fs::path& path) {
    if (path.empty()) return;

    const std::string key = normalizedPathKey(path);
    if (key.empty()) return;

    for (const auto& existing : paths) {
        if (normalizedPathKey(existing) == key) return;
    }

    paths.push_back(path.lexically_normal());
}

fs::path swordDataPathFromBase(const std::string& base) {
    if (base.empty()) return {};
    return fs::path(base) / ".sword";
}

fs::path swordDataPathWithoutDotFromBase(const std::string& base) {
    if (base.empty()) return {};
    return fs::path(base) / "sword";
}

fs::path macApplicationSupportSwordDataPath(const std::string& home) {
#if defined(__APPLE__)
    if (home.empty()) return {};
    return fs::path(home) / "Library" / "Application Support" / "Sword";
#else
    (void)home;
    return {};
#endif
}

std::vector<fs::path> candidateSwordDataDirs() {
    std::vector<fs::path> dirs;

    std::string exeDir = executableDir();
    if (!exeDir.empty()) {
        fs::path dir(exeDir);
#if defined(__APPLE__)
        // Inside .app bundle: Contents/MacOS/../Resources/sword
        dirs.push_back(dir.parent_path() / "Resources" / "sword");
#endif
        // FHS-like install or bundled layout
        dirs.push_back(dir.parent_path() / "share" / "sword");
#if defined(_WIN32)
        // Windows: sword data alongside the executable
        dirs.push_back(dir / "sword");
#endif
    }

#if defined(__APPLE__)
    dirs.push_back("/usr/local/share/sword");
    dirs.push_back("/opt/homebrew/share/sword");
#elif !defined(_WIN32)
    dirs.push_back("/usr/local/share/sword");
    dirs.push_back("/usr/share/sword");
#endif

    return dirs;
}

std::vector<fs::path> candidateUserSwordDataDirs() {
    std::vector<fs::path> dirs;

#if defined(_WIN32)
    appendUniquePath(dirs, swordDataPathFromBase(envPath("HOME")));
    appendUniquePath(dirs, swordDataPathFromBase(envPath("APPDATA")));
    appendUniquePath(dirs, swordDataPathFromBase(envPath("USERPROFILE")));
#elif defined(__APPLE__)
    const std::string home = envPath("HOME");
    appendUniquePath(dirs, swordDataPathFromBase(home));
    appendUniquePath(dirs, swordDataPathWithoutDotFromBase(home));
    appendUniquePath(dirs, macApplicationSupportSwordDataPath(home));
#else
    appendUniquePath(dirs, swordDataPathFromBase(envPath("HOME")));
#endif

    return dirs;
}

bool hasModsDir(const fs::path& path) {
    if (path.empty()) return false;

    std::error_code ec;
    return fs::is_directory(path / "mods.d", ec);
}

bool pathExists(const fs::path& path) {
    if (path.empty()) return false;

    std::error_code ec;
    return fs::is_directory(path, ec);
}

} // namespace

std::string bundledSwordDataPath() {
    for (const auto& path : candidateSwordDataDirs()) {
        if (looksLikeBundledSwordData(path)) {
            return path.string();
        }
    }

    return "";
}

std::string defaultUserSwordDataPath() {
    const auto candidates = candidateUserSwordDataDirs();

    for (const auto& path : candidates) {
        if (hasModsDir(path)) return path.string();
    }

    for (const auto& path : candidates) {
        if (pathExists(path)) return path.string();
    }

    return candidates.empty() ? std::string() : candidates.front().string();
}

std::vector<std::string> supplementalUserSwordDataPaths() {
    std::vector<std::string> paths;

#if defined(_WIN32)
    const fs::path homePath = swordDataPathFromBase(envPath("HOME"));
    const fs::path appDataPath = swordDataPathFromBase(envPath("APPDATA"));
    const fs::path userProfilePath = swordDataPathFromBase(envPath("USERPROFILE"));

    if (!userProfilePath.empty()) {
        const std::string userProfileKey = normalizedPathKey(userProfilePath);
        if (!userProfileKey.empty() &&
            userProfileKey != normalizedPathKey(homePath) &&
            userProfileKey != normalizedPathKey(appDataPath)) {
            paths.push_back(userProfilePath.lexically_normal().string());
        }
    }
#elif defined(__APPLE__)
    const std::string home = envPath("HOME");
    const fs::path dotSwordPath = swordDataPathFromBase(home);
    const fs::path plainSwordPath = swordDataPathWithoutDotFromBase(home);
    const fs::path appSupportPath = macApplicationSupportSwordDataPath(home);
    const std::string appSupportKey = normalizedPathKey(appSupportPath);

    if (!appSupportKey.empty() &&
        appSupportKey != normalizedPathKey(dotSwordPath) &&
        appSupportKey != normalizedPathKey(plainSwordPath)) {
        paths.push_back(appSupportPath.lexically_normal().string());
    }
#endif

    return paths;
}

} // namespace verdad
