#include "Game/Game.h"

#include "Assets/Dds.h"
#include "Assets/File.h"
#include "Core/Assert.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/DungeonMeshBuilder.h"

#include <algorithm>
#include <cmath>
#include <format>

using namespace DirectX;

namespace dungeon::game {

// ============================================================================
// Asset loading helpers
// ============================================================================
namespace {

// Required assets fail hard with the loader's reason — a missing model means
// the assets/ directory wasn't baked or copied next to the exe.
assets::ModelData LoadModelOrDie(const std::string& name) {
	auto model = assets::LoadModel(paths::Asset("models\\" + name));
	DN_ASSERT(model.has_value(), model.error() + " — run AssetBaker over assets/");
	return std::move(*model);
}

assets::SoundData LoadSound(const std::string& name) {
	auto sound = assets::LoadWavFile(paths::Asset("sounds\\" + name));
	if (!sound) log::Warn("{} (running silent)", sound.error());
	return std::move(sound).value_or(assets::SoundData{});
}

// Loads a texture by stem (no extension), preferring the baked .dds mip
// chain (no runtime filtering); falls back to the PNG + runtime mips so a
// fresh checkout still works before `AssetBaker mips` has run. Returns null
// if neither file exists.
std::unique_ptr<gfx::Texture> TryLoadTextureFile(gfx::GraphicsDevice& device,
												 const std::string& stemPath) {
	if (auto mips = assets::LoadDdsFile(stemPath + ".dds"))
		return std::make_unique<gfx::Texture>(device, *mips);
	if (auto image = assets::LoadImageFile(stemPath + ".png"))
		return std::make_unique<gfx::Texture>(device, *image);
	return nullptr;
}

std::unique_ptr<gfx::Texture> LoadTextureFile(gfx::GraphicsDevice& device,
											  const std::string& stemPath) {
	auto texture = TryLoadTextureFile(device, stemPath);
	DN_ASSERT(texture != nullptr,
			  "missing texture " + stemPath + " — run AssetBaker over assets/");
	return texture;
}

} // namespace

// ============================================================================
// Construction — cheap setup only; the heavy asset work is queued as load
// tasks that run one per frame behind the loading screen (see Update).
// ============================================================================
Game::Game(Window& window, gfx::GraphicsDevice& device, gfx::Renderer& renderer,
		   gfx::SpriteBatch& spriteBatch, audio::AudioEngine& audio)
	: m_window(window), m_device(device), m_renderer(renderer),
	  m_spriteBatch(spriteBatch), m_audio(audio),
	  m_map(paths::Asset("maps\\level1.map")),
	  m_party(m_map, m_map.StartX(), m_map.StartZ()),
	  m_ui(device, "", 17.0f), m_menuUi(device, "", 28.0f),
	  m_settingsUi(device, "", 28.0f), m_titleFont(device, "", 64.0f) {
	LoadQualitySetting();
	// Party event hooks (survive Party::Reset).
	m_party.onStep = [this] { m_audio.Play(m_sfxFootstep, 0.8f); };
	m_party.onBlocked = [this] {
		m_audio.Play(m_sfxBump, 0.9f);
		m_log->AddLine("You bump into a wall.");
	};
	m_party.onTurn = [this] { m_audio.Play(m_sfxTurn, 0.6f); };
	m_party.isOccupied = [this](int x, int z) {
		for (const Monster& monster : m_monsters) {
			if (monster.x == x && monster.z == z) {
				m_audio.Play(m_sfxMonster, 0.8f);
				m_log->AddLine(std::format("The {} blocks your way!",
										   m_monsterKinds.at(monster.kind)->name));
				return true;
			}
		}
		for (const auto& [bx, bz] : m_map.BrazierCells()) {
			if (bx == x && bz == z) {
				m_audio.Play(m_sfxBump, 0.7f);
				m_log->AddLine("A burning brazier bars your way.");
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

	BuildMenu();
	BuildLoadTasks();
}

// ============================================================================
// Staged loading. Each task is one frame's worth of blocking work; the
// loading screen renders between tasks. Order matters: textures register
// their variant counts before the geometry task buckets cells by variant.
// ============================================================================
void Game::BuildLoadTasks() {
	m_loadTasks = {
		{"Quarrying stone blocks", [this] { LoadDungeonBlocks(); }},
		{"Weaving the stonework", [this] { LoadAllSurfaceTextures(); }},
		{"Raising the dungeon", [this] { BuildDungeonMeshes(); }},
		{"Carving the serpent pillar",
		 [this] {
			 m_pillarModel = LoadModelOrDie("pillar.gltf");
			 m_pillarMesh = std::make_unique<gfx::Mesh>(m_device, m_pillarModel.meshes[0]);
			 m_pillarAnimator = anim::Animator(&m_pillarModel.skeleton, &m_pillarModel.clips);
			 m_pillarAnimator.Play("sway");
			 m_pillarPos = m_map.CellCenter(m_map.StartX(), m_map.StartZ() + 2);
		 }},
		{"Waking the monsters", [this] { LoadMonsters(); }},
		{"Kindling the fires",
		 [this] {
			 auto sconce = LoadModelOrDie("sconce.gltf");
			 m_sconceMesh = std::make_unique<gfx::Mesh>(m_device, sconce.meshes[0]);
			 m_sconceColor = sconce.materials[0].baseColorFactor;
			 auto brazier = LoadModelOrDie("brazier.gltf");
			 m_brazierMesh = std::make_unique<gfx::Mesh>(m_device, brazier.meshes[0]);
			 m_brazierColor = brazier.materials[0].baseColorFactor;
			 m_particleBatch = std::make_unique<gfx::ParticleBatch>(m_device);
			 BuildFires();
		 }},
		{"Tuning the echoes",
		 [this] {
			 m_sfxFootstep = LoadSound("footstep.wav");
			 m_sfxBump = LoadSound("bump.wav");
			 m_sfxTurn = LoadSound("turn.wav");
			 m_sfxClick = LoadSound("click.wav");
			 m_sfxMonster = LoadSound("monster.wav");
		 }},
		{"Stirring the dust",
		 [this] {
			 // Per-cell turbidity as a top-down density grid: one texel per
			 // dungeon cell, R channel; bilinear filtering blends region
			 // borders. The scene shader raymarches it (see scene.hlsl).
			 assets::ImageData grid;
			 grid.width = static_cast<u32>(m_map.Width());
			 grid.height = static_cast<u32>(m_map.Height());
			 grid.pixels.resize(static_cast<size_t>(grid.width) * grid.height * 4);
			 for (int z = 0; z < m_map.Height(); ++z) {
				 for (int x = 0; x < m_map.Width(); ++x) {
					 const size_t i =
						 (static_cast<size_t>(z) * grid.width + x) * 4;
					 grid.pixels[i + 0] =
						 static_cast<u8>(m_map.Turbidity(x, z) * 255.0f);
					 grid.pixels[i + 3] = 255;
				 }
			 }
			 m_turbidityMap = std::make_unique<gfx::Texture>(m_device, grid);
			 m_atmosphere.turbidityMap = m_turbidityMap.get();
			 m_atmosphere.worldExtent = {m_map.Width() * kCellSize,
										 m_map.Height() * kCellSize};
		 }},
		{"Painting the title",
		 [this] {
			 m_titleBackground =
				 LoadTextureFile(m_device, paths::Asset("textures\\title_bg"));
		 }},
		{"Lighting the torches",
		 [this] {
			 BuildHud();
			 log::Info("Game loaded: {}x{} dungeon, {} torches, {} monsters",
					   m_map.Width(), m_map.Height(), m_map.TorchCells().size(),
					   m_monsters.size());
		 }},
	};
}

// ============================================================================
// Quality tiers. The worn dungeon blocks exist at three baked tessellation
// levels and the scanned textures at two resolutions; switching quality
// reloads both and rebuilds the batched dungeon meshes in place (monsters
// and fires are unaffected).
// ============================================================================

const char* Game::QualitySuffix() const {
	switch (m_quality) {
	case Quality::Low:   return "low";
	case Quality::High:
	case Quality::Ultra: return "high"; // Ultra = high meshes + 4K textures
	default:             return "med";
	}
}

const char* Game::QualityTextureSuffix() const {
	switch (m_quality) {
	case Quality::Ultra: return "4k";
	case Quality::High:  return "2k";
	default:             return "1k";
	}
}

const char* Game::QualityLabel() const {
	switch (m_quality) {
	case Quality::Low:   return "Low";
	case Quality::High:  return "High";
	case Quality::Ultra: return "Ultra";
	default:             return "Medium";
	}
}

void Game::LoadDungeonBlocks() {
	// The old dungeon uses the worn, crumbling block set; the clean
	// *_block.gltf models remain baked for newer areas of the game.
	const std::string sfx = std::format("_{}.gltf", QualitySuffix());
	m_wallBlock = LoadModelOrDie("wall_block_worn" + sfx).meshes[0];
	m_floorBlock = LoadModelOrDie("floor_block_worn" + sfx).meshes[0];
	m_ceilingBlock = LoadModelOrDie("ceiling_block_worn" + sfx).meshes[0];
}

void Game::SetQuality(Quality quality) {
	if (quality == m_quality) return;
	const std::string oldTextureSuffix = QualityTextureSuffix();
	m_quality = quality;
	const bool textureResChanged = oldTextureSuffix != QualityTextureSuffix();
	SaveQualitySetting();
	if (m_settingsMenu)
		m_settingsMenu->SetLabel(0, std::format("Quality: {}", QualityLabel()));

	// Hot-swap the dungeon meshes (and textures, when crossing the 1K/2K
	// boundary) if they are already built. The GPU may still be reading the
	// old resources, so drain it first.
	if (!m_walls.meshes.empty()) {
		m_device.WaitIdle();
		m_walls.meshes.clear();
		m_floors.meshes.clear();
		m_ceilings.meshes.clear();
		LoadDungeonBlocks();
		if (textureResChanged) LoadAllSurfaceTextures();
		BuildDungeonMeshes();
		log::Info("Quality switched to {} ({} meshes, {} textures)", QualityLabel(),
				  QualitySuffix(), QualityTextureSuffix());
	}
}

void Game::LoadQualitySetting() {
	auto bytes = assets::ReadBinaryFile(paths::ExecutableDir() + "\\settings.ini");
	if (!bytes) return; // first run: keep the default
	const std::string text(bytes->begin(), bytes->end());
	const size_t pos = text.find("quality=");
	if (pos == std::string::npos || pos + 8 >= text.size()) return;
	const char digit = text[pos + 8];
	if (digit >= '0' && digit <= '3')
		m_quality = static_cast<Quality>(digit - '0');
}

void Game::SaveQualitySetting() const {
	const std::string text = std::format("quality={}\n", static_cast<int>(m_quality));
	if (!assets::WriteBinaryFile(paths::ExecutableDir() + "\\settings.ini",
								 text.data(), text.size()))
		log::Warn("Could not write settings.ini");
}

void Game::LoadTextureSet(Surface& surface, std::initializer_list<const char*> names,
						  float heightScale) {
	surface.albedo.clear(); // quality hot-swap reuses the same Surface objects
	surface.normal.clear();
	surface.heightScale = heightScale;
	const char* res = QualityTextureSuffix();
	for (const char* name : names) {
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
}

void Game::LoadAllSurfaceTextures() {
	LoadTextureSet(m_walls, {"wall_brick", "wall_stone", "wall_moss"}, 0.055f);
	LoadTextureSet(m_floors, {"floor_slabs", "floor_cobble"}, 0.045f);
	LoadTextureSet(m_ceilings, {"ceiling_rough", "ceiling_cracked"}, 0.035f);
}

void Game::BuildDungeonMeshes() {
	const DungeonGeometry geo = BuildDungeonGeometry(
		m_map, m_wallBlock, m_floorBlock, m_ceilingBlock,
		static_cast<u32>(m_walls.albedo.size()),
		static_cast<u32>(m_floors.albedo.size()),
		static_cast<u32>(m_ceilings.albedo.size()));

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
void Game::LoadMonsters() {
	auto kindOf = [this](char kind) -> MonsterKind& {
		auto it = m_monsterKinds.find(kind);
		if (it == m_monsterKinds.end()) {
			auto assets = std::make_unique<MonsterKind>();
			switch (kind) {
			case 'S': assets->model = LoadModelOrDie("skeleton.gltf"); assets->name = "skeleton"; break;
			case 'M': assets->model = LoadModelOrDie("mummy.gltf"); assets->name = "mummy"; break;
			default:  assets->model = LoadModelOrDie("blob.gltf"); assets->name = "blob"; break;
			}
			assets->mesh = std::make_unique<gfx::Mesh>(m_device, assets->model.meshes[0]);
			it = m_monsterKinds.emplace(kind, std::move(assets)).first;
		}
		return *it->second;
	};

	int phase = 0;
	for (const DungeonMap::MonsterSpawn& spawn : m_map.MonsterSpawns()) {
		MonsterKind& kind = kindOf(spawn.kind);
		Monster monster;
		monster.kind = spawn.kind;
		monster.x = spawn.x;
		monster.z = spawn.z;
		monster.animator = anim::Animator(&kind.model.skeleton, &kind.model.clips);
		monster.animator.Play("idle");
		monster.animator.Update(static_cast<float>(phase++) * 0.7f); // desync idles
		m_monsters.push_back(std::move(monster));
	}
}

// Places one Fire per sconce ('T') and brazier ('F') cell. Sconces mount on
// the first solid neighbor wall and face into the room; braziers stand at
// the cell center. Flame origins match the baked models (see ModelBaker).
void Game::BuildFires() {
	u32 seed = 1234;

	for (const auto& [tx, tz] : m_map.TorchCells()) {
		// Pick the wall this sconce hangs on.
		int dx = 0, dz = -1;
		if (!m_map.IsWalkable(tx, tz - 1)) { dx = 0; dz = -1; }
		else if (!m_map.IsWalkable(tx + 1, tz)) { dx = 1; dz = 0; }
		else if (!m_map.IsWalkable(tx, tz + 1)) { dx = 0; dz = 1; }
		else if (!m_map.IsWalkable(tx - 1, tz)) { dx = -1; dz = 0; }

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

// ============================================================================
// Landing page — title plus a MenuList; entries highlight on mouse hover or
// keyboard selection. Start New Game and Settings are wired up; Continue /
// Load / Save wait on the save system.
// ============================================================================
void Game::BuildMenu() {
	const float w = static_cast<float>(m_window.Width());
	const float h = static_cast<float>(m_window.Height());

	const float menuW = 420.0f;
	const float itemH = 58.0f;
	auto* menu = m_menuUi.Add<ui::MenuList>(
		gfx::Rect{(w - menuW) * 0.5f, h * 0.42f, menuW, itemH * 5}, itemH);

	menu->AddItem("Continue");           // not implemented yet
	menu->AddItem("Start New Game", [this] {
		m_audio.Play(m_sfxClick, 0.6f);
		StartNewGame();
	});
	menu->AddItem("Load");               // not implemented yet
	menu->AddItem("Save");               // not implemented yet
	menu->AddItem("Settings", [this] {
		m_audio.Play(m_sfxClick, 0.5f);
		m_menuPage = MenuPage::Settings;
	});

	// Settings page: quality tier (cycles Low/Medium/High/Ultra) + back.
	const float settingsH = itemH * 2;
	m_settingsMenu = m_settingsUi.Add<ui::MenuList>(
		gfx::Rect{(w - menuW) * 0.5f, h * 0.46f, menuW, settingsH}, itemH);
	m_settingsMenu->AddItem(std::format("Quality: {}", QualityLabel()), [this] {
		m_audio.Play(m_sfxClick, 0.5f);
		// SetQuality refreshes the menu label itself.
		SetQuality(static_cast<Quality>((static_cast<int>(m_quality) + 1) % 4));
	});
	m_settingsMenu->AddItem("Back", [this] {
		m_audio.Play(m_sfxClick, 0.5f);
		m_menuPage = MenuPage::Main;
	});
}

void Game::StartNewGame() {
	m_party.Reset(m_map.StartX(), m_map.StartZ());
	for (Monster& monster : m_monsters) monster.announced = false;
	ApplyTorchPalette(0);

	m_log->Clear();
	m_log->AddLine("You descend into the dungeon...");
	m_log->AddLine("Something shuffles in the dark.");
	m_log->AddLine("W/S move, A/D strafe, Q/E turn.");

	m_lastFacing = m_lastGridX = m_lastGridZ = -1; // force HUD label refresh
	m_state = AppState::Playing;
	log::Info("New game started");
}

// ============================================================================
// HUD — laid out once in absolute pixels from the initial window size.
// Widgets the game updates later are kept as raw pointers (m_log, m_compass,
// m_position); the UIContext owns all widgets.
// ============================================================================
void Game::BuildHud() {
	const float w = static_cast<float>(m_window.Width());
	const float h = static_cast<float>(m_window.Height());

	// Message log, bottom-left.
	m_log = m_ui.Add<ui::TextOutput>(gfx::Rect{16, h - 200, 520, 184});

	// Status labels, top-left.
	m_ui.Add<ui::Panel>(gfx::Rect{16, 16, 240, 64});
	m_compass = m_ui.Add<ui::Label>(gfx::Rect{28, 26, 220, 20}, "Facing: South");
	m_position = m_ui.Add<ui::Label>(gfx::Rect{28, 50, 220, 20}, "Position: -");
	m_position->dim = true;

	// Options panel, top-right.
	const float panelW = 250;
	const float px = w - panelW - 16;
	m_ui.Add<ui::Panel>(gfx::Rect{px, 16, panelW, 240});
	m_ui.Add<ui::Label>(gfx::Rect{px + 14, 26, panelW - 28, 20}, "Options");

	m_ui.Add<ui::Slider>(gfx::Rect{px + 14, 84, panelW - 28, 18}, "Volume", 0.0f, 1.0f,
						 1.0f, [this](float v) { m_audio.SetMasterVolume(v); });

	m_ui.Add<ui::Label>(gfx::Rect{px + 14, 116, panelW - 28, 20}, "Torchlight")->dim =
		true;
	m_ui.Add<ui::DropDown>(gfx::Rect{px + 14, 140, panelW - 28, 26},
						   std::vector<std::string>{"Warm flame", "Cold moonfire",
													"Eerie emberlight"},
						   0, [this](int index) {
							   m_audio.Play(m_sfxClick, 0.5f);
							   ApplyTorchPalette(index);
						   });

	m_ui.Add<ui::Button>(gfx::Rect{px + 14, 180, (panelW - 38) / 2, 28}, "Wait",
						 [this] {
							 m_audio.Play(m_sfxClick, 0.5f);
							 m_log->AddLine("You wait. The torches gutter.");
						 });
	m_ui.Add<ui::Button>(gfx::Rect{px + 24 + (panelW - 38) / 2, 180, (panelW - 38) / 2,
								   28},
						 "Help", [this] {
							 m_audio.Play(m_sfxClick, 0.5f);
							 m_log->AddLine("W/S move, A/D strafe, Q/E turn.");
							 m_log->AddLine("Mouse wheel scrolls this log.");
						 });
}

void Game::ApplyTorchPalette(int index) {
	switch (index) {
	case 1:  m_torchColor = {0.45f, 0.65f, 1.0f}; m_log->AddLine("The flames turn cold and blue."); break;
	case 2:  m_torchColor = {0.55f, 1.0f, 0.45f}; m_log->AddLine("An eerie green light spreads."); break;
	default: m_torchColor = {1.0f, 0.62f, 0.28f}; m_log->AddLine("Warm firelight returns."); break;
	}
}

// ============================================================================
// Per-frame simulation
// ============================================================================

void Game::UpdateCamera() {
	m_camera.SetPosition(m_party.EyePosition());
	m_camera.SetYawPitch(m_party.Yaw(), 0.0f);
	m_camera.SetLens(70.0f * kPi / 180.0f,
					 static_cast<float>(m_device.Width()) /
						 static_cast<float>(m_device.Height()),
					 0.05f, 100.0f);
}

// Rebuilds the light list every frame: the carried torch follows the camera,
// wall torches flicker with independent phases, the pillar glows. All
// flicker is product-of-sines — cheap, deterministic, and aperiodic enough.
void Game::UpdateLights(float time) {
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
void Game::AssignShadowSlots() {
	static_assert(gfx::kShadowSlots <= gfx::kMaxPointLights);
	const Vec3 eye = m_party.EyePosition();

	for (gfx::PointLight& light : m_lights.points) light.shadowSlot = -1;

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

void Game::UpdateMonsters(float dt) {
	const Vec3 partyPos = m_party.EyePosition();
	for (Monster& monster : m_monsters) {
		monster.animator.Update(dt);

		// Face the party (blobs don't care).
		const Vec3 pos = m_map.CellCenter(monster.x, monster.z);
		if (monster.kind != 'B')
			monster.yaw = std::atan2(partyPos.x - pos.x, partyPos.z - pos.z);

		// Announce once when the party first comes within one cell.
		const int dx = std::abs(monster.x - m_party.GridX());
		const int dz = std::abs(monster.z - m_party.GridZ());
		if (!monster.announced && std::max(dx, dz) <= 1) {
			monster.announced = true;
			const char* name = m_monsterKinds.at(monster.kind)->name;
			m_log->AddLine(std::format("A {} stirs before you!", name));
			m_audio.Play(m_sfxMonster, 0.7f);
		}
	}
}

void Game::Update(float dt) {
	m_time += dt;

	switch (m_state) {
	case AppState::Loading:
		// Run one load task per frame, but only after the loading screen has
		// been presented at least once.
		if (m_framesRendered > 0 && m_loadIndex < m_loadTasks.size()) {
			m_loadTasks[m_loadIndex].second();
			++m_loadIndex;
			if (m_loadIndex == m_loadTasks.size()) m_state = AppState::Menu;
		}
		return;

	case AppState::Menu:
		// The menu sits on baked title art; nothing in the world simulates.
		(m_menuPage == MenuPage::Main ? m_menuUi : m_settingsUi)
			.Update(m_window.GetInput());
		return;

	case AppState::Playing:
		break;
	}

	// --- Playing -------------------------------------------------------------
	// UI first so it can consume the mouse; keyboard always reaches the party.
	m_ui.Update(m_window.GetInput());
	m_party.HandleInput(m_window.GetInput());
	m_party.Update(dt);
	m_pillarAnimator.Update(dt);
	UpdateMonsters(dt);
	UpdateLights(m_time);
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

	// Reformat the status labels only when they actually change; per-frame
	// string formatting is needless heap churn.
	if (m_party.Facing() != m_lastFacing) {
		m_lastFacing = m_party.Facing();
		m_compass->text = std::format("Facing: {}", Party::FacingName(m_lastFacing));
	}
	if (m_party.GridX() != m_lastGridX || m_party.GridZ() != m_lastGridZ) {
		m_lastGridX = m_party.GridX();
		m_lastGridZ = m_party.GridZ();
		m_position->text = std::format("Position: {}, {}", m_lastGridX, m_lastGridZ);
	}
}

// ============================================================================
// Rendering — the command list arrives from GraphicsDevice::BeginFrame
// already cleared and bound. Loading shows a 2D-only screen; Menu draws the
// idle dungeon behind a darkened overlay; Playing draws the scene + HUD.
// ============================================================================

void Game::DrawSurface(ID3D12GraphicsCommandList* list, const Surface& surface) {
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

// Renders the cube shadow maps for every light that holds a slot this frame.
// Runs before the main pass; the same geometry submission is reused with the
// renderer's shadow pipeline bound.
void Game::RenderShadowMaps(ID3D12GraphicsCommandList* list) {
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

void Game::RenderScene(ID3D12GraphicsCommandList* list) {
	m_renderer.BeginScene(list, m_camera, m_lights, m_atmosphere);
	SubmitSceneGeometry(list);
	// Transparent flame/spark/smoke billboards last, over the opaque scene.
	m_particleBatch->Render(list, m_camera, m_particleScratch);
}

void Game::SubmitSceneGeometry(ID3D12GraphicsCommandList* list) {
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
		const MonsterKind& kind = *m_monsterKinds.at(monster.kind);
		const Vec3 pos = m_map.CellCenter(monster.x, monster.z);
		Mat4 world;
		XMStoreFloat4x4(&world, XMMatrixRotationY(monster.yaw) *
									XMMatrixTranslation(pos.x, 0, pos.z));
		gfx::MaterialParams material;
		material.baseColor = kind.model.materials[0].baseColorFactor;
		if (monster.kind == 'B') {
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

void Game::RenderLoadingScreen() {
	const float w = static_cast<float>(m_device.Width());
	const float h = static_cast<float>(m_device.Height());
	const ui::Theme& theme = m_menuUi.GetTheme();

	// Title.
	const char* title = "DUNGEON";
	const float titleW = m_titleFont.MeasureWidth(title);
	m_titleFont.Draw(m_spriteBatch, title, (w - titleW) * 0.5f, h * 0.32f, theme.accent);

	// Progress bar.
	const float total = static_cast<float>(m_loadTasks.size());
	const float progress = total > 0 ? static_cast<float>(m_loadIndex) / total : 1.0f;
	const gfx::Rect bar{w * 0.3f, h * 0.52f, w * 0.4f, 14.0f};
	m_spriteBatch.DrawRect(bar, theme.control);
	m_spriteBatch.DrawRect({bar.x, bar.y, bar.w * progress, bar.h}, theme.accent);
	ui::DrawBorder(m_spriteBatch, bar, theme.panelBorder);

	// Current step name under the bar.
	const char* step = m_loadIndex < m_loadTasks.size()
						   ? m_loadTasks[m_loadIndex].first
						   : "Entering the dungeon...";
	ui::Font& font = m_menuUi.GetFont();
	const float stepW = font.MeasureWidth(step);
	font.Draw(m_spriteBatch, step, (w - stepW) * 0.5f, bar.y + 28.0f, theme.textDim);
}

void Game::RenderMenuOverlay() {
	const float w = static_cast<float>(m_device.Width());
	const float h = static_cast<float>(m_device.Height());
	const ui::Theme& theme = m_menuUi.GetTheme();

	// Baked title art, stretched to the window, with a light darkening wash
	// so the menu text stays readable over the bright portal.
	m_spriteBatch.DrawSprite({0, 0, w, h}, {0, 0, 1, 1}, *m_titleBackground,
							 {1, 1, 1, 1});
	m_spriteBatch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.30f});

	// Title + subtitle.
	const char* title = "DUNGEON";
	const float titleW = m_titleFont.MeasureWidth(title);
	m_titleFont.Draw(m_spriteBatch, title, (w - titleW) * 0.5f, h * 0.16f, theme.accent);

	const char* subtitle =
		m_menuPage == MenuPage::Settings ? "settings" : "an old-school crawler";
	ui::Font& font = m_menuUi.GetFont();
	const float subW = font.MeasureWidth(subtitle);
	font.Draw(m_spriteBatch, subtitle, (w - subW) * 0.5f, h * 0.16f + 74.0f,
			  theme.textDim);

	(m_menuPage == MenuPage::Main ? m_menuUi : m_settingsUi).Render(m_spriteBatch);
}

void Game::Render(ID3D12GraphicsCommandList* list) {
	m_renderer.NewFrame(m_device.FrameIndex());
	m_spriteBatch.NewFrame(m_device.FrameIndex());
	if (m_particleBatch) m_particleBatch->NewFrame(m_device.FrameIndex());

	// The 3D scene only draws during play; Loading and Menu are 2D-only.
	if (m_state == AppState::Playing) {
		RenderShadowMaps(list);
		RenderScene(list);
	}

	// 2D pass.
	m_spriteBatch.Begin(list, m_device.Width(), m_device.Height());
	switch (m_state) {
	case AppState::Loading: RenderLoadingScreen(); break;
	case AppState::Menu:    RenderMenuOverlay(); break;
	case AppState::Playing: m_ui.Render(m_spriteBatch); break;
	}
	m_spriteBatch.End();

	++m_framesRendered;
}

} // namespace dungeon::game
