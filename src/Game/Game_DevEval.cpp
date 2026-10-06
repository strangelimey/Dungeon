// ============================================================================
// Game/Game_DevEval.cpp — the eval harness's dev-console commands.
//
// Split out of Game_DevCommands.cpp by concern (docs/eval-harness.md): the
// primitives that make a measurement mean anything (timescale/logecho/messages/
// seed/lockstep/aiwait/step/state), getting into a world and out of it without
// a mouse (newgame/reset/title),
// and staging and reading an encounter (arena/forward/freeze/blast/spawn/
// monsterclips/autoattack/tally).
// ============================================================================
#include "Game/Game.h"

#include "Game/DevCommandArgs.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace dungeon::game {

using devargs::Need;

void Game::RegisterEvalCommands() {
	m_console.Register({.name = "timescale",
						.group = CmdGroup::Simulation,
						.params = "[scale]",
						.summary = "scale simulation speed (1 = normal, 0 = frozen)"},
					   [this](const std::vector<std::string>& args) {
						   if (args.empty()) {
							   m_console.Print(std::format("timescale {:.2f}", m_timeScale));
							   return;
						   }
						   const float v = static_cast<float>(std::atof(args[0].c_str()));
						   if (v < 0.0f) {
							   m_console.Refuse("timescale must be >= 0");
							   return;
						   }
						   m_timeScale = v;
						   m_console.Print(std::format("timescale {:.2f}", v));
					   });

	// --- the eval harness's three primitives (docs/eval-harness.md) ---------
	// Everything else the harness needs is CONTENT — arenas, presets, spawns.
	// These three are what make a measurement mean anything at all.

	// The combat RNG is otherwise constant-seeded, so every run of the game rolls
	// the identical sequence: perfectly reproducible, and a single sample
	// forever. An eval varies this per encounter and reports the DISTRIBUTION —
	// tuning against one seeded fight is tuning against one lucky afternoon.
	// The console answers in a WINDOW, and a window can only be read with a
	// screenshot — which captures whatever happens to be in front of it. This
	// puts every console line into dungeon.log instead, which is what makes the
	// existing command surface drivable from a script at all.
	m_console.Register({.name = "logecho",
						.group = CmdGroup::Console,
						.params = "[on|off]",
						.summary = "mirror console output to dungeon.log"},
					   [this](const std::vector<std::string>& args) {
						   if (args.empty()) {
							   m_console.Print(std::format(
								   "logecho {}", m_console.MirrorToLog() ? "on" : "off"));
							   return;
						   }
						   const bool on = args[0] == "on" || args[0] == "1";
						   m_console.SetMirrorToLog(on);
						   m_console.Print(std::format("logecho {}", on ? "on" : "off"));
					   });
	// The HUD's MESSAGE LOG, which logecho does not reach: what the world said
	// ("Maren's Firelight fades."), as the player read it. A world line goes to
	// the footer and nowhere else, so without this a script could not tell which
	// line a thing said (code-review C9 - a light's fade read as the Sight spell's).
	m_console.Register({.name = "messages",
						.group = CmdGroup::Console,
						.params = "[n]",
						.summary = "print the newest lines of the HUD message log"},
					   [this](const std::vector<std::string>& args) {
						   const int n = args.empty() ? 10 : std::atoi(args[0].c_str());
						   const std::vector<std::string> lines =
							   m_ui.RecentLogLines(static_cast<size_t>(std::max(n, 0)));
						   m_console.Print(std::format("messages: {}", lines.size()));
						   for (const std::string& l : lines) m_console.Print("  | " + l);
					   });

	// The only route from the title screen into a fight that does not involve
	// clicking a menu entry. A scripted run starts at the menu, so without this
	// the harness would be back to posting mouse clicks at hardcoded pixels —
	// which is exactly what it exists to stop doing.
	//
	// It goes through the MENU ENTRY'S OWN CALLBACK rather than calling
	// StartNewGame directly, and that is not tidiness — the first version called
	// StartNewGame and crashed the process on its first unattended run. From a
	// cold boot `m_gameLoaded` is false and the HUD has never been built (it is a
	// LOAD TASK), so StartNewGame set AppState::Playing and the same frame's
	// state machine then dereferenced a HUD with no widgets. onStartNewGame is
	// where the "already loaded, or load first?" decision lives; a dev command
	// that reimplements a UI action will drift from it, and this one drifted
	// immediately.
	m_console.Register({.name = "newgame",
						.group = CmdGroup::SaveLoad,
						.summary = "start a new game through the menu entry's own callback"},
					   [this](const std::vector<std::string>&) {
						   if (!m_ui.onStartNewGame) {
							   m_console.Refuse("newgame: not wired yet");
							   return;
						   }
						   m_ui.onStartNewGame();
						   // A cold boot now runs a staged load with commands
						   // disabled, so a script's next line waits for the
						   // world by itself.
						   m_console.Print("starting a new game");
					   });

	// THE TRIP A WIPE MAKES, taken on purpose: back to the title with the world
	// left resident, so the title's Continue / Load / Start New Game reset it in
	// place. Through the pause menu's own Return to Main Menu callback, for the
	// reason `newgame` goes through the title's. What it exists to reach is the
	// frozen stretch between a game and the next one, where the AI workers keep
	// thinking from the last fight (code-review C52; tools/AITest.py).
	m_console.Register({.name = "title",
						.group = CmdGroup::SaveLoad,
						.summary = "return to the title screen, as the pause menu's entry does"},
					   [this](const std::vector<std::string>&) {
						   if (!m_gameLoaded || !m_ui.onReturnToMain) {
							   m_console.Refuse("title: no game to leave");
							   return;
						   }
						   if (m_state == AppState::Menu) {
							   m_console.Print("title: already there");
							   return;
						   }
						   m_ui.onReturnToMain();
						   m_console.Print("back to the title");
					   });

	// RECYCLE THE WORLD instead of reloading it (docs/eval-harness.md). A level
	// load is ~12 seconds and 80% of a suite's cost; this is the same baseline
	// for nothing. A script that wants a CLEAN test opens with it; a script
	// measuring PROGRESSION across a series simply does not call it, and inherits
	// whatever the previous one left — Michael's call, and the reason this is a
	// directive a script chooses rather than something the runner imposes.
	m_console.Register({.name = "reset",
						.group = CmdGroup::Simulation,
						.summary = "recycle the world to a new-game baseline without a reload"},
					   [this](const std::vector<std::string>&) {
						   // TIMED, because the whole justification is the number:
						   // a level load is ~12000 ms and a run of hundreds of
						   // tests cannot pay it each time. If this ever creeps
						   // toward that, the recycling has stopped being worth
						   // its own risk and the reader should be able to see so.
						   // (A cold load and a switch only STAGE their load here;
						   // their time is the load table's, not this line's.)
						   const auto t0 = std::chrono::steady_clock::now();
						   std::string detail;
						   const EvalReset how = ResetForEval(detail);
						   if (how == EvalReset::Refused) {
							   m_console.Refuse("reset: " + detail);
							   return;
						   }
						   const double ms =
							   std::chrono::duration<double, std::milli>(
								   std::chrono::steady_clock::now() - t0)
								   .count();
						   switch (how) {
						   case EvalReset::Loaded:
							   m_console.Print(std::format(
								   "reset: loaded in {:.0f} ms (nothing to recycle yet)", ms));
							   break;
						   case EvalReset::Switched:
							   // SAID, because a batch whose next suite silently
							   // moved level is the defect this path exists for
							   // (C300); Eval.ps1 -SelfTest looks for this line.
							   m_console.Print(std::format(
								   "reset: switched in {:.0f} ms (from {} to {}, by a "
								   "level load)", ms, detail, HarnessLevel()));
							   break;
						   default:
							   m_console.Print(std::format("reset: recycled in {:.0f} ms", ms));
							   break;
						   }
					   });

	// WHAT A RESET IS SUPPOSED TO HAVE CLEARED, counted (code-review batch 12).
	// resettest.eval prints it in both baselines, so a leak shows up as a line
	// that differs between them; batches 77-79 each inject one and close it.
	// Nothing here is a measurement - a number that is not zero after `reset`
	// is a reset bug, not a balance figure.
	m_console.Register({.name = "transients",
						.group = CmdGroup::Simulation,
						.summary = "print the transient state a reset must clear"},
					   [this](const std::vector<std::string>&) {
						   const DungeonWorld::TransientReport t = m_world->Transients();
						   m_console.Print(std::format("transients on {}", m_world->CurrentLevel()));
						   m_console.Print(std::format(
							   "  blasts={} monster_effects={} on {} monster(s)", t.blasts,
							   t.monsterEffects, t.monstersAffected));
						   m_console.Print(std::format(
							   "  broken fixtures={} decorations={} doors={}  hurt={} "
							   "piece_effects={}",
							   t.brokenFixtures, t.brokenDecorations, t.brokenDoors,
							   t.hurtPieces, t.pieceEffects));
						   m_console.Print(std::format(
							   "  fall={} fell={} fallT={:.2f} cursor={}",
							   t.fallPending ? "pending" : "none", t.fellPending ? 1 : 0,
							   t.fallT,
							   m_heldItem ? ItemToken(*m_heldItem, m_heldItem.Charge())
										  : std::string("none")));
						   m_console.Print(std::format(
							   "  undo={} redo={} resting={} lockstep={}", t.undo, t.redo,
							   t.resting ? "on" : "off", t.lockstep ? "on" : "off"));
						   // Other levels' stashes (C300: a reset forgets them).
						   // Eval.ps1 -SelfTest reads this line BEFORE a reset too,
						   // to know the batch left something to forget.
						   m_console.Print(std::format(
							   "  stashed maps={} ents={} states={} levels={}", t.stashedMaps,
							   t.stashedEnts, t.stashedStates, t.stashedLevels));
						   m_console.Print(std::format(
							   "  throw={:.2f},{:.2f},{:.2f},{:.2f} kindle={:.3f}",
							   t.throwCooldown[0], t.throwCooldown[1], t.throwCooldown[2],
							   t.throwCooldown[3], t.kindleClock));
					   });

	// --- the arena (docs/eval-harness.md) -----------------------------------
	// Carve a controlled space into the loaded map and empty the world into it.
	// Writes NO files: the editor's new-level button would author a .map/.ent
	// into the git tree, which an eval must not do on every run.
	m_console.Register(
		{.name = "arena",
		 .group = CmdGroup::Simulation,
		 .params = "<open|corridor|deadend|tjunction> [w] [h]",
		 .summary = "carve a test arena into the map and empty the world into it"},
		[this](const std::vector<std::string>& args) {
			if (!Need(m_console, args, 1)) return;
			DungeonWorld::ArenaShape shape{};
			if (!DungeonWorld::ArenaShapeFromName(args[0], shape)) {
				m_console.Refuse("unknown shape: " + args[0] +
								 " (open|corridor|deadend|tjunction)");
				return;
			}
			const int w = args.size() > 1 ? std::atoi(args[1].c_str()) : 9;
			const int h = args.size() > 2 ? std::atoi(args[2].c_str()) : w;
			DungeonWorld::ArenaInfo info;
			if (!m_world->BuildArena(shape, w, h, info)) {
				m_console.Refuse("arena: refused (see the log)");
				return;
			}
			// The bounds are PRINTED because a script cannot read a return value
			// — it can only hardcode cells and have a human check in the log
			// that they were the cells it got. Reported as the extent ACTUALLY
			// carved rather than as the arguments: a corridor ignores `h`, and
			// echoing the request would have had `arena corridor 11` claim an
			// 11x11 room in the one record anybody reads.
			m_console.Print(std::format(
				"arena {} {}x{}  floor {},{}..{},{}  centre {},{}", args[0],
				info.x1 - info.x0 + 1, info.z1 - info.z0 + 1, info.x0, info.z0,
				info.x1, info.z1, info.cx, info.cz));
		});

	// WALK. `tp` puts the party somewhere; this makes them GO there, which is a
	// different measurement: an encounter that begins already adjacent skips the
	// approach, and the approach is where the monster notices you, closes the
	// distance, and the corridor decides how many of them can reach you at once.
	//
	// The steps are QUEUED as the same discrete MoveActions a key press or a HUD
	// arrow produces — they play out over the following `step`, at the party's
	// own pace, rather than teleporting a cell at a time. A blocked step is
	// simply refused by Party::Act, as it would be for a player walking into a
	// wall, so `forward 20` down a six-cell corridor stops at the end.
	m_console.Register({.name = "forward",
						.group = CmdGroup::Party,
						.params = "[n]",
						.summary = "queue party steps forward, walked at the party's own pace"},
					   [this](const std::vector<std::string>& args) {
						   const int n = args.empty() ? 1 : std::atoi(args[0].c_str());
						   if (n < 1) {
							   m_console.Refuse("forward needs a positive count");
							   return;
						   }
						   m_world->GetHarness().pendingSteps += n;
						   m_console.Print(std::format("forward x{}", n));
					   });

	// Monsters hold still while everything that happens TO them keeps running.
	// A geometry probe's instruments must not wander off the cells they measure.
	// `hold` also keeps every monster from NOTICING the party, and an `alloctest`
	// window's first armed frame lets them go (Harness::frozenHeld).
	m_console.Register({.name = "freeze",
						.group = CmdGroup::Monsters,
						.params = "[on|off|hold]",
						.summary = "stop monsters acting while effects on them keep running"},
					   [this](const std::vector<std::string>& args) {
						   DungeonWorld::Harness& h = m_world->GetHarness();
						   if (args.empty()) {
							   m_console.Print(std::format(
								   "freeze {}", h.frozenHeld ? "held" : (h.frozen ? "on" : "off")));
							   return;
						   }
						   if (args[0] == "hold") {
							   h.frozen = h.frozenHeld = true;
							   m_console.Print("freeze held until an alloctest window opens");
							   return;
						   }
						   const bool on = args[0] == "on" || args[0] == "1";
						   h.frozen = on;
						   h.frozenHeld = false;
						   m_console.Print(std::format("freeze {}", on ? "on" : "off"));
					   });

	// Detonate a spell's authored blast at a cell — no caster, no mana, no skill
	// roll, no bolt flight. The geometry question asked directly.
	//
	// A blast plays out over TICKS (blast_rate seconds apart), so a script must
	// `step` afterwards to let it land; detonating and reading `monsters` in the
	// same breath measures the moment before it went off.
	m_console.Register({.name = "blast",
						.group = CmdGroup::Combat,
						.params = "<spell> <x> <z>",
						.summary = "detonate a spell's authored blast at a cell, no caster"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 3)) return;
						   const int x = std::atoi(args[1].c_str());
						   const int z = std::atoi(args[2].c_str());
						   if (!m_world->DetonateSpell(args[0], x, z)) {
							   m_console.Refuse(std::format(
								   "blast: refused '{}' (unknown spell, or it has "
								   "no blast_force)",
								   args[0]));
							   return;
						   }
						   m_console.Print(std::format("blast {} at {},{}", args[0],
													   x, z));
					   });

	// Launch a spell's bolt at the party from a cell, as a monster casts it, down
	// the QUADRANT LANE of a roster slot - the one thing a real caster will not
	// do: aim at a lane nobody stands in. That is how a script stages the shot
	// that flies past the party and breaks on the wall behind it (code-review
	// C44). Like `blast` it needs a `step` afterwards to fly.
	m_console.Register({.name = "bolt",
						.group = CmdGroup::Combat,
						.params = "<spell> <x> <z> [slot 0-3]",
						.summary = "launch a spell's bolt at the party down a slot's lane, no caster"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 3)) return;
						   const int x = std::atoi(args[1].c_str());
						   const int z = std::atoi(args[2].c_str());
						   const int slot = args.size() > 3 ? std::atoi(args[3].c_str()) : -1;
						   if (slot > 3) {
							   m_console.Refuse(std::format("bolt: slot {} is not 0-3", slot));
							   return;
						   }
						   if (!m_world->ShootSpellBolt(args[0], x, z, slot)) {
							   m_console.Refuse(std::format(
								   "bolt: refused '{}' from {},{} (unknown spell, no bolt, "
								   "or off the party's row and column)",
								   args[0], x, z));
							   return;
						   }
						   m_console.Print(slot >= 0
											   ? std::format("bolt {} from {},{} in slot {}'s lane",
															 args[0], x, z, slot)
											   : std::format("bolt {} from {},{} down the middle",
															 args[0], x, z));
					   });

	// Loose a monster's plain shot at the party's side from a square's centre -
	// no monster, so nothing kites, cools down or decides when. A check places a
	// flight to the square with it (code-review C48: a 60x frame must not carry a
	// shot over the party or through a wall). Damage 1 and accuracy 1000 by
	// default: a shot that lands but for a fumble, and hurts nobody much.
	m_console.Register({.name = "shot",
						.group = CmdGroup::Combat,
						.params = "<x> <z> <n|e|s|w> [damage] [accuracy]",
						.summary = "loose a plain monster shot from a square, at the party's side"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 3)) return;
						   const int x = std::atoi(args[0].c_str());
						   const int z = std::atoi(args[1].c_str());
						   const char d = args[2].empty() ? '?' : static_cast<char>(
																	  std::tolower(static_cast<unsigned char>(args[2][0])));
						   const char* kDirs = "nesw";
						   const char* at = std::strchr(kDirs, d);
						   if (!at || d == '\0') {
							   m_console.RefuseUsage();
							   return;
						   }
						   const auto dir = static_cast<Direction>(at - kDirs);
						   const float damage =
							   args.size() > 3 ? static_cast<float>(std::atof(args[3].c_str())) : 1.0f;
						   const float accuracy =
							   args.size() > 4 ? static_cast<float>(std::atof(args[4].c_str())) : 1000.0f;
						   if (!m_world->LaunchShot(x, z, dir, damage, accuracy)) {
							   m_console.Refuse(std::format("shot: {},{} is not open floor", x, z));
							   return;
						   }
						   m_console.Print(std::format("shot from {},{} {} (damage {:.1f}, accuracy {:.0f})",
													   x, z, args[2], damage, accuracy));
					   });

	// Place a monster, live. The editor's placement path (AddMonster) refuses an
	// unwalkable or occupied cell, and so does this — reported rather than
	// silent, because a spawn that did not happen is an encounter that is not
	// the one the script described.
	// `up` (any word after the cell) stands it up at once: a kit skeleton
	// otherwise spends 9.5-14 s rising and holding still (StandLastMonster).
	// `pierce` gives it a piercing edge, its criticals going under armour
	// (PierceLastMonster) - no authored monster has one.
	// `share` lets it join a square monsters of its size already stand in, in the
	// next free slot, as a level's records can (a bone swarm is four a square);
	// without it a spawn wants the square to itself, as the editor's brush does.
	m_console.Register({.name = "spawn",
						.group = CmdGroup::Monsters,
						.params = "<type> <x> <z> [n|e|s|w] [strength] [up] [pierce] [share]",
						.summary = "place a monster live, optionally scaling its hp and damage"},
					   [this](const std::vector<std::string>& given) {
						   if (!Need(m_console, given, 3)) return;
						   std::vector<std::string> args;
						   bool up = false, pierce = false, share = false;
						   for (size_t i = 0; i < given.size(); ++i) {
							   if (i >= 3 && given[i] == "up") up = true;
							   else if (i >= 3 && given[i] == "pierce") pierce = true;
							   else if (i >= 3 && given[i] == "share") share = true;
							   else args.push_back(given[i]);
						   }
						   const int x = std::atoi(args[1].c_str());
						   const int z = std::atoi(args[2].c_str());
						   Direction facing = Direction::South;
						   if (args.size() > 3) {
							   switch (std::tolower(
								   static_cast<unsigned char>(args[3][0]))) {
							   case 'n': facing = Direction::North; break;
							   case 'e': facing = Direction::East; break;
							   case 's': facing = Direction::South; break;
							   case 'w': facing = Direction::West; break;
							   default: break;
							   }
						   }
						   // An optional 5th argument SCALES this instance's hp and
						   // damage, leaving its catalog entry alone — for
						   // sweeping difficulty finely between authored types.
						   const float strength =
							   args.size() > 4
								   ? static_cast<float>(std::atof(args[4].c_str()))
								   : 1.0f;
						   if (!m_world->AddMonster(args[0], x, z, facing, share)) {
							   m_console.Refuse(std::format(
								   "spawn: refused '{}' at {},{} (unknown type, "
								   "not walkable, or cell taken{})",
								   args[0], x, z, share ? " - no free slot for its size" : ""));
							   return;
						   }
						   if (strength > 0.0f && strength != 1.0f)
						   m_world->ScaleLastMonster(strength);
						   if (up) m_world->StandLastMonster();
						   if (pierce) m_world->PierceLastMonster();
					   m_console.Print(std::format("spawned {} at {},{} x{:.2f}{}{}{}",
											   args[0], x, z, strength, up ? " up" : "",
											   pierce ? " pierce" : "", share ? " share" : ""));
					   });

	// A monster kind's CLIP TABLE, read and set live - what the editor's monster
	// config dialog does through the same two calls, without its window, and like
	// it unsaved (the .cat is untouched). It is how a script authors one more
	// cosmetic clip and shows a seeded sweep's combat does not move for it
	// (code-review C73: clip picks draw from their own stream). Setting a state
	// replaces its list and marks it supported; a clip the model does not ship is
	// REFUSED, where the dialog's apply would drop it silently.
	m_console.Register(
		{.name = "monsterclips",
		 .group = CmdGroup::Monsters,
		 .params = "<type> [<state> <clip> ...]",
		 .summary = "print a monster kind's clips per state, or set one state's (live, unsaved)"},
		[this](const std::vector<std::string>& args) {
			if (!Need(m_console, args, 1)) return;
			const std::string& type = args[0];
			// The force-load aborts on a missing model, so guard it as the dialog does.
			if (!m_project.monsters.Find(type) || !m_world->MonsterModelAvailable(type)) {
				m_console.Refuse(std::format("monsterclips: no monster type '{}' with a model", type));
				return;
			}
			DungeonWorld::AnimSupport supported;
			DungeonWorld::AnimClips clips;
			m_world->MonsterAnimConfig(type, supported, clips);
			const auto line = [&](int i) {
				std::string s = std::format("  {} =", anim::StateName(static_cast<anim::CreatureState>(i)));
				for (const std::string& c : clips[static_cast<size_t>(i)]) s += " " + c;
				return s;
			};
			if (args.size() == 1) {
				m_console.Print(std::format("monsterclips {}:", type));
				for (int i = 0; i < anim::kCreatureStateCount; ++i)
					if (!clips[static_cast<size_t>(i)].empty()) m_console.Print(line(i));
				return;
			}
			const std::optional<anim::CreatureState> state = anim::ParseState(args[1]);
			if (!state || args.size() < 3) {
				m_console.RefuseUsage();
				return;
			}
			const std::vector<std::string> shipped = m_world->MonsterClipNames(type);
			std::vector<std::string> list;
			for (size_t a = 2; a < args.size(); ++a) {
				if (std::find(shipped.begin(), shipped.end(), args[a]) == shipped.end()) {
					m_console.Refuse(std::format("monsterclips: {}'s model has no clip '{}'", type,
												 args[a]));
					return;
				}
				list.push_back(args[a]);
			}
			const int i = static_cast<int>(*state);
			clips[static_cast<size_t>(i)] = std::move(list);
			supported[static_cast<size_t>(i)] = true;
			m_world->ApplyMonsterAnimConfig(type, supported, clips);
			m_console.Print(std::format("monsterclips {} set:", type));
			m_console.Print(line(i));
		});

	// --- measuring an encounter (docs/eval-harness.md) ----------------------
	// Without this a measured encounter is the party STANDING STILL BEING HIT.
	// PartyAttack is driven by a hand-slot click or `swing`, so the first
	// two-tier comparison had the monster finish on full hp in both rungs and
	// still looked like a complete result.
	// `hold` turns it on PARKED until alloctest's first armed frame (the held
	// autocast's rule, Harness::autoAttackHeld): tools\AllocTest.ps1 -Swing puts
	// the party's first swing of the session inside its window that way.
	m_console.Register({.name = "autoattack",
						.group = CmdGroup::Simulation,
						.params = "[on|off|hold]",
						.summary = "make the party swing on its own whenever off cooldown"},
					   [this](const std::vector<std::string>& args) {
						   DungeonWorld::Harness& h = m_world->GetHarness();
						   // `hold` leaves it ON but held (TickAutoAttack skips a
						   // held one); the next `alloctest` window's first armed
						   // frame clears the hold, so its swings start there.
						   if (!args.empty()) {
							   h.autoAttack = args[0] == "on" || args[0] == "1" || args[0] == "hold";
							   h.autoAttackHeld = args[0] == "hold";
						   }
						   if (h.autoAttackHeld)
							   m_console.Print("autoattack held until an alloctest window opens");
						   else
							   m_console.Print(std::format("autoattack {}", h.autoAttack ? "on" : "off"));
					   });

	// CASTING ON A CLOCK (DungeonWorld::Harness::AutoCast). Each call adds one
	// (member, spell) to a round-robin that fires one entry every `every`
	// seconds of SIM time, so a frozen world casts nothing. The member is taught
	// the spell's symbols here, and the harness pays the mana at each cast -
	// the rotation measures what a cast DOES, not whether it can be afforded.
	// `bolt` adds a SHOT AT THE PARTY instead: `bolt`'s launch, optionally met by
	// `castsvc repel`'s, on the same clock - a world frame, which the console's
	// own never is (tools\AllocTest.ps1 -Burst).
	m_console.Register(
		{.name = "autocast",
		 .group = CmdGroup::Simulation,
		 .params = "[<member> <spell> [every]]\n"
				   "bolt <spell> <x> <z> <slot -1..3> [repel <power> <member>] [every]\n"
				   "hold\n"
				   "off",
		 .summary = "cast spells in a round-robin on a sim-time clock"},
		[this](const std::vector<std::string>& args) {
			DungeonWorld::Harness::AutoCast& ac = m_world->GetHarness().autoCast;
			// "off" only - a bare "0" is member 0, the first caster.
			if (!args.empty() && args[0] == "off") {
				ac = {};
				m_console.Print("autocast off");
				return;
			}
			// Park the rotation until the next `alloctest` window opens (see
			// AutoCast::held); the next cast then fires on its first frame.
			if (!args.empty() && args[0] == "hold") {
				ac.held = true;
				ac.timer = 0.0f;
				m_console.Print("autocast held until an alloctest window opens");
				return;
			}
			if (args.size() < 2) {
				m_console.Print(std::format("autocast: {} entries every {:.2f}s",
											ac.count, ac.every));
				for (int i = 0; i < ac.count; ++i) {
					const auto& e = ac.entries[static_cast<size_t>(i)];
					if (e.bolt)
						m_console.Print(std::format(
							"  bolt {} from {},{} in slot {}{}: {} shot, {} failed", e.Spell(),
							e.x, e.z, e.slot,
							e.repel > 0.0f ? std::format(" repel {:.2f} by member {}",
														 e.repel, e.member)
										   : std::string(),
							e.cast, e.failed));
					else
						m_console.Print(std::format("  member {} casts {}: {} cast, {} failed",
													e.member, e.Spell(), e.cast, e.failed));
				}
				return;
			}
			using Entry = DungeonWorld::Harness::AutoCast::Entry;
			if (args[0] == "bolt") {
				// bolt <spell> <x> <z> <slot> [repel <power> <member>] [every]
				if (args.size() < 5) {
					m_console.RefuseUsage();
					return;
				}
				size_t at = 5;
				float power = 0.0f;
				int member = 0;
				if (args.size() > at && args[at] == "repel") {
					if (args.size() < at + 3) {
						m_console.RefuseUsage();
						return;
					}
					power = static_cast<float>(std::atof(args[at + 1].c_str()));
					member = std::atoi(args[at + 2].c_str());
					if (!(power > 0.0f)) {
						m_console.Refuse("autocast bolt: a repel's power must be positive");
						return;
					}
					at += 3;
				}
				if (args.size() > at + 1) {
					m_console.RefuseUsage();
					return;
				}
				const int slot = std::atoi(args[4].c_str());
				if (slot < -1 || slot > 3 || member < 0 ||
					static_cast<size_t>(member) >= m_characters.size()) {
					m_console.Refuse("autocast bolt: a slot is -1..3 and a repel's member "
									 "is in the party");
					return;
				}
				if (!m_world->FindSpell(args[1])) {
					m_console.Refuse("autocast: no spell '" + args[1] + "'");
					return;
				}
				if (ac.count == DungeonWorld::Harness::AutoCast::kMaxEntries ||
					args[1].size() >= sizeof(Entry::spell)) {
					m_console.Refuse("autocast: rotation full (or id too long)");
					return;
				}
				Entry& e = ac.entries[static_cast<size_t>(ac.count++)];
				e = {};
				e.bolt = true;
				e.member = member;
				std::memcpy(e.spell, args[1].data(), args[1].size());
				e.len = static_cast<u8>(args[1].size());
				e.x = std::atoi(args[2].c_str());
				e.z = std::atoi(args[3].c_str());
				e.slot = slot;
				e.repel = power;
				ac.every = args.size() > at ? static_cast<float>(std::atof(args[at].c_str()))
											: (ac.every > 0.0f ? ac.every : 0.5f);
				ac.timer = 0.0f;
				m_console.Print(std::format("autocast += bolt {} from {},{} (every {:.2f}s, {} in rotation)",
											args[1], e.x, e.z, ac.every, ac.count));
				return;
			}
			const int m = std::atoi(args[0].c_str());
			if (m < 0 || static_cast<size_t>(m) >= m_characters.size()) {
				m_console.Refuse("autocast: no such member");
				return;
			}
			const Spell* spell = m_world->FindSpell(args[1]);
			if (!spell) {
				m_console.Refuse("autocast: no spell '" + args[1] + "'");
				return;
			}
			if (ac.count == DungeonWorld::Harness::AutoCast::kMaxEntries ||
				args[1].size() >= sizeof(Entry::spell)) {
				m_console.Refuse("autocast: rotation full (or id too long)");
				return;
			}
			Entry& e = ac.entries[static_cast<size_t>(ac.count++)];
			e.member = m;
			std::memcpy(e.spell, args[1].data(), args[1].size());
			e.len = static_cast<u8>(args[1].size());
			for (const SpellSymbol s : spell->Sequence()) m_characters[m].Learn(s);
			ac.every = args.size() > 2 ? static_cast<float>(std::atof(args[2].c_str()))
									   : (ac.every > 0.0f ? ac.every : 0.5f);
			ac.timer = 0.0f;
			m_console.Print(std::format("autocast += member {} {} (every {:.2f}s, {} in rotation)",
										m, args[1], ac.every, ac.count));
		});

	// The encounter's numbers, in one machine-readable line. `tally reset` marks
	// the start of a rung; `tally` prints what has happened since.
	m_console.Register({.name = "tally",
						.group = CmdGroup::Simulation,
						.params = "[reset]",
						.summary = "print the encounter counters as one key=value line"},
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty() && args[0] == "reset") {
							   m_world->GetHarness().tally = {};
							   m_console.Print("tally reset");
							   return;
						   }
						   // WHAT THE FIELDS MEAN, because two of them were
						   // guessed wrong by the audit that checked them
						   // (docs/eval-audit.md):
						   //
						   //   dealt   post-mitigation damage delivered to
						   //           monsters, INCLUDING the part that
						   //           overshoots a kill. Measured: a blast
						   //           doing 15.3 to a 6 hp target reports
						   //           15.3, not 6. Against a survivor it
						   //           reconciles exactly with the hp drop.
						   //   taken   the same on the party's side, and it is
						   //           PARTY-WIDE — a suite printing one
						   //           member's health beside it is comparing
						   //           two different populations.
						   //   swings  the party's MELEE swings only, so a
						   //           blast reports damage with zero swings
						   //           and hitrate `n/a`.
						   //   downed  distinct MEMBERS, not falls.
						   //   bolthits/boltmisses/expired/blasts  the carriers
						   //           (DungeonWorld::Tally says which count what).
						   //   struck/pierced  the MONSTERS' melee blows that
						   //           landed on a member, and the criticals among
						   //           them a piercing edge drove under the armour.
						   //   wallstops/stoppedin  the expiries that stopped
						   //           against a wall or a shut door, and the
						   //           square the last one stopped IN (`-`
						   //           with none) - not the open square in
						   //           front, where its flight ended.
						   //   partybursts/wardturns  burst bolts that went
						   //           off on contact with the party, and the
						   //           monster bolts a Wind Ward turned there.
						   //   repelweakened/repelturned/repelspent  what a
						   //           gust's repel did to each shot it met;
						   //           a spent one fell and is not in turned.
						   //   mswings the MONSTERS' melee swings, hit or miss.
						   //   mshots  the MONSTERS' ranged shots - only a
						   //           kiter fires them.
						   //   landat  the square the last throw came down on
						   //           (landed or burst), `-` for none.
						   //   expat   the square the last carrier that stopped
						   //           without striking stopped in - a wall's
						   //           own square when it broke on one.
						   m_console.Print(TallyLine());
					   });

	// A script cannot otherwise tell whether it is measuring anything at all.
	m_console.Register({.name = "state",
						.group = CmdGroup::Console,
						.summary = "print what the app is doing (loading/menu/playing/...)"},
					   [this](const std::vector<std::string>&) {
						   m_console.Print(std::format("state {}", StateName()));
					   });

	m_console.Register({.name = "seed",
						.group = CmdGroup::Simulation,
						.params = "<n>",
						.summary = "reseed the combat RNG"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   const auto n = static_cast<u32>(
							   std::strtoul(args[0].c_str(), nullptr, 10));
						   m_world->SeedCombat(n);
						   m_console.Print(std::format("combat seed {}", n));
					   });

	// Without this a stepped run is a fiction: the AI's four bucket workers tick
	// on WALL-CLOCK, so simulating thirty seconds inside a few frames lets the
	// monsters think perhaps twice. See ai::AsyncDirector::SetLockstep.
	m_console.Register({.name = "lockstep",
						.group = CmdGroup::Monsters,
						.params = "[on|off]\n"
								  "stats",
						.summary = "drive monster AI from sim time, not the wall clock"},
					   [this](const std::vector<std::string>& args) {
						   if (args.empty()) {
							   m_console.Print(std::format(
								   "lockstep {}", m_world->LockstepAI() ? "on" : "off"));
							   return;
						   }
						   // What the inline compute has done since lockstep last came
						   // on - rest turns it on too - so a harness can tell an AI
						   // that thought on the main thread, and found paths, from
						   // one that sat idle (AllocTest -Rest).
						   if (args[0] == "stats") {
							   const ai::AsyncDirector::InlineStats& s = m_world->LockstepStats();
							   m_console.Print(std::format(
								   "lockstep stats: {} ticks={} plans={} paths={} longest={}",
								   m_world->LockstepAI() ? "on" : "off", s.ticks, s.plans,
								   s.paths, s.longest));
							   return;
						   }
						   const bool on = args[0] == "on" || args[0] == "1";
						   m_world->SetLockstepAI(on);
						   m_console.Print(std::format("lockstep {}", on ? "on" : "off"));
					   });

	// THE ONE WALL-CLOCK WAIT, and it is the point: with lockstep off the bucket
	// workers think on the wall clock, and some defects live exactly there - a
	// worker still thinking while the world is frozen on the title (code-review
	// C52). Blocks the main thread until every bucket has published `batches`
	// more plan batches (default 2, so at least one whole tick BEGAN after the
	// call) and refuses rather than wait for nothing: under lockstep the workers
	// are paused and publish nothing.
	m_console.Register({.name = "aiwait",
						.group = CmdGroup::Monsters,
						.params = "[batches]",
						.summary = "block until every AI worker has published more plans (wall clock)"},
					   [this](const std::vector<std::string>& args) {
						   if (!m_world) {
							   m_console.Refuse("aiwait: no world");
							   return;
						   }
						   if (m_world->LockstepAI()) {
							   m_console.Refuse("aiwait: lockstep is on - its workers are "
												"paused and publish nothing");
							   return;
						   }
						   const int want =
							   args.empty() ? 2 : std::max(1, std::atoi(args[0].c_str()));
						   constexpr int kBuckets = ai::Scheduler::kBucketCount;
						   static_assert(kBuckets == 4, "the readout below names four buckets");
						   uint64_t from[kBuckets], now[kBuckets];
						   for (int b = 0; b < kBuckets; ++b) from[b] = m_world->AIPlanSeq(b);
						   // The slowest bucket ticks every 2 s, so two batches
						   // need about 4; the limit leaves room for a loaded
						   // machine and the governor.
						   constexpr double kLimitMs = 12000.0;
						   const auto t0 = std::chrono::steady_clock::now();
						   double ms = 0.0;
						   for (;;) {
							   bool all = true;
							   for (int b = 0; b < kBuckets; ++b) {
								   now[b] = m_world->AIPlanSeq(b);
								   if (now[b] - from[b] < static_cast<uint64_t>(want)) all = false;
							   }
							   ms = std::chrono::duration<double, std::milli>(
										std::chrono::steady_clock::now() - t0)
										.count();
							   if (all || ms > kLimitMs) break;
							   std::this_thread::sleep_for(std::chrono::milliseconds(5));
						   }
						   const std::string seqs =
							   std::format("{}/{}/{}/{}", now[0] - from[0], now[1] - from[1],
										   now[2] - from[2], now[3] - from[3]);
						   bool all = true;
						   for (int b = 0; b < kBuckets; ++b)
							   if (now[b] - from[b] < static_cast<uint64_t>(want)) all = false;
						   if (!all) {
							   m_console.Refuse(std::format(
								   "aiwait: not every bucket published {} batch(es) in {:.0f} ms "
								   "(published {})",
								   want, ms, seqs));
							   return;
						   }
						   m_console.Print(std::format(
							   "aiwait: every bucket published {}+ batch(es) in {:.0f} ms ({})",
							   want, ms, seqs));
					   });

	// Advance the world by sim seconds, now, in fixed ticks. Reports what it
	// actually RAN rather than what was asked for: a short answer means the run
	// hit the ceiling, changed level, ended its rest or lost the party, and says
	// which - an eval that silently measured less time than it believes is worse
	// than one that failed outright.
	m_console.Register({.name = "step",
						.group = CmdGroup::Simulation,
						.params = "<seconds>",
						.summary = "advance the world by sim seconds now, in fixed ticks"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   const float secs =
							   static_cast<float>(std::atof(args[0].c_str()));
						   if (secs <= 0.0f) {
							   m_console.Refuse(
								   "step needs a positive number of seconds");
							   return;
						   }
						   // SAY WHY, never a bare zero. Dev commands reach the
						   // world from the MENU too, so a script whose party
						   // has wiped would otherwise watch `tp` and `monsters`
						   // answer normally while every `step` quietly did
						   // nothing — a whole suite of encounters that never
						   // ran, reported as results.
						   if (std::string_view(StateName()) != "playing") {
							   m_console.Refuse(std::format(
								   "step: not playing (state: {}) — nothing stepped",
								   StateName()));
							   return;
						   }
						   // Warned, not refused: stepping without lockstep is
						   // still useful for eyeballing, and silently producing
						   // a meaningless number is the thing to avoid.
						   if (!m_world->LockstepAI())
							   m_console.Print("warning: lockstep is OFF — monsters "
											   "will barely think during this step");
						   StepStop why = StepStop::Complete;
						   const int ran = StepWorld(secs, why);
						   const float got = static_cast<float>(ran) /
											 kStepTicksPerSecond;
						   // A short run says WHY it stopped (StepStopReason, the
						   // words `rest until` uses too): a rested step's seconds
						   // ARE the length of the rest, which is the number a
						   // supply measurement is after, and a wipe's are how
						   // long the party lasted.
						   const std::string reason = StepStopReason(why);
						   m_console.Print(std::format("stepped {} ticks ({:.2f}s){}{}",
													   ran, got, reason.empty() ? "" : " - ",
													   reason));
						   // THE CEILING IS A REFUSAL, not a footnote. This line
						   // has always reported the truth and nothing read it:
						   // `step 3600` runs 3333.33s, and supplies.eval and
						   // rest.eval both called that "an hour" for a whole
						   // release (docs/eval-audit.md F3). A measurement taken
						   // over 92.6% of the time it claims is a wrong number,
						   // not a rounding note.
						   if (why == StepStop::Ceiling)
							   m_console.Refuse(std::format(
								   "step: asked for {:.2f}s but one call tops out "
								   "at {:.2f}s — this measured {:.2f}s LESS than "
								   "the script believes; split it across calls",
								   secs, kMaxStepSeconds, secs - got));
					   });

	// PLAY FRAMES, where `step` runs sim seconds (code-review C64). `step` feeds
	// the world its fixed ticks directly and never applies rest's multiplier, so
	// what that multiplier does to a frame was invisible to every script. This
	// runs `n` frames as a playing frame runs the world: `1/fps` real seconds
	// through Game::WorldDt - the one multiplier site - into the world's Update,
	// which takes a long result in fixed ticks. The dev `timescale` is left out
	// (a script holds it at 0 between lines). `whole` runs each frame's world dt
	// as ONE step, the ticks bypassed - what every resting frame did before C64 -
	// so a check can hand a 60x dt to what must cope with one on its own (a
	// flight's half-square steps, C48). Stops early, and says why, when a rest
	// that was on ends or the party changes level. The wall time a frame took is
	// in the line: the cost the tick cap bounds. So is what the LAST frame left
	// owed to the next (`owed`, the world's carry), and it has to be read here:
	// the runner's frames between two script lines are zero-dt whole steps at
	// timescale 0, which clear the carry, so a second `frames` line never sees
	// what the first one left.
	m_console.Register({.name = "frames",
						.group = CmdGroup::Simulation,
						.params = "<n> [fps] [whole]",
						.summary = "run n play frames of the world through the rest multiplier"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   const int n = std::atoi(args[0].c_str());
						   float fps = 60.0f;
						   bool whole = false;
						   for (size_t i = 1; i < args.size(); ++i) {
							   if (args[i] == "whole") whole = true;
							   else fps = static_cast<float>(std::atof(args[i].c_str()));
						   }
						   if (n < 1 || fps <= 0.0f) {
							   m_console.RefuseUsage();
							   return;
						   }
						   if (std::string_view(StateName()) != "playing") {
							   m_console.Refuse(std::format(
								   "frames: not playing (state: {}) - nothing ran", StateName()));
							   return;
						   }
						   static const Input kNoInput;
						   const bool resting = m_world->Resting();
						   // The AI's thinks inside these frames alone (lockstep's
						   // inline computes): `lockstep stats` on the next line would
						   // also count the frames that run the script's lines between.
						   const uint64_t thinksBefore = m_world->LockstepStats().ticks;
						   m_world->GetHarness().wholeSteps = whole;
						   int ran = 0, ticks = 0, capped = 0;
						   float world = 0.0f;
						   std::string stop;
						   const auto t0 = std::chrono::steady_clock::now();
						   while (ran < n) {
							   const float wdt = WorldDt(1.0f / fps, 1.0f);
							   m_world->Update(kNoInput, wdt, m_time, /*acceptInput=*/false);
							   m_time += wdt;
							   ++ran;
							   const DungeonWorld::UpdateRun& u = m_world->LastUpdate();
							   ticks += u.ticks;
							   world += u.seconds;
							   capped += u.capped ? 1 : 0;
							   // Followed nowhere, as `step` does: a run that changed
							   // level is no longer measuring what it set up.
							   if (m_world->ConsumeLevelTransition()) {
								   stop = " - stopped: the party changed level";
								   break;
							   }
							   if (resting && !m_world->Resting()) {
								   stop = std::format(" - rest ended: {}", m_world->RestEndReason());
								   break;
							   }
						   }
						   m_world->GetHarness().wholeSteps = false;
						   const double ms = std::chrono::duration<double, std::milli>(
												 std::chrono::steady_clock::now() - t0)
												 .count();
						   m_console.Print(std::format(
							   "frames: ran {} of {} at {:.0f} fps{}: world {:.2f}s in {} steps, "
							   "{} capped, {:.4f}s owed, {} AI thinks, {} in flight, {:.2f} ms a "
							   "frame{}",
							   ran, n, fps, whole ? " whole" : "", world, ticks, capped,
							   m_world->LastUpdate().owed,
							   m_world->LockstepStats().ticks - thinksBefore,
							   m_world->LiveProjectiles().size(), ms / ran, stop));
					   });
}

