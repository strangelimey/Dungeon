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

MapView::Transform MapView::ComputeTransform(const gfx::Rect& panel) const {
	const gfx::Rect g = GridArea(panel); // panel minus the dock in Editor mode
	const DungeonMap& map = m_world.Map();
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
	return m_mode == Mode::Editor || m_world.IsSeen(x, z);
}

bool MapView::CellAt(float px, float py, const gfx::Rect& panel, int& outX,
					 int& outZ) const {
	const Transform t = ComputeTransform(panel);
	if (t.cell <= 0.0f) return false;
	const int x = static_cast<int>(std::floor((px - t.ox) / t.cell));
	const int z = static_cast<int>(std::floor((py - t.oy) / t.cell));
	const DungeonMap& map = m_world.Map();
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

	const float mx = input.MouseX(), my = input.MouseY();
	const DungeonMap& map = m_world.Map();
	const bool editor = m_mode == Mode::Editor;
	const gfx::Rect grid = GridArea(panel); // panel minus the dock in Editor
	const bool overGrid = grid.Contains(mx, my);

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

	// In Editor a brush is always armed: left paints, so pan with the right
	// button. Player mode is view-only, so pan with the left.
	const MouseButton panBtn = editor ? MouseButton::Right : MouseButton::Left;
	if (overGrid && input.WasMousePressed(panBtn)) {
		m_panning = true;
		m_lastMouse = {mx, my};
	}
	if (m_panning && input.IsMouseDown(panBtn)) {
		m_pan.x += (mx - m_lastMouse.x) / grid.w;
		m_pan.y += (my - m_lastMouse.y) / grid.h;
		m_lastMouse = {mx, my};
	}
	if (input.WasMouseReleased(panBtn)) m_panning = false;

	// Editor painting over the grid: a fresh press always acts; holding paints a
	// stroke for the structural/surface brushes (MapEditor ignores drags for the
	// click-only Select/Erase tools and entity placement). The Edit* calls no-op
	// on unchanged cells, so a held stroke over one cell is cheap.
	if (editor && m_editor && overGrid) {
		int cx, cz;
		if (input.WasMousePressed(MouseButton::Left) && CellAt(mx, my, panel, cx, cz))
			m_editor->Paint(cx, cz, /*dragging*/ false);
		else if (input.IsMouseDown(MouseButton::Left) && CellAt(mx, my, panel, cx, cz))
			m_editor->Paint(cx, cz, /*dragging*/ true);
	}

	return panel.Contains(mx, my);
}

