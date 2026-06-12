// ============================================================================
// Game/Game.cpp — see Game.h. The module classes do the real work; this file
// is construction wiring, the staged-load task lists, and the state machine.
// ============================================================================
#include "Game/Game.h"

#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace dungeon::game {

// ============================================================================
// Construction — cheap setup only; the heavy asset work is queued as load
// tasks that run one per frame behind the loading screen (see Update).
// ============================================================================
Game::Game(Window& window, gfx::GraphicsDevice& device, gfx::Renderer& renderer,
		   gfx::SpriteBatch& spriteBatch, audio::AudioEngine& audio)
	: m_window(window), m_device(device), m_renderer(renderer),
	  m_spriteBatch(spriteBatch), m_audio(audio),
	  m_world(device, renderer, audio, m_sounds, m_settings),
	  m_ui(window, device, spriteBatch, audio, m_sounds, m_settings,
		   m_characters) {
	m_settings.Load();
	m_audio.SetMasterVolume(m_settings.volume);
	m_world.GetParty().SetKeys(m_settings.moveKeys);

	m_characters = CreateDefaultParty();
	ApplyPartySpeed();

	// Wire the modules together: world feedback goes to the HUD log, UI
	// actions drive the state machine.
	m_world.onMessage = [this](const std::string& line) { m_ui.AddLogLine(line); };
	m_ui.onStartNewGame = [this] {
		if (m_gameLoaded) {
			StartNewGame();
		} else {
			// First start: the boot load only fetched menu essentials, so
			// the dungeon loads now behind its own progress screen.
			BuildGameLoadTasks();
			m_state = AppState::LoadingGame;
			m_stateFrameMark = m_framesRendered;
		}
	};
	m_ui.onQuit = [this] { m_quitRequested = true; };
	m_ui.onResume = [this] { m_state = AppState::Playing; };
	m_ui.onOpenSheet = [this](size_t index) { OpenCharacterSheet(index); };
	m_ui.onQualitySelected = [this](int index) {
		SetQuality(static_cast<Quality>(index));
	};
	m_ui.onTorchPalette = [this](int index) { m_world.SetTorchPalette(index); };
	m_ui.onKeysChanged = [this] {
		m_world.GetParty().SetKeys(m_settings.moveKeys);
	};

	m_ui.BuildStaticUi();
	BuildBootLoadTasks();
}

// ============================================================================
// Staged loading. Each task is one frame's worth of blocking work; a loading
// screen renders between tasks. The boot list is the bare minimum to reach
// the landing page fast; the heavy dungeon load runs later, behind its own
// progress screen, when the player first starts a game.
// ============================================================================

void Game::BuildBootLoadTasks() {
	m_loadQueue.Clear();
	m_loadQueue.Add("Tuning the echoes", [this] { m_sounds.Load(); });
	m_loadQueue.Add("Painting the title", [this] { m_ui.LoadTitleArt(); });
}

void Game::BuildGameLoadTasks() {
	m_loadQueue.Clear();
	m_world.AppendLoadTasks(m_loadQueue);
	m_loadQueue.Add("Painting the party", [this] { LoadPortraits(); });
	m_loadQueue.Add("Lighting the torches", [this] {
		m_ui.BuildHud();
		log::Info("Game loaded: {}x{} dungeon, {} torches, {} monsters",
				  m_world.Map().Width(), m_world.Map().Height(),
				  m_world.Map().TorchCells().size(), m_world.MonsterCount());
	});
}

// Runs one queued task per rendered frame (never before the current loading
// screen has been presented once); returns true when the queue is done.
bool Game::RunLoadTasks() {
	if (m_framesRendered > m_stateFrameMark) m_loadQueue.RunOne();
	return m_loadQueue.Done();
}

void Game::LoadPortraits() {
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
}

// ============================================================================
// State transitions
// ============================================================================

void Game::StartNewGame() {
	m_world.ResetForNewGame();

	// Fresh stats for the same roster — element-wise so the addresses the
	// party-bar panels and the sheet point at stay valid, keeping the loaded
	// portrait (the defaults carry a null one).
	const std::vector<Character> fresh = CreateDefaultParty();
	for (size_t i = 0; i < m_characters.size() && i < fresh.size(); ++i) {
		const gfx::Texture* portrait = m_characters[i].portrait;
		m_characters[i] = fresh[i];
		m_characters[i].portrait = portrait;
	}
	m_ui.RefreshSheet();
	ApplyPartySpeed();

	m_ui.ClearLog();
	m_ui.AddLogLine("You descend into the dungeon...");
	m_ui.AddLogLine("Something shuffles in the dark.");
	m_ui.AddLogLine(m_settings.MoveKeysHelp());

	m_ui.ResetHudStatus();
	m_state = AppState::Playing;
	log::Info("New game started");
}

void Game::OpenCharacterSheet(size_t index) {
	m_audio.Play(m_sounds.click, 0.5f);
	m_ui.ShowSheet(index);
	m_state = AppState::CharacterSheet;
}

