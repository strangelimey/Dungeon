// ============================================================================
// Game/WorldMap.h — the overworld: terrain, locations, areas (docs/world-map.md).
//
// The world is a level, one tier up (Michael's own framing: "like a level,
// there's content and then a diff for the save"). This is the STATIC half —
// what ships with the project and never changes at runtime. Discovery, the
// party's world position and quest state are dynamic and live in the save, the
// same split `seen` makes for a dungeon.
//
// There is exactly ONE world per project, which is why terrain needs no
// per-level palette record the way surfaces do: a terrain kind declares its own
// `glyph` in terrain.cat and the grid is read through that. It is worth knowing
// why that differs from a level, because the level side is a standing trap —
// a level's `variant` records store the palette INDEX, so the palette is
// append-only forever and inserting an entry would silently repaint every cell
// above it. Glyphs carry no ordering, so reordering or renaming terrain cannot
// quietly rewrite the world.
//
// FREE OF Project, deliberately, exactly as DungeonMap is: what the map needs
// to know from the catalogs arrives as a resolved TerrainRules, passed in by
// the caller. So this parses and answers questions, and can be tested without
// a project, a device or a game.
//
// ASK IT QUESTIONS, DO NOT INDEX ITS CELLS. Callers ask what is at a position,
// what travel there costs, how dangerous it is — never for the grid itself.
// The grid is an implementation detail the design doc deliberately keeps
// replaceable (docs/world-map.md "The grid").
//
//   ; the overworld.
//   start 6 12
//   area lowlands 0 8 14 10 difficulty=0.15
//   location dungeon crypt 14 9
//   ;
//   ^^^MMM...            <- one glyph per terrain kind
//
// Records are lowercase and grid rows are not, the same rule the .map files use
// (Entity.h ReadLevelLines / SplitRecordTokens do the shared work).
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dungeon::game {

class WorldMap {
public:
	// One terrain kind, resolved from terrain.cat by the caller. `glyph` is
	// what stands for it in the grid; `travel` is hours to cross one cell;
	// `difficulty` is the 0..1 danger an area may override and an encounter
	// reads. Tags name the content pool a generated encounter draws from.
	struct Terrain {
		std::string id;
		char glyph = '?';
		bool passable = true;
		float travel = 1.0f;
		float difficulty = 0.0f;
		std::vector<std::string> tags;
	};
	using TerrainRules = std::vector<Terrain>;

	// Something standing on a world cell that the party can reach. `kind` is
	// "dungeon" today ("town" later); `id` names the entry in that kind's
	// catalog — a DUNGEON id, never a level stem, which is the tier boundary
	// made syntactic.
	struct Location {
		std::string kind;
		std::string id;
		int x = 0, z = 0;
		std::vector<std::pair<std::string, std::string>> params;

		const std::string* Param(std::string_view key) const;
	};

	// A named rectangle that overrides its terrain's difficulty. Optional —
	// terrain alone is enough for a plain world. Areas are tested in FILE
	// ORDER and the LAST match wins, so a broad region can be authored first
	// and exceptions carved out after it.
	struct Area {
		std::string id;
		int x = 0, z = 0, w = 0, h = 0;
		float difficulty = -1.0f; // < 0 = unset: the terrain's own stands

		bool Contains(int cx, int cz) const {
			return cx >= x && cz >= z && cx < x + w && cz < z + h;
		}
	};

	// Reads `path`. A MISSING file yields nullopt — a project need not have a
	// world yet, the same tolerance a missing catalog gets. Malformed CONTENT
	// is fatal with a clear message, exactly as a bad .map is: a world that
	// half-parsed would be worse than none.
	static std::optional<WorldMap> Load(const std::string& path,
										TerrainRules terrain);

	int Width() const { return m_width; }
	int Height() const { return m_height; }
	bool InBounds(int x, int z) const {
		return x >= 0 && z >= 0 && x < m_width && z < m_height;
	}

	// The terrain of a cell. Out of bounds returns the impassable fallback, so
	// callers can ask about any coordinate without guarding first.
	const Terrain& TerrainAt(int x, int z) const;
	bool Passable(int x, int z) const { return TerrainAt(x, z).passable; }
	float TravelHours(int x, int z) const { return TerrainAt(x, z).travel; }
	// The cell's danger: its area's override if one covers it, else its
	// terrain's own.
	float Difficulty(int x, int z) const;

	const std::vector<Terrain>& Terrains() const { return m_terrain; }
	const std::vector<Location>& Locations() const { return m_locations; }
	const std::vector<Area>& Areas() const { return m_areas; }
	// The location on a cell, or null. At most one stands on a cell.
	const Location* LocationAt(int x, int z) const;
	// The area covering a cell (last match wins), or null.
	const Area* AreaAt(int x, int z) const;

	// Where a new game puts the party. Authored by the `start` record.
	int StartX() const { return m_startX; }
	int StartZ() const { return m_startZ; }

private:
	WorldMap() = default;

	void ParseLocationRecord(const std::string& record, const std::string& path);
	void ParseAreaRecord(const std::string& record, const std::string& path);
	void ParseStartRecord(const std::string& record, const std::string& path);

	int m_width = 0, m_height = 0;
	std::vector<u8> m_cells; // index into m_terrain, row-major
	TerrainRules m_terrain;
	std::vector<Location> m_locations;
	std::vector<Area> m_areas;
	int m_startX = 0, m_startZ = 0;
};

} // namespace dungeon::game
