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

private:
	std::vector<CatalogEntry> m_entries;
};

} // namespace dungeon::game
