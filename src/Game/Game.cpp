#include "Game/Game.h"

#include "Assets/Dds.h"
#include "Assets/File.h"
#include "Core/Assert.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/DungeonMeshBuilder.h"

#include <algorithm>
#include <cctype>
#include <charconv>
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

// Surface texture sets. Order defines the variant index everywhere: the
// texture arrays, the worn block meshes (worn_<name>_<tier>.gltf), and the
// geometry buckets all pair up by position. Must match the worn specs in
// AssetBaker's ModelBaker.cpp.
constexpr const char* kWallTextures[] = {"wall_brick", "wall_stone", "wall_moss"};
constexpr const char* kFloorTextures[] = {"floor_slabs", "floor_cobble"};
constexpr const char* kCeilingTextures[] = {"ceiling_rough", "ceiling_cracked"};

// Font pixel heights at the 900px-tall design window (the layouts in
// BuildMenu/BuildHud are authored against the same design size). Game::Update
// rescales them against the live window height so text tracks the UI.
constexpr float kFontDesignWindowH = 900.0f;
constexpr float kHudFontH = 17.0f;
constexpr float kMenuFontH = 28.0f;
constexpr float kSheetFontH = 22.0f;
constexpr float kTitleFontH = 64.0f;
// Re-bakes wait for the window height to hold still this long, so an
// interactive resize drag doesn't drain the GPU on every size change.
constexpr float kFontSettleDelay = 0.25f;

// The user-editable theme colors (Settings → UI tab). One table drives the
// ini round-trip (theme_<key>=r,g,b,a) and the color-picker grid.
struct ThemeField {
	const char* key;
	const char* label;
	Vec4 ui::Theme::*field;
};
constexpr ThemeField kThemeFields[] = {
	{"panel", "Panel", &ui::Theme::panel},
	{"panelborder", "Border", &ui::Theme::panelBorder},
	{"control", "Control", &ui::Theme::control},
	{"controlhot", "Hot", &ui::Theme::controlHot},
	{"controlactive", "Active", &ui::Theme::controlActive},
	{"text", "Text", &ui::Theme::text},
	{"textdim", "Dim text", &ui::Theme::textDim},
	{"accent", "Accent", &ui::Theme::accent},
};

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
	  m_entities(paths::Asset("maps\\level1.ent"), m_map),
	  m_party(m_map, m_map.StartX(), m_map.StartZ()),
	  m_ui(device, "", kHudFontH), m_menuUi(device, "", kMenuFontH),
	  m_settingsUi(device, "", kMenuFontH), m_pauseUi(device, "", kMenuFontH),
	  m_sheetUi(device, "", kSheetFontH), m_titleFont(device, "", kTitleFontH) {
	LoadSettings();
	m_characters = CreateDefaultParty();
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
										   monster.kind->name));
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
	BuildPauseMenu();
	BuildCharacterSheet();
	BuildBootLoadTasks();
}

// ============================================================================
// Staged loading. Each task is one frame's worth of blocking work; a loading
// screen renders between tasks. The boot list is the bare minimum to reach
// the landing page fast; the heavy dungeon load runs later, behind its own
// progress screen, when the player first starts a game.
// ============================================================================

void Game::BuildBootLoadTasks() {
	m_loadTasks = {
		{"Tuning the echoes",
		 [this] {
			 m_sfxFootstep = LoadSound("footstep.wav");
			 m_sfxBump = LoadSound("bump.wav");
			 m_sfxTurn = LoadSound("turn.wav");
			 m_sfxClick = LoadSound("click.wav");
			 m_sfxMonster = LoadSound("monster.wav");
		 }},
		{"Painting the title",
		 [this] {
			 m_titleBackground =
				 LoadTextureFile(m_device, paths::Asset("textures\\title_bg"));
		 }},
	};
}

