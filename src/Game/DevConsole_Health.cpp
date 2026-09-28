// ============================================================================
// Game/DevConsole_Health.cpp - the console's HEALTH section: a strip per
// failing thread on the profile graphs' x-axis, marking when it threw, stalled
// or was rebooted, with a click-through to the event and its stack.
// ============================================================================
#include "Game/DevConsole_Panel.h"

#include "Core/Diagnostics.h"
#include "Core/StackTrace.h"

#include <algorithm>
#include <cstring> // memcpy
#include <format>

namespace dungeon::game {

using namespace devcon;

namespace {
// One strip per failing thread, a little taller than a line so neighbouring
// strips' marks do not run into each other.
float HealthRowH(float line) { return line * 1.3f; }
} // namespace

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

// HEALTH sits between PROFILE and THREADS, and only exists once something has
// gone wrong - a permanent empty strip would be a section that says nothing 99%
// of the time and trains you to skip past it on the 1%.
float DevConsole::HealthSectionHeight(const PanelCtx& p) const {
	const float line = p.line;
	const float healthRowH = HealthRowH(line);
	const float healthBlock =
		m_healthRowCount == 0 ? 0.0f
		: m_healthExpanded
			? line * 1.4f + healthRowH * static_cast<float>(m_healthRowCount) + line * 0.8f
			: line * 1.4f;
	return healthBlock;
}

// --- HEALTH: when each thread went wrong, on the profile graphs' x-axis ---
// The strip answers a question no instantaneous readout can: a worker that
// threw and recovered reads perfectly normal a second later, so "is it well"
// and "has it been well" are different questions and only this one answers
// the second.
void DevConsole::DrawHealthSection(const PanelCtx& p, float top) {
	gfx::SpriteBatch& batch = p.batch;
	const float width = p.width, line = p.line, labelX = p.labelX;
	const float healthRowH = HealthRowH(line);

	m_healthHits.clear();
	m_healthBtn = {};
	if (m_healthRowCount > 0) {
		float hy = top + p.sy;
		batch.DrawRect({0, hy, width, 1.0f}, kBorder);
		hy += line * 0.4f;
		m_font->Draw(batch, "HEALTH", labelX, hy, kAccent);

		const diag::Totals t = diag::ProcessTotals();
		m_font->Draw(batch,
					 std::format("{} events · {}s of history · click a mark for details",
								 t.total,
								 static_cast<int>(kProfHistory * kProfSampleSec)),
					 width * 0.15f, hy, kDim);
		m_healthBtn = DrawExpander(p, hy, m_healthExpanded);
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
}

void DevConsole::HealthClick(float mx, float my) {
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
}

} // namespace dungeon::game
