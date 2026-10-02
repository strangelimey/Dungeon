// ============================================================================
// Game/DevConsole_Perf.cpp - the console's PERFORMANCE section: the gauges at
// the top of the panel (frame rate, CPU, GPU, memory, descriptor slots), as
// bars or as graphs, and the history those graphs draw from.
// ============================================================================
#include "Game/DevConsole_Panel.h"

#include "Core/Profile.h"
#include "UI/Controls.h" // ui::DrawBorder

#include <algorithm>
#include <format>

namespace dungeon::game {

using namespace devcon;

void DevConsole::SamplePerfSeries(const gfx::GraphicsDevice& device) {
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
	bump(kProc, static_cast<float>(m.procMemMB));
}

void DevConsole::CommitPerfSeries() {
	for (PerfSeries& s : m_perfSeries) {
		s.samples[m_profHead] = s.pending;
		s.pending = 0.0f;
	}
}

// Header (the section name and the GPU's), then six gauges (or six graphs in
// three two-column rows). A collapsed section is its header row and nothing else.
// Every section can be reduced to one line, so the panel can be cut down to
// just the thing being watched rather than scrolled past everything else.
float DevConsole::PerfSectionHeight(const PanelCtx& p) const {
	const float line = p.line, graphH = p.graphH, graphGapY = p.graphGapY;
	int perfVisible = 0;
	for (int i = 0; i < kPerfLines; ++i)
		if (!m_perfHidden[i]) ++perfVisible;
	const int perfHiddenCount = kPerfLines - perfVisible;
	const int perfGraphRows = (perfVisible + 1) / 2;
	const float perfBody =
		!m_perfExpanded ? 0.0f
		: m_perfGraph   ? static_cast<float>(perfGraphRows) * (graphH + graphGapY) +
							  static_cast<float>(perfHiddenCount) * line
						: p.rowAdvance * static_cast<float>(kPerfLines);
	// Air between the header's buttons and the first row beneath them, the
	// THREADS header's gap, so the two sections read alike.
	const float headerGap = m_perfExpanded ? line * 0.4f : 0.0f;
	return line + headerGap + perfBody;
}

// --- performance panel (top) ------------------------------------------------
void DevConsole::DrawPerfSection(const PanelCtx& p, float top) {
	gfx::SpriteBatch& batch = p.batch;
	const gfx::GraphicsDevice& device = p.device;
	const float width = p.width, line = p.line, pad = p.pad, labelX = p.labelX;
	const float graphH = p.graphH, graphGapY = p.graphGapY, panelH = p.panelH;

	const PerfMonitor::Metrics& m = m_perf.Get();
	const gfx::GraphicsDevice::GpuMemoryInfo vram = device.QueryGpuMemory();
	const double gpuUsedGB = static_cast<double>(vram.usedBytes) / (1024.0 * 1024.0 * 1024.0);
	const double gpuBudgetGB = static_cast<double>(vram.budgetBytes) / (1024.0 * 1024.0 * 1024.0);
	// The same count PerfSectionHeight made, so the graph grid fills exactly the
	// rows the layout gave it.
	int perfVisible = 0;
	for (bool hidden : m_perfHidden)
		if (!hidden) ++perfVisible;
	const int perfGraphRows = (perfVisible + 1) / 2;
	float y = top + p.sy;

	// A gauge may carry a SUBSET drawn first, from the left, in its own colour:
	// the rest of the bar is then the part of the whole that is not the subset.
	auto gauge = [&](float gx, float gw, float gy, float frac, const Vec4& fill,
					 float subFrac, const Vec4& subFill) {
		const float gh = line * 0.7f;
		const float oy = gy + (line - gh) * 0.5f;
		const float whole = std::clamp(frac, 0.0f, 1.0f);
		const float sub = std::clamp(subFrac, 0.0f, whole);
		batch.DrawRect({gx, oy, gw, gh}, kGaugeBg);
		batch.DrawRect({gx, oy, gw * whole, gh}, fill);
		if (sub > 0.0f) batch.DrawRect({gx, oy, gw * sub, gh}, subFill);
		ui::DrawBorder(batch, {gx, oy, gw, gh}, kBorder);
	};
	// The title names the GPU it is measuring beside the section's own name - the
	// one fact about the machine that changes how every number below it reads.
	const char* kTitle = "PERFORMANCE   ";
	m_font->Draw(batch, kTitle, labelX, y, kAccent);
	const float gpuX = labelX + m_font->MeasureWidth(kTitle);
	m_font->Draw(batch, device.AdapterName(), gpuX, y, kText);
	const float titleEnd = gpuX + m_font->MeasureWidth(device.AdapterName());
	m_perfExpandBtn = DrawExpander(p, y, m_perfExpanded);
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
		// Collapsed, the header still answers the headline question - after the
		// title, which is a GPU name and so has no fixed length.
		m_font->Draw(batch,
					std::format("FPS {:.0f}   CPU {:.0f}%   GPU {:.0f}%", m.fps,
								m.cpuPercent, m.gpuPercent >= 0.0f ? m.gpuPercent : 0.0f),
					std::max(width * 0.25f, titleEnd + line * 2.0f),
					y, kDim);
	}
	y += line;
	if (m_perfExpanded) y += line * 0.4f; // the header gap PerfSectionHeight counts

