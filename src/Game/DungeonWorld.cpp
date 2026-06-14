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
	m_party.onStep = [this] { m_audio.Play(m_sounds.footstep, 0.8f); };
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
		return false;
	};

	m_lights.ambient = {0.035f, 0.032f, 0.045f};
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
	auto albedo = TryLoadTextureFile(m_device, stem);
	if (!albedo) {
		// Ultra's 4K sets are fetchable content (tools/FetchTextures.ps1);
		// drop to the always-present 2K set when they aren't installed.
		log::Warn("{} not found at {} — falling back to 2k", name, res);
		stem = paths::Asset(std::format("textures\\{}_2k", name));
		albedo = LoadTextureFile(m_device, stem);
	}
	surface.albedo.push_back(std::move(albedo));
	surface.normal.push_back(LoadTextureFile(m_device, stem + "_n"));
}

void DungeonWorld::LoadTextureSet(const SurfaceDef& def) {
	def.surface.albedo.clear(); // quality hot-swap reuses the same Surface objects
	def.surface.normal.clear();
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

	auto upload = [&](Surface& surface, const std::vector<assets::MeshData>& buckets) {
		for (const assets::MeshData& bucket : buckets)
			surface.meshes.push_back(bucket.vertices.empty()
										 ? nullptr
										 : std::make_unique<gfx::Mesh>(m_device, bucket));
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
		monster.x = spawn.x;
		monster.z = spawn.z;
		monster.yaw = DirYaw(spawn.facing);
		monster.animator = anim::Animator(&kind.model.skeleton, &kind.model.clips);
		monster.animator.Play("idle");
		monster.animator.Update(static_cast<float>(phase++) * 0.7f); // desync idles
		m_monsters.push_back(std::move(monster));
	}
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
	if (m_walls.meshes.empty()) return; // not built yet — the load tasks will

	// The GPU may still be reading the old resources, so drain it first.
	m_device.WaitIdle();
	m_walls.meshes.clear();
	m_floors.meshes.clear();
	m_ceilings.meshes.clear();
	LoadDungeonBlocks();
	if (textureResChanged) LoadAllSurfaceTextures();
	BuildDungeonMeshes();
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
	SetTorchPalette(0);
}

void DungeonWorld::SetTorchPalette(int index) {
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

void DungeonWorld::NewFrame(u32 frameIndex) {
	if (m_particleBatch) m_particleBatch->NewFrame(frameIndex);
}

// Renders the cube shadow maps for every light that holds a slot this frame.
// Runs before the main pass; the same geometry submission is reused with the
// renderer's shadow pipeline bound.
void DungeonWorld::RenderShadowMaps(ID3D12GraphicsCommandList* list) {
	for (const gfx::PointLight& light : m_lights.points) {
		if (light.shadowSlot < 0) continue;
		for (u32 face = 0; face < 6; ++face) {
			m_renderer.BeginShadowFace(list, static_cast<u32>(light.shadowSlot), face,
									   light.position, light.radius);
			SubmitSceneGeometry(list);
		}
	}
	m_renderer.EndShadows(list);
	m_device.BindBackBuffer(list); // the shadow pass redirected the OM
}

void DungeonWorld::RenderScene(ID3D12GraphicsCommandList* list) {
	m_renderer.BeginScene(list, m_camera, m_lights,
						  m_dustEnabled ? m_atmosphere : gfx::Atmosphere{});
	SubmitSceneGeometry(list);
	// Transparent flame/spark/smoke billboards last, over the opaque scene.
	m_particleBatch->Render(list, m_camera, m_particleScratch);
}

void DungeonWorld::SubmitSceneGeometry(ID3D12GraphicsCommandList* list) {
	DrawSurface(list, m_walls);
	DrawSurface(list, m_floors);
	DrawSurface(list, m_ceilings);

	// Pillar — polished jade, distinctly glossier than the stonework.
	Mat4 pillarWorld = Mat4Identity();
	pillarWorld._41 = m_pillarPos.x;
	pillarWorld._43 = m_pillarPos.z;
	gfx::MaterialParams pillarMaterial;
	pillarMaterial.baseColor = m_pillarModel.materials[0].baseColorFactor;
	pillarMaterial.specStrength = 0.45f;
	pillarMaterial.specPower = 48.0f;
	m_renderer.DrawMesh(list, *m_pillarMesh, pillarWorld, pillarMaterial,
						m_pillarAnimator.Palette());

	// Monsters. Bone and bandages are matte; the blob glistens wetly.
	for (const Monster& monster : m_monsters) {
		const MonsterKind& kind = *monster.kind;
		const Vec3 pos = m_map.CellCenter(monster.x, monster.z);
		Mat4 world;
		XMStoreFloat4x4(&world, XMMatrixRotationY(monster.yaw) *
									XMMatrixTranslation(pos.x, 0, pos.z));
		gfx::MaterialParams material;
		material.baseColor = kind.model.materials[0].baseColorFactor;
		if (kind.name == "blob") {
			material.specStrength = 0.55f;
			material.specPower = 32.0f;
		}
		m_renderer.DrawMesh(list, *kind.mesh, world, material,
							monster.animator.Palette());
	}

	// Fire props (sconces + braziers): slightly speculative iron.
	for (const Fire& fire : m_fires) {
		gfx::MaterialParams iron;
		iron.baseColor = fire.brazier ? m_brazierColor : m_sconceColor;
		iron.specStrength = 0.18f;
		iron.specPower = 28.0f;
		m_renderer.DrawMesh(list, fire.brazier ? *m_brazierMesh : *m_sconceMesh,
							fire.world, iron);
	}
}

void DungeonWorld::DrawSurface(ID3D12GraphicsCommandList* list,
							   const Surface& surface) {
	const Mat4 identity = Mat4Identity();
	for (size_t i = 0; i < surface.meshes.size(); ++i) {
		if (!surface.meshes[i]) continue;
		gfx::MaterialParams material;
		material.albedo = surface.albedo[i].get();
		material.normalMap = surface.normal[i].get();
		material.heightScale = surface.heightScale;
		// Dry, matte stone (the MaterialParams specular defaults).
		m_renderer.DrawMesh(list, *surface.meshes[i], identity, material);
	}
}

} // namespace dungeon::game
