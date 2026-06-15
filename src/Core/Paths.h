#pragma once

#include <string>

namespace dungeon::paths {

// Directory containing the running executable (no trailing slash).
const std::string& ExecutableDir();

// Resolves a path under the assets/ directory shipped next to the exe.
std::string Asset(const std::string& relative);

// The repo's source assets directory (compiled in for dev builds), so the
// in-game editor can persist edits back to the git tree; empty in a build with
// no source path baked in. No trailing slash.
const std::string& RepoAssetsDir();

// User's save directory: Documents\DungeonSaves (no trailing slash). The
// folder is NOT created here — the first WriteBinaryFile under it makes the
// path. Empty only if the known folder can't be resolved (never, in practice).
const std::string& SaveDir();

} // namespace dungeon::paths
