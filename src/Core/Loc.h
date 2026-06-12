// ============================================================================
// Core/Loc.h — user-facing text localization.
//
// One language is active at a time, loaded from a UTF-8 key=value file
// (assets/lang/<code>.lang: ';' comments, one `key=text` per line). Tr()
// looks a key up in the active table; a missing key returns the key itself,
// so untranslated entries are visible in the UI instead of crashing.
// Format() is Tr() plus std::vformat for entries with {} placeholders — the
// format string comes from the file at runtime, so a malformed translation
// falls back to the raw pattern rather than throwing.
//
// Developer-facing text (log::, asset names, ini keys) stays English and
// does NOT go through here.
// ============================================================================
#pragma once

#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon::loc {

// Replaces the active table with the file's contents. Returns false (table
// left empty) when the file is missing or unreadable — callers fall back to
// the shipped English file.
bool LoadFile(const std::string& path);

// The active language's text for `key`, or the key itself when missing.
std::string Tr(std::string_view key);

// Tr() + std::vformat. Bad placeholder syntax in a translation returns the
// unformatted pattern instead of throwing.
std::string VFormat(std::string_view key, std::format_args args);

template <typename... Args>
std::string Format(std::string_view key, Args&&... args) {
	return VFormat(key, std::make_format_args(args...));
}

// One installed language: the file stem ("en") and its self-declared display
// name (the file's `lang.name` entry, falling back to the code).
struct LanguageInfo {
	std::string code;
	std::string name;
};

// Scans a directory for *.lang files, sorted by code. Used by the Settings
// page to populate the language dropdown.
std::vector<LanguageInfo> ScanLanguages(const std::string& dir);

} // namespace dungeon::loc
