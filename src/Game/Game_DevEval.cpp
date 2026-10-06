// ============================================================================
// Game/Game_DevEval.cpp — the eval harness's dev-console commands.
//
// Split out of Game_DevCommands.cpp by concern (docs/eval-harness.md): the
// primitives that make a measurement mean anything (timescale/logecho/seed/
// lockstep/step/state), getting into a world without a mouse (newgame/reset),
// and staging and reading an encounter (arena/forward/freeze/blast/spawn/
// autoattack/tally).
// ============================================================================
#include "Game/Game.h"

#include "Game/DevCommandArgs.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <string_view>

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
							   m_console.Print("newgame: not wired yet");
							   return;
						   }
						   m_ui.onStartNewGame();
						   // A cold boot now runs a staged load with commands
						   // disabled, so a script's next line waits for the
						   // world by itself.
						   m_console.Print("starting a new game");
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
						   const bool fresh = !m_gameLoaded;
						   // TIMED, because the whole justification is the number:
						   // a level load is ~12000 ms and a run of hundreds of
						   // tests cannot pay it each time. If this ever creeps
						   // toward that, the recycling has stopped being worth
						   // its own risk and the reader should be able to see so.
						   const auto t0 = std::chrono::steady_clock::now();
						   if (!ResetForEval()) {
							   m_console.Print("reset: not wired yet");
							   return;
						   }
						   const double ms =
							   std::chrono::duration<double, std::milli>(
								   std::chrono::steady_clock::now() - t0)
								   .count();
						   m_console.Print(
							   fresh ? std::format("reset: loaded in {:.0f} ms "
												   "(nothing to recycle yet)", ms)
									 : std::format("reset: recycled in {:.0f} ms", ms));
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

	// Place a monster, live. The editor's placement path (AddMonster) refuses an
	// unwalkable or occupied cell, and so does this — reported rather than
	// silent, because a spawn that did not happen is an encounter that is not
	// the one the script described.
	// `up` (any word after the cell) stands it up at once: a kit skeleton
	// otherwise spends 9.5-14 s rising and holding still (StandLastMonster).
	// `pierce` gives it a piercing edge, its criticals going under armour
	// (PierceLastMonster) - no authored monster has one.
	m_console.Register({.name = "spawn",
						.group = CmdGroup::Monsters,
						.params = "<type> <x> <z> [n|e|s|w] [strength] [up] [pierce]",
						.summary = "place a monster live, optionally scaling its hp and damage"},
					   [this](const std::vector<std::string>& given) {
						   if (!Need(m_console, given, 3)) return;
						   std::vector<std::string> args;
						   bool up = false, pierce = false;
						   for (size_t i = 0; i < given.size(); ++i) {
							   if (i >= 3 && given[i] == "up") up = true;
							   else if (i >= 3 && given[i] == "pierce") pierce = true;
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
						   if (!m_world->AddMonster(args[0], x, z, facing)) {
							   m_console.Refuse(std::format(
								   "spawn: refused '{}' at {},{} (unknown type, "
								   "not walkable, or cell taken)",
								   args[0], x, z));
							   return;
						   }
						   if (strength > 0.0f && strength != 1.0f)
						   m_world->ScaleLastMonster(strength);
						   if (up) m_world->StandLastMonster();
						   if (pierce) m_world->PierceLastMonster();
					   m_console.Print(std::format("spawned {} at {},{} x{:.2f}{}{}",
											   args[0], x, z, strength, up ? " up" : "",
											   pierce ? " pierce" : ""));
					   });

	// --- measuring an encounter (docs/eval-harness.md) ----------------------
	// Without this a measured encounter is the party STANDING STILL BEING HIT.
	// PartyAttack is driven by a hand-slot click or `swing`, so the first
	// two-tier comparison had the monster finish on full hp in both rungs and
	// still looked like a complete result.
	m_console.Register({.name = "autoattack",
						.group = CmdGroup::Simulation,
						.params = "[on|off]",
						.summary = "make the party swing on its own whenever off cooldown"},
					   [this](const std::vector<std::string>& args) {
						   if (args.empty()) {
							   m_console.Print(std::format(
								   "autoattack {}",
								   m_world->GetHarness().autoAttack ? "on" : "off"));
							   return;
						   }
						   const bool on = args[0] == "on" || args[0] == "1";
						   m_world->GetHarness().autoAttack = on;
						   m_console.Print(std::format("autoattack {}", on ? "on" : "off"));
					   });

	// CASTING ON A CLOCK (DungeonWorld::Harness::AutoCast). Each call adds one
	// (member, spell) to a round-robin that fires one entry every `every`
	// seconds of SIM time, so a frozen world casts nothing. The member is taught
	// the spell's symbols here, and the harness pays the mana at each cast -
	// the rotation measures what a cast DOES, not whether it can be afforded.
	m_console.Register(
		{.name = "autocast",
		 .group = CmdGroup::Simulation,
		 .params = "[<member> <spell> [every]]\n"
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
					m_console.Print(std::format("  member {} casts {}: {} cast, {} failed",
												e.member, e.Spell(), e.cast, e.failed));
				}
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
			using Entry = DungeonWorld::Harness::AutoCast::Entry;
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
						.params = "[on|off]",
						.summary = "drive monster AI from sim time, not the wall clock"},
					   [this](const std::vector<std::string>& args) {
						   if (args.empty()) {
							   m_console.Print(std::format(
								   "lockstep {}", m_world->LockstepAI() ? "on" : "off"));
							   return;
						   }
						   const bool on = args[0] == "on" || args[0] == "1";
						   m_world->SetLockstepAI(on);
						   m_console.Print(std::format("lockstep {}", on ? "on" : "off"));
					   });

	// Advance the world by sim seconds, now, in fixed ticks. Reports what it
	// actually RAN rather than what was asked for: a short answer means the run
	// hit the ceiling or changed level, and an eval that silently measured less
	// time than it believes is worse than one that failed outright.
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
						   const bool wasResting = m_world->Resting();
						   StepStop why = StepStop::Complete;
						   const int ran = StepWorld(secs, why);
						   const float got = static_cast<float>(ran) /
											 kStepTicksPerSecond;
						   // A rested step says so, and says WHY it stopped: the
						   // seconds it ran ARE the length of the rest, which is
						   // the number a supply measurement is after.
						   std::string tail;
						   if (wasResting && !m_world->Resting())
							   tail = std::format(" — rest ended: {}",
												  m_world->RestEndReason());
						   else if (why == StepStop::LevelChange)
							   tail = " — stopped: the party changed level";
						   m_console.Print(std::format("stepped {} ticks ({:.2f}s){}",
													   ran, got, tail));
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
	// The carrier counts go AFTER secs: Eval.ps1 parses the fields before it as
	// one fixed sequence.
	return std::format(
		"TALLY dealt={:.1f} taken={:.1f} swings={} hits={} misses={} hitrate={} "
		"crits={} fumbles={} slain={} downed={} secs={:.1f} bolthits={} "
		"boltmisses={} expired={} blasts={} drops={} lifts={} throws={} "
		"throwstrikes={} throwlandings={} sceneryticks={} doused={} struck={} "
		"pierced={}",
		t.dealt, t.taken, swings, t.hits, t.misses, rate, t.crits, t.fumbles,
		t.monstersSlain, t.membersDowned, t.seconds, t.boltHits, t.boltMisses,
		t.expiries, t.blasts, t.drops, t.lifts, t.throws, t.throwStrikes,
		t.throwLandings, t.sceneryTicks, t.fixturesDoused, t.struck, t.pierced);
}

} // namespace dungeon::game
