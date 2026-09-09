// ============================================================================
// Game/WorldMapView.cpp — see WorldMapView.h.
// ============================================================================
#include "Game/WorldMapView.h"

#include "Core/Loc.h"
#include "Game/MapColors.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace dungeon::game {

namespace {

// Unknown ground: drawn, not omitted. A map with fog on it should still read as
// a map — omitting the unseen would make an unexplored world look like a small
// one, and the shape of what you have not been to is half of why you look.
const Vec4 kUnknown{0.07f, 0.07f, 0.09f, 1.0f};
const Vec4 kParty{0.35f, 0.85f, 1.0f, 1.0f};
const Vec4 kLocation{0.95f, 0.80f, 0.35f, 1.0f};
const Vec4 kHover{0.95f, 0.95f, 1.0f, 0.35f};

// How far a caption sits above the panel's bottom edge, in line heights.
constexpr float kCaptionLines = 2.4f;

} // namespace

gfx::Rect WorldMapView::GridArea(const gfx::Rect& panel) const {
	const float line = m_font ? m_font->Height() : 16.0f;
	const float caption = line * kCaptionLines;
	return {panel.x, panel.y, panel.w, panel.h - caption};
}

WorldMapView::Transform WorldMapView::ComputeTransform(const WorldMap& world,
													   const gfx::Rect& panel) const {
	const gfx::Rect g = GridArea(panel);
	const float mw = static_cast<float>(std::max(1, world.Width()));
	const float mh = static_cast<float>(std::max(1, world.Height()));
	const float fit = std::min(g.w / mw, g.h / mh); // the whole world at zoom 1
	const float cell = fit * m_zoom;
	const float gridW = mw * cell, gridH = mh * cell;
	const float ox = g.x + (g.w - gridW) * 0.5f + m_pan.x * g.w;
	const float oy = g.y + (g.h - gridH) * 0.5f + m_pan.y * g.h;
	return {cell, ox, oy};
}

bool WorldMapView::CellAt(float px, float py, const WorldMap& world,
						  const gfx::Rect& panel, int& outX, int& outZ) const {
	const Transform t = ComputeTransform(world, panel);
	if (t.cell <= 0.0f) return false;
	const int x = static_cast<int>(std::floor((px - t.ox) / t.cell));
	const int z = static_cast<int>(std::floor((py - t.oy) / t.cell));
	if (!world.InBounds(x, z)) return false;
	outX = x;
	outZ = z;
	return true;
}

void WorldMapView::Update(const Input& input, const WorldMap& world,
						  const gfx::Rect& panel) {
	// Sized from the panel, like the dungeon map — and set in BOTH Update and
	// Render, because the caption's band height is measured off the font and a
	// Render that ran first would lay the grid out against a stale one.
	SetFontHeight(std::clamp(panel.h * 0.030f, 11.0f, 30.0f));
	const float mx = input.MouseX(), my = input.MouseY();
	const bool over = mx >= panel.x && my >= panel.y && mx < panel.x + panel.w &&
					  my < panel.y + panel.h;

	m_hoverX = m_hoverZ = -1;
	if (over) {
		if (int cx, cz; CellAt(mx, my, world, panel, cx, cz)) {
			m_hoverX = cx;
			m_hoverZ = cz;
		}
	}

	// Cursor-anchored zoom: the cell under the pointer stays under it, so
	// zooming in on somewhere does not also walk the map out from under you.
	if (over && input.WheelDelta() != 0.0f) {
		const Transform before = ComputeTransform(world, panel);
		m_zoom = std::clamp(m_zoom * std::pow(1.2f, input.WheelDelta()), 1.0f, 12.0f);
		const Transform after = ComputeTransform(world, panel);
		if (before.cell > 0.0f && after.cell > 0.0f) {
			const gfx::Rect g = GridArea(panel);
			const float cellX = (mx - before.ox) / before.cell;
			const float cellY = (my - before.oy) / before.cell;
			m_pan.x += (mx - (after.ox + cellX * after.cell)) / g.w;
			m_pan.y += (my - (after.oy + cellY * after.cell)) / g.h;
		}
	}

	if (over && input.WasMousePressed(MouseButton::Right)) {
		m_dragging = true;
		m_dragFrom = {mx, my};
	}
	if (!input.IsMouseDown(MouseButton::Right)) m_dragging = false;
	if (m_dragging) {
		const gfx::Rect g = GridArea(panel);
		m_pan.x += (mx - m_dragFrom.x) / g.w;
		m_pan.y += (my - m_dragFrom.y) / g.h;
		m_dragFrom = {mx, my};
	}
}

