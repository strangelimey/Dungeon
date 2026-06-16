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

// Parses block-format text. Fields before the first "[id]" go into a block with
// an empty id. Whitespace around keys/values is trimmed. Malformed lines (no
// '=', no enclosing brackets) are skipped.
std::vector<Block> ParseBlocks(std::string_view text);

// Serializes blocks back to text: the unnamed block's fields first, then each
// "[id]" header with its "key = value" lines, a blank line between blocks.
std::string WriteBlocks(const std::vector<Block>& blocks);

} // namespace dungeon::game::serialize
