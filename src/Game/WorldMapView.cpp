// ============================================================================
// Game/WorldMapView.cpp — see WorldMapView.h.
// ============================================================================
#include "Game/WorldMapView.h"

#include "Core/Loc.h"
#include "Game/AssetUtil.h" // ToolbarIcon — the shared disc cache
#include "Game/MapColors.h"
#include "UI/Controls.h" // DrawButtonFace, DrawBorder

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

// The toolbar band, sized off the panel exactly as the dungeon editor's is: a
// square icon disc with air around it.
float ToolPad(const gfx::Rect& p) { return std::clamp(p.h * 0.010f, 3.0f, 9.0f); }
float ToolSide(const gfx::Rect& p) { return std::clamp(p.h * 0.042f, 16.0f, 40.0f); }

} // namespace

WorldMapView::WorldMapView(gfx::GraphicsDevice& device, ui::FontLibrary& fonts)
	: m_fonts(fonts) {
	m_icoSettings = ToolbarIcon(device, "level"); // the world's own settings
	m_icoSave = ToolbarIcon(device, "save");
	m_icoUndo = ToolbarIcon(device, "undo");
	m_icoRedo = ToolbarIcon(device, "redo");
	m_icoWorlds = ToolbarIcon(device, "worlds");
}

gfx::Rect WorldMapView::ToolbarRect(const gfx::Rect& panel) const {
	if (m_mode != Mode::Editor) return {panel.x, panel.y, panel.w, 0.0f};
	return {panel.x, panel.y, panel.w, ToolSide(panel) + ToolPad(panel) * 4};
}

std::vector<WorldMapView::ToolButton> WorldMapView::ToolbarButtons(
	const gfx::Rect& panel) const {
	std::vector<ToolButton> btns;
	if (m_mode != Mode::Editor) return btns;
	const gfx::Rect tb = ToolbarRect(panel);
	const float pad = ToolPad(panel), s = ToolSide(panel);
	// Built right-to-left from the band's right edge, like the level editor's,
	// so the two bands read as the same furniture one tier apart.
	float right = tb.x + tb.w - pad * 2;
	auto add = [&](Tool id, std::string label, const gfx::Texture* icon,
				   bool enabled) {
		right -= s;
		btns.push_back({id, {right, tb.y + pad * 2, s, s}, std::move(label), icon,
						enabled});
		right -= pad;
	};
	add(Tool::Save, loc::Tr("map.btn.save"), m_icoSave, true);
	add(Tool::Redo, loc::Tr("map.btn.redo"), m_icoRedo,
		canUndo && canUndo(/*redo*/ true));
	add(Tool::Undo, loc::Tr("map.btn.undo"), m_icoUndo,
		canUndo && canUndo(/*redo*/ false));
	add(Tool::Settings, loc::Tr("map.btn.world"), m_icoSettings, true);
	// Leftmost, and apart from the rest in meaning: every other disc acts on
	// THIS world, and this one is the way to the others.
	add(Tool::Worlds, loc::Tr("map.btn.worlds"), m_icoWorlds, true);
	return btns;
}

gfx::Rect WorldMapView::DungeonButton(const gfx::Rect& panel) const {
	// The SAME PIXELS MapView's way here occupies — neither view has a left
	// dock in the player's map, so top-left is the one corner both can agree
	// on, and the toggle stays put instead of jumping across the panel.
	const gfx::Rect g = GridArea(panel);
	const float pad = ToolPad(panel), s = ToolSide(panel);
	return {g.x + pad * 2, g.y + pad * 2, s * 3.0f, s};
}

