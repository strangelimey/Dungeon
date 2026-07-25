// ============================================================================
// Game/Game_DevCommands.cpp — split out of Game.cpp to keep files small (see Game.h).
// Dev-console command registration + its arg helpers.
// ============================================================================
#include "Game/Game.h"

#include "Assets/File.h"
#include "Assets/Image.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Graphics/DisplayEnum.h"
#include "Graphics/Texture.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <thread>
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

// Parses one symbol-id arg (fire/earth/air/water); on a bad token prints the
// shared usage line and returns false, so a command can `if (!...) return;`.
bool ParseSymbolArg(DevConsole& console, const std::string& arg, SpellSymbol& out) {
	if (ParseSymbol(arg, out)) return true;
	console.Print("symbol must be fire/earth/air/water");
	return false;
}

} // namespace
void Game::RegisterDevCommands() {
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
	m_console.Register("groups", "list monster groups (id: count [kinds] @ cell#slot)",
					   [this](const std::vector<std::string>&) {
						   for (const std::string& line : m_world.GroupsReport())
							   m_console.Print(line);
					   });
	m_console.Register(
		"threadspawn",
		"spawn a demo worker on the thread manager (arg: busy ms/tick, default 500)",
		[this](const std::vector<std::string>& args) {
			const int busyMs = args.empty() ? 500 : std::atoi(args[0].c_str());
			const threads::WorkerId id = m_threads.Spawn(
				[busyMs](const threads::Tick& t) {
					// A long but CANCELLABLE unit of work: long enough to trip the
					// watchdog (so it shows Stalled), yet it polls the stop token so
					// 'kill' still takes effect promptly.
					const auto end = std::chrono::steady_clock::now() +
									 std::chrono::milliseconds(busyMs);
					while (std::chrono::steady_clock::now() < end &&
						   !t.stop.stop_requested())
						std::this_thread::sleep_for(std::chrono::milliseconds(5));
				},
				{"demo.worker", 1.0f, /*watchdogMs=*/200, /*autoRestart=*/true});
			m_console.Print(
				std::format("spawned demo worker #{} ({} ms/tick)", id, busyMs));
		});
	m_console.Register(
		"threadwedge", "spawn a WEDGED demo worker (ignores its stop token) to test hard kill",
		[this](const std::vector<std::string>&) {
			const threads::WorkerId id = m_threads.Spawn(
				[](const threads::Tick&) {
					// Deliberately does NOT check the stop token: cooperative stop
					// can't end this — only a hard Kill (force-terminate) will.
					while (true) std::this_thread::sleep_for(std::chrono::milliseconds(50));
				},
				{"demo.wedged", 1.0f, /*watchdogMs=*/200});
			m_console.Print(std::format("spawned WEDGED worker #{} (use kill)", id));
		});
	m_console.Register("throttle", "manual global cadence scale (arg: e.g. 0.5; 1 = normal)",
					   [this](const std::vector<std::string>& args) {
						   const float s = args.empty() ? 1.0f
										   : static_cast<float>(std::atof(args[0].c_str()));
						   m_governorAuto = false; // manual override turns auto off
						   m_threads.SetGlobalThrottle(s);
						   m_console.Print(std::format("global throttle: {:.2f}x (auto off)",
													   m_threads.GlobalThrottle()));
					   });
	m_console.Register(
		"governor", "adaptive thread throttle (usage: governor auto [targetFps] | off)",
		[this](const std::vector<std::string>& args) {
			if (!args.empty() && args[0] == "auto") {
				m_governorAuto = true;
				if (args.size() >= 2) {
					const float fps = static_cast<float>(std::atof(args[1].c_str()));
					if (fps > 1.0f) m_governorTargetMs = 1000.0f / fps;
				}
				m_console.Print(std::format("governor: AUTO (target {:.1f} ms / {:.0f} fps)",
											m_governorTargetMs, 1000.0f / m_governorTargetMs));
			} else { // "off" or anything else
				m_governorAuto = false;
				m_threads.SetGlobalThrottle(1.0f);
				m_console.Print("governor: off (1.00x)");
			}
		});
	m_console.Register("threadprio", "set a worker's OS priority (usage: threadprio <id> <-2..2>)",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 2, "usage: threadprio <id> <-2..2>"))
							   return;
						   m_threads.SetPriority(
							   static_cast<threads::WorkerId>(std::atoi(args[0].c_str())),
							   std::clamp(std::atoi(args[1].c_str()), -2, 2));
						   m_console.Print("priority set");
					   });
	m_console.Register("threadaffinity", "pin a worker to a CPU mask (usage: threadaffinity <id> <mask>)",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 2, "usage: threadaffinity <id> <mask>"))
							   return;
						   m_threads.SetAffinity(
							   static_cast<threads::WorkerId>(std::atoi(args[0].c_str())),
							   std::strtoull(args[1].c_str(), nullptr, 0));
						   m_console.Print("affinity set");
					   });
	m_console.Register("threadreap", "drop stopped (dead/quarantined) workers from the registry",
					   [this](const std::vector<std::string>&) {
						   const size_t before = m_threads.Count();
						   m_threads.Reap();
						   m_console.Print(std::format("reaped {} worker(s)",
													   before - m_threads.Count()));
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
	m_console.Register("savemap", "write every edited level's .map/.ent to the project",
					   [this](const std::vector<std::string>&) {
						   if (!m_gameLoaded || (m_state != AppState::Playing &&
												 m_state != AppState::Paused)) {
							   m_console.Print("savemap only works in-game");
							   return;
						   }
						   // The active level plus every level whose stash holds
						   // in-memory edits — remote map edits included.
						   const std::vector<std::string> saved =
							   m_world.SaveAllLevels();
						   if (!saved.empty()) {
							   std::string list;
							   for (const std::string& s : saved)
								   list += (list.empty() ? "" : ", ") + s;
							   m_console.Print("saved levels: " + list);
						   } else {
							   m_console.Print("save failed (see log)");
						   }
					   });
	m_console.Register("synctosource",
					   "copy the active project (edits) into the repo source tree",
					   [this](const std::vector<std::string>&) {
						   m_console.Print(SyncProjectToSource()
											   ? "synced project -> source"
											   : "sync failed (see log)");
					   });
	m_console.Register("preview", "show a model in the 3D preview (off to close)",
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty() && args[0] == "off") {
							   // In-flight frames may still draw the mesh
							   if (m_previewMesh) m_device.WaitIdle();
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
						   // Replacing frees the old mesh — drain in-flight frames
						   if (m_previewMesh) m_device.WaitIdle();
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
	m_console.Register("buttons", "list buttons (id, cell, state)",
					   [this](const std::vector<std::string>&) {
						   const std::vector<std::string> list = m_world.ButtonList();
						   if (list.empty()) {
							   m_console.Print("no buttons");
							   return;
						   }
						   for (const std::string& l : list) m_console.Print("  " + l);
					   });
	m_console.Register("press", "toggle the button in cell x,z (exercises save)",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 2, "usage: press <x> <z>")) return;
						   const int x = std::atoi(args[0].c_str());
						   const int z = std::atoi(args[1].c_str());
						   bool on = false;
						   if (m_world.ToggleButtonAt(x, z, on))
							   m_console.Print(std::format("button {},{} -> {}", x, z,
														   on ? "on" : "off"));
						   else
							   m_console.Print(std::format("no button at {},{}", x, z));
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
						   if (!ParseSymbolArg(m_console, args[1], sym)) return;
						   m_characters[m].Learn(sym);
						   m_ui.RefreshSheet();
						   m_console.Print(std::format("{} learned {}", m_characters[m].name,
													   SymbolId(sym)));
					   });
	m_console.Register("rune", "give a rune tablet to the lead member's pack (dev)",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1,
									 "usage: rune <fire|earth|air|water>"))
							   return;
						   SpellSymbol sym;
						   if (!ParseSymbolArg(m_console, args[0], sym)) return;
						   const std::string typeId = RuneItemId(sym);
						   if (m_characters.empty() ||
							   !m_characters[0].inventory.Stow(typeId))
							   m_console.Print("pack full (or no party)");
						   else
							   m_console.Print(std::format("pack += {}", typeId));
					   });
	m_console.Register("give", "stow an items.cat item in a member's pack (dev)",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1,
									 "usage: give <item id> [member 0-3]"))
							   return;
						   const size_t m = args.size() > 1
							   ? static_cast<size_t>(std::atoi(args[1].c_str())) : 0;
						   if (m >= m_characters.size()) {
							   m_console.Print("no such member");
							   return;
						   }
						   if (!m_project.HasItem(args[0])) {
							   m_console.Print(std::format("no item '{}' in items/weapons/armor", args[0]));
							   return;
						   }
						   if (!m_characters[m].inventory.Stow(args[0]))
							   m_console.Print("pack full");
						   else
							   m_console.Print(std::format("{} pack += {}",
														   m_characters[m].name, args[0]));
					   });
	m_console.Register("threat",
					   "list per-member threat for every monster holding a grudge (dev)",
					   [this](const std::vector<std::string>&) {
						   const std::vector<std::string> lines = m_world.ThreatReport();
						   if (lines.empty()) {
							   m_console.Print("no threat anywhere");
							   return;
						   }
						   for (const std::string& l : lines) m_console.Print("  " + l);
					   });
	m_console.Register("cast", "cast a spell by symbol sequence (dev): cast <member> [hand 0/1] <sym>...",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 2,
									 "usage: cast <member 0-3> [hand 0/1] <sym> [sym...]"))
							   return;
						   const size_t m = static_cast<size_t>(std::atoi(args[0].c_str()));
						   if (m >= m_characters.size()) {
							   m_console.Print("no such member");
							   return;
						   }
						   // Optional hand (credits that hand's quick-cast MRU) —
						   // symbol names are never "0"/"1", so the token is
						   // unambiguous.
						   int hand = -1;
						   size_t first = 1;
						   if (args.size() > 2 && (args[1] == "0" || args[1] == "1")) {
							   hand = std::atoi(args[1].c_str());
							   first = 2;
						   }
						   std::vector<SpellSymbol> seq;
						   for (size_t i = first; i < args.size(); ++i) {
							   SpellSymbol s;
							   if (!ParseSymbolArg(m_console, args[i], s)) return;
							   seq.push_back(s);
						   }
						   const bool ok = m_world.CastSpell(m, seq, hand);
						   m_console.Print(ok ? "cast away" : "no cast (fizzle / no mana / unknown)");
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
	m_console.Register("dust", "volumetric dust: on/off, or a density (default 0.075)",
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty()) {
							   if (args[0] == "on" || args[0] == "off") {
								   m_world.SetDustEnabled(ArgOn(args[0]));
							   } else {
								   m_world.SetDustDensity(
									   static_cast<float>(std::atof(args[0].c_str())));
								   m_world.SetDustEnabled(true);
							   }
						   }
						   m_console.Print(m_world.DustEnabled()
											   ? std::format("dust on, density {:.3f}",
															 m_world.DustDensity())
											   : "dust off");
					   });
	m_console.Register("haze", "dust ambient pickup (mood tuning, default 0.9)",
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty())
							   m_world.SetHazeAmbient(
								   static_cast<float>(std::atof(args[0].c_str())));
						   m_console.Print(
							   std::format("haze ambient {:.2f}", m_world.HazeAmbient()));
					   });
	m_console.Register("ambient", "scale the ambient fill (mood tuning, default 1.0)",
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty())
							   m_world.SetAmbientScale(
								   static_cast<float>(std::atof(args[0].c_str())));
						   m_console.Print(
							   std::format("ambient x{:.2f}", m_world.AmbientScale()));
					   });
	m_console.Register("fov", "set camera field of view in degrees (default 70)",
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty())
							   m_world.SetFov(static_cast<float>(std::atof(args[0].c_str())));
						   m_console.Print(std::format("fov {:.0f}", m_world.Fov()));
					   });
}

// ============================================================================
// Staged loading. Each task is one frame's worth of blocking work; a loading
// screen renders between tasks. The boot list is the bare minimum to reach
// the landing page fast; the heavy dungeon load runs later, behind its own
// progress screen, when the player first starts a game.
// ============================================================================


} // namespace dungeon::game
