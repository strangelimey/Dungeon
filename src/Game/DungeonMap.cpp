#include "Game/DungeonMap.h"

#include "Assets/File.h"
#include "Core/Assert.h"
#include "Core/Log.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace dungeon::game {

DungeonMap::DungeonMap(const std::string& path) {
	auto bytes = assets::ReadBinaryFile(path);
	DN_ASSERT(bytes.has_value(), bytes.error());

	// Grid rows vs. entity records: records start with a lowercase letter
	// (grid glyphs never do — keep new glyphs out of 'a'..'z').
	std::vector<std::string> rows;
	std::vector<std::string> records;
	for (std::string& line : ReadLevelLines(*bytes)) {
		if (line[0] >= 'a' && line[0] <= 'z') records.push_back(std::move(line));
		else rows.push_back(std::move(line));
	}
	DN_ASSERT(!rows.empty(), "map file has no grid rows: " + path);

	m_height = static_cast<int>(rows.size());
	m_width = static_cast<int>(rows[0].size());
	m_cells.resize(static_cast<size_t>(m_width) * m_height, Cell::Wall);
	m_turbidity.resize(m_cells.size(), 0.0f);
	m_dusty.resize(m_cells.size(), 0);
	m_wallVar.resize(m_cells.size(), -1);
	m_floorVar.resize(m_cells.size(), -1);
	m_ceilingVar.resize(m_cells.size(), -1);

	// Sconces are gathered raw (from 'T' glyphs and fixture records) and have
	// their mount wall resolved once the whole grid is known — a glyph or an
	// unfaced record auto-detects, an explicit facing is taken as-is.
	struct RawSconce {
		int x, z;
		bool hasWall;
		Direction wall;
		bool lit = true;
		float brightness = kSconceBrightness;
		float turbidity = kSconceTurbidity;
	};
	std::vector<RawSconce> rawSconces;

	bool foundStart = false;
	for (int z = 0; z < m_height; ++z) {
		const std::string& row = rows[static_cast<size_t>(z)];
		DN_ASSERT(static_cast<int>(row.size()) == m_width,
				  std::format("ragged map row {} in {}", z, path));
		for (int x = 0; x < m_width; ++x) {
			const char c = row[static_cast<size_t>(x)];
			switch (c) {
			case '#': break; // solid rock (the default)
			case '.': break;
			case 'D':
				m_turbidity[static_cast<size_t>(z) * m_width + x] = 1.0f;
				m_dusty[static_cast<size_t>(z) * m_width + x] = 1;
				break;
			case 'T': rawSconces.push_back({x, z, false, Direction::North}); break;
			case 'F': m_braziers.emplace_back(x, z); break;
			case 'P':
				DN_ASSERT(!foundStart,
						  std::format("multiple 'P' start cells in {}", path));
				m_startX = x;
				m_startZ = z;
				foundStart = true;
				break;
			default:
				DN_ASSERT(false, std::format("unknown map glyph '{}' at column {}, row {} in {}",
											 c, x, z, path));
			}
			m_cells[static_cast<size_t>(z) * m_width + x] =
				c == '#' ? Cell::Wall : Cell::Floor;
		}
	}
	DN_ASSERT(foundStart, "map has no 'P' start cell: " + path);

	// Records: surface palettes and static decorations.
	for (const std::string& record : records) {
		if (record.starts_with("palette")) {
			ParsePaletteRecord(record, path);
			continue;
		}
		if (record.starts_with("fixture")) {
			// fixture <sconce|brazier> <x> <z> [facing]
			const std::vector<std::string_view> tok = SplitRecordTokens(record);
			DN_ASSERT(tok.size() >= 4,
					  std::format("fixture needs <sconce|brazier> <x> <z>: \"{}\" in {}",
								  record, path));
			const auto coord = [&](std::string_view t) {
				int v = 0;
				const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
				DN_ASSERT(ec == std::errc{} && end == t.data() + t.size(),
						  std::format("bad fixture coordinate \"{}\": \"{}\" in {}",
									  t, record, path));
				return v;
			};
			const int fx = coord(tok[2]), fz = coord(tok[3]);
			DN_ASSERT(IsWalkable(fx, fz),
					  std::format("fixture out of bounds or in solid rock: \"{}\" in {}",
								  record, path));
			if (tok[1] == "sconce") {
				// fixture sconce <x> <z> [facing] [lit=0|1] [bright=<cells>] [turb=<f>]
				RawSconce rs{fx, fz, false, Direction::North};
				const auto num = [&](std::string_view t) {
					float v = 0.0f;
					std::from_chars(t.data(), t.data() + t.size(), v);
					return v;
				};
				for (size_t i = 4; i < tok.size(); ++i) {
					Direction d;
					if (ParseDirection(tok[i], d)) {
						rs.wall = d;
						rs.hasWall = true;
						continue;
					}
					const size_t eq = tok[i].find('=');
					DN_ASSERT(eq != std::string_view::npos,
							  std::format("expected facing or key=value, got \"{}\": \"{}\" in {}",
										  tok[i], record, path));
					const std::string_view key = tok[i].substr(0, eq), val = tok[i].substr(eq + 1);
					if (key == "lit") rs.lit = !(val == "0" || val == "false");
					else if (key == "bright") rs.brightness = num(val);
					else if (key == "turb") rs.turbidity = num(val);
					else
						DN_ASSERT(false, std::format("unknown sconce key \"{}\": \"{}\" in {}",
													 key, record, path));
				}
				rawSconces.push_back(rs);
			} else if (tok[1] == "brazier") {
				m_braziers.emplace_back(fx, fz);
			} else {
				DN_ASSERT(false,
						  std::format("unknown fixture \"{}\" (sconce or brazier): \"{}\" in {}",
									  tok[1], record, path));
			}
			continue;
		}
		if (record.starts_with("stairs")) {
			ParseStairRecord(record, path);
			continue;
		}
		if (record.starts_with("variant")) {
			ParseVariantRecord(record, path);
			continue;
		}
		Entity e = ParseEntityRecord(record, path);
		DN_ASSERT(e.kind == EntityKind::Decoration,
				  std::format("only decorations are static — move \"{}\" to the .ent file ({})",
							  record, path));
		DN_ASSERT(IsWalkable(e.x, e.z),
				  std::format("decoration out of bounds or in solid rock: \"{}\" in {}",
							  record, path));
		// A "wall=<dir>" decoration hangs on that wall, so it must be solid.
		if (const std::string* w = e.Param("wall")) {
			Direction wd = Direction::North;
			DN_ASSERT(ParseDirection(*w, wd),
					  std::format("bad wall \"{}\": \"{}\" in {}", *w, record, path));
			DN_ASSERT(!IsWalkable(e.x + DirDX(wd), e.z + DirDZ(wd)),
					  std::format("wall-mounted decoration faces open floor, not a wall: "
								  "\"{}\" in {}", record, path));
		}
		m_decorations.push_back(std::move(e));
	}
	DN_ASSERT(!m_wallPalette.empty() && !m_floorPalette.empty() &&
				  !m_ceilingPalette.empty(),
			  "map must declare its surface palettes (palette <wall|floor|ceiling> "
			  "<id> ...): " + path);

	// Resolve each sconce's mount wall now the whole grid is known: an explicit
	// facing must point at solid rock; otherwise take the first solid neighbour
	// (N, E, S, W), defaulting north.
	constexpr Direction kScan[4] = {Direction::North, Direction::East,
									Direction::South, Direction::West};
	for (const RawSconce& rs : rawSconces) {
		Direction wall = Direction::North;
		if (rs.hasWall) {
			wall = rs.wall;
			DN_ASSERT(!IsWalkable(rs.x + DirDX(wall), rs.z + DirDZ(wall)),
					  std::format("sconce at {},{} faces open floor, not a wall, in {}",
								  rs.x, rs.z, path));
		} else {
			for (const Direction d : kScan)
				if (!IsWalkable(rs.x + DirDX(d), rs.z + DirDZ(d))) {
					wall = d;
					break;
				}
		}
		m_torches.push_back({rs.x, rs.z, wall, rs.lit, rs.brightness, rs.turbidity});
	}

	// Fires thicken the air around them (braziers more than sconces); recomputed
	// from the authored dusty base + every brazier + every lit sconce's own smoke.
	RebuildTurbidity();

	log::Info("Loaded map {}: {}x{}, {} torches, {} braziers, {} decorations",
			  path, m_width, m_height, m_torches.size(), m_braziers.size(),
			  m_decorations.size());
}

