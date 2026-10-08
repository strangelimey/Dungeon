// ============================================================================
// Game/DevConsole_Threads.cpp - the console's THREADS section: one row per
// managed worker (Core/ThreadManager.h) with its live stats, its health counts
// and the halt / rate / kill / boot controls.
// ============================================================================
#include "Game/DevConsole_Panel.h"

#include "Core/Diagnostics.h"
#include "UI/Controls.h" // ui::DrawBorder

#include <algorithm>
#include <format>

namespace dungeon::game {

using namespace devcon;

float DevConsole::ThreadsSectionHeight(const PanelCtx& p,
									   const std::vector<threads::WorkerInfo>& workers) const {
	const float line = p.line, rowAdvance = p.rowAdvance;
	const float threadsBlock =
		workers.empty()     ? 0.0f
		: m_threadsExpanded ? line * 1.4f + rowAdvance * static_cast<float>(workers.size())
							: line * 1.4f;
	return threadsBlock;
}

// --- threads panel (bottom, collapsed by default) ------------------------
// One row per managed worker (Core/ThreadManager.h) with live stats and four
// clickable controls. m_threadHits records the button rects for next frame's
// click hit-testing (Update), so the layout lives in exactly one place.
void DevConsole::DrawThreadsSection(const PanelCtx& p, float top,
									const std::vector<threads::WorkerInfo>& workers) {
	gfx::SpriteBatch& batch = p.batch;
	const float width = p.width, line = p.line, pad = p.pad, labelX = p.labelX;
	const float rowAdvance = p.rowAdvance;

	m_threadHits.clear();
	m_threadsBtn = {};
	if (!workers.empty()) {
		float ty = top + p.sy;
		batch.DrawRect({0, ty, width, 1.0f}, kBorder); // divider from the profile block
		ty += line * 0.4f;
		m_font->Draw(batch, "THREADS", labelX, ty, kAccent);
		const float gov = m_threadMgr.GlobalThrottle();
		if (gov != 1.0f)
			m_font->Draw(batch, std::format("governor {:.2f}x", gov), width * 0.15f, ty,
						kHeldColor);

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

		m_threadsBtn = DrawExpander(p, ty, m_threadsExpanded);
		ty += line;
	}
	if (!workers.empty() && m_threadsExpanded) {
		float ty = top + line * 1.4f + p.sy;

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
			// The HEALTH strip's colours (DevConsole_Panel.h): a stalled worker in
			// the stall's amber, a quarantined one in the kill's purple - and a
			// halted one in the held colour, since a halt is a hand on the worker,
			// not a fault in it.
			const Vec4 stCol = quar ? kKilledColor
							 : w.state == threads::State::Dead ? kDim
							 : w.state == threads::State::Stalled ? kStallColor
							 : w.paused ? kHeldColor
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
							 width * 0.635f, ty, bad > 0 ? kExceptionColor : kStallColor);
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
				button(killR, "kill", kDangerColor);
				hit.pause = pauseR;
				hit.slower = slowR;
				hit.faster = fastR;
				hit.kill = killR;
			}
			m_threadHits.push_back(hit);

			ty += rowAdvance;
		}
	}
}

DevConsole::PanelReport DevConsole::ReportPanel() const {
	const float s = m_hitScale > 0.0f ? m_hitScale : 1.0f;
	PanelReport r;
	r.panelH = m_panelH / s;
	r.contentH = m_contentH / s;
	r.threadsExpanded = m_threadsExpanded;
	r.clicksBelowPanel = m_clicksBelowPanel;
	// Rows top to bottom, so the first found is the first under the panel's foot.
	// WHOLLY below it: a row straddling the edge is partly on the panel, and its
	// visible part is a fair click.
	for (const ThreadHit& t : m_threadHits) {
		const threads::WorkerInfo info = m_threadMgr.Inspect(t.id);
		const struct {
			const gfx::Rect& rect;
			const char* name;
		} controls[] = {{t.pause, info.paused ? "run" : "halt"},
						{t.slower, "slower"},
						{t.faster, "faster"},
						{t.kill, "kill"},
						{t.boot, "boot"}};
		for (const auto& c : controls) {
			if (c.rect.w <= 0.0f || c.rect.y < m_panelH) continue;
			r.belowWorker = t.id;
			r.belowControl = c.name;
			r.belowX = (c.rect.x + c.rect.w * 0.5f) / s;
			r.belowY = (c.rect.y + c.rect.h * 0.5f) / s;
			return r;
		}
	}
	return r;
}

void DevConsole::ThreadsClick(float mx, float my) {
	if (m_threadsBtn.Contains(mx, my)) {
		m_threadsExpanded = !m_threadsExpanded;
		NoteSectionsChanged();
	}

	// Thread-panel control buttons (hit-tested against the rects Render laid out
	// last frame). Left-click toggles pause, halves/doubles the rate, or kills.
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

} // namespace dungeon::game
