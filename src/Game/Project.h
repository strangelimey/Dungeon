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

	// WHERE THE GAME BEGINS (docs/world-map.md). Empty `startDungeon` means the
	// world map — the ordinary opening. Naming a dungeon instead starts the
	// party inside it, at the level and cell given here.
	//
	// THE COORDINATES ARE HERE AND NOT IN THE DUNGEON, because a dungeon has no
	// start of its own any more (Michael, 2026-09-09): every way in says where
	// it leads, and the game's opening is just another way in — one that
	// belongs to the GAME rather than to a door on the map.
	std::string startDungeon; // empty = begin on the world map
	std::string startLevel;
	int startX = -1, startZ = -1; // -1,-1 = the level's own start cell

	// Where the EVAL HARNESS puts the party when it asks for a level rather
	// than the world map. Named here rather than "whichever level is first",
	// because the suites must not move when the level list is reordered — and
	// because there will be several harness levels, not one.
	std::string evalLevel;

	// THE MANIFEST AS IT WAS READ, comments and all. Save updates the fields
	// above INSIDE it rather than building a fresh block, for the reason the
	// catalogs keep their lead comments: project.ini is hand-authored
	// documentation — what the level list is, why eval_arena is on it, what
	// removing the four opening lines does — and an editor write used to delete
	// every word of it. (Found the day the [+] button started saving the
	// project routinely; before that it happened rarely enough to go unnoticed.)
	// Unknown keys survive for the same reason they do in a catalog.
	serialize::Block manifest;

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
	// The damage types themselves (docs/damage-system.md). Unlike every other
	// catalog here this one is not content ON a system — it IS the vocabulary
	// the combat maths is written in, so C++ names none of its entries and
	// resists elsewhere are authored against its ids.
	Catalog damagetypes;
	// The WORLD tier (docs/world-map.md). `terrain` is what a world-map cell IS
	// — the exact analogue of the wall/floor/ceiling surface catalogs, except
	// that with one world per project a kind declares its own grid `glyph`
	// instead of taking a per-level palette slot. `dungeons` is the container
	// that did not exist before: a named group of level stems with an entry
	// level, which `levels` below still lists flat.
	Catalog terrain, dungeons;
	// Quest DEFINITIONS: a display name and an ordered stage list. The party's
	// progress is save state (WorldState::quests), not content — a catalog says
	// what a quest IS, never where anyone has got to in it.
	Catalog quests;
	Catalog wallfeatures; // recessed wall niches (Phase 2)
	// The same idea laid flat, pointing down or up: a tile stamped IN PLACE OF a
	// cell's FLOOR or CEILING block, carrying a recess sunk into it or a vault
	// raised out of it. ONE catalog for both, with a `surface = floor|ceiling`
	// field, because they differ in exactly that one bit — a second catalog would
	// have been a near-copy of this one. Separate from wallfeatures because the
	// record has no facing (a floor has one orientation) and the mesh must match
	// its block's extent and UVs, not a wall panel's.
	Catalog surfacefeatures;
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
	// Const overload, for the read-only paths (the placement resolver runs on a
	// const Project every frame from the hover). Delegates, so the key table
	// stays in one place.
	const Catalog* CatalogForKey(const std::string& key) const {
		return const_cast<Project*>(this)->CatalogForKey(key);
	}

	// Every CONTENT catalog, for sweeps that don't care which category a type is
	// in — the asset picker's "does anything bind this asset" check. `imports` is
	// provenance rather than content, so it stays out, as it does of
	// CatalogForKey.
	std::vector<const Catalog*> AllCatalogs() const;

	// The catalog FILES this project is made of: the filename, the catalog it
	// loads into, and the header comment its writer prepends. The SAME table
	// Load and Save walk, exposed so a check can walk it too — a second list
	// would be a second chance to forget a file.
	struct CatalogFile {
		const char* file;
		const Catalog* catalog;
		const char* header;
	};
	std::vector<CatalogFile> CatalogFiles() const;

	// --- item resolution across the three item catalogs ----------------------
	// An item id may live in items, weapons OR armor. These resolve/iterate
	// across all three (items first) so a placed weapon or worn armor loads the
	// same as any other item — the runtime never needs to know the split.
	const CatalogEntry* FindItem(std::string_view id) const;
	bool HasItem(std::string_view id) const { return FindItem(id) != nullptr; }
	std::vector<const CatalogEntry*> AllItems() const;

	// --- the dungeon tier (docs/world-map.md, W5) ----------------------------
	// `levels` above stays the FLAT UNIVERSE — every stem the editor can open
	// and the checker walks. These are how that universe is PRESENTED: a
	// dungeon claims a set of stems in its `levels` field, and the editor shows
	// them grouped by it. They live here rather than being worked out at each
	// call site because the level picker, the world-settings dialog and the
	// checker adapter all ask the same question, and three answers could
	// disagree about which dungeon a level belongs to.
	//
	// The levels a dungeon claims, IN ITS OWN ORDER (the author's), filtered to
	// the stems that actually exist — a dungeon naming a deleted level is a
	// checker finding, not a row the picker should offer to open.
	std::vector<std::string> DungeonLevels(std::string_view dungeonId) const;
	// The dungeon claiming `stem`, or null. FIRST claim wins: two dungeons
	// naming one level is a checker error, and answering it twice here would
	// list the level twice.
	const CatalogEntry* DungeonOfLevel(std::string_view stem) const;
	// Stems NO dungeon claims, in manifest order. The checker warns about them,
	// and the editor must still be able to REACH them — a level that became
	// uneditable by being forgotten is a worse outcome than an untidy list.
	std::vector<std::string> OrphanLevels() const;

	// --- one project per WORLD (W7) -------------------------------------
	// Michael's word for a project is a WORLD, and after W6 that is what one
	// is: a self-contained game — its own overworld, dungeons, levels and
	// content. Several can sit side by side under assets/projects/ and the
	// game opens one of them, which is what lets a test scenario be a world
	// of its own rather than a corner of the demo.
	//
	// Every project folder under `root` that carries a project.ini, by
	// folder name, sorted. A folder without one is not a project — it is
	// some other thing that happens to live there.
	static std::vector<std::string> List(const std::string& root);
	// The folder a project name lives in (no trailing slash).
	static std::string FolderFor(const std::string& root, const std::string& name);
	// The name of the folder this project was LOADED from — what `List`
	// returns and what settings.ini stores. Read it rather than the setting
	// when reporting which world is open: a `-project` run deliberately
	// leaves the setting alone, so the two disagree by design.
	std::string FolderName() const;

	// Loads the project rooted at `folder` (reads project.ini + catalog/*.cat).
	// A missing manifest or catalog is tolerated (empty), so a brand-new project
	// folder loads cleanly; the caller validates what it needs.
	static Project Load(const std::string& folder);
	// project.ini as it would be WRITTEN, touching no file — the pure half of
	// Save, so the writer's fidelity can be DIFFED against what is on disk
	// (`catround`) instead of trusted. Catalog::Serialize is its twin.
	std::string ManifestText() const;
	// Writes the manifest and every catalog back to `folder`.
	bool Save() const;

	// The world map (world/world.map). One per project, and optional — a
	// project without one simply has no overworld yet.
	std::string WorldMapPath() const;

	// Level file paths under the project (levels/<stem>.map / .ent).
	std::string LevelMapPath(const std::string& stem) const;
	std::string LevelEntPath(const std::string& stem) const;

	// catalog/<file> path under the project.
	std::string CatalogPath(const std::string& file) const;
};

} // namespace dungeon::game
