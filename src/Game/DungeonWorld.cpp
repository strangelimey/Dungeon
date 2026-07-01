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
#include <cmath>
#include <format>

using namespace DirectX;

namespace dungeon::game {

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

DungeonWorld::DungeonWorld(gfx::GraphicsDevice& device, gfx::Renderer& renderer,
						   audio::AudioEngine& audio, const SoundBank& sounds,
						   const GameSettings& settings, const Project& project,
						   threads::Manager& threadManager)
	: m_device(device), m_renderer(renderer), m_audio(audio), m_sounds(sounds),
	  m_settings(settings), m_project(project),
	  m_map(project.LevelMapPath(FirstLevel(project))),
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
		// Stepping onto a stair raises a pending transition; Game polls it after
		// Update and drives the swap, so the level never changes mid-step.
		for (const StairLink& s : m_map.Stairs())
			if (s.x == px && s.z == pz) {
				m_pendingTransition =
					LevelTransition{s.destLevel, s.destX, s.destZ, s.destFacing};
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
		for (const auto& [bx, bz] : m_map.BrazierCells()) {
			if (bx == x && bz == z) {
				m_audio.Play(m_sounds.bump, 0.7f);
				onMessage(loc::Tr("log.brazier_blocks"));
				return true;
			}
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

	// Lifted from {0.035,0.032,0.045} after the albedo sRGB switch: textures now
	// decode darker, so the unlit fill needs more to keep corridors from reading
	// pitch-black. Cool tint preserved (no sun underground).
	m_lights.ambient = {0.052f, 0.048f, 0.064f};
	m_lights.directional.color = {0, 0, 0}; // no sun underground
	// Rebuilt every frame into retained capacity — no steady-state allocation.
	m_lights.points.reserve(gfx::kMaxPointLights);

	// Magic system: load the project's recipes. Casting produces a bolt spec that
	// CastSpell spawns into the shared moving-item engine below.
	m_magic.LoadSpells(m_project.spells);

	// Moving-item engine: wire its world seam so a projectile lives "on the map"
	// without the engine depending on the map/combat. resolveHit is faction-aware —
	// it dispatches by the item's target side.
	m_projectiles.isBlocked = [this](const Vec3& p) {
		const int cx = static_cast<int>(std::floor(p.x / kCellSize));
		const int cz = static_cast<int>(std::floor(p.z / kCellSize));
		return !m_map.IsWalkable(cx, cz); // wall / off-map stops the item
	};
	m_projectiles.resolveHit = [this](TargetSide side, const Vec3& p,
									  const AttackProfile& atk) {
		switch (side) {
		case TargetSide::Monsters:
			return ResolveSpellHit(p, atk); // a party spell strikes a monster
		case TargetSide::Party:
			return ResolveMonsterProjectileHit(p, atk); // a monster bolt strikes the party
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
	MarkSeen(x, z);
	RebuildChunksAround(x, z);
}

void DungeonWorld::EditVariant(int x, int z, SurfaceSel sel, int variant) {
	if (!m_map.IsWalkable(x, z)) return; // variants live on floor cells
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
	const std::string mount = CatalogGet(m_project.fixtures.Find(type), "mount", "floor");
	const bool ok = mount == "wall" ? m_map.AddSconce(x, z) : m_map.AddBrazier(x, z);
	if (!ok) return false;
	// Rebuild the fire instances + dust from the updated map (lights pick the new
	// fire up next frame; the GPU may still read the old turbidity, so drain it).
	m_device.WaitIdle();
	m_fires.clear();
	BuildFires();
	BuildTurbidityMap();
	MarkSeen(x, z);
	return true;
}

bool DungeonWorld::RemoveEntityAt(int x, int z) {
	for (auto it = m_monsters.begin(); it != m_monsters.end(); ++it)
		if (it->x == x && it->z == z) {
			m_monsters.erase(it);
			return true;
		}
	for (auto it = m_decorations.begin(); it != m_decorations.end(); ++it)
		if (it->x == x && it->z == z) {
			m_decorations.erase(it);
			return true;
		}
	return false;
}

std::vector<DungeonWorld::MapMarker> DungeonWorld::MonsterMarkers() const {
	std::vector<MapMarker> markers;
	markers.reserve(m_monsters.size());
	for (const Monster& m : m_monsters)
		if (m.Alive()) // a slain monster leaves no map marker
			markers.push_back({m.x, m.z, m.kind ? m.kind->name : std::string()});
	return markers;
}

std::vector<DungeonWorld::MapMarker> DungeonWorld::DecorationMarkers() const {
	std::vector<MapMarker> markers;
	markers.reserve(m_decorations.size());
	for (const Decoration& d : m_decorations) {
		if (d.stair) continue; // stairs draw from their own (typed) marker
		markers.push_back({d.x, d.z, d.kind ? d.kind->id : std::string()});
	}
	return markers;
}

void DungeonWorld::BeginLevelLoad(const std::string& stem, bool stashCurrent) {
	m_device.WaitIdle(); // the GPU may still be reading the old level's meshes

	// Save the level we're leaving so a later return restores its fog/progress
	// (skip for a throwaway baseline being replaced by a save's level).
	if (stashCurrent) StashActive();

	// Move-assign the new level into the existing objects (Party holds a
	// reference to m_map, so the object must persist — only its data changes).
	m_map = DungeonMap(m_project.LevelMapPath(stem));
	m_entities = DungeonEntities(m_project.LevelEntPath(stem), m_map);
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
	m_decorations.clear();
	m_fires.clear();
	m_projectiles.Clear(); // bolts/sparks don't survive a level change
	m_pendingTransition.reset();
	m_shadows.InvalidateCubes();
	ResolveSurfacePalettes();
}

std::optional<DungeonWorld::LevelTransition> DungeonWorld::ConsumeLevelTransition() {
	std::optional<LevelTransition> t = std::move(m_pendingTransition);
	m_pendingTransition.reset();
	return t;
}

namespace {
// How long a hit-feedback splat stays over a struck member's portrait.
constexpr float kHitFlashSeconds = 0.7f;

const char* DirName(Direction d) {
	switch (d) {
	case Direction::North: return "north";
	case Direction::East:  return "east";
	case Direction::West:  return "west";
	default:               return "south";
	}
}
const char* KindName(EntityKind k) {
	switch (k) {
	case EntityKind::Monster:    return "monster";
	case EntityKind::Button:     return "button";
	case EntityKind::Decoration: return "decoration";
	default:                     return "item";
	}
}
} // namespace

bool DungeonWorld::SaveLevel() const {
	const DungeonMap& map = m_map;

	// --- static layer (.map) ------------------------------------------------
	std::string m = std::format("; {} — written by the in-game editor.\n\n", m_currentLevel);
	auto palette = [&](const char* surface, const std::vector<std::string>& ids) {
		if (ids.empty()) return;
		m += std::format("palette {}", surface);
		for (const std::string& id : ids) m += " " + id;
		m += '\n';
	};
	palette("wall", map.WallPalette());
	palette("floor", map.FloorPalette());
	palette("ceiling", map.CeilingPalette());
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

	for (const WallSconce& s : map.Sconces())
		m += std::format("fixture sconce {} {} {}\n", s.x, s.z, DirName(s.wall));
	for (const auto& [bx, bz] : map.BrazierCells())
		m += std::format("fixture brazier {} {}\n", bx, bz);

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

	// Decorations reconstructed from the live instances (so editor placements /
	// removals persist); stair props are skipped — they are stairs records.
	for (const Decoration& d : m_decorations) {
		if (d.stair || !d.kind || d.kind->id.empty()) continue;
		if (d.wallMounted) {
			m += std::format("decoration {} {} {} wall={}", d.kind->id, d.x, d.z,
							 DirName(d.wall));
			if (d.solid) m += " solid=1";
		} else {
			m += std::format("decoration {} {} {} {}", d.kind->id, d.x, d.z,
							 DirName(d.facing));
			if (d.solid != d.kind->solidDefault)
				m += std::format(" solid={}", d.solid ? 1 : 0);
		}
		m += '\n';
	}

	// --- dynamic layer (.ent) -----------------------------------------------
	std::string e =
		std::format("; {} — written by the in-game editor (dynamic layer).\n\n", m_currentLevel);
	for (const Monster& mon : m_monsters)
		e += std::format("monster {} {} {} {}\n",
						 mon.kind ? mon.kind->name : std::string("?"), mon.x, mon.z,
						 DirName(mon.facing));
	// Items/buttons aren't editor-editable yet — carry the loaded records through.
	for (const Entity& ent : m_entities.All()) {
		if (ent.kind != EntityKind::Item && ent.kind != EntityKind::Button) continue;
		e += std::format("{} {} {} {} {}", KindName(ent.kind), ent.type, ent.x, ent.z,
						 DirName(ent.facing));
		for (const auto& [k, v] : ent.params) e += std::format(" {}={}", k, v);
		e += '\n';
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

void DungeonWorld::PlacePartyAt(int x, int z, Direction facing) {
	m_party.SetGridPosition(x, z);
	m_party.SetFacing(static_cast<int>(facing));
	MarkSeen(x, z);
}

void DungeonWorld::RebuildChunkRegion(int chunkX, int chunkZ) {
	const int chunksX = (m_map.Width() + kChunkCells - 1) / kChunkCells;
	const int chunkIndex = chunkZ * chunksX + chunkX;
	DungeonGeometry r = BuildDungeonRegion(m_map, m_wallBlocks, m_floorBlocks,
										   m_ceilingBlocks, chunkX, chunkZ);
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

void DungeonWorld::Update(const Input& input, float dt, float time, bool acceptInput) {
	m_time = time; // drives the rune emissive pulse in SubmitSceneGeometry
	if (acceptInput) m_party.HandleInput(input);
	m_party.Update(dt);
	if (m_pillarActive) m_pillarAnimator.Update(dt);
	UpdateMonsters(dt);
	m_projectiles.Update(dt); // fly bolts, resolve impacts/fizzles via the hooks
	UpdateLights(time);
	UpdateCamera();

	// Advance the fires and gather their particles, sorted back-to-front so
	// the alpha-blended smoke composites correctly (additive flames are
	// order-independent).
	m_particleScratch.clear();
	for (Fire& fire : m_fires) {
		fire.effect.Update(dt);
		fire.effect.AppendParticles(m_particleScratch);
	}
	// Projectiles in flight + their impact sparks render as additive billboards
	// alongside the flames (same premultiplied-additive blend).
	m_projectiles.AppendBillboards(m_particleScratch);
	// (Rune tablets glow as a whole via an additive emissive that pulses in their
	// element colour — applied to the mesh in SubmitSceneGeometry, not a billboard.)
	const Vec3 eye = m_party.EyePosition();
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

void DungeonWorld::UpdateCamera() {
	m_camera.SetPosition(m_party.EyePosition());
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
// wall torches flicker with independent phases, the pillar glows. All
// flicker is product-of-sines — cheap, deterministic, and aperiodic enough.
void DungeonWorld::UpdateLights(float time) {
	m_lights.points.clear();

	const Vec3 eye = m_party.EyePosition();
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
		light.radius = fire.brazier ? 14.4f : 7.0f;
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

	if (m_pillarActive) {
		gfx::PointLight glow;
		// Low and soft: near the base so its hotspot pools on the FLOOR around the
		// pillar (a magical ground glow) instead of burning a spotlight onto the
		// ceiling directly above, which it did when sitting high at mid-height.
		glow.position = {m_pillarPos.x, 0.7f, m_pillarPos.z};
		glow.radius = 3.6f;
		glow.color = {0.35f, 0.85f, 0.6f};
		glow.intensity = 0.7f + 0.12f * std::sin(time * 2.2f);
		// The glow casts the pillar's coil shadows from inside the mesh; that is
		// a fixture of the scene, not a fire popping in, and its short range
		// would otherwise fade the shadow out at any normal viewing distance.
		glow.fadeShadow = false;
		m_lights.points.push_back(glow);
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
	// large level the fire count alone can crowd out a light pushed late (the
	// pillar glow). Keep the ones NEAREST the eye instead of the first ones
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

	// Tick down each member's per-hand swing cooldowns so hands free up over
	// time, fade out the hit-feedback splat, and regenerate mana (scaled by
	// intelligence) so spent spell points recover between casts.
	if (m_roster)
		for (Character& member : *m_roster) {
			for (float& cd : member.handCooldown)
				if (cd > 0.0f) cd -= dt;
			if (member.hitFlash > 0.0f) member.hitFlash -= dt;
			if (member.IsAlive() && member.mana < member.maxMana) {
				member.mana += member.ManaRegenPerSec() * dt;
				if (member.mana > member.maxMana) member.mana = member.maxMana;
			}
		}

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

		if (monster.intent.mode == ai::Intent::Mode::Idle) continue; // idle: nothing to do
		if (monster.intent.mode == ai::Intent::Mode::Kite) {
			UpdateKiter(monster, static_cast<int>(i)); // skirmisher: hold range + shoot
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
		const bool atPost = monster.x == monster.targetX && monster.z == monster.targetZ &&
							orthoDist == 1;

		// Announce once, when the party is actually adjacent.
		if (!monster.announced && orthoDist <= 1) {
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
					monster.moveFrom = monster.visualPos;
					monster.x = c.x;
					monster.z = c.z;
					monster.slot = slot;
					++monster.aiCursor;
					monster.moving = true;
					monster.moveT = 0.0f;
					monster.moveCd = monster.kind->moveInterval;
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
	struct Side {
		int x, z, count;
	};
	std::vector<Side> sides;
	static constexpr int kDX[4] = {0, 0, -1, 1};
	static constexpr int kDZ[4] = {-1, 1, 0, 0};
	for (int d = 0; d < 4; ++d) {
		const int sx = px + kDX[d], sz = pz + kDZ[d];
		if (m_map.IsWalkable(sx, sz)) sides.push_back({sx, sz, 0});
	}
	if (sides.empty()) return;

	std::vector<int> idx;
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
	std::vector<bool> done(idx.size(), false);
	// Pass 1 (hysteresis): a monster already standing on a still-unclaimed side
	// HOLDS it, so a formed ring is stable frame-to-frame (no thrash / circling).
	for (size_t k = 0; k < idx.size(); ++k) {
		Monster& m = m_monsters[idx[k]];
		for (auto& s : sides)
			if (s.count == 0 && m.x == s.x && m.z == s.z) {
				m.targetX = s.x;
				m.targetZ = s.z;
				++s.count;
				done[k] = true;
				break;
			}
	}
	// Pass 2 (fill): the rest take the least-crowded side with room (empty sides
	// before any doubles up → surround), tie-broken by the side nearest the monster.
	for (size_t k = 0; k < idx.size(); ++k) {
		if (done[k]) continue;
		Monster& m = m_monsters[idx[k]];
		const int cap = SlotsPerCell(m.kind->size);
		int best = -1;
		for (int s = 0; s < static_cast<int>(sides.size()); ++s) {
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
		for (int z = 0; z < H; ++z)
			for (int x = 0; x < W; ++x)
				(*grid)[static_cast<size_t>(z) * W + x] = m_map.IsWalkable(x, z) ? 1 : 0;
		m_walkableCache = std::move(grid);
		m_walkableRev = m_map.Revision();
	}

	// Reuse a pooled snapshot that no worker (or the director) still holds — its
	// only ref is the pool's (use_count == 1). It is therefore not the published
	// snapshot and not in any worker's hands, so mutating it before we publish is
	// safe. clear() keeps the vectors'/set's capacity, so steady-state frames do
	// not allocate. The pool grows to the in-flight high-water mark (~workers+1).
	std::shared_ptr<ai::Snapshot> snap;
	for (auto& s : m_snapshotPool)
		if (s.use_count() == 1) { snap = s; break; }
	if (!snap) {
		snap = std::make_shared<ai::Snapshot>();
		m_snapshotPool.push_back(snap);
	}
	snap->blocked.clear();
	snap->occ.clear();
	snap->monsters.clear();

	snap->partyX = m_party.GridX();
	snap->partyZ = m_party.GridZ();
	snap->mapW = m_map.Width();
	snap->mapH = m_map.Height();
	snap->walkable = m_walkableCache;
	// Party cell is a hard block. Monster crowding is capacity-based: each live
	// monster bumps its cell's occupant count, tagged with the size's slots/cell so
	// a worker can tell a half-full same-size group (room) from a full or foreign one.
	snap->blocked.insert(snap->partyZ * snap->mapW + snap->partyX);
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
		snap->monsters.push_back({m.runtimeId, m.x, m.z, m.kind->aggroRange,
								  m.kind->iq, cap, f, m.aware, m.kind->facesTarget,
								  m.yaw, m.targetX, m.targetZ, m.kind->archetype});
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
void DungeonWorld::WoundMember(Character& target, float damage) {
	target.health -= damage;
	if (target.health < 0.0f) target.health = 0.0f;
	target.hitFlash = kHitFlashSeconds;
	target.hitSeverity = damage < 5.0f ? 0 : (damage < 10.0f ? 1 : 2);
	if (!target.IsAlive()) onMessage(loc::Format("log.member_down", target.name));
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

	const AttackProfile atk{monster.kind->damage, monster.kind->accuracy};
	const DefenseProfile def{target.Evasion(), target.Armor()};
	const AttackResult r = ResolveAttack(atk, def, m_combatRng);
	const std::string name = loc::Tr("monster." + monster.kind->name);

	if (!r.hit) {
		onMessage(loc::Format("log.monster_misses", name, target.name));
		return;
	}
	onMessage(loc::Format("log.monster_hits", name, target.name,
						  static_cast<int>(r.damage + 0.5f)));
	m_audio.Play(m_sounds.monster, 0.6f);
	WoundMember(target, r.damage);
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
		const int want = static_cast<int>(monster.kind->keepRange + 0.5f);
		auto score = [&](int cx, int cz) {
			const int d = std::max(std::abs(cx - px), std::abs(cz - pz));
			int s = std::abs(d - want) * 2; // primary: distance error
			// Strongly prefer a cell it can actually shoot from (on-axis + in range);
			// getting onto the party's row/column is the point of a kiter.
			const bool canFire = static_cast<float>(d) <= monster.kind->aggroRange &&
								 CellHasLineOfSight(cx, cz, px, pz);
			if (!canFire) s += 4;
			return s;
		};
		int bestScore = score(monster.x, monster.z);
		int bx = monster.x, bz = monster.z, bslot = monster.slot;
		static constexpr int kDX[4] = {1, -1, 0, 0}, kDZ[4] = {0, 0, 1, -1};
		for (int k = 0; k < 4; ++k) {
			const int nx = monster.x + kDX[k], nz = monster.z + kDZ[k];
			const int slot = FreeSlotInCell(nx, nz, monster.kind->size, selfIndex);
			if (slot < 0) continue; // unwalkable / occupied / the party cell
			const int s = score(nx, nz);
			if (s < bestScore) { bestScore = s; bx = nx; bz = nz; bslot = slot; }
		}
		if (bx != monster.x || bz != monster.z) {
			monster.moveFrom = monster.visualPos;
			monster.x = bx;
			monster.z = bz;
			monster.slot = bslot;
			monster.moving = true;
			monster.moveT = 0.0f;
			monster.moveCd = monster.kind->moveInterval;
		}
	}
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

	ProjectileSpec bolt;
	bolt.pos = origin;
	bolt.dir = dir;
	bolt.speed = 6.0f;
	bolt.range = (monster.kind->aggroRange + 1.0f) * kCellSize; // reach a bit past aggro
	bolt.atk = {monster.kind->damage, monster.kind->accuracy};
	bolt.color = {1.6f, 0.5f, 0.2f, 0.0f}; // ember-orange additive
	bolt.size = 0.18f;
	bolt.target = TargetSide::Party;
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

bool DungeonWorld::PartyAttack(size_t member, size_t hand) {
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

	attacker.handCooldown[hand] = attacker.AttackInterval(hand);
	if (!target) {
		onMessage(loc::Tr("log.attack_air"));
		return true;
	}

	const AttackProfile atk{attacker.AttackDamage(), attacker.Accuracy()};
	const DefenseProfile def{target->kind->evasion, target->kind->armor};
	const AttackResult r = ResolveAttack(atk, def, m_combatRng);
	const std::string name = loc::Tr("monster." + target->kind->name);

	if (!r.hit) {
		onMessage(loc::Format("log.party_misses", attacker.name, name));
		return true;
	}
	target->hp -= r.damage;
	ProvokeMonster(*target); // the struck monster alone notices + turns to the party
	int dmg = static_cast<int>(r.damage + 0.5f);
	onMessage(loc::Format("log.party_hits", attacker.name, name, dmg));
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

bool DungeonWorld::CastSpell(size_t member, std::span<const SpellSymbol> sequence) {
	if (!m_roster || member >= m_roster->size()) return false;
	Character& caster = (*m_roster)[member];
	if (!caster.IsAlive()) return false;

	// Bolt travels the party's faced cardinal direction (the grid facing, not the
	// free-look offset), spawned at the party eye.
	const Direction faced = static_cast<Direction>(m_party.Facing());
	const Vec3 origin = m_party.EyePosition();
	const Vec3 dir{static_cast<float>(DirDX(faced)), 0.0f,
				   static_cast<float>(DirDZ(faced))};

	const MagicSystem::CastReport r = m_magic.Cast(caster, sequence, origin, dir);
	switch (r.outcome) {
	case MagicSystem::CastOutcome::Cast:
		m_projectiles.Spawn(r.projectile); // the bolt now lives "on the map"
		onMessage(loc::Format("log.cast", caster.name, loc::Tr(r.spell->nameKey)));
		m_audio.Play(m_sounds.spellCast, 0.7f);
		return true;
	case MagicSystem::CastOutcome::NoMana:
		onMessage(loc::Format("log.cast_nomana", caster.name));
		return false;
	case MagicSystem::CastOutcome::Unknown:
		onMessage(loc::Format("log.cast_unknown", caster.name));
		return false;
	case MagicSystem::CastOutcome::NoRecipe:
	default:
		onMessage(loc::Tr("log.spell_fizzles"));
		return false;
	}
}

bool DungeonWorld::ResolveSpellHit(const Vec3& p, const AttackProfile& atk) {
	const int cx = static_cast<int>(std::floor(p.x / kCellSize));
	const int cz = static_cast<int>(std::floor(p.z / kCellSize));
	Monster* hit = nullptr;
	for (Monster& m : m_monsters)
		if (m.Alive() && m.x == cx && m.z == cz) { hit = &m; break; }
	if (!hit) return false; // open air — the bolt flies on

	const DefenseProfile def{hit->kind->evasion, hit->kind->armor};
	const AttackResult r = ResolveAttack(atk, def, m_combatRng);
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
		}
	} else {
		onMessage(loc::Format("log.spell_misses", name));
	}
	return true; // a monster was here, so the bolt is consumed (hit or miss)
}

bool DungeonWorld::ResolveMonsterProjectileHit(const Vec3& p, const AttackProfile& atk) {
	if (!m_roster || m_partyWiped) return false;
	const int cx = static_cast<int>(std::floor(p.x / kCellSize));
	const int cz = static_cast<int>(std::floor(p.z / kCellSize));
	if (cx != m_party.GridX() || cz != m_party.GridZ()) return false; // not the party's cell yet

	// Reached the party: strike a random standing member (the ranged mirror of
	// MonsterAttack). Consumed once it arrives, hit or miss (like a spell bolt).
	std::array<size_t, 4> alive;
	size_t n = 0;
	for (size_t i = 0; i < m_roster->size() && n < alive.size(); ++i)
		if ((*m_roster)[i].IsAlive()) alive[n++] = i;
	if (n == 0) return true;

	Character& target = (*m_roster)[alive[m_combatRng() % n]];
	const DefenseProfile def{target.Evasion(), target.Armor()};
	const AttackResult r = ResolveAttack(atk, def, m_combatRng);
	if (!r.hit) {
		onMessage(loc::Format("log.monster_ranged_misses", target.name));
		return true;
	}
	onMessage(loc::Format("log.monster_ranged_hits", target.name,
						  static_cast<int>(r.damage + 0.5f)));
	m_audio.Play(m_sounds.monster, 0.6f);
	WoundMember(target, r.damage);
	CheckPartyWipe();
	return true;
}

} // namespace dungeon::game
