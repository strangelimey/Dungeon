// ============================================================================
// Game/Game.h — the dungeon crawler's app state machine.
//
// Game is the thin coordinator over the module classes — it owns them, wires
// their callbacks together, and runs the state machine that decides what
// updates and renders each frame:
//
//   GameSettings — user options (quality, volume, theme, colors, keys),
//                  persisted to settings.ini next to the exe
//   SoundBank    — the loaded sound effects, shared by every system
//   LoadQueue    — staged loading, one task per rendered frame
//   DungeonWorld — the 3D world: map, party, monsters, fires, lights,
//                  camera; simulation + the shadow and scene passes
//   GameUI       — menus, the settings page, HUD, character sheet, loading
//                  screens and overlays
//
// App states:
//   Loading     — boot load: just the menu essentials (title art, click
//                 sounds), one step per frame, so the landing page appears
//                 fast even on a cold cache
//   Menu        — landing page over baked title art; mouse hover or
//                 keyboard selects an entry
//   LoadingGame — the heavy dungeon load (meshes, scanned textures, world
//                 build), entered from "Start New Game" the first time;
//                 staged one task per frame behind its own progress screen
//   Playing     — the crawler: UI input → world simulation
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

#include "Audio/AudioEngine.h"
#include "Game/AssetDialog.h"
#include "Game/Character.h"
#include "Game/DevConsole.h"
#include "Game/DungeonWorld.h"
#include "Game/GameSettings.h"
#include "Game/GameUI.h"
#include "Game/LoadQueue.h"
#include "Game/MapView.h"
#include "Game/Project.h"
#include "Game/SoundBank.h"
#include "Graphics/ModelPreview.h"
#include "Graphics/Renderer.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Process.h"
#include "Platform/Window.h"

