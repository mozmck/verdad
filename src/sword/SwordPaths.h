#ifndef VERDAD_SWORD_PATHS_H
#define VERDAD_SWORD_PATHS_H

#include <string>
#include <vector>

namespace verdad {

std::string bundledSwordDataPath();
std::string defaultUserSwordDataPath();
std::vector<std::string> supplementalUserSwordDataPaths();
std::vector<std::string> allUserSwordDataPaths();

} // namespace verdad

#endif
