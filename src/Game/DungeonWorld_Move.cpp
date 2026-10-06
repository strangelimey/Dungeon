// ============================================================================
// Game/DungeonWorld_Move.cpp - moving placed things: the editor's drag-and-drop
// (play-test #2, Michael 2026-09-28). See DungeonWorld.h "moving placed things".
//
// EVERY MOVE IS IN PLACE. The obvious build - erase, then place a new one at
// the drop - would lose everything the instance carries that the type does not:
// a monster's patrol and leash, a door's name and key, a lever's wiring, a
// sconce's brightness. So each kind changes its own position (live instance
// AND .ent / .map record, which must agree) and re-derives only what hangs off
// the position: a slot, a wall, a baked transform, the chunk it is stamped in.
//
// Each kind also applies the rule its PLACEMENT applies - a door only fits a
// doorway, a lever needs a wall - because a move that could put something
// where the brush would refuse it is a way to author content the loader later
// asserts on.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Core/Loc.h"

#include <algorithm>
#include <functional>

using namespace DirectX;

namespace dungeon::game {

namespace {

constexpr Direction kScan[4] = {Direction::North, Direction::East, Direction::South,
								Direction::West};

std::string DisplayOf(const Catalog& catalog, const std::string& id) {
	const CatalogEntry* e = catalog.Find(id);
	return e ? e->Display() : id;
}

} // namespace

DungeonWorld::MoveTarget DungeonWorld::TopMovableAt(int x, int z) const {
	MoveTarget t;
	t.x = x;
	t.z = z;
	// Top first: what STANDS on the square, then what is fixed to it, then the
	// square's own fabric. Dragging again takes the next one down.
	for (const Monster& m : m_monsters)
		if (m.x == x && m.z == z && m.kind) {
			t.kind = MoveTarget::Kind::Monster;
			t.runtimeId = m.runtimeId;
			t.label = DisplayOf(m_project.monsters, m.kind->name);
			return t;
		}
	for (int i = 0; i < static_cast<int>(m_items.size()); ++i) {
		const Item& it = m_items[static_cast<size_t>(i)];
		if (it.collected || it.x != x || it.z != z || !it.kind) continue;
		t.kind = MoveTarget::Kind::Item;
		t.index = i;
		// An item's name is a LANG key (items are named item.<id>), not a catalog
		// display string - the HUD's own lookup.
		t.label = it.kind->nameKey.empty() ? it.kind->id : loc::Tr(it.kind->nameKey);
		return t;
	}
	for (int i = 0; i < static_cast<int>(m_decorations.size()); ++i) {
		const Decoration& d = m_decorations[static_cast<size_t>(i)];
		if (d.stair || d.x != x || d.z != z || !d.kind) continue; // stairs: below
		t.kind = MoveTarget::Kind::Decoration;
		t.index = i;
		t.label = DisplayOf(m_project.decorations, d.kind->id);
		return t;
	}
	for (int i = 0; i < static_cast<int>(m_buttons.size()); ++i) {
		const Button& b = m_buttons[static_cast<size_t>(i)];
		if (b.x != x || b.z != z) continue;
		t.kind = MoveTarget::Kind::Button;
		t.index = i;
		t.label = b.kind ? DisplayOf(m_project.buttons, b.kind->id) : std::string("lever");
		return t;
	}
	for (const Door& d : m_doors)
		if (d.x == x && d.z == z) {
			t.kind = MoveTarget::Kind::Door;
			t.id = d.id;
			t.label = DisplayOf(m_project.doors, d.type);
			return t;
		}
	if (const FloorBrazier* b = m_map.BrazierAt(x, z)) {
		t.kind = MoveTarget::Kind::Brazier;
		t.label = DisplayOf(m_project.fixtures, b->type);
		return t;
	}
	for (const WallSconce& s : m_map.Sconces())
		if (s.x == x && s.z == z) {
			t.kind = MoveTarget::Kind::Sconce;
			t.wall = s.wall;
			t.label = DisplayOf(m_project.fixtures, s.type);
			return t;
		}
	if (const StairLink* s = m_map.StairAt(x, z)) {
		t.kind = MoveTarget::Kind::Stair;
		t.label = DisplayOf(m_project.stairs, s->type);
		return t;
	}
	for (const bool ceiling : {false, true})
		if (const SurfaceFeature* f = m_map.FeatureAt(x, z, ceiling)) {
			t.kind = MoveTarget::Kind::Feature;
			t.ceiling = ceiling;
			t.label = DisplayOf(m_project.surfacefeatures, f->type);
			return t;
		}
	return t; // Kind::None: nothing here moves (a niche or window stays carved)
}

bool DungeonWorld::MoveObject(const MoveTarget& t, int tx, int tz) {
	if (t.kind == MoveTarget::Kind::None || (tx == t.x && tz == t.z)) return false;
	auto say = [&](std::string_view line) {
		if (onMessage) onMessage(line);
	};
	auto refuse = [&](const char* key) {
		say(loc::Format(key, t.label, tx, tz));
		return false;
	};
	auto done = [&] {
		MarkSeen(tx, tz);
		say(loc::Format("map.move.done", t.label, tx, tz)); // logged by the editor's line
		return true;
	};
	// The first solid wall of (x,z), preferring `keep` - so a wall-hung thing
	// keeps its side of the room when the new square has a wall there too.
	auto wallAt = [&](int x, int z, Direction keep, Direction& out) {
		if (!m_map.IsWalkable(x + DirDX(keep), z + DirDZ(keep))) {
			out = keep;
			return true;
		}
		for (const Direction d : kScan)
			if (!m_map.IsWalkable(x + DirDX(d), z + DirDZ(d))) {
				out = d;
				return true;
			}
		return false;
	};

	switch (t.kind) {
	case MoveTarget::Kind::Monster: {
		auto it = std::find_if(m_monsters.begin(), m_monsters.end(),
							   [&](const Monster& m) { return m.runtimeId == t.runtimeId; });
		if (it == m_monsters.end()) return false; // gone since the pick-up
		if (!m_map.IsWalkable(tx, tz)) return refuse("map.move.blocked");
		for (const Monster& o : m_monsters)
			if (&o != &*it && o.x == tx && o.z == tz)
				return refuse("map.move.occupied"); // AddMonster's one-per-cell rule
		Monster& m = *it;
		// The leash anchor follows only when it WAS the spawn (the default); an
		// authored leashfrom= is a place in the level, not a place on the monster.
		const bool leashAtSpawn = m.leashX == m.spawnX && m.leashZ == m.spawnZ;
		// Spawn moves with it: the save layer diffs against the spawn, and after a
		// savemap the .ent says the new square, so the baseline is the new square.
		m.x = m.spawnX = tx;
		m.z = m.spawnZ = tz;
		if (leashAtSpawn) {
			m.leashX = tx;
			m.leashZ = tz;
		}
		const int self = static_cast<int>(it - m_monsters.begin());
		m.slot = std::max(0, FreeSlotInCell(tx, tz, m.kind->size, self));
		m.visualPos = m.moveFrom = SlotCenter(tx, tz, m.kind->size, m.slot);
		m.moveT = 0.0f;
		m.moving = false;
		m.aiPath.clear(); // a route from the old square leads nowhere now
		if (m.id >= 0)
			if (Entity* r = m_entities.MutableById(m.id)) {
				r->x = tx;
				r->z = tz;
				m_entsDirty = true;
			}
		return done();
	}
	case MoveTarget::Kind::Item: {
		if (t.index < 0 || t.index >= static_cast<int>(m_items.size())) return false;
		if (!m_map.IsWalkable(tx, tz)) return refuse("map.move.blocked");
		int here = 0;
		for (int i = 0; i < static_cast<int>(m_items.size()); ++i) {
			const Item& o = m_items[static_cast<size_t>(i)];
			if (i != t.index && !o.collected && o.niche < 0 && o.x == tx && o.z == tz) ++here;
		}
		if (here >= 4) return refuse("map.move.occupied"); // AddItem's four quarters
		Item& it = m_items[static_cast<size_t>(t.index)];
		it.x = tx;
		it.z = tz;
		it.niche = -1; // out of a pocket, onto the floor
		const Vec3 c = m_map.CellCenter(tx, tz);
		it.slot = FreeItemSlotNear(tx, tz, c.x, c.z, t.index);
		if (it.id >= 0)
			if (Entity* r = m_entities.MutableById(it.id)) {
				r->x = tx;
				r->z = tz;
				std::erase_if(r->params, [](const auto& p) {
					return p.first == "slot" || p.first == "niche";
				});
				r->params.emplace_back("slot", std::to_string(it.slot));
				m_entsDirty = true;
			}
		return done();
	}
	case MoveTarget::Kind::Decoration: {
		if (t.index < 0 || t.index >= static_cast<int>(m_decorations.size())) return false;
		if (!m_map.IsWalkable(tx, tz)) return refuse("map.move.blocked");
		Decoration& d = m_decorations[static_cast<size_t>(t.index)];
		if (!d.kind) return false;
		if (d.wallMounted) {
			Direction wall;
			if (!wallAt(tx, tz, d.wall, wall)) return refuse("map.move.nowall");
			d.wall = wall;
			d.facing = wall;
			const WallMount m = MountOnWall(tx, tz, wall);
			XMStoreFloat4x4(&d.world, UnitScale(d.kind->modelScale) * XMMatrixRotationY(m.yaw) *
										  XMMatrixTranslation(m.pos.x, 0, m.pos.z));
		} else {
			const Vec3 pos = m_map.CellCenter(tx, tz);
			XMStoreFloat4x4(&d.world, UnitScale(d.kind->modelScale) *
										  XMMatrixRotationY(DirYaw(d.facing)) *
										  XMMatrixTranslation(pos.x, 0, pos.z));
		}
		d.x = tx;
		d.z = tz;
		return done();
	}
	case MoveTarget::Kind::Button: {
		if (t.index < 0 || t.index >= static_cast<int>(m_buttons.size())) return false;
		if (!m_map.IsWalkable(tx, tz)) return refuse("map.move.blocked");
		for (int i = 0; i < static_cast<int>(m_buttons.size()); ++i)
			if (i != t.index && m_buttons[static_cast<size_t>(i)].x == tx &&
				m_buttons[static_cast<size_t>(i)].z == tz)
				return refuse("map.move.occupied"); // one lever per square
		Button& b = m_buttons[static_cast<size_t>(t.index)];
		Direction wall;
		if (!wallAt(tx, tz, b.facing, wall)) return refuse("map.move.nowall");
		b.x = tx;
		b.z = tz;
		b.facing = wall;
		if (Entity* r = m_entities.MutableById(b.id)) {
			r->x = tx;
			r->z = tz;
			r->facing = wall;
			m_entsDirty = true;
		}
		return done();
	}
	case MoveTarget::Kind::Door: {
		auto it = std::find_if(m_doors.begin(), m_doors.end(),
							   [&](const Door& d) { return d.id == t.id; });
		if (it == m_doors.end()) return false;
		Direction facing;
		if (!DungeonMap::DoorwayFacing(m_map, tx, tz, facing))
			return refuse("map.move.nodoorway");
		if (DoorAt(tx, tz)) return refuse("map.move.occupied");
		// AddDoor's rule: a closed door dropped on the party or a monster walls
		// them in. A corpse is nobody (DoorwayOccupied, code-review C65).
		if (DoorwayOccupied(tx, tz)) return refuse("map.move.occupied");
		Entity* r = m_entities.MutableById(t.id);
		if (!r) return false;
		r->x = tx;
		r->z = tz;
		r->facing = facing;
		m_entsDirty = true;
		// RESPAWNED from its record rather than patched, because a door derives a
		// dozen fields from its position and type (SpawnDoor is the one place that
		// knows them all); its RUN-TIME state - open, mid-swing, battered - is
		// carried across, since moving a door is not repairing or shutting it.
		const bool open = it->open;
		const float openT = it->openT;
		const Breakable brk = it->brk;
		m_doors.erase(it);
		SpawnDoor(*r);
		Door& moved = m_doors.back();
		moved.open = open;
		moved.openT = openT;
		moved.brk = brk;
		return done();
	}
	case MoveTarget::Kind::Brazier: {
		const FloorBrazier* b = m_map.BrazierAt(t.x, t.z);
		if (!b) return false;
		if (!m_map.IsWalkable(tx, tz) || m_map.BrazierAt(tx, tz) || m_map.StairAt(tx, tz))
			return refuse("map.move.blocked");
		const FloorBrazier copy = *b;
		m_map.RemoveFixtureAt(t.x, t.z); // takes the brazier first (the cell has one)
		m_map.AddBrazier(tx, tz, copy.type, copy.lit);
		m_map.SetBrazierProps(tx, tz, copy.lit, copy.brightness, copy.turbidity,
							  copy.flameColor);
		RebuildFiresAndDust();
		return done();
	}
	case MoveTarget::Kind::Sconce: {
		const auto& all = m_map.Sconces();
		auto it = std::find_if(all.begin(), all.end(), [&](const WallSconce& s) {
			return s.x == t.x && s.z == t.z && s.wall == t.wall;
		});
		if (it == all.end()) return false;
		const WallSconce copy = *it;
		if (!m_map.IsWalkable(tx, tz)) return refuse("map.move.blocked");
		m_map.RemoveSconceAt(t.x, t.z, t.wall);
		// Its own side of the room if the new square has a free wall there, else
		// the first free wall (AddSconce's auto-mount).
		if (!m_map.AddSconce(tx, tz, copy.type, copy.lit, copy.wall) &&
			!m_map.AddSconce(tx, tz, copy.type, copy.lit)) {
			m_map.AddSconce(copy.x, copy.z, copy.type, copy.lit, copy.wall); // put it back
			m_map.SetSconceProps(copy.x, copy.z, copy.wall, copy.lit, copy.brightness,
								 copy.turbidity, copy.flameColor);
			return refuse("map.move.nowall");
		}
		const Direction wall = m_map.Sconces().back().wall; // the one just added
		m_map.SetSconceProps(tx, tz, wall, copy.lit, copy.brightness, copy.turbidity,
							 copy.flameColor);
		RebuildFiresAndDust();
		return done();
	}
	case MoveTarget::Kind::Feature: {
		const SurfaceFeature* f = m_map.FeatureAt(t.x, t.z, t.ceiling);
		if (!f) return false;
		const SurfaceFeature copy = *f;
		if (!m_map.IsWalkable(tx, tz) || m_map.FeatureAt(tx, tz, t.ceiling))
			return refuse("map.move.blocked");
		m_map.RemoveFeature(t.x, t.z, t.ceiling);
		m_map.AddFeature(tx, tz, copy.type, copy.ceiling);
		// The feature IS the cell's surface block, so both squares re-stamp.
		RebuildChunksAround(t.x, t.z);
		RebuildChunksAround(tx, tz);
		return done();
	}
	case MoveTarget::Kind::Stair:
		if (!MoveStair(t, tx, tz)) return false;
		return done();
	case MoveTarget::Kind::None:
		break;
	}
	return false;
}

// A stair is the one move that reaches past the level being edited. A PAIRED
// stair stands on the SAME square on both floors (docs/level-building.md, the
// stair lesson), so its other half moves too, and that half has to fit on the
// other floor or neither moves. And any way IN that landed on the stair - a
// world-map doorway, the game's opening - follows it, which is the #1 deferral
// Michael gave to this feature: "leave it for #2".
bool DungeonWorld::MoveStair(const MoveTarget& t, int tx, int tz) {
	const StairLink* here = m_map.StairAt(t.x, t.z);
	if (!here) return false;
	const StairLink link = *here;
	auto say = [&](std::string_view line) {
		if (onMessage) onMessage(line);
	};
	if (!m_map.IsWalkable(tx, tz) || m_map.StairAt(tx, tz) || m_map.BrazierAt(tx, tz)) {
		say(loc::Format("map.move.blocked", t.label, tx, tz));
		return false;
	}

	// The other half, when this is one of a pair: on its destination level, at
	// its destination square, pointing back here. An exit has none (its dest is
	// a world-map location), and neither does a hand-written one-way link.
	const bool exit = CatalogBool(m_project.stairs.Find(link.type), "exit", false);
	const bool destIsLevel = std::find(m_project.levels.begin(), m_project.levels.end(),
									   link.destLevel) != m_project.levels.end();
	DungeonMap* other = nullptr;
	if (!exit && destIsLevel && link.destLevel != m_currentLevel) {
		DungeonMap& dst = EnsureMapStash(link.destLevel);
		const StairLink* p = dst.StairAt(link.destX, link.destZ);
		if (p && p->destLevel == m_currentLevel && p->destX == t.x && p->destZ == t.z)
			other = &dst;
	}
	if (other && (!other->IsWalkable(tx, tz) || other->StairAt(tx, tz) ||
				  other->BrazierAt(tx, tz))) {
		say(loc::Format("map.stairs.destblocked", tx, tz, link.destLevel));
		return false;
	}

	// A facing that still steps off onto floor is kept; one that would now step
	// into rock takes the square's first open side (DungeonMap::OpenFacing).
	auto facingFor = [](const DungeonMap& map, Direction keep, int x, int z) {
		return map.IsWalkable(x + DirDX(keep), z + DirDZ(keep)) ? keep
																: map.OpenFacing(x, z);
	};

	// This level: the record, and the prop drawn for it.
	StairLink moved;
	m_map.RemoveStair(t.x, t.z, &moved);
	std::erase_if(m_decorations, [&](const Decoration& d) {
		return d.stair && d.x == t.x && d.z == t.z;
	});
	moved.x = tx;
	moved.z = tz;
	moved.facing = facingFor(m_map, moved.facing, tx, tz);
	if (other) {
		moved.destX = tx;
		moved.destZ = tz;
	}
	m_map.AddStair(moved);
	PlaceStairProp(moved);
	// Holes: a down stair shows through the floor, an up one through the ceiling,
	// so both squares re-stamp.
	RebuildChunksAround(t.x, t.z);
	RebuildChunksAround(tx, tz);

	// The other floor: its half moves to the same new square and points back.
	if (other) {
		StairLink pair;
		other->RemoveStair(link.destX, link.destZ, &pair);
		pair.x = tx;
		pair.z = tz;
		pair.destX = tx;
		pair.destZ = tz;
		pair.facing = facingFor(*other, pair.facing, tx, tz);
		other->AddStair(pair);
		say(loc::Format("map.move.pair", link.destLevel, tx, tz));
	}
	MoveArrivals(t.x, t.z, tx, tz);
	return true;
}

// Every way in that landed on (fx,fz) of THIS level now lands on (tx,tz). Only
// an explicit square follows: a doorway or opening that says "the level's own
// start" (-1) never named this one.
void DungeonWorld::MoveArrivals(int fx, int fz, int tx, int tz) {
	RemapArrivals(m_currentLevel, [&](int& x, int& z) {
		if (x != fx || z != fz) return false;
		x = tx;
		z = tz;
		return true;
	});
}

void DungeonWorld::RemapArrivals(const std::string& stem,
								 const std::function<bool(int&, int&)>& remap) {
	auto say = [&](std::string_view line) {
		if (onMessage) onMessage(line);
	};
	if (m_worldForUndo && *m_worldForUndo) {
		WorldMap& world = **m_worldForUndo;
		std::vector<std::string> follow;
		for (const WorldMap::Location& l : world.Locations()) {
			if (l.entryX < 0 || l.entryZ < 0) continue; // lands on the level's start
			// Which level the doorway opens onto, resolved as entering it does
			// (Game::ArrivalsOn): its `level` when that belongs to its dungeon,
			// else the dungeon's first.
			const std::vector<std::string> levels = m_project.DungeonLevels(l.Dungeon());
			if (levels.empty()) continue;
			const bool named = std::find(levels.begin(), levels.end(), l.level) != levels.end();
			if ((named ? l.level : levels.front()) == stem) follow.push_back(l.id);
		}
		for (const std::string& id : follow)
			if (WorldMap::Location* l = world.MutableLocation(id);
				l && remap(l->entryX, l->entryZ))
				say(loc::Format("map.move.arrival", id, l->entryX, l->entryZ));
	}
	if (m_openLevel && m_openX && m_openZ && *m_openX >= 0 && *m_openZ >= 0) {
		const std::string level =
			m_openLevel->empty()
				? (m_project.levels.empty() ? std::string("level1") : m_project.levels.front())
				: *m_openLevel;
		if (level == stem && remap(*m_openX, *m_openZ)) {
			m_openingMoved = true;
			say(loc::Format("map.move.opening", *m_openX, *m_openZ));
		}
	}
}

} // namespace dungeon::game
