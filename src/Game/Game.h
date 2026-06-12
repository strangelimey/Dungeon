// ============================================================================
// Game/Game.h — the dungeon crawler itself.
//
// Game is the only class that sees every engine module at once. It owns the
// world (map, party, monsters, lights), the menus and HUD, the loaded
// assets, and a small app state machine:
//
//   Loading     — boot load: just the menu essentials (title art, click
//                 sounds), one step per frame, so the landing page appears
//                 fast even on a cold cache
//   Menu        — landing page over baked title art (assets/textures/
//                 title_bg.png); mouse hover or keyboard selects an entry
//   LoadingGame — the heavy dungeon load (meshes, scanned textures, world
//                 build), entered from "Start New Game" the first time;
//                 staged one task per frame behind its own progress screen
//   Playing     — the crawler: UI input → party movement → animators →
//                 lights (loading is staged, not threaded, in both states)
//   Paused      — Esc while playing: the world freezes (no simulation) and
//                 a pause menu (Save/Load/Settings/Exit/Back) draws over
//                 the frozen scene; Esc backs out / resumes
//   CharacterSheet — clicking a party-bar portrait while playing: the world
//                 freezes like Paused and the character details page draws
//                 over it (prev/next cycle members, Esc/Back resumes)
//
// Everything binary loads from the assets/ directory next to the exe
// (regenerate with tools/AssetBaker). Engine modules know nothing about
// dungeons — all gameplay rules live in this module.
// ============================================================================
#pragma once

#include "Animation/Animator.h"
#include "Audio/AudioEngine.h"
#include "Game/Character.h"
#include "Game/DungeonEntities.h"
#include "Game/DungeonMap.h"
#include "Game/FireEffect.h"
#include "Game/Party.h"
#include "Game/PartyHud.h"
#include "Graphics/Camera.h"
#include "Graphics/ParticleBatch.h"
#include "Graphics/Renderer.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Window.h"
#include "UI/Controls.h"
#include "UI/UIContext.h"

#include <flat_map>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace dungeon::game {

class Game {
public:
	Game(Window& window, gfx::GraphicsDevice& device, gfx::Renderer& renderer,
		 gfx::SpriteBatch& spriteBatch, audio::AudioEngine& audio);

	void Update(float dt);
	void Render(ID3D12GraphicsCommandList* list);

	// Set by the pause menu's Exit entry (and Esc outside of play); the
	// main loop polls it to leave cleanly.
	bool QuitRequested() const { return m_quitRequested; }

private:
	enum class AppState { Loading, Menu, LoadingGame, Playing, Paused, CharacterSheet };
	enum class MenuPage { Main, Settings };

	// Quality tiers, selected in Settings (persisted to settings.ini next to
	// the exe, hot-swapped without restart). Meshes: low/med/high worn-block
	// tessellation (Ultra reuses high). Textures: 1K (Low/Medium), 2K (High),
	// 4K (Ultra — fetchable content, see tools/FetchTextures.ps1; falls
	// back to 2K with a warning when the 4K sets are absent).
	enum class Quality { Low, Medium, High, Ultra };

	// A texture variant set: parallel albedo / normal+height pairs plus the
	// batched mesh bucket per variant.
	struct Surface {
		std::vector<std::unique_ptr<gfx::Texture>> albedo;
		std::vector<std::unique_ptr<gfx::Texture>> normal;
		std::vector<std::unique_ptr<gfx::Mesh>> meshes; // null where bucket empty
		float heightScale = 0.0f;
	};

	// Per-kind monster assets (shared) and per-instance state. Kinds are
	// entity type names from the .ent file ("skeleton" loads skeleton.gltf).
	struct MonsterKind {
		assets::ModelData model; // must outlive the Animators pointing into it
		std::unique_ptr<gfx::Mesh> mesh;
		std::string name;
	};
	struct Monster {
		const MonsterKind* kind = nullptr; // points into m_monsterKinds (stable)
		int x, z;
		float yaw = 0.0f;
		bool announced = false;
		anim::Animator animator;
	};

