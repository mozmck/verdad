#include "sword/SwordPaths.h"
#include "app/PlatformPaths.h"

#include <filesystem>
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

} // namespace

std::string bundledSwordDataPath() {
    for (const auto& path : candidateSwordDataDirs()) {
        if (looksLikeBundledSwordData(path)) {
            return path.string();
        }
    }

    return "";
}

} // namespace verdad
