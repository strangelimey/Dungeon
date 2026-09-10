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

#include "Core/MathTypes.h" // Vec4
#include "Core/Types.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dungeon::game {

// The DYNAMIC half of the world — the save-side twin of the WorldMap below,
// and the same split every level already makes: the map is authored and never
// changes, this is everything play does to it.
//
// ONE TYPE, used as both the runtime truth (Game holds one) and the save record
// (SaveData holds one). A parallel pair would need conversion code at two
// points, and conversion code between two structs with the same fields is
// exactly where a field gets added to one side and forgotten on the other.
//
// It is the GLOBAL tier of the save (docs/world-map.md "The save"): what is true
// of the whole game rather than of one dungeon. Per-level diffs stay where they
// are, keyed by level stem.
struct WorldState {
	// Is the party ON the world map rather than inside a dungeon? An explicit
	// flag rather than "currentLevel is empty", because an implicit encoding of
	// where the party is would be read wrongly exactly once.
	bool onWorldMap = false;
	int x = 0, z = 0;   // the party's world cell
	float time = 0.0f;  // hours elapsed in the world
	// The location the party ENTERED, while it is inside a dungeon — where
	// leaving puts it back. Empty when on the world map, and empty in a project
	// with no world at all (a game that is all dungeon still works: it simply
	// never has anywhere to come back to).
	//
	// The LOCATION, not the dungeon: two locations could open the same dungeon,
	// and coming out of the wrong one would be a teleport.
	std::string atLocation;

	// Revealed world cells — the world's fog of war, the same shape a level's
	// `seen` set has.
	std::vector<std::pair<int, int>> seen;
	// Location ids the party knows about. SEPARATE from `seen` on purpose: a
	// found map or clue reveals a location without revealing the ground around
	// it, and exploring reveals ground without necessarily naming what is on it
	// (docs/world-map.md "Discovery").
	std::vector<std::string> discovered;
	// WHERE EACH QUEST HAS GOT TO: quest id -> the STAGE it is at, by NAME.
	//
	// By name and not by index, for the reason a level's palette taught the
	// hard way: an index is a promise never to reorder, and a quest's stages
	// are exactly the kind of thing that gets one inserted in the middle. A
	// name that no longer exists is a CHECKED fault; an index that silently
	// means something else is not.
	//
	// A quest absent from this list has not started. That is why there is no
	// "not started" stage to author and forget.
	std::vector<std::pair<std::string, std::string>> quests;

	// Global flags: anything true of the GAME rather than of a place, that is
	// not a quest's progress — a rumour heard, a door bribed. Opaque key/value
	// on purpose: the things that do not deserve a quest's structure should
	// not have to pretend to it.
	std::vector<std::pair<std::string, std::string>> flags;

	// The stage a quest has reached, or null when it has not started.
	const std::string* QuestStage(std::string_view id) const;
	// Puts a quest at a stage. False when it was already there — callers
	// announce a step forward, and announcing it twice is the bug this stops.
	bool SetQuestStage(std::string id, std::string stage);
	// A global flag's value, or null. Absent and empty are different: a flag
	// set to "" was set.
	const std::string* Flag(std::string_view key) const;
	bool SetFlag(std::string key, std::string value);

	bool Discovered(std::string_view id) const;
	// Marks a location known. Returns false when it already was — callers
	// announce a discovery, and announcing it twice is the bug this prevents.
	bool Discover(std::string id);
	bool Seen(int cx, int cz) const;
	// Reveals a cell. Returns false when it was already revealed.
	bool MarkSeen(int cx, int cz);
};

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
		// The world map's ink for this kind (terrain.cat `color`). The map has
		// its own palette rather than the UI theme's, like the dungeon map.
		Vec4 color{0.5f, 0.5f, 0.5f, 1.0f};
	};
	using TerrainRules = std::vector<Terrain>;

	// Something standing on a world cell that the party can reach. `kind` is
	// "dungeon" today ("town" later).
	//
	// A LOCATION IS A DOORWAY, NOT A DUNGEON. `id` names the location itself
	// and `dungeon` names what is behind it, and they are separate because ONE
	// DUNGEON MAY HAVE SEVERAL WAYS IN — a front gate and a back way that comes
	// out somewhere else entirely on the map, landing in a different part of the
	// dungeon (Michael, 2026-09-09). An absent `dungeon` means "the same as my
	// id", which is what a single-entrance dungeon looks like and what every
	// location authored before this said.
	//
	// WHERE IT LANDS belongs to the location too, for the same reason: the back
	// way does not arrive where the front door does. An absent `level` falls
	// back to the dungeon's own `entry`, and an absent cell to that level's
	// start cell.
	struct Location {
		std::string kind;
		std::string id;
		int x = 0, z = 0;
		std::string dungeon;      // empty = same as id
		std::string level;        // empty = the dungeon's entry level
		int entryX = -1, entryZ = -1; // -1 = that level's own start cell
		std::vector<std::pair<std::string, std::string>> params;

		const std::string* Param(std::string_view key) const;
		// What is behind this doorway (`dungeon`, or the id itself).
		const std::string& Dungeon() const { return dungeon.empty() ? id : dungeon; }
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

	// --- editing (W3, docs/world-editor-plan.md) ----------------------------
	// Every mutator names a terrain / location by ID and validates: an editor
	// that could write a cell to a terrain that does not exist would author a
	// world its own loader aborts on.
	//
	// The grid stores an INDEX into m_terrain, so a terrain must be found
	// before a cell can be painted — which is also why there is no SetTerrain
	// taking an index: the caller would have to know the ordering, and the
	// whole point of glyphs was that nothing outside has to.
	bool SetTerrainAt(int x, int z, std::string_view terrainId);
	// Adds a location, refusing a duplicate id or an occupied cell — the two
	// things Load asserts on, checked here so the editor cannot author a world
	// that will not load.
	bool AddLocation(Location l);
	bool RemoveLocation(std::string_view id);
	// Moves one to another cell. False if the id is unknown, the cell is off
	// the grid, or another location already stands there.
	bool MoveLocation(std::string_view id, int x, int z);
	Location* MutableLocation(std::string_view id);
	void SetStart(int x, int z) { m_startX = x; m_startZ = z; }
	std::vector<Area>& MutableAreas() { return m_areas; }

	// --- writing (W1, docs/world-editor-plan.md) ----------------------------
	// The world as it would be written: records, then the grid, in the dialect
	// Load reads. Pure — it returns TEXT and touches no file, so a caller can
	// diff it, and so the round-trip check can compare without a disk.
	//
	// AUTHORING COMMENTS ARE NOT PRESERVED, the same rule the level writer
	// follows: the editor regenerates a header and notes belong in docs or code
	// (that is a standing project decision, not a limitation of this function).
	std::string Serialize() const;

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
