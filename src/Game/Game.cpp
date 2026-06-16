// ============================================================================
// Game/Game.cpp — see Game.h. The module classes do the real work; this file
// is construction wiring, the staged-load task lists, and the state machine.
// ============================================================================
#include "Game/Game.h"

#include "Assets/File.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Graphics/DisplayEnum.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <utility>

namespace dungeon::game {

namespace {

// Shared dev-console arg helpers (Game registers ~30 commands; these carry the
// repeated guards/parsing so each command body is just its action).

// Arg-count guard: prints `usage` and returns false when fewer than n args.
bool Need(DevConsole& console, const std::vector<std::string>& args, size_t n,
		  const char* usage) {
	if (args.size() < n) {
		console.Print(usage);
		return false;
	}
	return true;
}

// Joins args into one space-separated string (save-slot names may have spaces).
std::string JoinArgs(const std::vector<std::string>& args) {
	std::string out;
	for (const std::string& a : args)
		out += (out.empty() ? "" : " ") + a;
	return out;
}

// A toggle command's argument: "on"/"1" enable, anything else disables.
bool ArgOn(const std::string& a) { return a == "on" || a == "1"; }

} // namespace

// ============================================================================
// Construction — cheap setup only; the heavy asset work is queued as load
// tasks that run one per frame behind the loading screen (see Update).
// ============================================================================
Game::Game(Window& window, gfx::GraphicsDevice& device, gfx::Renderer& renderer,
		   gfx::SpriteBatch& spriteBatch, audio::AudioEngine& audio)
	: m_window(window), m_device(device), m_renderer(renderer),
	  m_spriteBatch(spriteBatch), m_audio(audio),
	  m_project(Project::Load(paths::Asset("projects\\dungeon-demo"))),
	  m_world(device, renderer, audio, m_sounds, m_settings, m_project),
	  m_ui(window, device, spriteBatch, audio, m_sounds, m_settings,
		   m_characters),
	  m_mapView(device, m_world, m_settings),
	  m_console(device),
	  m_modelPreview(device, 512),
	  m_assetDialog(device, window) {
	m_settings.Load();
	ApplyLanguage(false); // strings must exist before any UI builds
	m_audio.SetMasterVolume(m_settings.volume);
	m_device.SetPresentInterval(m_settings.presentInterval);
	m_world.GetParty().SetKeys(m_settings.moveKeys);

	m_characters = CreateDefaultParty();
	ApplyPartySpeed();
	m_world.SetRoster(&m_characters); // combat drains these; reset in place

	// Wire the modules together: world feedback goes to the HUD log, UI
	// actions drive the state machine.
	m_world.onMessage = [this](const std::string& line) { m_ui.AddLogLine(line); };
	// The party fell: end the run back at the title (Start New Game resets the
	// roster + monsters in place).
	m_world.onPartyWipe = [this] {
		m_state = AppState::Menu;
		m_ui.ResetToMainPage();
	};
	// A rune was stepped on: hold it in the satchel until a member memorizes it.
	m_world.onRunePickup = [this](SpellSymbol s) {
		m_satchel.push_back(s);
		m_ui.AddLogLine(loc::Format("log.rune_found", loc::Tr(SymbolKey(s))));
	};
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
	m_ui.onLoadSave = [this](const std::string& path) {
		if (m_gameLoaded) {
			LoadGame(path); // dungeon resident (pause-menu Load): apply now
		} else {
			// Landing-page Load/Continue on a cold start: stage the dungeon
			// load first, then apply the save when it finishes (see Update).
			m_pendingLoadPath = path;
			BuildGameLoadTasks();
			m_state = AppState::LoadingGame;
			m_stateFrameMark = m_framesRendered;
		}
	};
	m_ui.onSaveSlot = [this](const std::string& name) {
		SaveGame(name);
		m_state = AppState::Playing; // resume play after saving from the pause menu
	};
	m_ui.onOpenSheet = [this](size_t index) { OpenCharacterSheet(index); };
	m_ui.onQualitySelected = [this](int index) {
		SetQuality(static_cast<Quality>(index));
	};
	// Video tab frame-rate cap: live (just a present-interval change on the
	// device), persisted immediately like the language/key binds.
	m_ui.onFrameLimitSelected = [this](int index) {
		m_settings.presentInterval = kPresentIntervals[index];
		m_device.SetPresentInterval(m_settings.presentInterval);
		m_settings.Save();
	};
	// Video tab Apply: a monitor/resolution/mode change rebuilds the swapchain in
	// place; an adapter change can't (the device is bound to its GPU), so it
	// persists the choice and relaunches.
	m_ui.onVideoApply = [this] {
		m_settings.Save();
		ApplyDisplaySettings();
	};
	m_ui.onAdapterRestart = [this] {
		m_settings.Save();
		RestartApp();
	};
	m_ui.onTorchPalette = [this](int index) { m_world.SetTorchPalette(index); };
	m_ui.onMoveAction = [this](MoveAction action) {
		m_world.GetParty().Act(action);
	};
	m_ui.onHandAttack = [this](size_t member, size_t hand) {
		m_world.PartyAttack(member, hand);
	};
	m_ui.onKeysChanged = [this] {
		m_world.GetParty().SetKeys(m_settings.moveKeys);
	};
	// Recorded only — the rebuild would destroy the dropdown mid-callback;
	// Update applies it first thing next frame.
	m_ui.onLanguageSelected = [this](const std::string& code) {
		m_pendingLanguage = code;
	};

	// Editor: a palette "+ New" opens the asset-creation dialog for that category
	// (Walls/Floors/Ceilings import a texture folder; the rest import a model).
	m_mapView.onNewAsset = [this](MapView::PaletteCat cat) {
		const char* key = MapView::CategoryCatalogKey(cat);
		if (!*key) return; // Tools/Structure aren't creatable
		m_assetDialog.Open(loc::Tr(MapView::CategoryNameKey(cat)), key,
						   MapView::CategoryTextureSet(cat), m_settings.theme);
	};
	// Create runs AssetBaker on the picked source (P4c); the dialog stays open in
	// a "baking…" state until Update sees the subprocess finish.
	m_assetDialog.onCreate = [this](const AssetDialog::CreateRequest& req) {
		if (req.name.empty() || req.sourcePath.empty()) {
			log::Warn("asset create: need a name and a source");
			return;
		}
		m_bakeReq = req;
		m_bakeStep = 0;
		if (StartBakeStep()) {
			m_baking = true;
			m_assetDialog.SetBusy(true);
		} else {
			log::Warn("asset create: could not launch AssetBaker");
		}
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
						   if (!Need(m_console, args, 1, "usage: quality <0-3>")) return;
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
						   if (!Need(m_console, args, 1, "usage: lang <code>")) return;
						   m_pendingLanguage = args[0]; // applied next frame
						   m_console.Print("language: " + args[0]);
					   });
	m_console.Register("tp", "teleport the party to a cell",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 2, "usage: tp <x> <z>")) return;
						   const int x = std::atoi(args[0].c_str());
						   const int z = std::atoi(args[1].c_str());
						   if (m_world.GetParty().SetGridPosition(x, z))
							   m_console.Print(std::format("teleported to {},{}", x, z));
						   else
							   m_console.Print(std::format("{},{} is not walkable", x, z));
					   });

	// --- save / load ---
	m_console.Register("save", "save the game to a named slot (default quicksave)",
					   [this](const std::vector<std::string>& args) {
						   if (!m_gameLoaded) {
							   m_console.Print("no game loaded");
							   return;
						   }
						   std::string name = JoinArgs(args);
						   if (name.empty()) name = "quicksave";
						   SaveGame(name);
						   m_console.Print("saved: " + name);
					   });
	m_console.Register("load", "load a save by name (no arg lists saves)",
					   [this](const std::vector<std::string>& args) {
						   if (args.empty()) {
							   const std::vector<SaveSlot> slots = ListSaves();
							   if (slots.empty()) {
								   m_console.Print("no saves");
								   return;
							   }
							   for (const SaveSlot& s : slots)
								   m_console.Print(std::format("  {} [{}] {}", s.name,
															   s.level, s.timestamp));
							   return;
						   }
						   const std::string name = JoinArgs(args);
						   if (LoadGame(SaveSlotPath(name)))
							   m_console.Print("loaded: " + name);
						   else
							   m_console.Print("load failed (see log)");
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
							   map.Sconces().size()));
					   });
	m_console.Register("editor", "open the map in editor mode (off = player map)",
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty() && args[0] == "off") {
							   m_mapView.SetMode(MapView::Mode::Player);
							   m_console.Print("map: player mode");
							   return;
						   }
						   if (m_mapView.IsOpen())
							   m_mapView.SetMode(MapView::Mode::Editor);
						   else
							   m_mapView.Open(MapView::Mode::Editor);
						   m_console.Print("map: editor mode");
					   });
	m_console.Register("goto", "load another level by stem (e.g. goto level2)",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1, "usage: goto <level-stem>"))
							   return;
						   if (m_state != AppState::Playing) {
							   m_console.Print("goto only works in-game");
							   return;
						   }
						   const std::string& stem = args[0];
						   bool known = false;
						   for (const std::string& l : m_project.levels)
							   if (l == stem) known = true;
						   if (!known) {
							   m_console.Print("unknown level: " + stem);
							   return;
						   }
						   // Arrive at the level's start cell (-1 = resolve after load).
						   BeginLevelTransition(stem, -1, -1, Direction::South);
						   m_console.Print("loading " + stem + "...");
					   });
	m_console.Register("savemap", "write the active level's .map/.ent to the project",
					   [this](const std::vector<std::string>&) {
						   if (!m_gameLoaded || (m_state != AppState::Playing &&
												 m_state != AppState::Paused)) {
							   m_console.Print("savemap only works in-game");
							   return;
						   }
						   if (m_world.SaveLevel())
							   m_console.Print("saved level: " + m_world.CurrentLevel());
						   else
							   m_console.Print("save failed (see log)");
					   });
	m_console.Register("synctosource",
					   "copy the active project (edits) into the repo source tree",
					   [this](const std::vector<std::string>&) {
						   const std::string& repo = paths::RepoAssetsDir();
						   if (repo.empty()) {
							   m_console.Print("no source path baked in");
							   return;
						   }
						   namespace fs = std::filesystem;
						   const fs::path src = m_project.folder; // the build-copy project
						   const fs::path dst =
							   fs::path(repo) / "projects" / src.filename();
						   std::error_code ec;
						   fs::create_directories(dst, ec);
						   fs::copy(src, dst,
									fs::copy_options::recursive |
										fs::copy_options::overwrite_existing,
									ec);
						   if (ec)
							   m_console.Print("sync failed: " + ec.message());
						   else
							   m_console.Print("synced " + src.filename().string() +
											   " -> source");
					   });
	m_console.Register("preview", "show a model in the 3D preview (off to close)",
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty() && args[0] == "off") {
							   m_previewMesh.reset();
							   m_console.Print("preview off");
							   return;
						   }
						   if (!Need(m_console, args, 1, "usage: preview <model> (off)"))
							   return;
						   const std::string name = JoinArgs(args);
						   if (!assets::ReadBinaryFile(paths::Asset("models\\" + name + ".gltf"))) {
							   m_console.Print("no model: " + name);
							   return;
						   }
						   m_previewModel = LoadModelOrDie(name + ".gltf");
						   m_previewMesh = std::make_unique<gfx::Mesh>(
							   m_device, m_previewModel.meshes[0]);
						   m_previewMaterial = {};
						   if (!m_previewModel.materials.empty())
							   m_previewMaterial.baseColor =
								   m_previewModel.materials[0].baseColorFactor;
						   m_previewOrbit = 0.0f;
						   m_console.Print("preview: " + name);
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
						   if (!Need(m_console, args, 1, "usage: face <n|e|s|w>")) return;
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
						   if (!Need(m_console, args, 1, "usage: speed <mult>")) return;
						   const float v = static_cast<float>(std::atof(args[0].c_str()));
						   if (v <= 0.0f) {
							   m_console.Print("speed must be > 0");
							   return;
						   }
						   m_world.GetParty().SetSpeed(v);
						   m_console.Print(std::format("speed x{:.2f}", v));
					   });
	m_console.Register("learn", "grant a spell symbol to a member (dev)",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 2,
									 "usage: learn <member 0-3> <fire|earth|air|water>"))
							   return;
						   const size_t m = static_cast<size_t>(std::atoi(args[0].c_str()));
						   SpellSymbol sym;
						   if (m >= m_characters.size()) {
							   m_console.Print("no such member");
							   return;
						   }
						   if (!ParseSymbol(args[1], sym)) {
							   m_console.Print("symbol must be fire/earth/air/water");
							   return;
						   }
						   m_characters[m].Learn(sym);
						   m_ui.RefreshSheet();
						   m_console.Print(std::format("{} learned {}", m_characters[m].name,
													   SymbolId(sym)));
					   });
	m_console.Register("rune", "drop a rune symbol into the party satchel (dev)",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1,
									 "usage: rune <fire|earth|air|water>"))
							   return;
						   SpellSymbol sym;
						   if (!ParseSymbol(args[0], sym)) {
							   m_console.Print("symbol must be fire/earth/air/water");
							   return;
						   }
						   m_satchel.push_back(sym);
						   m_console.Print(std::format("satchel += {}", SymbolId(sym)));
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
						   if (!args.empty()) m_world.SetShadowsEnabled(ArgOn(args[0]));
						   m_console.Print(m_world.ShadowsEnabled() ? "shadows on"
																	: "shadows off");
					   });
	m_console.Register("dust", "toggle volumetric dust (on/off)",
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty()) m_world.SetDustEnabled(ArgOn(args[0]));
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

	// Honor a saved borderless/exclusive display mode now that the window and
	// device exist (windowed at the default size needs nothing).
	ApplyDisplaySettings();
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
				  m_world.Map().Sconces().size(), m_world.MonsterCount());
	});
}

