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

// What pressing a lever does to a flag (a button record's `sets=` / `clears=` /
// `toggles=` param - the op IS the key, so one record says both at once).
enum class FlagOp : u8 { None, Set, Clear, Toggle };
// The record key for an op ("" for None), and back (None for anything else).
inline const char* FlagOpKey(FlagOp op) {
	return op == FlagOp::Set ? "sets" : op == FlagOp::Clear ? "clears"
		 : op == FlagOp::Toggle ? "toggles" : "";
}
inline FlagOp FlagOpFromKey(std::string_view key) {
	return key == "sets" ? FlagOp::Set : key == "clears" ? FlagOp::Clear
		 : key == "toggles" ? FlagOp::Toggle : FlagOp::None;
}

// A LIST THAT TAKES A NEW ENTRY WITHOUT CONSTRUCTING ONE (code-review C212). The
// world state changes in guarded frames - a lifted item moves a quest on or
// reveals a place, a lever sets a flag - and a std::vector of strings cannot
// take a new entry there without allocating: emplace_back constructs strings,
// which the debug CRT allocates for at any length. So this keeps SPARE entries
// past its live count, their strings built already and given room (Reserve,
// sized from the catalogs at every new game and load - Game::ReserveWorldState),
// and a new entry takes the next spare and assigns into it. Past the spares it
// grows like a vector - something no catalog accounts for - and the allocation
// guard reports that.
//
// It reads like the vector it replaced over its LIVE entries (size / empty /
// range-for). A copy carries the spares but not their room (a save's copy needs
// none; a load re-reserves the state it took).
template <class T>
class SpareList {
public:
	size_t size() const { return m_count; }
	bool empty() const { return m_count == 0; }
	T* begin() { return m_items.data(); }
	T* end() { return m_items.data() + m_count; }
	const T* begin() const { return m_items.data(); }
	const T* end() const { return m_items.data() + m_count; }
	// How many new entries it can still take without constructing one.
	size_t Spare() const { return m_items.size() - m_count; }

	// The next entry, emptied: a spare's strings when one is left.
	T& Append() {
		if (m_count == m_items.size()) m_items.emplace_back();
		T& e = m_items[m_count++];
		Empty(e);
		return e;
	}
	// Room for `spare` new entries past the live ones, and every string - live
	// ones too, whose values a later set may lengthen - able to hold `capacity`
	// characters. Never shrinks anything.
	void Reserve(size_t spare, size_t capacity) {
		if (m_items.size() < m_count + spare) m_items.resize(m_count + spare);
		for (T& e : m_items) Room(e, capacity);
	}

private:
	static void Empty(std::string& s) { s.clear(); }
	static void Empty(std::pair<std::string, std::string>& p) {
		p.first.clear();
		p.second.clear();
	}
	static void Room(std::string& s, size_t n) { s.reserve(n); }
	static void Room(std::pair<std::string, std::string>& p, size_t n) {
		p.first.reserve(n);
		p.second.reserve(n);
	}

