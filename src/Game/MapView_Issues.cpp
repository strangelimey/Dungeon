// ============================================================================
// Game/MapView_Issues.cpp - live validation on the editor's map
// (docs/editor-updates-plan.md, P2). Split out of MapView.cpp by concern.
//
// Game re-runs the checker when an edit ends (Game::RefreshLiveIssues) and
// hands the findings here. This file only SHOWS them:
//   - a box on every square of the viewed level a finding names, red for an
//     error and amber for a warning (red wins where both land);
//   - on hover, what is wrong there, word-wrapped beside the square;
//   - a badge on the Check disc counting the findings that have no square
//     at all (a level or the world as a whole), which only the list can show.
// A stair fault is a PAIR, so its far end is boxed too (Issue::also), on
// whichever level that is - the map shows one level at a time.
//
// Editor frames are not in the allocation guard (Game::SteadyStateFrame
// needs the map closed), so the tooltip's strings are ordinary strings.
// ============================================================================
#include "Game/MapView.h"

#include "Core/Loc.h"
#include "Game/MapColors.h"
#include "UI/Controls.h" // ui::DrawBorder
#include "UI/TextWrap.h"

#include <algorithm>
#include <format>
#include <utility>

namespace dungeon::game {

namespace {
using validate::Issue;
using validate::Severity;

Vec4 Ink(Severity s) { return s == Severity::Error ? kIssueError : kIssueWarning; }
} // namespace

void MapView::IssuesAt(int x, int z, std::vector<const Issue*>& out) const {
	if (!m_issues || x < 0 || z < 0) return;
	const std::string& here = ViewedLevel();
	for (const Issue& is : *m_issues) {
		bool hit = is.level == here && is.x == x && is.z == z;
		for (const validate::Spot& s : is.also)
			if (s.level == here && s.x == x && s.z == z) hit = true;
		if (hit) out.push_back(&is);
	}
}

void MapView::RenderIssueBoxes(gfx::SpriteBatch& batch, const gfx::Rect& panel) const {
	if (m_mode != Mode::Editor || !m_issues || m_issues->empty()) return;
	const DungeonMap& map = ViewedMap();
	const int w = map.Width(), h = map.Height();
	if (w <= 0 || h <= 0) return;
	// Worst severity per square first (0 none, 1 warning, 2 error), then one
	// box each - so a square named by an error and a warning draws once, red.
	std::vector<unsigned char> worst(static_cast<size_t>(w) * h, 0);
	const std::string& here = ViewedLevel();
	const auto mark = [&](int x, int z, Severity s) {
		if (x < 0 || z < 0 || x >= w || z >= h) return;
		unsigned char& m = worst[static_cast<size_t>(z) * w + x];
		m = std::max<unsigned char>(m, s == Severity::Error ? 2 : 1);
	};
	for (const Issue& is : *m_issues) {
		if (is.level == here) mark(is.x, is.z, is.severity);
		for (const validate::Spot& s : is.also)
			if (s.level == here) mark(s.x, s.z, is.severity);
	}
	const Transform t = ComputeTransform(panel);
	// INSIDE the square, so the hover and selection rings (drawn just outside
	// it, over this) stay readable on a boxed square.
	const float bw = std::clamp(t.cell * 0.07f, 1.5f, 4.0f);
	for (int z = 0; z < h; ++z)
		for (int x = 0; x < w; ++x) {
			const unsigned char m = worst[static_cast<size_t>(z) * w + x];
			if (m == 0) continue;
			const Vec4 ink = Ink(m == 2 ? Severity::Error : Severity::Warning);
			const gfx::Rect r{t.ox + x * t.cell, t.oy + z * t.cell, t.cell, t.cell};
			batch.DrawRect(r, {ink.x, ink.y, ink.z, 0.22f});
			batch.DrawRect({r.x, r.y, r.w, bw}, ink);
			batch.DrawRect({r.x, r.y + r.h - bw, r.w, bw}, ink);
			batch.DrawRect({r.x, r.y + bw, bw, r.h - 2 * bw}, ink);
			batch.DrawRect({r.x + r.w - bw, r.y + bw, bw, r.h - 2 * bw}, ink);
		}
}

void MapView::RenderIssueTooltip(gfx::SpriteBatch& batch, const ui::Theme& theme,
								 const gfx::Rect& panel) {
	const bool fresh = std::exchange(m_updatedSinceRender, false);
	if (!fresh || m_mode != Mode::Editor || m_hoverX < 0 || !m_issues) return;
	std::vector<const Issue*> here;
	IssuesAt(m_hoverX, m_hoverZ, here);
	if (here.empty()) return;

	// The text: each finding's message, and for a square that is the FAR end of
	// a stair fault, where the stair it belongs to stands.
	const std::string& viewed = ViewedLevel();
	std::vector<std::pair<Severity, std::string>> lines;
	for (const Issue* is : here) {
		std::string text = loc::Format(is->messageKey, is->a, is->b);
		if (is->level != viewed)
			text = loc::Format("map.check.farend", is->level, is->x, is->z) + " " + text;
		lines.push_back({is->severity, std::move(text)});
	}

	const ui::Font& font = *m_font;
	const float pad = DockPad(panel) * 1.5f;
	const float lineH = font.Height();
	const gfx::Rect grid = GridArea(panel);
	const float maxW = std::min(grid.w * 0.45f, lineH * 22.0f);
	const float mark = lineH * 0.5f; // the severity swatch before each finding
	// Measure with the same wrap the draw uses (ui::WrapLines), so the box is
	// the height of what it holds.
	float textW = 0.0f;
	int rows = 0;
	for (const auto& [sev, text] : lines)
		rows += ui::WrapLines(font, text, maxW, [&](std::string_view l, int) {
			textW = std::max(textW, font.MeasureWidth(l));
		});
	const float gap = lineH * 0.35f * static_cast<float>(lines.size() - 1);
	const float boxW = pad * 3 + mark + textW;
	const float boxH = pad * 2 + lineH * static_cast<float>(rows) + gap;

	// Placed like the hand-slot tooltips: never over the thing it explains -
	// below the square by preference, above if it would run off the bottom,
	// and pulled back inside the panel at the sides.
	const Transform t = ComputeTransform(panel);
	const float cx = t.ox + m_hoverX * t.cell, cy = t.oy + m_hoverZ * t.cell;
	float bx = cx, by = cy + t.cell + pad;
	if (by + boxH > panel.y + panel.h - 2.0f) by = cy - pad - boxH;
	bx = std::clamp(bx, panel.x + 2.0f, panel.x + panel.w - boxW - 2.0f);
	by = std::max(by, panel.y + 2.0f);
	const gfx::Rect box{bx, by, boxW, boxH};
	batch.DrawRect(box, {0.10f, 0.10f, 0.13f, 0.97f});
	ui::DrawBorder(batch, box, theme.panelBorder);

	float y = by + pad;
	for (const auto& [sev, text] : lines) {
		batch.DrawRect({bx + pad, y + (lineH - mark) * 0.5f, mark, mark}, Ink(sev));
		const float x = bx + pad * 2 + mark;
		const int n = ui::WrapLines(font, text, maxW, [&](std::string_view l, int i) {
			font.Draw(batch, l, x, y + lineH * static_cast<float>(i), theme.text);
		});
		y += lineH * static_cast<float>(n) + lineH * 0.35f;
	}
}

void MapView::RenderCheckBadge(gfx::SpriteBatch& batch, const gfx::Rect& disc) const {
	if (!m_issues) return;
	int count = 0;
	bool error = false;
	for (const Issue& is : *m_issues)
		if (is.x < 0) {
			++count;
			error = error || is.severity == Severity::Error;
		}
	if (count == 0) return;
	const std::string n = count > 99 ? "99+" : std::to_string(count);
	const ui::Font& font = *m_font;
	const float h = std::max(font.Height() * 0.9f, disc.h * 0.42f);
	const float w = std::max(h, font.MeasureWidth(n) + h * 0.5f);
	// On the disc's top-right shoulder, the usual place for a count.
	const gfx::Rect r{disc.x + disc.w - w * 0.7f, disc.y - h * 0.25f, w, h};
	const Vec4 ink = Ink(error ? Severity::Error : Severity::Warning);
	batch.DrawRect(r, ink);
	ui::DrawBorder(batch, r, {0.0f, 0.0f, 0.0f, 0.6f});
	font.Draw(batch, n, r.x + (r.w - font.MeasureWidth(n)) * 0.5f,
			  r.y + (r.h - font.Height()) * 0.5f, {0.08f, 0.06f, 0.05f, 1.0f});
}

} // namespace dungeon::game
