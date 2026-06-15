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

	// Sconces are gathered raw (from 'T' glyphs and fixture records) and have
	// their mount wall resolved once the whole grid is known — a glyph or an
	// unfaced record auto-detects, an explicit facing is taken as-is.
	struct RawSconce {
		int x, z;
		bool hasWall;
		Direction wall;
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
			case 'D': m_turbidity[static_cast<size_t>(z) * m_width + x] = 1.0f; break;
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

	// Records: texture palettes and static decorations.
	for (const std::string& record : records) {
		if (record.starts_with("textures")) {
			ParseTextureRecord(record, path);
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
				RawSconce rs{fx, fz, false, Direction::North};
				if (tok.size() >= 5) {
					DN_ASSERT(ParseDirection(tok[4], rs.wall),
							  std::format("bad sconce facing \"{}\": \"{}\" in {}",
										  tok[4], record, path));
					rs.hasWall = true;
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
	DN_ASSERT(!m_wallTextures.empty() && !m_floorTextures.empty() &&
				  !m_ceilingTextures.empty(),
			  "map must declare its texture palettes (textures <wall|floor|ceiling> "
			  "<set> ...): " + path);

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
		m_torches.push_back({rs.x, rs.z, wall});
	}

	// Fires thicken the air around them (braziers more than sconces).
	for (const auto& [bx, bz] : m_braziers) AddFireTurbidity(bx, bz, 0.55f);
	for (const WallSconce& s : m_torches) AddFireTurbidity(s.x, s.z, 0.28f);

	log::Info("Loaded map {}: {}x{}, {} torches, {} braziers, {} decorations",
			  path, m_width, m_height, m_torches.size(), m_braziers.size(),
			  m_decorations.size());
}

// "textures <wall|floor|ceiling> <set> [...]" — the level's surface texture
// palette. The game loads only the sets named here (and their worn block
// meshes), so a level pays for exactly the materials it uses.
void DungeonMap::ParseTextureRecord(const std::string& record, const std::string& path) {
	const std::vector<std::string_view> tokens = SplitRecordTokens(record);
	DN_ASSERT(tokens.size() >= 3,
			  std::format("texture record needs <wall|floor|ceiling> and at least "
						  "one set: \"{}\" in {}", record, path));

	std::vector<std::string>* list = nullptr;
	if (tokens[1] == "wall") list = &m_wallTextures;
	else if (tokens[1] == "floor") list = &m_floorTextures;
	else if (tokens[1] == "ceiling") list = &m_ceilingTextures;
	DN_ASSERT(list != nullptr,
			  std::format("unknown surface \"{}\" (wall, floor, or ceiling): \"{}\" in {}",
						  tokens[1], record, path));
	DN_ASSERT(list->empty(),
			  std::format("duplicate \"textures {}\" record in {}", tokens[1], path));
	list->assign(tokens.begin() + 2, tokens.end());
}

// Fires raise the air turbidity of their own square and the squares nearby
// (smoke hangs around flames). Chebyshev rings: full / half / quarter.
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

void DungeonMap::SetCell(int x, int z, Cell cell) {
	if (x < 0 || z < 0 || x >= m_width || z >= m_height) return;
	Cell& slot = m_cells[static_cast<size_t>(z) * m_width + x];
	if (slot == cell) return;
	slot = cell;
	++m_revision;
}

} // namespace dungeon::game
