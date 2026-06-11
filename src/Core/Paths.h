#pragma once

#include <string>

namespace dungeon::paths {

// Directory containing the running executable (no trailing slash).
const std::string& ExecutableDir();

// Resolves a path under the assets/ directory shipped next to the exe.
std::string Asset(const std::string& relative);

} // namespace dungeon::paths
