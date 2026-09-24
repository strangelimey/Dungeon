// ============================================================================
// Game/Game_DevDiagnostics.cpp — the dev console's instruments.
//
// Split out of Game_DevCommands.cpp by concern: the checks that PROVE a rule
// (the allocation guard, the one-pipeline ledger — each with a way to make it
// fail on purpose), the thread manager's controls and stress workers, the
// health record (crashpoke/health), and the UI tree audits (uitree/uioverlap).
// ============================================================================
#include "Game/Game.h"

#include "Core/AllocTrack.h"
#include "Core/Assert.h"
#include "Core/Diagnostics.h"
#include "Core/Log.h"
#include "Core/StackTrace.h"
#include "Game/DevCommandArgs.h"
#include "UI/TreeInspector.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <string>
#include <thread>

namespace dungeon::game {

using devargs::Need;

void Game::RegisterDiagnosticCommands() {
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
				m_console.Refuse("no such member");
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
}

} // namespace dungeon::game
