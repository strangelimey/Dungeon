// ============================================================================
// Game/DevConsole.cpp — see DevConsole.h.
// ============================================================================
#include "Game/DevConsole.h"

#include "Core/Diagnostics.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Profile.h"
#include "Core/StackTrace.h"
#include "UI/Controls.h" // ui::DrawBorder

#include <Windows.h> // VK_* codes

#include <algorithm>
#include <cmath>
#include <cstring> // strcmp, matching zone landmarks by name
#include <format>
#include <sstream>

namespace dungeon::game {

namespace {
constexpr float kDesignWindowH = 900.0f; // font authored against this height
constexpr float kFontH = 16.0f;          // console font px at the design height
constexpr size_t kMaxOutput = 500;       // scrollback cap

// Console palette (dev-facing, not themed).
const Vec4 kBackground{0.03f, 0.03f, 0.05f, 0.92f};
const Vec4 kPerfBg{0.06f, 0.06f, 0.09f, 1.0f};
const Vec4 kBorder{0.35f, 0.38f, 0.48f, 1.0f};
const Vec4 kText{0.85f, 0.88f, 0.92f, 1.0f};
const Vec4 kDim{0.55f, 0.58f, 0.66f, 1.0f};
const Vec4 kAccent{0.55f, 0.85f, 0.55f, 1.0f};
const Vec4 kWarn{0.95f, 0.65f, 0.35f, 1.0f}; // a readout near a hard ceiling
const Vec4 kGaugeBg{0.13f, 0.13f, 0.17f, 1.0f};

// --- the profile panel's rows -----------------------------------------------
// Flattened ahead of drawing because the panel's HEIGHT has to be known before
// its background is filled, and because it bounds the work: a tree is walked
// once into a fixed array rather than twice against a live snapshot.
//
// Names are COPIED. A zone's name is a string literal and would survive, but a
// thread's lives in the ThreadReport, which is a local of the builder — pointing
// at it would dangle the moment the array outlived the snapshot. The node DATA
// is fine to reach through: it lives in registry storage until that thread's
// next publish.
// Sized for a tree with detail RAISED, not for the default one. Turning a branch
// up is the whole point of the click-to-expand control below, and it is exactly
// what multiplies the row count — at 44 (what fitted on screen before the panel
// scrolled) two raised subtrees ran the array out and the rest of the tree
// silently stopped existing. Overflow is reported now rather than trusted not to
// happen; this is a stack array in two frames, ~26 KB each.
constexpr int kMaxProfRows = 256;
// Matches the series pool: every measure that HAS history can be graphed, since
// the panel scrolls now and no longer has to fit them all on screen at once.
// What keeps the cost down is culling the ones scrolled out of view, not a cap.
constexpr int kMaxGraphs = 32;

struct ProfRow {
	char name[32] = {};
	u32 tid = 0;  // owning thread + node index: the stable key the graph view
	u32 node = 0; // uses to follow one measure across frames as the tree grows
	u32 slot = 0; // registry slot: the key that addresses ONE source (see below)
	int depth = 0;
	// The two halves of this node's detail state. `detail` is its own override
	// (-1 = inherit) and `inherited` what its ancestors grant it; together they
	// say what the row's control should show and what a click on it should do.
	// Kept apart rather than pre-combined because "raised here" and "raised by a
	// parent" are different things to look at and only the first is clearable.
	i8 detail = -1;
	i8 inherited = 0;
	double inclMs = 0.0;
	double exclMs = 0.0;
	double maxMs = 0.0;
	u64 calls = 0;
	float frac = 0.0f;  // of this thread's whole period, for the bar
	bool header = false; // a thread's title row rather than a zone
	u64 periods = 0;
	bool dropped = false; // that thread has lost scopes to a full pool
};

void CopyName(char (&dst)[32], const char* src) {
	if (!src) src = "?";
	size_t i = 0;
	for (; i + 1 < sizeof(dst) && src[i]; ++i) dst[i] = src[i];
	dst[i] = '\0';
}

// Split on the PREPROCESSOR, not `if constexpr`: this is not a template, so a
// discarded constexpr branch is still COMPILED, and every line of the body below
// gets flagged unreachable in a build without DN_PROFILE.
#if DN_PROFILE
// Fills `out` with at most `cap` rows and, through `total`, says how many the
// tree ACTUALLY has. The two differ once a raised subtree outgrows the array,
// and the caller draws the difference: a tree that quietly stopped at the cap
// reads as a tree that ends there, which is the one reading a profiler must
// never invite.
int BuildProfileRows(ProfRow* out, int cap, int* total = nullptr) {
	const prof::Clock clock = prof::ClockInfo();
	prof::ThreadReport reports[prof::kMaxThreads];
	const int threadCount = prof::SnapshotAll(reports, prof::kMaxThreads);

	int count = 0; // rows written
	int seen = 0;  // rows the tree holds, whether or not they fitted
	auto claim = [&]() -> ProfRow* {
		++seen;
		return count < cap ? &out[count++] : nullptr;
	};

	for (int i = 0; i < threadCount; ++i) {
		const prof::ThreadReport& r = reports[i];

		if (ProfRow* head = claim()) {
			head->header = true;
			head->tid = r.osThreadId;
			head->slot = r.slot;
			CopyName(head->name, r.name);
			head->periods = r.periods;
			head->dropped = r.nodeOverflows > 0 || r.depthOverflows > 0;
		}

		// The bar is a fraction of everything this thread recorded in the period,
		// summed over the roots — not of the frame's wall clock, which the worker
		// threads have no relationship to.
		u64 threadTotal = 0;
		for (u32 c = r.root; c != prof::kInvalidNode; c = r.nodes[c].nextSibling)
			threadTotal += r.nodes[c].inclusive;

		// Depth-first with an explicit stack. Children are PREPENDED as they are
		// discovered, so pushing them in list order and popping restores call
		// order — the tree reads the way the code ran.
		//
		// `inherited` rides the stack because a node's effective level is a
		// property of the PATH taken to it, not of the node: it is whatever the
		// nearest ancestor carrying an override grants, and walking back up to
		// find that ancestor per row would re-derive on every node what coming
		// down already knew.
		struct Visit {
			u32 node;
			int depth;
			i8 inherited;
		};
		Visit stack[prof::kMaxDepth * 2];
		constexpr int kStackCap = static_cast<int>(std::size(stack));
		int top = 0;
		for (u32 c = r.root; c != prof::kInvalidNode && top < kStackCap;
			 c = r.nodes[c].nextSibling)
			stack[top++] = {c, 0, r.baseThreshold};

		while (top > 0) {
			const Visit v = stack[--top];
			const prof::NodeView& node = r.nodes[v.node];

			// THE SAME GATE Collector::Enter applies, asked of the published tree:
			// would this scope be recorded right now? A node is never removed once
			// created — that is what keeps node indices stable across publishes,
			// which the graph series and the smoothing both key on — so without
			// this test a subtree stayed on screen forever after it was revealed,
			// frozen at x0 calls, and lowering the detail back down looked like it
			// had done nothing at all.
			//
			// So the list shows what is BEING MEASURED, not what once was, and
			// clearing an override collapses the branch it opened. A node admitted
			// by the gate but simply not called this period still shows, at x0 —
			// "ran nothing" and "is not being watched" are different facts and only
			// the second should remove a row.
			const i8 level = node.zone ? static_cast<i8>(node.zone->level) : i8{0};
			if (level > v.inherited) continue; // and with it, its whole subtree

			if (ProfRow* row = claim()) {
				CopyName(row->name, node.zone ? node.zone->name : "?");
				row->tid = r.osThreadId;
				row->node = v.node;
				row->slot = r.slot;
				row->depth = v.depth;
				row->detail = node.detail;
				row->inherited = v.inherited;
				row->inclMs = prof::TicksToMs(node.inclusive, clock);
				row->exclMs = prof::TicksToMs(node.Exclusive(), clock);
				row->maxMs = prof::TicksToMs(node.maxTicks, clock);
				row->calls = node.calls;
				row->frac = threadTotal > 0
								? static_cast<float>(static_cast<double>(node.inclusive) /
													 static_cast<double>(threadTotal))
								: 0.0f;
			}

			// A node's own override governs its CHILDREN — Collector::Enter applies
			// it to the frame it just pushed — so it is passed down, never applied
			// to the row it sits on.
			const i8 grants = node.detail >= 0 ? node.detail : v.inherited;
			for (u32 c = node.firstChild; c != prof::kInvalidNode && top < kStackCap;
				 c = r.nodes[c].nextSibling)
				stack[top++] = {c, v.depth + 1, grants};
		}
	}
	if (total) *total = seen;
	return count;
}
#else
int BuildProfileRows(ProfRow*, int, int* total = nullptr) {
	if (total) *total = 0;
	return 0;
}
#endif

// ----------------------------------------------------------------------------
// The detail control on a tree row, as ONE rule the marker and the click both
// ask, so what a row shows and what clicking it does cannot drift apart.
//
// What a row grants its children right now: its own override if it carries one,
// else whatever it inherited.
i8 EffectiveDetail(const ProfRow& r) { return r.detail >= 0 ? r.detail : r.inherited; }

// Where a click takes it. One step deeper each time, then back to inheriting —
// a single target that both expands and undoes, rather than a widen button
// beside a reset nobody would find. Returns -1 to CLEAR.
//
// The cycle counts from the EFFECTIVE level, not from the override, so the first
// click on a row under an already-raised parent still deepens by one instead of
// re-granting a level it was getting anyway and looking broken.
i8 NextDetail(const ProfRow& r) {
	const i8 next = static_cast<i8>(EffectiveDetail(r) + 1);
	return next > static_cast<i8>(prof::kLevelDetail) ? static_cast<i8>(-1) : next;
}

// A click here can do NOTHING: the row is already granting the deepest level
// call sites use, and it is inheriting that rather than holding an override, so
// there is not even one to clear. Reported rather than silently absorbed.
//
// Note what this deliberately does NOT cover: a row at the deepest level by its
// OWN override still has somewhere to go — back to inheriting — and that is the
// third step of the cycle. Testing the effective level alone would swallow it
// and leave a raised subtree with no way to put it back but the typed command.
bool AtMaxDetail(const ProfRow& r) {
	return r.detail < 0 && r.inherited >= static_cast<i8>(prof::kLevelDetail);
}

// ----------------------------------------------------------------------------
// WHAT IS HOLDING THE FRAME RATE DOWN. The panel measured work but never WAITING,
// and the CPU-versus-GPU question lives entirely in the waiting: a frame's wall
// clock is CPU work plus two blocks, one on the frame fence and one in Present.
// With those three separated (prof::kZoneWaitGpu / kZoneRecord / kZonePresent)
// the answer is arithmetic rather than inference.
struct FrameBudget {
	bool valid = false;    // the main thread's landmarks were found
	bool gpuKnown = false; // GPU timestamps exist (they do not on WARP)
	// Whose `frame` row this describes. Carried so the stacked bar can be drawn
	// on that row and no other: "frame" is not a reserved word, and a worker that
	// one day names a zone the same would otherwise get the main thread's budget
	// painted beside its own unrelated timings.
	u32 tid = 0;
	double frameMs = 0.0;
	double waitGpuMs = 0.0; // stopped, because the GPU is frames behind
	double presentMs = 0.0; // stopped, in Present
	double capMs = 0.0;     // stopped, because WE said so (the frame cap)
	double cpuMs = 0.0;     // the frame minus every block: work, by elimination
	double gpuBusyMs = 0.0; // the GPU source's spans, summed

