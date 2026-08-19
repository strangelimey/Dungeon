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

// Compares the active table against a reference file (en.lang) and logs one
// warning listing every key the reference has that the table lacks — those
// keys render raw in the UI, so this makes translation drift visible at load
// instead of during play. Returns the missing count (0 = in sync or the
// reference file is unreadable).
size_t LogMissingKeys(const std::string& referencePath);

// The active language's text for `key`, or the key itself when missing.
//
// COPIES. The table already owns this text, so a caller that only reads it
// wants View() below — this exists for the callers that genuinely take
// ownership (a widget storing its label, a string being concatenated).
std::string Tr(std::string_view key);

// The same lookup WITHOUT the copy: a view of the table's own storage.
//
// This is the one that should be reached for by default. Tr() returning by
// value meant every lookup allocated — 365 call sites of it — and during
// settled gameplay the only thing calling it is a message, which is why the
// steady-state allocation guard spent its life reporting bump messages and
// being told they were fine. They were not fine; the copy was never needed.
//
// Valid until the language table is reloaded (a language switch), which
// rebuilds every UI string anyway.
//
// CAVEAT, which ViewKey (below the Line class) exists to handle: a MISSING key
// is returned AS ITSELF, so in that case the result borrows the KEY's storage
// rather than the table's. That is safe for the string literals this is
// normally handed; it is not safe for a key assembled on the fly.
std::string_view View(std::string_view key);

// A formatted line, held INLINE — no heap, no lifetime rules, cheap to pass.
//
// Message text is short and bounded, so it does not need an allocation to
// exist. Anything longer than the buffer is truncated rather than growing:
// a log line that runs past 256 characters is a bug in the line, and silently
// costing an allocation to print it is worse than clipping it.
class Line {
public:
	Line() = default;
	Line(std::string_view text) { Assign(text); }
	std::string_view View() const { return {m_buf, m_len}; }
	operator std::string_view() const { return View(); }
	const char* c_str() const { return m_buf; }
	size_t size() const { return m_len; }
	bool empty() const { return m_len == 0; }
	// Writes `text`, clipped to capacity. Public because VFormat fills one.
	void Assign(std::string_view text);
	static constexpr size_t kCapacity = 255;

private:
	char m_buf[kCapacity + 1] = {};
	size_t m_len = 0;
};

// The lookup for a key built from a fixed PREFIX and a runtime id — the
// convention dynamic ids follow (monster.<type>, item.<id>, skill.<id>,
// stat.<id>; see CLAUDE.md's Core/Loc notes).
//
// Composing such a key with `+` allocated a std::string every time, and that is
// most of what combat says out loud. This assembles it in a stack buffer
// instead. Returning BY VALUE also closes View()'s caveat above: when the
// translation is missing the key comes back as the text, and a view of a
// concatenated temporary would dangle the moment a caller held on to it.
Line ViewKey(std::string_view prefix, std::string_view id);

// Tr() + std::vformat. Bad placeholder syntax in a translation returns the
// unformatted pattern instead of throwing.
//
// ALLOCATES, like Tr — and for the same reason it still exists: the callers
// that keep the result (a widget label, a dialog title) want a string. The
// message path uses FormatLine below and allocates nothing. Migrating the rest
// is the other half of this job.
std::string VFormat(std::string_view key, std::format_args args);

template <typename... Args>
std::string Format(std::string_view key, Args&&... args) {
	return VFormat(key, std::make_format_args(args...));
}

// View() + vformat into a Line: the same text, no allocation. What anything on
// the MESSAGE path should call.
Line VFormatLine(std::string_view key, std::format_args args);

template <typename... Args>
Line FormatLine(std::string_view key, Args&&... args) {
	return VFormatLine(key, std::make_format_args(args...));
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

// Lets a Line be a format ARGUMENT — FormatLine("log.x", ViewKey("monster.", t)).
// Needed explicitly: std::make_format_args resolves a formatter for the static
// type, so Line's implicit conversion to string_view never gets a look in.
template <>
struct std::formatter<dungeon::loc::Line> : std::formatter<std::string_view> {
	template <typename Ctx>
	auto format(const dungeon::loc::Line& line, Ctx& ctx) const {
		return std::formatter<std::string_view>::format(line.View(), ctx);
	}
};