// Order matters: textures register their variant counts before the geometry
// task buckets cells by variant. The texture work is split one task per
// material — the scanned sets are the bulk of the load (~300 MB at Ultra),
// and per-material tasks keep the progress bar moving through them.
void Game::BuildGameLoadTasks() {
	m_loadTasks.clear();
	m_loadTasks.emplace_back("Quarrying stone blocks", [this] { LoadDungeonBlocks(); });

	auto addTextureTasks = [this](Surface& surface, std::span<const char* const> names,
								  float heightScale) {
		for (size_t i = 0; i < names.size(); ++i) {
			const char* name = names[i];
			const bool first = i == 0; // first material resets the set
			std::string label = std::format("Weaving the stonework ({})", name);
			std::ranges::replace(label, '_', ' ');
			m_loadTasks.emplace_back(
				std::move(label), [this, &surface, name, heightScale, first] {
					if (first) {
						surface.albedo.clear();
						surface.normal.clear();
						surface.heightScale = heightScale;
					}
					LoadSurfaceMaterial(surface, name);
				});
		}
	};
	addTextureTasks(m_walls, kWallTextures, 0.055f);
	addTextureTasks(m_floors, kFloorTextures, 0.045f);
	addTextureTasks(m_ceilings, kCeilingTextures, 0.035f);

	m_loadTasks.emplace_back("Raising the dungeon", [this] { BuildDungeonMeshes(); });
	m_loadTasks.emplace_back("Carving the serpent pillar", [this] {
		m_pillarModel = LoadModelOrDie("pillar.gltf");
		m_pillarMesh = std::make_unique<gfx::Mesh>(m_device, m_pillarModel.meshes[0]);
		m_pillarAnimator = anim::Animator(&m_pillarModel.skeleton, &m_pillarModel.clips);
		m_pillarAnimator.Play("sway");
		m_pillarPos = m_map.CellCenter(m_map.StartX(), m_map.StartZ() + 2);
	});
	m_loadTasks.emplace_back("Waking the monsters", [this] { LoadMonsters(); });
	m_loadTasks.emplace_back("Kindling the fires", [this] {
		auto sconce = LoadModelOrDie("sconce.gltf");
		m_sconceMesh = std::make_unique<gfx::Mesh>(m_device, sconce.meshes[0]);
		m_sconceColor = sconce.materials[0].baseColorFactor;
		auto brazier = LoadModelOrDie("brazier.gltf");
		m_brazierMesh = std::make_unique<gfx::Mesh>(m_device, brazier.meshes[0]);
		m_brazierColor = brazier.materials[0].baseColorFactor;
		m_particleBatch = std::make_unique<gfx::ParticleBatch>(m_device);
		BuildFires();
	});
	m_loadTasks.emplace_back("Stirring the dust", [this] {
		// Per-cell turbidity as a top-down density grid: one texel per
		// dungeon cell, R channel; bilinear filtering blends region
		// borders. The scene shader raymarches it (see scene.hlsl).
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
	});
	m_loadTasks.emplace_back("Painting the party", [this] {
		m_portraitTextures.clear();
		for (Character& member : m_characters) {
			std::string stem = "portrait_" + member.name;
			std::ranges::transform(stem, stem.begin(), [](char c) {
				return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			});
			auto texture =
				TryLoadTextureFile(m_device, paths::Asset("textures\\" + stem));
			if (!texture)
				log::Warn("missing {}.png — falling back to the initial tile", stem);
			member.portrait = texture.get();
			m_portraitTextures.push_back(std::move(texture));
		}
	});
	m_loadTasks.emplace_back("Lighting the torches", [this] {
		BuildHud();
		log::Info("Game loaded: {}x{} dungeon, {} torches, {} monsters",
				  m_map.Width(), m_map.Height(), m_map.TorchCells().size(),
				  m_monsters.size());
	});
}

// Runs one queued task per rendered frame (never before the current loading
// screen has been presented once); returns true when the queue is done.
bool Game::RunLoadTasks() {
	if (m_framesRendered > m_stateFrameMark && m_loadIndex < m_loadTasks.size()) {
		m_loadTasks[m_loadIndex].second();
		++m_loadIndex;
	}
	return m_loadIndex == m_loadTasks.size();
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
	// The old dungeon uses the worn, crumbling block set — one mesh per
	// texture variant, displaced at bake time by that texture's height map
	// so geometry relief matches the painted bricks/slabs. The clean
	// *_block.gltf models remain baked for newer areas of the game.
	auto load = [&](std::vector<assets::MeshData>& blocks,
					std::span<const char* const> names) {
		blocks.clear();
		for (const char* name : names)
			blocks.push_back(
				LoadModelOrDie(std::format("worn_{}_{}.gltf", name, QualitySuffix()))
					.meshes[0]);
	};
	load(m_wallBlocks, kWallTextures);
	load(m_floorBlocks, kFloorTextures);
	load(m_ceilingBlocks, kCeilingTextures);
}

