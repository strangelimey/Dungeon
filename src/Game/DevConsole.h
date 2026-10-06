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
//
// Every command registers a CmdInfo - name, group, params, one-line summary -
// and those four fields drive BOTH readers: `help` (grouped, columned, or one
// command in full) and the type-ahead box above the prompt.
//
// One class, seven files: DevConsole.cpp is the frame (input, the scrollback,
// the panel's layout), DevConsole_Commands.cpp the registry and its readers
// (`help`, the type-ahead), and each section of the readout panel is its own
// file - DevConsole_Perf / _Profile / _Health / _Threads.cpp, with the
// profile's snapshots in _Snapshots.cpp - sharing only what DevConsole_Panel.h
// declares.
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

namespace devcon {
struct ProfileFrame; // DevConsole_Panel.h
}

// ---------------------------------------------------------------------------
// What a command is FOR, and the order `help` lists the groups in. An enum
// rather than a string so a command cannot land in a misspelt group of its
// own; DevConsole_Commands.cpp holds each group's title and `help` key.
// ---------------------------------------------------------------------------
enum class CmdGroup : u8 {
	Console,
	Settings,
	Rendering,
	SaveLoad,
	Party,
	Characters,
	Combat,
	Monsters,
	Simulation,
	Levels,
	Types,
	World,
	Diagnostics,
	Threads,
	Profiling,
	Count
};

// A command's description, split so the listing and the type-ahead can lay it
// out in columns.
//   params  - the synopsis in the usual notation: <required> [optional] a|b
//             choices, ... repeats. "" = takes no arguments. A command with
//             several verbs gives ONE FORM PER LINE ('\n'-separated) rather
//             than one long `a | b | c` run. An EMPTY FIRST form ("\nstatus")
//             is the command typed bare, for one whose bare form does
//             something its verbs do not (lists, where `status` reports) -
//             `[status]` would claim the two are the same.
//   summary - one line, no parameters in it.
struct CmdInfo {
	std::string_view name;
	CmdGroup group = CmdGroup::Console;
	std::string_view params;
	std::string_view summary;
};

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
	// while the console is CLOSED — two of the top gauges (VRAM, descriptor
	// slots) are the device's to answer, and a graph you have to open the console
	// to start recording is no use for catching what already happened. The sprite
	// batch is here for the same reason: the UI arena gauge is its to answer.
	void Update(const Input& input, float dt, float windowW, float windowH,
				const gfx::GraphicsDevice& device, const gfx::SpriteBatch& sprites);
	// Drawn inside the caller's SpriteBatch Begin/End, after the HUD/overlays.
	void Render(gfx::SpriteBatch& batch, const gfx::GraphicsDevice& device,
				float width, float height);

	// Command registry. `fn` receives the whitespace-split arguments (without
	// the command name). ASSERTS on a duplicate name - Execute runs the first
	// match, so a second registration is silently unreachable (the per-member
	// `threat` was, for months) - and on an empty or multi-line summary.
	void Register(const CmdInfo& info, std::function<void(const std::vector<std::string>&)> fn);
	// Print appends a line to the scrollback.
	void Print(std::string line);
	// REFUSES with the RUNNING command's registered params, one line per form, so
	// an arity error can never disagree with what `help` says. Only meaningful
	// inside a handler; see devargs::Need.
	void RefuseUsage();

	// A command that RAN and DECLINED to do what it was asked: a `tp` onto rock,
	// a `spawn` into a wall, an unknown spell, a `step` that could not run the
	// time it was given. Printed exactly like any other line — the difference is
	// that the eval runner COUNTS it.
	//
	// This exists because RunLine's bool answers "was there a command by that
	// name", which is not the question a measurement script needs answered. A
	// rung that asked for three monsters, got one because two cells were rock,
	// and printed a perfectly plausible TALLY under a header claiming three,
	// came back PASS (docs/eval-audit.md F11/F15).
	//
	// NOT for a query with nothing to say. `monsters` printing "no monsters" is
	// an ANSWER and stays a Print. Refuse means "you asked me to change the
	// world and I did not" — the distinction is whether a script that carried
	// on regardless would be measuring something other than what it wrote.
	void Refuse(std::string line);

	// True once if the last Execute refused. Clears on read, and Execute clears
	// it before dispatching, so a refusal can never carry into the next line.
	bool ConsumeRefusal() {
		const bool refused = m_refused;
		m_refused = false;
		return refused;
	}
	// Whether the last Execute was refused by the GATE below (the title screen
	// has no world) rather than by the command's own rule. The eval runner's
	// `expect-refuse` asks it: a probe of a rule that "passed" because no game
	// was running would be a check of nothing.
	bool RefusedByGate() const { return m_gateRefused; }

	// Gates command EXECUTION (typing/scrollback stay live). The Game disables
	// commands while a staged load is mid-flight — the world is only partially
	// built then, and a handler that reaches into it (cast/save/quality/...)
	// would touch objects a later load task creates. A gated Enter prints a
	// notice and keeps the line in history for an easy re-run after the load.
	void SetCommandsEnabled(bool enabled) { m_commandsEnabled = enabled; }
	// A PER-COMMAND gate, asked before every command runs — typed or scripted
	// (docs/world-on-demand.md: the title screen has no world, and most
	// commands reach into one). Returns "" to allow, else the refusal to print;
	// a refusal counts as one (Refuse), so a script that runs a world command
	// before starting a game fails rather than measuring nothing.
	std::function<std::string(std::string_view name)> gate;
	bool CommandsEnabled() const { return m_commandsEnabled; }

	// The readout panel's three sections, expanded or collapsed. The console
	// knows nothing of settings.ini: Game seeds the state with SetSections and
	// persists it from onSectionsChanged, which fires on every toggle (a header
	// click or `profile on|off`).
	struct Sections {
		bool perf = false;
		bool profile = false;
		bool threads = false;
	};
	void SetSections(const Sections& s) {
		m_perfExpanded = s.perf;
		m_profileExpanded = s.profile;
		m_threadsExpanded = s.threads;
	}
	Sections GetSections() const { return {m_perfExpanded, m_profileExpanded, m_threadsExpanded}; }
	std::function<void()> onSectionsChanged;

	// MIRROR EVERY CONSOLE LINE TO dungeon.log (the eval harness; `logecho`).
	// The console's scrollback is a WINDOW: reading it means taking a
	// screenshot, and a screenshot captures whatever window is in FRONT. So the
	// console's output — which is where `monsters`, `step` and every other
	// command answer — is invisible to a script driving the game, and that is
	// most of what makes the game hard to drive without clicking.
	//
	// Mirroring turns the whole existing command surface into something readable
	// from a file, and does it without a second reporting path that could
	// disagree with what the console shows. Off by default: a play session
	// should not pay for it.
	void SetMirrorToLog(bool on) { m_mirrorToLog = on; }
	bool MirrorToLog() const { return m_mirrorToLog; }

	// Run one line as if it had been typed, and REPORT whether a command matched.
	// The eval runner needs the answer: a script line that quietly did nothing —
	// a typo, or a command that only exists on another branch — would otherwise
	// leave the run reporting a clean pass over an encounter it never set up.
	// The interactive path (Enter) discards the bool; only a script counts them.
	//
	// This bool is only HALF the question, and the other half is ConsumeRefusal
	// above: a command whose name exists and which then refused returns true
	// here. Ask both, or the run is only checked for typos.
	bool RunLine(const std::string& line) { return Execute(line); }

