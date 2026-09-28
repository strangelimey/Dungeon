// ============================================================================
// Game/DungeonWorld_Resize.cpp - resizing a level (play-test #3, Michael
// 2026-09-28: "adjust a level's width and height easily from within the editor
// screen", by dragging the map's edges). See DungeonWorld.h "resizing a level".
//
// THE SHAPE OF IT. The level is taken as a map + records, reframed (every grid
// remapped, every record shifted - DungeonMap::Reframe / DungeonEntities::
// Reframe), and handed to InstallLevel, the generator's in-place replace, which
// already copes with a level changing size under the running world (the fog
// mask, the walkable cache, the deferred rebake; two crashes taught it that).
//
// WHAT A LEFT OR TOP EDGE COSTS. Growing or trimming on the right and bottom
// changes no square's number. On the left or top every square is renumbered,
// and a square number is a cross-level reference: a stair and its pair stand on
// the SAME square on both floors, a stair elsewhere may point at a square here,
// a world-map doorway and the game's opening each name one. All of them move
// with the level - the pair halves physically, on their own floors, which is
// why a resize can be refused by a neighbouring floor's layout.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Core/Loc.h"

#include <algorithm>
#include <functional>
#include <optional>

namespace dungeon::game {

namespace {
// A level smaller than this cannot hold a floor square walled on every side; a
// bigger one than this is a typo in a drag, not a dungeon floor.
constexpr int kMinSide = 3;
constexpr int kMaxSide = 256;
} // namespace

bool DungeonWorld::ResizeLevel(const std::string& stem, int x0, int z0, int x1,
							   int z1) {
	auto say = [&](std::string_view line) {
		if (onMessage) onMessage(line);
	};
	const int w = x1 - x0, h = z1 - z0;
	if (w < kMinSide || h < kMinSide || w > kMaxSide || h > kMaxSide) {
		say(loc::Format("map.resize.size", w, h, kMinSide, kMaxSide));
		return false;
	}
	const bool live = stem == m_currentLevel;
	if (!live) {
		EnsureMapStash(stem);
		EnsureEntStash(stem);
	}
	// A level as it stands, for READING: live, stashed, or parsed from its file
	// into `temp` WITHOUT stashing it. The scan below reads every level (a stair
	// anywhere may point here), and a stash is a level with edits - every one
	// would be rewritten by the next save for nothing. Only the levels that
	// actually change are stashed, at the commit.
	auto readMap = [&](const std::string& level,
					   std::optional<DungeonMap>& temp) -> const DungeonMap& {
		if (level == m_currentLevel) return m_map;
		if (auto it = m_levelMaps.find(level); it != m_levelMaps.end()) return *it->second;
		temp.emplace(m_project.LevelMapPath(level), FixtureTypesOf(m_project));
		return *temp;
	};
	// For WRITING, once every level that changes has been stashed (a stash is
	// created by inserting into a flat_map, which would invalidate a reference
	// taken to another - so none is taken before then).
	auto mapOf = [&](const std::string& level) -> DungeonMap& {
		return level == m_currentLevel ? m_map : *m_levelMaps.find(level)->second;
	};

	// The level as it stands. The live one as its SAVE would write it - the
	// decorations synced back into records, and the .ent re-parsed from the text
	// SaveLevel writes, which is the one place that has every monster as it stands
	// (an editor-placed one has no record at all). A browsed one is its stash.
	DungeonMap map = mapOf(stem);
	if (live) map.SetDecorationRecords(LiveDecorationRecords());
	DungeonEntities ents = live ? DungeonEntities::FromText(ActiveEntText(), map, stem)
								: *m_levelEnts.find(stem)->second;
	const int ow = map.Width(), oh = map.Height();
	if (x0 == 0 && z0 == 0 && x1 == ow && z1 == oh) return false; // nothing to do

	// NOTHING IS SILENTLY DELETED (his rule). Everything placed stands on a floor
	// square - an entity, a fixture, a stair, a feature, the start - so counting
	// the floor a trim would cut off covers them all; the one thing that lives
	// in the ROCK is a window bored through it.
	auto outside = [&](int x, int z) { return x < x0 || z < z0 || x >= x1 || z >= z1; };
	int floor = 0, bores = 0;
	for (int z = 0; z < oh; ++z)
		for (int x = 0; x < ow; ++x)
			if (outside(x, z) && map.IsWalkable(x, z)) ++floor;
	for (const WallBore& b : map.Bores())
		if (outside(b.x, b.z)) ++bores;
	if (floor > 0 || bores > 0) {
		say(loc::Format("map.resize.content", floor, bores));
		return false;
	}

	// Every square renumbers by (dx,dz). Zero when only the right/bottom moved.
	const int dx = -x0, dz = -z0;
	const bool shifted = dx != 0 || dz != 0;

	// Stairs on OTHER levels that point at a square here. A pair's half stands on
	// that same square on its own floor, so it moves there too - and must fit, or
	// the whole resize is refused before anything has changed.
	struct Inbound {
		std::string level;
		int x, z;       // where it stands now
		bool pair;      // stands on the square it points at (moves with it)
	};
	std::vector<Inbound> inbound;
	if (shifted)
		for (const std::string& level : m_project.levels) {
			if (level == stem) continue;
			std::optional<DungeonMap> temp;
			const DungeonMap& other = readMap(level, temp);
			for (const StairLink& s : other.Stairs()) {
				if (s.destLevel != stem ||
					CatalogBool(m_project.stairs.Find(s.type), "exit", false))
					continue;
				const bool pair = s.x == s.destX && s.z == s.destZ;
				if (pair) {
					const int nx = s.x + dx, nz = s.z + dz;
					const StairLink* there = other.StairAt(nx, nz);
					if (!other.IsWalkable(nx, nz) || (there && there != &s) ||
						other.BrazierAt(nx, nz)) {
						say(loc::Format("map.stairs.destblocked", nx, nz, level));
						return false;
					}
				}
				inbound.push_back({level, s.x, s.z, pair});
			}
		}

	// --- commit ------------------------------------------------------------
	// The levels that change get their stashes now, before any reference.
	for (const Inbound& in : inbound)
		if (in.level != m_currentLevel) EnsureMapStash(in.level);
	const int partyX = m_party.GridX(), partyZ = m_party.GridZ();
	const std::vector<u8> oldSeen = live ? m_seen : std::vector<u8>{};
	// A resize renumbers the party's square too, so the undo step has to carry
	// where it stood (EditorSnapshot::partyX).
	if (live && m_pendingUndo) {
		m_pendingUndo->partyX = partyX;
		m_pendingUndo->partyZ = partyZ;
	}

	map.Reframe(x0, z0, w, h);
	ents.Reframe(dx, dz, w);
	// This level's own stairs into a moving pair: their dest square is the pair's
	// square, which is moving by the same (dx,dz).
	for (const Inbound& in : inbound)
		if (in.pair)
			for (const StairLink& s : map.Stairs())
				if (s.destLevel == in.level && s.destX == in.x && s.destZ == in.z)
					map.SetStairDestCell(s.x, s.z, in.x + dx, in.z + dz);

	// The other floors. A pair's half moves to the renumbered square; a one-way
	// link only has its dest renumbered.
	for (const Inbound& in : inbound) {
		DungeonMap& other = mapOf(in.level);
		if (!in.pair) {
			const StairLink* s = other.StairAt(in.x, in.z);
			if (s) other.SetStairDestCell(in.x, in.z, s->destX + dx, s->destZ + dz);
			continue;
		}
		StairLink s;
		if (!other.RemoveStair(in.x, in.z, &s)) continue;
		s.x += dx;
		s.z += dz;
		s.destX += dx;
		s.destZ += dz;
		if (!other.IsWalkable(s.x + DirDX(s.facing), s.z + DirDZ(s.facing)))
			s.facing = other.OpenFacing(s.x, s.z);
		other.AddStair(s);
		if (in.level == m_currentLevel) { // the pair is on the level being played
			std::erase_if(m_decorations, [&](const Decoration& d) {
				return d.stair && d.x == in.x && d.z == in.z;
			});
			PlaceStairProp(s);
			RebuildChunksAround(in.x, in.z);
			RebuildChunksAround(s.x, s.z);
		}
	}

	// The ways in: doorways and the opening.
	if (shifted)
		RemapArrivals(stem, [&](int& x, int& z) {
			x += dx;
			z += dz;
			return true;
		});

	// A stashed level's live diffs name squares in the old numbering; the active
	// level's are dropped by InstallLevel for the same reason.
	if (!live) m_levelStates.erase(stem);
	InstallLevel(stem, std::move(map), std::move(ents));

	if (live) {
		// InstallLevel stood the party on the start square; put it back where it
		// was, renumbered (it was on floor, and no floor was cut), and carry the
		// fog across so the map does not go dark.
		m_party.SetGridPosition(partyX + dx, partyZ + dz);
		for (int z = 0; z < oh; ++z)
			for (int x = 0; x < ow; ++x) {
				const int nx = x + dx, nz = z + dz;
				if (nx < 0 || nz < 0 || nx >= w || nz >= h) continue;
				m_seen[static_cast<size_t>(nz) * w + nx] =
					oldSeen[static_cast<size_t>(z) * ow + x];
			}
	}
	say(loc::Format("map.resize.done", stem, w, h));
	return true;
}

} // namespace dungeon::game
