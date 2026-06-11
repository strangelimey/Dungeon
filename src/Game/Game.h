// ============================================================================
// Game/Game.h — the dungeon crawler itself.
//
// Game is the only class that sees every engine module at once. It owns the
// world (map, party, monsters, lights), the menus and HUD, the loaded
// assets, and a small app state machine:
//
//   Loading  — assets load one step per frame so a progress screen can
//              render between steps (loading is staged, not threaded)
//   Menu     — landing page over baked title art (assets/textures/
//              title_bg.png); mouse hover or keyboard selects an entry
//   Playing  — the crawler: UI input → party movement → animators → lights
//
// Everything binary loads from the assets/ directory next to the exe
// (regenerate with tools/AssetBaker). Engine modules know nothing about
// dungeons — all gameplay rules live in this module.
// ============================================================================
#pragma once

#include "Animation/Animator.h"
#include "Audio/AudioEngine.h"
#include "Game/DungeonMap.h"
#include "Game/Party.h"
#include "Graphics/Camera.h"
#include "Graphics/Renderer.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Window.h"
#include "UI/Controls.h"
#include "UI/UIContext.h"

#include <flat_map>
#include <functional>
#include <memory>
#include <vector>

namespace dungeon::game {

class Game {
public:
	Game(Window& window, gfx::GraphicsDevice& device, gfx::Renderer& renderer,
		 gfx::SpriteBatch& spriteBatch, audio::AudioEngine& audio);

	void Update(float dt);
	void Render(ID3D12GraphicsCommandList* list);

private:
	enum class AppState { Loading, Menu, Playing };

	// A texture variant set: parallel albedo / normal+height pairs plus the
	// batched mesh bucket per variant.
	struct Surface {
		std::vector<std::unique_ptr<gfx::Texture>> albedo;
		std::vector<std::unique_ptr<gfx::Texture>> normal;
		std::vector<std::unique_ptr<gfx::Mesh>> meshes; // null where bucket empty
		float heightScale = 0.0f;
	};

	// Per-kind monster assets (shared) and per-instance state.
	struct MonsterKind {
		assets::ModelData model; // must outlive the Animators pointing into it
		std::unique_ptr<gfx::Mesh> mesh;
		const char* name;
	};
	struct Monster {
		char kind;
		int x, z;
		float yaw = 0.0f;
		bool announced = false;
		anim::Animator animator;
	};

	// --- loading (one task per frame while the loading screen shows) -------
	void BuildLoadTasks();
	void LoadTextureSet(Surface& surface, std::initializer_list<const char*> names,
						float heightScale);
	void BuildDungeonMeshes();
	void LoadMonsters();

	// --- menu / HUD ---------------------------------------------------------
	void BuildMenu();
	void BuildHud();
	void StartNewGame();

	// --- per-frame ------------------------------------------------------------
	void UpdateCamera();
	void UpdateLights(float time);
	void UpdateMonsters(float dt);
	void ApplyTorchPalette(int index);

	// --- rendering -------------------------------------------------------------
	void RenderScene(ID3D12GraphicsCommandList* list);
	void DrawSurface(ID3D12GraphicsCommandList* list, const Surface& surface);
	void RenderLoadingScreen();
	void RenderMenuOverlay();

	bool MonsterAt(int x, int z) const;

	Window& m_window;
	gfx::GraphicsDevice& m_device;
	gfx::Renderer& m_renderer;
	gfx::SpriteBatch& m_spriteBatch;
	audio::AudioEngine& m_audio;

	// --- app state -------------------------------------------------------------
	AppState m_state = AppState::Loading;
	std::vector<std::pair<const char*, std::function<void()>>> m_loadTasks;
	size_t m_loadIndex = 0;
	u32 m_framesRendered = 0;

	// --- world -----------------------------------------------------------------
	DungeonMap m_map;
	Party m_party;
	gfx::Camera m_camera;
	gfx::LightSet m_lights;

	Surface m_walls;
	Surface m_floors;
	Surface m_ceilings;
	// Block model geometry held between the texture and mesh-build tasks.
	assets::MeshData m_wallBlock, m_floorBlock, m_ceilingBlock;

	assets::ModelData m_pillarModel;
	std::unique_ptr<gfx::Mesh> m_pillarMesh;
	anim::Animator m_pillarAnimator;
	Vec3 m_pillarPos{};

	std::flat_map<char, std::unique_ptr<MonsterKind>> m_monsterKinds;
	std::vector<Monster> m_monsters;

	assets::SoundData m_sfxFootstep;
	assets::SoundData m_sfxBump;
	assets::SoundData m_sfxTurn;
	assets::SoundData m_sfxClick;
	assets::SoundData m_sfxMonster;

	// --- UI ---------------------------------------------------------------------
	ui::UIContext m_ui;       // in-game HUD (17px font)
	ui::UIContext m_menuUi;   // landing page (28px font)
	ui::Font m_titleFont;     // big face for "DUNGEON" titles
	std::unique_ptr<gfx::Texture> m_titleBackground; // landing-page art
	ui::TextOutput* m_log = nullptr;
	ui::Label* m_compass = nullptr;
	ui::Label* m_position = nullptr;

	Vec3 m_torchColor{1.0f, 0.62f, 0.28f};
	float m_time = 0.0f;

	// Last values shown in the HUD labels (reformat only on change).
	int m_lastFacing = -1;
	int m_lastGridX = -1;
	int m_lastGridZ = -1;
};

} // namespace dungeon::game
