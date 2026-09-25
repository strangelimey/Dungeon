// ============================================================================
// Game/Game_DevCommands.cpp — split out of Game.cpp to keep files small (see Game.h).
// Dev-console command registration: the general commands (quit, quality, save/
// load, the editor's level commands, navigation, the renderer's mood knobs) and
// the typeface audition. The rest live by concern in Game_DevWorld /
// _DevDungeons / _DevDiagnostics / _DevParty / _DevEval, sharing their arg
// helpers through Game/DevCommandArgs.h.
// ============================================================================
#include "Game/Game.h"

#include "Assets/File.h"
#include "Game/Serialize.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/DevCommandArgs.h"
#include "Game/GenerateKnobs.h"
#include "Game/Threat.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <string>
#include <utility>

namespace dungeon::game {

using devargs::Need;
using devargs::JoinArgs;
using devargs::ArgOn;

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
						   m_pendingQuality = static_cast<Quality>(q); // applied next frame
						   m_console.Print(std::format("quality set to {}", q));
					   });
	m_console.Register("framecap",
					   "cap the frame rate to the window's monitor (on/off)",
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty()) m_device.SetFrameCapEnabled(ArgOn(args[0]));
						   const int hz = m_device.FrameCapHz();
						   // key=value to the log so a harness can read the target it
						   // is meant to hold the frame rate against, rather than
						   // hardcoding this machine's monitor.
						   log::Info("framecap enabled={} hz={} monitor={}",
									 m_device.FrameCapEnabled() ? 1 : 0, hz,
									 m_device.RefreshHz());
						   m_console.Print(
							   m_device.FrameCapEnabled()
								   ? std::format("frame cap ON - {} Hz (monitor {} Hz / "
												 "present interval)",
												 hz, m_device.RefreshHz())
								   : std::format("frame cap OFF - paced by DWM, which on a "
												 "mixed-refresh desktop is the FASTEST "
												 "monitor, not this one ({} Hz)",
												 m_device.RefreshHz()));
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
						   if (m_world->GetParty().SetGridPosition(x, z))
							   m_console.Print(std::format("teleported to {},{}", x, z));
						   else
							   m_console.Refuse(
								   std::format("{},{} is not walkable", x, z));
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
						   if (SaveGame(name)) m_console.Print("saved: " + name);
						   else m_console.Refuse("not saved (see log)");
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
						   const Party& p = m_world->GetParty();
						   static const char* kDirs[] = {"north", "east", "south", "west"};
						   m_console.Print(std::format("{},{} facing {}", p.GridX(),
													   p.GridZ(), kDirs[p.Facing() & 3]));
					   });
	// A FINGERPRINT OF THE STATIC LAYER, not just its size. Every other readout
	// in the harness describes the party or the creatures standing on the map —
	// which is exactly how the first `reset` equivalence test came back
	// "identical" while the world was still an empty carved box. The WALKABLE
	// count is the load-bearing one: `arena` walls every cell before carving, so
	// a map that never came back shows up here and nowhere else.
	m_console.Register("mapinfo", "print dungeon size and a static-layer fingerprint",
					   [this](const std::vector<std::string>&) {
						   const DungeonMap& map = m_world->Map();
						   int walkable = 0;
						   for (int z = 0; z < map.Height(); ++z)
							   for (int x = 0; x < map.Width(); ++x)
								   if (map.IsWalkable(x, z)) ++walkable;
						   m_console.Print(std::format(
							   "{}x{} map, start {},{}, {} walkable, {} monsters, "
							   "{} torches, {} braziers",
							   map.Width(), map.Height(), map.StartX(), map.StartZ(),
							   walkable, m_world->MonsterCount(),
							   map.Sconces().size(), map.Braziers().size()));
					   });
	m_console.Register("groups", "list monster groups (id: count [kinds] @ cell#slot)",
					   [this](const std::vector<std::string>&) {
						   for (const std::string& line : m_world->GroupsReport())
							   m_console.Print(line);
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
	m_console.Register("goto", "load another level by stem (e.g. goto crypt2)",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1, "usage: goto <level-stem>"))
							   return;
						   if (m_state != AppState::Playing) {
							   m_console.Refuse(std::format(
								   "goto only works in-game (state: {})", StateName()));
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
	// THE RANKING DIFFICULTY PICKS BY (docs/level-building.md P4), readable
	// before anything is tuned against it. With tags, the pool exactly as the
	// generator draws it for that theme; without, every kind. Machine-readable
	// lines (`threat <id> <threat> ...`) so a harness can join them to a level's
	// monsters.
	m_console.Register(
		"threat",
		"monster kinds ranked by derived threat: threat [tag ...] (a theme's pool)",
		[this](const std::vector<std::string>& args) {
			generate::Params p;
			FillPools(p, args);
			std::vector<size_t> order(p.monsterIds.size());
			for (size_t i = 0; i < order.size(); ++i) order[i] = i;
			std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
				return p.monsterThreat[a] < p.monsterThreat[b];
			});
			for (const size_t i : order) {
				const threat::Parts t = ThreatOf(*m_project.monsters.Find(p.monsterIds[i]));
				// melee= and shot= are per second BEFORE the ranged edge; offence=
				// is the better of the two with the edge applied, so a shot's
				// weight in the ranking reads straight off the line.
				m_console.Print(std::format(
					"threat {} {:.2f} offence={:.2f} melee={:.2f} shot={:.2f} "
					"toughness={:.1f} hit={:.2f} behit={:.2f}",
					p.monsterIds[i], t.threat, t.offence, t.melee, t.shot, t.toughness,
					t.hit, t.beHit));
			}
			m_console.Print(std::format("threat: {} kind(s){}", order.size(),
										args.empty() ? "" : " in that theme's pool"));
		});
	m_console.Register("generate",
					   "rough out a new level: generate [dungeon|again] [knob:value ...] | dialog [new|off|tab <n>] | "
					   "preset [list|save|load|delete] [name] | play [stem] "
					   "(a new floor of the viewed dungeon by default, and the view jumps "
					   "to it; `again` rerolls the VIEWED level in place, as the dialog's "
					   "Regenerate does; knobs as the dialog names them, e.g. path:8 "
					   "seed:7 - unset ones keep the dialog's)",
					   [this](const std::vector<std::string>& args) {
						   if (!m_gameLoaded || (m_state != AppState::Playing &&
												 m_state != AppState::Paused)) {
							   // A REFUSAL, naming the state: this answering "only
							   // works in-game" to a party wiped with the console
							   // open read as the game losing its state, and as a
							   // Print a script carried on past it as if it ran.
							   m_console.Refuse(std::format(
								   "generate only works in-game (state: {})", StateName()));
							   return;
						   }
						   // The DIALOG itself, through the same entry points as its
						   // two toolbar buttons - so the UI sweep (InGameTest.ps1)
						   // can audit both modes without a mouse.
						   if (!args.empty() && args[0] == "dialog") {
							   const std::string mode = args.size() > 1 ? args[1] : "";
							   if (mode == "off")
								   m_generateDialog.Close();
							   else if (mode == "tab" && args.size() > 2)
								   m_generateDialog.ShowTab(std::atoi(args[2].c_str()));
							   else if (mode == "new" && m_mapView.onNewLevel)
								   m_mapView.onNewLevel(m_mapView.ViewedDungeon());
							   else if (m_mapView.onGenerate)
								   m_mapView.onGenerate();
							   m_console.Print(std::format(
								   "generate dialog: {}",
								   !m_generateDialog.IsOpen() ? "closed"
								   : m_generateDialog.GetMode() ==
										   GenerateDialog::Mode::Create
									   ? "create"
									   : "regenerate"));
							   return;
						   }
						   // PLAY (P5): the dialog's Play buttons, without a mouse.
						   if (!args.empty() && args[0] == "play") {
							   const std::string stem =
								   args.size() > 1 ? args[1] : m_mapView.ViewedLevel();
							   m_console.Print(PlayLevel(stem)
												   ? "generate: playing " + stem
												   : "generate: cannot play " + stem);
							   return;
						   }
						   // PRESETS (P4b), through the same Game functions the
						   // dialog's Presets tab calls.
						   if (!args.empty() && args[0] == "preset") {
							   const std::string op = args.size() > 1 ? args[1] : "list";
							   const std::string name = args.size() > 2 ? args[2] : "";
							   if (op == "save") {
								   const std::string id =
									   SaveGenPreset(name, m_generateDialog.Knobs());
								   m_console.Print(id.empty() ? "preset: not saved"
															  : "preset: saved " + id);
							   } else if (op == "load") {
								   generate::Params p = m_generateDialog.Knobs();
								   if (LoadGenPreset(name, p)) {
									   m_generateDialog.SetKnobs(p);
									   m_console.Print("preset: loaded " + name + " (" +
													   generate::Encode(p) + ")");
								   } else {
									   m_console.Print("preset: no such preset " + name);
								   }
							   } else if (op == "delete") {
								   m_console.Print(DeleteGenPreset(name)
													   ? "preset: deleted " + name
													   : "preset: no such preset " + name);
							   } else {
								   // Name AND recipe, so a harness can see exactly what
								   // a save stored (no seed, by design).
								   for (const std::string& n : GenPresetNames())
									   m_console.Print("preset " + n + " " +
													   m_project.genpresets.Find(n)->Get("knobs", ""));
							   }
							   return;
						   }
						   // The same knobs the dialog holds, overridden by name
						   // through the same table - so a scripted run and a
						   // dialog run cannot disagree about what a knob means.
						   generate::Params p = m_generateDialog.Knobs();
						   std::string line, dungeon = m_mapView.ViewedDungeon();
						   bool again = false;
						   for (const std::string& a : args)
							   if (a == "again")
								   again = true;
							   else if (a.find(':') == std::string::npos)
								   dungeon = a; // a bare word names the dungeon
							   else
								   line += a + ' ';
						   generate::Decode(line, p);
						   // The dialog's two buttons, without a mouse: a new floor
						   // (and the view follows it, as the dialog's does), or a
						   // reroll of the one being viewed.
						   std::string stem;
						   if (again) {
							   if (RegenerateViewedLevel(p)) stem = m_mapView.ViewedLevel();
						   } else {
							   stem = CreateNewLevel(dungeon, &p);
							   if (!stem.empty()) m_mapView.SetViewLevel(stem);
						   }
						   if (stem.empty()) {
							   m_console.Print("generate: failed");
							   return;
						   }
						   m_console.Print(std::format(
							   "generate: wrote {} ({})", stem, generate::Encode(p)));
						   m_console.Print("generate: built " + GenReportText());
						   // The dialog shows the knobs that built what it reports
						   // on, or its sliders would contradict its own report line.
						   m_generateDialog.SetKnobs(p);
						   ShowGenReport(stem);
						   // A generated level is CHECKED immediately: the whole
						   // reason the lock ordering is built by construction is
						   // so this passes, and saying so is how you find out it
						   // stopped.
						   const std::vector<validate::Issue> issues = ValidateProject();
						   int errors = 0;
						   for (const validate::Issue& i : issues)
							   if (i.severity == validate::Severity::Error) ++errors;
						   m_console.Print(std::format(
							   "generate: check says {} error(s), {} warning(s)",
							   errors, issues.size() - errors));
						   for (const validate::Issue& i : issues)
							   if (i.level == stem)
								   m_console.Print(std::format(
									   "  {} {} @{},{} {} {}",
									   i.severity == validate::Severity::Error ? "ERR"
																			   : "warn",
									   i.level, i.x, i.z, i.messageKey, i.a));
					   });
	m_console.Register("validate", "check the whole project for playability faults",
					   [this](const std::vector<std::string>&) {
						   if (!m_gameLoaded || (m_state != AppState::Playing &&
												 m_state != AppState::Paused)) {
							   m_console.Refuse(std::format(
								   "validate only works in-game (state: {})", StateName()));
							   return;
						   }
						   const std::vector<validate::Issue> issues = ValidateProject();
						   if (issues.empty()) {
							   m_console.Print("validate: clean - no faults found");
							   return;
						   }
						   int errors = 0;
						   for (const validate::Issue& i : issues)
							   if (i.severity == validate::Severity::Error) ++errors;
						   m_console.Print(std::format("validate: {} error(s), {} warning(s)",
													   errors, issues.size() - errors));
						   for (const validate::Issue& i : issues) {
							   // The console is dev-facing, so the loc KEY is
							   // printed rather than the translation: it names the
							   // check that fired, which is what you want when the
							   // question is "why did this fire".
							   std::string where = i.level;
							   if (i.x >= 0) where += std::format(" @{},{}", i.x, i.z);
							   m_console.Print(std::format(
								   "  {} {} {} {}",
								   i.severity == validate::Severity::Error ? "ERR " : "warn",
								   where, i.messageKey, i.a));
						   }
					   });
	m_console.Register(
		"undo", "undo one editor step (the toolbar's < / Ctrl+Z)",
		[this](const std::vector<std::string>&) {
			// The editor's history, reachable without a keyboard shortcut. It
			// is ONE history across the tiers now (a world paint and a level
			// paint land in the same stack), so this takes back whichever came
			// last — which is the behaviour worth being able to test.
			if (!m_world->CanUndo()) {
				m_console.Print("nothing to undo");
				return;
			}
			m_world->Undo();
			m_console.Print("undone");
		});
	m_console.Register(
		"redo", "redo one editor step (the toolbar's > / Ctrl+Y)",
		[this](const std::vector<std::string>&) {
			if (!m_world->CanRedo()) {
				m_console.Print("nothing to redo");
				return;
			}
			m_world->Redo();
			m_console.Print("redone");
		});
	m_console.Register("savemap",
					   "write every edited level's .map/.ent, and the world, to the project",
					   [this](const std::vector<std::string>&) {
						   if (!m_gameLoaded || (m_state != AppState::Playing &&
												 m_state != AppState::Paused)) {
							   m_console.Refuse(std::format(
								   "savemap only works in-game (state: {})", StateName()));
							   return;
						   }
						   // The active level plus every level whose stash holds
						   // in-memory edits — remote map edits included.
						   const std::vector<std::string> saved =
							   m_world->SaveAllLevels();
						   if (!saved.empty()) {
							   std::string list;
							   for (const std::string& s : saved)
								   list += (list.empty() ? "" : ", ") + s;
							   m_console.Print("saved levels: " + list);
						   } else {
							   m_console.Print("save failed (see log)");
						   }
						   // AND THE WORLD, which is a level's peer now rather
						   // than a hand-authored file the editor never touched.
						   // Reported separately: "saved levels" answering for
						   // the world too would hide a world that failed.
						   if (m_worldMap)
							   m_console.Print(SaveWorld() ? "saved world"
														   : "world save failed");
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
						   const std::vector<std::string> list = m_world->MonsterList();
						   if (list.empty()) {
							   m_console.Print("no monsters");
							   return;
						   }
						   for (const std::string& l : list) m_console.Print("  " + l);
					   });
	m_console.Register("buttons", "list buttons (id, cell, state)",
					   [this](const std::vector<std::string>&) {
						   const std::vector<std::string> list = m_world->ButtonList();
						   if (list.empty()) {
							   m_console.Print("no buttons");
							   return;
						   }
						   for (const std::string& l : list) m_console.Print("  " + l);
					   });
	m_console.Register("smash",
					   "damage what is breakable in a cell (dev): smash <x> <z> [amount]",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 2,
									 "usage: smash <x> <z> [amount]"))
							   return;
						   const int x = std::atoi(args[0].c_str());
						   const int z = std::atoi(args[1].c_str());
						   const float amount =
							   args.size() > 2 ? std::strtof(args[2].c_str(), nullptr)
											   : 100.0f;
						   const int n = m_world->SmashAt(x, z, amount);
						   m_console.Print(
							   n > 0 ? std::format("struck {} breakable(s) at {},{}", n,
												   x, z)
									 : std::format("nothing breakable at {},{}", x, z));
					   });
	m_console.Register("press", "toggle the button in cell x,z (exercises save)",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 2, "usage: press <x> <z>")) return;
						   const int x = std::atoi(args[0].c_str());
						   const int z = std::atoi(args[1].c_str());
						   bool on = false;
						   if (m_world->ToggleButtonAt(x, z, on))
							   m_console.Print(std::format("button {},{} -> {}", x, z,
														   on ? "on" : "off"));
						   else
							   m_console.Print(std::format("no button at {},{}", x, z));
					   });
	m_console.Register("lights", "print active point-light count",
					   [this](const std::vector<std::string>&) {
						   m_console.Print(std::format("{} active point lights",
													   m_world->ActiveLightCount()));
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
							   m_console.Refuse("direction must be n/e/s/w");
							   return;
						   }
						   m_world->GetParty().SetFacing(facing);
						   m_console.Print("facing set");
					   });
	m_console.Register("home", "teleport the party to the start cell",
					   [this](const std::vector<std::string>&) {
						   const DungeonMap& map = m_world->Map();
						   m_world->GetParty().SetGridPosition(map.StartX(), map.StartZ());
						   m_console.Print(std::format("home at {},{}", map.StartX(),
													   map.StartZ()));
					   });
	m_console.Register("speed", "set party pace multiplier",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1, "usage: speed <mult>")) return;
						   const float v = static_cast<float>(std::atof(args[0].c_str()));
						   if (v <= 0.0f) {
							   m_console.Refuse("speed must be > 0");
							   return;
						   }
						   m_world->GetParty().SetSpeed(v);
						   m_console.Print(std::format("speed x{:.2f}", v));
					   });

	m_console.Register("noclip", "toggle walking through walls",
					   [this](const std::vector<std::string>&) {
						   Party& p = m_world->GetParty();
						   p.SetNoclip(!p.Noclip());
						   m_console.Print(p.Noclip() ? "noclip on" : "noclip off");
					   });

	// --- fonts: the audition (docs/fonts.md Phase 4) ---
	m_console.Register(
		"fonts", "show each role's typeface, the installed faces, and the live atlases",
		[this](const std::vector<std::string>&) {
			m_console.Print("roles:");
			for (int i = 0; i < ui::kFontRoleCount; ++i) {
				const auto role = static_cast<ui::FontRole>(i);
				const ui::FaceSpec& spec = m_fonts.Face(role);
				m_console.Print(std::format(
					"  {:<8} {:<46} scale {:.2f}", ui::FontRoleName(role),
					spec.path.empty() ? "(system fallback)" : spec.path, spec.scale));
			}
			const auto faces = InstalledFonts();
			m_console.Print(std::format("{} installed face(s):", faces.size()));
			for (size_t i = 0; i < faces.size(); ++i)
				m_console.Print(std::format("  [{}] {}", i, faces[i]));
			const auto live = m_fonts.LiveFonts();
			m_console.Print(std::format("{} live atlas(es):", live.size()));
			for (const auto& f : live)
				m_console.Print(std::format("  {:>4}px  {}", f.pixelHeight, f.face));
		});

	m_console.Register(
		"font",
		"audition a face live: font <role> <name|index|next|prev|off> | "
		"font scale <role> <n> | font save",
		[this](const std::vector<std::string>& args) { FontCommand(args); });

	// --- render debug ---
	m_console.Register("shadows", "toggle shadow rendering (on/off)",
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty()) m_world->SetShadowsEnabled(ArgOn(args[0]));
						   m_console.Print(m_world->ShadowsEnabled() ? "shadows on"
																	: "shadows off");
					   });
	m_console.Register("shadowrate",
				"fire shadow re-render rate: <hz> [per-frame budget]",
				[this](const std::vector<std::string>& args) {
					if (!args.empty()) {
						const float hz = std::strtof(args[0].c_str(), nullptr);
						const int budget =
							args.size() > 1 ? std::atoi(args[1].c_str()) : -1;
						m_world->SetShadowFlicker(hz, budget);
					}
					m_console.Print(std::format(
						"fire shadows re-render at {:.1f} Hz, at most {} cube(s)/frame",
						m_world->ShadowFlickerHz(), m_world->ShadowFlickerBudget()));
				});
	m_console.Register("dust", "volumetric dust: on/off, or a density (default 0.075)",
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty()) {
							   if (args[0] == "on" || args[0] == "off") {
								   m_world->SetDustEnabled(ArgOn(args[0]));
							   } else {
								   m_world->SetDustDensity(
									   static_cast<float>(std::atof(args[0].c_str())));
								   m_world->SetDustEnabled(true);
							   }
						   }
						   m_console.Print(m_world->DustEnabled()
											   ? std::format("dust on, density {:.3f}",
															 m_world->DustDensity())
											   : "dust off");
					   });
	m_console.Register("haze", "dust ambient pickup (mood tuning, default 0.9)",
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty())
							   m_world->SetHazeAmbient(
								   static_cast<float>(std::atof(args[0].c_str())));
						   m_console.Print(
							   std::format("haze ambient {:.2f}", m_world->HazeAmbient()));
					   });
	m_console.Register("ambient", "scale the ambient fill (mood tuning, default 1.0)",
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty())
							   m_world->SetAmbientScale(
								   static_cast<float>(std::atof(args[0].c_str())));
						   m_console.Print(
							   std::format("ambient x{:.2f}", m_world->AmbientScale()));
					   });
	m_console.Register("fov", "set camera field of view in degrees (default 70)",
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty())
							   m_world->SetFov(static_cast<float>(std::atof(args[0].c_str())));
						   m_console.Print(std::format("fov {:.0f}", m_world->Fov()));
					   });
}

