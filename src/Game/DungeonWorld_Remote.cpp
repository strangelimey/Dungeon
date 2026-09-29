// ============================================================================
// Game/DungeonWorld_Remote.cpp - split out of DungeonWorld_Editing.cpp to keep files
// small. Stair pairs across levels, the level edit stashes, remote
// (browsed-level) editing, the browse snapshot and the map markers.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Assets/File.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"

#include <algorithm>
#include <cstdlib> // atof — the .ent `seconds=` override
#include <filesystem>
#include <format>
#include <optional>

using namespace DirectX;

namespace dungeon::game {
// ============================================================================
// Stair pairs (editor). The ACTIVE level's half is live state (link + prop,
// persisted by `savemap` like every other edit); the DESTINATION level's half
// is edited textually in its .map file — one appended / removed "stairs ..."
// line — because that level isn't loaded, and its static layer is re-parsed
// from disk on every entry, so the file is authoritative. Validate-then-write:
// the destination map is parse-checked first (its loader DN_ASSERTs on bad
// records, so nothing invalid may ever be written).
// ============================================================================
DungeonMap& DungeonWorld::EnsureMapStash(const std::string& stem) {
	auto it = m_levelMaps.find(stem);
	if (it == m_levelMaps.end())
		it = m_levelMaps
				 .insert_or_assign(stem, std::make_unique<DungeonMap>(
											 m_project.LevelMapPath(stem),
											 FixtureTypesOf(m_project)))
				 .first;
	return *it->second;
}

DungeonEntities& DungeonWorld::EnsureEntStash(const std::string& stem) {
	auto it = m_levelEnts.find(stem);
	if (it == m_levelEnts.end()) {
		const DungeonMap& map = EnsureMapStash(stem);
		it = m_levelEnts
				 .insert_or_assign(stem, std::make_unique<DungeonEntities>(
											 m_project.LevelEntPath(stem), map))
				 .first;
	}
	return *it->second;
}

bool DungeonWorld::AddStairAt(const std::string& stem, const std::string& type,
							  int x, int z) {
	auto say = [&](const std::string& s) {
		if (onMessage) onMessage(s);
	};
	const CatalogEntry* entry = m_project.stairs.Find(type);
	if (!entry) {
		say(loc::Format("map.place.blocked", type));
		return false;
	}

	// A WAY OUT is one stair, not a pair: it leads to a world-map LOCATION, and
	// nothing stands on the far side of it. Going on to the pair logic below
	// asked for the level above, so on a dungeon's top floor - the one floor an
	// exit belongs on - the brush always refused (play-test #1). It lands
	// pointing where this level's existing exit points, else nowhere ("-", the
	// generator's token); the editor then asks where it leads (the stair
	// inspector's location list), which the world map, not this class, knows.
	if (CatalogBool(entry, "exit", false)) {
		const bool live = stem == m_currentLevel;
		if (!live) EnsureMapStash(stem);
		DungeonMap& map = live ? m_map : *m_levelMaps.find(stem)->second;
		if (!map.IsWalkable(x, z) || map.StairAt(x, z) || map.BrazierAt(x, z)) {
			say(loc::Format("map.place.blocked", entry->Display()));
			return false;
		}
		StairLink link;
		link.type = type;
		link.x = x;
		link.z = z;
		link.facing = map.OpenFacing(x, z); // stepped off into the level, not rock
		link.destLevel = "-";
		for (const StairLink& s : map.Stairs())
			if (CatalogBool(m_project.stairs.Find(s.type), "exit", false) &&
				s.destLevel != "-") {
				link.destLevel = s.destLevel;
				break;
			}
		map.AddStair(link);
		if (live) {
			PlaceStairProp(link);
			MarkSeen(x, z);
			RebuildChunksAround(x, z);
		}
		say(loc::Format("map.stairs.exitplaced", entry->Display()));
		return true;
	}

	// The type's direction (stairs.cat `up`) picks the destination: the previous
	// / next stem from `stem` - in its DUNGEON's depth order (dungeons.cat
	// `levels`) first, the vertical stack, and only when the dungeon has no
	// floor that way, in the project's flat list.
	//
	// The dungeon comes first because the flat list INTERLEAVES dungeons: a
	// floor added to a dungeon is appended at the list's end, so crypt3 sat
	// after eval_arena and a stair down from crypt2 led into the arena
	// (docs/level-building.md P1). The flat fallback stays because a stair
	// BETWEEN dungeons, off one's last floor, is legitimate content the brush
	// has always been able to author - the W10 delete rule "a stair from
	// outside leading in" exists for it, and WorldTest builds one this way.
	const bool up = CatalogBool(entry, "up", false);
	auto neighbour = [&](const std::vector<std::string>& levels) -> std::string {
		const auto cur = std::find(levels.begin(), levels.end(), stem);
		if (cur == levels.end()) return {};
		if (up) return cur != levels.begin() ? *(cur - 1) : std::string();
		return cur + 1 != levels.end() ? *(cur + 1) : std::string();
	};
	std::string dest;
	if (const CatalogEntry* dungeon = m_project.DungeonOfLevel(stem))
		dest = neighbour(m_project.DungeonLevels(dungeon->id));
	if (dest.empty()) dest = neighbour(m_project.levels);
	if (dest.empty()) {
		say(loc::Tr(up ? "map.stairs.noup" : "map.stairs.nodown"));
		return false;
	}

	// The paired half: the entry's explicit `pair` type when it names one (a
	// pit pairs with the ceiling hole and vice versa), else the first
	// opposite-direction type in the catalog (stairs down <-> stairs up).
	std::string pairType = CatalogGet(entry, "pair", "");
	if (pairType.empty())
		for (const CatalogEntry& e : m_project.stairs.Entries())
			if (CatalogBool(&e, "up", false) != up) {
				pairType = e.id;
				break;
			}
	if (pairType.empty() || !m_project.stairs.Contains(pairType)) {
		say(loc::Tr("map.stairs.nopair"));
		return false;
	}

	// Each side's authoritative map: the LIVE one for the active level, the
	// level's stash otherwise (created from the file on first edit). Both
	// stashes are ensured BEFORE taking references — a flat_map insertion
	// would invalidate a sibling reference.
	const bool srcLive = stem == m_currentLevel;
	const bool dstLive = dest == m_currentLevel;
	if (!srcLive) EnsureMapStash(stem);
	if (!dstLive) EnsureMapStash(dest);
	DungeonMap& src = srcLive ? m_map : *m_levelMaps.find(stem)->second;
	DungeonMap& dst = dstLive ? m_map : *m_levelMaps.find(dest)->second;

	if (!src.IsWalkable(x, z) || src.StairAt(x, z) || src.BrazierAt(x, z)) {
		say(loc::Format("map.place.blocked", entry->Display()));
		return false;
	}
	// The pair lands on the SAME cell one level up/down.
	if (!dst.IsWalkable(x, z) || dst.StairAt(x, z) || dst.BrazierAt(x, z)) {
		say(loc::Format("map.stairs.destblocked", x, z, dest));
		return false;
	}

	StairLink link;
	link.type = type;
	link.x = x;
	link.z = z;
	link.destLevel = dest;
	link.destX = x;
	link.destZ = z; // each side arrives standing on its counterpart
	link.facing = src.OpenFacing(x, z); // each half is stepped off into ITS level
	StairLink pair = link;
	pair.type = pairType;
	pair.destLevel = stem;
	pair.facing = dst.OpenFacing(x, z);

	src.AddStair(link);
	dst.AddStair(pair);
	// Only a live side has a 3D presence: the prop, the fog reveal, and — for
	// a down stair — the floor hole its shaft shows through (chunk rebuild).
	if (srcLive) {
		PlaceStairProp(link);
		MarkSeen(x, z);
		RebuildChunksAround(x, z);
	}
	if (dstLive) {
		PlaceStairProp(pair);
		MarkSeen(x, z);
		RebuildChunksAround(x, z);
	}
	say(loc::Format("map.stairs.placed", entry->Display(), dest));
	return true;
}

bool DungeonWorld::RemovePairedStair(const std::string& fromStem,
									 const StairLink& removed) {
	// The pair stands on `removed`'s destination cell and links back to the
	// stair's own level+cell; anything else is a hand-authored one-way link.
	auto matches = [&](const StairLink* s) {
		return s && s->destLevel == fromStem && s->destX == removed.x &&
			   s->destZ == removed.z;
	};
	// An EXIT has no pair: its dest names a world-map LOCATION ("crypt_gate"),
	// or "-" for nowhere yet, never a level. Treating that as a stem sent
	// EnsureMapStash off to parse levels/crypt_gate.map, and the map loader's
	// missing-file assert took the game down - on a middle-click erase, the
	// inspector's Delete, or a wall painted over the exit (play-test #1). A
	// hand-written one-way link to a level the project does not list is the
	// same case, so ANY dest outside the level list ends here.
	if (CatalogBool(m_project.stairs.Find(removed.type), "exit", false) ||
		std::find(m_project.levels.begin(), m_project.levels.end(),
				  removed.destLevel) == m_project.levels.end())
		return false;
	if (removed.destLevel == m_currentLevel) {
		if (!matches(m_map.StairAt(removed.destX, removed.destZ))) return false;
		m_map.RemoveStair(removed.destX, removed.destZ);
		std::erase_if(m_decorations, [&](const Decoration& d) {
			return d.stair && d.x == removed.destX && d.z == removed.destZ;
		});
		// A removed down stair's floor hole closes with the chunk rebuild.
		RebuildChunksAround(removed.destX, removed.destZ);
		return true;
	}
	DungeonMap& dst = EnsureMapStash(removed.destLevel);
	if (!matches(dst.StairAt(removed.destX, removed.destZ))) return false;
	return dst.RemoveStair(removed.destX, removed.destZ);
}

bool DungeonWorld::RemoveStairAt(int x, int z) {
	StairLink removed;
	if (!m_map.RemoveStair(x, z, &removed)) return false;
	std::erase_if(m_decorations, [&](const Decoration& d) {
		return d.stair && d.x == x && d.z == z;
	});
	RebuildChunksAround(x, z); // a down stair's floor hole closes
	const bool pair = RemovePairedStair(m_currentLevel, removed);
	// Split rather than a conditional: the two arms are now a loc::Line and a
	// string_view, which have no common type to pick.
	if (onMessage) {
		if (pair) onMessage(loc::FormatLine("map.stairs.removed", removed.destLevel));
		else onMessage(loc::View("map.erase.removed"));
	}
	return true;
}

// ============================================================================
// Remote level editing — the map overlay edits ANY level. Every op targets the
// level's in-memory stashes; `savemap` (SaveAllLevels) persists them.
// ============================================================================
void DungeonWorld::EditCellRemote(const std::string& stem, int x, int z,
								  Cell cell) {
	DungeonMap& map = EnsureMapStash(stem);
	const u32 rev = map.Revision();
	map.SetCell(x, z, cell);
	if (map.Revision() == rev) return; // unchanged / out of bounds
	map.PruneFixturesForCell(x, z);
	PruneStashRecordsForCell(stem, x, z);
}

void DungeonWorld::EditVariantRemote(const std::string& stem, int x, int z,
									 SurfaceSel sel, int variant) {
	DungeonMap& map = EnsureMapStash(stem);
	// Same cell-type gate as EditVariant: walls on solid cells, the rest on
	// floor cells.
	if ((sel == SurfaceSel::Wall) == map.IsWalkable(x, z)) return;
	switch (sel) {
	case SurfaceSel::Wall:    map.SetWallVariant(x, z, variant); break;
	case SurfaceSel::Floor:   map.SetFloorVariant(x, z, variant); break;
	case SurfaceSel::Ceiling: map.SetCeilingVariant(x, z, variant); break;
	}
}

bool DungeonWorld::AddDecorationRemote(const std::string& stem,
									   const std::string& type, int x, int z) {
	DungeonMap& map = EnsureMapStash(stem);
	if (!map.IsWalkable(x, z) || !m_project.decorations.Contains(type))
		return false;
	Entity e;
	e.kind = EntityKind::Decoration;
	e.type = type;
	e.x = x;
	e.z = z;
	map.AddDecorationRecord(std::move(e));
	return true;
}

bool DungeonWorld::AddDecorationRemote(const std::string& stem,
									   const std::string& type, int x, int z,
									   Direction wall) {
	DungeonMap& map = EnsureMapStash(stem);
	if (!map.IsWalkable(x, z) || !m_project.decorations.Contains(type))
		return false;
	if (map.IsWalkable(x + DirDX(wall), z + DirDZ(wall))) return false; // nothing to hang on
	Entity e;
	e.kind = EntityKind::Decoration;
	e.type = type;
	e.x = x;
	e.z = z;
	e.facing = wall;
	e.params.emplace_back("wall", DirToken(wall)); // hangs flat on that wall
	map.AddDecorationRecord(std::move(e));
	return true;
}

bool DungeonWorld::AddMonsterRemote(const std::string& stem,
									const std::string& type, int x, int z) {
	DungeonEntities& ents = EnsureEntStash(stem);
	const DungeonMap& map = *m_levelMaps.find(stem)->second;
	if (!map.IsWalkable(x, z) || !m_project.monsters.Contains(type)) return false;
	for (const Entity& e : ents.At(x, z))
		if (e.kind == EntityKind::Monster) return false; // one monster per cell
	Entity e;
	e.kind = EntityKind::Monster;
	e.type = type;
	e.x = x;
	e.z = z;
	ents.Add(std::move(e));
	return true;
}

bool DungeonWorld::AddFixtureRemote(const std::string& stem,
									const std::string& type, int x, int z) {
	DungeonMap& map = EnsureMapStash(stem);
	const CatalogEntry* def = m_project.fixtures.Find(type);
	if (!def) return false;
	const bool lit = CatalogBool(def, "flame", true);
	// AddSconce/AddBrazier validate the cell themselves (and rebuild the map's
	// turbidity); the live-only fire/particle rebuild does not apply here.
	return def->Get("mount", "floor") == "wall"
			   ? map.AddSconce(x, z, type, lit)
			   : map.AddBrazier(x, z, type, lit);
}

bool DungeonWorld::AddFixtureRemote(const std::string& stem,
									const std::string& type, int x, int z,
									Direction wall) {
	DungeonMap& map = EnsureMapStash(stem);
	const CatalogEntry* def = m_project.fixtures.Find(type);
	if (!def) return false;
	// Only a wall kind has a face; a floor kind ignores the pick (see AddFixture).
	if (def->Get("mount", "floor") != "wall")
		return AddFixtureRemote(stem, type, x, z);
	return map.AddSconce(x, z, type, CatalogBool(def, "flame", true), wall);
}

bool DungeonWorld::AddNicheRemote(const std::string& stem, const std::string& type,
								  int x, int z) {
	// Edits the level's stashed map; MapView rebuilds the browse snapshot after.
	return EnsureMapStash(stem).AddNiche(x, z, type);
}

bool DungeonWorld::AddNicheRemote(const std::string& stem, const std::string& type,
								  int x, int z, Direction wall) {
	return EnsureMapStash(stem).AddNiche(x, z, type, wall);
}

bool DungeonWorld::AddSurfaceFeatureRemote(const std::string& stem,
										   const std::string& type, int x, int z) {
	return EnsureMapStash(stem).AddFeature(x, z, type, FeatureIsCeiling(type));
}

void DungeonWorld::EraseRemote(const std::string& stem, int x, int z) {
	auto say = [&](const std::string& s) {
		if (onMessage) onMessage(s);
	};
	DungeonEntities& ents = EnsureEntStash(stem);
	DungeonMap& map = *m_levelMaps.find(stem)->second;

	StairLink removed;
	if (map.RemoveStair(x, z, &removed)) {
		const bool pair = RemovePairedStair(stem, removed);
		say(pair ? loc::Format("map.stairs.removed", removed.destLevel)
				 : loc::Tr("map.erase.removed"));
		return;
	}
	for (const Entity& e : ents.At(x, z))
		if (e.kind == EntityKind::Monster || e.kind == EntityKind::Door ||
			e.kind == EntityKind::Button || e.kind == EntityKind::Item) {
			ents.RemoveById(e.id);
			say(loc::Tr("map.erase.removed"));
			return;
		}
	if (map.RemoveDecorationRecordAt(x, z) || map.RemoveFixtureAt(x, z) ||
		map.RemoveNicheFacingWall(x, z) || map.RemoveAnyFeature(x, z)) {
		say(loc::Tr("map.erase.removed"));
		return;
	}
	map.SetWallVariant(x, z, -1);
	map.SetFloorVariant(x, z, -1);
	map.SetCeilingVariant(x, z, -1);
	say(loc::Format("map.erase.reset", x, z));
}

void DungeonWorld::PruneStashRecordsForCell(const std::string& stem, int x,
											int z) {
	DungeonEntities& ents = EnsureEntStash(stem);
	DungeonMap& map = *m_levelMaps.find(stem)->second;
	if (!map.IsWalkable(x, z)) {
		// Painted solid: nothing can keep standing on the cell. Stairs go
		// through the pair helper so the other level's half dies too.
		StairLink removed;
		if (map.RemoveStair(x, z, &removed)) RemovePairedStair(stem, removed);
		map.RemoveDecorationRecordsAt(x, z);
		ents.RemoveAt(x, z);
		return;
	}
	// Painted open: re-face button records that mounted on this cell onto
	// another solid wall of their own cell, else drop them (the live prune's
	// record half; wall-mounted decoration records re-resolve via the soft
	// loader on the next entry).
	std::vector<int> dropIds;
	for (const Entity& e : ents.All()) {
		if (e.kind != EntityKind::Button) continue;
		if (e.x + DirDX(e.facing) != x || e.z + DirDZ(e.facing) != z) continue;
		bool refaced = false;
		constexpr Direction kScan[4] = {Direction::North, Direction::East,
										Direction::South, Direction::West};
		for (const Direction d : kScan)
			if (!map.IsWalkable(e.x + DirDX(d), e.z + DirDZ(d))) {
				if (Entity* mut = ents.MutableById(e.id)) mut->facing = d;
				refaced = true;
				break;
			}
		if (!refaced) dropIds.push_back(e.id);
	}
	for (const int id : dropIds) ents.RemoveById(id);
}

std::unique_ptr<DungeonWorld::LevelBrowse> DungeonWorld::BrowseLevel(
	const std::string& stem) {
	// Both layers prefer the in-session stash (unsaved edits) over the file.
	const auto ms = m_levelMaps.find(stem);
	DungeonMap map = ms != m_levelMaps.end()
						 ? DungeonMap(*ms->second)
						 : DungeonMap(m_project.LevelMapPath(stem),
									  FixtureTypesOf(m_project));
	const auto es = m_levelEnts.find(stem);
	auto browse =
		es != m_levelEnts.end()
			? std::make_unique<LevelBrowse>(stem, std::move(map),
											DungeonEntities(*es->second))
			: std::make_unique<LevelBrowse>(stem, std::move(map),
											m_project.LevelEntPath(stem));
	// Fog: the cell list stashed when the party last left the level, spread into
	// the same w*h mask shape IsSeen reads (left empty for a never-visited level).
	if (const auto st = m_levelStates.find(stem); st != m_levelStates.end()) {
		const int w = browse->map.Width(), h = browse->map.Height();
		browse->seen.assign(static_cast<size_t>(w) * h, 0);
		for (const auto& [x, z] : st->second.seen)
			if (x >= 0 && z >= 0 && x < w && z < h)
				browse->seen[static_cast<size_t>(z) * w + x] = 1;
	}
	return browse;
}

std::vector<DungeonWorld::MapMarker> DungeonWorld::MonsterMarkers() const {
	std::vector<MapMarker> markers;
	markers.reserve(m_monsters.size());
	for (const Monster& m : m_monsters)
		if (m.Alive()) // a slain monster leaves no map marker
			markers.push_back({m.x, m.z, m.kind ? m.kind->name : std::string(),
							   m.facing,
							   m.kind ? MonsterIconFor(m.kind->name) : nullptr,
							   !m.kind || m.kind->facesTarget});
	return markers;
}

const gfx::Texture* DungeonWorld::MonsterIconFor(const std::string& type) const {
	if (!m_monsterIconsBaked) return nullptr; // RT still transparent
	const auto it = m_monsterKinds.find(type);
	return it != m_monsterKinds.end() ? it->second->iconTarget.get() : nullptr;
}

const gfx::Texture* DungeonWorld::DecorationIconFor(const std::string& type) const {
	if (!m_decorationIconsBaked) return nullptr; // RT still transparent
	const auto it = m_decorationKinds.find(type);
	return it != m_decorationKinds.end() ? it->second->iconTarget.get() : nullptr;
}

const gfx::Texture* DungeonWorld::ItemIconLookup(const std::string& type) const {
	// The no-load twin of ItemIconFor: model items own baked icons (the HUD's),
	// placeholder items return null (the map keeps its small square for them).
	const auto it = m_itemKinds.find(type);
	return it != m_itemKinds.end() && m_itemIconsBaked
			   ? it->second->iconTarget.get()
			   : nullptr;
}

std::vector<DungeonWorld::MapMarker> DungeonWorld::DecorationMarkers() const {
	std::vector<MapMarker> markers;
	markers.reserve(m_decorations.size());
	for (const Decoration& d : m_decorations) {
		if (d.stair) continue; // stairs draw from their own (typed) marker
		markers.push_back({d.x, d.z, d.kind ? d.kind->id : std::string(), d.facing,
						   m_decorationIconsBaked && d.kind
							   ? d.kind->iconTarget.get()
							   : nullptr,
						   !d.kind || d.kind->facingArrow});
	}
	return markers;
}

} // namespace dungeon::game
