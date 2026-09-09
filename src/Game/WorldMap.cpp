// ============================================================================
// Game/WorldMap.cpp — see WorldMap.h.
// ============================================================================
#include "Game/WorldMap.h"

#include "Assets/File.h"
#include "Core/Assert.h"
#include "Game/Entity.h" // ReadLevelLines, SplitRecordTokens

#include <charconv>
#include <format>

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

const std::string* WorldMap::Location::Param(std::string_view key) const {
	for (const auto& [k, v] : params)
		if (k == key) return &v;
	return nullptr;
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
	// rule the whole level dialect is split on.
	for (size_t i = 0; i < w.m_terrain.size(); ++i) {
		const char g = w.m_terrain[i].glyph;
		DN_ASSERT(!(g >= 'a' && g <= 'z'),
				  std::format("terrain {} has lowercase glyph '{}' — records are "
							  "lowercase, grid rows are not",
							  w.m_terrain[i].id, g));
		for (size_t j = 0; j < i; ++j)
			DN_ASSERT(w.m_terrain[j].glyph != g,
					  std::format("terrain {} and {} share glyph '{}'",
								  w.m_terrain[j].id, w.m_terrain[i].id, g));
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
		l.params.emplace_back(std::move(k), std::move(v));
	}
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
