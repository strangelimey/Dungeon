// ============================================================================
// Game/MapView.cpp — see MapView.h.
//
// The shared map VIEWPORT behind both modes:
//   Player (M key)        — fog of war: only DungeonWorld::IsSeen cells and
//                           their contents draw. The eventual map-fragment /
//                           reveal-spell items just feed the same fog set
//                           (MarkSeen), so they need nothing here.
//   Editor (`editor` cmd) — the whole map and every creature/item draw,
//                           ignoring fog, plus the left-dock brush palette.
// One renderer + one pick math drive both. The editor's palette + brush logic
// live in MapEditor (set via SetEditor); MapView draws the left dock chrome and
// hit-tests the grid, then drives the editor for the body + the brush apply.
// ============================================================================
#include "Game/MapView.h"

#include "Core/Loc.h"
#include "Game/Entity.h"
#include "Game/MapColors.h"
#include "Game/MapEditor.h"
#include "UI/Controls.h" // ui::DrawBorder

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace dungeon::game {

namespace {
constexpr float kPi = 3.14159265f;

// Tints a base cell color by a surface variant index so painted texture types
// read distinctly on the map. variant < 1 (unpainted, or the base palette slot)
// keeps the base; higher indices add a stable hue so brick/stone/mossy differ.
Vec4 VariantTint(const Vec4& base, int variant) {
	if (variant < 1) return base;
	static const Vec4 hue[] = {
		{0.00f, 0.00f, 0.00f, 0.0f}, {0.10f, 0.22f, 0.06f, 0.0f},
		{0.06f, 0.12f, 0.26f, 0.0f}, {0.26f, 0.10f, 0.06f, 0.0f}};
	const Vec4& h = hue[static_cast<size_t>(variant) % 4];
	const auto cl = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
	return {cl(base.x + h.x), cl(base.y + h.y), cl(base.z + h.z), base.w};
}

// Font px at the design window height (re-baked to track the real height).
constexpr float kFontH = 18.0f;

// Editor dock metrics, all derived from the panel so Update (window pixels)
// and Render (device pixels) agree. (DockPad is a public MapView static so
// MapEditor can lay out the palette body with the same padding.)
float DockBtnH(const gfx::Rect& p) { return std::clamp(p.h * 0.06f, 28.0f, 56.0f); }
float CollapsedDockW(const gfx::Rect& p) { return std::clamp(p.w * 0.032f, 34.0f, 60.0f); }
float ExpandedLeftW(const gfx::Rect& p) { return std::clamp(p.w * 0.16f, 120.0f, 260.0f); }
float ExpandedRightW(const gfx::Rect& p) { return std::clamp(p.w * 0.18f, 150.0f, 300.0f); }

// Y of the first item below a dock's collapse button + header line.
float DockBodyTop(const gfx::Rect& dock, const gfx::Rect& panel) {
	const float pad = MapView::DockPad(panel), h = DockBtnH(panel);
	return dock.y + pad + h + pad + h * 0.7f + pad;
}
} // namespace

float MapView::DockPad(const gfx::Rect& p) { return std::clamp(p.h * 0.010f, 3.0f, 9.0f); }

MapView::MapView(gfx::GraphicsDevice& device, DungeonWorld& world,
				 GameSettings& settings)
	: m_device(device), m_world(world), m_settings(settings),
	  m_font(device, "", kFontH) {}

const DungeonMap& MapView::ViewedMap() const {
	return m_browse ? m_browse->map : m_world.Map();
}

const std::string& MapView::ViewedLevel() const {
	return m_browse ? m_browse->stem : m_world.CurrentLevel();
}

std::string MapView::LevelNeighbor(int step) const {
	const std::vector<std::string>& levels = m_world.GetProject().levels;
	const auto it = std::find(levels.begin(), levels.end(), ViewedLevel());
	if (it == levels.end()) return {};
	const ptrdiff_t i = (it - levels.begin()) + step;
	if (i < 0 || i >= static_cast<ptrdiff_t>(levels.size())) return {};
	return levels[static_cast<size_t>(i)];
}

void MapView::StepViewLevel(int step) {
	const std::string stem = LevelNeighbor(step);
	if (stem.empty()) return;
	if (stem == m_world.CurrentLevel()) m_browse.reset(); // back on live state
	else m_browse = m_world.BrowseLevel(stem);
	m_zoom = 1.0f; // refit: levels differ in size
	m_pan = {0.0f, 0.0f};
}

gfx::Rect MapView::LevelUpButton(const gfx::Rect& panel) const {
	const gfx::Rect g = GridArea(panel);
	const float pad = DockPad(panel);
	const float s = std::clamp(panel.h * 0.042f, 22.0f, 40.0f);
	return {g.x + pad * 2, panel.y + pad * 2, s, s};
}

gfx::Rect MapView::LevelDownButton(const gfx::Rect& panel) const {
	const gfx::Rect up = LevelUpButton(panel);
	return {up.x + up.w + DockPad(panel), up.y, up.w, up.h};
}

gfx::Rect MapView::SaveSourceButton(const gfx::Rect& panel) const {
	const gfx::Rect g = GridArea(panel);
	const float pad = DockPad(panel);
	const float s = std::clamp(panel.h * 0.042f, 22.0f, 40.0f);
	const float w = s * 4.0f; // room for the localized label
	return {g.x + g.w - pad * 2 - w, panel.y + pad * 2, w, s};
}

gfx::Rect MapView::SaveButton(const gfx::Rect& panel) const {
	const gfx::Rect src = SaveSourceButton(panel);
	return {src.x - DockPad(panel) - src.w, src.y, src.w, src.h};
}

gfx::Rect MapView::RedoButton(const gfx::Rect& panel) const {
	const gfx::Rect save = SaveButton(panel);
	return {save.x - DockPad(panel) - save.h, save.y, save.h, save.h};
}

gfx::Rect MapView::UndoButton(const gfx::Rect& panel) const {
	const gfx::Rect redo = RedoButton(panel);
	return {redo.x - DockPad(panel) - redo.w, redo.y, redo.w, redo.h};
}

void MapView::DoUndoRedo(bool redo) {
	if (redo) m_world.Redo();
	else m_world.Undo();
	// A restored stash must show immediately on a browsed level (the snapshot
	// is a copy, like the after-paint rebuild in Update).
	if (m_browse) m_browse = m_world.BrowseLevel(m_browse->stem);
}

MapView::Transform MapView::ComputeTransform(const gfx::Rect& panel) const {
	const gfx::Rect g = GridArea(panel); // panel minus the dock in Editor mode
	const DungeonMap& map = ViewedMap();
	const float mw = static_cast<float>(map.Width());
	const float mh = static_cast<float>(map.Height());
	const float fit = std::min(g.w / mw, g.h / mh); // whole map fits at zoom 1
	const float cell = fit * m_zoom;
	const float gridW = mw * cell, gridH = mh * cell;
	const float ox = g.x + (g.w - gridW) * 0.5f + m_pan.x * g.w;
	const float oy = g.y + (g.h - gridH) * 0.5f + m_pan.y * g.h;
	return {cell, ox, oy};
}

gfx::Rect MapView::GridArea(const gfx::Rect& panel) const {
	// The right key dock is present in both modes; the left brush dock is
	// Editor-only.
	const float l = m_mode == Mode::Editor ? LeftDockRect(panel).w : 0.0f;
	const float r = RightDockRect(panel).w;
	return {panel.x + l, panel.y, panel.w - l - r, panel.h};
}

gfx::Rect MapView::LeftDockRect(const gfx::Rect& panel) const {
	const float w = m_settings.mapPaletteCollapsed ? CollapsedDockW(panel)
												   : ExpandedLeftW(panel);
	return {panel.x, panel.y, w, panel.h};
}

gfx::Rect MapView::RightDockRect(const gfx::Rect& panel) const {
	const float w = LegendCollapsed() ? CollapsedDockW(panel)
									  : ExpandedRightW(panel);
	return {panel.x + panel.w - w, panel.y, w, panel.h};
}

bool MapView::LegendCollapsed() const {
	return m_mode == Mode::Editor ? m_settings.mapLegendCollapsed
								  : m_settings.mapPlayerKeyCollapsed;
}

void MapView::ToggleLegend() {
	bool& flag = m_mode == Mode::Editor ? m_settings.mapLegendCollapsed
										: m_settings.mapPlayerKeyCollapsed;
	flag = !flag;
	m_settings.Save();
}

gfx::Rect MapView::LeftCollapseButton(const gfx::Rect& panel) const {
	const gfx::Rect d = LeftDockRect(panel);
	const float pad = DockPad(panel);
	return {d.x + pad, d.y + pad, d.w - 2 * pad, DockBtnH(panel)};
}

gfx::Rect MapView::RightCollapseButton(const gfx::Rect& panel) const {
	const gfx::Rect d = RightDockRect(panel);
	const float pad = DockPad(panel);
	return {d.x + pad, d.y + pad, d.w - 2 * pad, DockBtnH(panel)};
}

gfx::Rect MapView::PaletteBody(const gfx::Rect& panel) const {
	const gfx::Rect d = LeftDockRect(panel);
	const float pad = DockPad(panel);
	const float top = DockBodyTop(d, panel);
	return {d.x + pad, top, d.w - 2 * pad, d.y + d.h - top - pad};
}

bool MapView::CellVisible(int x, int z) const {
	if (m_mode == Mode::Editor) return true;
	if (!m_browse) return m_world.IsSeen(x, z);
	// Browsed level: the fog stashed when the party last left it (a
	// never-visited level has no stash — nothing is revealed).
	const DungeonMap& map = m_browse->map;
	if (m_browse->seen.empty() || x < 0 || z < 0 || x >= map.Width() ||
		z >= map.Height())
		return false;
	return m_browse->seen[static_cast<size_t>(z) * map.Width() + x] != 0;
}

bool MapView::CellAt(float px, float py, const gfx::Rect& panel, int& outX,
					 int& outZ) const {
	const Transform t = ComputeTransform(panel);
	if (t.cell <= 0.0f) return false;
	const int x = static_cast<int>(std::floor((px - t.ox) / t.cell));
	const int z = static_cast<int>(std::floor((py - t.oy) / t.cell));
	const DungeonMap& map = ViewedMap();
	if (x < 0 || z < 0 || x >= map.Width() || z >= map.Height()) return false;
	outX = x;
	outZ = z;
	return true;
}

bool MapView::Update(const Input& input, const gfx::Rect& panel) {
	if (!m_open) {
		m_panning = false;
		return false;
	}

	// Keep the icon/label font sized to the panel (re-bakes only when the
	// rounded height actually changes, i.e. on window resize — not on zoom).
	m_font.SetHeight(std::clamp(panel.h * 0.030f, 11.0f, 30.0f));

	// If the party arrived on the level being browsed (the world keeps
	// simulating under the open map), snap to the live view of it.
	if (m_browse && m_browse->stem == m_world.CurrentLevel()) m_browse.reset();

	// A latched undo/redo executes one frame AFTER its trigger, so the busy
	// button state rendered last frame is what the (long, blocking) restore
	// freezes on screen — see m_pendingHistory.
	if (m_pendingHistory != 0) {
		const bool redo = m_pendingHistory > 0;
		m_pendingHistory = 0;
		DoUndoRedo(redo);
	}

	const float mx = input.MouseX(), my = input.MouseY();
	const DungeonMap& map = ViewedMap();
	const bool editor = m_mode == Mode::Editor;
	const gfx::Rect grid = GridArea(panel); // panel minus the dock in Editor
	const bool overGrid = grid.Contains(mx, my);

	// Editor keyboard: Ctrl+Z / Ctrl+Y = undo/redo. The overlay otherwise
	// leaves the keyboard alone (movement keys keep reaching the party), but
	// the Ctrl chord can't collide with a bound movement key.
	if (editor && m_pendingHistory == 0 && input.IsKeyDown(0x11 /*VK_CONTROL*/)) {
		if (input.WasKeyPressed('Z')) m_pendingHistory = -1;
		else if (input.WasKeyPressed('Y')) m_pendingHistory = +1;
	}

	// Track the hovered cell (for Render highlights, e.g. the faint item icon).
	if (int hx, hz; overGrid && CellAt(mx, my, panel, hx, hz)) {
		m_hoverX = hx;
		m_hoverZ = hz;
	} else {
		m_hoverX = m_hoverZ = -1;
	}

	// Track the hovered chrome button (Render styles it via the shared
	// ui::DrawButtonFace). Mirrors the click gating: hidden/unavailable
	// buttons never read as hot.
	m_hoverBtn = HoverBtn::None;
	if (!LevelNeighbor(-1).empty() && LevelUpButton(panel).Contains(mx, my))
		m_hoverBtn = HoverBtn::LevelUp;
	else if (!LevelNeighbor(+1).empty() && LevelDownButton(panel).Contains(mx, my))
		m_hoverBtn = HoverBtn::LevelDown;
	else if (editor && m_world.CanUndo() && UndoButton(panel).Contains(mx, my))
		m_hoverBtn = HoverBtn::Undo;
	else if (editor && m_world.CanRedo() && RedoButton(panel).Contains(mx, my))
		m_hoverBtn = HoverBtn::Redo;
	else if (editor && SaveButton(panel).Contains(mx, my))
		m_hoverBtn = HoverBtn::Save;
	else if (editor && SaveSourceButton(panel).Contains(mx, my))
		m_hoverBtn = HoverBtn::SaveSource;
	else if (editor && LeftCollapseButton(panel).Contains(mx, my))
		m_hoverBtn = HoverBtn::CollapseL;
	else if (RightCollapseButton(panel).Contains(mx, my))
		m_hoverBtn = HoverBtn::CollapseR;

	// Wheel zooms about the cursor: keep the map point under the pointer fixed.
	if (overGrid && input.WheelDelta() != 0.0f && map.Width() > 0) {
		const Transform t0 = ComputeTransform(panel);
		const float fx = (mx - t0.ox) / t0.cell; // map point (in cells) at cursor
		const float fz = (my - t0.oy) / t0.cell;
		m_zoom = std::clamp(m_zoom * std::pow(1.2f, input.WheelDelta()), 1.0f, 10.0f);
		const float fit = std::min(grid.w / map.Width(), grid.h / map.Height());
		const float cell = fit * m_zoom;
		const float gridW = map.Width() * cell, gridH = map.Height() * cell;
		m_pan.x = (mx - fx * cell - grid.x - (grid.w - gridW) * 0.5f) / grid.w;
		m_pan.y = (my - fz * cell - grid.y - (grid.h - gridH) * 0.5f) / grid.h;
	}

	// Wheel over the expanded left palette dock scrolls its accordion (editor).
	if (editor && m_editor && !m_settings.mapPaletteCollapsed &&
		input.WheelDelta() != 0.0f && LeftDockRect(panel).Contains(mx, my)) {
		m_editor->OnWheel(input.WheelDelta(), panel);
	}

	// Dock interactions, each claiming the click so it never also pans/paints.
	if (input.WasMousePressed(MouseButton::Left)) {
		// Level-browse arrows (both modes). A hidden arrow (edge level) is not
		// hit-tested either, so a click there falls through to the grid.
		if (!LevelNeighbor(-1).empty() && LevelUpButton(panel).Contains(mx, my)) {
			StepViewLevel(-1);
			return true;
		}
		if (!LevelNeighbor(+1).empty() && LevelDownButton(panel).Contains(mx, my)) {
			StepViewLevel(+1);
			return true;
		}
		// Editor save buttons (top-right of the grid): Save / To source.
		if (editor && onSave) {
			if (SaveButton(panel).Contains(mx, my)) {
				onSave(false);
				return true;
			}
			if (SaveSourceButton(panel).Contains(mx, my)) {
				onSave(true);
				return true;
			}
		}
		// Undo/redo buttons (left of Save) — hidden when their stack is empty,
		// so a click there falls through like a hidden level arrow; disabled
		// (latched trigger pending) they swallow the click but do nothing.
		if (editor) {
			if (m_world.CanUndo() && UndoButton(panel).Contains(mx, my)) {
				if (m_pendingHistory == 0) m_pendingHistory = -1;
				return true;
			}
			if (m_world.CanRedo() && RedoButton(panel).Contains(mx, my)) {
				if (m_pendingHistory == 0) m_pendingHistory = +1;
				return true;
			}
		}
		// Right key dock collapse — both modes (flips the mode's own flag).
		if (RightCollapseButton(panel).Contains(mx, my)) {
			ToggleLegend();
			return true;
		}
		// Left palette dock (Editor only): collapse button + accordion body.
		if (editor) {
			if (LeftCollapseButton(panel).Contains(mx, my)) {
				m_settings.mapPaletteCollapsed = !m_settings.mapPaletteCollapsed;
				m_settings.Save();
				return true;
			}
			if (m_editor && !m_settings.mapPaletteCollapsed &&
				PaletteBody(panel).Contains(mx, my)) {
				m_editor->OnClick(mx, my, panel);
				return true; // a click anywhere in the dock body is the dock's
			}
		}
	}

	// Right-click a palette item opens its config dialog (monsters: the anim
	// editor). Handled before the pan logic so it never starts a right-drag pan;
	// safe because pan-start needs overGrid (false over the dock).
	if (editor && m_editor && !m_settings.mapPaletteCollapsed &&
		input.WasMousePressed(MouseButton::Right) && PaletteBody(panel).Contains(mx, my)) {
		m_editor->OnRightClick(mx, my, panel);
		return true;
	}

	// In Editor left paints the armed brush, so pan with the right button.
	// Player mode is view-only, so pan with the left.
	const MouseButton panBtn = editor ? MouseButton::Right : MouseButton::Left;
	if (overGrid && input.WasMousePressed(panBtn)) {
		m_panning = true;
		m_lastMouse = {mx, my};
		m_panStart = {mx, my};
	}
	if (m_panning && input.IsMouseDown(panBtn)) {
		m_pan.x += (mx - m_lastMouse.x) / grid.w;
		m_pan.y += (my - m_lastMouse.y) / grid.h;
		m_lastMouse = {mx, my};
	}
	if (input.WasMouseReleased(panBtn)) {
		const bool wasPanning = m_panning;
		m_panning = false;
		// A STATIONARY right-click (no drag since the press) in Editor mode
		// inspects the cell — the former Select tool: contents + selection +
		// the object's edit dialog. A real drag stays a pan (the sub-3px pan
		// a click causes is imperceptible).
		if (wasPanning && editor && m_editor) {
			const float dx = mx - m_panStart.x, dy = my - m_panStart.y;
			if (dx * dx + dy * dy < 9.0f) {
				if (int cx, cz; CellAt(mx, my, panel, cx, cz))
					m_editor->InspectAt(cx, cz);
			}
		}
	}

	// Middle-click erases the cell (the former Erase tool; one undo step each).
	if (editor && m_editor && overGrid &&
		input.WasMousePressed(MouseButton::Middle)) {
		if (int cx, cz; CellAt(mx, my, panel, cx, cz)) {
			m_editor->EraseAt(cx, cz);
			// A remote erase edits the browsed level's stash — refresh the view.
			if (m_browse) m_browse = m_world.BrowseLevel(m_browse->stem);
			return true;
		}
	}

	// Editor painting over the grid: a fresh press always acts; holding paints a
	// stroke for the structural/surface brushes (MapEditor ignores drags for the
	// click-only Select/Erase tools and entity placement). The Edit* calls no-op
	// on unchanged cells, so a held stroke over one cell is cheap. On a BROWSED
	// level the brush routes to the level's stash (MapEditor reads ViewedLevel);
	// the snapshot is rebuilt after a paint so the edit draws next frame (pure
	// in-memory copies — no file IO).
	if (editor && m_editor && overGrid) {
		int cx, cz;
		bool painted = false;
		if (input.WasMousePressed(MouseButton::Left) && CellAt(mx, my, panel, cx, cz)) {
			m_editor->Paint(cx, cz, /*dragging*/ false);
			painted = true;
		} else if (input.IsMouseDown(MouseButton::Left) && CellAt(mx, my, panel, cx, cz)) {
			m_editor->Paint(cx, cz, /*dragging*/ true);
			painted = true;
		}
		if (painted && m_browse) m_browse = m_world.BrowseLevel(m_browse->stem);
	}

	return panel.Contains(mx, my);
}

void MapView::Render(gfx::SpriteBatch& batch, const ui::Theme& theme,
					 const gfx::Rect& panel) {
	if (!m_open) return;

	const DungeonMap& map = ViewedMap();
	const Transform t = ComputeTransform(panel);
	const gfx::Rect grid = GridArea(panel); // panel minus the dock in Editor

	// Panel base, then clip the map to the grid area (inset a touch) so a
	// panned/zoomed map never spills over the frame or under the dock.
	batch.DrawRect(panel, kMapBg);
	const gfx::Rect clip{grid.x + 2, grid.y + 2, grid.w - 4, grid.h - 4};
	batch.SetScissor(&clip);

	const float inset = std::clamp(t.cell * 0.08f, 0.5f, 2.0f); // grid gaps
	auto cellRect = [&](int x, int z) -> gfx::Rect {
		return {t.ox + x * t.cell + inset, t.oy + z * t.cell + inset,
				t.cell - 2 * inset, t.cell - 2 * inset};
	};
	auto cellCenter = [&](int x, int z) -> Vec2 {
		return {t.ox + (x + 0.5f) * t.cell, t.oy + (z + 0.5f) * t.cell};
	};
	// Editor hover: the cell under the mouse gets a 50%-alpha selection-preview ring
	// (see below), and the faint floor-item icon fades IN to full (see itemMarker).
	// Primary markers stay fully visible so you can still see what you're hovering.
	// Player mode has no selection concept, so no hover effect there.
	const bool editorHover = m_mode == Mode::Editor;
	auto hovered = [&](int x, int z) { return editorHover && x == m_hoverX && z == m_hoverZ; };
	// The Select-tool square outline (four thin bars ringing a cell), drawn in `col`
	// — shared by the persistent selection (opaque) and the hover preview (50%).
	auto selOutline = [&](int x, int z, const Vec4& col) {
		const gfx::Rect r = cellRect(x, z);
		const float bw = std::clamp(t.cell * 0.06f, 1.5f, 4.0f);
		batch.DrawRect({r.x - bw, r.y - bw, r.w + 2 * bw, bw}, col);
		batch.DrawRect({r.x - bw, r.y + r.h, r.w + 2 * bw, bw}, col);
		batch.DrawRect({r.x - bw, r.y, bw, r.h}, col);
		batch.DrawRect({r.x + r.w, r.y, bw, r.h}, col);
	};
	auto marker = [&](int x, int z, float frac, const Vec4& c) {
		const Vec2 ctr = cellCenter(x, z);
		const float h = t.cell * frac * 0.5f;
		batch.DrawRect({ctr.x - h, ctr.y - h, h * 2, h * 2}, c);
	};
	// A marker pushed flush against the (dir) cell edge instead of the centre,
	// so a wall fixture shows on its wall — leaving room for more than one per
	// cell on different walls.
	auto edgeMarker = [&](int x, int z, float frac, const Vec4& c, Vec2 dir) {
		const Vec2 ctr = cellCenter(x, z);
		const float h = t.cell * frac * 0.5f;
		const float px = ctr.x + dir.x * (t.cell * 0.5f - h);
		const float py = ctr.y + dir.y * (t.cell * 0.5f - h);
		batch.DrawRect({px - h, py - h, h * 2, h * 2}, c);
	};
	// A small green arrow pointing the way a placed thing faces (Editor authoring
	// aid). Local frame: apex up = north at facing 0, rotated facing*90° clockwise
	// (screen Y down), so it matches the party triangle + compass. Sits toward the
	// facing edge so it reads as a pointer over the marker square.
	auto facingArrow = [&](int x, int z, Direction f) {
		if (t.cell < 10.0f) return;
		const Vec2 c = cellCenter(x, z);
		const float a = static_cast<float>(static_cast<int>(f)) * (kPi * 0.5f);
		const float cs = std::cos(a), sn = std::sin(a);
		auto rot = [&](float lx, float ly) -> Vec2 {
			return {c.x + lx * cs - ly * sn, c.y + lx * sn + ly * cs};
		};
		const float off = t.cell * 0.24f, r = t.cell * 0.15f, w = t.cell * 0.13f;
		batch.DrawTriangle(rot(0.0f, -(off + r)), rot(-w, -off), rot(w, -off), kFacingArrow);
	};
	// The floor-item marker: tucked into the cell's lower-LEFT corner, faint so
	// it stays out of the way — full opacity only when the cell is hovered.
	// (Items sit ON the floor, so they read as a subtle secondary of whatever
	// else shares the square, not a primary marker.) A model item draws its
	// baked HUD icon there; placeholder items keep the small green square.
	auto itemMarker = [&](int x, int z, const std::string& type) {
		const Vec2 ctr = cellCenter(x, z);
		const gfx::Texture* icon = m_world.ItemIconLookup(type);
		const float h = t.cell * (icon ? 0.17f : 0.11f); // half-size
		const float gap = t.cell * 0.05f;                // inset from the edges
		const float px = ctr.x - (t.cell * 0.5f - h - gap);
		const float py = ctr.y + (t.cell * 0.5f - h - gap);
		const float a = hovered(x, z) ? 1.0f : 0.55f;
		if (icon)
			batch.DrawSprite({px - h, py - h, h * 2, h * 2}, {0, 0, 1, 1}, *icon,
							 {1, 1, 1, a});
		else
			batch.DrawRect({px - h, py - h, h * 2, h * 2}, {kItem.x, kItem.y, kItem.z, a});
	};
	// A type initial centred on a marker (skipped when cells are too small to
	// read); the upper-cased first letter of the catalog id.
	auto label = [&](int x, int z, const std::string& type) {
		if (type.empty() || t.cell < 16.0f) return;
		char ch = type[0];
		if (ch >= 'a' && ch <= 'z') ch -= 32;
		const std::string s(1, ch);
		const Vec2 c = cellCenter(x, z);
		m_font.Draw(batch, s, c.x - m_font.MeasureWidth(s) * 0.5f,
					c.y - m_font.Height() * 0.5f, kMarkerInk);
	};
	// A small stack-count badge ("x3") at the cell's bottom-right corner.
	auto countBadge = [&](int x, int z, int n) {
		if (n < 2 || t.cell < 20.0f) return;
		const std::string s = "x" + std::to_string(n);
		const Vec2 c = cellCenter(x, z);
		m_font.Draw(batch, s, c.x + t.cell * 0.5f - m_font.MeasureWidth(s) - 1.0f,
					c.y + t.cell * 0.5f - m_font.Height(), theme.text);
	};

	// A solid cell shows the wall-variant of an adjacent painted floor cell (walls
	// belong to the floor cells they border), so a painted wall type tints the
	// surrounding wall squares on the map.
	auto wallCellVariant = [&](int x, int z) -> int {
		const int n[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
		for (const auto& d : n)
			if (map.At(x + d[0], z + d[1]) == Cell::Floor) {
				const int v = map.WallVariant(x + d[0], z + d[1]);
				if (v >= 0) return v;
			}
		return -1;
	};

	// 1) Floors and walls, tinted by surface variant (Player: revealed only).
	for (int z = 0; z < map.Height(); ++z)
		for (int x = 0; x < map.Width(); ++x) {
			if (!CellVisible(x, z)) continue;
			const Vec4 col = map.At(x, z) == Cell::Wall
								 ? VariantTint(kWall, wallCellVariant(x, z))
								 : VariantTint(kFloor, map.FloorVariant(x, z));
			batch.DrawRect(cellRect(x, z), col);
		}

	// 2) Start cell — an accent outline.
	if (CellVisible(map.StartX(), map.StartZ()))
		ui::DrawBorder(batch, cellRect(map.StartX(), map.StartZ()), theme.accent);

	// A baked-icon marker: the kind's own model rendered into a small RT
	// (UpdateMapIcons), drawn centred at `frac` of the cell. The square+letter
	// markers stay the fallback while an icon hasn't baked yet.
	auto iconMarker = [&](int x, int z, float frac, const gfx::Texture& icon) {
		const Vec2 ctr = cellCenter(x, z);
		const float h = t.cell * frac * 0.5f;
		batch.DrawSprite({ctr.x - h, ctr.y - h, h * 2.0f, h * 2.0f}, {0, 0, 1, 1},
						 icon, {1, 1, 1, 1});
	};
	// The edge-hugging flavour, for wall fixtures (mirrors edgeMarker).
	auto edgeIcon = [&](int x, int z, float frac, const gfx::Texture& icon,
						Vec2 dir) {
		const Vec2 ctr = cellCenter(x, z);
		const float h = t.cell * frac * 0.5f;
		const float px = ctr.x + dir.x * (t.cell * 0.5f - h);
		const float py = ctr.y + dir.y * (t.cell * 0.5f - h);
		batch.DrawSprite({px - h, py - h, h * 2.0f, h * 2.0f}, {0, 0, 1, 1}, icon,
						 {1, 1, 1, 1});
	};

	// 3) Fixtures and static decorations (both from the static map layer).
	for (const WallSconce& s : map.Sconces()) {
		if (!CellVisible(s.x, s.z)) continue;
		const Vec2 dir{static_cast<float>(DirDX(s.wall)),
					   static_cast<float>(DirDZ(s.wall))};
		if (const gfx::Texture* icon = m_world.SconceIcon())
			edgeIcon(s.x, s.z, 0.34f, *icon, dir);
		else
			edgeMarker(s.x, s.z, 0.16f, kTorch, dir);
	}
	for (const FloorBrazier& b : map.Braziers()) {
		if (!CellVisible(b.x, b.z)) continue;
		if (const gfx::Texture* icon = m_world.BrazierIcon())
			iconMarker(b.x, b.z, 0.62f, *icon);
		else
			marker(b.x, b.z, 0.46f, kBrazier);
	}
	// Decorations: the LIVE world list for the active level (so editor
	// placements/removals show), the map's records for a browsed one.
	std::vector<DungeonWorld::MapMarker> decos;
	if (!m_browse) {
		decos = m_world.DecorationMarkers();
	} else {
		for (const Entity& e : m_browse->map.Decorations())
			decos.push_back({e.x, e.z, e.type, e.facing,
							 m_world.DecorationIconFor(e.type),
							 m_world.DecorationShowsFacing(e.type)});
	}
	for (const auto& m : decos) {
		if (!CellVisible(m.x, m.z)) continue;
		if (m.icon) {
			iconMarker(m.x, m.z, 0.74f, *m.icon);
		} else {
			marker(m.x, m.z, 0.38f, kDecoration);
			label(m.x, m.z, m.type);
		}
		if (m_mode == Mode::Editor && m.facingArrow)
			facingArrow(m.x, m.z, m.facing);
	}

	// Stairs (over the decoration marker they also occupy) — a distinct color,
	// with a dark arrow for which way they lead (up/down from stairs.cat's `up`
	// field; both modes — the player map wants it as much as the editor).
	const Catalog& stairCat = m_world.GetProject().stairs;
	for (const StairLink& s : map.Stairs()) {
		if (!CellVisible(s.x, s.z)) continue;
		marker(s.x, s.z, 0.44f, kStair);
		if (t.cell < 10.0f) continue; // too small to read, like facingArrow
		const bool up = CatalogBool(stairCat.Find(s.type), "up", false);
		const Vec2 c = cellCenter(s.x, s.z);
		const float h = t.cell * 0.14f, w = t.cell * 0.12f;
		const float d = up ? -1.0f : 1.0f; // screen Y grows down: -1 = apex up
		batch.DrawTriangle({c.x, c.y + d * h}, {c.x - w, c.y - d * h},
						   {c.x + w, c.y - d * h}, kMapBg);
	}

	// Doors: a bar across the cell, perpendicular to the travel axis (the way
	// the panel actually spans the doorway). Open doors fade to half alpha so
	// a shut door reads at a glance. Live list for the active level, .ent
	// records for a browsed one.
	{
		std::vector<DungeonWorld::DoorMarker> doors;
		if (!m_browse) {
			doors = m_world.DoorMarkers();
		} else {
			for (const Entity& e : m_browse->entities.All())
				if (e.kind == EntityKind::Door) {
					const std::string* open = e.Param("open");
					doors.push_back({e.x, e.z, e.facing, open && *open != "0"});
				}
		}
		for (const auto& d : doors) {
			if (!CellVisible(d.x, d.z)) continue;
			const Vec2 c = cellCenter(d.x, d.z);
			// Travel north-south -> the panel spans east-west (a wide bar).
			const bool spanX =
				d.facing == Direction::North || d.facing == Direction::South;
			const float len = t.cell * 0.38f, thick = t.cell * 0.10f;
			Vec4 col = kDoor;
			col.w = d.open ? 0.5f : 1.0f;
			batch.DrawRect({c.x - (spanX ? len : thick), c.y - (spanX ? thick : len),
							(spanX ? len : thick) * 2, (spanX ? thick : len) * 2},
						   col);
		}
	}

	// 4) Dynamic entities. Monsters come from the LIVE world list for the active
	// level, drawn once per cell with a type initial and a stack count when
	// several share a square. A browsed level has no live state: the editor
	// shows its authored .ent spawns; the player map shows no monsters there
	// (they have moved since — stale markers would only mislead).
	std::vector<DungeonWorld::MapMarker> mons;
	if (!m_browse) {
		mons = m_world.MonsterMarkers();
	} else if (m_mode == Mode::Editor) {
		for (const Entity& e : m_browse->entities.All())
			if (e.kind == EntityKind::Monster)
				mons.push_back({e.x, e.z, e.type, e.facing,
								m_world.MonsterIconFor(e.type),
								m_world.MonsterShowsFacing(e.type)});
	}
	for (size_t i = 0; i < mons.size(); ++i) {
		bool firstInCell = true;
		for (size_t j = 0; j < i; ++j)
			if (mons[j].x == mons[i].x && mons[j].z == mons[i].z) { firstInCell = false; break; }
		if (!firstInCell || !CellVisible(mons[i].x, mons[i].z)) continue;
		int count = 0;
		for (const auto& m : mons)
			if (m.x == mons[i].x && m.z == mons[i].z) ++count;
		// A baked head-shot icon draws instead of the colored square + type
		// initial (which stays the fallback for a not-yet-baked/unknown kind).
		if (mons[i].icon) {
			iconMarker(mons[i].x, mons[i].z, 0.92f, *mons[i].icon);
		} else {
			marker(mons[i].x, mons[i].z, 0.5f, kMonster);
			label(mons[i].x, mons[i].z, mons[i].type);
		}
		countBadge(mons[i].x, mons[i].z, count);
		if (m_mode == Mode::Editor && mons[i].facingArrow)
			facingArrow(mons[i].x, mons[i].z, mons[i].facing);
	}
	const std::vector<Entity>& ents =
		m_browse ? m_browse->entities.All() : m_world.Entities().All();
	for (const Entity& e : ents) {
		if (!CellVisible(e.x, e.z)) continue;
		switch (e.kind) {
		case EntityKind::Item:   itemMarker(e.x, e.z, e.type); break;
		case EntityKind::Button:
			// The lever's baked icon (buttons share the decoration kind cache),
			// else the blue square for a legacy/unknown type.
			if (const gfx::Texture* icon = m_world.DecorationIconFor(e.type))
				iconMarker(e.x, e.z, 0.5f, *icon);
			else
				marker(e.x, e.z, 0.3f, kButton);
			break;
		default:                 break; // monsters: live list above; decorations: static
		}
	}

	// 4b) Editor selection: a high-contrast outline on the selected square, and the
	// selected creature's patrol route as high-contrast arrows (waypoint order,
	// closing the loop) — persistent while selected, and growing live while laying.
	const Vec4 kSel{1.0f, 0.95f, 0.20f, 1.0f};     // bright yellow
	const Vec4 kSelDark{0.0f, 0.0f, 0.0f, 0.85f};  // dark backing for contrast
	// Hover preview: the same square outline on the cell under the mouse, at 50%
	// alpha, so it reads as "click to select here". Skipped when it coincides with
	// the actual selection (which draws opaque below).
	const bool selHere = m_editor && m_editor->HasSelection() &&
						 m_editor->SelX() == m_hoverX && m_editor->SelZ() == m_hoverZ;
	// The hover ring previews the brush target on any viewed level; the
	// SELECTION (and its route overlay) is a live-instance thing, so it only
	// draws on the active level.
	if (editorHover && m_hoverX >= 0 && !selHere)
		selOutline(m_hoverX, m_hoverZ, {kSel.x, kSel.y, kSel.z, 0.5f});

	if (m_editor && m_editor->HasSelection() && !m_browse) {
		selOutline(m_editor->SelX(), m_editor->SelZ(), kSel); // opaque selection ring

		const std::vector<ai::Cell>* route = m_world.MonsterPatrol(m_editor->SelectedMonster());
		if (route && !route->empty()) {
			const float th = std::clamp(t.cell * 0.10f, 2.0f, 6.0f);
			auto arrow = [&](Vec2 a, Vec2 b, const Vec4& col, float thick) {
				const float dx = b.x - a.x, dy = b.y - a.y;
				const float len = std::sqrt(dx * dx + dy * dy);
				if (len < 1e-3f) return;
				const float ang = std::atan2(dy, dx);
				const Vec2 dir{dx / len, dy / len}, perp{-dir.y, dir.x};
				const float head = std::min(len * 0.4f, thick * 3.0f);
				const float shaft = len - head;
				const Vec2 mid{a.x + dir.x * shaft * 0.5f, a.y + dir.y * shaft * 0.5f};
				batch.DrawRectRotated(mid, {shaft, thick}, ang, col);
				const Vec2 base{b.x - dir.x * head, b.y - dir.y * head};
				const float hw = thick * 1.6f;
				batch.DrawTriangle(b, {base.x + perp.x * hw, base.y + perp.y * hw},
								   {base.x - perp.x * hw, base.y - perp.y * hw}, col);
			};
			const size_t n = route->size();
			for (size_t k = 0; n >= 2 && k < n; ++k) { // n arrows, closing last -> first
				const Vec2 a = cellCenter((*route)[k].x, (*route)[k].z);
				const ai::Cell& nx = (*route)[(k + 1) % n];
				const Vec2 b = cellCenter(nx.x, nx.z);
				arrow(a, b, kSelDark, th + 2.0f); // dark backing
				arrow(a, b, kSel, th);            // bright fill
			}
			for (const ai::Cell& wp : *route) marker(wp.x, wp.z, 0.26f, kSel);
		}
	}

	// 5) The party — a triangle pointing the way it faces (facing*90° clockwise
	// from north-up; screen Y is down so the rotation matches the compass).
	// Only on its own level: a browsed level doesn't hold the party.
	if (!m_browse) {
		const Party& party = m_world.GetParty();
		const Vec2 c = cellCenter(party.GridX(), party.GridZ());
		const float r = t.cell * 0.36f;
		const float a = party.Facing() * (kPi * 0.5f);
		const float cs = std::cos(a), sn = std::sin(a);
		auto rot = [&](float lx, float ly) -> Vec2 {
			return {c.x + lx * cs - ly * sn, c.y + lx * sn + ly * cs};
		};
		batch.DrawTriangle(rot(0, -r), rot(-r * 0.72f, r * 0.7f),
						   rot(r * 0.72f, r * 0.7f), theme.accent);
	}

	batch.SetScissor(nullptr);
	ui::DrawBorder(batch, panel, theme.panelBorder);

	const float pad = std::clamp(panel.w * 0.012f, 4.0f, 16.0f);

	const float dpad = DockPad(panel);
	const float btnH = DockBtnH(panel);

	// A dock = its panel background + a collapse button showing flip arrows
	// (drawn through the shared button face so it hovers like every button).
	auto drawDockFrame = [&](const gfx::Rect& dock, const gfx::Rect& btn,
							 const char* arrow, HoverBtn id) {
		batch.DrawRect(dock, theme.panel);
		ui::DrawBorder(batch, dock, theme.panelBorder);
		ui::DrawButtonFace(batch, m_font, btn, arrow, theme, m_hoverBtn == id);
	};

	// --- Left palette dock (Editor only; collapsed -> only the ">>" button). The
	// frame + collapse + "Brushes" header are MapView's (they affect layout); the
	// accordion body is filled by MapEditor::RenderBody.
	if (m_mode == Mode::Editor) {
		const gfx::Rect ld = LeftDockRect(panel);
		drawDockFrame(ld, LeftCollapseButton(panel),
					  m_settings.mapPaletteCollapsed ? ">>" : "<<",
					  HoverBtn::CollapseL);
		if (!m_settings.mapPaletteCollapsed) {
			m_font.Draw(batch, loc::Tr("map.brushes"), ld.x + dpad,
						ld.y + dpad + btnH + dpad, theme.textDim);
			if (m_editor) m_editor->RenderBody(batch, theme, panel);
		}
	}

	// --- Right key dock (BOTH modes; collapsed -> only the "<<" button). The
	// Player key is a trimmed subset (the obvious wall/floor rows are dropped).
	{
		const gfx::Rect rd = RightDockRect(panel);
		drawDockFrame(rd, RightCollapseButton(panel),
					  LegendCollapsed() ? "<<" : ">>", HoverBtn::CollapseR);
		if (!LegendCollapsed()) {
			m_font.Draw(batch, loc::Tr("map.key"), rd.x + dpad,
						rd.y + dpad + btnH + dpad, theme.textDim);
			// A swatch (filled / outlined / triangle) + label per symbol; the
			// `player` flag drops a row from the Player key. Party and start use
			// the live theme accent, so the table is built here.
			enum class Sym { Filled, Outline, Triangle };
			struct Row { Sym sym; Vec4 color; const char* key; bool player; };
			const Row rows[] = {
				{Sym::Triangle, theme.accent, "map.key.party", true},
				{Sym::Outline, theme.accent, "map.key.start", true},
				{Sym::Filled, kWall, "map.key.wall", false},
				{Sym::Filled, kFloor, "map.key.floor", false},
				{Sym::Filled, kTorch, "map.key.torch", true},
				{Sym::Filled, kBrazier, "map.key.brazier", true},
				{Sym::Filled, kMonster, "map.key.monster", true},
				{Sym::Filled, kItem, "map.key.item", true},
				{Sym::Filled, kButton, "map.key.button", true},
				{Sym::Filled, kDecoration, "map.key.decoration", true},
				{Sym::Filled, kDoor, "map.key.door", true},
				{Sym::Filled, kStair, "map.key.stairs", true},
			};
			const gfx::Rect rclip{rd.x + 2, rd.y + 2, rd.w - 4, rd.h - 4};
			batch.SetScissor(&rclip);
			const float rowH = std::clamp(panel.h * 0.05f, 22.0f, 44.0f);
			float y = DockBodyTop(rd, panel);
			for (const Row& row : rows) {
				if (m_mode == Mode::Player && !row.player) continue;
				const float sw = rowH - dpad * 2;
				const gfx::Rect box{rd.x + dpad, y + dpad, sw, sw};
				switch (row.sym) {
				case Sym::Filled: batch.DrawRect(box, row.color); break;
				case Sym::Outline: ui::DrawBorder(batch, box, row.color); break;
				case Sym::Triangle:
					batch.DrawTriangle({box.x + sw * 0.5f, box.y},
									   {box.x, box.y + sw}, {box.x + sw, box.y + sw},
									   row.color);
					break;
				}
				m_font.Draw(batch, loc::Tr(row.key), box.x + sw + dpad,
							y + (rowH - m_font.Height()) * 0.5f, theme.text);
				y += rowH;
			}
			batch.SetScissor(nullptr);
		}
	}

	// Level-browse header (both modes): [^]/[v] arrows + the viewed level's
	// stem, top-left of the grid area. An edge level hides its dead-direction
	// arrow (nothing above the top level / below the bottom); the stem draws in
	// the accent color while browsing, as a "not where the party is" flag.
	{
		// Every chrome button draws through the shared ui::DrawButtonFace, so
		// hover reads exactly like the dialog buttons (m_hoverBtn is tracked by
		// Update in window pixels — identity, not coordinates, crosses the
		// Update/Render pixel-space split).
		auto face = [&](const gfx::Rect& r, const std::string& label,
						HoverBtn id, bool enabled = true) {
			ui::DrawButtonFace(batch, m_font, r, label, theme,
							   enabled && m_hoverBtn == id, /*held*/ false,
							   enabled);
		};
		const std::string above = LevelNeighbor(-1), below = LevelNeighbor(+1);
		const gfx::Rect upR = LevelUpButton(panel), dnR = LevelDownButton(panel);
		if (!above.empty()) face(upR, "^", HoverBtn::LevelUp);
		if (!below.empty()) face(dnR, "v", HoverBtn::LevelDown);
		m_font.Draw(batch, ViewedLevel(), dnR.x + dnR.w + dpad * 2,
					upR.y + (upR.h - m_font.Height()) * 0.5f,
					m_browse ? theme.accent : theme.text);

		// Editor save buttons, top-right of the grid (Update hit-tests the same
		// rects): Save = write every edited level; To source = also copy the
		// project into the repo tree. Undo/redo draw only while their stacks
		// have steps (hit-testing matches, so a hidden button never eats a
		// click); while a restore is latched they draw DISABLED for the frame
		// it executes on, so the click visibly takes even if it hitches.
		if (m_mode == Mode::Editor) {
			face(SaveButton(panel), loc::Tr("map.btn.save"), HoverBtn::Save);
			face(SaveSourceButton(panel), loc::Tr("map.btn.source"),
				 HoverBtn::SaveSource);
			const bool busy = m_pendingHistory != 0;
			if (m_world.CanUndo())
				face(UndoButton(panel), "<", HoverBtn::Undo, !busy);
			if (m_world.CanRedo())
				face(RedoButton(panel), ">", HoverBtn::Redo, !busy);
		}
	}

	// Player title, centered over the grid area (clear of the key dock).
	if (m_mode == Mode::Player) {
		const std::string title = loc::Tr("map.title");
		m_font.Draw(batch, title,
					grid.x + (grid.w - m_font.MeasureWidth(title)) * 0.5f,
					panel.y + pad, theme.text);
	}

	// Footer (kept within the grid area, clear of the docks): pan/zoom hint
	// (left) + party cell (right). PLAYER mode only — the editor keeps its
	// bottom row clear for map cells (its controls are discoverable enough).
	if (m_mode == Mode::Player) {
		const float footY = panel.y + panel.h - m_font.Height() - pad;
		m_font.Draw(batch, loc::Tr("map.hint"), grid.x + pad, footY, theme.textDim);

		const Party& party = m_world.GetParty();
		const std::string pos =
			loc::Format("map.position", party.GridX(), party.GridZ());
		m_font.Draw(batch, pos, grid.x + grid.w - m_font.MeasureWidth(pos) - pad,
					footY, theme.textDim);
	}

}

} // namespace dungeon::game
