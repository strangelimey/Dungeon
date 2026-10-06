// ============================================================================
// Game/Game_DevDiagnostics.cpp — the dev console's instruments.
//
// Split out of Game_DevCommands.cpp by concern: the checks that PROVE a rule
// (the allocation guard, the one-pipeline ledger — each with a way to make it
// fail on purpose), the thread manager's controls and stress workers, the
// health record (crashpoke/health), and the UI tree audits (uitree/uioverlap,
// and clippoke for the walk's clip).
// ============================================================================
#include "Game/Game.h"

#include "Assets/File.h"
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

namespace {
// A worker by id ("7") or by the name the THREADS panel shows ("demo.wedged",
// "ai.bucket2") - a name is what you remember, and what a script can know
// before the worker exists. For a name, the newest worker of it that still has
// a thread, else the newest of it at all. kInvalidWorker when nothing matches.
threads::WorkerId FindWorker(const threads::Manager& mgr, const std::string& who) {
	if (!who.empty() && std::isdigit(static_cast<unsigned char>(who[0])))
		return static_cast<threads::WorkerId>(std::atoi(who.c_str()));
	threads::WorkerId any = threads::kInvalidWorker, live = threads::kInvalidWorker;
	for (const threads::WorkerInfo& w : mgr.SnapshotAll()) {
		if (w.name != who) continue;
		any = w.id;
		if (w.state != threads::State::Dead && w.state != threads::State::Quarantined)
			live = w.id;
	}
	return live != threads::kInvalidWorker ? live : any;
}
} // namespace

