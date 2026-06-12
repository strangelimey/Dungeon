// ============================================================================
// Core/Loc.cpp — see Loc.h.
// ============================================================================
#include "Core/Loc.h"

#include "Core/Log.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace dungeon::loc {

namespace {

// Heterogeneous lookup so Tr(string_view) never allocates a temporary key.
struct StringHash {
	using is_transparent = void;
	size_t operator()(std::string_view s) const {
		return std::hash<std::string_view>{}(s);
	}
};
using StringTable =
	std::unordered_map<std::string, std::string, StringHash, std::equal_to<>>;

StringTable g_table;

std::string_view Trim(std::string_view s) {
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
	while (!s.empty() &&
		   (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
		s.remove_suffix(1);
	return s;
}

// Parses `key=text` lines into `out` (';' comments, blank lines skipped).
// Returns false when the file can't be opened.
bool ParseFile(const std::string& path, StringTable& out) {
	std::ifstream file(path, std::ios::binary);
	if (!file) return false;
	std::string line;
	while (std::getline(file, line)) {
		const std::string_view trimmed = Trim(line);
		if (trimmed.empty() || trimmed.front() == ';') continue;
		const size_t eq = trimmed.find('=');
		if (eq == std::string_view::npos) continue;
		const std::string_view key = Trim(trimmed.substr(0, eq));
		const std::string_view value = Trim(trimmed.substr(eq + 1));
		if (!key.empty()) out[std::string(key)] = std::string(value);
	}
	return true;
}

} // namespace

bool LoadFile(const std::string& path) {
	StringTable table;
	if (!ParseFile(path, table)) {
		log::Warn("Language file not found: {}", path);
		return false;
	}
	g_table = std::move(table);
	log::Info("Loaded {} strings from {}", g_table.size(), path);
	return true;
}

std::string Tr(std::string_view key) {
	const auto it = g_table.find(key);
	return it != g_table.end() ? it->second : std::string(key);
}

std::string VFormat(std::string_view key, std::format_args args) {
	const std::string pattern = Tr(key);
	try {
		return std::vformat(pattern, args);
	} catch (const std::format_error&) {
		log::Warn("Bad format placeholders in language entry '{}'", key);
		return pattern;
	}
}

std::vector<LanguageInfo> ScanLanguages(const std::string& dir) {
	std::vector<LanguageInfo> languages;
	std::error_code ec; // missing dir = no languages, not an exception
	for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".lang")
			continue;
		LanguageInfo info;
		info.code = entry.path().stem().string();
		StringTable table;
		if (!ParseFile(entry.path().string(), table)) continue;
		const auto name = table.find(std::string_view("lang.name"));
		info.name = name != table.end() ? name->second : info.code;
		languages.push_back(std::move(info));
	}
	std::ranges::sort(languages, {}, &LanguageInfo::code);
	return languages;
}

} // namespace dungeon::loc
