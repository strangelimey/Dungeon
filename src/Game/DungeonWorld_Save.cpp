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

// See the declaration: ONE list for every place a level is replaced or put back.
// A transient added to the level belongs here, not at a call site.
void DungeonWorld::ClearLevelTransients() {
	m_projectiles.Clear();  // bolts, sparks, puffs, a thrown thing still in the air
	m_pendingBoltCount = 0; // a volley's bolts still waiting their turn
	// A blast is a wavefront still spreading, and a poison gas then HANGS for its
	// linger: kept, it went on biting the same coordinates of whatever level or
	// game came next. The table is fixed (C49), so emptying it is its count.
	m_activeBlastCount = 0;
	m_lightStones = {};     // an Earth light set down (a level's own state, saved)
	// Emptied, not mended: whoever puts the level back seeds it from ITS map
	// (the load's fires task, InstallLevel's and the arena's fire rebuild,
	// ResetForNewGame), and the seed carries an old entry over BY CELL (C293).
	m_fixtureBreaks.clear();
	// A burn, a poison or a bleed on a monster - the plume and its light follow,
	// being derived from the list. A level load has already emptied m_monsters;
	// a new game and a load over the same level keep their monsters (C292).
	for (Monster& m : m_monsters) m.effects.clear();
	// A stair or a pit under way: either would swap levels on the next frame,
	// out of a game that has just begun or been loaded.
	m_pendingTransition.reset();
	m_falling = false; // the kept m_fall's level string stays reserved
	m_fallT = -1.0f;
}

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
		// A NEW IDENTITY, so no plan thought before the reset can reach it. The
		// AI workers keep thinking while the world is frozen - on the title after
		// a wipe, in the pause menu - from the last snapshot, where this monster
		// was hunting the party, and every tick of theirs is a fresh batch the
		// first frame back would apply, latching `aware` again for every
		// non-idle plan: a Continue or a Start New Game on the same level began
		// with the last fight's monsters already hunting, sleepers included
		// (code-review C52). Plans match by runtimeId, so a batch naming the old
		// ids now finds nobody - including one a worker is still building, which
		// a "skip batches up to this seq" mark could not have caught. Before
		// RebaseDamageLedger below, which keys monsters by this id.
		monster.runtimeId = m_nextMonsterId++;
		monster.aiPath.clear(); // and the route its last plan gave it
		monster.aiCursor = 0;
	}
	m_partyWiped = false;
	// Everything the level had in flight or under way - shots, a volley, a blast
	// still spreading, an Earth light, the fixture damage table, what rides the
	// monsters, a stair or a pit fall (code-review C292). A LOAD comes through
	// here too, so a gas thrown before it used to go on biting the loaded game.
	ClearLevelTransients();
	m_fellPending = false; // and a fall's bruise still owed: a fresh start owes nothing
	// The fixture damage table again, from this same map: the level stays, so no
	// level load re-seeds it. Fresh, since ClearLevelTransients emptied it - the
	// re-seed carries an old entry's wreck over by cell, and this is where a
	// smashed sconce used to survive a new game or a load (C293).
	SeedFixtureBreakables();
	ClearTracks();          // and the tracks monsters left (6g)
	// Rebuild items from the .ent baseline so runes return to their spawn cells
	// (and any dropped tablets from a prior session are forgotten).
	m_items.clear();
	LoadItems();
	for (Button& b : m_buttons) b.activated = false; // un-press for a fresh run
	for (Door& d : m_doors) { // back to the authored state, no ghost slide
		// Whole again first (C293): a door smashed after a save came back from
		// the load SHUT AND WRECKED - blocking the way, past opening or breaking.
		// A load re-breaks what the save says was broken (ApplyActiveSnapshot).
		d.brk.Mend();
		d.open = d.initialOpen;
		d.openT = d.open ? 1.0f : 0.0f;
	}
	// A smashed prop keeps its place in the list (DecorationTarget says why), so
	// mending it is lifting the flag; the save's `broken` lines re-break it.
	for (Decoration& deco : m_decorations) deco.brk.Mend();
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
	// Everything above snapped back with no travel for the shadow cubes to see
	// (code-review C178): a same-level new game starts them over too.
	m_shadows.InvalidateCubes();
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
	// --- 0. what no level file puts back -----------------------------------
	// The harness's modes, rest, the clocks, other levels' stashes and the undo
	// history. What the LEVEL had under way (a blast, the fixture damage table, a
	// fall) is ResetForNewGame's below, as it is for a real new game (C292).
	ResetEvalTransients();

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

	// --- 2. the dynamic layer -----------------------------------------------
	// Party pose, monster hp/threat/awareness, the wipe latch, the level's
	// transients (blasts, projectiles, the fixture damage table - seeded afresh
	// from the map just read - monster effects, a fall), items, buttons, doors
	// and props mended, niches, fog, torch palette. AFTER the map, because it
	// puts the party on the map's start cell. Damage done to the dungeon is
	// mended in there too, for a new game and a load alike (C292, C293); it used
	// to be mended here alone, so only the harness's reset saw it.
	ResetForNewGame();

	// --- 3. make it real -----------------------------------------------------
	// The FULL bake, as `arena` does: every cell in the map may have changed.
	BuildDungeonMeshes();
	RebuildFiresAndDust(); // the level's fires are back, and a doused one burns
}