	// --- loading (one task per frame while a loading screen shows) ---------
	void BuildBootLoadTasks(); // menu essentials, run before the landing page
	void BuildGameLoadTasks(); // the dungeon itself, run on first game start
	bool RunLoadTasks();       // executes one task per frame; true when done
	void LoadSurfaceMaterial(Surface& surface, const char* name);
	void LoadTextureSet(Surface& surface, std::span<const char* const> names,
						float heightScale);
	void BuildDungeonMeshes();
	void LoadMonsters();

	// --- menu / HUD ---------------------------------------------------------
	void BuildMenu();
	void BuildPauseMenu();
	void BuildHud();
	// Re-derives the party-bar slot rects from m_partyBarScale and shifts the
	// widgets beneath the bar to match; no-op until BuildHud has run.
	void ApplyPartyBarScale();
	void BuildCharacterSheet();
	void OpenCharacterSheet(size_t index); // freezes the world, shows the page
	void StartNewGame();

	// --- quality ---------------------------------------------------------------
	void LoadDungeonBlocks();        // loads the worn block set for m_quality
	void LoadAllSurfaceTextures();   // loads the texture sets for m_quality
	void SetQuality(Quality quality);
	// settings.ini next to the exe: quality=0..3, volume=0..1,
	// barscale=0.5..1.5, baropacity=0..1, theme_<name>=r,g,b,a (see
	// kThemeFields in Game.cpp).
	void LoadSettings();
	void SaveSettings() const;
	// Pushes m_theme into every UIContext (each owns a copy).
	void ApplyTheme();
	const char* QualitySuffix() const;        // "low" / "med" / "high" (meshes)
	const char* QualityTextureSuffix() const; // "1k" / "2k" / "4k" (texture sets)
	const char* QualityLabel() const;         // "Low" / "Medium" / "High" / "Ultra"

	// --- per-frame ------------------------------------------------------------
	void UpdateCamera();
	void UpdateLights(float time);
	void UpdateMonsters(float dt);
	void ApplyTorchPalette(int index);

	// --- rendering -------------------------------------------------------------
	void AssignShadowSlots();
	void RenderShadowMaps(ID3D12GraphicsCommandList* list);
	void RenderScene(ID3D12GraphicsCommandList* list);
	// All 3D draw calls, shared verbatim by the shadow and main passes.
	void SubmitSceneGeometry(ID3D12GraphicsCommandList* list);
	void DrawSurface(ID3D12GraphicsCommandList* list, const Surface& surface);
	void RenderLoadingScreen();     // boot: black screen, title, progress
	void RenderGameLoadingScreen(); // game load: title art + progress
	void DrawLoadProgress(float barY); // shared bar + step label
	void RenderMenuOverlay();
	void RenderPauseOverlay(); // dark wash + pause menu over the frozen scene
	void RenderCharacterSheetOverlay(); // dark wash + the details page

	Window& m_window;
	gfx::GraphicsDevice& m_device;
	gfx::Renderer& m_renderer;
	gfx::SpriteBatch& m_spriteBatch;
	audio::AudioEngine& m_audio;

	// --- app state -------------------------------------------------------------
	AppState m_state = AppState::Loading;
	std::vector<std::pair<std::string, std::function<void()>>> m_loadTasks;
	size_t m_loadIndex = 0;
	bool m_gameLoaded = false; // dungeon assets resident (first start done)
	u32 m_framesRendered = 0;
	// Frame count when the current loading state was entered; tasks only run
	// once its screen has been presented at least once.
	u32 m_stateFrameMark = 0;
	bool m_quitRequested = false;

	// --- world -----------------------------------------------------------------
	DungeonMap m_map;          // static layer (.map): structure, fixtures
	DungeonEntities m_entities; // dynamic layer (.ent): monsters, items, buttons
	Party m_party;
	// Party roster (up to four). Filled once in the constructor and never
	// resized — the party-bar panels and the sheet hold pointers into it, so
	// StartNewGame resets the members in place.
	std::vector<Character> m_characters;
	// Baked portrait textures, parallel to m_characters (entries may be null
	// when the asset is missing; Character::portrait points in here).
	std::vector<std::unique_ptr<gfx::Texture>> m_portraitTextures;
	size_t m_sheetIndex = 0; // member shown by the character sheet
	gfx::Camera m_camera;
	gfx::LightSet m_lights;
	// Scratch reused by AssignShadowSlots: (distance², light index), sorted
	// nearest-first each frame into retained capacity.
	std::vector<std::pair<float, size_t>> m_shadowCandidates;
	gfx::Atmosphere m_atmosphere; // per-cell air turbidity (dust)
	std::unique_ptr<gfx::Texture> m_turbidityMap;

