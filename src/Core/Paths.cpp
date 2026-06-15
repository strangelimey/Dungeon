#include "Core/Paths.h"

#include "Core/StringUtil.h"

#include <Windows.h>
#include <ShlObj.h>

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

std::string Asset(const std::string& relative) {
	return ExecutableDir() + "\\assets\\" + relative;
}

const std::string& RepoAssetsDir() {
	static const std::string dir =
#ifdef DN_REPO_ASSETS
		DN_REPO_ASSETS;
#else
		"";
#endif
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