private:
	bool Execute(const std::string& line); // false = no such command
	bool m_refused = false;                // the running command called Refuse
	bool m_gateRefused = false;            // ...and it was the gate that refused it

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

	// The seven gauges at the top of the panel, given the same treatment. These
	// differ from the profile series in one way that matters: each has a NATURAL
	// maximum (the display's refresh rate, 100%, installed RAM, the VRAM budget,
	// the descriptor ceiling, the UI's vertex arena), so they are drawn against a
	// fixed scale.
	// Autoscaling would redraw 3% CPU as a full graph and make idle look like a
	// crisis. FPS is the interesting one: its ceiling is the MONITOR's refresh
	// rate, which is the only number that makes "is this fast enough" answerable
	// rather than just large.
	// kProc (this process's working set) is a SERIES but not a gauge of its own:
	// it is always a subset of the system RAM in use, so it draws STACKED inside
	// the RAM gauge rather than as a second bar against the same ceiling. It sits
	// past kPerfLines so the gauge count, the hide flags and the layout never
	// see it.
	// kUi is the sprite batch's per-frame vertex arena (code-review C163): a fixed
	// ceiling with a peak, like the descriptor slots it sits beside.
	enum PerfLine { kFps, kCpu, kGpu, kRam, kVram, kSrv, kUi, kPerfLines, kProc = kPerfLines,
					kPerfSeriesCount };
	// The graph view's rows: the frame rate alone across the top (the headline,
	// and the one gauge with no partner), the rest two to a row.
	int PerfGraphRows() const;
	struct PerfSeries {
		float pending = 0.0f;
		float samples[kProfHistory] = {};
	};
	PerfSeries m_perfSeries[kPerfSeriesCount];
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

	// THE TWO PROCESSORS GET ONE COLOUR EACH, WHEREVER THEY APPEAR. The gauges at
	// the top of the panel and the frame budget below it are read together and
	// describe the same two pieces of silicon, so a reader is entitled to assume
	// the colours agree. They did not: the budget's first palette painted CPU
	// work in the gauges' RAM amber and Present in the gauges' CPU blue, which
	// made the biggest block on the bar look like CPU time — the exact
	// misreading the bar was built to prevent.
	//
	// Defined here and used by BOTH the gauge table and the budget, so agreement
	// is structural rather than a thing to remember.
	static constexpr Vec4 kCpuColor{0.45f, 0.70f, 0.95f, 1.0f};
	static constexpr Vec4 kGpuColor{0.55f, 0.85f, 0.55f, 1.0f};

	// The budget's four, three of them derived from that rule:
	//   cpu     = the CPU's colour, because it IS CPU time
	//   gpu     = the GPU's colour, likewise
	//   wait    = the GPU's colour DIMMED — time the CPU lost to the GPU, so it
	//             belongs to the GPU's story without being GPU work
	//   present = neutral grey, because it is not work at all. Idle should look
	//             idle rather than borrow a colour that means something ran.
	static constexpr Vec4 kBudgetCpuColor = kCpuColor;
	static constexpr Vec4 kBudgetGpuColor = kGpuColor;
	static constexpr Vec4 kBudgetWaitColor{0.34f, 0.52f, 0.36f, 1.0f};
	static constexpr Vec4 kBudgetPresentColor{0.44f, 0.44f, 0.48f, 1.0f};
	// The frame cap: idle like Present, so grey like Present — but a LIGHTER
	// grey, since the two sit side by side in the same bar and must be told
	// apart. Lighter rather than darker because this colour also draws the
	// `bound by cap` verdict, and the first attempt at a darker grey was very
	// nearly unreadable as text on a dark panel — the verdict is the one line
	// that has to survive a glance.
	static constexpr Vec4 kBudgetCapColor{0.60f, 0.61f, 0.68f, 1.0f};

	// The ordinary per-node share bar, and it is deliberately NOT the CPU's blue
	// any more. Once blue means "CPU time", a blue bar on the `present` row —
	// which is the CPU doing nothing — says the opposite of the grey segment
	// standing for that same measurement in the frame bar directly above it. A
	// generic proportion needs a colour that claims nothing, so it gets a muted
	// steel and the meaningful colours stay meaningful.
	static constexpr Vec4 kShareColor{0.42f, 0.52f, 0.62f, 1.0f};

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

	// --- the frame bar's hover tooltip ---------------------------------------
	// The stacked bar says the proportions at a glance but names none of them;
	// the colour key is up on the section header, which is the wrong place to
	// look when the thing you are pointing at is down here. Hovering draws the
	// same bar again, larger, with every part labelled and measured.
	//
	// Only the FLAG crosses frames. The rect is recorded by Render and
	// hit-tested by the next Update like every other control on this panel, but
	// the numbers are re-read from the live budget at draw time — storing them
	// would give the tooltip its own copy that could disagree with the bar it is
	// explaining.
	gfx::Rect m_frameBarRect{};
	bool m_frameBarHover = false;

	// The section header carries seven terms in four colours and explains none
	// of them. Hovering it names each one. Same idiom, and deliberately ONE
	// tooltip for the whole line rather than seven: the terms are a single
	// sentence about where a frame went, and reading them one hover at a time
	// would hide the relationship that makes them worth having.
	gfx::Rect m_profHeaderRect{};
	bool m_profHeaderHover = false;

	// Render's canvas size, kept so Update can put the mouse into the same space
	// as the rects Render recorded. Update is handed WINDOW pixels and Render
	// DEVICE pixels, which are equal until something scales them apart; the
	// panel's existing hit-tests compare the two directly and have simply never
	// met a case where they differ.
	float m_renderW = 0.0f;
	float m_renderH = 0.0f;

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

	// --- snapshots: this scene against that one ------------------------------
	// A profiler that only shows NOW cannot answer the question anyone actually
	// has, which is "did that change help". Reading two numbers off two
	// screenshots taken a minute apart is not a comparison — the panel is live,
	// both were sampled over different moments, and nothing lines the rows up.
	//
	// A snapshot RECORDS over a few seconds rather than freezing an instant, for
	// the same reason the readout is smoothed: one frame is not a measurement.
	// Means for the timings, MAX for `worst` — averaging worst cases produces a
	// number that is neither, the same rule the window already follows.
	//
	// Rows are keyed by thread + SLASH PATH, not by index. Between two snapshots
	// the tree will usually have changed shape — that is half the point of
	// taking them — so a positional key would diff one scope against another and
	// report confident nonsense.
	static constexpr int kSnapRows = 192;
	// EIGHT, not four. A real session is a chain — low, ultra, shadowmax, then a
	// baseline for the next question — and four ran out mid-investigation, after
	// which `snap` refused and the `diff` that followed reported an unknown name.
	// The refusal is right (silently evicting the baseline you are measuring
	// against would be worse), so the fix is headroom.
	static constexpr int kSnapSlots = 8;
	struct SnapRow {
		char thread[32] = {};
		char path[128] = {};
		double incl = 0.0, excl = 0.0, calls = 0.0;
		double worst = 0.0;
	};
	struct Snapshot {
		char name[32] = {};
		bool used = false;
		float seconds = 0.0f; // how long it actually recorded for
		int samples = 0;
		int rows = 0;
		SnapRow row[kSnapRows];
		// The frame budget, averaged over the same window.
		double frameMs = 0.0, cpuMs = 0.0, waitMs = 0.0, presentMs = 0.0, capMs = 0.0,
			   gpuMs = 0.0;
		bool budgetValid = false;
	};
	Snapshot m_snaps[kSnapSlots];

	// Recording state. Only one at a time: two overlapping recordings would each
	// be measuring the other's cost as well as the scene's.
	int m_snapTarget = -1;
	float m_snapLeft = 0.0f;

	// All in DevConsole_Snapshots.cpp. SnapCommand is the `profile` command's
	// snap / snaps / diff verbs, handed over whole so the command's snapshot
	// half lives beside the code it drives.
	void SnapCommand(const std::vector<std::string>& args);
	int SnapSlot(std::string_view name) const; // existing slot, or -1
	int SnapFreeSlot();                        // reuse by name, else a free one
	// Walks the tree itself rather than borrowing the sampler's rows: the row
	// type is a drawing detail private to the console's own files, and one extra
	// tree walk for the few seconds a recording lasts is not worth leaking it into
	// this header.
	void SnapAccumulate(float dt);
	void SnapFinish();
	void SnapDiff(const Snapshot& a, const Snapshot& b);

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
	void SampleHistory(float dt, const gfx::GraphicsDevice& device,
					   const gfx::SpriteBatch& sprites);
	void SampleProfileSeries();
	void CommitProfileSeries();

	// --- the readout panel's sections ----------------------------------------
	// Render lays the panel out top to bottom and hands every section the same
	// PanelCtx (DevConsole_Panel.h). Each answers for its own height, drawing and
	// clicks in its own file, so a new section is one file plus its lines here.
	// Clicks and hovers take the mouse already in Render's space.
	struct PanelCtx;
	// Shared by the sections, so they live with the frame (DevConsole.cpp).
	gfx::Rect DrawExpander(const PanelCtx& p, float y, bool expanded);
	float DrawCheckbox(const PanelCtx& p, float x, float y, bool on, int perfLine, u32 tid,
					   u32 node);

	// PERFORMANCE (DevConsole_Perf.cpp)
	void SamplePerfSeries(const gfx::GraphicsDevice& device, const gfx::SpriteBatch& sprites);
	void CommitPerfSeries();
	float PerfSectionHeight(const PanelCtx& p) const;
	void DrawPerfSection(const PanelCtx& p, float top);
	void PerfClick(float mx, float my);

	// PROFILE (DevConsole_Profile.cpp)
	void RegisterProfileCommand();
	void PrepareProfile(devcon::ProfileFrame& f) const;
	float ProfileSectionHeight(const PanelCtx& p, const devcon::ProfileFrame& f) const;
	void DrawProfileSection(const PanelCtx& p, float top, const devcon::ProfileFrame& f);
	void ProfileHover(float mx, float my);
	void ProfileClick(float mx, float my);

	// HEALTH (DevConsole_Health.cpp)
	float HealthSectionHeight(const PanelCtx& p) const;
	void DrawHealthSection(const PanelCtx& p, float top);
	void HealthClick(float mx, float my);

	// THREADS (DevConsole_Threads.cpp)
	float ThreadsSectionHeight(const PanelCtx& p,
							   const std::vector<threads::WorkerInfo>& workers) const;
	void DrawThreadsSection(const PanelCtx& p, float top,
							const std::vector<threads::WorkerInfo>& workers);
	void ThreadsClick(float mx, float my);

	bool m_open = false;
	bool m_commandsEnabled = true;   // false while a staged load is mid-flight
	bool m_mirrorToLog = false;      // `logecho`: every console line also to dungeon.log
	// Every section collapses to its header, so the panel can be cut down to just
	// the one thing being watched. All three start collapsed (Michael, 2026-10-01)
	// so opening the console shows the scrollback first; each header still answers
	// its headline question, and a click expands the one you came to read. The
	// state persists in settings.ini (console_*_expanded, see SetSections).
	void NoteSectionsChanged() {
		if (onSectionsChanged) onSectionsChanged();
	}
	bool m_perfExpanded = false;
	bool m_profileExpanded = false;
	bool m_threadsExpanded = false;

	bool m_profileGraph = false; // list of current values, or scrolling graphs
	bool m_perfGraph = false;    // the top gauges, as bars or as graphs

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

	// A scrollback line. Plain lines are the ordinary case; `help` writes HEADERS
	// (accent) and ROWS, whose three columns - name, params, summary - draw in
	// three inks so the eye can run down one of them. The splits are byte
	// offsets into `text`, which is still the whole line (the log mirror and the
	// clipboard-free reading of a screenshot both want it that way).
	enum class LineStyle : u8 { Plain, Header, Row };
	struct OutLine {
		std::string text;
		LineStyle style = LineStyle::Plain;
		u16 nameEnd = 0;   // Row: [0, nameEnd) is the name
		u16 paramsEnd = 0; // Row: [nameEnd, paramsEnd) the params, the rest the summary
	};
	std::deque<OutLine> m_output; // scrollback (oldest front)
	void PrintLine(OutLine line); // Print, for a styled line
	std::vector<std::string> m_history;
	int m_historyIndex = -1; // -1 = editing a fresh line
	int m_scroll = 0;        // lines scrolled up from the bottom
	float m_caretBlink = 0.0f;

	struct Command {
		std::string name;
		CmdGroup group = CmdGroup::Console;
		std::string params;
		std::string summary;
		std::function<void(const std::vector<std::string>&)> fn;
	};
	std::vector<Command> m_commands;
	const Command* m_running = nullptr; // set by Execute around the handler, for RefuseUsage

	// --- help + type-ahead (DevConsole_Commands.cpp) --------------------------
	void RegisterHelp();
	void Help(const std::vector<std::string>& args);
	// Writes `cmds` as column-aligned rows under one header per group.
	void PrintCommandTable(const std::vector<const Command*>& cmds);
	void PrintCommandDetail(const Command& cmd);
	const Command* FindCommand(std::string_view name) const;

	// THE TYPE-AHEAD. While the FIRST word is being typed a box above the input
	// line lists the commands matching it; once a known command and a space are
	// in, the box shrinks to that command's params (the HINT).
	//
	// A line RECALLED from history does not open the list: Up/Down move the
	// list's selection while it is showing, so a recall that opened it would
	// turn the second Up into a selection move instead of stepping further back.
	// The list opens on an edit and closes on a recall, Enter, Tab or Esc.
	bool m_suggestOpen = false;
	int m_suggestSel = 0;
	int m_suggestPrefixCount = 0;          // the first N of m_suggest are prefix matches
	std::vector<const Command*> m_suggest; // matches for the word being typed
	std::string m_suggestFor;              // the word m_suggest was built for
	// True while the first word is still being typed (no space yet).
	bool EditingName() const;
	void RefreshSuggestions();
	bool SuggestListVisible() const;
	// Handles the type-ahead's keys (Tab, Up/Down, Esc). Returns true when it
	// consumed one, so the history keys and Esc-to-close stand down.
	bool UpdateSuggest(const Input& input);
	// Enter on a half-typed name takes the highlighted command into the line
	// instead of running it. Returns true when it claimed this Enter.
	bool SuggestTakesEnter();
	// One Enter, where it fell in the typed text: the type-ahead's claim, else
	// run the line (or refuse it while a load is mid-flight) and clear it.
	void SubmitLine();
	void AcceptSuggestion();
	void DrawSuggest(gfx::SpriteBatch& batch, float width, float inputY, float line, float pad,
					 float labelX);
};

} // namespace dungeon::game
