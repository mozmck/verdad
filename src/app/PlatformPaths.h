#ifndef VERDAD_PLATFORM_PATHS_H
#define VERDAD_PLATFORM_PATHS_H

#include <string>

namespace verdad {

/// Return the absolute path of the currently running executable.
std::string executablePath();

/// Return the directory containing the currently running executable.
std::string executableDir();

} // namespace verdad

#endif // VERDAD_PLATFORM_PATHS_H