	// The six gauges as ONE table, so the bar view and the graph view cannot
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
		// An optional SUBSET of the value, stacked inside the same gauge against
		// the same scale. Its text follows the main text in its own colour, so the
		// label doubles as the bar's legend.
		std::string subText = {};
		float subValue = 0.0f;
		Vec4 subColor = {};
		int subSeries = -1;
	};
	auto labelWidth = [&](const PerfItem& it) {
		return m_font->MeasureWidth(it.text) + m_font->MeasureWidth(it.subText);
	};
	// A dimmed label (a hidden graph's line) dims its subset text with it.
	auto drawLabel = [&](const PerfItem& it, float x, float ly, const Vec4& col, bool dim) {
		m_font->Draw(batch, it.text, x, ly, col);
		if (!it.subText.empty())
			m_font->Draw(batch, it.subText, x + m_font->MeasureWidth(it.text), ly,
						dim ? kDim : it.subColor);
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
		// This process's working set rides INSIDE the system RAM bar: it is always
		// a subset of the memory in use, so stacking it against the same ceiling
		// shows the game's share directly, and the rest of the bar is everything
		// else that is running.
		{"RAM",
		 std::format("RAM  {:.1f} / {:.1f} GB", m.sysMemUsedMB / 1024.0,
					 m.sysMemTotalMB / 1024.0),
		 static_cast<float>(m.sysMemUsedMB), static_cast<float>(sysTotalMB),
		 {0.85f, 0.70f, 0.40f, 1.0f}, false,
		 std::format("  working set {:.2f}", m.procMemMB / 1024.0),
		 static_cast<float>(m.procMemMB), {0.95f, 0.42f, 0.28f, 1.0f}, kProc},
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
		// Labels are RIGHT-aligned against the bars, so each line of text ends
		// beside the bar it describes instead of trailing off at a different
		// length above a column of bars that all start at one x. The column is as
		// wide as the widest label, but never narrower than the widest the SRV
		// line can get, so it does not twitch as digits come and go.
		float labelW = m_font->MeasureWidth("SRV  1024 / 1024 (peak 1024)");
		for (const PerfItem& it : items)
			labelW = std::max(labelW, labelWidth(it));
		const float labelRight = labelX + labelW;
		// A clear gap either side, in line heights so it tracks the font: enough
		// that the bars read as their own column rather than running edge to edge.
		const float barX = labelRight + line * 2.0f;
		const float barW = std::max(width - line * 6.0f - barX, 0.0f);
		for (int oi = 0; oi < kPerfLines; ++oi) {
			const int i = kPerfOrder[oi];
			const PerfItem& it = items[i];
			const bool unavailable = i == kGpu && m.gpuPercent < 0.0f;
			drawLabel(it, labelRight - labelWidth(it), y,
					  it.warn ? kWarn : unavailable ? kDim : kText, unavailable);
			if (!unavailable)
				gauge(barX, barW, y, it.value / it.scale, it.color, it.subValue / it.scale,
					  it.subColor);
			y += p.rowAdvance;
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
			const float cw = DrawCheckbox(p, gx, gy, true, i, 0, 0);
			drawLabel(it, gx + cw, gy, it.warn ? kWarn : kText, false);
			const gfx::Rect plot{gx, gy + line, pgw, graphH - line};
			DrawSeriesGraph(batch, plot, m_perfSeries[i].samples, kProfHistory, m_profHead,
							it.scale, it.color);
			// The subset's band over the whole's, on the same scale - since it never
			// exceeds the whole, the two fills read as a stack.
			if (it.subSeries >= 0)
				DrawSeriesGraph(batch, plot, m_perfSeries[it.subSeries].samples, kProfHistory,
								m_profHead, it.scale, it.subColor, false, true);
		}
		y += static_cast<float>(perfGraphRows) * (graphH + graphGapY);

		// The hidden ones, one line each, still checkable — in the same display
		// order, so a graph reappears where you would look for it.
		for (int oi = 0; oi < kPerfLines; ++oi) {
			const int i = kPerfOrder[oi];
			if (!m_perfHidden[i]) continue;
			const float cw = DrawCheckbox(p, pad * 2.0f, y, false, i, 0, 0);
			drawLabel(items[i], pad * 2.0f + cw, y, kDim, true);
			y += line;
		}
	}
}

void DevConsole::PerfClick(float mx, float my) {
	if (m_perfExpandBtn.Contains(mx, my)) {
		m_perfExpanded = !m_perfExpanded;
		NoteSectionsChanged();
	}
	if (m_perfViewBtn.Contains(mx, my)) m_perfGraph = !m_perfGraph;
}

} // namespace dungeon::game
