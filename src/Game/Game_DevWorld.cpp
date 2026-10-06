// ============================================================================
// Game/Game_DevWorld.cpp — the world tier's dev-console commands.
//
// Split out of Game_DevCommands.cpp (past three thousand lines) by concern:
// travelling the overworld (world/worldmap/travel/quest/camp/encounters/
// enter/leave/confirm/worldpos/discover), the worlds beside this one and the
// map's pages, the project-wide file checks (catround/levels; levelcheck has
// its own file, Game_LevelCheck.cpp), and the world editor (worldedit/
// worldview/terrainbrush/paint/worldprops/worldloc/worldarea/worldsettings/
// newtype/newasset/bake/typeset/typerefs/saveworld; `typeset dialog` drives the
// type editor itself, step by step). The dungeon tier's commands are next door
// in Game_DevDungeons.cpp.
// ============================================================================
#include "Game/Game.h"

#include "Assets/File.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/DevCommandArgs.h" // Need
#include "Game/Serialize.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iterator>
#include <string>
#include <thread>
#include <utility>

namespace dungeon::game {

using devargs::Need;

namespace {

// Where a writer's text parts from the text it should have given - the first
// line that differs, for a `catround` case's report. Empty when they agree.
std::string FirstDifference(const std::string& got, const std::string& want) {
	if (got == want) return {};
	const auto lines = [](const std::string& s) {
		std::vector<std::string> out;
		for (size_t pos = 0; pos < s.size();) {
			size_t end = s.find('\n', pos);
			if (end == std::string::npos) end = s.size();
			std::string line = s.substr(pos, end - pos);
			if (!line.empty() && line.back() == '\r') line.pop_back();
			out.push_back(std::move(line));
			pos = end + 1;
		}
		return out;
	};
	const std::vector<std::string> a = lines(got), b = lines(want);
	for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
		const std::string x = i < a.size() ? "'" + a[i] + "'" : "(the end)";
		const std::string y = i < b.size() ? "'" + b[i] + "'" : "(the end)";
		if (x != y) return std::format("line {} is {}, want {}", i + 1, x, y);
	}
	return "the line endings differ";
}

} // namespace

