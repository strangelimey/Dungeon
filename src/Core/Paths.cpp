#include "Core/Paths.h"

#include "Core/StringUtil.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string_view>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

namespace dungeon::paths {

namespace {

// EVERY PATH HERE IS UTF-8, whatever the process code page (code-review C384).
// path::string() converts through the ANSI code page and THROWS on a character
// that page cannot hold - and ExecutableDir is first asked by crash::Install,
// before any handler is up to report the throw. The exe manifest
// (src/Core/Utf8CodePage.manifest) makes that page UTF-8 too, so the narrow
// file APIs read these strings back as written; this side does not lean on it.
std::string Utf8(const std::filesystem::path& p) { return str::Narrow(p.native()); }

// A UTF-8 string baked in by CMake (the source is compiled /utf-8), as a path.
std::filesystem::path FromUtf8(std::string_view s) {
	return std::filesystem::path(str::Widen(s));
}

// The running exe's full path, however long: MAX_PATH truncates silently.
std::filesystem::path ModulePath() {
	std::wstring buffer(MAX_PATH, L'\0');
	for (;;) {
		const DWORD n =
			GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (n < buffer.size()) {
			buffer.resize(n);
			return std::filesystem::path(buffer);
		}
		buffer.resize(buffer.size() * 2);
	}
}

struct CoTaskMemDeleter {
	void operator()(wchar_t* p) const { CoTaskMemFree(p); }
};

} // namespace

const std::string& ExecutableDir() {
	static const std::string dir = Utf8(ModulePath().parent_path());
	return dir;
}

const std::string& ExecutableName() {
	static const std::string name = [] {
		std::string stem = Utf8(ModulePath().stem());
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
		if (const std::filesystem::path baked = FromUtf8(DN_ASSETS_DIR);
			!baked.empty() && std::filesystem::is_directory(baked, ec)) {
			// CMake hands us forward slashes; normalise so paths logged (and
			// compared, see RepoAssetsDir) match the rest of the codebase.
			return Utf8(std::filesystem::path(baked).make_preferred());
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
		if (const std::filesystem::path baked = FromUtf8(DN_REPO_ASSETS); !baked.empty())
			return Utf8(std::filesystem::path(baked).make_preferred());
#endif
		return std::string();
	}();
	return dir;
}

const std::string& SaveDir() {
	static const std::string dir = [] {
		PWSTR raw = nullptr;
		const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &raw);
		// Owned at once: the shell allocates even on some failures.
		const std::unique_ptr<wchar_t, CoTaskMemDeleter> path(raw);
		if (FAILED(hr) || !path) return std::string();
		return Utf8(std::filesystem::path(path.get())) + "\\DungeonSaves";
	}();
	return dir;
}

} // namespace dungeon::paths
