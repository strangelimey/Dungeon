// ============================================================================
// Game/DungeonWorld_Editing.cpp — split out of DungeonWorld.cpp to keep files
// small (see DungeonWorld.h). Holds the editor's cell/variant/placement edits,
// doors/buttons/niches, remote (browsed-level) editing + stashes, level
// browse/load/save/rename serialization, map markers, and editor undo/redo.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Assets/File.h"
#include "Assets/Image.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/DungeonMeshBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>

using namespace DirectX;

namespace dungeon::game {
void DungeonWorld::EditCell(int x, int z, Cell cell) {
	const u32 rev = m_map.Revision();
	m_map.SetCell(x, z, cell);
	if (m_map.Revision() == rev) return; // unchanged
	// A structural edit can strand fixtures (buried under a painted wall, or a
	// sconce whose mount wall just opened up) — prune them with the cell, or the
	// saved .map would assert on its next load.
	if (m_map.PruneFixturesForCell(x, z)) RebuildFiresAndDust();
	// Same for the dynamic layer and decorations: nothing may keep standing on
	// (or mounting against) the repainted cell — SaveLevel and the level stash
	// persist live state, and the loaders reject records contradicting the grid.
	PruneEntitiesForCell(x, z);
	MarkSeen(x, z);
	RebuildChunksAround(x, z);
}

void DungeonWorld::PruneEntitiesForCell(int x, int z) {
	if (!m_map.IsWalkable(x, z)) {
		// Cell painted solid: nothing can stand on it any more. Stairs go through
		// RemoveStairAt so the paired return record on the other level dies too;
		// that also erases the stair prop, leaving plain decorations to the sweep.
		if (m_map.StairAt(x, z)) RemoveStairAt(x, z);
		std::erase_if(m_monsters,
					  [&](const Monster& m) { return m.x == x && m.z == z; });
		std::erase_if(m_items, [&](const Item& i) { return i.x == x && i.z == z; });
		std::erase_if(m_buttons,
					  [&](const Button& b) { return b.x == x && b.z == z; });
		std::erase_if(m_decorations,
					  [&](const Decoration& d) { return d.x == x && d.z == z; });
		std::erase_if(m_doors, [&](const Door& d) { return d.x == x && d.z == z; });
		// The .ent baseline records under the new wall. Record edits diverge
		// m_entities from the file on disk — flag it so a level swap stashes
		// them (see BeginLevelLoad) instead of re-parsing the stale file.
		if (m_entities.RemoveAt(x, z) > 0) m_entsDirty = true;
		return;
	}

	// Cell painted open: buttons and wall decorations in the neighbouring cells
	// that mounted on it hang on air now. Re-mount each on a solid wall of its
	// own cell, else drop it (the sconce treatment in PruneFixturesForCell).
	auto solidWall = [&](int cx, int cz, Direction& out) {
		constexpr Direction kScan[4] = {Direction::North, Direction::East,
										Direction::South, Direction::West};
		for (const Direction d : kScan)
			if (!m_map.IsWalkable(cx + DirDX(d), cz + DirDZ(d))) {
				out = d;
				return true;
			}
		return false;
	};
	for (size_t i = m_buttons.size(); i-- > 0;) {
		Button& b = m_buttons[i];
		if (b.x + DirDX(b.facing) != x || b.z + DirDZ(b.facing) != z) continue;
		Direction d;
		if (solidWall(b.x, b.z, d)) {
			b.facing = d;
			// The writer emits buttons from their .ent record, so re-face it too.
			if (Entity* e = m_entities.MutableById(b.id)) e->facing = d;
			m_entsDirty = true;
		} else {
			m_entities.RemoveById(b.id);
			m_buttons.erase(m_buttons.begin() + static_cast<ptrdiff_t>(i));
			m_entsDirty = true;
		}
	}
	for (size_t i = m_decorations.size(); i-- > 0;) {
		Decoration& deco = m_decorations[i];
		if (!deco.wallMounted) continue;
		if (deco.x + DirDX(deco.wall) != x || deco.z + DirDZ(deco.wall) != z)
			continue;
		Direction d;
		if (solidWall(deco.x, deco.z, d)) {
			deco.wall = d;
			const WallMount m = MountOnWall(deco.x, deco.z, d);
			XMStoreFloat4x4(&deco.world,
							XMMatrixRotationY(m.yaw) *
								XMMatrixTranslation(m.pos.x, 0, m.pos.z));
		} else {
			m_decorations.erase(m_decorations.begin() + static_cast<ptrdiff_t>(i));
		}
	}
}

void DungeonWorld::EditVariant(int x, int z, SurfaceSel sel, int variant) {
	// Wall variants live on the SOLID cell (the block owns its texture — all
	// four faces); floor/ceiling variants on the floor cell they surface. A
	// paint on the wrong cell type is a no-op.
	if ((sel == SurfaceSel::Wall) == m_map.IsWalkable(x, z)) return;
	const u32 rev = m_map.Revision();
	switch (sel) {
	case SurfaceSel::Wall:    m_map.SetWallVariant(x, z, variant); break;
	case SurfaceSel::Floor:   m_map.SetFloorVariant(x, z, variant); break;
	case SurfaceSel::Ceiling: m_map.SetCeilingVariant(x, z, variant); break;
	}
	if (m_map.Revision() == rev) return; // unchanged
	MarkSeen(x, z);
	RebuildChunksAround(x, z);
}

const gfx::Texture* DungeonWorld::SurfaceAlbedoForId(SurfaceSel sel,
													 const std::string& id) const {
	// The loaded albedo arrays sit in ACTIVE-palette order (LoadTextureSet), so
	// finding the id's palette index finds its texture.
	const std::vector<std::string>& pal = sel == SurfaceSel::Wall	 ? m_map.WallPalette()
										  : sel == SurfaceSel::Floor ? m_map.FloorPalette()
																	 : m_map.CeilingPalette();
	const Surface& surface = sel == SurfaceSel::Wall	? m_walls
							 : sel == SurfaceSel::Floor ? m_floors
														: m_ceilings;
	for (size_t i = 0; i < pal.size() && i < surface.albedo.size(); ++i)
		if (pal[i] == id) return surface.albedo[i].get();
	return nullptr;
}

bool DungeonWorld::AddDecoration(const std::string& type, int x, int z,
								 Direction facing) {
	if (!m_map.IsWalkable(x, z)) return false;
	if (!m_project.decorations.Contains(type)) return false;
	DecorationKind& kind = DecorationKindFor(type, m_project.decorations);
	Decoration deco;
	deco.kind = &kind;
	deco.x = x;
	deco.z = z;
	deco.facing = facing;
	const Vec3 pos = m_map.CellCenter(x, z);
	XMStoreFloat4x4(&deco.world, XMMatrixRotationY(DirYaw(facing)) *
									 XMMatrixTranslation(pos.x, 0, pos.z));
	deco.solid = kind.solidDefault;
	m_decorations.push_back(std::move(deco));
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::AddWallDecoration(const std::string& type, int x, int z,
									 Direction wall) {
	if (!m_map.IsWalkable(x, z)) return false;
	if (m_map.IsWalkable(x + DirDX(wall), z + DirDZ(wall))) return false; // nothing to hang on
	if (!m_project.decorations.Contains(type)) return false;
	DecorationKind& kind = DecorationKindFor(type, m_project.decorations);
	Decoration deco;
	deco.kind = &kind;
	deco.x = x;
	deco.z = z;
	deco.facing = wall;
	deco.wallMounted = true; // written back as the `wall=` record param
	deco.wall = wall;
	// Offset to the wall face and turned to look into the room — the same mount
	// helper the sconces use, so hung props line up with them.
	const WallMount m = MountOnWall(x, z, wall);
	XMStoreFloat4x4(&deco.world, XMMatrixRotationY(m.yaw) *
									 XMMatrixTranslation(m.pos.x, 0, m.pos.z));
	deco.solid = false; // it's on the wall — the floor stays walkable
	m_decorations.push_back(std::move(deco));
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::AddMonster(const std::string& type, int x, int z,
							  Direction facing) {
	if (!m_map.IsWalkable(x, z)) return false;
	if (!m_project.monsters.Contains(type)) return false;
	for (const Monster& m : m_monsters)
		if (m.x == x && m.z == z) return false; // one monster per cell
	MonsterKind& kind = MonsterKindFor(type);
	// id = -1 marks an editor-placed monster (no .ent baseline); the save layer
	// stores these whole (a "monster" row) rather than as a diff, so they
	// round-trip — see SnapshotActive / ApplyActiveSnapshot.
	m_monsters.push_back(MakeMonster(kind, -1, x, z, facing));
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::AddFixture(const std::string& type, int x, int z) {
	if (!m_map.IsWalkable(x, z)) return false;
	const CatalogEntry* def = m_project.fixtures.Find(type);
	// A flameless kind (empty bowl) places UNLIT so the map's turbidity grid —
	// which only smokes lit fixtures — stays truthful in the record too.
	const bool lit = CatalogBool(def, "flame", true);
	const bool ok = CatalogGet(def, "mount", "floor") == "wall"
						? m_map.AddSconce(x, z, type, lit)
						: m_map.AddBrazier(x, z, type, lit);
	if (!ok) return false;
	RebuildFiresAndDust(); // lights/flame/smoke pick the new fire up next frame
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::AddFixture(const std::string& type, int x, int z,
							  Direction wall) {
	if (!m_map.IsWalkable(x, z)) return false;
	const CatalogEntry* def = m_project.fixtures.Find(type);
	// Only a `mount = wall` kind has a face to hang on; a floor kind (brazier)
	// ignores the pick and stands at the cell centre as usual.
	if (CatalogGet(def, "mount", "floor") != "wall") return AddFixture(type, x, z);
	if (!m_map.AddSconce(x, z, type, CatalogBool(def, "flame", true), wall))
		return false;
	RebuildFiresAndDust();
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::AddNiche(const std::string& type, int x, int z) {
	if (!m_map.AddNiche(x, z, type)) return false; // no free solid wall
	RebuildChunksAround(x, z); // re-stamp the cell's wall panel as the niche
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::AddNiche(const std::string& type, int x, int z, Direction wall) {
	if (!m_map.AddNiche(x, z, type, wall)) return false; // not solid, or face taken
	RebuildChunksAround(x, z); // re-stamp that face's wall panel as the niche
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::RemoveNicheAtFace(int x, int z, Direction wall) {
	if (!m_map.RemoveNiche(x, z, wall)) return false;
	RebuildChunksAround(x, z); // re-stamp that face as a plain wall panel again
	return true;
}

bool DungeonWorld::RemoveNicheAtWall(int wx, int wz) {
	if (!m_map.RemoveNicheFacingWall(wx, wz)) return false;
	RebuildChunksAround(wx, wz); // covers the adjacent floor cell's chunk too
	return true;
}

bool DungeonWorld::AddBore(const std::string& type, int x, int z) {
	if (!m_map.AddBore(type, x, z)) return false;
	RebuildChunksAround(x, z); // re-stamps the two flanking floor cells' faces
	return true;
}

bool DungeonWorld::RemoveBoreAt(int x, int z) {
	if (!m_map.RemoveBoreAt(x, z)) return false;
	RebuildChunksAround(x, z);
	return true;
}

bool DungeonWorld::WallSeeThrough(int x, int z, int axis) const {
	// Permanent authored bores; a future see-through spell ORs a transient set in.
	return m_map.WallBoredAlong(x, z, axis);
}

bool DungeonWorld::RemoveEntityAt(int x, int z) {
	for (auto it = m_monsters.begin(); it != m_monsters.end(); ++it)
		if (it->x == x && it->z == z) {
			m_monsters.erase(it);
			return true;
		}
	for (auto it = m_doors.begin(); it != m_doors.end(); ++it)
		if (it->x == x && it->z == z) {
			// Doors are record-backed: the .ent record goes with the instance.
			m_entities.RemoveById(it->id);
			m_entsDirty = true;
			m_doors.erase(it);
			return true;
		}
	for (auto it = m_buttons.begin(); it != m_buttons.end(); ++it)
		if (it->x == x && it->z == z) {
			m_entities.RemoveById(it->id); // record-backed, like doors
			m_entsDirty = true;
			m_buttons.erase(it);
			return true;
		}
	for (auto it = m_decorations.begin(); it != m_decorations.end(); ++it)
		if (it->x == x && it->z == z && !it->stair) { // stairs: RemoveStairAt only
			m_decorations.erase(it);
			return true;
		}
	for (auto it = m_items.begin(); it != m_items.end(); ++it)
		if (!it->collected && it->x == x && it->z == z) {
			// Record-backed when authored (id >= 0); a session-dropped tablet
			// (id < 0) has no record to remove.
			if (it->id >= 0) {
				m_entities.RemoveById(it->id);
				m_entsDirty = true;
			}
			m_items.erase(it);
			return true;
		}
	return false;
}

// ============================================================================
// Doors. Record-backed (the .ent layer carries them, like items/buttons):
// placement authors the record AND spawns the live instance, so the writer,
// the level stash, and remote editing all see one source of truth. Open-state
// changes are dynamic (save diffs), never written back to the record.
// ============================================================================
DungeonWorld::Door* DungeonWorld::DoorAt(int x, int z) {
	for (Door& d : m_doors)
		if (d.x == x && d.z == z) return &d;
	return nullptr;
}

const DungeonWorld::Door* DungeonWorld::DoorAt(int x, int z) const {
	for (const Door& d : m_doors)
		if (d.x == x && d.z == z) return &d;
	return nullptr;
}

bool DungeonWorld::DoorwayFacing(const DungeonMap& map, int x, int z,
								 Direction& out) {
	if (!map.IsWalkable(x, z)) return false;
	const bool sidesEW = !map.IsWalkable(x - 1, z) && !map.IsWalkable(x + 1, z);
	const bool sidesNS = !map.IsWalkable(x, z - 1) && !map.IsWalkable(x, z + 1);
	if (sidesEW == sidesNS) return false; // open room, or boxed in — no doorway
	// Walls east+west -> the panel spans them, travel runs north-south.
	out = sidesEW ? Direction::North : Direction::East;
	return true;
}

void DungeonWorld::SpawnDoor(const Entity& record) {
	Door door;
	door.id = record.id;
	door.x = record.x;
	door.z = record.z;
	door.facing = record.facing;
	if (const std::string* n = record.Param("name")) door.name = *n;
	if (const std::string* k = record.Param("key")) door.key = *k;
	if (const std::string* o = record.Param("open")) door.initialOpen = *o != "0";
	door.open = door.initialOpen;
	door.openT = door.open ? 1.0f : 0.0f;
	door.panel = &DecorationKindFor(record.type, m_project.doors);
	door.frame = &DecorationKindFor("door_frame", m_project.doors);
	m_doors.push_back(std::move(door));
}

bool DungeonWorld::AddDoor(const std::string& type, int x, int z) {
	auto say = [&](const std::string& s) {
		if (onMessage) onMessage(s);
	};
	if (!m_project.doors.Contains(type) || DoorAt(x, z)) return false;
	Direction facing;
	if (!DoorwayFacing(m_map, x, z, facing)) {
		say(loc::Tr("map.door.nodoorway"));
		return false;
	}
	// Spawning a CLOSED door under the party or a monster would wall them in.
	if ((x == m_party.GridX() && z == m_party.GridZ()) ||
		MonsterRuntimeIdAt(x, z) != 0)
		return false;
	Entity record;
	record.kind = EntityKind::Door;
	record.type = type;
	record.x = x;
	record.z = z;
	record.facing = facing;
	record.id = m_entities.Add(record); // Add() assigns; mirror it locally
	m_entsDirty = true;
	SpawnDoor(record);
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::AddDoorRemote(const std::string& stem,
								 const std::string& type, int x, int z) {
	DungeonEntities& ents = EnsureEntStash(stem);
	const DungeonMap& map = *m_levelMaps.find(stem)->second;
	if (!m_project.doors.Contains(type)) return false;
	Direction facing;
	if (!DoorwayFacing(map, x, z, facing)) {
		if (onMessage) onMessage(loc::Tr("map.door.nodoorway"));
		return false;
	}
	for (const Entity& e : ents.At(x, z))
		if (e.kind == EntityKind::Door) return false; // one door per cell
	Entity record;
	record.kind = EntityKind::Door;
	record.type = type;
	record.x = x;
	record.z = z;
	record.facing = facing;
	ents.Add(std::move(record));
	return true;
}

bool DungeonWorld::ToggleDoor(Door& door) {
	// Anything standing in the doorway jams a closing panel.
	if (door.open && MonsterRuntimeIdAt(door.x, door.z) != 0) {
		if (onMessage) onMessage(loc::Tr("log.door_jammed"));
		return false;
	}
	door.open = !door.open;
	if (onMessage)
		onMessage(loc::Tr(door.open ? "log.door_open" : "log.door_close"));
	return true;
}

bool DungeonWorld::ToggleDoorAhead() {
	const Direction f = static_cast<Direction>(m_party.Facing());
	Door* door = DoorAt(m_party.GridX() + DirDX(f), m_party.GridZ() + DirDZ(f));
	if (!door) return false;
	// A keyed door refuses the hand unless a member carries the key item (the
	// key stays — the door re-locks when shut). Wired buttons still move it
	// (mechanisms don't need the key).
	if (!door->open && !door->key.empty()) {
		if (!PartyHasItem(door->key)) {
			if (onMessage) onMessage(loc::Tr("log.door_locked"));
			return true;
		}
		if (onMessage) onMessage(loc::Tr("log.door_unlock"));
	}
	ToggleDoor(*door);
	return true; // the click was for the door even if it jammed
}

bool DungeonWorld::PartyHasItem(std::string_view typeId) const {
	if (typeId.empty() || !m_roster) return false;
	for (const Character& member : *m_roster)
		if (member.inventory.Has(typeId)) return true;
	return false;
}

bool DungeonWorld::DoorSettings(int x, int z, bool& open, std::string& key,
								std::string& name) const {
	const Door* door = DoorAt(x, z);
	if (!door) return false;
	open = door->open;
	key = door->key;
	name = door->name;
	return true;
}

void DungeonWorld::SetDoorSettings(int x, int z, bool open, const std::string& key,
								   const std::string& name) {
	Door* door = DoorAt(x, z);
	if (!door) return;
	door->open = open;
	door->initialOpen = open; // the editor edits the AUTHORED state
	door->key = key;
	door->name = name;
	// Mirror onto the .ent record so the writer/stash carry it. Default-valued
	// params are removed to keep records minimal.
	if (Entity* record = m_entities.MutableById(door->id)) {
		auto set = [&](const char* k, const std::string& v) {
			std::erase_if(record->params,
						  [&](const auto& p) { return p.first == k; });
			if (!v.empty()) record->params.emplace_back(k, v);
		};
		set("open", open ? "1" : "");
		set("key", key);
		set("name", name);
		m_entsDirty = true;
	}
}

std::vector<gfx::PreviewSubmesh> DungeonWorld::DoorPreviewSubs(int x, int z) const {
	std::vector<gfx::PreviewSubmesh> subs;
	const Door* door = DoorAt(x, z);
	if (!door) return subs;
	for (const DecorationKind* kind : {door->frame, door->panel}) {
		if (!kind || !kind->mesh) continue;
		gfx::MaterialParams mat;
		mat.doubleSided = true;
		ApplyPropMaterial(mat, *kind, 0.85f);
		subs.push_back({kind->mesh.get(), mat});
	}
	return subs;
}

void DungeonWorld::ToggleDoorsNamed(const std::string& name) {
	if (name.empty()) return;
	for (Door& door : m_doors)
		if (door.name == name) ToggleDoor(door);
}

// A button targeting `name` also opens/closes any niche with that name — the
// secret-niche reveal. Flips the map's runtime `open` and re-stamps each touched
// cell's wall panel (blank ⇄ recessed pocket).
bool DungeonWorld::ToggleNichesNamed(const std::string& name) {
	const std::vector<std::pair<int, int>> touched = m_map.ToggleNichesNamed(name);
	for (const auto& [x, z] : touched) RebuildChunksAround(x, z);
	return !touched.empty();
}

std::vector<std::string> DungeonWorld::NicheNames() const { return m_map.NicheNames(); }

std::vector<DungeonWorld::NicheFace> DungeonWorld::NicheFacesAt(int cx, int cz) const {
	// A niche is selected ONLY by clicking the WALL BLOCK it is carved into — it
	// reads as being IN the wall, so clicking the floor square it opens onto (even
	// though the niche is registered there) does nothing, which is less confusing.
	std::vector<NicheFace> faces;
	if (m_map.IsWalkable(cx, cz)) return faces; // clicked a floor cell — not a wall
	const int nbr[4][2] = {{cx, cz - 1}, {cx + 1, cz}, {cx, cz + 1}, {cx - 1, cz}};
	for (const auto& f : nbr)
		if (const WallNiche* n = m_map.NicheAt(f[0], f[1], cx - f[0], cz - f[1]))
			faces.push_back({f[0], f[1], n->wall});
	return faces;
}

const WallNiche* DungeonWorld::NicheOn(int x, int z, Direction wall) const {
	return m_map.NicheAt(x, z, DirDX(wall), DirDZ(wall));
}

void DungeonWorld::SetNichePropsAt(int x, int z, Direction wall, const std::string& name,
								   bool hidden, const std::string& type) {
	if (m_map.SetNichePropsAt(x, z, wall, name, hidden, type)) RebuildChunksAround(x, z);
}

bool DungeonWorld::RemoveNiche(int x, int z, Direction wall) {
	if (!m_map.RemoveNiche(x, z, wall)) return false;
	RebuildChunksAround(x, z);
	return true;
}

std::vector<DungeonWorld::DoorMarker> DungeonWorld::DoorMarkers() const {
	std::vector<DoorMarker> markers;
	markers.reserve(m_doors.size());
	for (const Door& d : m_doors) markers.push_back({d.x, d.z, d.facing, d.open});
	return markers;
}

// ============================================================================
// Buttons. Record-backed like doors; the lever mounts on a solid wall of its
// cell and toggles the doors its target= names when pressed.
// ============================================================================
bool DungeonWorld::AddButton(const std::string& type, int x, int z) {
	if (!m_project.buttons.Contains(type) || !m_map.IsWalkable(x, z)) return false;
	for (const Button& b : m_buttons)
		if (b.x == x && b.z == z) return false; // one button per cell (editor rule)
	// Auto-mount on the first solid neighbour wall, like the 'T' glyph.
	constexpr Direction kScan[4] = {Direction::North, Direction::East,
									Direction::South, Direction::West};
	Direction wall = Direction::North;
	bool found = false;
	for (const Direction d : kScan)
		if (!m_map.IsWalkable(x + DirDX(d), z + DirDZ(d))) {
			wall = d;
			found = true;
			break;
		}
	if (!found) {
		if (onMessage) onMessage(loc::Tr("map.button.nowall"));
		return false;
	}
	Entity record;
	record.kind = EntityKind::Button;
	record.type = type;
	record.x = x;
	record.z = z;
	record.facing = wall;
	record.id = m_entities.Add(record);
	m_entsDirty = true;
	Button b;
	b.id = record.id;
	b.x = x;
	b.z = z;
	b.facing = wall;
	b.kind = &DecorationKindFor(type, m_project.buttons);
	m_buttons.push_back(std::move(b));
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::AddButtonRemote(const std::string& stem,
								   const std::string& type, int x, int z) {
	DungeonEntities& ents = EnsureEntStash(stem);
	const DungeonMap& map = *m_levelMaps.find(stem)->second;
	if (!m_project.buttons.Contains(type) || !map.IsWalkable(x, z)) return false;
	for (const Entity& e : ents.At(x, z))
		if (e.kind == EntityKind::Button) return false;
	constexpr Direction kScan[4] = {Direction::North, Direction::East,
									Direction::South, Direction::West};
	Direction wall = Direction::North;
	bool found = false;
	for (const Direction d : kScan)
		if (!map.IsWalkable(x + DirDX(d), z + DirDZ(d))) {
			wall = d;
			found = true;
			break;
		}
	if (!found) {
		if (onMessage) onMessage(loc::Tr("map.button.nowall"));
		return false;
	}
	Entity record;
	record.kind = EntityKind::Button;
	record.type = type;
	record.x = x;
	record.z = z;
	record.facing = wall;
	ents.Add(std::move(record));
	return true;
}

bool DungeonWorld::AddItem(const std::string& type, int x, int z) {
	if (!m_project.items.Contains(type) || !m_map.IsWalkable(x, z)) return false;
	// One item per quarter slot — a full cell (4 on the floor) refuses rather
	// than letting FreeItemSlotNear stack overlapping tablets. Niche items pile
	// separately (they don't use the floor quarters), so they don't count.
	int here = 0;
	for (const Item& it : m_items)
		if (!it.collected && it.niche < 0 && it.x == x && it.z == z) ++here;
	if (here >= 4) return false;
	Entity record;
	record.kind = EntityKind::Item;
	record.type = type;
	record.x = x;
	record.z = z;
	record.id = m_entities.Add(record);
	m_entsDirty = true;
	ItemKind& kind = ItemKindFor(type);
	const Vec3 c = m_map.CellCenter(x, z);
	const int slot = FreeItemSlotNear(x, z, c.x, c.z, -1);
	m_items.push_back({&kind, record.id, x, z, false, slot});
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::NicheOpenAt(int x, int z, Direction wall) const {
	const WallNiche* n = m_map.NicheAt(x, z, DirDX(wall), DirDZ(wall));
	return n && n->open;
}

Vec3 DungeonWorld::NicheItemPos(int x, int z, Direction wall) const {
	const int dx = DirDX(wall), dz = DirDZ(wall);
	const Vec3 c = m_map.CellCenter(x, z);
	const float into = kCellSize * 0.5f + 0.18f; // just inside the wall, in the pocket
	// Rest on the pocket floor — its height is the niche mesh's py0 (must track
	// ModelBaker's BuildWallNiche / BuildWallNicheArch): 0.75 for the plain niche,
	// 0.50 for the arch. Placing below it would bury the item behind the frame.
	float floorY = 0.75f;
	if (const WallNiche* n = m_map.NicheAt(x, z, dx, dz); n && n->type == "niche_arch")
		floorY = 0.50f;
	return {c.x + dx * into, floorY + 0.02f, c.z + dz * into};
}

bool DungeonWorld::AddNicheItem(const std::string& type, int x, int z, Direction wall) {
	if (!m_project.items.Contains(type)) return false;
	if (!m_map.NicheAt(x, z, DirDX(wall), DirDZ(wall))) return false; // no niche here
	Entity record;
	record.kind = EntityKind::Item;
	record.type = type;
	record.x = x;
	record.z = z;
	static const char* kDirName[4] = {"north", "east", "south", "west"}; // Direction order
	record.params.emplace_back("niche", kDirName[static_cast<int>(wall)]); // .ent round-trips it
	record.id = m_entities.Add(record);
	m_entsDirty = true;
	ItemKind& kind = ItemKindFor(type);
	m_items.push_back({&kind, record.id, x, z, false, 0, static_cast<int>(wall)});
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::AddItemRemote(const std::string& stem,
								 const std::string& type, int x, int z) {
	DungeonEntities& ents = EnsureEntStash(stem);
	const DungeonMap& map = *m_levelMaps.find(stem)->second;
	if (!m_project.items.Contains(type) || !map.IsWalkable(x, z)) return false;
	int here = 0;
	for (const Entity& e : ents.At(x, z))
		if (e.kind == EntityKind::Item) ++here;
	if (here >= 4) return false; // one per quarter, like the live rule
	Entity record;
	record.kind = EntityKind::Item;
	record.type = type;
	record.x = x;
	record.z = z;
	ents.Add(std::move(record));
	return true;
}

bool DungeonWorld::PressButtonFacing() {
	const Direction f = static_cast<Direction>(m_party.Facing());
	for (Button& b : m_buttons)
		if (b.x == m_party.GridX() && b.z == m_party.GridZ() && b.facing == f) {
			b.activated = !b.activated;
			m_audio.Play(m_sounds.bump, 0.4f); // a soft clunk until a click exists
			if (onMessage) onMessage(loc::Tr("log.button_press"));
			ToggleDoorsNamed(b.target);
			ToggleNichesNamed(b.target); // secret-niche reveal
			return true;
		}
	return false;
}

bool DungeonWorld::ButtonSettings(int x, int z, std::string& target) const {
	for (const Button& b : m_buttons)
		if (b.x == x && b.z == z) {
			target = b.target;
			return true;
		}
	return false;
}

void DungeonWorld::SetButtonSettings(int x, int z, const std::string& target) {
	for (Button& b : m_buttons)
		if (b.x == x && b.z == z) {
			b.target = target;
			if (Entity* record = m_entities.MutableById(b.id)) {
				std::erase_if(record->params,
							  [](const auto& p) { return p.first == "target"; });
				if (!target.empty()) record->params.emplace_back("target", target);
				m_entsDirty = true;
			}
			return;
		}
}

std::vector<std::string> DungeonWorld::DoorNames() const {
	std::vector<std::string> names;
	for (const Door& d : m_doors)
		if (!d.name.empty() &&
			std::find(names.begin(), names.end(), d.name) == names.end())
			names.push_back(d.name);
	return names;
}

std::vector<gfx::PreviewSubmesh> DungeonWorld::ButtonPreviewSubs(int x, int z) const {
	std::vector<gfx::PreviewSubmesh> subs;
	for (const Button& b : m_buttons)
		if (b.x == x && b.z == z && b.kind && b.kind->mesh) {
			gfx::MaterialParams mat;
			mat.doubleSided = true;
			ApplyPropMaterial(mat, *b.kind, 0.85f);
			subs.push_back({b.kind->mesh.get(), mat});
			break;
		}
	return subs;
}

// ============================================================================
// Stair pairs (editor). The ACTIVE level's half is live state (link + prop,
// persisted by `savemap` like every other edit); the DESTINATION level's half
// is edited textually in its .map file — one appended / removed "stairs ..."
// line — because that level isn't loaded, and its static layer is re-parsed
// from disk on every entry, so the file is authoritative. Validate-then-write:
// the destination map is parse-checked first (its loader DN_ASSERTs on bad
// records, so nothing invalid may ever be written).
// ============================================================================
namespace {

const char* DirName(Direction d) {
	switch (d) {
	case Direction::North: return "north";
	case Direction::East:  return "east";
	case Direction::West:  return "west";
	default:               return "south";
	}
}

} // namespace

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

	// The type's direction (stairs.cat `up`) picks the destination: the previous
	// / next stem from `stem` in the project's level order (the vertical stack).
	const bool up = CatalogBool(entry, "up", false);
	const auto& levels = m_project.levels;
	const auto cur = std::find(levels.begin(), levels.end(), stem);
	std::string dest;
	if (cur != levels.end()) {
		if (up && cur != levels.begin()) dest = *(cur - 1);
		if (!up && cur + 1 != levels.end()) dest = *(cur + 1);
	}
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
	StairLink pair = link;
	pair.type = pairType;
	pair.destLevel = stem;

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
	if (onMessage)
		onMessage(pair ? loc::Format("map.stairs.removed", removed.destLevel)
					   : loc::Tr("map.erase.removed"));
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
	e.params.emplace_back("wall", DirName(wall)); // hangs flat on that wall
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
		map.RemoveNicheFacingWall(x, z)) {
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

void DungeonWorld::BeginLevelLoad(const std::string& stem, bool stashCurrent) {
	m_device.WaitIdle(); // the GPU may still be reading the old level's meshes

	// Undo steps snapshot the ACTIVE level's live state, so they are only
	// restorable while that level is live — a transition invalidates them.
	ClearUndoHistory();

	// Save the level we're leaving so a later return restores its fog/progress
	// AND its unsaved edits (skip for a throwaway baseline being replaced by a
	// save's level). The .ent records stash only when they diverged from disk
	// (a prune/re-face edited them) — else the file re-parse is identical.
	if (stashCurrent) {
		StashActive();
		StashStaticMap();
		if (m_entsDirty)
			m_levelEnts.insert_or_assign(
				m_currentLevel, std::make_unique<DungeonEntities>(m_entities));
	}

	// Move-assign the new level into the existing objects (Party holds a
	// reference to m_map, so the object must persist — only its data changes).
	// A stashed level (this session's unsaved editor edits) takes precedence
	// over the files on disk, layer by layer.
	if (auto it = m_levelMaps.find(stem); it != m_levelMaps.end()) {
		m_map = std::move(*it->second);
		m_levelMaps.erase(it);
	} else {
		m_map = DungeonMap(m_project.LevelMapPath(stem), FixtureTypesOf(m_project));
	}
	if (auto it = m_levelEnts.find(stem); it != m_levelEnts.end()) {
		m_entities = std::move(*it->second);
		m_levelEnts.erase(it);
		m_entsDirty = true; // still diverged from the file; re-stash on leave
	} else {
		m_entities = DungeonEntities(m_project.LevelEntPath(stem), m_map);
		m_entsDirty = false;
	}
	m_currentLevel = stem;

	// Reset per-level state. The shared caches (m_monsterKinds, m_decorationKinds,
	// m_propTextures) persist — they are keyed by name and reused across levels.
	// Instance lists must be cleared (LoadMonsters/LoadDecorations/BuildFires
	// push_back); the surface chunks/blocks/textures self-reset when the caller
	// re-runs AppendLoadTasks.
	m_seen.assign(static_cast<size_t>(m_map.Width()) * m_map.Height(), 0);
	m_monsters.clear(); // new monsters get fresh runtimeIds; stale plans find no match
	m_walkableCache.reset(); // force a fresh walkability grid for the new level's map
	m_items.clear();
	m_buttons.clear();
	m_doors.clear();
	m_decorations.clear();
	m_fires.clear();
	m_projectiles.Clear(); // bolts/sparks don't survive a level change
	m_pendingTransition.reset();
	m_pendingFall.reset(); // the swap IS the fall's end
	m_fallT = -1.0f;
	m_shadows.InvalidateCubes();
	ResolveSurfacePalettes();
}

std::vector<Entity> DungeonWorld::LiveDecorationRecords() const {
	// Live decoration placements as .map records (mirrors the SaveLevel
	// writer's decoration emit; stair props are stairs records).
	std::vector<Entity> records;
	records.reserve(m_decorations.size());
	for (const Decoration& d : m_decorations) {
		if (d.stair || !d.kind || d.kind->id.empty()) continue;
		Entity e;
		e.kind = EntityKind::Decoration;
		e.type = d.kind->id;
		e.x = d.x;
		e.z = d.z;
		e.facing = d.facing;
		if (d.wallMounted) {
			e.params.emplace_back("wall", DirName(d.wall));
			if (d.solid) e.params.emplace_back("solid", "1");
		} else if (d.solid != d.kind->solidDefault) {
			e.params.emplace_back("solid", d.solid ? "1" : "0");
		}
		records.push_back(std::move(e));
	}
	return records;
}

void DungeonWorld::StashStaticMap() {
	auto copy = std::make_unique<DungeonMap>(m_map);
	copy->SetDecorationRecords(LiveDecorationRecords());
	m_levelMaps.insert_or_assign(m_currentLevel, std::move(copy));
}

std::optional<DungeonWorld::LevelTransition> DungeonWorld::ConsumeLevelTransition() {
	std::optional<LevelTransition> t = std::move(m_pendingTransition);
	m_pendingTransition.reset();
	return t;
}

namespace {
const char* KindName(EntityKind k) {
	switch (k) {
	case EntityKind::Monster:    return "monster";
	case EntityKind::Button:     return "button";
	case EntityKind::Decoration: return "decoration";
	case EntityKind::Door:       return "door";
	default:                     return "item";
	}
}
} // namespace

// Serializes a level's static layer (.map). `decoLines` carries the decoration
// records, pre-serialized by the caller: the ACTIVE level derives them from
// live instances (SaveLevel), a stashed level already holds them as records
// (WriteStashedLevel). Everything else lives on the DungeonMap.
static std::string SerializeMapStatic(const std::string& stem,
									  const DungeonMap& map,
									  const std::string& decoLines) {
	std::string m = std::format("; {} — written by the in-game editor.\n\n", stem);
	auto palette = [&](const char* surface, const std::vector<std::string>& ids) {
		if (ids.empty()) return;
		m += std::format("palette {}", surface);
		for (const std::string& id : ids) m += " " + id;
		m += '\n';
	};
	palette("wall", map.WallPalette());
	palette("floor", map.FloorPalette());
	palette("ceiling", map.CeilingPalette());
	// Per-level atmosphere (the Level settings dialog's mood knobs): only set
	// values are written — an untouched level carries no record and follows
	// the world defaults.
	if (map.DustDensity() >= 0.0f || map.HazeAmbient() >= 0.0f ||
		map.AmbientScale() >= 0.0f) {
		m += "atmosphere";
		if (map.DustDensity() >= 0.0f) m += std::format(" dust={:g}", map.DustDensity());
		if (map.HazeAmbient() >= 0.0f) m += std::format(" haze={:g}", map.HazeAmbient());
		if (map.AmbientScale() >= 0.0f)
			m += std::format(" ambient={:g}", map.AmbientScale());
		m += '\n';
	}
	m += ";\n";

	// Grid: 'P' start, '#' wall, 'D' authored-dusty floor, '.' floor. Fixtures
	// are emitted as records below, so their cells stay plain floor.
	for (int z = 0; z < map.Height(); ++z) {
		std::string row(static_cast<size_t>(map.Width()), '.');
		for (int x = 0; x < map.Width(); ++x) {
			if (x == map.StartX() && z == map.StartZ()) row[x] = 'P';
			else if (map.At(x, z) == Cell::Wall) row[x] = '#';
			else if (map.AuthoredDusty(x, z)) row[x] = 'D';
		}
		m += row;
		m += '\n';
	}
	m += ";\n";

	// The kind token is the instance's fixtures.cat id (the parser fills the
	// default for glyph shorthand, so it is never empty).
	for (const WallSconce& s : map.Sconces()) {
		m += std::format("fixture {} {} {} {}", s.type, s.x, s.z, DirName(s.wall));
		// Only non-default light/smoke settings are written (keeps the .map minimal).
		if (!s.lit) m += " lit=0";
		if (s.brightness != kSconceBrightness) m += std::format(" bright={:g}", s.brightness);
		if (s.turbidity != kSconceTurbidity) m += std::format(" turb={:g}", s.turbidity);
		m += '\n';
	}
	for (const FloorBrazier& b : map.Braziers()) {
		m += std::format("fixture {} {} {}", b.type, b.x, b.z);
		if (!b.lit) m += " lit=0";
		if (b.brightness != kBrazierBrightness) m += std::format(" bright={:g}", b.brightness);
		if (b.turbidity != kBrazierTurbidity) m += std::format(" turb={:g}", b.turbidity);
		m += '\n';
	}

	for (const WallNiche& n : map.Niches()) {
		m += std::format("niche {} {} {} {}", n.type, n.x, n.z, DirName(n.wall));
		if (!n.name.empty()) m += std::format(" name={}", n.name); // button target id
		if (n.hidden) m += " hidden=1"; // starts closed (runtime open state = save)
		m += '\n';
	}
	for (const WallBore& b : map.Bores())
		m += std::format("bore {} {} {} {}\n", b.type, b.x, b.z, b.axis);

	for (const StairLink& s : map.Stairs())
		m += std::format("stairs {} {} {} {} dest={} destx={} destz={} destfacing={}\n",
						 s.type, s.x, s.z, DirName(s.facing), s.destLevel, s.destX,
						 s.destZ, DirName(s.destFacing));

	for (int z = 0; z < map.Height(); ++z)
		for (int x = 0; x < map.Width(); ++x) {
			if (map.WallVariant(x, z) >= 0)
				m += std::format("variant wall {} {} {}\n", x, z, map.WallVariant(x, z));
			if (map.FloorVariant(x, z) >= 0)
				m += std::format("variant floor {} {} {}\n", x, z, map.FloorVariant(x, z));
			if (map.CeilingVariant(x, z) >= 0)
				m += std::format("variant ceiling {} {} {}\n", x, z, map.CeilingVariant(x, z));
		}

	m += decoLines;
	return m;
}

// One generic entity record line: kind, type, cell, facing, then the record's
// key=value params verbatim (a stashed record already carries everything).
static std::string SerializeRecord(const char* kind, const Entity& e) {
	std::string line = std::format("{} {} {} {} {}", kind, e.type, e.x, e.z,
								   DirName(e.facing));
	for (const auto& [k, v] : e.params) line += std::format(" {}={}", k, v);
	line += '\n';
	return line;
}

bool DungeonWorld::SaveLevel() const {
	// Decorations reconstructed from the live instances (so editor placements /
	// removals persist); stair props are skipped — they are stairs records.
	std::string deco;
	for (const Decoration& d : m_decorations) {
		if (d.stair || !d.kind || d.kind->id.empty()) continue;
		if (d.wallMounted) {
			deco += std::format("decoration {} {} {} wall={}", d.kind->id, d.x,
								d.z, DirName(d.wall));
			if (d.solid) deco += " solid=1";
		} else {
			deco += std::format("decoration {} {} {} {}", d.kind->id, d.x, d.z,
								DirName(d.facing));
			if (d.solid != d.kind->solidDefault)
				deco += std::format(" solid={}", d.solid ? 1 : 0);
		}
		deco += '\n';
	}
	std::string m = SerializeMapStatic(m_currentLevel, m_map, deco);

	// --- dynamic layer (.ent) -----------------------------------------------
	std::string e =
		std::format("; {} — written by the in-game editor (dynamic layer).\n\n", m_currentLevel);
	for (const Monster& mon : m_monsters) {
		e += std::format("monster {} {} {} {}", mon.kind ? mon.kind->name : std::string("?"),
						 mon.x, mon.z, DirName(mon.facing));
		// Per-instance AI overrides (authored, round-tripped by the editor inspector).
		if (mon.asleep) e += " asleep=1";
		if (mon.leashRange > 0.0f) e += std::format(" leash={:g}", mon.leashRange);
		if (mon.leashX != mon.spawnX || mon.leashZ != mon.spawnZ)
			e += std::format(" leashfrom={},{}", mon.leashX, mon.leashZ);
		// Per-instance behaviour overrides (only when they differ from the type).
		static const char* kArch[] = {"brute",   "skirmisher", "caster",
									  "swarm", "lurker",     "sentry"};
		if (mon.archOverride) e += std::format(" archetype={}", kArch[static_cast<int>(*mon.archOverride)]);
		if (mon.keepOverride) e += std::format(" keeprange={:g}", *mon.keepOverride);
		if (mon.fleeOverride) e += std::format(" fleebelow={:g}", *mon.fleeOverride);
		if (mon.spellOverride && !mon.spellOverride->empty())
			e += std::format(" spell={}", *mon.spellOverride);
		if (!mon.patrol.empty()) {
			e += " patrol=";
			for (size_t k = 0; k < mon.patrol.size(); ++k)
				e += std::format("{}{},{}", k ? ";" : "", mon.patrol[k].x, mon.patrol[k].z);
		}
		e += '\n';
	}
	// Items/buttons/doors are record-backed (placement/erase edits m_entities
	// directly), so their records ARE current.
	for (const Entity& ent : m_entities.All()) {
		if (ent.kind != EntityKind::Item && ent.kind != EntityKind::Button &&
			ent.kind != EntityKind::Door)
			continue;
		e += SerializeRecord(KindName(ent.kind), ent);
	}

	const bool okMap =
		assets::WriteBinaryFile(m_project.LevelMapPath(m_currentLevel), m.data(), m.size());
	const bool okEnt =
		assets::WriteBinaryFile(m_project.LevelEntPath(m_currentLevel), e.data(), e.size());
	if (okMap && okEnt)
		log::Info("Saved level {}: {} decorations, {} monsters", m_currentLevel,
				  m_decorations.size(), m_monsters.size());
	else
		log::Warn("Failed to write level {} files", m_currentLevel);
	return okMap && okEnt;
}

bool DungeonWorld::WriteStashedLevel(const std::string& stem) const {
	const auto ms = m_levelMaps.find(stem);
	if (ms == m_levelMaps.end()) return false;
	const DungeonMap& map = *ms->second;

	// A stash's decorations are already records (the live-instance sync happens
	// when the map is stashed / remote edits author records directly).
	std::string deco;
	for (const Entity& e : map.Decorations())
		deco += SerializeRecord("decoration", e);

	const std::string m = SerializeMapStatic(stem, map, deco);
	bool ok = assets::WriteBinaryFile(m_project.LevelMapPath(stem), m.data(),
									  m.size());

	// The .ent is rewritten only when its records were edited (a stash exists);
	// an untouched dynamic layer keeps its file byte-identical.
	if (const auto es = m_levelEnts.find(stem); es != m_levelEnts.end()) {
		std::string e = std::format(
			"; {} — written by the in-game editor (dynamic layer).\n\n", stem);
		for (const Entity& ent : es->second->All())
			e += SerializeRecord(KindName(ent.kind), ent);
		ok &= assets::WriteBinaryFile(m_project.LevelEntPath(stem), e.data(),
									  e.size());
	}
	if (ok) log::Info("Saved stashed level {}", stem);
	else log::Warn("Failed to write stashed level {} files", stem);
	return ok;
}

std::vector<std::string> DungeonWorld::SaveAllLevels() {
	std::vector<std::string> saved;
	if (SaveLevel()) saved.push_back(m_currentLevel);
	for (const auto& [stem, map] : m_levelMaps)
		if (WriteStashedLevel(stem)) saved.push_back(stem);
	return saved;
}

bool DungeonWorld::RenameLevel(const std::string& oldStem,
							   const std::string& newStem) {
	// Disk first (the .ent failure path can roll the .map back, keeping the
	// pair consistent) — a stash-backed level renames its files too, so the
	// next savemap writes to the new paths and no stale pair lingers.
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::rename(m_project.LevelMapPath(oldStem), m_project.LevelMapPath(newStem),
			   ec);
	if (ec) {
		log::Warn("rename level: map move failed: {}", ec.message());
		return false;
	}
	fs::rename(m_project.LevelEntPath(oldStem), m_project.LevelEntPath(newStem),
			   ec);
	if (ec) {
		std::error_code undo;
		fs::rename(m_project.LevelMapPath(newStem),
				   m_project.LevelMapPath(oldStem), undo);
		log::Warn("rename level: ent move failed: {}", ec.message());
		return false;
	}

	// In-memory keys follow the stem: the three per-level stashes...
	auto rekey = [&](auto& stash) {
		auto it = stash.find(oldStem);
		if (it == stash.end()) return;
		auto node = std::move(it->second);
		stash.erase(oldStem);
		stash.insert_or_assign(newStem, std::move(node));
	};
	rekey(m_levelMaps);
	rekey(m_levelEnts);
	if (auto it = m_levelStates.find(oldStem); it != m_levelStates.end()) {
		SaveData::LevelState state = std::move(it->second);
		m_levelStates.erase(oldStem);
		state.stem = newStem; // the block writes its own stem line
		m_levelStates.insert_or_assign(newStem, std::move(state));
	}
	// ...and the active stem.
	if (m_currentLevel == oldStem) m_currentLevel = newStem;

	// Repoint every stair dest= that names the old stem: the active map is
	// fixed live, every other level via its stash — EnsureMapStash lazily
	// parses disk-only levels, so their fix persists on the next savemap.
	// (The caller updates Project::levels after this returns, so the walk
	// still sees the OLD stem in the list — map it to the new one.)
	m_map.RenameStairDest(oldStem, newStem);
	for (const std::string& stem : m_project.levels) {
		const std::string& actual = stem == oldStem ? newStem : stem;
		if (actual == m_currentLevel) continue;
		EnsureMapStash(actual).RenameStairDest(oldStem, newStem);
	}

	// Undo snapshots hold whole stash sets keyed by the old stem (and the old
	// file paths' contents); restoring one across a rename would resurrect the
	// dead name. Renames are rare — drop the history like a level transition.
	ClearUndoHistory();

	// NOTE: existing save FILES still reference the old stem (save current= /
	// level blocks) — loading one after a rename will die on the missing
	// level. Editor-side renames assume dev-cycle saves; re-save after.
	log::Info("Renamed level {} -> {}", oldStem, newStem);
	return true;
}

// ============================================================================
// Editor undo/redo. Snapshot-based — see the header for the design rationale.
// ============================================================================
DungeonWorld::EditorSnapshot DungeonWorld::CaptureEditorState() const {
	EditorSnapshot s{m_currentLevel, m_map,           m_entities,
					 m_entsDirty,    SnapshotActive(), {},
					 {}};
	// The map copy carries the live decoration placements as records, so the
	// restore's LoadDecorations rebuilds them (AddDecoration only appends a
	// live instance — same sync a level-swap stash does).
	s.map.SetDecorationRecords(LiveDecorationRecords());
	for (const auto& [stem, map] : m_levelMaps)
		s.stashMaps.emplace(stem, std::make_unique<DungeonMap>(*map));
	for (const auto& [stem, ents] : m_levelEnts)
		s.stashEnts.emplace(stem, std::make_unique<DungeonEntities>(*ents));
	return s;
}

void DungeonWorld::RestoreEditorState(EditorSnapshot snap) {
	// Cheap in editor mode (only sprite work is queued — the scene passes are
	// skipped while the full-screen editor is up), and still required: the
	// turbidity texture below is replaced, and an in-flight frame must not
	// read a freed resource.
	m_device.WaitIdle();

	// Static layer + records (move-assign like BeginLevelLoad — Party holds a
	// reference to m_map, so the object must persist, only its data changes).
	m_map = std::move(snap.map);
	m_entities = std::move(snap.ents);
	m_entsDirty = snap.entsDirty;
	// Remote-edited stashes are replaced wholesale: a level with no stash in
	// the snapshot reverts to file authority (its entry is simply gone).
	m_levelMaps.clear();
	for (auto&& [stem, map] : snap.stashMaps)
		m_levelMaps.insert_or_assign(stem, std::move(map));
	m_levelEnts.clear();
	for (auto&& [stem, ents] : snap.stashEnts)
		m_levelEnts.insert_or_assign(stem, std::move(ents));

	// Dynamic layer: respawn from the records, then apply the captured live
	// diffs — the same flow a level re-entry uses (editor-placed monsters ride
	// the snapshot's whole-spawn rows).
	m_monsters.clear(); // fresh runtimeIds; stale async AI plans find no match
	m_items.clear();
	m_buttons.clear();
	m_doors.clear();
	m_decorations.clear();
	m_walkableCache.reset();
	LoadDecorations();
	LoadStairs();
	LoadMonsters();
	LoadItems();
	LoadButtons();
	LoadDoors();
	m_levelStates[m_currentLevel] = std::move(snap.state);
	ApplyActiveSnapshot();

	// Fires/turbidity from the restored fixtures. The SURFACE rebake is
	// deferred (m_geometryDirty → FlushGeometry): any cell may differ, so it
	// would be the full quality-swap rebuild — but the full-screen editor
	// hides the scene, so the stale chunks are never drawn, undo stays fast,
	// and a whole editing session pays for one rebake on the way out.
	RebuildFiresAndDust();
	m_geometryDirty = true;
}

void DungeonWorld::FlushGeometry() {
	if (!m_geometryDirty) return;
	m_device.WaitIdle(); // in-flight frames may still read the old chunk meshes
	m_walls.chunks.clear();
	m_floors.chunks.clear();
	m_ceilings.chunks.clear();
	BuildDungeonMeshes(); // clears m_geometryDirty itself
	m_shadows.InvalidateCubes();
}

void DungeonWorld::BeginUndoStep() {
	m_pendingUndo = CaptureEditorState();
}

void DungeonWorld::CommitUndoStep(bool changed) {
	if (!m_pendingUndo) return;
	if (changed) {
		constexpr size_t kMaxUndoSteps = 64;
		m_undoStack.push_back(std::move(*m_pendingUndo));
		if (m_undoStack.size() > kMaxUndoSteps)
			m_undoStack.erase(m_undoStack.begin());
		m_redoStack.clear(); // a new edit forks history
	}
	m_pendingUndo.reset();
}

void DungeonWorld::Undo() {
	if (m_undoStack.empty()) {
		if (onMessage) onMessage(loc::Tr("map.undo.none"));
		return;
	}
	m_redoStack.push_back(CaptureEditorState());
	EditorSnapshot snap = std::move(m_undoStack.back());
	m_undoStack.pop_back();
	RestoreEditorState(std::move(snap));
	if (onMessage) onMessage(loc::Tr("map.undo.done"));
}

void DungeonWorld::Redo() {
	if (m_redoStack.empty()) {
		if (onMessage) onMessage(loc::Tr("map.redo.none"));
		return;
	}
	m_undoStack.push_back(CaptureEditorState());
	EditorSnapshot snap = std::move(m_redoStack.back());
	m_redoStack.pop_back();
	RestoreEditorState(std::move(snap));
	if (onMessage) onMessage(loc::Tr("map.redo.done"));
}

void DungeonWorld::ClearUndoHistory() {
	m_undoStack.clear();
	m_redoStack.clear();
	m_pendingUndo.reset();
}


} // namespace dungeon::game
