// ============================================================================
// Game/MapView.cpp — see MapView.h.
//
// Two modes share one renderer and one pick math:
//   Player (M key)        — fog of war: only DungeonWorld::IsSeen cells and
//                           their contents draw. The eventual map-fragment /
//                           reveal-spell items just feed the same fog set
//                           (MarkSeen), so they need nothing here.
//   Editor (`editor` cmd) — the whole map and every creature/item draw,
//                           ignoring fog, plus the tool palette and painting.
// ============================================================================
#include "Game/MapView.h"

#include "Core/Loc.h"
#include "Game/Entity.h"
#include "UI/Controls.h" // ui::DrawBorder

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace dungeon::game {

namespace {
constexpr float kPi = 3.14159265f;

// Map palette (stylized, not the UI theme — these are the dungeon's own ink).
const Vec4 kMapBg{0.04f, 0.04f, 0.06f, 1.0f}; // panel fill (opaque: full-screen editor covers all)
const Vec4 kWall{0.46f, 0.42f, 0.36f, 1.0f};   // solid structure (the bright ink)
const Vec4 kFloor{0.13f, 0.13f, 0.16f, 1.0f};  // walkable (recedes)
const Vec4 kTorch{1.0f, 0.62f, 0.28f, 1.0f};
const Vec4 kBrazier{1.0f, 0.45f, 0.16f, 1.0f};
const Vec4 kMonster{0.85f, 0.22f, 0.22f, 1.0f};
const Vec4 kItem{0.42f, 0.85f, 0.42f, 1.0f};
const Vec4 kButton{0.42f, 0.62f, 0.95f, 1.0f};
const Vec4 kDecoration{0.74f, 0.54f, 0.92f, 1.0f}; // static props (columns, fountains, ...)
const Vec4 kCeiling{0.30f, 0.30f, 0.34f, 1.0f};    // ceiling palette swatch
const Vec4 kDoor{0.78f, 0.60f, 0.35f, 1.0f};       // door category
const Vec4 kStair{0.60f, 0.72f, 0.78f, 1.0f};      // stair category
const Vec4 kToolSelect{0.45f, 0.70f, 0.95f, 1.0f}; // Select tool
const Vec4 kToolErase{0.90f, 0.40f, 0.40f, 1.0f};  // Erase tool

// Font px at the design window height (re-baked to track the real height).
constexpr float kFontH = 18.0f;

// Editor dock metrics, all derived from the panel so Update (window pixels)
// and Render (device pixels) agree.
float DockPad(const gfx::Rect& p) { return std::clamp(p.h * 0.010f, 3.0f, 9.0f); }
float DockBtnH(const gfx::Rect& p) { return std::clamp(p.h * 0.06f, 28.0f, 56.0f); }
float CollapsedDockW(const gfx::Rect& p) { return std::clamp(p.w * 0.032f, 34.0f, 60.0f); }
float ExpandedLeftW(const gfx::Rect& p) { return std::clamp(p.w * 0.16f, 120.0f, 260.0f); }
float ExpandedRightW(const gfx::Rect& p) { return std::clamp(p.w * 0.18f, 150.0f, 300.0f); }

// Y of the first item below a dock's collapse button + header line.
float DockBodyTop(const gfx::Rect& dock, const gfx::Rect& panel) {
	const float pad = DockPad(panel), h = DockBtnH(panel);
	return dock.y + pad + h + pad + h * 0.7f + pad;
}
} // namespace

MapView::MapView(gfx::GraphicsDevice& device, DungeonWorld& world,
				 GameSettings& settings)
	: m_device(device), m_world(world), m_settings(settings),
	  m_font(device, "", kFontH) {
	// Open the most-used categories by default; the rest start collapsed.
	m_catOpen[static_cast<size_t>(PaletteCat::Tools)] = true;
	m_catOpen[static_cast<size_t>(PaletteCat::Structure)] = true;
	m_catOpen[static_cast<size_t>(PaletteCat::Walls)] = true;
}

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

const char* MapView::CategoryNameKey(PaletteCat cat) {
	switch (cat) {
	case PaletteCat::Tools:       return "map.cat.tools";
	case PaletteCat::Structure:   return "map.cat.structure";
	case PaletteCat::Walls:       return "map.cat.walls";
	case PaletteCat::Floors:      return "map.cat.floors";
	case PaletteCat::Ceilings:    return "map.cat.ceilings";
	case PaletteCat::Decorations: return "map.cat.decorations";
	case PaletteCat::Fixtures:    return "map.cat.fixtures";
	case PaletteCat::Monsters:    return "map.cat.monsters";
	case PaletteCat::Doors:       return "map.cat.doors";
	case PaletteCat::Stairs:      return "map.cat.stairs";
	case PaletteCat::Items:       return "map.cat.items";
	default:                      return "";
	}
}

// Resolves a category's items: built-in tools/structure, the level's surface
// palette (Walls/Floors/Ceilings, display names from the project's surface
// catalogs), or the project's entity catalogs.
std::vector<MapView::PaletteItem> MapView::CategoryItems(PaletteCat cat) const {
	const DungeonMap& map = m_world.Map();
	const Project& proj = m_world.GetProject();

	// A surface palette (list of catalog ids) resolved to display name + swatch.
	auto surfaceItems = [&](const std::vector<std::string>& palette,
							const Catalog& catalog, const Vec4& swatch) {
		std::vector<PaletteItem> items;
		for (const std::string& id : palette) {
			const CatalogEntry* e = catalog.Find(id);
			items.push_back({e ? e->Display() : id, swatch, id});
		}
		return items;
	};
	// An entity catalog resolved to display name + swatch + id.
	auto catalogItems = [&](const Catalog& catalog, const Vec4& swatch) {
		std::vector<PaletteItem> items;
		for (const CatalogEntry& e : catalog.Entries())
			items.push_back({e.Display(), swatch, e.id});
		return items;
	};

	switch (cat) {
	case PaletteCat::Tools:
		return {{loc::Tr("map.tool.select"), kToolSelect},
				{loc::Tr("map.tool.erase"), kToolErase}};
	case PaletteCat::Structure:
		return {{loc::Tr("map.brush.wall"), kWall},
				{loc::Tr("map.brush.floor"), kFloor}};
	case PaletteCat::Walls:    return surfaceItems(map.WallPalette(), proj.walls, kWall);
	case PaletteCat::Floors:   return surfaceItems(map.FloorPalette(), proj.floors, kFloor);
	case PaletteCat::Ceilings: return surfaceItems(map.CeilingPalette(), proj.ceilings, kCeiling);
	case PaletteCat::Decorations: return catalogItems(proj.decorations, kDecoration);
	case PaletteCat::Fixtures:    return catalogItems(proj.fixtures, kTorch);
	case PaletteCat::Monsters:    return catalogItems(proj.monsters, kMonster);
	case PaletteCat::Doors:       return catalogItems(proj.doors, kDoor);
	case PaletteCat::Stairs:      return catalogItems(proj.stairs, kStair);
	case PaletteCat::Items:       return catalogItems(proj.items, kItem);
	default:                      return {};
	}
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

void MapView::BuildPaletteRows(const gfx::Rect& panel, std::vector<PaletteRow>& out,
							   float& contentHeight) const {
	out.clear();
	const gfx::Rect body = PaletteBody(panel);
	const float pad = DockPad(panel);
	const float headerH = std::clamp(panel.h * 0.045f, 22.0f, 42.0f);
	const float itemH = std::clamp(panel.h * 0.040f, 20.0f, 36.0f);

	float y = body.y - m_paletteScroll;
	for (int c = 0; c < static_cast<int>(PaletteCat::Count); ++c) {
		const PaletteCat cat = static_cast<PaletteCat>(c);
		out.push_back({PaletteRow::Kind::Header, cat, -1, {body.x, y, body.w, headerH}});
		y += headerH;
		if (m_catOpen[c]) {
			const std::vector<PaletteItem> items = CategoryItems(cat);
			if (items.empty()) {
				out.push_back({PaletteRow::Kind::Empty, cat, -1, {body.x, y, body.w, itemH}});
				y += itemH;
			} else {
				for (int i = 0; i < static_cast<int>(items.size()); ++i) {
					out.push_back({PaletteRow::Kind::Item, cat, i,
								   {body.x, y, body.w, itemH}});
					y += itemH;
				}
			}
		}
		y += pad; // gap between categories
	}
	contentHeight = (y + m_paletteScroll) - body.y;
}

void MapView::ApplyBrush(int cx, int cz, bool dragging) {
	using SS = DungeonWorld::SurfaceSel;
	const DungeonMap& map = m_world.Map();
	auto log = [&](const std::string& s) {
		if (m_world.onMessage) m_world.onMessage(s);
	};

	switch (m_sel.cat) {
	case PaletteCat::Structure: {
		const Cell target = m_sel.index == 0 ? Cell::Wall : Cell::Floor;
		const Party& party = m_world.GetParty();
		const bool wouldTrapParty = target == Cell::Wall && cx == party.GridX() &&
									cz == party.GridZ();
		if (!wouldTrapParty) m_world.EditCell(cx, cz, target);
		break;
	}
	case PaletteCat::Walls:    m_world.EditVariant(cx, cz, SS::Wall, m_sel.index); break;
	case PaletteCat::Floors:   m_world.EditVariant(cx, cz, SS::Floor, m_sel.index); break;
	case PaletteCat::Ceilings: m_world.EditVariant(cx, cz, SS::Ceiling, m_sel.index); break;
	case PaletteCat::Tools: {
		if (dragging) break; // tools act on a single click, not a stroke
		if (m_sel.index == 0) { // Select: report the cell's contents
			const char* base = map.At(cx, cz) == Cell::Wall ? "wall" : "floor";
			int props = 0;
			for (const auto& [px, pz] : m_world.DecorationCells())
				if (px == cx && pz == cz) ++props;
			int mons = 0;
			for (const auto& [px, pz] : m_world.MonsterCells())
				if (px == cx && pz == cz) ++mons;
			std::string details = base;
			if (mons) details += std::format(", {} monster{}", mons, mons == 1 ? "" : "s");
			if (props) details += std::format(", {} prop{}", props, props == 1 ? "" : "s");
			log(loc::Format("map.select.contents", cx, cz, details));
		} else { // Erase: remove a runtime entity, else reset surface overrides
			if (m_world.RemoveEntityAt(cx, cz)) {
				log(loc::Tr("map.erase.removed"));
			} else {
				m_world.EditVariant(cx, cz, SS::Wall, -1);
				m_world.EditVariant(cx, cz, SS::Floor, -1);
				m_world.EditVariant(cx, cz, SS::Ceiling, -1);
				log(loc::Format("map.erase.reset", cx, cz));
			}
		}
		break;
	}
	case PaletteCat::Decorations:
	case PaletteCat::Monsters: {
		if (dragging) break; // placement is a single click
		const std::vector<PaletteItem> items = CategoryItems(m_sel.cat);
		if (m_sel.index < 0 || m_sel.index >= static_cast<int>(items.size())) break;
		const std::string& id = items[m_sel.index].id;
		const bool ok = m_sel.cat == PaletteCat::Monsters
							? m_world.AddMonster(id, cx, cz, Direction::South)
							: m_world.AddDecoration(id, cx, cz, Direction::South);
		log(loc::Format(ok ? "map.place.done" : "map.place.blocked",
						items[m_sel.index].label));
		break;
	}
	default: { // Fixtures/Doors/Stairs/Items — placement wiring lands later
		if (dragging) break;
		const std::vector<PaletteItem> items = CategoryItems(m_sel.cat);
		if (m_sel.index >= 0 && m_sel.index < static_cast<int>(items.size()))
			log(loc::Format("map.place.todo", items[m_sel.index].label));
		break;
	}
	}
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

	// Wheel over the expanded left palette dock scrolls its accordion.
	if (editor && !m_settings.mapPaletteCollapsed && input.WheelDelta() != 0.0f &&
		LeftDockRect(panel).Contains(mx, my)) {
		std::vector<PaletteRow> rows;
		float content = 0.0f;
		BuildPaletteRows(panel, rows, content);
		const float maxScroll = std::max(0.0f, content - PaletteBody(panel).h);
		m_paletteScroll = std::clamp(m_paletteScroll - input.WheelDelta() * 28.0f,
									 0.0f, maxScroll);
	}

	// Dock interactions, each claiming the click so it never also pans/paints.
	if (input.WasMousePressed(MouseButton::Left)) {
		// Right key dock collapse — both modes (flips the mode's own flag).
		if (RightCollapseButton(panel).Contains(mx, my)) {
			ToggleLegend();
			return true;
		}
		// Left palette dock (Editor only): collapse button + accordion.
		if (editor) {
			if (LeftCollapseButton(panel).Contains(mx, my)) {
				m_settings.mapPaletteCollapsed = !m_settings.mapPaletteCollapsed;
				m_settings.Save();
				return true;
			}
			if (!m_settings.mapPaletteCollapsed &&
				PaletteBody(panel).Contains(mx, my)) {
				std::vector<PaletteRow> rows;
				float content = 0.0f;
				BuildPaletteRows(panel, rows, content);
				for (const PaletteRow& r : rows) {
					if (!r.rect.Contains(mx, my)) continue;
					if (r.kind == PaletteRow::Kind::Header)
						m_catOpen[static_cast<size_t>(r.cat)] =
							!m_catOpen[static_cast<size_t>(r.cat)];
					else if (r.kind == PaletteRow::Kind::Item)
						m_sel = {r.cat, r.index};
					break;
				}
				return true; // a click anywhere in the dock body is the dock's
			}
		}
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
	// stroke for the structural/surface brushes (ApplyBrush ignores drags for the
	// click-only Select/Erase tools and entity placement). The Edit* calls no-op
	// on unchanged cells, so a held stroke over one cell is cheap.
	if (editor && overGrid) {
		int cx, cz;
		if (input.WasMousePressed(MouseButton::Left) && CellAt(mx, my, panel, cx, cz))
			ApplyBrush(cx, cz, /*dragging*/ false);
		else if (input.IsMouseDown(MouseButton::Left) && CellAt(mx, my, panel, cx, cz))
			ApplyBrush(cx, cz, /*dragging*/ true);
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

	// 1) Floors and walls (Player: only revealed cells; Editor: all).
	for (int z = 0; z < map.Height(); ++z)
		for (int x = 0; x < map.Width(); ++x) {
			if (!CellVisible(x, z)) continue;
			batch.DrawRect(cellRect(x, z),
						   map.At(x, z) == Cell::Wall ? kWall : kFloor);
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
	// Decorations from the LIVE world list (so editor placements/removals show);
	// drawn as centre markers (the wall-edge nicety the static records carried is
	// dropped now that placement is live — restore via a stored wall dir later).
	for (const auto& [x, z] : m_world.DecorationCells())
		if (CellVisible(x, z)) marker(x, z, 0.38f, kDecoration);

	// Stairs (over the decoration marker they also occupy) — a distinct color.
	for (const StairLink& s : m_world.Map().Stairs())
		if (CellVisible(s.x, s.z)) marker(s.x, s.z, 0.44f, kStair);

	// 4) Dynamic entities. Monsters come from the LIVE world list (placed/erased
	// reflect immediately); items/buttons still come from the .ent layer until
	// they too become live-editable.
	for (const auto& [x, z] : m_world.MonsterCells())
		if (CellVisible(x, z)) marker(x, z, 0.5f, kMonster);
	for (const Entity& e : m_world.Entities().All()) {
		if (!CellVisible(e.x, e.z)) continue;
		switch (e.kind) {
		case EntityKind::Item:   marker(e.x, e.z, 0.34f, kItem); break;
		case EntityKind::Button: marker(e.x, e.z, 0.3f, kButton); break;
		default:                 break; // monsters: live list above; decorations: static
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

	// --- Left palette dock (Editor only; collapsed -> only the ">>" button).
	// A collapsible accordion: one header per category, its items beneath when
	// expanded, the whole body scroll-clipped so scrolled rows never spill out.
	if (m_mode == Mode::Editor) {
		const gfx::Rect ld = LeftDockRect(panel);
		drawDockFrame(ld, LeftCollapseButton(panel),
					  m_settings.mapPaletteCollapsed ? ">>" : "<<");
		if (!m_settings.mapPaletteCollapsed) {
			m_font.Draw(batch, loc::Tr("map.brushes"), ld.x + dpad,
						ld.y + dpad + btnH + dpad, theme.textDim);
			const gfx::Rect body = PaletteBody(panel);
			batch.SetScissor(&body);
			std::vector<PaletteRow> rows;
			float content = 0.0f;
			BuildPaletteRows(panel, rows, content);
			const float arrowW = m_font.MeasureWidth("+");
			std::vector<PaletteItem> items; // the current category's items
			for (const PaletteRow& r : rows) {
				const gfx::Rect& rc = r.rect;
				if (r.kind == PaletteRow::Kind::Header) items = CategoryItems(r.cat);
				if (rc.y + rc.h < body.y || rc.y > body.y + body.h) continue; // off-view
				const float ty = rc.y + (rc.h - m_font.Height()) * 0.5f;
				switch (r.kind) {
				case PaletteRow::Kind::Header: {
					batch.DrawRect(rc, theme.control);
					ui::DrawBorder(batch, rc, theme.panelBorder);
					const char* arrow = m_catOpen[static_cast<size_t>(r.cat)] ? "-" : "+";
					m_font.Draw(batch, arrow, rc.x + dpad, ty, theme.textDim);
					m_font.Draw(batch, loc::Tr(CategoryNameKey(r.cat)),
								rc.x + dpad * 2 + arrowW, ty, theme.text);
					break;
				}
				case PaletteRow::Kind::Empty:
					m_font.Draw(batch, loc::Tr("map.cat.empty"), rc.x + dpad * 3, ty,
								theme.textDim);
					break;
				case PaletteRow::Kind::Item: {
					if (r.index < 0 || r.index >= static_cast<int>(items.size())) break;
					const bool active = m_sel.cat == r.cat && m_sel.index == r.index;
					if (active) {
						batch.DrawRect(rc, theme.controlActive);
						ui::DrawBorder(batch, rc, theme.panelBorder);
					}
					const float indent = dpad * 3, sw = rc.h - dpad * 2;
					batch.DrawRect({rc.x + indent, rc.y + dpad, sw, sw},
								   items[r.index].swatch);
					m_font.Draw(batch, items[r.index].label, rc.x + indent + sw + dpad,
								ty, active ? theme.text : theme.textDim);
					break;
				}
				}
			}
			batch.SetScissor(nullptr);
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
