// ============================================================================
// Game/Catalog.h — a typed list of content definitions, one per category.
//
// A catalog is the data form of what used to be hardcoded in C++ (the
// procedural-decoration table, the monster type→model convention, the surface
// texture palette). Each CatalogEntry is a named record: a stable `id` (what
// levels reference) plus free-form key=value fields naming pool assets
// (assets/textures, assets/models) and parameters. The fields a category reads
// are documented at each catalog file (catalog/*.cat in a project); unknown
// fields are preserved verbatim across a load → save round-trip, so the editor
// and future versions can add fields without losing old ones.
//
// Catalogs are owned by a Project (Project.h) and serialize through the block
// format (Serialize.h).
// ============================================================================
#pragma once

#include "Game/Serialize.h"

#include <string>
#include <string_view>
#include <vector>

namespace dungeon::game {

// One content definition. `fields` is the raw record; the typed accessors read
// the conventional keys (display, mesh, texture, height_scale, solid, ...) and
// delegate to the shared serialize:: field helpers.
struct CatalogEntry {
	std::string id;
	// Comment lines that introduced this entry in the .cat, kept verbatim so an
	// editor write preserves the file's authoring notes (serialize::Block::lead).
	// An entry rebuilt from an existing one carries them along for free.
	std::vector<std::string> lead;
	std::vector<serialize::Field> fields;

	// Human-readable name (the "display" field, falling back to the id).
	std::string Display() const;
	std::string Get(std::string_view key, std::string_view fallback = {}) const {
		return serialize::Get(fields, key, fallback);
	}
	float GetFloat(std::string_view key, float fallback) const {
		return serialize::GetFloat(fields, key, fallback);
	}
	bool GetBool(std::string_view key, bool fallback) const {
		return serialize::GetBool(fields, key, fallback);
	}
	const std::string* Find(std::string_view key) const {
		return serialize::Find(fields, key);
	}
	void Set(std::string key, std::string value) {
		serialize::Set(fields, std::move(key), std::move(value));
	}
};

// Reads a field from a possibly-null catalog entry, falling back when the entry
// or the field is absent — collapses the "def ? def->Get(...) : fallback" idiom.
inline std::string CatalogGet(const CatalogEntry* e, std::string_view key,
							  std::string_view fallback) {
	return e ? e->Get(key, fallback) : std::string(fallback);
}
inline bool CatalogBool(const CatalogEntry* e, std::string_view key, bool fallback) {
	return e ? e->GetBool(key, fallback) : fallback;
}

// --- tags --------------------------------------------------------------------
// `tags` is a free-form, space-separated, case-insensitive set naming the WORLD
// an entry belongs to — `undead`, `stone`, `outdoor`. Distinct from `category`,
// which says what an entry IS (a weapon, a key) and groups the palette; a type
// can be a weapon in a stone dungeon, and one field cannot say both.
//
// Multi-valued deliberately: a mossy stone set is both `stone` and `outdoor`,
// and a single `theme` field would force a false choice at authoring time —
// the kind of schema decision that is painful to reverse once content carries it.
//
// Two consumers, which is why the field lives on every catalog rather than on
// the ones that need it first: the level generator picks from tag-matched sets,
// and the editor palette ranks on-theme types first.
//
// AN ABSENT `tags` MEANS "FITS ANYWHERE", NEVER "FITS NOTHING". Matching is a
// preference, never a gate — untagged content must stay usable, or every
// existing type would vanish from the palette the day a level picks a theme.
std::vector<std::string> ParseTags(std::string_view value);
// The entry's `tags`, parsed and lowercased; empty for a null entry.
std::vector<std::string> CatalogTags(const CatalogEntry* e);
// Does `e` carry any of `wanted`? An empty `wanted` — no theme picked — is true
// for everything, and so is an entry with no tags of its own (see above).
bool CatalogMatchesTags(const CatalogEntry* e, const std::vector<std::string>& wanted);

// An ordered set of entries with id lookup. Loading a missing file yields an
// empty catalog (a project need not define every category).
class Catalog {
public:
	Catalog() = default;

	// Reads the .cat file at `path`; a missing file leaves the catalog empty
	// (not an error — categories are optional). Malformed entries are skipped.
	void Load(const std::string& path);
	// Writes the catalog back to `path` (creates parent dirs). The header
	// comment names the category for hand-editors.
	bool Save(const std::string& path, std::string_view headerComment = {}) const;

	const CatalogEntry* Find(std::string_view id) const;
	bool Contains(std::string_view id) const { return Find(id) != nullptr; }

	const std::vector<CatalogEntry>& Entries() const { return m_entries; }
	bool Empty() const { return m_entries.empty(); }

	// Adds (or replaces, by id) an entry and returns it — the editor's create
	// path. Returns a reference stable only until the next Add/Remove.
	CatalogEntry& Add(CatalogEntry entry);
	void Remove(std::string_view id);
	// Renames an entry IN PLACE (the editor's type rename). Remove + Add would
	// move it to the end of the file, taking its lead comments — including a
	// first entry's, which is the file's header — along with it. False when
	// `id` is absent or `newId` is taken.
	bool Rename(std::string_view id, std::string newId);

private:
	std::vector<CatalogEntry> m_entries;
};

} // namespace dungeon::game
