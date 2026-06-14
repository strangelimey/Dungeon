// ============================================================================
// Game/Game.cpp — see Game.h. The module classes do the real work; this file
// is construction wiring, the staged-load task lists, and the state machine.
// ============================================================================
#include "Game/Game.h"

#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
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
		   m_characters),
	  m_mapView(device, m_world),
	  m_console(device) {
	m_settings.Load();
	ApplyLanguage(false); // strings must exist before any UI builds
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
	m_ui.onMoveAction = [this](MoveAction action) {
		m_world.GetParty().Act(action);
	};
	m_ui.onKeysChanged = [this] {
		m_world.GetParty().SetKeys(m_settings.moveKeys);
	};
	// Recorded only — the rebuild would destroy the dropdown mid-callback;
	// Update applies it first thing next frame.
	m_ui.onLanguageSelected = [this](const std::string& code) {
		m_pendingLanguage = code;
	};

	// Developer console commands (dev-facing, English). The generic ones
	// (help/clear/echo) live in DevConsole; these reach into the app state.
	m_console.Register("quit", "exit the game",
					   [this](const std::vector<std::string>&) { m_quitRequested = true; });
	m_console.Register("exit", "exit the game",
					   [this](const std::vector<std::string>&) { m_quitRequested = true; });
	m_console.Register("fps", "print the current frame rate",
					   [this](const std::vector<std::string>&) {
						   m_console.Print(std::format("{:.1f} fps", m_console.Fps()));
					   });
	m_console.Register("quality", "set quality tier 0-3 (low/med/high/ultra)",
					   [this](const std::vector<std::string>& args) {
						   if (args.empty()) {
							   m_console.Print("usage: quality <0-3>");
							   return;
						   }
						   const int q = std::atoi(args[0].c_str());
						   if (q < 0 || q > 3) {
							   m_console.Print("quality must be 0-3");
							   return;
						   }
						   SetQuality(static_cast<Quality>(q));
						   m_console.Print(std::format("quality set to {}", q));
					   });
	m_console.Register("lang", "switch language by code (e.g. en, de)",
					   [this](const std::vector<std::string>& args) {
						   if (args.empty()) {
							   m_console.Print("usage: lang <code>");
							   return;
						   }
						   m_pendingLanguage = args[0]; // applied next frame
						   m_console.Print("language: " + args[0]);
					   });
	m_console.Register("tp", "teleport the party to a cell",
					   [this](const std::vector<std::string>& args) {
						   if (args.size() < 2) {
							   m_console.Print("usage: tp <x> <z>");
							   return;
						   }
						   const int x = std::atoi(args[0].c_str());
						   const int z = std::atoi(args[1].c_str());
						   if (m_world.GetParty().SetGridPosition(x, z))
							   m_console.Print(std::format("teleported to {},{}", x, z));
						   else
							   m_console.Print(std::format("{},{} is not walkable", x, z));
					   });

	// --- diagnostics (read-only) ---
	m_console.Register("pos", "print party position and facing",
					   [this](const std::vector<std::string>&) {
						   const Party& p = m_world.GetParty();
						   static const char* kDirs[] = {"north", "east", "south", "west"};
						   m_console.Print(std::format("{},{} facing {}", p.GridX(),
													   p.GridZ(), kDirs[p.Facing() & 3]));
					   });
	m_console.Register("mapinfo", "print dungeon size and counts",
					   [this](const std::vector<std::string>&) {
						   const DungeonMap& map = m_world.Map();
						   m_console.Print(std::format(
							   "{}x{} map, {} monsters, {} torches", map.Width(),
							   map.Height(), m_world.MonsterCount(),
							   map.TorchCells().size()));
					   });
	m_console.Register("monsters", "list monsters and their cells",
					   [this](const std::vector<std::string>&) {
						   const std::vector<std::string> list = m_world.MonsterList();
						   if (list.empty()) {
							   m_console.Print("no monsters");
							   return;
						   }
						   for (const std::string& l : list) m_console.Print("  " + l);
					   });
	m_console.Register("lights", "print active point-light count",
					   [this](const std::vector<std::string>&) {
						   m_console.Print(std::format("{} active point lights",
													   m_world.ActiveLightCount()));
					   });
	m_console.Register("ver", "print build and GPU info",
					   [this](const std::vector<std::string>&) {
#ifdef _DEBUG
						   const char* cfg = "debug";
#else
						   const char* cfg = "release";
#endif
						   m_console.Print(std::format("Dungeon ({}) built {} - {}", cfg,
													   __DATE__, m_device.AdapterName()));
					   });

	// --- navigation ---
	m_console.Register("face", "turn the party to n/e/s/w",
					   [this](const std::vector<std::string>& args) {
						   if (args.empty()) {
							   m_console.Print("usage: face <n|e|s|w>");
							   return;
						   }
						   int facing = -1;
						   switch (std::tolower(static_cast<unsigned char>(args[0][0]))) {
						   case 'n': facing = 0; break;
						   case 'e': facing = 1; break;
						   case 's': facing = 2; break;
						   case 'w': facing = 3; break;
						   }
						   if (facing < 0) {
							   m_console.Print("direction must be n/e/s/w");
							   return;
						   }
						   m_world.GetParty().SetFacing(facing);
						   m_console.Print("facing set");
					   });
	m_console.Register("home", "teleport the party to the start cell",
					   [this](const std::vector<std::string>&) {
						   const DungeonMap& map = m_world.Map();
						   m_world.GetParty().SetGridPosition(map.StartX(), map.StartZ());
						   m_console.Print(std::format("home at {},{}", map.StartX(),
													   map.StartZ()));
					   });
	m_console.Register("speed", "set party pace multiplier",
					   [this](const std::vector<std::string>& args) {
						   if (args.empty()) {
							   m_console.Print("usage: speed <mult>");
							   return;
						   }
						   const float v = static_cast<float>(std::atof(args[0].c_str()));
						   if (v <= 0.0f) {
							   m_console.Print("speed must be > 0");
							   return;
						   }
						   m_world.GetParty().SetSpeed(v);
						   m_console.Print(std::format("speed x{:.2f}", v));
					   });
	m_console.Register("timescale", "scale sim speed (1 normal, 0 freeze)",
					   [this](const std::vector<std::string>& args) {
						   if (args.empty()) {
							   m_console.Print(std::format("timescale {:.2f}", m_timeScale));
							   return;
						   }
						   const float v = static_cast<float>(std::atof(args[0].c_str()));
						   if (v < 0.0f) {
							   m_console.Print("timescale must be >= 0");
							   return;
						   }
						   m_timeScale = v;
						   m_console.Print(std::format("timescale {:.2f}", v));
					   });
	m_console.Register("noclip", "toggle walking through walls",
					   [this](const std::vector<std::string>&) {
						   Party& p = m_world.GetParty();
						   p.SetNoclip(!p.Noclip());
						   m_console.Print(p.Noclip() ? "noclip on" : "noclip off");
					   });

	// --- render debug ---
	m_console.Register("shadows", "toggle shadow rendering (on/off)",
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty())
							   m_world.SetShadowsEnabled(args[0] == "on" || args[0] == "1");
						   m_console.Print(m_world.ShadowsEnabled() ? "shadows on"
																	: "shadows off");
					   });
	m_console.Register("dust", "toggle volumetric dust (on/off)",
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty())
							   m_world.SetDustEnabled(args[0] == "on" || args[0] == "1");
						   m_console.Print(m_world.DustEnabled() ? "dust on" : "dust off");
					   });
	m_console.Register("fov", "set camera field of view in degrees (default 70)",
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty())
							   m_world.SetFov(static_cast<float>(std::atof(args[0].c_str())));
						   m_console.Print(std::format("fov {:.0f}", m_world.Fov()));
					   });

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
	m_loadQueue.SetDoneLabel(loc::Tr("load.done"));
	m_loadQueue.Add(loc::Tr("load.echoes"), [this] { m_sounds.Load(); });
	m_loadQueue.Add(loc::Tr("load.title_art"), [this] { m_ui.LoadTitleArt(); });
}