// The party moves as fast as its slowest member: take the roster minimum and
// hand it to the Party, which scales its step and turn rates by it.
void Game::ApplyPartySpeed() {
	float slowest = m_characters.empty() ? 1.0f : m_characters[0].moveSpeed;
	for (const Character& member : m_characters)
		slowest = std::min(slowest, member.moveSpeed);
	m_world.GetParty().SetSpeed(slowest);
}

void Game::SetQuality(Quality quality) {
	if (quality == m_settings.quality) return;
	const std::string oldTextureSuffix = m_settings.TextureSuffix();
	m_settings.quality = quality;
	const bool textureResChanged = oldTextureSuffix != m_settings.TextureSuffix();
	m_settings.Save();
	m_world.ApplyQuality(textureResChanged);
}

// ============================================================================
// The state machine
// ============================================================================

void Game::Update(float dt) {
	m_time += dt;
	m_ui.UpdateFonts(dt);

	const Input& input = m_window.GetInput();

	switch (m_state) {
	case AppState::Loading:
		if (input.WasKeyPressed(VK_ESCAPE)) m_quitRequested = true;
		if (RunLoadTasks()) m_state = AppState::Menu;
		return;

	case AppState::Menu:
		// The menu sits on baked title art; nothing in the world simulates.
		// Esc backs out of settings, or quits from the landing list — unless
		// a key-bind box is armed, where Esc just cancels the capture.
		if (input.WasKeyPressed(VK_ESCAPE) && !m_ui.KeyCaptureActive()) {
			if (!m_ui.CloseSettingsPage()) m_quitRequested = true;
		}
		m_ui.UpdateMenu(input);
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
		// page) updates. Esc backs out of settings (but an armed key-bind box
		// gets it first, as its cancel), or resumes play.
		if (input.WasKeyPressed(VK_ESCAPE) && !m_ui.KeyCaptureActive()) {
			m_audio.Play(m_sounds.click, 0.5f);
			if (!m_ui.CloseSettingsPage()) m_state = AppState::Playing;
			return;
		}
		m_ui.UpdatePause(input);
		return;

	case AppState::CharacterSheet:
		// Frozen like Paused; only the sheet page updates. Esc resumes.
		if (input.WasKeyPressed(VK_ESCAPE)) {
			m_audio.Play(m_sounds.click, 0.5f);
			m_state = AppState::Playing;
			return;
		}
		m_ui.UpdateSheet(input);
		return;

	case AppState::Playing:
		break;
	}

	// --- Playing -------------------------------------------------------------
	// Esc freezes the world and opens the pause menu.
	if (input.WasKeyPressed(VK_ESCAPE)) {
		m_audio.Play(m_sounds.click, 0.5f);
		m_ui.ResetToMainPage();
		m_state = AppState::Paused;
		return;
	}

	// UI first so it can consume the mouse; keyboard always reaches the party.
	m_ui.UpdateHud(input);
	// A portrait click may have opened the character sheet — freeze now
	// rather than simulating one more frame.
	if (m_state != AppState::Playing) return;
	m_world.Update(input, dt, m_time);

	Party& party = m_world.GetParty();
	m_ui.SetHudStatus(party.Facing(), party.GridX(), party.GridZ());
}

// ============================================================================
// Rendering — the command list arrives from GraphicsDevice::BeginFrame
// already cleared and bound. Loading, Menu, and LoadingGame are 2D-only
// (title art / progress screens); Playing draws the 3D scene + HUD.
// ============================================================================
void Game::Render(ID3D12GraphicsCommandList* list) {
	m_renderer.NewFrame(m_device.FrameIndex());
	m_spriteBatch.NewFrame(m_device.FrameIndex());
	m_world.NewFrame(m_device.FrameIndex());

	// The 3D scene draws during play and under the pause/character-sheet
	// overlays (frozen); Loading and Menu are 2D-only.
	if (m_state == AppState::Playing || m_state == AppState::Paused ||
		m_state == AppState::CharacterSheet) {
		m_world.RenderShadowMaps(list);
		m_world.RenderScene(list);
	}

	// 2D pass.
	m_spriteBatch.Begin(list, m_device.Width(), m_device.Height());
	switch (m_state) {
	case AppState::Loading:     m_ui.RenderLoadingScreen(m_loadQueue); break;
	case AppState::Menu:        m_ui.RenderMenuOverlay(); break;
	case AppState::LoadingGame: m_ui.RenderGameLoadingScreen(m_loadQueue); break;
	case AppState::Playing:     m_ui.RenderHud(); break;
	case AppState::Paused:      m_ui.RenderPauseOverlay(); break;
	case AppState::CharacterSheet: m_ui.RenderCharacterSheetOverlay(); break;
	}
	m_spriteBatch.End();

	++m_framesRendered;
}

} // namespace dungeon::game
