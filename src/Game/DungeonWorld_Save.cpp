// ============================================================================
// Game/DungeonWorld_Save.cpp — save / load + level-state stashing for
// DungeonWorld (declarations in DungeonWorld.h). Split out of DungeonWorld.cpp:
// resetting for a new game, snapshotting the active level's dynamic state,
// stashing/restoring visited levels, and whole-game CaptureState/ApplyState.
// ============================================================================
#include "Game/DungeonWorld.h"


#include <algorithm>
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
		monster.threat = {};   // and every grudge with them (threat/lock reset)
		monster.threatLock = -1;
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
	m_projectiles.Clear(); // drop any bolts/sparks still in flight from a prior run
	// Rebuild items from the .ent baseline so runes return to their spawn cells
	// (and any dropped tablets from a prior session are forgotten).
	m_items.clear();
	LoadItems();
	for (Button& b : m_buttons) b.activated = false; // un-press for a fresh run
	for (Door& d : m_doors) { // back to the authored state, no ghost slide
		d.open = d.initialOpen;
		d.openT = d.open ? 1.0f : 0.0f;
	}
	// Re-hide any secret niche opened this session; re-stamp the changed walls.
	if (m_map.ResetNicheOpen())
		for (const WallNiche& n : m_map.Niches()) RebuildChunksAround(n.x, n.z);
	std::fill(m_seen.begin(), m_seen.end(), static_cast<u8>(0));
	MarkSeen(m_party.GridX(), m_party.GridZ());
	SetTorchPalette(0);
	m_levelStates.clear(); // forget any explored levels
}