void Game::RegisterDiagnosticCommands() {
	m_console.Register(
		{.name = "alloctest",
		 .group = CmdGroup::Diagnostics,
		 .params = "[seconds]",
		 .summary = "measure steady frames; PASS = zero heap allocations"},
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
			m_allocTestTransitions = 0;
			m_allocTestStart = alloc::Stats();
			m_console.Print(std::format(
				"alloctest: {:.0f}s of steady frames — closing the console (frames only "
				"arm while it is shut); the result lands here and in dungeon.log",
				seconds));
			if (m_console.IsOpen()) m_console.Toggle();
		});
	m_console.Register(
		{.name = "allocpoke",
		 .group = CmdGroup::Diagnostics,
		 .params = "[seconds]\n"
				   "once",
		 .summary = "allocate on purpose (proves the guard can fail)"},
		[this](const std::vector<std::string>& args) {
			// ONCE: a single allocation on the first armed frame after this one.
			// The console stays OPEN, so that frame is the first one after it shuts
			// - an `alloctest` typed next makes it the window's first frame, the one
			// whose stacks went uncaptured until code-review C214.
			if (!args.empty() && args[0] == "once") {
				m_allocPokeOnce = true;
				m_console.Print("allocpoke: one allocation on the first armed frame after "
								"the console shuts");
				return;
			}
			const float seconds =
				args.empty() ? 30.0f
							 : std::clamp(static_cast<float>(std::atof(args[0].c_str())),
										  1.0f, 600.0f);
			m_allocPokeRemaining = seconds;
			m_console.Print(std::format("allocpoke: allocating every frame for {:.0f}s",
										seconds));
			if (m_console.IsOpen()) m_console.Toggle();
		});
	// The typing harness's way to fail on purpose (tools\TypingTest.ps1
	// -SelfTest): the loss Input exists to prevent, done deliberately. The
	// console stays open, so it is the harness's next lines that go missing -
	// WHOLE, through the end of the line the window closes in (Game::Update),
	// since a fragment could complete into any command, `save` included.
	m_console.Register(
		{.name = "inputpoke",
		 .group = CmdGroup::Diagnostics,
		 .params = "[seconds]",
		 .summary = "drop typed lines for a while on purpose (proves TypingTest can fail)"},
		[this](const std::vector<std::string>& args) {
			// SECONDS, not frames: the harness paces itself in milliseconds, and
			// a frame count is a different length on every monitor.
			m_inputPokeRemaining =
				args.empty() ? 1.0f
							 : std::clamp(static_cast<float>(std::atof(args[0].c_str())),
										  0.1f, 60.0f);
			m_console.Print(std::format("inputpoke: dropping typed lines for {:.1f}s",
										m_inputPokeRemaining));
		});
	// The file layer's own answer for one path (code-review C231, C384):
	// tools\PathsTest.ps1 reads a file and a directory in a folder outside ASCII
	// through it. The rest of the line is the path, so one holding single spaces
	// survives the console's word split.
	m_console.Register(
		{.name = "readfile",
		 .group = CmdGroup::Diagnostics,
		 .params = "<path>",
		 .summary = "read a file through assets::ReadBinaryFile; print its size or why not"},
		[this](const std::vector<std::string>& args) {
			if (!Need(m_console, args, 1)) return;
			std::string path = args[0];
			for (size_t i = 1; i < args.size(); ++i) path += ' ' + args[i];
			const auto bytes = assets::ReadBinaryFile(path);
			if (bytes)
				m_console.Print(std::format("readfile: {} bytes from {}", bytes->size(), path));
			else
				m_console.Print(std::format("readfile: none - {}", bytes.error()));
		});
	// A KEY, PRESSED AND LET GO, as the window delivers one (Input::OnKey down,
	// then up): this frame's Update hears it like a key the player hit. A
	// script's lines run BEFORE the frame reads its input (PumpEvalScript sits
	// at the top of UpdateStates), so the key reaches whatever owns the input
	// this frame - a dialog's Esc, the editor's ladder, the pause menu. A check
	// of what those do with a key goes through here rather than calling the
	// handler it means to reach, which would skip the very ordering under test
	// (code-review C81: an Esc that a drop-down should take). Typed at the OPEN
	// console, the console owns the frame and hears it first.
	m_console.Register(
		{.name = "presskey",
		 .group = CmdGroup::Diagnostics,
		 .params = "esc|enter|back|space|<letter or digit>",
		 .summary = "press and release a key this frame, as the keyboard would"},
		[this](const std::vector<std::string>& args) {
			if (!Need(m_console, args, 1)) return;
			std::string name = args[0];
			for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			int key = -1;
			if (name == "esc") key = vk::Escape;
			else if (name == "enter") key = vk::Return;
			else if (name == "back") key = vk::Back;
			else if (name == "space") key = vk::Space;
			else if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0])))
				key = std::toupper(static_cast<unsigned char>(name[0])); // 'A'.., '0'..
			if (key < 0) {
				m_console.RefuseUsage();
				return;
			}
			Input& input = m_window.GetInput();
			input.OnKey(key, true);
			input.OnKey(key, false);
			m_console.Print("presskey: " + name);
		});
	// --- the one-pipeline check (Game/DamageLedger.h, docs/effects.md) --------
	// The same three-command shape the allocation guard uses, for the same
	// reason: a readout, an arming switch, and a way to make it FAIL on purpose.
	m_console.Register(
		{.name = "pipeline",
		 .group = CmdGroup::Diagnostics,
		 .summary = "report what moved health and whether anything went around it"},
		[this](const std::vector<std::string>&) {
			for (const std::string& line : m_world->DamageLedgerReport())
				m_console.Print(line);
		});
	m_console.Register(
		{.name = "pipelineguard",
		 .group = CmdGroup::Diagnostics,
		 .params = "[on|off]\n"
				   "strict [on|off]\n"
				   "reset",
		 .summary = "arm, tighten or reset the one-pipeline check; bare prints state"},
		[this](const std::vector<std::string>& args) {
			ledger::Ledger& led = m_world->DamageLedger();
			if (!args.empty()) {
				const std::string& a = args[0];
				if (a == "on" || a == "off") {
					led.Arm(a == "on");
					// An arming takes a FRESH baseline: whatever moved while it was
					// off is not a violation, it is simply unobserved, and reporting
					// it would make turning the check on look like finding a bug.
					m_world->RebaseDamageLedger();
				} else if (a == "strict") {
					led.SetStrict(args.size() < 2 || args[1] == "on");
				} else if (a == "reset") {
					led.ResetStats();
					m_world->RebaseDamageLedger();
				} else {
					m_console.RefuseUsage(); // the registered forms (C442)
					return;
				}
			}
			m_console.Print(std::format("pipelineguard: armed={} strict={}",
										led.Armed() ? "on" : "off",
										led.Strict() ? "on" : "off"));
		});
	m_console.Register(
		{.name = "pipelinepoke",
		 .group = CmdGroup::Diagnostics,
		 .params = "[member]",
		 .summary = "move health WITHOUT the pipeline (proves the check can fail)"},
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
		{.name = "allocguard",
		 .group = CmdGroup::Diagnostics,
		 .params = "[status]\n"
				   "strict <on|off>\n"
				   "partypage <on|off>\n"
				   "reset",
		 .summary = "steady-state allocation guard: per-thread counts, strict, reset"},
		[this](const std::vector<std::string>& args) {
			const std::string sub = args.empty() ? "status" : args[0];
			// tools\AllocTest.ps1 -PartyPage's switch: the IDLE party creation
			// page is guarded too (Game::GuardedState). Not saved.
			if (sub == "partypage") {
				if (!Need(m_console, args, 2, "usage: allocguard partypage <on|off>")) return;
				m_guardPartyPage = args[1] == "on" || args[1] == "1";
				m_console.Print(std::format("party page guarded: {}",
											m_guardPartyPage ? "on" : "off"));
				return;
			}
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
			// What the log could not name: violating frames with no stack, and
			// captures the full stack set turned away (code-review C214 / C226) -
			// captures, not sites: a repeating site counts each time.
			if (g.framesUncaptured > 0 || g.stacksTurnedAway > 0)
				m_console.Print(std::format("  {} violating frame(s) with no stack captured, {} "
											"captured stack(s) not logged (the stack set is "
											"full; a repeating site counts each time)",
											g.framesUncaptured, g.stacksTurnedAway));
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
	m_console.Register(
		{.name = "loadstats",
		 .group = CmdGroup::Diagnostics,
		 .summary = "reprint the last staged load's per-task time/allocation table"},
		[this](const std::vector<std::string>&) {
			LogLoadStats(/*echoToConsole=*/true);
		});
	m_console.Register(
		{.name = "threadspawn",
		 .group = CmdGroup::Threads,
		 .params = "[busy-ms]",
		 .summary = "spawn a demo worker that is busy for a while every tick"},
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
		{.name = "threadwedge",
		 .group = CmdGroup::Threads,
		 .summary = "spawn a WEDGED worker that ignores its stop token (tests kill)"},
		[this](const std::vector<std::string>&) {
			const threads::WorkerId id = m_threads.Spawn(
				[](const threads::Tick&) {
					// Deliberately does NOT check the stop token: cooperative stop
					// can't end this — only a hard Kill (force-terminate) will.
					while (true) std::this_thread::sleep_for(std::chrono::milliseconds(50));
				},
				{"demo.wedged", 1.0f, /*watchdogMs=*/200});
			m_console.Print(std::format("spawned WEDGED worker #{} (use threadkill)", id));
		});
	// The THREADS panel's kill button as a command, so a script can reach the
	// one failure kind nothing else drives: a forced kill (diag Killed), with
	// the stack the victim was stuck in (code-review C387; HealthTest `kill`).
	m_console.Register(
		{.name = "threadkill",
		 .group = CmdGroup::Threads,
		 .params = "<id|name>",
		 .summary = "hard-kill a worker: stop it, force-terminating one that will not stop"},
		[this](const std::vector<std::string>& args) {
			if (!Need(m_console, args, 1)) return;
			const threads::WorkerId id = FindWorker(m_threads, args[0]);
			if (m_threads.Inspect(id).id == threads::kInvalidWorker) {
				m_console.Print(std::format("no worker '{}' (see `threads`)", args[0]));
				return;
			}
			m_threads.Kill(id); // blocks through the 250 ms grace of a wedged one
			const threads::WorkerInfo after = m_threads.Inspect(id);
			m_console.Print(std::format("killed '{}' #{}: {}", after.name, id,
										threads::StateName(after.state)));
		});
	m_console.Register(
		{.name = "crashpoke",
		 .group = CmdGroup::Diagnostics,
		 .params = "[throw]\n"
				   "uiclip\n"
				   "worker\n"
				   "fault\n"
				   "assert",
		 .summary = "break something on purpose (proves the health record catches it)"},
		[this](const std::vector<std::string>& args) {
			const std::string what = args.empty() ? "throw" : args[0];

			// Thrown from a console callback, which runs inside Game::Update,
			// which runs inside the main loop's try — so this exercises the real
			// main-thread path, not a special case built to be caught.
			if (what == "throw")
				throw std::runtime_error("crashpoke: a deliberate main-thread throw");

			// The same throw, from INSIDE a clipping scroll area's walk (code-review
			// C208): a scratch tree whose area throws once, beside a button outside
			// it. The tree stays up and is walked every frame (Game::Update), so a
			// click on the button afterwards shows whether the clip outlived the
			// throw - before the fix it did, in every context, and the click was
			// lost. Its point is logged first: the throw ends this command.
			if (what == "uiclip") {
				const ui::UIContext* hud = m_ui.UiTree("hud");
				m_clipPoke = std::make_unique<ClipThrowPoke>(
					m_fonts, hud ? hud->DesignHeight() : 17.0f);
				const float w = static_cast<float>(m_window.Width());
				const float h = static_cast<float>(m_window.Height());
				m_clipPoke->Layout(w, h);
				const gfx::Rect b = m_clipPoke->ButtonRect();
				const gfx::Rect a = m_clipPoke->AreaRect();
				log::Info("crashpoke uiclip: the button outside the scroll area "
						  "[{:.0f},{:.0f} {:.0f}x{:.0f}] is at {:.0f},{:.0f} - click it "
						  "once the throw is recorded",
						  a.x, a.y, a.w, a.h, b.x + b.w * 0.5f, b.y + b.h * 0.5f);
				m_clipPoke->Arm();
				m_clipPoke->Update(Input{}, w, h); // throws from inside the area's clip
				return;
			}

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
			m_console.RefuseUsage(); // the registered forms (C442)
		});
	m_console.Register(
		{.name = "health",
		 .group = CmdGroup::Diagnostics,
		 .params = "[thread]\n"
				   "probe <id|name>",
		 .summary = "health record: recent failures, a thread's stacks, a live probe"},
		[this](const std::vector<std::string>& args) {
			// --- the probe ---------------------------------------------------
			// A STALLED thread has thrown nothing, so the record has nothing to
			// show: it is still running, just not finishing. The only way to
			// answer "what is it stuck on" is to go and look.
			if (!args.empty() && args[0] == "probe") {
				if (!Need(m_console, args, 2, "usage: health probe <worker id|name>"))
					return;
				const std::string& who = args[1];
				const threads::WorkerId id = FindWorker(m_threads, who);
				const threads::WorkerInfo info = m_threads.Inspect(id);
				if (info.id == threads::kInvalidWorker) {
					m_console.Refuse(std::format("no worker '{}' (see `threads`)", who));
					return;
				}
				void* frames[stack::kMaxFrames];
				const int n = m_threads.CaptureStack(id, frames, stack::kMaxFrames);
				if (n == 0) {
					m_console.Print(std::format(
						"could not walk '{}' (#{}, {}) - dead, quarantined, mid-reboot, or this "
						"thread",
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
						// Same plumbing rule as the log and the timeline (a walked
						// stack whole, with room for the OS frames on top of the
						// job's); `shown` counts survivors so the budget is not
						// spent on ntdll.
						const int budget = ev[i].walked ? 12 : 6;
						for (int f = 0, shown = 0; f < ev[i].frameCount && shown < budget; ++f) {
							const std::string fr = stack::Describe(ev[i].frames[f]);
							if (!ev[i].walked && stack::IsPlumbingFrame(fr)) continue;
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
	m_console.Register(
		{.name = "throttle",
		 .group = CmdGroup::Threads,
		 .params = "[scale]",
		 .summary = "set the manual global cadence scale (1 = normal; governor off)"},
		[this](const std::vector<std::string>& args) {
			const float s = args.empty() ? 1.0f
										 : static_cast<float>(std::atof(args[0].c_str()));
			m_governorAuto = false; // manual override turns auto off
			m_threads.SetGlobalThrottle(s);
			m_console.Print(std::format("global throttle: {:.2f}x (auto off)",
										m_threads.GlobalThrottle()));
		});
	m_console.Register(
		{.name = "governor",
		 .group = CmdGroup::Threads,
		 .params = "auto [target-fps]\n"
				   "off",
		 .summary = "adaptive thread throttle: eases cadences when frames run long"},
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
	m_console.Register(
		{.name = "threadprio",
		 .group = CmdGroup::Threads,
		 .params = "<id> <-2..2>",
		 .summary = "set a worker's OS thread priority"},
		[this](const std::vector<std::string>& args) {
			if (!Need(m_console, args, 2)) return;
			m_threads.SetPriority(static_cast<threads::WorkerId>(std::atoi(args[0].c_str())),
								  std::clamp(std::atoi(args[1].c_str()), -2, 2));
			m_console.Print("priority set");
		});
	m_console.Register(
		{.name = "threadaffinity",
		 .group = CmdGroup::Threads,
		 .params = "<id> <mask>",
		 .summary = "pin a worker to a CPU affinity mask"},
		[this](const std::vector<std::string>& args) {
			if (!Need(m_console, args, 2)) return;
			m_threads.SetAffinity(static_cast<threads::WorkerId>(std::atoi(args[0].c_str())),
								  std::strtoull(args[1].c_str(), nullptr, 0));
			m_console.Print("affinity set");
		});
	// The THREADS panel as text: a line per worker in the registry, which is what
	// a script can read (the panel is a picture). A world switch must leave no
	// Dead `ai.bucketN` behind - each held a job aimed at the director it
	// destroyed (code-review C69; tools/AITest.py reads this).
	m_console.Register(
		{.name = "threads",
		 .group = CmdGroup::Threads,
		 .summary = "list every managed worker: id, name, state, ticks, restarts"},
		[this](const std::vector<std::string>&) {
			const std::vector<threads::WorkerInfo> all = m_threads.SnapshotAll();
			m_console.Print(std::format("threads: {} worker(s)", all.size()));
			for (const threads::WorkerInfo& w : all)
				m_console.Print(std::format("  #{} {} {} it={} re={}{}", w.id, w.name,
											threads::StateName(w.state), w.iterations,
											w.restarts, w.paused ? " paused" : ""));
		});
	m_console.Register(
		{.name = "threadreap",
		 .group = CmdGroup::Threads,
		 .summary = "drop stopped (dead/quarantined) workers from the registry"},
		[this](const std::vector<std::string>&) {
			const size_t before = m_threads.Count();
			m_threads.Reap();
			m_console.Print(std::format("reaped {} worker(s)", before - m_threads.Count()));
		});
	m_console.Register(
		{.name = "uitree",
		 .group = CmdGroup::Diagnostics,
		 .params = "[on|off]\n"
				   "dump [tree]",
		 .summary = "outline the UI control tree, or print one tree's pixel rects"},
		[this](const std::vector<std::string>& args) {
			if (!args.empty() && args[0] == "dump") {
				const std::string name = args.size() > 1 ? args[1] : "hud";
				ui::UIContext* tree = m_ui.UiTree(name);
				if (!tree) {
					m_console.Refuse(std::format("unknown tree '{}'; try: {}", name,
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
		{.name = "uioverlap",
		 .group = CmdGroup::Diagnostics,
		 .params = "[label]",
		 .summary = "audit visible widget trees for overlaps and escapes (also logged)"},
		[this](const std::vector<std::string>& args) {
			// A findings list is worth reading somewhere other than a console
			// that scrolls, so it goes to dungeon.log too — which is what makes
			// a scripted sweep of every screen collectable afterwards. An
			// optional argument labels the run, so a log holding a dozen of them
			// says which screen each was.
			const std::string label = args.empty() ? std::string() : args[0];
			m_console.Print("uioverlap: auditing the next frame's trees...");
			// The header carries the app STATE: a label only says the command
			// ran, and a sweep that meant the paused menu or the sheet but found
			// the title (a party wiped mid-sweep) is a clean audit of the wrong
			// screen. tools\InGameTest.ps1 requires each label's state, and the
			// screen's own status line before it (code-review C427).
			if (!label.empty()) log::Info("uioverlap [{}] --- state {}", label, StateName());
			// Verbatim: the summary line names itself and the findings are
			// indented under the header above, so a tag here only read as
			// "uioverlap uioverlap: clean".
			ui::inspect::ArmOverlapAudit([this](const std::string& line) {
				m_console.Print(line);
				log::Info("{}", line);
			});
		});
	m_console.Register(
		{.name = "clippoke",
		 .group = CmdGroup::Diagnostics,
		 .summary = "check a scroll area nested in a tab page leaves its siblings unclipped"},
		[this](const std::vector<std::string>&) {
			// A scratch tree, walked here and gone with the command (Game/
			// ClipPoke.h). Logged as well as printed: tools\HealthTest.ps1 reads
			// only dungeon.log, with the console's echo off.
			const ui::UIContext* hud = m_ui.UiTree("hud");
			const std::string line = RunNestedClipCheck(
				m_fonts, hud ? hud->DesignHeight() : 17.0f, m_spriteBatch,
				static_cast<float>(m_window.Width()), static_cast<float>(m_window.Height()));
			m_console.Print(line);
			log::Info("{}", line);
		});
}

} // namespace dungeon::game
