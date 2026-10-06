// ============================================================================
// Game/Game_DevCommands.cpp — split out of Game.cpp to keep files small (see Game.h).
// Dev-console command registration: the general commands (quit, quality, save/
// load, the editor's level commands, navigation, the renderer's mood knobs) and
// the typeface audition. The rest live by concern in Game_DevWorld /
// _DevDungeons / _DevDiagnostics / _DevParty / _DevEval, sharing their arg
// helpers through Game/DevCommandArgs.h.
// ============================================================================
#include "Game/Game.h"

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
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace dungeon::game {

using devargs::Need;
using devargs::JoinArgs;
using devargs::ArgOn;

void Game::RegisterDevCommands() {
	// Developer console commands (dev-facing, English). The generic ones
	// (help/clear/echo) live in DevConsole; these reach into the app state.
	m_console.Register({.name = "quit", .group = CmdGroup::Console, .summary = "exit the game"},
					   [this](const std::vector<std::string>&) { m_quitRequested = true; });
	m_console.Register({.name = "exit", .group = CmdGroup::Console, .summary = "exit the game"},
					   [this](const std::vector<std::string>&) { m_quitRequested = true; });
	m_console.Register({.name = "fps",
						.group = CmdGroup::Profiling,
						.summary = "print the current frame rate"},
					   [this](const std::vector<std::string>&) {
						   m_console.Print(std::format("{:.1f} fps", m_console.Fps()));
					   });
	m_console.Register({.name = "quality",
						.group = CmdGroup::Settings,
						.params = "<0-3>",
						.summary = "set the quality tier (low/med/high/ultra)"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   const int q = std::atoi(args[0].c_str());
						   if (q < 0 || q > 3) {
							   m_console.Refuse("quality must be 0-3");
							   return;
						   }
						   m_pendingQuality = static_cast<Quality>(q); // applied next frame
						   m_console.Print(std::format("quality set to {}", q));
					   });
	m_console.Register({.name = "framecap",
						.group = CmdGroup::Settings,
						.params = "[on|off]",
						.summary = "cap the frame rate to the window's monitor"},
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
	m_console.Register({.name = "lang",
						.group = CmdGroup::Settings,
						.params = "<code>",
						.summary = "switch language by code (e.g. en, de); saved unless a script ran it"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   m_pendingLanguage = args[0]; // applied next frame
						   // Typed, it is the Settings dropdown's switch and is
						   // saved; from a SCRIPT it is drawn and never saved
						   // (m_scriptLanguage), so a run cannot leave settings.ini
						   // in its language.
						   m_pendingLanguageScripted = EvalRunning();
						   m_console.Print("language: " + args[0]);
					   });
	m_console.Register({.name = "tp",
						.group = CmdGroup::Party,
						.params = "<x> <z>",
						.summary = "teleport the party to a cell"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 2)) return;
						   const int x = std::atoi(args[0].c_str());
						   const int z = std::atoi(args[1].c_str());
						   if (m_world->GetParty().SetGridPosition(x, z))
							   m_console.Print(std::format("teleported to {},{}", x, z));
						   else
							   m_console.Refuse(
								   std::format("{},{} is not walkable", x, z));
					   });

	// --- save / load ---
	m_console.Register({.name = "save",
						.group = CmdGroup::SaveLoad,
						.params = "[name]",
						.summary = "save the game to a named slot (default quicksave)"},
					   [this](const std::vector<std::string>& args) {
						   if (!m_gameLoaded) {
							   m_console.Refuse("no game loaded");
							   return;
						   }
						   std::string name = JoinArgs(args);
						   if (name.empty()) name = "quicksave";
						   if (SaveGame(name)) m_console.Print("saved: " + name);
						   else m_console.Refuse("not saved (see log)");
					   });
	m_console.Register({.name = "load",
						.group = CmdGroup::SaveLoad,
						.params = "[name]",
						.summary = "load a save by name, or list the saves"},
					   [this](const std::vector<std::string>& args) {
						   if (args.empty()) {
							   // The list the Load page shows (SaveListWorld).
							   const std::vector<SaveSlot> slots = ListSaves(SaveListWorld());
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
						   // A load that failed leaves the world as it was: a
						   // script carrying on would measure that, not the save.
						   if (LoadGame(SaveSlotPath(name)))
							   m_console.Print("loaded: " + name);
						   else
							   m_console.Refuse("load failed (see log)");
					   });
	// The Save / Load page's Delete, by name: the row's own call (GameUI::
	// DeleteSaveSlot), so a delete that fails leaves the page saying so and the
	// reason in the log, and the refusal here is that same sentence.
	m_console.Register({.name = "deletesave",
						.group = CmdGroup::SaveLoad,
						.params = "<name>",
						.summary = "delete a save by name, as its row's Delete does"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   const std::string name = JoinArgs(args);
						   const std::string path = SaveSlotPath(name);
						   if (!std::filesystem::exists(path)) {
							   m_console.Refuse("deletesave: no save named '" + name + "'");
							   return;
						   }
						   if (m_ui.DeleteSaveSlot(path, name))
							   m_console.Print("deleted: " + name);
						   else
							   m_console.Refuse("deletesave: " + m_ui.SaveNotice());
					   });

	// --- diagnostics (read-only) ---
	m_console.Register({.name = "pos",
						.group = CmdGroup::Party,
						.summary = "print party position and facing"},
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
	m_console.Register({.name = "mapinfo",
						.group = CmdGroup::Levels,
						.summary = "print dungeon size and a static-layer fingerprint"},
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
	// One hash per surface; identical before/after an edit = no vertex moved.
	m_console.Register({.name = "geomhash",
						.group = CmdGroup::Levels,
						.summary = "fingerprint the active level's surface geometry"},
					   [this](const std::vector<std::string>&) {
						   const DungeonWorld::GeometryPrint g = m_world->GeometryFingerprint();
						   m_console.Print(std::format(
							   "geomhash {} walls={:016x} floors={:016x} ceilings={:016x} "
							   "verts={}",
							   m_world->CurrentLevel(), g.walls, g.floors, g.ceilings,
							   g.vertices));
						   // Uploaded chunks vs a fresh bake: a chunk an edit forgot
						   // to rebuild makes these two disagree. An undo restore
						   // DEFERS its rebake to editor close on purpose, so a
						   // mismatch then is the debt, not a defect: say which.
						   const char* verdict = g.layout == g.liveLayout ? "match"
												 : m_world->GeometryDirty() ? "deferred"
																			: "STALE";
						   m_console.Print(std::format("geomlayout {} fresh={:016x} live={:016x} {}",
													   m_world->CurrentLevel(), g.layout,
													   g.liveLayout, verdict));
					   });
	m_console.Register({.name = "groups",
						.group = CmdGroup::Monsters,
						.summary = "list monster groups (id: count [kinds] @ cell#slot)"},
					   [this](const std::vector<std::string>&) {
						   for (const std::string& line : m_world->GroupsReport())
							   m_console.Print(line);
					   });
	// Bare opens the editor, `off` the player map. The rest drive what a mouse
	// does, for a harness: `inspect` is a right-click on the square, `place` arms
	// that palette row and left-clicks it, `drag` is the same row dragged over
	// several squares as one stroke, `erase` a middle-click, `fill` the Shift /
	// Ctrl / area / fill-level gestures, `move` a drag with no brush armed.
	m_console.Register({.name = "editor",
						.group = CmdGroup::Levels,
						.params = "[off]\n"
								  "place <category> <id> <x> <z> [north|east|south|west]\n"
								  "drag <category> <id> <x> <z> [<x> <z> ...]\n"
								  "erase <x> <z>\n"
								  "fill <category> <id> rect <x0> <z0> <x1> <z1>\n"
								  "fill <category> <id> flood|area <x> <z>\n"
								  "fill <category> <id> level\n"
								  "inspect <x> <z> [monster|door|button|stair|...]\n"
								  "inspect off\n"
								  "inspector [status|esc|save|delete|tab <n>]\n"
								  "inspector editroute|clearroute|archetype <name>\n"
								  "inspector open on|off\n"
								  "inspector popup [<n>]\n"
								  "levelsettings [status|open|popup [<n>]]\n"
								  "levelsettings dust|haze|ambient <value>\n"
								  "newasset [status|<category>|popup [<n>]]\n"
								  "levellist [status|open]\n"
								  "arm <category> <id>\n"
								  "route [status|<x> <z>]\n"
								  "route key enter|esc|back\n"
								  "pick <x> <z>\n"
								  "cell <x> <z>\n"
								  "records <x> <z>\n"
								  "move <x> <z> <to-x> <to-z>\n"
								  "resize <x0> <z0> <x1> <z1>\n"
								  "tool [paint|rect|flood|area|pick|corridor|room|stamp|region]\n"
								  "shape corridor|room|region <ax> <az> <bx> <bz>\n"
								  "shape stamp <id> <x> <z> [turns]\n"
								  "shape seed <n>\n"
								  "palette [mode stage|kind]\n"
								  "palette group <name>|filter [text]|groups\n"
								  "palette items <catalog>\n"
								  "palette use <id> [link]\n"
								  "palette expand|collapse|swatches\n"
								  "dock [left|right <px>]\n"
								  "overview [world|dungeon|level]\n"
								  "overview follow <key> [world|dungeon|level]\n"
								  "disarm\n"
								  "pause [on|off]\n"
								  "view\n"
								  "issues\n"
								  "rev",
						.summary = "open the map editor, or drive its brushes and gestures"},
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty() && args[0] == "off") {
							   m_mapView.SetMode(MapView::Mode::Player);
							   m_console.Print("map: player mode");
							   return;
						   }
						   // The brush and the erase ladder, for a harness: the SAME
						   // MapEditor calls a click makes, so what a play-tester's
						   // click would do (a refusal, a dialog) happens here too.
						   // The editor's report lands on its message line and in
						   // dungeon.log ("editor: ..."), which is what to check.
						   // The drag (MapEditor::BeginMove / EndMove - what a left
						   // press and release do with no brush armed), and putting
						   // the brush down.
						   if (!args.empty() && args[0] == "disarm") {
							   m_console.Print(m_mapEditor.Disarm() ? "editor: brush put down"
																	: "editor: no brush armed");
							   return;
						   }
						   // The toolbar's pause/play button, pressed (`on` / `off`),
						   // or bare to say where it stands. The editor's own: with
						   // the editor not up there is no button to press. A bare
						   // `editor` or any subcommand after it must leave it as it
						   // is (code-review C78).
						   if (!args.empty() && args[0] == "pause") {
							   const bool editing = m_mapView.IsOpen() &&
													m_mapView.CurrentMode() == MapView::Mode::Editor;
							   if (args.size() >= 2) {
								   if (args[1] != "on" && args[1] != "off") {
									   m_console.RefuseUsage();
									   return;
								   }
								   if (!editing) {
									   m_console.Refuse("editor pause: the editor is not open");
									   return;
								   }
								   m_mapView.SetEditorPaused(args[1] == "on");
							   }
							   m_console.Print(std::format(
								   "editor pause: {}", !editing				   ? "no editor"
													   : m_mapView.EditorPaused() ? "paused"
																				  : "running"));
							   return;
						   }
						   // The edge drag: the new window in the viewed level's
						   // current squares (x1/z1 exclusive), as one undo step.
						   if (!args.empty() && args[0] == "view") {
							   const gfx::Rect panel =
								   MapPanel(static_cast<float>(m_window.Width()),
											static_cast<float>(m_window.Height()));
							   const gfx::Rect r = m_mapView.MapRect(panel);
							   const DungeonMap& vm = m_mapView.ViewedMap();
							   m_console.Print(std::format(
								   "editor view: {} {}x{} map {:.0f},{:.0f} {:.0f}x{:.0f} band {:.0f}",
								   m_mapView.ViewedLevel(), vm.Width(), vm.Height(), r.x, r.y,
								   r.w, r.h, m_mapView.HandleBand(panel)));
							   return;
						   }
						   if (!args.empty() && args[0] == "resize") {
							   if (!Need(m_console, args, 5,
										 "usage: editor resize <x0> <z0> <x1> <z1>"))
								   return;
							   const std::string stem = m_mapView.ViewedLevel();
							   m_world->BeginUndoStep();
							   const bool ok = m_world->ResizeLevel(
								   stem, std::atoi(args[1].c_str()), std::atoi(args[2].c_str()),
								   std::atoi(args[3].c_str()), std::atoi(args[4].c_str()));
							   m_world->CommitUndoStep(ok);
							   // As the edge drag does (MapView.cpp): a BROWSED level is
							   // drawn from its snapshot, which would still show the old
							   // size.
							   m_mapView.RefreshBrowse();
							   if (ok) m_console.Print(std::format("editor resize: {} done", stem));
							   else m_console.Refuse(std::format("editor resize: {} refused", stem));
							   return;
						   }
						   if (!args.empty() && args[0] == "move") {
							   if (!Need(m_console, args, 5,
										 "usage: editor move <x> <z> <to x> <to z>"))
								   return;
							   if (m_mapView.IsOpen())
								   m_mapView.SetMode(MapView::Mode::Editor);
							   else
								   m_mapView.Open(MapView::Mode::Editor);
							   const int fx = std::atoi(args[1].c_str());
							   const int fz = std::atoi(args[2].c_str());
							   // THE TRUTH, not the request (C442): this said "moved"
							   // whether or not anything was there to take or could
							   // land where it was sent. The editor's own reason is
							   // on its message line ("editor: ..." in the log).
							   if (!m_mapEditor.BeginMove(fx, fz)) {
								   m_console.Refuse(std::format(
									   "editor move: nothing to take at {},{}", fx, fz));
								   return;
							   }
							   if (!m_mapEditor.EndMove(std::atoi(args[3].c_str()),
														std::atoi(args[4].c_str()))) {
								   m_console.Refuse(std::format("editor move: {},{} did not go "
																"to {},{}",
																fx, fz, args[3], args[4]));
								   return;
							   }
							   m_console.Print(std::format("editor move: {},{} -> {},{}", fx, fz,
														   args[3], args[4]));
							   return;
						   }
						   // The edit counter live validation keys on: moves on a
						   // change, stays put on a no-op (DungeonWorld::EditRevision).
						   // The tool strip: pick a tool by name, or bare to say which.
						   if (!args.empty() && args[0] == "tool") {
							   using Tool = MapEditor::Tool;
							   if (args.size() >= 2) {
								   int found = -1;
								   for (int i = 0; i < static_cast<int>(Tool::Count); ++i)
									   if (args[1] == MapEditor::ToolName(static_cast<Tool>(i)))
										   found = i;
								   if (found < 0) {
									   m_console.Refuse("usage: editor tool [paint|rect|flood|area|pick|"
														"corridor|room|stamp|region]");
									   return;
								   }
								   m_mapEditor.SetTool(static_cast<Tool>(found));
							   }
							   m_console.Print(std::format(
								   "editor tool: {}", MapEditor::ToolName(m_mapEditor.ActiveTool())));
							   return;
						   }
						   // THE SHAPE BRUSHES without a mouse (Phase 6): the same
						   // shape the drag previews, committed the way its release
						   // commits it, on the viewed level in the current style.
						   //   editor shape corridor|room|region <ax> <az> <bx> <bz>
						   //   editor shape stamp <id> <x> <z> [turns]
						   //   editor shape seed <n>      (the next winding / region)
						   if (!args.empty() && args[0] == "shape") {
							   if (args.size() >= 3 && args[1] == "seed") {
								   m_mapEditor.SetShapeSeed(
									   static_cast<u32>(std::strtoul(args[2].c_str(), nullptr, 10)));
								   m_console.Print(std::format("editor shape seed {}", args[2]));
								   return;
							   }
							   if (m_mapView.IsOpen())
								   m_mapView.SetMode(MapView::Mode::Editor);
							   else
								   m_mapView.Open(MapView::Mode::Editor);
							   const auto n = [&](size_t i) { return std::atoi(args[i].c_str()); };
							   carve::Shape shape;
							   const std::string kind = args.size() >= 2 ? args[1] : std::string();
							   if ((kind == "corridor" || kind == "room" || kind == "region") &&
								   args.size() >= 6) {
								   shape = kind == "corridor" ? m_mapEditor.CorridorShape(n(2), n(3), n(4), n(5))
										   : kind == "room"	 ? m_mapEditor.RoomShape(n(2), n(3), n(4), n(5))
															 : m_mapEditor.RegionShape(n(2), n(3), n(4), n(5));
							   } else if (kind == "stamp" && args.size() >= 5) {
								   if (!m_project.shapes.Contains(args[2])) {
									   m_console.Refuse(std::format("editor shape: no shape '{}'", args[2]));
									   return;
								   }
								   m_mapEditor.SetCurrentStamp(args[2]);
								   while (args.size() >= 6 &&
										  m_mapEditor.StampTurns() != (n(5) % 4 + 4) % 4)
									   m_mapEditor.TurnStamp();
								   shape = m_mapEditor.StampShape(n(3), n(4));
							   } else {
								   m_console.Refuse("usage: editor shape corridor|room|region <ax> <az> "
													"<bx> <bz> | stamp <id> <x> <z> [turns] | seed <n>");
								   return;
							   }
							   const MapEditor::ShapeResult r = m_mapEditor.ApplyShape(shape);
							   m_mapView.RefreshBrowse();
							   m_console.Print(std::format(
								   "editor shape {}: squares={} solid={} opened={} raised={} painted={}",
								   kind, shape.open.size(), shape.solid.size(), r.opened, r.raised,
								   r.painted));
							   return;
						   }
						   // What the live check boxes on the VIEWED level, one line a
						   // square (a stair's far end and every lost item included),
						   // then the Check badge's count. Refreshes first - the
						   // harness has no mouse, so nothing is ever mid-stroke.
						   if (!args.empty() && args[0] == "issues") {
							   if (m_mapView.IsOpen())
								   m_mapView.SetMode(MapView::Mode::Editor);
							   else
								   m_mapView.Open(MapView::Mode::Editor);
							   RefreshLiveIssues(/*pointerHeld*/ false);
							   const std::string& here = m_mapView.ViewedLevel();
							   int badge = 0;
							   for (const validate::Issue& is : m_liveIssues) {
								   const char* sev =
									   is.severity == validate::Severity::Error ? "error" : "warning";
								   if (is.x < 0) ++badge;
								   if (is.level == here && is.x >= 0)
									   m_console.Print(std::format("editor box {} {},{} {} {}", here,
																   is.x, is.z, sev, is.messageKey));
								   for (const validate::Spot& s : is.also)
									   if (s.level == here)
										   m_console.Print(std::format("editor box {} {},{} {} {} (from {} {},{})",
																	   here, s.x, s.z, sev,
																	   is.messageKey, is.level,
																	   is.x, is.z));
							   }
							   m_console.Print(std::format("editor issues: {} finding(s), badge {}",
														   m_liveIssues.size(), badge));
							   return;
						   }
						   // One square of the VIEWED level: what each surface stores
						   // (a pinned palette index, a theme, or the default
						   // hash) and the texture that resolves to - what the 3D
						   // scene and the map both draw.
						   if (!args.empty() && args[0] == "cell") {
							   if (!Need(m_console, args, 3, "usage: editor cell <x> <z>")) return;
							   const DungeonMap& map = m_mapView.ViewedMap();
							   const int x = std::atoi(args[1].c_str()), z = std::atoi(args[2].c_str());
							   std::string line = std::format("editor cell {} {},{} {}", m_mapView.ViewedLevel(),
															  x, z, map.IsWalkable(x, z) ? "open" : "solid");
							   static constexpr const char* kName[3] = {"wall", "floor", "ceiling"};
							   for (int s = 0; s < 3; ++s) {
								   const Surface sf = static_cast<Surface>(s);
								   const int v = map.Variant(sf, x, z);
								   const int slot = DungeonMap::ThemeSlotOf(v);
								   const std::string stored = v >= 0 ? std::format("pin{}", v)
															  : slot >= 0 ? "theme:" + map.ThemeId(slot)
																		  : std::string("hash");
								   const std::vector<std::string>& pal = map.Palette(sf);
								   const u32 i = ResolveSurfaceVariant(map, x, z, sf,
																	   static_cast<u32>(pal.size()));
								   line += std::format(" {}={}/{}", kName[s], stored,
													   i < pal.size() ? pal[i] : "-");
							   }
							   m_console.Print(line);
							   return;
						   }
						   // The .ent RECORDS on one square of the VIEWED level, as it
						   // reads (nothing stashed), each with its id and whether the
						   // level's held state names that id (`held=1`): what tells a
						   // placement's fresh id from an erased record's, and an
						   // undone erase from one that lost its diff (C327).
						   if (!args.empty() && args[0] == "records") {
							   if (!Need(m_console, args, 3, "usage: editor records <x> <z>")) return;
							   const std::string stem = m_mapView.ViewedLevel();
							   const int x = std::atoi(args[1].c_str()), z = std::atoi(args[2].c_str());
							   const auto recs = m_world->RecordsAt(stem, x, z);
							   if (recs.empty())
								   m_console.Print(std::format("editor records {} {},{}: none", stem, x, z));
							   for (const DungeonWorld::RecordReport& r : recs)
								   m_console.Print(std::format("editor records {} {},{}: {} {} id={} held={}",
															   stem, x, z, r.kind, r.type, r.id,
															   r.held ? 1 : 0));
							   return;
						   }
						   // The eyedropper (Alt+click) on a square: says what it armed.
						   if (!args.empty() && args[0] == "pick") {
							   if (!Need(m_console, args, 3, "usage: editor pick <x> <z>")) return;
							   if (m_mapView.IsOpen())
								   m_mapView.SetMode(MapView::Mode::Editor);
							   else
								   m_mapView.Open(MapView::Mode::Editor);
							   m_mapEditor.PickAt(std::atoi(args[1].c_str()), std::atoi(args[2].c_str()));
							   const MapEditor::PaletteCat c = m_mapEditor.ArmedCat();
							   m_console.Print(std::format(
								   "editor pick: {} {}",
								   c == MapEditor::PaletteCat::Count ? "-" : MapEditor::CategoryCatalogKey(c),
								   m_mapEditor.ArmedId()));
							   return;
						   }
						   // The palette's category bar, as the bar and the filter
						   // box would drive it: bare = what shows now; `mode
						   // stage|kind`, `group <name>`, `filter [text]` change it
						   // first; `groups` prints both tables.
						   if (!args.empty() && args[0] == "palette") {
							   PrintPalette(args);
							   return;
						   }
						   // The docks and the overview (MapView_Docks.cpp):
						   // `dock [left|right <px>]` sets a width as a drag
						   // would, then prints the layout everything else is
						   // measured from; `overview [world|dungeon|level]`
						   // prints the panel's lines for that scope.
						   if (!args.empty() && (args[0] == "dock" || args[0] == "overview")) {
							   PrintDocks(args);
							   return;
						   }
						   if (!args.empty() && args[0] == "rev") {
							   m_console.Print(
								   std::format("editor rev {}", m_world->EditRevision()));
							   return;
						   }
						   // The modifier gestures, for a harness: `rect` is a click on
						   // the first corner then a Shift+click on the second (the
						   // rectangle anchors on the last painted square), `flood` a
						   // Ctrl+click. Timed, since batching the chunk rebuilds is
						   // what made a big fill cheap.
						   if (!args.empty() && args[0] == "fill") {
							   const std::string how = args.size() >= 4 ? args[3] : "";
							   const bool rect = how == "rect", flood = how == "flood",
										  areaFill = how == "area", level = how == "level";
							   const size_t need = rect ? 8u : level ? 4u : 6u;
							   if ((!rect && !flood && !areaFill && !level) || args.size() < need) {
								   m_console.Refuse("usage: editor fill <category> <id> rect <x0> "
													"<z0> <x1> <z1> | flood <x> <z> | area <x> <z> "
													"| level");
								   return;
							   }
							   if (m_mapView.IsOpen())
								   m_mapView.SetMode(MapView::Mode::Editor);
							   else
								   m_mapView.Open(MapView::Mode::Editor);
							   const MapEditor::PaletteCat cat =
								   MapEditor::CatForCatalogKey(args[1]);
							   if (cat == MapEditor::PaletteCat::Count ||
								   !m_mapEditor.Arm(cat, args[2])) {
								   m_console.Refuse(std::format(
									   "editor fill: no palette row '{}' in '{}'", args[2],
									   args[1]));
								   return;
							   }
							   const auto num = [&](size_t i) { return std::atoi(args[i].c_str()); };
							   const auto t0 = std::chrono::steady_clock::now();
							   if (rect) {
								   m_mapEditor.Paint(num(4), num(5), /*dragging*/ false);
								   m_mapEditor.PaintRect(num(6), num(7));
							   } else if (flood) {
								   m_mapEditor.FloodFill(num(4), num(5));
							   } else if (areaFill) {
								   m_mapEditor.AreaFill(num(4), num(5));
							   } else {
								   m_mapEditor.FillLevel();
							   }
							   const double ms = std::chrono::duration<double, std::milli>(
													 std::chrono::steady_clock::now() - t0)
													 .count();
							   m_console.Print(std::format("editor fill: {} {} in {:.1f} ms",
														   args[3], args[2], ms));
							   return;
						   }
						   // A left DRAG, for a harness: arm the row, then the press
						   // on the first square and the held drag over the rest,
						   // inside one stroke - exactly what MapView does between a
						   // press and its release, so the stroke's undo step is the
						   // one a mouse drag gets.
						   if (!args.empty() && args[0] == "drag") {
							   if (args.size() < 5 || (args.size() - 3) % 2 != 0) {
								   m_console.Refuse("usage: editor drag <category> <id> <x> <z> "
													"[<x> <z> ...]");
								   return;
							   }
							   if (m_mapView.IsOpen())
								   m_mapView.SetMode(MapView::Mode::Editor);
							   else
								   m_mapView.Open(MapView::Mode::Editor);
							   const MapEditor::PaletteCat cat =
								   MapEditor::CatForCatalogKey(args[1]);
							   if (cat == MapEditor::PaletteCat::Count ||
								   !m_mapEditor.Arm(cat, args[2])) {
								   m_console.Refuse(std::format(
									   "editor drag: no palette row '{}' in '{}'", args[2],
									   args[1]));
								   return;
							   }
							   m_mapEditor.BeginStroke();
							   for (size_t i = 3; i + 1 < args.size(); i += 2)
								   m_mapEditor.Paint(std::atoi(args[i].c_str()),
													 std::atoi(args[i + 1].c_str()),
													 /*dragging*/ i > 3);
							   m_mapEditor.EndStroke();
							   m_console.Print(std::format("editor drag: {} over {} squares",
														   args[2], (args.size() - 3) / 2));
							   return;
						   }
						   if (!args.empty() && (args[0] == "place" || args[0] == "erase")) {
							   const bool place = args[0] == "place";
							   if (!Need(m_console, args, place ? 5 : 3,
										 "usage: editor place <category> <id> <x> <z> | "
										 "erase <x> <z>"))
								   return;
							   if (m_mapView.IsOpen())
								   m_mapView.SetMode(MapView::Mode::Editor);
							   else
								   m_mapView.Open(MapView::Mode::Editor);
							   const size_t at = place ? 3 : 1;
							   const int x = std::atoi(args[at].c_str());
							   const int z = std::atoi(args[at + 1].c_str());
							   if (!place) {
								   // The truth (C442): a square with nothing on it and
								   // no surface override to reset erased nothing.
								   const bool erased = m_mapEditor.EraseAt(x, z);
								   // A browsed level's snapshot is rebuilt, as the
								   // middle-click erase does (MapView::Update): the map
								   // draws from it and the next place's default face
								   // reads it, so a stale one kept the erased thing.
								   m_mapView.RefreshBrowse();
								   if (erased)
									   m_console.Print(std::format("editor erase: {},{}", x, z));
								   else
									   m_console.Refuse(std::format(
										   "editor erase: nothing to erase at {},{}", x, z));
								   return;
							   }
							   const MapEditor::PaletteCat cat =
								   MapEditor::CatForCatalogKey(args[1]);
							   if (cat == MapEditor::PaletteCat::Count ||
								   !m_mapEditor.Arm(cat, args[2])) {
								   m_console.Refuse(std::format(
									   "editor place: no palette row '{}' in '{}'", args[2],
									   args[1]));
								   return;
							   }
							   // THE WALL a wall-mounted kind hangs on. A mouse names it by
							   // pointing; a script names it here, or gets the VIEWED
							   // level's first free face (N, E, S, W - the rule a map load
							   // uses for a 'T' glyph; MapEditor::DefaultWallFace). With no
							   // face at all a sconce brush refused every time, and the
							   // command still said it had placed one; with the active
							   // map's first solid face (C449) it refused on a browsed
							   // level, or where that face was already taken.
							   WallFace face;
							   Direction wall = Direction::North;
							   if (!m_mapEditor.BrushIsWallMounted()) {
								   // a floor kind takes no face
							   } else if (args.size() > 5 && ParseDirection(args[5], wall)) {
								   face = {x, z, wall, true};
							   } else {
								   m_mapEditor.DefaultWallFace(x, z, face); // none: refused below
							   }
							   const u64 rev0 = m_world->EditRevision();
							   m_mapEditor.Paint(x, z, /*dragging*/ false, face);
							   // A browsed level is drawn - and its next default face
							   // read - from a snapshot, rebuilt after each paint as a
							   // brush's own click does (MapView::Update).
							   m_mapView.RefreshBrowse();
							   // A PLACEMENT that changed nothing placed nothing - say so as
							   // a refusal, so a script that meant to stage something fails
							   // instead of measuring an empty square. A surface brush is
							   // exempt: repainting a square its own texture is a no-op that
							   // the editrev suite checks on purpose.
							   const bool surface = cat == MapEditor::PaletteCat::Walls ||
													cat == MapEditor::PaletteCat::Floors ||
													cat == MapEditor::PaletteCat::Ceilings ||
													cat == MapEditor::PaletteCat::Themes;
							   if (!surface && m_world->EditRevision() == rev0) {
								   m_console.Refuse(std::format(
									   "editor place: no {} placed at {},{} (the editor's "
									   "reason is in the message log)",
									   args[2], x, z));
								   return;
							   }
							   // The face it hung on, when it took one: a script reads where
							   // the default put it.
							   m_console.Print(std::format("editor place: {} at {},{}{}", args[2], x, z,
														   face.valid ? std::string(" on ") + DirToken(face.wall)
																	  : std::string()));
							   return;
						   }
						   // The open inspector's controls and keys, and the patrol
						   // route's grid clicks and keys (Game_Inspect.cpp): each
						   // the call its control makes, then where it stands.
						   if (!args.empty() && args[0] == "inspector") {
							   InspectorCommand(args);
							   return;
						   }
						   if (!args.empty() && args[0] == "route") {
							   RouteCommand(args);
							   return;
						   }
						   // The editor's other dialogs, its level drop-down and a
						   // palette row's arm, for the Esc ladders' check (C81).
						   if (!args.empty() &&
							   (args[0] == "levelsettings" || args[0] == "newasset" ||
								args[0] == "levellist" || args[0] == "arm")) {
							   EditorDialogCommand(args);
							   return;
						   }
						   // The right-click, for a harness: select the square and
						   // open its inspector (or the chooser), then say which.
						   if (!args.empty() && args[0] == "inspect") {
							   if (args.size() >= 2 && args[1] == "off") {
								   CloseInspectors();
								   m_console.Print("editor inspect: closed");
								   return;
							   }
							   if (!Need(m_console, args, 3, "usage: editor inspect <x> <z> | off"))
								   return;
							   if (m_mapView.IsOpen())
								   m_mapView.SetMode(MapView::Mode::Editor);
							   else
								   m_mapView.Open(MapView::Mode::Editor);
							   // A real right-click cannot land while a dialog is up,
							   // so none is: a leftover would read as this square's.
							   for (InstanceInspector* ii : InstanceInspectors()) ii->Close();
							   m_projectileInspector.Close();
							   m_inspectPicker.Close();
							   m_mapEditor.InspectAt(std::atoi(args[1].c_str()),
													 std::atoi(args[2].c_str()));
							   // A square holding several things opens the chooser;
							   // a kind named picks its row (a door with a monster
							   // standing in its doorway, C356).
							   if (args.size() >= 4 && m_inspectPicker.IsOpen() &&
								   !PickInspectTarget(args[3])) {
								   m_console.Refuse(std::format(
									   "editor inspect: no {} to pick at {},{}", args[3], args[1],
									   args[2]));
								   return;
							   }
							   const InstanceInspector* open = ActiveInstanceInspector();
							   const std::string what = std::format(
								   "editor inspect: {}", open == &m_stairInspector ? "stair"
													 : open						 ? "inspector"
													 : m_projectileInspector.IsOpen() ? "projectile"
													 : m_inspectPicker.IsOpen() ? "chooser"
																				 : "nothing");
							   m_console.Print(what);
							   // The projectile card's rows, as its labels show them
							   // (code-review C107: their units are the language's).
							   if (m_projectileInspector.IsOpen())
								   for (const auto& [label, value] : m_projectileInspector.Rows())
									   m_console.Print(std::format("editor projectile: {} = {}",
																   label, value));
							   // Logged too: InGameTest's sweep types this into a
							   // console the log does not echo, and a sweep of a
							   // dialog that never opened must not read as clean.
							   log::Info("{} ({}, {} on {})", what, args[1], args[2],
										 m_world->CurrentLevel());
							   return;
						   }
						   // Bare opens the editor. A verb nobody wrote used to land
						   // here too and say "map: editor mode" - a typo'd gesture
						   // reported as a success (C442).
						   if (!args.empty()) {
							   m_console.RefuseUsage();
							   return;
						   }
						   if (m_mapView.IsOpen())
							   m_mapView.SetMode(MapView::Mode::Editor);
						   else
							   m_mapView.Open(MapView::Mode::Editor);
						   m_console.Print("map: editor mode");
					   });
	m_console.Register({.name = "goto",
						.group = CmdGroup::Levels,
						.params = "<level-stem>",
						.summary = "load another level by stem (e.g. crypt2)"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
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
							   m_console.Refuse("unknown level: " + stem);
							   return;
						   }
						   // Arrive at the level's start cell (-1 = resolve after load),
						   // facing as any way in does.
						   BeginLevelTransition(stem, -1, -1, std::nullopt);
						   m_console.Print("loading " + stem + "...");
					   });
	// THE RANKING DIFFICULTY PICKS BY (docs/level-building.md P4), readable
	// before anything is tuned against it. With tags, the pool exactly as the
	// generator draws it for those tags; without, every kind. Machine-readable
	// lines (`threat <id> <threat> ...`) so a harness can join them to a level's
	// monsters.
	m_console.Register(
		{.name = "threat",
		 .group = CmdGroup::Monsters,
		 .params = "[tag ...]",
		 .summary = "rank monster kinds by power - derived threat or the override (a tag's pool, or all)"},
		[this](const std::vector<std::string>& args) {
			generate::Params p;
			FillPools(p, args);
			std::vector<size_t> order(p.monsterIds.size());
			for (size_t i = 0; i < order.size(); ++i) order[i] = i;
			std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
				return p.monsterThreat[a] < p.monsterThreat[b];
			});
			for (const size_t i : order) {
				const CatalogEntry& e = *m_project.monsters.Find(p.monsterIds[i]);
				const threat::Parts t = ThreatOf(e);
				// melee= and shot= are per second BEFORE the ranged edge; offence=
				// is the better of the two with the edge applied, so a shot's
				// weight in the ranking reads straight off the line. power= is
				// what the pool RANKS by (the threat, or the authored override,
				// marked); band= the palette's pips. Both go at the END: the
				// harnesses match the line's head (LevelBuildTest reads
				// "threat <id> <n> offence=...").
				const bool authored = e.GetFloat("power", 0.0f) > 0.0f;
				m_console.Print(std::format(
					"threat {} {:.2f} offence={:.2f} melee={:.2f} shot={:.2f} "
					"toughness={:.1f} hit={:.2f} behit={:.2f} power={:.2f}{} band={}",
					p.monsterIds[i], t.threat, t.offence, t.melee, t.shot, t.toughness,
					t.hit, t.beHit, p.monsterThreat[i], authored ? "(set)" : "",
					m_world->MonsterBand(e.id)));
			}
			m_console.Print(std::format("threat: {} kind(s){}", order.size(),
										args.empty() ? "" : " in that tag's pool"));
		});
	// A new floor of the viewed dungeon by default, and the view jumps to it;
	// `again` rerolls the VIEWED level in place, as the dialog's Regenerate does;
	// `populate` keeps its shape and rerolls its monsters and loot (tool-
	// refinement Phase 7). Knobs as the dialog names them, e.g. path:8 seed:7 -
	// unset ones keep the dialog's, except the style (none unless named).
	m_console.Register({.name = "generate",
						.group = CmdGroup::Levels,
						.params = "[dungeon] [<knob>:<value> ...]\n"
								  "again [<knob>:<value> ...]\n"
								  "populate [<knob>:<value> ...]\n"
								  "dialog [new|off]\n"
								  "dialog tab <n>\n"
								  "dialog create|empty|populate\n"
								  "dialog style <id|->\n"
								  "dialog popup [<n>]\n"
								  "preset [list]\n"
								  "preset save|load|delete <name>\n"
								  "play [stem]",
						.summary = "rough out a new level, reroll or populate the viewed one"},
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
							   // REFUSED, never quietly something else: the UI sweep
							   // (tools\InGameTest.ps1) audits whatever is on screen after
							   // these, so a tab the form lacks, a verb nobody wrote or a
							   // style this world does not have used to leave the dialog
							   // as it was - or open REGENERATE for a typo - and the sweep
							   // passed over the wrong screen (code-review C427).
							   const std::string mode = args.size() > 1 ? args[1] : "";
							   // A press on its n-th shown drop-down (opened by the
							   // next Update), or bare, whether one is open - what
							   // its Esc asks first (C81). Never opens the dialog.
							   if (mode == "popup") {
								   if (args.size() > 2 && m_generateDialog.IsOpen()) {
									   m_generateDialog.OpenPopup(std::atoi(args[2].c_str()));
									   m_console.Print("generate dialog popup: pressing #" + args[2]);
								   } else {
									   m_console.Print(std::format(
										   "generate dialog popup: dialog {} popup {}",
										   m_generateDialog.IsOpen() ? "open" : "closed",
										   m_generateDialog.PopupOpen() ? "open" : "shut"));
								   }
								   return;
							   }
							   if (mode == "off") {
								   m_generateDialog.Close();
							   } else if (mode == "tab") {
								   if (!Need(m_console, args, 3)) return;
								   if (!m_generateDialog.IsOpen() ||
									   !m_generateDialog.ShowTab(std::atoi(args[2].c_str()))) {
									   m_console.Refuse(std::format(
										   "generate dialog: no tab {} to show (the dialog is {}, "
										   "tabs 0..{})",
										   args[2], m_generateDialog.IsOpen() ? "open" : "closed",
										   GenerateDialog::TabCount() - 1));
									   return;
								   }
							   } else if (mode == "new") {
								   if (!m_mapView.onNewLevel) {
									   m_console.Refuse("generate dialog: no [+] to open it from");
									   return;
								   }
								   m_mapView.onNewLevel(m_mapView.ViewedDungeon());
							   } else if (mode == "create" || mode == "empty") {
								   // Phase 7: its buttons and its style dropdown, as clicks.
								   const std::string stem = m_generateDialog.PressCreate(mode == "create");
								   m_console.Print("generate dialog: made " + (stem.empty() ? "-" : stem));
							   } else if (mode == "populate") {
								   m_generateDialog.PressPopulate();
							   } else if (mode == "style") {
								   if (!Need(m_console, args, 3)) return;
								   const std::string id = args[2] == "-" ? std::string() : args[2];
								   if (!id.empty() && !m_project.styles.Contains(id)) {
									   m_console.Refuse("generate dialog: this world has no style '" + id + "'");
									   return;
								   }
								   m_generateDialog.PickChoice("style", id);
							   } else if (!mode.empty()) {
								   m_console.RefuseUsage();
								   return;
							   } else if (m_mapView.onGenerate) {
								   m_mapView.onGenerate();
							   }
							   // The STATE after the verb, not a claim about it - and the
							   // tab is part of it: a sweep of tab 2 that is showing tab 0
							   // is clean for the wrong reason.
							   const std::string& style = m_generateDialog.Knobs().style;
							   m_console.Print(std::format(
								   "generate dialog: {} tab {} style={} knobs {}",
								   !m_generateDialog.IsOpen() ? "closed"
								   : m_generateDialog.GetMode() ==
											   GenerateDialog::Mode::Create
									   ? "create"
									   : "regenerate",
								   m_generateDialog.ActiveTab(), style.empty() ? "-" : style,
								   generate::Encode(m_generateDialog.Knobs())));
							   return;
						   }
						   // PLAY (P5): the dialog's Play buttons, without a mouse.
						   if (!args.empty() && args[0] == "play") {
							   const std::string stem =
								   args.size() > 1 ? args[1] : m_mapView.ViewedLevel();
							   if (PlayLevel(stem)) m_console.Print("generate: playing " + stem);
							   else m_console.Refuse("generate: cannot play " + stem);
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
								   if (id.empty()) m_console.Refuse("preset: not saved");
								   else m_console.Print("preset: saved " + id);
							   } else if (op == "load") {
								   generate::Params p = m_generateDialog.Knobs();
								   if (LoadGenPreset(name, p)) {
									   m_generateDialog.SetKnobs(p);
									   m_console.Print("preset: loaded " + name + " (" +
													   generate::Encode(p) + ")");
								   } else {
									   m_console.Refuse("preset: no such preset " + name);
								   }
							   } else if (op == "delete") {
								   if (DeleteGenPreset(name)) m_console.Print("preset: deleted " + name);
								   else m_console.Refuse("preset: no such preset " + name);
							   } else if (op != "list") {
								   m_console.RefuseUsage(); // a typo'd verb used to list
								   return;
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
						   // NO STYLE unless the line names one (`style:<id>`): the
						   // dialog's last one rides settings.ini, and a script
						   // must build the same floor whatever was clicked last.
						   p.style.clear();
						   std::string line, dungeon = m_mapView.ViewedDungeon();
						   bool again = false, populate = false;
						   for (const std::string& a : args)
							   if (a == "again")
								   again = true;
							   else if (a == "populate")
								   populate = true;
							   else if (a.find(':') == std::string::npos)
								   dungeon = a; // a bare word names the dungeon
							   else
								   line += a + ' ';
						   generate::Decode(line, p);
						   if (!p.style.empty() && !m_project.styles.Contains(p.style)) {
							   m_console.Refuse("generate: this world has no style '" + p.style + "'");
							   return;
						   }
						   // POPULATE ONLY (Phase 7): the viewed level's shape kept,
						   // its monsters and loot rerolled from these knobs.
						   if (populate) {
							   const int placed = PopulateViewedLevel(p);
							   if (placed < 0) {
								   m_console.Refuse("generate: populate failed");
								   return;
							   }
							   m_console.Print(std::format("generate: populated {} ({})",
														   m_mapView.ViewedLevel(), generate::Encode(p)));
							   m_console.Print("generate: built " + GenReportText());
							   return;
						   }
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
							   m_console.Refuse("generate: failed");
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
	m_console.Register({.name = "validate",
						.group = CmdGroup::Levels,
						.summary = "check the whole project for playability faults"},
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
		{.name = "undo",
		 .group = CmdGroup::Levels,
		 .summary = "undo one editor step (the toolbar's < / Ctrl+Z)"},
		[this](const std::vector<std::string>&) {
			// The editor's history, reachable without a keyboard shortcut. It
			// is ONE history across the tiers now (a world paint and a level
			// paint land in the same stack), so this takes back whichever came
			// last — which is the behaviour worth being able to test.
			// Nothing to take back is a REFUSE: a script that meant to undo an
			// edit and found none is about to measure the edit it meant gone.
			if (!m_world->CanUndo()) {
				m_console.Refuse("nothing to undo");
				return;
			}
			m_world->Undo();
			m_console.Print("undone");
		});
	m_console.Register(
		{.name = "redo",
		 .group = CmdGroup::Levels,
		 .summary = "redo one editor step (the toolbar's > / Ctrl+Y)"},
		[this](const std::vector<std::string>&) {
			if (!m_world->CanRedo()) {
				m_console.Refuse("nothing to redo");
				return;
			}
			m_world->Redo();
			m_console.Print("redone");
		});
	m_console.Register({.name = "savemap",
						.group = CmdGroup::Levels,
						.summary = "write every edited level, and the world, to the project"},
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
							   m_console.Refuse("save failed (see log)");
						   }
						   // AND THE WORLD, which is a level's peer now rather
						   // than a hand-authored file the editor never touched.
						   // Reported separately: "saved levels" answering for
						   // the world too would hide a world that failed.
						   if (m_worldMap) {
							   if (SaveWorld()) m_console.Print("saved world");
							   else m_console.Refuse("world save failed");
						   }
						   // The opening, when a dragged stair carried it along.
						   if (m_world->ConsumeOpeningMoved()) {
							   if (m_project.Save()) m_console.Print("saved project opening");
							   else m_console.Refuse("project save failed");
						   }
					   });
	// WHAT THE NEXT savemap WRITES BESIDES THE ACTIVE LEVEL (code-review C298,
	// C307, C308): each level whose map or .ent is stashed - every one of them
	// the save rewrites - and those whose dynamic state is held; then whether
	// the active level's map and records differ from its files. A level only
	// visited, or only read, must appear in neither stash list.
	m_console.Register({.name = "stashes",
						.group = CmdGroup::Levels,
						.summary = "list the levels the next savemap writes, and why"},
					   [this](const std::vector<std::string>&) {
						   if (!m_world) {
							   m_console.Refuse("stashes: no world loaded");
							   return;
						   }
						   const DungeonWorld::StashReport s = m_world->Stashes();
						   m_console.Print(std::format("stashes: maps={} ents={} states={}", s.maps,
													   s.ents, s.states));
						   m_console.Print(std::format("stashes: active {} map={} ents={}{}",
													   m_world->CurrentLevel(),
													   s.mapEdited ? "edited" : "clean",
													   s.entsEdited ? "edited" : "clean",
													   s.parked ? " parked" : ""));
					   });
	m_console.Register({.name = "synctosource",
						.group = CmdGroup::Levels,
						.summary = "copy the active project (edits) into the repo source tree"},
					   [this](const std::vector<std::string>&) {
						   if (SyncProjectToSource()) m_console.Print("synced project -> source");
						   else m_console.Refuse("sync failed (see log)");
					   });
	m_console.Register({.name = "preview",
						.group = CmdGroup::Rendering,
						.params = "<model>\n"
								  "off",
						.summary = "show a model in the 3D preview, or close it"},
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty() && args[0] == "off") {
							   // In-flight frames may still draw the mesh
							   if (m_previewMesh) m_device.WaitIdle();
							   m_previewMesh.reset();
							   m_console.Print("preview off");
							   return;
						   }
						   if (!Need(m_console, args, 1)) return;
						   const std::string name = JoinArgs(args);
						   // A bare NAME, as a catalog's `model` is: whichever of
						   // .gltf / .glb is installed (code-review C301).
						   const std::string file = ResolveModelFile(name);
						   if (!ModelFileInstalled(file)) {
							   m_console.Refuse("no model: " + name);
							   return;
						   }
						   m_previewModel = LoadModelOrDie(file);
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
	m_console.Register({.name = "monsters",
						.group = CmdGroup::Monsters,
						.summary = "list monsters and their cells"},
					   [this](const std::vector<std::string>&) {
						   const std::vector<std::string> list = m_world->MonsterList();
						   if (list.empty()) {
							   m_console.Print("no monsters");
							   return;
						   }
						   for (const std::string& l : list) m_console.Print("  " + l);
					   });
	m_console.Register({.name = "buttons",
						.group = CmdGroup::Levels,
						.summary = "list buttons (id, cell, state)"},
					   [this](const std::vector<std::string>&) {
						   const std::vector<std::string> list = m_world->ButtonList();
						   if (list.empty()) {
							   m_console.Print("no buttons");
							   return;
						   }
						   for (const std::string& l : list) m_console.Print("  " + l);
					   });
	// Each door as it stands in play beside its record: an inspector edit that
	// did not move the leaf, or a wrecked one, sets the two apart (C356).
	m_console.Register({.name = "doors",
						.group = CmdGroup::Levels,
						.summary = "list doors (id, type, cell, open or shut, the authored state)"},
					   [this](const std::vector<std::string>&) {
						   const std::vector<std::string> list = m_world->DoorList();
						   if (list.empty()) {
							   m_console.Print("no doors");
							   return;
						   }
						   for (const std::string& l : list) m_console.Print("  " + l);
					   });
	m_console.Register({.name = "smash",
						.group = CmdGroup::Combat,
						.params = "<x> <z> [amount]",
						.summary = "damage what is breakable in a cell (default 100)"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 2)) return;
						   const int x = std::atoi(args[0].c_str());
						   const int z = std::atoi(args[1].c_str());
						   const float amount =
							   args.size() > 2 ? std::strtof(args[2].c_str(), nullptr)
											   : 100.0f;
						   const int n = m_world->SmashAt(x, z, amount);
						   // A REFUSAL when it struck nothing, so a script aimed at
						   // an empty square fails rather than measuring nothing:
						   // pipeline.eval's breakables section smashed three empty
						   // cells of eval_arena for its whole life and passed.
						   if (n > 0)
							   m_console.Print(
								   std::format("struck {} breakable(s) at {},{}", n, x, z));
						   else
							   m_console.Refuse(std::format("nothing breakable at {},{}", x, z));
					   });
	m_console.Register({.name = "breakables",
						.group = CmdGroup::Combat,
						.params = "[x z]\n<x> <z> <effect> [magnitude per sec for a DoT] [seconds]",
						.summary = "list breakable pieces (hp, broken, effects), or land an effect on a cell's"},
					   [this](const std::vector<std::string>& args) {
						   int x = -1, z = -1;
						   if (args.size() >= 2) {
							   x = std::atoi(args[0].c_str());
							   z = std::atoi(args[1].c_str());
						   }
						   if (args.size() >= 3) {
							   const float mag = args.size() > 3
								   ? std::strtof(args[3].c_str(), nullptr) : 8.0f;
							   const float secs = args.size() > 4
								   ? std::strtof(args[4].c_str(), nullptr) : 60.0f;
							   const int n =
								   m_world->ApplyEffectToBreakables(x, z, args[2], mag, secs);
							   if (n < 0)
								   m_console.Refuse(std::format("no effect '{}'", args[2]));
							   else if (n == 0)
								   m_console.Refuse(
									   std::format("nothing breakable at {},{}", x, z));
							   else
								   m_console.Print(std::format("{} breakable(s) at {},{} gain {}",
															   n, x, z, args[2]));
							   return;
						   }
						   const std::vector<std::string> list =
							   m_world->BreakableReport(x, z);
						   if (list.empty()) {
							   m_console.Print("no breakables");
							   return;
						   }
						   for (const std::string& l : list) m_console.Print("  " + l);
					   });
	// `party` honours the lever's flag= wait, as a hand would.
	m_console.Register({.name = "press",
						.group = CmdGroup::Levels,
						.params = "<x> <z> [party]",
						.summary = "toggle the button in a cell (exercises save)"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 2)) return;
						   const int x = std::atoi(args[0].c_str());
						   const int z = std::atoi(args[1].c_str());
						   bool on = false;
						   const bool party = args.size() > 2 && args[2] == "party";
						   // No button is a REFUSE (C442); a lever the party's hand
						   // cannot move (its flag= wait) is an OUTCOME, read off
						   // the state this reports.
						   if (m_world->ToggleButtonAt(x, z, on, party))
							   m_console.Print(std::format("button {},{} -> {}", x, z,
														   on ? "on" : "off"));
						   else
							   m_console.Refuse(std::format("no button at {},{}", x, z));
					   });
	// A niche's runtime open state, or set it: a lever's reveal (ToggleNichesNamed)
	// without a lever wired to a named niche - a test can shut what it placed.
	// `put` lays an item in it (AddNicheItem), which the editor's item brush
	// cannot reach until code-review C351 lands (batch 82). `looks` lists the
	// walls pre-built for the lever names (code-review C211): a press swaps one in.
	m_console.Register({.name = "niche",
						.group = CmdGroup::Levels,
						.params = "<x> <z> <n|e|s|w> [open|shut]\n<x> <z> <n|e|s|w> put <item>\nlooks",
						.summary = "a wall niche's open state; open or shut it, or lay an item in it"},
					   [this](const std::vector<std::string>& args) {
						   if (args.size() == 1 && args[0] == "looks") {
							   for (const std::string& line : m_world->NicheLooksReport())
								   m_console.Print(line);
							   return;
						   }
						   if (!Need(m_console, args, 3)) return;
						   const int x = std::atoi(args[0].c_str());
						   const int z = std::atoi(args[1].c_str());
						   const std::string& d = args[2];
						   const Direction wall = d == "e"   ? Direction::East
												  : d == "s" ? Direction::South
												  : d == "w" ? Direction::West
															 : Direction::North;
						   if (!m_world->NicheOn(x, z, wall)) {
							   m_console.Refuse(std::format("no niche at {},{} {}", x, z, d));
							   return;
						   }
						   if (args.size() > 3 && args[3] == "put") {
							   if (!Need(m_console, args, 5)) return;
							   if (!m_world->AddNicheItem(args[4], x, z, wall)) {
								   m_console.Refuse(std::format("niche: '{}' is not an item", args[4]));
								   return;
							   }
							   m_console.Print(std::format("niche {},{} {}: holds {}", x, z, d, args[4]));
							   return;
						   }
						   if (args.size() > 3)
							   m_world->SetNicheOpen(x, z, wall, args[3] == "open");
						   m_console.Print(std::format("niche {},{} {}: {}", x, z, d,
													   m_world->NicheOpenAt(x, z, wall) ? "open"
																						: "shut"));
					   });
	// Is every click target where it is drawn (DungeonWorld_PickProbe.cpp)?
	m_console.Register({.name = "pickprobe",
						.group = CmdGroup::Levels,
						.summary = "shoot every click target in reach from where it is drawn"},
					   [this](const std::vector<std::string>&) {
						   for (const std::string& line : m_world->ProbePicks())
							   m_console.Print(line);
					   });
	m_console.Register({.name = "opendoor",
						.group = CmdGroup::Levels,
						.params = "<x> <z>",
						.summary = "the party's hand on a door: its flag wait, its key, then the toggle"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 2)) return;
						   const int x = std::atoi(args[0].c_str());
						   const int z = std::atoi(args[1].c_str());
						   bool open = false;
						   // As `press`: no door refuses; a sealed or locked one is
						   // an outcome the reported state shows.
						   if (m_world->HandOnDoorAt(x, z, open))
							   m_console.Print(std::format("door {},{} -> {}", x, z,
														   open ? "open" : "shut"));
						   else
							   m_console.Refuse(std::format("no door at {},{}", x, z));
					   });
	// The door / lever / stair inspectors' flag rows, without a mouse: the SAME
	// setters their Apply calls, reading the object's settings first so only
	// the flag wiring changes. `flag=` (or bare `flag=` to clear) is what it
	// waits on; a lever also takes one of sets= / clears= / toggles= (or `op=`
	// alone to clear the op). The same rows' other half of the wiring rides
	// along: a door's `name=` and a lever's `target=` (the door name it works),
	// so a script can wire a lever to a door it placed (code-review C357).
	m_console.Register(
		{.name = "flagwire",
		 .group = CmdGroup::Levels,
		 .params = "<x> <z> door [flag=[id]] [name=[id]]\n"
				   "<x> <z> stair [flag=[id]]\n"
				   "<x> <z> lever [flag=[id]] [sets=|clears=|toggles=<id>] [op=] [target=[name]]",
		 .summary = "wire a door's, lever's or stair's flags (a door's name, a lever's target) as its inspector does"},
		[this](const std::vector<std::string>& args) {
			if (!Need(m_console, args, 3)) return;
			const int x = std::atoi(args[0].c_str());
			const int z = std::atoi(args[1].c_str());
			const std::string& what = args[2];
			std::optional<std::string> flag, name, target;
			std::optional<std::pair<FlagOp, std::string>> op;
			for (size_t i = 3; i < args.size(); ++i) {
				const size_t eq = args[i].find('=');
				const std::string k = args[i].substr(0, eq);
				const std::string v = eq == std::string::npos ? "" : args[i].substr(eq + 1);
				// A name is written into a whitespace-tokenised key=value record,
				// so it keeps to the inspector's Name field's characters.
				const bool recordSafe = std::all_of(v.begin(), v.end(), [](char ch) {
					return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-';
				});
				if (k == "flag") flag = v;
				else if ((k == "name" && what == "door") || (k == "target" && what == "lever")) {
					if (!recordSafe) {
						m_console.Refuse(std::format("flagwire: '{}' is not a record-safe name", v));
						return;
					}
					(k == "name" ? name : target) = v;
				}
				else if (k == "op") op = std::pair{FlagOp::None, std::string()};
				else if (FlagOpFromKey(k) != FlagOp::None) op = std::pair{FlagOpFromKey(k), v};
				else {
					m_console.Refuse(std::format("flagwire: unknown '{}'", args[i]));
					return;
				}
			}
			if (what == "door") {
				DungeonWorld::DoorEdit e;
				if (!m_world->DoorSettings(x, z, e)) {
					m_console.Refuse(std::format("flagwire: no door at {},{}", x, z));
					return;
				}
				if (flag) e.flag = *flag;
				if (name) e.name = *name;
				m_world->SetDoorSettings(x, z, e);
				// The name only when one was given: the line read back is exact.
				m_console.Print(std::format("flagwire door {},{} flag={}{}", x, z, e.flag,
											name ? " name=" + e.name : std::string()));
			} else if (what == "lever") {
				DungeonWorld::ButtonEdit e;
				if (!m_world->ButtonSettings(x, z, e)) {
					m_console.Refuse(std::format("flagwire: no lever at {},{}", x, z));
					return;
				}
				if (flag) e.needs = *flag;
				if (op) std::tie(e.op, e.sets) = *op;
				if (target) e.target = *target;
				m_world->SetButtonSettings(x, z, e);
				m_world->ButtonSettings(x, z, e); // as written
				m_console.Print(std::format("flagwire lever {},{} flag={} {}={}{}", x, z, e.needs,
											*FlagOpKey(e.op) ? FlagOpKey(e.op) : "op", e.sets,
											target ? " target=" + e.target : std::string()));
			} else if (what == "stair") {
				StairLink s;
				if (!m_world->StairSettings(x, z, s)) {
					m_console.Refuse(std::format("flagwire: no stair at {},{}", x, z));
					return;
				}
				if (flag) m_world->SetStairFlag(x, z, *flag);
				m_world->StairSettings(x, z, s);
				m_console.Print(std::format("flagwire stair {},{} flag={}", x, z, s.flag));
			} else {
				m_console.Refuse("flagwire: door, lever or stair");
			}
		});
	m_console.Register({.name = "ver", .group = CmdGroup::Console, .summary = "print build and GPU info"},
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
	m_console.Register({.name = "face",
						.group = CmdGroup::Party,
						.params = "n|e|s|w",
						.summary = "turn the party to a compass direction"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
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
	m_console.Register({.name = "home",
						.group = CmdGroup::Party,
						.summary = "teleport the party to the start cell"},
					   [this](const std::vector<std::string>&) {
						   const DungeonMap& map = m_world->Map();
						   m_world->GetParty().SetGridPosition(map.StartX(), map.StartZ());
						   m_console.Print(std::format("home at {},{}", map.StartX(),
													   map.StartZ()));
					   });
	m_console.Register({.name = "speed",
						.group = CmdGroup::Party,
						.params = "<mult>",
						.summary = "set party pace multiplier"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   const float v = static_cast<float>(std::atof(args[0].c_str()));
						   if (v <= 0.0f) {
							   m_console.Refuse("speed must be > 0");
							   return;
						   }
						   m_world->GetParty().SetSpeed(v);
						   m_console.Print(std::format("speed x{:.2f}", v));
					   });

	m_console.Register({.name = "noclip",
						.group = CmdGroup::Party,
						.summary = "toggle walking through walls"},
					   [this](const std::vector<std::string>&) {
						   Party& p = m_world->GetParty();
						   p.SetNoclip(!p.Noclip());
						   m_console.Print(p.Noclip() ? "noclip on" : "noclip off");
					   });

	// --- fonts: the audition (docs/fonts.md Phase 4) ---
	m_console.Register(
		{.name = "fonts",
		 .group = CmdGroup::Settings,
		 .summary = "show each role's typeface, the installed faces and live atlases"},
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

	// Bare prints these forms; a bare <role> reports its face.
	m_console.Register(
		{.name = "font",
		 .group = CmdGroup::Settings,
		 .params = "<role> [<name>|<index>|next|prev|off]\n"
				   "scale <role> <n>\n"
				   "save",
		 .summary = "audition a typeface live, set a role's optical size, or save"},
		[this](const std::vector<std::string>& args) { FontCommand(args); });

	// --- render debug ---
	// This frame's lights, each with where it came from and the lights.cat
	// profile it was made from (lighting-updates Phase 2). `profiles` lists
	// the profiles themselves, as parsed.
	m_console.Register({.name = "lights",
						.group = CmdGroup::Rendering,
						.params = "\nprofiles\nreload",
						.summary = "this frame's lights and their profiles"},
					   [this](const std::vector<std::string>& args) {
						   // A hand edit to lights.cat, taken live: re-read the file,
						   // then the profiles (every light looks its own up per frame).
						   if (!args.empty() && args[0] == "reload") {
							   m_project.lights.Load(m_project.CatalogPath("lights.cat"));
							   m_world->ReloadLightProfiles();
							   m_console.Print(std::format("lights: {} profiles reloaded",
														   m_world->LightProfiles().size()));
							   return;
						   }
						   if (!args.empty() && args[0] == "profiles") {
							   for (const light::Profile& p : m_world->LightProfiles())
								   m_console.Print(std::format(
									   "  {:<13} rgb {} i {:.2f} r {:.2f} sq  {} rate {:.2f} "
									   "depth {:.2f}  wander {:.4f}  {}{}",
									   p.id,
									   p.sourceColor ? std::string("source")
													 : std::format("{:.2f} {:.2f} {:.2f}", p.color.x,
																   p.color.y, p.color.z),
									   p.intensity, p.radius, light::PulseName(p.pulse),
									   p.pulseRate, p.pulseDepth, p.wander,
									   p.shadow ? "shadow" : "no shadow",
									   p.longFade ? " long-fade" : ""));
							   return;
						   }
						   for (const std::string& line : m_world->DescribeLights())
							   m_console.Print(line);
					   });
	// What things in flight shed (lighting-updates Phase 4): the spark pool's
	// pressure and each trails.cat profile; `reload` takes a hand edit live.
	m_console.Register({.name = "trails",
						.group = CmdGroup::Rendering,
						.params = "\nreload",
						.summary = "the spark pool's use and the trail profiles"},
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty() && args[0] == "reload") {
							   m_project.trails.Load(m_project.CatalogPath("trails.cat"));
							   m_world->ReloadLightProfiles();
						   }
						   for (const std::string& line : m_world->DescribeTrails())
							   m_console.Print(line);
					   });
	// The Earth lights set down on this level (Stonelight, lighting-updates 6f).
	m_console.Register({.name = "lightstones",
						.group = CmdGroup::Rendering,
						.params = "[clear]",
						.summary = "the Earth light stones set down on this level"},
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty() && args[0] == "clear") m_world->ClearLightStones();
						   for (const std::string& line : m_world->DescribeLightStones())
							   m_console.Print(line);
					   });
	// The tracks monsters have left on this level (lighting-updates 6g): how
	// many still show and the freshest, each with the way its maker went.
	m_console.Register({.name = "tracks",
						.group = CmdGroup::Monsters,
						.params = "[clear]\nadd <x> <z> <n|e|s|w>",
						.summary = "the tracks monsters have left on this level"},
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty() && args[0] == "clear") m_world->ClearTracks();
						   // A fresh monster track planted by hand, for a frozen test.
						   if (!args.empty() && args[0] == "add") {
							   if (!Need(m_console, args, 4)) return;
							   const std::string& d = args[3];
							   const Direction dir = d == "e"   ? Direction::East
													 : d == "s" ? Direction::South
													 : d == "w" ? Direction::West
																: Direction::North;
							   m_world->AddTrack(std::atoi(args[1].c_str()),
												 std::atoi(args[2].c_str()), dir);
						   }
						   for (const std::string& line : m_world->DescribeTracks())
							   m_console.Print(line);
					   });
	// The light budget's measuring tools (lighting-updates Phase 3): a load of
	// test lights round the party, and the tiled light lists on or off, so the
	// tiles' saving can be read off `profile snap` in one build.
	// `fill` (code-review C181) is the CEILING's test: as many test lights as the
	// candidate list holds, pushed AHEAD of the fires, so the fire loop runs with
	// the list full and every PushLight it makes comes back null; `lights` then
	// prints what the ceiling refused.
	m_console.Register({.name = "lightstress",
						.group = CmdGroup::Rendering,
						.params = "<count> [near]\nfill [near]\noff",
						.summary = "scatter test lights over the level, or near the party (a load for the budget)"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   const bool fill = args[0] == "fill";
						   const int n = args[0] == "off" ? 0
										 : fill            ? DungeonWorld::LightCandidateCeiling()
														   : std::atoi(args[0].c_str());
						   const bool nearby = args.size() >= 2 && args[1] == "near";
						   m_console.Print(std::format("lightstress: {} test lights{}{}",
													   m_world->SetStressLights(n, nearby, fill),
													   nearby ? " near the party" : "",
													   fill ? ", ahead of the fires (fill)" : ""));
					   });
	m_console.Register({.name = "lighttiles",
						.group = CmdGroup::Rendering,
						.params = "[on|off]",
						.summary = "tiled light lists in the scene shader (off = every light everywhere)"},
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty()) m_world->SetLightTiling(ArgOn(args[0]));
						   m_console.Print(m_world->LightTiling() ? "lighttiles on"
																  : "lighttiles off");
					   });
	m_console.Register({.name = "glass",
						.group = CmdGroup::Rendering,
						.params = "[status]",
						.summary = "the transparent queue: queued, frames, peak, overflows, dropped"},
					   [this](const std::vector<std::string>&) {
						   const gfx::TransparentStats& s = m_renderer.Stats();
						   m_console.Print(std::format(
							   "glass queued={} frames={} peak={} overflows={} dropped={}",
							   s.queued, s.frames, s.peak, s.overflows, s.dropped));
					   });
	// The UI's vertex arena (code-review C163): the console perf panel's UI gauge
	// as one line. Bytes, so a harness can compare against the cell layer's size;
	// the last FINISHED frame's, since a command runs mid-frame.
	m_console.Register({.name = "sprites",
						.group = CmdGroup::Rendering,
						.summary = "the UI sprite arena: last frame's bytes, the peak, batches dropped"},
					   [this](const std::vector<std::string>&) {
						   const gfx::SpriteArenaStats& s = m_spriteBatch.ArenaStats();
						   constexpr double kMB = 1024.0 * 1024.0;
						   m_console.Print(std::format(
							   "sprites last={} peak={} capacity={} ({:.2f} / {:.2f} MB, peak "
							   "{:.2f}) lastdrops={} drops={} dropvertices={}",
							   s.lastFrame, s.peak, s.capacity, s.lastFrame / kMB,
							   s.capacity / kMB, s.peak / kMB, s.lastDrops, s.drops,
							   s.droppedVerts));
					   });
	// What the load paths leave resident (code-review C154 / C222 / C471). The
	// texture readout is a quality swap's evidence - every set at the tier asked
	// for, none STALE, and the SRV gauge back where it was after a round trip -
	// and `load` puts a set no level names through the prop loader, which is how
	// a harness sees a set with no normal map come back flat. Eval.ps1 -SelfTest
	// reads both.
	m_console.Register({.name = "textures",
						.group = CmdGroup::Rendering,
						.params = "\nload <set>",
						.summary = "the texture sets loaded at a tier, their sizes and the SRV gauge"},
					   [this](const std::vector<std::string>& args) {
						   if (!m_world) {
							   m_console.Print("no world loaded");
							   return;
						   }
						   if (!args.empty() && args[0] == "load") {
							   if (!Need(m_console, args, 2)) return;
							   m_console.Print(m_world->ProbeTextureSet(args[1]));
							   return;
						   }
						   if (!args.empty()) {
							   m_console.RefuseUsage();
							   return;
						   }
						   for (const std::string& line : m_world->DescribeTextures())
							   m_console.Print(line);
					   });
	m_console.Register({.name = "modelcache",
						.group = CmdGroup::Rendering,
						.summary = "the model cache: files, and the CPU image bytes any still pin"},
					   [this](const std::vector<std::string>&) {
						   if (!m_world) {
							   m_console.Print("no world loaded");
							   return;
						   }
						   for (const std::string& line : m_world->DescribeModelCache())
							   m_console.Print(line);
					   });
	// A decoration kind's model as drawn (code-review C253): its bounds with
	// the node transforms baked, the cull sphere taken from them, and the
	// node-space radius the multi-material sphere used to be read from. Loads
	// the kind; Eval.ps1 -SelfTest reads viking_dagger's (lifetimes.eval).
	m_console.Register({.name = "decokind",
						.group = CmdGroup::Rendering,
						.params = "<decoration type>",
						.summary = "a decoration kind's bounds and cull sphere, as drawn"},
					   [this](const std::vector<std::string>& args) {
						   if (!m_world) {
							   m_console.Refuse("no world loaded");
							   return;
						   }
						   if (!Need(m_console, args, 1)) return;
						   const std::string line = m_world->DescribeDecorationKind(args[0]);
						   if (line.empty()) {
							   m_console.Refuse(std::format(
								   "decokind: '{}' is not in decorations.cat", args[0]));
							   return;
						   }
						   m_console.Print(line);
					   });
	// `status` is the shadow cache's readout (code-review C178 / C187): each
	// slot's light and its re-renders by reason, which is how a harness sees a
	// door's move or a walking Firelight reach a cube. `door` is the pass that
	// first drew a door's latest pose, which a slot's `lastpass` must reach.
	// `ignore` is that harness's mutation switch (AllocTest -Lights
	// -ShadowSelfTest): the change notes dropped, or a wandering light's moves
	// not counted.
	m_console.Register({.name = "shadows",
						.group = CmdGroup::Rendering,
						.params = "[on|off]\nstatus\ndoor <x> <z>\nignore notes|moves|both|none",
						.summary = "toggle shadow rendering, or read the shadow cube cache"},
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty() && args[0] == "status") {
							   for (const std::string& line : m_world->DescribeShadows())
								   m_console.Print(line);
							   return;
						   }
						   if (!args.empty() && args[0] == "door") {
							   if (args.size() < 3) {
								   m_console.RefuseUsage();
								   return;
							   }
							   const int x = std::atoi(args[1].c_str());
							   const int z = std::atoi(args[2].c_str());
							   const std::string line = m_world->DescribeDoorShadow(x, z);
							   m_console.Print(line.empty() ? std::format("no door at {},{}", x, z)
															: line);
							   return;
						   }
						   if (!args.empty() && args[0] == "ignore") {
							   const std::string what = args.size() > 1 ? args[1] : "";
							   if (what != "notes" && what != "moves" && what != "both" &&
								   what != "none") {
								   m_console.RefuseUsage();
								   return;
							   }
							   m_world->SetShadowIgnore(what == "notes" || what == "both",
														what == "moves" || what == "both");
							   m_console.Print(m_world->DescribeShadows().front());
							   return;
						   }
						   if (!args.empty()) m_world->SetShadowsEnabled(ArgOn(args[0]));
						   m_console.Print(m_world->ShadowsEnabled() ? "shadows on"
																	: "shadows off");
					   });
	m_console.Register({.name = "shadowrate",
						.group = CmdGroup::Rendering,
						.params = "[<hz> [per-frame-budget]]",
						.summary = "fire shadow re-render rate and per-frame cube budget"},
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
	m_console.Register({.name = "dust",
						.group = CmdGroup::Rendering,
						.params = "[on|off|<density>]",
						.summary = "volumetric dust on/off, or its density (default 0.075)"},
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
	m_console.Register({.name = "haze",
						.group = CmdGroup::Rendering,
						.params = "[value]",
						.summary = "dust ambient pickup (mood tuning, default 0.9)"},
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty())
							   m_world->SetHazeAmbient(
								   static_cast<float>(std::atof(args[0].c_str())));
						   m_console.Print(
							   std::format("haze ambient {:.2f}", m_world->HazeAmbient()));
					   });
	m_console.Register({.name = "ambient",
						.group = CmdGroup::Rendering,
						.params = "[scale]",
						.summary = "scale the ambient fill (mood tuning, default 1.0)"},
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty())
							   m_world->SetAmbientScale(
								   static_cast<float>(std::atof(args[0].c_str())));
						   m_console.Print(
							   std::format("ambient x{:.2f}", m_world->AmbientScale()));
					   });
	m_console.Register({.name = "fov",
						.group = CmdGroup::Settings,
						.params = "[degrees]",
						.summary = "camera field of view in degrees (default 70)"},
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

	// Every decline below REFUSES (code-review C442): a face that was not
	// swapped is an audition of the old one.
	if (args[0] == "save") {
		if (SaveFontCatalog()) m_console.Print("fonts.cat written");
		else m_console.Refuse("could not write fonts.cat (see the log)");
		return;
	}

	if (args[0] == "scale") {
		ui::FontRole role{};
		if (args.size() < 3 || !ui::FontRoleFromName(args[1], role)) {
			m_console.Refuse("usage: font scale <body|display|script|mono> <n>");
			return;
		}
		ui::FaceSpec spec = m_fonts.Face(role);
		spec.scale = static_cast<float>(std::atof(args[2].c_str()));
		m_fonts.SetFace(role, spec);
		// An audition over a language's face becomes the role's own face.
		m_langFontBase[static_cast<size_t>(role)].reset();
		describe(role);
		return;
	}

	ui::FontRole role{};
	if (!ui::FontRoleFromName(args[0], role)) {
		m_console.Refuse(std::format("unknown role '{}' (body|display|script|mono)",
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
			m_console.Refuse(std::format("no face [{}] - `fonts` lists them", index));
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
			m_console.Refuse(std::format("no installed face matches '{}'", what));
			return;
		}
		if (hits.size() > 1) {
			m_console.Refuse(std::format("'{}' matches {} faces:", what, hits.size()));
			for (const std::string& h : hits) m_console.Print("  " + h);
			return;
		}
		spec.path = paths::Asset(hits.front());
	}

	m_fonts.SetFace(role, spec);
	m_langFontBase[static_cast<size_t>(role)].reset(); // as `font scale` above
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
		// The role's own face, never one the current LANGUAGE lays over it.
		const ui::FaceSpec& spec = BaseFace(role);
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

void Game::PrintThumbSurveyLine(std::string_view head, const std::string& name,
								const std::string& stem, const gfx::Rect& rect, bool drawn) {
	const std::optional<Vec4> mean = stem.empty() ? std::nullopt : StoredMeanColor(stem);
	m_console.Print(std::format(
		"{} {} rect={},{},{},{} drawn={} mean={}", head, name, static_cast<int>(rect.x),
		static_cast<int>(rect.y), static_cast<int>(rect.w), static_cast<int>(rect.h), drawn ? 1 : 0,
		mean ? std::format("{:.3f},{:.3f},{:.3f},{:.3f}", mean->x, mean->y, mean->z, mean->w)
			 : std::string("-")));
}

// `editor palette [mode stage|kind | group <name> | filter [text] | groups |
// items <catalog> | expand | collapse | swatches]`.
// Every change goes through the same MapEditor calls the bar and the filter box
// make, then the line says what the accordion lists: the grouping, the picked
// group, the filter, and each section showing with how many of its rows show.
void Game::PrintPalette(const std::vector<std::string>& args) {
	using G = MapEditor::Grouping;
	auto modeName = [](G g) { return g == G::Kind ? "kind" : "stage"; };
	// THE SWATCH BRIGHTNESS SURVEY (code-review C158): every texture swatch the
	// palette drew last frame, where, and the stored mean of the set it came
	// from - what a correct (linear) draw of it averages to. A harness
	// photographs the window and sets the two side by side (InGameTest).
	if (args.size() >= 2 && args[1] == "swatches") {
		// The record is the LAST draw's: with the editor shut it is stale.
		if (!m_mapView.IsOpen() || m_mapView.CurrentMode() != MapView::Mode::Editor) {
			m_console.Refuse("editor palette swatches: the editor is not open");
			return;
		}
		const std::span<const MapEditor::DrawnSwatch> drawn = m_mapEditor.DrawnSwatches();
		m_console.Print(std::format("editor palette swatches: {} drawn, dock {}", drawn.size(),
									m_settings.mapPaletteCollapsed ? "collapsed" : "open"));
		for (const MapEditor::DrawnSwatch& s : drawn) {
			const auto [set, stem] = m_world->SwatchSource(s.texture);
			PrintThumbSurveyLine("editor palette swatch", set.empty() ? "?" : set, stem, s.rect,
								 true);
		}
		return;
	}
	if (args.size() >= 2 && args[1] == "groups") {
		for (const G g : {G::Stage, G::Kind})
			for (int i = 0; i < MapEditor::GroupCount(g); ++i) {
				std::string line = std::format("editor palette group {} {}:", modeName(g),
											   MapEditor::GroupName(g, i));
				for (const MapEditor::PaletteCat c : MapEditor::GroupCategories(g, i))
					line += std::format(" {}", MapEditor::CategoryCatalogKey(c));
				m_console.Print(line);
			}
		return;
	}
	// A Quest items & flags row, used as a click would use it (on its go-to
	// link with `link`), then what that left: the armed brush, the viewed level
	// and selected square, whether a type editor opened.
	if (args.size() >= 3 && args[1] == "use") {
		const bool link = args.size() > 3 && args[3] == "link";
		if (!m_mapEditor.UseQuestRow(args[2], link)) {
			m_console.Refuse(std::format("editor palette: no quest row '{}'", args[2]));
			return;
		}
		const MapEditor::PaletteCat armed = m_mapEditor.ArmedCat();
		m_console.Print(std::format(
			"editor palette used {} armed={}:{} view={} sel={},{} typeeditor={}", args[2],
			armed == MapEditor::PaletteCat::Count ? "-" : MapEditor::CategoryCatalogKey(armed),
			m_mapEditor.ArmedId(), m_mapView.ViewedLevel(), m_mapEditor.SelX(),
			m_mapEditor.SelZ(), m_typeDialog.IsOpen() ? m_typeDialog.Id() : std::string("-")));
		return;
	}
	// One section's rows as the accordion resolves them - the power band a
	// monster row's pips draw included.
	if (args.size() >= 3 && args[1] == "items") {
		const MapEditor::PaletteCat cat = MapEditor::CatForCatalogKey(args[2]);
		if (cat == MapEditor::PaletteCat::Count) {
			m_console.Refuse(std::format("editor palette: no section '{}'", args[2]));
			return;
		}
		// The head is fixed (phase 13 matches it); what a row adds goes after:
		// its group, the catalog it stands for and where it goes, for the
		// Quest items & flags section.
		for (const MapEditor::PaletteItem& it : m_mapEditor.Items(cat))
			m_console.Print(std::format(
				"editor palette item {} {} band={} group='{}' ref={} goto={} label='{}' lens={}",
				args[2], it.id, it.band, it.group, it.ref.empty() ? "-" : it.ref,
				it.gotoLevel.empty() ? std::string("-")
									 : std::format("{}@{},{}", it.gotoLevel, it.gotoX, it.gotoZ),
				it.label, it.onTags ? "on" : "off"));
		return;
	}
	if (args.size() >= 3 && args[1] == "mode") {
		if (args[2] != "stage" && args[2] != "kind") {
			m_console.Refuse("usage: editor palette mode stage|kind");
			return;
		}
		m_mapEditor.SetPaletteGrouping(args[2] == "kind" ? G::Kind : G::Stage);
	} else if (args.size() >= 3 && args[1] == "group") {
		const G g = m_mapEditor.PaletteGrouping();
		int found = -1;
		for (int i = 0; i < MapEditor::GroupCount(g); ++i)
			if (args[2] == MapEditor::GroupName(g, i)) found = i;
		if (found < 0) {
			m_console.Refuse(std::format("editor palette: no group '{}' when grouped by {}",
										 args[2], modeName(g)));
			return;
		}
		m_mapEditor.SetActiveGroup(found);
	} else if (args.size() >= 2 && args[1] == "filter") {
		m_mapEditor.SetFilter(args.size() >= 3 ? args[2] : std::string());
	} else if (args.size() >= 2 && args[1] == "expand") {
		m_mapEditor.ExpandShown();
	} else if (args.size() >= 2 && args[1] == "collapse") {
		m_mapEditor.CollapseAll();
	}
	const G g = m_mapEditor.PaletteGrouping();
	std::string line =
		std::format("editor palette: {} {} filter='{}' shows:", modeName(g),
					MapEditor::GroupName(g, m_mapEditor.ActiveGroup()), m_mapEditor.Filter());
	for (const MapEditor::ShownSection& s : m_mapEditor.ShownSections())
		line += std::format(" {}({})", MapEditor::CategoryCatalogKey(s.cat), s.items);
	m_console.Print(line);
}

void Game::PrintDocks(const std::vector<std::string>& args) {
	if (m_mapView.IsOpen()) m_mapView.SetMode(MapView::Mode::Editor);
	else m_mapView.Open(MapView::Mode::Editor);
	// The panel Update hands the view: window pixels, as a drag would see.
	const gfx::Rect panel = MapPanel(static_cast<float>(m_window.Width()),
									 static_cast<float>(m_window.Height()));
	using Scope = MapView::OverviewScope;
	if (args[0] == "overview") {
		static constexpr const char* kNames[] = {"world", "dungeon", "level"};
		Scope scope = m_mapView.Scope();
		// `overview follow <key> [world|dungeon|level]`: click that line of the
		// panel (in its current scope unless named) - Phase 7's "what next" is
		// the one that matters - and say where it led.
		if (args.size() >= 3 && args[1] == "follow") {
			if (args.size() >= 4)
				for (int i = 0; i < 3; ++i)
					if (args[3] == kNames[i]) scope = static_cast<Scope>(i);
			for (const MapView::OverviewLine& l : m_mapView.OverviewContent(scope))
				if (l.key == args[2]) {
					m_mapView.FollowOverviewLink(l.link);
					m_console.Print(std::format("editor overview follow {} -> {} (palette {})", l.key,
												l.link.empty() ? "-" : l.link,
												MapEditor::GroupName(m_mapEditor.PaletteGrouping(),
																	 m_mapEditor.ActiveGroup())));
					return;
				}
			m_console.Refuse("editor overview follow: no line '" + args[2] + "'");
			return;
		}
		if (args.size() >= 2)
			for (int i = 0; i < 3; ++i)
				if (args[1] == kNames[i]) scope = static_cast<Scope>(i);
		for (const MapView::OverviewLine& l : m_mapView.OverviewContent(scope))
			m_console.Print(std::format("editor overview {} {} {}", kNames[static_cast<int>(scope)],
										l.key, l.title ? l.label : l.value));
		return;
	}
	if (args.size() >= 3) {
		const MapView::Dock d = args[1] == "left"	 ? MapView::Dock::Left
								: args[1] == "right" ? MapView::Dock::Right
													 : MapView::Dock::None;
		if (d == MapView::Dock::None) { // used to set nothing and print the layout
			m_console.Refuse("usage: editor dock [left|right <px>]");
			return;
		}
		m_mapView.SetDockWidth(d, static_cast<float>(std::atof(args[2].c_str())), panel);
	}
	const gfx::Rect g = m_mapView.GridRect(panel);
	const gfx::Rect s = m_mapView.StripRect(panel);
	const gfx::Rect b = m_mapView.PaletteBody(panel);
	m_console.Print(std::format(
		"editor dock panel={:.0f} left={:.0f} right={:.0f} grid={:.0f},{:.0f},{:.0f} "
		"strip={:.0f} palette={:.0f},{:.0f}",
		panel.w, m_mapView.DockWidth(MapView::Dock::Left, panel),
		m_mapView.DockWidth(MapView::Dock::Right, panel), g.x, g.w, g.x + g.w, s.x, b.x,
		b.w));
}

} // namespace dungeon::game
