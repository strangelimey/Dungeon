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
	// surface palette; the rest define placeable content. doors/stairs/items are
	// declared now (P0) but populated in later phases.
	Catalog walls, floors, ceilings;
	Catalog decorations, fixtures, monsters;
	Catalog doors, stairs, items, spells;

	// The catalog for a kind key ("walls", "floors", "ceilings", "decorations",
	// "fixtures", "monsters", "doors", "stairs", "items", "spells"), or null if
	// unknown.
	Catalog* CatalogForKey(const std::string& key);

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
