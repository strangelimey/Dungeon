#include "Core/Paths.h"

#include "Core/StringUtil.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

namespace dungeon::paths {

const std::string& ExecutableDir() {
	static const std::string dir = [] {
		wchar_t buffer[MAX_PATH]{};
		GetModuleFileNameW(nullptr, buffer, MAX_PATH);
		return std::filesystem::path(buffer).parent_path().string();
	}();
	return dir;
}

const std::string& ExecutableName() {
	static const std::string name = [] {
		wchar_t buffer[MAX_PATH]{};
		GetModuleFileNameW(nullptr, buffer, MAX_PATH);
		std::string stem = std::filesystem::path(buffer).stem().string();
		// Lowercased so the game's log keeps the exact name every doc, script
		// and habit already spells — dungeon.log, not Dungeon.log. Windows
		// would treat them as the same file, but the listing would not match
		// what CLAUDE.md tells the next person to open.
		std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return stem;
	}();
	return name;
}

const std::string& AssetsDir() {
	static const std::string dir = [] {
		// A dev build runs straight out of the repo's assets tree: one copy for
		// every config, no post-build duplication, and an editor write lands
		// exactly where the next build reads from. The directory check is what
		// makes a PACKAGED exe work — copy assets\ beside it, leave the stale
		// build machine's path baked in, and this falls through to the copy.
#ifdef DN_ASSETS_DIR
		std::error_code ec;
		if (const std::string baked = DN_ASSETS_DIR;
			!baked.empty() && std::filesystem::is_directory(baked, ec)) {
			// CMake hands us forward slashes; normalise so paths logged (and
			// compared, see RepoAssetsDir) match the rest of the codebase.
			return std::filesystem::path(baked).make_preferred().string();
		}
#endif
		return ExecutableDir() + "\\assets";
	}();
	return dir;
}

std::string Asset(const std::string& relative) {
	return AssetsDir() + "\\" + relative;
}

const std::string& RepoAssetsDir() {
	static const std::string dir = [] {
#ifdef DN_REPO_ASSETS
		// Normalised like AssetsDir(), so the two compare equal by string when
		// they name the same directory (the usual dev-build case).
		if (const std::string baked = DN_REPO_ASSETS; !baked.empty())
			return std::filesystem::path(baked).make_preferred().string();
#endif
		return std::string();
	}();
	return dir;
}

const std::string& SaveDir() {
	static const std::string dir = [] {
		PWSTR path = nullptr;
		std::string result;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path)))
			result = std::filesystem::path(path).string() + "\\DungeonSaves";
		if (path) CoTaskMemFree(path);
		return result;
	}();
	return dir;
}

} // namespace dungeon::paths