// "palette <wall|floor|ceiling> <id> [...]" — the level's surface palette as a
// list of catalog ids (project catalog/{walls,floors,ceilings}.cat). DungeonWorld
// resolves each id to a texture set + worn block mesh, so a level pays for exactly
// the materials it uses.
void DungeonMap::ParsePaletteRecord(const std::string& record, const std::string& path) {
	const std::vector<std::string_view> tokens = SplitRecordTokens(record);
	DN_ASSERT(tokens.size() >= 3,
			  std::format("palette record needs <wall|floor|ceiling> and at least "
						  "one id: \"{}\" in {}", record, path));

	std::vector<std::string>* list = nullptr;
	if (tokens[1] == "wall") list = &m_wallPalette;
	else if (tokens[1] == "floor") list = &m_floorPalette;
	else if (tokens[1] == "ceiling") list = &m_ceilingPalette;
	DN_ASSERT(list != nullptr,
			  std::format("unknown surface \"{}\" (wall, floor, or ceiling): \"{}\" in {}",
						  tokens[1], record, path));
	DN_ASSERT(list->empty(),
			  std::format("duplicate \"palette {}\" record in {}", tokens[1], path));
	list->assign(tokens.begin() + 2, tokens.end());
}

// "stairs <type> <x> <z> [facing] dest=<level> destx=<n> destz=<n>
// [destfacing=<dir>]" — a portal on a floor cell that transitions to another
// level when the party steps onto it (P6). `type` is a stairs.cat id.
void DungeonMap::ParseStairRecord(const std::string& record, const std::string& path) {
	const std::vector<std::string_view> tok = SplitRecordTokens(record);
	DN_ASSERT(tok.size() >= 4,
			  std::format("stairs needs <type> <x> <z>: \"{}\" in {}", record, path));
	const auto coord = [&](std::string_view t) {
		int v = 0;
		const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
		DN_ASSERT(ec == std::errc{} && end == t.data() + t.size(),
				  std::format("bad stairs number \"{}\": \"{}\" in {}", t, record, path));
		return v;
	};

	StairLink s;
	s.type = std::string(tok[1]);
	s.x = coord(tok[2]);
	s.z = coord(tok[3]);
	DN_ASSERT(IsWalkable(s.x, s.z),
			  std::format("stairs out of bounds or in solid rock: \"{}\" in {}",
						  record, path));

	size_t i = 4;
	// Optional facing: a bare direction token (key=value params have no facing).
	if (tok.size() > i && tok[i].find('=') == std::string_view::npos) {
		DN_ASSERT(ParseDirection(tok[i], s.facing),
				  std::format("bad stairs facing \"{}\": \"{}\" in {}", tok[i], record, path));
		++i;
	}
	for (; i < tok.size(); ++i) {
		const std::string_view kv = tok[i];
		const size_t eq = kv.find('=');
		DN_ASSERT(eq != std::string_view::npos,
				  std::format("stairs param needs key=value: \"{}\" in {}", kv, path));
		const std::string_view key = kv.substr(0, eq), val = kv.substr(eq + 1);
		if (key == "dest") s.destLevel = std::string(val);
		else if (key == "destx") s.destX = coord(val);
		else if (key == "destz") s.destZ = coord(val);
		else if (key == "destfacing")
			DN_ASSERT(ParseDirection(val, s.destFacing),
					  std::format("bad destfacing \"{}\": \"{}\" in {}", val, record, path));
		else
			DN_ASSERT(false,
					  std::format("unknown stairs param \"{}\": \"{}\" in {}", key, record, path));
	}
	DN_ASSERT(!s.destLevel.empty(),
			  std::format("stairs needs dest=<level>: \"{}\" in {}", record, path));
	m_stairs.push_back(std::move(s));
}

