#include "sword/SwordPaths.h"

#include <filesystem>
#include <vector>

#ifdef __linux__
#include <limits.h>
#include <unistd.h>
#endif

namespace verdad {
namespace {

bool looksLikeBundledSwordData(const std::filesystem::path& path) {
    if (path.empty()) return false;

    std::error_code ec;
    return std::filesystem::is_directory(path / "locales.d", ec) &&
           std::filesystem::exists(path / "mods.d" / "globals.conf", ec);
}

std::vector<std::filesystem::path> candidateSwordDataDirs() {
    std::vector<std::filesystem::path> dirs;

#ifdef __linux__
    char exePath[PATH_MAX];
    ssize_t pathLen = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (pathLen > 0) {
        exePath[pathLen] = '\0';
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        dirs.push_back(exeDir.parent_path() / "share" / "sword");
    }
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