	// Cap is its own verdict rather than being folded into Display. Both mean
	// "not the hardware", but they call for opposite actions: display-bound is
	// finished — the screen cannot show more — while cap-bound is a limit YOU
	// set and can raise. Reporting a self-imposed ceiling as a hardware one
	// would send someone hunting for a bottleneck that is a config line.
	enum class Bound { Unknown, Cpu, Gpu, Display, Cap };
	Bound bound = Bound::Unknown;
};

// Fractions of the frame at which a reading is called. Not tuned — chosen so the
// verdict only speaks when one thing clearly dominates, because a confident wrong
// answer here sends someone optimizing the wrong half of the engine for a day.
constexpr double kGpuSaturated = 0.85; // GPU busy this much of the frame = the ceiling
constexpr double kBarelyWaiting = 0.15; // blocked less than this = the CPU fills the frame

template <typename ShownFn>
FrameBudget MeasureFrameBudget(const ProfRow* rows, int count, ShownFn&& shownIncl) {
	FrameBudget b;
	auto is = [](const char* a, const char* lit) { return std::strcmp(a, lit) == 0; };

	const char* thread = "";
	for (int i = 0; i < count; ++i) {
		const ProfRow& r = rows[i];
		if (r.header) {
			thread = r.name;
			continue;
		}
		if (is(thread, prof::kThreadMain)) {
			// By name, which is the only identity a zone has — see the landmark
			// constants in Profile.h, which exist so this cannot drift.
			if (is(r.name, prof::kZoneFrame) && r.depth == 0) {
				b.frameMs = shownIncl(r);
				b.tid = r.tid;
				b.valid = true;
			} else if (is(r.name, prof::kZoneWaitGpu)) {
				b.waitGpuMs = shownIncl(r);
			} else if (is(r.name, prof::kZonePresent)) {
				b.presentMs = shownIncl(r);
			} else if (is(r.name, prof::kZoneWaitCap)) {
				b.capMs = shownIncl(r);
			}
		} else if (is(thread, prof::kSourceGpu) && r.depth == 0) {
			// The GPU's spans are FLAT roots by construction (GpuProfiler.h), so
			// summing the depth-0 rows is the frame's GPU busy time and double-
			// counts nothing.
			b.gpuBusyMs += shownIncl(r);
			b.gpuKnown = true;
		}
	}
	if (!b.valid) return b;

	// Work by ELIMINATION rather than by reading `record`: whatever the frame did
	// not spend blocked, it spent doing something, and that includes update and
	// the parts of render that no zone happens to cover. Reading `record` alone
	// would quietly under-count and make the CPU look cheaper than it is.
	b.cpuMs = b.frameMs - b.waitGpuMs - b.presentMs - b.capMs;
	if (b.cpuMs < 0.0) b.cpuMs = 0.0; // the waits are sampled inside the frame; clamp
	if (b.frameMs <= 0.0) return b;

	const double gpuFrac = b.gpuBusyMs / b.frameMs;
	const double waitFrac = (b.waitGpuMs + b.presentMs + b.capMs) / b.frameMs;

	// ORDER MATTERS, and GPU is tested first on purpose. A saturated GPU shows up
	// as a long block in EITHER wait — on the fence, or in Present with no back
	// buffer free — so asking "which wait was longest" cannot tell GPU-bound from
	// display-bound. Asking the GPU how busy it was can.
	if (b.gpuKnown && gpuFrac >= kGpuSaturated) b.bound = FrameBudget::Bound::Gpu;
	else if (waitFrac < kBarelyWaiting) b.bound = FrameBudget::Bound::Cpu;
	// Between the two kinds of doing-nothing, whichever consumed more of the
	// frame names the reason. A capped frame still parks briefly in Present, so
	// the presence of either wait proves nothing on its own.
	else if (b.capMs > b.presentMs) b.bound = FrameBudget::Bound::Cap;
	else b.bound = FrameBudget::Bound::Display;
	return b;
}

// Draws one scrolling line graph: newest sample at the RIGHT, oldest at the left,
// with `head` naming the next write slot (and therefore the oldest value).
//
// `scale` is the value at the top of the plot, passed in rather than derived,
// because the two callers want opposite things. A profile timing has no natural
// ceiling, so it autoscales to its own window. A percentage or a memory total
// DOES have one, and autoscaling those would redraw 3% CPU as a full graph and
// make an idle machine look pegged.
//
// SpriteBatch has no line primitive, so a segment is a thin rect rotated onto the
// vector between two points (DrawRectRotated was already there for the map's
// facing arrows).
//
// `background` and `fill` exist so several series can share one plot. An overlay
// draws the frame first, filled, as the envelope everything else sits inside,
// then the others as bare lines over it — three translucent bands stacked on one
// another turn to mud and stop reading as anything.
void DrawSeriesGraph(gfx::SpriteBatch& batch, const gfx::Rect& plot, const float* samples,
					 int count, int head, float scale, const Vec4& color,
					 bool background = true, bool fill = true) {
	if (background) {
		batch.DrawRect(plot, kGaugeBg);
		ui::DrawBorder(batch, plot, kBorder);
	}
	if (count < 2 || scale <= 0.0f) return;

	const float stepX = plot.w / static_cast<float>(count - 1);
	const float base = plot.y + plot.h;
	auto yFor = [&](float v) {
		return base - std::clamp(v / scale, 0.0f, 1.0f) * plot.h;
	};

	// FILLED UNDER THE LINE, and this is not decoration. On a fixed scale a real
	// reading can be a small fraction of its ceiling — VRAM at 1 GB of an 11 GB
	// budget is 9%, which on a plot this tall is a 1.5px line four pixels off the
	// floor and reads as an EMPTY graph. A band cannot be mistaken for nothing.
	if (fill) {
		const Vec4 fillColor{color.x, color.y, color.z, 0.22f};
		for (int k = 0; k < count; ++k) {
			const float top = yFor(samples[(head + k) % count]);
			if (base - top < 0.5f) continue;
			const float fx = plot.x + static_cast<float>(k) * stepX;
			const float fw = std::min(stepX + 1.0f, plot.x + plot.w - fx);
			if (fw > 0.0f) batch.DrawRect({fx, top, fw, base - top}, fillColor);
		}
	}

	float lx = plot.x;
	float ly = yFor(samples[head]);
	for (int k = 1; k < count; ++k) {
		const float nx = plot.x + static_cast<float>(k) * stepX;
		const float ny = yFor(samples[(head + k) % count]);
		const float dx = nx - lx, dy = ny - ly;
		const float len = std::sqrt(dx * dx + dy * dy);
		if (len >= 0.01f)
			batch.DrawRectRotated({(lx + nx) * 0.5f, (ly + ny) * 0.5f}, {len, 1.5f},
								  std::atan2(dy, dx), color);
		lx = nx;
		ly = ny;
	}
}

std::vector<std::string> Tokenize(const std::string& line) {
	std::vector<std::string> tokens;
	std::istringstream stream(line);
	std::string token;
	while (stream >> token) tokens.push_back(token);
	return tokens;
}
} // namespace

DevConsole::DevConsole(ui::FontLibrary& fonts, threads::Manager& threadManager)
	: m_fonts(fonts), m_font(&fonts.Get(ui::FontRole::Mono, kFontH)),
	  m_threadMgr(threadManager) {
	// Every OS query in the readout runs on its own worker rather than on the
	// frame — see PerfMonitor::StartOsSampler for what they measured at.
	m_perf.StartOsSampler(threadManager);

	// Generic built-ins. Gameplay-aware commands are registered by the Game.
	Register("help", "list available commands", [this](const std::vector<std::string>&) {
		for (const Command& cmd : m_commands)
			Print(std::format("  {:<10} {}", cmd.name, cmd.help));
	});
	Register("clear", "clear the console output", [this](const std::vector<std::string>&) {
		m_output.clear();
		m_scroll = 0;
	});
	Register("profile",
			 "panel|dump|detail <path> <lvl>|smooth <secs>|snap <name> [secs]|snaps|diff <a> <b>",
			 [this](const std::vector<std::string>& args) {
				 if constexpr (!prof::kEnabled) {
					 Print("profiling is not compiled in (build debug-profile or "
						   "release-profile)");
				 } else if (!args.empty() && args[0] == "dump") {
					 // Beside the exe, next to dungeon.log, for the same reason:
					 // per-run output, not content.
					 const std::string name = args.size() > 1 ? args[1] : "trace.json";
					 const std::string path = paths::ExecutableDir() + "\\" + name;
					 const prof::TraceStats st = prof::DumpTrace(path.c_str());
					 if (!st.ok) {
						 Print("trace dump FAILED (see dungeon.log)");
					 } else {
						 Print(std::format("wrote {} events from {} threads over {:.1f} ms",
										   st.events, st.threads, st.spanMs));
						 if (st.truncated)
							 Print(std::format("  {} scopes still open at the dump, closed "
											   "at the last timestamp",
											   st.truncated));
						 if (st.discarded)
							 Print(std::format("  {} discarded (the ring had wrapped past "
											   "their opening scope)",
											   st.discarded));
						 if (st.overrun)
							 Print(std::format("  WARNING: {} events overwritten mid-read; "
											   "freeze the world and dump again",
											   st.overrun));
						 Print("  open in Perfetto (ui.perfetto.dev) or speedscope");
					 }
				 } else if (!args.empty() && args[0] == "detail") {
					 if (args.size() < 2) {
						 prof::DetailEntry entries[32];
						 const int n = prof::ListDetails(entries, 32);
						 if (n == 0)
							 Print("no detail overrides; try 'profile detail <path> "
								   "<level>', e.g. 'profile detail frame/render 1' - "
								   "or click a row of the tree above");
						 for (int e = 0; e < n; ++e)
							 Print(std::format("  [{}] {} = {}", entries[e].thread,
											   entries[e].path, entries[e].level));
						 return;
					 }
					 // Level defaults to 1 (a subsystem's phases). -1 clears.
					 int level = 1;
					 if (args.size() > 2) {
						 try {
							 level = std::stoi(args[2]);
						 } catch (const std::exception&) {
							 Print("level must be a number (-1 clears)");
							 return;
						 }
					 }
					 const int matched =
						 prof::SetDetail(args[1], static_cast<i8>(std::clamp(level, -1, 127)));
					 if (matched == 0)
						 Print(std::format("'{}' matched no recorded node - the path must "
										   "name a scope that has already run",
										   args[1]));
					 else
						 Print(std::format("{} set to level {} on {} thread{}", args[1], level,
										   matched, matched == 1 ? "" : "s"));
				 } else if (!args.empty() && args[0] == "snap") {
					 if (args.size() < 2) {
						 Print("usage: profile snap <name> [seconds]");
						 return;
					 }
					 if (m_snapTarget >= 0) {
						 Print("a recording is already running");
						 return;
					 }
					 float secs = 3.0f;
					 if (args.size() > 2) {
						 try {
							 secs = std::stof(args[2]);
						 } catch (const std::exception&) {
							 Print("seconds must be a number");
							 return;
						 }
					 }
					 // Reuse the slot of the same name so re-taking a reading
					 // after a tweak does not silently fill the table with
					 // near-identical entries.
					 int slot = SnapSlot(args[1]);
					 if (slot < 0) slot = SnapFreeSlot();
					 if (slot < 0) {
						 Print(std::format("all {} snapshot slots are used - "
										   "'profile snap <existing name>' overwrites one",
										   kSnapSlots));
						 return;
					 }
					 m_snaps[slot] = Snapshot{};
					 m_snaps[slot].used = true;
					 CopyName(m_snaps[slot].name, args[1].c_str());
					 m_snapTarget = slot;
					 m_snapLeft = std::clamp(secs, 0.25f, 60.0f);
					 Print(std::format("recording '{}' for {:.1f}s...", args[1], m_snapLeft));
				 } else if (!args.empty() && args[0] == "snaps") {
					 int n = 0;
					 for (const Snapshot& s : m_snaps) {
						 if (!s.used) continue;
						 ++n;
						 Print(std::format("  {:<12} {} rows, {:.1f}s, frame {:.3f} ms", s.name,
										   s.rows, s.seconds, s.frameMs));
					 }
					 if (n == 0) Print("no snapshots; 'profile snap <name> [secs]'");
				 } else if (!args.empty() && args[0] == "diff") {
					 if (args.size() < 3) {
						 Print("usage: profile diff <before> <after>");
						 return;
					 }
					 const int a = SnapSlot(args[1]), b = SnapSlot(args[2]);
					 if (a < 0 || b < 0) {
						 Print(std::format("unknown snapshot '{}'", a < 0 ? args[1] : args[2]));
						 return;
					 }
					 SnapDiff(m_snaps[a], m_snaps[b]);
				 } else if (!args.empty() && args[0] == "smooth") {
					 // How long the list's digits hold still. `off` is 0, which
					 // commits every frame and is the unsmoothed readout exactly.
					 if (args.size() < 2) {
						 Print(m_profSmoothSec > 0.0f
								   ? std::format("readout averaged over {:.0f} ms",
												 m_profSmoothSec * 1000.0f)
								   : std::string("readout is live (unsmoothed)"));
						 return;
					 }
					 float secs = 0.0f;
					 if (args[1] != "off") {
						 try {
							 secs = std::stof(args[1]);
						 } catch (const std::exception&) {
							 Print("seconds must be a number, or 'off'");
							 return;
						 }
					 }
					 // Upper bound is a readout that looks frozen, not a limit of
					 // the machinery: past a couple of seconds a stale number is
					 // indistinguishable from a hung one.
					 m_profSmoothSec = std::clamp(secs, 0.0f, 2.0f);
					 m_profSmoothTimer = 0.0f;
					 Print(m_profSmoothSec > 0.0f
							   ? std::format("readout averaged over {:.0f} ms",
											 m_profSmoothSec * 1000.0f)
							   : std::string("readout is live (unsmoothed)"));
				 } else {
					 if (!args.empty())
						 m_profileExpanded = args[0] != "off" && args[0] != "0";
					 else
						 m_profileExpanded = !m_profileExpanded;
					 Print(std::format("profile panel {}",
									   m_profileExpanded ? "expanded" : "collapsed"));
				 }
			 });
	Register("echo", "echo the arguments", [this](const std::vector<std::string>& args) {
		std::string line;
		for (size_t i = 0; i < args.size(); ++i)
			line += (i ? " " : "") + args[i];
		Print(line);
	});

	Print("Developer console - type 'help' for commands.");
}

void DevConsole::Toggle() {
	m_open = !m_open;
	if (m_open) {
		m_scroll = 0;
		m_caretBlink = 0.0f;
		m_historyIndex = -1;
	}
}

void DevConsole::Register(std::string name, std::string help,
						  std::function<void(const std::vector<std::string>&)> fn) {
	m_commands.push_back({std::move(name), std::move(help), std::move(fn)});
}

void DevConsole::Print(std::string line) {
	m_output.push_back(std::move(line));
	while (m_output.size() > kMaxOutput) m_output.pop_front();
	m_scroll = 0; // jump to the newest line
}

void DevConsole::Execute(const std::string& line) {
	Print("> " + line);
	const std::vector<std::string> tokens = Tokenize(line);
	if (tokens.empty()) return;

	std::string name = tokens[0];
	std::ranges::transform(name, name.begin(), [](char c) {
		return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	});
	const std::vector<std::string> args(tokens.begin() + 1, tokens.end());

	for (const Command& cmd : m_commands) {
		if (cmd.name == name) {
			cmd.fn(args);
			return;
		}
	}
	Print("unknown command: " + name);
}

// Same preprocessor split as BuildProfileRows, and for the same reason: a
// discarded `if constexpr` branch in a non-template is still compiled.
#if DN_PROFILE
// The profile half. Split out so the perf half below keeps working in a build
// with no profiler: CPU, GPU, memory and descriptor slots are not the
// profiler's to report and must graph in every configuration.
void DevConsole::SampleProfileSeries() {
	static_assert(kMaxProfRows <= DevConsole::kProfSmoothSlots,
				  "every listed row needs a smoothing slot");

	ProfRow rows[kMaxProfRows];
	const int n = BuildProfileRows(rows, kMaxProfRows);

	for (int i = 0; i < m_profSeriesCount; ++i) m_profSeries[i].seen = false;
	for (int i = 0; i < m_profSmoothCount; ++i) m_profSmooth[i].seen = false;

	// The frame budget's history, sampled from the RAW rows rather than the
	// smoothed ones: a graph is a SHAPE and wants the spike the smoothing exists
	// to iron out of the digits. Maxima per window, like every other series here.
	{
		const FrameBudget fb =
			MeasureFrameBudget(rows, n, [](const ProfRow& r) { return r.inclMs; });
		if (fb.valid) {
			auto bump = [&](BudgetLine which, double v) {
				m_budgetSeries[which].pending =
					std::max(m_budgetSeries[which].pending, static_cast<float>(v));
			};
			bump(kBudFrame, fb.frameMs);
			bump(kBudCpu, fb.cpuMs);
			if (fb.gpuKnown) bump(kBudGpu, fb.gpuBusyMs);
		}
	}

	const char* thread = "";
	for (int i = 0; i < n; ++i) {
		const ProfRow& r = rows[i];
		if (r.header) {
			thread = r.name;
			continue;
		}

		// Smoothing FIRST, and deliberately not inside the series lookup below:
		// the graph pool holds 32 measures while the list draws every row there
		// is, so gating this on a free series slot would leave the 33rd row
		// flickering at frame rate while its neighbours sat still.
		{
			ProfSmooth* sm = nullptr;
			for (int j = 0; j < m_profSmoothCount; ++j) {
				ProfSmooth& c = m_profSmooth[j];
				if (c.used && c.tid == r.tid && c.node == r.node) {
					sm = &c;
					break;
				}
			}
			if (!sm) {
				for (int j = 0; j < m_profSmoothCount && !sm; ++j)
					if (!m_profSmooth[j].used) sm = &m_profSmooth[j];
				if (!sm && m_profSmoothCount < kProfSmoothSlots)
					sm = &m_profSmooth[m_profSmoothCount++];
				if (sm) {
					*sm = ProfSmooth{};
					sm->used = true;
					sm->tid = r.tid;
					sm->node = r.node;
				}
			}
			if (sm) {
				sm->sumIncl += r.inclMs;
				sm->sumExcl += r.exclMs;
				sm->sumCalls += static_cast<double>(r.calls);
				sm->sumFrac += r.frac;
				sm->winMax = std::max(sm->winMax, r.maxMs);
				++sm->count;
				sm->seen = true;
			}
		}

		// Keyed by (thread, node index), NEVER by row position: the tree grows as
		// detail is raised, and a row-indexed history would smear one measure's
		// past onto whichever measure inherited its row.
		ProfSeries* s = nullptr;
		for (int j = 0; j < m_profSeriesCount; ++j) {
			ProfSeries& c = m_profSeries[j];
			if (c.used && c.tid == r.tid && c.node == r.node) {
				s = &c;
				break;
			}
		}
		if (!s) {
			for (int j = 0; j < m_profSeriesCount && !s; ++j)
				if (!m_profSeries[j].used) s = &m_profSeries[j];
			if (!s && m_profSeriesCount < kProfSeries) s = &m_profSeries[m_profSeriesCount++];
			if (!s) continue;  // full; a 33rd measure simply is not graphed
			*s = ProfSeries{}; // a new measure starts with a blank past, not a stale one
			s->used = true;
			s->tid = r.tid;
			s->node = r.node;
		}
		CopyName(s->name, r.name);
		CopyName(s->thread, thread);
		s->depth = r.depth;
		s->pending = std::max(s->pending, static_cast<float>(r.inclMs));
		s->seen = true;
	}

}

void DevConsole::CommitProfileSeries() {
	for (int j = 0; j < m_profSeriesCount; ++j) {
		ProfSeries& s = m_profSeries[j];
		if (!s.used) continue;
		s.samples[m_profHead] = s.pending;
		s.pending = 0.0f;
		if (!s.seen) s.used = false; // its thread went away; free the slot
	}
	// On the same head as everything else, so a budget spike lines up with the
	// zone graph that explains it.
	for (PerfSeries& s : m_budgetSeries) {
		s.samples[m_profHead] = s.pending;
		s.pending = 0.0f;
	}
}

void DevConsole::CommitProfileSmooth() {
	for (int j = 0; j < m_profSmoothCount; ++j) {
		ProfSmooth& s = m_profSmooth[j];
		if (!s.used) continue;

		// A window that caught nothing keeps the last committed reading rather
		// than dropping to zero. A node the tree still lists but that did not run
		// this window has not measured 0.000 ms; it has measured nothing, and
		// showing zero would claim the first when the row means the second.
		if (s.count > 0) {
			const double inv = 1.0 / static_cast<double>(s.count);
			s.incl = s.sumIncl * inv;
			s.excl = s.sumExcl * inv;
			s.calls = s.sumCalls * inv;
			s.frac = s.sumFrac * inv;
			s.maxMs = s.winMax; // the worst call in the window, NOT the mean of them
			s.ready = true;
		}

		s.sumIncl = s.sumExcl = s.sumCalls = s.sumFrac = 0.0;
		s.winMax = 0.0;
		s.count = 0;
		if (!s.seen) s.used = false; // the node is gone; free the slot
	}
}

// ----------------------------------------------------------------------------
// Snapshots. See the header for why these record over seconds and key by path.
int DevConsole::SnapSlot(std::string_view name) const {
	for (int i = 0; i < kSnapSlots; ++i)
		if (m_snaps[i].used && name == m_snaps[i].name) return i;
	return -1;
}

int DevConsole::SnapFreeSlot() {
	for (int i = 0; i < kSnapSlots; ++i)
		if (!m_snaps[i].used) return i;
	return -1;
}

void DevConsole::SnapAccumulate(float dt) {
	if (m_snapTarget < 0) return;
	Snapshot& s = m_snaps[m_snapTarget];

	ProfRow rows[kMaxProfRows];
	const int n = BuildProfileRows(rows, kMaxProfRows);

	// Same pre-order path rebuild the list view uses: ancestors are whatever is
	// sitting at the shallower depths when this row comes up.
	const char* nameAtDepth[prof::kMaxDepth] = {};
	const char* thread = "";
	for (int i = 0; i < n; ++i) {
		const ProfRow& r = rows[i];
		if (r.header) {
			thread = r.name;
			continue;
		}
		if (r.depth >= static_cast<int>(prof::kMaxDepth)) continue;
		nameAtDepth[r.depth] = r.name;

		char path[128];
		size_t w = 0;
		for (int d = 0; d <= r.depth && w + 1 < sizeof(path); ++d) {
			if (!nameAtDepth[d]) continue;
			if (w > 0) path[w++] = '/';
			for (const char* c = nameAtDepth[d]; *c && w + 1 < sizeof(path); ++c)
				path[w++] = *c;
		}
		path[w] = '\0';

		SnapRow* dst = nullptr;
		for (int j = 0; j < s.rows; ++j)
			if (std::strcmp(s.row[j].thread, thread) == 0 &&
				std::strcmp(s.row[j].path, path) == 0) {
				dst = &s.row[j];
				break;
			}
		if (!dst) {
			if (s.rows >= kSnapRows) continue; // full; the deepest rows drop
			dst = &s.row[s.rows++];
			CopyName(dst->thread, thread);
			std::memcpy(dst->path, path, w + 1);
		}
		dst->incl += r.inclMs;
		dst->excl += r.exclMs;
		dst->calls += static_cast<double>(r.calls);
		dst->worst = std::max(dst->worst, r.maxMs); // MAX, not a mean of maxima
	}

	const FrameBudget fb =
		MeasureFrameBudget(rows, n, [](const ProfRow& r) { return r.inclMs; });
	if (fb.valid) {
		s.frameMs += fb.frameMs;
		s.cpuMs += fb.cpuMs;
		s.waitMs += fb.waitGpuMs;
		s.presentMs += fb.presentMs;
		s.capMs += fb.capMs;
		s.gpuMs += fb.gpuBusyMs;
		s.budgetValid = true;
	}

	++s.samples;
	s.seconds += dt;
	m_snapLeft -= dt;
	if (m_snapLeft <= 0.0f) SnapFinish();
}

void DevConsole::SnapFinish() {
	if (m_snapTarget < 0) return;
	Snapshot& s = m_snaps[m_snapTarget];
	m_snapTarget = -1;

	if (s.samples <= 0) {
		s.used = false;
		Print("snapshot recorded nothing (is the profiler compiled in?)");
		return;
	}
	const double inv = 1.0 / static_cast<double>(s.samples);
	for (int i = 0; i < s.rows; ++i) {
		s.row[i].incl *= inv;
		s.row[i].excl *= inv;
		s.row[i].calls *= inv;
		// worst is already a max over the window
	}
	s.frameMs *= inv;
	s.cpuMs *= inv;
	s.waitMs *= inv;
	s.presentMs *= inv;
	s.capMs *= inv;
	s.gpuMs *= inv;

	// Logged as well as printed. A snapshot's summary is the only durable record
	// of a state you have already left — by the time it is interesting, the
	// setting has been changed and the console line has scrolled — and a
	// recording that survives nowhere is a measurement you have to retake.
	auto say = [&](const std::string& line) {
		Print(line);
		log::Write(log::Level::Info, line);
	};
	say(std::format("snapshot '{}' recorded: {} rows over {:.1f}s ({} frames)", s.name, s.rows,
					s.seconds, s.samples));
	if (s.budgetValid)
		say(std::format("  frame {:.3f}  cpu {:.3f}  wait {:.3f}  present {:.3f}  cap {:.3f}  "
						"gpu {:.3f}",
						s.frameMs, s.cpuMs, s.waitMs, s.presentMs, s.capMs, s.gpuMs));

	// A MACHINE-READABLE twin, log only. The line above is laid out for a person
	// and a harness parsing it would break the moment a column is widened or a
	// term added — which is exactly what just happened to it when the frame cap
	// landed. key=value survives both.
	if (s.budgetValid)
		log::Write(log::Level::Info,
				   std::format("profilesnap {} frame={:.4f} cpu={:.4f} wait={:.4f} "
							   "present={:.4f} cap={:.4f} gpu={:.4f} rows={} samples={} "
							   "secs={:.2f}",
							   s.name, s.frameMs, s.cpuMs, s.waitMs, s.presentMs, s.capMs,
							   s.gpuMs, s.rows, s.samples, s.seconds));
}

void DevConsole::SnapDiff(const Snapshot& a, const Snapshot& b) {
	// ALSO TO THE LOG, and this is not a nicety. A diff is twenty-odd lines and
	// the console shows three of them above the prompt, so the answer scrolls
	// past the moment it is printed — the first real use of this feature
	// produced a correct comparison that could not be read. dungeon.log is
	// where a multi-line answer belongs; the console keeps the headline.
	auto say = [&](const std::string& s) {
		Print(s);
		log::Write(log::Level::Info, s);
	};

	// Budget first: it is the headline, and the only part that answers "is it
	// faster" rather than "what moved".
	say(std::format("diff '{}' -> '{}'  ({:.1f}s vs {:.1f}s)", a.name, b.name, a.seconds,
					b.seconds));
	auto delta = [&](const char* label, double x, double y) {
		const double d = y - x;
		const double pct = x > 0.0001 ? d / x * 100.0 : 0.0;
		say(std::format("  {:<9} {:>8.3f} -> {:>8.3f}   {:+8.3f} ms  {:+7.1f}%", label, x, y,
						d, pct));
	};
	if (a.budgetValid && b.budgetValid) {
		delta("frame", a.frameMs, b.frameMs);
		delta("cpu", a.cpuMs, b.cpuMs);
		delta("wait.gpu", a.waitMs, b.waitMs);
		delta("present", a.presentMs, b.presentMs);
		delta("cap", a.capMs, b.capMs);
		delta("gpu", a.gpuMs, b.gpuMs);
	}

	// Then the rows that MOVED, biggest absolute change first — a diff sorted by
	// name buries the one line worth reading among forty that did not budge.
	struct Change {
		const SnapRow* from;
		const SnapRow* to;
		double d;
	};
	std::vector<Change> changes;
	for (int j = 0; j < b.rows; ++j) {
		const SnapRow& to = b.row[j];
		const SnapRow* from = nullptr;
		for (int i = 0; i < a.rows; ++i)
			if (std::strcmp(a.row[i].thread, to.thread) == 0 &&
				std::strcmp(a.row[i].path, to.path) == 0) {
				from = &a.row[i];
				break;
			}
		changes.push_back({from, &to, to.incl - (from ? from->incl : 0.0)});
	}
	// Rows that VANISHED matter as much as rows that appeared: a scope that
	// stopped running is a change, and a diff that only walks `b` would miss it.
	for (int i = 0; i < a.rows; ++i) {
		const SnapRow& from = a.row[i];
		bool found = false;
		for (int j = 0; j < b.rows && !found; ++j)
			found = std::strcmp(b.row[j].thread, from.thread) == 0 &&
					std::strcmp(b.row[j].path, from.path) == 0;
		if (!found) changes.push_back({&from, nullptr, -from.incl});
	}
	std::ranges::sort(changes, [](const Change& x, const Change& y) {
		return std::abs(x.d) > std::abs(y.d);
	});

	int shown = 0;
	for (const Change& c : changes) {
		if (shown >= 16) break;
		if (std::abs(c.d) < 0.0005) break; // below the readout's own precision
		const char* thread = c.to ? c.to->thread : c.from->thread;
		const char* path = c.to ? c.to->path : c.from->path;
		const double x = c.from ? c.from->incl : 0.0;
		const double y = c.to ? c.to->incl : 0.0;
		say(std::format("  {:<7} {:<28} {:>7.3f} -> {:>7.3f}  {:+7.3f} ms{}", thread, path, x,
						y, c.d, !c.from ? "  (new)" : (!c.to ? "  (gone)" : "")));
		++shown;
	}
	if (shown == 0) say("  no row moved by more than 0.001 ms");
	Print("  (full diff also written to dungeon.log)");
}

const DevConsole::ProfSmooth* DevConsole::SmoothFor(u32 tid, u32 node) const {
	for (int j = 0; j < m_profSmoothCount; ++j) {
		const ProfSmooth& s = m_profSmooth[j];
		if (s.used && s.ready && s.tid == tid && s.node == node) return &s;
	}
	return nullptr;
}
#else
void DevConsole::SampleProfileSeries() {}
void DevConsole::CommitProfileSeries() {}
void DevConsole::CommitProfileSmooth() {}
const DevConsole::ProfSmooth* DevConsole::SmoothFor(u32, u32) const { return nullptr; }
int DevConsole::SnapSlot(std::string_view) const { return -1; }
int DevConsole::SnapFreeSlot() { return -1; }
void DevConsole::SnapAccumulate(float) {}
void DevConsole::SnapFinish() {}
void DevConsole::SnapDiff(const Snapshot&, const Snapshot&) {}
#endif

void DevConsole::SampleHistory(float dt, const gfx::GraphicsDevice& device) {
	// Each slot keeps the MAX since the last commit, so a spike survives the
	// downsampling rather than being averaged into the baseline around it.
	const PerfMonitor::Metrics& m = m_perf.Get();
	// Every frame, and a DXGI call rather than a cached value — worth its own
	// zone so it can be told apart from the profiler snapshot beside it.
	gfx::GraphicsDevice::GpuMemoryInfo vram{};
	{
		DN_PROFILE_ZONE_L(prof::kLevelDetail, "vram");
		vram = device.QueryGpuMemory();
	}
	auto bump = [&](PerfLine which, float v) {
		m_perfSeries[which].pending = std::max(m_perfSeries[which].pending, v);
	};
	bump(kFps, m.fps);
	bump(kCpu, m.cpuPercent);
	bump(kGpu, m.gpuPercent >= 0.0f ? m.gpuPercent : 0.0f); // < 0 means unavailable
	bump(kRam, static_cast<float>(m.sysMemUsedMB));
	bump(kVram, static_cast<float>(vram.usedBytes) / (1024.0f * 1024.0f));
	bump(kSrv, static_cast<float>(device.SrvLive()));

	{
		DN_PROFILE_ZONE_L(prof::kLevelDetail, "snapshot");
		SampleProfileSeries();
	}

	// A recording in progress takes every frame, not the smoothed value: it is
	// building its own average over its own window and wants the raw samples to
	// do it from.
	SnapAccumulate(dt);

	// Its own cadence, slower than the graph's: the graphs are SHAPES and want
	// the fine sampling, while the digits beside them want to sit still long
	// enough to be read. A window of 0 commits every frame, which is the old
	// unsmoothed behaviour and needs no branch of its own.
	m_profSmoothTimer += dt;
	if (m_profSmoothTimer >= m_profSmoothSec) {
		m_profSmoothTimer = 0.0f;
		CommitProfileSmooth();
	}

	m_profSampleTimer += dt;
	if (m_profSampleTimer < kProfSampleSec) return;
	m_profSampleTimer = 0.0f;

	for (PerfSeries& s : m_perfSeries) {
		s.samples[m_profHead] = s.pending;
		s.pending = 0.0f;
	}
	CommitProfileSeries();
	SampleHealth();

	// One cursor for both sets, advanced once: every graph on screen shares an
	// x-axis, so a spike in one lines up with a spike in another.
	m_profHead = (m_profHead + 1) % kProfHistory;
}

// Severity order, for a window that caught more than one kind. A stall beside a
// restart should read as the stall — the restart is the answer to it, not the
// news — and anything fatal outranks everything.
static int HealthSeverity(diag::Kind k) {
	switch (k) {
	case diag::Kind::Fatal: return 5;
	case diag::Kind::Fault: return 4;
	case diag::Kind::Killed: return 3;
	case diag::Kind::Stall: return 2;
	case diag::Kind::Exception: return 1;
	case diag::Kind::Restart: return 0;
	}
	return 0;
}

void DevConsole::SampleHealth() {
	diag::ThreadHealth all[diag::kMaxThreads];
	const int n = diag::SnapshotThreads(all, diag::kMaxThreads);

	for (int i = 0; i < n; ++i) {
		// Threads with a clean record never claim a row: a timeline of twelve
		// blank strips hides the one that is not blank.
		if (all[i].total == 0) continue;

		HealthRow* row = nullptr;
		for (int r = 0; r < m_healthRowCount; ++r)
			if (m_healthRows[r].used && m_healthRows[r].slot == all[i].slot) {
				row = &m_healthRows[r];
				break;
			}
		if (!row && m_healthRowCount < kHealthRows) {
			row = &m_healthRows[m_healthRowCount++];
			*row = HealthRow{};
			row->used = true;
			row->slot = all[i].slot;
			std::memcpy(row->name, all[i].name, sizeof(row->name));
			// `prev` stays ZERO, so the events that caused the row to exist are
			// the first thing it draws. Seeding it with the current counts
			// instead — to keep a backlog from drawing as one spike — silently
			// swallowed every thread whose FIRST event was also its only one: a
			// worker that stalled once got a row and no mark on it. There is no
			// backlog to worry about anyway, because sampling runs every frame
			// whether the console is open or not, so a row is claimed within
			// 50 ms of the failure that earns it.
		}
		if (!row) continue; // more failing threads than the strip has rows

		u64 added = 0;
		int worst = -1;
		for (int k = 0; k < diag::kKindCount; ++k) {
			const u64 delta = all[i].counts[k] - row->prev[k];
			row->prev[k] = all[i].counts[k];
			if (delta == 0) continue;
			added += delta;
			const auto kind = static_cast<diag::Kind>(k);
			if (worst < 0 || HealthSeverity(kind) > HealthSeverity(static_cast<diag::Kind>(worst)))
				worst = k;
		}

		HealthCell& cell = row->cells[m_profHead];
		cell.count = static_cast<u8>(added > 255 ? 255 : added);
		cell.kind = worst < 0 ? 0xFF : static_cast<u8>(worst);
		// The newest event's index, so a click can find it again in the ring.
		cell.lastIndex = added > 0 ? static_cast<u32>(all[i].total - 1) : 0;
	}
}

void DevConsole::ReportHealthCell(int row, int cell) {
	if (row < 0 || row >= m_healthRowCount) return;
	const HealthRow& r = m_healthRows[row];

	// SNAP to the nearest mark. A cell is one 240th of the strip — about a pixel
	// wide — so demanding an exact hit made the click-through unusable in the
	// one situation it exists for: the first real attempt landed in a gap
	// between two marks and reported nothing, on a row visibly covered in them.
	constexpr int kSlop = 4;
	int best = -1;
	for (int d = 0; d <= kSlop && best < 0; ++d) {
		const int a = (cell - d + kProfHistory) % kProfHistory;
		const int b = (cell + d) % kProfHistory;
		if (r.cells[a].count > 0) best = a;
		else if (r.cells[b].count > 0) best = b;
	}
	if (best < 0) {
		Print(std::format("'{}': nothing recorded around there", r.name));
		return;
	}
	const HealthCell& c = r.cells[best];

	diag::EventView ev[diag::kEventsPerThread];
	const int n = diag::ReadEvents(r.slot, ev, diag::kEventsPerThread);
	for (int i = 0; i < n; ++i) {
		if (ev[i].index != c.lastIndex) continue;
		Print(std::format("'{}' #{} {} tick {}: {}", r.name, ev[i].index,
						  diag::KindName(ev[i].kind), ev[i].iteration, ev[i].message));
		// The same plumbing rule the log uses, so a stack reads identically
		// wherever it is shown. `shown` counts frames that SURVIVED the filter —
		// counting raw frames would spend the budget on the dispatcher.
		for (int f = 0, shown = 0; f < ev[i].frameCount && shown < 8; ++f) {
			const std::string frame = stack::Describe(ev[i].frames[f]);
			if (stack::IsPlumbingFrame(frame)) continue;
			Print("    " + frame);
			++shown;
		}
		if (c.count > 1)
			Print(std::format("  ({} events in that window; this is the newest)", c.count));
		return;
	}
	// The ring is 16 deep and the timeline is 12 seconds: an old mark can easily
	// outlive the event it points at. Saying so beats printing a neighbour.
	Print(std::format("'{}': event #{} has scrolled out of the record ({} in that "
					  "window) — `health {}` for what is left",
					  r.name, c.lastIndex, c.count, r.name));
}

void DevConsole::Update(const Input& input, float dt, float windowW, float windowH,
						const gfx::GraphicsDevice& device) {
	{
		DN_PROFILE_ZONE_L(prof::kLevelDetail, "perfmon");
		m_perf.Tick(dt);
	}
	// Every frame, open or closed: switching to a graph view should show the last
	// twelve seconds, not start blank.
	{
		DN_PROFILE_ZONE_L(prof::kLevelDetail, "history");
		SampleHistory(dt, device);
	}
	if (!m_open) return;

	m_caretBlink += dt;
	(void)windowW;
	// Re-resolve rather than re-bake: same size + same face is a map lookup.
	// GameUI::UpdateFonts commits every library font, this one included.
	m_font = &m_fonts.Get(ui::FontRole::Mono, kFontH * (windowH / kDesignWindowH));

	// Thread-panel control buttons (hit-tested against the rects Render laid out
	// last frame). Left-click toggles pause, halves/doubles the rate, or kills.
	if (input.WasMousePressed(MouseButton::Left)) {
		const float mx = input.MouseX(), my = input.MouseY();
		for (const ThreadHit& t : m_threadHits) {
			const threads::WorkerInfo info = m_threadMgr.Inspect(t.id);
			if (t.pause.Contains(mx, my)) {
				if (info.paused) m_threadMgr.Resume(t.id);
				else m_threadMgr.Pause(t.id);
			} else if (t.slower.Contains(mx, my)) {
				m_threadMgr.SetRate(t.id, std::max(info.hz * 0.5f, 0.1f));
			} else if (t.faster.Contains(mx, my)) {
				m_threadMgr.SetRate(t.id, std::min(info.hz * 2.0f, 60.0f));
			} else if (t.kill.Contains(mx, my)) {
				m_threadMgr.Kill(t.id); // hard: force-terminates a wedged worker
			} else if (t.boot.Contains(mx, my)) {
				m_threadMgr.Restart(t.id);
			}
		}
		if (m_perfExpandBtn.Contains(mx, my)) m_perfExpanded = !m_perfExpanded;
		if (m_profExpandBtn.Contains(mx, my)) m_profileExpanded = !m_profileExpanded;
		if (m_profViewBtn.Contains(mx, my)) m_profileGraph = !m_profileGraph;
		if (m_perfViewBtn.Contains(mx, my)) m_perfGraph = !m_perfGraph;
		if (m_threadsBtn.Contains(mx, my)) m_threadsExpanded = !m_threadsExpanded;
		if (m_healthBtn.Contains(mx, my)) m_healthExpanded = !m_healthExpanded;
		// Clicking a mark on the timeline. The cell under the cursor is found by
		// the same oldest-at-the-left mapping Render draws with, inverted.
		for (const HealthHit& h : m_healthHits) {
			if (!h.strip.Contains(mx, my)) continue;
			const float f = (h.strip.x + h.strip.w - mx) / h.strip.w;
			const int age = std::clamp(static_cast<int>(f * kProfHistory), 0,
									   kProfHistory - 1);
			// Same origin as Render's — the newest WRITTEN cell, not the head.
			const int newest = (m_profHead + kProfHistory - 1) % kProfHistory;
			const int cell = (newest - age + kProfHistory) % kProfHistory;
			ReportHealthCell(h.row, cell);
			break;
		}
		// Clicking a row of the tree drills into it: one level deeper each time,
		// then back to inheriting. The zones it reveals are already compiled in and
		// were being gated out, so they start recording on the owning thread's next
		// tick and appear beneath the row a frame or two later.
		for (const ProfDetailHit& d : m_profDetailHits) {
			if (!d.box.Contains(mx, my)) continue;

			// A subtree with nothing deeper authored under it would take the click,
			// change a number nothing reads, and show no new rows. Say so instead:
			// the reason is the call sites, not the control.
			if (d.atMax) {
				Print(std::format("profile: {} is at the deepest level call sites use",
								  d.path));
			} else if (!prof::SetDetailNode(d.slot, d.node, d.next)) {
				// The snapshot this rect was laid out from is a frame old, so the
				// node can have gone in between — a rebooted worker Resets its tree.
				Print(std::format("profile: {} is no longer recorded", d.path));
			} else if (d.next < 0) {
				Print(std::format("profile: {} detail cleared", d.path));
			} else {
				Print(std::format("profile: {} detail {}", d.path, d.next));
			}
			break;
		}
		for (const GraphToggle& g : m_graphToggles) {
			if (!g.box.Contains(mx, my)) continue;
			if (g.perfLine >= 0) {
				m_perfHidden[g.perfLine] = !m_perfHidden[g.perfLine];
			} else {
				for (int j = 0; j < m_profSeriesCount; ++j) {
					ProfSeries& c = m_profSeries[j];
					if (c.used && c.tid == g.tid && c.node == g.node) {
						c.hidden = !c.hidden;
						break;
					}
				}
			}
			break;
		}
	}

	// Typed characters (skip the toggle key so `~`/backtick never self-types).
	for (char c : input.TypedChars())
		if (c != '`' && c != '~') m_input.push_back(c);

	if (input.WasKeyPressed(VK_BACK) && !m_input.empty()) m_input.pop_back();

	if (input.WasKeyPressed(VK_RETURN)) {
		if (!m_input.empty()) {
			m_history.push_back(m_input);
			// Gated while a staged load runs (see SetCommandsEnabled): the
			// world is partially built, so no handler may touch it. The line
			// stays in history — recall it with Up once the load finishes.
			if (m_commandsEnabled) Execute(m_input);
			else Print("commands are unavailable while loading");
			m_input.clear();
		}
		m_historyIndex = -1;
	}

	// Command history recall.
	if (input.WasKeyPressed(VK_UP) && !m_history.empty()) {
		if (m_historyIndex == -1)
			m_historyIndex = static_cast<int>(m_history.size()) - 1;
		else if (m_historyIndex > 0)
			--m_historyIndex;
		m_input = m_history[static_cast<size_t>(m_historyIndex)];
	}
	if (input.WasKeyPressed(VK_DOWN) && m_historyIndex != -1) {
		if (m_historyIndex < static_cast<int>(m_history.size()) - 1) {
			++m_historyIndex;
			m_input = m_history[static_cast<size_t>(m_historyIndex)];
		} else {
			m_historyIndex = -1;
			m_input.clear();
		}
	}

	// The wheel goes to whatever is UNDER it: the readout panel while the cursor
	// is over the panel, the scrollback otherwise. Same rule the widget tree
	// states for ConsumeWheel — whoever can act on it claims it — arrived at here
	// by hand because the console is not part of that tree.
	const float wheel = input.WheelDelta();
	if (wheel != 0.0f && input.MouseY() < m_panelH) {
		// Clamped by Render, which is the only place the content height is known.
		m_panelScroll -= wheel * m_lineH * 3.0f;
	} else {
		// Scroll the output (wheel, or PageUp/PageDown).
		int scrollLines = static_cast<int>(wheel);
		if (input.WasKeyPressed(VK_PRIOR)) scrollLines += 5;
		if (input.WasKeyPressed(VK_NEXT)) scrollLines -= 5;
		if (scrollLines != 0) {
			m_scroll = std::clamp(m_scroll + scrollLines, 0,
								  static_cast<int>(m_output.size()));
			m_caretBlink = 0.0f;
		}
	}

	if (input.WasKeyPressed(VK_ESCAPE)) m_open = false;
}

void DevConsole::Render(gfx::SpriteBatch& batch, const gfx::GraphicsDevice& device,
						float width, float height) {
	const float line = m_font->LineAdvance();
	const float pad = line * 0.5f;

	// Full-screen dim background.
	batch.DrawRect({0, 0, width, height}, kBackground);

	// --- performance panel (top) --------------------------------------------
	const PerfMonitor::Metrics& m = m_perf.Get();
	const gfx::GraphicsDevice::GpuMemoryInfo vram = device.QueryGpuMemory();
	const double gpuUsedGB = static_cast<double>(vram.usedBytes) / (1024.0 * 1024.0 * 1024.0);
	const double gpuBudgetGB = static_cast<double>(vram.budgetBytes) / (1024.0 * 1024.0 * 1024.0);

	// Threads section sits directly below the 8-line perf block; the panel grows
	// to fit one row per managed worker.
	// Graph geometry is shared by both panels, so it has to be known before the
	// perf block's height can be — which is what everything below it is measured
	// from.
	const float graphH = line * 3.4f;
	const float graphGapY = line * 0.4f;

	const std::vector<threads::WorkerInfo> workers = m_threadMgr.SnapshotAll();
	// Header, then five gauges (or five graphs in three two-column rows), then
	// the two plain text rows.
	// A collapsed section is its header row and nothing else. Every section can
	// be reduced to one line, so the panel can be cut down to just the thing
	// being watched rather than scrolled past everything else.
	int perfVisible = 0;
	for (int i = 0; i < kPerfLines; ++i)
		if (!m_perfHidden[i]) ++perfVisible;
	const int perfHiddenCount = kPerfLines - perfVisible;
	const int perfGraphRows = (perfVisible + 1) / 2;
	const float perfBody =
		!m_perfExpanded ? 0.0f
		: m_perfGraph   ? static_cast<float>(perfGraphRows) * (graphH + graphGapY) +
							  static_cast<float>(perfHiddenCount) * line
						: line * 6.0f;
	// The two plain text rows belong to the body, not the header.
	const float perfTail = m_perfExpanded ? line * 2.0f : 0.0f;
	const float rowAdvance = line * 1.2f;

	// PROFILE sits directly under the gauges; THREADS goes to the BOTTOM of the
	// panel and starts COLLAPSED. It is a control surface — halt, rate, kill,
	// boot — rather than something you watch, so it should not push the readout
	// you are actually reading down the screen to make room for buttons.
	const float profileTop = pad + line + perfBody + perfTail;
	const float threadsBlock =
		workers.empty()     ? 0.0f
		: m_threadsExpanded ? line * 1.4f + rowAdvance * static_cast<float>(workers.size())
							: line * 1.4f;

	// HEALTH sits between them, and only exists once something has gone wrong —
	// a permanent empty strip would be a section that says nothing 99% of the
	// time and trains you to skip past it on the 1%.
	const float healthRowH = line * 1.3f;
	const float healthBlock =
		m_healthRowCount == 0 ? 0.0f
		: m_healthExpanded
			? line * 1.4f + healthRowH * static_cast<float>(m_healthRowCount) + line * 0.8f
			: line * 1.4f;

	// Flattened first: the panel's background is filled before anything is drawn
	// into it, so its height has to be known up front. In a build without
	// DN_PROFILE this returns 0 rows and the section collapses to one line saying
	// which configs have it.
	ProfRow profRows[kMaxProfRows];
	int profRowTotal = 0;
	const int profRowCount =
		m_profileExpanded ? BuildProfileRows(profRows, kMaxProfRows, &profRowTotal) : 0;

	// Graph view plots the zone rows only — a thread header has no measure of its
	// own, and a graph of nothing would just be a flat line taking up a cell.
	int profGraphCount = 0;
	for (int i = 0; i < profRowCount; ++i)
		if (!profRows[i].header) ++profGraphCount;
	// Split the zone rows by whether their series is hidden. The hidden ones are
	// not dropped — they become one-line rows under the grid, still carrying the
	// checkbox that brings them back.
	int profVisible = 0, profHiddenCount = 0;
	for (int i = 0; i < profRowCount; ++i) {
		if (profRows[i].header) continue;
		bool hid = false;
		for (int j = 0; j < m_profSeriesCount; ++j) {
			const ProfSeries& c = m_profSeries[j];
			if (c.used && c.tid == profRows[i].tid && c.node == profRows[i].node) {
				hid = c.hidden;
				break;
			}
		}
		if (hid) ++profHiddenCount; else ++profVisible;
	}
	const int graphsShown = std::min(profVisible, kMaxGraphs);
	const int graphRows = (graphsShown + 1) / 2; // two columns
	// The tree's rows, plus the column header above them, plus the "do not fit"
	// note when it shows — so neither of those is the thing the panel clips.
	const int listRows =
		profRowCount + 1 + (profRowCount < profRowTotal ? 1 : 0);

	// Header line + the TSC/toggle line, then the body. Nothing is dropped to
	// make it fit any more: the panel SCROLLS, so the content is laid out at its
	// natural height and the window decides how much of it you see.
	const float profileHeaderH = line * 2.4f;
	// The graph view leads with TWO full-width plots — the frame budget in ms and
	// utilisation in per cent — so it is two graphs' worth of height more than
	// the per-node grid accounts for. Counted unconditionally rather than on
	// fb.valid, which is not known this early: over-reserving costs a little
	// scroll, under-reserving clips.
	const float profileBody =
		!m_profileExpanded ? 0.0f
		: !prof::kEnabled ? rowAdvance
		: m_profileGraph
			? static_cast<float>(graphRows + 2) * (graphH + graphGapY) +
				  static_cast<float>(profHiddenCount) * line
			: rowAdvance * static_cast<float>(listRows);
	const float profileBlock = profileHeaderH + profileBody;

	// CONTENT height (everything, laid out) against the VISIBLE height (what the
	// window can spare above the scrollback and the prompt). The panel is the
	// smaller; the difference is what there is to scroll through.
	const float contentH = profileTop + profileBlock + healthBlock + threadsBlock + pad;
	const float panelH = std::min(contentH, height - line * 6.0f);
	m_panelScroll = std::clamp(m_panelScroll, 0.0f, std::max(0.0f, contentH - panelH));
	m_panelH = panelH;
	m_lineH = line;
	const float sy = -m_panelScroll; // added to every content-space y below
	batch.DrawRect({0, 0, width, panelH}, kPerfBg);
	ui::DrawBorder(batch, {0, 0, width, panelH}, kBorder);

	// Everything from here to the matching reset is CONTENT, drawn at
	// content-space y plus `sy` and clipped to the panel. Without the scissor a
	// scrolled-up row would draw over the border and out across the scrollback.
	const gfx::Rect panelClip{0, 0, width, panelH};
	batch.SetScissor(&panelClip);

	m_graphToggles.clear();
	m_profDetailHits.clear();
	// Text checkboxes, because this is a monospaced dev console and "[x]" reads
	// better here than a drawn box would. Registers the hit rect only if it is
	// actually ON the panel: input is clipped the same way drawing is, so a
	// checkbox scrolled out of view cannot be clicked through the scrollback.
	// The collapse control every section header carries. Its face names what a
	// click DOES, not the current state, so "hide" is the button that hides.
	auto expander = [&](float ey, bool expanded) {
		const char* face = expanded ? " hide " : " show ";
		const float bw2 = m_font->MeasureWidth(face);
		const gfx::Rect r{width - pad * 2.0f - bw2, ey, bw2, line};
		batch.DrawRect(r, kGaugeBg);
		ui::DrawBorder(batch, r, kBorder);
		m_font->Draw(batch, face, r.x, ey, kAccent);
		return r;
	};

	auto checkbox = [&](float cx, float cy, bool on, int perfLine, u32 tid, u32 node) {
		const char* face = on ? "[x] " : "[ ] ";
		m_font->Draw(batch, face, cx, cy, on ? kAccent : kDim);
		const gfx::Rect box{cx, cy, m_font->MeasureWidth(face), line};
		if (cy + line > 0.0f && cy < panelH)
			m_graphToggles.push_back({box, perfLine, tid, node});
		return box.w;
	};

	const float labelX = pad * 2.0f;
	const float gaugeX = width * 0.40f;
	const float gaugeW = width * 0.28f;
	float y = pad + sy;

	auto gauge = [&](float gy, float frac, const Vec4& fill) {
		const float gh = line * 0.7f;
		const float oy = gy + (line - gh) * 0.5f;
		batch.DrawRect({gaugeX, oy, gaugeW, gh}, kGaugeBg);
		batch.DrawRect({gaugeX, oy, gaugeW * std::clamp(frac, 0.0f, 1.0f), gh}, fill);
		ui::DrawBorder(batch, {gaugeX, oy, gaugeW, gh}, kBorder);
	};
	auto row = [&](const std::string& text) {
		m_font->Draw(batch, text, labelX, y, kText);
		y += line;
	};

	m_font->Draw(batch, "PERFORMANCE", labelX, y, kAccent);
	m_perfExpandBtn = expander(y, m_perfExpanded);
	m_perfViewBtn = {};
	if (m_perfExpanded) {
		// The view toggle only exists while the section does — a control for a
		// body that is not on screen is a control that cannot mean anything.
		const char* face = m_perfGraph ? " bars " : " graph ";
		const float bw2 = m_font->MeasureWidth(face);
		m_perfViewBtn = {m_perfExpandBtn.x - bw2 - pad, y, bw2, line};
		batch.DrawRect(m_perfViewBtn, kGaugeBg);
		ui::DrawBorder(batch, m_perfViewBtn, kBorder);
		m_font->Draw(batch, face, m_perfViewBtn.x, y, kAccent);
	} else {
		// Collapsed, the header still answers the headline question.
		m_font->Draw(batch,
					std::format("FPS {:.0f}   CPU {:.0f}%   GPU {:.0f}%", m.fps,
								m.cpuPercent, m.gpuPercent >= 0.0f ? m.gpuPercent : 0.0f),
					width * 0.25f, y, kDim);
	}
	y += line;

	// The five gauges as ONE table, so the bar view and the graph view cannot
	// disagree about what a measure is or what it is measured against. Each
	// carries its own SCALE — a real ceiling in every case, which is why these
	// graph against a fixed axis while a profile timing autoscales.
	const double sysTotalMB = m.sysMemTotalMB > 0 ? m.sysMemTotalMB : 1.0;
	const double vramUsedMB = gpuUsedGB * 1024.0;
	const double vramBudgetMB = gpuBudgetGB > 0 ? gpuBudgetGB * 1024.0 : 1.0;
	const float srvLive = static_cast<float>(device.SrvLive());
	const float srvCap = static_cast<float>(gfx::GraphicsDevice::SrvCapacity());

	struct PerfItem {
		const char* name;
		std::string text;
		float value;
		float scale;
		Vec4 color;
		bool warn;
	};
	const int refreshHz = device.RefreshHz();
	const float fpsCeiling = refreshHz > 0 ? static_cast<float>(refreshHz) : 240.0f;
	const PerfItem items[kPerfLines] = {
		// Against the DISPLAY's refresh rate, not an arbitrary round number: a
		// full bar then means "as fast as this screen can show", which is the
		// only sense in which a frame rate is good enough.
		{"FPS", std::format("FPS  {:.0f} / {} Hz", m.fps, refreshHz), m.fps, fpsCeiling,
		 {0.55f, 0.85f, 0.55f, 1.0f}, false},
		// The two processors take the shared colours (DevConsole.h): the frame
		// budget below paints CPU and GPU time in these, and a reader comparing
		// the two sections is entitled to assume they agree.
		{"CPU", std::format("CPU  {:.0f}%", m.cpuPercent), m.cpuPercent, 100.0f, kCpuColor,
		 false},
		{"GPU",
		 m.gpuPercent >= 0.0f ? std::format("GPU  {:.0f}%", m.gpuPercent)
							  : std::string("GPU  n/a"),
		 m.gpuPercent >= 0.0f ? m.gpuPercent : 0.0f, 100.0f, kGpuColor, false},
		{"RAM",
		 std::format("RAM  {:.1f} / {:.1f} GB", m.sysMemUsedMB / 1024.0,
					 m.sysMemTotalMB / 1024.0),
		 static_cast<float>(m.sysMemUsedMB), static_cast<float>(sysTotalMB),
		 {0.85f, 0.70f, 0.40f, 1.0f}, false},
		{"VRAM", std::format("VRAM {:.2f} / {:.2f} GB", gpuUsedGB, gpuBudgetGB),
		 static_cast<float>(vramUsedMB), static_cast<float>(vramBudgetMB),
		 {0.80f, 0.55f, 0.85f, 1.0f}, false},
		// Descriptor slots: a FIXED ceiling, unlike the two above, so the peak
		// rides along — a number that climbs and never comes back down is the
		// shape of a leak.
		{"SRV",
		 std::format("SRV  {} / {} (peak {})", device.SrvLive(),
					 gfx::GraphicsDevice::SrvCapacity(), device.SrvHighWater()),
		 srvLive, srvCap, {0.60f, 0.75f, 0.90f, 1.0f}, srvLive / srvCap > 0.9f},
	};

	// ONE display order for both views, so nothing moves when you toggle between
	// them. It is not the order the enum declares: filling the two-column grid in
	// declaration order put CPU beside FPS and RAM beside GPU, pairs that mean
	// nothing next to each other. Read down the columns it is the two PROCESSORS
	// side by side and the two MEMORIES side by side, with the frame rate and the
	// descriptor ceiling — the only two with no natural partner — heading them.
	constexpr PerfLine kPerfOrder[kPerfLines] = {kFps, kSrv, kGpu, kCpu, kVram, kRam};

	if (!m_perfExpanded) {
		// nothing: the header above is the whole section
	} else if (!m_perfGraph) {
		for (int oi = 0; oi < kPerfLines; ++oi) {
			const int i = kPerfOrder[oi];
			const PerfItem& it = items[i];
			m_font->Draw(batch, it.text, labelX, y,
						it.warn ? kWarn : (i == kGpu && m.gpuPercent < 0.0f) ? kDim : kText);
			if (!(i == kGpu && m.gpuPercent < 0.0f))
				gauge(y, it.value / it.scale, it.color);
			y += line;
		}
	} else {
		const float pgw = (width - pad * 6.0f) * 0.5f;
		int shown = 0;
		for (int oi = 0; oi < kPerfLines; ++oi) {
			const int i = kPerfOrder[oi];
			if (m_perfHidden[i]) continue;
			const PerfItem& it = items[i];
			const int col = shown % 2, gr = shown / 2;
			++shown;
			const float gx = pad * 2.0f + static_cast<float>(col) * (pgw + pad * 2.0f);
			const float gy = y + static_cast<float>(gr) * (graphH + graphGapY);
			if (gy > panelH || gy + graphH < 0.0f) continue; // scrolled out of view
			const float cw = checkbox(gx, gy, true, i, 0, 0);
			m_font->Draw(batch, it.text, gx + cw, gy, it.warn ? kWarn : kText);
			DrawSeriesGraph(batch, {gx, gy + line, pgw, graphH - line},
							m_perfSeries[i].samples, kProfHistory, m_profHead, it.scale,
							it.color);
		}
		y += static_cast<float>(perfGraphRows) * (graphH + graphGapY);

		// The hidden ones, one line each, still checkable — in the same display
		// order, so a graph reappears where you would look for it.
		for (int oi = 0; oi < kPerfLines; ++oi) {
			const int i = kPerfOrder[oi];
			if (!m_perfHidden[i]) continue;
			const float cw = checkbox(pad * 2.0f, y, false, i, 0, 0);
			m_font->Draw(batch, items[i].text, pad * 2.0f + cw, y, kDim);
			y += line;
		}
	}

	if (m_perfExpanded) {
		row(std::format("Process working set: {:.0f} MB", m.procMemMB));
		row("GPU: " + device.AdapterName());
	}

	// --- profile panel (below the gauges) -----------------------------------
	// One tree per measured thread, deepest-first indentation, with a bar giving
	// each node's share of what its thread recorded this period. The numbers are
	// the LAST PUBLISHED period — a frame for the main thread, a tick for a
	// worker — so this is a live readout rather than a running total.
	{
		float py = profileTop + sy;
		batch.DrawRect({0, py, width, 1.0f}, kBorder);
		py += line * 0.4f;
		m_font->Draw(batch, "PROFILE", labelX, py, kAccent);
		m_profExpandBtn = expander(py, m_profileExpanded);
		m_profViewBtn = {};

		if (!m_profileExpanded) {
			// Collapsed: say what is being measured, so the reason to expand is
			// visible without expanding.
			m_font->Draw(batch,
						std::format("{} zones over {} threads", profGraphCount,
									profRowCount - profGraphCount),
						width * 0.25f, py, kDim);
		} else if constexpr (prof::kEnabled) {
			const prof::Clock clock = prof::ClockInfo();
			m_font->Draw(batch, std::format("TSC {:.0f} MHz", clock.mhz), width * 0.15f, py,
						kDim);
			// A mean has to SAY it is one. Left unlabelled these read as this
			// frame's cost, and someone would chase a 4 ms average as though it
			// were the frame in front of them.
			if (!m_profileGraph)
				m_font->Draw(batch,
							m_profSmoothSec > 0.0f
								? std::format("avg {:.0f} ms", m_profSmoothSec * 1000.0f)
								: std::string("live"),
							width * 0.25f, py, kDim);

			// THE VERDICT, on the section header rather than buried in the tree:
			// it is the one line worth reading before any of the numbers, because
			// it says which half of the engine the numbers are even about.
			//
			// Computed from the SMOOTHED readings, not the raw ones. A verdict
			// recomputed per frame would flip between two answers several times a
			// second near a boundary and be worth nothing; the window that made the
			// digits readable makes this stable for the same reason.
			// Hoisted: the verdict text below and the stacked bar on the `frame`
			// row further down are two views of ONE measurement, and computing it
			// twice would let them disagree on screen.
			const FrameBudget fb = MeasureFrameBudget(
				profRows, profRowCount, [&](const ProfRow& r) {
					const ProfSmooth* sm = SmoothFor(r.tid, r.node);
					return sm ? sm->incl : r.inclMs;
				});
			{
				if (fb.valid && fb.bound != FrameBudget::Bound::Unknown) {
					// Coloured by what it asks of you: display-bound is the
					// healthy answer and stays quiet, the other two are things to
					// go and look at. GPU takes the same purple as the VRAM gauge.
					const char* face = "";
					Vec4 col = kDim;
					switch (fb.bound) {
					case FrameBudget::Bound::Cpu:
						face = "bound by CPU";
						col = {0.85f, 0.70f, 0.40f, 1.0f};
						break;
					case FrameBudget::Bound::Gpu:
						face = "bound by GPU";
						col = {0.80f, 0.55f, 0.85f, 1.0f};
						break;
					case FrameBudget::Bound::Display:
						face = "bound by display";
						col = kDim;
						break;
					case FrameBudget::Bound::Cap:
						face = "bound by cap";
						col = kBudgetCapColor;
						break;
					default: break;
					}
					const float vx = width * 0.40f;
					m_font->Draw(batch, face, vx, py, col);

					// THE EVIDENCE, always beside the verdict. A one-word answer
					// with no working shown is a thing to be believed rather than
					// checked, and this one is a heuristic over three numbers that
					// are all right there.
					//
					// EACH TERM IN ITS SEGMENT'S COLOUR, which makes this line the
					// legend for the stacked bar below and for the budget graph,
					// at no cost in space. A separate legend would be a fourth
					// place for the same four colours to disagree.
					float ex = vx + m_font->MeasureWidth("bound by display  ");
					auto term = [&](const std::string& s, const Vec4& c) {
						m_font->Draw(batch, s, ex, py, c);
						ex += m_font->MeasureWidth(s);
					};
					term(std::format("cpu {:.2f}", fb.cpuMs), kBudgetCpuColor);
					term(" · ", kDim);
					term(std::format("wait {:.2f}", fb.waitGpuMs), kBudgetWaitColor);
					term(" · ", kDim);
					term(std::format("present {:.2f}", fb.presentMs), kBudgetPresentColor);
					term(" · ", kDim);
					// Only when it is doing something. An always-present `cap 0.00`
					// would be a permanent column reporting the absence of a
					// feature most of the time.
					if (fb.capMs > 0.005) {
						term(std::format("cap {:.2f}", fb.capMs), kBudgetCapColor);
						term(" · ", kDim);
					}
					term(fb.gpuKnown ? std::format("gpu {:.2f}", fb.gpuBusyMs)
									 : std::string("gpu n/a"),
						 fb.gpuKnown ? kBudgetGpuColor : kDim);
				}
			}
			if (!clock.invariantTsc)
				m_font->Draw(batch, "NOT INVARIANT - timings may drift", width * 0.28f, py,
							kWarn);

			// The list/graph toggle. Same idiom as the thread controls: Render
			// lays the rect out, next frame's Update hit-tests it, so the geometry
			// lives in exactly one place. The label names the DESTINATION, not the
			// current state — a button saying "graph" takes you to the graph.
			{
				const char* face = m_profileGraph ? " list " : " graph ";
				const float bw2 = m_font->MeasureWidth(face);
				m_profViewBtn = {m_profExpandBtn.x - bw2 - pad, py, bw2, line};
				batch.DrawRect(m_profViewBtn, kGaugeBg);
				ui::DrawBorder(batch, m_profViewBtn, kBorder);
				m_font->Draw(batch, face, m_profViewBtn.x, py, kAccent);
			}
			py += line;

			const float indent = m_font->MeasureWidth("  ");
			// Every detail marker is three characters, so one measurement places
			// the name column for all of them and it cannot shift as levels change.
			const float markerW = m_font->MeasureWidth("[+] ");
			const float barX = width * 0.62f;
			const float barW = width * 0.26f;
			if (m_profileGraph) {
				// THE BUDGET OVERLAY, first and full width, because it is the one
				// graph that answers a question rather than reporting a number:
				// three lines on ONE shared scale, so which of them is tracking
				// the frame IS which one is the ceiling. Per-node graphs below
				// each autoscale to themselves, which makes them unusable for
				// exactly this comparison — everything looks equally full.
				//
				// Why it earns full width: an intermittent bound is a shape a few
				// samples wide, and at half width in a two-column grid it would be
				// a handful of pixels.
				if (fb.valid) {
					const float bgw = width - pad * 4.0f;
					const gfx::Rect plot{pad * 2.0f, py + line, bgw, graphH - line};

					// ONE scale for all three, taken from the frame's own peak: the
					// whole point is that they are commensurable. Autoscaling each
					// would redraw a 0.2 ms CPU line as tall as a 4 ms frame and
					// invert the reading.
					float peak = 0.0f;
					for (int k = 0; k < kProfHistory; ++k)
						peak = std::max(peak, m_budgetSeries[kBudFrame].samples[k]);
					if (peak <= 0.0f) peak = 1.0f;

					m_font->Draw(batch, "FRAME BUDGET", pad * 2.0f, py, kAccent);
					m_font->Draw(batch, std::format("peak {:.2f} ms", peak),
								pad * 2.0f + m_font->MeasureWidth("FRAME BUDGET  "), py, kDim);

					// Frame filled as the envelope, the other two as bare lines
					// over it — see DrawSeriesGraph's note on stacked bands.
					DrawSeriesGraph(batch, plot, m_budgetSeries[kBudFrame].samples,
									kProfHistory, m_profHead, peak, kDim, true, true);
					DrawSeriesGraph(batch, plot, m_budgetSeries[kBudCpu].samples,
									kProfHistory, m_profHead, peak, kBudgetCpuColor, false,
									false);
					if (fb.gpuKnown)
						DrawSeriesGraph(batch, plot, m_budgetSeries[kBudGpu].samples,
										kProfHistory, m_profHead, peak, kBudgetGpuColor,
										false, false);

					// Named in their own colours, on the plot, because a line has
					// no other way to say which it is.
					float lx = plot.x + pad;
					const float ly = plot.y + pad * 0.5f;
					auto key = [&](const char* s, const Vec4& c) {
						m_font->Draw(batch, s, lx, ly, c);
						lx += m_font->MeasureWidth(s);
					};
					key("frame ", kText);
					key("cpu ", kBudgetCpuColor);
					if (fb.gpuKnown) key("gpu", kBudgetGpuColor);

					py += graphH + graphGapY;

					// UTILISATION, on a FIXED 0..100% scale — the same three
					// numbers asked the other question. The budget graph above is
					// in milliseconds and autoscales, so "the frame got busier"
					// and "the frame got longer" look identical on it; here the
					// ceiling is fixed at fully-occupied, so height means load and
					// the EMPTY SPACE ABOVE THE LINES IS THE HEADROOM. That gap is
					// the whole reading: 7% and 27% is a picture of an engine
					// doing almost nothing, which no ms graph can show you because
					// it has no idea what "full" would be.
					//
					// Two lines, not a stack. The CPU's idle and the GPU's idle
					// are different quantities — they run concurrently, so their
					// busy fractions do not add up to anything meaningful.
					{
						float cpuPct[kProfHistory], gpuPct[kProfHistory];
						for (int k = 0; k < kProfHistory; ++k) {
							const float f = m_budgetSeries[kBudFrame].samples[k];
							cpuPct[k] = f > 0.0f
											? m_budgetSeries[kBudCpu].samples[k] / f * 100.0f
											: 0.0f;
							gpuPct[k] = f > 0.0f
											? m_budgetSeries[kBudGpu].samples[k] / f * 100.0f
											: 0.0f;
						}
						const gfx::Rect uplot{pad * 2.0f, py + line, bgw, graphH - line};

						const double cpuNow = fb.frameMs > 0.0 ? fb.cpuMs / fb.frameMs * 100.0
															   : 0.0;
						const double gpuNow = fb.frameMs > 0.0
												  ? fb.gpuBusyMs / fb.frameMs * 100.0
												  : 0.0;
						m_font->Draw(batch, "UTILISATION", pad * 2.0f, py, kAccent);
						float ux = pad * 2.0f + m_font->MeasureWidth("UTILISATION  ");
						auto uterm = [&](const std::string& s, const Vec4& c) {
							m_font->Draw(batch, s, ux, py, c);
							ux += m_font->MeasureWidth(s);
						};
						uterm(std::format("cpu {:.0f}%", cpuNow), kBudgetCpuColor);
						uterm("  ", kDim);
						if (fb.gpuKnown) {
							uterm(std::format("gpu {:.0f}%", gpuNow), kBudgetGpuColor);
							uterm("  ", kDim);
						}
						// Named for what it is: the MAIN THREAD's idle. The GPU
						// has its own, and calling either "the" idle would be a
						// claim about the wrong processor.
						uterm(std::format("main thread idle {:.0f}%", 100.0 - cpuNow), kDim);

						// Filled, both of them: at 7% a bare line is four pixels
						// off the floor and reads as an empty graph — the same
						// reason DrawSeriesGraph fills at all.
						DrawSeriesGraph(batch, uplot, cpuPct, kProfHistory, m_profHead, 100.0f,
										kBudgetCpuColor, true, true);
						if (fb.gpuKnown)
							DrawSeriesGraph(batch, uplot, gpuPct, kProfHistory, m_profHead,
											100.0f, kBudgetGpuColor, false, true);
						py += graphH + graphGapY;
					}
				}

				// A line per measure, newest at the RIGHT and scrolling left as
				// samples commit. Each graph autoscales to its own window, because
				// a shared scale would flatten every worker into the floor next to
				// a 4 ms frame; the peak is printed so the scale is never a
				// mystery. Two columns, oldest-first left to right.
				const float gw = (width - pad * 6.0f) * 0.5f;
				int drawn = 0;
				const char* curThread = "";
				for (int i = 0; i < profRowCount && drawn < graphsShown; ++i) {
					const ProfRow& pr = profRows[i];
					if (pr.header) {
						curThread = pr.name;
						continue;
					}

					const ProfSeries* ser = nullptr;
					for (int j = 0; j < m_profSeriesCount; ++j)
						if (m_profSeries[j].used && m_profSeries[j].tid == pr.tid &&
							m_profSeries[j].node == pr.node) {
							ser = &m_profSeries[j];
							break;
						}
					if (ser && ser->hidden) continue; // drawn as a row below instead

					const int col = drawn % 2, gr = drawn / 2;
					const float gx = pad * 2.0f + static_cast<float>(col) * (gw + pad * 2.0f);
					const float gy = py + static_cast<float>(gr) * (graphH + graphGapY);
					++drawn;

					// Scrolled out of view: skip it. A graph is ~480 quads, so
					// culling is what lets the cap be generous — the scissor would
					// hide these anyway, after paying to build every one of them.
					// `drawn` still advances, or the grid would reflow as it scrolls.
					if (gy > panelH || gy + graphH < 0.0f) continue;

					const float cw = checkbox(gx, gy, true, -1, pr.tid, pr.node);
					m_font->Draw(batch, std::format("{}/{}", curThread, pr.name), gx + cw, gy,
								kText);

					// A timing has no natural ceiling, so unlike the five gauges
					// above these autoscale to their own window. The peak is
					// printed beside the current value so the axis is never a
					// mystery.
					float peak = 0.0f;
					if (ser)
						for (float v : ser->samples) peak = std::max(peak, v);
					m_font->Draw(batch, std::format("{:.3f} peak {:.3f} ms", pr.inclMs, peak),
								gx + gw * 0.42f, gy, kDim);

					const gfx::Rect plot{gx, gy + line, gw, graphH - line};
					if (ser)
						DrawSeriesGraph(batch, plot, ser->samples, kProfHistory, m_profHead,
										peak > 0.0001f ? peak : 0.0001f,
										{0.45f, 0.70f, 0.95f, 1.0f});
					else
						DrawSeriesGraph(batch, plot, nullptr, 0, 0, 0.0f, kDim);
				}
				py += static_cast<float>(graphRows) * (graphH + graphGapY);

				// The hidden ones, one line each, still carrying the checkbox
				// that brings them back — in tree order, where you left them.
				curThread = "";
				for (int i = 0; i < profRowCount; ++i) {
					const ProfRow& pr = profRows[i];
					if (pr.header) {
						curThread = pr.name;
						continue;
					}
					bool hid = false;
					for (int j = 0; j < m_profSeriesCount; ++j) {
						const ProfSeries& c = m_profSeries[j];
						if (c.used && c.tid == pr.tid && c.node == pr.node) {
							hid = c.hidden;
							break;
						}
					}
					if (!hid) continue;
					const float cw = checkbox(pad * 2.0f, py, false, -1, pr.tid, pr.node);
					m_font->Draw(batch, std::format("{}/{}", curThread, pr.name),
								pad * 2.0f + cw, py, kDim);
					m_font->Draw(batch, std::format("{:.3f} ms", pr.inclMs), width * 0.30f, py,
								kDim);
					py += line;
				}

				// NEVER drop silently. A panel showing eight of twelve measures
				// looks exactly like a panel showing all of them, and the reader
				// would go on believing the worker threads were being watched.
				if (graphsShown < profVisible)
					m_font->Draw(batch,
								std::format("({} more past the {} graph cap)",
											profVisible - graphsShown, kMaxGraphs),
								labelX, py, kDim);
			} else {
			// The column header. The numbers were self-labelling before — each
			// carried a "max " prefix or was left to be inferred — and inferring
			// is exactly what nobody could do: inclusive and exclusive are two
			// three-decimal numbers side by side with nothing to tell them apart.
			// One line of vertical space buys that permanently, and it lets the
			// values below drop their inline prefixes and read as a table.
			//
			// WORST, not "max": max reads as a ceiling — a limit something is
			// bounded by — when the column is the worst single call actually seen
			// in the window. Naming it after what it is stops it being read as a
			// budget.
			m_font->Draw(batch, "scope", labelX, py, kDim);
			m_font->Draw(batch, "incl ms", width * 0.30f, py, kDim);
			m_font->Draw(batch, "excl ms", width * 0.38f, py, kDim);
			m_font->Draw(batch, "calls", width * 0.46f, py, kDim);
			m_font->Draw(batch, "worst ms", width * 0.52f, py, kDim);
			m_font->Draw(batch, "share", barX, py, kDim);
			py += rowAdvance;

			// THE TRACK EVERY BAR SITS IN. A bar filled to 96% has no visible
			// right-hand edge — the fill runs into the border and the eye reads
			// the coloured part as the whole, which is precisely the misreading a
			// share bar exists to prevent. Quarter gridlines give the width a
			// scale, so a segment can be judged against the total instead of
			// against nothing.
			//
			// DRAWN TWICE, once behind the fill and once faintly over it, because
			// behind alone vanishes under exactly the bars that need it most. The
			// over-draw is weak enough to read as a tick on a filled span and the
			// under-draw is what shows on an empty one.
			//
			// 1px, which the UI rules allow only for hairlines — a fractional
			// gridline blurs across two columns and stops being a line.
			const Vec4 kGridUnder{1.0f, 1.0f, 1.0f, 0.10f};
			const Vec4 kGridOver{1.0f, 1.0f, 1.0f, 0.16f};
			auto barQuarters = [&](const gfx::Rect& r, const Vec4& c) {
				for (int q = 1; q < 4; ++q)
					batch.DrawRect({r.x + r.w * (static_cast<float>(q) * 0.25f), r.y, 1.0f,
									r.h},
								   c);
			};
			auto barTrack = [&](const gfx::Rect& r) {
				batch.DrawRect(r, kGaugeBg);
				barQuarters(r, kGridUnder);
			};
			auto barEdge = [&](const gfx::Rect& r) {
				barQuarters(r, kGridOver);
				ui::DrawBorder(batch, r, kBorder);
			};
			// Vertical extent of the bar column, gathered as the rows draw so the
			// gridlines can be run down the whole of it afterwards. Drawn after
			// rather than before because they have to cross the FILLS as well as
			// the gaps — behind, they would disappear under exactly the long bars
			// whose length is hardest to judge.
			float gridTop = -1.0f, gridBot = -1.0f;

			// GROUP FRAMES, drawn in their own pass BEFORE the rows so every one
			// of them sits behind the text rather than over whichever rows happen
			// to come after it.
			//
			// A frame encloses a parent and its whole subtree and runs the full
			// width of the readout — names, numbers and bar alike — because the
			// grouping is a fact about the ROW, not about the name column. Reading
			// across, it says which parent's total the number in front of you is
			// part of, which the indentation alone only says back at the far left.
			//
			// The LEFT edge steps in with depth, tracking the name it belongs to,
			// so nested groups are told apart by where they start; the right edge
			// is common, so they stack into one clean margin instead of a ragged
			// staircase. Very low alpha: this is grouping, and it must not compete
			// with the bars it encloses.
			{
				const float rowsTop = py;
				const float groupW = std::min(barW, width - pad * 2.0f - barX);
				// Alpha found by looking, not by taste: 0.07 was invisible at 1:1
				// and only showed up magnified, which is a decoration rather than
				// a cue. This is the lightest value that survives a glance without
				// competing with the gridlines (0.16) inside it.
				const Vec4 kGroupFrame{1.0f, 1.0f, 1.0f, 0.11f};
				for (int i = 0; i < profRowCount; ++i) {
					const ProfRow& pr = profRows[i];
					if (pr.header) continue;

					// The subtree is the run of rows deeper than this one — the
					// walk is pre-order, so it is contiguous and ends at the first
					// row that is not. A thread header carries depth 0 and so
					// terminates any group, which is what stops a frame running
					// off the end of its own thread.
					int last = i;
					for (int j = i + 1; j < profRowCount; ++j) {
						if (profRows[j].header || profRows[j].depth <= pr.depth) break;
						last = j;
					}
					if (last == i) continue; // a leaf is not a group

					const float gx =
						labelX + indent * static_cast<float>(pr.depth + 1) - pad * 0.4f;
					const float gy = rowsTop + static_cast<float>(i) * rowAdvance;
					const float gh = static_cast<float>(last - i + 1) * rowAdvance;
					ui::DrawBorder(batch, {gx, gy, barX + groupW - gx, gh}, kGroupFrame);
				}
			}

			// The path of the row being drawn, kept as the names at each depth so
			// far. The walk is pre-order, so by the time a row is reached its
			// ancestors are exactly the entries below it — no second traversal to
			// reconstruct what coming down already passed through.
			const char* nameAtDepth[prof::kMaxDepth] = {};
			for (int i = 0; i < profRowCount; ++i) {
				const ProfRow& pr = profRows[i];
				if (pr.header) {
					m_font->Draw(batch, pr.name, labelX, py, kAccent);
					m_font->Draw(batch, std::format("{} periods", pr.periods), width * 0.30f,
								py, kDim);
					if (pr.dropped)
						m_font->Draw(batch, "SCOPES DROPPED", width * 0.46f, py, kWarn);
				} else {
					const float nameX = labelX + indent * static_cast<float>(pr.depth + 1);

					// The detail marker, and it distinguishes three states rather
					// than two: an override set HERE reads differently from a level
					// inherited from a parent, because only the first is this row's
					// to clear. All three faces are the same width so raising a
					// level cannot shuffle the name column sideways.
					const i8 eff = EffectiveDetail(pr);
					const bool own = pr.detail >= 0;
					const std::string face = own    ? std::format("[{}]", pr.detail)
											 : eff > 0 ? std::format("({})", eff)
													   : std::string("[+]");
					m_font->Draw(batch, face, nameX, py,
								own ? kAccent : eff > 0 ? kText : kDim);

					m_font->Draw(batch, pr.name, nameX + markerW, py, kText);

					// The held window if there is one, the raw period if this node
					// only appeared mid-window. Falling back to raw rather than to
					// nothing matters most right after a subtree is expanded, which
					// is exactly when every new row would otherwise read 0.000.
					const ProfSmooth* sm = SmoothFor(pr.tid, pr.node);
					const double dIncl = sm ? sm->incl : pr.inclMs;
					const double dExcl = sm ? sm->excl : pr.exclMs;
					const double dMax = sm ? sm->maxMs : pr.maxMs;
					const double dCalls = sm ? sm->calls
											 : static_cast<double>(pr.calls);
					const float dFrac = sm ? static_cast<float>(sm->frac) : pr.frac;

					m_font->Draw(batch, std::format("{:.3f}", dIncl), width * 0.30f, py,
								kText);
					m_font->Draw(batch, std::format("{:.3f}", dExcl), width * 0.38f, py,
								kDim);
					// ONE DECIMAL, because this is a mean over the window and means
					// are fractional. Rounded to an integer it printed `x0` for
					// gpu.shadows — a pass whose cube cache genuinely skips most
					// frames — beside a non-zero time, which reads as a
					// contradiction rather than as "runs about a third of the
					// time". The fraction IS the measurement here.
					m_font->Draw(batch, std::format("x{:.1f}", dCalls), width * 0.46f, py,
								kDim);
					// Bare, no "worst " prefix: the header above names the column,
					// and repeating it on every row would be the label drawn forty
					// times to say what one line already says.
					m_font->Draw(batch, std::format("{:.3f}", dMax), width * 0.52f, py,
								kDim);

					// THE FRAME'S OWN BAR is the exception to the rule below, and
					// it earns the space precisely because a root's share bar
					// would have been the useless always-full one: the slot is
					// free. Three segments — work, blocked on the GPU, blocked in
					// Present — which DO partition the frame's wall clock, since
					// cpu is defined as what is left after the two waits. So this
					// bar is a decomposition and always exactly fills its width,
					// and its proportions are the verdict's reasoning made visible.
					//
					// The GPU's own busy time is a SEPARATE hairline beneath, not
					// a fourth segment, and that is not a layout preference: GPU
					// work overlaps the next frame's CPU work rather than
					// following it, so it is not a slice of this frame's serial
					// budget and stacking it into one would claim time twice.
					const bool frameRow = fb.valid && fb.frameMs > 0.0 &&
										  pr.depth == 0 &&
										  std::strcmp(pr.name, prof::kZoneFrame) == 0 &&
										  pr.tid == fb.tid;
					if (frameRow) {
						const float bh = line * 0.40f;
						const float oy = py + (line - bh) * 0.5f - line * 0.10f;
						const float bw2 = std::min(barW, width - pad * 2.0f - barX);
						if (bw2 > 0.0f) {
							const gfx::Rect track{barX, oy, bw2, bh};
							barTrack(track);
							const auto seg = [&](double ms, float x, const Vec4& c) {
								const float w =
									bw2 * static_cast<float>(
											  std::clamp(ms / fb.frameMs, 0.0, 1.0));
								if (w > 0.0f) batch.DrawRect({x, oy, w, bh}, c);
								return x + w;
							};
							float sx = barX;
							sx = seg(fb.cpuMs, sx, kBudgetCpuColor);
							sx = seg(fb.waitGpuMs, sx, kBudgetWaitColor);
							sx = seg(fb.presentMs, sx, kBudgetPresentColor);
							seg(fb.capMs, sx, kBudgetCapColor);
							barEdge(track);

							if (fb.gpuKnown) {
								// The same quarters on the same width, so the GPU
								// hairline can be read against the frame above it
								// rather than only against itself. No border: at
								// two pixels tall a border is the whole bar.
								const float gh = line * 0.14f;
								const float gy = oy + bh + line * 0.06f;
								const gfx::Rect gtrack{barX, gy, bw2, gh};
								const float gw =
									bw2 * static_cast<float>(
											  std::clamp(fb.gpuBusyMs / fb.frameMs, 0.0, 1.0));
								barTrack(gtrack);
								if (gw > 0.0f)
									batch.DrawRect({barX, gy, gw, gh}, kBudgetGpuColor);
								barQuarters(gtrack, kGridOver);
							}
							// The frame's bar shares the column's axis like every
							// other, so the run of gridlines starts here.
							if (gridTop < 0.0f) gridTop = oy;
							gridBot = oy + bh;
						}
					}

					// DEPTH PIPS, in the gutter — one per level, right-aligned so
					// they end just short of the 0% line and deepen leftward.
					//
					// This is the nesting cue the bars gave up when they stopped
					// being indented, put back WITHOUT costing the axis: it lives
					// outside the measured area entirely, so nothing here can be
					// mistaken for a quantity. That is the whole trick — depth and
					// value both wanted to be encoded in x, and they can coexist
					// only by not sharing a region.
					//
					// Structural, not data: a low-alpha hairline tone, so a glance
					// down the column reads the bars and has to look for these.
					if (pr.depth > 0) {
						constexpr float kPipW = 2.0f;
						const float pipH = line * 0.30f;
						const float pipGap = 3.0f;
						const float pipY = py + (line - pipH) * 0.5f;
						// Never let them reach the numbers: past this the deepest
						// levels simply stop drawing pips rather than colliding
						// with the `worst ms` column.
						const float pipLimit = width * 0.575f;
						for (int d = 0; d < pr.depth; ++d) {
							const float px =
								barX - pad - kPipW - static_cast<float>(d) * (kPipW + pipGap);
							if (px < pipLimit) break;
							batch.DrawRect({px, pipY, kPipW, pipH}, {1.0f, 1.0f, 1.0f, 0.22f});
						}
					}

					// Share of everything this thread recorded in the period —
					// NOT of the frame, since a worker ticking at 0.5 Hz has no
					// relationship to a frame's wall clock and scaling it by one
					// would read as permanently idle.
					//
					// ROOTS GET NO BAR. A root's share is trivially the whole
					// tree, so drawing it gave every worker a full-width bar
					// beside a 0.000 ms reading — the panel's first version said
					// four idle threads were saturated. A bar that is always full
					// carries no information and actively misleads, so the bar is
					// only drawn where it discriminates.
					if (pr.depth > 0) {
						const float bh = line * 0.6f;
						const float oy = py + (line - bh) * 0.5f;
						// ONE ORIGIN FOR EVERY BAR — no longer indented by depth.
						// The indent used to echo the name column so nesting read
						// down the bars too, but it put each depth's 0% at a
						// different x, which means the bars never shared an axis
						// and two of them could not be compared by eye at all.
						// A common origin is what lets the gridlines run the whole
						// height and mean the same thing on every row; nesting is
						// still fully carried by the names, where it was never
						// ambiguous.
						const float bw2 = std::min(barW, width - pad * 2.0f - barX);
						if (bw2 > 0.0f) {
							const gfx::Rect track{barX, oy, bw2, bh};
							barTrack(track);

							// A LANDMARK ROW TAKES ITS SEGMENT'S COLOUR from the
							// frame bar above, so the stack and the rows it
							// decomposes cannot contradict each other — `present`
							// grey up there and blue down here was saying the CPU
							// worked for 3.9 ms and did nothing for 3.9 ms at once.
							Vec4 fill = kShareColor;
							if (fb.valid && pr.tid == fb.tid) {
								if (std::strcmp(pr.name, prof::kZoneRecord) == 0)
									fill = kBudgetCpuColor;
								else if (std::strcmp(pr.name, prof::kZoneWaitGpu) == 0)
									fill = kBudgetWaitColor;
								else if (std::strcmp(pr.name, prof::kZonePresent) == 0)
									fill = kBudgetPresentColor;
							}
							// The SAME smoothed reading as the digits beside it. A
							// bar still twitching at frame rate next to a number
							// holding still reads as the two disagreeing.
							batch.DrawRect(
								{barX, oy, bw2 * std::clamp(dFrac, 0.0f, 1.0f), bh}, fill);
							barEdge(track);
							if (gridTop < 0.0f) gridTop = oy;
							gridBot = oy + bh;
						}
					}

					// The whole marker-and-name run is the click target, not just
					// the marker: the row's NAME is what you are pointing at when
					// you decide you want more detail there, and a three-character
					// box is a small thing to ask someone to hit.
					//
					// Registered only while actually on the panel, exactly as the
					// graph checkboxes are — input is clipped the same way drawing
					// is, so a row scrolled out of view must not stay clickable
					// through the scrollback.
					if (pr.depth < static_cast<int>(prof::kMaxDepth)) {
						nameAtDepth[pr.depth] = pr.name;
						if (py + line > 0.0f && py < panelH) {
							ProfDetailHit hit;
							hit.box = {nameX, py,
									   markerW + m_font->MeasureWidth(pr.name), line};
							hit.slot = pr.slot;
							hit.node = pr.node;
							hit.next = NextDetail(pr);
							hit.atMax = AtMaxDetail(pr);

							size_t w = 0;
							for (int d = 0; d <= pr.depth && w + 1 < sizeof(hit.path); ++d) {
								if (!nameAtDepth[d]) continue;
								if (w > 0) hit.path[w++] = '/';
								for (const char* c = nameAtDepth[d];
									 *c && w + 1 < sizeof(hit.path); ++c)
									hit.path[w++] = *c;
							}
							hit.path[w] = '\0';
							m_profDetailHits.push_back(hit);
						}
					}
				}
				py += rowAdvance;
			}
			// THE COLUMN'S AXIS, run down the whole readout in one pass now that
			// every bar shares an origin. Per-bar ticks said "this bar is 3/4
			// full"; a continuous line says "these two bars cross the same mark",
			// which is the comparison the column exists to support and the one
			// short ticks on separate rows cannot make.
			if (gridTop >= 0.0f && gridBot > gridTop) {
				const float gw = std::min(barW, width - pad * 2.0f - barX);
				for (int q = 1; q < 4; ++q)
					batch.DrawRect({barX + gw * (static_cast<float>(q) * 0.25f), gridTop,
									1.0f, gridBot - gridTop},
								   kGridOver);
			}

			// Only reachable now that the array can actually run out — the tree
			// grows every time a subtree is raised, which is what the rows above
			// are for.
			if (profRowCount < profRowTotal)
				m_font->Draw(batch, std::format("({} more rows do not fit)",
											   profRowTotal - profRowCount),
							labelX, py, kDim);
			}
		} else {
			py += line;
			m_font->Draw(batch, "not compiled in - build debug-profile or release-profile",
						labelX, py, kDim);
		}
	}

	// --- threads panel (bottom, collapsed by default) ------------------------
	// One row per managed worker (Core/ThreadManager.h) with live stats and four
	// clickable controls. m_threadHits records the button rects for next frame's
	// click hit-testing (Update), so the layout lives in exactly one place.
	// --- HEALTH: when each thread went wrong, on the profile graphs' x-axis ---
	// The strip answers a question no instantaneous readout can: a worker that
	// threw and recovered reads perfectly normal a second later, so "is it well"
	// and "has it been well" are different questions and only this one answers
	// the second.
	m_healthHits.clear();
	m_healthBtn = {};
	if (m_healthRowCount > 0) {
		float hy = profileTop + profileBlock + sy;
		batch.DrawRect({0, hy, width, 1.0f}, kBorder);
		hy += line * 0.4f;
		m_font->Draw(batch, "HEALTH", labelX, hy, kAccent);

		const diag::Totals t = diag::ProcessTotals();
		m_font->Draw(batch,
					 std::format("{} events · {}s of history · click a mark for details",
								 t.total,
								 static_cast<int>(kProfHistory * kProfSampleSec)),
					 width * 0.15f, hy, kDim);
		m_healthBtn = expander(hy, m_healthExpanded);
		hy += line;

		if (m_healthExpanded) {
			// One colour per kind, matching the THREADS panel's vocabulary so a
			// red mark and a red state mean the same thing in both places.
			const Vec4 kExc{0.95f, 0.45f, 0.30f, 1.0f};
			const Vec4 kStall{0.90f, 0.75f, 0.30f, 1.0f};
			const Vec4 kBoot{0.45f, 0.75f, 0.95f, 1.0f};
			const Vec4 kDead{0.80f, 0.45f, 0.85f, 1.0f};
			auto kindColor = [&](u8 k) -> Vec4 {
				switch (static_cast<diag::Kind>(k)) {
				case diag::Kind::Fatal:
				case diag::Kind::Fault: return {1.0f, 0.35f, 0.35f, 1.0f};
				case diag::Kind::Killed: return kDead;
				case diag::Kind::Stall: return kStall;
				case diag::Kind::Restart: return kBoot;
				default: return kExc;
				}
			};

			const float stripX = width * 0.15f;
			const float stripW = width * 0.70f;
			const float cellW = stripW / static_cast<float>(kProfHistory);
			for (int r = 0; r < m_healthRowCount; ++r) {
				const HealthRow& row = m_healthRows[r];
				m_font->Draw(batch, row.name, labelX, hy, kText);

				const gfx::Rect strip{stripX, hy, stripW, line};
				batch.DrawRect(strip, kGaugeBg);

				for (int c = 0; c < kProfHistory; ++c) {
					const HealthCell& cell = row.cells[c];
					if (cell.count == 0) continue;
					// Oldest at the LEFT. Age is measured from the newest WRITTEN
					// cell, not from m_profHead — the head is advanced after the
					// sample is stored, so it points at the next cell to fill,
					// which still holds data from a full lap ago. Measuring from
					// it drew the newest mark one cell early and the stalest one
					// at "now".
					const int newest = (m_profHead + kProfHistory - 1) % kProfHistory;
					const int age = (newest - c + kProfHistory) % kProfHistory;
					const float x =
						stripX + stripW - static_cast<float>(age + 1) * cellW;
					// A minimum width of one pixel: at 240 cells across a strip
					// the honest width is sub-pixel, and a mark that rounds away
					// is a failure the timeline did not report.
					const float w2 = std::max(cellW, 1.0f);
					batch.DrawRect({x, hy + 1.0f, w2, line - 2.0f}, kindColor(cell.kind));
				}
				m_healthHits.push_back({strip, r});
				hy += healthRowH;
			}
			m_font->Draw(batch, "  older", stripX, hy, kDim);
			const float nowW = m_font->MeasureWidth("now");
			m_font->Draw(batch, "now", stripX + stripW - nowW, hy, kDim);
		}
	}

	m_threadHits.clear();
	m_threadsBtn = {};
	if (!workers.empty()) {
		float ty = profileTop + profileBlock + healthBlock + sy;
		batch.DrawRect({0, ty, width, 1.0f}, kBorder); // divider from the profile block
		ty += line * 0.4f;
		m_font->Draw(batch, "THREADS", labelX, ty, kAccent);
		const float gov = m_threadMgr.GlobalThrottle();
		if (gov != 1.0f)
			m_font->Draw(batch, std::format("governor {:.2f}x", gov), width * 0.15f, ty,
						{0.55f, 0.85f, 0.95f, 1.0f});

		// Collapsed, the header still has to answer the question the panel exists
		// for at a glance: is anything WRONG? A count of workers plus any that are
		// not simply running means an expand is a decision, not a fishing trip.
		if (!m_threadsExpanded) {
			int notRunning = 0;
			for (const threads::WorkerInfo& w : workers)
				if (w.paused || w.state == threads::State::Dead ||
					w.state == threads::State::Stalled ||
					w.state == threads::State::Quarantined)
					++notRunning;
			m_font->Draw(batch, std::format("{} workers", workers.size()), width * 0.25f, ty,
						kDim);
			if (notRunning > 0)
				m_font->Draw(batch, std::format("{} not running", notRunning), width * 0.34f,
							ty, kWarn);
		}

		m_threadsBtn = expander(ty, m_threadsExpanded);
		ty += line;
	}
	if (!workers.empty() && m_threadsExpanded) {
		float ty = profileTop + profileBlock + healthBlock + line * 1.4f + sy;

		const Vec4 kPaused{0.90f, 0.75f, 0.30f, 1.0f};
		const Vec4 kStalled{0.95f, 0.45f, 0.30f, 1.0f};
		const Vec4 kQuar{0.80f, 0.45f, 0.85f, 1.0f};
		const Vec4 kKill{0.90f, 0.50f, 0.50f, 1.0f};
		const float bw = line * 2.6f, bh = line, bgap = line * 0.4f;

		auto button = [&](const gfx::Rect& r, const std::string& label, const Vec4& col) {
			batch.DrawRect(r, kGaugeBg);
			ui::DrawBorder(batch, r, kBorder);
			const float tw = m_font->MeasureWidth(label);
			m_font->Draw(batch, label, r.x + (r.w - tw) * 0.5f, r.y, col);
		};

		// The health record, snapshotted ONCE for the whole panel rather than
		// per row: the snapshot takes the registry lock, and taking it eight
		// times a frame to draw eight rows would be the readout getting in the
		// way of the thing it reports on.
		diag::ThreadHealth health[diag::kMaxThreads];
		const int healthCount = diag::SnapshotThreads(health, diag::kMaxThreads);

		for (const threads::WorkerInfo& w : workers) {
			const bool quar = w.state == threads::State::Quarantined;
			const bool dead = w.state == threads::State::Dead || quar;
			const Vec4 stCol = quar ? kQuar
							 : w.state == threads::State::Dead ? kDim
							 : w.state == threads::State::Stalled ? kStalled
							 : w.paused ? kPaused
							 : kAccent;
			m_font->Draw(batch, w.name, labelX, ty, kText);
			m_font->Draw(batch, threads::StateName(w.state), width * 0.15f, ty, stCol);
			m_font->Draw(batch, std::format("it {}", w.iterations), width * 0.25f, ty, kDim);
			m_font->Draw(batch, std::format("{:.2f}/{:.2f}ms", w.lastMs, w.avgMs),
						width * 0.34f, ty, kDim);
			m_font->Draw(batch, std::format("{:.2f}hz", w.hz), width * 0.44f, ty, kDim);
			// Re-think PERIOD in ms — the actual cadence value, which reveals the
			// coprime/prime bucket intervals (251/499/997/1999) that the rounded Hz
			// hides (499ms reads as 2.00hz, etc.). Derived from hz (= 1000/hz).
			if (w.hz > 0.0f)
				m_font->Draw(batch, std::format("{:.0f}ms", 1000.0f / w.hz),
							width * 0.50f, ty, kDim);
			m_font->Draw(batch, std::format("p{}", w.priority), width * 0.55f, ty, kDim);
			if (w.restarts > 0)
				m_font->Draw(batch, std::format("re {}", w.restarts), width * 0.585f, ty,
							kDim);

			// What this worker has recorded. A worker that threw and recovered
			// looks identical to a healthy one in every other column — its
			// timings, its tick count and its state all read normal — so without
			// this the panel actively hides the thing worth knowing.
			for (int i = 0; i < healthCount; ++i) {
				if (w.name != health[i].name || health[i].total == 0) continue;
				const u64 bad = health[i].Count(diag::Kind::Exception) +
								health[i].Count(diag::Kind::Fault) +
								health[i].Count(diag::Kind::Fatal);
				const u64 stalls = health[i].Count(diag::Kind::Stall);
				m_font->Draw(batch,
							 stalls > 0 ? std::format("!{} ~{}", bad, stalls)
										: std::format("!{}", bad),
							 width * 0.635f, ty, bad > 0 ? kStalled : kPaused);
				break;
			}

			const float killX = width - pad * 2.0f - bw;
			const float fastX = killX - (bw + bgap);
			const float slowX = fastX - (bw + bgap);
			const float pauseX = slowX - (bw + bgap);
			ThreadHit hit{w.id, {}, {}, {}, {}, {}};
			if (dead) {
				// A dead worker offers a single 'boot' to relaunch it (Restart).
				const gfx::Rect bootR{killX, ty, bw, bh};
				button(bootR, "boot", kAccent);
				hit.boot = bootR;
			} else {
				const gfx::Rect pauseR{pauseX, ty, bw, bh}, slowR{slowX, ty, bw, bh},
					fastR{fastX, ty, bw, bh}, killR{killX, ty, bw, bh};
				button(pauseR, w.paused ? "run" : "halt", kText);
				button(slowR, "<<", kText);
				button(fastR, ">>", kText);
				button(killR, "kill", kKill);
				hit.pause = pauseR;
				hit.slower = slowR;
				hit.faster = fastR;
				hit.kill = killR;
			}
			m_threadHits.push_back(hit);

			ty += rowAdvance;
		}
	}

	batch.SetScissor(nullptr);

	// A thumb on the right edge, only when there is something to scroll to. The
	// panel has no visible frame of its own beyond the border, so without this
	// there is nothing to say the readout continues past the bottom.
	if (contentH > panelH) {
		const float tw = line * 0.35f;
		const float frac = panelH / contentH;
		const float th = std::max(panelH * frac, line);
		const float ty2 = (panelH - th) * (m_panelScroll / (contentH - panelH));
		batch.DrawRect({width - tw, 0, tw, panelH}, kGaugeBg);
		batch.DrawRect({width - tw, ty2, tw, th}, kBorder);
	}

	// --- output log + input line (bottom) -----------------------------------
	const float inputY = height - line - pad;
	const float promptW = m_font->MeasureWidth("> ");
	m_font->Draw(batch, "> ", labelX, inputY, kAccent);
	m_font->Draw(batch, m_input, labelX + promptW, inputY, kText);
	if (std::fmod(m_caretBlink, 1.0f) < 0.5f) {
		const float caretX = labelX + promptW + m_font->MeasureWidth(m_input);
		batch.DrawRect({caretX + 1.0f, inputY, 2.0f, line}, kText);
	}
	batch.DrawRect({0, inputY - pad * 0.5f, width, 1.0f}, kBorder);

	// Scrollback, newest at the bottom just above the input line.
	const float logTop = panelH + pad;
	const float logBottom = inputY - pad;
	int visible = static_cast<int>((logBottom - logTop) / line);
	if (visible < 0) visible = 0;
	const int total = static_cast<int>(m_output.size());
	const int end = std::max(0, total - m_scroll); // index past the last shown
	const int start = std::max(0, end - visible);
	float ly = logBottom - line;
	for (int i = end - 1; i >= start; --i) {
		m_font->Draw(batch, m_output[static_cast<size_t>(i)], labelX, ly, kText);
		ly -= line;
	}
}

} // namespace dungeon::game