void Game::BeginLevelTransition(const std::string& stem, int x, int z,
								Direction facing, bool stashCurrent) {
	m_world.BeginLevelLoad(stem, stashCurrent); // swap + reset per-level state now
	m_loadQueue.Clear();          // re-stage only the world rebuild (portraits /
	m_loadQueue.SetDoneLabel(loc::Tr("load.done")); // HUD persist across levels)
	m_world.AppendLoadTasks(m_loadQueue);
	m_pendingLevelX = x;
	m_pendingLevelZ = z;
	m_pendingLevelFacing = facing;
	m_state = AppState::LoadingLevel;
	m_stateFrameMark = m_framesRendered;
}

// Launches the AssetBaker command for the current bake step (P4c). Models are a
// single import-model; texture sets import the maps (step 0) then rebake the
// worn block meshes that sample them (step 1).
bool Game::StartBakeStep() {
	const std::string baker = paths::ExecutableDir() + "\\AssetBaker.exe";
	const std::string assets = paths::ExecutableDir() + "\\assets";
	const auto q = [](const std::string& s) { return "\"" + s + "\""; };

	std::string cmd;
	if (!m_bakeReq.textureSet)
		cmd = q(baker) + " import-model " + q(m_bakeReq.sourcePath) + " " + q(assets) +
			  " " + m_bakeReq.name;
	else if (m_bakeStep == 0)
		cmd = q(baker) + " import " + q(m_bakeReq.sourcePath) + " " + q(assets) + " " +
			  m_bakeReq.name;
	else {
		// Bake worn block meshes for just the new set (its kind = the catalog).
		const std::string kind = m_bakeReq.catalogKey == "floors"    ? "floor"
								 : m_bakeReq.catalogKey == "ceilings" ? "ceiling"
																	  : "wall";
		cmd = q(baker) + " wornblock " + kind + " " + m_bakeReq.name + " " + q(assets);
	}
	log::Info("AssetBaker: {}", cmd);
	return m_bake.Start(cmd);
}

