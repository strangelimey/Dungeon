// ============================================================================
// Game/Project.h — a game built in the editor: its content catalogs and levels.
//
// A project is a folder under assets/projects/<name>/ holding DEFINITIONS and
// LEVELS, kept separate from the shared baked asset POOL (assets/textures,
// assets/models, worn_*, lang, shaders — the things AssetBaker emits). Catalog
// entries name pool assets + parameters; levels reference catalog ids. The
// editor reads and writes a project; the game loads one to play it.
//
//   assets/projects/<name>/
//     project.ini            manifest: name, level list, default fixture ids
//     catalog/*.cat          one Catalog per category (Catalog.h)
//     levels/<stem>.map+.ent  the levels (still parsed by DungeonMap/Entities)
//
// project.ini is block format (Serialize.h) with the manifest in the leading
// unnamed block:
//     name = Dungeon Demo
//     levels = level1               ; space-separated stems
//     default_sconce = sconce       ; catalog ids the 'T'/'F' glyphs resolve to
//     default_brazier = brazier
// ============================================================================
#pragma once

#include "Game/Catalog.h"

#include <string>
#include <vector>

namespace dungeon::game {

struct Project {
	std::string folder; // assets/projects/<name> (no trailing slash)
	std::string name;
	std::vector<std::string> levels;        // level stems, in menu order
	std::string defaultSconce = "sconce";   // 'T' glyph → this fixture id
	std::string defaultBrazier = "brazier"; // 'F' glyph → this fixture id

	// The content catalogs (see Catalog.h). Walls/floors/ceilings define the
	// surface palette; the rest define placeable content. attacks/balance are
	// the combat model's data (Balance.h): per-attack numbers + the knob sheet.
	Catalog walls, floors, ceilings;
	Catalog decorations, fixtures, monsters;
	Catalog doors, stairs, buttons, items, spells;
	Catalog attacks, balance;
	// Status effects (docs/effects.md): display name / icon / stacking per
	// effect id — numbers and look only. An effect's identity and behaviour
	// are its class (Game/Effect/), exactly like a spell's.
	Catalog effects;
	Catalog wallfeatures; // recessed wall niches (Phase 2)
	// Weapons and armor are ITEMS at runtime (placed as item entities, carried,
	// equipped) but authored in their own catalogs so their weapon/armor-only
	// settings don't clutter every other item. The split is purely
	// organizational: the runtime resolves an item id across all three via
	// FindItem / AllItems below, so nothing downstream cares which file it came
	// from.
	Catalog weapons, armor;
	// PROVENANCE, not content: one entry per asset the editor imported, keyed by
	// its pool name, recording where it came from and how. The baked assets
	// themselves are gitignored (assets/textures, assets/models), so without
	// this a type created in the editor reaches git as a catalog entry whose
	// asset a fresh clone cannot rebuild. tools/ReplayImports.ps1 re-runs them.
	// Deliberately absent from CatalogForKey — it is not a content category.
	Catalog imports;

	// The catalog for a kind key ("walls", "floors", "ceilings", "decorations",
	// "fixtures", "monsters", "doors", "stairs", "buttons", "items", "weapons",
	// "armor", "spells", "attacks", "balance"), or null if unknown.
	Catalog* CatalogForKey(const std::string& key);

	// Every CONTENT catalog, for sweeps that don't care which category a type is
	// in — the asset picker's "does anything bind this asset" check. `imports` is
	// provenance rather than content, so it stays out, as it does of
	// CatalogForKey.
	std::vector<const Catalog*> AllCatalogs() const;

	// --- item resolution across the three item catalogs ----------------------
	// An item id may live in items, weapons OR armor. These resolve/iterate
	// across all three (items first) so a placed weapon or worn armor loads the
	// same as any other item — the runtime never needs to know the split.
	const CatalogEntry* FindItem(std::string_view id) const;
	bool HasItem(std::string_view id) const { return FindItem(id) != nullptr; }
	std::vector<const CatalogEntry*> AllItems() const;

	// Loads the project rooted at `folder` (reads project.ini + catalog/*.cat).
	// A missing manifest or catalog is tolerated (empty), so a brand-new project
	// folder loads cleanly; the caller validates what it needs.
	static Project Load(const std::string& folder);
	// Writes the manifest and every catalog back to `folder`.
	bool Save() const;

	// Level file paths under the project (levels/<stem>.map / .ent).
	std::string LevelMapPath(const std::string& stem) const;
	std::string LevelEntPath(const std::string& stem) const;

	// catalog/<file> path under the project.
	std::string CatalogPath(const std::string& file) const;
};

} // namespace dungeon::game
