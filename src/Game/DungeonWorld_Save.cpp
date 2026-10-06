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

namespace {
// An effects list as it survives a save, and back - ONE conversion for a
// monster's list and a piece of dungeon's alike (a member's goes through Game).
void CaptureEffectList(const std::vector<fx::Inst>& from,
					   std::vector<SaveData::EffectState>& to) {
	for (const fx::Inst& inst : from) {
		if (!inst.kind) continue;
		to.push_back({inst.kind->Id(), SymbolId(inst.school), inst.timeLeft,
					  inst.duration, inst.magnitude, inst.source,
					  std::string(inst.NameKey())});
		if (inst.tinted) {
			to.back().tinted = true;
			to.back().tint[0] = inst.tint.x;
			to.back().tint[1] = inst.tint.y;
			to.back().tint[2] = inst.tint.z;
		}
	}
}

// An id the project's effect classes don't know - an older or newer save - is
// skipped, never misread. Clears first and pushes back, so a list reserved at
// load (fx::ReserveEffects) keeps its capacity.
void RestoreEffectList(const fx::EffectBook& book,
					   const std::vector<SaveData::EffectState>& from,
					   std::vector<fx::Inst>& to) {
	to.clear();
	for (const SaveData::EffectState& fx : from) {
		SpellSymbol school = SpellSymbol::Fire;
		ParseSymbol(fx.school, school);
		const fx::EffectKind* kind = book.FindLegacy(fx.id, school);
		if (!kind || fx.time <= 0.0f) continue;
		to.push_back({kind, school, fx.magnitude, fx.time,
					  std::max(fx.duration, fx.time), fx.source});
		if (fx.tinted) {
			to.back().tinted = true;
			to.back().tint = {fx.tint[0], fx.tint[1], fx.tint[2]};
		}
	}
}
} // namespace

void DungeonWorld::ResetForNewGame() {
	m_party.Reset(m_map.StartX(), m_map.StartZ());
	m_leader = 0; // slot 0 (Brand) leads a new game
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
	m_pendingBoltCount = 0; // and any volley still waiting its turn
	m_lightStones = {};     // and any Earth light set down (a level's own state)
	ClearTracks();          // and the tracks monsters left (6g)
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
	// Every fire back to how the level was authored, and nothing left smoking.
	m_map.ResetFixtureBurning();
	SyncFiresFromMap();
	std::fill(m_seen.begin(), m_seen.end(), static_cast<u8>(0));
	MarkSeen(m_party.GridX(), m_party.GridZ());
	m_levelStates.clear(); // forget any explored levels
	m_parked = false;
	// Every monster is back at full and the world is a different world: the
	// baselines the one-pipeline check was holding describe state that no longer
	// exists (Game/DamageLedger.h). Take a fresh one rather than reporting the
	// reset itself as a hundred writes that went around the pipeline.
	RebaseDamageLedger();
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
	m_activeBlastCount = 0;
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
	// One member, so a field added to Harness is reset here for free — the four
	// loose bools this replaced were four chances to forget one.
	m_harness = {};
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
		CaptureEffectList(m.effects, e.effects);
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
			e.charge = item.charge; // a half-burnt torch keeps what is left
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
	// Fires lit or put out in play: a diff from the authored `lit`, like a niche.
	// (A smashed fixture is out too, but its `broken` entry already says so and
	// restores it dark, so this records it again harmlessly.)
	// A bracket holding a torch other than its own is a diff too (a magical torch
	// mounted in it), even burning just as authored.
	for (const WallSconce& s : m_map.Sconces())
		if (s.flipped || s.empty || !s.torch.empty())
			ls.fires.push_back({s.x, s.z, static_cast<int>(s.wall), s.Burning(), s.empty,
								s.torch, s.torchCharge});
	for (const FloorBrazier& b : m_map.Braziers())
		if (b.flipped) ls.fires.push_back({b.x, b.z, -1, b.Burning()});
	// Pieces HURT but standing: their hp and whatever rides them, so a door left
	// burning is still burning - and still battered - after a load. Same key as a
	// broken one. A piece at full hp carrying nothing writes no line.
	const auto damaged = [&ls](const Breakable& brk, int x, int z, std::string_view type,
							   int wall) {
		if (!brk.Alive() || (brk.hp >= brk.maxHp && brk.effects.empty())) return;
		SaveData::DamagedPiece d{x, z, std::string(type), wall, brk.hp, {}};
		CaptureEffectList(brk.effects, d.effects);
		ls.damaged.push_back(std::move(d));
	};
	for (const Decoration& d : m_decorations) damaged(d.brk, d.x, d.z, d.kind->id, -1);
	for (const Door& d : m_doors) damaged(d.brk, d.x, d.z, d.type, -1);
	for (const FixtureBreak& fb : m_fixtureBreaks)
		damaged(fb.brk, fb.x, fb.z, fb.type, fb.wall);
	// Earth lights set down (6f): each still burning, with what it has left.
	for (const LightStone& s : m_lightStones)
		if (s.timeLeft > 0.0f) ls.stones.push_back({s.x, s.z, s.power, s.timeLeft, s.duration});
	// Monster tracks not yet faded (6g), as AGES against the clock now.
	const int w = std::max(m_map.Width(), 1);
	for (size_t i = 0; i < m_tracks.size(); ++i) {
		const Track& t = m_tracks[i];
		const float age = TrackAge(t);
		if (age < 0.0f) continue;
		ls.tracks.push_back({static_cast<int>(i % static_cast<size_t>(w)),
							 static_cast<int>(i / static_cast<size_t>(w)), static_cast<int>(t.dir),
							 static_cast<int>(t.maker), age});
	}
	return ls;
}

