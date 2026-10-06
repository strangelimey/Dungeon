// ============================================================================
// Game/WorldMap.cpp — see WorldMap.h.
// ============================================================================
#include "Game/WorldMap.h"

#include "Assets/File.h"
#include "Core/Assert.h"
#include "Game/Entity.h" // ReadLevelLines, SplitRecordTokens

#include <algorithm>
#include <charconv>
#include <format>
#include <string>

namespace dungeon::game {

namespace {

// The terrain a query gets for a cell outside the grid: impassable, free to
// not-cross, harmless. Callers may ask about any coordinate without guarding
// first, and an out-of-bounds answer that says "you cannot go there" is the one
// that keeps them honest.
const WorldMap::Terrain kNowhere{.id = "", .glyph = ' ', .passable = false,
								 .travel = 0.0f, .difficulty = 0.0f};

int ParseCoord(std::string_view t, const std::string& record,
			   const std::string& path) {
	int v = 0;
	const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
	DN_ASSERT(ec == std::errc{} && end == t.data() + t.size(),
			  std::format("bad world number \"{}\": \"{}\" in {}", t, record, path));
	return v;
}

float ParseNumber(std::string_view t, const std::string& record,
				  const std::string& path) {
	float v = 0.0f;
	const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
	DN_ASSERT(ec == std::errc{} && end == t.data() + t.size(),
			  std::format("bad world number \"{}\": \"{}\" in {}", t, record, path));
	return v;
}

// Splits "key=value" off a record token. False for a bare token.
bool SplitParam(std::string_view tok, std::string& key, std::string& value) {
	const size_t eq = tok.find('=');
	if (eq == std::string_view::npos) return false;
	key = std::string(tok.substr(0, eq));
	value = std::string(tok.substr(eq + 1));
	return true;
}

} // namespace

namespace {
// The shared shape of `quests` and `flags`: a small ordered id -> value list.
// Ordered rather than a map so a save reads in the order things happened, which
// is the only ordering a human reading one actually wants.
using PairList = SpareList<std::pair<std::string, std::string>>;

const std::string* FindPair(const PairList& list, std::string_view key) {
	for (const auto& [k, v] : list)
		if (k == key) return &v;
	return nullptr;
}
// Find first, then assign: nothing is constructed unless the key is new, and a
// new key takes a spare entry (see SpareList).
bool SetPair(PairList& list, std::string_view key, std::string_view value) {
	for (auto& [k, v] : list)
		if (k == key) {
			if (v == value) return false; // already there; nothing to announce
			v.assign(value);
			return true;
		}
	auto& [k, v] = list.Append();
	k.assign(key);
	v.assign(value);
	return true;
}
} // namespace

void WorldState::Reserve(size_t questRoom, size_t flagRoom, size_t placeRoom,
						 size_t capacity) {
	quests.Reserve(questRoom, capacity);
	flags.Reserve(flagRoom, capacity);
	discovered.Reserve(placeRoom, capacity);
}

const std::string* WorldState::QuestStage(std::string_view id) const {
	return FindPair(quests, id);
}

bool WorldState::SetQuestStage(std::string_view id, std::string_view stage) {
	return SetPair(quests, id, stage);
}

const std::string* WorldState::Flag(std::string_view key) const {
	return FindPair(flags, key);
}

bool WorldState::SetFlag(std::string_view key, std::string_view value) {
	return SetPair(flags, key, value);
}

bool WorldState::FlagOn(std::string_view key) const {
	const std::string* v = FindPair(flags, key);
	return v && *v != "0";
}

bool WorldState::SetFlagOn(std::string_view key, bool on) {
	if (FlagOn(key) == on) return false; // includes "off" on a flag never set
	return SetPair(flags, key, on ? "1" : "0");
}

bool WorldState::Discovered(std::string_view id) const {
	return std::find(discovered.begin(), discovered.end(), id) != discovered.end();
}

bool WorldState::Discover(std::string_view id) {
	if (Discovered(id)) return false;
	discovered.Append().assign(id);
	return true;
}

bool WorldState::Seen(int cx, int cz) const {
	return std::find(seen.begin(), seen.end(), std::pair{cx, cz}) != seen.end();
}

bool WorldState::MarkSeen(int cx, int cz) {
	if (Seen(cx, cz)) return false;
	seen.emplace_back(cx, cz);
	return true;
}

const std::string* WorldMap::Location::Param(std::string_view key) const {
	for (const auto& [k, v] : params)
		if (k == key) return &v;
	return nullptr;
}

// --- terrain glyphs (code-review C345) ---------------------------------------

WorldMap::GlyphFault WorldMap::CheckGlyph(char c) {
	if (c >= 'a' && c <= 'z') return GlyphFault::Lowercase;
	// Printable ASCII past the space, and not the comment mark: ReadLevelLines
	// drops a line that STARTS with ';', so a row whose first cell were one
	// would vanish from the grid and every row under it would move up.
	const unsigned char u = static_cast<unsigned char>(c);
	if (u <= ' ' || u >= 0x7F || c == ';') return GlyphFault::Reserved;
	return GlyphFault::None;
}

WorldMap::GlyphFault WorldMap::CheckGlyphText(std::string_view text) {
	return text.size() == 1 ? CheckGlyph(text[0]) : GlyphFault::Length;
}

const char* WorldMap::GlyphFaultText(GlyphFault fault) {
	switch (fault) {
	case GlyphFault::None: return "a usable glyph";
	case GlyphFault::Length: return "a glyph is exactly one character";
	case GlyphFault::Lowercase: return "lowercase starts a record - grid rows are not lowercase";
	case GlyphFault::Reserved:
		return "';', whitespace, control bytes and non-ASCII cannot stand in a grid row";
	case GlyphFault::Taken: return "another terrain already has it";
	}
	return "?";
}

char WorldMap::FreeGlyph(std::string_view taken) {
	// The plainest characters first, so a new kind reads as a letter in the
	// file. '?' is left out on purpose: it is what an unreadable catalog glyph
	// falls back to (Game_World.cpp GlyphOf), and a kind handed it would share
	// with the first broken one.
	constexpr std::string_view kCandidates =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!$%&*+=@<>|/:";
	for (const char c : kCandidates)
		if (taken.find(c) == std::string_view::npos) return c;
	return '\0';
}

WorldMap::TerrainFault WorldMap::CheckTerrains(const TerrainRules& rules) {
	for (size_t i = 0; i < rules.size(); ++i) {
		if (const GlyphFault f = CheckGlyph(rules[i].glyph); f != GlyphFault::None)
			return {f, i, i};
		for (size_t j = 0; j < i; ++j)
			if (rules[j].glyph == rules[i].glyph) return {GlyphFault::Taken, j, i};
	}
	return {};
}

std::optional<WorldMap> WorldMap::Load(const std::string& path,
									   TerrainRules terrain) {
	auto bytes = assets::ReadBinaryFile(path);
	if (!bytes) return std::nullopt; // no world authored yet — not an error

	WorldMap w;
	w.m_terrain = std::move(terrain);
	DN_ASSERT(!w.m_terrain.empty(),
			  "world map needs at least one terrain kind (catalog/terrain.cat): " + path);

	// A glyph is how the grid names a terrain, so two kinds cannot share one and
	// none may be lowercase — records are lowercase and grid rows are not, the
	// rule the whole level dialect is split on. The rules are CheckTerrains',
	// which the editor refuses by and the checker reports, so nothing the
	// editor writes can stop here.
	if (const TerrainFault f = CheckTerrains(w.m_terrain); f.fault != GlyphFault::None) {
		const Terrain& a = w.m_terrain[f.a];
		const Terrain& b = w.m_terrain[f.b];
		DN_ASSERT(false, f.fault == GlyphFault::Taken
							 ? std::format("terrain {} and {} share glyph '{}' in {}", a.id,
										   b.id, a.glyph, path)
							 : std::format("terrain {} has glyph 0x{:02x} - {} ({})", a.id,
										   static_cast<unsigned char>(a.glyph),
										   GlyphFaultText(f.fault), path));
	}

	std::vector<std::string> rows;
	std::vector<std::string> records;
	for (std::string& line : ReadLevelLines(*bytes)) {
		if (line[0] >= 'a' && line[0] <= 'z') records.push_back(std::move(line));
		else rows.push_back(std::move(line));
	}
	DN_ASSERT(!rows.empty(), "world map has no grid rows: " + path);

	w.m_height = static_cast<int>(rows.size());
	w.m_width = static_cast<int>(rows[0].size());
	w.m_cells.resize(static_cast<size_t>(w.m_width) * w.m_height, 0);
	for (int z = 0; z < w.m_height; ++z) {
		const std::string& row = rows[static_cast<size_t>(z)];
		DN_ASSERT(static_cast<int>(row.size()) == w.m_width,
				  std::format("ragged world row {} in {}", z, path));
		for (int x = 0; x < w.m_width; ++x) {
			const char c = row[static_cast<size_t>(x)];
			size_t kind = w.m_terrain.size();
			for (size_t i = 0; i < w.m_terrain.size(); ++i)
				if (w.m_terrain[i].glyph == c) {
					kind = i;
					break;
				}
			DN_ASSERT(kind < w.m_terrain.size(),
					  std::format("unknown world glyph '{}' at column {}, row {} in "
								  "{} — no terrain.cat entry declares it",
								  c, x, z, path));
			w.m_cells[static_cast<size_t>(z) * w.m_width + x] =
				static_cast<u8>(kind);
		}
	}

	for (const std::string& record : records) {
		if (record.starts_with("location")) w.ParseLocationRecord(record, path);
		else if (record.starts_with("area")) w.ParseAreaRecord(record, path);
		else if (record.starts_with("start")) w.ParseStartRecord(record, path);
		else
			DN_ASSERT(false, std::format("unknown world record \"{}\" in {}",
										 record, path));
	}

	// Two locations on one cell would make LocationAt ambiguous and the map view
	// unreadable; the party can only enter one thing per square.
	for (size_t i = 0; i < w.m_locations.size(); ++i) {
		const Location& a = w.m_locations[i];
		DN_ASSERT(w.InBounds(a.x, a.z),
				  std::format("location {} at {},{} is off the world grid in {}",
							  a.id, a.x, a.z, path));
		for (size_t j = 0; j < i; ++j)
			DN_ASSERT(!(w.m_locations[j].x == a.x && w.m_locations[j].z == a.z),
					  std::format("locations {} and {} share cell {},{} in {}",
								  w.m_locations[j].id, a.id, a.x, a.z, path));
	}
	DN_ASSERT(w.InBounds(w.m_startX, w.m_startZ),
			  std::format("world start {},{} is off the grid in {}", w.m_startX,
						  w.m_startZ, path));

	return w;
}

// "location <kind> <id> <x> <z> [key=value ...]" — a dungeon entrance (later a
// town). `id` names a DUNGEON, not a level: which level it opens at is the
// dungeon's business, not the world's.
void WorldMap::ParseLocationRecord(const std::string& record,
								   const std::string& path) {
	const std::vector<std::string_view> tok = SplitRecordTokens(record);
	DN_ASSERT(tok.size() >= 5,
			  std::format("location needs <kind> <id> <x> <z>: \"{}\" in {}",
						  record, path));
	Location l;
	l.kind = std::string(tok[1]);
	l.id = std::string(tok[2]);
	l.x = ParseCoord(tok[3], record, path);
	l.z = ParseCoord(tok[4], record, path);
	for (size_t i = 5; i < tok.size(); ++i) {
		std::string k, v;
		DN_ASSERT(SplitParam(tok[i], k, v),
				  std::format("stray token \"{}\" in location record \"{}\" in {}",
							  tok[i], record, path));
		// The typed ones are lifted out; anything else stays a free param, the
		// same way an entity record carries its wiring.
		if (k == "dungeon") l.dungeon = v;
		else if (k == "level") l.level = v;
		else if (k == "entryx") l.entryX = ParseCoord(v, record, path);
		else if (k == "entryz") l.entryZ = ParseCoord(v, record, path);
		else l.params.emplace_back(std::move(k), std::move(v));
	}
	// Half a cell is not a cell: an entry that names one coordinate and not the
	// other would silently land on the level's start row or column, which reads
	// as "the entrance moved" rather than as the authoring slip it is.
	DN_ASSERT((l.entryX < 0) == (l.entryZ < 0),
			  std::format("location {} names entryx or entryz but not both: "
						  "\"{}\" in {}", l.id, record, path));
	m_locations.push_back(std::move(l));
}

// "area <id> <x> <z> <w> <h> [difficulty=<0..1>]" — a rectangle overriding its
// terrain's danger. Last match wins (see the header), so author broad first.
void WorldMap::ParseAreaRecord(const std::string& record,
							   const std::string& path) {
	const std::vector<std::string_view> tok = SplitRecordTokens(record);
	DN_ASSERT(tok.size() >= 6,
			  std::format("area needs <id> <x> <z> <w> <h>: \"{}\" in {}", record,
						  path));
	Area a;
	a.id = std::string(tok[1]);
	a.x = ParseCoord(tok[2], record, path);
	a.z = ParseCoord(tok[3], record, path);
	a.w = ParseCoord(tok[4], record, path);
	a.h = ParseCoord(tok[5], record, path);
	DN_ASSERT(a.w > 0 && a.h > 0,
			  std::format("area {} has no extent: \"{}\" in {}", a.id, record, path));
	for (size_t i = 6; i < tok.size(); ++i) {
		std::string k, v;
		DN_ASSERT(SplitParam(tok[i], k, v),
				  std::format("stray token \"{}\" in area record \"{}\" in {}",
							  tok[i], record, path));
		if (k == "difficulty") a.difficulty = ParseNumber(v, record, path);
		else
			DN_ASSERT(false, std::format("unknown area key \"{}\": \"{}\" in {}", k,
										 record, path));
	}
	m_areas.push_back(std::move(a));
}

// "start <x> <z>" — where a new game puts the party on the world.
void WorldMap::ParseStartRecord(const std::string& record,
								const std::string& path) {
	const std::vector<std::string_view> tok = SplitRecordTokens(record);
	DN_ASSERT(tok.size() == 3,
			  std::format("start needs <x> <z>: \"{}\" in {}", record, path));
	m_startX = ParseCoord(tok[1], record, path);
	m_startZ = ParseCoord(tok[2], record, path);
}

bool WorldMap::SetTerrainAt(int x, int z, std::string_view terrainId) {
	if (!InBounds(x, z)) return false;
	for (size_t i = 0; i < m_terrain.size(); ++i)
		if (m_terrain[i].id == terrainId) {
			m_cells[static_cast<size_t>(z) * m_width + x] = static_cast<u8>(i);
			return true;
		}
	return false; // an unknown terrain would author a world Load aborts on
}

int WorldMap::TerrainCells(std::string_view id) const {
	int n = 0;
	for (size_t i = 0; i < m_terrain.size(); ++i)
		if (m_terrain[i].id == id)
			for (const u8 c : m_cells)
				if (static_cast<size_t>(c) == i) ++n;
	return n;
}

bool WorldMap::SyncTerrains(TerrainRules next) {
	if (next.empty() || CheckTerrains(next).fault != GlyphFault::None) return false;
	// Where each of today's kinds lands in `next`, by id. A kind `next` lacks
	// maps nowhere, which is only a refusal if some cell is that kind.
	std::vector<size_t> to(m_terrain.size(), next.size());
	for (size_t i = 0; i < m_terrain.size(); ++i)
		for (size_t j = 0; j < next.size(); ++j)
			if (next[j].id == m_terrain[i].id) {
				to[i] = j;
				break;
			}
	for (const u8 c : m_cells)
		if (static_cast<size_t>(c) >= to.size() || to[c] >= next.size()) return false;
	for (u8& c : m_cells) c = static_cast<u8>(to[c]);
	m_terrain = std::move(next);
	return true;
}

bool WorldMap::RenameTerrain(std::string_view id, std::string newId) {
	Terrain* renaming = nullptr;
	for (Terrain& t : m_terrain) {
		if (t.id == newId) return false;
		if (t.id == id) renaming = &t;
	}
	if (!renaming) return false;
	renaming->id = std::move(newId);
	return true;
}

namespace {
// An entry the loader reads back: both halves set and on the grid's side of
// zero, or both the "none" -1 (code-review C343).
bool EntryWhole(int x, int z) {
	return (x >= 0 && z >= 0) || (x == -1 && z == -1);
}
} // namespace

bool WorldMap::AddLocation(Location l) {
	if (!InBounds(l.x, l.z) || !EntryWhole(l.entryX, l.entryZ)) return false;
	for (const Location& e : m_locations)
		if (e.id == l.id || (e.x == l.x && e.z == l.z)) return false;
	m_locations.push_back(std::move(l));
	return true;
}

bool WorldMap::SetLocationEntry(std::string_view id, int x, int z) {
	if (x < 0 || z < 0) return false;
	Location* l = MutableLocation(id);
	if (!l) return false;
	l->entryX = x;
	l->entryZ = z;
	return true;
}

bool WorldMap::ClearLocationEntry(std::string_view id) {
	Location* l = MutableLocation(id);
	if (!l) return false;
	l->entryX = l->entryZ = -1;
	return true;
}

bool WorldMap::RemoveLocation(std::string_view id) {
	const size_t before = m_locations.size();
	std::erase_if(m_locations, [&](const Location& l) { return l.id == id; });
	return m_locations.size() != before;
}

bool WorldMap::MoveLocation(std::string_view id, int x, int z) {
	if (!InBounds(x, z)) return false;
	Location* moving = nullptr;
	for (Location& l : m_locations) {
		if (l.id == id) moving = &l;
		// Checked BEFORE the move, and against every OTHER location: a move
		// onto an occupied cell has to fail whole rather than leave two
		// doorways sharing a square, which LocationAt cannot represent.
		else if (l.x == x && l.z == z) return false;
	}
	if (!moving) return false;
	moving->x = x;
	moving->z = z;
	return true;
}

WorldMap::Location* WorldMap::MutableLocation(std::string_view id) {
	for (Location& l : m_locations)
		if (l.id == id) return &l;
	return nullptr;
}

bool WorldMap::SetStart(int x, int z) {
	// IMPASSABLE GROUND IS REFUSED, not clamped or corrected: the checker
	// reports a start the party cannot stand on, and an editor that could
	// author one would be handing its own checker a fault to find.
	if (!InBounds(x, z) || !Passable(x, z)) return false;
	m_startX = x;
	m_startZ = z;
	return true;
}

bool WorldMap::AddArea(Area a) {
	if (a.w <= 0 || a.h <= 0) return false; // Load asserts on a zero extent
	for (const Area& e : m_areas)
		if (e.id == a.id) return false; // see the header: the editor addresses by id
	m_areas.push_back(std::move(a)); // APPENDED — the newest row wins
	return true;
}

bool WorldMap::RemoveArea(std::string_view id) {
	for (auto it = m_areas.begin(); it != m_areas.end(); ++it)
		if (it->id == id) {
			m_areas.erase(it);
			return true;
		}
	return false;
}

bool WorldMap::MoveArea(std::string_view id, int index) {
	if (index < 0 || index >= static_cast<int>(m_areas.size())) return false;
	auto it = std::find_if(m_areas.begin(), m_areas.end(),
						   [&](const Area& a) { return a.id == id; });
	if (it == m_areas.end()) return false;
	// A MOVE TO WHERE IT ALREADY IS CHANGES NOTHING, and says so, so a caller
	// bracketing this as an undo step does not put an empty one on the stack
	// (the SetTerrainAt lesson: "I found it" is not "something changed").
	if (it - m_areas.begin() == index) return false;
	Area moved = std::move(*it);
	m_areas.erase(it);
	m_areas.insert(m_areas.begin() + index, std::move(moved));
	return true;
}

WorldMap::Area* WorldMap::MutableArea(std::string_view id) {
	for (Area& a : m_areas)
		if (a.id == id) return &a;
	return nullptr;
}

std::string WorldMap::Serialize() const {
	std::string m = "; The overworld - written by the in-game editor.\n\n";
	m += std::format("start {} {}\n", m_startX, m_startZ);

	// AREAS IN FILE ORDER, because the order IS the rule: they may overlap and
	// the LAST match wins. Writing them sorted, or grouped, would silently
	// change which area owns a cell.
	for (const Area& a : m_areas) {
		m += std::format("area {} {} {} {} {}", a.id, a.x, a.z, a.w, a.h);
		if (a.difficulty >= 0.0f) m += std::format(" difficulty={:g}", a.difficulty);
		m += '\n';
	}

	for (const Location& l : m_locations) {
		m += std::format("location {} {} {} {}", l.kind, l.id, l.x, l.z);
		// Only what was AUTHORED: `dungeon` absent means "same as my id", and
		// writing it out anyway would turn every location into one that had made
		// a choice it had not.
		if (!l.dungeon.empty()) m += " dungeon=" + l.dungeon;
		if (!l.level.empty()) m += " level=" + l.level;
		// A WHOLE entry or none: half of one is what the loader asserts on, and
		// writing it would have the save that wrote it abort on its own read-back
		// (code-review C343).
		if (l.entryX >= 0 && l.entryZ >= 0)
			m += std::format(" entryx={} entryz={}", l.entryX, l.entryZ);
		for (const auto& [k, v] : l.params) m += std::format(" {}={}", k, v);
		m += '\n';
	}

	m += ";\n";
	// The grid, through each cell's terrain GLYPH - which is why a terrain
	// declares one: the writer needs no palette-index bookkeeping, and a
	// reordered terrain.cat cannot repaint the world.
	for (int z = 0; z < m_height; ++z) {
		std::string row;
		row.reserve(static_cast<size_t>(m_width));
		for (int x = 0; x < m_width; ++x) row += TerrainAt(x, z).glyph;
		m += row + "\n";
	}
	return m;
}
const WorldMap::Terrain& WorldMap::TerrainAt(int x, int z) const {
	if (!InBounds(x, z)) return kNowhere;
	return m_terrain[m_cells[static_cast<size_t>(z) * m_width + x]];
}

float WorldMap::Difficulty(int x, int z) const {
	if (const Area* a = AreaAt(x, z); a && a->difficulty >= 0.0f)
		return a->difficulty;
	return TerrainAt(x, z).difficulty;
}

const WorldMap::Location* WorldMap::LocationAt(int x, int z) const {
	for (const Location& l : m_locations)
		if (l.x == x && l.z == z) return &l;
	return nullptr;
}

const WorldMap::Area* WorldMap::AreaAt(int x, int z) const {
	const Area* found = nullptr;
	for (const Area& a : m_areas)
		if (a.Contains(x, z)) found = &a; // last match wins
	return found;
}

} // namespace dungeon::game
