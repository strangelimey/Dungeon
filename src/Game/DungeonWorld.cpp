// ============================================================================
// Game/DungeonWorld.cpp — see DungeonWorld.h.
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

namespace {
// The unlit fill (no sun underground). Lifted from {0.035,0.032,0.045} after
// the albedo sRGB switch; the dev console's `ambient <x>` scales it live for
// mood tuning (SetAmbientScale).
constexpr Vec3 kBaseAmbient{0.052f, 0.048f, 0.064f};
} // namespace

// ============================================================================
// Construction — cheap setup only (the map files parse fast); the heavy asset
// work is queued by AppendLoadTasks and runs behind the loading screen.
// ============================================================================
// The project's first level stem (the one the game opens), falling back to
// "level1" for a project whose manifest names no levels. A static member (not a
// file-local) because both the ctor here and AppendLoadTasks in
// DungeonWorld_Load.cpp need it.
std::string DungeonWorld::FirstLevel(const Project& p) {
	return p.levels.empty() ? std::string("level1") : p.levels.front();
}

// The fixture-record routing DungeonMap's parser needs (it has no catalog
// access): which ids hang on walls, and what the 'T'/'F' glyphs resolve to.
FixtureTypes DungeonWorld::FixtureTypesOf(const Project& p) {
	FixtureTypes t;
	t.wallMount.clear();
	for (const CatalogEntry& e : p.fixtures.Entries())
		if (e.Get("mount", "floor") == "wall") t.wallMount.push_back(e.id);
	t.sconceDefault = p.defaultSconce;
	t.brazierDefault = p.defaultBrazier;
	return t;
}

const gfx::Texture* DungeonWorld::FixtureIcon(const std::string& type) const {
	const auto it = m_fixtureKinds.find(type);
	return it == m_fixtureKinds.end() ? nullptr : it->second->iconTarget.get();
}

DungeonWorld::DungeonWorld(gfx::GraphicsDevice& device, gfx::Renderer& renderer,
						   audio::AudioEngine& audio, const SoundBank& sounds,
						   const GameSettings& settings, const Project& project,
						   threads::Manager& threadManager)
	: m_device(device), m_renderer(renderer), m_audio(audio), m_sounds(sounds),
	  m_settings(settings), m_project(project),
	  m_map(project.LevelMapPath(FirstLevel(project)), FixtureTypesOf(project)),
	  m_entities(project.LevelEntPath(FirstLevel(project)), m_map),
	  m_party(m_map, m_map.StartX(), m_map.StartZ()), m_director(threadManager) {
	m_currentLevel = FirstLevel(project);
	// Resolve the level's palette ids → texture set names + height scales before
	// any load task runs (SurfaceDefs and LoadDungeonBlocks read the results).
	ResolveSurfacePalettes();
	// Party event hooks (survive Party::Reset). Feedback goes out through the
	// shared sound bank and the onMessage log line.
	m_party.onStep = [this] {
		m_audio.Play(m_sounds.footstep, 0.8f);
		const int px = m_party.GridX(), pz = m_party.GridZ();
		MarkSeen(px, pz);
		// Marching is exertion (docs/combat.md Phase 4): every standing
		// member spends the per-step trickle. The spend's regen holdoff makes
		// SUSTAINED marching a net drain while strolling-with-pauses stays
		// neutral — and it trains VIT (part 3's conditioning loop).
		if (m_roster)
			for (Character& member : *m_roster)
				if (member.IsAlive())
					SpendStamina(member, m_balance.staminaStep);
		// Stepping onto a stair raises a pending transition; Game polls it after
		// Update and drives the swap, so the level never changes mid-step. A
		// non-traversable link (a pit's ceiling hole — you can't climb it) is
		// scenery. A `fall` link (the pit itself) doesn't swap immediately:
		// the transition is LATCHED and Update sequences the plunge — the step
		// glide finishes, the camera drops through the hole, then the swap —
		// with the party's facing preserved so they land looking the way they
		// fell (movement is swallowed meanwhile, queued actions dropped).
		for (const StairLink& s : m_map.Stairs())
			if (s.x == px && s.z == pz) {
				const CatalogEntry* e = m_project.stairs.Find(s.type);
				if (!CatalogBool(e, "traverse", true)) break;
				if (CatalogBool(e, "fall", false)) {
					if (onMessage) onMessage(loc::Tr("world.pitfall"));
					m_pendingFall = LevelTransition{
						s.destLevel, s.destX, s.destZ,
						static_cast<Direction>(m_party.Facing())};
					m_fallT = -1.0f; // wait for the step glide to finish
					m_party.ClearBufferedAction();
				} else {
					m_pendingTransition =
						LevelTransition{s.destLevel, s.destX, s.destZ, s.destFacing};
				}
				break;
			}
	};
	m_party.onBlocked = [this] {
		m_audio.Play(m_sounds.bump, 0.9f);
		onMessage(loc::Tr("log.bump"));
	};
	m_party.onTurn = [this] { m_audio.Play(m_sounds.turn, 0.6f); };
	m_party.onBumpImpact = [this] { OnBumpImpact(); };
	m_party.isOccupied = [this](int x, int z) {
		for (const Monster& monster : m_monsters) {
			if (monster.Alive() && monster.x == x && monster.z == z) {
				m_audio.Play(m_sounds.monster, 0.8f);
				onMessage(loc::Format("log.monster_blocks",
									  loc::Tr("monster." + monster.kind->name)));
				return true;
			}
		}
		if (m_map.BrazierAt(x, z)) {
			m_audio.Play(m_sounds.bump, 0.7f);
			onMessage(loc::Tr("log.brazier_blocks"));
			return true;
		}
		if (const Door* door = DoorAt(x, z); door && !door->open) {
			m_audio.Play(m_sounds.bump, 0.7f);
			onMessage(loc::Tr("log.door_blocks"));
			return true;
		}
		for (const Decoration& deco : m_decorations) {
			if (deco.solid && deco.x == x && deco.z == z) {
				m_audio.Play(m_sounds.bump, 0.7f);
				onMessage(loc::Tr("log.decoration_blocks"));
				return true;
			}
		}
		return false;
	};

	// Fog of war: nothing revealed until the party stands somewhere. Seed the
	// start cell so the map isn't blank the moment it opens.
	m_seen.assign(static_cast<size_t>(m_map.Width()) * m_map.Height(), 0);
	MarkSeen(m_party.GridX(), m_party.GridZ());

	// The unlit ambient fill (kBaseAmbient above); the dev console's
	// `ambient <x>` scales it live for mood tuning.
	m_lights.ambient = kBaseAmbient;
	m_ambientScale = 1.0f;
	m_lights.directional.color = {0, 0, 0}; // no sun underground
	// Rebuilt every frame into retained capacity — no steady-state allocation.
	m_lights.points.reserve(gfx::kMaxPointLights);

	// The attack formula's tuning (docs/combat.md): knob sheet + per-attack
	// numbers from the project's balance.cat/attacks.cat (missing files keep
	// the C++ first-cut defaults). Magic reads it too (spell_stat).
	m_balance.Load(m_project.balance, m_project.attacks);

	// Magic system: build the spell registry (the Spell classes + the
	// project's spells.cat numeric overrides), and wire the CAST SERVICES —
	// the capability surface a spell's Cast() lands its effect through
	// (Spell/Spell.h), so the magic module stays walled off from the world.
	m_magic.LoadSpells(m_project.spells);
	m_magic.SetBalance(&m_balance);
	m_magic.SetCastServices(
		{[this](const ProjectileSpec& bolt) { m_projectiles.Spawn(bolt); },
		 [this](const Character& member, const std::string& line) {
			 MemberMessage(member, line);
		 }});

	// Moving-item engine: wire its world seam so a projectile lives "on the map"
	// without the engine depending on the map/combat. resolveHit is faction-aware —
	// it dispatches by the item's target side.
	m_projectiles.isBlocked = [this](const Vec3& p) {
		const int cx = static_cast<int>(std::floor(p.x / kCellSize));
		const int cz = static_cast<int>(std::floor(p.z / kCellSize));
		if (!m_map.IsWalkable(cx, cz)) return true; // wall / off-map stops it
		const Door* door = DoorAt(cx, cz);
		return door && !door->open; // a closed door stops bolts like a wall
	};
	m_projectiles.resolveHit = [this](TargetSide side, const ProjectileImpact& impact) {
		switch (side) {
		case TargetSide::Monsters:
			return ResolveSpellHit(impact); // a party spell strikes a monster
		case TargetSide::Party:
			// A monster bolt strikes the party (push doesn't apply — the party
			// isn't displaceable; a future gust trap would need its own path).
			return ResolveMonsterProjectileHit(impact);
		}
		return false;
	};
	m_projectiles.onFizzle = [this](const Vec3&) { m_audio.Play(m_sounds.spellFizzle, 0.6f); };
}

// ============================================================================
// Per-frame simulation
// ============================================================================

