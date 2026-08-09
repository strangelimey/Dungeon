// ============================================================================
// Game/DevConsole.h — the Quake-style developer console (toggle with `~`).
//
// A fullscreen overlay drawn on top of everything else: the upper region is a
// live performance panel (FPS, CPU/GPU utilization, system/GPU/process memory,
// adapter name — Task-Manager style), the lower region is a scrollback log and
// a command input line. It is dev-facing, so all text stays English (no Loc).
//
// The console does NOT pause the game — while open the world keeps simulating;
// the Game just routes input here (so the party doesn't move while you type)
// and freezes nothing. Esc closes it. Commands come from a small registry:
// the console seeds the generic ones (help/clear/echo) and the Game registers
// the gameplay-aware ones (quit/fps/quality/lang/tp).
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Core/ThreadManager.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "Platform/PerfMonitor.h"
#include "UI/Font.h"
#include "UI/FontLibrary.h"

#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace dungeon::game {

class DevConsole {
public:
	// No GraphicsDevice: the console draws with a font borrowed from the
	// library, and Render takes the device it queries for GPU/VRAM readouts.
	DevConsole(ui::FontLibrary& fonts, threads::Manager& threadManager);

	bool IsOpen() const { return m_open; }
	void Toggle();

	// Latest smoothed frame rate (sampled every frame, even when closed).
	float Fps() const { return m_perf.Get().fps; }

	// Called every frame (the FPS sampler keeps ticking even when closed).
	// While open, consumes typed characters and editing/history/scroll keys.
	// The device is here only so the history the graph view draws keeps filling
	// while the console is CLOSED — two of the five top gauges (VRAM, descriptor
	// slots) are the device's to answer, and a graph you have to open the console
	// to start recording is no use for catching what already happened.
	void Update(const Input& input, float dt, float windowW, float windowH,
				const gfx::GraphicsDevice& device);
	// Drawn inside the caller's SpriteBatch Begin/End, after the HUD/overlays.
	void Render(gfx::SpriteBatch& batch, const gfx::GraphicsDevice& device,
				float width, float height);

	// Command registry. `fn` receives the whitespace-split arguments (without
	// the command name). Print appends a line to the scrollback.
	void Register(std::string name, std::string help,
				  std::function<void(const std::vector<std::string>&)> fn);
	void Print(std::string line);

	// Gates command EXECUTION (typing/scrollback stay live). The Game disables
	// commands while a staged load is mid-flight — the world is only partially
	// built then, and a handler that reaches into it (cast/save/quality/...)
	// would touch objects a later load task creates. A gated Enter prints a
	// notice and keeps the line in history for an easy re-run after the load.
	void SetCommandsEnabled(bool enabled) { m_commandsEnabled = enabled; }

private:
	void Execute(const std::string& line);

	ui::FontLibrary& m_fonts;
	// Borrowed from the library (Mono: this is a column-aligned readout).
	// Re-pointed every Update, so a face swap lands next frame.
	const ui::Font* m_font = nullptr;
	PerfMonitor m_perf;
	threads::Manager& m_threadMgr;

	// Per-thread control-button rects, rebuilt by Render each frame and hit-tested
	// by the next Update (the panel is static, so one frame of lag is invisible).
	// This keeps the button layout in one place — Render — instead of duplicated.
	struct ThreadHit {
		threads::WorkerId id;
		gfx::Rect pause, slower, faster, kill; // live-worker controls
		gfx::Rect boot;                        // dead-worker reboot (others empty)
	};
	std::vector<ThreadHit> m_threadHits;

	// --- profile history, for the graph view --------------------------------
	// The instantaneous view answers "what is it doing"; this answers "what
	// CHANGED", which is the question a stutter actually poses. Kept here rather
	// than in the profiler because it is a presentation concern: the collector
	// publishes a period and forgets it, and nothing in the engine should carry
	// screen-history it does not use.
	//
	// SAMPLED ON A TIMER, NOT PER FRAME. At 240 fps a per-frame ring would hold
	// about a second, which is too short to see anything travel across it. Each
	// slot instead holds the MAXIMUM seen since the last commit — a mean would
	// average away the one frame that spiked, and that frame is the whole reason
	// to be looking.
	static constexpr int kProfHistory = 240;   // samples per series
	static constexpr int kProfSeries = 32;     // distinct nodes tracked
	static constexpr float kProfSampleSec = 0.05f; // 240 x 50 ms = 12 s of history

	struct ProfSeries {
		u32 tid = 0;   // owning thread, half of the stable key
		u32 node = 0;  // node index within that thread, the other half
		char name[32] = {};
		char thread[32] = {};
		int depth = 0;
		bool used = false;
		bool seen = false;  // matched a live node this sample; else it has gone
		float pending = 0.0f; // max since the last commit
		float samples[kProfHistory] = {};
	};
	ProfSeries m_profSeries[kProfSeries];
	int m_profSeriesCount = 0;

	// The six gauges at the top of the panel, given the same treatment. These
	// differ from the profile series in one way that matters: each has a NATURAL
	// maximum (the display's refresh rate, 100%, installed RAM, the VRAM budget,
	// the descriptor ceiling), so they are drawn against a fixed scale.
	// Autoscaling would redraw 3% CPU as a full graph and make idle look like a
	// crisis. FPS is the interesting one: its ceiling is the MONITOR's refresh
	// rate, which is the only number that makes "is this fast enough" answerable
	// rather than just large.
	enum PerfLine { kFps, kCpu, kGpu, kRam, kVram, kSrv, kPerfLines };
	struct PerfSeries {
		float pending = 0.0f;
		float samples[kProfHistory] = {};
	};
	PerfSeries m_perfSeries[kPerfLines];

	// One head and one timer for BOTH sets, so every graph on screen shares an
	// x-axis and a spike in one lines up with a spike in another.
	int m_profHead = 0;
	float m_profSampleTimer = 0.0f;

	// Accumulates history every frame, open or closed, so switching to a graph
	// view shows the last twelve seconds rather than starting blank. The profile
	// halves are no-ops without DN_PROFILE; the perf half always runs.
	void SampleHistory(float dt, const gfx::GraphicsDevice& device);
	void SampleProfileSeries();
	void CommitProfileSeries();

	bool m_open = false;
	bool m_commandsEnabled = true;   // false while a staged load is mid-flight
	bool m_showProfile = true;       // the profile panel costs real screen height
	bool m_profileGraph = false;     // list of current values, or scrolling graphs
	bool m_perfGraph = false;        // the five top gauges, as bars or as graphs
	// THREADS is a control surface, not a readout, so it starts collapsed and
	// sits at the BOTTOM: its buttons should not push the numbers you came to
	// read further down the screen.
	bool m_threadsExpanded = false;
	gfx::Rect m_profViewBtn{};       // the toggles, all laid out by Render
	gfx::Rect m_perfViewBtn{};
	gfx::Rect m_threadsBtn{};
	std::string m_input;             // current edit line
	std::deque<std::string> m_output; // scrollback (oldest front)
	std::vector<std::string> m_history;
	int m_historyIndex = -1; // -1 = editing a fresh line
	int m_scroll = 0;        // lines scrolled up from the bottom
	float m_caretBlink = 0.0f;

	struct Command {
		std::string name;
		std::string help;
		std::function<void(const std::vector<std::string>&)> fn;
	};
	std::vector<Command> m_commands;
};

} // namespace dungeon::game
