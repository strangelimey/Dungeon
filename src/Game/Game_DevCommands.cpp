// ============================================================================
// Game/Game_DevCommands.cpp — split out of Game.cpp to keep files small (see Game.h).
// Dev-console command registration + its arg helpers.
// ============================================================================
#include "Game/Game.h"

#include "Assets/File.h"
#include "Core/AllocTrack.h"
#include "Core/Assert.h"
#include "Core/Diagnostics.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/StackTrace.h"
#include "Game/AssetUtil.h"
#include "UI/TreeInspector.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <stdexcept>
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
	m_console.Register(
		"alloctest", "measure N seconds (default 10) of steady frames; PASS = zero allocations",
		[this](const std::vector<std::string>& args) {
			if (!alloc::kEnabled) {
				m_console.Print("allocation tracking is compiled out of this build");
				return;
			}
			const float seconds =
				args.empty() ? 10.0f
							 : std::clamp(static_cast<float>(std::atof(args[0].c_str())),
										  1.0f, 600.0f);
			m_allocTestRemaining = seconds;
			// Generous: the window only spends on armed frames, and reaching one
			// costs a console close plus the 120-frame warm-up.
			m_allocTestDeadline = seconds * 3.0f + 15.0f;
			m_allocTestFrames = 0;
			m_allocTestStart = alloc::Stats();
			m_console.Print(std::format(
				"alloctest: {:.0f}s of steady frames — closing the console (frames only "
				"arm while it is shut); the result lands here and in dungeon.log",
				seconds));
			if (m_console.IsOpen()) m_console.Toggle();
		});
	m_console.Register(
		"allocpoke", "allocate on purpose for N seconds (proves the guard can fail)",
		[this](const std::vector<std::string>& args) {
			const float seconds =
				args.empty() ? 30.0f
							 : std::clamp(static_cast<float>(std::atof(args[0].c_str())),
										  1.0f, 600.0f);
			m_allocPokeRemaining = seconds;
			m_console.Print(std::format("allocpoke: allocating every frame for {:.0f}s",
										seconds));
			if (m_console.IsOpen()) m_console.Toggle();
		});
	// --- the one-pipeline check (Game/DamageLedger.h, docs/effects.md) --------
	// The same three-command shape the allocation guard uses, for the same
	// reason: a readout, an arming switch, and a way to make it FAIL on purpose.
	m_console.Register(
		"pipeline", "one-pipeline check: what moved health, and whether anything went around it",
		[this](const std::vector<std::string>&) {
			for (const std::string& line : m_world.DamageLedgerReport())
				m_console.Print(line);
		});
	m_console.Register(
		"pipelineguard", "arm the one-pipeline check: pipelineguard [on|off|strict on|strict off|reset]",
		[this](const std::vector<std::string>& args) {
			ledger::Ledger& led = m_world.DamageLedger();
			if (!args.empty()) {
				const std::string& a = args[0];
				if (a == "on" || a == "off") {
					led.Arm(a == "on");
					// An arming takes a FRESH baseline: whatever moved while it was
					// off is not a violation, it is simply unobserved, and reporting
					// it would make turning the check on look like finding a bug.
					m_world.RebaseDamageLedger();
				} else if (a == "strict") {
					led.SetStrict(args.size() < 2 || args[1] == "on");
				} else if (a == "reset") {
					led.ResetStats();
					m_world.RebaseDamageLedger();
				} else {
					m_console.Print("usage: pipelineguard [on|off|strict on|strict off|reset]");
					return;
				}
			}
			m_console.Print(std::format("pipelineguard: armed={} strict={}",
										led.Armed() ? "on" : "off",
										led.Strict() ? "on" : "off"));
		});
	m_console.Register(
		"pipelinepoke", "move health WITHOUT the pipeline (proves the check can fail): pipelinepoke [member]",
		[this](const std::vector<std::string>& args) {
			const size_t m =
				args.empty() ? 0 : static_cast<size_t>(std::atoi(args[0].c_str()));
			if (m >= m_characters.size()) {
				m_console.Print("no such member");
				return;
			}
			Character& c = m_characters[m];
			// A point either way, whichever direction the bar has room for — a
			// poke that clamps to no change would report nothing and read exactly
			// like a check that missed it.
			const float delta = c.health > 1.0f ? -1.0f : 1.0f;
			c.health += delta;
			m_console.Print(std::format(
				"pipelinepoke: {} health {:+.1f} with no DamageEvent — the next "
				"checkpoint should report it",
				c.name, delta));
		});
	m_console.Register(
		"allocguard", "steady-state allocation guard: status | strict on|off | reset",
		[this](const std::vector<std::string>& args) {
			const std::string sub = args.empty() ? "status" : args[0];
			if (sub == "strict") {
				if (!Need(m_console, args, 2, "usage: allocguard strict <on|off>")) return;
				alloc::SetStrict(args[1] == "on" || args[1] == "1");
				m_console.Print(std::format("strict mode {}",
											alloc::Strict() ? "ON — a violating frame will abort"
															: "off"));
				return;
			}
			if (sub == "reset") {
				alloc::ResetStats();
				m_console.Print("guard stats + reported-stack memory cleared");
				return;
			}
			if (!alloc::kEnabled) {
				m_console.Print("allocation tracking is compiled out of this build");
				return;
			}
			const alloc::GuardStats g = alloc::Stats();
			m_console.Print(std::format("armed {} frames, {} violating, {} allocs, "
										"{} call sites reported (strict {})",
										g.framesArmed, g.framesViolating, g.violations,
										g.stacksReported, alloc::Strict() ? "on" : "off"));
			m_console.Print(m_steadyFrames > 120
								? "this frame: steady (armed)"
								: std::format("this frame: settling ({} quiet frames)",
											  m_steadyFrames));
			alloc::ThreadReport threads[alloc::kMaxThreads];
			const int n = alloc::SnapshotAll(threads, alloc::kMaxThreads);
			for (int i = 0; i < n; ++i)
				m_console.Print(std::format("  {:<12} {:>10} allocs  {:>10} frees",
											threads[i].name, threads[i].counters.allocs,
											threads[i].counters.frees));
		});
	m_console.Register("loadstats",
					   "reprint the last staged load's per-task time/allocation table",
					   [this](const std::vector<std::string>&) {
						   LogLoadStats(/*echoToConsole=*/true);
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
	// A FINGERPRINT OF THE STATIC LAYER, not just its size. Every other readout
	// in the harness describes the party or the creatures standing on the map —
	// which is exactly how the first `reset` equivalence test came back
	// "identical" while the world was still an empty carved box. The WALKABLE
	// count is the load-bearing one: `arena` walls every cell before carving, so
	// a map that never came back shows up here and nowhere else.
	m_console.Register("mapinfo", "print dungeon size and a static-layer fingerprint",
					   [this](const std::vector<std::string>&) {
						   const DungeonMap& map = m_world.Map();
						   int walkable = 0;
						   for (int z = 0; z < map.Height(); ++z)
							   for (int x = 0; x < map.Width(); ++x)
								   if (map.IsWalkable(x, z)) ++walkable;
						   m_console.Print(std::format(
							   "{}x{} map, start {},{}, {} walkable, {} monsters, "
							   "{} torches, {} braziers",
							   map.Width(), map.Height(), map.StartX(), map.StartZ(),
							   walkable, m_world.MonsterCount(),
							   map.Sconces().size(), map.Braziers().size()));
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
	m_console.Register(
		"crashpoke",
		"break something on purpose: throw | worker | fault | assert (proves the "
		"health record catches it)",
		[this](const std::vector<std::string>& args) {
			const std::string what = args.empty() ? "throw" : args[0];

			// Thrown from a console callback, which runs inside Game::Update,
			// which runs inside the main loop's try — so this exercises the real
			// main-thread path, not a special case built to be caught.
			if (what == "throw")
				throw std::runtime_error("crashpoke: a deliberate main-thread throw");

			// A worker failing a DIFFERENT way every tick: the case that made the
			// log throttle key on the thread rather than the message.
			if (what == "worker") {
				const threads::WorkerId id = m_threads.Spawn(
					[](const threads::Tick& t) {
						throw std::runtime_error(
							std::format("crashpoke: deliberate failure on tick {}",
										t.iteration));
					},
					{"demo.thrower", 2.0f, /*watchdogMs=*/0});
				m_console.Print(std::format(
					"spawned THROWING worker #{} — it fails every tick and keeps "
					"running; watch `health` and dungeon.log",
					id));
				return;
			}

			// The two that END the process, which is the point: each should leave
			// a report and a minidump where today there is silence.
			if (what == "fault") {
				m_console.Print("crashpoke: dereferencing null — expect a crash report");
				volatile int* p = nullptr;
				*p = 1;
				return;
			}
			if (what == "assert") {
				m_console.Print("crashpoke: firing an assert — expect a crash report");
				DN_ASSERT(false, "crashpoke: a deliberate assertion failure");
				return;
			}
			m_console.Print("usage: crashpoke <throw|worker|fault|assert>");
		});
	m_console.Register(
		"health",
		"health record: `health` recent failures | `health <thread>` one thread's "
		"events + stacks | `health probe <id>` what a live worker is doing NOW",
		[this](const std::vector<std::string>& args) {
			// --- the probe ---------------------------------------------------
			// A STALLED thread has thrown nothing, so the record has nothing to
			// show: it is still running, just not finishing. The only way to
			// answer "what is it stuck on" is to go and look.
			if (!args.empty() && args[0] == "probe") {
				if (!Need(m_console, args, 2, "usage: health probe <worker id|name>"))
					return;
				// By id or by name — a name is what the THREADS panel shows and
				// what you actually remember ("demo.wedged", "ai.bucket2").
				const std::string& who = args[1];
				threads::WorkerId id = threads::kInvalidWorker;
				if (!who.empty() && std::isdigit(static_cast<unsigned char>(who[0]))) {
					id = static_cast<threads::WorkerId>(std::atoi(who.c_str()));
				} else {
					for (const threads::WorkerInfo& w : m_threads.SnapshotAll())
						if (w.name == who) { id = w.id; break; }
				}
				const threads::WorkerInfo info = m_threads.Inspect(id);
				if (info.id == threads::kInvalidWorker) {
					m_console.Print(std::format("no worker '{}' (see `threads`)", who));
					return;
				}
				void* frames[stack::kMaxFrames];
				const int n = m_threads.CaptureStack(id, frames, stack::kMaxFrames);
				if (n == 0) {
					m_console.Print(std::format(
						"could not walk '{}' (#{}, {}) — dead, quarantined, or this thread",
						info.name, id, threads::StateName(info.state)));
					return;
				}
				// To the console AND the log: a probe is evidence, and dungeon.log
				// is where evidence is read afterwards (the console scrolls away
				// and screenshots of it are not greppable).
				const std::string head =
					std::format("probe '{}' #{} [{}] tick {}, beat {:.0f} ms ago:",
								info.name, id, threads::StateName(info.state),
								info.iterations, info.heartbeatAgeMs);
				m_console.Print(head);
				log::Info("{}", head);
				// EVERY frame, unfiltered — unlike a crash report. For a wedged
				// thread the OS frame IS the answer: NtWaitForSingleObject names a
				// lock it is blocked on, NtDelayExecution a sleep it is sitting in.
				// Filtering those out would throw away the diagnosis.
				for (int i = 0; i < n && i < 20; ++i) {
					const std::string line = "  " + stack::Describe(frames[i]);
					m_console.Print(line);
					log::Info("{}", line);
				}
				return;
			}

			diag::ThreadHealth all[diag::kMaxThreads];
			const int tn = diag::SnapshotThreads(all, diag::kMaxThreads);

			// --- one thread: its counts, its events, and their stacks ---------
			if (!args.empty()) {
				bool found = false;
				for (int j = 0; j < tn; ++j) {
					if (args[0] != all[j].name) continue;
					found = true;
					m_console.Print(std::format(
						"'{}' [{}] — {} events: {} exception, {} fault, {} stall, {} "
						"restart, {} killed, {} fatal",
						all[j].name, all[j].live ? "live" : "gone", all[j].total,
						all[j].Count(diag::Kind::Exception), all[j].Count(diag::Kind::Fault),
						all[j].Count(diag::Kind::Stall), all[j].Count(diag::Kind::Restart),
						all[j].Count(diag::Kind::Killed), all[j].Count(diag::Kind::Fatal)));

					diag::EventView ev[diag::kEventsPerThread];
					const int n = diag::ReadEvents(all[j].slot, ev, diag::kEventsPerThread);
					for (int i = n - 1; i >= 0; --i) { // newest first
						m_console.Print(std::format("  #{} {} tick {}: {}", ev[i].index,
													diag::KindName(ev[i].kind),
													ev[i].iteration, ev[i].message));
						// Same plumbing rule as the log and the timeline; `shown`
						// counts survivors so the budget is not spent on ntdll.
						for (int f = 0, shown = 0; f < ev[i].frameCount && shown < 6; ++f) {
							const std::string fr = stack::Describe(ev[i].frames[f]);
							if (stack::IsPlumbingFrame(fr)) continue;
							m_console.Print("      " + fr);
							++shown;
						}
					}
				}
				if (!found)
					m_console.Print(std::format("no thread named '{}' in the record", args[0]));
				return;
			}

			// --- everything, newest first ------------------------------------
			const diag::Totals t = diag::ProcessTotals();
			m_console.Print(std::format(
				"{} events — {} exception, {} fault, {} stall, {} restart, {} killed, "
				"{} fatal ({} throws seen)",
				t.total, t.Count(diag::Kind::Exception), t.Count(diag::Kind::Fault),
				t.Count(diag::Kind::Stall), t.Count(diag::Kind::Restart),
				t.Count(diag::Kind::Killed), t.Count(diag::Kind::Fatal),
				stack::ThrowsSeen()));
			if (t.total == 0) {
				m_console.Print("nothing has gone wrong yet");
				return;
			}
			diag::EventView events[12];
			diag::Slot slots[12];
			const int n = diag::ReadAllEvents(events, 12, slots);
			for (int i = 0; i < n; ++i) {
				const char* owner = "?";
				for (int j = 0; j < tn; ++j)
					if (all[j].slot == slots[i]) owner = all[j].name;
				m_console.Print(std::format("  {} [{}] {}", diag::KindName(events[i].kind),
											owner, events[i].message));
			}
			m_console.Print("`health <thread>` for stacks, `health probe <id>` for a live one");
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
						   const int n = m_world.SmashAt(x, z, amount);
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
	m_console.Register(
		"uitree",
		"outline the UI control tree (on|off, or dump [<tree>] to print one)",
		[this](const std::vector<std::string>& args) {
			if (!args.empty() && args[0] == "dump") {
				const std::string name = args.size() > 1 ? args[1] : "hud";
				ui::UIContext* tree = m_ui.UiTree(name);
				if (!tree) {
					m_console.Print(std::format("unknown tree '{}'; try: {}", name,
												GameUI::UiTreeNames()));
					return;
				}
				m_console.Print(std::format("--- {} ---", name));
				ui::inspect::Dump(
					*tree, [this](const std::string& line) { m_console.Print(line); });
				return;
			}
			const bool on = args.empty() ? !ui::inspect::Enabled() : args[0] != "off";
			ui::inspect::SetEnabled(on);
			m_console.Print(on ? "ui tree overlay ON (hover a widget to see its chain)"
							   : "ui tree overlay off");
		});
	m_console.Register(
		"uioverlap",
		"audit visible widget trees for overlaps (optional label -> dungeon.log)",
		[this](const std::vector<std::string>& args) {
			// A findings list is worth reading somewhere other than a console
			// that scrolls, so it goes to dungeon.log too — which is what makes
			// a scripted sweep of every screen collectable afterwards. An
			// optional argument labels the run, so a log holding a dozen of them
			// says which screen each was.
			const std::string label = args.empty() ? std::string() : args[0];
			m_console.Print("uioverlap: auditing the next frame's trees...");
			if (!label.empty()) log::Info("uioverlap [{}] ---", label);
			// Verbatim: the summary line names itself and the findings are
			// indented under the header above, so a tag here only read as
			// "uioverlap uioverlap: clean".
			ui::inspect::ArmOverlapAudit([this](const std::string& line) {
				m_console.Print(line);
				log::Info("{}", line);
			});
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
	// The offense/defense split before its slider exists
	// (docs/damage-system.md). Worth keeping once the UI lands: setting an
	// exact share is how the split gets MEASURED, where dragging a slider is
	// how it gets FELT, and those are different questions.
	m_console.Register("guard", "set a hand's offense share 0..N (dev)",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1,
									 "usage: guard <share 0..N> [member 0-3]"))
							   return;
						   const float share =
							   static_cast<float>(std::atof(args[0].c_str()));
						   const size_t m = args.size() > 1
							   ? static_cast<size_t>(std::atoi(args[1].c_str())) : 0;
						   if (m >= m_characters.size()) {
							   m_console.Print("no such member");
							   return;
						   }
						   // Deliberately NO upper clamp, unlike the slider (which
						   // stops at exert_max): this is how a stance past what
						   // the UI allows gets tried at all.
						   if (share < 0.0f) {
							   m_console.Print("share cannot be negative");
							   return;
						   }
						   Character& c = m_characters[m];
						   c.offenseShare = share;
						   // The held-back share is reported UNCLAMPED, because a
						   // negative one is the whole point past 1 — the guard
						   // becomes a penalty, and a readout that floored it at
						   // 0% would say an over-exerted stance and an all-out
						   // one were the same thing.
						   const float held = (1.0f - share) * 100.0f;
						   m_console.Print(std::format(
							   "{} offense {:.2f} (guarding with {:.0f}% of hand "
							   "skill{})",
							   c.name, share, held,
							   share > 1.0f ? " — OVER-EXERTED" : ""));
					   });

	// THE PARTY'S SWING, from the console. `equip` and `wear` exist because the
	// armor system was untestable without them; this is the same gap one step
	// further on — every attack-side rule (the stance, crit pierce, and now the
	// whole fumble consequence table) could only be reached by clicking a hand
	// slot in the HUD, which no script drives reliably. A verb of "" takes the
	// neutral attack, exactly as the hand menu's default does.
	m_console.Register("swing", "attack with a member's hand (dev): swing <member> [hand] [verb]",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1,
									 "usage: swing <member 0-3> [hand 0/1] [verb]"))
							   return;
						   const size_t m =
							   static_cast<size_t>(std::atoi(args[0].c_str()));
						   if (m >= m_characters.size()) {
							   m_console.Print("no such member");
							   return;
						   }
						   const size_t hand = args.size() > 1
							   ? static_cast<size_t>(std::atoi(args[1].c_str())) : 0;
						   const std::string verb = args.size() > 2 ? args[2] : "";
						   // Reports what the hand did rather than staying silent:
						   // false means the swing never happened at all (down,
						   // still on cooldown, rear rank without a polearm), which
						   // is a different thing from a swing that missed and
						   // otherwise looks identical from a script.
						   m_console.Print(m_world.PartyAttack(m, hand, verb)
											   ? "swung"
											   : "that hand cannot swing now");
					   });

	// `equip` reaches a HAND; this reaches the doll — the only place worn armor
	// counts (DungeonWorld::WornArmorClass). Without it there is no scriptable
	// way to put armor ON a character, which made the whole armor system
	// untestable except by dragging things in the sheet.
	m_console.Register("wear", "put an item in its worn doll slot (dev)",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1,
									 "usage: wear <item id|none> [member 0-3]"))
							   return;
						   const size_t m = args.size() > 1
							   ? static_cast<size_t>(std::atoi(args[1].c_str())) : 0;
						   if (m >= m_characters.size()) {
							   m_console.Print("no such member");
							   return;
						   }
						   Character& c = m_characters[m];
						   if (args[0] == "none") {
							   // Strip the worn doll, hands untouched — the
							   // quickest way back to a bare-skinned baseline
							   // for comparing against armored numbers.
							   for (int i = 0; i < kEquipCount; ++i) {
								   if (i == static_cast<int>(EquipSlot::LeftHand) ||
									   i == static_cast<int>(EquipSlot::RightHand))
									   continue;
								   c.inventory.equipment[static_cast<size_t>(i)].typeId.clear();
							   }
							   m_console.Print(std::format("{} is unarmored", c.name));
							   return;
						   }
						   if (!m_project.HasItem(args[0])) {
							   m_console.Print(std::format(
								   "no item '{}' in items/weapons/armor", args[0]));
							   return;
						   }
						   // The item says where it goes — the same rule the
						   // paper doll enforces, so the console cannot put
						   // something somewhere the UI would refuse.
						   const WearSlot w = m_itemCategories.WornAt(args[0]);
						   if (w == WearSlot::None) {
							   m_console.Print(std::format(
								   "'{}' has no `wear` slot — hold it instead (equip)",
								   args[0]));
							   return;
						   }
						   for (int i = 0; i < kEquipCount; ++i) {
							   const EquipSlot slot = static_cast<EquipSlot>(i);
							   if (!WearSlotFits(w, slot)) continue;
							   c.inventory.equipment[static_cast<size_t>(i)].typeId = args[0];
							   m_console.Print(std::format("{} wears {} ({})", c.name,
														   args[0], WearSlotId(w)));
							   return;
						   }
					   });

	// `give` fills the pack; this puts a weapon straight in a hand, which is
	// what a combat test actually needs (no cursor drag, no HUD clicking).
	m_console.Register("equip", "put an item in a member's hand (dev)",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1,
									 "usage: equip <item id> [member 0-3] [hand 0/1]"))
							   return;
						   const size_t m = args.size() > 1
							   ? static_cast<size_t>(std::atoi(args[1].c_str())) : 0;
						   const int hand = args.size() > 2
							   ? std::clamp(std::atoi(args[2].c_str()), 0, 1) : 0;
						   if (m >= m_characters.size()) {
							   m_console.Print("no such member");
							   return;
						   }
						   if (!m_project.HasItem(args[0])) {
							   m_console.Print(std::format("no item '{}' in items/weapons/armor", args[0]));
							   return;
						   }
						   m_characters[m].inventory.Hand(hand).typeId = args[0];
						   m_console.Print(std::format("{} {} hand = {}",
													   m_characters[m].name,
													   hand == 0 ? "left" : "right",
													   args[0]));
					   });
	// Land a status effect directly, skipping the cast. Setting a ward up in a
	// live fight is otherwise a coin toss — vocabulary, mana, and the fumble
	// roll all have to go your way, and then a monster has to choose to hit
	// the bearer before you see the ward DO anything.
	m_console.Register("effect", "apply a status effect to a member or the monster ahead (dev)",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1,
									 "usage: effect <id> [member 0-3 | ahead] [magnitude] [seconds]"))
							   return;
						   const float mag = args.size() > 2
							   ? std::strtof(args[2].c_str(), nullptr) : 8.0f;
						   const float secs = args.size() > 3
							   ? std::strtof(args[3].c_str(), nullptr) : 60.0f;
						   // "ahead" targets the monster the party is facing —
						   // the only way to put an effect ON a monster by hand,
						   // and the way to watch one tick without a weapon that
						   // procs it (docs/effects.md P3).
						   if (args.size() > 1 && args[1] == "ahead") {
							   m_console.Print(m_world.ApplyEffectAhead(args[0], mag, secs)
												   ? std::format("monster ahead gains {}", args[0])
												   : "no monster ahead (or no such effect)");
							   return;
						   }
						   const size_t m = args.size() > 1
							   ? static_cast<size_t>(std::atoi(args[1].c_str())) : 0;
						   if (m >= m_characters.size()) {
							   m_console.Print("no such member");
							   return;
						   }
						   const fx::EffectKind* kind = m_world.Effects().Find(args[0]);
						   if (!kind) {
							   m_console.Print(std::format("no effect '{}'", args[0]));
							   return;
						   }
						   const float magnitude = args.size() > 2
							   ? std::strtof(args[2].c_str(), nullptr) : 8.0f;
						   const float seconds = args.size() > 3
							   ? std::strtof(args[3].c_str(), nullptr) : 60.0f;
						   // The school picks a ward's flavour and every effect's
						   // tint, so derive it from the kind — `effect fireshield`
						   // must land the FIRE ward, not an oddly tinted one.
						   SpellSymbol school = SpellSymbol::Fire;
						   if (args[0] == "stoneskin" || args[0] == "poison")
							   school = SpellSymbol::Earth;
						   else if (args[0] == "windward") school = SpellSymbol::Air;
						   else if (args[0] == "waterveil") school = SpellSymbol::Water;
						   fx::Apply(m_characters[m].effects, *kind, school, magnitude,
									 seconds);
						   m_ui.RefreshSheet();
						   m_console.Print(std::format("{} gains {} ({} for {}s)",
													   m_characters[m].name, args[0],
													   magnitude, seconds));
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
	m_console.Register("logecho", "mirror console output to dungeon.log",
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
	m_console.Register("newgame", "start a new game (dev)",
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

	// The party's side of an encounter, in one machine-readable block. `monsters`
	// has printed the other side for a while; without this a harness can watch a
	// fight and never learn what it COST, which is most of what a balance pass
	// is trying to find out.
	m_console.Register("party", "each member's hp/stamina/mana + stance (dev)",
					   [this](const std::vector<std::string>&) {
						   for (size_t i = 0; i < m_characters.size(); ++i) {
							   const Character& c = m_characters[i];
							   m_console.Print(std::format(
								   "  [{}] {:<6} hp {:.1f}/{:.1f}  st {:.1f}/{:.1f}  "
								   "mp {:.1f}/{:.1f}  share {:.2f}{}{}",
								   i, c.name, c.health, c.maxHealth, c.stamina,
								   c.maxStamina, c.mana, c.maxMana, c.offenseShare,
								   c.dead ? "  DEAD" : (c.IsAlive() ? "" : "  DOWN"),
								   c.exhausted ? "  EXHAUSTED" : ""));
						   }
					   });

	// The three regeneration RATES, which nothing else can show: a bar's VALUE
	// is visible and its SLOPE is not, so the ordering the model asks for —
	// stamina/sec > mana/sec > health/sec at equal investment
	// (docs/health-and-healing.md) — was a claim no measurement could reach.
	//
	// Rates are printed AT FULL FLOW, before the state gate, because that is
	// what the knobs describe; the live gate is named at the end of each line so
	// a reading taken mid-swing is not mistaken for the tuning.
	//
	// THE ORDERING IS CHECKED ON A REFERENCE ROW, NOT PER MEMBER, and getting
	// that wrong the first time is worth recording: a per-member verdict called
	// Brand BROKEN, and Brand is right — he is a brute with INT 8, so his mana
	// crawls and ought to. The claim is about the KNOBS at EQUAL investment, and
	// a party of four deliberately unequal characters can never test it. So the
	// reference member has every aptitude at the stat curve's baseline (worth
	// exactly nothing, by construction) and one practice level shared by all
	// three pools — measured UNTRAINED and TRAINED, since a crossing can hide at
	// either end. Reporting whether an authored property holds is measurement;
	// what to do about it is Michael's.
	m_console.Register("regen", "health/stamina/mana per second, and the ordering (dev)",
					   [this](const std::vector<std::string>&) {
						   const Balance& bal = m_world.GetBalance();
						   const resource::PoolRules pools = bal.Resources();
						   const CurveRules statCurve = bal.StatCurve();
						   const CurveRules paceCurve = bal.PaceCurve();
						   for (size_t i = 0; i < m_characters.size(); ++i) {
							   const Character& c = m_characters[i];
							   const auto rate = [&](resource::Kind k) {
								   return c.RegenPerSec(k, pools.For(k), statCurve);
							   };
							   m_console.Print(std::format(
								   "  [{}] {:<6} health {:.3f}/s  stamina {:.3f}/s  "
								   "mana {:.3f}/s  pace {:.2f}  {}",
								   i, c.name, rate(resource::Kind::Health),
								   rate(resource::Kind::Stamina),
								   rate(resource::Kind::Mana),
								   c.MoveSpeed(paceCurve),
								   c.staminaHoldoff > 0.0f ? "exerting" : "idle"));
						   }
						   // The PARTY's pace is the slowest member's, so it is
						   // its own line: the interesting case is a member
						   // training hard and the number not moving at all.
						   m_console.Print(std::format(
							   "  party pace {:.2f} (the slowest member's)",
							   m_world.GetParty().Speed()));
						   // The reference rows. Each pool is sized at the same
						   // investment it is being rated at, so the per-max term
						   // is honest rather than borrowed from someone else's
						   // body: max = aptitude + practice, with no authored base.
						   const float apt = statCurve.baseline;
						   for (const float lvl : {0.0f, 10.0f}) {
							   const auto rate = [&](resource::Kind k) {
								   const resource::Rules& r = pools.For(k);
								   return resource::RegenPerSec(
									   r, statCurve, apt,
									   resource::Maximum(r, 0.0f, apt, lvl), lvl);
							   };
							   const float hp = rate(resource::Kind::Health);
							   const float st = rate(resource::Kind::Stamina);
							   const float mp = rate(resource::Kind::Mana);
							   m_console.Print(std::format(
								   "  ref  practice {:<2.0f} health {:.3f}/s  "
								   "stamina {:.3f}/s  mana {:.3f}/s  order {}",
								   lvl, hp, st, mp,
								   st > mp && mp > hp ? "ok" : "BROKEN"));
						   }
					   });

	// Food and water, and how long they have left. The REMAINING TIME is the
	// point of the readout: a meter at 62 means nothing on its own, because the
	// drain rate depends on the member's conditioning — the fitter member burns
	// more, which is the brake the whole design rests on. Printed in hours,
	// because a supply run is a question about hours and not about seconds.
	m_console.Register("supplies", "each member's food and water, and hours left (dev)",
					   [this](const std::vector<std::string>&) {
						   const Balance& bal = m_world.GetBalance();
						   const resource::SupplyRules food =
							   bal.SupplyOf(resource::Supply::Food);
						   const resource::SupplyRules water =
							   bal.SupplyOf(resource::Supply::Water);
						   for (size_t i = 0; i < m_characters.size(); ++i) {
							   const Character& c = m_characters[i];
							   const float cond =
								   c.PracticeLevel(resource::Kind::Stamina);
							   const auto hours = [&](const resource::SupplyRules& r,
													  float level) {
								   const float rate = resource::DrainPerSec(r, cond);
								   return rate > 0.0f ? level / rate / 3600.0f : 0.0f;
							   };
							   m_console.Print(std::format(
								   "  [{}] {:<6} food {:5.1f}/{:.0f} ({:4.1f}h)  "
								   "water {:5.1f}/{:.0f} ({:4.1f}h)  cond {:.0f}{}{}",
								   i, c.name, c.food, food.max, hours(food, c.food),
								   c.water, water.max, hours(water, c.water), cond,
								   c.FindEffect("starving") ? "  STARVING" : "",
								   c.FindEffect("parched") ? "  PARCHED" : ""));
						   }
					   });

	// RECYCLE THE WORLD instead of reloading it (docs/eval-harness.md). A level
	// load is ~12 seconds and 80% of a suite's cost; this is the same baseline
	// for nothing. A script that wants a CLEAN test opens with it; a script
	// measuring PROGRESSION across a series simply does not call it, and inherits
	// whatever the previous one left — Michael's call, and the reason this is a
	// directive a script chooses rather than something the runner imposes.
	m_console.Register("reset",
					   "recycle the world to a new-game baseline, no reload (dev)",
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

	// Open (or close) the character sheet. It exists because the sheet was
	// reachable ONLY by clicking a portrait, which is why `/check-ingame`
	// reports it as a screen it cannot sweep — so the one screen with the most
	// hand-laid-out content in the game was also the one screen `uioverlap`
	// never saw. `sheet <n>` then `uioverlap` closes half of that gap.
	m_console.Register("sheet", "open the character sheet (dev): sheet <member|off>",
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty() && args[0] == "off") {
							   if (m_state == AppState::CharacterSheet)
								   m_state = AppState::Playing;
							   m_console.Print("sheet closed");
							   return;
						   }
						   const size_t m =
							   args.empty()
								   ? 0
								   : static_cast<size_t>(std::atoi(args[0].c_str()));
						   if (m >= m_characters.size()) {
							   m_console.Print("no such member");
							   return;
						   }
						   // Through the same entry point the portrait click uses,
						   // so this cannot drift from what a player sees.
						   OpenCharacterSheet(m);
						   m_console.Print(
							   std::format("sheet open: {}", m_characters[m].name));
					   });

	// The rest STATE, for a script and for a quick look. It reports the world
	// speed too, since that is the whole mechanism and the number a reader needs
	// to interpret how much simulated time a `step` just covered.
	// BARE `rest` REPORTS AND DOES NOT TOGGLE. It was a toggle for about ten
	// minutes, and the eval script written against it read `rest` as a status
	// query at four places — each of which silently turned the state back on and
	// made the auto-stop rules look broken when they were working. A query that
	// mutates is a trap, and this one caught its own author.
	m_console.Register("rest",
					   "the rest state (dev): rest [on|off|until [secs]], bare = report",
					   [this](const std::vector<std::string>& args) {
						   // `rest until` — enter rest AND run the world until it
						   // ends. This is the form a script wants, and the reason
						   // it exists is a trap worth recording: `rest on` followed
						   // by `step N` does NOT measure a rest. UpdateRest does not
						   // depend on dt, so it fires on the very next ORDINARY
						   // frame — and with timescale 0 that frame happens between
						   // the two console commands. If there was nothing to
						   // recover, rest was already over before the step began,
						   // and the step then ran its FULL budget of dungeon time
						   // with no rest in progress: supplies drained for fifteen
						   // minutes and the table read as "resting is expensive".
						   //
						   // Stepping from inside the same command leaves no frame
						   // in between, so the state cannot end before the clock
						   // starts. StepWorld already stops the moment rest ends.
						   if (!args.empty() && args[0] == "until") {
							   const float cap =
								   args.size() > 1
									   ? static_cast<float>(std::atof(args[1].c_str()))
									   : 3600.0f;
							   m_world.SetResting(true);
							   const int ran = StepWorld(cap);
							   m_console.Print(std::format(
								   "rested {:.2f}s — {}",
								   static_cast<float>(ran) / 60.0f,
								   m_world.Resting() ? "still resting (hit the cap)"
													 : m_world.RestEndReason()));
							   return;
						   }
						   if (!args.empty()) m_world.SetResting(args[0] != "off");
						   const char* why = m_world.RestEndReason();
						   m_console.Print(std::format(
							   "rest {} (world x{:.0f}){}{}",
							   m_world.Resting() ? "on" : "off",
							   m_world.RestTimeScale(),
							   *why && !m_world.Resting() ? "  last ended: " : "",
							   *why && !m_world.Resting() ? why : ""));
					   });

	// Eat or drink a catalog item outright — no inventory, no hand slot. The UI
	// path (a hand-menu `eat`/`drink`) runs the very same DungeonWorld::
	// ConsumeItem, so this exercises the arithmetic and the effect-lifting that
	// a script cannot reach by clicking (see [[hands-on-visual-testing]]).
	m_console.Register("consume", "eat or drink an item (dev): consume <item> [member]",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1,
									 "usage: consume <item id> [member 0-3]"))
							   return;
						   const size_t m =
							   args.size() > 1
								   ? static_cast<size_t>(std::atoi(args[1].c_str()))
								   : 0;
						   if (m >= m_characters.size()) {
							   m_console.Print("no such member");
							   return;
						   }
						   const resource::Refill got =
							   m_world.ConsumeItem(m_characters[m], args[0]);
						   m_console.Print(
							   got.Any()
								   ? std::format("{} consumes {}: food +{:.1f} water +{:.1f}",
												 m_characters[m].name, args[0],
												 got.food, got.water)
								   : std::format("{} gains nothing from {} "
												 "(not consumable, or already full)",
												 m_characters[m].name, args[0]));
					   });

	// Seeding a supply state, so a script can start a rung hungry instead of
	// stepping eight hours to get there.
	m_console.Register("setsupply",
					   "set food/water (dev): setsupply <member|all> <food|water> <n>",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 3,
									 "usage: setsupply <member 0-3|all> "
									 "<food|water> <n>"))
							   return;
						   const bool all = args[0] == "all";
						   const size_t one =
							   static_cast<size_t>(std::atoi(args[0].c_str()));
						   if (!all && one >= m_characters.size()) {
							   m_console.Print("no such member");
							   return;
						   }
						   resource::Supply which{};
						   if (args[1] == "food") which = resource::Supply::Food;
						   else if (args[1] == "water") which = resource::Supply::Water;
						   else {
							   m_console.Print("expected food or water");
							   return;
						   }
						   const float max =
							   m_world.GetBalance().SupplyOf(which).max;
						   const float v = std::clamp(
							   static_cast<float>(std::atof(args[2].c_str())), 0.0f,
							   max);
						   for (size_t i = 0; i < m_characters.size(); ++i) {
							   if (!all && i != one) continue;
							   m_characters[i].SupplyLevel(which) = v;
						   }
						   m_console.Print(std::format("{} {} = {:.1f}",
													   all ? "party" : m_characters[one].name,
													   args[1], v));
					   });

	// --- the arena (docs/eval-harness.md) -----------------------------------
	// Carve a controlled space into the loaded map and empty the world into it.
	// Writes NO files: the editor's new-level button would author a .map/.ent
	// into the git tree, which an eval must not do on every run.
	m_console.Register(
		"arena", "carve a test arena (dev): arena <open|corridor|deadend|tjunction> [w] [h]",
		[this](const std::vector<std::string>& args) {
			if (!Need(m_console, args, 1,
					  "usage: arena <open|corridor|deadend|tjunction> [w] [h]"))
				return;
			DungeonWorld::ArenaShape shape{};
			if (!DungeonWorld::ArenaShapeFromName(args[0], shape)) {
				m_console.Print("unknown shape: " + args[0] +
								" (open|corridor|deadend|tjunction)");
				return;
			}
			const int w = args.size() > 1 ? std::atoi(args[1].c_str()) : 9;
			const int h = args.size() > 2 ? std::atoi(args[2].c_str()) : w;
			DungeonWorld::ArenaInfo info;
			if (!m_world.BuildArena(shape, w, h, info)) {
				m_console.Print("arena: refused (see the log)");
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
	m_console.Register("forward", "walk the party (dev): forward [n]",
					   [this](const std::vector<std::string>& args) {
						   const int n = args.empty() ? 1 : std::atoi(args[0].c_str());
						   if (n < 1) {
							   m_console.Print("forward needs a positive count");
							   return;
						   }
						   m_world.QueueForward(n);
						   m_console.Print(std::format("forward x{}", n));
					   });

	// Monsters hold still while everything that happens TO them keeps running.
	// A geometry probe's instruments must not wander off the cells they measure.
	m_console.Register("freeze", "monsters stop acting (dev): freeze on|off",
					   [this](const std::vector<std::string>& args) {
						   if (args.empty()) {
							   m_console.Print(std::format(
								   "freeze {}",
								   m_world.FrozenMonsters() ? "on" : "off"));
							   return;
						   }
						   const bool on = args[0] == "on" || args[0] == "1";
						   m_world.SetFreezeMonsters(on);
						   m_console.Print(std::format("freeze {}", on ? "on" : "off"));
					   });

	// Detonate a spell's authored blast at a cell — no caster, no mana, no skill
	// roll, no bolt flight. The geometry question asked directly.
	//
	// A blast plays out over TICKS (blast_rate seconds apart), so a script must
	// `step` afterwards to let it land; detonating and reading `monsters` in the
	// same breath measures the moment before it went off.
	m_console.Register("blast", "detonate a spell's blast (dev): blast <spell> <x> <z>",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 3,
									 "usage: blast <spell id> <x> <z>"))
							   return;
						   const int x = std::atoi(args[1].c_str());
						   const int z = std::atoi(args[2].c_str());
						   if (!m_world.DetonateSpell(args[0], x, z)) {
							   m_console.Print(std::format(
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
	m_console.Register("spawn", "place a monster (dev): spawn <type> <x> <z> [n|e|s|w]",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 3,
									 "usage: spawn <type> <x> <z> [facing]"))
							   return;
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
						   if (!m_world.AddMonster(args[0], x, z, facing)) {
							   m_console.Print(std::format(
								   "spawn: refused '{}' at {},{} (unknown type, "
								   "not walkable, or cell taken)",
								   args[0], x, z));
							   return;
						   }
						   if (strength > 0.0f && strength != 1.0f)
						   m_world.ScaleLastMonster(strength);
					   m_console.Print(std::format("spawned {} at {},{} x{:.2f}",
											   args[0], x, z, strength));
					   });

	// --- seeding a rung (docs/eval-harness.md) ------------------------------
	// A single eval run cannot play from fresh characters to end-game, so a rung
	// has to START where it wants to measure. These two put a member wherever on
	// the curve the test needs.
	m_console.Register("setstat", "set a stat (dev): setstat <member> <stat> <n>",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 3,
									 "usage: setstat <member 0-3> "
									 "<str|dex|vit|wil|int> <n>"))
							   return;
						   const size_t m =
							   static_cast<size_t>(std::atoi(args[0].c_str()));
						   if (m >= m_characters.size()) {
							   m_console.Print("no such member");
							   return;
						   }
						   Character& c = m_characters[m];
						   const int n = std::atoi(args[2].c_str());
						   const std::string& s = args[1];
						   if (s.starts_with("str")) c.strength = n;
						   else if (s.starts_with("dex")) c.dexterity = n;
						   else if (s.starts_with("vit")) c.vitality = n;
						   else if (s.starts_with("wil")) c.willpower = n;
						   else if (s.starts_with("int")) c.intelligence = n;
						   else {
							   m_console.Print("unknown stat: " + s);
							   return;
						   }
						   // Health/stamina/mana maxima DERIVE from stats
						   // (Character::RecomputeMaxima), so a stat set without
						   // this leaves a level-20 fighter with a novice's hit
						   // points and every number after it measured wrong.
						   m_world.RecomputePartyMaxima();
						   m_console.Print(std::format("{} {} = {}", c.name, s, n));
					   });

	// Levels DERIVE from raw xp (floor(sqrt)), so this sets the xp that yields
	// the level asked for — squaring is the honest inverse, and it means a
	// seeded skill trains onward from exactly where a played one would have.
	// Without it, giving a caster a usable fire skill for a test means casting
	// thirty times and hoping the mana holds out.
	m_console.Register("setskill", "set a skill level (dev): setskill <member> <skill> <level>",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 3,
									 "usage: setskill <member 0-3> <skill id> <level>"))
							   return;
						   const size_t m =
							   static_cast<size_t>(std::atoi(args[0].c_str()));
						   if (m >= m_characters.size()) {
							   m_console.Print("no such member");
							   return;
						   }
						   const int level = std::atoi(args[2].c_str());
						   if (level < 0) {
							   m_console.Print("level cannot be negative");
							   return;
						   }
						   Character& c = m_characters[m];
						   const float xp = static_cast<float>(level) * level;
						   c.skillXp[args[1]] = xp;
						   // RE-DERIVE, for exactly the reason `setstat` already
						   // had to: a RESOURCE practice feeds the pool maxima
						   // and the walking pace now, so a skill set without
						   // this leaves a conditioned member carrying a novice's
						   // stamina bar and the party walking at the old speed —
						   // and everything measured afterwards is quietly wrong.
						   m_world.RecomputePartyMaxima();
						   m_console.Print(std::format("{} {} = level {} ({:.0f} xp)",
													   c.name, args[1],
													   c.SkillLevel(args[1]), xp));
					   });

	// --- measuring an encounter (docs/eval-harness.md) ----------------------
	// Without this a measured encounter is the party STANDING STILL BEING HIT.
	// PartyAttack is driven by a hand-slot click or `swing`, so the first
	// two-tier comparison had the monster finish on full hp in both rungs and
	// still looked like a complete result.
	m_console.Register("autoattack", "party swings off cooldown (dev): autoattack on|off",
					   [this](const std::vector<std::string>& args) {
						   if (args.empty()) {
							   m_console.Print(std::format(
								   "autoattack {}",
								   m_world.AutoAttack() ? "on" : "off"));
							   return;
						   }
						   const bool on = args[0] == "on" || args[0] == "1";
						   m_world.SetAutoAttack(on);
						   m_console.Print(std::format("autoattack {}", on ? "on" : "off"));
					   });

	// The encounter's numbers, in one machine-readable line. `tally reset` marks
	// the start of a rung; `tally` prints what has happened since.
	m_console.Register("tally", "encounter counters (dev): tally [reset]",
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty() && args[0] == "reset") {
							   m_world.ResetTally();
							   m_console.Print("tally reset");
							   return;
						   }
						   const DungeonWorld::Tally& t = m_world.GetTally();
						   const int swings = t.hits + t.misses;
						   // ONE LINE, key=value, so a sweep's output can be
						   // grepped and diffed without parsing prose. Damage is
						   // in absolute POINTS, never a fraction of health —
						   // the healing model is still to be designed, and
						   // fractions would change meaning the day it lands.
						   m_console.Print(std::format(
							   "TALLY dealt={:.1f} taken={:.1f} swings={} hits={} "
							   "misses={} hitrate={:.3f} crits={} fumbles={} "
							   "slain={} downed={} secs={:.1f}",
							   t.dealt, t.taken, swings, t.hits, t.misses,
							   swings > 0 ? static_cast<float>(t.hits) / swings : 0.0f,
							   t.crits, t.fumbles, t.monstersSlain, t.membersDowned,
							   t.seconds));
					   });

	// RESET THE PARTY BETWEEN RUNGS. A ladder runs many encounters in one
	// process, and the first one that wipes ends the run: a wipe returns to the
	// TITLE SCREEN (Game_Wiring's onPartyWipe), after which every `step` is
	// correctly refused and every rung after it measures nothing. Found exactly
	// that way — rung 2 of the first two-tier script never ran.
	//
	// `newgame` would also fix it and costs a full staged reload per rung; this
	// restores in place. It deliberately does NOT touch stats, skills, gear or
	// stance: those are what a preset SEEDED, and a heal that undid the seeding
	// would make the second rung measure the first one's party.
	m_console.Register("heal", "restore the party to full (dev): heal [member]",
					   [this](const std::vector<std::string>& args) {
						   const auto restore = [this](Character& c) {
							   c.dead = false;
							   c.health = c.maxHealth;
							   c.stamina = c.maxStamina;
							   c.mana = c.maxMana;
							   c.exhausted = false;
							   c.staminaHoldoff = 0.0f;
							   c.stabilize = 0.0f;
							   c.hitFlash = 0.0f;
							   c.handCooldown[0] = c.handCooldown[1] = 0.0f;
							   // A burn carried over from the previous rung
							   // would tick into the next one's numbers.
							   c.effects.clear();
						   };
						   if (!args.empty()) {
							   const size_t m =
								   static_cast<size_t>(std::atoi(args[0].c_str()));
							   if (m >= m_characters.size()) {
								   m_console.Print("no such member");
								   return;
							   }
							   restore(m_characters[m]);
							   // A harness fiat, not a game rule: nothing a player
							   // can do heals like this, so it is REBASED rather
							   // than sanctioned — there is no route worth naming
							   // (Game/DamageLedger.h).
							   m_world.RebaseDamageLedger();
							   m_console.Print(
								   std::format("healed {}", m_characters[m].name));
							   return;
						   }
						   for (Character& c : m_characters) restore(c);
						   m_world.RebaseDamageLedger();
						   // A wipe left the app on the title screen; put it back
						   // in play, or the heal fixes the party and the next
						   // `step` still refuses.
						   m_ui.ResetHudStatus();
						   // ...and clear the wipe LATCH, which gates every
						   // monster attack. Without it the party stands up and
						   // nothing ever swings at them again — a whole sweep
						   // of rungs reporting forty-five seconds of nothing.
						   m_world.ClearWipeLatch();
						   ResumeAfterHeal();
						   m_console.Print(std::format("healed the party ({})",
													   StateName()));
					   });

	// The seeding half of `party`: what a member IS, rather than how they are
	// doing. A rung that seeded nothing (a typo'd skill id, a member index past
	// the roster) would otherwise run and report a perfectly plausible number
	// for the wrong character.
	m_console.Register("char", "a member's stats, skills and gear (dev): char <member>",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1, "usage: char <member 0-3>")) return;
						   const size_t m =
							   static_cast<size_t>(std::atoi(args[0].c_str()));
						   if (m >= m_characters.size()) {
							   m_console.Print("no such member");
							   return;
						   }
						   const Character& c = m_characters[m];
						   m_console.Print(std::format(
							   "  {}  str {} dex {} vit {} wil {} int {}", c.name,
							   c.strength, c.dexterity, c.vitality, c.willpower,
							   c.intelligence));
						   m_console.Print(std::format(
							   "    hp {:.1f}/{:.1f}  st {:.1f}/{:.1f}  mp {:.1f}/{:.1f}",
							   c.health, c.maxHealth, c.stamina, c.maxStamina, c.mana,
							   c.maxMana));
						   for (const auto& [id, xp] : c.skillXp)
							   m_console.Print(std::format("    skill {:<12} level {} ({:.1f} xp)",
														   id, Character::LevelForXp(xp), xp));
						   // THE CREEP POOLS, and they are here for one reason:
						   // the resource practices must creep NOTHING
						   // (docs/health-and-healing.md). Without this line the
						   // only evidence is that a stat has not moved yet — and
						   // a slow leak reads exactly like no leak until the pool
						   // crosses 1.0, which is the "absent and correct report
						   // identically" trap this project has already paid for.
						   // A non-zero pool beside a resource skill IS the bug.
						   for (const auto& [stat, pool] : c.statProgress)
							   if (pool > 0.0f)
								   m_console.Print(std::format(
									   "    creep {:<12} {:.3f} toward the next point",
									   stat, pool));
						   for (int h = 0; h < 2; ++h) {
							   const ItemSlot& slot = c.inventory.Hand(h);
							   m_console.Print(std::format(
								   "    hand{} {}", h,
								   slot.Empty() ? "(empty)" : slot.typeId));
						   }
						   for (int e = 0; e < kEquipCount; ++e) {
							   // The HANDS are equipment slots too (Inventory::
							   // Hand indexes this same array), so listing every
							   // slot printed each weapon twice and read as a
							   // member wearing their own sword.
							   const auto id = static_cast<EquipSlot>(e);
							   if (id == EquipSlot::LeftHand ||
								   id == EquipSlot::RightHand)
								   continue;
							   const ItemSlot& slot = c.inventory.equipment[
								   static_cast<size_t>(e)];
							   if (!slot.Empty())
								   m_console.Print(std::format("    worn  {}", slot.typeId));
						   }
					   });

	// A script cannot otherwise tell whether it is measuring anything at all.
	m_console.Register("state", "what the app is doing (loading/menu/playing/...)",
					   [this](const std::vector<std::string>&) {
						   m_console.Print(std::format("state {}", StateName()));
					   });

	m_console.Register("seed", "reseed the combat RNG (dev): seed <n>",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1, "usage: seed <n>")) return;
						   const auto n = static_cast<u32>(
							   std::strtoul(args[0].c_str(), nullptr, 10));
						   m_world.SeedCombat(n);
						   m_console.Print(std::format("combat seed {}", n));
					   });

	// Without this a stepped run is a fiction: the AI's four bucket workers tick
	// on WALL-CLOCK, so simulating thirty seconds inside a few frames lets the
	// monsters think perhaps twice. See ai::AsyncDirector::SetLockstep.
	m_console.Register("lockstep", "drive monster AI from sim time, not the clock",
					   [this](const std::vector<std::string>& args) {
						   if (args.empty()) {
							   m_console.Print(std::format(
								   "lockstep {}", m_world.LockstepAI() ? "on" : "off"));
							   return;
						   }
						   const bool on = args[0] == "on" || args[0] == "1";
						   m_world.SetLockstepAI(on);
						   m_console.Print(std::format("lockstep {}", on ? "on" : "off"));
					   });

	// Advance the world by sim seconds, now, in fixed ticks. Reports what it
	// actually RAN rather than what was asked for: a short answer means the run
	// hit the ceiling or changed level, and an eval that silently measured less
	// time than it believes is worse than one that failed outright.
	m_console.Register("step", "advance the sim by N seconds (dev): step <seconds>",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1, "usage: step <seconds>")) return;
						   const float secs =
							   static_cast<float>(std::atof(args[0].c_str()));
						   if (secs <= 0.0f) {
							   m_console.Print("step needs a positive number of seconds");
							   return;
						   }
						   // SAY WHY, never a bare zero. Dev commands reach the
						   // world from the MENU too, so a script whose party
						   // has wiped would otherwise watch `tp` and `monsters`
						   // answer normally while every `step` quietly did
						   // nothing — a whole suite of encounters that never
						   // ran, reported as results.
						   if (std::string_view(StateName()) != "playing") {
							   m_console.Print(std::format(
								   "step: not playing (state: {}) — nothing stepped",
								   StateName()));
							   return;
						   }
						   // Warned, not refused: stepping without lockstep is
						   // still useful for eyeballing, and silently producing
						   // a meaningless number is the thing to avoid.
						   if (!m_world.LockstepAI())
							   m_console.Print("warning: lockstep is OFF — monsters "
											   "will barely think during this step");
						   const bool wasResting = m_world.Resting();
						   const int ran = StepWorld(secs);
						   // A rested step says so, and says WHY it stopped: the
						   // seconds it ran ARE the length of the rest, which is
						   // the number a supply measurement is after.
						   m_console.Print(std::format(
							   "stepped {} ticks ({:.2f}s){}", ran,
							   static_cast<float>(ran) / 60.0f,
							   wasResting && !m_world.Resting()
								   ? std::format(" — rest ended: {}",
												 m_world.RestEndReason())
								   : ""));
					   });

	m_console.Register("noclip", "toggle walking through walls",
					   [this](const std::vector<std::string>&) {
						   Party& p = m_world.GetParty();
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
						   if (!args.empty()) m_world.SetShadowsEnabled(ArgOn(args[0]));
						   m_console.Print(m_world.ShadowsEnabled() ? "shadows on"
																	: "shadows off");
					   });
	m_console.Register("shadowrate",
				"fire shadow re-render rate: <hz> [per-frame budget]",
				[this](const std::vector<std::string>& args) {
					if (!args.empty()) {
						const float hz = std::strtof(args[0].c_str(), nullptr);
						const int budget =
							args.size() > 1 ? std::atoi(args[1].c_str()) : -1;
						m_world.SetShadowFlicker(hz, budget);
					}
					m_console.Print(std::format(
						"fire shadows re-render at {:.1f} Hz, at most {} cube(s)/frame",
						m_world.ShadowFlickerHz(), m_world.ShadowFlickerBudget()));
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
