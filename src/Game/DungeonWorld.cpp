// ============================================================================
// Game/DungeonWorld.cpp — see DungeonWorld.h.
// ============================================================================
#include "Game/DungeonWorld.h"

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
DungeonWorld::DungeonWorld(gfx::GraphicsDevice& device, gfx::Renderer& renderer,
						   audio::AudioEngine& audio, const SoundBank& sounds,
						   const GameSettings& settings)
	: m_device(device), m_renderer(renderer), m_audio(audio), m_sounds(sounds),
	  m_settings(settings), m_map(paths::Asset("maps\\level1.map")),
	  m_entities(paths::Asset("maps\\level1.ent"), m_map),
	  m_party(m_map, m_map.StartX(), m_map.StartZ()) {
	// Party event hooks (survive Party::Reset). Feedback goes out through the
	// shared sound bank and the onMessage log line.
	m_party.onStep = [this] {
		m_audio.Play(m_sounds.footstep, 0.8f);
		MarkSeen(m_party.GridX(), m_party.GridZ());
	};
	m_party.onBlocked = [this] {
		m_audio.Play(m_sounds.bump, 0.9f);
		onMessage(loc::Tr("log.bump"));
	};
	m_party.onTurn = [this] { m_audio.Play(m_sounds.turn, 0.6f); };
	m_party.isOccupied = [this](int x, int z) {
		for (const Monster& monster : m_monsters) {
			if (monster.x == x && monster.z == z) {
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
	m_shadowCandidates.reserve(gfx::kMaxPointLights);
}

// ============================================================================
// Staged loading — one queued task per frame (see LoadQueue).
// ============================================================================

// The three surface texture sets and the height scales their parallax uses —
// the single source of those constants, shared by the staged loader and the
// quality hot-swap (LoadAllSurfaceTextures).
std::array<DungeonWorld::SurfaceDef, 3> DungeonWorld::SurfaceDefs() {
	return {{{m_walls, m_map.WallTextures(), 0.055f},
			 {m_floors, m_map.FloorTextures(), 0.045f},
			 {m_ceilings, m_map.CeilingTextures(), 0.035f}}};
}

void DungeonWorld::AppendLoadTasks(LoadQueue& queue) {
	queue.Add(loc::Tr("load.blocks"), [this] { LoadDungeonBlocks(); });

	// One task per material (the scanned sets dominate the load); the first
	// material of each set resets the surface, exactly as LoadTextureSet does.
	for (const SurfaceDef& def : SurfaceDefs()) {
		Surface& surface = def.surface;
		const float heightScale = def.heightScale;
		for (size_t i = 0; i < def.names.size(); ++i) {
			const std::string& name = def.names[i];
			const bool first = i == 0; // first material resets the set
			std::string spaced = name; // asset id, shown with the '_'s opened up
			std::ranges::replace(spaced, '_', ' ');
			queue.Add(loc::Format("load.surface", spaced),
					  [this, &surface, name, heightScale, first] {
						  if (first) {
							  surface.albedo.clear();
							  surface.normal.clear();
							  surface.mr.clear();
							  surface.heightScale = heightScale;
						  }
						  LoadSurfaceMaterial(surface, name);
					  });
		}
	}

	queue.Add(loc::Tr("load.dungeon"), [this] { BuildDungeonMeshes(); });
	queue.Add(loc::Tr("load.pillar"), [this] {
		m_pillarModel = LoadModelOrDie("pillar.gltf");
		m_pillarMesh = std::make_unique<gfx::Mesh>(m_device, m_pillarModel.meshes[0]);
		m_pillarAnimator = anim::Animator(&m_pillarModel.skeleton, &m_pillarModel.clips);
		m_pillarAnimator.Play("sway");
		m_pillarPos = m_map.CellCenter(m_map.StartX(), m_map.StartZ() + 2);
	});
	queue.Add(loc::Tr("load.monsters"), [this] { LoadMonsters(); });
	queue.Add(loc::Tr("load.decorations"), [this] { LoadDecorations(); });
	queue.Add(loc::Tr("load.fires"), [this] {
		auto sconce = LoadModelOrDie("sconce.gltf");
		m_sconceMesh = std::make_unique<gfx::Mesh>(m_device, sconce.meshes[0]);
		m_sconceColor = sconce.materials[0].baseColorFactor;
		auto brazier = LoadModelOrDie("brazier.gltf");
		m_brazierMesh = std::make_unique<gfx::Mesh>(m_device, brazier.meshes[0]);
		m_brazierColor = brazier.materials[0].baseColorFactor;
		m_particleBatch = std::make_unique<gfx::ParticleBatch>(m_device);
		BuildFires();
	});
	queue.Add(loc::Tr("load.dust"), [this] { BuildTurbidityMap(); });
}

void DungeonWorld::LoadDungeonBlocks() {
	// The old dungeon uses the worn, crumbling block set — one mesh per
	// texture variant, displaced at bake time by that texture's height map
	// so geometry relief matches the painted bricks/slabs. The clean
	// *_block.gltf models remain baked for newer areas of the game.
	auto load = [&](std::vector<assets::MeshData>& blocks,
					std::span<const std::string> names) {
		blocks.clear();
		for (const std::string& name : names)
			blocks.push_back(
				LoadModelOrDie(
					std::format("worn_{}_{}.gltf", name, m_settings.MeshSuffix()))
					.meshes[0]);
	};
	load(m_wallBlocks, m_map.WallTextures());
	load(m_floorBlocks, m_map.FloorTextures());
	load(m_ceilingBlocks, m_map.CeilingTextures());
}

// Loads one material's albedo + normal pair at the current quality tier and
// appends it to the surface's variant arrays.
void DungeonWorld::LoadSurfaceMaterial(Surface& surface, const std::string& name) {
	const char* res = m_settings.TextureSuffix();
	std::string stem = paths::Asset(std::format("textures\\{}_{}", name, res));
	auto albedo = TryLoadTextureFile(m_device, stem, /*srgb*/ true);
	if (!albedo) {
		// Ultra's 4K sets are fetchable content (tools/FetchTextures.ps1);
		// drop to the always-present 2K set when they aren't installed.
		log::Warn("{} not found at {} — falling back to 2k", name, res);
		stem = paths::Asset(std::format("textures\\{}_2k", name));
		albedo = LoadTextureFile(m_device, stem, /*srgb*/ true);
	}
	surface.albedo.push_back(std::move(albedo));
	surface.normal.push_back(LoadTextureFile(m_device, stem + "_n")); // linear
	// ORM (occlusion/roughness/metallic) — present once the set is re-imported;
	// null until then (the renderer falls back to a neutral default).
	surface.mr.push_back(TryLoadTextureFile(m_device, stem + "_mr"));
}

void DungeonWorld::LoadTextureSet(const SurfaceDef& def) {
	def.surface.albedo.clear(); // quality hot-swap reuses the same Surface objects
	def.surface.normal.clear();
	def.surface.mr.clear();
	def.surface.heightScale = def.heightScale;
	for (const std::string& name : def.names)
		LoadSurfaceMaterial(def.surface, name);
}

void DungeonWorld::LoadAllSurfaceTextures() {
	for (const SurfaceDef& def : SurfaceDefs()) LoadTextureSet(def);
}

void DungeonWorld::BuildDungeonMeshes() {
	const DungeonGeometry geo =
		BuildDungeonGeometry(m_map, m_wallBlocks, m_floorBlocks, m_ceilingBlocks);

	auto upload = [&](Surface& surface, const std::vector<GeometryChunk>& chunks) {
		surface.chunks.clear();
		for (const GeometryChunk& gc : chunks) {
			SurfaceChunk sc;
			sc.variant = gc.variant;
			sc.boundsMin = gc.boundsMin;
			sc.boundsMax = gc.boundsMax;
			sc.mesh = std::make_unique<gfx::Mesh>(m_device, gc.mesh);
			surface.chunks.push_back(std::move(sc));
		}
	};
	upload(m_walls, geo.walls);
	upload(m_floors, geo.floors);
	upload(m_ceilings, geo.ceilings);
}

// Loads each monster model once (shared per kind) and creates one animator
// per spawn. The shared ModelData must stay alive for the animators' sake —
// it lives in m_monsterKinds for the app's lifetime.
void DungeonWorld::LoadMonsters() {
	auto kindOf = [this](const std::string& type) -> MonsterKind& {
		auto it = m_monsterKinds.find(type);
		if (it == m_monsterKinds.end()) {
			auto assets = std::make_unique<MonsterKind>();
			assets->model = LoadModelOrDie(type + ".gltf");
			assets->name = type;
			assets->mesh = std::make_unique<gfx::Mesh>(m_device, assets->model.meshes[0]);
			it = m_monsterKinds.emplace(type, std::move(assets)).first;
		}
		return *it->second;
	};

	int phase = 0;
	for (const Entity& spawn : m_entities.All()) {
		if (spawn.kind != EntityKind::Monster) continue;
		MonsterKind& kind = kindOf(spawn.type);
		Monster monster;
		monster.kind = &kind;
		monster.id = spawn.id;
		monster.x = monster.spawnX = spawn.x;
		monster.z = monster.spawnZ = spawn.z;
		monster.yaw = DirYaw(spawn.facing);
		monster.animator = anim::Animator(&kind.model.skeleton, &kind.model.clips);
		monster.animator.Play("idle");
		monster.animator.Update(static_cast<float>(phase++) * 0.7f); // desync idles
		m_monsters.push_back(std::move(monster));
	}
}

// Loads each decoration model once (shared per type, like monsters) and bakes
// one placed instance per .map "decoration" record. Authored facing +Z, so a
// record's facing rotates the prop the same way a monster's does. Everything
// is solid (blocks the party) except open passages like the archway; a
// "solid=0"/"solid=1" param on the record overrides the default.
void DungeonWorld::LoadDecorations() {
	// The built-in procedural props (the rest are imported authored models).
	auto isProcedural = [](const std::string& t) {
		return t == "column" || t == "archway" || t == "fountain" || t == "statue" ||
			   t == "door" || t == "barrel" || t == "crate" || t == "chest";
	};
	// Texture set per prop type: wooden props get planks, the stone ones a
	// dungeon-stone set; imported models carry their own same-named set.
	auto setForType = [&](const std::string& type) -> std::string {
		if (type == "door" || type == "barrel" || type == "crate" || type == "chest")
			return "wood_planks";
		if (isProcedural(type)) return "wall_stone";
		return type; // imported model: <name>_<res> texture set
	};
	// Loads a texture set once (shared across kinds): sRGB albedo + linear
	// normal/height + ORM, with the same res→2k fallback as the surfaces.
	auto texForSet = [this](const std::string& set) -> const PropTextures* {
		auto it = m_propTextures.find(set);
		if (it == m_propTextures.end()) {
			auto pt = std::make_unique<PropTextures>();
			const char* res = m_settings.TextureSuffix();
			std::string stem = paths::Asset(std::format("textures\\{}_{}", set, res));
			pt->albedo = TryLoadTextureFile(m_device, stem, /*srgb*/ true);
			if (!pt->albedo) {
				stem = paths::Asset(std::format("textures\\{}_2k", set));
				pt->albedo = LoadTextureFile(m_device, stem, /*srgb*/ true);
			}
			pt->normal = LoadTextureFile(m_device, stem + "_n");
			pt->mr = TryLoadTextureFile(m_device, stem + "_mr");
			pt->heightScale = 0.03f;
			it = m_propTextures.emplace(set, std::move(pt)).first;
		}
		return it->second.get();
	};

	auto kindOf = [&](const std::string& type) -> DecorationKind& {
		auto it = m_decorationKinds.find(type);
		if (it == m_decorationKinds.end()) {
			auto kind = std::make_unique<DecorationKind>();
			kind->model = LoadModelOrDie(type + ".gltf");
			kind->mesh = std::make_unique<gfx::Mesh>(m_device, kind->model.meshes[0]);
			kind->color = kind->model.materials[0].baseColorFactor;
			kind->tex = texForSet(setForType(type));
			kind->authored = !isProcedural(type);
			it = m_decorationKinds.emplace(type, std::move(kind)).first;
		}
		return *it->second;
	};

	for (const Entity& record : m_map.Decorations()) {
		DecorationKind& kind = kindOf(record.type);
		Decoration deco;
		deco.kind = &kind;
		deco.x = record.x;
		deco.z = record.z;
		deco.solid = record.type != "archway"; // passages let the party through
		if (const std::string* s = record.Param("solid")) deco.solid = *s != "0";

		const Vec3 pos = m_map.CellCenter(deco.x, deco.z);
		XMStoreFloat4x4(&deco.world, XMMatrixRotationY(DirYaw(record.facing)) *
										 XMMatrixTranslation(pos.x, 0, pos.z));
		m_decorations.push_back(std::move(deco));
	}
	log::Info("Placed {} decorations ({} kinds)", m_decorations.size(),
			  m_decorationKinds.size());
}

// Places one Fire per sconce ('T') and brazier ('F') cell. Sconces mount on
// the first solid neighbor wall and face into the room; braziers stand at
// the cell center. Flame origins match the baked models (see ModelBaker).
void DungeonWorld::BuildFires() {
	u32 seed = 1234;

	// Sconce mount: the first solid neighbor (N, E, S, W), defaulting to north.
	constexpr int kNeighborX[4] = {0, 1, 0, -1};
	constexpr int kNeighborZ[4] = {-1, 0, 1, 0};

	for (const auto& [tx, tz] : m_map.TorchCells()) {
		// Pick the wall this sconce hangs on.
		int dx = kNeighborX[0], dz = kNeighborZ[0];
		for (int n = 0; n < 4; ++n) {
			if (!m_map.IsWalkable(tx + kNeighborX[n], tz + kNeighborZ[n])) {
				dx = kNeighborX[n];
				dz = kNeighborZ[n];
				break;
			}
		}

		const Vec3 center = m_map.CellCenter(tx, tz);
		const Vec3 mount{center.x + dx * (kCellSize * 0.5f - 0.02f), 0.0f,
						 center.z + dz * (kCellSize * 0.5f - 0.02f)};
		// Authored facing +Z; rotate so +Z points away from the wall.
		const float yaw = std::atan2(static_cast<float>(-dx), static_cast<float>(-dz));

		Fire fire;
		fire.brazier = false;
		XMStoreFloat4x4(&fire.world, XMMatrixRotationY(yaw) *
										 XMMatrixTranslation(mount.x, 0, mount.z));
		// Flame local offset (0, 1.78, 0.22) rotated by yaw.
		fire.flamePos = {mount.x + std::sin(yaw) * 0.22f, 1.78f,
						 mount.z + std::cos(yaw) * 0.22f};
		fire.phase = static_cast<float>(seed) * 1.7f;
		fire.effect = FireEffect(fire.flamePos, 0.55f, seed++);
		m_fires.push_back(std::move(fire));
	}

	for (const auto& [bx, bz] : m_map.BrazierCells()) {
		const Vec3 center = m_map.CellCenter(bx, bz);
		Fire fire;
		fire.brazier = true;
		XMStoreFloat4x4(&fire.world, XMMatrixTranslation(center.x, 0, center.z));
		fire.flamePos = {center.x, 0.72f, center.z};
		fire.phase = static_cast<float>(seed) * 1.7f;
		fire.effect = FireEffect(fire.flamePos, 1.0f, seed++);
		m_fires.push_back(std::move(fire));
	}
	log::Info("Lit {} fires ({} sconces, {} braziers)", m_fires.size(),
			  m_map.TorchCells().size(), m_map.BrazierCells().size());
}

// Per-cell turbidity as a top-down density grid: one texel per dungeon cell,
// R channel; bilinear filtering blends region borders. The scene shader
// raymarches it (see scene.hlsl).
void DungeonWorld::BuildTurbidityMap() {
	assets::ImageData grid;
	grid.width = static_cast<u32>(m_map.Width());
	grid.height = static_cast<u32>(m_map.Height());
	grid.pixels.resize(static_cast<size_t>(grid.width) * grid.height * 4);
	for (int z = 0; z < m_map.Height(); ++z) {
		for (int x = 0; x < m_map.Width(); ++x) {
			const size_t i = (static_cast<size_t>(z) * grid.width + x) * 4;
			grid.pixels[i + 0] = static_cast<u8>(m_map.Turbidity(x, z) * 255.0f);
			grid.pixels[i + 3] = 255;
		}
	}
	m_turbidityMap = std::make_unique<gfx::Texture>(m_device, grid);
	m_atmosphere.turbidityMap = m_turbidityMap.get();
	m_atmosphere.worldExtent = {m_map.Width() * kCellSize,
								m_map.Height() * kCellSize};
}

// ============================================================================
// Quality hot-swap (see GameSettings's Quality) — the worn blocks exist at
// three baked tessellation levels and the scanned textures at three
// resolutions; switching reloads both and rebuilds the batched dungeon
// meshes in place (monsters and fires are unaffected).
// ============================================================================
void DungeonWorld::ApplyQuality(bool textureResChanged) {
	if (m_walls.chunks.empty()) return; // not built yet — the load tasks will

	// The GPU may still be reading the old resources, so drain it first.
	m_device.WaitIdle();
	m_walls.chunks.clear();
	m_floors.chunks.clear();
	m_ceilings.chunks.clear();
	LoadDungeonBlocks();
	if (textureResChanged) LoadAllSurfaceTextures();
	BuildDungeonMeshes();
	// The surface tessellation changed but the map Revision did not, so force a
	// re-render of every cached shadow cube (otherwise standing still after a
	// quality swap leaves the torch/glow cubes showing the old geometry).
	for (ShadowSlotCache& cache : m_shadowCache) cache.lightId = -1;
	log::Info("Quality switched to {} ({} meshes, {} textures)",
			  m_settings.QualityLabel(), m_settings.MeshSuffix(),
			  m_settings.TextureSuffix());
}

// ============================================================================
// Per-frame simulation
// ============================================================================

void DungeonWorld::ResetForNewGame() {
	m_party.Reset(m_map.StartX(), m_map.StartZ());
	for (Monster& monster : m_monsters) monster.announced = false;
	std::fill(m_seen.begin(), m_seen.end(), static_cast<u8>(0));
	MarkSeen(m_party.GridX(), m_party.GridZ());
	SetTorchPalette(0);
}

void DungeonWorld::CaptureState(SaveData& out) const {
	out.partyX = m_party.GridX();
	out.partyZ = m_party.GridZ();
	out.partyFacing = m_party.Facing();
	out.torchPalette = m_torchPalette;

	out.seen.clear();
	for (int z = 0; z < m_map.Height(); ++z)
		for (int x = 0; x < m_map.Width(); ++x)
			if (m_seen[static_cast<size_t>(z) * m_map.Width() + x])
				out.seen.emplace_back(x, z);

	// Diff against the .ent baseline: a monster gets an override row once it
	// has moved off its spawn cell or has announced itself (its position is the
	// spawn until monsters roam, but `announced` flips on approach).
	out.entities.clear();
	for (const Monster& m : m_monsters)
		if (m.x != m.spawnX || m.z != m.spawnZ || m.announced)
			out.entities.push_back({m.id, m.x, m.z, m.announced});
}

void DungeonWorld::ApplyState(const SaveData& in) {
	m_party.SetGridPosition(in.partyX, in.partyZ); // keeps facing, clears interp
	m_party.SetFacing(in.partyFacing);
	SetTorchPalette(in.torchPalette);

	std::fill(m_seen.begin(), m_seen.end(), static_cast<u8>(0));
	for (const auto& [x, z] : in.seen)
		if (x >= 0 && z >= 0 && x < m_map.Width() && z < m_map.Height())
			m_seen[static_cast<size_t>(z) * m_map.Width() + x] = 1;

	// Runs after ResetForNewGame cleared every monster's announced flag, so the
	// rows here restore both the moved cell and the announced state.
	for (const SaveData::EntityState& e : in.entities)
		for (Monster& m : m_monsters)
			if (m.id == e.id) {
				m.x = e.x;
				m.z = e.z;
				m.announced = e.announced;
				break;
			}
}

bool DungeonWorld::IsSeen(int x, int z) const {
	if (x < 0 || z < 0 || x >= m_map.Width() || z >= m_map.Height()) return false;
	return m_seen[static_cast<size_t>(z) * m_map.Width() + x] != 0;
}

void DungeonWorld::EditCell(int x, int z, Cell cell) {
	const u32 rev = m_map.Revision();
	m_map.SetCell(x, z, cell);
	if (m_map.Revision() == rev) return; // unchanged
	MarkSeen(x, z);
	RebuildGeometry();
}

void DungeonWorld::RebuildGeometry() {
	if (m_walls.chunks.empty()) return; // not built yet
	m_device.WaitIdle();                // GPU may still read the old meshes
	m_walls.chunks.clear();
	m_floors.chunks.clear();
	m_ceilings.chunks.clear();
	BuildDungeonMeshes();
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
	if (acceptInput) m_party.HandleInput(input);
	m_party.Update(dt);
	m_pillarAnimator.Update(dt);
	UpdateMonsters(dt);
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

void DungeonWorld::UpdateCamera() {
	m_camera.SetPosition(m_party.EyePosition());
	m_camera.SetYawPitch(m_party.Yaw(), 0.0f);
	m_camera.SetLens(m_fovDegrees * kPi / 180.0f,
					 static_cast<float>(m_device.Width()) /
						 static_cast<float>(m_device.Height()),
					 0.05f, 100.0f);
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
		light.radius = fire.brazier ? 7.5f : 7.0f;
		light.color = fire.brazier
						  ? Vec3{m_torchColor.x, m_torchColor.y * 0.85f, m_torchColor.z * 0.8f}
						  : m_torchColor;
		const float base = fire.brazier ? 2.3f : 1.8f;
		light.intensity = base * (0.9f + 0.1f * std::sin(time * 11.0f + fire.phase) *
											 std::sin(time * 7.3f + fire.phase));
		light.flickerShadow = true; // wandering origin → throttle its shadow cube
		m_lights.points.push_back(light);
	}

	gfx::PointLight glow;
	glow.position = {m_pillarPos.x, 1.3f, m_pillarPos.z};
	glow.radius = 5.0f;
	glow.color = {0.3f, 0.9f, 0.6f};
	glow.intensity = 1.2f + 0.2f * std::sin(time * 2.2f);
	m_lights.points.push_back(glow);

	AssignShadowSlots();
}

// Hands the kShadowSlots shadow cubes to the lights nearest the camera —
// slot 0 (highest resolution + PCF) to the closest, coarser slots outward,
// nothing beyond that. Only lights the renderer will upload (the first
// kMaxPointLights) compete, so a slot is never spent on a dropped light.
// The carried torch sits at the eye so it always wins slot 0: its surface
// shadows mostly hide behind their casters, but it is exactly what carves
// shafts through dusty air around nearby pillars.
void DungeonWorld::AssignShadowSlots() {
	static_assert(gfx::kShadowSlots <= gfx::kMaxPointLights);
	const Vec3 eye = m_party.EyePosition();

	for (gfx::PointLight& light : m_lights.points) light.shadowSlot = -1;
	if (!m_shadowsEnabled) return; // dev console: lights stay lit, just unshadowed

	m_shadowCandidates.clear();
	const size_t lightCount =
		std::min<size_t>(m_lights.points.size(), gfx::kMaxPointLights);
	for (size_t i = 0; i < lightCount; ++i) {
		const Vec3 d = Sub(m_lights.points[i].position, eye);
		m_shadowCandidates.emplace_back(d.x * d.x + d.y * d.y + d.z * d.z, i);
	}
	std::ranges::sort(m_shadowCandidates);

	const size_t count = std::min<size_t>(m_shadowCandidates.size(), gfx::kShadowSlots);
	for (size_t slot = 0; slot < count; ++slot)
		m_lights.points[m_shadowCandidates[slot].second].shadowSlot =
			static_cast<int>(slot);
}

void DungeonWorld::UpdateMonsters(float dt) {
	const Vec3 partyPos = m_party.EyePosition();
	for (Monster& monster : m_monsters) {
		monster.animator.Update(dt);

		// Face the party (blobs don't care).
		const Vec3 pos = m_map.CellCenter(monster.x, monster.z);
		if (monster.kind->name != "blob")
			monster.yaw = std::atan2(partyPos.x - pos.x, partyPos.z - pos.z);

		// Announce once when the party first comes within one cell.
		const int dx = std::abs(monster.x - m_party.GridX());
		const int dz = std::abs(monster.z - m_party.GridZ());
		if (!monster.announced && std::max(dx, dz) <= 1) {
			monster.announced = true;
			onMessage(loc::Format("log.monster_stirs",
								  loc::Tr("monster." + monster.kind->name)));
			m_audio.Play(m_sounds.monster, 0.7f);
		}
	}
}

// ============================================================================
// Rendering — the command list arrives from GraphicsDevice::BeginFrame
// already cleared and bound.
// ============================================================================

// --- view culling -----------------------------------------------------------

DungeonWorld::ViewCull DungeonWorld::ViewCull::FromFrustum(const Mat4& m) {
	// Gribb-Hartmann from a row-vector view-proj (clip = p * M): the clip
	// components read the COLUMNS of M, so planes are column sums/differences.
	const Vec4 c1{m._11, m._21, m._31, m._41};
	const Vec4 c2{m._12, m._22, m._32, m._42};
	const Vec4 c3{m._13, m._23, m._33, m._43};
	const Vec4 c4{m._14, m._24, m._34, m._44};
	auto add = [](const Vec4& a, const Vec4& b) {
		return Vec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
	};
	auto sub = [](const Vec4& a, const Vec4& b) {
		return Vec4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
	};
	ViewCull cull;
	cull.isSphere = false;
	const Vec4 raw[6] = {add(c4, c1), sub(c4, c1), add(c4, c2),
						 sub(c4, c2), c3,          sub(c4, c3)}; // L R B T near far
	for (int i = 0; i < 6; ++i) {
		Vec4 p = raw[i];
		const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
		if (len > 1e-8f) { p.x /= len; p.y /= len; p.z /= len; p.w /= len; }
		cull.planes[i] = p;
	}
	return cull;
}

DungeonWorld::ViewCull DungeonWorld::ViewCull::FromSphere(const Vec3& center,
														 float radius) {
	ViewCull cull;
	cull.isSphere = true;
	cull.sphereC = center;
	cull.sphereR = radius;
	return cull;
}

bool DungeonWorld::ViewCull::TestSphere(const Vec3& c, float r) const {
	if (isSphere) {
		const Vec3 d = Sub(c, sphereC);
		const float reach = sphereR + r;
		return d.x * d.x + d.y * d.y + d.z * d.z <= reach * reach;
	}
	for (const Vec4& p : planes)
		if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -r) return false; // outside
	return true;
}

bool DungeonWorld::ViewCull::TestAABB(const Vec3& lo, const Vec3& hi) const {
	if (isSphere) {
		// Squared distance from the sphere center to the AABB.
		auto axis = [](float c, float l, float h) {
			const float v = c < l ? l - c : (c > h ? c - h : 0.0f);
			return v * v;
		};
		const float d2 = axis(sphereC.x, lo.x, hi.x) + axis(sphereC.y, lo.y, hi.y) +
						 axis(sphereC.z, lo.z, hi.z);
		return d2 <= sphereR * sphereR;
	}
	for (const Vec4& p : planes) {
		// Positive vertex: the AABB corner farthest along the plane normal. If
		// it is outside, the whole box is (conservative — no false negatives).
		const float px = p.x >= 0 ? hi.x : lo.x;
		const float py = p.y >= 0 ? hi.y : lo.y;
		const float pz = p.z >= 0 ? hi.z : lo.z;
		if (p.x * px + p.y * py + p.z * pz + p.w < 0.0f) return false;
	}
	return true;
}

void DungeonWorld::NewFrame(u32 frameIndex) {
	if (m_particleBatch) m_particleBatch->NewFrame(frameIndex);
}

bool DungeonWorld::AnimatedCasterNear(const gfx::PointLight& light) const {
	auto inReach = [&](const Vec3& c, float r) {
		const Vec3 d = Sub(c, light.position);
		const float reach = light.radius + r;
		return d.x * d.x + d.y * d.y + d.z * d.z <= reach * reach;
	};
	if (inReach({m_pillarPos.x, 1.2f, m_pillarPos.z}, 1.2f)) return true; // sway
	for (const Monster& m : m_monsters) {
		const Vec3 c = m_map.CellCenter(m.x, m.z);
		if (inReach({c.x, 1.0f, c.z}, 1.5f)) return true;
	}
	return false;
}

// Renders the cube shadow maps for every light that holds a slot this frame.
// Runs before the main pass with the shadow pipeline bound. A slot's cube is
// reused from the previous frame (left in its SRV state, the barrier guard
// skips it) unless the light changed/moved, a flicker tick is due, geometry
// changed, or an animating caster sits within the light.
void DungeonWorld::RenderShadowMaps(ID3D12GraphicsCommandList* list) {
	++m_frameCounter;
	constexpr u64 kFlickerInterval = 2;  // re-render wandering fire cubes at half rate
	constexpr float kPosEps2 = 0.0004f;  // 2 cm: a steady light re-renders once it moves
	const u32 rev = m_map.Revision();

	for (size_t i = 0; i < m_lights.points.size(); ++i) {
		const gfx::PointLight& light = m_lights.points[i];
		if (light.shadowSlot < 0) continue;
		const int slot = light.shadowSlot;
		ShadowSlotCache& cache = m_shadowCache[slot];

		const Vec3 d = Sub(light.position, cache.pos);
		const bool moved = d.x * d.x + d.y * d.y + d.z * d.z > kPosEps2;
		const bool flickerDue =
			(m_frameCounter + static_cast<u64>(slot)) % kFlickerInterval == 0;
		const bool needsRender = cache.lightId != static_cast<int>(i) ||
								 cache.revision != rev || AnimatedCasterNear(light) ||
								 (light.flickerShadow ? flickerDue : moved);
		if (!needsRender) continue; // reuse the cube already bound as an SRV

		const ViewCull cull = ViewCull::FromSphere(light.position, light.radius);
		for (u32 face = 0; face < 6; ++face) {
			m_renderer.BeginShadowFace(list, static_cast<u32>(slot), face,
									   light.position, light.radius);
			SubmitSceneGeometry(list, &cull);
		}
		cache = {static_cast<int>(i), light.position, rev};
	}
	m_renderer.EndShadows(list);
	m_device.BindBackBuffer(list); // the shadow pass redirected the OM
}

void DungeonWorld::RenderScene(ID3D12GraphicsCommandList* list) {
	m_renderer.BeginScene(list, m_camera, m_lights,
						  m_dustEnabled ? m_atmosphere : gfx::Atmosphere{});
	const ViewCull cull = ViewCull::FromFrustum(m_camera.ViewProj());
	SubmitSceneGeometry(list, &cull);
	// Transparent flame/spark/smoke billboards last, over the opaque scene.
	m_particleBatch->Render(list, m_camera, m_particleScratch);
}

void DungeonWorld::SubmitSceneGeometry(ID3D12GraphicsCommandList* list,
									  const ViewCull* cull) {
	// A discrete mesh draws only if its bounding sphere passes the cull (camera
	// frustum in the main pass, light reach in the shadow pass).
	auto visible = [&](const Vec3& c, float r) {
		return !cull || cull->TestSphere(c, r);
	};

	DrawSurface(list, m_walls, cull);
	DrawSurface(list, m_floors, cull);
	DrawSurface(list, m_ceilings, cull);

	// Pillar — polished jade, distinctly glossier than the stonework.
	if (visible({m_pillarPos.x, 1.2f, m_pillarPos.z}, 1.8f)) {
		Mat4 pillarWorld = Mat4Identity();
		pillarWorld._41 = m_pillarPos.x;
		pillarWorld._43 = m_pillarPos.z;
		gfx::MaterialParams pillarMaterial;
		pillarMaterial.baseColor = m_pillarModel.materials[0].baseColorFactor;
		pillarMaterial.roughness = 0.22f; // polished jade
		m_renderer.DrawMesh(list, *m_pillarMesh, pillarWorld, pillarMaterial,
							m_pillarAnimator.Palette());
	}

	// Static architecture decorations (columns, archways, fountains, ...):
	// textured stone/wood with bump + parallax + ORM, falling back to the flat
	// glTF material color if the texture set is missing.
	for (const Decoration& deco : m_decorations) {
		// radius 2.0 covers the widest prop (the archway spans the full cell).
		if (!visible({deco.world._41, 1.2f, deco.world._43}, 2.0f)) continue;
		gfx::MaterialParams material;
		material.doubleSided = !deco.kind->authored; // authored meshes back-cull
		const PropTextures* tex = deco.kind->tex;
		if (tex && tex->albedo) {
			material.albedo = tex->albedo.get();
			material.normalMap = tex->normal.get();
			material.heightScale = tex->heightScale;
			if (tex->mr) {
				material.metalRough = tex->mr.get();
				material.metallic = 1.0f;
				material.roughness = 1.0f;
			}
		} else {
			material.baseColor = deco.kind->color;
			material.roughness = 0.85f;
		}
		m_renderer.DrawMesh(list, *deco.kind->mesh, deco.world, material);
	}

	// Monsters. Bone and bandages are matte; the blob glistens wetly.
	for (const Monster& monster : m_monsters) {
		const MonsterKind& kind = *monster.kind;
		const Vec3 pos = m_map.CellCenter(monster.x, monster.z);
		if (!visible({pos.x, 1.0f, pos.z}, 1.5f)) continue;
		Mat4 world;
		XMStoreFloat4x4(&world, XMMatrixRotationY(monster.yaw) *
									XMMatrixTranslation(pos.x, 0, pos.z));
		gfx::MaterialParams material;
		material.baseColor = kind.model.materials[0].baseColorFactor;
		if (kind.name == "blob") material.roughness = 0.18f; // wet glisten
		m_renderer.DrawMesh(list, *kind.mesh, world, material,
							monster.animator.Palette());
	}

	// Fire props (sconces + braziers): bare iron.
	for (const Fire& fire : m_fires) {
		if (!visible(fire.flamePos, 1.2f)) continue;
		gfx::MaterialParams iron;
		iron.baseColor = fire.brazier ? m_brazierColor : m_sconceColor;
		iron.metallic = 1.0f;
		iron.roughness = 0.5f;
		m_renderer.DrawMesh(list, fire.brazier ? *m_brazierMesh : *m_sconceMesh,
							fire.world, iron);
	}
}

void DungeonWorld::DrawSurface(ID3D12GraphicsCommandList* list,
							   const Surface& surface, const ViewCull* cull) {
	const Mat4 identity = Mat4Identity();
	for (const SurfaceChunk& chunk : surface.chunks) {
		if (cull && !cull->TestAABB(chunk.boundsMin, chunk.boundsMax)) continue;
		const int v = chunk.variant;
		gfx::MaterialParams material;
		material.albedo = surface.albedo[v].get();
		material.normalMap = surface.normal[v].get();
		material.heightScale = surface.heightScale;
		// When the set has an ORM map it drives roughness/metallic per-texel
		// (factors at 1.0 = pass through); otherwise dry matte stone.
		if (surface.mr[v]) {
			material.metalRough = surface.mr[v].get();
			material.metallic = 1.0f;
			material.roughness = 1.0f;
		}
		m_renderer.DrawMesh(list, *chunk.mesh, identity, material);
	}
}

} // namespace dungeon::game
