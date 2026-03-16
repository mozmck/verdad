#include "app/PlatformPaths.h"

#include <filesystem>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#elif defined(__linux__)
#include <climits>
#include <unistd.h>
#endif

namespace verdad {

std::string executablePath() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::string(buf, len);
    }
    return "";
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        auto resolved = std::filesystem::canonical(buf, ec);
        if (!ec) return resolved.string();
        return std::string(buf);
    }
    return "";
#elif defined(__linux__)
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return std::string(buf);
    }
    return "";
#else
    return "";
#endif
}

std::string executableDir() {
    std::string path = executablePath();
    if (path.empty()) return "";
    return std::filesystem::path(path).parent_path().string();
}

} // namespace verdad