void WorldMapView::Render(gfx::SpriteBatch& batch, const ui::Theme& theme,
						  const WorldMap& world, const WorldState& state,
						  const gfx::Rect& panel) {
	SetFontHeight(std::clamp(panel.h * 0.030f, 11.0f, 30.0f));
	const Transform t = ComputeTransform(world, panel);
	const gfx::Rect grid = GridArea(panel);

	batch.DrawRect(panel, kMapBg);
	const gfx::Rect clip{grid.x + 2, grid.y + 2, grid.w - 4, grid.h - 4};
	batch.SetScissor(&clip);

	const float inset = std::clamp(t.cell * 0.06f, 0.0f, 1.5f);
	auto cellRect = [&](int x, int z) -> gfx::Rect {
		return {t.ox + x * t.cell + inset, t.oy + z * t.cell + inset,
				t.cell - 2 * inset, t.cell - 2 * inset};
	};
	auto cellCenter = [&](int x, int z) -> Vec2 {
		return {t.ox + (x + 0.5f) * t.cell, t.oy + (z + 0.5f) * t.cell};
	};

	for (int z = 0; z < world.Height(); ++z)
		for (int x = 0; x < world.Width(); ++x) {
			const bool seen = state.Seen(x, z);
			batch.DrawRect(cellRect(x, z),
						   seen ? world.TerrainAt(x, z).color : kUnknown);
		}

	// Locations, over the ground: a discovered one is a diamond, and an
	// undiscovered one is nothing at all — that is what discovery MEANS.
	for (const WorldMap::Location& l : world.Locations()) {
		if (!state.Discovered(l.id)) continue;
		const Vec2 c = cellCenter(l.x, l.z);
		const float h = t.cell * 0.34f;
		batch.DrawTriangle({c.x, c.y - h}, {c.x + h, c.y}, {c.x - h, c.y}, kLocation);
		batch.DrawTriangle({c.x, c.y + h}, {c.x + h, c.y}, {c.x - h, c.y}, kLocation);
	}

	if (m_hoverX >= 0) {
		const gfx::Rect r = cellRect(m_hoverX, m_hoverZ);
		const float bw = std::clamp(t.cell * 0.08f, 1.0f, 3.0f);
		batch.DrawRect({r.x - bw, r.y - bw, r.w + 2 * bw, bw}, kHover);
		batch.DrawRect({r.x - bw, r.y + r.h, r.w + 2 * bw, bw}, kHover);
		batch.DrawRect({r.x - bw, r.y, bw, r.h}, kHover);
		batch.DrawRect({r.x + r.w, r.y, bw, r.h}, kHover);
	}

	// The party last, so it is never hidden by what it is standing on.
	{
		const Vec2 c = cellCenter(state.x, state.z);
		const float h = t.cell * 0.30f;
		batch.DrawRect({c.x - h, c.y - h, h * 2, h * 2}, kParty);
	}

	batch.SetScissor(nullptr);

	if (!m_font) return;
	const float line = m_font->Height();
	const float pad = line * 0.4f;
	float y = panel.y + panel.h - line * kCaptionLines + pad * 0.5f;

	// The caption is TWO lines and the split is deliberate: where you are, then
	// what you are pointing at. The second answers "what does that square cost"
	// before you walk into it.
	const WorldMap::Terrain& here = world.TerrainAt(state.x, state.z);
	m_font->Draw(batch,
				 loc::Format("world.caption.here", here.id,
							 std::format("{},{}", state.x, state.z),
							 std::format("{:.1f}", state.time)),
				 panel.x + pad, y, theme.text);
	y += line;

	if (m_hoverX >= 0) {
		const WorldMap::Terrain& over = world.TerrainAt(m_hoverX, m_hoverZ);
		const WorldMap::Location* loc = world.LocationAt(m_hoverX, m_hoverZ);
		const bool known = loc && state.Discovered(loc->id);
		// An impassable square says so instead of quoting a travel time it will
		// never charge — the number would be a lie you could act on.
		m_font->Draw(batch,
					 known ? loc::Format("world.caption.location", loc->id,
										 over.id)
					 : over.passable
						 ? loc::Format("world.caption.over", over.id,
									   std::format("{:.1f}", over.travel),
									   std::format("{:.2f}",
												   world.Difficulty(m_hoverX, m_hoverZ)))
						 : loc::Format("world.caption.blocked", over.id),
					 panel.x + pad, y, theme.textDim);
	} else {
		m_font->Draw(batch, loc::View("world.caption.help"), panel.x + pad, y,
					 theme.textDim);
	}
}

} // namespace dungeon::game
