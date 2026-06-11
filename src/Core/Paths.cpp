#include "Core/Paths.h"

#include "Core/StringUtil.h"

#include <Windows.h>

#include <filesystem>

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

} // namespace dungeon::paths