#include <memory>
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
	enum class AppState {
		Loading,
		Menu,
		LoadingGame,
		LoadingLevel, // mid-game level transition (P6): re-stage the world load
		Playing,
		Paused,
		CharacterSheet
	};

	// --- loading (one task per frame while a loading screen shows) ---------
	void BuildBootLoadTasks(); // menu essentials, run before the landing page
	void BuildGameLoadTasks(); // the dungeon itself, run on first game start

	// Asset bake (P4c): launch the AssetBaker command for the current step; and,
	// when the bake finishes, write the new catalog entry + save the project.
	bool StartBakeStep();
	void FinishBake();

	// Starts a mid-game level transition (P6): swaps the world to `stem`, stages
	// its load behind the loading screen, and arrives at (x,z,facing) when done
	// (x<0 = the level's start cell). `stashCurrent` saves the level being left
	// for a later return; pass false when leaving a throwaway baseline (save load).
	void BeginLevelTransition(const std::string& stem, int x, int z,
							  Direction facing, bool stashCurrent = true);
	bool RunLoadTasks();       // executes one task per frame; true when done
	void LoadPortraits();      // baked party portraits (load task)

	// --- state transitions --------------------------------------------------
	void StartNewGame();
	// Resets the roster to a fresh default party in place (keeping each slot's
	// loaded portrait), so the panel/sheet pointers into m_characters stay
	// valid. Shared by StartNewGame and LoadGame.
	void ResetRoster();
	// Captures the live world + roster to a named slot under SaveDir. Requires
	// the dungeon to be loaded (m_gameLoaded); no-op otherwise.
	void SaveGame(const std::string& name);
	// Loads a save file: rebuilds the level baseline, applies the save on top,
	// and enters Playing. Requires the dungeon already loaded (the deferred
	// first-load path is wired by the menu, step 2). Returns false on failure.
	bool LoadGame(const std::string& path);
	void OpenCharacterSheet(size_t index); // freezes the world, shows the page
	void SetQuality(Quality quality);      // persists + hot-swaps the world
	// Applies m_settings' display mode (windowed/borderless/exclusive + monitor
	// + resolution) to the window and swapchain in place. Called at boot and by
	// the Video tab's Apply button for non-adapter changes.
	void ApplyDisplaySettings();
	// Relaunches the executable (a fresh process picks up the new adapter, the
	// only way to switch GPUs) and flags this instance to quit.
	void RestartApp();
	// Loads the settings' language file (falling back to English when it is
	// missing); rebuild=true also re-creates every UI page in the new
	// language. The language dropdown only records m_pendingLanguage —
	// Update applies it at the top of the next frame, after the dropdown's
	// callback has fully unwound (the rebuild destroys the dropdown).
	void ApplyLanguage(bool rebuild);
	// Feeds the slowest member's moveSpeed into the Party as its pace
	// multiplier; call whenever the roster's stats are (re)filled.
	void ApplyPartySpeed();

	Window& m_window;
	gfx::GraphicsDevice& m_device;
	gfx::Renderer& m_renderer;
	gfx::SpriteBatch& m_spriteBatch;
	audio::AudioEngine& m_audio;

	// --- app state -------------------------------------------------------------
	AppState m_state = AppState::Loading;
	LoadQueue m_loadQueue;
	bool m_gameLoaded = false; // dungeon assets resident (first start done)
	u32 m_framesRendered = 0;
	// Frame count when the current loading state was entered; tasks only run
	// once its screen has been presented at least once.
	u32 m_stateFrameMark = 0;
	bool m_quitRequested = false;
	float m_time = 0.0f;
	// Dev console `timescale`: multiplies the world's dt (1 = normal, 0 = freeze).
	float m_timeScale = 1.0f;
	// Language code picked in Settings this frame, applied (strings reloaded,
	// UI rebuilt) at the top of the next Update; empty = no change pending.
	std::string m_pendingLanguage;
	// Save chosen from the landing page before the dungeon was resident: the
	// heavy load runs first (LoadingGame), then this save is applied instead of
	// starting fresh. Empty = the load should StartNewGame as usual.
	std::string m_pendingLoadPath;

	// Pending level-transition arrival (set by BeginLevelTransition, applied when
	// the LoadingLevel queue finishes): the cell + facing the party enters at.
	int m_pendingLevelX = 0, m_pendingLevelZ = 0;
	Direction m_pendingLevelFacing = Direction::South;

	// --- modules (construction order matters: settings load first, the world
	// and UI reference settings/sounds/characters) -------------------------------
	GameSettings m_settings;
	// The active project: content catalogs + levels (assets/projects/<name>).
	// Loaded before the world (which reads it for level paths and catalogs);
	// the editor will read and write it.
	Project m_project;
	SoundBank m_sounds;
	// Party roster (up to four). Filled once in the constructor and never
	// resized — the party-bar panels and the sheet hold pointers into it, so
	// StartNewGame resets the members in place.
	std::vector<Character> m_characters;
	// Rune satchel: symbols the party has picked up but not yet committed to a
	// member (memorized via the character sheet). Party-level; saved whole.
	std::vector<SpellSymbol> m_satchel;
	// Baked portrait textures, parallel to m_characters (entries may be null
	// when the asset is missing; Character::portrait points in here).
	std::vector<std::unique_ptr<gfx::Texture>> m_portraitTextures;
	DungeonWorld m_world;
	GameUI m_ui;
	// Map/editor overlay (toggle with `M` while playing). Like the console it
	// does NOT pause the world — the party keeps walking; the overlay only
	// claims the mouse for panning/zooming/editing.
	MapView m_mapView;
	// Fullscreen dev overlay (toggle with `~`); does not pause the world.
	DevConsole m_console;

	// Editor 3D model preview (P4a). The offscreen render target plus the model
	// currently shown in it (dev `preview <model>`); a null mesh = inactive. The
	// model spins by m_previewOrbit each frame. P4b embeds this in the asset
	// dialog; for now it draws full-screen via the dev command.
	gfx::ModelPreview m_modelPreview;
	assets::ModelData m_previewModel;
	std::unique_ptr<gfx::Mesh> m_previewMesh;
	gfx::MaterialParams m_previewMaterial;
	float m_previewOrbit = 0.0f;

	// Asset-creation dialog (P4b), opened from the palette's "+ New".
	AssetDialog m_assetDialog;
	// Asset bake (P4c): the AssetBaker subprocess for the dialog's Create. A
	// texture-set import is two steps (import textures, then rebake worn meshes);
	// a model import is one. Polled in Update so the frame never blocks.
	platform::Process m_bake;
	AssetDialog::CreateRequest m_bakeReq;
	bool m_baking = false;
	int m_bakeStep = 0;

	// Child process launched to restart the game on an adapter change (it
	// outlives us; we quit right after).
	platform::Process m_restart;

	// The map overlay's panel in the given surface's pixel space (window pixels
	// for input, device pixels for drawing): full-screen in Editor mode (it
	// covers everything), else an 80%-centered rect for the player map.
	gfx::Rect MapPanel(float surfaceW, float surfaceH) const {
		if (m_mapView.CurrentMode() == MapView::Mode::Editor)
			return {0.0f, 0.0f, surfaceW, surfaceH};
		const float pw = surfaceW * 0.8f, ph = surfaceH * 0.8f;
		return {(surfaceW - pw) * 0.5f, (surfaceH - ph) * 0.5f, pw, ph};
	}
};

} // namespace dungeon::game