void Game::RegisterWorldCommands() {
	// --- travelling the overworld ---------------------------------------
	m_console.Register(
		{.name = "world",
		 .group = CmdGroup::World,
		 .summary = "print the world map: terrain, areas and locations"},
		[this](const std::vector<std::string>&) {
			for (const std::string& line : WorldReport())
				m_console.Print(line);
		});
	m_console.Register(
		{.name = "worldmap",
		 .group = CmdGroup::World,
		 .params = "[on|off]",
		 .summary = "enter or leave the world map; bare reports where the party is"},
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
			const bool on = args[0] == "on" || args[0] == "1";
			if (!on && args[0] != "off" && args[0] != "0") {
				m_console.RefuseUsage(); // a typo'd word used to mean "off"
				return;
			}
			SetOnWorldMap(on);
			// An `on` that did not get there REFUSES (C442): SetOnWorldMap only
			// warns into the log, and a script that asked for the world map
			// would go on to measure a dungeon.
			if (on && m_state != AppState::WorldMap) {
				m_console.Refuse(!m_worldMap ? "worldmap: the project has no world map"
											 : "worldmap: no game in progress to travel in");
				return;
			}
			m_console.Print(m_state == AppState::WorldMap
								? "on the world map"
								: "in a dungeon");
		});
	m_console.Register(
		{.name = "travel",
		 .group = CmdGroup::World,
		 .params = "<n|s|e|w> [count]",
		 .summary = "step across the world map, stopping where blocked"},
		[this](const std::vector<std::string>& args) {
			if (!m_worldMap) {
				m_console.Refuse("no world map loaded");
				return;
			}
			if (!Need(m_console, args, 1)) return;
			int dx = 0, dz = 0;
			const char d = args[0].empty() ? ' ' : args[0][0];
			if (d == 'n') dz = -1;
			else if (d == 's') dz = 1;
			else if (d == 'w') dx = -1;
			else if (d == 'e') dx = 1;
			else {
				m_console.Refuse("direction must be n, s, e or w");
				return;
			}
			const int count = args.size() > 1 ? std::max(1, std::atoi(args[1].c_str())) : 1;
			int moved = 0;
			for (int i = 0; i < count && TravelStep(dx, dz); ++i) ++moved;
			// Reports what it DID, not what it was asked to do: a step into
			// water stops the run, and a count that silently came up short is
			// how a test measures the wrong journey. A BLOCKED step is the
			// world's answer, not a decline, so it stays a Print (C442 - the
			// locked door's rule); worldtravel.eval walks into water on purpose.
			m_console.Print(std::format(
				"travelled {} of {} to {},{} - {:.2f}h elapsed{}", moved, count,
				m_worldState.x, m_worldState.z, m_worldState.time,
				moved < count ? " (blocked)" : ""));
		});
	m_console.Register(
		{.name = "quest",
		 .group = CmdGroup::World,
		 .params = "[list]\n"
				   "<id> <stage>\n"
				   "flags",
		 .summary = "list every authored quest, set a quest's stage, or show flags"},
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
	// FLAGS (flags.cat): read or set one, list them by scope. Lines are
	// `flag <id> <on|off> <scope>` so a harness can read them back.
	m_console.Register(
		{.name = "flag",
		 .group = CmdGroup::World,
		 .params = "<id> [on|off]",
		 .summary = "read a flag, or set it on or off"},
		[this](const std::vector<std::string>& args) {
			if (!Need(m_console, args, 1)) return;
			const CatalogEntry* e = m_project.flags.Find(args[0]);
			if (args.size() > 1) {
				if (args[1] != "on" && args[1] != "off") {
					m_console.Refuse("flag: on or off");
					return;
				}
				m_worldState.SetFlagOn(args[0], args[1] == "on");
			}
			const std::string scope = e ? e->Get("dungeon", "") : std::string();
			m_console.Print(std::format("flag {} {} {}{}", args[0],
										m_worldState.FlagOn(args[0]) ? "on" : "off",
										scope.empty() ? "world" : "dungeon:" + scope,
										e ? "" : " (not in flags.cat)"));
		});
	// `dungeon` alone = the party's dungeon.
	m_console.Register(
		{.name = "flags",
		 .group = CmdGroup::World,
		 .params = "[world]\ndungeon [id]",
		 .summary = "list the flags and whether each is on, by scope"},
		[this](const std::vector<std::string>& args) {
			// Which scope to show: all, the world's, or one dungeon's (the
			// party's own when none is named).
			std::string want = "*";
			if (!args.empty() && args[0] == "world") want.clear();
			else if (!args.empty() && args[0] == "dungeon") {
				if (args.size() > 1) want = args[1];
				else if (const CatalogEntry* d = m_project.DungeonOfLevel(m_world->CurrentLevel()))
					want = d->id;
				else {
					m_console.Print("flags: the party's level belongs to no dungeon");
					return;
				}
			}
			int shown = 0;
			for (const CatalogEntry& e : m_project.flags.Entries()) {
				const std::string scope = e.Get("dungeon", "");
				if (want != "*" && scope != want) continue;
				m_console.Print(std::format("flag {} {} {}", e.id,
											m_worldState.FlagOn(e.id) ? "on" : "off",
											scope.empty() ? "world" : "dungeon:" + scope));
				++shown;
			}
			if (shown == 0) m_console.Print("flags: none authored in that scope");
		});
	m_console.Register(
		{.name = "camp",
		 .group = CmdGroup::World,
		 .summary = "camp on the world map until rest ends by itself"},
		[this](const std::vector<std::string>&) {
			if (!m_worldState.onWorldMap) {
				m_console.Refuse("camping is a world-map action (you are in a level)");
				return;
			}
			const float hours = Camp();
			// Reports the REASON as well as the hours, because "camped 0.0h" on
			// its own reads as a bug and is usually a party too hungry to rest.
			m_console.Print(std::format("camped {:.2f}h — {}", hours,
										m_world->RestEndReason()[0]
											? m_world->RestEndReason()
											: "did not start"));
		});
	m_console.Register(
		{.name = "encounter",
		 .group = CmdGroup::World,
		 .params = "[difficulty]",
		 .summary = "force a random encounter on the party's world cell"},
		[this](const std::vector<std::string>& args) {
			if (!m_worldMap) {
				m_console.Refuse("no world map loaded");
				return;
			}
			const WorldMap::Terrain& t =
				m_worldMap->TerrainAt(m_worldState.x, m_worldState.z);
			const float d = args.empty()
								? m_worldMap->Difficulty(m_worldState.x, m_worldState.z)
								: static_cast<float>(std::atof(args[0].c_str()));
			// A FORCED encounter that did not start is a setup line declined
			// (C442): the fight a script goes on to measure is not there.
			if (StartEncounter(d, t.tags, m_world->Rng()()))
				m_console.Print(std::format("encounter on {} at difficulty {:.2f}", t.id, d));
			else
				m_console.Refuse("no encounter (see log)");
		});
	m_console.Register(
		{.name = "encounters",
		 .group = CmdGroup::World,
		 .params = "[on|off|<rate>]",
		 .summary = "switch random encounter rolls or set their rate"},
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
		{.name = "enter",
		 .group = CmdGroup::World,
		 .params = "[location]",
		 .summary = "enter a world location's dungeon (default: the one here)"},
		[this](const std::vector<std::string>& args) {
			if (!m_worldMap) {
				m_console.Refuse("no world map loaded");
				return;
			}
			std::string id = args.empty() ? std::string() : args[0];
			if (id.empty()) {
				const WorldMap::Location* l =
					m_worldMap->LocationAt(m_worldState.x, m_worldState.z);
				if (!l) {
					m_console.Refuse(std::format("nothing at {},{}", m_worldState.x,
												 m_worldState.z));
					return;
				}
				id = l->id;
			}
			if (EnterLocation(id)) m_console.Print(std::format("entering {}", id));
			else m_console.Refuse(std::format("could not enter {}", id));
		});
	m_console.Register(
		{.name = "leave",
		 .group = CmdGroup::World,
		 .params = "[location]",
		 .summary = "leave the dungeon for the world (default: the way you came)"},
		[this](const std::vector<std::string>& args) {
			// The optional argument is what an EXIT STAIR supplies — which door
			// this is. The console can reach it and a script cannot reach the
			// stair itself (a stair fires on a party STEP, and `tp` sets the
			// cell without stepping), so this is how the two-doors rule is
			// exercised unattended.
			const std::string via = args.empty() ? std::string() : args[0];
			if (LeaveDungeon(via))
				m_console.Print(std::format("back on the world at {},{}", m_worldState.x,
											m_worldState.z));
			else
				m_console.Refuse("no world map to leave to");
		});
	// THE YES/NO QUESTION (GameUI::AskYesNo): what it asks; a doorway's or an
	// exit's raised as walking onto one does (OfferEntrance / OfferExit - `enter`
	// and `leave` above go IN or OUT without asking); and an answer as Enter / Esc
	// gives one. A question names the game it was asked in, so a new game, a load,
	// a reset, the title and a world switch take it down unanswered (code-review
	// C115; resettest.eval stages one before each, EditorTest phase 30 a switch).
	m_console.Register(
		{.name = "confirm",
		 .group = CmdGroup::World,
		 .params = "[status]\n"
				   "enter\n"
				   "leave [location]\n"
				   "yes | no",
		 .summary = "the Yes/No question: what it asks; raise a doorway's or an exit's; answer it"},
		[this](const std::vector<std::string>& args) {
			const std::string what = args.empty() ? "status" : args[0];
			if (what == "yes" || what == "no") {
				if (!m_ui.PromptActive()) {
					m_console.Refuse("confirm: no question is up to answer");
					return;
				}
				const std::string asked = m_ui.ConfirmTitle();
				m_ui.AnswerConfirm(what == "yes");
				m_console.Print(std::format("confirm: answered {} to '{}'", what, asked));
				return;
			}
			if (what == "enter" || what == "leave") {
				if (m_ui.PromptActive()) {
					m_console.Refuse("confirm: a question is up already - '" +
									 m_ui.ConfirmTitle() + "'");
					return;
				}
				if (what == "enter") {
					// The doorway the party stands on, as the step onto it asks.
					if (m_state != AppState::WorldMap) {
						m_console.Refuse("confirm enter: the party is not on the world map");
						return;
					}
					OfferEntrance();
					if (!m_ui.PromptActive()) {
						m_console.Refuse(std::format("confirm enter: no doorway the party "
													 "knows of at {},{}",
													 m_worldState.x, m_worldState.z));
						return;
					}
				} else {
					// An exit stair's question, asked wherever the party stands.
					if (m_state != AppState::Playing) {
						m_console.Refuse("confirm leave: the party is not in a level");
						return;
					}
					OfferExit(args.size() > 1 ? args[1] : std::string());
				}
			} else if (what != "status") {
				m_console.RefuseUsage();
				return;
			}
			m_console.Print(m_ui.PromptActive()
								? std::format("confirm: open - '{}'", m_ui.ConfirmTitle())
								: std::string("confirm: none"));
		});
	m_console.Register(
		{.name = "worldpos",
		 .group = CmdGroup::World,
		 .params = "<x> <z>",
		 .summary = "move the party to a world cell"},
		[this](const std::vector<std::string>& args) {
			if (!m_worldMap) {
				m_console.Refuse("no world map loaded");
				return;
			}
			if (!Need(m_console, args, 2)) return;
			const int x = std::atoi(args[0].c_str());
			const int z = std::atoi(args[1].c_str());
			// Refuse rather than clamp: a silently corrected coordinate makes a
			// test that asked for the wrong cell look like it passed. (It said
			// "refuse" here for months while it printed - C442.)
			if (!m_worldMap->InBounds(x, z)) {
				m_console.Refuse(std::format("{},{} is off the world grid", x, z));
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
		{.name = "discover",
		 .group = CmdGroup::World,
		 .params = "\n"
				   "<location>",
		 .summary = "mark a world location discovered; bare lists every location"},
		[this](const std::vector<std::string>& args) {
			if (!m_worldMap) {
				m_console.Refuse("no world map loaded");
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
				m_console.Refuse(std::format("no location '{}' on the world map",
											 args[0]));
				return;
			}
			m_console.Print(m_worldState.Discover(args[0])
								? std::format("discovered {}", args[0])
								: std::format("{} was already discovered", args[0]));
		});

	// --- worlds, the map's pages, and the project's files -----------------
	m_console.Register(
		{.name = "worlds",
		 .group = CmdGroup::World,
		 .params = "\n"
				   "status\n"
				   "new <name> [blank [style=<id>]|copy|level <stem>]\n"
				   "new <name> wizard [style=<id>] [tag=<tag>] [size=<n>] [difficulty=<0..1>] [seed=<n>]\n"
				   "load <name>\n"
				   "delete <name> <name again>\n"
				   "dialog [off|<open|create|delete|confirm> <name>]\n"
				   "tags\n"
				   "newdialog [off|switch|create <name>]\n"
				   "newdialog source <blank|copy|wizard|level [stem]>\n"
				   "newdialog style <id|->\n"
				   "newdialog wizard <tag|-> <size> <difficulty> <seed>\n"
				   "newdialog popup [<n>]",
		 .summary = "list worlds on disk; create, load, delete; drive the world dialogs"},
		[this](const std::vector<std::string>& a) {
			// A WORLD IS A PROJECT FOLDER (assets/projects/<name>): its own
			// overworld, dungeons, levels and content. Loaded when a game starts
			// and switched in the process (docs/world-on-demand.md) — it used to
			// relaunch.
			const std::string root = paths::Asset("projects");
			if (a.empty()) {
				for (const std::string& name : Project::List(root))
					m_console.Print(std::format(
						"  {}{}", name,
						m_world && name == m_project.FolderName() ? "  (open)" : ""));
				return;
			}
			// WHAT IS RESIDENT, and what it holds on the GPU: a switch that
			// leaks is the failure this design invites, and the SRV count is the
			// gauge that shows it (a world's textures are most of it).
			if (a[0] == "status") {
				m_console.Print(std::format(
					"world {}  game {}  srv {} / {} (peak {})",
					m_world ? m_project.FolderName() : std::string("none"),
					m_gameLoaded ? "loaded" : "not loaded", m_device.SrvLive(),
					gfx::GraphicsDevice::SrvCapacity(), m_device.SrvHighWater()));
				return;
			}
			if (a[0] == "new" && a.size() >= 2) {
				// How it starts (Game/NewWorld.h): blank from the template (the
				// default), this world whole, or one of its levels.
				NewWorldSpec spec;
				if (a.size() >= 3 && a[2] == "copy") spec.source = NewWorldSpec::Source::CopyWorld;
				else if (a.size() >= 4 && a[2] == "level") {
					spec.source = NewWorldSpec::Source::CopyLevel;
					spec.level = a[3];
				} else if (a.size() >= 3 && a[2] == "wizard") {
					// The wizard's knobs as key=value, in any order; absent =
					// the dialog's defaults (NewWorld.h).
					spec.source = NewWorldSpec::Source::Wizard;
					for (size_t i = 3; i < a.size(); ++i) {
						const size_t eq = a[i].find('=');
						if (eq == std::string::npos) continue;
						const std::string k = a[i].substr(0, eq), v = a[i].substr(eq + 1);
						if (k == "tag") spec.tag = v;
						else if (k == "size") spec.size = std::atoi(v.c_str());
						else if (k == "difficulty") spec.difficulty = std::strtof(v.c_str(), nullptr);
						else if (k == "seed") spec.seed = static_cast<u32>(std::strtoul(v.c_str(), nullptr, 10));
					}
				} else if (a.size() >= 3 && a[2] != "blank") {
					m_console.RefuseUsage(); // the registered forms, not a hand copy
					return;
				}
				// Blank and wizard start in a LIBRARY style (Phase 7) when asked.
				if (spec.source == NewWorldSpec::Source::Blank ||
					spec.source == NewWorldSpec::Source::Wizard)
					for (size_t i = 3; i < a.size(); ++i)
						if (a[i].starts_with("style=")) spec.style = a[i].substr(6);
				std::string problem;
				const std::string made = CreateWorld(a[1], spec, &problem);
				if (made.empty())
					m_console.Refuse("could not create: " + problem);
				else
					m_console.Print(std::format("created world '{}' - "
												"`worlds load {}` to open it",
												made, made));
				return;
			}
			if (a[0] == "load" && a.size() >= 2) {
				if (m_world && a[1] == m_project.FolderName()) {
					m_console.Print("already in '" + a[1] + "'"); // as asked
					return;
				}
				// A new game there, from the next frame: the switch destroys this
				// world, and a console command is running inside it. Refused with
				// SwitchWorld's reason - no such world, or a bake running (C234).
				std::string why;
				if (SwitchWorld(a[1], &why)) m_console.Print("switching to " + a[1]);
				else m_console.Refuse("worlds load: " + why);
				return;
			}
			if (a[0] == "delete" && a.size() >= 2) {
				// The console's form of the typed confirmation: the name TWICE,
				// matched exactly. The rules are DeleteWorld's either way, and
				// every one of them REFUSES (C442).
				if (a.size() < 3 || a[2] != a[1]) {
					m_console.Refuse("to delete, type the name twice: worlds delete " +
									 a[1] + " " + a[1] + " (case-sensitive)");
					return;
				}
				if (const std::string why = WorldDeleteRefusal(a[1]); !why.empty()) {
					m_console.Refuse(why);
					return;
				}
				if (DeleteWorld(a[1])) m_console.Print("deleted world '" + a[1] + "'");
				else m_console.Refuse("could not delete (see the log)");
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
					m_console.Refuse("the worlds dialog needs the world map "
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
			if (a[0] == "tags") { // the wizard's tag choices (the template's tags)
				std::string list;
				for (const std::string& t : WizardTags()) list += (list.empty() ? "" : " ") + t;
				m_console.Print("wizard tags: " + list);
				return;
			}
			if (a[0] == "newdialog") {
				// The New world dialog (P4), for a harness: the same calls its
				// buttons make. It is modal in a level and on the world screen
				// alike, so unlike the Worlds dialog it opens from either.
				// `popup [<n>]` presses its n-th shown drop-down (opened by its
				// next Update), or bare says whether one is open - what its Esc
				// asks first (C81). It never opens the dialog.
				if (a.size() >= 2 && a[1] == "popup") {
					if (a.size() >= 3 && m_newWorldDialog.IsOpen()) {
						m_newWorldDialog.OpenPopup(std::atoi(a[2].c_str()));
						m_console.Print("new world dialog popup: pressing #" + a[2]);
					} else {
						m_console.Print(std::format(
							"new world dialog popup: dialog {} popup {}",
							m_newWorldDialog.IsOpen() ? "open" : "closed",
							m_newWorldDialog.PopupOpen() ? "open" : "shut"));
					}
					return;
				}
				if (a.size() >= 2 && a[1] == "off") {
					m_newWorldDialog.Close();
				} else {
					if (!m_newWorldDialog.IsOpen()) m_newWorldDialog.Open();
					using S = NewWorldSpec::Source;
					if (a.size() >= 6 && a[1] == "wizard") {
						// wizard <tag|-> <size> <difficulty> <seed>: the rows' values.
						m_newWorldDialog.SetWizard(
							a[2] == "-" ? std::string() : a[2], std::atoi(a[3].c_str()),
							std::strtof(a[4].c_str(), nullptr),
							static_cast<u32>(std::strtoul(a[5].c_str(), nullptr, 10)));
					} else if (a.size() >= 3 && a[1] == "source") {
						// A source word, level or style the dialog does not offer is
						// REFUSED, not taken as Blank or as a pick with nothing
						// behind it: the UI sweep audits what is on screen after
						// these (tools\InGameTest.ps1, code-review C427).
						if (a[2] == "copy") m_newWorldDialog.SetSource(S::CopyWorld);
						else if (a[2] == "wizard") m_newWorldDialog.SetSource(S::Wizard);
						else if (a[2] == "blank") m_newWorldDialog.SetSource(S::Blank);
						else if (a[2] == "level") {
							const std::string stem = a.size() >= 4 ? a[3] : std::string();
							const std::vector<std::string>& levels = m_newWorldDialog.Levels();
							if (!stem.empty() && std::ranges::find(levels, stem) == levels.end()) {
								m_console.Refuse("new world dialog: this world has no level '" + stem + "'");
								return;
							}
							m_newWorldDialog.SetSource(S::CopyLevel, stem);
						} else {
							m_console.RefuseUsage();
							return;
						}
					} else if (a.size() >= 3 && a[1] == "style") {
						const std::string id = a[2] == "-" ? std::string() : a[2];
						if (!id.empty() && !m_newWorldDialog.OffersStyle(id)) {
							m_console.Refuse("new world dialog: the library has no style '" + id + "'");
							return;
						}
						m_newWorldDialog.SetStyle(id);
					} else if (a.size() >= 3 && a[1] == "create") {
						m_newWorldDialog.Create(a[2]);
					} else if (a.size() >= 2 && a[1] == "switch") {
						m_newWorldDialog.SwitchNow();
					}
					m_newWorldDialog.ApplyPending(); // not inside a tree walk here
				}
				static constexpr const char* kSource[] = {"blank", "copy", "level", "wizard"};
				const NewWorldSpec& sp = m_newWorldDialog.Spec();
				m_console.Print(std::format(
					"new world dialog {}: source {} made '{}' style {} - {}",
					m_newWorldDialog.IsOpen() ? "open" : "closed",
					kSource[static_cast<int>(m_newWorldDialog.Source())],
					m_newWorldDialog.Made(), sp.style.empty() ? "-" : sp.style,
					m_newWorldDialog.Note()));
				if (sp.source == NewWorldSpec::Source::Wizard)
					m_console.Print(std::format("  wizard tag '{}' size {} difficulty {:.2f} seed {}",
												sp.tag, sp.size, sp.difficulty, sp.seed));
				if (sp.source == NewWorldSpec::Source::CopyLevel)
					m_console.Print(std::format("  copy level '{}'", sp.level));
				return;
			}
			// The registered forms: this hand-written line had drifted from them
			// (no `status`, no `tags`, no wizard) - code-review C442.
			m_console.RefuseUsage();
		});
	m_console.Register(
		{.name = "mappage",
		 .group = CmdGroup::Levels,
		 .params = "[dungeon|world]\n"
				   "open\n"
				   "close",
		 .summary = "open, close or page the player map without a keyboard"},
		[this](const std::vector<std::string>& a) {
			// The M key and the toggle button, reachable by a harness. It
			// reports the page, whether the toggle is even OFFERED, and whether
			// the map is open — three different facts: a project with no
			// overworld has no second page and must not advertise one, and the
			// page is DERIVED from the overlay being up, so it cannot outlive
			// it.
			if (!a.empty()) {
				if (a[0] == "world" && !m_worldMap) {
					m_console.Refuse("this project has no world map");
					return;
				}
				// open/close are exactly what M does, page reset included.
				if (a[0] == "open" || a[0] == "close") {
					if (a[0] == "open") m_mapView.Open(MapView::Mode::Player);
					else m_mapView.Close();
					ShowMapPage(MapPage::Dungeon);
				} else if (a[0] == "world" || a[0] == "dungeon") {
					ShowMapPage(a[0] == "world" ? MapPage::World : MapPage::Dungeon);
					// The page is DERIVED from the overlay, so the world page
					// asked for with the map shut is not shown - a refusal, and
					// the readout below still says where it stands (the line
					// worldprops.eval reads).
					if (a[0] == "world" && !ShowingWorldPage())
						m_console.Refuse("mappage: the world page shows only while "
										 "the map is open");
				} else {
					m_console.RefuseUsage(); // a typo used to turn to the dungeon page
					return;
				}
			}
			m_console.Print(std::format(
				"map page: {} (toggle {}, map {})",
				ShowingWorldPage() ? "world" : "dungeon",
				m_mapView.hasWorld ? "offered" : "hidden",
				m_mapView.IsOpen() ? "open" : "closed"));
		});
	m_console.Register(
		{.name = "catround",
		 .group = CmdGroup::Levels,
		 .summary = "check every project file survives being written back unchanged"},
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

			// THE CASES NO PROJECT CARRIES (code-review C323). The files above
			// show what the writers do to the files this project happens to
			// have, and the three ways a write still lost comments were all
			// elsewhere: a catalog with no entry yet, the first entry deleted,
			// and the monster dialog's Save. Each case is text handed to the
			// REAL writer and the text it must give back - in memory, nothing
			// written.
			const auto crlf = [](std::string_view s) {
				return serialize::NormalizeEol(std::string(s));
			};
			std::vector<std::pair<const char*, std::string>> cases;
			{
				// A CATALOG WITH NO ENTRY YET (the template's flags.cat): its
				// comments are the whole file and come back whole - and the
				// first entry added goes UNDER them, not above.
				const std::string header =
					crlf("; Flags: a catalog with no entry yet.\n;\n; Fields: display.\n\n");
				Catalog c;
				c.LoadText(header);
				std::string why = FirstDifference(c.Serialize("Flags: generated."), header);
				CatalogEntry first;
				first.id = "first";
				first.Set("display", "First");
				c.Add(std::move(first));
				if (why.empty())
					why = FirstDifference(c.Serialize("Flags: generated."),
										  header + crlf("[first]\ndisplay = First\n"));
				cases.emplace_back("header-only", why);
			}
			{
				// DELETING THE FIRST ENTRY: the file's header passes to the next
				// one, above that entry's own note - and with nothing left, it
				// is the file.
				const std::string head = crlf("; Things: the file's header.\n; fields: x, y.\n\n");
				Catalog c;
				c.LoadText(head + crlf("[a]\nx = 1\n\n; b's own note\n[b]\ny = 2\n"));
				c.Remove("a");
				std::string why = FirstDifference(c.Serialize("Things: generated."),
												  head + crlf("; b's own note\n[b]\ny = 2\n"));
				c.Remove("b");
				if (why.empty())
					why = FirstDifference(c.Serialize("Things: generated."),
										  head + crlf("; b's own note\n"));
				cases.emplace_back("first-entry", why);
			}
			{
				// THE MONSTER DIALOG'S SAVE, through the rows it owns: saved as it
				// was opened, the entry comes back as it was - threat_threshold
				// where it stood, its note above it - and a row set back to its
				// default goes, note and all.
				const std::string head = crlf("; Monsters: the file's header.\n\n"
											  "[cr_brute]\n"
											  "display = Round trip\n");
				const std::string note =
					crlf("; A single-minded brute (the note a Save used to delete).\n"
						 "threat_threshold = 0.6\n");
				const std::string tail = crlf("model = skel_warrior\n"
											  "archetype = brute\n"
											  "hp = 22\n"
											  "states = idle walk\n"
											  "anim_idle = Idle\n"
											  "anim_walk = Walk Walk2\n"
											  "size = large\n");
				MonsterConfigDialog::Config cfg;
				cfg.type = "cr_brute";
				cfg.archetype = ai::Archetype::Brute;
				cfg.threat.threshold = 0.6f;
				const auto idle = static_cast<size_t>(anim::CreatureState::Idle);
				const auto walk = static_cast<size_t>(anim::CreatureState::Walk);
				cfg.supported[idle] = cfg.supported[walk] = true;
				cfg.clips[idle] = {"Idle"};
				cfg.clips[walk] = {"Walk", "Walk2"};
				Catalog c;
				c.LoadText(head + note + tail);
				const auto save = [&] {
					CatalogEntry e = *c.Find("cr_brute");
					ApplyMonsterConfig(e, cfg);
					c.Add(std::move(e));
					return c.Serialize("Monsters: generated.");
				};
				std::string why = FirstDifference(save(), head + note + tail);
				cfg.threat.threshold = 1.0f;
				if (why.empty()) why = FirstDifference(save(), head + tail);
				cases.emplace_back("monster-config", why);
			}
			int passed = 0;
			for (const auto& [name, why] : cases) {
				passed += why.empty() ? 1 : 0;
				m_console.Print(std::format("catround case {}: {}", name,
											why.empty() ? "ok" : "DIFFERS - " + why));
			}
			m_console.Print(
				std::format("catround cases {} of {} pass", passed, cases.size()));
		});
	m_console.Register(
		{.name = "levels",
		 .group = CmdGroup::Levels,
		 .params = "[list]\n"
				   "new [dungeon]\n"
				   "view <stem>",
		 .summary = "list levels by dungeon, create a level, or browse one"},
		[this](const std::vector<std::string>& a) {
			// THE PICKER'S LIST, WITHOUT A MOUSE. The toolbar dropdown is what
			// W5 actually built; this prints the same grouping (through the same
			// Project helpers) so a harness can see that a level is in the
			// dungeon it was made in, which no screenshot can assert.
			if (a.size() >= 2 && a[0] == "view") {
				// Picking a level in that dropdown: browse it, which is what
				// the editor's Generate button (and `generate again`) acts on.
				// Only a stem the picker OFFERS - the manifest's, as `goto`
				// checks - and checked BEFORE the browse: SetViewLevel takes any
				// stem, and BrowseLevel of one with no file asserts in
				// DungeonMap's load, so asking afterwards could never refuse.
				if (!m_world) {
					m_console.Refuse("levels view: no world loaded");
					return;
				}
				if (std::find(m_project.levels.begin(), m_project.levels.end(), a[1]) ==
					m_project.levels.end()) {
					m_console.Refuse(std::format("levels view: no level '{}' (viewing {})", a[1],
												 m_mapView.ViewedLevel()));
					return;
				}
				m_mapView.SetViewLevel(a[1]);
				m_console.Print("viewing " + m_mapView.ViewedLevel());
				return;
			}
			if (!a.empty() && a[0] == "new") {
				// The [+] button's path. With no argument it lands in the
				// dungeon of the level being VIEWED, exactly as the button does.
				const std::string dungeon =
					a.size() >= 2 ? a[1] : m_mapView.ViewedDungeon();
				const std::string stem = CreateNewLevel(dungeon);
				if (stem.empty())
					m_console.Refuse("could not create");
				else
					m_console.Print(std::format("created {} in {}", stem,
												dungeon.empty() ? "no dungeon" : dungeon));
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
	// `levelcheck`, the project-wide check of the pool, is Game_LevelCheck.cpp.

	// --- the world editor -------------------------------------------------
	m_console.Register(
		{.name = "worldedit",
		 .group = CmdGroup::World,
		 .params = "[on|off]",
		 .summary = "switch the world map between edit and play mode"},
		[this](const std::vector<std::string>& args) {
			if (!m_worldMap) {
				m_console.Refuse("no world map loaded");
				return;
			}
			if (!args.empty())
				m_worldMapView.SetMode(args[0] == "off" ? WorldMapView::Mode::Play
														: WorldMapView::Mode::Editor);
			// REPORTS when bare, like `rest` and `encounters`: a mode command
			// whose meaning depends on a state you cannot see is a coin flip.
			// It reports the MODE, which is the travel screen's: the player
			// map's world page stays play in either (WorldMapView::Editing,
			// code-review C79), and `worldview` says what is drawn.
			const bool editor = m_worldMapView.CurrentMode() == WorldMapView::Mode::Editor;
			m_console.Print(editor ? "world editing (fog off)" : "world playing (fog on)");
		});
	m_console.Register(
		{.name = "worldview",
		 .group = CmdGroup::World,
		 .params = "\n"
				   "click <x> <z> left|right",
		 .summary = "report the world view as drawn, or click one of its cells"},
		[this](const std::vector<std::string>& a) {
			// The world view WHERE IT IS UP - the travel screen, or the player
			// map's world page in a dungeon - read off the view itself, and
			// clicked through its own Update. The page is the case that matters:
			// it must stay play in Editor mode (code-review C77, C79), and only a
			// click through the real input path can show that it neither paints
			// nor opens a dialog.
			if (!m_worldMap) {
				m_console.Refuse("no world map loaded");
				return;
			}
			const bool travel = m_state == AppState::WorldMap;
			if (!travel && !ShowingWorldPage()) {
				m_console.Refuse("the world view is not up (`worldmap on`, or `mappage open` "
								 "then `mappage world`)");
				return;
			}
			// The view's OWN overlay flag, as the frame before derived it
			// (UpdateStates derives it after the console and the script have
			// run their commands) - READ, never set. A report that set it first
			// would read back its own write, and pass with the derivation the
			// C79 fix rests on gone or moved after the view's Update. A view
			// that disagrees with the screen that is up is the defect, so it
			// refuses and says so.
			if (m_worldMapView.IsOverlay() == travel) {
				m_console.Refuse(std::format(
					"the world view's overlay flag says {} but the {} is up - the "
					"per-frame derivation did not run",
					m_worldMapView.IsOverlay() ? "map page" : "travel screen",
					travel ? "travel screen" : "map page"));
				return;
			}
			const float w = static_cast<float>(m_window.Width());
			const float h = static_cast<float>(m_window.Height());
			const gfx::Rect panel = travel ? WorldPanel(w, h) : MapPanel(w, h);
			if (!a.empty()) {
				if (a[0] != "click" || a.size() < 4 || (a[3] != "left" && a[3] != "right")) {
					m_console.RefuseUsage();
					return;
				}
				const int x = std::atoi(a[1].c_str());
				const int z = std::atoi(a[2].c_str());
				Vec2 p{};
				if (!m_worldMapView.CellPoint(*m_worldMap, panel, x, z, p)) {
					m_console.Refuse(std::format("{},{} is off the world grid", x, z));
					return;
				}
				const std::string was = m_worldMap->TerrainAt(x, z).id;
				// TWO FRAMES of what a mouse sends: the press at the cell's
				// centre, then the button up there. The stroke a paint opens is
				// left for the state's own Update to close - which is the point:
				// on the map page nothing would.
				const MouseButton button =
					a[3] == "right" ? MouseButton::Right : MouseButton::Left;
				Input press;
				press.OnMouseMove(p.x, p.y);
				press.OnMouseButton(button, true);
				m_worldMapView.Update(press, *m_worldMap, panel);
				Input release;
				release.OnMouseMove(p.x, p.y);
				m_worldMapView.Update(release, *m_worldMap, panel);
				std::string dialogs;
				const auto open = [&dialogs](bool up, const char* name) {
					if (up) dialogs += (dialogs.empty() ? "" : " ") + std::string(name);
				};
				open(m_worldSettingsDialog.IsOpen(), "settings");
				open(m_worldsDialog.IsOpen(), "worlds");
				open(m_newWorldDialog.IsOpen(), "newworld");
				m_console.Print(std::format(
					"worldview click {},{} {}: {} -> {}, stroke {}, dialogs {}", x, z, a[3],
					was, m_worldMap->TerrainAt(x, z).id, m_worldStroke ? "open" : "none",
					dialogs.empty() ? "none" : dialogs));
				return;
			}
			// FOG IS WHAT RENDER DREW, recorded by WorldMapView::Render itself
			// (LastDrawn) on the last rendered frame - not Editing() asked a
			// second time, which would agree with `editing` whatever Render
			// did. Only a frame that drew this view, as this page, counts: a
			// headless run draws nothing and says "not drawn". The toolbar count
			// is ToolbarButtons' own list.
			const bool editor = m_worldMapView.CurrentMode() == WorldMapView::Mode::Editor;
			const WorldMapView::Drawn& drawn = m_worldMapView.LastDrawn();
			const bool drewThis = m_worldViewFrame != 0 && m_worldViewFrame == m_framesRendered &&
								  drawn.overlay == !travel;
			m_console.Print(std::format(
				"world view: {}, mode {}, editing {}, toolbar {}, fog {}",
				travel ? "travel screen" : "map page", editor ? "editor" : "play",
				m_worldMapView.Editing() ? "yes" : "no", m_worldMapView.ToolCount(panel),
				!drewThis ? "not drawn" : drawn.fogLifted ? "off" : "on"));
		});
	m_console.Register(
		{.name = "backdrop",
		 .group = CmdGroup::World,
		 .summary = "print what the last rendered frame drew behind the app state"},
		[this](const std::vector<std::string>&) {
			// WHAT RENDER DID, recorded by Render itself: the state whose picture
			// it drew behind the state's own page - set in the switch case that
			// drew it, so a frame that drew none reads "over nothing" - and
			// whether the 3D scene pass ran. The pause menu and the sheet opened
			// from the world map must read "over worldmap - scene skipped"; they
			// used to draw the parked dungeon (code-review C365). LOGGED as well
			// as printed, because InGameTest sweeps with the console echo off and
			// reads the log. A headless run renders nothing, and says so rather
			// than reporting a frame that never happened.
			const std::string line =
				m_backdropFrame == 0
					? std::string("backdrop: nothing rendered yet (a headless run draws nothing)")
					: std::format("backdrop: {} over {} - scene {} (frame {})", StateName(),
								  m_drawnBackdrop ? StateWord(*m_drawnBackdrop) : "nothing",
								  m_drewScene ? "drawn" : "skipped", m_backdropFrame);
			m_console.Print(line);
			log::Info("{}", line);
		});
	m_console.Register(
		{.name = "terrainbrush",
		 .group = CmdGroup::World,
		 .params = "[<terrain>|off]",
		 .summary = "arm or disarm the world terrain brush"},
		[this](const std::vector<std::string>& args) {
			if (!m_worldMap) {
				m_console.Refuse("no world map loaded");
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
						m_console.Refuse(std::format("no terrain '{}'", args[0]));
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
		{.name = "paint",
		 .group = CmdGroup::World,
		 .params = "<x> <z>",
		 .summary = "paint the armed terrain brush on a world cell"},
		[this](const std::vector<std::string>& args) {
			// The mouse path's rules, reachable without a mouse: same armed
			// brush, same undo bracketing, same refusal to repaint a cell that
			// is already that terrain.
			if (!m_worldMap) {
				m_console.Refuse("no world map loaded");
				return;
			}
			if (!Need(m_console, args, 2)) return;
			if (m_worldMapView.ArmedTerrain().empty()) {
				m_console.Refuse("no terrain armed (arm with terrainbrush)");
				return;
			}
			const int x = std::atoi(args[0].c_str());
			const int z = std::atoi(args[1].c_str());
			if (!m_worldMap->InBounds(x, z)) {
				m_console.Refuse(std::format("{},{} is off the world grid", x, z));
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
			m_world->BeginUndoStep();
			const bool changed =
				m_worldMap->SetTerrainAt(x, z, m_worldMapView.ArmedTerrain());
			m_world->CommitUndoStep(changed);
			// "already that terrain" above is the world as asked, so it prints;
			// a paint the map declined is a refusal (C442).
			if (changed)
				m_console.Print(std::format("{},{} {} -> {}", x, z, was,
											m_worldMapView.ArmedTerrain()));
			else
				m_console.Refuse(std::format("{},{} unchanged", x, z));
		});
	m_console.Register(
		{.name = "worldprops",
		 .group = CmdGroup::World,
		 .params = "\n"
				   "start <x> <z>\n"
				   "opening <dungeon> <level> <x> <z>\n"
				   "opening world\n"
				   "eval <level>",
		 .summary = "show or set the world start, the game's opening and harness level"},
		[this](const std::vector<std::string>& a) {
			if (!m_worldMap) {
				m_console.Refuse("no world map loaded");
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
				m_world->BeginUndoStep();
				const bool ok = m_worldMap->SetStart(x, z);
				m_world->CommitUndoStep(ok);
				// A start that did not move is REFUSED, not reported: a script
				// carrying on from it would begin somewhere it did not ask for.
				if (ok) m_console.Print(std::format("world start {},{}", x, z));
				else if (!m_worldMap->InBounds(x, z))
					m_console.Refuse("off the world grid");
				else
					m_console.Refuse(std::format("{},{} is impassable ({})", x, z,
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
				m_console.RefuseUsage(); // the registered forms, not a drifted copy
				return;
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
		{.name = "worldloc",
		 .group = CmdGroup::World,
		 .params = "\n"
				   "add <kind> <id> <x> <z>\n"
				   "del <id>\n"
				   "move <id> <x> <z>\n"
				   "set <id> <kind|dungeon|level> <value>\n"
				   "set <id> entry <x> <z>|none\n"
				   "set <id> entryx|entryz <value>",
		 .summary = "list, add, remove, move or edit world locations"},
		[this](const std::vector<std::string>& a) {
			if (!m_worldMap) {
				m_console.Refuse("no world map loaded");
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
			// a world its own loader rejects - and each one is a REFUSE, so a
			// script whose setup it stopped fails instead of measuring on (C442).
			const auto say = [this](bool ok, const std::string& done, const char* why) {
				if (ok) m_console.Print(done);
				else m_console.Refuse(why);
			};
			const std::string& verb = a[0];
			if (verb == "add" && a.size() >= 5) {
				WorldMap::Location l;
				l.kind = a[1];
				l.id = a[2];
				l.x = std::atoi(a[3].c_str());
				l.z = std::atoi(a[4].c_str());
				m_world->BeginUndoStep();
				const bool ok = w.AddLocation(std::move(l));
				m_world->CommitUndoStep(ok);
				say(ok, std::format("added {} at {},{}", a[2], a[3], a[4]),
					"refused: duplicate id, occupied cell, or off the grid");
			} else if (verb == "del" && a.size() >= 2) {
				m_world->BeginUndoStep();
				const bool ok = w.RemoveLocation(a[1]);
				m_world->CommitUndoStep(ok);
				say(ok, "removed " + a[1], "no such location");
			} else if (verb == "move" && a.size() >= 4) {
				m_world->BeginUndoStep();
				const bool ok = w.MoveLocation(a[1], std::atoi(a[2].c_str()),
											   std::atoi(a[3].c_str()));
				m_world->CommitUndoStep(ok);
				say(ok, std::format("{} -> {},{}", a[1], a[2], a[3]),
					"refused: unknown id, occupied cell, or off the grid");
			} else if (verb == "set" && a.size() >= 4) {
				WorldMap::Location* l = w.MutableLocation(a[1]);
				if (!l) {
					m_console.Refuse("no such location");
					return;
				}
				const std::string& field = a[2];
				if (field == "entry" || field == "entryx" || field == "entryz") {
					// WHERE IT LANDS goes through the map's one rule for an entry
					// (code-review C343): both coordinates or neither, never below
					// zero. `entryx 5` on a location with no entry used to leave
					// entryz at -1, and the save that wrote it aborted on its own
					// read-back. One half still moves on its own when the other is
					// set - what is refused is HALF AN ENTRY, not the form.
					int x = l->entryX, z = l->entryZ, given = 0;
					const bool none = field == "entry" && a[3] == "none";
					if (field == "entry" && !none) {
						if (a.size() < 5) {
							m_console.RefuseUsage();
							return;
						}
						x = std::atoi(a[3].c_str());
						z = std::atoi(a[4].c_str());
						given = std::min(x, z);
					} else if (field == "entryx") {
						x = given = std::atoi(a[3].c_str());
					} else if (field == "entryz") {
						z = given = std::atoi(a[3].c_str());
					}
					m_world->BeginUndoStep();
					const bool ok = none ? w.ClearLocationEntry(a[1])
										 : w.SetLocationEntry(a[1], x, z);
					m_world->CommitUndoStep(ok);
					if (ok)
						m_console.Print(none ? std::format("{}.entry = none (the level's own start)",
														   a[1])
											 : std::format("{}.entry = {},{}", a[1], x, z));
					else if (given < 0)
						m_console.Refuse("refused: an entry cell cannot be negative");
					else
						m_console.Refuse(std::format(
							"refused: half an entry - {} lands on both entryx and entryz or "
							"neither (worldloc set {} entry <x> <z>)",
							a[1], a[1]));
					return;
				}
				m_world->BeginUndoStep();
				bool ok = true;
				if (field == "dungeon") l->dungeon = a[3];
				else if (field == "level") l->level = a[3];
				else if (field == "kind") l->kind = a[3];
				else ok = false;
				m_world->CommitUndoStep(ok);
				say(ok, std::format("{}.{} = {}", a[1], field, a[3]),
					"field must be kind/dungeon/level/entry/entryx/entryz");
			} else {
				m_console.RefuseUsage();
			}
		});
	m_console.Register(
		{.name = "worldarea",
		 .group = CmdGroup::World,
		 .params = "\n"
				   "add <id> <x> <z> <w> <h> [difficulty]\n"
				   "del <id>\n"
				   "order <id> <index>\n"
				   "at <x> <z>",
		 .summary = "list, add, remove or reorder world areas; ask who owns a cell"},
		[this](const std::vector<std::string>& a) {
			if (!m_worldMap) {
				m_console.Refuse("no world map loaded");
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
				m_world->BeginUndoStep();
				const bool ok = m_worldMap->AddArea(std::move(ar));
				m_world->CommitUndoStep(ok);
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
				// Each refusal a REFUSE as well (C442), counted by a script.
				if (ok)
					m_console.Print(std::format("added {} (row {}, wins over earlier)",
												a[1], areas.size() - 1));
				else if (w <= 0 || h <= 0)
					m_console.Refuse("refused: an area needs a positive extent");
				else
					m_console.Refuse(std::format(
						"refused: an area named '{}' already exists", a[1]));
			} else if (verb == "del" && a.size() >= 2) {
				m_world->BeginUndoStep();
				const bool ok = m_worldMap->RemoveArea(a[1]);
				m_world->CommitUndoStep(ok);
				if (ok) m_console.Print("removed " + a[1]);
				else m_console.Refuse("no such area");
			} else if (verb == "order" && a.size() >= 3) {
				const int to = std::atoi(a[2].c_str());
				m_world->BeginUndoStep();
				const bool ok = m_worldMap->MoveArea(a[1], to);
				m_world->CommitUndoStep(ok);
				if (ok) m_console.Print(std::format("{} is now row {}", a[1], to));
				else m_console.Refuse("no such area, index out of range, or already there");
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
				m_console.RefuseUsage();
			}
		});
	m_console.Register(
		{.name = "worldsettings",
		 .group = CmdGroup::World,
		 .params = "[location]\n"
				   "off\n"
				   "status\n"
				   "start <x> <z>\n"
				   "move <x> <z>\n"
				   "entry <x> <z>|none\n"
				   "add|delete",
		 .summary = "open, close, report or edit through the world settings dialog"},
		[this](const std::vector<std::string>& a) {
			// The toolbar's Settings disc, reachable without a mouse. It does
			// NOT duplicate the rules — the dialog's callbacks are the same
			// WorldMap calls `worldprops`/`worldloc`/`worldarea` make — so this
			// opens and closes the thing, and drives the two fields whose
			// refusals speak in a status row and the doorway rows that add and
			// delete.
			if (!a.empty() && a[0] == "off") {
				m_worldSettingsDialog.Close();
				m_console.Print("world settings closed");
				return;
			}
			// THE STATUS ROWS, EACH TAB'S OWN (code-review C102). `start` is the
			// World tab's start fields and `move` the Doorways tab's cell fields
			// for the selected doorway - the SAME member calls those fields make
			// - and `status` prints what each tab's row SHOWS, read off its
			// Label, so a note written into the other tab's row reads as wrong.
			// `entry` is the Doorways tab's landing cell (its checkbox and two
			// fields; `none` = the box unticked), whose refusal of a negative
			// speaks in that row too (code-review C343).
			if (!a.empty() && (a[0] == "status" || a[0] == "start" || a[0] == "move" ||
							   a[0] == "entry")) {
				WorldSettingsDialog& d = m_worldSettingsDialog;
				if (!d.IsOpen()) {
					m_console.Refuse("world settings are not open");
					return;
				}
				if (a[0] == "entry" && a.size() == 2 && a[1] == "none") {
					d.SetSelectedEntry(-1, -1);
				} else if (a[0] != "status") {
					if (!Need(m_console, a, 3)) return;
					const int x = std::atoi(a[1].c_str());
					const int z = std::atoi(a[2].c_str());
					if (a[0] == "start") d.SetStart(x, z);
					else if (a[0] == "move") d.MoveSelected(x, z);
					else d.SetSelectedEntry(x, z);
				}
				static constexpr const char* kTab[] = {"world", "areas", "doorways"};
				const int tab = d.ActiveTab();
				m_console.Print(std::format(
					"world settings open: tab {} selected '{}' - world note '{}' - "
					"doorways note '{}'",
					tab >= 0 && tab < 3 ? kTab[tab] : "?", d.SelectedLocation(),
					d.NoteShown(WorldSettingsDialog::NoteTab::World),
					d.NoteShown(WorldSettingsDialog::NoteTab::Doorways)));
				return;
			}
			// `add` and `delete` are the Doorways tab's "+ Add" and Delete rows,
			// the same member calls. Both rebuild the tab a frame later (the
			// deferred-rebuild rule), so they say only what they did, and the
			// row they leave behind is read by a `status` on the next line - a
			// delete must leave no note about the doorway it took.
			if (!a.empty() && (a[0] == "add" || a[0] == "delete")) {
				WorldSettingsDialog& d = m_worldSettingsDialog;
				if (!d.IsOpen()) {
					m_console.Refuse("world settings are not open");
					return;
				}
				if (a[0] == "add") {
					d.AddLocation();
				} else if (d.SelectedLocation().empty()) {
					m_console.Refuse("no doorway is selected to delete");
					return;
				} else {
					d.DeleteSelected();
				}
				m_console.Print(std::format("world settings {}: selected '{}'", a[0],
											d.SelectedLocation()));
				return;
			}
			if (!m_worldMap) {
				m_console.Refuse("no world map loaded");
				return;
			}
			// ON THE WORLD SCREEN ONLY, because that is the one state whose
			// Update routes input to it. Opened over a dungeon it would draw a
			// modal nothing could type into or close - a console command
			// reaching further than the button it stands for. A REFUSE, so a
			// sweep that meant to audit the dialog fails rather than auditing
			// the dungeon under it (C442).
			if (m_state != AppState::WorldMap) {
				m_console.Refuse("world settings need the world map "
								 "(try `worldmap on`)");
				return;
			}
			OpenWorldSettings(a.empty() ? std::string() : a[0]);
			if (m_worldSettingsDialog.IsOpen()) m_console.Print("world settings open");
			else m_console.Refuse("could not open");
		});
	// The asset pool browser without the type editor in front of it: the
	// harness cannot click a `texture` field. It shares ThumbCache with the
	// portrait picker, so this is how that cache's other client gets exercised;
	// status prints the SRV gauge for the same reason the picker's does.
	m_console.Register(
		{.name = "assetpicker",
		 .group = CmdGroup::Types,
		 .params = "textures|models [name]\nonly [name...]\nscroll <0..1>\nsurvey\noff|status",
		 .summary = "open the asset pool browser (in the editor), or report it"},
		[this](const std::vector<std::string>& args) {
			const std::string sub = args.empty() ? "status" : args[0];
			// `only`: the open picker lists just these (none = all again) - several
			// at once, which the search box cannot (the map-icon survey's models).
			if (sub == "only" || sub == "scroll" || sub == "survey") {
				if (!m_assetPicker.IsOpen()) {
					m_console.Refuse(std::format("assetpicker {}: the picker is not open", sub));
					return;
				}
			}
			if (sub == "only") {
				m_assetPicker.ShowOnly({args.begin() + 1, args.end()});
			} else if (sub == "scroll") {
				if (!Need(m_console, args, 2)) return;
				m_assetPicker.ScrollTo(static_cast<float>(std::atof(args[1].c_str())));
			} else if (sub == "survey") {
				// THE BRIGHTNESS SURVEY (code-review C158): each texture tile in view,
				// where its image was drawn and its set's stored mean - what a correct
				// (linear) draw of it averages to. A harness photographs the window
				// and sets the two side by side (InGameTest). On the MODEL grid each
				// tile's rect alone (`assetpicker model`, mean `-`): a model tile is
				// a render, compared with the same tile from another build
				// (code-review C253's survey of every model the node bake touches).
				const bool models = m_assetPicker.CurrentMode() == AssetPicker::Mode::Models;
				const std::vector<AssetPicker::SurveyTile> tiles = m_assetPicker.SurveyTiles();
				m_console.Print(std::format("assetpicker survey: {} tiles", tiles.size()));
				for (const AssetPicker::SurveyTile& t : tiles)
					PrintThumbSurveyLine(models ? "assetpicker model" : "assetpicker tile", t.name,
										 t.stem, t.img, t.drawn);
				return;
			} else if (sub == "textures" || sub == "models") {
				m_pickApply = nullptr; // a pick goes nowhere
				// A name opens on that asset, selected and previewed, as a field
				// naming it would.
				m_assetPicker.Open(sub == "textures" ? AssetPicker::Mode::Textures
													 : AssetPicker::Mode::Models,
								   args.size() > 1 ? args[1] : std::string(), loc::Tr(sub == "textures" ? "map.type.texture"
																 : "map.type.model"),
								   m_settings.theme);
			} else if (sub == "off") {
				m_assetPicker.Close();
			} else if (sub != "status") {
				m_console.RefuseUsage();
				return;
			}
			// The cache's side (code-review C111): the tiles in view, how many still
			// show no image and how many found none, and what eviction did since the
			// picker opened. A RELOAD is a tile evicted and wanted again - with the
			// view still, one evicted while it was on screen. Then the heap line.
			const AssetPicker::ThumbStatus st = m_assetPicker.GetThumbStatus();
			m_console.Print(std::format(
				"assetpicker {} thumbs={} srv={} peak={} visible={} blank={} missing={} "
				"onscreen={} cap={} evicted={} reloads={} refused={} heapline={} heaptop={}",
				m_assetPicker.IsOpen() ? "open" : "closed", m_assetPicker.ThumbCount(),
				m_device.SrvLive(), m_device.SrvHighWater(), st.visible, st.blank, st.missing,
				st.counts.onScreen, st.counts.cap, st.counts.evicted, st.counts.reloads,
				st.counts.refused, st.counts.heapLine, st.counts.heapTop));
		});
	// The thumbnail caches' cap, FORCED (code-review C111): every ThumbCache
	// evicts against it and does not grow it to the screen, so a cap below what
	// is in view can be set and the pickers' status lines show whether a tile
	// on screen was ever evicted (`reloads=` with the view still). `heap` moves
	// the SRV line no thumbnail loads past (ThumbCache::kHeapLine), so a check
	// can put it just above what is live and watch a cache stop there.
	m_console.Register(
		{.name = "thumbcap",
		 .group = CmdGroup::Types,
		 .params = "[<n>|off]\nheap <n>|off",
		 .summary = "force the thumbnail caches' cap or heap line (checks of the eviction rules)"},
		[this](const std::vector<std::string>& args) {
			if (!args.empty() && args[0] == "heap") {
				if (args.size() < 2) {
					m_console.RefuseUsage();
					return;
				}
				if (args[1] == "off") {
					ThumbCacheKnobs::heapLineOverride = 0;
				} else {
					const int n = std::atoi(args[1].c_str());
					if (n < 1 || n > static_cast<int>(gfx::kSrvHeapCapacity)) {
						m_console.RefuseUsage();
						return;
					}
					ThumbCacheKnobs::heapLineOverride = static_cast<u32>(n);
				}
				m_console.Print(std::format(
					"thumbcap heap {}{}", ThumbCacheKnobs::HeapLine(),
					ThumbCacheKnobs::heapLineOverride ? " (forced)" : " (the real line)"));
				return;
			}
			if (!args.empty()) {
				if (args[0] == "off") {
					ThumbCacheKnobs::capOverride = 0;
				} else {
					const int n = std::atoi(args[0].c_str());
					if (n < 1) {
						m_console.RefuseUsage();
						return;
					}
					ThumbCacheKnobs::capOverride = static_cast<size_t>(n);
				}
			}
			if (ThumbCacheKnobs::capOverride)
				m_console.Print(std::format("thumbcap {} (forced, not grown to the screen)",
											ThumbCacheKnobs::capOverride));
			else
				m_console.Print("thumbcap off (each cache's own, grown to twice what is on screen "
								"within the heap line)");
		});
	m_console.Register(
		{.name = "newtype",
		 .group = CmdGroup::Types,
		 .params = "<dungeons|terrain|quests|flags|themes>",
		 .summary = "create a new pure-data type in a catalog"},
		[this](const std::vector<std::string>& args) {
			// The palette's "+ New..." for these categories, reachable without a
			// mouse — the harness cannot click, and this is the path W2 adds.
			if (!Need(m_console, args, 1)) return;
			const MapEditor::PaletteCat cat = MapEditor::CatForCatalogKey(args[0]);
			if (cat == MapEditor::PaletteCat::Count ||
				!MapEditor::CategoryAuthorable(cat)) {
				m_console.Refuse(std::format(
					"'{}' is not a pure-data category (dungeons/terrain/quests/flags/themes)",
					args[0]));
				return;
			}
			const std::string id = CreateAuthoredType(cat);
			if (id.empty()) m_console.Refuse("could not create");
			else m_console.Print(std::format("created {} '{}'", args[0], id));
		});
	m_console.Register(
		{.name = "newasset",
		 .group = CmdGroup::Types,
		 .params = "<category> installed <asset> <id>\n<category> pick <asset> <id>\n"
				   "<category> import <folder|file> <id>\ncreate\n"
				   "plan <category> <asset>\npreview\nstatus | off",
		 .summary = "the create dialog's Use installed or Import: pick, type the id, click "
					"Create; or what adopting a texture set would do"},
		[this](const std::vector<std::string>& args) {
			// The palette's "+ New..." dialog without a mouse. The create goes
			// through the dialog itself - its Validate, its Create, then onCreate -
			// so a refusal is the one the form shows (code-review C407). `pick`
			// stops short of Create and `create` clicks it later, so something can
			// happen in between (the form judged the pick when it was made).
			const auto catOf = [this](const std::string& key) {
				const MapEditor::PaletteCat cat = MapEditor::CatForCatalogKey(key);
				if (cat == MapEditor::PaletteCat::Count || !MapEditor::CategoryPlaceable(cat) ||
					MapEditor::CategoryAuthorable(cat)) {
					m_console.Refuse(std::format("newasset: '{}' has no create dialog", key));
					return MapEditor::PaletteCat::Count;
				}
				return cat;
			};
			if (args.size() == 3 && args[0] == "plan") {
				// AdoptSurfaceSet's answer, the rule onCreate acts on - "bake" is
				// the one case a create would start AssetBaker.
				if (catOf(args[1]) == MapEditor::PaletteCat::Count) return;
				const SurfaceAdopt adopt = AdoptSurfaceSet(args[1], args[2]);
				m_console.Print(std::format("newasset plan {} '{}': {}", args[1], args[2],
											!adopt.refusal.empty() ? "refused - " + adopt.refusal
											: adopt.bake ? std::string("bake")
														 : std::string("use as it is")));
				return;
			}
			// The footer's Create clicked, and what came of it: what the form held
			// is read first, since a made type closes it.
			const auto create = [this] {
				const std::string key = m_assetDialog.CatalogKey();
				const std::string id = m_assetDialog.TypedName();
				// What it is made from: the pool asset, or an import's folder or file.
				const std::string asset = m_assetDialog.Asset().empty()
											  ? m_assetDialog.SourcePath()
											  : m_assetDialog.Asset();
				m_assetDialog.ClickCreate();
				const Catalog* catalog = m_project.CatalogForKey(key);
				const char* outcome = m_baking                 ? "baking"
									  : m_assetDialog.IsOpen() ? "refused"
									  : catalog && catalog->Contains(id) ? "created"
																		 : "closed";
				const std::string problem = m_assetDialog.IsOpen() ? m_assetDialog.Problem()
																   : std::string();
				m_console.Print(std::format("newasset {} '{}' from {}: {}{}", key, id, asset,
											outcome, problem.empty() ? "" : " - " + problem));
			};
			if (args.size() == 4 && (args[1] == "installed" || args[1] == "pick")) {
				const MapEditor::PaletteCat cat = catOf(args[0]);
				if (cat == MapEditor::PaletteCat::Count) return;
				OpenCreateDialog(cat, AssetDialog::Source::Installed, args[2]);
				m_assetDialog.TypeName(args[3]);
				if (args[1] == "installed") {
					create();
					return;
				}
				const std::string problem = m_assetDialog.Problem();
				m_console.Print(std::format("newasset {} '{}' from {}: picked - {}", args[0],
											args[3], args[2],
											problem.empty() ? std::string("ready") : problem));
				return;
			}
			// IMPORT, Browse handed the folder (a texture set) or model file: a
			// texture set bakes in two runs, its maps then its worn meshes, and
			// lands with its imports.cat record (EditorTest phase 30 refuses a world
			// switch across both). A path may hold spaces: every word between
			// `import` and the id.
			if (args.size() >= 4 && args[1] == "import") {
				const MapEditor::PaletteCat cat = catOf(args[0]);
				if (cat == MapEditor::PaletteCat::Count) return;
				std::string source = args[2];
				for (size_t i = 3; i + 1 < args.size(); ++i) source += " " + args[i];
				OpenCreateDialog(cat, AssetDialog::Source::Import);
				m_assetDialog.PickSource(source);
				m_assetDialog.TypeName(args.back());
				create();
				return;
			}
			if (args.size() == 1 && args[0] == "create") {
				if (!m_assetDialog.IsOpen()) {
					m_console.Refuse("newasset: no create dialog is open");
					return;
				}
				create();
				return;
			}
			if (args.size() == 1 && args[0] == "preview") {
				// What the preview pane was handed, in the model's own units: a
				// bought .glb's node baked or not (code-review C253; Eval.ps1
				// -SelfTest reads it beside `decokind`).
				if (!m_assetDialog.IsOpen() || !m_assetDialog.HasPreview()) {
					m_console.Refuse("newasset: no create dialog with a preview is open");
					return;
				}
				m_console.Print(std::format("newasset preview {} radius={:.4f}",
											m_assetDialog.Asset(), m_assetDialog.PreviewRadius()));
				return;
			}
			if (args.size() == 1 && args[0] == "off") m_assetDialog.Close();
			else if (!args.empty() && !(args.size() == 1 && args[0] == "status")) {
				m_console.RefuseUsage();
				return;
			}
			m_console.Print(m_assetDialog.IsOpen()
								? std::format("newasset: open - {}", m_assetDialog.Problem())
								: std::string("newasset: closed"));
		});
	// THE ASSET BAKE in flight (StartBakeStep's AssetBaker): what is baking, and a
	// WALL-CLOCK wait for its baker to exit - the frame after, Update's poll lands
	// it (FinishBake writes the type; a texture import starts its second step), so
	// a script's next line sees the result. The `aiwait` shape: the main thread
	// blocks, and the wait refuses rather than run on forever. What it exists for
	// is EditorTest phase 30: a world switch asked while a bake runs is refused,
	// and the bake lands in the world that started it (code-review C234).
	m_console.Register(
		{.name = "bake",
		 .group = CmdGroup::Types,
		 .params = "[status]\nwait [seconds]",
		 .summary = "the running asset bake; wait on the wall clock for its baker to exit"},
		[this](const std::vector<std::string>& args) {
			const std::string what = args.empty() ? "status" : args[0];
			if (what == "wait") {
				if (!m_baking) {
					m_console.Refuse("bake wait: no bake is running");
					return;
				}
				const double limit =
					args.size() > 1 ? std::max(1.0, std::atof(args[1].c_str())) : 300.0;
				const auto t0 = std::chrono::steady_clock::now();
				double secs = 0.0;
				bool running = true;
				while ((running = m_bake.Running()) && secs < limit) {
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
					secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
							   .count();
				}
				if (running) {
					m_console.Refuse(std::format("bake wait: '{}' still baking after {:.0f} s",
												 m_bakeReq.name, secs));
					return;
				}
				// A texture import is two runs (maps, then worn meshes): its first
				// exit starts the second, which another `bake wait` waits out.
				const bool more = m_bakeReq.textureSet && m_bakeStep == 0 && !m_restyleBake;
				m_console.Print(std::format("bake: the baker for '{}' exited (code {}) after "
											"{:.1f} s - {} next frame",
											m_bakeReq.name, m_bake.ExitCode(), secs,
											more ? "its second step starts" : "it lands"));
				return;
			}
			if (what != "status") {
				m_console.RefuseUsage();
				return;
			}
			m_console.Print(m_baking ? std::format("bake: running - {} '{}', step {}{}",
												   m_bakeReq.catalogKey, m_bakeReq.name,
												   m_bakeStep, m_restyleBake ? " (a restyle)" : "")
									 : std::string("bake: idle"));
		});
	m_console.Register(
		{.name = "typeset",
		 .group = CmdGroup::Types,
		 .params = "<category> <id> <field> [value...]\n"
				   "rename <category> <id> <new>\n"
				   "delete <category> <id>\n"
				   "dialog [status] | <category> <id> | off | rows [all] | fields | derived | "
				   "tab <n> | stage <n> <id> | stage add | pick <field> <asset> | delete | save",
		 .summary = "the type editor's Save, Rename or Delete (no value removes a field), "
					"or the editor itself step by step"},
		[this](const std::vector<std::string>& args) {
			// The type editor's own paths, reachable without a mouse. Save goes
			// through m_typeDialog.onSave, not WriteTypeFields alone: the Save
			// also APPLIES the change (a surface's materials, a prop's kind, a
			// theme's squares on every level), and that is what a harness
			// needs to see. Rename and Delete are the title's and footer's.
			if (!args.empty() && args[0] == "dialog") {
				TypesetDialog(args);
				return;
			}
			if (args.size() >= 3 && (args[0] == "rename" || args[0] == "delete")) {
				std::string problem;
				const bool rename = args[0] == "rename";
				if (rename && !Need(m_console, args, 4,
									"usage: typeset rename <category> <id> <new>"))
					return;
				const bool ok = rename ? RenameType(args[1], args[2], args[3], problem)
									   : DeleteType(args[1], args[2], problem);
				// The refused half REFUSES now (C442): a rename or delete a
				// script went on to measure the effects of had not happened.
				const std::string line =
					std::format("typeset {} {} '{}': {}{}", args[0], args[1], args[2],
								ok ? "done" : "refused", problem.empty() ? "" : " - " + problem);
				if (ok) m_console.Print(line);
				else m_console.Refuse(line);
				return;
			}
			if (!Need(m_console, args, 3)) return;
			const Catalog* cat = m_project.CatalogForKey(args[0]);
			if (!cat || !cat->Find(args[1])) {
				m_console.Refuse(std::format("typeset: no {} '{}'", args[0], args[1]));
				return;
			}
			TypeEditorDialog::Config cfg;
			cfg.catalogKey = args[0];
			cfg.id = args[1];
			std::string value;
			for (size_t i = 3; i < args.size(); ++i) value += (i > 3 ? " " : "") + args[i];
			serialize::Field field;
			field.key = args[2];
			field.value = value;
			cfg.fields.push_back(std::move(field));
			// The Save's own refusal (a model the category could not load, C301)
			// REFUSES here too: nothing was written.
			if (const std::string refused =
					m_typeDialog.onSave ? m_typeDialog.onSave(cfg) : std::string();
				!refused.empty()) {
				m_console.Refuse(std::format("typeset {} '{}': {} refused - {}", args[0],
											 args[1], args[2], refused));
				return;
			}
			m_console.Print(std::format("typeset {} '{}': {} = {}", args[0], args[1], args[2],
										value.empty() ? "(removed)" : value));
		});
	// The monster type's animation + behaviour dialog (the type editor's extra
	// button), for a harness: the same calls its controls make (code-review
	// C99 - a Caster pick has to leave a spell that Save writes).
	m_console.Register(
		{.name = "monsterdialog",
		 .group = CmdGroup::Types,
		 .params = "[status]\n"
				   "<id>\n"
				   "archetype <name>\n"
				   "save\n"
				   "esc\n"
				   "off",
		 .summary = "open the monster type dialog, pick its archetype, save or cancel it, "
					"or report it"},
		[this](const std::vector<std::string>& args) {
			const auto& kArch = ai::kArchetypeNames; // in enum order
			const std::string verb = args.empty() ? std::string("status") : args[0];
			if (verb == "off") {
				m_monsterDialog.Close();
			} else if (verb == "save" || verb == "esc" || verb == "archetype") {
				if (!m_monsterDialog.IsOpen()) {
					m_console.Refuse("monsterdialog: the dialog is not open");
					return;
				}
				if (verb == "save") {
					m_monsterDialog.ClickSave();
				} else if (verb == "esc") {
					m_monsterDialog.Cancel(); // Esc's own call: the live kind put back
				} else {
					int found = -1;
					for (int i = 0; i < static_cast<int>(std::size(kArch)); ++i)
						if (args.size() >= 2 && args[1] == kArch[i]) found = i;
					if (found < 0) {
						m_console.RefuseUsage();
						return;
					}
					m_monsterDialog.PickArchetype(static_cast<ai::Archetype>(found));
					m_monsterDialog.ApplyPending(); // no Update runs under the console
				}
			} else if (verb != "status") {
				if (!m_project.monsters.Find(verb)) {
					m_console.Refuse(std::format("monsterdialog: no monster '{}'", verb));
					return;
				}
				OpenMonsterConfig(verb);
				if (!m_monsterDialog.IsOpen()) {
					m_console.Refuse(std::format("monsterdialog: '{}' did not open", verb));
					return;
				}
			}
			if (!m_monsterDialog.IsOpen()) {
				m_console.Print("monsterdialog: closed");
				return;
			}
			// The spell the kind OPENED with (what a load gave it) beside the
			// working copy's, which may be the default, and the row's.
			const MonsterConfigDialog::Config& c = m_monsterDialog.Current();
			const int arch = static_cast<int>(c.archetype);
			m_console.Print(std::format(
				"monsterdialog: open {} archetype {} opened '{}' spell '{}' shown '{}'", c.type,
				arch >= 0 && arch < static_cast<int>(std::size(kArch)) ? kArch[arch] : "?",
				m_monsterDialog.Opened().spell, c.spell, m_monsterDialog.ShownSpell()));
		});
	m_console.Register(
		{.name = "typerefs",
		 .group = CmdGroup::Types,
		 .params = "<category> <id>",
		 .summary = "count the level records and other references naming a type"},
		[this](const std::vector<std::string>& args) {
			if (!Need(m_console, args, 2)) return;
			// BOTH HALVES, reported separately, because they answer different
			// questions: levels are where a placement lives, and the catalog +
			// WORLD half is where a doorway or a hook does.
			const DungeonWorld::TypeUsage lv = m_world->SweepTypeRefs(args[0], args[1]);
			const int other = SweepCatalogRefs(args[0], args[1], nullptr);
			m_console.Print(std::format("{} '{}': {} level record(s), {} other "
										"reference(s)",
										args[0], args[1], lv.count, other));
		});
	m_console.Register(
		{.name = "saveworld",
		 .group = CmdGroup::World,
		 .summary = "write world/world.map alone (savemap writes the levels too)"},
		[this](const std::vector<std::string>&) {
			// SEPARATE FROM `savemap` because savemap rewrites every level file
			// as well, and a level writer regenerates headers — so using it to
			// test the WORLD writer quietly stripped the authoring notes off
			// eval_arena. A command that does one thing can be used to check
			// that one thing.
			if (!m_worldMap) {
				m_console.Refuse("no world map loaded");
				return;
			}
			if (SaveWorld()) m_console.Print("saved world");
			else m_console.Refuse("world save failed");
		});
}

// --- the type editor, step by step (`typeset dialog`) -------------------------

void Game::TypesetDialog(const std::vector<std::string>& args) {
	const std::string verb = args.size() >= 2 ? args[1] : std::string();
	// What the form built for each schema row (TypeEditorDialog::BuiltRows),
	// then a count - read off the widget tree, so a row that built nothing
	// says "none" instead of being skipped.
	const auto printRows = [this](const std::string& key) {
		size_t none = 0;
		for (const TypeEditorDialog::BuiltRow& r : m_typeDialog.BuiltRows()) {
			if (r.widgets == "none") ++none;
			m_console.Print(std::format("typeset row {} {} {} {}", key, r.spec->key,
										FieldKindName(r.spec->kind), r.widgets));
		}
		m_console.Print(std::format("typeset rows {} '{}': {} schema row(s), {} built, {} "
									"with nothing",
									key, m_typeDialog.Id(), m_typeDialog.Schema().size(),
									m_typeDialog.BuiltRows().size(), none));
	};
	if (verb == "rows" && args.size() >= 3 && args[2] == "all") {
		// EVERY category with a schema, opened on its first entry the way the
		// palette's right-click opens it: the sweep that shows each kind of row
		// builds a control (code-review C101).
		for (size_t c = 0; c < static_cast<size_t>(MapEditor::PaletteCat::Count); ++c) {
			const auto cat = static_cast<MapEditor::PaletteCat>(c);
			const std::string key = MapEditor::CategoryCatalogKey(cat);
			if (SchemaFor(key).empty()) {
				m_console.Print(std::format("typeset rows {}: no schema", key));
				continue;
			}
			const Catalog* catalog = m_project.CatalogForKey(key);
			if (!catalog || catalog->Entries().empty()) {
				m_console.Refuse(std::format("typeset rows {}: no entry to open", key));
				continue;
			}
			OpenTypeEditor(cat, catalog->Entries().front().id);
			if (!m_typeDialog.IsOpen()) {
				m_console.Refuse(std::format("typeset rows {}: the editor did not open", key));
				continue;
			}
			printRows(key);
			m_typeDialog.Close();
		}
		return;
	}
	const bool open = m_typeDialog.IsOpen();
	const bool needsOpen = verb == "rows" || verb == "fields" || verb == "derived" ||
						   verb == "tab" || verb == "stage" || verb == "pick" ||
						   verb == "delete" || verb == "save";
	if (needsOpen && !open) {
		m_console.Refuse("typeset dialog: no type editor is open");
		return;
	}
	if (verb == "off") {
		m_typeDialog.Close();
	} else if (verb == "rows") {
		printRows(m_typeDialog.CatalogKey());
	} else if (verb == "fields") {
		// The WORKING COPY, which Save writes - not the catalog.
		for (const serialize::Field& f : m_typeDialog.Fields())
			m_console.Print(std::format("typeset field {} = {}", f.key, f.value));
	} else if (verb == "derived") {
		// The value each DERIVED row names - a monster's power, a surface's relief
		// (its texture set's own, Assets/WornSets.h) - as the form shows it.
		for (const FieldSpec& spec : m_typeDialog.Schema())
			if (const std::optional<float> d = m_typeDialog.DerivedValue(spec))
				m_console.Print(std::format("typeset derived {} = {:.4f}", spec.key, *d));
	} else if (verb == "tab" && args.size() >= 3) {
		m_typeDialog.SelectTab(std::atoi(args[2].c_str()));
	} else if (verb == "stage" && args.size() == 3 && args[2] == "add") {
		// The "+ Add a stage" click: a rebuild, which resets every id field.
		if (!m_typeDialog.ClickAddStage()) {
			m_console.Refuse("typeset dialog: no stage rows");
			return;
		}
	} else if (verb == "stage" && args.size() >= 4) {
		// One keystroke batch into stage <n>'s id (1 = the first stage).
		const int n = std::atoi(args[2].c_str());
		if (n < 1 || !m_typeDialog.TypeStageId(static_cast<size_t>(n - 1), args[3])) {
			m_console.Refuse(std::format("typeset dialog: no stage {}", args[2]));
			return;
		}
	} else if (verb == "pick" && args.size() >= 4) {
		// What the asset picker hands back to a texture / model row: the row's
		// value, as a pick writes it (no picker is opened).
		if (!m_typeDialog.PickAsset(args[2], args[3])) {
			m_console.Refuse(std::format("typeset dialog: no texture or model row '{}'", args[2]));
			return;
		}
	} else if (verb == "save") {
		// The footer Save's click. A refusal (a model the category could not
		// load, C301) leaves the form open with the reason in its notice and
		// REFUSES here, since nothing was saved.
		if (!m_typeDialog.ClickSave()) {
			m_typeDialog.ApplyPending();
			m_console.Refuse(std::format("typeset dialog: save refused - {}", m_typeDialog.Notice()));
			return;
		}
	} else if (verb == "delete") {
		// The footer Delete's click - the FIRST of two for every category but a
		// dungeon, so it only arms (and says so in the notice). The notice is
		// another owner's, which a stage id typed after it must leave standing.
		m_typeDialog.ClickDelete();
	} else if (verb.empty() || verb == "status") {
		// Just where it stands (below) - and, run from a script, a frame drawn
		// with the dialog as it is.
	} else if (args.size() >= 3 && !needsOpen) {
		const MapEditor::PaletteCat cat = MapEditor::CatForCatalogKey(args[1]);
		if (cat == MapEditor::PaletteCat::Count) {
			m_console.Refuse(std::format("typeset dialog: no category '{}'", args[1]));
			return;
		}
		OpenTypeEditor(cat, args[2]);
		if (!m_typeDialog.IsOpen()) {
			m_console.Refuse(std::format("typeset dialog: no {} '{}'", args[1], args[2]));
			return;
		}
	} else {
		m_console.RefuseUsage(); // prints the registered forms
		return;
	}
	// A click deferred its rebuild (it fires inside the tree walk, and the
	// dialog's Update does not run while the console is up): apply it, so what
	// is read next is the view the step produced.
	m_typeDialog.ApplyPending();
	m_console.Print(m_typeDialog.IsOpen()
						? std::format("typeset dialog: open {} '{}' - {}", m_typeDialog.CatalogKey(),
									  m_typeDialog.Id(), m_typeDialog.Notice())
						: std::string("typeset dialog: closed"));
}

} // namespace dungeon::game
