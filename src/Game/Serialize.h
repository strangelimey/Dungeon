// ============================================================================
// Game/Serialize.h — the block text format shared by the project file and the
// content catalogs.
//
// A tiny line-based format, in the same spirit as the .map/.ini files already
// in the tree (UTF-8, ';' comments). It groups "key = value" fields under
// "[id]" headers:
//
//   ; a comment
//   key = value            ; fields before the first [id] form the unnamed
//   [some_id]              ; block (id == ""); the manifest uses this
//   display = Some Thing
//   height_scale = 0.055
//
// ParseBlocks turns text into ordered Blocks (order preserved so a load → save
// round-trip is stable); WriteBlocks turns them back. Values run to end of line
// (trimmed); ';' only starts a comment at the start of a trimmed line, so a
// value may not contain one. This is the single (de)serialization primitive for
// Catalog and Project.
// ============================================================================
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace dungeon::game::serialize {

struct Field {
	std::string key;
	std::string value;
	// Comment lines that preceded this field, verbatim. A comment inside a block
	// annotates the field under it (monsters.cat explains a threat_threshold that
	// way), so it has to travel with that field — attaching it to the block would
	// float it to the top on the next write, describing the wrong thing.
	std::vector<std::string> lead;
};

// Field accessors over a raw field list — the one implementation shared by
// serialize::Block and game::CatalogEntry (both are an id + a Field vector).
const std::string* Find(const std::vector<Field>& fields, std::string_view key);
std::string Get(const std::vector<Field>& fields, std::string_view key,
				std::string_view fallback = {});
float GetFloat(const std::vector<Field>& fields, std::string_view key, float fallback);
bool GetBool(const std::vector<Field>& fields, std::string_view key, bool fallback);
// Replaces the field's value if `key` exists, else appends it.
void Set(std::vector<Field>& fields, std::string key, std::string value);

struct Block {
	std::string id; // "" for the leading unnamed block (the manifest)
	// Comment lines that preceded this block's "[id]" header, verbatim (';'
	// included). Carried so a load → save round-trip keeps the file's authoring
	// notes — the catalogs document their own fields at the top, and an editor
	// write used to delete that documentation.
	std::vector<std::string> lead;
	std::vector<Field> fields;

	const std::string* Find(std::string_view key) const {
		return serialize::Find(fields, key);
	}
	std::string Get(std::string_view key, std::string_view fallback = {}) const {
		return serialize::Get(fields, key, fallback);
	}
	void Set(std::string key, std::string value) {
		serialize::Set(fields, std::move(key), std::move(value));
	}
};

// The line ending every block-format file is WRITTEN with. READING accepts
// either — ParseBlocks splits on '\n' and Trim eats the '\r' — so this governs
// only what lands on disk, and it is CRLF because these are Windows-side
// authored files a person opens in an editor beside the game's own .ini and
// .log. (2026-08-11: the catalogs had drifted to 19 LF files and one CRLF,
// because the writer emitted '\n' while one file had arrived from a Python
// tool. Normalising the files alone would not have held — the next editor save
// rewrites a whole .cat, so consistency has to come from the WRITER.)
//
// It lives HERE, in the header, because three writers emit into these same
// files — WriteBlocks plus the .cat and manifest header lines their callers
// prepend — and a disagreement between them would give one file two kinds of
// line, which is worse than the drift it was meant to fix.
inline constexpr std::string_view kEol = "\r\n";

// Parses block-format text. Fields before the first "[id]" go into a block with
// an empty id. Whitespace around keys/values is trimmed. Malformed lines (no
// '=', no enclosing brackets) are skipped. Tolerant of either line ending.
std::vector<Block> ParseBlocks(std::string_view text);

// Serializes blocks back to text: the unnamed block's fields first, then each
// "[id]" header with its "key = value" lines, a blank line between blocks.
// Lines end with kEol.
std::string WriteBlocks(const std::vector<Block>& blocks);

} // namespace dungeon::game::serialize
