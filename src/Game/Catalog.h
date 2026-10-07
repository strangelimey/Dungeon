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

#include "Core/MathTypes.h" // Vec4 (the shared "color" field)
#include "Game/Serialize.h"

#include <string>
#include <string_view>
#include <utility>
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
	// The same as a VIEW of the entry's own text, for a guarded frame (a quest
	// moving on as an item is lifted): Display() returns a copy.
	std::string_view DisplayView() const;
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
// Can a piece of this type be broken (decorations / fixtures / doors
// `breakable`)? OFF unless it says so. The key was `destructible` until
// 2026-10-03, and a catalog still spelling it that way is read the same -
// a world made before the rename must not quietly turn unbreakable.
inline bool CatalogBreakable(const CatalogEntry* e) {
	if (!e) return false;
	if (e->Find("breakable")) return e->GetBool("breakable", false);
	return e->GetBool("destructible", false);
}

// --- tags --------------------------------------------------------------------
// `tags` is a free-form, space-separated, case-insensitive set naming the WORLD
// an entry belongs to — `undead`, `stone`, `outdoor`. Distinct from `category`,
// which says what an entry IS (a weapon, a key) and groups the palette; a type
// can be a weapon in a stone dungeon, and one field cannot say both.
//
// Multi-valued deliberately: a mossy stone set is both `stone` and `outdoor`,
// and a single `tags` field would force a false choice at authoring time —
// the kind of schema decision that is painful to reverse once content carries it.
//
// Two consumers, which is why the field lives on every catalog rather than on
// the ones that need it first: the level generator picks from tag-matched sets,
// and the editor palette ranks on-tag types first.
//
// AN ABSENT `tags` MEANS "FITS ANYWHERE", NEVER "FITS NOTHING". Matching is a
// preference, never a gate — untagged content must stay usable, or every
// existing type would vanish from the palette the day a level picks tags.
std::vector<std::string> ParseTags(std::string_view value);
// An ID LIST - a dungeon's `levels`, a quest's `stages`, the manifest's level
// list - split the same way (whitespace and commas) but KEEPING CASE. An id is
// matched exactly and a level stem is a FILE NAME, so ParseTags' lowercasing
// is wrong for every one of them: a level renamed `Keep1` fell out of its own
// dungeon's list, a doorway onto it entered the dungeon's first level instead,
// and the checker reported errors that were not there (code-review C331).
// Every id list goes through this; nothing splits one by hand.
std::vector<std::string> SplitIds(std::string_view value);
// The entry's `tags`, parsed and lowercased; empty for a null entry.
std::vector<std::string> CatalogTags(const CatalogEntry* e);
// Does `e` carry any of `wanted`? An empty `wanted` — no tags picked — is true
// for everything, and so is an entry with no tags of its own (see above).
bool CatalogMatchesTags(const CatalogEntry* e, const std::vector<std::string>& wanted);

// --- the shared "color" field ------------------------------------------------
// "r,g,b[,a]", floats 0..1, whitespace- and/or comma-separated. Malformed or
// absent leaves `out` untouched and returns false, so a caller's default stands.
//
// It lives HERE rather than in whichever loader wanted it first because the
// FIELD is a catalog convention: decoration tints, prop material colours and
// world terrain inks all spell it the same way, and a second parser is a second
// grammar waiting to disagree with the first.
bool CatalogColor(const CatalogEntry* e, std::string_view key, Vec4& out);

// An ordered set of entries with id lookup. Loading a missing file yields an
// empty catalog (a project need not define every category).
class Catalog {
public:
	Catalog() = default;

	// Reads the .cat file at `path`; a missing file leaves the catalog empty
	// (not an error — categories are optional). Malformed entries are skipped.
	void Load(const std::string& path);
	// The same from text already in hand - what `catround`'s cases build their
	// catalogs from, so the writer is checked on files no project has to carry.
	void LoadText(std::string_view text);
	// The catalog as it would be WRITTEN, touching no file. Pure, so a caller
	// can diff it against what is on disk — which is how the writer's fidelity
	// is checked (the dev console's `catround`) rather than trusted. The same
	// shape WorldMap::Serialize takes, and for the same reason.
	std::string Serialize(std::string_view headerComment = {}) const;
	// Writes the catalog back to `path` (creates parent dirs). The header
	// comment names the category for hand-editors.
	bool Save(const std::string& path, std::string_view headerComment = {}) const;

	const CatalogEntry* Find(std::string_view id) const;
	// Mutable overload, for an editor path that changes ONE field of an entry
	// that already exists (a new level joining its dungeon's `levels` list).
	// Delegates, so the lookup itself stays in one place. Like Add's return, it
	// is valid only until the next Add/Remove.
	CatalogEntry* Find(std::string_view id) {
		return const_cast<CatalogEntry*>(std::as_const(*this).Find(id));
	}
	bool Contains(std::string_view id) const { return Find(id) != nullptr; }

	const std::vector<CatalogEntry>& Entries() const { return m_entries; }
	bool Empty() const { return m_entries.empty(); }

	// Adds (or replaces, by id) an entry and returns it — the editor's create
	// path. Returns a reference stable only until the next Add/Remove. The first
	// entry of a header-only file takes that header as its lead (see m_trailer).
	CatalogEntry& Add(CatalogEntry entry);
	// Drops an entry and its lead comments - EXCEPT the first entry's, which
	// are the file's header and pass to whatever is first after it (or to the
	// trailer, when nothing is left).
	void Remove(std::string_view id);
	// Drops EVERY entry and keeps the file's header: the first entry's lead, or
	// in a catalog with no entry its trailer - what Serialize writes as the
	// file's own header. Every other comment goes with the entries (a new world
	// clearing a source world's places, Game_NewWorld.cpp ClearPlaces).
	void ClearEntries();
	// Renames an entry IN PLACE (the editor's type rename). Remove + Add would
	// move it to the end of the file, taking its lead comments — including a
	// first entry's, which is the file's header — along with it. False when
	// `id` is absent or `newId` is taken.
	bool Rename(std::string_view id, std::string newId);

private:
	std::vector<CatalogEntry> m_entries;
	// Comment lines after the last entry (serialize::ParseBlocks' trailer). In a
	// catalog with no entries yet - the template's flags.cat, quests.cat and
	// dungeons.cat - that is the file's whole documentation, which every write
	// used to drop (code-review C323).
	std::vector<std::string> m_trailer;
};

} // namespace dungeon::game
