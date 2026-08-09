// ============================================================================
// Game/DevConsole.cpp — see DevConsole.h.
// ============================================================================
#include "Game/DevConsole.h"

#include "Core/Profile.h"
#include "UI/Controls.h" // ui::DrawBorder

#include <Windows.h> // VK_* codes

#include <algorithm>
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

struct ProfRow {
	char name[32] = {};
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
	Register("profile", "show/hide the profile panel [on|off]",
			 [this](const std::vector<std::string>& args) {
				 if (!args.empty())
					 m_showProfile = args[0] != "off" && args[0] != "0";
				 else
					 m_showProfile = !m_showProfile;
				 if constexpr (!prof::kEnabled)
					 Print("profiling is not compiled in (build debug-profile or "
						   "release-profile)");
				 else
					 Print(std::format("profile panel {}", m_showProfile ? "on" : "off"));
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

void DevConsole::Update(const Input& input, float dt, float windowW, float windowH) {
	m_perf.Tick(dt);
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

	// Scroll the output (wheel, or PageUp/PageDown).
	int scrollLines = static_cast<int>(input.WheelDelta());
	if (input.WasKeyPressed(VK_PRIOR)) scrollLines += 5;
	if (input.WasKeyPressed(VK_NEXT)) scrollLines -= 5;
	if (scrollLines != 0) {
		m_scroll = std::clamp(m_scroll + scrollLines, 0,
							  static_cast<int>(m_output.size()));
		m_caretBlink = 0.0f;
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
	const std::vector<threads::WorkerInfo> workers = m_threadMgr.SnapshotAll();
	const float threadsTop = pad + line * 8.0f;
	const float rowAdvance = line * 1.2f;
	const float threadsBlock =
		workers.empty() ? 0.0f
						: line * 1.4f + rowAdvance * static_cast<float>(workers.size());

	// Flattened first: the panel's background is filled before anything is drawn
	// into it, so its height has to be known up front. In a build without
	// DN_PROFILE this returns 0 rows and the section collapses to one line saying
	// which configs have it.
	ProfRow profRows[kMaxProfRows];
	const int profRowCount = m_showProfile ? BuildProfileRows(profRows, kMaxProfRows) : 0;
	const float profileBlock =
		!m_showProfile ? 0.0f
					   : line * 1.4f + rowAdvance * static_cast<float>(
											prof::kEnabled ? profRowCount : 1);

	const float panelH = threadsTop + threadsBlock + profileBlock + pad;
	batch.DrawRect({0, 0, width, panelH}, kPerfBg);
	ui::DrawBorder(batch, {0, 0, width, panelH}, kBorder);

	const float labelX = pad * 2.0f;
	const float gaugeX = width * 0.40f;
	const float gaugeW = width * 0.28f;
	float y = pad;

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
	m_font->Draw(batch, std::format("FPS {:.0f}", m.fps), gaugeX, y, kAccent);
	y += line;

	m_font->Draw(batch, std::format("CPU  {:.0f}%", m.cpuPercent), labelX, y, kText);
	gauge(y, m.cpuPercent / 100.0f, {0.45f, 0.70f, 0.95f, 1.0f});
	y += line;

	if (m.gpuPercent >= 0.0f) {
		m_font->Draw(batch, std::format("GPU  {:.0f}%", m.gpuPercent), labelX, y, kText);
		gauge(y, m.gpuPercent / 100.0f, {0.55f, 0.85f, 0.55f, 1.0f});
	} else {
		m_font->Draw(batch, "GPU  n/a", labelX, y, kDim);
	}
	y += line;

	const float sysFrac = m.sysMemTotalMB > 0
							   ? static_cast<float>(m.sysMemUsedMB / m.sysMemTotalMB)
							   : 0.0f;
	m_font->Draw(batch,
				std::format("RAM  {:.1f} / {:.1f} GB", m.sysMemUsedMB / 1024.0,
							m.sysMemTotalMB / 1024.0),
				labelX, y, kText);
	gauge(y, sysFrac, {0.85f, 0.70f, 0.40f, 1.0f});
	y += line;

	const float vramFrac = gpuBudgetGB > 0 ? static_cast<float>(gpuUsedGB / gpuBudgetGB) : 0.0f;
	m_font->Draw(batch, std::format("VRAM {:.2f} / {:.2f} GB", gpuUsedGB, gpuBudgetGB),
				labelX, y, kText);
	gauge(y, vramFrac, {0.80f, 0.55f, 0.85f, 1.0f});
	y += line;

	// Descriptor slots: a FIXED ceiling, unlike the two gauges above, so the
	// gauge is against capacity and the peak rides along — a number that climbs
	// and never comes back down is the shape of a leak.
	const float srvFrac = static_cast<float>(device.SrvLive()) /
						  static_cast<float>(gfx::GraphicsDevice::SrvCapacity());
	m_font->Draw(batch,
				std::format("SRV  {} / {} (peak {})", device.SrvLive(),
							gfx::GraphicsDevice::SrvCapacity(), device.SrvHighWater()),
				labelX, y, srvFrac > 0.9f ? kWarn : kText);
	gauge(y, srvFrac, {0.60f, 0.75f, 0.90f, 1.0f});
	y += line;

	row(std::format("Process working set: {:.0f} MB", m.procMemMB));
	row("GPU: " + device.AdapterName());

	// --- threads panel (top, below perf) ------------------------------------
	// One row per managed worker (Core/ThreadManager.h) with live stats and four
	// clickable controls. m_threadHits records the button rects for next frame's
	// click hit-testing (Update), so the layout lives in exactly one place.
	m_threadHits.clear();
	if (!workers.empty()) {
		float ty = threadsTop;
		batch.DrawRect({0, ty, width, 1.0f}, kBorder); // divider from the perf block
		ty += line * 0.4f;
		m_font->Draw(batch, "THREADS", labelX, ty, kAccent);
		const float gov = m_threadMgr.GlobalThrottle();
		if (gov != 1.0f)
			m_font->Draw(batch, std::format("governor {:.2f}x", gov), width * 0.15f, ty,
						{0.55f, 0.85f, 0.95f, 1.0f});
		ty += line;

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

	// --- profile panel (top, below threads) ---------------------------------
	// One tree per measured thread, deepest-first indentation, with a bar giving
	// each node's share of what its thread recorded this period. The numbers are
	// the LAST PUBLISHED period — a frame for the main thread, a tick for a
	// worker — so this is a live readout rather than a running total.
	if (m_showProfile) {
		float py = threadsTop + threadsBlock;
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
			py += line;

			const float indent = m_font->MeasureWidth("  ");
			const float barX = width * 0.62f;
			const float barW = width * 0.26f;
			for (int i = 0; i < profRowCount; ++i) {
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
						batch.DrawRect({barX, oy, barW, bh}, kGaugeBg);
						batch.DrawRect(
							{barX, oy, barW * std::clamp(pr.frac, 0.0f, 1.0f), bh},
							{0.45f, 0.70f, 0.95f, 1.0f});
					}
				}
				py += rowAdvance;
			}
			if (profRowCount == kMaxProfRows)
				m_font->Draw(batch, "(truncated)", labelX, py, kDim);
		} else {
			py += line;
			m_font->Draw(batch, "not compiled in - build debug-profile or release-profile",
						labelX, py, kDim);
		}
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
