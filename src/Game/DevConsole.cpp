// ============================================================================
// Game/DevConsole.cpp — see DevConsole.h.
// ============================================================================
#include "Game/DevConsole.h"

#include "Core/Paths.h"
#include "Core/Profile.h"
#include "UI/Controls.h" // ui::DrawBorder

#include <Windows.h> // VK_* codes

#include <algorithm>
#include <cmath>
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
constexpr int kMaxProfRows = 44;
// Matches the series pool: every measure that HAS history can be graphed, since
// the panel scrolls now and no longer has to fit them all on screen at once.
// What keeps the cost down is culling the ones scrolled out of view, not a cap.
constexpr int kMaxGraphs = 32;

struct ProfRow {
	char name[32] = {};
	u32 tid = 0;  // owning thread + node index: the stable key the graph view
	u32 node = 0; // uses to follow one measure across frames as the tree grows
	int depth = 0;
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
int BuildProfileRows(ProfRow* out, int cap) {
	const prof::Clock clock = prof::ClockInfo();
	prof::ThreadReport reports[prof::kMaxThreads];
	const int threadCount = prof::SnapshotAll(reports, prof::kMaxThreads);

	int count = 0;
	for (int i = 0; i < threadCount && count < cap; ++i) {
		const prof::ThreadReport& r = reports[i];

		ProfRow& head = out[count++];
		head.header = true;
		head.tid = r.osThreadId;
		CopyName(head.name, r.name);
		head.periods = r.periods;
		head.dropped = r.nodeOverflows > 0 || r.depthOverflows > 0;

		// The bar is a fraction of everything this thread recorded in the period,
		// summed over the roots — not of the frame's wall clock, which the worker
		// threads have no relationship to.
		u64 total = 0;
		for (u32 c = r.root; c != prof::kInvalidNode; c = r.nodes[c].nextSibling)
			total += r.nodes[c].inclusive;

		// Depth-first with an explicit stack. Children are PREPENDED as they are
		// discovered, so pushing them in list order and popping restores call
		// order — the tree reads the way the code ran.
		struct Visit {
			u32 node;
			int depth;
		};
		Visit stack[prof::kMaxDepth * 2];
		constexpr int kStackCap = static_cast<int>(std::size(stack));
		int top = 0;
		for (u32 c = r.root; c != prof::kInvalidNode && top < kStackCap;
			 c = r.nodes[c].nextSibling)
			stack[top++] = {c, 0};

		while (top > 0 && count < cap) {
			const Visit v = stack[--top];
			const prof::NodeView& node = r.nodes[v.node];

			ProfRow& row = out[count++];
			CopyName(row.name, node.zone ? node.zone->name : "?");
			row.tid = r.osThreadId;
			row.node = v.node;
			row.depth = v.depth;
			row.inclMs = prof::TicksToMs(node.inclusive, clock);
			row.exclMs = prof::TicksToMs(node.Exclusive(), clock);
			row.maxMs = prof::TicksToMs(node.maxTicks, clock);
			row.calls = node.calls;
			row.frac = total > 0 ? static_cast<float>(static_cast<double>(node.inclusive) /
													  static_cast<double>(total))
								 : 0.0f;

			for (u32 c = node.firstChild; c != prof::kInvalidNode && top < kStackCap;
				 c = r.nodes[c].nextSibling)
				stack[top++] = {c, v.depth + 1};
		}
	}
	return count;
}
#else
int BuildProfileRows(ProfRow*, int) { return 0; }
#endif

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
void DrawSeriesGraph(gfx::SpriteBatch& batch, const gfx::Rect& plot, const float* samples,
					 int count, int head, float scale, const Vec4& color) {
	batch.DrawRect(plot, kGaugeBg);
	ui::DrawBorder(batch, plot, kBorder);
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
	const Vec4 fillColor{color.x, color.y, color.z, 0.22f};
	for (int k = 0; k < count; ++k) {
		const float top = yFor(samples[(head + k) % count]);
		if (base - top < 0.5f) continue;
		const float fx = plot.x + static_cast<float>(k) * stepX;
		const float fw = std::min(stepX + 1.0f, plot.x + plot.w - fx);
		if (fw > 0.0f) batch.DrawRect({fx, top, fw, base - top}, fillColor);
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
			 "panel [on|off] | dump [file] | detail <path> <level>",
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
								   "<level>', e.g. 'profile detail frame/render 1'");
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
				 } else {
					 if (!args.empty())
						 m_showProfile = args[0] != "off" && args[0] != "0";
					 else
						 m_showProfile = !m_showProfile;
					 Print(std::format("profile panel {}", m_showProfile ? "on" : "off"));
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
	ProfRow rows[kMaxProfRows];
	const int n = BuildProfileRows(rows, kMaxProfRows);

	for (int i = 0; i < m_profSeriesCount; ++i) m_profSeries[i].seen = false;

	const char* thread = "";
	for (int i = 0; i < n; ++i) {
		const ProfRow& r = rows[i];
		if (r.header) {
			thread = r.name;
			continue;
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
}
#else
void DevConsole::SampleProfileSeries() {}
void DevConsole::CommitProfileSeries() {}
#endif

void DevConsole::SampleHistory(float dt, const gfx::GraphicsDevice& device) {
	// Each slot keeps the MAX since the last commit, so a spike survives the
	// downsampling rather than being averaged into the baseline around it.
	const PerfMonitor::Metrics& m = m_perf.Get();
	const gfx::GraphicsDevice::GpuMemoryInfo vram = device.QueryGpuMemory();
	auto bump = [&](PerfLine which, float v) {
		m_perfSeries[which].pending = std::max(m_perfSeries[which].pending, v);
	};
	bump(kFps, m.fps);
	bump(kCpu, m.cpuPercent);
	bump(kGpu, m.gpuPercent >= 0.0f ? m.gpuPercent : 0.0f); // < 0 means unavailable
	bump(kRam, static_cast<float>(m.sysMemUsedMB));
	bump(kVram, static_cast<float>(vram.usedBytes) / (1024.0f * 1024.0f));
	bump(kSrv, static_cast<float>(device.SrvLive()));

	SampleProfileSeries();

	m_profSampleTimer += dt;
	if (m_profSampleTimer < kProfSampleSec) return;
	m_profSampleTimer = 0.0f;

	for (PerfSeries& s : m_perfSeries) {
		s.samples[m_profHead] = s.pending;
		s.pending = 0.0f;
	}
	CommitProfileSeries();

	// One cursor for both sets, advanced once: every graph on screen shares an
	// x-axis, so a spike in one lines up with a spike in another.
	m_profHead = (m_profHead + 1) % kProfHistory;
}

void DevConsole::Update(const Input& input, float dt, float windowW, float windowH,
						const gfx::GraphicsDevice& device) {
	m_perf.Tick(dt);
	// Every frame, open or closed: switching to a graph view should show the last
	// twelve seconds, not start blank.
	SampleHistory(dt, device);
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
		if (m_profViewBtn.Contains(mx, my)) m_profileGraph = !m_profileGraph;
		if (m_perfViewBtn.Contains(mx, my)) m_perfGraph = !m_perfGraph;
		if (m_threadsBtn.Contains(mx, my)) m_threadsExpanded = !m_threadsExpanded;
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
	int perfVisible = 0;
	for (int i = 0; i < kPerfLines; ++i)
		if (!m_perfHidden[i]) ++perfVisible;
	const int perfHiddenCount = kPerfLines - perfVisible;
	const int perfGraphRows = (perfVisible + 1) / 2;
	const float perfBody =
		m_perfGraph ? static_cast<float>(perfGraphRows) * (graphH + graphGapY) +
						  static_cast<float>(perfHiddenCount) * line
					: line * 6.0f;
	const float rowAdvance = line * 1.2f;

	// PROFILE sits directly under the gauges; THREADS goes to the BOTTOM of the
	// panel and starts COLLAPSED. It is a control surface — halt, rate, kill,
	// boot — rather than something you watch, so it should not push the readout
	// you are actually reading down the screen to make room for buttons.
	const float profileTop = pad + line * 3.0f + perfBody;
	const float threadsBlock =
		workers.empty()     ? 0.0f
		: m_threadsExpanded ? line * 1.4f + rowAdvance * static_cast<float>(workers.size())
							: line * 1.4f;

	// Flattened first: the panel's background is filled before anything is drawn
	// into it, so its height has to be known up front. In a build without
	// DN_PROFILE this returns 0 rows and the section collapses to one line saying
	// which configs have it.
	ProfRow profRows[kMaxProfRows];
	const int profRowCount = m_showProfile ? BuildProfileRows(profRows, kMaxProfRows) : 0;

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
	const int listRows = profRowCount;

	// Header line + the TSC/toggle line, then the body. Nothing is dropped to
	// make it fit any more: the panel SCROLLS, so the content is laid out at its
	// natural height and the window decides how much of it you see.
	const float profileHeaderH = line * 2.4f;
	const float profileBody =
		!prof::kEnabled ? rowAdvance
		: m_profileGraph
			? static_cast<float>(graphRows) * (graphH + graphGapY) +
				  static_cast<float>(profHiddenCount) * line
			: rowAdvance * static_cast<float>(listRows);
	const float profileBlock = !m_showProfile ? 0.0f : profileHeaderH + profileBody;

	// CONTENT height (everything, laid out) against the VISIBLE height (what the
	// window can spare above the scrollback and the prompt). The panel is the
	// smaller; the difference is what there is to scroll through.
	const float contentH = profileTop + profileBlock + threadsBlock + pad;
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
	// Text checkboxes, because this is a monospaced dev console and "[x]" reads
	// better here than a drawn box would. Registers the hit rect only if it is
	// actually ON the panel: input is clipped the same way drawing is, so a
	// checkbox scrolled out of view cannot be clicked through the scrollback.
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
	{
		// Same toggle idiom as the profile panel: Render lays the rect out, next
		// frame's Update hit-tests it, and the label names the DESTINATION.
		const char* face = m_perfGraph ? " bars " : " graph ";
		const float bw2 = m_font->MeasureWidth(face);
		m_perfViewBtn = {width - pad * 2.0f - bw2, y, bw2, line};
		batch.DrawRect(m_perfViewBtn, kGaugeBg);
		ui::DrawBorder(batch, m_perfViewBtn, kBorder);
		m_font->Draw(batch, face, m_perfViewBtn.x, y, kAccent);
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
		{"CPU", std::format("CPU  {:.0f}%", m.cpuPercent), m.cpuPercent, 100.0f,
		 {0.45f, 0.70f, 0.95f, 1.0f}, false},
		{"GPU",
		 m.gpuPercent >= 0.0f ? std::format("GPU  {:.0f}%", m.gpuPercent)
							  : std::string("GPU  n/a"),
		 m.gpuPercent >= 0.0f ? m.gpuPercent : 0.0f, 100.0f,
		 {0.55f, 0.85f, 0.55f, 1.0f}, false},
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

	if (!m_perfGraph) {
		for (int i = 0; i < kPerfLines; ++i) {
			const PerfItem& it = items[i];
			m_font->Draw(batch, it.text, labelX, y,
						it.warn ? kWarn : (i == kGpu && m.gpuPercent < 0.0f) ? kDim : kText);
			if (!(i == kGpu && m.gpuPercent < 0.0f))
				gauge(y, it.value / it.scale, it.color);
			y += line;
		}
	} else {
		// DISPLAY ORDER for the two-column grid, which is not the order the enum
		// declares. Filling the grid in declaration order put CPU beside FPS and
		// RAM beside GPU — pairs that mean nothing next to each other. This lays
		// the rows out as the two PROCESSORS side by side and the two MEMORIES
		// side by side, with the frame rate and the descriptor ceiling (the only
		// two with no partner) heading their columns.
		//
		// The bar view keeps the declaration order: it is a single column, so
		// "beside" does not arise there and CPU/GPU and RAM/VRAM already fall
		// adjacent vertically.
		constexpr PerfLine kGraphOrder[kPerfLines] = {kFps, kSrv, kGpu, kCpu, kVram, kRam};

		const float pgw = (width - pad * 6.0f) * 0.5f;
		int shown = 0;
		for (int oi = 0; oi < kPerfLines; ++oi) {
			const int i = kGraphOrder[oi];
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
			const int i = kGraphOrder[oi];
			if (!m_perfHidden[i]) continue;
			const float cw = checkbox(pad * 2.0f, y, false, i, 0, 0);
			m_font->Draw(batch, items[i].text, pad * 2.0f + cw, y, kDim);
			y += line;
		}
	}

	row(std::format("Process working set: {:.0f} MB", m.procMemMB));
	row("GPU: " + device.AdapterName());

	// --- profile panel (below the gauges) -----------------------------------
	// One tree per measured thread, deepest-first indentation, with a bar giving
	// each node's share of what its thread recorded this period. The numbers are
	// the LAST PUBLISHED period — a frame for the main thread, a tick for a
	// worker — so this is a live readout rather than a running total.
	if (m_showProfile) {
		float py = profileTop + sy;
		batch.DrawRect({0, py, width, 1.0f}, kBorder);
		py += line * 0.4f;
		m_font->Draw(batch, "PROFILE", labelX, py, kAccent);

		if constexpr (prof::kEnabled) {
			const prof::Clock clock = prof::ClockInfo();
			m_font->Draw(batch, std::format("TSC {:.0f} MHz", clock.mhz), width * 0.15f, py,
						kDim);
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
				m_profViewBtn = {width - pad * 2.0f - bw2, py, bw2, line};
				batch.DrawRect(m_profViewBtn, kGaugeBg);
				ui::DrawBorder(batch, m_profViewBtn, kBorder);
				m_font->Draw(batch, face, m_profViewBtn.x, py, kAccent);
			}
			py += line;

			const float indent = m_font->MeasureWidth("  ");
			const float barX = width * 0.62f;
			const float barW = width * 0.26f;
			if (m_profileGraph) {
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
			} else
			for (int i = 0; i < listRows; ++i) {
				const ProfRow& pr = profRows[i];
				if (pr.header) {
					m_font->Draw(batch, pr.name, labelX, py, kAccent);
					m_font->Draw(batch, std::format("{} periods", pr.periods), width * 0.30f,
								py, kDim);
					if (pr.dropped)
						m_font->Draw(batch, "SCOPES DROPPED", width * 0.46f, py, kWarn);
				} else {
					m_font->Draw(batch, pr.name,
								labelX + indent * static_cast<float>(pr.depth + 1), py,
								kText);
					m_font->Draw(batch, std::format("{:.3f}", pr.inclMs), width * 0.30f, py,
								kText);
					m_font->Draw(batch, std::format("{:.3f}", pr.exclMs), width * 0.38f, py,
								kDim);
					m_font->Draw(batch, std::format("x{}", pr.calls), width * 0.46f, py, kDim);
					m_font->Draw(batch, std::format("max {:.3f}", pr.maxMs), width * 0.52f,
								py, kDim);

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
						// INDENTED BY THE SAME STEP AS THE NAME, so a bar sits
						// under its parent's bar the way its label sits under its
						// parent's label and the nesting reads down the column
						// instead of having to be reconstructed from the text.
						// The WIDTH is unchanged by depth — equal widths have to
						// keep meaning equal shares, or the indent would quietly
						// rescale every child.
						const float bx = barX + indent * static_cast<float>(pr.depth + 1);
						const float bw2 = std::min(barW, width - pad * 2.0f - bx);
						if (bw2 > 0.0f) {
							batch.DrawRect({bx, oy, bw2, bh}, kGaugeBg);
							batch.DrawRect(
								{bx, oy, bw2 * std::clamp(pr.frac, 0.0f, 1.0f), bh},
								{0.45f, 0.70f, 0.95f, 1.0f});
						}
					}
				}
				py += rowAdvance;
			}
			if (listRows < profRowCount)
				m_font->Draw(batch, std::format("({} more rows do not fit)",
											   profRowCount - listRows),
							labelX, py, kDim);
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
	m_threadHits.clear();
	m_threadsBtn = {};
	if (!workers.empty()) {
		float ty = profileTop + profileBlock + sy;
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

		{
			const char* face = m_threadsExpanded ? " hide " : " show ";
			const float bw2 = m_font->MeasureWidth(face);
			m_threadsBtn = {width - pad * 2.0f - bw2, ty, bw2, line};
			batch.DrawRect(m_threadsBtn, kGaugeBg);
			ui::DrawBorder(batch, m_threadsBtn, kBorder);
			m_font->Draw(batch, face, m_threadsBtn.x, ty, kAccent);
		}
		ty += line;
	}
	if (!workers.empty() && m_threadsExpanded) {
		float ty = profileTop + profileBlock + line * 1.4f + sy;

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
