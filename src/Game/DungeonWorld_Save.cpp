// ============================================================================
// Game/DungeonWorld_Save.cpp — save / load + level-state stashing for
// DungeonWorld (declarations in DungeonWorld.h). Split out of DungeonWorld.cpp:
// resetting for a new game, snapshotting the active level's dynamic state,
// stashing/restoring visited levels, and whole-game CaptureState/ApplyState.
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
#include <cmath>
#include <format>
#include <queue>

using namespace DirectX;

namespace dungeon::game {

void DungeonWorld::ResetForNewGame() {
	m_party.Reset(m_map.StartX(), m_map.StartZ());
	for (size_t i = 0; i < m_monsters.size(); ++i) {
		Monster& monster = m_monsters[i];
		monster.announced = false;
		monster.aware = false; // forget the party — a fresh game starts unalerted
		monster.intent = {};   // drop standing orders so it idles until it notices
		monster.hp = monster.MaxHp();
		monster.attackCd = 0.0f;
		// Monsters roam now (AI v1) — return them to their .ent spawn cell and
		// clear any in-flight glide so a same-level new game starts clean.
		monster.x = monster.spawnX;
		monster.z = monster.spawnZ;
		monster.yaw = monster.targetYaw = DirYaw(monster.facing); // back to spawn facing
		monster.moving = false;
		monster.moveT = 0.0f;
		monster.moveCd = 0.0f;
		// Re-derive a free slot in the spawn cell (group members fan out again).
		monster.slot = std::max(
			0, FreeSlotInCell(monster.x, monster.z, monster.kind->size, static_cast<int>(i)));
		monster.visualPos =
			SlotCenter(monster.x, monster.z, monster.kind->size, monster.slot);
	}
	m_partyWiped = false;
	m_magic.Clear(); // drop any spell bolts/sparks still in flight from a prior run
	// Rebuild items from the .ent baseline so runes return to their spawn cells
	// (and any dropped tablets from a prior session are forgotten).
	m_items.clear();
	LoadItems();
	for (Button& b : m_buttons) b.activated = false; // un-press for a fresh run
	std::fill(m_seen.begin(), m_seen.end(), static_cast<u8>(0));
	MarkSeen(m_party.GridX(), m_party.GridZ());
	SetTorchPalette(0);
	m_levelStates.clear(); // forget any explored levels
}

SaveData::LevelState DungeonWorld::SnapshotActive() const {
	SaveData::LevelState ls;
	ls.stem = m_currentLevel;
	for (int z = 0; z < m_map.Height(); ++z)
		for (int x = 0; x < m_map.Width(); ++x)
			if (m_seen[static_cast<size_t>(z) * m_map.Width() + x])
				ls.seen.emplace_back(x, z);
	// Every dynamic entity round-trips through one generic EntityState, as either
	// a DIFF (a .ent baseline that drifted from its spawn, keyed by id) or a SPAWN
	// (a runtime entity with no baseline, stored whole). The two modes and the
	// per-kind fields live in SaveData::EntityState.

	// Monsters: a baseline gets a diff once it has moved off its spawn cell,
	// announced itself, or taken damage (incl. being slain). An editor-placed
	// monster (id < 0) has no baseline, so it is stored whole to recreate.
	for (const Monster& m : m_monsters) {
		SaveData::EntityState e;
		e.kind = EntityKind::Monster;
		if (m.id < 0) {
			e.id = -1;
			e.type = m.kind ? m.kind->name : std::string();
			e.x = m.x;
			e.z = m.z;
			e.facing = static_cast<int>(m.facing);
			e.announced = m.announced;
			e.aware = m.aware;
			e.hp = m.hp;
			e.slot = m.slot;
			e.spawnX = m.spawnX;
			e.spawnZ = m.spawnZ;
			ls.entities.push_back(std::move(e));
		} else if (m.x != m.spawnX || m.z != m.spawnZ || m.announced || m.aware ||
				   m.hp != m.MaxHp()) {
			e.id = m.id;
			e.x = m.x;
			e.z = m.z;
			e.announced = m.announced;
			e.aware = m.aware;
			e.hp = m.hp;
			e.slot = m.slot;
			ls.entities.push_back(std::move(e));
		}
	}
	// Items: a baseline rune gets a one-bit diff once collected; a dropped tablet
	// (id < 0) still on the floor is stored whole. A collected dropped tablet is
	// simply gone — no record (it falls out of both branches).
	for (const Item& item : m_items) {
		SaveData::EntityState e;
		e.kind = EntityKind::Item;
		if (item.id >= 0) {
			if (item.collected) {
				e.id = item.id;
				e.collected = true;
				ls.entities.push_back(std::move(e));
			}
		} else if (!item.collected) {
			e.id = -1;
			e.type = item.kind->id;
			e.x = item.x;
			e.z = item.z;
			e.slot = item.slot;
			ls.entities.push_back(std::move(e));
		}
	}
	// Buttons: a baseline button gets a diff once it has been activated.
	for (const Button& b : m_buttons)
		if (b.activated) {
			SaveData::EntityState e;
			e.kind = EntityKind::Button;
			e.id = b.id;
			e.activated = true;
			ls.entities.push_back(std::move(e));
		}
	return ls;
}

void DungeonWorld::StashActive() {
	m_levelStates[m_currentLevel] = SnapshotActive();
}

void DungeonWorld::ApplyActiveSnapshot() {
	auto it = m_levelStates.find(m_currentLevel);
	if (it == m_levelStates.end()) return; // first visit — nothing to restore
	const SaveData::LevelState& ls = it->second;

	std::fill(m_seen.begin(), m_seen.end(), static_cast<u8>(0));
	for (const auto& [x, z] : ls.seen)
		if (x >= 0 && z >= 0 && x < m_map.Width() && z < m_map.Height())
			m_seen[static_cast<size_t>(z) * m_map.Width() + x] = 1;
	// Editor-placed monsters and dropped tablets have no .ent baseline, so the
	// snapshot's whole SPAWN rows are authoritative: drop any live ones (e.g.
	// placed/dropped earlier this session, or the .ent baseline LoadItems rebuilt)
	// and recreate them from the save below. Baseline diffs apply onto the kept
	// baseline instances by id.
	std::erase_if(m_monsters, [](const Monster& m) { return m.id < 0; });
	std::erase_if(m_items, [](const Item& i) { return i.id < 0; });
	// v6 migration: that save stored a whole floor snapshot (no per-item diff).
	// Mark every baseline rune collected up front; the Item rows below revive the
	// ones actually on the floor — matched by cell + type, so an untouched baseline
	// keeps its .ent id (and won't re-serialize as a drop that later duplicates
	// it). A rune absent from the snapshot (picked up in the v6 save) stays gone.
	if (ls.fullFloorSnapshot)
		for (Item& item : m_items)
			if (item.id >= 0) item.collected = true;

	for (const SaveData::EntityState& e : ls.entities) {
		switch (e.kind) {
		case EntityKind::Monster:
			if (e.id < 0) {
				// Whole editor-placed monster — recreate at its spawn, then snap to
				// the saved live cell/state.
				if (!m_project.monsters.Contains(e.type)) break;
				MonsterKind& kind = MonsterKindFor(e.type);
				Monster m = MakeMonster(kind, -1, e.spawnX, e.spawnZ,
										static_cast<Direction>(e.facing));
				m.x = e.x;
				m.z = e.z;
				m.announced = e.announced;
				m.aware = e.aware;
				if (e.hp >= 0.0f) m.hp = e.hp; // -1 = older save → keep spawn hp
				m.slot = e.slot; // saved sub-cell slot (Phase 3)
				m.visualPos = SlotCenter(m.x, m.z, m.kind->size, m.slot);
				m_monsters.push_back(std::move(m));
			} else {
				for (Monster& m : m_monsters)
					if (m.id == e.id) {
						m.x = e.x;
						m.z = e.z;
						m.announced = e.announced;
						m.aware = e.aware;
						if (e.hp >= 0.0f) m.hp = e.hp; // -1 = older save → keep spawn hp
						m.moving = false; // snap to the saved cell, no glide from origin
						m.moveT = 0.0f;
						m.slot = e.slot; // saved sub-cell slot (Phase 3)
						m.visualPos = SlotCenter(m.x, m.z, m.kind->size, m.slot);
						break;
					}
			}
			break;
		case EntityKind::Item:
			if (ls.fullFloorSnapshot) {
				// v6 floor row: revive the collected baseline rune of this type at
				// this cell (keeping its id), else lay a non-baseline tablet down.
				bool revived = false;
				for (Item& item : m_items)
					if (item.id >= 0 && item.collected && item.x == e.x &&
						item.z == e.z && item.kind && item.kind->id == e.type) {
						item.collected = false;
						revived = true;
						break;
					}
				if (!revived) {
					ItemKind& kind = ItemKindFor(e.type);
					const Vec3 c = m_map.CellCenter(e.x, e.z); // v6 had no slot
					const int slot = FreeItemSlotNear(e.x, e.z, c.x, c.z, -1);
					m_items.push_back({&kind, m_nextDropId--, e.x, e.z, false, slot});
				}
			} else if (e.id < 0) {
				// Dropped tablet — lay it on the floor with a fresh runtime id, at
				// its saved quarter slot.
				ItemKind& kind = ItemKindFor(e.type);
				m_items.push_back({&kind, m_nextDropId--, e.x, e.z, false, e.slot});
			} else {
				// Baseline rune collected — mark the kept instance lifted.
				for (Item& item : m_items)
					if (item.id == e.id) {
						item.collected = e.collected;
						break;
					}
			}
			break;
		case EntityKind::Button:
			for (Button& b : m_buttons)
				if (b.id == e.id) {
					b.activated = e.activated;
					break;
				}
			break;
		default: break; // decorations are static — never in a save
		}
	}
	m_levelStates.erase(it); // the live state is authoritative now
}

void DungeonWorld::CaptureState(SaveData& out) const {
	out.currentLevel = m_currentLevel;
	out.partyX = m_party.GridX();
	out.partyZ = m_party.GridZ();
	out.partyFacing = m_party.Facing();
	out.lookYaw = m_party.LookYaw();
	out.lookPitch = m_party.LookPitch();
	out.looking = m_party.IsLooking();
	out.torchPalette = m_torchPalette;

	// Every inactive visited level, plus the live one.
	out.levels.clear();
	for (const auto& [stem, ls] : m_levelStates) out.levels.push_back(ls);
	out.levels.push_back(SnapshotActive());
}

void DungeonWorld::ApplyState(const SaveData& in) {
	m_party.SetGridPosition(in.partyX, in.partyZ); // keeps facing, clears interp
	m_party.SetFacing(in.partyFacing);
	// Re-layer the free-look offset on the restored facing (SetFacing cleared it).
	m_party.SetLookState(in.lookYaw, in.lookPitch, in.looking);
	SetTorchPalette(in.torchPalette);

	// Load every level's saved state into the per-level store. The active level's
	// state is applied by ApplyActiveSnapshot once Game has routed to
	// in.currentLevel (its entity diff needs the monsters built).
	m_levelStates.clear();
	for (const SaveData::LevelState& ls : in.levels) m_levelStates[ls.stem] = ls;
}

} // namespace dungeon::game