void DungeonWorld::StashActive() {
	// A thrown item still in the air comes down first, so it is stashed with
	// the level rather than dropped with the flights (Phase 10).
	m_projectiles.LandCargo();
	m_levelStates[m_currentLevel] = SnapshotActive();
}

void DungeonWorld::ParkActive() {
	if (m_parked) return;
	// The same three layers a stair stashes (BeginLevelLoad): the dynamic state
	// (the dead stay dead), the static map (unsaved editor work), and the .ent
	// records when they have drifted from the file.
	StashActive();
	StashStaticMap();
	if (m_entsDirty)
		m_levelEnts.insert_or_assign(m_currentLevel,
									 std::make_unique<DungeonEntities>(m_entities));
	m_parked = true;
}

void DungeonWorld::ApplyActiveSnapshot() {
	auto it = m_levelStates.find(m_currentLevel);
	m_parked = false; // the live level is the authority again from here
	if (it == m_levelStates.end()) {
		// First visit — nothing to restore, but the level is still a NEW WORLD
		// to the one-pipeline check. Its monsters were rebuilt in the storage
		// the last level's used, and the ledger matches by ADDRESS, so without
		// this a fresh skeleton read as the old level's monster healing +6 with
		// nothing to explain it (every first entry through a world-map doorway).
		RebaseDamageLedger();
		return;
	}
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
	// Rebuild a monster's afflictions from the snapshot (v22). An id the project's
	// effect classes don't know — an older or newer save — is skipped, never
	// misread; the plume follows automatically, since it is derived from the list.
	const auto restoreEffects = [this](const SaveData::EntityState& e, Monster& m) {
		RestoreEffectList(m_effects, e.effects, m.effects);
	};
	// A monster the snapshot carries was placed by hand or has been met, moved or
	// hurt - it is already up. Without this a recreated one would lie back down
	// and replay its spawn clip, and a rising monster does not act, so a reload
	// mid-fight would buy 10-14 s of free blows. (An untouched baseline monster
	// has no snapshot row and still rises at its post, as on a first visit.)
	const auto alreadyUp = [](Monster& m) {
		m.spawnReq = false;
		m.spawnAnim = 0.0f;
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
				alreadyUp(m);
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
						alreadyUp(m);
						m.visualPos = SlotCenter(m.x, m.z, m.kind->size, m.slot);
						break;
					}
			}
			break;
		case EntityKind::Item:
			if (e.id < 0) {
				// Dropped tablet — lay it back with a fresh runtime id, at its saved
				// quarter slot (or piled in its wall niche, e.niche >= 0).
				ItemKind& kind = ItemKindFor(e.type);
				m_items.push_back(
					{&kind, m_nextDropId--, e.x, e.z, false, e.slot, e.niche, e.charge});
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
	ReserveDropRoom(); // the saved drops took some of LoadItems' headroom
	// Wall-niche reveal state: set each saved niche's open flag, then re-stamp its
	// wall (a no-op if the geometry isn't built yet — the load's mesh bake then
	// reads the restored open state directly).
	for (const SaveData::NicheOpen& n : ls.niches)
		if (m_map.SetNicheOpenAt(n.x, n.z, static_cast<Direction>(n.wall), n.open))
			RebuildChunksAround(n.x, n.z);
	// Fires lit or put out in play. Restored QUIETLY: a fire found out on
	// arrival went out long ago, and its smoke with it.
	for (const SaveData::FireBurning& f : ls.fires) {
		if (f.empty) SetSconceEmpty(f.x, f.z, f.wall, true); // its torch was taken
		else SetFireBurning(f.x, f.z, f.wall, f.burning, /*smoke*/ false);
		// ...and which torch is in it, when not its own.
		if (!f.empty && !f.torch.empty() && f.wall >= 0)
			m_map.SetSconceTorch(f.x, f.z, f.wall, f.torch, f.torchCharge);
	}
	// Earth lights set down (6f), as they were left - into the pool's first
	// slots, the rest free. One off the map (the level was edited under the
	// save) is dropped.
	m_lightStones = {};
	size_t stoneSlot = 0;
	for (const SaveData::LightStone& s : ls.stones) {
		if (stoneSlot >= m_lightStones.size() || s.timeLeft <= 0.0f) continue;
		if (s.x < 0 || s.z < 0 || s.x >= m_map.Width() || s.z >= m_map.Height()) continue;
		m_lightStones[stoneSlot++] = {s.x, s.z, s.power, s.timeLeft, s.duration, 0.0f, 0.0f};
	}
	// Monster tracks (6g), each as old as it was when the level was left or
	// saved. One off the map, or naming no maker this build knows, is dropped.
	ClearTracks();
	for (const SaveData::TrackState& t : ls.tracks) {
		if (t.maker < static_cast<int>(TrackMaker::Monster) ||
			t.maker > static_cast<int>(TrackMaker::Party) || t.dir < 0 || t.dir > 3)
			continue;
		RecordTrack(t.x, t.z, static_cast<Direction>(t.dir), static_cast<TrackMaker>(t.maker));
		const size_t cell = static_cast<size_t>(t.z) * static_cast<size_t>(m_map.Width()) +
							static_cast<size_t>(t.x);
		if (t.x >= 0 && t.z >= 0 && t.x < m_map.Width() && cell < m_tracks.size())
			m_tracks[cell].stamp = m_trackClock - static_cast<double>(t.age);
	}
	// Re-break what was broken (v24). A saved entry naming a prop this level no
	// longer has is simply dropped — the level was edited under the save, and a
	// missing prop is exactly the outcome the entry wanted anyway. And one naming a
	// piece whose type is no longer `breakable` is IGNORED, as a damaged entry
	// is below: unticked means unbreakable, and a save must not smash a door or a
	// prop the type now says cannot be (it would open that door for good). Fixtures
	// already hold to it - an undestructible one has no FixtureBreak to match.
	for (const SaveData::BrokenProp& b : ls.broken) {
		bool found = false;
		for (Decoration& d : m_decorations)
			if (d.x == b.x && d.z == b.z && d.kind->id == b.type) {
				found = true;
				if (!d.brk.Damageable()) break;
				d.brk.broken = true;
				d.brk.hp = 0.0f;
				break;
			}
		if (found) continue;
		for (Door& d : m_doors)
			if (d.x == b.x && d.z == b.z && d.type == b.type) {
				found = true;
				if (!d.brk.Damageable()) break;
				d.brk.broken = true;
				d.brk.hp = 0.0f;
				d.open = true; // the way stays open, and stays unclosable
				d.openT = 1.0f;
				break;
			}
		if (found) continue;
		for (FixtureBreak& fb : m_fixtureBreaks)
			if (fb.x == b.x && fb.z == b.z && fb.type == b.type &&
				fb.wall == b.wall) {
				fb.brk.broken = true;
				fb.brk.hp = 0.0f;
				// and it comes back DARK, not merely broken (quietly: no fresh smoke)
				SetFireBurning(fb.x, fb.z, fb.wall, false, /*smoke*/ false);
				break;
			}
	}
	// Re-hurt what was hurt: hp and the effects riding it. Like a broken entry, one
	// naming a piece the level no longer has is dropped; one naming a piece that is
	// no longer breakable (its type was made indestructible) is ignored.
	for (const SaveData::DamagedPiece& s : ls.damaged) {
		const auto restore = [&](Breakable& brk) {
			if (!brk.Alive()) return;
			brk.hp = std::clamp(s.hp, 0.01f, brk.maxHp);
			RestoreEffectList(m_effects, s.effects, brk.effects);
		};
		bool found = false;
		for (Decoration& d : m_decorations)
			if (!found && d.x == s.x && d.z == s.z && d.kind->id == s.type) {
				restore(d.brk);
				found = true;
			}
		for (Door& d : m_doors)
			if (!found && d.x == s.x && d.z == s.z && d.type == s.type) {
				restore(d.brk);
				found = true;
			}
		for (FixtureBreak& fb : m_fixtureBreaks)
			if (!found && fb.x == s.x && fb.z == s.z && fb.type == s.type &&
				fb.wall == s.wall) {
				restore(fb.brk);
				found = true;
			}
	}
	m_levelStates.erase(it); // the live state is authoritative now
	RebaseDamageLedger();    // restored hit points are not writes to explain
}