void MapView::Render(gfx::SpriteBatch& batch, const ui::Theme& theme,
					 const gfx::Rect& panel) {
	if (!m_open) return;

	const DungeonMap& map = m_world.Map();
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

	// 3) Fixtures and static decorations (both from the static map layer).
	for (const WallSconce& s : map.Sconces())
		if (CellVisible(s.x, s.z))
			edgeMarker(s.x, s.z, 0.30f, kTorch,
					   {static_cast<float>(DirDX(s.wall)), static_cast<float>(DirDZ(s.wall))});
	for (const auto& [x, z] : map.BrazierCells())
		if (CellVisible(x, z)) marker(x, z, 0.46f, kBrazier);
	// Decorations from the LIVE world list (so editor placements/removals show),
	// labelled with their type initial.
	for (const auto& m : m_world.DecorationMarkers()) {
		if (!CellVisible(m.x, m.z)) continue;
		marker(m.x, m.z, 0.38f, kDecoration);
		label(m.x, m.z, m.type);
	}

	// Stairs (over the decoration marker they also occupy) — a distinct color.
	for (const StairLink& s : m_world.Map().Stairs())
		if (CellVisible(s.x, s.z)) marker(s.x, s.z, 0.44f, kStair);

	// 4) Dynamic entities. Monsters come from the LIVE world list, drawn once per
	// cell with a type initial and a stack count when several share a square;
	// items/buttons still come from the .ent layer until they too are editable.
	const std::vector<DungeonWorld::MapMarker> mons = m_world.MonsterMarkers();
	for (size_t i = 0; i < mons.size(); ++i) {
		bool firstInCell = true;
		for (size_t j = 0; j < i; ++j)
			if (mons[j].x == mons[i].x && mons[j].z == mons[i].z) { firstInCell = false; break; }
		if (!firstInCell || !CellVisible(mons[i].x, mons[i].z)) continue;
		int count = 0;
		for (const auto& m : mons)
			if (m.x == mons[i].x && m.z == mons[i].z) ++count;
		marker(mons[i].x, mons[i].z, 0.5f, kMonster);
		label(mons[i].x, mons[i].z, mons[i].type);
		countBadge(mons[i].x, mons[i].z, count);
	}
	for (const Entity& e : m_world.Entities().All()) {
		if (!CellVisible(e.x, e.z)) continue;
		switch (e.kind) {
		case EntityKind::Item:   marker(e.x, e.z, 0.34f, kItem); break;
		case EntityKind::Button: marker(e.x, e.z, 0.3f, kButton); break;
		default:                 break; // monsters: live list above; decorations: static
		}
	}

	// 4b) Editor selection: a high-contrast outline on the selected square, and the
	// selected creature's patrol route as high-contrast arrows (waypoint order,
	// closing the loop) — persistent while selected, and growing live while laying.
	if (m_editor && m_editor->HasSelection()) {
		const Vec4 kSel{1.0f, 0.95f, 0.20f, 1.0f};     // bright yellow
		const Vec4 kSelDark{0.0f, 0.0f, 0.0f, 0.85f};  // dark backing for contrast
		// Selection outline: four thin bars ringing the cell.
		const gfx::Rect r = cellRect(m_editor->SelX(), m_editor->SelZ());
		const float bw = std::clamp(t.cell * 0.06f, 1.5f, 4.0f);
		batch.DrawRect({r.x - bw, r.y - bw, r.w + 2 * bw, bw}, kSel);
		batch.DrawRect({r.x - bw, r.y + r.h, r.w + 2 * bw, bw}, kSel);
		batch.DrawRect({r.x - bw, r.y, bw, r.h}, kSel);
		batch.DrawRect({r.x + r.w, r.y, bw, r.h}, kSel);

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
	{
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

	// A dock = its panel background + a collapse button showing flip arrows.
	auto drawDockFrame = [&](const gfx::Rect& dock, const gfx::Rect& btn,
							 const char* arrow) {
		batch.DrawRect(dock, theme.panel);
		ui::DrawBorder(batch, dock, theme.panelBorder);
		batch.DrawRect(btn, theme.control);
		ui::DrawBorder(batch, btn, theme.panelBorder);
		m_font.Draw(batch, arrow,
					btn.x + (btn.w - m_font.MeasureWidth(arrow)) * 0.5f,
					btn.y + (btn.h - m_font.Height()) * 0.5f, theme.text);
	};

	// --- Left palette dock (Editor only; collapsed -> only the ">>" button). The
	// frame + collapse + "Brushes" header are MapView's (they affect layout); the
	// accordion body is filled by MapEditor::RenderBody.
	if (m_mode == Mode::Editor) {
		const gfx::Rect ld = LeftDockRect(panel);
		drawDockFrame(ld, LeftCollapseButton(panel),
					  m_settings.mapPaletteCollapsed ? ">>" : "<<");
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
					  LegendCollapsed() ? "<<" : ">>");
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

	// Player title, centered over the grid area (clear of the key dock).
	if (m_mode == Mode::Player) {
		const std::string title = loc::Tr("map.title");
		m_font.Draw(batch, title,
					grid.x + (grid.w - m_font.MeasureWidth(title)) * 0.5f,
					panel.y + pad, theme.text);
	}

	// Footer (kept within the grid area, clear of the docks): pan/zoom hint
	// (left) + party cell (right).
	const float footY = panel.y + panel.h - m_font.Height() - pad;
	const char* hintKey = m_mode == Mode::Editor ? "map.hint.editor" : "map.hint";
	m_font.Draw(batch, loc::Tr(hintKey), grid.x + pad, footY, theme.textDim);

	const Party& party = m_world.GetParty();
	const std::string pos =
		loc::Format("map.position", party.GridX(), party.GridZ());
	m_font.Draw(batch, pos, grid.x + grid.w - m_font.MeasureWidth(pos) - pad,
				footY, theme.textDim);
}

} // namespace dungeon::game