	Surface m_walls;
	Surface m_floors;
	Surface m_ceilings;
	// Worn block geometry, one mesh per texture variant (same order as the
	// surface texture sets), held between the load and mesh-build tasks.
	std::vector<assets::MeshData> m_wallBlocks, m_floorBlocks, m_ceilingBlocks;

	assets::ModelData m_pillarModel;
	std::unique_ptr<gfx::Mesh> m_pillarMesh;
	anim::Animator m_pillarAnimator;
	Vec3 m_pillarPos{};

	std::flat_map<std::string, std::unique_ptr<MonsterKind>> m_monsterKinds;
	std::vector<Monster> m_monsters;

	// Fires: wall sconces (at 'T' cells, mounted on the adjacent wall) and
	// floor braziers ('F' cells). Each carries a flickering point light at
	// its flame origin and a FireEffect particle simulation.
	struct Fire {
		bool brazier = false;
		Mat4 world;        // prop transform
		Vec3 flamePos;     // particle + light origin
		float phase = 0;   // flicker phase
		FireEffect effect;
	};
	void BuildFires();
	std::vector<Fire> m_fires;
	std::unique_ptr<gfx::Mesh> m_sconceMesh;
	std::unique_ptr<gfx::Mesh> m_brazierMesh;
	Vec4 m_sconceColor{1, 1, 1, 1};
	Vec4 m_brazierColor{1, 1, 1, 1};
	std::unique_ptr<gfx::ParticleBatch> m_particleBatch;
	std::vector<gfx::ParticleInstance> m_particleScratch;

	assets::SoundData m_sfxFootstep;
	assets::SoundData m_sfxBump;
	assets::SoundData m_sfxTurn;
	assets::SoundData m_sfxClick;
	assets::SoundData m_sfxMonster;

	Quality m_quality = Quality::Medium;
	MenuPage m_menuPage = MenuPage::Main;

	// --- UI ---------------------------------------------------------------------
	ui::UIContext m_ui;        // in-game HUD (17px font)
	ui::UIContext m_menuUi;    // landing page (28px font)
	ui::UIContext m_settingsUi; // settings page (28px font, shared by pause)
	ui::UIContext m_pauseUi;   // pause menu (28px font)
	ui::UIContext m_sheetUi;   // character sheet (22px font)
	ui::Font m_titleFont;     // big face for "DUNGEON" titles
	// User-editable control colors (Settings → UI tab, persisted); the master
	// copy every context's theme is set from — see ApplyTheme.
	ui::Theme m_theme;
	std::unique_ptr<gfx::Texture> m_titleBackground; // landing-page art
	ui::TextOutput* m_log = nullptr;
	ui::Label* m_compass = nullptr;
	ui::Label* m_position = nullptr;
	CharacterSheet* m_sheet = nullptr;

	// Party-bar appearance (Settings → UI tab, persisted). Scale resizes the
	// slots about the bar's top center; opacity multiplies the slot background
	// alpha (pushed into each panel's backgroundOpacity).
	float m_partyBarScale = 1.0f;
	float m_partyBarOpacity = 1.0f;
	std::vector<CharacterPanel*> m_partyPanels; // owned by m_ui
	// Widgets authored beneath the bar and their design-pixel Y at scale 1;
	// ApplyPartyBarScale shifts them so they track the bar's bottom edge.
	std::vector<std::pair<ui::Widget*, float>> m_belowBarWidgets;
	float m_hudDesignW = 0.0f, m_hudDesignH = 0.0f; // window size at BuildHud

	Vec3 m_torchColor{1.0f, 0.62f, 0.28f};
	float m_time = 0.0f;
	// Font re-bake debounce: last seen window height and how long it has
	// held (fonts re-bake once it settles — see the top of Update).
	float m_fontWindowH = 0.0f;
	float m_fontSettle = 0.0f;

	// Last values shown in the HUD labels (reformat only on change).
	int m_lastFacing = -1;
	int m_lastGridX = -1;
	int m_lastGridZ = -1;
};

} // namespace dungeon::game