void DungeonWorld::ResetEvalTransients() {
	// A blast mid-flight, the fixture damage table and a pit fall mid-plunge used
	// to be cleared HERE, so only the harness's reset was rid of them - a real
	// new game, a load or a stair carried them on (code-review C292, C293). They
	// are ClearLevelTransients' now, which both of a reset's ways reach through
	// ResetForNewGame (and the switch through its level load as well).

	// The harness's own modes. NOT `lockstep` or the RNG seed: those are how the
	// run is DRIVEN rather than what it contains, and silently changing them
	// under a script would be its own kind of contamination — a script that set
	// them once at the top would find them gone after its first reset.
	// One member, so a field added to Harness is reset here for free — the four
	// loose bools this replaced were four chances to forget one.
	m_harness = {};
	m_resting = false;
	m_restEndReason = "";
	// The two clocks a fresh world starts at zero (C294, brought forward from
	// batch 78 because the `transients` readout caught the kindle clock with
	// nothing injected: it runs every frame, so a reset handed the next test
	// whatever phase the last one ended on - 0.100 against a new game's 0.250).
	// C294's other half, a new game or a load doing the same, is still batch 78.
	m_throwCooldown = {};
	m_kindleClock = 0.0f;

	// EVERY OTHER LEVEL BACK TO ITS FILE (C300). A script that walked out of a
	// dungeon, edited a level it was browsing or travelled through an ambush left
	// stashes - a parked level's fog and kills, a remote edit's map - and the
	// next script's way into that level would find them there, where a fresh
	// process would read the files. (m_levelStates, the dynamic stashes, goes
	// in ResetForNewGame.)
	m_levelMaps.clear();
	m_levelEnts.clear();
	// ...and the editor's history, whose every step is a snapshot of a session
	// this reset just ended. Undo after it would restore the old one's fog, dead
	// monsters and door states. ONE call site for C300 and C297: when C297 puts
	// this in ResetForNewGame, which ResetForEval calls, this line goes.
	ClearUndoHistory();
}

DungeonWorld::TransientReport DungeonWorld::Transients() const {
	TransientReport r;
	r.blasts = static_cast<int>(m_activeBlastCount); // the live ones, not the table
	for (const Monster& m : m_monsters) {
		r.monsterEffects += static_cast<int>(m.effects.size());
		if (!m.effects.empty()) ++r.monstersAffected;
	}
	// A piece of dungeon counts once whatever it is: broken, else hurt, and
	// whatever is riding it either way.
	const auto piece = [&r](const Breakable& brk, int& broken) {
		if (!brk.Damageable()) return;
		if (brk.broken) ++broken;
		else if (brk.hp < brk.maxHp) ++r.hurtPieces;
		r.pieceEffects += static_cast<int>(brk.effects.size());
	};
	for (const FixtureBreak& fb : m_fixtureBreaks) piece(fb.brk, r.brokenFixtures);
	for (const Decoration& d : m_decorations) piece(d.brk, r.brokenDecorations);
	for (const Door& d : m_doors) piece(d.brk, r.brokenDoors);
	r.fallPending = m_falling;
	r.fellPending = m_fellPending;
	r.fallT = m_fallT;
	r.undo = static_cast<int>(m_undoStack.size());
	r.redo = static_cast<int>(m_redoStack.size());
	r.stashedMaps = static_cast<int>(m_levelMaps.size());
	r.stashedEnts = static_cast<int>(m_levelEnts.size());
	r.stashedStates = static_cast<int>(m_levelStates.size());
	// The stems, so a leftover says WHICH level (each container is sorted; the
	// union is merged and de-duplicated here).
	std::vector<std::string> stems;
	for (const auto& [stem, map] : m_levelMaps) stems.push_back(stem);
	for (const auto& [stem, ents] : m_levelEnts) stems.push_back(stem);
	for (const auto& [stem, state] : m_levelStates) stems.push_back(stem);
	std::sort(stems.begin(), stems.end());
	stems.erase(std::unique(stems.begin(), stems.end()), stems.end());
	for (const std::string& stem : stems)
		r.stashedLevels += (r.stashedLevels.empty() ? "" : " ") + stem;
	if (r.stashedLevels.empty()) r.stashedLevels = "none";
	r.resting = m_resting;
	r.lockstep = LockstepAI();
	r.throwCooldown = m_throwCooldown;
	r.kindleClock = m_kindleClock;
	return r;
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
	// Doors, levers, floor items, bodies and broken props all jump to the saved
	// state below, with no travel for the shadow cubes to see (code-review C178).
	m_shadows.InvalidateCubes();

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
	ReserveAIPools();  // ...and its placed monsters some of the AI pools'
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
	if (includeLive && !m_parked) {
		out.levels.push_back(SnapshotActive());
		// A thrown thing still in the air goes in as the floor item it would be
		// if it came down now, in the square it is over, and keeps flying here
		// (code-review C47). Here and not in SnapshotActive,
		// whose other callers - a level's stash, an undo step - must not see a
		// flight as an item it would then hold twice.
		SaveFlyingCargo(out.levels.back());
	}
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