void Game::SetQuality(Quality quality) {
	if (quality == m_quality) return;
	const std::string oldTextureSuffix = QualityTextureSuffix();
	m_quality = quality;
	const bool textureResChanged = oldTextureSuffix != QualityTextureSuffix();
	SaveSettings();

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

void Game::LoadSettings() {
	auto bytes = assets::ReadBinaryFile(paths::ExecutableDir() + "\\settings.ini");
	if (!bytes) return; // first run: keep the defaults
	const std::string text(bytes->begin(), bytes->end());

	const size_t qpos = text.find("quality=");
	if (qpos != std::string::npos && qpos + 8 < text.size()) {
		const char digit = text[qpos + 8];
		if (digit >= '0' && digit <= '3')
			m_quality = static_cast<Quality>(digit - '0');
	}

	const size_t vpos = text.find("volume=");
	if (vpos != std::string::npos) {
		float volume = 1.0f;
		if (std::from_chars(text.data() + vpos + 7, text.data() + text.size(),
							volume)
				.ec == std::errc{})
			m_audio.SetMasterVolume(std::clamp(volume, 0.0f, 1.0f));
	}

	const size_t spos = text.find("barscale=");
	if (spos != std::string::npos) {
		float scale = 1.0f;
		if (std::from_chars(text.data() + spos + 9, text.data() + text.size(),
							scale)
				.ec == std::errc{})
			m_partyBarScale = std::clamp(scale, 0.5f, 1.5f);
	}

	const size_t opos = text.find("baropacity=");
	if (opos != std::string::npos) {
		float opacity = 1.0f;
		if (std::from_chars(text.data() + opos + 11, text.data() + text.size(),
							opacity)
				.ec == std::errc{})
			m_partyBarOpacity = std::clamp(opacity, 0.0f, 1.0f);
	}

	// Theme colors: theme_<key>=r,g,b,a — a color only applies if all four
	// channels parse (a malformed line keeps that entry's default).
	for (const ThemeField& field : kThemeFields) {
		const std::string key = std::format("theme_{}=", field.key);
		const size_t pos = text.find(key);
		if (pos == std::string::npos) continue;
		Vec4 color = m_theme.*(field.field);
		size_t cursor = pos + key.size();
		bool ok = true;
		for (int i = 0; i < 4 && ok; ++i) {
			float value = 0.0f;
			const auto result = std::from_chars(text.data() + cursor,
												text.data() + text.size(), value);
			ok = result.ec == std::errc{};
			if (!ok) break;
			(&color.x)[i] = std::clamp(value, 0.0f, 1.0f);
			cursor = static_cast<size_t>(result.ptr - text.data());
			if (i < 3) {
				ok = cursor < text.size() && text[cursor] == ',';
				++cursor;
			}
		}
		if (ok) m_theme.*(field.field) = color;
	}
	ApplyTheme();
}

void Game::ApplyTheme() {
	for (ui::UIContext* ctx :
		 {&m_ui, &m_menuUi, &m_settingsUi, &m_pauseUi, &m_sheetUi})
		ctx->SetTheme(m_theme);
}

void Game::SaveSettings() const {
	std::string text =
		std::format("quality={}\nvolume={:.2f}\nbarscale={:.2f}\nbaropacity={:.2f}\n",
					static_cast<int>(m_quality), m_audio.MasterVolume(),
					m_partyBarScale, m_partyBarOpacity);
	for (const ThemeField& field : kThemeFields) {
		const Vec4& c = m_theme.*(field.field);
		text += std::format("theme_{}={:.3f},{:.3f},{:.3f},{:.3f}\n", field.key,
							c.x, c.y, c.z, c.w);
	}
	if (!assets::WriteBinaryFile(paths::ExecutableDir() + "\\settings.ini",
								 text.data(), text.size()))
		log::Warn("Could not write settings.ini");
}

// Loads one material's albedo + normal pair at the current quality tier and
// appends it to the surface's variant arrays.
void Game::LoadSurfaceMaterial(Surface& surface, const char* name) {
	const char* res = QualityTextureSuffix();
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

void Game::LoadTextureSet(Surface& surface, std::span<const char* const> names,
						  float heightScale) {
	surface.albedo.clear(); // quality hot-swap reuses the same Surface objects
	surface.normal.clear();
	surface.heightScale = heightScale;
	for (const char* name : names) LoadSurfaceMaterial(surface, name);
}

void Game::LoadAllSurfaceTextures() {
	LoadTextureSet(m_walls, kWallTextures, 0.055f);
	LoadTextureSet(m_floors, kFloorTextures, 0.045f);
	LoadTextureSet(m_ceilings, kCeilingTextures, 0.035f);
}

void Game::BuildDungeonMeshes() {
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
void Game::LoadMonsters() {
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
// Widget bounds are normalized fractions of their container (see Widget.h).
// Layouts here are still authored in design pixels for readability and
// converted with this helper; they scale with the live window size.
static gfx::Rect Norm(const gfx::Rect& designPx, const gfx::Rect& container) {
	return {(designPx.x - container.x) / container.w,
			(designPx.y - container.y) / container.h, designPx.w / container.w,
			designPx.h / container.h};
}

void Game::BuildMenu() {
	const float w = static_cast<float>(m_window.Width());
	const float h = static_cast<float>(m_window.Height());
	const gfx::Rect window{0, 0, w, h};

	const float menuW = 420.0f;
	const float itemH = 58.0f;
	auto* menu = m_menuUi.Add<ui::MenuList>(
		Norm({(w - menuW) * 0.5f, h * 0.42f, menuW, itemH * 4}, window),
		1.0f / 4.0f); // item height: one quarter of the list

	menu->AddItem("Continue");           // not implemented yet
	menu->AddItem("Start New Game", [this] {
		m_audio.Play(m_sfxClick, 0.6f);
		if (m_gameLoaded) {
			StartNewGame();
		} else {
			// First start: the boot load only fetched menu essentials, so
			// the dungeon loads now behind its own progress screen.
			BuildGameLoadTasks();
			m_loadIndex = 0;
			m_state = AppState::LoadingGame;
			m_stateFrameMark = m_framesRendered;
		}
	});
	menu->AddItem("Load");               // not implemented yet
	menu->AddItem("Settings", [this] {
		m_audio.Play(m_sfxClick, 0.5f);
		m_menuPage = MenuPage::Settings;
	});

	// Settings page: Game / Video / Audio tabs over a shared page area, with
	// a Back button beneath. Tab children are fractions of the PAGE (the
	// area below the strip); each row leaves room for the 28px settings font.
	// Sized generously — settings keep accumulating. The block spans from
	// just under the subtitle (ends ~246 design px) to just above the Back
	// button, which itself must clear the 900px design window.
	const float tabsW = 760.0f;
	const float tabsH = 540.0f;
	const float stripH = 48.0f;
	const float tabsX = (w - tabsW) * 0.5f;
	const float tabsY = h * 0.29f;
	auto* tabs = m_settingsUi.Add<ui::TabControl>(
		Norm({tabsX, tabsY, tabsW, tabsH}, window), stripH / tabsH);
	const size_t tabGame = tabs->AddTab("Game");
	const size_t tabVideo = tabs->AddTab("Video");
	const size_t tabAudio = tabs->AddTab("Audio");
	const size_t tabUi = tabs->AddTab("UI");
	const gfx::Rect page{0, 0, tabsW, tabsH - stripH}; // child design space
	const float pad = 24.0f;
	const float rowW = page.w - 2 * pad;

	// Game: nothing to configure yet.
	tabs->AddChild<ui::Label>(tabGame, Norm({pad, pad, rowW, 28}, page),
							  "Nothing here yet.")
		->dim = true;

	// Video: quality tier (hot-swaps meshes/textures in place).
	tabs->AddChild<ui::Label>(tabVideo, Norm({pad, pad, rowW, 28}, page), "Quality")
		->dim = true;
	tabs->AddChild<ui::DropDown>(
		tabVideo, Norm({pad, pad + 38, rowW, 40}, page),
		std::vector<std::string>{"Low", "Medium", "High", "Ultra"},
		static_cast<int>(m_quality), [this](int index) {
			m_audio.Play(m_sfxClick, 0.5f);
			SetQuality(static_cast<Quality>(index));
		});

	// Audio: master volume (the slider draws its own label above the track).
	// Live while dragging; persisted once on release.
	auto* volume = tabs->AddChild<ui::Slider>(
		tabAudio, Norm({pad, pad + 38, rowW, 22}, page), "Volume", 0.0f, 1.0f,
		m_audio.MasterVolume(), [this](float v) { m_audio.SetMasterVolume(v); });
	volume->onRelease = [this] { SaveSettings(); };

	// UI → Party Bar: scale resizes the bar live (about its top center) and
	// opacity fades the slot backgrounds. Both apply while dragging and
	// persist on release; safe before the HUD exists (the panel list is empty
	// until the first game load).
	tabs->AddChild<ui::Label>(tabUi, Norm({pad, pad, rowW, 28}, page), "Party Bar");
	auto* barScale = tabs->AddChild<ui::Slider>(
		tabUi, Norm({pad, pad + 70, rowW, 22}, page), "Scale", 0.5f, 1.5f,
		m_partyBarScale, [this](float v) {
			m_partyBarScale = v;
			ApplyPartyBarScale();
		});
	barScale->onRelease = [this] { SaveSettings(); };
	auto* barOpacity = tabs->AddChild<ui::Slider>(
		tabUi, Norm({pad, pad + 126, rowW, 22}, page), "Background opacity", 0.0f,
		1.0f, m_partyBarOpacity, [this](float v) {
			m_partyBarOpacity = v;
			for (CharacterPanel* panel : m_partyPanels)
				panel->backgroundOpacity = v;
		});
	barOpacity->onRelease = [this] { SaveSettings(); };

	// UI → Theme Colors: the eight shared control colors (kThemeFields), two
	// pickers per row. Edits recolor every context live (ApplyTheme) and
	// persist once when a picker's popup closes.
	tabs->AddChild<ui::Label>(tabUi, Norm({pad, pad + 172, rowW, 28}, page),
							  "Theme Colors");
	const float colW = (rowW - 16.0f) * 0.5f;
	size_t themeIndex = 0;
	for (const ThemeField& field : kThemeFields) {
		const gfx::Rect cell{pad + (colW + 16.0f) * static_cast<float>(themeIndex % 2),
							 pad + 216.0f + 46.0f * static_cast<float>(themeIndex / 2),
							 colW, 36.0f};
		auto* picker = tabs->AddChild<ui::ColorPicker>(
			tabUi, Norm(cell, page), field.label, m_theme.*(field.field),
			[this, member = field.field](const Vec4& color) {
				m_theme.*member = color;
				ApplyTheme();
			});
		picker->onClose = [this] { SaveSettings(); };
		++themeIndex;
	}

	const float backW = 220.0f;
	m_settingsUi.Add<ui::Button>(
		Norm({(w - backW) * 0.5f, tabsY + tabsH + 28, backW, 44}, window), "Back",
		[this] {
			m_audio.Play(m_sfxClick, 0.5f);
			m_menuPage = MenuPage::Main;
		});
}

// In-game pause menu (Esc while playing): same look as the landing list,
// drawn over the frozen scene under a dark wash (RenderPauseOverlay).
// Settings routes to the same shared page as the landing menu; Save/Load
// wait on the save system.
void Game::BuildPauseMenu() {
	const float w = static_cast<float>(m_window.Width());
	const float h = static_cast<float>(m_window.Height());
	const gfx::Rect window{0, 0, w, h};

	const float menuW = 420.0f;
	const float itemH = 58.0f;
	auto* menu = m_pauseUi.Add<ui::MenuList>(
		Norm({(w - menuW) * 0.5f, h * 0.42f, menuW, itemH * 5}, window),
		1.0f / 5.0f);
	menu->AddItem("Save");               // not implemented yet
	menu->AddItem("Load");               // not implemented yet
	menu->AddItem("Settings", [this] {
		m_audio.Play(m_sfxClick, 0.5f);
		m_menuPage = MenuPage::Settings;
	});
	menu->AddItem("Exit", [this] {
		m_audio.Play(m_sfxClick, 0.5f);
		m_quitRequested = true;
	});
	menu->AddItem("Back", [this] {
		m_audio.Play(m_sfxClick, 0.5f);
		m_state = AppState::Playing;
	});
}

// Character details page (clicking a party-bar portrait): the sheet widget
// draws the page itself; prev/next buttons cycle the roster and Back (or
// Esc) resumes play. Like the pause menu it overlays the frozen scene.
void Game::BuildCharacterSheet() {
	const float w = static_cast<float>(m_window.Width());
	const float h = static_cast<float>(m_window.Height());
	const gfx::Rect window{0, 0, w, h};

	const float sheetW = 620.0f;
	const float sheetH = 520.0f;
	const float sx = (w - sheetW) * 0.5f;
	const float sy = (h - sheetH) * 0.5f - 24.0f;
	m_sheet = m_sheetUi.Add<CharacterSheet>(Norm({sx, sy, sheetW, sheetH}, window),
											&m_titleFont);

	const float btnY = sy + sheetH + 16.0f;
	m_sheetUi.Add<ui::Button>(Norm({sx, btnY, 64, 40}, window), "<", [this] {
		const size_t count = m_characters.size();
		OpenCharacterSheet((m_sheetIndex + count - 1) % count);
	});
	m_sheetUi.Add<ui::Button>(Norm({sx + sheetW - 64, btnY, 64, 40}, window), ">",
							  [this] {
								  OpenCharacterSheet((m_sheetIndex + 1) %
													 m_characters.size());
							  });
	m_sheetUi.Add<ui::Button>(Norm({(w - 180.0f) * 0.5f, btnY, 180, 40}, window),
							  "Back", [this] {
								  m_audio.Play(m_sfxClick, 0.5f);
								  m_state = AppState::Playing;
							  });
}

void Game::OpenCharacterSheet(size_t index) {
	m_audio.Play(m_sfxClick, 0.5f);
	m_sheetIndex = index;
	m_sheet->SetCharacter(m_characters[index]);
	m_state = AppState::CharacterSheet;
}

void Game::StartNewGame() {
	m_party.Reset(m_map.StartX(), m_map.StartZ());
	for (Monster& monster : m_monsters) monster.announced = false;
	ApplyTorchPalette(0);

	// Fresh stats for the same roster — element-wise so the addresses the
	// party-bar panels and the sheet point at stay valid, keeping the loaded
	// portrait (the defaults carry a null one).
	const std::vector<Character> fresh = CreateDefaultParty();
	for (size_t i = 0; i < m_characters.size() && i < fresh.size(); ++i) {
		const gfx::Texture* portrait = m_characters[i].portrait;
		m_characters[i] = fresh[i];
		m_characters[i].portrait = portrait;
	}
	m_sheet->SetCharacter(m_characters[m_sheetIndex]);

	m_log->Clear();
	m_log->AddLine("You descend into the dungeon...");
	m_log->AddLine("Something shuffles in the dark.");
	m_log->AddLine("W/S move, A/D strafe, Q/E turn.");

	m_lastFacing = m_lastGridX = m_lastGridZ = -1; // force HUD label refresh
	m_state = AppState::Playing;
	log::Info("New game started");
}

// ============================================================================
// HUD — authored in design pixels from the initial window size, stored as
// window fractions (Norm), so it scales with the screen. Widgets the game
// updates later are kept as raw pointers (m_log, m_compass, m_position); the
// UIContext owns all widgets.
// ============================================================================
void Game::BuildHud() {
	const float w = static_cast<float>(m_window.Width());
	const float h = static_cast<float>(m_window.Height());
	const gfx::Rect window{0, 0, w, h};

	// Party bar across the top: one clickable slot per member (portrait,
	// name, health/stamina/mana). Clicking opens the character sheet. The
	// slot rects come from ApplyPartyBarScale (called below) so the Settings →
	// UI scale slider can resize the bar at runtime; the layout always
	// reserves four slots so a short roster keeps its slot size.
	m_hudDesignW = w;
	m_hudDesignH = h;
	m_partyPanels.clear();
	for (size_t i = 0; i < m_characters.size() && i < 4; ++i) {
		auto* panel = m_ui.Add<CharacterPanel>(gfx::Rect{}, &m_characters[i],
											   &m_titleFont,
											   [this, i] { OpenCharacterSheet(i); });
		panel->backgroundOpacity = m_partyBarOpacity;
		m_partyPanels.push_back(panel);
	}
	const float barH = 96.0f; // design height at scale 1
	const float belowBar = 16.0f + barH + 16.0f; // top edge for the side panels

	// Widgets under the bar register here so ApplyPartyBarScale can shift
	// them when the bar grows or shrinks (design Y recovered from the
	// fraction Norm just stored).
	m_belowBarWidgets.clear();
	auto below = [this, h](ui::Widget* widget) {
		m_belowBarWidgets.push_back({widget, widget->bounds.y * h});
	};

	// Message log, bottom-left.
	m_log = m_ui.Add<ui::TextOutput>(Norm({16, h - 200, 520, 184}, window));

	// Status labels, below the party bar on the left.
	below(m_ui.Add<ui::Panel>(Norm({16, belowBar, 240, 64}, window)));
	m_compass = m_ui.Add<ui::Label>(Norm({28, belowBar + 10, 220, 20}, window),
									"Facing: South");
	below(m_compass);
	m_position = m_ui.Add<ui::Label>(Norm({28, belowBar + 34, 220, 20}, window),
									 "Position: -");
	m_position->dim = true;
	below(m_position);

	// Options panel, below the party bar on the right.
	const float panelW = 250;
	const float px = w - panelW - 16;
	below(m_ui.Add<ui::Panel>(Norm({px, belowBar, panelW, 176}, window)));
	below(m_ui.Add<ui::Label>(Norm({px + 14, belowBar + 10, panelW - 28, 20}, window),
							  "Options"));

	auto* torchLabel = m_ui.Add<ui::Label>(
		Norm({px + 14, belowBar + 40, panelW - 28, 20}, window), "Torchlight");
	torchLabel->dim = true;
	below(torchLabel);
	below(m_ui.Add<ui::DropDown>(
		Norm({px + 14, belowBar + 64, panelW - 28, 26}, window),
		std::vector<std::string>{"Warm flame", "Cold moonfire",
								 "Eerie emberlight"},
		0, [this](int index) {
			m_audio.Play(m_sfxClick, 0.5f);
			ApplyTorchPalette(index);
		}));

	below(m_ui.Add<ui::Button>(
		Norm({px + 14, belowBar + 104, (panelW - 38) / 2, 28}, window), "Wait",
		[this] {
			m_audio.Play(m_sfxClick, 0.5f);
			m_log->AddLine("You wait. The torches gutter.");
		}));
	below(m_ui.Add<ui::Button>(
		Norm({px + 24 + (panelW - 38) / 2, belowBar + 104, (panelW - 38) / 2, 28},
			 window),
		"Help", [this] {
			m_audio.Play(m_sfxClick, 0.5f);
			m_log->AddLine("W/S move, A/D strafe, Q/E turn.");
			m_log->AddLine("Mouse wheel scrolls this log.");
		}));

	ApplyPartyBarScale();
}

// Re-derives the party-bar slot rects from m_partyBarScale — anchored to the
// top center, so scale 1 reproduces the design layout above (16px margins,
// 10px gaps, 96px tall) — and shifts the status/options widgets by the bar's
// growth so they stay 16px clear of its bottom edge. No-op until BuildHud has
// run (the settings sliders exist from boot, the HUD only after a game load).
void Game::ApplyPartyBarScale() {
	if (m_partyPanels.empty()) return;
	const float w = m_hudDesignW;
	const float h = m_hudDesignH;
	const gfx::Rect window{0, 0, w, h};
	const float s = m_partyBarScale;
	// The bar already spans the full window at scale 1, so width is pinned
	// there: scales above 1 grow the bar vertically only (the portraits and
	// resource bars follow the slot height).
	const float ws = std::min(s, 1.0f);
	const float slotGap = 10.0f * ws;
	const float slotW = (w - 2 * 16.0f - 3 * 10.0f) / 4.0f * ws;
	const float barX = (w - 4 * slotW - 3 * slotGap) * 0.5f;
	for (size_t i = 0; i < m_partyPanels.size(); ++i)
		m_partyPanels[i]->bounds =
			Norm({barX + static_cast<float>(i) * (slotW + slotGap), 16.0f, slotW,
				  96.0f * s},
				 window);

	const float shift = 96.0f * (s - 1.0f);
	for (auto& [widget, designY] : m_belowBarWidgets)
		widget->bounds.y = (designY + shift) / h;
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
		if (monster.kind->name != "blob")
			monster.yaw = std::atan2(partyPos.x - pos.x, partyPos.z - pos.z);

		// Announce once when the party first comes within one cell.
		const int dx = std::abs(monster.x - m_party.GridX());
		const int dz = std::abs(monster.z - m_party.GridZ());
		if (!monster.announced && std::max(dx, dz) <= 1) {
			monster.announced = true;
			m_log->AddLine(std::format("A {} stirs before you!", monster.kind->name));
			m_audio.Play(m_sfxMonster, 0.7f);
		}
	}
}

void Game::Update(float dt) {
	m_time += dt;

	// Keep fonts in step with the window height so text scales with the
	// normalized UI. Re-bakes are debounced until the height has settled
	// for kFontSettleDelay (each one drains the GPU), then run between
	// frames (never while a command list records); until then text simply
	// renders at the old size inside the already-scaled widgets.
	const float windowH = static_cast<float>(m_window.Height());
	if (windowH != m_fontWindowH) {
		m_fontWindowH = windowH;
		m_fontSettle = 0.0f;
	} else if (m_fontSettle < kFontSettleDelay &&
			   (m_fontSettle += dt) >= kFontSettleDelay) {
		const float fontScale = windowH / kFontDesignWindowH;
		m_ui.GetFont().SetHeight(kHudFontH * fontScale);
		m_menuUi.GetFont().SetHeight(kMenuFontH * fontScale);
		m_settingsUi.GetFont().SetHeight(kMenuFontH * fontScale);
		m_pauseUi.GetFont().SetHeight(kMenuFontH * fontScale);
		m_sheetUi.GetFont().SetHeight(kSheetFontH * fontScale);
		m_titleFont.SetHeight(kTitleFontH * fontScale);
	}

	const Input& input = m_window.GetInput();
	const float winW = static_cast<float>(m_window.Width());

	switch (m_state) {
	case AppState::Loading:
		if (input.WasKeyPressed(VK_ESCAPE)) m_quitRequested = true;
		if (RunLoadTasks()) m_state = AppState::Menu;
		return;

	case AppState::Menu:
		// The menu sits on baked title art; nothing in the world simulates.
		// Esc backs out of settings, or quits from the landing list.
		if (input.WasKeyPressed(VK_ESCAPE)) {
			if (m_menuPage == MenuPage::Settings) m_menuPage = MenuPage::Main;
			else m_quitRequested = true;
		}
		(m_menuPage == MenuPage::Main ? m_menuUi : m_settingsUi)
			.Update(input, winW, windowH);
		return;

	case AppState::LoadingGame:
		if (input.WasKeyPressed(VK_ESCAPE)) m_quitRequested = true;
		if (RunLoadTasks()) {
			m_gameLoaded = true;
			StartNewGame(); // sets AppState::Playing
		}
		return;

	case AppState::Paused:
		// The world is frozen — only the pause menu (or the shared settings
		// page) updates. Esc backs out of settings, or resumes play.
		if (input.WasKeyPressed(VK_ESCAPE)) {
			m_audio.Play(m_sfxClick, 0.5f);
			if (m_menuPage == MenuPage::Settings) m_menuPage = MenuPage::Main;
			else m_state = AppState::Playing;
			return;
		}
		(m_menuPage == MenuPage::Main ? m_pauseUi : m_settingsUi)
			.Update(input, winW, windowH);
		return;

	case AppState::CharacterSheet:
		// Frozen like Paused; only the sheet page updates. Esc resumes.
		if (input.WasKeyPressed(VK_ESCAPE)) {
			m_audio.Play(m_sfxClick, 0.5f);
			m_state = AppState::Playing;
			return;
		}
		m_sheetUi.Update(input, winW, windowH);
		return;

	case AppState::Playing:
		break;
	}

	// --- Playing -------------------------------------------------------------
	// Esc freezes the world and opens the pause menu.
	if (input.WasKeyPressed(VK_ESCAPE)) {
		m_audio.Play(m_sfxClick, 0.5f);
		m_menuPage = MenuPage::Main;
		m_state = AppState::Paused;
		return;
	}

	// UI first so it can consume the mouse; keyboard always reaches the party.
	m_ui.Update(input, winW, windowH);
	// A portrait click may have opened the character sheet — freeze now
	// rather than simulating one more frame.
	if (m_state != AppState::Playing) return;
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
// already cleared and bound. Loading, Menu, and LoadingGame are 2D-only
// (title art / progress screens); Playing draws the 3D scene + HUD.
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

// Progress bar + current step name, shared by both loading screens.
void Game::DrawLoadProgress(float barY) {
	const float w = static_cast<float>(m_device.Width());
	const float h = static_cast<float>(m_device.Height());
	const ui::Theme& theme = m_menuUi.GetTheme();

	const float total = static_cast<float>(m_loadTasks.size());
	const float progress = total > 0 ? static_cast<float>(m_loadIndex) / total : 1.0f;
	const gfx::Rect bar{w * 0.3f, barY, w * 0.4f, h * (14.0f / kFontDesignWindowH)};
	m_spriteBatch.DrawRect(bar, theme.control);
	m_spriteBatch.DrawRect({bar.x, bar.y, bar.w * progress, bar.h}, theme.accent);
	ui::DrawBorder(m_spriteBatch, bar, theme.panelBorder);

	const std::string_view step = m_loadIndex < m_loadTasks.size()
									  ? std::string_view(m_loadTasks[m_loadIndex].first)
									  : std::string_view("Entering the dungeon...");
	ui::Font& font = m_menuUi.GetFont();
	const float stepW = font.MeasureWidth(step);
	font.Draw(m_spriteBatch, step, (w - stepW) * 0.5f, bar.y + bar.h * 2.0f,
			  theme.textDim);
}

void Game::RenderLoadingScreen() {
	const float w = static_cast<float>(m_device.Width());
	const float h = static_cast<float>(m_device.Height());
	const ui::Theme& theme = m_menuUi.GetTheme();

	const char* title = "DUNGEON";
	const float titleW = m_titleFont.MeasureWidth(title);
	m_titleFont.Draw(m_spriteBatch, title, (w - titleW) * 0.5f, h * 0.32f, theme.accent);

	DrawLoadProgress(h * 0.52f);
}

// Shown between "Start New Game" and Playing: the title art again, washed
// darker than the menu so the bar and step names read clearly.
void Game::RenderGameLoadingScreen() {
	const float w = static_cast<float>(m_device.Width());
	const float h = static_cast<float>(m_device.Height());
	const ui::Theme& theme = m_menuUi.GetTheme();

	m_spriteBatch.DrawSprite({0, 0, w, h}, {0, 0, 1, 1}, *m_titleBackground,
							 {1, 1, 1, 1});
	m_spriteBatch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.55f});

	const char* title = "DUNGEON";
	const float titleW = m_titleFont.MeasureWidth(title);
	m_titleFont.Draw(m_spriteBatch, title, (w - titleW) * 0.5f, h * 0.16f, theme.accent);

	const char* subtitle = "descending...";
	ui::Font& font = m_menuUi.GetFont();
	const float subW = font.MeasureWidth(subtitle);
	font.Draw(m_spriteBatch, subtitle, (w - subW) * 0.5f,
			  h * (0.16f + 74.0f / kFontDesignWindowH), theme.textDim);

	DrawLoadProgress(h * 0.56f);
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
	font.Draw(m_spriteBatch, subtitle, (w - subW) * 0.5f,
			  h * (0.16f + 74.0f / kFontDesignWindowH), theme.textDim);

	(m_menuPage == MenuPage::Main ? m_menuUi : m_settingsUi)
		.Render(m_spriteBatch, w, h);
}

// Esc pause: the frozen scene stays up behind a dark wash, with a menu list
// like the landing page. The settings page is the same one the landing menu
// uses (m_menuPage routes both).
void Game::RenderPauseOverlay() {
	const float w = static_cast<float>(m_device.Width());
	const float h = static_cast<float>(m_device.Height());
	const ui::Theme& theme = m_pauseUi.GetTheme();

	m_spriteBatch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.55f});

	const char* title = "PAUSED";
	const float titleW = m_titleFont.MeasureWidth(title);
	m_titleFont.Draw(m_spriteBatch, title, (w - titleW) * 0.5f, h * 0.16f,
					 theme.accent);

	if (m_menuPage == MenuPage::Settings) {
		const char* subtitle = "settings";
		ui::Font& font = m_pauseUi.GetFont();
		const float subW = font.MeasureWidth(subtitle);
		font.Draw(m_spriteBatch, subtitle, (w - subW) * 0.5f,
				  h * (0.16f + 74.0f / kFontDesignWindowH), theme.textDim);
	}

	(m_menuPage == MenuPage::Main ? m_pauseUi : m_settingsUi)
		.Render(m_spriteBatch, w, h);
}

