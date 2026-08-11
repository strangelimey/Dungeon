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
		bool hidden = false; // collapsed to a one-line row in the graph view
		float pending = 0.0f; // max since the last commit
		float samples[kProfHistory] = {};
	};
	ProfSeries m_profSeries[kProfSeries];
	int m_profSeriesCount = 0;

	// --- readout smoothing (the LIST view's numbers) ------------------------
	// Every row is one PUBLISHED PERIOD — a frame for the main thread — so at
	// 240 fps the digits changed 240 times a second and could not be read at
	// all. Averaging alone does not fix that: a mean recomputed every frame is
	// smoother but still repaints its low digits every frame, and the eye needs
	// the number to HOLD STILL more than it needs it to be exact.
	//
	// So the window is a TUMBLING one, not a sliding one. Samples accumulate for
	// kProfSmoothSec, then the mean is committed and DISPLAYED UNCHANGED until
	// the next commit. One mechanism buys both halves: the value stops being a
	// single noisy sample, and it stops moving long enough to read.
	//
	// The exception is `max`, which takes the MAXIMUM over the window rather than
	// the mean, for the same reason the graph history does — a mean would average
	// away the one call that spiked, and that call is the whole reason the column
	// is there. Averaging a worst case produces a number that is neither.
	static constexpr int kProfSmoothSlots = 256; // >= kMaxProfRows (asserted)
	static constexpr float kProfSmoothSec = 0.25f; // 4 readable updates a second

	struct ProfSmooth {
		u32 tid = 0;  // the same (thread, node index) key the series use, and
		u32 node = 0; // for the same reason: row position is not an identity
		bool used = false;
		bool seen = false; // matched a live row this frame; else the node has gone

		// Accumulating over the current window.
		double sumIncl = 0.0, sumExcl = 0.0, sumCalls = 0.0, sumFrac = 0.0;
		double winMax = 0.0;
		int count = 0;

		// Committed at the end of a window — what the list actually draws, held
		// steady until the next one. `ready` is false only for a node discovered
		// mid-window, which draws its raw values for the remainder rather than a
		// zero it never measured.
		bool ready = false;
		double incl = 0.0, excl = 0.0, calls = 0.0, maxMs = 0.0, frac = 0.0;
	};
	ProfSmooth m_profSmooth[kProfSmoothSlots];
	int m_profSmoothCount = 0;
	float m_profSmoothTimer = 0.0f;
	// Runtime-tunable, because 250 ms is a guess at what reads well and the right
	// answer differs between watching a steady frame and chasing a spike. Zero
	// commits every frame, which is exactly the old unsmoothed behaviour — "off"
	// needs no separate code path.
	float m_profSmoothSec = kProfSmoothSec;

	// Accumulates one frame's rows / commits the window. Both no-ops without
	// DN_PROFILE, like the series pair above.
	void CommitProfileSmooth();
	// The committed values for a row, or null if it has none yet.
	const ProfSmooth* SmoothFor(u32 tid, u32 node) const;

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
	bool m_perfHidden[kPerfLines] = {};

	// --- the frame budget over time ------------------------------------------
	// The verdict says what is holding the frame back NOW. This is the same
	// three numbers with twelve seconds of history behind them, which is the only
	// way to see an INTERMITTENT bound — a hitch that is GPU-bound for 200 ms
	// inside an otherwise display-bound run never shows up in an instantaneous
	// readout, because by the time you have read it, it is over.
	//
	// Shares m_profHead with every other graph, so a spike here lines up with the
	// zone that caused it.
	//
	// KEPT AS MAXIMA PER WINDOW, independently per line — so these are three
	// ENVELOPES, not a decomposition. cpu and gpu at one x may come from
	// different frames inside that 50 ms, and reading them as slices that should
	// sum to frame would be wrong. The stacked bar is where the decomposition
	// lives; this is where the trends do.
	enum BudgetLine { kBudFrame, kBudCpu, kBudGpu, kBudgetLines };
	PerfSeries m_budgetSeries[kBudgetLines];

	// ONE palette for the budget, because it is read in three places — the
	// stacked bar on the frame row, this graph, and the evidence text that acts
	// as their shared legend. Three sites picking their own colours is three
	// chances for "amber" to mean something different in each.
	static constexpr Vec4 kBudgetCpuColor{0.85f, 0.70f, 0.40f, 1.0f};
	static constexpr Vec4 kBudgetWaitColor{0.80f, 0.55f, 0.85f, 1.0f};
	static constexpr Vec4 kBudgetPresentColor{0.45f, 0.70f, 0.95f, 1.0f};
	static constexpr Vec4 kBudgetGpuColor{0.55f, 0.85f, 0.55f, 1.0f};

	// Checkbox rects for the graph views, rebuilt by Render and hit-tested by the
	// next Update — the same idiom as the thread controls, so the geometry of a
	// clickable thing lives in exactly one place.
	//
	// HIDING DOES NOT REMOVE. A hidden graph collapses to a one-line row carrying
	// the same checkbox, listed under the visible ones, so the control that hid it
	// is the control that brings it back and it is still where you left it. That
	// is why there is no separate "restore" UI to find.
	struct GraphToggle {
		gfx::Rect box;
		int perfLine = -1; // >= 0 for a top gauge, else a profile node below
		u32 tid = 0;
		u32 node = 0;
	};
	std::vector<GraphToggle> m_graphToggles;

	// --- click-to-expand on the tree ----------------------------------------
	// A zone row in the LIST view is a control: clicking it raises that subtree's
	// detail a level, so the inner zones under it start recording and appear
	// beneath it next frame. Same laid-out-by-Render, hit-tested-by-Update idiom
	// as everything else on the panel.
	//
	// Addressed by (slot, node), NOT by the path the `profile detail` command
	// takes. A path names a node in every thread whose tree has it, which is the
	// right behaviour for a typed command reaching all four AI workers at once
	// and the wrong one for a click that landed on exactly one row.
	//
	// The path is carried anyway, purely to say what happened in the scrollback:
	// a click whose subtree has no deeper zones changes nothing visible, and a
	// control that silently does nothing reads as a broken one.
	struct ProfDetailHit {
		gfx::Rect box;
		u32 slot = 0;
		u32 node = 0;
		i8 next = -1;     // the level this click applies (-1 clears)
		bool atMax = false; // already as deep as call sites go; report, don't act
		char path[128] = {};
	};
	std::vector<ProfDetailHit> m_profDetailHits;

	// --- health over time ---------------------------------------------------
	// A mark per sample window per thread: when it threw, when it stalled, when
	// it was rebooted. Shares the profile history's x-axis (m_profHead,
	// kProfSampleSec), so a mark lines up with the spike it caused.
	//
	// NOT drawn as rows on the profile graphs, for two reasons. Those rows are
	// per NODE and health is per THREAD, so the granularity does not match; and
	// the profiler is compiled out of plain debug and release builds while the
	// health record is not — a crash happens in an ordinary build, which is
	// exactly where a timeline is most wanted. It gets its own strip on the same
	// axis instead.
	//
	// A cell keeps the MOST SEVERE kind in its window rather than the last, for
	// the same reason the profile series keeps the max: the one event worth
	// seeing must not be averaged away by the three around it.
	struct HealthCell {
		u8 count = 0;      // events recorded in this window
		u8 kind = 0xFF;    // most severe diag::Kind seen, 0xFF = nothing
		u32 lastIndex = 0; // its per-thread event index, for the click-through
	};
	static constexpr int kHealthRows = 12; // threads shown on the timeline
	struct HealthRow {
		char name[32] = {};
		u32 slot = 0;
		bool used = false;
		u64 prev[6] = {}; // per-kind totals at the last commit (diag::kKindCount)
		HealthCell cells[kProfHistory];
	};
	HealthRow m_healthRows[kHealthRows];
	int m_healthRowCount = 0;
	bool m_healthExpanded = true;
	gfx::Rect m_healthBtn{};

	// Laid out by Render, hit-tested by the next Update — the panel's standing
	// idiom, so a clickable thing's geometry lives in exactly one place.
	struct HealthHit {
		gfx::Rect strip;
		int row = 0;
	};
	std::vector<HealthHit> m_healthHits;

	// Diffs the record's per-thread counters into this sample's cells.
	void SampleHealth();
	// Prints one cell's event — message, thread, tick, stack — to the scrollback.
	void ReportHealthCell(int row, int cell);

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
	// Every section collapses to its header, so the panel can be cut down to just
	// the one thing being watched. THREADS starts collapsed because it is a
	// CONTROL surface — halt, rate, kill, boot — rather than a readout, and its
	// buttons should not push the numbers you came to read down the screen.
	bool m_perfExpanded = true;
	bool m_profileExpanded = true;
	bool m_threadsExpanded = false;

	bool m_profileGraph = false; // list of current values, or scrolling graphs
	bool m_perfGraph = false;    // the six top gauges, as bars or as graphs

	// All laid out by Render, hit-tested by the next Update.
	gfx::Rect m_profViewBtn{};
	gfx::Rect m_perfViewBtn{};
	gfx::Rect m_perfExpandBtn{};
	gfx::Rect m_profExpandBtn{};
	gfx::Rect m_threadsBtn{};

	// The readout panel scrolls as ONE unit rather than per section: three scroll
	// states and three pinned headers is machinery this does not need yet, and
	// scrolling the gauges away to reach the profile is a normal thing to do.
	//
	// Deliberately NOT ui::ScrollArea. The console is outside the widget tree by
	// design — its own palette, no Loc, immediate-mode drawing, and a scrollback
	// that already scrolls itself — so it clips with SpriteBatch::SetScissor,
	// which exists for exactly this. Converting the whole console to the control
	// tree is a real option, just a separate one; see the note in the .cpp.
	float m_panelScroll = 0.0f;
	float m_panelH = 0.0f; // last frame's panel height, for wheel hit-testing
	float m_lineH = 16.0f; // last frame's line advance, so Update can step by lines
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