// "variant <wall|floor|ceiling> <x> <z> <index>" — a per-cell surface variant
// override (the editor pins a cell to a specific palette index; the writer emits
// these for painted cells). Parsed after the grid is built.
void DungeonMap::ParseVariantRecord(const std::string& record, const std::string& path) {
	const std::vector<std::string_view> tok = SplitRecordTokens(record);
	DN_ASSERT(tok.size() >= 5,
			  std::format("variant needs <wall|floor|ceiling> <x> <z> <index>: \"{}\" in {}",
						  record, path));
	const auto num = [&](std::string_view t) {
		int v = 0;
		const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
		DN_ASSERT(ec == std::errc{} && end == t.data() + t.size(),
				  std::format("bad variant number \"{}\": \"{}\" in {}", t, record, path));
		return v;
	};
	const int x = num(tok[2]), z = num(tok[3]), idx = num(tok[4]);
	if (tok[1] == "wall") SetWallVariant(x, z, idx);
	else if (tok[1] == "floor") SetFloorVariant(x, z, idx);
	else if (tok[1] == "ceiling") SetCeilingVariant(x, z, idx);
	else
		DN_ASSERT(false, std::format("unknown variant surface \"{}\": \"{}\" in {}",
									 tok[1], record, path));
}

// Fires raise the air turbidity of their own square and the squares nearby
// (smoke hangs around flames). Chebyshev rings: full / half / quarter.
void DungeonMap::RebuildTurbidity() {
	// Reset to the authored dusty base ('D' cells = 1.0), then re-add fire smoke.
	for (size_t i = 0; i < m_turbidity.size(); ++i)
		m_turbidity[i] = m_dusty[i] ? 1.0f : 0.0f;
	for (const auto& [bx, bz] : m_braziers) AddFireTurbidity(bx, bz, 0.55f);
	for (const WallSconce& s : m_torches)
		if (s.lit) AddFireTurbidity(s.x, s.z, s.turbidity);
	++m_revision;
}

