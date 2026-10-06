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
		// Validated against the map as the level READS - its stash when it has
		// one, else its file. It used to be the map's stash, made here, so every
		// records-only edit rewrote the .map as well (C307).
		const DungeonMap& map = *LevelForReading(stem).map;
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
		// Checked as the level READS; stashed only once the stair will land (a
		// refusal used to leave a stash behind for savemap to rewrite: C307).
		if (!CellFreeForStair(stem, x, z)) {
			say(loc::Format("map.place.blocked", entry->Display()));
			return false;
		}
		DungeonMap& map = live ? m_map : EnsureMapStash(stem);
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

	// Both squares checked as the levels READ, before anything is stashed: a
	// refused link used to leave both stashes behind (C307).
	if (!CellFreeForStair(stem, x, z)) {
		say(loc::Format("map.place.blocked", entry->Display()));
		return false;
	}
	// The pair lands on the SAME cell one level up/down.
	if (!CellFreeForStair(dest, x, z)) {
		say(loc::Format("map.stairs.destblocked", x, z, dest));
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
		ReserveFallRoom(); // a pit placed here names `dest` (C210)
	}
	if (dstLive) {
		PlaceStairProp(pair);
		MarkSeen(x, z);
		RebuildChunksAround(x, z);
		ReserveFallRoom();
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
	// Looked for as the level READS, and stashed only to take it away (C307).
	return EditMapStash(removed.destLevel, [&](DungeonMap& dst) {
		return matches(dst.StairAt(removed.destX, removed.destZ)) &&
			   dst.RemoveStair(removed.destX, removed.destZ);
	});
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
// level's in-memory stashes; `savemap` (SaveAllLevels) persists them. A stash is
// made only by an edit that CHANGES its layer (EditMapStash / EditEntStash):
// a refusal or a no-op leaves the level as its files have it, and a records
// edit never stashes the map, nor a map edit the records (C307).
// ============================================================================
void DungeonWorld::EditCellRemote(const std::string& stem, int x, int z,
								  Cell cell) {
	const bool changed = EditMapStash(stem, [&](DungeonMap& map) {
		const u32 rev = map.Revision();
		map.SetCell(x, z, cell);
		if (map.Revision() == rev) return false; // unchanged / out of bounds
		map.PruneFixturesForCell(x, z);
		return true;
	});
	if (changed) PruneStashRecordsForCell(stem, x, z);
}

void DungeonWorld::EditVariantRemote(const std::string& stem, int x, int z,
									 SurfaceSel sel, int variant) {
	EditMapStash(stem, [&](DungeonMap& map) {
		// Same cell-type gate as EditVariant: walls on solid cells, the rest on
		// floor cells. The setters bump the revision only on a change.
		if ((sel == SurfaceSel::Wall) == map.IsWalkable(x, z)) return false;
		const u32 rev = map.Revision();
		switch (sel) {
		case SurfaceSel::Wall:    map.SetWallVariant(x, z, variant); break;
		case SurfaceSel::Floor:   map.SetFloorVariant(x, z, variant); break;
		case SurfaceSel::Ceiling: map.SetCeilingVariant(x, z, variant); break;
		}
		return map.Revision() != rev;
	});
}

bool DungeonWorld::AddDecorationRemote(const std::string& stem,
									   const std::string& type, int x, int z) {
	if (!m_project.decorations.Contains(type)) return false;
	return EditMapStash(stem, [&](DungeonMap& map) {
		if (!map.IsWalkable(x, z)) return false;
		Entity e;
		e.kind = EntityKind::Decoration;
		e.type = type;
		e.x = x;
		e.z = z;
		map.AddDecorationRecord(std::move(e));
		return true;
	});
}

bool DungeonWorld::AddDecorationRemote(const std::string& stem,
									   const std::string& type, int x, int z,
									   Direction wall) {
	if (!m_project.decorations.Contains(type)) return false;
	return EditMapStash(stem, [&](DungeonMap& map) {
		if (!map.IsWalkable(x, z)) return false;
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
	});
}

bool DungeonWorld::AddMonsterRemote(const std::string& stem,
									const std::string& type, int x, int z) {
	if (!m_project.monsters.Contains(type)) return false;
	return EditEntStash(stem, [&](DungeonEntities& ents, const DungeonMap& map) {
		if (!map.IsWalkable(x, z)) return false;
		for (const Entity& e : ents.At(x, z))
			if (e.kind == EntityKind::Monster) return false; // one monster per cell
		Entity e;
		e.kind = EntityKind::Monster;
		e.type = type;
		e.x = x;
		e.z = z;
		ents.Add(std::move(e));
		return true;
	});
}

bool DungeonWorld::AddFixtureRemote(const std::string& stem,
									const std::string& type, int x, int z) {
	const CatalogEntry* def = m_project.fixtures.Find(type);
	if (!def) return false;
	const bool lit = CatalogBool(def, "flame", true);
	// AddSconce/AddBrazier validate the cell themselves (and rebuild the map's
	// turbidity); the live-only fire/particle rebuild does not apply here.
	const bool wall = def->Get("mount", "floor") == "wall";
	return EditMapStash(stem, [&](DungeonMap& map) {
		return wall ? map.AddSconce(x, z, type, lit) : map.AddBrazier(x, z, type, lit);
	});
}

bool DungeonWorld::AddFixtureRemote(const std::string& stem,
									const std::string& type, int x, int z,
									Direction wall) {
	const CatalogEntry* def = m_project.fixtures.Find(type);
	if (!def) return false;
	// Only a wall kind has a face; a floor kind ignores the pick (see AddFixture).
	if (def->Get("mount", "floor") != "wall")
		return AddFixtureRemote(stem, type, x, z);
	const bool lit = CatalogBool(def, "flame", true);
	return EditMapStash(stem, [&](DungeonMap& map) { return map.AddSconce(x, z, type, lit, wall); });
}

bool DungeonWorld::AddNicheRemote(const std::string& stem, const std::string& type,
								  int x, int z) {
	// Edits the level's stashed map; MapView rebuilds the browse snapshot after.
	return EditMapStash(stem, [&](DungeonMap& map) { return map.AddNiche(x, z, type); });
}

bool DungeonWorld::AddNicheRemote(const std::string& stem, const std::string& type,
								  int x, int z, Direction wall) {
	return EditMapStash(stem, [&](DungeonMap& map) { return map.AddNiche(x, z, type, wall); });
}

bool DungeonWorld::AddSurfaceFeatureRemote(const std::string& stem,
										   const std::string& type, int x, int z) {
	const bool ceiling = FeatureIsCeiling(type);
	return EditMapStash(stem,
						[&](DungeonMap& map) { return map.AddFeature(x, z, type, ceiling); });
}

bool DungeonWorld::EraseRemote(const std::string& stem, int x, int z) {
	auto say = [&](const std::string& s) {
		if (onMessage) onMessage(s);
	};
	// The ladder is climbed as the level READS, and each rung that takes
	// something stashes only the layer it takes it from: an erase used to
	// stash both layers before looking, so an erase of nothing - or of a
	// record - rewrote a .map and an .ent nobody had changed (C307).
	StairLink removed;
	if (EditMapStash(stem, [&](DungeonMap& map) { return map.RemoveStair(x, z, &removed); })) {
		const bool pair = RemovePairedStair(stem, removed);
		say(pair ? loc::Format("map.stairs.removed", removed.destLevel)
				 : loc::Tr("map.erase.removed"));
		return true;
	}
	if (EditEntStash(stem, [&](DungeonEntities& ents, const DungeonMap&) {
			for (const Entity& e : ents.At(x, z))
				if (e.kind == EntityKind::Monster || e.kind == EntityKind::Door ||
					e.kind == EntityKind::Button || e.kind == EntityKind::Item) {
					ents.RemoveById(e.id);
					return true;
				}
			return false;
		})) {
		say(loc::Tr("map.erase.removed"));
		return true;
	}
	if (EditMapStash(stem, [&](DungeonMap& map) {
			return map.RemoveDecorationRecordAt(x, z) || map.RemoveFixtureAt(x, z) ||
				   map.RemoveNicheFacingWall(x, z) || map.RemoveAnyFeature(x, z);
		})) {
		say(loc::Tr("map.erase.removed"));
		return true;
	}
	const bool reset = EditMapStash(stem, [&](DungeonMap& map) {
		const u32 rev = map.Revision(); // the setters bump it only on a change
		map.SetWallVariant(x, z, -1);
		map.SetFloorVariant(x, z, -1);
		map.SetCeilingVariant(x, z, -1);
		return map.Revision() != rev;
	});
	say(loc::Format("map.erase.reset", x, z));
	return reset;
}

void DungeonWorld::PruneStashRecordsForCell(const std::string& stem, int x,
											int z) {
	// Called after a paint CHANGED the cell, so the map is stashed already; the
	// records are stashed only when one of them has to go or turn (C307 - the
	// .ent of every remote paint used to be rewritten).
	DungeonMap& map = EnsureMapStash(stem);
	if (!map.IsWalkable(x, z)) {
		// Painted solid: nothing can keep standing on the cell. Stairs go
		// through the pair helper so the other level's half dies too.
		StairLink removed;
		if (map.RemoveStair(x, z, &removed)) RemovePairedStair(stem, removed);
		map.RemoveDecorationRecordsAt(x, z);
		EditEntStash(stem, [&](DungeonEntities& ents, const DungeonMap&) {
			return ents.RemoveAt(x, z) > 0;
		});
		return;
	}
	// Painted open: re-face button records that mounted on this cell onto
	// another solid wall of their own cell, else drop them (the live prune's
	// record half; wall-mounted decoration records re-resolve via the soft
	// loader on the next entry).
	EditEntStash(stem, [&](DungeonEntities& ents, const DungeonMap& level) {
		std::vector<int> dropIds;
		bool changed = false;
		for (const Entity& e : ents.All()) {
			if (e.kind != EntityKind::Button) continue;
			if (e.x + DirDX(e.facing) != x || e.z + DirDZ(e.facing) != z) continue;
			bool refaced = false;
			constexpr Direction kScan[4] = {Direction::North, Direction::East,
											Direction::South, Direction::West};
			for (const Direction d : kScan)
				if (!level.IsWalkable(e.x + DirDX(d), e.z + DirDZ(d))) {
					if (Entity* mut = ents.MutableById(e.id)) mut->facing = d;
					refaced = changed = true;
					break;
				}
			if (!refaced) dropIds.push_back(e.id);
		}
		for (const int id : dropIds) ents.RemoveById(id);
		return changed || !dropIds.empty();
	});
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