void DungeonWorld::CaptureState(SaveData& out, bool includeLive) const {
	out.currentLevel = m_currentLevel;
	out.partyX = m_party.GridX();
	out.partyZ = m_party.GridZ();
	out.partyFacing = m_party.Facing();
	out.lookYaw = m_party.LookYaw();
	out.lookPitch = m_party.LookPitch();
	out.looking = m_party.IsLooking();
	out.leader = m_leader;

	// Every inactive visited level, plus the live one — unless it is parked,
	// when the store already holds it and a second copy would be written.
	out.levels.clear();
	for (const auto& [stem, ls] : m_levelStates) out.levels.push_back(ls);
	if (includeLive && !m_parked) out.levels.push_back(SnapshotActive());
}

void DungeonWorld::ApplyState(const SaveData& in) {
	m_party.SetGridPosition(in.partyX, in.partyZ); // keeps facing, clears interp
	m_party.SetFacing(in.partyFacing);
	// Re-layer the free-look offset on the restored facing (SetFacing cleared it).
	m_party.SetLookState(in.lookYaw, in.lookPitch, in.looking);
	// The leader as saved; Update hands it on if the roster says they are down.
	m_leader = in.leader;

	// Load every level's saved state into the per-level store. The active level's
	// state is applied by ApplyActiveSnapshot once Game has routed to
	// in.currentLevel (its entity diff needs the monsters built).
	m_levelStates.clear();
	for (const SaveData::LevelState& ls : in.levels) m_levelStates[ls.stem] = ls;
}

} // namespace dungeon::game