bool DungeonWorld::IsSeen(int x, int z) const {
	if (x < 0 || z < 0 || x >= m_map.Width() || z >= m_map.Height()) return false;
	return m_seen[static_cast<size_t>(z) * m_map.Width() + x] != 0;
}

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
	// than letting FreeItemSlotNear stack overlapping tablets.
	int here = 0;
	for (const Item& it : m_items)
		if (!it.collected && it.x == x && it.z == z) ++here;
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
	if (map.RemoveDecorationRecordAt(x, z) || map.RemoveFixtureAt(x, z)) {
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
// How long a hit-feedback splat stays over a struck member's portrait.
constexpr float kHitFlashSeconds = 0.7f;
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

void DungeonWorld::PlacePartyAt(int x, int z, Direction facing) {
	m_party.SetGridPosition(x, z);
	m_party.SetFacing(static_cast<int>(facing));
	MarkSeen(x, z);
}

bool DungeonWorld::FloorHoleAt(int x, int z) const {
	// A down stair / pit drops its cell's floor block: the below-grade shaft
	// mesh (with its own collar and walls) replaces it. Absent `hole` field:
	// any down-leading type opens the floor, an up type opens nothing.
	const StairLink* s = m_map.StairAt(x, z);
	if (!s) return false;
	const CatalogEntry* e = m_project.stairs.Find(s->type);
	return CatalogGet(e, "hole", CatalogBool(e, "up", false) ? "none" : "floor") ==
		   "floor";
}

bool DungeonWorld::CeilingHoleAt(int x, int z) const {
	// A pit's lower half drops its cell's CEILING block (hole = ceiling,
	// explicit only) — the rising shaft mesh replaces it.
	const StairLink* s = m_map.StairAt(x, z);
	if (!s) return false;
	return CatalogGet(m_project.stairs.Find(s->type), "hole", "none") == "ceiling";
}

void DungeonWorld::RebuildChunkRegion(int chunkX, int chunkZ) {
	const int chunksX = (m_map.Width() + kChunkCells - 1) / kChunkCells;
	const int chunkIndex = chunkZ * chunksX + chunkX;
	DungeonGeometry r = BuildDungeonRegion(
		m_map, m_wallBlocks, m_floorBlocks, m_ceilingBlocks, chunkX, chunkZ,
		[this](int x, int z) {
			return CellHoles{FloorHoleAt(x, z), CeilingHoleAt(x, z)};
		});
	auto replace = [&](Surface& surface, std::vector<GeometryChunk>& fresh) {
		std::erase_if(surface.chunks,
					  [&](const SurfaceChunk& sc) { return sc.chunk == chunkIndex; });
		for (GeometryChunk& gc : fresh) surface.chunks.push_back(MakeSurfaceChunk(gc));
	};
	replace(m_walls, r.walls);
	replace(m_floors, r.floors);
	replace(m_ceilings, r.ceilings);
}

void DungeonWorld::RebuildChunksAround(int x, int z) {
	if (m_walls.chunks.empty()) return; // geometry not built yet
	m_device.WaitIdle();                // old chunk meshes may still be in flight

	// The edit changes (x,z) plus the wall faces its orthogonal neighbours share
	// with it, so rebuild every distinct chunk those cells fall in (≤ 5).
	const int n[5][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
	int doneX[5], doneZ[5], count = 0;
	for (const auto& o : n) {
		const int cx = x + o[0], cz = z + o[1];
		if (cx < 0 || cz < 0 || cx >= m_map.Width() || cz >= m_map.Height()) continue;
		const int rx = cx / kChunkCells, rz = cz / kChunkCells;
		bool seen = false;
		for (int i = 0; i < count; ++i)
			if (doneX[i] == rx && doneZ[i] == rz) seen = true;
		if (seen) continue;
		doneX[count] = rx;
		doneZ[count] = rz;
		++count;
		RebuildChunkRegion(rx, rz);
	}
}

void DungeonWorld::MarkSeen(int x, int z) {
	for (int dz = -1; dz <= 1; ++dz) {
		for (int dx = -1; dx <= 1; ++dx) {
			const int cx = x + dx, cz = z + dz;
			if (cx < 0 || cz < 0 || cx >= m_map.Width() || cz >= m_map.Height())
				continue;
			m_seen[static_cast<size_t>(cz) * m_map.Width() + cx] = 1;
		}
	}
}

void DungeonWorld::SetTorchPalette(int index) {
	m_torchPalette = index;
	switch (index) {
	case 1:  m_torchColor = {0.45f, 0.65f, 1.0f}; onMessage(loc::Tr("log.torch_cold")); break;
	case 2:  m_torchColor = {0.55f, 1.0f, 0.45f}; onMessage(loc::Tr("log.torch_eerie")); break;
	default: m_torchColor = {1.0f, 0.62f, 0.28f}; onMessage(loc::Tr("log.torch_warm")); break;
	}
}

// How long the pit-fall camera drop takes once the step glide has finished.
static constexpr float kPitFallSeconds = 0.55f;

void DungeonWorld::Update(const Input& input, float dt, float time, bool acceptInput) {
	m_time = time; // drives the rune emissive pulse in SubmitSceneGeometry
	if (acceptInput && !m_pendingFall) m_party.HandleInput(input);
	m_party.Update(dt);

	// Door panels slide toward their open/shut target (sideways into the wall).
	constexpr float kDoorSlideSeconds = 0.7f;
	for (Door& door : m_doors) {
		const float target = door.open ? 1.0f : 0.0f;
		if (door.openT < target)
			door.openT = std::min(target, door.openT + dt / kDoorSlideSeconds);
		else if (door.openT > target)
			door.openT = std::max(target, door.openT - dt / kDoorSlideSeconds);
	}

	// Pit fall sequencing: let the step glide onto the pit play out, then run
	// the camera drop (PartyEye applies it), then raise the latched transition.
	if (m_pendingFall) {
		if (m_fallT < 0.0f) {
			if (!m_party.IsMoving()) m_fallT = 0.0f;
		} else if ((m_fallT += dt) >= kPitFallSeconds) {
			m_pendingTransition = *m_pendingFall;
			m_pendingFall.reset();
			m_fallT = -1.0f;
		}
	}
	UpdateMonsters(dt);
	m_projectiles.Update(dt); // fly bolts, resolve impacts/fizzles via the hooks
	UpdateLights(time);
	UpdateCamera();

	// Advance the fires and gather their particles, sorted back-to-front so
	// the alpha-blended smoke composites correctly (additive flames are
	// order-independent).
	m_particleScratch.clear();
	for (Fire& fire : m_fires) {
		if (!fire.lit) continue; // an unlit torch has no flame/smoke
		fire.effect.Update(dt);
		fire.effect.AppendParticles(m_particleScratch);
	}
	// Projectiles in flight + their impact sparks render as additive billboards
	// alongside the flames (same premultiplied-additive blend).
	m_projectiles.AppendBillboards(m_particleScratch);
	// (Rune tablets glow as a whole via an additive emissive that pulses in their
	// element colour — applied to the mesh in SubmitSceneGeometry, not a billboard.)
	const Vec3 eye = PartyEye();
	const Vec3 fwd = m_camera.Forward();
	std::ranges::sort(m_particleScratch, std::greater{},
					  [&](const gfx::ParticleInstance& p) {
						  const Vec3 d = Sub(p.position, eye);
						  return d.x * fwd.x + d.y * fwd.y + d.z * fwd.z;
					  });
}

void DungeonWorld::SetFov(float degrees) {
	m_fovDegrees = std::clamp(degrees, 20.0f, 140.0f);
}

void DungeonWorld::SetAmbientScale(float s) {
	m_ambientScale = std::clamp(s, 0.0f, 10.0f);
	m_lights.ambient = {kBaseAmbient.x * m_ambientScale,
						kBaseAmbient.y * m_ambientScale,
						kBaseAmbient.z * m_ambientScale};
}

void DungeonWorld::EffectiveAtmosphere(const DungeonMap& map, float& dust,
									   float& haze, float& ambient) {
	const gfx::Atmosphere defaults;
	dust = map.DustDensity() >= 0.0f ? map.DustDensity() : defaults.density;
	haze = map.HazeAmbient() >= 0.0f ? map.HazeAmbient() : defaults.hazeAmbient;
	ambient = map.AmbientScale() >= 0.0f ? map.AmbientScale() : 1.0f;
}

void DungeonWorld::SetLevelAtmosphere(const std::string& stem, float dust,
									  float haze, float ambient) {
	if (stem == m_currentLevel) {
		m_map.SetAtmosphere(dust, haze, ambient);
		// Density/haze are per-frame shader constants and ambient is a light
		// value — apply directly; the turbidity GRID is untouched by these.
		m_atmosphere.density = dust;
		m_atmosphere.hazeAmbient = haze;
		SetAmbientScale(ambient);
	} else {
		EnsureMapStash(stem).SetAtmosphere(dust, haze, ambient);
	}
}

std::vector<std::string> DungeonWorld::MonsterList() const {
	std::vector<std::string> out;
	out.reserve(m_monsters.size());
	for (const Monster& m : m_monsters)
		out.push_back(std::format("{} @ {},{}", m.kind ? m.kind->name : "?", m.x, m.z));
	return out;
}

bool DungeonWorld::ToggleButtonAt(int x, int z, bool& out) {
	for (Button& b : m_buttons)
		if (b.x == x && b.z == z) {
			b.activated = !b.activated;
			out = b.activated;
			// The target wiring's first consumer: toggle the doors it names.
			ToggleDoorsNamed(b.target);
			return true;
		}
	return false;
}

std::vector<std::string> DungeonWorld::ButtonList() const {
	std::vector<std::string> out;
	out.reserve(m_buttons.size());
	for (const Button& b : m_buttons)
		out.push_back(std::format("{} @ {},{} = {}", b.id, b.x, b.z,
								  b.activated ? "on" : "off"));
	return out;
}

Vec3 DungeonWorld::PartyEye() const {
	Vec3 eye = m_party.EyePosition();
	if (m_pendingFall && m_fallT > 0.0f) {
		// Accelerating, gravity-ish: a full storey down by the time the swap
		// hits, so the view passes clean through the pit's shaft.
		const float t = std::min(m_fallT / kPitFallSeconds, 1.0f);
		eye.y -= t * t * (kWallHeight + 0.6f);
	}
	return eye;
}

void DungeonWorld::UpdateCamera() {
	m_camera.SetPosition(PartyEye());
	m_camera.SetYawPitch(m_party.EyeYaw(), m_party.EyePitch());
	m_camera.SetLens(m_fovDegrees * kPi / 180.0f,
					 static_cast<float>(m_device.Width()) /
						 static_cast<float>(m_device.Height()),
					 0.05f, 100.0f);
}

// A rune's "breath": one multiplier (~0.2 dim .. ~1.9 bright, phase-offset per
// item) shared by BOTH its emissive self-glow (SubmitSceneGeometry, in
// DungeonWorld_Render.cpp) and the light it casts (UpdateLights here), so the
// tablet and the light it throws pulse in exact lockstep — hence a static member
// rather than a file-local. Both callers pass the same frame time and item id.
float DungeonWorld::RunePulse(float time, int id) {
	return 1.05f + 0.85f * std::sin(time * 3.0f + static_cast<float>(id));
}

// Rebuilds the light list every frame: the carried torch follows the camera,
// wall torches flicker with independent phases. All flicker is
// product-of-sines — cheap, deterministic, and aperiodic enough.
void DungeonWorld::UpdateLights(float time) {
	m_lights.points.clear();

	const Vec3 eye = PartyEye(); // the carried torch falls with the camera
	const float flicker =
		0.92f + 0.08f * std::sin(time * 9.0f) * std::sin(time * 13.7f + 1.3f);
	gfx::PointLight torch;
	torch.position = {eye.x, eye.y + 0.25f, eye.z};
	torch.radius = 9.0f;
	torch.color = m_torchColor;
	torch.intensity = 2.6f * flicker;
	m_lights.points.push_back(torch);

	// One flickering light per fire, sitting just above its flame. Braziers
	// burn bigger and a touch redder than the wall sconces. The light
	// POSITION wanders too (incommensurate sine products, per-fire phase):
	// the shadow cubes re-render from the moved origin every frame, so the
	// shadows themselves dance the way real firelight does.
	for (const Fire& fire : m_fires) {
		if (!fire.lit) continue; // an unlit torch casts no light
		gfx::PointLight light;
		const float amp = fire.brazier ? 0.042f : 0.028f;
		const float wx = amp * std::sin(time * 7.3f + fire.phase) *
						 std::sin(time * 3.1f + fire.phase * 2.0f);
		const float wy = amp * 0.6f * std::sin(time * 9.1f + fire.phase * 1.3f);
		const float wz = amp * std::sin(time * 6.7f + fire.phase * 0.7f) *
						 std::sin(time * 2.6f + fire.phase);
		light.position = {fire.flamePos.x + wx, fire.flamePos.y + 0.15f + wy,
						  fire.flamePos.z + wz};
		// Braziers reach ~6 cells (14.4 m) so their shadow has room to fade in
		// gently over distance (AssignShadowSlots); inverse-square falloff keeps
		// the far tail dim, so this widens the lit pool without blowing out up
		// close. Wall sconces stay tighter.
		light.radius = fire.lightRadius;
		light.color = fire.brazier
						  ? Vec3{m_torchColor.x, m_torchColor.y * 0.85f, m_torchColor.z * 0.8f}
						  : m_torchColor;
		const float base = fire.brazier ? 2.3f : 1.8f;
		light.intensity = base * (0.9f + 0.1f * std::sin(time * 11.0f + fire.phase) *
											 std::sin(time * 7.3f + fire.phase));
		light.flickerShadow = true; // wandering origin → throttle its shadow cube
		light.longShadowFade = fire.brazier; // braziers fade over their long reach;
											 // sconces keep near-field shadows crisp
		m_lights.points.push_back(light);
	}

	// Each uncollected rune throws a soft pulsing light in its element colour,
	// breathing in lockstep with the tablet's emissive glow (same RunePulse).
	// Pure fill light — castsShadow=false keeps the cluster near the start from
	// stealing the few shadow cubes from the torch/fires.
	for (const Item& item : m_items) {
		if (item.collected || !item.kind->isRune) continue;
		const Vec3 c = SlotCenter(item.x, item.z, SizeClass::Medium, item.slot);
		gfx::PointLight glow;
		glow.position = {c.x, 0.4f, c.z};
		glow.radius = 4.8f;
		const Vec4& g = item.kind->glow;
		glow.color = {g.x, g.y, g.z};
		glow.intensity = 2.3f * RunePulse(time, item.id);
		glow.castsShadow = false;
		m_lights.points.push_back(glow);
	}

	// The renderer uploads only the active light budget (Settings → Video → Max
	// Lights, Low=16 .. Ultra=64) and shadow slots only consider those, so on a
	// large level the fire count alone can crowd out a light pushed late (a
	// rune glow). Keep the ones NEAREST the eye instead of the first ones
	// pushed; the carried torch sits at the eye, so it always survives (and
	// still wins shadow slot 0 in AssignShadowSlots).
	const size_t budget = static_cast<size_t>(
		std::clamp(m_settings.maxPointLights, 1, static_cast<int>(gfx::kMaxPointLights)));
	if (m_lights.points.size() > budget) {
		auto distSq = [&](const gfx::PointLight& l) {
			const Vec3 d = Sub(l.position, eye);
			return d.x * d.x + d.y * d.y + d.z * d.z;
		};
		std::nth_element(m_lights.points.begin(),
						 m_lights.points.begin() + budget, m_lights.points.end(),
						 [&](const gfx::PointLight& a, const gfx::PointLight& b) {
							 return distSq(a) < distSq(b);
						 });
		m_lights.points.resize(budget);
	}

	m_shadows.AssignSlots(m_lights.points, eye, m_shadowsEnabled);
}

void DungeonWorld::UpdateMonsters(float dt) {
	const Vec3 partyPos = m_party.EyePosition();

	// Danger gate for the unconscious (docs/combat.md Phase 5): any live
	// monster within its own aggro range of the party resets every downed
	// member's stabilize clock — nobody comes to mid-melee.
	bool danger = false;
	{
		const int px = m_party.GridX(), pz = m_party.GridZ();
		for (const Monster& m : m_monsters)
			if (m.Alive() && std::abs(m.x - px) + std::abs(m.z - pz) <=
								 static_cast<int>(m.kind->aggroRange)) {
				danger = true;
				break;
			}
	}

	// Tick down each member's per-hand swing cooldowns so hands free up over
	// time, fade out the hit-feedback splat, regenerate mana (scaled by
	// intelligence) so spent spell points recover between casts, age any
	// active ward (the Protect shields) so it fades with a log line, and run
	// the unconscious members' stabilize clocks.
	if (m_roster)
		for (Character& member : *m_roster) {
			// Self-stabilize: an UNCONSCIOUS (not dead) member accrues safe
			// seconds and wakes at a fraction of max once the coast is clear.
			if (!member.IsAlive() && !member.dead) {
				if (danger) {
					member.stabilize = 0.0f;
				} else {
					member.stabilize += dt;
					if (member.stabilize >= m_balance.stabilizeTime) {
						member.stabilize = 0.0f;
						member.health =
							m_balance.stabilizeHealth * member.maxHealth;
						MemberMessage(member, loc::Format("log.member_wakes",
														  member.name));
					}
				}
			}
			for (float& cd : member.handCooldown)
				if (cd > 0.0f) cd -= dt;
			if (member.hitFlash > 0.0f) member.hitFlash -= dt;
			if (member.IsAlive() && member.mana < member.maxMana) {
				member.mana += member.ManaRegenPerSec() * dt;
				if (member.mana > member.maxMana) member.mana = member.maxMana;
			}
			// Stamina regen (docs/combat.md Phase 4): the holdoff after any
			// spend keeps sustained exertion a net drain; then the bar
			// refills and the exhausted latch clears with hysteresis (past
			// the exhaust_recover fraction, so it can't flicker at zero).
			if (member.IsAlive() && member.staminaHoldoff > 0.0f) {
				member.staminaHoldoff -= dt;
			} else if (member.IsAlive() && member.stamina < member.maxStamina) {
				member.stamina +=
					(m_balance.staminaRegen +
					 m_balance.staminaRegenMax * member.maxStamina) *
					dt;
				if (member.stamina > member.maxStamina)
					member.stamina = member.maxStamina;
				if (member.exhausted &&
					member.stamina >=
						m_balance.exhaustRecover * member.maxStamina) {
					member.exhausted = false;
					MemberMessage(member,
								  loc::Format("log.recovered", member.name));
				}
			}
			// Age the status effects; an expired one leaves with its kind's
			// fade line. (Spend-to-die wards — the water pool, the air
			// charges — are erased at their spend site instead, so their
			// burst/still lines replace the fade.) Poison/bleed DoT damage
			// accumulates HERE and wounds once after the loop — WoundMember
			// can mutate the effects list (a water ward bursts soaking it),
			// so it must not run mid-iteration.
			float dot = 0.0f;
			for (StatusEffect& e : member.effects) {
				e.timeLeft -= dt;
				if (e.kind == StatusKind::Poison || e.kind == StatusKind::Bleed)
					dot += e.magnitude * dt;
				if (e.timeLeft > 0.0f) continue;
				if (e.kind == StatusKind::Ward)
					MemberMessage(member, loc::Format("log.shield_fades", member.name));
				else if (e.kind == StatusKind::Poison || e.kind == StatusKind::Bleed)
					MemberMessage(member, loc::Format("log.effect_fades", member.name,
													  loc::Tr(e.nameKey)));
			}
			std::erase_if(member.effects,
						  [](const StatusEffect& e) { return e.timeLeft <= 0.0f; });
			// The quiet DoT wound — it ticks a DOWNED member too, and a wound
			// on someone already at 0 is death by the overkill rule (Phase 5):
			// poison finishes the fallen, so get them clear of the fight.
			if (dot > 0.0f) WoundMember(member, dot, /*quiet=*/true);
		}
	// A DoT tick can down (or finish) the last standing member — the wipe
	// latch must notice without a monster swinging.
	if (m_roster) CheckPartyWipe();

	// Re-derive groups from current co-location (monsters sharing a cell are one
	// group — merge/split as they converge/spread), then assign formation targets
	// (surround), publish the world for the worker threads, and adopt their plans.
	// All cheap main-thread work — the pathfinding itself runs on the bucket threads.
	ReconcileGroups();
	AssignFormation();
	BuildAISnapshot();
	ConsumeAIPlans();

	for (size_t i = 0; i < m_monsters.size(); ++i) {
		Monster& monster = m_monsters[i];
		DriveMonsterAnim(monster, dt); // animates the living AND the dying (death clip)
		if (!monster.Alive()) continue; // downed — no AI, no movement, not solid
		if (monster.attackCd > 0.0f) monster.attackCd -= dt;
		if (monster.moveCd > 0.0f) monster.moveCd -= dt;

		// Advance an in-flight glide; the logical cell already moved when the
		// step committed, so the tween just slides visualPos to the new centre.
		if (monster.moving) {
			monster.moveT += dt / std::max(monster.kind->moveInterval, 0.05f);
			const Vec3 target = SlotCenter(monster.x, monster.z, monster.kind->size,
										   monster.slot);
			if (monster.moveT >= 1.0f) {
				monster.moving = false;
				monster.moveT = 0.0f;
				monster.visualPos = target;
			} else {
				const float s = monster.moveT * monster.moveT *
								(3.0f - 2.0f * monster.moveT); // smoothstep
				monster.visualPos = {monster.moveFrom.x + (target.x - monster.moveFrom.x) * s,
									 monster.moveFrom.y + (target.y - monster.moveFrom.y) * s,
									 monster.moveFrom.z + (target.z - monster.moveFrom.z) * s};
			}
		} else {
			// Settled in a cell (no step in flight): reposition WITHIN the cell
			// (Phase 4). First, item 7 — a grouped, aware monster shifts toward the
			// free slot nearest the party (the front rank), claiming it if closer
			// than its current slot. Sequential on the main thread, so two monsters
			// never claim the same slot in one tick.
			const int cap = SlotsPerCell(monster.kind->size);
			const int adjDist = std::max(std::abs(monster.x - m_party.GridX()),
										 std::abs(monster.z - m_party.GridZ()));
			if (monster.aware && cap > 1 && adjDist <= 1 &&
				AliveInGroup(monster.groupId) >= 2) {
				u32 used = 0; // slots held by other live same-size monsters here
				for (size_t j = 0; j < m_monsters.size(); ++j) {
					if (j == i) continue;
					const Monster& o = m_monsters[j];
					if (o.Alive() && o.x == monster.x && o.z == monster.z &&
						SlotsPerCell(o.kind->size) == cap && o.slot >= 0 && o.slot < cap)
						used |= (1u << o.slot);
				}
				auto slotDistSq = [&](int s) {
					const Vec3 c = SlotCenter(monster.x, monster.z, monster.kind->size, s);
					const float dx = partyPos.x - c.x, dz = partyPos.z - c.z;
					return dx * dx + dz * dz;
				};
				int best = monster.slot;
				float bestD = slotDistSq(monster.slot);
				for (int s = 0; s < cap; ++s) {
					if (s == monster.slot || (used & (1u << s))) continue;
					const float d = slotDistSq(s);
					if (d < bestD - 0.01f) { bestD = d; best = s; } // margin damps churn
				}
				monster.slot = best; // claim (atomic: main-thread serial)
			}
			// Ease toward the desired in-cell anchor (front-centre for a lone
			// sub-cell monster, else the slot centre). A no-op once settled there.
			const Vec3 anchor = DesiredAnchor(monster, partyPos);
			constexpr float kInCellSettle = 6.0f;
			const float k = std::min(1.0f, dt * kInCellSettle);
			monster.visualPos = {monster.visualPos.x + (anchor.x - monster.visualPos.x) * k,
								 monster.visualPos.y + (anchor.y - monster.visualPos.y) * k,
								 monster.visualPos.z + (anchor.z - monster.visualPos.z) * k};
		}

		// Facing (every frame, for ALL monsters incl. idle ones). A moving monster
		// turns toward its DIRECTION OF TRAVEL; a stationary AWARE monster faces the
		// party; otherwise it holds its resting facing (so a group can stand facing
		// away while the party sneaks up). The visual yaw eases toward the target so
		// turns glide. Radially-symmetric monsters (faces=false, the blob) never turn.
		if (monster.kind->facesTarget) {
			if (monster.moving) {
				const Vec3 dest = SlotCenter(monster.x, monster.z, monster.kind->size,
											 monster.slot);
				const float dx = dest.x - monster.moveFrom.x;
				const float dz = dest.z - monster.moveFrom.z;
				if (dx * dx + dz * dz > 1e-6f) monster.targetYaw = std::atan2(dx, dz);
			} else if (monster.aware) {
				monster.targetYaw = std::atan2(partyPos.x - monster.visualPos.x,
											   partyPos.z - monster.visualPos.z);
			}
			constexpr float kPi = 3.14159265358979f;
			constexpr float kTurnLerp = 12.0f; // higher = snappier turn
			float d = monster.targetYaw - monster.yaw;
			while (d > kPi) d -= 2.0f * kPi;
			while (d < -kPi) d += 2.0f * kPi;
			monster.yaw += d * std::min(1.0f, dt * kTurnLerp);
		}

		if (monster.intent.mode == ai::Intent::Mode::Idle) {
			// Idle behaviour: a patroller walks its route (which also carries it back
			// to post after a chase); else a leashed monster displaced from its anchor
			// walks home.
			if (!monster.patrol.empty())
				UpdatePatroller(monster, static_cast<int>(i));
			else if (monster.leashRange > 0.0f &&
					 (monster.x != monster.leashX || monster.z != monster.leashZ))
				UpdateReturner(monster, static_cast<int>(i));
			continue;
		}
		if (monster.intent.mode == ai::Intent::Mode::Kite) {
			UpdateKiter(monster, static_cast<int>(i)); // skirmisher: hold range + shoot
			continue;
		}
		if (monster.intent.mode == ai::Intent::Mode::Flee) {
			UpdateFleer(monster, static_cast<int>(i)); // wounded: run from the party
			continue;
		}

		// ACT (every frame, at the monster's OWN cadence): execute the standing
		// orders the workers handed us. A low-IQ monster still moves fast and swings
		// relentlessly here; only its CHANGE OF MIND (re-planning) lags.

		// A monster swings only when it has REACHED its assigned attack cell AND is
		// orthogonally adjacent to the party — not from a diagonal, and not from a
		// near side it was merely passing through on its way to the side it was
		// assigned. So a monster steered to an open flank circles around to it
		// instead of stopping early and clumping the group on the approach corner.
		// (This orthogonal-only rule from the movement work subsumes the animation
		// branch's separate Manhattan-distance diagonal-attack fix.)
		const int orthoDist = std::abs(monster.x - m_party.GridX()) +
							  std::abs(monster.z - m_party.GridZ());
		// A monster melees from its post within its REACH (Phase 7): 1 = the
		// adjacent ring as always; a pike (monsters.cat `reach = 2`) strikes
		// from its queue post too — but only down a clear shared row/column
		// (the orthogonal rule; distance 1 is orthogonal by construction).
		const bool inReach =
			orthoDist == 1 ||
			(orthoDist <= monster.kind->reach &&
			 (monster.x == m_party.GridX() || monster.z == m_party.GridZ()) &&
			 CellHasLineOfSight(monster.x, monster.z, m_party.GridX(),
								m_party.GridZ()));
		const bool atPost = monster.x == monster.targetX && monster.z == monster.targetZ &&
							inReach;

		// Announce once, when the party is actually adjacent (or a pike is
		// already close enough to strike from its queue post).
		if (!monster.announced && (orthoDist <= 1 || atPost)) {
			monster.announced = true;
			onMessage(loc::Format("log.monster_stirs",
								  loc::Tr("monster." + monster.kind->name)));
			m_audio.Play(m_sounds.monster, 0.7f);
		}

		if (atPost) {
			// On its assigned side: swing at a random standing member off cooldown.
			if (monster.attackCd <= 0.0f) MonsterAttack(monster);
		} else if (!monster.moving && monster.moveCd <= 0.0f) {
			// Not adjacent: follow the cached path the workers computed. Skip any
			// cells already at/behind the monster, then take the next one only if
			// it is still a free 4-neighbour (re-validated against LIVE occupancy,
			// since the path was pathed on another thread against a snapshot). A
			// stale/blocked step is dropped — the next plan will re-route.
			while (monster.aiCursor < monster.aiPath.size() &&
				   monster.aiPath[monster.aiCursor].x == monster.x &&
				   monster.aiPath[monster.aiCursor].z == monster.z)
				++monster.aiCursor;

			if (monster.aiCursor < monster.aiPath.size()) {
				const ai::Cell c = monster.aiPath[monster.aiCursor];
				const bool adjacent = std::abs(c.x - monster.x) +
										  std::abs(c.z - monster.z) == 1;
				// Claim a free slot in the destination cell (re-validated against LIVE
				// occupancy — the path was planned on a worker against a snapshot).
				const int slot = adjacent ? FreeSlotInCell(c.x, c.z, monster.kind->size,
														   static_cast<int>(i))
										  : -1;
				if (slot >= 0) {
					StepMonsterTo(monster, c.x, c.z, slot);
					++monster.aiCursor;
				} else {
					monster.aiPath.clear(); // diverged or blocked: wait for a re-plan
					monster.aiCursor = 0;
				}
			}
		}
	}
}

float DungeonWorld::ClipDuration(const MonsterKind& kind, const std::string& name) const {
	for (const auto& c : kind.model.clips)
		if (c.name == name) return c.duration;
	return 0.0f;
}

// The ladder from live simulation onto a CreatureState, highest priority first:
// death and the one-shot events (spawn/hit/swing) win over the locomotion and
// awareness loops, so a flinch or swing plays out before the monster returns to
// walking or resting. Pure read of monster state — knows nothing about clips.
anim::CreatureState DungeonWorld::DesiredState(const Monster& m) const {
	using S = anim::CreatureState;
	const auto sup = [&](S s) { return m.kind->stateSupported[static_cast<int>(s)]; };
	// Death is unconditional (the clip table gates the visual; an unsupported Die
	// just has no clip → the corpse vanishes at once). Every other optional rung
	// falls through when the kind doesn't support it, down to the Idle floor.
	if (!m.Alive())                                             return S::Die;
	if ((m.spawnReq  || m.spawnAnim  > 0.0f) && sup(S::Spawn))  return S::Spawn;
	if ((m.hitReq    || m.hitAnim    > 0.0f) && sup(S::Hit))    return S::Hit;
	if ((m.attackReq || m.attackAnim > 0.0f) && sup(S::Attack)) return S::Attack;
	if (m.moving && sup(S::Walk))                               return S::Walk;
	if (m.aware  && sup(S::InCombat))                           return S::InCombat;
	return S::Idle;
}

// Resolves a state to an actual clip name through the kind's table, choosing a
// random variation when several are authored (empty = the state is unauthored).
std::string DungeonWorld::PickClip(const MonsterKind& kind, anim::CreatureState state) {
	const auto& cands = kind.animClips[static_cast<int>(state)];
	if (cands.empty()) return {};
	if (cands.size() == 1) return cands.front();
	return cands[m_combatRng() % cands.size()];
}

// Per-monster clip state machine. Resolves the desired CreatureState from live
// state (DesiredState), looks it up in the kind's data-driven animClips table (a
// random variation when several are authored), cross-fades on a change (a held
// looping clip re-Plays as a no-op inside the Animator), and advances the
// animator — including for downed monsters, so the death clip plays out before
// the corpse vanishes (deathAnim). An UNAUTHORED state resolves to no clip: a
// looping state simply leaves the previous clip playing (degrades to the
// pre-clip look), a one-shot is skipped so the ladder falls through next frame.
void DungeonWorld::DriveMonsterAnim(Monster& monster, float dt) {
	if (!monster.animator.HasSkeleton()) return; // flat models (blob): nothing to skin

	constexpr float kAnimFade = 0.12f; // cross-fade window between clips

	// Count down the active one-shot timers (a state holds while its timer runs).
	for (float* t : {&monster.spawnAnim, &monster.attackAnim, &monster.hitAnim,
					 &monster.deathAnim})
		if (*t > 0.0f) *t -= dt;

	const anim::CreatureState want = DesiredState(monster);
	if (want != monster.animState) {
		const std::string clip = PickClip(*monster.kind, want);
		if (!clip.empty()) {
			monster.animator.Play(clip, anim::IsLooping(want), kAnimFade);
			// Arm this state's hold timer from the variation we actually chose, so
			// the visual lasts exactly the clip that's playing (0 for the loops).
			const float d = ClipDuration(*monster.kind, clip);
			switch (want) {
			case anim::CreatureState::Spawn:  monster.spawnAnim = d; break;
			case anim::CreatureState::Attack: monster.attackAnim = d; break;
			case anim::CreatureState::Hit:    monster.hitAnim = d; break;
			case anim::CreatureState::Die:    monster.deathAnim = d; break;
			default: break; // looping states need no hold timer
			}
			monster.animState = want;
		} else if (anim::IsLooping(want)) {
			// Unauthored loop (e.g. no walk clip): adopt the state so we don't retry
			// every frame, but leave the previous clip playing.
			monster.animState = want;
		}
		// Unauthored one-shot: do nothing — the cleared request below lets the
		// ladder fall through to an authored state next frame.
	}

	// Consume the momentary event triggers (each is live for exactly one frame).
	monster.spawnReq = monster.attackReq = monster.hitReq = false;

	monster.animator.Update(dt);
}

DungeonWorld::Monster* DungeonWorld::MonsterByRuntimeId(u32 id) {
	if (id == 0) return nullptr;
	for (Monster& m : m_monsters)
		if (m.runtimeId == id) return &m;
	return nullptr;
}

bool DungeonWorld::MonsterInstanceAt(int cx, int cz, u32& runtimeId, std::string& type,
									 bool& asleep, float& leashRange, ai::Archetype& archetype,
									 float& keepRange, float& fleeBelow, std::string& spell,
									 Direction& facing) const {
	for (const Monster& m : m_monsters) {
		if (m.x != cx || m.z != cz || !m.kind) continue;
		runtimeId = m.runtimeId;
		return MonsterInstanceById(m.runtimeId, type, asleep, leashRange, archetype, keepRange,
								   fleeBelow, spell, facing);
	}
	return false;
}

bool DungeonWorld::MonsterInstanceById(u32 runtimeId, std::string& type, bool& asleep,
									   float& leashRange, ai::Archetype& archetype,
									   float& keepRange, float& fleeBelow, std::string& spell,
									   Direction& facing) const {
	for (const Monster& m : m_monsters) {
		if (m.runtimeId != runtimeId || !m.kind) continue;
		type = m.kind->name;
		asleep = m.asleep;
		leashRange = m.leashRange;
		archetype = m.Archetype(); // effective values (override or the type default)
		keepRange = m.KeepRange();
		fleeBelow = m.FleeBelow();
		spell = m.Spell();
		facing = m.facing;
		return true;
	}
	return false;
}

void DungeonWorld::ApplyMonsterInstance(u32 runtimeId, bool asleep, float leashRange,
										ai::Archetype archetype, float keepRange,
										float fleeBelow, const std::string& spell,
										Direction facing) {
	Monster* m = MonsterByRuntimeId(runtimeId);
	if (!m || !m->kind) return;
	m->asleep = asleep;
	m->leashRange = leashRange;
	if (m->facing != facing) { // turn a (stationary) monster to face the new way
		m->facing = facing;
		m->yaw = m->targetYaw = DirYaw(facing);
	}
	// Keep an override only when it differs from the type default (else inherit, so
	// the .ent stays minimal and a later type edit still flows through).
	m->archOverride =
		archetype == m->kind->archetype ? std::nullopt : std::optional(archetype);
	m->keepOverride =
		std::abs(keepRange - m->kind->keepRange) < 0.01f ? std::nullopt : std::optional(keepRange);
	m->fleeOverride =
		std::abs(fleeBelow - m->kind->fleeBelow) < 0.01f ? std::nullopt : std::optional(fleeBelow);
	m->spellOverride = spell == m->kind->spell ? std::nullopt : std::optional(spell);
}

void DungeonWorld::AddPatrolWaypoint(u32 runtimeId, int cx, int cz) {
	Monster* m = MonsterByRuntimeId(runtimeId);
	if (!m) return;
	// Ignore a repeat of the last cell (a double-click or a drag landing twice).
	if (!m->patrol.empty() && m->patrol.back().x == cx && m->patrol.back().z == cz) return;
	m->patrol.push_back({cx, cz});
}

void DungeonWorld::RemoveLastPatrolWaypoint(u32 runtimeId) {
	if (Monster* m = MonsterByRuntimeId(runtimeId); m && !m->patrol.empty())
		m->patrol.pop_back();
}

void DungeonWorld::ClearPatrol(u32 runtimeId) {
	if (Monster* m = MonsterByRuntimeId(runtimeId)) {
		m->patrol.clear();
		m->patrolIdx = 0;
	}
}

const std::vector<ai::Cell>* DungeonWorld::MonsterPatrol(u32 runtimeId) const {
	for (const Monster& m : m_monsters)
		if (m.runtimeId == runtimeId) return &m.patrol;
	return nullptr;
}

u32 DungeonWorld::MonsterRuntimeIdAt(int cx, int cz) const {
	for (const Monster& m : m_monsters)
		if (m.x == cx && m.z == cz) return m.runtimeId;
	return 0;
}

bool DungeonWorld::SconceAt(int cx, int cz, Direction* wall) const {
	for (const WallSconce& s : m_map.Sconces())
		if (s.x == cx && s.z == cz) {
			if (wall) *wall = s.wall;
			return true;
		}
	return false;
}

std::vector<std::pair<u32, std::string>> DungeonWorld::MonstersAt(int cx, int cz) const {
	std::vector<std::pair<u32, std::string>> out;
	for (const Monster& m : m_monsters)
		if (m.x == cx && m.z == cz && m.kind)
			out.emplace_back(m.runtimeId, m.kind->name);
	return out;
}

std::vector<Direction> DungeonWorld::SconcesAt(int cx, int cz) const {
	std::vector<Direction> out;
	for (const WallSconce& s : m_map.Sconces())
		if (s.x == cx && s.z == cz) out.push_back(s.wall);
	return out;
}

std::vector<Direction> DungeonWorld::SolidWallsAt(int cx, int cz) const {
	std::vector<Direction> out;
	for (Direction d : {Direction::North, Direction::East, Direction::South, Direction::West})
		if (!m_map.IsWalkable(cx + DirDX(d), cz + DirDZ(d))) out.push_back(d);
	return out;
}

void DungeonWorld::RebuildFiresAndDust() {
	// Rebuild the fire instances + dust from the current map (shared by the sconce
	// edits and AddFixture): the sconce prop/light/flame/smoke follow next frame.
	// Drain the GPU first since it may still read the old turbidity grid.
	m_device.WaitIdle();
	m_fires.clear();
	BuildFires();
	BuildTurbidityMap();
}

bool DungeonWorld::RemountSconce(int cx, int cz, Direction from, Direction to) {
	if (!m_map.SetSconceWall(cx, cz, from, to)) return false;
	RebuildFiresAndDust();
	return true;
}

bool DungeonWorld::TorchSettings(int cx, int cz, Direction wall, bool& lit, float& brightness,
								 float& turbidity) const {
	for (const WallSconce& s : m_map.Sconces())
		if (s.x == cx && s.z == cz && s.wall == wall) {
			lit = s.lit;
			brightness = s.brightness;
			turbidity = s.turbidity;
			return true;
		}
	return false;
}

bool DungeonWorld::SetTorchSettings(int cx, int cz, Direction wall, bool lit, float brightness,
									float turbidity) {
	if (!m_map.SetSconceProps(cx, cz, wall, lit, brightness, turbidity)) return false;
	RebuildFiresAndDust();
	return true;
}

bool DungeonWorld::BrazierAt(int cx, int cz) const {
	return m_map.BrazierAt(cx, cz) != nullptr;
}

bool DungeonWorld::SolidDecorationAt(int cx, int cz) const {
	for (const Decoration& deco : m_decorations)
		if (deco.solid && deco.x == cx && deco.z == cz) return true;
	return false;
}

bool DungeonWorld::BrazierSettings(int cx, int cz, bool& lit, float& brightness,
								   float& turbidity) const {
	const FloorBrazier* b = m_map.BrazierAt(cx, cz);
	if (!b) return false;
	lit = b->lit;
	brightness = b->brightness;
	turbidity = b->turbidity;
	return true;
}

std::string DungeonWorld::SconceTypeAt(int cx, int cz, Direction wall) const {
	for (const WallSconce& s : m_map.Sconces())
		if (s.x == cx && s.z == cz && s.wall == wall) return s.type;
	return "sconce";
}

std::string DungeonWorld::BrazierTypeAt(int cx, int cz) const {
	const FloorBrazier* b = m_map.BrazierAt(cx, cz);
	return b ? b->type : "brazier";
}

bool DungeonWorld::SetBrazierSettings(int cx, int cz, bool lit, float brightness,
									  float turbidity) {
	if (!m_map.SetBrazierProps(cx, cz, lit, brightness, turbidity)) return false;
	RebuildFiresAndDust();
	return true;
}

bool DungeonWorld::RemoveFixtureAt(int cx, int cz) {
	if (!m_map.RemoveFixtureAt(cx, cz)) return false;
	RebuildFiresAndDust();
	return true;
}

std::vector<std::pair<int, std::string>> DungeonWorld::DecorationsAt(int cx, int cz) const {
	std::vector<std::pair<int, std::string>> out;
	for (int i = 0; i < static_cast<int>(m_decorations.size()); ++i) {
		const Decoration& d = m_decorations[i];
		if (d.x == cx && d.z == cz && !d.stair) // stairs edit via their own record
			out.emplace_back(i, d.kind ? d.kind->id : std::string("?"));
	}
	return out;
}

std::vector<std::pair<int, std::string>> DungeonWorld::ItemsAt(int cx, int cz) const {
	std::vector<std::pair<int, std::string>> out;
	for (const Entity& e : m_entities.All())
		if (e.kind == EntityKind::Item && e.x == cx && e.z == cz)
			out.emplace_back(e.id, e.type);
	return out;
}

std::vector<ProjectileInfo> DungeonWorld::ProjectilesAt(int cx, int cz) const {
	std::vector<ProjectileInfo> out;
	for (const ProjectileInfo& p : m_projectiles.Live())
		if (static_cast<int>(p.pos.x / kCellSize) == cx &&
			static_cast<int>(p.pos.z / kCellSize) == cz)
			out.push_back(p);
	return out;
}

std::string DungeonWorld::DecorationTypeByIndex(int index) const {
	if (index < 0 || index >= static_cast<int>(m_decorations.size())) return {};
	const Decoration& d = m_decorations[static_cast<size_t>(index)];
	return d.kind ? d.kind->id : std::string();
}

bool DungeonWorld::DecorationShowsFacing(const std::string& type) const {
	const auto it = m_decorationKinds.find(type);
	return it == m_decorationKinds.end() || it->second->facingArrow;
}

bool DungeonWorld::MonsterShowsFacing(const std::string& type) const {
	const auto it = m_monsterKinds.find(type);
	return it == m_monsterKinds.end() || it->second->facesTarget;
}

void DungeonWorld::SetDecorationFacingArrow(const std::string& type, bool show) {
	const auto it = m_decorationKinds.find(type);
	if (it != m_decorationKinds.end()) it->second->facingArrow = show;
}

bool DungeonWorld::RemoveDecorationByIndex(int index) {
	if (index < 0 || index >= static_cast<int>(m_decorations.size())) return false;
	if (m_decorations[static_cast<size_t>(index)].stair) return false; // RemoveStairAt owns those
	m_decorations.erase(m_decorations.begin() + index);
	return true;
}

bool DungeonWorld::RemoveItemById(int entityId) {
	for (auto it = m_items.begin(); it != m_items.end(); ++it)
		if (it->id == entityId) {
			if (entityId >= 0) { // authored/placed: the .ent record goes too
				m_entities.RemoveById(entityId);
				m_entsDirty = true;
			}
			m_items.erase(it);
			return true;
		}
	return false;
}

Direction DungeonWorld::DecorationFacing(int index) const {
	if (index >= 0 && index < static_cast<int>(m_decorations.size()))
		return m_decorations[static_cast<size_t>(index)].facing;
	return Direction::South;
}

void DungeonWorld::SetDecorationFacing(int index, Direction facing) {
	if (index < 0 || index >= static_cast<int>(m_decorations.size())) return;
	Decoration& d = m_decorations[static_cast<size_t>(index)];
	d.facing = facing;
	// Standing props rotate to the new facing; wall-mounted props keep their wall
	// orientation (the record facing is stored, but the transform stays wall-baked).
	if (!d.wallMounted) {
		const Vec3 pos = m_map.CellCenter(d.x, d.z);
		XMStoreFloat4x4(&d.world, XMMatrixRotationY(DirYaw(facing)) *
									  XMMatrixTranslation(pos.x, 0, pos.z));
	}
}

Direction DungeonWorld::ItemFacing(int entityId) const {
	for (const Entity& e : m_entities.All())
		if (e.id == entityId) return e.facing;
	return Direction::South;
}

void DungeonWorld::SetItemFacing(int entityId, Direction facing) {
	if (Entity* e = m_entities.MutableById(entityId)) e->facing = facing;
}

bool DungeonWorld::AnyInspectableAt(int cx, int cz) const {
	std::string target;
	return MonsterRuntimeIdAt(cx, cz) != 0 || SconceAt(cx, cz) || BrazierAt(cx, cz) ||
		   DoorAt(cx, cz) != nullptr || ButtonSettings(cx, cz, target) ||
		   !DecorationsAt(cx, cz).empty() || !ItemsAt(cx, cz).empty() ||
		   !ProjectilesAt(cx, cz).empty();
}

void DungeonWorld::ReconcileGroups() {
	// A GROUP is the set of monsters currently sharing a cell. Recomputed every
	// frame so groups MERGE automatically when monsters converge into one cell and
	// SPLIT when they spread apart — two lone monsters that end up in the same cell
	// become one group of two (and so take distinct slots + reposition normally,
	// instead of both treating themselves as "alone" and stacking at front-centre).
	u32 next = 1;
	for (Monster& m : m_monsters) m.groupId = 0;
	for (size_t i = 0; i < m_monsters.size(); ++i) {
		if (!m_monsters[i].Alive() || m_monsters[i].groupId != 0) continue;
		const u32 g = next++;
		for (size_t j = i; j < m_monsters.size(); ++j) {
			Monster& o = m_monsters[j];
			if (o.Alive() && o.x == m_monsters[i].x && o.z == m_monsters[i].z)
				o.groupId = g;
		}
	}
	m_nextGroupId = next; // keep the source past the last id used this frame
}

int DungeonWorld::AliveInGroup(u32 group) const {
	if (group == 0) return 0;
	int n = 0;
	for (const Monster& m : m_monsters)
		if (m.groupId == group && m.Alive()) ++n;
	return n;
}

Vec3 DungeonWorld::DesiredAnchor(const Monster& m, const Vec3& partyPos) const {
	// A lone Medium-or-smaller monster slides to FRONT-CENTRE so it can reach both
	// front party members: CENTRED on the cross-axis (the centre-line between the
	// grid columns) and at the CENTRE OF THE FRONT ROW of its slot grid, on the
	// side facing the party (snapped to the dominant cardinal axis — never a
	// diagonal). Larger sizes (Large/Huge) are already centred and keep their slot
	// anchor (item 8).
	if (m.aware && IsSubCellSize(m.kind->size) && AliveInGroup(m.groupId) <= 1) {
		const Vec3 c = m_map.CellCenter(m.x, m.z);
		const float dx = partyPos.x - c.x, dz = partyPos.z - c.z;
		const int dim = SlotDim(m.kind->size);
		// Distance from cell centre to the centre of an outer (front) row.
		const float frontOff = static_cast<float>(dim - 1) / (2.0f * dim) * kCellSize;
		if (std::abs(dx) >= std::abs(dz)) // party is mainly east/west → shift along X
			return {c.x + (dx >= 0.0f ? frontOff : -frontOff), c.y, c.z};
		return {c.x, c.y, c.z + (dz >= 0.0f ? frontOff : -frontOff)}; // mainly N/S
	}
	return SlotCenter(m.x, m.z, m.kind->size, m.slot);
}

void DungeonWorld::AssignFormation() {
	const int px = m_party.GridX(), pz = m_party.GridZ();
	// Default: HOLD position (target = own cell). Aware monsters get an attack cell
	// below; unaware ones aim at the party cell so cone perception can still fire;
	// overflow (no open side) keeps holding and queues behind the front.
	for (Monster& m : m_monsters) {
		m.targetX = m.x;
		m.targetZ = m.z;
		if (m.Alive() && !m.aware) {
			m.targetX = px;
			m.targetZ = pz;
		}
	}
	// Attack cells = the party's walkable orthogonal neighbours (the sides it can
	// be hit from). Track how many monsters we've assigned to each, to spread them.
	// At most 4 sides, so a fixed array + count — this pass runs EVERY frame and
	// must not heap-allocate (CLAUDE.md memory strategy).
	struct Side {
		int x, z, count;
	};
	std::array<Side, 4> sides;
	int sideCount = 0;
	static constexpr int kDX[4] = {0, 0, -1, 1};
	static constexpr int kDZ[4] = {-1, 1, 0, 0};
	for (int d = 0; d < 4; ++d) {
		const int sx = px + kDX[d], sz = pz + kDZ[d];
		if (m_map.IsWalkable(sx, sz)) sides[sideCount++] = {sx, sz, 0};
	}
	if (sideCount == 0) return;

	// Aware attackers, sorted nearest-to-party first. Member scratch, reused
	// frame to frame (clear keeps capacity) — no per-frame allocation.
	std::vector<int>& idx = m_formationScratch;
	idx.clear();
	for (size_t i = 0; i < m_monsters.size(); ++i)
		if (m_monsters[i].Alive() && m_monsters[i].aware)
			idx.push_back(static_cast<int>(i));
	auto cheby = [](int ax, int az, int bx, int bz) {
		return std::max(std::abs(ax - bx), std::abs(az - bz));
	};
	std::sort(idx.begin(), idx.end(), [&](int a, int b) {
		return cheby(m_monsters[a].x, m_monsters[a].z, px, pz) <
			   cheby(m_monsters[b].x, m_monsters[b].z, px, pz);
	});
	// Pass 1 (hysteresis): a monster already standing on a still-unclaimed side
	// HOLDS it, so a formed ring is stable frame-to-frame (no thrash / circling).
	// A claimed monster's entry is struck out (-1) so pass 2 skips it — the
	// sentinel doubles as the old separate `done` flags.
	for (size_t k = 0; k < idx.size(); ++k) {
		Monster& m = m_monsters[idx[k]];
		for (int s = 0; s < sideCount; ++s)
			if (sides[s].count == 0 && m.x == sides[s].x && m.z == sides[s].z) {
				m.targetX = sides[s].x;
				m.targetZ = sides[s].z;
				++sides[s].count;
				idx[k] = -1;
				break;
			}
	}
	// Pass 2 (fill): the rest take the least-crowded side with room (empty sides
	// before any doubles up → surround), tie-broken by the side nearest the monster.
	for (size_t k = 0; k < idx.size(); ++k) {
		if (idx[k] < 0) continue; // claimed a side in pass 1
		Monster& m = m_monsters[idx[k]];
		const int cap = SlotsPerCell(m.kind->size);
		int best = -1;
		for (int s = 0; s < sideCount; ++s) {
			if (sides[s].count >= cap) continue;
			if (best < 0 || sides[s].count < sides[best].count ||
				(sides[s].count == sides[best].count &&
				 cheby(m.x, m.z, sides[s].x, sides[s].z) <
					 cheby(m.x, m.z, sides[best].x, sides[best].z)))
				best = s;
		}
		if (best >= 0) {
			m.targetX = sides[best].x;
			m.targetZ = sides[best].z;
			++sides[best].count;
		}
		// else: every side full → keep holding (queue behind for promotion).
	}
}

std::vector<std::string> DungeonWorld::GroupsReport() const {
	// Gather groups in first-seen order so the report is stable run to run.
	std::vector<u32> order;
	for (const Monster& m : m_monsters) {
		if (!m.Alive()) continue;
		if (std::find(order.begin(), order.end(), m.groupId) == order.end())
			order.push_back(m.groupId);
	}
	std::vector<std::string> lines;
	if (order.empty()) {
		lines.push_back("no live monsters");
		return lines;
	}
	for (u32 g : order) {
		std::string kinds, cells;
		int n = 0;
		for (const Monster& m : m_monsters) {
			if (m.groupId != g || !m.Alive()) continue;
			++n;
			if (!kinds.empty()) kinds += ',';
			kinds += m.kind ? m.kind->name : "?";
			if (!cells.empty()) cells += ' ';
			cells += std::format("{},{}#{}", m.x, m.z, m.slot);
		}
		lines.push_back(std::format("group {}: {} [{}] @ {}", g, n, kinds, cells));
	}
	return lines;
}

// Build the immutable world snapshot the AI worker threads read, and publish it.
// The walkability grid is shared across frames (rebuilt only when the map's
// revision changes); the rest is a cheap copy of the party cell + live monster
// positions, plus the per-monster think inputs.
void DungeonWorld::BuildAISnapshot() {
	const int W = m_map.Width(), H = m_map.Height();
	// Rebuild the shared grid when the map changed OR its size no longer matches
	// (a level swap can reuse the same Revision() value but different dimensions —
	// reusing a stale grid there would read out of bounds on the worker thread).
	if (m_walkableRev != m_map.Revision() || !m_walkableCache ||
		m_walkableCache->size() != static_cast<size_t>(W) * H) {
		auto grid = std::make_shared<std::vector<uint8_t>>(
			static_cast<size_t>(W) * H); // value-initialised to 0
		// Braziers block like walls (the party bumps them too) — bake them into
		// the grid so monsters don't path through the fire. Placement/removal
		// bumps the map Revision, so edits invalidate this cache like any paint.
		for (int z = 0; z < H; ++z)
			for (int x = 0; x < W; ++x)
				(*grid)[static_cast<size_t>(z) * W + x] =
					(m_map.IsWalkable(x, z) && !m_map.BrazierAt(x, z)) ? 1 : 0;
		m_walkableCache = std::move(grid);
		m_walkableRev = m_map.Revision();
	}

	// Reuse a pooled snapshot that no worker (or the director) still holds — its
	// only ref is the pool's (use_count == 1). It is therefore not the published
	// snapshot and not in any worker's hands, so mutating it before we publish is
	// safe. The flat grids are zero-FILLED in place (assign reuses their buffers)
	// and clear() keeps the vectors' capacity, so steady-state frames do not
	// allocate. The pool grows to the in-flight high-water mark (~workers+1).
	std::shared_ptr<ai::Snapshot> snap;
	for (auto& s : m_snapshotPool)
		if (s.use_count() == 1) { snap = s; break; }
	if (!snap) {
		snap = std::make_shared<ai::Snapshot>();
		m_snapshotPool.push_back(snap);
	}
	snap->monsters.clear();

	snap->partyX = m_party.GridX();
	snap->partyZ = m_party.GridZ();
	snap->mapW = m_map.Width();
	snap->mapH = m_map.Height();
	snap->walkable = m_walkableCache;
	const size_t cellCount = static_cast<size_t>(W) * H;
	snap->blocked.assign(cellCount, 0);
	snap->occ.assign(cellCount, ai::CellOcc{});
	// Party cell is a hard block. Monster crowding is capacity-based: each live
	// monster bumps its cell's occupant count, tagged with the size's slots/cell so
	// a worker can tell a half-full same-size group (room) from a full or foreign one.
	snap->blocked[static_cast<size_t>(snap->partyZ) * snap->mapW + snap->partyX] = 1;
	// Solid decorations block like braziers, but they live in the world list, not
	// the map — placing/removing one does NOT bump the map Revision the walkable
	// cache keys off. So they go into `blocked` (rebuilt every frame) instead of
	// the cached grid: an editor placement takes effect on the next snapshot with
	// no invalidation to get wrong.
	for (const Decoration& deco : m_decorations)
		if (deco.solid)
			snap->blocked[static_cast<size_t>(deco.z) * snap->mapW + deco.x] = 1;
	// Closed doors block like solid decorations — and for the same reason they
	// live in the per-frame grid, not the cached one: opening one must take
	// effect on the next snapshot with no revision bookkeeping.
	for (const Door& door : m_doors)
		if (!door.open)
			snap->blocked[static_cast<size_t>(door.z) * snap->mapW + door.x] = 1;
	for (const Monster& m : m_monsters) {
		if (!m.Alive()) continue;
		const int cap = SlotsPerCell(m.kind->size);
		const int f = FootprintCells(m.kind->size);
		// Mark every cell the footprint covers (Huge = its 2x2 block), so a passer-by
		// of any size sees the cell occupied. The BFS self-excludes the pathed agent's
		// own footprint, so this aggregate count doesn't trap a Huge against itself.
		for (int fz = m.z; fz < m.z + f; ++fz)
			for (int fx = m.x; fx < m.x + f; ++fx) {
				if (fx < 0 || fz < 0 || fx >= snap->mapW || fz >= snap->mapH) continue;
				ai::CellOcc& o = snap->occ[fz * snap->mapW + fx];
				o.capacity = static_cast<uint8_t>(cap);
				++o.count;
			}
		const float hpFrac = m.kind->maxHp > 0.0f ? m.hp / m.kind->maxHp : 1.0f;
		// A swarm senses in all directions (no blind spot) regardless of its render
		// facing — the archetype bundles omnidirectional perception, so the brain
		// skips the sight cone for it just like it does for the blob (faces=false).
		const bool directional =
			m.kind->facesTarget && m.Archetype() != ai::Archetype::Swarm;
		// Named fields (not positional) — the Agent has grown past a dozen members
		// and a misplaced value would be silent; designated initos keep it honest.
		snap->monsters.push_back(ai::Agent{.id = m.runtimeId,
										   .x = m.x,
										   .z = m.z,
										   .aggroRange = m.kind->aggroRange,
										   .iq = m.kind->iq,
										   .capacity = cap,
										   .footprint = f,
										   .aware = m.aware,
										   .directional = directional,
										   .facingYaw = m.yaw,
										   .targetX = m.targetX,
										   .targetZ = m.targetZ,
										   .archetype = m.Archetype(),
										   .hpFrac = hpFrac,
										   .fleeBelow = m.FleeBelow(),
										   .asleep = m.asleep,
										   .leashX = m.leashX,
										   .leashZ = m.leashZ,
										   .leashRange = m.leashRange});
	}
	m_director.Publish(snap); // pass a copy — the pool keeps its own ref
}

// Adopt the freshest plan batch from each bucket into the matching monsters. We
// apply a batch only once (tracked by its sequence). Plans are keyed by stable
// runtimeId, so a plan whose monster has died/moved buckets/been erased simply
// finds no match here — it can never be misapplied to a different monster.
void DungeonWorld::ConsumeAIPlans() {
	for (int b = 0; b < ai::Scheduler::kBucketCount; ++b) {
		const ai::AsyncDirector::Batch batch = m_director.TakePlans(b);
		if (!batch.plans || batch.seq == m_lastPlanSeq[b]) continue; // nothing new
		m_lastPlanSeq[b] = batch.seq;
		for (const ai::Plan& plan : *batch.plans) {
			Monster* monster = MonsterByRuntimeId(plan.id);
			if (!monster) continue; // its monster is gone — drop the plan
			// First time the brain decides to act on the party (engage OR kite), the
			// monster has NOTICED it — latch awareness so it stays engaged even once
			// the party slips out of its sight cone (sticky; only a new game / reload
			// clears it).
			if (plan.intent.mode != ai::Intent::Mode::Idle) monster->aware = true;
			monster->intent = plan.intent;
			monster->aiPath = plan.path;
			// Align the cursor to the monster's current cell (it may have stepped
			// since the snapshot); fall back to the path start if it isn't on it.
			monster->aiCursor = 0;
			for (size_t k = 0; k < monster->aiPath.size(); ++k)
				if (monster->aiPath[k].x == monster->x &&
					monster->aiPath[k].z == monster->z) {
					monster->aiCursor = k + 1;
					break;
				}
		}
	}
}

void DungeonWorld::ProvokeMonster(Monster& monster) {
	// Latch awareness (sticky — the brain keeps it engaged) and set the engage
	// intent now so it reacts THIS frame (turn to the party, then chase/swing)
	// without waiting for its next async think. Only this monster wakes; its
	// neighbours stay oblivious until they notice the party themselves.
	monster.aware = true;
	monster.intent.mode = ai::Intent::Mode::Engage;
	monster.intent.targetX = m_party.GridX();
	monster.intent.targetZ = m_party.GridZ();
}

int DungeonWorld::FreeSlotInCell(int x, int z, SizeClass size, int self) const {
	const int f = FootprintCells(size); // 1, or 2 for Huge (a 2x2-cell block)
	const int cap = SlotsPerCell(size);
	// Every cell of the footprint must be in bounds, walkable, and clear of the
	// party. A 1-wide corridor fails this for a Huge → it can't enter (item 1).
	for (int fz = z; fz < z + f; ++fz)
		for (int fx = x; fx < x + f; ++fx) {
			if (fx < 0 || fz < 0 || fx >= m_map.Width() || fz >= m_map.Height())
				return -1;
			if (!m_map.IsWalkable(fx, fz)) return -1;
			if (m_map.BrazierAt(fx, fz)) return -1;    // blocks monsters like the party
			if (SolidDecorationAt(fx, fz)) return -1;  // ditto (statues, crates, ...)
			if (const Door* d = DoorAt(fx, fz); d && !d->open) return -1; // shut door
			if (fx == m_party.GridX() && fz == m_party.GridZ()) return -1;
		}
	// Mark the slots already taken in this cell. An occupant whose footprint
	// overlaps ours blocks the whole cell unless both are single-cell monsters of
	// the SAME size — only those share via distinct slots (homogeneous-group rule).
	u32 used = 0; // bitmask; cap <= 16 fits comfortably
	for (size_t i = 0; i < m_monsters.size(); ++i) {
		if (static_cast<int>(i) == self) continue;
		const Monster& o = m_monsters[i];
		if (!o.Alive()) continue;
		const int fo = FootprintCells(o.kind->size);
		const bool overlap = !(o.x + fo - 1 < x || o.x > x + f - 1 ||
							   o.z + fo - 1 < z || o.z > z + f - 1);
		if (!overlap) continue;
		if (f > 1 || fo > 1 || SlotsPerCell(o.kind->size) != cap) return -1;
		if (o.slot >= 0 && o.slot < cap) used |= (1u << o.slot);
	}
	for (int s = 0; s < cap; ++s)
		if (!(used & (1u << s))) return s;
	return -1;
}

bool DungeonWorld::CellFreeForMonster(int x, int z, int self) const {
	const SizeClass size = (self >= 0 && self < static_cast<int>(m_monsters.size()))
							   ? m_monsters[self].kind->size
							   : SizeClass::Large;
	return FreeSlotInCell(x, z, size, self) >= 0;
}

// One monster strike against a random standing party member. Sets the swing
// cooldown whether or not it lands so a packed cell doesn't machine-gun.
// Apply damage to a standing member: clamp health, flash the hit splat (severity by
// raw damage — small < 5, medium < 10, hard otherwise; a placeholder scale), and
// log a downing. The one place a member takes damage, shared by every attack path.
void DungeonWorld::MemberMessage(const Character& member,
								 const std::string& line) const {
	if (onMemberMessage) onMemberMessage(line, member.portraitColor);
	else onMessage(line);
}

void DungeonWorld::WoundMember(Character& target, float damage, bool quiet) {
	// Water Veil: water guards by ABSORBING — the ward soaks damage into its
	// pool (the ward's magnitude) before any reaches health, and BURSTS when
	// the pool is spent (unlike the timed wards, it dies by spending). Sitting
	// in the one place a member takes damage, it soaks every source alike —
	// melee, ranged, a wall bump, even a poison tick (silently — quiet mode
	// is per-frame). A partial soak lets the remainder through to the normal
	// wound path below.
	if (StatusEffect* ward = target.FindWard(SpellSymbol::Water);
		ward && damage > 0.0f) {
		const float soaked = std::min(ward->magnitude, damage);
		ward->magnitude -= soaked;
		damage -= soaked;
		if (!quiet)
			MemberMessage(target, loc::Format("log.shield_soaks", target.name));
		if (ward->magnitude <= 0.0f) {
			target.RemoveWard(SpellSymbol::Water); // spent — burst, not fade
			MemberMessage(target, loc::Format("log.shield_bursts", target.name));
		}
		if (damage <= 0.0f) return; // fully absorbed — no wound, no splat
	}
	// Death needs deliberate OVERKILL (docs/combat.md Phase 5): a hit landing
	// on a member ALREADY down — a poison tick against the unconscious counts
	// — or a single blow past the overkill knob. Anything less leaves them
	// unconscious, stabilizing back up once the danger passes (the tick in
	// UpdateMonsters).
	const bool wasDown = !target.IsAlive();
	target.health -= damage;
	if (target.health < 0.0f) target.health = 0.0f;
	if (!quiet) {
		target.hitFlash = kHitFlashSeconds;
		target.hitSeverity = damage < 5.0f ? 0 : (damage < 10.0f ? 1 : 2);
	}
	if (target.IsAlive()) return;
	target.stabilize = 0.0f; // the wound that downed them restarts the clock
	if (!target.dead &&
		(wasDown || damage >= m_balance.overkill * target.maxHealth)) {
		target.dead = true;
		MemberMessage(target, loc::Format("log.member_dies", target.name));
	} else if (!target.dead && !wasDown) {
		MemberMessage(target, loc::Format("log.member_down", target.name));
	}
}

// The one place skills grow (docs/skills.md). Levels derive from raw XP
// (floor(sqrt)), so a level-up is just "the derived number changed"; the
// SOURCE's associated stats creep behind at the creep_rate knob, the gain
// SPLIT EVENLY across them (docs/combat.md part 2 — a sword doesn't train
// stats twice as fast as a club), through the member's statProgress pools.
void DungeonWorld::GrantSkillXp(Character& member, std::string_view skillId,
								float xp, std::span<const std::string> stats) {
	if (skillId.empty() || xp <= 0.0f || !member.IsAlive()) return;

	float& total = member.skillXp[std::string(skillId)];
	const int before = Character::LevelForXp(total);
	total += xp;
	const int after = Character::LevelForXp(total);
	if (after > before)
		MemberMessage(member,
					  loc::Format("log.skill_up", member.name,
								  loc::Tr("skill." + std::string(skillId)), after));

	if (stats.empty()) return; // an unclassed source trains no stat
	const float gain =
		xp * m_balance.creepRate / static_cast<float>(stats.size());
	for (const std::string& stat : stats) {
		float& pool = member.statProgress[stat];
		pool += gain;
		if (pool < 1.0f) continue;
		pool -= 1.0f;
		GrantStatPoint(member, stat);
	}
}

// A whole stat point lands: increment, log, and re-derive the resource maxima
// (the resource formula — a VIT point is FELT as a bigger health/stamina pool,
// and the growth carries the current value so it reads as growth, not damage).
void DungeonWorld::GrantStatPoint(Character& member, std::string_view stat) {
	int value = 0;
	if (stat == "strength") value = ++member.strength;
	else if (stat == "dexterity") value = ++member.dexterity;
	else if (stat == "vitality") value = ++member.vitality;
	else if (stat == "willpower") value = ++member.willpower;
	else if (stat == "intelligence") value = ++member.intelligence;
	else return; // unknown id — parse already warned
	member.RecomputeMaxima(m_balance.kHealth, m_balance.kStamina, m_balance.kMana);
	MemberMessage(member, loc::Format("log.stat_up", member.name,
									  loc::Tr("stat." + std::string(stat)), value));
}

// Exertion (docs/combat.md part 3 + Phase 4): stamina is the exertion meter —
// every point spent feeds VIT's creep pool, holds regen off for a beat, and
// an emptied bar latches EXHAUSTED (the swing penalties; cleared with
// hysteresis in the regen tick). Swings and marching both arrive here.
void DungeonWorld::SpendStamina(Character& member, float points) {
	if (points <= 0.0f || !member.IsAlive()) return;
	member.stamina -= points;
	member.staminaHoldoff = m_balance.staminaHoldoff;
	if (member.stamina <= 0.0f) {
		member.stamina = 0.0f;
		if (!member.exhausted) {
			member.exhausted = true;
			MemberMessage(member, loc::Format("log.exhausted", member.name));
		}
	}
	float& pool = member.statProgress["vitality"];
	pool += points * m_balance.vitExertion;
	if (pool < 1.0f) return;
	pool -= 1.0f;
	GrantStatPoint(member, "vitality");
}

void DungeonWorld::RecomputePartyMaxima() {
	if (!m_roster) return;
	for (Character& member : *m_roster)
		member.RecomputeMaxima(m_balance.kHealth, m_balance.kStamina,
							   m_balance.kMana);
}

// --- the defender side (docs/combat.md part 4) --------------------------------
// One resist table per side, summed from its sources and clamped by the
// balance rule (±resist_clamp; an authored nature cell at 1.0 = immunity).

DefenseProfile DungeonWorld::PartyDefense(const Character& member,
										  DamageType type) {
	float resist = member.natureResists[type]; // the race layer
	float soak = 0.0f;
	for (const ItemSlot& slot : member.inventory.equipment) {
		if (slot.Empty()) continue;
		const ItemKind& kind = ItemKindFor(slot.typeId);
		resist += kind.resists[type];
		soak += kind.armor;
	}
	// Stone Skin: earth hardens — the ward's magnitude converts to PHYSICAL
	// resist at the stoneskin_resist knob (elemental bolts pass it by).
	if (IsPhysical(type))
		if (const StatusEffect* ward = member.FindWard(SpellSymbol::Earth))
			resist += ward->magnitude * m_balance.stoneskinResist;
	return {member.Evasion(), soak,
			m_balance.ClampResist(resist, member.natureResists[type])};
}

DefenseProfile DungeonWorld::MonsterDefense(const MonsterKind& kind,
											DamageType type) const {
	return {kind.evasion, kind.armor,
			m_balance.ClampResist(kind.resists[type], kind.resists[type])};
}

void DungeonWorld::ApplyHitEffect(Character& target, StatusKind kind,
								  const MonsterKind::HitEffect& fx) {
	if (fx.dps <= 0.0f || fx.duration <= 0.0f) return;
	std::uniform_real_distribution<float> roll(0.0f, 1.0f);
	if (roll(m_combatRng) > fx.chance) return;
	// Reapply REFRESHES (the ward-recast rule): the old effect goes, the new
	// one lands whole. School carries only the HUD tint — poison rides earth
	// green, bleed fire red (the palette convention until richer art exists).
	const bool poison = kind == StatusKind::Poison;
	target.RemoveEffect(kind);
	target.effects.push_back({kind,
							  poison ? SpellSymbol::Earth : SpellSymbol::Fire,
							  poison ? "effect.poison" : "effect.bleed",
							  fx.duration, fx.duration, fx.dps});
	MemberMessage(target, loc::Format(poison ? "log.poisoned" : "log.bleeding",
									  target.name));
}

// Latch the party wipe exactly once when the last member falls (message + callback).
// Returns true the frame it latches. Shared by the melee/ranged/bump damage paths.
bool DungeonWorld::CheckPartyWipe() {
	if (m_partyWiped) return false;
	for (const Character& m : *m_roster)
		if (m.IsAlive()) return false; // someone still up
	m_partyWiped = true;
	onMessage(loc::Tr("log.party_wipe"));
	if (onPartyWipe) onPartyWipe();
	return true;
}

void DungeonWorld::MonsterAttack(Monster& monster) {
	if (!m_roster || m_partyWiped) return;

	// Pick uniformly among the members still up.
	std::array<size_t, 4> alive;
	size_t n = 0;
	for (size_t i = 0; i < m_roster->size() && n < alive.size(); ++i)
		if ((*m_roster)[i].IsAlive()) alive[n++] = i;
	if (n == 0) return;

	Character& target = (*m_roster)[alive[m_combatRng() % n]];
	monster.attackCd = monster.kind->attackInterval;
	// Request the swing animation (one-shot; DriveMonsterAnim picks the variation
	// and times the hold, then the state machine returns to walk/idle). No attack
	// clip authored → DesiredState still yields Attack for a frame but PickClip is
	// empty, so nothing plays — the pre-clip look, as before.
	monster.attackReq = true;

	const AttackProfile atk{monster.kind->damage, monster.kind->accuracy,
							monster.kind->damageType};
	const DefenseProfile def = PartyDefense(target, atk.type);
	const AttackResult r = ResolveAttack(atk, def, m_balance.Strike(), m_combatRng);
	const std::string name = loc::Tr("monster." + monster.kind->name);

	if (!r.hit) {
		MemberMessage(target, loc::Format("log.monster_misses", name, target.name));
		return;
	}
	MemberMessage(target, loc::Format("log.monster_hits", name, target.name,
									  static_cast<int>(r.damage + 0.5f)));
	m_audio.Play(m_sounds.monster, 0.6f);
	WoundMember(target, r.damage);
	// On-hit DoTs (Phase 6): the landed blow may envenom/open a wound —
	// applied even if the blow itself downed them (the ticks finish the job).
	ApplyHitEffect(target, StatusKind::Poison, monster.kind->poison);
	ApplyHitEffect(target, StatusKind::Bleed, monster.kind->bleed);
	// Fire Shield retaliation: fire guards by burning back — a monster that
	// LANDS a melee blow on a fire-warded member is scorched for the ward's
	// power (the hit itself is not reduced; earth is the school that hardens).
	// Fires even if the blow downs the member — the ward outlives its bearer's
	// last stand by exactly one burn.
	if (const StatusEffect* ward = target.FindWard(SpellSymbol::Fire)) {
		monster.hp -= ward->magnitude;
		onMessage(loc::Format("log.shield_burns", name,
							  static_cast<int>(ward->magnitude + 0.5f)));
		if (!monster.Alive()) {
			monster.hp = 0.0f; // downed monster stays in the list (save restore)
			onMessage(loc::Format("log.monster_slain", name));
		} else {
			monster.hitReq = true; // the burn stings — flinch like any hit
		}
	}
	CheckPartyWipe();
}

// A blocked move has lurched the party into the obstacle. Every standing member
// is jarred for a small flat amount, with the smallest splat over each portrait
// and a single grunt — then we re-check for a wipe so a final stumble still ends
// the run cleanly.
void DungeonWorld::OnBumpImpact() {
	if (!m_roster || m_partyWiped) return;

	constexpr float kBumpDamage = 2.0f; // small flat jar, regardless of armor
	bool anyHurt = false;
	for (Character& member : *m_roster) {
		if (!member.IsAlive()) continue;
		WoundMember(member, kBumpDamage); // severity 0 at this damage; logs any downing
		anyHurt = true;
	}
	if (!anyHurt) return;

	onMessage(loc::Format("log.bump_hurt", static_cast<int>(kBumpDamage + 0.5f)));
	m_audio.Play(m_sounds.oof, 0.8f);
	CheckPartyWipe();
}

// The one place a monster's one-cell move is committed — the logical cell/slot
// snap the instant the step commits (so occupancy is atomic, like the party),
// while visualPos glides from where it stood over moveInterval.
void DungeonWorld::StepMonsterTo(Monster& monster, int x, int z, int slot) {
	monster.moveFrom = monster.visualPos;
	monster.x = x;
	monster.z = z;
	monster.slot = slot;
	monster.moving = true;
	monster.moveT = 0.0f;
	monster.moveCd = monster.kind->moveInterval;
}

// Greedy local step for the kite/flee executors: pick the lowest-scoring of the
// monster's own cell (the hold baseline) and its four free orthogonal neighbours,
// and step there. `score` is evaluated on candidate CELLS; only walkable, slot-
// free neighbours are considered (FreeSlotInCell also excludes the party cell).
void DungeonWorld::GreedyStep(Monster& monster, int selfIndex,
							  const std::function<int(int cx, int cz)>& score) {
	int bestScore = score(monster.x, monster.z); // own cell = hold baseline
	int bx = monster.x, bz = monster.z, bslot = monster.slot;
	static constexpr int kDX[4] = {1, -1, 0, 0}, kDZ[4] = {0, 0, 1, -1};
	for (int k = 0; k < 4; ++k) {
		const int nx = monster.x + kDX[k], nz = monster.z + kDZ[k];
		const int slot = FreeSlotInCell(nx, nz, monster.kind->size, selfIndex);
		if (slot < 0) continue; // unwalkable / occupied / the party cell
		const int s = score(nx, nz);
		if (s < bestScore) { bestScore = s; bx = nx; bz = nz; bslot = slot; }
	}
	if (bx != monster.x || bz != monster.z) StepMonsterTo(monster, bx, bz, bslot);
}

// ----------------------------------------------------------------------------
// Skirmisher (archetype = skirmisher): hold at range and shoot. The brain sets
// intent == Kite (no path); this executor drives movement + firing directly from
// live party position on the main thread, every frame at the monster's cadence.
// ----------------------------------------------------------------------------
void DungeonWorld::UpdateKiter(Monster& monster, int selfIndex) {
	const int px = m_party.GridX(), pz = m_party.GridZ();
	const int dist = std::max(std::abs(monster.x - px), std::abs(monster.z - pz));
	const bool los = CellHasLineOfSight(monster.x, monster.z, px, pz);

	// Announce once, like a brute, when it first has the party in reach.
	if (!monster.announced) {
		monster.announced = true;
		onMessage(loc::Format("log.monster_stirs", loc::Tr("monster." + monster.kind->name)));
		m_audio.Play(m_sounds.monster, 0.7f);
	}

	// Fire when it can see the party and is within its shooting reach (its perception
	// range), off cooldown. A blocked line holds fire (it repositions instead).
	if (los && static_cast<float>(dist) <= monster.kind->aggroRange &&
		monster.attackCd <= 0.0f)
		MonsterRangedAttack(monster);

	// Hold keepRange while lining up a shot: greedy 1-step to the free 4-neighbour
	// that best trades off distance-to-keepRange against being able to FIRE — i.e.
	// on a clear cardinal line to the party (orthogonal LoS) within reach. So it
	// backs off when crowded, closes when too far, and side-steps onto the party's
	// row/column to get the axis a bolt needs. Holds when its own cell scores best.
	// No BFS: kiting is a local decision the host makes each step against LIVE occupancy.
	if (!monster.moving && monster.moveCd <= 0.0f) {
		const int want = static_cast<int>(monster.KeepRange() + 0.5f);
		GreedyStep(monster, selfIndex, [&](int cx, int cz) {
			const int d = std::max(std::abs(cx - px), std::abs(cz - pz));
			int s = std::abs(d - want) * 2; // primary: distance error
			// Strongly prefer a cell it can actually shoot from (on-axis + in range);
			// getting onto the party's row/column is the point of a kiter.
			const bool canFire = static_cast<float>(d) <= monster.kind->aggroRange &&
								 CellHasLineOfSight(cx, cz, px, pz);
			if (!canFire) s += 4;
			return s;
		});
	}
}

// ----------------------------------------------------------------------------
// Flee (intent == Flee): a wounded monster below its fleeBelow threshold breaks
// off and runs. Greedy orthogonal 1-step that MAXIMISES distance from the party
// (the opposite of the brute chase); no attack. Holds if boxed in — a cornered
// monster has nowhere to run. Announce is left to whatever woke it.
// ----------------------------------------------------------------------------
void DungeonWorld::UpdateFleer(Monster& monster, int selfIndex) {
	if (monster.moving || monster.moveCd > 0.0f) return; // mid-step / on cooldown

	// Run away: score = NEGATED squared distance to the party, so GreedyStep (which
	// minimises) picks the FARTHEST free neighbour, and holds if none is farther.
	const int px = m_party.GridX(), pz = m_party.GridZ();
	GreedyStep(monster, selfIndex, [&](int cx, int cz) {
		const int dx = cx - px, dz = cz - pz;
		return -(dx * dx + dz * dz);
	});
}

void DungeonWorld::UpdateReturner(Monster& monster, int selfIndex) {
	if (monster.moving || monster.moveCd > 0.0f) return; // mid-step / on cooldown
	// Walk home: score = squared distance to the leash anchor, so GreedyStep steps
	// to the nearest free neighbour (and holds once it arrives).
	const int ax = monster.leashX, az = monster.leashZ;
	GreedyStep(monster, selfIndex, [&](int cx, int cz) {
		const int dx = cx - ax, dz = cz - az;
		return dx * dx + dz * dz;
	});
}

void DungeonWorld::UpdatePatroller(Monster& monster, int selfIndex) {
	if (monster.patrol.empty()) return;
	if (monster.moving || monster.moveCd > 0.0f) return; // mid-step / on cooldown
	// Advance to the next waypoint once standing on the current one (wrap the loop).
	monster.patrolIdx %= monster.patrol.size();
	const ai::Cell& cur = monster.patrol[monster.patrolIdx];
	if (monster.x == cur.x && monster.z == cur.z)
		monster.patrolIdx = (monster.patrolIdx + 1) % monster.patrol.size();
	const ai::Cell& wp = monster.patrol[monster.patrolIdx];
	// Greedy orthogonal step toward the waypoint (best for open/line routes; a wall
	// between can stall it — lay waypoints densely, or add BFS later).
	GreedyStep(monster, selfIndex, [&](int cx, int cz) {
		const int dx = cx - wp.x, dz = cz - wp.z;
		return dx * dx + dz * dz;
	});
}

void DungeonWorld::MonsterRangedAttack(Monster& monster) {
	monster.attackCd = monster.kind->attackInterval;
	monster.attackReq = true; // play the swing/cast gesture if the rig ships one

	// Launch a bolt down the CARDINAL axis it shares with the party (the caller only
	// fires when CellHasLineOfSight is true, which is orthogonal-only — so the party
	// is straight N/E/S/W). Aiming along the axis, like the party's own spell bolts,
	// keeps everything on the 4-directional grid; no diagonal shots. It flies through
	// the shared moving-item engine and strikes the party when it reaches their cell
	// (TargetSide::Party -> ResolveMonsterProjectileHit); a wall fizzles it.
	const int px = m_party.GridX(), pz = m_party.GridZ();
	Vec3 dir{0.0f, 0.0f, 0.0f};
	if (monster.z == pz && monster.x != px)
		dir.x = px > monster.x ? 1.0f : -1.0f; // same row: fire east/west
	else if (monster.x == px && monster.z != pz)
		dir.z = pz > monster.z ? 1.0f : -1.0f; // same column: fire north/south
	else
		return; // not axis-aligned (shouldn't happen — caller gates on orthogonal LoS)
	Vec3 origin = SlotCenter(monster.x, monster.z, monster.kind->size, monster.slot);
	origin.y += 0.6f;

	// A CASTER (archetype = caster with a monsters.cat `spell`) throws that
	// spell's bolt — Spell::MonsterBolt, the same class the party casts from,
	// at the monster's accuracy (no monster mana/vocab; it just shoots on
	// cooldown). A spell with no thrown form (a ward) yields nothing, and any
	// other ranged monster (a skirmisher), a plain ember bolt.
	const Spell* spell =
		monster.Spell().empty() ? nullptr : m_magic.FindSpell(monster.Spell());
	if (spell) {
		if (const std::optional<ProjectileSpec> bolt =
				spell->MonsterBolt(origin, dir, monster.kind->accuracy)) {
			m_projectiles.Spawn(*bolt);
			m_audio.Play(m_sounds.spellCast, 0.6f); // the cast voice
			return;
		}
	}
	ProjectileSpec bolt;
	bolt.pos = origin;
	bolt.dir = dir;
	bolt.target = TargetSide::Party;
	bolt.speed = 6.0f;
	bolt.range = (monster.kind->aggroRange + 1.0f) * kCellSize; // a bit past aggro
	// The plain ember bolt types as the monster's melee (its dmgtype); a real
	// spell bolt above types by its school inside MakeBolt.
	bolt.atk = {monster.kind->damage, monster.kind->accuracy,
				monster.kind->damageType};
	bolt.color = {1.6f, 0.5f, 0.2f, 0.0f}; // ember-orange additive
	bolt.size = 0.18f;
	m_projectiles.Spawn(bolt);
	m_audio.Play(m_sounds.monster, 0.5f); // soft launch cue (reuse the monster voice)
}

bool DungeonWorld::CellHasLineOfSight(int x0, int z0, int x1, int z1) const {
	// ORTHOGONAL-only over the LIVE map, mirroring ai::SnapshotView::HasLineOfSight:
	// a clear line exists only down a shared row or column (no diagonal sight/fire),
	// with every cell strictly between walkable; endpoints never block.
	if (x0 == x1 && z0 == z1) return true;
	if (x0 == x1) {
		const int s = z0 < z1 ? 1 : -1;
		for (int z = z0 + s; z != z1; z += s)
			if (!m_map.IsWalkable(x0, z)) return false;
		return true;
	}
	if (z0 == z1) {
		const int s = x0 < x1 ? 1 : -1;
		for (int x = x0 + s; x != x1; x += s)
			if (!m_map.IsWalkable(x, z0)) return false;
		return true;
	}
	return false; // not axis-aligned — no orthogonal line
}

bool DungeonWorld::PartyAttack(size_t member, size_t hand, std::string_view verb) {
	if (!m_roster || member >= m_roster->size() || hand > 1) return false;
	Character& attacker = (*m_roster)[member];
	if (!attacker.IsAlive() || attacker.handCooldown[hand] > 0.0f) return false;

	// The cell directly ahead of the party.
	const Direction faced = static_cast<Direction>(m_party.Facing());
	const int tx = m_party.GridX() + DirDX(faced);
	const int tz = m_party.GridZ() + DirDZ(faced);

	Monster* target = nullptr;
	for (Monster& m : m_monsters)
		if (m.Alive() && m.x == tx && m.z == tz) { target = &m; break; }

	// The swinging hand's weapon: catalog damage/speed/stats feed the formula
	// below; its `skill` is the weapon class (docs/skills.md) — a bare hand
	// swings, and trains, unarmed. The class level scales the profile, the
	// landed blow below trains the class + creeps its associated stats.
	const ItemSlot& held = attacker.inventory.Hand(static_cast<int>(hand));
	const ItemKind* weapon = held.Empty() ? nullptr : &ItemKindFor(held.typeId);

	// The rear rank can't reach (Phase 7): roster slots 0-1 are the FRONT
	// line, 2-3 the REAR — a rear member swings only a polearm (`reach =
	// polearm`); everything else, bare hands included, whiffs on distance
	// alone. Ranged and spells ignore rank (CastSpell has no gate).
	if (member >= 2 && !(weapon && weapon->polearm)) {
		MemberMessage(attacker, loc::Tr("log.no_reach"));
		return true;
	}

	const std::string_view skillId =
		weapon ? std::string_view(weapon->skill) : std::string_view("unarmed");
	const int level = attacker.SkillLevel(skillId);
	// The attack formula (docs/combat.md part 5). The ATTACK (the executed
	// verb) supplies the damage type + its three numbers; the weapon supplies
	// the base damage/pace (unarmed knobs when bare/unstated); the associated
	// stats' average is the attack bonus; accuracy is ALWAYS DEX.
	const AttackSpec* spec = m_balance.FindAttack(verb);
	if (!spec) spec = &Balance::Neutral();
	const std::span<const std::string> stats =
		weapon && !weapon->stats.empty() ? std::span<const std::string>(weapon->stats)
										 : std::span<const std::string>(UnarmedStats());
	const float statAvg = attacker.StatAvg(stats);
	const float base =
		weapon && weapon->damage > 0.0f ? weapon->damage : m_balance.unarmedBase;
	const float pace =
		weapon && weapon->speed > 0.0f ? weapon->speed : m_balance.unarmedSpeed;

	// Exhaustion penalties (Phase 4) read the state at swing time — the swing
	// that empties the bar lands whole; the NEXT one pays.
	const bool winded = attacker.exhausted;

	// A whiffed swing pays the attack's pace too — committing to a chop
	// costs the chop. And every swing, hit or miss, is EXERTION: it spends
	// (stamina_swing + stamina_weight × weapon kg) × the attack's stam.
	float interval =
		pace *
		(m_balance.speedBase -
		 m_balance.speedStat * static_cast<float>(attacker.dexterity)) *
		spec->pace;
	if (interval < m_balance.intervalMin) interval = m_balance.intervalMin;
	if (interval > m_balance.intervalMax) interval = m_balance.intervalMax;
	if (winded) interval *= m_balance.exhaustPace; // beyond the normal cap
	attacker.handCooldown[hand] = interval;
	SpendStamina(attacker,
				 (m_balance.staminaSwing +
				  m_balance.staminaWeight * (weapon ? weapon->weight : 0.0f)) *
					 spec->stam);
	if (!target) {
		MemberMessage(attacker, loc::Tr("log.attack_air"));
		return true;
	}

	const AttackProfile atk{
		(base + m_balance.statDamage * statAvg) * spec->dmg *
			(1.0f + m_balance.skillDamage * static_cast<float>(level)) *
			(winded ? m_balance.exhaustDamage : 1.0f),
		m_balance.accBase +
			m_balance.accStat * static_cast<float>(attacker.dexterity) +
			m_balance.accSkill * static_cast<float>(level) + spec->acc,
		spec->type};
	const DefenseProfile def = MonsterDefense(*target->kind, atk.type);
	const AttackResult r = ResolveAttack(atk, def, m_balance.Strike(), m_combatRng);
	const std::string name = loc::Tr("monster." + target->kind->name);

	if (!r.hit) {
		MemberMessage(attacker, loc::Format("log.party_misses", attacker.name, name));
		return true;
	}
	target->hp -= r.damage;
	ProvokeMonster(*target); // the struck monster alone notices + turns to the party
	int dmg = static_cast<int>(r.damage + 0.5f);
	MemberMessage(attacker, loc::Format("log.party_hits", attacker.name, name, dmg));
	GrantSkillXp(attacker, skillId, 1.0f, stats); // a LANDED blow trains its class
	m_audio.Play(m_sounds.monster, 0.7f);

	if (!target->Alive()) {
		target->hp = 0.0f; // a downed monster stays in the list (so a new game /
		// save can restore it) but renders, blocks, and acts as dead.
		onMessage(loc::Format("log.monster_slain", name));
	} else {
		target->hitReq = true; // survivor flinches (a fatal blow goes straight to Die)
	}
	return true;
}

// ============================================================================
// Spell casting — a thin façade over the MagicSystem (Magic.h). This routes the
// party pose into the cast, spawns the resulting bolt into the moving-item engine
// (m_projectiles), and turns the cast outcome into HUD/audio feedback. The recipe
// lookup + mana live in the magic module; bolt flight, impacts, and sparks live in
// the shared engine (Projectiles.h) — its impact hook is ResolveSpellHit below.
// ============================================================================

bool DungeonWorld::CastSpell(size_t member, std::span<const SpellSymbol> sequence,
							 int hand) {
	if (!m_roster || member >= m_roster->size()) return false;
	Character& caster = (*m_roster)[member];
	if (!caster.IsAlive()) return false;

	// Bolt travels the party's faced cardinal direction (the grid facing, not the
	// free-look offset) — but down the CASTER'S QUADRANT lane, not the cell's
	// center line (Michael, 2026-07-10): the portraits read front-left,
	// front-right, rear-left, rear-right, so roster columns 0/2 are the
	// on-screen LEFT pair, 1/3 the right — each a quarter-cell off center.
	// (Repositioning members is a later feature.) The monster mirror already
	// holds (their bolts spawn at their sub-cell slot); a future ranged
	// weapon fires from the same lane.
	const Direction faced = static_cast<Direction>(m_party.Facing());
	const Direction lateral = static_cast<Direction>(
		(static_cast<int>(faced) + (member % 2 == 0 ? 3 : 1)) % 4);
	Vec3 origin = m_party.EyePosition();
	origin.x += static_cast<float>(DirDX(lateral)) * (kCellSize * 0.25f);
	origin.z += static_cast<float>(DirDZ(lateral)) * (kCellSize * 0.25f);
	const Vec3 dir{static_cast<float>(DirDX(faced)), 0.0f,
				   static_cast<float>(DirDZ(faced))};

	const MagicSystem::CastReport r =
		m_magic.Cast(caster, sequence, origin, dir, m_combatRng);
	switch (r.outcome) {
	case MagicSystem::CastOutcome::Cast:
		// The spell's own Cast() override has already landed the effect
		// through the cast services (Spell/Spell.h) — a bolt is flying, a
		// ward settled. What remains is the COMMON aftermath every success
		// shares, whatever the spell did.
		MemberMessage(caster, loc::Format("log.cast", caster.name,
										  loc::Tr(r.spell->NameKey())));
		// A spell is LEARNED the first time it is successfully cast — the
		// failed outcomes below (a Fumble included) teach nothing.
		if (caster.learnedSpells.insert(r.spell->Id()).second)
			MemberMessage(caster, loc::Format("log.spell_learned", caster.name,
											  loc::Tr(r.spell->NameKey())));
		// The freshest cast leads the FIRING hand's quick list (each hand keeps
		// its own repertoire); a hand-less cast (dev console) touches neither,
		// and a spellbook cast (kBookHands — member-driven, not hand-fired)
		// credits BOTH so the discovery reaches either hand's menu.
		if (hand == 0 || hand == 1) {
			caster.TouchSpellMru(static_cast<size_t>(hand), r.spell->Id());
		} else if (hand == kBookHands) {
			caster.TouchSpellMru(0, r.spell->Id());
			caster.TouchSpellMru(1, r.spell->Id());
		}
		// The school skill grows with every SUCCESSFUL cast, in proportion to
		// the spell's mana (a dearer spell teaches more) — docs/skills.md.
		GrantSkillXp(caster, SymbolId(r.spell->School()), r.spell->Mana() * 0.25f,
					 SchoolStats(r.spell->School()));
		m_audio.Play(m_sounds.spellCast, 0.7f);
		return true;
	case MagicSystem::CastOutcome::Fumble:
		// The skill roll failed: the mana is spent, nothing happens, and
		// nothing is learned — the recipe stays anonymous until a cast lands.
		MemberMessage(caster, loc::Format("log.cast_fumble", caster.name));
		m_audio.Play(m_sounds.spellFizzle, 0.6f);
		return false;
	case MagicSystem::CastOutcome::NoMana:
		MemberMessage(caster, loc::Format("log.cast_nomana", caster.name));
		return false;
	case MagicSystem::CastOutcome::Unknown:
		MemberMessage(caster, loc::Format("log.cast_unknown", caster.name));
		return false;
	case MagicSystem::CastOutcome::NoRecipe:
	default:
		onMessage(loc::Tr("log.spell_fizzles"));
		return false;
	}
}

bool DungeonWorld::CastSpellById(size_t member, std::string_view id, int hand) {
	const Spell* def = m_magic.FindSpell(id);
	if (!def) return false; // stale default / catalog typo — nothing to cast
	return CastSpell(member, def->Sequence(), hand);
}

namespace {
// The lane-hit window (Michael, 2026-07-10): the lateral distance between a
// bolt's travel line and a body's sub-cell position. 0 = the same side,
// cell/4 = a centered body (hit), ~cell/2+ = the opposite quadrant (flies
// past) — 0.35 splits hit from miss. Shared by both directions of fire.
constexpr float kLaneHalfWidth = kCellSize * 0.35f;

// Lateral offset of `pos` from a cardinal bolt's travel line.
float LaneOffset(const ProjectileImpact& impact, const Vec3& pos) {
	return std::abs(impact.dir.x) > 0.5f ? pos.z - impact.pos.z
										 : pos.x - impact.pos.x;
}
} // namespace

bool DungeonWorld::ResolveSpellHit(const ProjectileImpact& impact) {
	const int cx = static_cast<int>(std::floor(impact.pos.x / kCellSize));
	const int cz = static_cast<int>(std::floor(impact.pos.z / kCellSize));
	// A bolt flies down its LANE (the caster's quadrant line): it hits a
	// monster on the same side or in the middle of the cell, and flies
	// straight past one hugging the opposite side (Michael, 2026-07-10).
	// The window is the lateral distance between the bolt's line and the
	// monster's sub-cell slot: 0 same side, cell/4 for a centered (large)
	// body, ~cell/2+ for the opposite quadrant — 0.35 splits hit from miss.
	// (kLaneHalfWidth — shared with the party mirror below.)
	Monster* hit = nullptr;
	int hitIndex = -1;
	for (size_t i = 0; i < m_monsters.size(); ++i) {
		Monster& m = m_monsters[i];
		if (!m.Alive() || m.x != cx || m.z != cz) continue;
		const Vec3 mp = SlotCenter(m.x, m.z, m.kind->size, m.slot);
		if (std::abs(LaneOffset(impact, mp)) > kLaneHalfWidth)
			continue; // opposite side
		hit = &m;
		hitIndex = static_cast<int>(i);
		break;
	}
	if (!hit) return false; // open air (or only wrong-lane bodies) — flies on

	const DefenseProfile def = MonsterDefense(*hit->kind, impact.atk.type);
	const AttackResult r =
		ResolveAttack(impact.atk, def, m_balance.Strike(), m_combatRng);
	const std::string name = loc::Tr("monster." + hit->kind->name);
	if (r.hit) {
		hit->hp -= r.damage;
		ProvokeMonster(*hit); // a spell strike also wakes its target
		onMessage(loc::Format("log.spell_hits", name, static_cast<int>(r.damage + 0.5f)));
		m_audio.Play(m_sounds.spellImpact, 0.7f);
		if (!hit->Alive()) {
			hit->hp = 0.0f; // downed monster stays in the list (save can restore it)
			onMessage(loc::Format("log.spell_slain", name));
		} else {
			hit->hitReq = true; // survivor flinches (a fatal blow goes straight to Die)
			// Displacement (the air-school shove): a landed hit with `push` walks
			// the survivor up to that many cells along the bolt's travel, one
			// StepMonsterTo per cell so occupancy commits atomically; the first
			// blocked/occupied cell (FreeSlotInCell covers walls, closed doors,
			// packed cells, and the party's cell) stops it early. The final step
			// wins the visual glide, so the shove reads as one continuous slide.
			if (impact.push > 0) {
				const int dx = impact.dir.x > 0.5f ? 1 : (impact.dir.x < -0.5f ? -1 : 0);
				const int dz = impact.dir.z > 0.5f ? 1 : (impact.dir.z < -0.5f ? -1 : 0);
				int pushed = 0;
				for (int step = 0; step < impact.push; ++step) {
					const int nx = hit->x + dx, nz = hit->z + dz;
					const int slot = FreeSlotInCell(nx, nz, hit->kind->size, hitIndex);
					if (slot < 0) break; // wall / door / occupied — the shove stops
					StepMonsterTo(*hit, nx, nz, slot);
					++pushed;
				}
				if (pushed > 0)
					onMessage(loc::Format("log.spell_pushes", name));
			}
		}
	} else {
		onMessage(loc::Format("log.spell_misses", name));
	}
	return true; // a monster was here, so the bolt is consumed (hit or miss)
}

bool DungeonWorld::ResolveMonsterProjectileHit(const ProjectileImpact& impact) {
	if (!m_roster || m_partyWiped) return false;
	const int cx = static_cast<int>(std::floor(impact.pos.x / kCellSize));
	const int cz = static_cast<int>(std::floor(impact.pos.z / kCellSize));
	if (cx != m_party.GridX() || cz != m_party.GridZ()) return false; // not the party's cell yet

	// Reached the party: the LANE mirror of the party's shots (Michael,
	// 2026-07-10) — the bolt can only strike a standing member whose facing-
	// relative QUADRANT (portraits read front-left/front-right/rear-left/
	// rear-right) sits in its lane; with nobody in the lane it flies straight
	// past the party. A random in-lane member takes the strike. Consumed once
	// it connects with anyone, hit or miss (like a spell bolt).
	const Direction faced = static_cast<Direction>(m_party.Facing());
	const float q = kCellSize * 0.25f;
	const Vec3 center{(static_cast<float>(m_party.GridX()) + 0.5f) * kCellSize,
					  0.0f,
					  (static_cast<float>(m_party.GridZ()) + 0.5f) * kCellSize};
	std::array<size_t, 4> inLane;
	size_t n = 0;
	for (size_t i = 0; i < m_roster->size() && i < 4; ++i) {
		if (!(*m_roster)[i].IsAlive()) continue;
		const Direction lateral = static_cast<Direction>(
			(static_cast<int>(faced) + (i % 2 == 0 ? 3 : 1)) % 4);
		const float row = i < 2 ? q : -q; // front pair toward the facing
		const Vec3 mp{center.x + static_cast<float>(DirDX(faced)) * row +
						  static_cast<float>(DirDX(lateral)) * q,
					  0.0f,
					  center.z + static_cast<float>(DirDZ(faced)) * row +
						  static_cast<float>(DirDZ(lateral)) * q};
		if (std::abs(LaneOffset(impact, mp)) <= kLaneHalfWidth) inLane[n++] = i;
	}
	if (n == 0) return false; // nobody in this lane — the bolt flies past

	Character& target = (*m_roster)[inLane[m_combatRng() % n]];
	// Wind Ward: air guards by DEFLECTING — a bolt aimed at the warded member
	// is turned aside outright (no strike roll), spending one of the ward's
	// charges (its magnitude); the last deflection stills the wind. Bolts
	// aimed at unwarded neighbours fly true — the ward wraps its caster alone.
	if (StatusEffect* ward = target.FindWard(SpellSymbol::Air)) {
		ward->magnitude -= 1.0f;
		MemberMessage(target, loc::Format("log.shield_deflects", target.name));
		if (ward->magnitude <= 0.0f) {
			target.RemoveWard(SpellSymbol::Air); // spent — stills, not fade
			MemberMessage(target, loc::Format("log.shield_stills", target.name));
		}
		return true; // the bolt is spent against the wind
	}
	const DefenseProfile def = PartyDefense(target, impact.atk.type);
	const AttackResult r =
		ResolveAttack(impact.atk, def, m_balance.Strike(), m_combatRng);
	if (!r.hit) {
		MemberMessage(target, loc::Format("log.monster_ranged_misses", target.name));
		return true;
	}
	MemberMessage(target, loc::Format("log.monster_ranged_hits", target.name,
									  static_cast<int>(r.damage + 0.5f)));
	m_audio.Play(m_sounds.monster, 0.6f);
	WoundMember(target, r.damage);
	CheckPartyWipe();
	return true;
}

} // namespace dungeon::game