// ============================================================================
// The typeface audition (docs/fonts.md Phase 4).
//
// Roles resolve through the library on EVERY frame (UIContext::UseFont, and the
// FontAt/FontFor lookups), so swapping a face here needs no invalidation: the
// next frame simply resolves somewhere else. That is what makes it possible to
// flip through candidates while looking at the real HUD at its real 17px, which
// is the only way this decision should be made.
// ============================================================================

namespace {
// fonts.cat stores `file` relative to assets/; FaceSpec holds the resolved
// absolute path. These two convert, so the console can talk in the short form.
std::string FontToRelative(const std::string& absolute) {
	const std::string prefix = paths::Asset("");
	std::string rel = absolute.starts_with(prefix) ? absolute.substr(prefix.size())
												   : absolute;
	std::ranges::replace(rel, '\\', '/');
	return rel;
}
} // namespace

void Game::FontCommand(const std::vector<std::string>& args) {
	const std::vector<std::string> faces = InstalledFonts();

	auto describe = [this](ui::FontRole role) {
		const ui::FaceSpec& s = m_fonts.Face(role);
		m_console.Print(std::format(
			"{} = {}  scale {:.2f}", ui::FontRoleName(role),
			s.path.empty() ? "(system fallback)" : FontToRelative(s.path), s.scale));
	};

	if (args.empty()) {
		m_console.Print("font <role> <name|index|next|prev|off>   swap a face live");
		m_console.Print("font scale <role> <n>                    optical size");
		m_console.Print("font save                                write fonts.cat");
		m_console.Print("`fonts` lists the roles, installed faces and live atlases.");
		return;
	}

	if (args[0] == "save") {
		m_console.Print(SaveFontCatalog() ? "fonts.cat written"
										  : "could not write fonts.cat (see the log)");
		return;
	}

	if (args[0] == "scale") {
		ui::FontRole role{};
		if (args.size() < 3 || !ui::FontRoleFromName(args[1], role)) {
			m_console.Print("usage: font scale <body|display|script|mono> <n>");
			return;
		}
		ui::FaceSpec spec = m_fonts.Face(role);
		spec.scale = static_cast<float>(std::atof(args[2].c_str()));
		m_fonts.SetFace(role, spec);
		describe(role);
		return;
	}

	ui::FontRole role{};
	if (!ui::FontRoleFromName(args[0], role)) {
		m_console.Print(std::format("unknown role '{}' (body|display|script|mono)",
									args[0]));
		return;
	}
	if (args.size() < 2) { // `font body` just reports
		describe(role);
		return;
	}

	ui::FaceSpec spec = m_fonts.Face(role);
	const std::string& what = args[1];

	if (what == "off") {
		spec.path.clear();
	} else if (what == "next" || what == "prev") {
		// Slot 0 is the system fallback, 1..N the installed faces, so a cycle
		// always passes back through "as it shipped" for comparison.
		const int slots = static_cast<int>(faces.size()) + 1;
		int slot = 0;
		for (size_t i = 0; i < faces.size(); ++i)
			if (spec.path == paths::Asset(faces[i])) {
				slot = static_cast<int>(i) + 1;
				break;
			}
		slot = (slot + (what == "next" ? 1 : slots - 1)) % slots;
		spec.path = slot == 0 ? std::string() : paths::Asset(faces[slot - 1]);
	} else if (std::isdigit(static_cast<unsigned char>(what[0]))) {
		const size_t index = static_cast<size_t>(std::atoi(what.c_str()));
		if (index >= faces.size()) {
			m_console.Print(std::format("no face [{}] — `fonts` lists them", index));
			return;
		}
		spec.path = paths::Asset(faces[index]);
	} else {
		// Substring match on the relative path, case-insensitive: `font body
		// alegreya` is enough. Ambiguity is reported rather than guessed at.
		std::string needle = what;
		std::ranges::transform(needle, needle.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		std::vector<std::string> hits;
		for (const std::string& f : faces) {
			std::string hay = f;
			std::ranges::transform(hay, hay.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			if (hay.find(needle) != std::string::npos) hits.push_back(f);
		}
		if (hits.empty()) {
			m_console.Print(std::format("no installed face matches '{}'", what));
			return;
		}
		if (hits.size() > 1) {
			m_console.Print(std::format("'{}' matches {} faces:", what, hits.size()));
			for (const std::string& h : hits) m_console.Print("  " + h);
			return;
		}
		spec.path = paths::Asset(hits.front());
	}

	m_fonts.SetFace(role, spec);
	describe(role);
}

bool Game::SaveFontCatalog() {
	// Round-trip the existing file so its header and per-role comments survive
	// (serialize::Block::lead); Catalog::Add replaces an entry IN PLACE, so the
	// role order is stable too.
	const std::string binPath = paths::Asset("fonts\\fonts.cat");
	Catalog cat;
	cat.Load(binPath);
	for (int i = 0; i < ui::kFontRoleCount; ++i) {
		const auto role = static_cast<ui::FontRole>(i);
		const ui::FaceSpec& spec = m_fonts.Face(role);
		CatalogEntry entry;
		if (const CatalogEntry* existing = cat.Find(ui::FontRoleName(role)))
			entry = *existing;
		else
			entry.id = ui::FontRoleName(role);
		entry.Set("file", spec.path.empty() ? std::string() : FontToRelative(spec.path));
		entry.Set("scale", std::format("{:.2f}", spec.scale));
		cat.Add(std::move(entry));
	}

	bool ok = cat.Save(binPath);
	if (!ok) log::Warn("font save: could not write {}", binPath);

	// fonts.cat lives in the shared POOL, which synctosource does not copy (it
	// carries the project). A dev build writes the source tree already (binPath
	// IS under it), so only a packaged build needs the second write to keep the
	// chosen faces.
	if (const std::string& repo = paths::RepoAssetsDir();
		!repo.empty() && paths::AssetsDir() != repo) {
		const std::string srcPath = repo + "\\fonts\\fonts.cat";
		if (cat.Save(srcPath))
			log::Info("font save: also wrote {}", srcPath);
		else {
			log::Warn("font save: could not write {}", srcPath);
			ok = false;
		}
	}
	return ok;
}

// ============================================================================
// Staged loading. Each task is one frame's worth of blocking work; a loading
// screen renders between tasks. The boot list is the bare minimum to reach
// the landing page fast; the heavy dungeon load runs later, behind its own
// progress screen, when the player first starts a game.
// ============================================================================


} // namespace dungeon::game
