// ============================================================================
// Game/Game_DevWorld.cpp — the world tier's dev-console commands.
//
// Split out of Game_DevCommands.cpp (past three thousand lines) by concern:
// travelling the overworld (world/worldmap/travel/quest/camp/encounters/
// enter/leave/worldpos/discover), the worlds beside this one and the map's
// pages, the project-wide file checks (catround/levels/levelcheck), and the
// world editor (worldedit/terrainbrush/paint/worldprops/worldloc/worldarea/
// worldsettings/newtype/typerefs/saveworld). The dungeon tier's commands are
// next door in Game_DevDungeons.cpp.
// ============================================================================
#include "Game/Game.h"

#include "Assets/File.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/Serialize.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <utility>

namespace dungeon::game {

void Game::RegisterWorldCommands() {
	// --- travelling the overworld ---------------------------------------
	m_console.Register("world",
					   "print the world map: terrain, areas and locations",
					   [this](const std::vector<std::string>&) {
						   for (const std::string& line : WorldReport())
							   m_console.Print(line);
					   });
	m_console.Register(
		"worldmap", "enter or leave the world map: worldmap [on|off]",
		[this](const std::vector<std::string>& args) {
			// Bare `worldmap` REPORTS rather than toggles — the same choice
			// `rest` made, and for the same reason: a state command whose
			// meaning depends on the state you cannot see is a coin flip.
			if (args.empty()) {
				m_console.Print(m_state == AppState::WorldMap
									? "on the world map"
									: "in a dungeon");
				return;
			}
			SetOnWorldMap(args[0] == "on" || args[0] == "1");
			m_console.Print(m_state == AppState::WorldMap
								? "on the world map"
								: "in a dungeon");
		});
	m_console.Register(
		"travel", "step on the world map: travel <n|s|e|w> [count]",
		[this](const std::vector<std::string>& args) {
			if (args.empty()) {
				m_console.Print("usage: travel <n|s|e|w> [count]");
				return;
			}
			int dx = 0, dz = 0;
			const char d = args[0].empty() ? ' ' : args[0][0];
			if (d == 'n') dz = -1;
			else if (d == 's') dz = 1;
			else if (d == 'w') dx = -1;
			else if (d == 'e') dx = 1;
			else {
				m_console.Print("direction must be n, s, e or w");
				return;
			}
			const int count = args.size() > 1 ? std::max(1, std::atoi(args[1].c_str())) : 1;
			int moved = 0;
			for (int i = 0; i < count && TravelStep(dx, dz); ++i) ++moved;
			// Reports what it DID, not what it was asked to do: a step into
			// water stops the run, and a count that silently came up short is
			// how a test measures the wrong journey.
			m_console.Print(std::format(
				"travelled {} of {} to {},{} - {:.2f}h elapsed{}", moved, count,
				m_worldState.x, m_worldState.z, m_worldState.time,
				moved < count ? " (blocked)" : ""));
		});
	m_console.Register(
		"quest", "quests: quest (list) | quest <id> <stage> (set) | quest flags",
		[this](const std::vector<std::string>& args) {
			if (!args.empty() && args[0] == "flags") {
				if (m_worldState.flags.empty()) m_console.Print("no flags set");
				for (const auto& [k, v] : m_worldState.flags)
					m_console.Print(std::format("  {} = {}", k, v));
				return;
			}
			if (args.size() >= 2) {
				m_console.Print(m_worldState.SetQuestStage(args[0], args[1])
									? std::format("{} -> {}", args[0], args[1])
									: std::format("{} was already at {}", args[0],
												  args[1]));
				return;
			}
			// The LIST shows every authored quest, not only the started ones:
			// "which quests exist and where am I in each" is the question, and
			// a list of only what you have touched cannot answer the first half.
			for (const CatalogEntry& e : m_project.quests.Entries()) {
				const std::string* at = m_worldState.QuestStage(e.id);
				m_console.Print(std::format(
					"  {:<16} {:<22} {}", e.id, e.Display(),
					at ? std::format("at '{}' of [{}]", *at, e.Get("stages", ""))
					   : std::format("not started [{}]", e.Get("stages", ""))));
			}
			if (m_project.quests.Empty()) m_console.Print("no quests authored");
		});
	m_console.Register(
		"camp", "camp on the world map until rest ends by itself",
		[this](const std::vector<std::string>&) {
			if (!m_worldState.onWorldMap) {
				m_console.Print("camping is a world-map action (you are in a level)");
				return;
			}
			const float hours = Camp();
			// Reports the REASON as well as the hours, because "camped 0.0h" on
			// its own reads as a bug and is usually a party too hungry to rest.
			m_console.Print(std::format("camped {:.2f}h — {}", hours,
										m_world.RestEndReason()[0]
											? m_world.RestEndReason()
											: "did not start"));
		});
	m_console.Register(
		"encounter", "force a random encounter here: encounter [difficulty]",
		[this](const std::vector<std::string>& args) {
			if (!m_worldMap) {
				m_console.Print("no world map loaded");
				return;
			}
			const WorldMap::Terrain& t =
				m_worldMap->TerrainAt(m_worldState.x, m_worldState.z);
			const float d = args.empty()
								? m_worldMap->Difficulty(m_worldState.x, m_worldState.z)
								: static_cast<float>(std::atof(args[0].c_str()));
			m_console.Print(StartEncounter(d, t.tags, m_world.Rng()())
								? std::format("encounter on {} at difficulty {:.2f}",
											  t.id, d)
								: "no encounter (see log)");
		});
	m_console.Register(
		"encounters", "encounter rolls: encounters [on|off|<rate>]",
		[this](const std::vector<std::string>& args) {
			if (!args.empty()) {
				if (args[0] == "off") m_encountersOff = true;
				else if (args[0] == "on") m_encountersOff = false;
				else {
					m_encounterRate = static_cast<float>(std::atof(args[0].c_str()));
					m_encountersOff = false;
				}
			}
			// Reports the RATE and the switch separately, because a rate of zero
			// and "switched off" are different states and a reader has to be
			// able to tell which one they are looking at.
			m_console.Print(std::format("encounters {} rate {:.3f} (chance = "
										"difficulty x hours x rate)",
										m_encountersOff ? "off" : "on",
										m_encounterRate));
		});
	m_console.Register(
		"enter", "enter a world location's dungeon: enter [id] (default: here)",
		[this](const std::vector<std::string>& args) {
			if (!m_worldMap) {
				m_console.Print("no world map loaded");
				return;
			}
			std::string id = args.empty() ? std::string() : args[0];
			if (id.empty()) {
				const WorldMap::Location* l =
					m_worldMap->LocationAt(m_worldState.x, m_worldState.z);
				if (!l) {
					m_console.Print(std::format("nothing at {},{}", m_worldState.x,
												m_worldState.z));
					return;
				}
				id = l->id;
			}
			m_console.Print(EnterLocation(id)
								? std::format("entering {}", id)
								: std::format("could not enter {}", id));
		});
	m_console.Register(
		"leave", "leave the dungeon: leave [location] (default: the way you came)",
		[this](const std::vector<std::string>& args) {
			// The optional argument is what an EXIT STAIR supplies — which door
			// this is. The console can reach it and a script cannot reach the
			// stair itself (a stair fires on a party STEP, and `tp` sets the
			// cell without stepping), so this is how the two-doors rule is
			// exercised unattended.
			const std::string via = args.empty() ? std::string() : args[0];
			m_console.Print(LeaveDungeon(via)
								? std::format("back on the world at {},{}",
											  m_worldState.x, m_worldState.z)
								: "no world map to leave to");
		});
	m_console.Register(
		"worldpos", "move the party's world cell: worldpos <x> <z>",
		[this](const std::vector<std::string>& args) {
			if (!m_worldMap) {
				m_console.Print("no world map loaded");
				return;
			}
			if (args.size() < 2) {
				m_console.Print("usage: worldpos <x> <z>");
				return;
			}
			const int x = std::atoi(args[0].c_str());
			const int z = std::atoi(args[1].c_str());
			// Refuse rather than clamp: a silently corrected coordinate makes a
			// test that asked for the wrong cell look like it passed.
			if (!m_worldMap->InBounds(x, z)) {
				m_console.Print(std::format("{},{} is off the world grid", x, z));
				return;
			}
			m_worldState.x = x;
			m_worldState.z = z;
			m_worldState.MarkSeen(x, z);
			m_console.Print(std::format("world position {},{} on {} ({})", x, z,
										m_worldMap->TerrainAt(x, z).id,
										m_worldMap->Passable(x, z) ? "passable"
																   : "impassable"));
		});
	m_console.Register(
		"discover", "mark a world location discovered: discover <id>",
		[this](const std::vector<std::string>& args) {
			if (!m_worldMap) {
				m_console.Print("no world map loaded");
				return;
			}
			if (args.empty()) {
				for (const WorldMap::Location& l : m_worldMap->Locations())
					m_console.Print(std::format(
						"{} {} at {},{}{}", l.kind, l.id, l.x, l.z,
						m_worldState.Discovered(l.id) ? "  (discovered)" : ""));
				return;
			}
			const WorldMap::Location* found = nullptr;
			for (const WorldMap::Location& l : m_worldMap->Locations())
				if (l.id == args[0]) found = &l;
			if (!found) {
				m_console.Print(std::format("no location '{}' on the world map",
											args[0]));
				return;
			}
			m_console.Print(m_worldState.Discover(args[0])
								? std::format("discovered {}", args[0])
								: std::format("{} was already discovered", args[0]));
		});

	// --- worlds, the map's pages, and the project's files -----------------
	m_console.Register(
		"worlds",
		"the worlds beside this one: worlds | new <name> | load <name> | "
		"delete <name> <name again> | dialog",
		[this](const std::vector<std::string>& a) {
			// A WORLD IS A PROJECT FOLDER (assets/projects/<name>): its own
			// overworld, dungeons, levels and content. Switching RELAUNCHES,
			// because the choice is read before any of that exists — so this
			// says so rather than appearing to hang.
			const std::string root = paths::Asset("projects");
			if (a.empty()) {
				for (const std::string& name : Project::List(root))
					m_console.Print(std::format(
						"  {}{}", name,
						name == m_project.FolderName() ? "  (open)" : ""));
				m_console.Print("switching relaunches the game");
				return;
			}
			if (a[0] == "new" && a.size() >= 2) {
				const std::string made = CreateWorld(a[1]);
				m_console.Print(made.empty()
									? "could not create (see the log)"
									: std::format("created world '{}' - "
												  "`worlds load {}` to open it",
												  made, made));
				return;
			}
			if (a[0] == "load" && a.size() >= 2) {
				if (a[1] == m_project.FolderName()) {
					m_console.Print("already in '" + a[1] + "'");
					return;
				}
				m_console.Print(SwitchWorld(a[1]) ? "relaunching into " + a[1]
												  : "no such world");
				return;
			}
			if (a[0] == "delete" && a.size() >= 2) {
				// The console's form of the typed confirmation: the name TWICE,
				// matched exactly. The rules are DeleteWorld's either way.
				if (a.size() < 3 || a[2] != a[1]) {
					m_console.Print("to delete, type the name twice: worlds delete " +
									a[1] + " " + a[1] + " (case-sensitive)");
					return;
				}
				if (const std::string why = WorldDeleteRefusal(a[1]); !why.empty()) {
					m_console.Print(why);
					return;
				}
				m_console.Print(DeleteWorld(a[1]) ? "deleted world '" + a[1] + "'"
												  : "could not delete (see the log)");
				return;
			}
			if (a[0] == "dialog") {
				// The toolbar's Worlds disc and its rows, for a harness. The
				// dialog's clicks are the SAME calls (ClickOpen / Create), so
				// what this reports is what a mouse would have got. ON THE
				// WORLD SCREEN ONLY, the `worldsettings` rule: that is the one
				// state whose Update routes input to it.
				if (a.size() >= 2 && a[1] == "off") {
					m_worldsDialog.Close();
				} else if (m_state != AppState::WorldMap) {
					m_console.Print("the worlds dialog needs the world map "
									"(try `worldmap on`)");
					return;
				} else if (!m_worldsDialog.IsOpen()) {
					m_worldsDialog.Open(m_project.FolderName());
				}
				if (m_worldsDialog.IsOpen() && a.size() >= 3) {
					if (a[1] == "open") m_worldsDialog.ClickOpen(a[2]);
					else if (a[1] == "create") m_worldsDialog.Create(a[2]);
					else if (a[1] == "delete") m_worldsDialog.ClickDelete(a[2]);
					else if (a[1] == "confirm") m_worldsDialog.ConfirmDelete(a[2]);
					m_worldsDialog.ApplyPending(); // not inside a tree walk here
				}
				std::string list;
				for (const std::string& w : m_worldsDialog.Worlds())
					list += (list.empty() ? "" : " ") + w;
				m_console.Print(std::format(
					"worlds dialog {}: [{}] armed '{}' deleting '{}' - {}",
					m_worldsDialog.IsOpen() ? "open" : "closed", list,
					m_worldsDialog.Armed(), m_worldsDialog.Deleting(),
					m_worldsDialog.Note()));
				return;
			}
			m_console.Print("usage: worlds [new|load] <name> | delete <name> <name> | "
							"dialog [open|create|delete|confirm <name>|off]");
		});
	m_console.Register(
		"mappage",
		"the player map, without a keyboard: mappage | open | close | dungeon | world",
		[this](const std::vector<std::string>& a) {
			// The M key and the toggle button, reachable by a harness. It
			// reports the page, whether the toggle is even OFFERED, and whether
			// the map is open — three different facts: a project with no
			// overworld has no second page and must not advertise one, and the
			// page is DERIVED from the overlay being up, so it cannot outlive
			// it.
			if (!a.empty()) {
				if (a[0] == "world" && !m_worldMap) {
					m_console.Print("this project has no world map");
					return;
				}
				// open/close are exactly what M does, page reset included.
				if (a[0] == "open" || a[0] == "close") {
					if (a[0] == "open") m_mapView.Open(MapView::Mode::Player);
					else m_mapView.Close();
					ShowMapPage(MapPage::Dungeon);
				} else {
					ShowMapPage(a[0] == "world" ? MapPage::World : MapPage::Dungeon);
				}
			}
			m_console.Print(std::format(
				"map page: {} (toggle {}, map {})",
				ShowingWorldPage() ? "world" : "dungeon",
				m_mapView.hasWorld ? "offered" : "hidden",
				m_mapView.IsOpen() ? "open" : "closed"));
		});
	m_console.Register(
		"catround",
		"check every project file survives being written back unchanged",
		[this](const std::vector<std::string>&) {
			// THE WRITERS' FIDELITY, CHECKED. Every editor action that touches a
			// type saves the WHOLE project, so a writer that quietly drops a
			// comment or a blank line rewrites files nobody edited — and it did:
			// project.ini lost every word of its documentation on each save, and
			// the balance sheet lost the blank lines between its groups. Neither
			// was noticed by eye, because the damage lands in the files the
			// change was not about.
			//
			// It asks the REAL WRITERS (Project::ManifestText,
			// Catalog::Serialize) for the text they would write and diffs it
			// against what is on disk. Checking the serialize:: primitive
			// instead would be checking the wrong thing — the header line each
			// writer prepends is part of the file and not part of the primitive,
			// and the first version of this reported an empty catalog as broken
			// for exactly that reason. Nothing is written: a check that repaired
			// what it measured could not fail twice.
			int checked = 0, bad = 0, missing = 0;
			const auto same = [&](const std::string& path, const std::string& text) {
				auto bytes = assets::ReadBinaryFile(path);
				if (!bytes) {
					// COUNTED AND NAMED, never silently skipped. An absent file
					// is legitimate (a project need not define every category)
					// — but so is a MISTYPED PATH, and the two are the same
					// event here. Passing over it quietly is how this check
					// reported 23 of 23 while never once looking at
					// project.ini: the path had lost a backslash, the read
					// failed, and the count said nothing.
					++missing;
					m_console.Print("  absent: " + path);
					return;
				}
				++checked;
				const std::string before(bytes->begin(), bytes->end());
				if (serialize::NormalizeEol(before) == serialize::NormalizeEol(text))
					return;
				++bad;
				m_console.Print("  differs: " + path);
			};
			same(m_project.folder + "\\project.ini", m_project.ManifestText());
			for (const auto& [file, cat, header] : m_project.CatalogFiles())
				same(m_project.CatalogPath(file), cat->Serialize(header));
			m_console.Print(std::format(
				"catround {} of {} file(s) round-trip, {} absent",
				checked - bad, checked, missing));
		});
	m_console.Register(
		"levels",
		"the project's levels, grouped by the dungeon that claims them: "
		"levels | new [dungeon]",
		[this](const std::vector<std::string>& a) {
			// THE PICKER'S LIST, WITHOUT A MOUSE. The toolbar dropdown is what
			// W5 actually built; this prints the same grouping (through the same
			// Project helpers) so a harness can see that a level is in the
			// dungeon it was made in, which no screenshot can assert.
			if (!a.empty() && a[0] == "new") {
				// The [+] button's path. With no argument it lands in the
				// dungeon of the level being VIEWED, exactly as the button does.
				const std::string dungeon =
					a.size() >= 2 ? a[1] : m_mapView.ViewedDungeon();
				const std::string stem = CreateNewLevel(dungeon);
				m_console.Print(stem.empty()
									? "could not create"
									: std::format("created {} in {}", stem,
												  dungeon.empty() ? "no dungeon"
																  : dungeon));
				return;
			}
			for (const CatalogEntry& d : m_project.dungeons.Entries()) {
				const std::vector<std::string> lv = m_project.DungeonLevels(d.id);
				std::string list;
				for (const std::string& s : lv) list += (list.empty() ? "" : " ") + s;
				m_console.Print(std::format("  {:<10} ({}) {}", d.id, lv.size(), list));
			}
			const std::vector<std::string> orphans = m_project.OrphanLevels();
			std::string list;
			for (const std::string& s : orphans) list += (list.empty() ? "" : " ") + s;
			// PRINTED EVEN WHEN EMPTY, because "no orphans" is the interesting
			// answer: it is what says the grouping accounts for every level the
			// manifest holds.
			m_console.Print(std::format("  {:<10} ({}) {}", "(no dungeon)",
										orphans.size(), list));
			m_console.Print(std::format("viewing {} in {}", m_mapView.ViewedLevel(),
										m_mapView.ViewedDungeon().empty()
											? "no dungeon"
											: m_mapView.ViewedDungeon()));
		});
	m_console.Register(
		"levelcheck",
		"verify every level file is present and every model a type names is installed",
		[this](const std::vector<std::string>&) {
			// WHAT THIS GUARDS, and why it is scoped this narrowly: the baked pool
			// (assets/models, assets/textures) is GITIGNORED, so a fresh clone — or
			// a new worktree provisioned from a stale file list — has catalog
			// entries whose assets are absent. A missing TEXTURE renders magenta
			// and is survivable; a missing MODEL is a LoadModelOrDie and takes the
			// process down at level load, possibly on a level nobody has visited
			// in weeks. That asymmetry is why only models are fatal here.
			//
			// It does NOT re-validate records against the map (bounds, walkability,
			// a button facing a wall) — the loader already does that, and a second
			// copy of those rules here would be the very drift this suite exists
			// to catch.
			const std::vector<std::string> installed = InstalledModels();
			const auto haveModel = [&installed](const std::string& m) {
				return std::ranges::find(installed, m) != installed.end();
			};

			int types = 0, missingModels = 0, missingFiles = 0;
			for (const Catalog* cat : m_project.AllCatalogs()) {
				for (const CatalogEntry& e : cat->Entries()) {
					++types;
					const std::string model = e.Get("model", "");
					if (model.empty() || haveModel(model)) continue;
					++missingModels;
					m_console.Print(std::format("  MISSING MODEL '{}' named by type '{}'",
												model, e.id));
					log::Warn("levelcheck: missing model '{}' named by type '{}'", model,
							  e.id);
				}
			}

			for (const std::string& stem : m_project.levels) {
				for (const std::string& path :
					 {m_project.LevelMapPath(stem), m_project.LevelEntPath(stem)}) {
					std::error_code ec;
					if (std::filesystem::exists(path, ec)) continue;
					++missingFiles;
					m_console.Print(std::format("  MISSING LEVEL FILE {}", path));
					log::Warn("levelcheck: missing level file {}", path);
				}
			}

			const bool ok = missingModels == 0 && missingFiles == 0;
			const std::string verdict = std::format(
				"levelcheck RESULT={} levels={} types={} missing_models={} "
				"missing_files={} installed_models={}",
				ok ? "PASS" : "FAIL", m_project.levels.size(), types, missingModels,
				missingFiles, installed.size());
			m_console.Print(verdict);
			log::Info("{}", verdict); // the harness reads this from dungeon.log
		});

	// --- the world editor -------------------------------------------------
	m_console.Register(
		"worldedit", "world map edit mode: worldedit [on|off]",
		[this](const std::vector<std::string>& args) {
			if (!m_worldMap) {
				m_console.Print("no world map loaded");
				return;
			}
			if (!args.empty())
				m_worldMapView.SetMode(args[0] == "off" ? WorldMapView::Mode::Play
														: WorldMapView::Mode::Editor);
			// REPORTS when bare, like `rest` and `encounters`: a mode command
			// whose meaning depends on a state you cannot see is a coin flip.
			m_console.Print(m_worldMapView.Editing() ? "world editing (fog off)"
													 : "world playing (fog on)");
		});
	m_console.Register(
		"terrainbrush", "arm the world terrain brush: terrainbrush [id|off]",
		[this](const std::vector<std::string>& args) {
			if (!m_worldMap) {
				m_console.Print("no world map loaded");
				return;
			}
			if (!args.empty()) {
				if (args[0] == "off") m_worldMapView.ArmTerrain({});
				else {
					// Refuse an unknown id rather than arming a brush that
					// would paint nothing: SetTerrainAt would decline every
					// cell and the click would look broken.
					bool known = false;
					for (const WorldMap::Terrain& t : m_worldMap->Terrains())
						if (t.id == args[0]) known = true;
					if (!known) {
						m_console.Print(std::format("no terrain '{}'", args[0]));
						return;
					}
					m_worldMapView.ArmTerrain(args[0]);
				}
			}
			m_console.Print(m_worldMapView.ArmedTerrain().empty()
								? "no terrain armed"
								: "armed: " + m_worldMapView.ArmedTerrain());
		});
	m_console.Register(
		"paint", "paint the armed terrain on a world cell: paint <x> <z>",
		[this](const std::vector<std::string>& args) {
			// The mouse path's rules, reachable without a mouse: same armed
			// brush, same undo bracketing, same refusal to repaint a cell that
			// is already that terrain.
			if (!m_worldMap || args.size() < 2) {
				m_console.Print("usage: paint <x> <z> (arm with terrainbrush)");
				return;
			}
			if (m_worldMapView.ArmedTerrain().empty()) {
				m_console.Print("no terrain armed");
				return;
			}
			const int x = std::atoi(args[0].c_str());
			const int z = std::atoi(args[1].c_str());
			if (!m_worldMap->InBounds(x, z)) {
				m_console.Print(std::format("{},{} is off the world grid", x, z));
				return;
			}
			const std::string was = m_worldMap->TerrainAt(x, z).id;
			// A CELL ALREADY THAT TERRAIN IS NOT AN EDIT, and must not push an
			// undo step. SetTerrainAt returns true for "I found the terrain and
			// set it", not "something changed" — so trusting it put a no-op
			// step on the stack, and the next Ctrl+Z spent itself taking back
			// nothing. The mouse path had this right; the console did not.
			if (was == m_worldMapView.ArmedTerrain()) {
				m_console.Print(std::format("{},{} is already {}", x, z, was));
				return;
			}
			m_world.BeginUndoStep();
			const bool changed =
				m_worldMap->SetTerrainAt(x, z, m_worldMapView.ArmedTerrain());
			m_world.CommitUndoStep(changed);
			m_console.Print(changed ? std::format("{},{} {} -> {}", x, z, was,
												  m_worldMapView.ArmedTerrain())
									: std::format("{},{} unchanged", x, z));
		});
	m_console.Register(
		"worldprops",
		"the world's own properties: worldprops | start <x> <z> | "
		"opening <dungeon> <level> <x> <z> | opening world | eval <level>",
		[this](const std::vector<std::string>& a) {
			if (!m_worldMap) {
				m_console.Print("no world map loaded");
				return;
			}
			if (a.empty()) {
				m_console.Print(std::format("world start {},{}",
											m_worldMap->StartX(), m_worldMap->StartZ()));
				// WHERE THE GAME BEGINS is the manifest's, not the world map's
				// — the world's start is where a party STANDS when it begins on
				// the map, and the opening may be a dungeon instead. They are
				// printed together because that distinction is invisible from
				// either file alone.
				m_console.Print(m_project.startDungeon.empty()
									? "opening: the world map"
									: std::format("opening: {} / {} at {},{}",
												  m_project.startDungeon,
												  m_project.startLevel,
												  m_project.startX, m_project.startZ));
				m_console.Print("harness level: " +
								(m_project.evalLevel.empty() ? std::string("(unset)")
															 : m_project.evalLevel));
				return;
			}
			if (a[0] == "start" && a.size() >= 3) {
				const int x = std::atoi(a[1].c_str()), z = std::atoi(a[2].c_str());
				// THE RULE IS THE MAP'S (WorldMap::SetStart) and what is left
				// here is the reporting — the settings dialog is a second way
				// in, and a refusal that lived in one of them would not be the
				// same editor from the other.
				m_world.BeginUndoStep();
				const bool ok = m_worldMap->SetStart(x, z);
				m_world.CommitUndoStep(ok);
				if (ok) m_console.Print(std::format("world start {},{}", x, z));
				else if (!m_worldMap->InBounds(x, z))
					m_console.Print("off the world grid");
				else
					m_console.Print(std::format("{},{} is impassable ({})", x, z,
												m_worldMap->TerrainAt(x, z).id));
			} else if (a[0] == "opening" && a.size() >= 2 && a[1] == "world") {
				m_project.startDungeon.clear();
				m_project.startLevel.clear();
				m_project.startX = m_project.startZ = -1;
				m_console.Print("opening: the world map");
			} else if (a[0] == "opening" && a.size() >= 5) {
				m_project.startDungeon = a[1];
				m_project.startLevel = a[2];
				m_project.startX = std::atoi(a[3].c_str());
				m_project.startZ = std::atoi(a[4].c_str());
				m_console.Print(std::format("opening: {} / {} at {},{}", a[1], a[2],
											a[3], a[4]));
			} else if (a[0] == "eval" && a.size() >= 2) {
				m_project.evalLevel = a[1];
				m_console.Print("harness level: " + a[1]);
			} else {
				m_console.Print("usage: worldprops [start|opening|eval] ...");
			}
			// THE MANIFEST IS NOT UNDOABLE and says so rather than pretending:
			// the editor's history snapshots the world and the levels, not
			// project.ini, and a half-undoable dialog would be worse than an
			// honest one. It is written by the project save, not by savemap.
			if (!a.empty() && (a[0] == "opening" || a[0] == "eval"))
				m_console.Print("(manifest change - not undoable; project.ini is "
								"written by a project save)");
		});
	m_console.Register(
		"worldloc",
		"world locations: worldloc | add <kind> <id> <x> <z> | del <id> | "
		"move <id> <x> <z> | set <id> <field> <value>",
		[this](const std::vector<std::string>& a) {
			if (!m_worldMap) {
				m_console.Print("no world map loaded");
				return;
			}
			WorldMap& w = *m_worldMap;
			if (a.empty()) {
				for (const WorldMap::Location& l : w.Locations())
					m_console.Print(std::format(
						"  {:<8} {:<16} {},{}  -> {} {} {},{}", l.kind, l.id, l.x,
						l.z, l.Dungeon(),
						l.level.empty() ? std::string("(unset)") : l.level,
						l.entryX, l.entryZ));
				if (w.Locations().empty()) m_console.Print("no locations");
				return;
			}
			// EVERY BRANCH BRACKETS ITS OWN UNDO STEP and reports what it did.
			// A world edit that silently did nothing is the failure mode here:
			// the refusals (duplicate id, occupied cell, off the grid) are the
			// rules Load asserts on, enforced early so the editor cannot author
			// a world its own loader rejects.
			const std::string& verb = a[0];
			if (verb == "add" && a.size() >= 5) {
				WorldMap::Location l;
				l.kind = a[1];
				l.id = a[2];
				l.x = std::atoi(a[3].c_str());
				l.z = std::atoi(a[4].c_str());
				m_world.BeginUndoStep();
				const bool ok = w.AddLocation(std::move(l));
				m_world.CommitUndoStep(ok);
				m_console.Print(ok ? std::format("added {} at {},{}", a[2], a[3], a[4])
								   : "refused: duplicate id, occupied cell, or off "
									 "the grid");
			} else if (verb == "del" && a.size() >= 2) {
				m_world.BeginUndoStep();
				const bool ok = w.RemoveLocation(a[1]);
				m_world.CommitUndoStep(ok);
				m_console.Print(ok ? "removed " + a[1] : "no such location");
			} else if (verb == "move" && a.size() >= 4) {
				m_world.BeginUndoStep();
				const bool ok = w.MoveLocation(a[1], std::atoi(a[2].c_str()),
											   std::atoi(a[3].c_str()));
				m_world.CommitUndoStep(ok);
				m_console.Print(ok ? std::format("{} -> {},{}", a[1], a[2], a[3])
								   : "refused: unknown id, occupied cell, or off "
									 "the grid");
			} else if (verb == "set" && a.size() >= 4) {
				WorldMap::Location* l = w.MutableLocation(a[1]);
				if (!l) {
					m_console.Print("no such location");
					return;
				}
				m_world.BeginUndoStep();
				bool ok = true;
				if (a[2] == "dungeon") l->dungeon = a[3];
				else if (a[2] == "level") l->level = a[3];
				else if (a[2] == "entryx") l->entryX = std::atoi(a[3].c_str());
				else if (a[2] == "entryz") l->entryZ = std::atoi(a[3].c_str());
				else if (a[2] == "kind") l->kind = a[3];
				else ok = false;
				m_world.CommitUndoStep(ok);
				m_console.Print(ok ? std::format("{}.{} = {}", a[1], a[2], a[3])
								   : "field must be kind/dungeon/level/entryx/entryz");
			} else {
				m_console.Print("usage: worldloc [add|del|move|set] ...");
			}
		});
	m_console.Register(
		"worldarea",
		"world areas: worldarea | add <id> <x> <z> <w> <h> [difficulty] | "
		"del <id> | order <id> <index>",
		[this](const std::vector<std::string>& a) {
			if (!m_worldMap) {
				m_console.Print("no world map loaded");
				return;
			}
			std::vector<WorldMap::Area>& areas = m_worldMap->MutableAreas();
			if (a.empty()) {
				// IN FILE ORDER, numbered, because the order IS the rule: areas
				// may overlap and the LAST match wins. A list that sorted them
				// would hide the only thing that decides which one owns a cell.
				for (size_t i = 0; i < areas.size(); ++i)
					m_console.Print(std::format(
						"  [{}] {:<12} {},{} {}x{}  difficulty {}", i, areas[i].id,
						areas[i].x, areas[i].z, areas[i].w, areas[i].h,
						areas[i].difficulty >= 0.0f
							? std::format("{:.2f}", areas[i].difficulty)
							: std::string("(terrain's own)")));
				if (areas.empty()) m_console.Print("no areas");
				m_console.Print("later rows win where they overlap");
				return;
			}
			// Every verb below goes through the MAP's rules (WorldMap::AddArea
			// and friends), not through the vector: the settings dialog is a
			// second way in, and a refusal only one of them made would be two
			// editors wearing one name.
			const std::string& verb = a[0];
			if (verb == "add" && a.size() >= 6) {
				WorldMap::Area ar;
				ar.id = a[1];
				ar.x = std::atoi(a[2].c_str());
				ar.z = std::atoi(a[3].c_str());
				ar.w = std::atoi(a[4].c_str());
				ar.h = std::atoi(a[5].c_str());
				if (a.size() >= 7) ar.difficulty = static_cast<float>(std::atof(a[6].c_str()));
				const int w = ar.w, h = ar.h;
				m_world.BeginUndoStep();
				const bool ok = m_worldMap->AddArea(std::move(ar));
				m_world.CommitUndoStep(ok);
				// APPENDED, and that is not arbitrary: the newest area wins
				// where it overlaps, which is what someone carving an exception
				// out of a broad region means.
				//
				// EACH REFUSAL SAYS WHICH RULE REFUSED. One sentence for both read
				// the same whichever fired, which is not only worse to read: the
				// harness check for the duplicate rule PASSED with that rule
				// deleted, because the extent case beside it printed the very same
				// words. The reporting is the decision's alibi, so it has to be as
				// specific as the decision.
				if (ok)
					m_console.Print(std::format("added {} (row {}, wins over earlier)",
												a[1], areas.size() - 1));
				else if (w <= 0 || h <= 0)
					m_console.Print("refused: an area needs a positive extent");
				else
					m_console.Print(std::format(
						"refused: an area named '{}' already exists", a[1]));
			} else if (verb == "del" && a.size() >= 2) {
				m_world.BeginUndoStep();
				const bool ok = m_worldMap->RemoveArea(a[1]);
				m_world.CommitUndoStep(ok);
				m_console.Print(ok ? "removed " + a[1] : "no such area");
			} else if (verb == "order" && a.size() >= 3) {
				const int to = std::atoi(a[2].c_str());
				m_world.BeginUndoStep();
				const bool ok = m_worldMap->MoveArea(a[1], to);
				m_world.CommitUndoStep(ok);
				m_console.Print(ok ? std::format("{} is now row {}", a[1], to)
								   : "no such area, index out of range, or already "
									 "there");
			} else if (verb == "at" && a.size() >= 3) {
				// WHICH AREA OWNS THIS CELL, and what that makes it. The
				// ordering rule is otherwise invisible: a list can show the
				// order, but only this can show that the order DID something.
				const int x = std::atoi(a[1].c_str()), z = std::atoi(a[2].c_str());
				const WorldMap::Area* owner = m_worldMap->AreaAt(x, z);
				m_console.Print(std::format(
					"{},{} difficulty {:.2f} from {}", x, z,
					m_worldMap->Difficulty(x, z),
					owner ? owner->id : m_worldMap->TerrainAt(x, z).id +
											 std::string(" (no area)")));
			} else {
				m_console.Print("usage: worldarea [add|del|order|at] ...");
			}
		});
	m_console.Register(
		"worldsettings",
		"open the world settings dialog: worldsettings [location] | off",
		[this](const std::vector<std::string>& a) {
			// The toolbar's Settings disc, reachable without a mouse. It does
			// NOT duplicate the rules — the dialog's callbacks are the same
			// WorldMap calls `worldprops`/`worldloc`/`worldarea` make — so this
			// exists to open and close the thing, which is all a harness can
			// check about a dialog anyway.
			if (!a.empty() && a[0] == "off") {
				m_worldSettingsDialog.Close();
				m_console.Print("world settings closed");
				return;
			}
			if (!m_worldMap) {
				m_console.Print("no world map loaded");
				return;
			}
			// ON THE WORLD SCREEN ONLY, because that is the one state whose
			// Update routes input to it. Opened over a dungeon it would draw a
			// modal nothing could type into or close - a console command
			// reaching further than the button it stands for.
			if (m_state != AppState::WorldMap) {
				m_console.Print("world settings need the world map "
								"(try `worldmap on`)");
				return;
			}
			OpenWorldSettings(a.empty() ? std::string() : a[0]);
			m_console.Print(m_worldSettingsDialog.IsOpen() ? "world settings open"
														   : "could not open");
		});
	m_console.Register(
		"newtype", "create a pure-data type: newtype <dungeons|terrain|quests>",
		[this](const std::vector<std::string>& args) {
			// The palette's "+ New..." for these categories, reachable without a
			// mouse — the harness cannot click, and this is the path W2 adds.
			if (args.empty()) {
				m_console.Print("usage: newtype <dungeons|terrain|quests>");
				return;
			}
			const MapEditor::PaletteCat cat = MapEditor::CatForCatalogKey(args[0]);
			if (cat == MapEditor::PaletteCat::Count ||
				!MapEditor::CategoryAuthorable(cat)) {
				m_console.Print(std::format(
					"'{}' is not a pure-data category (dungeons/terrain/quests)",
					args[0]));
				return;
			}
			const std::string id = CreateAuthoredType(cat);
			m_console.Print(id.empty() ? "could not create"
									   : std::format("created {} '{}'", args[0], id));
		});
	m_console.Register(
		"typerefs", "count what references a type: typerefs <category> <id>",
		[this](const std::vector<std::string>& args) {
			if (args.size() < 2) {
				m_console.Print("usage: typerefs <category> <id>");
				return;
			}
			// BOTH HALVES, reported separately, because they answer different
			// questions: levels are where a placement lives, and the catalog +
			// WORLD half is where a doorway or a hook does.
			const DungeonWorld::TypeUsage lv = m_world.SweepTypeRefs(args[0], args[1]);
			const int other = SweepCatalogRefs(args[0], args[1], nullptr);
			m_console.Print(std::format("{} '{}': {} level record(s), {} other "
										"reference(s)",
										args[0], args[1], lv.count, other));
		});
	m_console.Register(
		"saveworld", "write world/world.map alone (savemap writes the levels too)",
		[this](const std::vector<std::string>&) {
			// SEPARATE FROM `savemap` because savemap rewrites every level file
			// as well, and a level writer regenerates headers — so using it to
			// test the WORLD writer quietly stripped the authoring notes off
			// eval_arena. A command that does one thing can be used to check
			// that one thing.
			if (!m_worldMap) {
				m_console.Print("no world map loaded");
				return;
			}
			m_console.Print(SaveWorld() ? "saved world" : "world save failed");
		});
}

} // namespace dungeon::game
