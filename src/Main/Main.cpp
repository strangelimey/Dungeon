// Composition root: constructs the engine modules, wires them together, and
// runs the frame loop. No game logic lives here.

#include "Audio/AudioEngine.h"
#include "Core/AllocTrack.h"
#include "Core/CrashHandler.h"
#include "Core/Diagnostics.h"
#include "Core/Log.h"
#include "Core/Profile.h"
#include "Core/StackTrace.h"
#include "Core/Time.h"
#include "Game/Game.h"
#include "Game/GameSettings.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/Renderer.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Window.h"

#include <Windows.h>

#include <shellapi.h> // CommandLineToArgvW — the `-eval` flag

#include <format>
#include <string>
#include <string_view>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
	using namespace dungeon;

#ifdef _DEBUG
	// Show a console for logs in debug builds. It is OURS — allocated here — so
	// its code page is ours to set, and log lines are UTF-8 (see Core/Log.h).
	AllocConsole();
	FILE* unused = nullptr;
	freopen_s(&unused, "CONOUT$", "w", stdout);
	freopen_s(&unused, "CONOUT$", "w", stderr);
	log::UseUtf8Console();
#endif

	// Names this thread for the allocation counters — and, because the operator
	// new replacements live in that translation unit, is what makes the linker
	// pull them in at all (see Core/AllocTrack.h's linker note).
	alloc::Init();

	// Calibrates the TSC and names this thread "main". No-op without DN_PROFILE.
	prof::Init();

	// The health record, then the handlers that feed it what no catch clause can
	// see. Installed HERE, before the window and the device exist, because a
	// fault during device creation is exactly as worth reporting as one during
	// play — and until now was exactly as silent.
	diag::Init();
	crash::Install();

	log::Info("Dungeon starting...");

	// Display config is needed before the window/device exist, so read settings
	// here too; Game loads its own (live) copy from the same file.
	game::GameSettings boot;
	boot.Load();

	// `-headless` (docs/eval-harness.md): no window on screen and no drawing —
	// the game simulates, the dev console still runs, and everything worth
	// reading comes out of dungeon.log. Read BEFORE the window exists, because
	// whether it is ever shown is a property of its creation.
	//
	// WHAT IT DOES NOT DO is remove the graphics device. The swapchain is bound
	// to an HWND, and prising the device out would mean a null path at every gfx
	// call site — mesh building, texture upload, icon bakes, font atlases — for
	// no gain, because what a headless run saves is the PER-FRAME cost, not the
	// once-per-process cost of owning a device. A machine with no GPU is already
	// handled a layer down: GraphicsDevice falls back to WARP.
	//
	// It is a FLAG rather than something `-eval` implies, because watching an
	// eval run play out is exactly how several of these scripts were debugged.
	bool headless = false;
	{
		int argc = 0;
		LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		for (int i = 1; argv && i < argc; ++i)
			if (std::wstring_view(argv[i]) == L"-headless") headless = true;
		if (argv) LocalFree(argv);
	}

	WindowDesc desc;
	desc.title = "Dungeon";
	desc.hidden = headless;
	if (boot.displayWidth > 0 && boot.displayHeight > 0) {
		desc.width = static_cast<u32>(boot.displayWidth);
		desc.height = static_cast<u32>(boot.displayHeight);
	}
	Window window(desc);

	gfx::GraphicsDevice device(window.Handle(), window.Width(), window.Height(),
							   boot.adapterLuid);
	gfx::Renderer renderer(device);
	gfx::SpriteBatch spriteBatch(device);
	audio::AudioEngine audioEngine;

	window.onResize = [&device](u32 w, u32 h) { device.Resize(w, h); };

	game::Game game(window, device, renderer, spriteBatch, audioEngine);

	// `-eval <script> [script...]`: run console scripts and exit with their
	// verdict instead of waiting for someone to play. Read HERE rather than up
	// with the display settings because the scripts are the Game's to own —
	// nothing earlier could hold them.
	//
	// SEVERAL SCRIPTS RUN IN ONE PROCESS, which is the whole point: a dungeon
	// load is ~12 seconds and a `reset` is ~340 ms, so twenty scripts in one
	// process pay one load instead of twenty (docs/eval-harness.md). Every
	// non-flag argument after `-eval` is a script, and `-eval` may be repeated.
	//
	// THE FIRST script failing to LOAD is fatal on the spot: a "test run" that
	// silently sat at the title screen would report whatever the harness assumed
	// rather than what happened. A LATER one failing is handled by the runner,
	// which counts it and carries on with the rest.
	{
		int argc = 0;
		LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		bool bad = false, first = true;
		for (int i = 1; argv && i < argc; ++i) {
			if (std::wstring_view(argv[i]) != L"-eval") continue;
			// Consume every following argument that is not itself a flag.
			for (int j = i + 1; j < argc && argv[j][0] != L'-'; ++j) {
				const std::wstring wide(argv[j]);
				const std::string path(wide.begin(), wide.end()); // ASCII paths only
				if (first) {
					bad = !game.LoadEvalScript(path);
					first = false;
				} else {
					game.QueueEvalScript(path);
				}
			}
		}
		if (argv) LocalFree(argv);
		if (bad) return 2; // distinct from a FAILING script: this one never ran
	}

	Timer timer;
	const float clearColor[4] = {0.01f, 0.01f, 0.015f, 1.0f};

	// The main thread's failure policy (docs/diagnostics.md): a frame that throws
	// is survived, and the SAME thing happening ten frames running is not.
	//
	// One bad frame is worth surviving — the world is still there and the next
	// frame usually draws — but a fault that repeats every frame is a broken
	// build, and spinning on it forever buys nothing except thousands of
	// identical reports and a game nobody can play.
	//
	// KNOWN SHARP EDGE: a throw between device.BeginFrame and device.EndFrame
	// leaves the command list open, so the following frame is unlikely to be
	// sound. The counter below is what bounds that — a render fault that really
	// has broken the device will trip it within ten frames rather than limping on
	// indefinitely.
	constexpr int kMaxConsecutiveFrameFailures = 10;
	int consecutiveFailures = 0;
	u64 frameIndex = 0;
	bool fatal = false;

	const auto frameFailed = [&](const char* what) {
		// The throw-time stack, not this catch site's — see Core/StackTrace.
		void* frames[stack::kMaxFrames];
		const int n = stack::ThrowFrames(frames, stack::kMaxFrames);
		diag::Record({.kind = diag::Kind::Exception,
					  .iteration = frameIndex,
					  .message = what,
					  .frames = n > 0 ? frames : nullptr,
					  .frameCount = n});
		if (++consecutiveFailures < kMaxConsecutiveFrameFailures) return;
		crash::ReportFatal(
			std::format("the main thread threw on {} consecutive frames — giving up. "
						"Last failure: {}",
						consecutiveFailures, what));
		fatal = true;
	};

	while (window.PumpMessages()) {
		const float dt = timer.Tick();
		++frameIndex;

		// The steady-state allocation guard brackets the WHOLE frame — update,
		// render and present — because the rule covers all of it. Game::Update
		// decides whether this frame counts (alloc::ArmFrame).
		alloc::BeginFrame();
		try {
			// The profiler's root scope. Everything instrumented below hangs off
			// this one, so the tree has a single trunk and "frame" is a real node
			// with a total rather than an implied one. ScopedZone unwinds
			// correctly if anything below throws.
			DN_PROFILE_ZONE(prof::kZoneFrame);
			{
				DN_PROFILE_ZONE(prof::kZoneUpdate);
				game.Update(dt);
			}

			// HEADLESS: the whole render half of the frame goes, and with it the
			// frame cap. That cap is the bigger of the two savings and the less
			// obvious one — it holds every frame to the monitor's refresh, and an
			// eval script runs one line per frame, so a run with nothing to show
			// would otherwise be paced by a display nobody is looking at.
			//
			// EndHeadlessFrame is not optional bookkeeping: the staged loader
			// gates on the frame counter that lives at the bottom of Render, so
			// without it the run never finishes loading (see its definition).
			//
			// Written as a BRANCH rather than an early `continue`, deliberately:
			// the loop's tail publishes the profiler's frame, ends the allocation
			// guard's, and ends the input frame, and a headless run that skipped
			// those would drift from a normal one in three ways that would each
			// take a while to notice.
			if (headless) {
				game.EndHeadlessFrame();
			} else {
				ID3D12GraphicsCommandList* list = nullptr;
				{
					DN_PROFILE_ZONE(prof::kZoneRender);
					list = device.BeginFrame(clearColor);
					// The CPU work of a frame, separated from the two WAITS either
					// side of it (wait.gpu inside BeginFrame, present inside
					// EndFrame). Those three partition `render` into the only three
					// things it can be doing, and which of them dominates IS the
					// answer to whether the frame is CPU-bound, GPU-bound or
					// display-bound. Undivided, all three read as "rendering is
					// expensive".
					{
						DN_PROFILE_ZONE(prof::kZoneRecord);
						game.Render(list);
					}
					device.EndFrame();
				}

				// Hold the frame to the refresh rate of the monitor the window is
				// on (GraphicsDevice::WaitFrameCap explains why Present cannot do
				// this).
				//
				// A SIBLING of `render`, not part of it, and its own zone: this is
				// a wait we chose, and folding it into anything else would show up
				// as that thing getting slower. Named so the budget can subtract it
				// from CPU time — otherwise capping the frame rate would make the
				// console report the engine as CPU-bound, which is the precise
				// opposite of what a frame spent deliberately idle means.
				{
					DN_PROFILE_ZONE(prof::kZoneWaitCap);
					device.WaitFrameCap();
				}
			}
			consecutiveFailures = 0; // a frame that finished clears the streak
		} catch (const std::exception& e) {
			frameFailed(e.what());
		} catch (...) {
			frameFailed("unknown exception (not derived from std::exception)");
		}

		// Ends the frame's period for THIS thread; a worker publishes per tick
		// instead (see Collector::CopyAndReset).
		prof::PublishThisThread();

		alloc::ReportFrame(alloc::EndFrame());

		window.GetInput().EndFrame();

		// Esc is state-dependent (pause in-game, back out of menus); the
		// game raises this when it actually means quit.
		if (game.QuitRequested() || fatal) break;
	}

	device.WaitIdle();
	log::Info("Dungeon shutting down.");

	// Whole-run heap totals per thread. Cheap, and it is the standing check
	// that the operator new replacements are actually linked in — all-zero
	// counters mean the CRT's own new is still in play, not a quiet run.
	if constexpr (alloc::kEnabled) {
		alloc::ThreadReport reports[alloc::kMaxThreads];
		const int n = alloc::SnapshotAll(reports, alloc::kMaxThreads);
		for (int i = 0; i < n; ++i) {
			const alloc::Counters& c = reports[i].counters;
			log::Info("heap[{}] tid {}: {} allocs ({} excused), {} frees, {:.1f} MB requested",
					  reports[i].name, reports[i].osThreadId, c.allocs, c.excused, c.frees,
					  static_cast<double>(c.bytes) / (1024.0 * 1024.0));
		}
	}

	// The LAST published period per thread, as an indented tree. A stand-in for
	// the console panel until that exists: the collector needs to be checkable
	// from a plain run, and an all-zero tree is the tell that instrumentation
	// never reached the registry.
	//
	// It is the last FRAME, not the run — counters reset every publish, which is
	// what keeps a live readout live. A whole-run total is a different feature
	// and would want its own accumulator.
	if constexpr (prof::kEnabled) {
		const prof::Clock clock = prof::ClockInfo();
		prof::ThreadReport reports[prof::kMaxThreads];
		const int n = prof::SnapshotAll(reports, prof::kMaxThreads);
		for (int i = 0; i < n; ++i) {
			const prof::ThreadReport& r = reports[i];
			log::Info("profile[{}] tid {}: {} nodes, final period of {}{}", r.name,
					  r.osThreadId, r.nodeCount, r.periods,
					  r.nodeOverflows || r.depthOverflows
						  ? std::format(" (DROPPED {} nodes, {} depth)", r.nodeOverflows,
										r.depthOverflows)
						  : std::string{});

			// The published tree is flat, with sibling/child indices, so walking
			// it takes an explicit stack rather than recursion into a depth we do
			// not know. Sized for the deepest possible nesting times the widest
			// sibling list we would ever queue at once.
			struct Visit {
				u32 node;
				int depth;
			};
			constexpr int kVisitMax = static_cast<int>(prof::kMaxNodes);
			Visit stack[kVisitMax];
			int top = 0;
			for (u32 c = r.root; c != prof::kInvalidNode && top < kVisitMax;
				 c = r.nodes[c].nextSibling)
				stack[top++] = {c, 0};

			while (top > 0) {
				const Visit v = stack[--top];
				const prof::NodeView& node = r.nodes[v.node];
				log::Info("  {:>{}}{} {:.3f} ms incl, {:.3f} ms excl, {} calls", "",
						  v.depth * 2, node.zone ? node.zone->name : "?",
						  prof::TicksToMs(node.inclusive, clock),
						  prof::TicksToMs(node.Exclusive(), clock), node.calls);
				for (u32 c = node.firstChild; c != prof::kInvalidNode && top < kVisitMax;
					 c = r.nodes[c].nextSibling)
					stack[top++] = {c, v.depth + 1};
			}
		}
	}
	// A scripted run's verdict IS the process's: 0 only when every line matched a
	// command AND the queue emptied. A run that timed out fails even though every
	// line it managed to run succeeded. An ordinary play session has no script,
	// and EvalExitCode is 0 for it.
	return game.EvalExitCode();
}