	std::vector<T> m_items; // [0, m_count) live, the rest spare (empty, with room)
	size_t m_count = 0;
};

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
	//
	// This and the two lists below are SpareLists: each takes a new entry in a
	// guarded frame without allocating, given room by Reserve.
	SpareList<std::string> discovered;
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
	SpareList<std::pair<std::string, std::string>> quests;

	// Global flags: anything true of the GAME rather than of a place, that is
	// not a quest's progress — a rumour heard, a door bribed. Opaque key/value
	// on purpose: the things that do not deserve a quest's structure should
	// not have to pretend to it.
	SpareList<std::pair<std::string, std::string>> flags;

	// THE SETTERS FIND FIRST and take views, so a set that changes nothing
	// constructs nothing (a re-lift of a quest item, a lever pressed again); a
	// change assigns into the entry's own strings, and only a NEW entry takes a
	// spare (SpareList). Allocation-free in play once Reserve has given the
	// lists room for what the catalogs can make.
	//
	// Room for `quests` / `flags` / `places` new entries and every string able
	// to hold `capacity` characters (Game::ReserveWorldState sizes it).
	void Reserve(size_t quests, size_t flags, size_t places, size_t capacity);

	// The stage a quest has reached, or null when it has not started.
	const std::string* QuestStage(std::string_view id) const;
	// Puts a quest at a stage. False when it was already there — callers
	// announce a step forward, and announcing it twice is the bug this stops.
	bool SetQuestStage(std::string_view id, std::string_view stage);
	// A global flag's value, or null. Absent and empty are different: a flag
	// set to "" was set.
	const std::string* Flag(std::string_view key) const;
	bool SetFlag(std::string_view key, std::string_view value);
	// THE ON/OFF VIEW an authored flag (flags.cat) is read and written through.
	// On = set to anything but "0" (so a hand-authored `flag = seal=broken` on an
	// item reads as on); off = absent or "0". Switching one off WRITES "0" rather
	// than erasing it, so a save shows it was touched. The doors and buttons that
	// call this are pressed in play: the SetFlag rule above.
	bool FlagOn(std::string_view key) const;
	// False when it was already in that state (the SetFlag rule).
	bool SetFlagOn(std::string_view key, bool on);

	bool Discovered(std::string_view id) const;
	// Marks a location known. Returns false when it already was — callers
	// announce a discovery, and announcing it twice is the bug this prevents.
	bool Discover(std::string_view id);
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

	// --- terrain glyphs (code-review C345) ----------------------------------
	// THE RULES A GLYPH KEEPS, stated once: Load asserts on them, the type
	// editor's Save refuses by them and the checker reports them, so the three
	// cannot drift into a world the editor wrote and the loader will not open.
	// A glyph IS one grid cell, so it is one character, and the level dialect
	// decides which characters are free to be one: a lowercase letter starts a
	// record, ';' a comment, and whitespace, a control byte or anything past
	// ASCII would not survive an editor, a code page or a trimmed line.
	enum class GlyphFault : u8 { None, Length, Lowercase, Reserved, Taken };
	static GlyphFault CheckGlyph(char c);
	// The same of a glyph as AUTHORED (terrain.cat's text), which must also be
	// exactly one character - absent, empty or "MM" is a Length fault.
	static GlyphFault CheckGlyphText(std::string_view text);
	// The fault in English, for a log line or an assert (the UI says it in its
	// own words, through Loc).
	static const char* GlyphFaultText(GlyphFault fault);
	// The first glyph `taken` does not hold, from a fixed list of the plainest
	// (A-Z, then the digits, then a few marks), or '\0' when every one is taken.
	// A "+ New" terrain is given one, so two new kinds never share a default.
	static char FreeGlyph(std::string_view taken);
	// The first reason `rules` cannot be read as a world's terrain kinds: a kind
	// whose glyph breaks a rule (`a`), or two sharing one (`a` and `b`, Taken).
	struct TerrainFault {
		GlyphFault fault = GlyphFault::None;
		size_t a = 0, b = 0;
	};
	static TerrainFault CheckTerrains(const TerrainRules& rules);

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
		// -1, -1 = that level's own start cell. BOTH OR NEITHER, and never
		// another negative: an editor changes them only through SetLocationEntry
		// / ClearLocationEntry (code-review C343).
		int entryX = -1, entryZ = -1;
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
	// How many cells are this terrain - what a delete of the kind is refused by.
	int TerrainCells(std::string_view id) const;
	// Replaces the terrain kinds with `next`, MATCHED BY ID (code-review C345):
	// every cell keeps its terrain under that terrain's index in `next`, so a
	// changed glyph rewrites the grid's text for free and an added kind changes
	// no cell. Refuses - false, nothing changed - what Load would refuse: no
	// kinds, a glyph breaking the rules, or a kind some cell is that `next` does
	// not hold. The catalog is the authority: the type editor's Save, a "+ New",
	// a delete and an undo restore all hand the world terrain.cat's kinds here.
	bool SyncTerrains(TerrainRules next);
	// Renames a kind where it stands - the cells hold indices, so nothing else
	// moves. False for an unknown id or one already taken.
	bool RenameTerrain(std::string_view id, std::string newId);
	// Adds a location, refusing a duplicate id (Load does not check one, but the
	// editor addresses a location by id and could not tell two apart), an
	// occupied or off-grid cell (both of which Load asserts on), and half an
	// entry or a negative one (which Load asserts on too).
	bool AddLocation(Location l);
	bool RemoveLocation(std::string_view id);
	// Moves one to another cell. False if the id is unknown, the cell is off
	// the grid, or another location already stands there.
	bool MoveLocation(std::string_view id, int x, int z);
	// WHERE A DOORWAY LANDS, both coordinates or neither (code-review C343): the
	// one way an entry changes, because the loader asserts on half of one and
	// Serialize can only write a whole one. Set refuses an unknown id and a
	// negative coordinate - so setting one half of an entry that has none, its
	// other half still -1, is refused too. Clear goes back to the level's own
	// start cell. Each false = nothing changed.
	bool SetLocationEntry(std::string_view id, int x, int z);
	bool ClearLocationEntry(std::string_view id);
	// Everything else about a location (kind, dungeon, level, params). Its
	// entry goes through SetLocationEntry, its cell through MoveLocation.
	Location* MutableLocation(std::string_view id);
	// Moves where a new game puts the party. Refuses off the grid, and refuses
	// IMPASSABLE ground — the checker calls that an error, and an editor must
	// not be able to author a world the checker rejects a moment later.
	//
	// THE RULE LIVES HERE AND NOT IN ITS CALLERS. It sat in the `worldprops`
	// command, which was fine while the console was the only way in; the
	// settings dialog is a second way, and two paths to one action with the
	// rule in only one of them is the exact shape of the no-op undo step W3
	// spent an afternoon on.
	bool SetStart(int x, int z);

	// Areas, addressed BY ID like everything else the editor edits. The list
	// stays ordered — the order IS the rule, last match wins — so MoveArea is
	// an operation rather than a sort, and AddArea APPENDS: a newly carved
	// exception wins over the broad region it was carved out of, which is what
	// carving one means.
	//
	// A DUPLICATE ID IS REFUSED, for a different reason from a location's: the
	// loader tolerates two areas sharing a name, but the editor addresses one
	// BY id, and two rows answering to one name is not something it can
	// represent. Hand-authored duplicates still load; Remove and Move act on
	// the FIRST match.
	bool AddArea(Area a);
	bool RemoveArea(std::string_view id);
	bool MoveArea(std::string_view id, int index);
	Area* MutableArea(std::string_view id);
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