// See the declaration for why this is defined as "where a new game would leave
// it". Everything below is something a real new game gets from the LEVEL LOAD
// rather than from ResetForNewGame — plus the harness's own modes, which no
// player path has any reason to touch.
void DungeonWorld::ResetForEval() {
	// --- 1. THE STATIC LAYER, back from the project files -------------------
	// `arena` sets EVERY cell to wall before carving, and strips the map's own
	// fixtures, stairs, niches and features on the way past — so a reset that
	// only rewound the dynamic side would hand the next test an empty box with
	// the authored monsters standing in rock.
	//
	// THIS IS THE PART THE FIRST VERSION MISSED, and the way it was missed is
	// worth more than the fix: the equivalence check passed, because it printed
	// the party, the supplies and the monsters, and NONE of those show map
	// geometry. A test that does not look at the thing that differs reports
	// "identical" just as loudly as one that does.
	//
	// Re-parsing costs a text file and the mesh bake; the twelve seconds a real
	// load costs are MODELS AND TEXTURES, and those are cached by now.
	m_map = DungeonMap(m_project.LevelMapPath(m_currentLevel),
					   FixtureTypesOf(m_project));
	m_entities = DungeonEntities(m_project.LevelEntPath(m_currentLevel), m_map);
	// Every live object re-placed from those records. Also the reason harness
	// `spawn`s disappear: they were never records, only instances.
	RespawnFromRecords(/*geometryToo=*/false);
	SeedFixtureBreakables();

	// --- 2. the dynamic layer -----------------------------------------------
	// Party pose, monster hp/threat/awareness, the wipe latch, projectiles,
	// items, buttons, doors, niches, fog, torch palette. AFTER the map, because
	// it puts the party on the map's start cell.
	ResetForNewGame();

	// A blast is a wavefront mid-flight; a `step` that ends between its ticks
	// leaves one live, and it would detonate into the next test.
	m_activeBlasts.clear();
	// Damage done to the DUNGEON (save v24). A smashed decoration KEEPS its
	// record — the adapter holds a reference and the save has to be able to name
	// what broke — so the flag is lifted rather than the entry erased. Fixtures
	// live in their own side-table keyed by cell+wall, so that is rebuilt whole.
	for (Decoration& deco : m_decorations) {
		deco.brk.broken = false;
		deco.brk.hp = deco.brk.maxHp;
		deco.brk.effects.clear();
	}
	m_fixtureBreaks.clear();
	SeedFixtureBreakables();

	// --- 3. make it real -----------------------------------------------------
	// The FULL bake, as `arena` does: every cell in the map may have changed.
	BuildDungeonMeshes();
	RebuildFiresAndDust(); // the level's fires are back, and a doused one burns

	// A pit fall caught mid-plunge would swap levels on the first frame of the
	// next test, and the bruise is latched separately from the transition.
	m_pendingFall.reset();
	m_fellPending = false;
	m_fallT = -1.0f;

	// The harness's own modes. NOT `lockstep` or the RNG seed: those are how the
	// run is DRIVEN rather than what it contains, and silently changing them
	// under a script would be its own kind of contamination — a script that set
	// them once at the top would find them gone after its first reset.
	m_tally = {};
	m_autoAttack = false;
	m_freezeMonsters = false;
	m_pendingSteps = 0;
	m_resting = false;
	m_restEndReason = "";
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

	// What a monster is afflicted by (v22) — the same record a member stores, so
	// a burn or a poison survives a save instead of quietly going out.
	const auto captureEffects = [](const Monster& m, SaveData::EntityState& e) {
		for (const fx::Inst& inst : m.effects) {
			if (!inst.kind) continue;
			e.effects.push_back({inst.kind->Id(), SymbolId(inst.school),
								 inst.timeLeft, inst.duration, inst.magnitude,
								 inst.source, std::string(inst.NameKey())});
		}
	};

	// Monsters: a baseline gets a diff once it has moved off its spawn cell,
	// announced itself, taken damage (incl. being slain), or picked up an
	// affliction. An editor-placed monster (id < 0) has no baseline, so it is
	// stored whole to recreate.
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
			e.threat = m.threat;
			e.threatLock = m.threatLock;
			captureEffects(m, e);
			ls.entities.push_back(std::move(e));
		} else if (m.x != m.spawnX || m.z != m.spawnZ || m.announced || m.aware ||
				   m.hp != m.MaxHp() || m.ThreatAny() || m.threatLock >= 0 ||
				   !m.effects.empty()) {
			e.id = m.id;
			e.x = m.x;
			e.z = m.z;
			e.announced = m.announced;
			e.aware = m.aware;
			e.hp = m.hp;
			e.slot = m.slot;
			e.threat = m.threat;
			e.threatLock = m.threatLock;
			captureEffects(m, e);
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
			e.niche = item.niche; // -1 = floor drop; else the wall it fell into
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
	// Doors: a diff once the open state differs from the authored record
	// (`activated` carries "open" — the same togglable-state slot buttons use).
	for (const Door& d : m_doors)
		if (d.open != d.initialOpen) {
			SaveData::EntityState e;
			e.kind = EntityKind::Door;
			e.id = d.id;
			e.activated = d.open;
			ls.entities.push_back(std::move(e));
		}
	// Wall niches: a diff once the runtime open state differs from the authored
	// default (!hidden) — a secret niche a button revealed (or an editor toggle).
	for (const WallNiche& n : m_map.Niches())
		if (n.open != !n.hidden)
			ls.niches.push_back({n.x, n.z, static_cast<int>(n.wall), n.open});
	// Smashed props (v24). Decorations are STATIC .map records, so being broken is
	// dynamic state and belongs here — the same split `seen` makes. Keyed by cell +
	// type, never by index: an index survives only until the editor inserts a record
	// ahead of it. This is also why a broken prop keeps its place in m_decorations
	// rather than being erased — an erased record could not be named here.
	for (const Decoration& d : m_decorations)
		if (d.Gone()) ls.broken.push_back({d.x, d.z, d.kind->id});
	// Doors too, even though they DO ride `entities`: that only carries open/closed,
	// and a broken door is not merely an open one — it can never be shut again, and
	// must not come back at full hp to be broken a second time. Same record, same
	// key, so both kinds restore through one path.
	for (const Door& d : m_doors)
		if (d.brk.broken) ls.broken.push_back({d.x, d.z, d.type});
	// Fixtures, from the side-table their damage state lives in. The WALL matters
	// here and nowhere else: two sconces can share a cell.
	for (const FixtureBreak& fb : m_fixtureBreaks)
		if (fb.brk.broken) ls.broken.push_back({fb.x, fb.z, fb.type, fb.wall});
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

	// Rebuild a monster's afflictions from the snapshot (v22). An id the project's
	// effect classes don't know — an older or newer save — is skipped, never
	// misread; the plume follows automatically, since it is derived from the list.
	const auto restoreEffects = [this](const SaveData::EntityState& e, Monster& m) {
		m.effects.clear();
		for (const SaveData::EffectState& fx : e.effects) {
			SpellSymbol school = SpellSymbol::Fire;
			ParseSymbol(fx.school, school);
			const fx::EffectKind* kind = m_effects.FindLegacy(fx.id, school);
			if (!kind || fx.time <= 0.0f) continue;
			m.effects.push_back({kind, school, fx.magnitude, fx.time,
								 std::max(fx.duration, fx.time), fx.source});
		}
	};

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
				m.threat = e.threat; // v19 (older saves: zeroes / -1)
				m.threatLock = e.threatLock;
				restoreEffects(e, m);
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
						m.threat = e.threat; // v19 (older saves: zeroes / -1)
						m.threatLock = e.threatLock;
						restoreEffects(e, m);
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
				// Dropped tablet — lay it back with a fresh runtime id, at its saved
				// quarter slot (or piled in its wall niche, e.niche >= 0).
				ItemKind& kind = ItemKindFor(e.type);
				m_items.push_back(
					{&kind, m_nextDropId--, e.x, e.z, false, e.slot, e.niche});
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
		case EntityKind::Door:
			for (Door& d : m_doors)
				if (d.id == e.id) {
					d.open = e.activated;
					d.openT = d.open ? 1.0f : 0.0f; // snap, no ghost slide
					break;
				}
			break;
		default: break; // decorations are static — never in a save
		}
	}
	// Wall-niche reveal state: set each saved niche's open flag, then re-stamp its
	// wall (a no-op if the geometry isn't built yet — the load's mesh bake then
	// reads the restored open state directly).
	for (const SaveData::NicheOpen& n : ls.niches)
		if (m_map.SetNicheOpenAt(n.x, n.z, static_cast<Direction>(n.wall), n.open))
			RebuildChunksAround(n.x, n.z);
	// Re-break what was broken (v24). A saved entry naming a prop this level no
	// longer has is simply dropped — the level was edited under the save, and a
	// missing prop is exactly the outcome the entry wanted anyway.
	for (const SaveData::BrokenProp& b : ls.broken) {
		bool found = false;
		for (Decoration& d : m_decorations)
			if (d.x == b.x && d.z == b.z && d.kind->id == b.type) {
				d.brk.broken = true;
				d.brk.hp = 0.0f;
				found = true;
				break;
			}
		if (found) continue;
		for (Door& d : m_doors)
			if (d.x == b.x && d.z == b.z && d.type == b.type) {
				d.brk.broken = true;
				d.brk.hp = 0.0f;
				d.open = true; // the way stays open, and stays unclosable
				d.openT = 1.0f;
				found = true;
				break;
			}
		if (found) continue;
		for (FixtureBreak& fb : m_fixtureBreaks)
			if (fb.x == b.x && fb.z == b.z && fb.type == b.type &&
				fb.wall == b.wall) {
				fb.brk.broken = true;
				fb.brk.hp = 0.0f;
				DouseFixture(fb); // and it comes back DARK, not merely broken
				break;
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