bool DungeonMap::SetSconceProps(int x, int z, Direction wall, bool lit, float brightness,
								float turbidity) {
	for (WallSconce& s : m_torches)
		if (s.x == x && s.z == z && s.wall == wall) {
			s.lit = lit;
			s.brightness = brightness;
			s.turbidity = turbidity;
			RebuildTurbidity(); // bumps Revision()
			return true;
		}
	return false;
}

void DungeonMap::AddFireTurbidity(int x, int z, float amount) {
	for (int dz = -2; dz <= 2; ++dz) {
		for (int dx = -2; dx <= 2; ++dx) {
			const int cx = x + dx, cz = z + dz;
			if (!IsWalkable(cx, cz)) continue;
			const int ring = std::max(std::abs(dx), std::abs(dz));
			const float weight = ring == 0 ? 1.0f : (ring == 1 ? 0.5f : 0.22f);
			float& cell = m_turbidity[static_cast<size_t>(cz) * m_width + cx];
			cell = std::min(1.0f, cell + amount * weight);
		}
	}
}

Cell DungeonMap::At(int x, int z) const {
	if (x < 0 || z < 0 || x >= m_width || z >= m_height) return Cell::Wall;
	return m_cells[static_cast<size_t>(z) * m_width + x];
}

bool DungeonMap::IsWalkable(int x, int z) const { return At(x, z) == Cell::Floor; }

bool DungeonMap::AddSconce(int x, int z) {
	if (!IsWalkable(x, z)) return false;
	// Mount on the first solid neighbour (N, E, S, W) — same rule as a 'T' glyph.
	constexpr Direction kScan[4] = {Direction::North, Direction::East,
									Direction::South, Direction::West};
	for (const Direction d : kScan)
		if (!IsWalkable(x + DirDX(d), z + DirDZ(d))) {
			m_torches.push_back({x, z, d});
			AddFireTurbidity(x, z, 0.28f);
			++m_revision;
			return true;
		}
	return false; // no wall to hang on
}

bool DungeonMap::SetSconceWall(int x, int z, Direction from, Direction to) {
	if (from == to) return true;
	if (IsWalkable(x + DirDX(to), z + DirDZ(to))) return false; // target wall not solid
	for (WallSconce& s : m_torches)
		if (s.x == x && s.z == z && s.wall == from) {
			s.wall = to;
			++m_revision;
			return true;
		}
	return false;
}

bool DungeonMap::AddBrazier(int x, int z) {
	if (!IsWalkable(x, z)) return false;
	m_braziers.emplace_back(x, z);
	AddFireTurbidity(x, z, 0.55f);
	++m_revision;
	return true;
}

void DungeonMap::SetCell(int x, int z, Cell cell) {
	if (x < 0 || z < 0 || x >= m_width || z >= m_height) return;
	Cell& slot = m_cells[static_cast<size_t>(z) * m_width + x];
	if (slot == cell) return;
	slot = cell;
	++m_revision;
}

} // namespace dungeon::game
