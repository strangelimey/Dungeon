// ============================================================================
// Game/DungeonWorld.cpp — see DungeonWorld.h.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Profile.h"
#include "Game/DungeonMeshBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

using namespace DirectX;

namespace dungeon::game {

namespace {
// The unlit fill (no sun underground). Lifted from {0.035,0.032,0.045} after
// the albedo sRGB switch; the dev console's `ambient <x>` scales it live for
// mood tuning (SetAmbientScale).
constexpr Vec3 kBaseAmbient{0.052f, 0.048f, 0.064f};
// A burning body's plume burns a little bigger than a brazier. Shared by the
// plume itself and the buffer reserve that has to allow for one.
constexpr float kPlumeScale = 1.1f;
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
			if (deco.Blocks() && deco.x == x && deco.z == z) {
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

	// The damage types themselves (docs/damage-system.md) — the vocabulary
	// everything below is written in, so it is built before all of it.
	m_damageTypes.Build(m_project.damagetypes);
	if (!m_damageTypes.Find("bash", m_bashType))
		log::Warn("damagetypes.cat defines no 'bash' type; collisions (wall "
				  "bumps, pit landings) will deal '{}' instead",
				  m_damageTypes.Id(m_bashType));

	// The attack formula's tuning (docs/combat.md): knob sheet + per-attack
	// numbers from the project's balance.cat/attacks.cat (missing files keep
	// the C++ first-cut defaults). Magic reads it too (spell_stat).
	m_balance.Load(m_project.balance, m_project.attacks, m_damageTypes);

	// Magic system: build the spell registry (the Spell classes + the
	// project's spells.cat numeric overrides), and wire the CAST SERVICES —
	// the capability surface a spell's Cast() lands its effect through
	// (Spell/Spell.h), so the magic module stays walled off from the world.
	// Status effects (docs/effects.md): the kind registry — every class in
	// Game/Effect/ plus the project's effects.cat overrides. Built BEFORE the
	// cast services, which hand spells an applyEffect resolving through it.
	m_effects.Build(m_project.effects, m_damageTypes);

	m_magic.LoadSpells(m_project.spells, m_damageTypes);
	m_magic.SetBalance(&m_balance);
	m_magic.SetCastServices(
		{[this](const ProjectileSpec& bolt) { m_projectiles.Spawn(bolt); },
		 [this](const Character& member, const std::string& line) {
			 MemberMessage(member, line);
		 },
		 [this](Character& target, std::string_view id, SpellSymbol school,
				float magnitude, float duration) {
			 if (const fx::EffectKind* kind = m_effects.Find(id))
				 fx::Apply(target.effects, *kind, school, magnitude, duration);
			 else
				 log::Warn("cast wants effect '{}', which has no kind", id);
		 }});

	// Moving-item engine: wire its world seam so a projectile lives "on the map"
	// without the engine depending on the map/combat. resolveHit is faction-aware —
	// it dispatches by the item's target side.
	m_projectiles.isBlocked = [this](const Vec3& p, const Vec3& dir) {
		const int cx = static_cast<int>(std::floor(p.x / kCellSize));
		const int cz = static_cast<int>(std::floor(p.z / kCellSize));
		if (!m_map.IsWalkable(cx, cz)) {
			// A bore along the bolt's travel axis lets it fly through the wall.
			const int axis = std::abs(dir.x) >= std::abs(dir.z) ? 0 : 1;
			return !WallSeeThrough(cx, cz, axis); // solid & unbored stops it
		}
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
	m_projectiles.onExpire = [this](const ProjectileExpiry& expiry) {
		ResolveProjectileExpiry(expiry);
	};
}

// ============================================================================
// Per-frame simulation
// ============================================================================

bool DungeonWorld::IsSeen(int x, int z) const {
	if (x < 0 || z < 0 || x >= m_map.Width() || z >= m_map.Height()) return false;
	return m_seen[static_cast<size_t>(z) * m_map.Width() + x] != 0;
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
		m_map, m_wallBlocks, m_floorBlocks, m_ceilingBlocks, m_walls.uAspect,
		m_floors.uAspect, m_ceilings.uAspect, chunkX, chunkZ,
		[this](int x, int z) {
			return CellHoles{FloorHoleAt(x, z), CeilingHoleAt(x, z)};
		},
		[this](const std::string& type) { return NicheMeshFor(type); },
		[this](const std::string& type) { return BoreMeshFor(type); },
		[this](const std::string& type) { return FloorFeatureMeshFor(type); },
		[this](const std::string& type) { return CeilingFeatureMeshFor(type); });
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
	// Instrumented HERE, in the callee, rather than at the call site. Game::Update
	// reaches this from TWO places — the normal playing path and the one taken
	// while the dev console owns input — and a zone on one of them would have the
	// profile quietly omit the world's cost exactly when you are looking at the
	// console to read it. Which is what the first attempt did.
	DN_PROFILE_ZONE_L(prof::kLevelSystem, "world");

	m_time = time; // drives the rune emissive pulse in SubmitSceneGeometry

	// THE ONE-PIPELINE CHECK (Game/DamageLedger.h), four checkpoints a frame.
	// A violation is found at a checkpoint, long after the stack that caused it
	// has gone, so the boundaries are placed where the phases genuinely divide:
	// the phase name plus the victim is what stands in for a stack.
	//
	// This first one covers the gap OUTSIDE the world update — a hand-slot click
	// resolving a swing, a dev command, an editor edit — which is where a party
	// attack actually lands, since Game::Update drives it rather than this loop.
	CheckDamageLedger("outside the world update");
	// Landed from a pit on the level just below: the shaft's own blow, charged
	// at the TOP of the first frame after the arrival — the flag is raised at
	// the bottom of the frame that raised the transition, and the host clears
	// the message log between the two, so this is where the line survives.
	if (m_fellPending) {
		m_fellPending = false;
		OnFallImpact();
	}
	if (acceptInput && !m_pendingFall) m_party.HandleInput(input);
	m_party.Update(dt);

	// Door leaves travel toward their open/shut target. The DURATION is per type
	// (doors.cat `open_seconds`), because a stone slab that grinds and a wooden
	// door that swings are the same motion at different speeds; the render
	// decides the shape of the movement, this only decides how long it takes.
	for (Door& door : m_doors) {
		const float target = door.open ? 1.0f : 0.0f;
		const float step = dt / door.openSeconds;
		if (door.openT < target)
			door.openT = std::min(target, door.openT + step);
		else if (door.openT > target)
			door.openT = std::max(target, door.openT - step);
		// The opener's throw and its recovery, at two speeds — down fast, back
		// slow. Both are animated: the first cut stepped straight to full travel
		// on the click and it read as a glitch rather than as a pull, because
		// nothing the eye can follow happened on the way down.
		if (door.pullRising) {
			door.pullT += dt / kPullDownSeconds;
			if (door.pullT >= 1.0f) {
				door.pullT = 1.0f;
				door.pullRising = false;
			}
		} else if (door.pullT > 0.0f) {
			door.pullT = std::max(0.0f, door.pullT - dt / kPullSeconds);
		}
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
			// The landing IS a collision (docs/effects.md) — but it is charged
			// on the far side of the swap, not here: the host CLEARS the message
			// log as it places the party on the new level, so a bruise reported
			// now would be wiped before anyone read it. Latched, and spent on
			// the first frame after they arrive.
			m_fellPending = true;
		}
	}
	// Everything the party's own movement can cost them: a wall bump, a pit
	// landing, and the exertion of the steps themselves.
	CheckDamageLedger("party movement");
	UpdateMonsters(dt);
	// The busiest phase by far — monster blows, every DoT bite on both sides,
	// regeneration, supplies, and the unconscious waking up.
	CheckDamageLedger("monsters, effects and regeneration");
	m_projectiles.Update(dt); // fly bolts, resolve impacts/fizzles via the hooks
	UpdateBlasts(dt);         // advance live blasts a tick at their own speed
	CheckDamageLedger("projectiles and blasts");
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
	// A burning monster carries its own plume, DERIVED from its effects: an
	// effect whose kind burns (effects.cat `plume`) gets one, and losing that
	// effect puts it out. It also has to FOLLOW the body — the origin is
	// re-aimed every frame, which no fixture's flame ever needs.
	for (Monster& monster : m_monsters) {
		const fx::Inst* burning = PlumeEffect(monster);
		if (!burning) {
			monster.plume.reset();
			continue;
		}
		if (!monster.plume)
			monster.plume = std::make_unique<FireEffect>(
				BurnOrigin(monster), kPlumeScale, monster.runtimeId * 2654435761u);
		monster.plume->SetTint(BurnTint(burning->school));
		monster.plume->SetOrigin(BurnOrigin(monster));
		monster.plume->Update(dt);
		monster.plume->AppendParticles(m_particleScratch);
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

// Sizes m_particleScratch to the busiest frame this level can produce, so no
// frame ever has to grow it. Called whenever the fire set changes — BuildFires
// is the one funnel (level load, and the editor's RebuildFiresAndDust).
void DungeonWorld::ReserveParticleScratch() {
	// The fires are known and steady, so they are counted. The other two sources
	// are transient — a plume exists only while its monster is alight, a bolt
	// only while it flies — so both are provisioned for the WORST case rather
	// than the current one: every monster on fire at once, and a busy fight's
	// bolts and impact bursts. At 32 bytes an instance that insurance is cheap,
	// and what it buys is not growing in the middle of the fight that needs it.
	size_t peak = 0;
	for (const Fire& fire : m_fires)
		peak += static_cast<size_t>(fire.effect.SteadyCount());
	peak += m_monsters.size() *
			static_cast<size_t>(FireEffect::SteadyCountFor(kPlumeScale));
	peak += 128; // projectiles in flight + their impact sparks (bursts of 6..14)
	// Headroom over the mean: spawn times and lifetimes are both random, so the
	// live count wanders above the settled figure SteadyCount reports.
	m_particleScratch.reserve(peak + peak / 4);
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

// See the header for why the cooldowns move with the latch, and why only when
// it was actually set.
void DungeonWorld::ClearWipeLatch() {
	if (!m_partyWiped) return; // a mid-fight heal: leave the encounter alone
	m_partyWiped = false;
	for (Monster& m : m_monsters)
		if (m.Alive() && m.kind) m.attackCd = m.kind->attackInterval;
}

std::vector<std::string> DungeonWorld::MonsterList() const {
	std::vector<std::string> out;
	out.reserve(m_monsters.size());
	for (const Monster& m : m_monsters) {
		// Cell, HEALTH and the EFFECTS it is carrying. The last two were added
		// because the list used to say only what a monster was and where, which
		// cannot answer the question the console is usually open to answer — did
		// that blow land, and did it leave anything behind. An effect's remaining
		// seconds is the part that says it is still running.
		// ONE DECIMAL, not none. These monsters ARE the blast suite's instrument
		// — a row of fixed-hp probes whose remaining health reads the falloff off
		// directly — and spells.cat states the numbers being checked to one
		// decimal ("3.5 around, 17.5 at the feet"). Truncating to whole points
		// made a nine-probe table carry up to +/-9 of accumulated rounding
		// against a `dealt` of 71.2, which is 12% of the measurement
		// (docs/eval-audit.md F20): one-decimal claims read with integer
		// instruments.
		std::string line = std::format("{} @ {},{}  hp {:.1f}",
									   m.kind ? m.kind->name : "?", m.x, m.z, m.hp);
		if (!m.Alive()) line += " (dead)";
		for (const fx::Inst& e : m.effects)
			line += std::format("  [{} {:.1f} {:.1f}s]", e.Id(), e.magnitude,
								e.timeLeft);
		out.push_back(std::move(line));
	}
	return out;
}

bool DungeonWorld::ToggleButtonAt(int x, int z, bool& out) {
	for (Button& b : m_buttons)
		if (b.x == x && b.z == z) {
			b.activated = !b.activated;
			out = b.activated;
			// The target wiring: toggle the doors AND niches it names.
			ToggleDoorsNamed(b.target);
			ToggleNichesNamed(b.target);
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
namespace {
// Party facing -> front-cell delta (N E S W; matches Party's kDirX/kDirZ).
constexpr int kFrontDX[4] = {0, 1, 0, -1};
constexpr int kFrontDZ[4] = {-1, 0, 1, 0};

// The school flavouring the active see-through peek, if any LIVE member holds a
// Sight effect. Fire wins when several schools are up (its lighting drives the
// reveal); the party shares one camera, so one school flavours the single ghost.
bool ActiveSightSchool(const std::vector<Character>* roster, SpellSymbol& out) {
	if (!roster) return false;
	bool found = false;
	for (const Character& c : *roster) {
		if (!c.IsAlive()) continue;
		for (const fx::Inst& e : c.effects) {
			if (!e.Is("sight")) continue;
			if (!found || e.school == SpellSymbol::Fire) out = e.school;
			found = true;
		}
	}
	return found;
}
} // namespace

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

	// A burning monster is a moving lamp: its own flickering glow in the
	// element's colour, so a torched skeleton lights the room it runs through.
	// Shadowless (like the rune glows) — a transient light must not steal a
	// shadow cube from the torch and the fires.
	for (const Monster& monster : m_monsters) {
		const fx::Inst* burning = PlumeEffect(monster);
		if (!burning) continue;
		const Vec3 o = BurnOrigin(monster);
		gfx::PointLight glow;
		glow.position = {o.x, o.y + 0.1f, o.z};
		glow.radius = 5.5f;
		const Vec4& c = ElementColor(burning->school);
		glow.color = {c.x, c.y, c.z};
		glow.intensity = 1.9f * (0.85f + 0.15f * std::sin(time * 12.0f +
														  monster.runtimeId) *
											 std::sin(time * 8.1f + monster.runtimeId));
		glow.castsShadow = false;
		m_lights.points.push_back(glow);
	}

	// Each uncollected rune throws a soft pulsing light in its element colour,
	// breathing in lockstep with the tablet's emissive glow (same RunePulse).
	// An ENCHANTED weapon lying on the floor does the same in its own element —
	// the tell that this blade is the fiery one. Pure fill light —
	// castsShadow=false keeps the cluster near the start from stealing the few
	// shadow cubes from the torch/fires.
	for (const Item& item : m_items) {
		if (item.collected || !(item.kind->isRune || item.kind->enchanted)) continue;
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

	// See-through peek (the Sight spell): recompute the ghosted wall cell from
	// the active Sight effects. Only a SOLID cell directly ahead ghosts (an open
	// cell is a no-op — you already see it). A fire-school peek (Ember Sight)
	// also drops a warm FILL light in the first open cell past the wall so the
	// revealed room — and any creature in it — shows through the ghost; pushed
	// before the budget cull so its near position keeps it (it's adjacent).
	m_sightCell = {0.0f, 0.0f, 0.0f, 0.0f};
	m_sightTint = {0.0f, 0.0f, 0.0f, 0.0f};
	m_sightHole = {0.0f, 0.0f, 0.0f, 0.0f};
	if (SpellSymbol sightSchool; ActiveSightSchool(m_roster, sightSchool)) {
		const int f = m_party.Facing() & 3;
		const int gx = m_party.GridX(), gz = m_party.GridZ();
		if (!m_map.IsWalkable(gx + kFrontDX[f], gz + kFrontDZ[f])) {
			// One round hole through the wall ahead, flavoured by the school:
			//  Air   — bores the tunnel DEEP down the row (many walls: "far sight");
			//  Water — a WIDER, clearer scrying window (hidden-content reveal is future);
			//  Earth — permanently MAPS the room it reveals (and lasts longest);
			//  Fire  — lights the revealed room so its contents show in the dark.
			const int depth = (sightSchool == SpellSymbol::Air) ? 6 : 1;
			const float radius = (sightSchool == SpellSymbol::Water) ? 0.72f : 0.55f;
			const int ax = gx + kFrontDX[f], az = gz + kFrontDZ[f];             // front cell
			const int bx = gx + kFrontDX[f] * depth, bz = gz + kFrontDZ[f] * depth; // far end
			m_sightCell = {std::min(ax, bx) * kCellSize, std::min(az, bz) * kCellSize,
						   (std::max(ax, bx) + 1) * kCellSize,
						   (std::max(az, bz) + 1) * kCellSize};
			const Vec4 c = ElementColor(sightSchool);
			m_sightTint = {c.x, c.y, c.z, 0.5f};
			// Round hole centred on the face at eye height. N/S facing (f 0/2)
			// faces a wall whose across-axis is world X; E/W faces one across Z.
			const bool acrossIsX = (f == 0 || f == 2);
			m_sightHole = {eye.y, radius, acrossIsX ? 1.0f : 0.0f, 0.0f};
			// The first OPEN cell past the wall — the room the hole reveals.
			int rvx = 0, rvz = 0;
			bool revealed = false;
			for (int i = 2; i <= depth + 3; ++i) {
				const int cx = gx + kFrontDX[f] * i, cz = gz + kFrontDZ[f] * i;
				if (m_map.IsWalkable(cx, cz)) { rvx = cx; rvz = cz; revealed = true; break; }
			}
			if (revealed && sightSchool == SpellSymbol::Fire) {
				gfx::PointLight lp;
				lp.position = m_map.CellCenter(rvx, rvz, 1.2f);
				lp.radius = kCellSize * 2.2f;
				lp.color = {1.0f, 0.55f, 0.25f}; // warm ember
				lp.intensity = 2.0f;
				lp.castsShadow = false; // fill only — no cube stolen
				m_lights.points.push_back(lp);
			}
			if (revealed && sightSchool == SpellSymbol::Earth)
				MarkSeen(rvx, rvz); // the surveyor remembers the room it read
		}
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
	// The resource knobs, gathered ONCE for the whole roster rather than per
	// member per pool: they are the same for everyone and assembling them is
	// pure arithmetic over the balance sheet, but a steady-state frame is not
	// the place to do it four times over.
	const resource::PoolRules pools = m_balance.Resources();
	const CurveRules statCurve = m_balance.StatCurve();
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
						// The other end of the one-pipeline rule (docs/effects.md
						// named this exception when the invariant was first swept
						// by hand): coming round is not damage arriving, it is the
						// one place health goes UP for a reason no effect owns.
						const ledger::Explained accounted{
							m_damageLedger, member.health,
							ledger::Reason::Stabilize};
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
			// --- REGENERATION (docs/health-and-healing.md) --------------------
			// ONE model for all three pools: a rate built from the member's
			// APTITUDE and PRACTICE (Character::RegenPerSec), gated by what
			// they are doing. The stamina holdoff after any spend already IS
			// the "exerting" signal, so the gate needed no new state:
			//
			//   exerting  stamina 0 · mana x mana_exert · health 0
			//   idle      all three at full flow
			//
			// Resting is deliberately not a third row — it is a TIME
			// multiplier, so it feeds these same rates more seconds rather
			// than different numbers, and cannot drift out of step with them.
			//
			// The whole block is skipped for a DOWNED member: recovery from 0
			// is the stabilize clock above, which is a different rule.
			const bool exerting = member.staminaHoldoff > 0.0f;
			if (member.IsAlive() && exerting) member.staminaHoldoff -= dt;
			if (member.IsAlive()) {
				// Returns the points actually restored — which is what the
				// practice trains on, and is zero at a full pool. That is the
				// property that makes constitution unfarmable: standing still
				// at full health regains nothing, so it teaches nothing.
				const auto regen = [&](resource::Kind kind, float& value,
									   float max, float scale) {
					if (scale <= 0.0f || value >= max) return 0.0f;
					// THE WRITE A SOURCE SCAN CANNOT SEE, and the reason the
					// one-pipeline check is a runtime one (Game/DamageLedger.h):
					// `value` aliases health here, so the identifier never
					// appears on the assignment. The ledger keys on the ADDRESS,
					// so the alias costs it nothing — and the mana and stamina
					// calls, whose pools are not watched, credit nothing.
					const ledger::Explained accounted{m_damageLedger, value,
													  ledger::Reason::Regen};
					const float gained = std::min(
						max - value,
						member.RegenPerSec(kind, pools.For(kind), statCurve) *
							scale * dt);
					value += gained;
					return gained;
				};
				regen(resource::Kind::Mana, member.mana, member.maxMana,
					  exerting ? m_balance.manaExert : 1.0f);
				// HEALTH is the only pool whose RECOVERY trains its practice
				// (the other two train on being SPENT), so this is the one
				// regen whose return value is used.
				GrantResourceXp(member, resource::Kind::Health,
								regen(resource::Kind::Health, member.health,
									  member.maxHealth, exerting ? 0.0f : 1.0f));
				// Stamina, and the exhausted latch clearing with hysteresis
				// (past the exhaust_recover fraction, so it can't flicker at
				// zero) — docs/combat.md Phase 4.
				if (!exerting) {
					regen(resource::Kind::Stamina, member.stamina,
						  member.maxStamina, 1.0f);
					if (member.exhausted &&
						member.stamina >=
							m_balance.exhaustRecover * member.maxStamina) {
						member.exhausted = false;
						MemberMessage(member,
									  loc::Format("log.recovered", member.name));
					}
				}
			}
			// Food and water (docs/health-and-healing.md). BEFORE the effect
			// tick, deliberately: an emptied meter raises its starving/parched
			// effect and that effect must bite on the SAME frame, or the first
			// moment of deprivation is silently free. It also means an eaten
			// apple lifts the effect before it can bite again.
			TickSupplies(member, dt);
			// Age the effects and let their DoTs bite (the shared TickEffects —
			// the monster loop below runs the very same call). An expired one
			// leaves with its category's fade line; spend-to-die wards — the
			// water pool, the air charges — are erased where they spend
			// themselves instead, so their burst/still lines replace the fade.
			//
			// A DoT ticks a DOWNED member too, and a wound on someone already
			// at 0 is death by the overkill rule (Phase 5): poison finishes
			// the fallen, so get them clear of the fight.
			PartyTarget bitten{*this, member};
			TickEffects(bitten, member.effects, dt, [&](const fx::Inst& e) {
				switch (e.kind->Kind()) {
				case fx::Category::Ward:
					MemberMessage(member, loc::Format("log.shield_fades", member.name));
					break;
				case fx::Category::Marker:
					MemberMessage(member, loc::Format("log.sight_fades", member.name));
					break;
				case fx::Category::Dot:
					MemberMessage(member,
								  loc::Format("log.effect_fades", member.name,
											  loc::Tr(e.NameKey())));
					break;
				}
			});
			bitten.NarrateFall(); // the one line a tick does say
		}
	// A DoT tick can down (or finish) the last standing member — the wipe
	// latch must notice without a monster swinging.
	if (m_roster) CheckPartyWipe();
	// The two ways rest ends by itself (deprivation, or nothing left to gain).
	// After the supply and regen ticks, so it judges this frame's state rather
	// than the last one's.
	UpdateRest();

	// Re-derive groups from current co-location (monsters sharing a cell are one
	// group — merge/split as they converge/spread), then assign formation targets
	// (surround), publish the world for the worker threads, and adopt their plans.
	// All cheap main-thread work — the pathfinding itself runs on the bucket threads.
	// The eval tally's clock, and the party's own swings. Both ride the monster
	// cadence because that is where a fight happens, and both are no-ops in an
	// ordinary play session.
	m_harness.tally.seconds += dt;
	// Feed the queued walk one step at a time, as each tween finishes — see
	// Harness::pendingSteps for why a loop cannot do this. A step the map
	// refuses (a wall, a monster in the way) still consumes one from the queue,
	// so `forward 20` down a six-cell corridor stops at the end instead of
	// shoving forever.
	if (m_harness.pendingSteps > 0 && !m_party.IsMoving()) {
		--m_harness.pendingSteps;
		m_party.Act(MoveAction::Forward);
	}
	TickAutoAttack();

	ReconcileGroups();
	AssignFormation();
	BuildAISnapshot();
	// Between the publish and the adopt, because that is exactly where the
	// worker threads would have done their thinking. A no-op unless lockstep is
	// on, in which case the workers are paused and this is the only thinking
	// that happens.
	TickLockstepAI(dt);
	ConsumeAIPlans();

	for (size_t i = 0; i < m_monsters.size(); ++i) {
		Monster& monster = m_monsters[i];
		DriveMonsterAnim(monster, dt); // animates the living AND the dying (death clip)
		// A monster's effects age and bite exactly like a member's — the same
		// TickEffects, differing only in what an expiry is called. Before the
		// Alive check, so a DoT's own killing blow takes the ordinary downed
		// path this same frame.
		if (!monster.effects.empty()) {
			const bool wasUp = monster.Alive();
			// Whether it was ALIGHT decides how its death reads — burning away
			// to nothing, or simply slain by whatever else was eating at it.
			const bool wasBurning = PlumeEffect(monster) != nullptr;
			const std::string name = loc::Tr("monster." + monster.kind->name);
			MonsterTarget afflicted{*this, monster};
			TickEffects(afflicted, monster.effects, dt, [&](const fx::Inst& e) {
				onMessage(e.Is("burn")
							  ? loc::Format("log.monster_burns_out", name)
							  : loc::Format("log.effect_fades", name,
											loc::Tr(e.NameKey())));
			});
			// A DoT that finished it says so — the counterpart of the "slain"
			// a blow's caller would have printed. (The corpse was stripped of
			// its effects by the apply stage on the way through.)
			if (wasUp && !monster.Alive())
				onMessage(loc::Format(
					wasBurning ? "log.monster_burns_away" : "log.monster_slain",
					name));
		}
		if (!monster.Alive()) continue; // downed — no AI, no movement, not solid
		if (monster.attackCd > 0.0f) monster.attackCd -= dt;
		if (monster.moveCd > 0.0f) monster.moveCd -= dt;
		// Threat drains toward zero — grudges fade back to the uniform-random
		// pick between fights. Decay never raises a score, so the silent
		// re-evaluation can only RELEASE a lock, never announce a new one.
		if (monster.ThreatAny()) {
			const float drain =
				m_balance.threatDecay * monster.kind->threatTuning.decay * dt;
			for (float& t : monster.threat) t = std::max(0.0f, t - drain);
			UpdateThreatLock(monster, false);
		}

		// Advance an in-flight glide; the logical cell already moved when the
		// step committed, so the tween just slides visualPos to the new anchor
		// (cell centre for a lone idle wanderer, else the slot centre).
		if (monster.moving) {
			monster.moveT += dt / std::max(monster.kind->moveInterval, 0.05f);
			const Vec3 target = MonsterStepTarget(monster);
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
				// A grudge-holding monster crowds toward its THREAT target's
				// quadrant instead of the cell centre — a sub-cell shooter
				// lines its lane up with them, a swarm piles onto their side.
				const int aimAt = ThreatTarget(monster);
				const Vec3 aimPt =
					aimAt >= 0 ? PartyMemberSubPos(static_cast<size_t>(aimAt))
							   : partyPos;
				auto slotDistSq = [&](int s) {
					const Vec3 c = SlotCenter(monster.x, monster.z, monster.kind->size, s);
					const float dx = aimPt.x - c.x, dz = aimPt.z - c.z;
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
				const Vec3 dest = MonsterStepTarget(monster);
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

		// FROZEN (the eval harness's `freeze`): the monster still animates,
		// still burns, still takes a blast — it simply does not ACT. A geometry
		// probe needs its instruments to hold still: the first blast suite had
		// two of its nine warriors walk out of the squares they were measuring
		// and then maul the party, so the table described where they ended up
		// rather than what the blast did to where they were.
		if (m_harness.frozen) continue;

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
// Returns a REFERENCE into the kind's table (which outlives every call) rather
// than a copy: clip names run past the small-string buffer, so returning by
// value put a heap allocation in the per-frame monster update every time a
// state changed (found by the steady-state allocation guard).
const std::string& DungeonWorld::PickClip(const MonsterKind& kind,
										  anim::CreatureState state) {
	static const std::string kNoClip;
	const auto& cands = kind.animClips[static_cast<int>(state)];
	if (cands.empty()) return kNoClip;
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
		const std::string& clip = PickClip(*monster.kind, want);
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

bool DungeonWorld::MonsterThreatById(u32 runtimeId, std::array<float, 4>& threat,
									 int& lock) const {
	for (const Monster& m : m_monsters)
		if (m.runtimeId == runtimeId) {
			threat = m.threat;
			lock = m.threatLock;
			return true;
		}
	return false;
}

std::vector<std::string> DungeonWorld::ThreatReport() const {
	std::vector<std::string> out;
	for (const Monster& m : m_monsters) {
		if (!m.kind || !m.ThreatAny()) continue;
		const std::string lock =
			m.threatLock >= 0 && m_roster &&
					static_cast<size_t>(m.threatLock) < m_roster->size()
				? (*m_roster)[m.threatLock].name
				: std::string("-");
		out.push_back(std::format("{}#{} [{:.1f} {:.1f} {:.1f} {:.1f}] lock={}",
								  m.kind->name, m.runtimeId, m.threat[0],
								  m.threat[1], m.threat[2], m.threat[3], lock));
	}
	return out;
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
		if (deco.Blocks() && deco.x == cx && deco.z == cz) return true;
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
		XMStoreFloat4x4(&d.world, UnitScale(d.kind->modelScale) * XMMatrixRotationY(DirYaw(facing)) *
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
		   !ProjectilesAt(cx, cz).empty() || !NicheFacesAt(cx, cz).empty();
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
	return MonsterStepTarget(m);
}

Vec3 DungeonWorld::MonsterStepTarget(const Monster& m) const {
	// A lone wanderer owns its whole square — walk its centre, not the slot
	// corner its size class would grid it to. Engaging (or grouped, or
	// cell-filling) monsters keep the slot so formation/occupancy reads true.
	if (m.intent.mode == ai::Intent::Mode::Idle && IsSubCellSize(m.kind->size) &&
		AliveInGroup(m.groupId) <= 1)
		return m_map.CellCenter(m.x, m.z);
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
		if (deco.Blocks())
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
void DungeonWorld::TickLockstepAI(float dt) {
	if (!m_director.Lockstep()) return;
	for (int b = 0; b < ai::Scheduler::kBucketCount; ++b) {
		const float interval = ai::Scheduler::BucketInterval(b);
		if (interval <= 0.0f) continue;
		m_bucketClock[b] += dt;
		if (m_bucketClock[b] < interval) continue;
		// ONE think per bucket per frame, with the remainder CARRIED rather than
		// dropped, so the long-run rate is exactly the bucket's cadence.
		//
		// Deliberately not a catch-up loop. Thinking twice against one frame's
		// world would produce two identical plans, because a monster does not
		// MOVE until its executor runs later in this same update — so the second
		// pass would reason from the positions the first one did. The rate is
		// kept honest instead by `step` feeding small fixed dt (see
		// Game::StepWorld): a bucket owed forty thinks gets them across forty
		// steps, which is also the only way the movement and attack cooldowns
		// those thinks feed can pace correctly.
		m_bucketClock[b] -= interval;
		m_director.ComputeInline(b);
	}
}

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

void DungeonWorld::AddThreat(Monster& monster, size_t member, float damage) {
	if (member >= monster.threat.size() || damage <= 0.0f) return;
	monster.threat[member] +=
		damage * m_balance.threatScale * monster.kind->threatTuning.scale;
	UpdateThreatLock(monster, true);
}

void DungeonWorld::UpdateThreatLock(Monster& monster, bool announce) {
	if (!m_roster) return;
	const float threshold =
		m_balance.threatThreshold * monster.kind->threatTuning.threshold;
	const int prev = monster.threatLock;
	const bool held = prev >= 0 &&
					  static_cast<size_t>(prev) < m_roster->size() &&
					  (*m_roster)[prev].IsAlive() &&
					  monster.threat[prev] >= threshold;

	// The alive argmax at/above the threshold — the strongest claim right now.
	int top = -1;
	for (size_t i = 0; i < m_roster->size() && i < monster.threat.size(); ++i) {
		if (!(*m_roster)[i].IsAlive() || monster.threat[i] < threshold) continue;
		if (top < 0 || monster.threat[i] > monster.threat[top])
			top = static_cast<int>(i);
	}

	// A held lock is STICKY: the challenger must exceed it by the switch margin
	// (two even attackers must not trade aggro every hit). An unheld lock just
	// takes the argmax (or stays released).
	const float switchMargin =
		m_balance.threatSwitch * monster.kind->threatTuning.switchMargin;
	int next = top;
	if (held && !(top >= 0 && top != prev &&
				  monster.threat[top] > monster.threat[prev] + switchMargin))
		next = prev;

	monster.threatLock = next;
	if (announce && next >= 0 && next != prev)
		MemberMessage((*m_roster)[next],
					  loc::Format("log.monster_locks",
								  loc::Tr("monster." + monster.kind->name),
								  (*m_roster)[next].name));
}

int DungeonWorld::ThreatTarget(const Monster& monster) const {
	if (!m_roster) return -1;
	const float threshold =
		m_balance.threatThreshold * monster.kind->threatTuning.threshold;
	// The locked member keeps the monster's attention while they stand...
	if (monster.threatLock >= 0 &&
		static_cast<size_t>(monster.threatLock) < m_roster->size() &&
		(*m_roster)[monster.threatLock].IsAlive() &&
		monster.threat[monster.threatLock] >= threshold)
		return monster.threatLock;
	// ...else the alive argmax at/above threshold (a downed lock's aggro passes
	// to the next-worst offender without touching the stored lock).
	int best = -1;
	for (size_t i = 0; i < m_roster->size() && i < monster.threat.size(); ++i) {
		if (!(*m_roster)[i].IsAlive() || monster.threat[i] < threshold) continue;
		if (best < 0 || monster.threat[i] > monster.threat[best])
			best = static_cast<int>(i);
	}
	return best;
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
} // namespace dungeon::game