// Portrait click: the frozen scene under a dark wash, with the sheet page
// (and its prev/next/Back buttons) on top.
void Game::RenderCharacterSheetOverlay() {
	const float w = static_cast<float>(m_device.Width());
	const float h = static_cast<float>(m_device.Height());

	m_spriteBatch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.55f});
	m_sheetUi.Render(m_spriteBatch, w, h);
}

void Game::Render(ID3D12GraphicsCommandList* list) {
	m_renderer.NewFrame(m_device.FrameIndex());
	m_spriteBatch.NewFrame(m_device.FrameIndex());
	if (m_particleBatch) m_particleBatch->NewFrame(m_device.FrameIndex());

	// The 3D scene draws during play and under the pause/character-sheet
	// overlays (frozen); Loading and Menu are 2D-only.
	if (m_state == AppState::Playing || m_state == AppState::Paused ||
		m_state == AppState::CharacterSheet) {
		RenderShadowMaps(list);
		RenderScene(list);
	}

	// 2D pass.
	m_spriteBatch.Begin(list, m_device.Width(), m_device.Height());
	switch (m_state) {
	case AppState::Loading:     RenderLoadingScreen(); break;
	case AppState::Menu:        RenderMenuOverlay(); break;
	case AppState::LoadingGame: RenderGameLoadingScreen(); break;
	case AppState::Playing:
		m_ui.Render(m_spriteBatch, static_cast<float>(m_device.Width()),
					static_cast<float>(m_device.Height()));
		break;
	case AppState::Paused:      RenderPauseOverlay(); break;
	case AppState::CharacterSheet: RenderCharacterSheetOverlay(); break;
	}
	m_spriteBatch.End();

	++m_framesRendered;
}

} // namespace dungeon::game