gfx::Rect WorldMapView::GridArea(const gfx::Rect& panel) const {
	const float line = m_font ? m_font->Height() : 16.0f;
	const float caption = line * kCaptionLines;
	const float top = ToolbarRect(panel).h;
	return {panel.x, panel.y + top, panel.w, panel.h - caption - top};
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
	const bool inPanel = mx >= panel.x && my >= panel.y &&
						 mx < panel.x + panel.w && my < panel.y + panel.h;

	// THE BAND CLAIMS ITS OWN PIXELS FIRST. A zoomed-in grid runs under the
	// toolbar, so a point in the band still maps to a cell — and a click that
	// both pressed a tool and painted the square behind it would be one
	// gesture doing two things.
	m_hoverTool = Tool::None;
	const gfx::Rect band = ToolbarRect(panel);
	const bool inBand = band.h > 0.0f && band.Contains(mx, my);
	if (inBand) {
		for (const ToolButton& b : ToolbarButtons(panel))
			if (b.enabled && b.rect.Contains(mx, my)) m_hoverTool = b.id;
		if (input.WasMousePressed(MouseButton::Left) && m_hoverTool != Tool::None &&
			onTool) {
			const Tool clicked = m_hoverTool;
			// Drop the hover BEFORE firing. A tool that opens a modal takes the
			// input with it, so this Update stops running — and the tooltip,
			// which is only ever cleared here, would hang over the dialog.
			m_hoverTool = Tool::None;
			onTool(clicked);
		}
	}
	// The overlay's way back to the dungeon map claims its own pixels too, for
	// the reason the band does: the grid runs under it.
	m_hoverDungeon = false;
	bool onDungeonBtn = false;
	if (ShowDungeonButton() && DungeonButton(panel).Contains(mx, my)) {
		m_hoverDungeon = true;
		onDungeonBtn = true;
		if (input.WasMousePressed(MouseButton::Left)) {
			m_hoverDungeon = false; // it is about to be replaced by another view
			onShowDungeon();
			return;
		}
	}
	const bool over = inPanel && !inBand && !onDungeonBtn;

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

	// --- painting (Editor mode) ---------------------------------------------
	// LEFT paints the armed terrain, and a DRAG keeps painting — the owner
	// brackets the whole stroke as one undo step, the same bargain the dungeon
	// editor makes. Nothing armed means a click does nothing, rather than
	// meaning "paint the first terrain".
	if (Editing() && !m_armed.empty() && onPaint) {
		if (over && input.WasMousePressed(MouseButton::Left)) m_painting = true;
		if (!input.IsMouseDown(MouseButton::Left)) m_painting = false;
		if (m_painting && m_hoverX >= 0) onPaint(m_hoverX, m_hoverZ, m_armed);
	} else {
		m_painting = false;
	}
	// RIGHT-CLICK INSPECTS rather than pans, in Editor mode — a stationary
	// click, so a right-DRAG still pans. The same gesture split the dungeon
	// editor uses, and for the same reason: panning is too useful to give up.
	if (Editing() && over && input.WasMousePressed(MouseButton::Right)) {
		m_rightFrom = {mx, my};
		m_rightDown = true;
	}
	if (m_rightDown && !input.IsMouseDown(MouseButton::Right)) {
		m_rightDown = false;
		const float moved = std::abs(mx - m_rightFrom.x) + std::abs(my - m_rightFrom.y);
		if (moved <= 3.0f && m_hoverX >= 0 && onInspect) onInspect(m_hoverX, m_hoverZ);
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
	// As the player's OVERLAY it needs the frame the dungeon map has, or the
	// two pages of one control read as two different kinds of screen. The
	// travel screen owns the whole window and has no edge to draw.
	if (m_overlay) ui::DrawBorder(batch, panel, theme.panelBorder);
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

	// FOG IS THE MODE'S DIFFERENCE. Editing draws the whole world: you cannot
	// place what you cannot see, and an editor that hid ground behind the
	// party's ignorance would be unusable for the one job it has.
	const bool showAll = Editing();
	for (int z = 0; z < world.Height(); ++z)
		for (int x = 0; x < world.Width(); ++x) {
			const bool seen = showAll || state.Seen(x, z);
			batch.DrawRect(cellRect(x, z),
						   seen ? world.TerrainAt(x, z).color : kUnknown);
		}

	// Locations, over the ground: a discovered one is a diamond, and an
	// undiscovered one is nothing at all — that is what discovery MEANS.
	for (const WorldMap::Location& l : world.Locations()) {
		if (!showAll && !state.Discovered(l.id)) continue;
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

	// The party last, so it is never hidden by what it is standing on — and
	// SMALLER than the location diamond, so the reverse cannot happen either.
	// A party parked on a dungeon entrance is the single most likely thing to
	// be looking at, and at 0.30 the square covered the diamond almost exactly.
	{
		const Vec2 c = cellCenter(state.x, state.z);
		const float h = t.cell * 0.18f;
		batch.DrawRect({c.x - h, c.y - h, h * 2, h * 2}, kParty);
	}

	batch.SetScissor(nullptr);

	if (!m_font) return;
	// The overlay's way back to the dungeon map. Drawn before the band so the
	// Editor's toolbar wins the corner if both were ever up at once — they
	// cannot be today (the overlay is Play mode), and a silent overlap would
	// be worse than a stated precedence.
	if (ShowDungeonButton())
		ui::DrawButtonFace(batch, *m_font, DungeonButton(panel),
						   loc::Tr("map.btn.showdungeon"), theme, m_hoverDungeon,
						   /*held*/ false, /*enabled*/ true);
	// The Editor band, across the panel top: a subtle lift over the base plus a
	// 1px seam, so it reads as fixed chrome the grid scrolls under — the same
	// treatment (and the same drawing) the level editor's toolbar has.
	if (Editing()) {
		const gfx::Rect tb = ToolbarRect(panel);
		const float pad = ToolPad(panel);
		batch.DrawRect(tb, {1.0f, 1.0f, 1.0f, 0.045f});
		batch.DrawRect({tb.x, tb.y + tb.h - 1.0f, tb.w, 1.0f}, theme.panelBorder);

		// The list is held in a NAMED local, not iterated as a temporary: the
		// tooltip below points INTO it, and a range-for over the returned
		// vector keeps it alive only until the loop ends — after which `tip`
		// named freed memory and the first hover faulted in MeasureWidth.
		const std::vector<ToolButton> btns = ToolbarButtons(panel);
		const ToolButton* tip = nullptr;
		for (const ToolButton& b : btns) {
			// Hover is matched by IDENTITY (Update tracked which tool, not
			// where): Update runs in window pixels and this in device pixels,
			// and a coordinate carried across that split is a bug waiting for
			// a scaled display.
			const bool hot = b.enabled && m_hoverTool == b.id;
			if (hot) tip = &b;
			if (b.icon) {
				const float d = std::min(b.rect.w, b.rect.h);
				const float f = !b.enabled ? 0.32f : hot ? 1.15f : 0.85f;
				batch.DrawSpriteRotated(
					{b.rect.x + b.rect.w * 0.5f, b.rect.y + b.rect.h * 0.5f},
					{d, d}, 0.0f, {0.0f, 0.0f, 1.0f, 1.0f}, *b.icon, {f, f, f, 1.0f});
			} else {
				// No art: the LABEL is written for the tooltip and is far wider
				// than a disc, so trim it to what the button can hold — the
				// full name is still one hover away.
				std::string fit = b.label;
				while (fit.size() > 1 && m_font->MeasureWidth(fit) > b.rect.w - pad)
					fit.pop_back();
				ui::DrawButtonFace(batch, *m_font, b.rect, fit, theme, hot,
								   /*held*/ false, b.enabled);
			}
		}
		if (tip) { // the hovered tool's name, just under the band
			const float tw = m_font->MeasureWidth(tip->label);
			const float pad2 = pad * 1.5f;
			gfx::Rect tr{tip->rect.x + tip->rect.w * 0.5f - tw * 0.5f - pad2,
						 tb.y + tb.h + 2.0f, tw + pad2 * 2, m_font->Height() + pad2};
			tr.x = std::clamp(tr.x, panel.x + 2.0f, panel.x + panel.w - tr.w - 2.0f);
			batch.DrawRect(tr, kMapBg);
			ui::DrawBorder(batch, tr, theme.panelBorder);
			m_font->Draw(batch, tip->label, tr.x + pad2, tr.y + pad2 * 0.5f,
						 theme.text);
		}
	}

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
		// The travel screen's hint says you can WALK; the overlay's must not,
		// because out of it you cannot — the movement keys are the party's,
		// down in the dungeon, and a hint that promises otherwise is a lie the
		// player has to try before disbelieving.
		m_font->Draw(batch,
					 loc::View(m_overlay ? "map.hint" : "world.caption.help"),
					 panel.x + pad, y,
					 theme.textDim);
	}
}

} // namespace dungeon::game