void Game::BuildGameLoadTasks() {
	m_loadQueue.Clear();
	m_loadQueue.SetDoneLabel(loc::Tr("load.done"));
	m_world.AppendLoadTasks(m_loadQueue);
	m_loadQueue.Add(loc::Tr("load.portraits"), [this] { LoadPortraits(); });
	m_loadQueue.Add(loc::Tr("load.hud"), [this] {
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
	m_ui.AddLogLine(loc::Tr("log.descend"));
	m_ui.AddLogLine(loc::Tr("log.shuffle"));
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

void Game::ApplyLanguage(bool rebuild) {
	if (!m_pendingLanguage.empty()) {
		m_settings.language = m_pendingLanguage;
		m_pendingLanguage.clear();
		m_settings.Save();
	}
	if (!loc::LoadFile(paths::Asset("lang\\" + m_settings.language + ".lang")) &&
		m_settings.language != "en")
		loc::LoadFile(paths::Asset("lang\\en.lang"));
	if (rebuild) m_ui.RebuildForLanguage();
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
	const float wdt = dt * m_timeScale; // world dt (dev console `timescale`)
	m_time += wdt;

	// A language picked last frame applies now, before any widget updates —
	// the rebuild destroys every widget, so none may be mid-callback.
	if (!m_pendingLanguage.empty()) ApplyLanguage(true);

	m_ui.UpdateFonts(dt);

	const Input& input = m_window.GetInput();

	// The dev console toggles with `~` and overlays any state. While it is
	// open it captures input (so the party can't move) but the world keeps
	// simulating — it does NOT pause the game. The FPS sampler ticks every
	// frame regardless.
	const bool consoleWasOpen = m_console.IsOpen();
	if (input.WasKeyPressed(VK_OEM_3)) m_console.Toggle();
	m_console.Update(input, dt, static_cast<float>(m_window.Width()),
					 static_cast<float>(m_window.Height()));
	// The console owns the whole frame's input if it was open at the start (or
	// just opened) — so the very keystroke that closes it (Esc or `~`) never
	// also reaches the pause menu / HUD this frame.
	if (m_console.IsOpen() || consoleWasOpen) {
		if (m_state == AppState::Playing) {
			m_world.Update(input, wdt, m_time, /*acceptInput=*/false);
			Party& party = m_world.GetParty();
			m_ui.SetHudStatus(party.Facing(), party.GridX(), party.GridZ());
		}
		return;
	}

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
	// Map overlay: a toggle that never pauses the world. While it is open the
	// party still walks (keyboard) — the overlay only claims the mouse for
	// panning/zooming/editing, and Esc/M closes it instead of pausing.
	if (input.WasKeyPressed('M')) m_mapView.Toggle();
	if (m_mapView.IsOpen()) {
		if (input.WasKeyPressed(VK_ESCAPE)) {
			m_mapView.Close();
			return;
		}
		m_mapView.Update(input, MapPanel(static_cast<float>(m_window.Width()),
										 static_cast<float>(m_window.Height())));
		m_world.Update(input, wdt, m_time); // keyboard still moves the party
		Party& party = m_world.GetParty();
		m_ui.SetHudStatus(party.Facing(), party.GridX(), party.GridZ());
		return;
	}

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
	m_world.Update(input, wdt, m_time);

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
	case AppState::Playing:
		m_ui.RenderHud();
		if (m_mapView.IsOpen()) {
			// Dim the scene behind the panel, then draw the map over the HUD.
			m_spriteBatch.DrawRect({0, 0, static_cast<float>(m_device.Width()),
									static_cast<float>(m_device.Height())},
								   {0, 0, 0, 0.45f});
			m_mapView.Render(m_spriteBatch, m_settings.theme,
							 MapPanel(static_cast<float>(m_device.Width()),
									  static_cast<float>(m_device.Height())));
		}
		break;
	case AppState::Paused:      m_ui.RenderPauseOverlay(); break;
	case AppState::CharacterSheet: m_ui.RenderCharacterSheetOverlay(); break;
	}
	if (m_console.IsOpen())
		m_console.Render(m_spriteBatch, m_device, static_cast<float>(m_device.Width()),
						 static_cast<float>(m_device.Height()));
	m_spriteBatch.End();

	++m_framesRendered;
}

} // namespace dungeon::game