std::string Game::TallyLine() const {
	if (!m_world) return "TALLY (no world)";
	const DungeonWorld::Tally& t = m_world->GetHarness().tally;
	const int swings = t.hits + t.misses;
	// ONE LINE, key=value, so a sweep's output can be grepped and diffed
	// without parsing prose. Damage is in absolute POINTS, never a fraction of
	// health - the healing model is still to be designed, and fractions would
	// change meaning the day it lands.
	// `n/a` RATHER THAN 0.000 WHEN NOTHING SWUNG. A rate over no trials is not
	// zero, it is undefined, and printing 0.000 made "the party never swung"
	// look identical to "the party missed every time" - which is exactly the
	// pair a blast table (swings=0 by nature) sits next to (docs/eval-audit.md
	// F8). Parsers should read hitrate as [0-9.]+|n/a.
	const std::string rate =
		swings > 0 ? std::format("{:.3f}", static_cast<float>(t.hits) / swings)
				   : std::string("n/a");
	// The square the last wall stop stopped IN, `x,z`, or `-` with none - never
	// a -1,-1 a parser could take for a square.
	const std::string stoppedIn =
		t.wallStops > 0 ? std::format("{},{}", t.wallStopX, t.wallStopZ)
						: std::string("-");
	// The carrier counts go AFTER secs: Eval.ps1 parses the fields before it as
	// one fixed sequence.
	return std::format(
		"TALLY dealt={:.1f} taken={:.1f} swings={} hits={} misses={} hitrate={} "
		"crits={} fumbles={} slain={} downed={} secs={:.1f} bolthits={} "
		"boltmisses={} expired={} blasts={} drops={} lifts={} throws={} "
		"throwstrikes={} throwlandings={} sceneryticks={} doused={} struck={} "
		"pierced={} wallstops={} stoppedin={} partybursts={} wardturns={} "
		"repelweakened={} repelturned={} repelspent={} mswings={} mshots={} "
		"landat={} expat={} severefumbles={} fumbledrops={} clipdraws={}",
		t.dealt, t.taken, swings, t.hits, t.misses, rate, t.crits, t.fumbles,
		t.monstersSlain, t.membersDowned, t.seconds, t.boltHits, t.boltMisses,
		t.expiries, t.blasts, t.drops, t.lifts, t.throws, t.throwStrikes,
		t.throwLandings, t.sceneryTicks, t.fixturesDoused, t.struck, t.pierced,
		t.wallStops, stoppedIn, t.partyBursts, t.wardTurns, t.repelWeakened,
		t.repelTurned, t.repelSpent, t.monsterSwings, t.monsterShots,
		t.landX < 0 ? std::string("-") : std::format("{},{}", t.landX, t.landZ),
		t.expireX < 0 ? std::string("-") : std::format("{},{}", t.expireX, t.expireZ),
		t.severeFumbles, t.fumbleDrops, t.clipDraws);
}

} // namespace dungeon::game
