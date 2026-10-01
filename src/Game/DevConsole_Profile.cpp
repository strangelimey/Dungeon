// ============================================================================
// Game/DevConsole_Profile.cpp - the console's PROFILE section: the zone tree
// (list or graphs), the frame-budget verdict and its tooltips, the readout
// smoothing, and the `profile` command. Snapshots and diffs, which that command
// also reaches, are DevConsole_Snapshots.cpp.
// ============================================================================
#include "Game/DevConsole_Panel.h"

#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Profile.h"
#include "UI/Controls.h" // ui::DrawBorder

#include <algorithm>
#include <cmath>
#include <cstring> // strcmp, matching zone landmarks by name
#include <format>

namespace dungeon::game {

using namespace devcon;

// Declared in DevConsole_Panel.h: snapshots name their rows with it too.
void devcon::CopyName(char (&dst)[32], const char* src) {
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
int devcon::BuildProfileRows(ProfRow* out, int cap, int* total) {
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
int devcon::BuildProfileRows(ProfRow*, int, int* total) {
	if (total) *total = 0;
	return 0;
}
#endif

namespace {
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

} // namespace

// The `profile` command: the section's typed controls, and the only way in to
// trace dumps and snapshots.
void DevConsole::RegisterProfileCommand() {
	Register({.name = "profile",
			  .group = CmdGroup::Profiling,
			  .params = "panel\n"
						"dump\n"
						"detail <path> <level>\n"
						"smooth <secs>\n"
						"snap <name> [secs]\n"
						"snaps\n"
						"diff <before> <after>",
			  .summary = "the zone profiler: panel view, dumps, detail levels, snapshots and diffs"},
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
				 } else if (!args.empty() &&
							(args[0] == "snap" || args[0] == "snaps" || args[0] == "diff")) {
					 SnapCommand(args); // DevConsole_Snapshots.cpp
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
}

// Same preprocessor split as BuildProfileRows, and for the same reason: a
// discarded `if constexpr` branch in a non-template is still compiled.
#if DN_PROFILE
// The profile half. Split out so the perf half (DevConsole_Perf.cpp) keeps
// working in a build with no profiler: CPU, GPU, memory and descriptor slots
// are not the profiler's to report and must graph in every configuration.
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
#endif

void DevConsole::PrepareProfile(ProfileFrame& f) const {
	f.count = m_profileExpanded ? BuildProfileRows(f.rows, kMaxProfRows, &f.total) : 0;
	const ProfRow* profRows = f.rows;
	const int profRowCount = f.count;

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
	f.zones = profGraphCount;
	f.visible = profVisible;
	f.hidden = profHiddenCount;
}

float DevConsole::ProfileSectionHeight(const PanelCtx& p, const ProfileFrame& f) const {
	const float line = p.line, rowAdvance = p.rowAdvance;
	const float graphH = p.graphH, graphGapY = p.graphGapY;
	const int profRowCount = f.count, profRowTotal = f.total, profHiddenCount = f.hidden;
	const int graphsShown = std::min(f.visible, kMaxGraphs);
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
	return profileBlock;
}

// --- profile panel (below the gauges) -----------------------------------
// One tree per measured thread, deepest-first indentation, with a bar giving
// each node's share of what its thread recorded this period. The numbers are
// the LAST PUBLISHED period — a frame for the main thread, a tick for a
// worker — so this is a live readout rather than a running total.
// Named `pc` and `frame` rather than the other sections' `p`, because the body
// below has its own `p` (a tooltip part) and `f` (a frame time).
void DevConsole::DrawProfileSection(const PanelCtx& pc, float top, const ProfileFrame& frame) {
	gfx::SpriteBatch& batch = pc.batch;
	const float width = pc.width, line = pc.line, pad = pc.pad, labelX = pc.labelX;
	const float panelH = pc.panelH, rowAdvance = pc.rowAdvance;
	const float graphH = pc.graphH, graphGapY = pc.graphGapY;
	const ProfRow* profRows = frame.rows;
	const int profRowCount = frame.count, profRowTotal = frame.total;
	const int profGraphCount = frame.zones, profVisible = frame.visible;
	const int graphsShown = std::min(profVisible, kMaxGraphs);
	const int graphRows = (graphsShown + 1) / 2; // two columns

	m_profDetailHits.clear();
	float py = top + pc.sy;
	batch.DrawRect({0, py, width, 1.0f}, kBorder);
	py += line * 0.4f;
	m_font->Draw(batch, "PROFILE", labelX, py, kAccent);
	m_profExpandBtn = DrawExpander(pc, py, m_profileExpanded);
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
		//
		// Shown in BOTH views. It was list-only while the graph view's
		// figures were raw; now that they are held too, hiding the label
		// there would leave a whole view quietly reporting means as
		// instants — the precise thing the label exists to prevent.
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
		// The header's hover strip, stopping short of the two BUTTONS on the
		// same line: a tooltip popping up over `graph` and `hide` while you
		// were reaching for them would fight the controls.
		m_profHeaderRect = {labelX, py,
							std::max(0.0f, m_profViewBtn.x - pad - labelX), line};
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

				const float cw = DrawCheckbox(pc, gx, gy, true, -1, pr.tid, pr.node);
				m_font->Draw(batch, std::format("{}/{}", curThread, pr.name), gx + cw, gy,
							kText);

				// A timing has no natural ceiling, so unlike the five gauges
				// above these autoscale to their own window. The peak is
				// printed beside the current value so the axis is never a
				// mystery.
				float peak = 0.0f;
				if (ser)
					for (float v : ser->samples) peak = std::max(peak, v);
				// The SAME held reading the list view shows. The plot wants raw
				// samples — a graph is a shape, and smoothing it would iron out
				// the spike it exists to show — but the NUMBER beside it is read
				// rather than watched, and at frame rate it cannot be. The peak
				// needs no help: it is already a max over the whole window.
				const ProfSmooth* gsm = SmoothFor(pr.tid, pr.node);
				m_font->Draw(batch,
							std::format("{:.3f} peak {:.3f} ms",
										gsm ? gsm->incl : pr.inclMs, peak),
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
				const float cw = DrawCheckbox(pc, pad * 2.0f, py, false, -1, pr.tid, pr.node);
				m_font->Draw(batch, std::format("{}/{}", curThread, pr.name),
							pad * 2.0f + cw, py, kDim);
				// Held too. A collapsed measure is still a number someone is
				// reading, and it has no graph beside it to read instead.
				const ProfSmooth* hsm = SmoothFor(pr.tid, pr.node);
				m_font->Draw(batch,
							std::format("{:.3f} ms", hsm ? hsm->incl : pr.inclMs),
							width * 0.30f, py, kDim);
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
		// Where the frame bar ended up, so the tooltip — drawn after every
		// row, or the rows below would paint over it — can be placed against
		// it without recomputing the layout.
		gfx::Rect tipAnchor{};

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

						// The hover target covers the stack AND the GPU
						// hairline under it: they are one reading, and a
						// tooltip that vanished when the pointer drifted two
						// pixels onto the green would be a puzzle rather than
						// a control. Recorded for the next Update to test;
						// remembered here so the tooltip drawn after the rows
						// knows where to sit.
						m_frameBarRect = {barX, oy, bw2, (gridBot + line * 0.20f) - oy};
						tipAnchor = m_frameBarRect;
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

		// --- tooltips ------------------------------------------------
		// Drawn LAST, so no row can paint over them, and still inside the
		// panel's scissor so they cannot escape onto the scrollback.
		//
		// Placement is shared: BELOW the thing being explained by
		// preference, ABOVE when that would run past the panel, and never ON
		// it — the row under the pointer is what the tooltip is about, and
		// covering it would answer a question by hiding it.
		auto placeTip = [&](const gfx::Rect& anchor, float w, float h) {
			return ui::PlaceTooltip(anchor, w, h, {0, 0, width, panelH}, ui::TipSide::Below,
									line * 0.3f, pad, ui::TipAlign::Start);
		};
		auto tipPanel = [&](const gfx::Rect& r) {
			// Near-opaque on purpose: it is a panel over a busy readout, and
			// a translucent one would leave the numbers behind it legible
			// through the numbers in front.
			batch.DrawRect(r, {0.10f, 0.10f, 0.13f, 0.97f});
			ui::DrawBorder(batch, r, kBorder);
		};

		// What every term on the header line means. One tooltip for the whole
		// line: they are a single sentence about where a frame went, and
		// seven separate hovers would hide the relationship between them.
		if (m_profHeaderHover) {
			struct Term {
				const char* name;
				const char* what;
				Vec4 col;
			};
			const Term terms[] = {
				{"TSC", "the CPU timestamp clock; every timing converts through it",
				 kDim},
				{"avg", "the digits are a mean over this window ('worst' is a max)",
				 kDim},
				{"bound by", "what is holding the frame back right now", kText},
				{"cpu", "the main thread WORKING: the frame minus every block below",
				 kBudgetCpuColor},
				{"wait", "blocked on the GPU - it is running frames behind",
				 kBudgetWaitColor},
				{"present", "blocked in Present - waiting for the display",
				 kBudgetPresentColor},
				{"cap", "held back by the frame cap (dev: framecap on|off)",
				 kBudgetCapColor},
				{"gpu", "GPU busy time - CONCURRENT, not a slice of the frame",
				 kBudgetGpuColor},
			};
			const float tipPad = line * 0.5f;
			const float nameW = m_font->MeasureWidth("bound by  ");
			float widest = 0.0f;
			for (const Term& t : terms)
				widest = std::max(widest, nameW + m_font->MeasureWidth(t.what));
			const gfx::Rect tip = placeTip(m_profHeaderRect, widest + tipPad * 2.0f,
										   tipPad * 2.0f +
											   line * static_cast<float>(std::size(terms)));
			tipPanel(tip);
			float ry = tip.y + tipPad;
			for (const Term& t : terms) {
				// The term in ITS OWN COLOUR, so the tooltip doubles as the
				// key for the bar and the graph rather than being a third
				// place the same four colours are described.
				m_font->Draw(batch, t.name, tip.x + tipPad, ry, t.col);
				m_font->Draw(batch, t.what, tip.x + tipPad + nameW, ry, kText);
				ry += line;
			}
		}

		if (m_frameBarHover && fb.valid && fb.frameMs > 0.0 && tipAnchor.w > 0.0f) {
			struct Part {
				const char* name;
				double ms;
				Vec4 col;
				bool stacked; // false = drawn under the stack, not part of it
			};
			const Part parts[] = {
				{"cpu", fb.cpuMs, kBudgetCpuColor, true},
				{"wait.gpu", fb.waitGpuMs, kBudgetWaitColor, true},
				{"present", fb.presentMs, kBudgetPresentColor, true},
				{"cap", fb.capMs, kBudgetCapColor, true},
				{"gpu busy", fb.gpuBusyMs, kBudgetGpuColor, false},
			};
			constexpr int kParts = static_cast<int>(std::size(parts));

			// Sized to its widest row rather than to a guess, so a three-digit
			// millisecond reading cannot run out through the border.
			const float sw = line * 0.6f;             // colour swatch
			const float gap = m_font->MeasureWidth("  ");
			float widest = m_font->MeasureWidth("frame 000.000 ms");
			for (const Part& p : parts)
				widest = std::max(widest,
								  sw + gap +
									  m_font->MeasureWidth(
										  std::format("{:<9} {:>7.3f} ms  {:>3.0f}%",
													  p.name, p.ms, 0.0)));
			const float tipPad = line * 0.5f;
			const float miniH = line * 0.7f;
			const float tipW = widest + tipPad * 2.0f;
			const float tipH = tipPad * 2.0f + miniH + line * 0.4f +
							   line * static_cast<float>(kParts + 1) + line * 0.3f;

			const gfx::Rect tip = placeTip(tipAnchor, tipW, tipH);
			const float tx = tip.x, ty = tip.y;
			tipPanel(tip);

			// The same bar again, wider, so the tooltip is recognisably about
			// the thing under the pointer rather than a table that happens to
			// share its numbers.
			const gfx::Rect mini{tx + tipPad, ty + tipPad, tipW - tipPad * 2.0f, miniH};
			batch.DrawRect(mini, kGaugeBg);
			float sx2 = mini.x;
			for (const Part& p : parts) {
				if (!p.stacked) continue;
				const float w =
					mini.w * static_cast<float>(std::clamp(p.ms / fb.frameMs, 0.0, 1.0));
				if (w > 0.0f) batch.DrawRect({sx2, mini.y, w, mini.h}, p.col);
				sx2 += w;
			}
			ui::DrawBorder(batch, mini, kBorder);

			float ry = mini.y + miniH + line * 0.4f;
			for (const Part& p : parts) {
				// A rule before the GPU row: it is measured against the same
				// frame but does not SIT in the stack — it overlaps the next
				// frame's work — and the separation has to be visible or the
				// tooltip teaches the double-count the bar was built to avoid.
				if (!p.stacked) {
					batch.DrawRect({tx + tipPad, ry + line * 0.1f, tipW - tipPad * 2.0f,
									1.0f},
								   kBorder);
					ry += line * 0.3f;
				}
				batch.DrawRect({tx + tipPad, ry + (line - sw) * 0.5f, sw, sw}, p.col);
				m_font->Draw(batch,
							std::format("{:<9} {:>7.3f} ms  {:>3.0f}%", p.name, p.ms,
										p.ms / fb.frameMs * 100.0),
							tx + tipPad + sw + gap, ry, kText);
				ry += line;
			}
			m_font->Draw(batch, std::format("frame {:.3f} ms", fb.frameMs), tx + tipPad,
						ry, kAccent);
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

void DevConsole::ProfileHover(float mx, float my) {
	m_frameBarHover = m_frameBarRect.Contains(mx, my);
	m_profHeaderHover = m_profHeaderRect.Contains(mx, my);
}

void DevConsole::ProfileClick(float mx, float my) {
	if (m_profExpandBtn.Contains(mx, my)) m_profileExpanded = !m_profileExpanded;
	if (m_profViewBtn.Contains(mx, my)) m_profileGraph = !m_profileGraph;
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
}

} // namespace dungeon::game