// The bake succeeded: append the new entry to the right project catalog and save
// (so the type is usable — model kinds load lazily on first placement). Writes go
// to the asset copy next to the exe, not the git source tree.
void Game::FinishBake() {
	if (Catalog* cat = m_project.CatalogForKey(m_bakeReq.catalogKey)) {
		CatalogEntry e;
		e.id = m_bakeReq.name;
		e.Set("display", m_bakeReq.name);
		if (m_bakeReq.textureSet) {
			e.Set("texture", m_bakeReq.name);
			e.Set("height_scale", std::format("{:.3f}", m_bakeReq.material.heightScale));
		} else {
			e.Set("model", m_bakeReq.name);
			e.Set("texture", m_bakeReq.name);
			e.Set("authored", "1");
			e.Set("solid", "1");
		}
		cat->Add(std::move(e));
		m_project.Save();
		log::Info("Created asset '{}' in {}", m_bakeReq.name, m_bakeReq.catalogKey);
	}
	m_assetDialog.SetBusy(false);
	m_assetDialog.Close();
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

void Game::ResetRoster() {
	// Element-wise so the addresses the party-bar panels and the sheet point at
	// stay valid, keeping each slot's loaded portrait (the defaults carry null).
	const std::vector<Character> fresh = CreateDefaultParty();
	for (size_t i = 0; i < m_characters.size() && i < fresh.size(); ++i) {
		const gfx::Texture* portrait = m_characters[i].portrait;
		m_characters[i] = fresh[i];
		m_characters[i].portrait = portrait;
	}
}

void Game::StartNewGame() {
	m_world.ResetForNewGame();
	ResetRoster();
	m_satchel.clear(); // a fresh party has memorized nothing and holds no runes
	m_ui.RefreshSheet();
	ApplyPartySpeed();

	// A new game always begins on the first level; if a prior game left the world
	// on a deeper level, load the first one fresh (the loading screen handles it).
	const std::string first =
		m_project.levels.empty() ? std::string("level1") : m_project.levels.front();
	if (m_world.CurrentLevel() != first) {
		BeginLevelTransition(first, -1, -1, Direction::South, /*stashCurrent=*/false);
		log::Info("New game started (loading {})", first);
		return; // the LoadingLevel done-handler resumes play
	}

	m_ui.ClearLog();
	m_ui.AddLogLine(loc::Tr("log.descend"));
	m_ui.AddLogLine(loc::Tr("log.shuffle"));
	m_ui.AddLogLine(m_settings.MoveKeysHelp());

	m_ui.ResetHudStatus();
	m_state = AppState::Playing;
	log::Info("New game started");
}

void Game::SaveGame(const std::string& name) {
	if (!m_gameLoaded) {
		log::Warn("SaveGame: no game loaded");
		return;
	}
	SaveData data;
	data.name = name;
	data.timestamp = std::format("{:%Y-%m-%d %H:%M:%S}",
								 std::chrono::floor<std::chrono::seconds>(
									 std::chrono::system_clock::now()));
	m_world.CaptureState(data);
	for (const Character& member : m_characters)
		data.characters.push_back({member.health, member.maxHealth, member.stamina,
								   member.maxStamina, member.mana, member.maxMana,
								   member.knownSymbols});
	for (SpellSymbol s : m_satchel) data.satchel.push_back(static_cast<int>(s));
	WriteSave(data, SaveSlotPath(name));
}

bool Game::LoadGame(const std::string& path) {
	if (!m_gameLoaded) {
		log::Warn("LoadGame: dungeon not loaded yet");
		return false;
	}
	auto data = ReadSave(path);
	if (!data) {
		log::Warn("LoadGame: could not read {}", path);
		return false;
	}

	// Rebuild the baseline (party home, fog cleared, monsters at spawn, palette
	// reset), then lay the save on top.
	m_world.ResetForNewGame();
	ResetRoster();
	for (size_t i = 0; i < m_characters.size() && i < data->characters.size(); ++i) {
		const SaveData::CharState& c = data->characters[i];
		m_characters[i].health = c.health;     m_characters[i].maxHealth = c.maxHealth;
		m_characters[i].stamina = c.stamina;   m_characters[i].maxStamina = c.maxStamina;
		m_characters[i].mana = c.mana;         m_characters[i].maxMana = c.maxMana;
		m_characters[i].knownSymbols = c.knownSymbols;
	}
	m_satchel.clear();
	for (int s : data->satchel)
		if (s >= 0 && s < static_cast<int>(kSymbolCount))
			m_satchel.push_back(static_cast<SpellSymbol>(s));
	m_world.ApplyState(*data); // fills the per-level store + party pose/torch

	m_ui.RefreshSheet();
	ApplyPartySpeed();

	// Route to the saved level. If it is the one already active, restore its
	// live state inline; otherwise load it (arriving at the saved pose, without
	// stashing the throwaway baseline) and let the loader finish the restore.
	if (m_world.CurrentLevel() != data->currentLevel) {
		m_ui.ClearLog();
		BeginLevelTransition(data->currentLevel, data->partyX, data->partyZ,
							 static_cast<Direction>(data->partyFacing),
							 /*stashCurrent=*/false);
		log::Info("Loaded game from {} (loading {})", path, data->currentLevel);
		return true;
	}

	m_world.ApplyActiveSnapshot(); // restore the active level's fog + entity diff
	m_ui.ClearLog(); // SetTorchPalette logged a line during ApplyState
	m_ui.AddLogLine(loc::Tr("log.descend"));
	const Party& party = m_world.GetParty();
	m_ui.ResetHudStatus();
	m_ui.SetHudStatus(party.Facing(), party.GridX(), party.GridZ());
	m_state = AppState::Playing;
	log::Info("Loaded game from {}", path);
	return true;
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
	// The light budget follows the tier; re-point the Video tab's dropdown at it.
	m_settings.maxPointLights = GameSettings::QualityLightBudget(quality);
	m_ui.SyncMaxLights();
	m_settings.Save();
	m_world.ApplyQuality(textureResChanged);
}

void Game::ApplyDisplaySettings() {
	// Resolve the active adapter's outputs so we can position a borderless window
	// or target a monitor for exclusive full-screen.
	const std::vector<gfx::AdapterInfo> adapters = gfx::EnumerateAdapters();
	const gfx::AdapterInfo* active = nullptr;
	for (const gfx::AdapterInfo& a : adapters)
		if (a.luid == m_device.AdapterLuid()) {
			active = &a;
			break;
		}
	const int out = m_settings.displayOutput;
	const gfx::OutputInfo* output =
		(active && out >= 0 && out < static_cast<int>(active->outputs.size()))
			? &active->outputs[static_cast<size_t>(out)]
			: nullptr;

	switch (m_settings.fullscreen) {
	case gfx::FullscreenMode::Windowed: {
		const u32 w = m_settings.displayWidth > 0 ? static_cast<u32>(m_settings.displayWidth)
												  : m_window.Width();
		const u32 h = m_settings.displayHeight > 0
						  ? static_cast<u32>(m_settings.displayHeight)
						  : m_window.Height();
		m_device.SetFullscreen(false, 0, 0, 0); // drop any exclusive state first
		m_window.SetWindowed(w, h);
		break;
	}
	case gfx::FullscreenMode::Borderless: {
		m_device.SetFullscreen(false, 0, 0, 0);
		if (output)
			m_window.SetBorderless(output->x, output->y,
								   static_cast<u32>(output->width),
								   static_cast<u32>(output->height));
		break;
	}
	case gfx::FullscreenMode::Exclusive: {
		u32 w = static_cast<u32>(m_settings.displayWidth);
		u32 h = static_cast<u32>(m_settings.displayHeight);
		if ((w == 0 || h == 0) && output) { // default to the monitor's native size
			w = static_cast<u32>(output->width);
			h = static_cast<u32>(output->height);
		}
		m_device.SetFullscreen(true, static_cast<u32>(out > 0 ? out : 0), w, h);
		break;
	}
	}
}

void Game::RestartApp() {
	// Leave any exclusive full-screen so the new process can claim the display.
	m_device.SetFullscreen(false, 0, 0, 0);
	if (!m_restart.Start("\"" + paths::ExecutableDir() + "\\Dungeon.exe\""))
		log::Warn("Could not relaunch the game for the adapter change");
	m_quitRequested = true;
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
	// A Video-tab adapter/monitor change last frame repopulates the settings page
	// now, for the same reason: the rebuild destroys the dropdown that triggered it.
	m_ui.ApplyPendingVideoRebuild();

	m_ui.UpdateFonts(dt);
	if (m_previewMesh) m_previewOrbit += dt * 0.6f; // spin the editor 3D preview

	// Poll the asset bake (P4c): non-blocking, so the "baking…" dialog stays
	// responsive. A texture import runs a second step (worn meshes) before it's
	// done; on success FinishBake writes the catalog entry.
	if (m_baking && !m_bake.Running()) {
		if (m_bake.ExitCode() != 0) {
			log::Warn("AssetBaker failed (exit {})", m_bake.ExitCode());
			m_baking = false;
			m_assetDialog.SetBusy(false);
		} else if (m_bakeReq.textureSet && m_bakeStep == 0) {
			m_bakeStep = 1; // textures imported — now rebake worn block meshes
			if (!StartBakeStep()) {
				m_baking = false;
				m_assetDialog.SetBusy(false);
			}
		} else {
			FinishBake();
			m_baking = false;
		}
	}

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
			if (!m_pendingLoadPath.empty()) {
				const std::string path = std::exchange(m_pendingLoadPath, {});
				if (!LoadGame(path)) { // bad/corrupt file: fall back to the menu
					m_state = AppState::Menu;
					m_ui.ResetToMainPage();
				}
			} else {
				StartNewGame(); // sets AppState::Playing
			}
		}
		return;

	case AppState::LoadingLevel:
		if (RunLoadTasks()) {
			// Restore this level's saved fog/progress (if visited before — the
			// monsters now exist for the entity diff), then place the party.
			m_world.ApplyActiveSnapshot();
			int px = m_pendingLevelX, pz = m_pendingLevelZ;
			if (px < 0) { // sentinel: arrive at the new level's start cell
				px = m_world.Map().StartX();
				pz = m_world.Map().StartZ();
			}
			m_world.PlacePartyAt(px, pz, m_pendingLevelFacing);
			m_ui.ClearLog();
			m_state = AppState::Playing;
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
	// The asset-creation dialog is modal over the editor: while it is up it owns
	// input and the world/overlay are frozen.
	if (m_assetDialog.IsOpen()) {
		m_assetDialog.Update(input, static_cast<float>(m_window.Width()),
							 static_cast<float>(m_window.Height()), dt);
		return;
	}

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
		if (auto t = m_world.ConsumeLevelTransition()) {
			m_mapView.Close(); // a stair step starts a new level load
			BeginLevelTransition(t->level, t->x, t->z, t->facing);
			return;
		}
		Party& party = m_world.GetParty();
		m_ui.SetHudStatus(party.Facing(), party.GridX(), party.GridZ());
		return;
	}

	// Esc freezes the world and opens the pause menu.
	if (input.WasKeyPressed(VK_ESCAPE)) {
		m_audio.Play(m_sounds.click, 0.5f);
		m_ui.ResetToMainPage();
		m_ui.RebuildPauseMenu(); // Load entry tracks whether a save now exists
		m_state = AppState::Paused;
		return;
	}

	// UI first so it can consume the mouse; keyboard always reaches the party.
	m_ui.UpdateHud(input, dt);
	// A portrait click may have opened the character sheet — freeze now
	// rather than simulating one more frame.
	if (m_state != AppState::Playing) return;
	m_world.Update(input, wdt, m_time);
	if (auto t = m_world.ConsumeLevelTransition()) {
		BeginLevelTransition(t->level, t->x, t->z, t->facing);
		return;
	}

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

	// The full-screen editor map covers everything, so skip the 3D scene (and
	// the HUD below) while it is up — nothing else needs drawing behind it.
	const bool editorMap = m_state == AppState::Playing && m_mapView.IsOpen() &&
						   m_mapView.CurrentMode() == MapView::Mode::Editor;

	// The offscreen 3D preview feeds from the asset dialog's picked model (P4b)
	// or the dev `preview` command (P4a). Render() redirects the OM, so rebind
	// the back buffer for the 2D pass.
	const gfx::Mesh* pvMesh = nullptr;
	gfx::MaterialParams pvMat;
	float pvOrbit = 0.0f;
	if (m_assetDialog.IsOpen() && m_assetDialog.HasPreview()) {
		pvMesh = &m_assetDialog.PreviewMesh();
		pvMat = m_assetDialog.PreviewMaterial();
		pvOrbit = m_assetDialog.Orbit();
	} else if (m_previewMesh) {
		pvMesh = m_previewMesh.get();
		pvMat = m_previewMaterial;
		pvOrbit = m_previewOrbit;
	}
	const bool devPreviewFullscreen = m_previewMesh && !m_assetDialog.IsOpen();

	if (pvMesh) {
		m_modelPreview.Render(list, m_renderer, *pvMesh, pvMat, pvOrbit);
		m_device.BindBackBuffer(list);
	}
	// The 3D scene draws during play and under the pause/character-sheet
	// overlays (frozen); Loading and Menu are 2D-only. The full-screen dev
	// preview replaces it; the editor map and dialog skip it too.
	else if ((m_state == AppState::Playing || m_state == AppState::Paused ||
			  m_state == AppState::CharacterSheet) &&
			 !editorMap) {
		m_world.RenderShadowMaps(list);
		m_world.RenderScene(list);
	}

	// 2D pass.
	m_spriteBatch.Begin(list, m_device.Width(), m_device.Height());
	switch (m_state) {
	case AppState::Loading:     m_ui.RenderLoadingScreen(m_loadQueue); break;
	case AppState::Menu:        m_ui.RenderMenuOverlay(); break;
	case AppState::LoadingGame:  m_ui.RenderGameLoadingScreen(m_loadQueue); break;
	case AppState::LoadingLevel: m_ui.RenderGameLoadingScreen(m_loadQueue); break;
	case AppState::Playing: {
		const float dw = static_cast<float>(m_device.Width());
		const float dh = static_cast<float>(m_device.Height());
		if (editorMap) {
			// Full-screen editor — drawn alone, no HUD or scene behind it.
			m_mapView.Render(m_spriteBatch, m_settings.theme, MapPanel(dw, dh));
		} else {
			m_ui.RenderHud();
			if (m_mapView.IsOpen()) {
				// Player map: dim the scene behind the 80% panel, over the HUD.
				m_spriteBatch.DrawRect({0, 0, dw, dh}, {0, 0, 0, 0.45f});
				m_mapView.Render(m_spriteBatch, m_settings.theme, MapPanel(dw, dh));
			}
		}
		break;
	}
	case AppState::Paused:      m_ui.RenderPauseOverlay(); break;
	case AppState::CharacterSheet: m_ui.RenderCharacterSheetOverlay(); break;
	}
	const float dw = static_cast<float>(m_device.Width());
	const float dh = static_cast<float>(m_device.Height());
	if (m_assetDialog.IsOpen()) {
		// The asset dialog overlays the editor; it draws its own frame, then we
		// blit the rendered preview model into its preview pane.
		m_assetDialog.Render(m_spriteBatch, dw, dh);
		if (m_assetDialog.HasPreview())
			m_spriteBatch.DrawSprite(m_assetDialog.PreviewRect(dw, dh), {0, 0, 1, 1},
									 m_modelPreview.Srv(), {1, 1, 1, 1});
	} else if (devPreviewFullscreen) {
		// Dev `preview` command: dim the frame and blit the model full-screen.
		m_spriteBatch.DrawRect({0, 0, dw, dh}, {0.04f, 0.04f, 0.06f, 1.0f});
		const float s = std::min(dw, dh) * 0.85f;
		m_spriteBatch.DrawSprite({(dw - s) * 0.5f, (dh - s) * 0.5f, s, s},
								 {0, 0, 1, 1}, m_modelPreview.Srv(), {1, 1, 1, 1});
	}
	if (m_console.IsOpen())
		m_console.Render(m_spriteBatch, m_device, static_cast<float>(m_device.Width()),
						 static_cast<float>(m_device.Height()));
	m_spriteBatch.End();

	++m_framesRendered;
}

} // namespace dungeon::game
