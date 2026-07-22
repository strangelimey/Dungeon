#include "Game/DungeonMap.h"

#include "Assets/File.h"
#include "Core/Assert.h"
#include "Core/Log.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace dungeon::game {

DungeonMap::DungeonMap(const std::string& path, FixtureTypes fixtures) {
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
		std::string type = "sconce"; // fixtures.cat id (glyphs take the default)
	};
	std::vector<RawSconce> rawSconces;
	// A record's kind token routes by the catalog's mount field (wall-mount ids
	// become sconces, everything else stands on the floor).
	const auto isWallMount = [&](std::string_view id) {
		return std::find(fixtures.wallMount.begin(), fixtures.wallMount.end(),
						 id) != fixtures.wallMount.end();
	};

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
			case 'T':
				rawSconces.push_back({x, z, false, Direction::North, true,
									  kSconceBrightness, kSconceTurbidity,
									  fixtures.sconceDefault});
				break;
			case 'F': {
				FloorBrazier b{x, z};
				b.type = fixtures.brazierDefault;
				m_braziers.push_back(std::move(b));
				break;
			}
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
			// fixture <sconce|brazier> <x> <z> [facing] [lit=0|1] [bright=<cells>] [turb=<f>]
			// (facing names the wall a sconce mounts on; a brazier has no wall and
			// ignores it — tolerated so the two kinds share one grammar).
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
			// Light/smoke tail shared by both kinds: a malformed number aborts loudly
			// (it must never silently become 0 = an invisible light), lit reads like
			// Serialize's GetBool (0/f/F = false), and both values are clamped to
			// sane ranges so a hand-authored bright=0 can't degenerate the shadow
			// projection (1/radius) downstream.
			const auto num = [&](std::string_view t) {
				float v = 0.0f;
				const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
				DN_ASSERT(ec == std::errc{} && end == t.data() + t.size(),
						  std::format("bad fixture number \"{}\": \"{}\" in {}", t, record,
									  path));
				return v;
			};
			const auto fireKey = [&](std::string_view t, bool& lit, float& brightness,
									 float& turbidity) {
				const size_t eq = t.find('=');
				DN_ASSERT(eq != std::string_view::npos,
						  std::format("expected facing or key=value, got \"{}\": \"{}\" in {}",
									  t, record, path));
				const std::string_view key = t.substr(0, eq), val = t.substr(eq + 1);
				if (key == "lit") lit = val.empty() || (val.front() != '0' &&
														val.front() != 'f' && val.front() != 'F');
				else if (key == "bright") brightness = std::max(num(val), kFixtureMinBrightness);
				else if (key == "turb") turbidity = std::clamp(num(val), 0.0f, 1.0f);
				else
					DN_ASSERT(false, std::format("unknown fixture key \"{}\": \"{}\" in {}",
												 key, record, path));
			};
			// The kind token is a fixtures.cat id; the catalog's mount field
			// (threaded in as FixtureTypes) decides wall vs floor. An unknown
			// id stands on the floor — DungeonWorld's kind resolution warns
			// there, where the catalog is actually known.
			if (isWallMount(tok[1])) {
				RawSconce rs{fx, fz, false, Direction::North};
				rs.type = std::string(tok[1]);
				for (size_t i = 4; i < tok.size(); ++i) {
					Direction d;
					if (ParseDirection(tok[i], d)) {
						rs.wall = d;
						rs.hasWall = true;
						continue;
					}
					fireKey(tok[i], rs.lit, rs.brightness, rs.turbidity);
				}
				rawSconces.push_back(std::move(rs));
			} else {
				FloorBrazier b{fx, fz};
				b.type = std::string(tok[1]);
				for (size_t i = 4; i < tok.size(); ++i) {
					Direction d;
					if (ParseDirection(tok[i], d)) continue; // no wall to mount on — ignored
					fireKey(tok[i], b.lit, b.brightness, b.turbidity);
				}
				m_braziers.push_back(std::move(b));
			}
			continue;
		}
		if (record.starts_with("niche")) {
			// niche [<type>] <x> <z> [facing] — a recessed pocket on a solid wall of
			// a walkable cell. The optional first token is the wallfeatures.cat id
			// (absent = "niche"; detected by whether it parses as a coordinate, so
			// old untyped records still load). facing names the wall (else the first
			// solid neighbour, the sconce mount rule). A niche that no longer faces
			// solid rock is dropped (tolerant, unlike a decoration's hard assert).
			const std::vector<std::string_view> tok = SplitRecordTokens(record);
			const auto asInt = [](std::string_view t, int& out) {
				const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), out);
				return ec == std::errc{} && end == t.data() + t.size();
			};
			// tok[1] is the type unless it parses as the x coordinate (untyped form).
			int probe = 0;
			const bool typed = tok.size() >= 2 && !asInt(tok[1], probe);
			const size_t c = typed ? 2 : 1; // index of the x token
			std::string type = typed ? std::string(tok[1]) : "niche";
			DN_ASSERT(tok.size() >= c + 2,
					  std::format("niche needs <x> <z>: \"{}\" in {}", record, path));
			int nx = 0, nz = 0;
			DN_ASSERT(asInt(tok[c], nx) && asInt(tok[c + 1], nz),
					  std::format("bad niche coordinate: \"{}\" in {}", record, path));
			if (!IsWalkable(nx, nz)) continue; // buried — drop it
			// The facing token is optional; key=value params (name=, hidden=) may
			// follow it OR take its slot, so scan from the first post-coordinate
			// token and treat a bare direction as the wall.
			Direction wall = Direction::North;
			bool haveWall = false;
			std::string name;
			bool hidden = false;
			for (size_t i = c + 2; i < tok.size(); ++i) {
				Direction d;
				if (!haveWall && ParseDirection(tok[i], d)) {
					if (!IsWalkable(nx + DirDX(d), nz + DirDZ(d))) {
						wall = d;
						haveWall = true;
					}
					continue;
				}
				const size_t eq = tok[i].find('=');
				if (eq == std::string_view::npos) continue; // unknown bare token
				const std::string_view key = tok[i].substr(0, eq), val = tok[i].substr(eq + 1);
				if (key == "name") name = std::string(val);
				else if (key == "hidden")
					hidden = val.empty() || (val.front() != '0' && val.front() != 'f');
			}
			if (!haveWall && !FreeNicheWall(nx, nz, wall)) continue; // no solid wall
			m_niches.push_back({nx, nz, wall, std::move(type), std::move(name), hidden,
								/*open=*/!hidden});
			continue;
		}
		if (record.starts_with("bore")) {
			// bore [<type>] <x> <z> <axis> — a see-through hole through a solid wall
			// block (axis 0 = X, 1 = Z; optional type = the wallfeatures.cat bore
			// shape, absent = "window"). Dropped if the cell isn't a solid wall
			// whose two flanking cells on that axis are floor (tolerant, like niches).
			const std::vector<std::string_view> tok = SplitRecordTokens(record);
			const auto asInt = [](std::string_view t, int& out) {
				const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), out);
				return ec == std::errc{} && end == t.data() + t.size();
			};
			int probe = 0;
			const bool typed = tok.size() >= 2 && !asInt(tok[1], probe);
			const size_t c = typed ? 2 : 1; // index of the x token
			std::string type = typed ? std::string(tok[1]) : "window";
			int bx = 0, bz = 0, axis = 0;
			DN_ASSERT(tok.size() >= c + 3 && asInt(tok[c], bx) && asInt(tok[c + 1], bz) &&
						  asInt(tok[c + 2], axis),
					  std::format("bore needs <x> <z> <axis>: \"{}\" in {}", record, path));
			if (IsWalkable(bx, bz)) continue; // not a wall
			const bool ok = axis == 0 ? (IsWalkable(bx - 1, bz) && IsWalkable(bx + 1, bz))
									  : (IsWalkable(bx, bz - 1) && IsWalkable(bx, bz + 1));
			if (!ok) continue; // flanks aren't both floor — nothing to see through
			m_bores.push_back({bx, bz, axis, std::move(type)});
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
		if (record.starts_with("atmosphere")) {
			// atmosphere [dust=<density>] [haze=<x>] [ambient=<x>] — the level's
			// mood knobs (any subset; unset keys keep the world defaults). The
			// editor's Level settings dialog authors this record.
			const std::vector<std::string_view> tok = SplitRecordTokens(record);
			for (size_t i = 1; i < tok.size(); ++i) {
				const size_t eq = tok[i].find('=');
				DN_ASSERT(eq != std::string_view::npos,
						  std::format("expected key=value, got \"{}\": \"{}\" in {}",
									  tok[i], record, path));
				const std::string_view key = tok[i].substr(0, eq);
				const std::string_view val = tok[i].substr(eq + 1);
				float v = 0.0f;
				const auto [p, ec] = std::from_chars(val.data(), val.data() + val.size(), v);
				DN_ASSERT(ec == std::errc{} && p == val.data() + val.size(),
						  std::format("bad atmosphere number \"{}\": \"{}\" in {}", val,
									  record, path));
				if (key == "dust") m_dustDensity = std::max(0.0f, v);
				else if (key == "haze") m_hazeAmbient = std::max(0.0f, v);
				else if (key == "ambient") m_ambientScale = std::clamp(v, 0.0f, 10.0f);
				else
					DN_ASSERT(false, std::format("unknown atmosphere key \"{}\": \"{}\" in {}",
												 key, record, path));
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
		m_torches.push_back(
			{rs.x, rs.z, wall, rs.lit, rs.brightness, rs.turbidity, rs.type});
	}

	// Fires thicken the air around them (braziers more than sconces); recomputed
	// from the authored dusty base + every LIT fixture's own smoke.
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
	// Wall variants belong to SOLID cells (the block owns its texture);
	// floor/ceiling variants to walkable ones. A record on the wrong cell type
	// is stale — files from the old floor-cell wall model, or a cell repainted
	// structurally since — and is dropped here, so the next save writes it out.
	if (tok[1] == "wall") { if (!IsWalkable(x, z)) SetWallVariant(x, z, idx); }
	else if (tok[1] == "floor") { if (IsWalkable(x, z)) SetFloorVariant(x, z, idx); }
	else if (tok[1] == "ceiling") { if (IsWalkable(x, z)) SetCeilingVariant(x, z, idx); }
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
	for (const FloorBrazier& b : m_braziers)
		if (b.lit) AddFireTurbidity(b.x, b.z, b.turbidity);
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

bool DungeonMap::SetBrazierProps(int x, int z, bool lit, float brightness, float turbidity) {
	for (FloorBrazier& b : m_braziers)
		if (b.x == x && b.z == z) {
			b.lit = lit;
			b.brightness = brightness;
			b.turbidity = turbidity;
			RebuildTurbidity(); // bumps Revision()
			return true;
		}
	return false;
}

const FloorBrazier* DungeonMap::BrazierAt(int x, int z) const {
	for (const FloorBrazier& b : m_braziers)
		if (b.x == x && b.z == z) return &b;
	return nullptr;
}

bool DungeonMap::RemoveFixtureAt(int x, int z) {
	// Brazier first: it is the cell-centre marker an erase click aims at; a
	// sconce (edge marker) only goes once the cell has no brazier. Removal is
	// single-shot — one fixture per call, first match in list order.
	for (size_t i = 0; i < m_braziers.size(); ++i)
		if (m_braziers[i].x == x && m_braziers[i].z == z) {
			m_braziers.erase(m_braziers.begin() + static_cast<ptrdiff_t>(i));
			RebuildTurbidity();
			return true;
		}
	for (size_t i = 0; i < m_torches.size(); ++i)
		if (m_torches[i].x == x && m_torches[i].z == z) {
			m_torches.erase(m_torches.begin() + static_cast<ptrdiff_t>(i));
			RebuildTurbidity();
			return true;
		}
	return false;
}

bool DungeonMap::PruneFixturesForCell(int x, int z) {
	bool changed = false;
	if (!IsWalkable(x, z)) {
		// Cell painted solid: nothing can stand on it any more.
		changed |= std::erase_if(m_torches, [&](const WallSconce& s) {
					   return s.x == x && s.z == z;
				   }) > 0;
		changed |= std::erase_if(m_braziers, [&](const FloorBrazier& b) {
					   return b.x == x && b.z == z;
				   }) > 0;
		changed |= std::erase_if(m_niches, [&](const WallNiche& n) {
					   return n.x == x && n.z == z;
				   }) > 0;
	} else {
		// Cell painted open: sconces that hung on it face open floor now. Re-mount
		// each on a free solid wall of its own cell, else drop it.
		for (size_t i = m_torches.size(); i-- > 0;) {
			WallSconce& s = m_torches[i];
			if (s.x + DirDX(s.wall) != x || s.z + DirDZ(s.wall) != z) continue;
			Direction d;
			if (FreeSconceWall(s.x, s.z, d)) s.wall = d;
			else m_torches.erase(m_torches.begin() + static_cast<ptrdiff_t>(i));
			changed = true;
		}
		// A niche whose wall was painted open loses its backing rock — re-mount on
		// a free solid wall of its own cell, else drop it (no light to rebuild).
		for (size_t i = m_niches.size(); i-- > 0;) {
			WallNiche& n = m_niches[i];
			if (n.x + DirDX(n.wall) != x || n.z + DirDZ(n.wall) != z) continue;
			Direction d;
			if (FreeNicheWall(n.x, n.z, d)) n.wall = d;
			else m_niches.erase(m_niches.begin() + static_cast<ptrdiff_t>(i));
			changed = true;
		}
	}
	// A bore whose cell is no longer a solid wall between two floor cells (on its
	// axis) is invalid — the edit that walkable-painted its cell or a flank drops
	// it. Cheap to revalidate every bore (there are few).
	changed |= std::erase_if(m_bores, [&](const WallBore& b) {
				   if (IsWalkable(b.x, b.z)) return true;
				   return b.axis == 0
							  ? !(IsWalkable(b.x - 1, b.z) && IsWalkable(b.x + 1, b.z))
							  : !(IsWalkable(b.x, b.z - 1) && IsWalkable(b.x, b.z + 1));
			   }) > 0;
	if (changed) RebuildTurbidity(); // bumps Revision() (re-stamps the chunk too)
	return changed;
}

void DungeonMap::AddFireTurbidity(int x, int z, float amount) {
	for (int dz = -2; dz <= 2; ++dz) {
		for (int dx = -2; dx <= 2; ++dx) {
			const int cx = x + dx, cz = z + dz;
			if (!IsWalkable(cx, cz)) continue;
			const int ring = std::max(std::abs(dx), std::abs(dz));
			const float weight = ring == 0 ? 1.0f : (ring == 1 ? 0.5f : 0.22f);
			float& cell = m_turbidity[static_cast<size_t>(cz) * m_width + cx];
			// Opacity-style accumulation: overlapping fires SATURATE instead of
			// summing (a second torch thickens the air by what's still clear,
			// not by its full amount), so a sconce-lined hall stays hazy rather
			// than maxing out to authored-'D' thickness and burying its surface
			// shadows under in-scatter. 'D' cells sit at 1.0 and are unchanged
			// by any accumulation rule.
			cell = 1.0f - (1.0f - cell) * (1.0f - amount * weight);
		}
	}
}

Cell DungeonMap::At(int x, int z) const {
	if (x < 0 || z < 0 || x >= m_width || z >= m_height) return Cell::Wall;
	return m_cells[static_cast<size_t>(z) * m_width + x];
}

bool DungeonMap::IsWalkable(int x, int z) const { return At(x, z) == Cell::Floor; }

bool DungeonMap::FreeSconceWall(int x, int z, Direction& out) const {
	constexpr Direction kScan[4] = {Direction::North, Direction::East,
									Direction::South, Direction::West};
	for (const Direction d : kScan) {
		if (IsWalkable(x + DirDX(d), z + DirDZ(d))) continue; // needs a solid wall
		bool taken = false;
		for (const WallSconce& s : m_torches)
			if (s.x == x && s.z == z && s.wall == d) {
				taken = true;
				break;
			}
		if (taken) continue;
		out = d;
		return true;
	}
	return false;
}

bool DungeonMap::AddSconce(int x, int z, std::string type, bool lit) {
	if (!IsWalkable(x, z)) return false;
	// No face named (the 'T' glyph, or a map load): mount on the first free solid
	// neighbour (N, E, S, W). The editor names the face explicitly instead.
	Direction d;
	if (!FreeSconceWall(x, z, d)) return false; // no free wall to hang on
	return AddSconce(x, z, std::move(type), lit, d);
}

bool DungeonMap::AddSconce(int x, int z, std::string type, bool lit,
						   Direction wall) {
	if (!IsWalkable(x, z)) return false;
	if (IsWalkable(x + DirDX(wall), z + DirDZ(wall))) return false; // nothing to hang on
	for (const WallSconce& s : m_torches)
		if (s.x == x && s.z == z && s.wall == wall) return false; // face already used
	m_torches.push_back({x, z, wall, lit, kSconceBrightness, kSconceTurbidity,
						 std::move(type)});
	RebuildTurbidity(); // bumps Revision()
	return true;
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

bool DungeonMap::AddBrazier(int x, int z, std::string type, bool lit) {
	// One per cell: every per-instance surface (inspect, edit, erase) addresses
	// a brazier by its cell, so a stacked twin would be an uneditable ghost.
	if (!IsWalkable(x, z) || BrazierAt(x, z)) return false;
	m_braziers.push_back(
		{x, z, lit, kBrazierBrightness, kBrazierTurbidity, std::move(type)});
	RebuildTurbidity(); // bumps Revision()
	return true;
}

bool DungeonMap::FreeNicheWall(int x, int z, Direction& out) const {
	constexpr Direction kScan[4] = {Direction::North, Direction::East,
									Direction::South, Direction::West};
	for (const Direction d : kScan) {
		if (IsWalkable(x + DirDX(d), z + DirDZ(d))) continue; // needs a solid wall
		bool taken = false;
		for (const WallNiche& n : m_niches)
			if (n.x == x && n.z == z && n.wall == d) {
				taken = true;
				break;
			}
		if (taken) continue;
		out = d;
		return true;
	}
	return false;
}

const WallNiche* DungeonMap::NicheAt(int x, int z, int dx, int dz) const {
	for (const WallNiche& n : m_niches)
		if (n.x == x && n.z == z && DirDX(n.wall) == dx && DirDZ(n.wall) == dz)
			return &n;
	return nullptr;
}

bool DungeonMap::AddNiche(int x, int z, std::string type) {
	// No face named (map load, or a caller with nothing to point at): fall back
	// to the first free solid wall. The editor names the face explicitly.
	if (!IsWalkable(x, z)) return false;
	Direction d;
	if (!FreeNicheWall(x, z, d)) return false; // no free solid wall to carve into
	return AddNiche(x, z, std::move(type), d);
}

bool DungeonMap::AddNiche(int x, int z, std::string type, Direction wall) {
	if (!IsWalkable(x, z)) return false;
	if (IsWalkable(x + DirDX(wall), z + DirDZ(wall))) return false; // no rock to carve
	if (NicheAt(x, z, DirDX(wall), DirDZ(wall))) return false;      // face already used
	m_niches.push_back({x, z, wall, std::move(type)});
	++m_revision; // the mesh builder re-stamps this cell's panel as a niche
	return true;
}

bool DungeonMap::RemoveNiche(int x, int z, Direction wall) {
	for (size_t i = 0; i < m_niches.size(); ++i)
		if (m_niches[i].x == x && m_niches[i].z == z && m_niches[i].wall == wall) {
			m_niches.erase(m_niches.begin() + static_cast<ptrdiff_t>(i));
			++m_revision;
			return true;
		}
	return false;
}

bool DungeonMap::RemoveNicheFacingWall(int wx, int wz) {
	if (IsWalkable(wx, wz)) return false; // must be a solid wall block
	const int nbr[4][2] = {{wx, wz - 1}, {wx + 1, wz}, {wx, wz + 1}, {wx - 1, wz}};
	for (const auto& f : nbr)
		if (const WallNiche* n = NicheAt(f[0], f[1], wx - f[0], wz - f[1]))
			return RemoveNiche(f[0], f[1], n->wall);
	return false;
}

std::vector<Direction> DungeonMap::NicheWallsAt(int x, int z) const {
	std::vector<Direction> walls;
	for (const WallNiche& n : m_niches)
		if (n.x == x && n.z == z) walls.push_back(n.wall);
	return walls;
}

bool DungeonMap::SetNichePropsAt(int x, int z, Direction wall, std::string name,
								 bool hidden, std::string type) {
	for (WallNiche& n : m_niches)
		if (n.x == x && n.z == z && n.wall == wall) {
			n.name = std::move(name);
			n.hidden = hidden;
			n.open = !hidden; // the authored start state changed; reset runtime open
			n.type = std::move(type);
			++m_revision;
			return true;
		}
	return false;
}

bool DungeonMap::SetNicheOpenAt(int x, int z, Direction wall, bool open) {
	for (WallNiche& n : m_niches)
		if (n.x == x && n.z == z && n.wall == wall) {
			if (n.open != open) {
				n.open = open;
				++m_revision;
			}
			return true;
		}
	return false;
}

bool DungeonMap::ResetNicheOpen() {
	bool changed = false;
	for (WallNiche& n : m_niches)
		if (n.open != !n.hidden) {
			n.open = !n.hidden;
			changed = true;
		}
	if (changed) ++m_revision;
	return changed;
}

std::vector<std::pair<int, int>> DungeonMap::ToggleNichesNamed(const std::string& name) {
	std::vector<std::pair<int, int>> touched;
	if (name.empty()) return touched;
	for (WallNiche& n : m_niches)
		if (n.name == name) {
			n.open = !n.open;
			touched.emplace_back(n.x, n.z);
		}
	if (!touched.empty()) ++m_revision;
	return touched;
}

const WallBore* DungeonMap::BoreAlong(int x, int z, int axis) const {
	for (const WallBore& b : m_bores)
		if (b.x == x && b.z == z && b.axis == axis) return &b;
	return nullptr;
}

bool DungeonMap::AddBore(std::string type, int x, int z) {
	if (IsWalkable(x, z)) return false; // a bore is through a SOLID wall block
	int axis = -1;
	if (IsWalkable(x - 1, z) && IsWalkable(x + 1, z)) axis = 0;      // floor E+W → X
	else if (IsWalkable(x, z - 1) && IsWalkable(x, z + 1)) axis = 1; // floor N+S → Z
	if (axis < 0) return false;                                     // not a 1-block wall
	for (const WallBore& b : m_bores)
		if (b.x == x && b.z == z) return false; // already bored
	m_bores.push_back({x, z, axis, std::move(type)});
	++m_revision;
	return true;
}

bool DungeonMap::RemoveBoreAt(int x, int z) {
	for (size_t i = 0; i < m_bores.size(); ++i)
		if (m_bores[i].x == x && m_bores[i].z == z) {
			m_bores.erase(m_bores.begin() + static_cast<ptrdiff_t>(i));
			++m_revision;
			return true;
		}
	return false;
}

std::vector<std::string> DungeonMap::NicheNames() const {
	std::vector<std::string> names;
	for (const WallNiche& n : m_niches)
		if (!n.name.empty() &&
			std::find(names.begin(), names.end(), n.name) == names.end())
			names.push_back(n.name);
	return names;
}

bool DungeonMap::RemoveDecorationRecordAt(int x, int z) {
	for (size_t i = 0; i < m_decorations.size(); ++i)
		if (m_decorations[i].x == x && m_decorations[i].z == z) {
			m_decorations.erase(m_decorations.begin() + static_cast<ptrdiff_t>(i));
			return true;
		}
	return false;
}

size_t DungeonMap::RemoveDecorationRecordsAt(int x, int z) {
	return std::erase_if(m_decorations,
						 [&](const Entity& e) { return e.x == x && e.z == z; });
}

bool DungeonMap::AddStair(const StairLink& link) {
	if (!IsWalkable(link.x, link.z) || StairAt(link.x, link.z)) return false;
	m_stairs.push_back(link);
	return true;
}

const StairLink* DungeonMap::StairAt(int x, int z) const {
	for (const StairLink& s : m_stairs)
		if (s.x == x && s.z == z) return &s;
	return nullptr;
}

bool DungeonMap::RemoveStair(int x, int z, StairLink* removed) {
	for (size_t i = 0; i < m_stairs.size(); ++i)
		if (m_stairs[i].x == x && m_stairs[i].z == z) {
			if (removed) *removed = m_stairs[i];
			m_stairs.erase(m_stairs.begin() + static_cast<ptrdiff_t>(i));
			return true;
		}
	return false;
}

size_t DungeonMap::RenameStairDest(const std::string& oldStem,
								   const std::string& newStem) {
	size_t n = 0;
	for (StairLink& s : m_stairs)
		if (s.destLevel == oldStem) {
			s.destLevel = newStem;
			++n;
		}
	return n;
}

void DungeonMap::SetCell(int x, int z, Cell cell) {
	if (x < 0 || z < 0 || x >= m_width || z >= m_height) return;
	Cell& slot = m_cells[static_cast<size_t>(z) * m_width + x];
	if (slot == cell) return;
	slot = cell;
	++m_revision;
}

} // namespace dungeon::game
