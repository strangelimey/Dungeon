// ============================================================================
// Core/Loc.cpp — see Loc.h.
// ============================================================================
#include "Core/Loc.h"

#include "Core/Log.h"

#include <algorithm>
#include <cstring>
#include <iterator>
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

size_t LogMissingKeys(const std::string& referencePath) {
	StringTable reference;
	if (!ParseFile(referencePath, reference)) return 0;
	std::vector<std::string_view> missing;
	for (const auto& [key, value] : reference)
		if (!g_table.contains(key)) missing.push_back(key);
	if (missing.empty()) return 0;
	std::ranges::sort(missing);
	std::string list;
	for (const std::string_view key : missing) {
		if (!list.empty()) list += ", ";
		list += key;
	}
	log::Warn("Language table is missing {} key(s) vs {}: {}", missing.size(),
			  referencePath, list);
	return missing.size();
}

std::string Tr(std::string_view key) { return std::string(View(key)); }

std::string_view View(std::string_view key) {
	const auto it = g_table.find(key);
	// A missing key yields the KEY itself. Every caller passes a string literal
	// or a table-owned id, so the view outlives the call either way.
	return it != g_table.end() ? std::string_view(it->second) : key;
}

Line ViewKey(std::string_view prefix, std::string_view id) {
	// Assemble the key in a stack buffer rather than with `+`. Clipped the same
	// way a Line is: a key too long to fit was never going to match an entry.
	char key[Line::kCapacity];
	const size_t pn = std::min(prefix.size(), Line::kCapacity);
	const size_t in = std::min(id.size(), Line::kCapacity - pn);
	std::memcpy(key, prefix.data(), pn);
	std::memcpy(key + pn, id.data(), in);
	return Line(View(std::string_view(key, pn + in)));
}

void Line::Assign(std::string_view text) {
	m_len = std::min(text.size(), kCapacity);
	std::memcpy(m_buf, text.data(), m_len);
	m_buf[m_len] = '\0';
}

std::string VFormat(std::string_view key, std::format_args args) {
	const std::string_view pattern = View(key);
	try {
		return std::vformat(pattern, args);
	} catch (const std::format_error&) {
		log::Warn("Bad format placeholders in language entry '{}'", key);
		return std::string(pattern);
	}
}

namespace {
// An output iterator that writes into a fixed buffer and DROPS anything past
// it. std::vformat_to is the only type-erased formatting sink the standard
// offers — format_to_n needs a compile-time format string, which a runtime
// language table cannot give it — so the clipping has to live in the iterator.
struct ClipIter {
	using iterator_category = std::output_iterator_tag;
	using value_type = void;
	using difference_type = std::ptrdiff_t;
	using pointer = void;
	using reference = void;

	char* buf = nullptr;
	size_t cap = 0;
	size_t* len = nullptr;

	ClipIter& operator=(char c) {
		if (*len < cap) buf[(*len)++] = c;
		return *this;
	}
	ClipIter& operator*() { return *this; }
	ClipIter& operator++() { return *this; }
	ClipIter operator++(int) { return *this; }
};
} // namespace

Line VFormatLine(std::string_view key, std::format_args args) {
	const std::string_view pattern = View(key);
	char buf[Line::kCapacity];
	size_t len = 0;
	Line out;
	try {
		std::vformat_to(ClipIter{buf, Line::kCapacity, &len}, pattern, args);
		out.Assign({buf, len});
	} catch (const std::format_error&) {
		log::Warn("Bad format placeholders in language entry '{}'", key);
		out.Assign(pattern);
	}
	return out;
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
