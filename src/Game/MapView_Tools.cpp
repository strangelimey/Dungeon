// ============================================================================
// Game/MapView_Tools.cpp - the editor's TOOL STRIP (docs/editor-updates-plan.md,
// P1). Split out of MapView.cpp by concern: the strip's layout, its buttons,
// its drawing, and what a left press on the grid does under each tool.
//
// The strip is a column of discs between the palette dock and the grid:
// Paint, Rectangle, Flood, Area and Eyedropper (MapEditor::Tool), then Fill
// level. Its buttons join the toolbar's list (AppendStripButtons), so hover,
// click and render already walk them; only what is different about them lives
// here - where they sit, the ring on the picked tool, the tooltip beside them.
//
// The modifier gestures predate the strip and still work: holding Shift, Ctrl
// or Alt borrows Rectangle, Flood or Eyedropper for that one click, whatever
// tool is picked (Michael: tools you pick, the keys kept as shortcuts).
// ============================================================================
#include "Game/MapView.h"

#include "Core/Loc.h"
#include "Game/MapColors.h"
#include "Game/MapEditor.h"
#include "UI/Controls.h" // ui::DrawBorder

#include <algorithm>

namespace dungeon::game {

namespace {
using Tool = MapEditor::Tool;
constexpr int kToolCount = static_cast<int>(Tool::Count);
constexpr const char* kToolTips[kToolCount] = {
	"map.tool.paint", "map.tool.rect",     "map.tool.flood", "map.tool.area", "map.tool.pick",
	"map.tool.corridor", "map.tool.room", "map.tool.stamp", "map.tool.region"};
} // namespace

gfx::Rect MapView::ToolStripRect(const gfx::Rect& panel) const {
	if (m_mode != Mode::Editor) return {panel.x, panel.y, 0.0f, 0.0f};
	const gfx::Rect ld = LeftDockRect(panel);
	// As wide as a toolbar disc plus the band's padding: ToolbarRect's height IS
	// that (one disc + 4 pads), so the strip and the band read as one chrome.
	return {ld.x + ld.w, ld.y, ToolbarRect(panel).h, ld.h};
}

void MapView::AppendStripButtons(std::vector<ToolButton>& btns,
								 const gfx::Rect& panel) const {
	if (m_mode != Mode::Editor || !m_editor) return;
	const gfx::Rect strip = ToolStripRect(panel);
	const float pad = DockPad(panel);
	const float s = ToolbarRect(panel).h - pad * 4;
	const float x = strip.x + pad * 2;
	float y = strip.y + pad * 2;
	const Tool active = m_editor->ActiveTool();
	for (int i = 0; i < kToolCount; ++i) {
		ToolButton b{static_cast<HoverBtn>(static_cast<int>(HoverBtn::ToolPaint) + i),
					 {x, y, s, s},
					 loc::Tr(kToolTips[i]),
					 m_icoTools[static_cast<size_t>(i)],
					 /*visible*/ true,
					 /*enabled*/ true};
		b.selected = static_cast<int>(active) == i;
		b.strip = true;
		btns.push_back(std::move(b));
		y += s + pad;
	}
	// Fill level stands apart from the tools: it is an ACTION, done the moment
	// it is clicked, not a mode the next click runs through. Enabled only with a
	// surface brush armed - it has nothing to paint otherwise.
	y += pad * 3;
	ToolButton fill{HoverBtn::FillLevel, {x, y, s, s}, loc::Tr("map.tool.filllevel"),
					m_icoFillLevel, true, m_editor->ArmedPaints()};
	fill.strip = true;
	btns.push_back(std::move(fill));
}

void MapView::RenderToolStrip(gfx::SpriteBatch& batch, const ui::Theme& theme,
							  const gfx::Rect& panel) const {
	if (m_mode != Mode::Editor || !m_editor) return;
	const gfx::Rect strip = ToolStripRect(panel);
	batch.DrawRect(strip, theme.panel);
	ui::DrawBorder(batch, strip, theme.panelBorder);
	// The picked tool: an accent ring just outside its disc. Drawn before the
	// discs so the icon sits on top of it.
	std::vector<ToolButton> btns;
	AppendStripButtons(btns, panel);
	for (const ToolButton& b : btns) {
		if (!b.selected) continue;
		const float g = std::max(2.0f, DockPad(panel) * 0.6f);
		const gfx::Rect ring{b.rect.x - g, b.rect.y - g, b.rect.w + g * 2, b.rect.h + g * 2};
		batch.DrawRect(ring, {theme.accent.x, theme.accent.y, theme.accent.z, 0.25f});
		ui::DrawBorder(batch, ring, theme.accent);
	}
	// The Rectangle tool's box while it is being dragged: exactly the squares
	// the release will paint.
	if (m_rectDrag) {
		const Transform t = ComputeTransform(panel);
		const int x0 = std::min(m_rectX0, m_rectX1), x1 = std::max(m_rectX0, m_rectX1);
		const int z0 = std::min(m_rectZ0, m_rectZ1), z1 = std::max(m_rectZ0, m_rectZ1);
		const gfx::Rect box{t.ox + x0 * t.cell, t.oy + z0 * t.cell,
							(x1 - x0 + 1) * t.cell, (z1 - z0 + 1) * t.cell};
		const gfx::Rect grid = GridArea(panel);
		batch.SetScissor(&grid);
		batch.DrawRect(box, {theme.accent.x, theme.accent.y, theme.accent.z, 0.18f});
		ui::DrawBorder(batch, box, theme.accent);
		ui::DrawBorder(batch, {box.x + 1, box.y + 1, box.w - 2, box.h - 2}, theme.accent);
		batch.SetScissor(nullptr);
	}
	RenderShapePreview(batch, theme, panel);
}

bool MapView::UpdateBrush(const Input& input, const gfx::Rect& panel, float mx, float my,
						  bool overGrid, bool& painted) {
	painted = false;
	// The Rectangle drag paints on the RELEASE, read wherever the pointer is -
	// off the grid included, where it keeps the last square it was over.
	if (m_rectDrag && !input.IsMouseDown(MouseButton::Left)) {
		m_rectDrag = false;
		m_editor->PaintRectBetween(m_rectX0, m_rectZ0, m_rectX1, m_rectZ1);
		painted = true;
		return true;
	}
	// A SHAPE drag (corridor, room, region) commits on its release the same way,
	// and commits exactly what it previewed.
	if (m_shapeDrag && !input.IsMouseDown(MouseButton::Left)) {
		m_shapeDrag = false;
		m_editor->ApplyShape(m_shapeTool == Tool::Region
								 ? m_editor->RegionShape(m_rectX0, m_rectZ0, m_rectX1, m_rectZ1)
								 : m_editor->preview);
		m_editor->preview = {};
		painted = true;
		return true;
	}
	if (UpdateShapeTools(input, panel, mx, my, overGrid, painted)) return true;
	if (!overGrid) return false;
	const bool shift = input.IsKeyDown(0x10 /*VK_SHIFT*/);
	const bool ctrl = input.IsKeyDown(0x11 /*VK_CONTROL*/);
	const bool alt = input.IsKeyDown(0x12 /*VK_MENU*/);
	const bool modifier = shift || ctrl || alt;
	// Which tool this press runs through: a held modifier borrows its tool, and
	// only a SURFACE brush has anything for Rectangle/Flood/Area to spread - a
	// placement brush (or none, or a route being laid) behaves as Paint, which
	// is what a click did before there were tools. The eyedropper needs no brush.
	Tool tool = alt ? Tool::Pick : shift ? Tool::Rect : ctrl ? Tool::Flood
												   : m_editor->ActiveTool();
	if (tool != Tool::Pick && (!m_editor->ArmedPaints() || m_editor->LayingRoute()))
		tool = Tool::Paint;
	// A shape tool that UpdateShapeTools left alone (a stamp's press off the
	// grid, a modifier held) falls back to a plain click too.
	if (MapEditor::ShapeTool(tool)) tool = Tool::Paint;

	int cx, cz;
	const bool pressed = input.WasMousePressed(MouseButton::Left);
	// DRAG-AND-DROP (play-test #2): with NO brush armed, a plain left press picks
	// up the top thing on the square and the release drops it (MapView::Update).
	if (tool == Tool::Paint && !modifier && !m_editor->LayingRoute() &&
		m_editor->ArmedCat() == MapEditor::PaletteCat::Count && pressed &&
		CellAt(mx, my, panel, cx, cz)) {
		m_editor->DropFilterFocus();
		m_editor->BeginMove(cx, cz);
		return true;
	}
	if (m_editor->Moving()) return true; // mid-drag: nothing paints
	if (m_rectDrag) { // the box follows the pointer until the release
		if (CellAt(mx, my, panel, cx, cz)) m_rectX1 = cx, m_rectZ1 = cz;
		return true;
	}
	if (pressed && CellAt(mx, my, panel, cx, cz)) {
		m_editor->DropFilterFocus(); // painting reclaims the keyboard
		switch (tool) {
		case Tool::Pick: m_editor->PickAt(cx, cz); break; // never mutates
		case Tool::Rect:
			// Shift+click keeps its old meaning - the box from the LAST painted
			// square - while the picked tool drags a box from this one.
			if (shift) {
				m_editor->PaintRect(cx, cz);
				painted = true;
			} else {
				m_rectDrag = true;
				m_rectX0 = m_rectX1 = cx;
				m_rectZ0 = m_rectZ1 = cz;
			}
			break;
		case Tool::Flood:
			m_editor->FloodFill(cx, cz);
			painted = true;
			break;
		case Tool::Area:
			m_editor->AreaFill(cx, cz);
			painted = true;
			break;
		default:
			// m_hoverFace and m_hoverPlace were both resolved from this same
			// pointer position earlier this frame, so the click commits the pose
			// that was on screen - the ghost is handed over, not recomputed.
			m_editor->BeginStroke(); // press .. release = one undo step
			m_editor->Paint(cx, cz, /*dragging*/ false, m_hoverFace, &m_hoverPlace);
			painted = true;
			break;
		}
		return true;
	}
	if (tool == Tool::Paint && !modifier && input.IsMouseDown(MouseButton::Left) &&
		CellAt(mx, my, panel, cx, cz)) {
		// A drag can arrive without a plain press (a modifier let go mid-hold):
		// it still gets a stroke, so it is still undoable.
		m_editor->BeginStroke();
		m_editor->Paint(cx, cz, /*dragging*/ true, m_hoverFace, &m_hoverPlace);
		painted = true;
		return true;
	}
	return false;
}

bool MapView::UpdateShapeTools(const Input& input, const gfx::Rect& panel, float mx, float my,
							   bool overGrid, bool& painted) {
	const Tool tool = m_editor->ActiveTool();
	const bool held = input.IsKeyDown(0x10) || input.IsKeyDown(0x11) || input.IsKeyDown(0x12);
	if (!MapEditor::ShapeTool(tool) || held || m_editor->LayingRoute()) {
		m_editor->preview = {};
		return false;
	}
	int cx, cz;
	const bool on = overGrid && CellAt(mx, my, panel, cx, cz);
	if (m_shapeDrag) { // the far end follows the pointer until the release
		if (on && (cx != m_rectX1 || cz != m_rectZ1)) {
			m_rectX1 = cx, m_rectZ1 = cz;
			if (tool == Tool::Corridor)
				m_editor->preview = m_editor->CorridorShape(m_rectX0, m_rectZ0, cx, cz);
			else if (tool == Tool::Room)
				m_editor->preview = m_editor->RoomShape(m_rectX0, m_rectZ0, cx, cz);
		}
		return true;
	}
	if (tool == Tool::Stamp) {
		// R turns the stamp (not while the filter box holds the keyboard).
		if (!m_editor->KeyboardCaptured() && input.WasKeyPressed('R')) m_editor->TurnStamp();
		m_editor->preview = on ? m_editor->StampShape(cx, cz) : carve::Shape{};
		if (on && input.WasMousePressed(MouseButton::Left)) {
			m_editor->DropFilterFocus();
			m_editor->ApplyShape(m_editor->preview);
			painted = true;
			return true;
		}
		return false;
	}
	m_editor->preview = {};
	if (on && input.WasMousePressed(MouseButton::Left)) {
		m_editor->DropFilterFocus();
		m_shapeDrag = true;
		m_shapeTool = tool;
		m_rectX0 = m_rectX1 = cx;
		m_rectZ0 = m_rectZ1 = cz;
		if (tool == Tool::Corridor) m_editor->preview = m_editor->CorridorShape(cx, cz, cx, cz);
		else if (tool == Tool::Room) m_editor->preview = m_editor->RoomShape(cx, cz, cx, cz);
		return true;
	}
	return false;
}

void MapView::RenderShapePreview(gfx::SpriteBatch& batch, const ui::Theme& theme,
								 const gfx::Rect& panel) const {
	if (m_mode != Mode::Editor || !m_editor) return;
	const Transform t = ComputeTransform(panel);
	const gfx::Rect grid = GridArea(panel);
	batch.SetScissor(&grid);
	// What opens, tinted by role; what a stamp makes solid, dark.
	const Vec4 room{theme.accent.x, theme.accent.y, theme.accent.z, 0.35f};
	const Vec4 corridor{0.45f, 0.75f, 0.95f, 0.35f};
	for (const carve::Square& q : m_editor->preview.open)
		batch.DrawRect({t.ox + q.x * t.cell, t.oy + q.z * t.cell, t.cell, t.cell},
					   q.role == carve::Role::Corridor ? corridor : room);
	for (const auto& [x, z] : m_editor->preview.solid)
		batch.DrawRect({t.ox + x * t.cell, t.oy + z * t.cell, t.cell, t.cell},
					   {0.05f, 0.05f, 0.06f, 0.75f});
	// A region's box (its contents are generated on the release).
	if (m_shapeDrag && m_shapeTool == Tool::Region) {
		const int x0 = std::min(m_rectX0, m_rectX1), x1 = std::max(m_rectX0, m_rectX1);
		const int z0 = std::min(m_rectZ0, m_rectZ1), z1 = std::max(m_rectZ0, m_rectZ1);
		const gfx::Rect box{t.ox + x0 * t.cell, t.oy + z0 * t.cell, (x1 - x0 + 1) * t.cell,
							(z1 - z0 + 1) * t.cell};
		// Red while too small to generate in (RegionShape's 6x6 floor).
		const bool tooSmall = x1 - x0 + 1 < 6 || z1 - z0 + 1 < 6;
		batch.DrawRect(box, tooSmall ? Vec4{0.8f, 0.3f, 0.25f, 0.18f}
									 : Vec4{theme.accent.x, theme.accent.y, theme.accent.z, 0.18f});
		ui::DrawBorder(batch, box, tooSmall ? Vec4{0.9f, 0.35f, 0.3f, 1.0f} : theme.accent);
	}
	batch.SetScissor(nullptr);
}

} // namespace dungeon::game
