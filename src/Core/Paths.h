#pragma once

#include <string>

namespace dungeon::paths {

// Directory containing the running executable (no trailing slash). Per-config
// runtime files live here — dungeon.log, settings.ini, shadercache/ — since
// each build config wants its own.
const std::string& ExecutableDir();

// The one assets directory this build reads AND writes (no trailing slash).
// A dev build resolves to the repo's source tree (DN_ASSETS_DIR), so every
// config — debug, release, vs, any future profile build — shares a SINGLE copy
// rather than each linking its own 14 GB duplicate. A packaged build, where
// that path doesn't exist, falls back to assets\ beside the exe.
const std::string& AssetsDir();

// Resolves a path under AssetsDir().
std::string Asset(const std::string& relative);

// The repo's source assets directory (compiled in for dev builds), so the
// in-game editor can persist edits back to the git tree; empty in a build with
// no source path baked in. No trailing slash. In a dev build this is the SAME
// directory as AssetsDir() — the game already runs from the source tree, so
// there is nothing to copy back and SyncProjectToSource says so.
const std::string& RepoAssetsDir();

// User's save directory: Documents\DungeonSaves (no trailing slash). The
// folder is NOT created here — the first WriteBinaryFile under it makes the
// path. Empty only if the known folder can't be resolved (never, in practice).
const std::string& SaveDir();

} // namespace dungeon::paths
