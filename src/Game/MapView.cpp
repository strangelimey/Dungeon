// ============================================================================
// Game/MapView.cpp — see MapView.h.
// ============================================================================
#include "Game/MapView.h"

#include "Core/Loc.h"
#include "Game/Entity.h"
#include "UI/Controls.h" // ui::DrawBorder

#include <algorithm>
#include <cmath>

namespace dungeon::game {

namespace {
constexpr float kPi = 3.14159265f;

// Map palette (stylized, not the UI theme — these are the dungeon's own ink).
const Vec4 kMapBg{0.04f, 0.04f, 0.06f, 0.96f}; // panel fill (near-opaque)
const Vec4 kWall{0.46f, 0.42f, 0.36f, 1.0f};   // solid structure (the bright ink)
const Vec4 kFloor{0.13f, 0.13f, 0.16f, 1.0f};  // walkable (recedes)
const Vec4 kTorch{1.0f, 0.62f, 0.28f, 1.0f};
const Vec4 kBrazier{1.0f, 0.45f, 0.16f, 1.0f};
const Vec4 kMonster{0.85f, 0.22f, 0.22f, 1.0f};
const Vec4 kItem{0.42f, 0.85f, 0.42f, 1.0f};
const Vec4 kButton{0.42f, 0.62f, 0.95f, 1.0f};

// Font px at the design window height (re-baked to track the real height).
constexpr float kFontH = 18.0f;
} // namespace

MapView::MapView(gfx::GraphicsDevice& device, DungeonWorld& world)
	: m_device(device), m_world(world), m_font(device, "", kFontH) {}

MapView::Transform MapView::ComputeTransform(const gfx::Rect& panel) const {
	const DungeonMap& map = m_world.Map();
	const float mw = static_cast<float>(map.Width());
	const float mh = static_cast<float>(map.Height());
	const float fit = std::min(panel.w / mw, panel.h / mh); // whole map fits at zoom 1
	const float cell = fit * m_zoom;
	const float gridW = mw * cell, gridH = mh * cell;
	const float ox = panel.x + (panel.w - gridW) * 0.5f + m_pan.x * panel.w;
	const float oy = panel.y + (panel.h - gridH) * 0.5f + m_pan.y * panel.h;
	return {cell, ox, oy};
}

MapView::Tool MapView::ToolForButton(int index) {
	switch (index) {
	case 1:  return Tool::PaintFloor;
	case 2:  return Tool::PaintWall;
	default: return Tool::None;
	}
}

const char* MapView::ToolLabelKey(Tool tool) {
	switch (tool) {
	case Tool::PaintFloor: return "map.tool.floor";
	case Tool::PaintWall:  return "map.tool.wall";
	default:               return "map.tool.view";
	}
}

gfx::Rect MapView::ButtonRect(const gfx::Rect& panel, int index) const {
	const float h = std::clamp(panel.h * 0.05f, 16.0f, 40.0f);
	const float w = std::clamp(panel.w * 0.13f, 64.0f, 180.0f);
	const float pad = std::clamp(panel.w * 0.012f, 4.0f, 16.0f);
	return {panel.x + pad + index * (w + pad), panel.y + pad, w, h};
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
	const bool inside = panel.Contains(mx, my);
	const DungeonMap& map = m_world.Map();

	// Wheel zooms about the cursor: keep the map point under the pointer fixed.
	if (inside && input.WheelDelta() != 0.0f && map.Width() > 0) {
		const Transform t0 = ComputeTransform(panel);
		const float fx = (mx - t0.ox) / t0.cell; // map point (in cells) at cursor
		const float fz = (my - t0.oy) / t0.cell;
		m_zoom = std::clamp(m_zoom * std::pow(1.2f, input.WheelDelta()), 1.0f, 10.0f);
		const float fit = std::min(panel.w / map.Width(), panel.h / map.Height());
		const float cell = fit * m_zoom;
		const float gridW = map.Width() * cell, gridH = map.Height() * cell;
		m_pan.x = (mx - fx * cell - panel.x - (panel.w - gridW) * 0.5f) / panel.w;
		m_pan.y = (my - fz * cell - panel.y - (panel.h - gridH) * 0.5f) / panel.h;
	}

	// Tool palette: a left-click on a button selects its tool and claims the
	// click (so it never also pans or paints).
	if (inside && input.WasMousePressed(MouseButton::Left)) {
		for (int i = 0; i < kToolCount; ++i) {
			if (ButtonRect(panel, i).Contains(mx, my)) {
				m_tool = ToolForButton(i);
				return true;
			}
		}
	}

	// Pan with the button that isn't painting: left in view mode, right while a
	// paint tool is armed (so the left button is free to paint).
	const MouseButton panBtn =
		m_tool == Tool::None ? MouseButton::Left : MouseButton::Right;
	if (inside && input.WasMousePressed(panBtn)) {
		m_panning = true;
		m_lastMouse = {mx, my};
	}
	if (m_panning && input.IsMouseDown(panBtn)) {
		m_pan.x += (mx - m_lastMouse.x) / panel.w;
		m_pan.y += (my - m_lastMouse.y) / panel.h;
		m_lastMouse = {mx, my};
	}
	if (input.WasMouseReleased(panBtn)) m_panning = false;

	// Paint while the left button is held (EditCell no-ops on unchanged cells,
	// so holding over one cell is cheap; dragging paints a stroke).
	if (m_tool != Tool::None && inside && input.IsMouseDown(MouseButton::Left)) {
		int cx, cz;
		if (CellAt(mx, my, panel, cx, cz)) {
			const Cell target =
				m_tool == Tool::PaintWall ? Cell::Wall : Cell::Floor;
			const Party& party = m_world.GetParty();
			const bool wouldTrapParty = target == Cell::Wall &&
										cx == party.GridX() && cz == party.GridZ();
			if (!wouldTrapParty) m_world.EditCell(cx, cz, target);
		}
	}

	return inside;
}

void MapView::Render(gfx::SpriteBatch& batch, const ui::Theme& theme,
					 const gfx::Rect& panel) {
	if (!m_open) return;

	const DungeonMap& map = m_world.Map();
	const Transform t = ComputeTransform(panel);

	// Panel base + a one-cell-inset interior the grid is clipped to, so a
	// panned/zoomed map never spills over the frame.
	batch.DrawRect(panel, kMapBg);
	const gfx::Rect interior{panel.x + 2, panel.y + 2, panel.w - 4, panel.h - 4};
	batch.SetScissor(&interior);

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

	// 1) Floors and walls (only fog-of-war-revealed cells).
	for (int z = 0; z < map.Height(); ++z)
		for (int x = 0; x < map.Width(); ++x) {
			if (!m_world.IsSeen(x, z)) continue;
			batch.DrawRect(cellRect(x, z),
						   map.At(x, z) == Cell::Wall ? kWall : kFloor);
		}

	// 2) Start cell — an accent outline.
	if (m_world.IsSeen(map.StartX(), map.StartZ()))
		ui::DrawBorder(batch, cellRect(map.StartX(), map.StartZ()), theme.accent);

	// 3) Fixtures (from the static map).
	for (const auto& [x, z] : map.TorchCells())
		if (m_world.IsSeen(x, z)) marker(x, z, 0.32f, kTorch);
	for (const auto& [x, z] : map.BrazierCells())
		if (m_world.IsSeen(x, z)) marker(x, z, 0.46f, kBrazier);

	// 4) Dynamic entities (from the .ent layer).
	for (const Entity& e : m_world.Entities().All()) {
		if (!m_world.IsSeen(e.x, e.z)) continue;
		switch (e.kind) {
		case EntityKind::Monster: marker(e.x, e.z, 0.5f, kMonster); break;
		case EntityKind::Item:    marker(e.x, e.z, 0.34f, kItem); break;
		case EntityKind::Button:  marker(e.x, e.z, 0.3f, kButton); break;
		case EntityKind::Decoration: break; // structural flavor; skip on the map
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

	// Tool palette (top-left): a button per tool, the active one highlighted.
	for (int i = 0; i < kToolCount; ++i) {
		const Tool tool = ToolForButton(i);
		const gfx::Rect b = ButtonRect(panel, i);
		batch.DrawRect(b, tool == m_tool ? theme.controlActive : theme.control);
		ui::DrawBorder(batch, b, theme.panelBorder);
		const std::string label = loc::Tr(ToolLabelKey(tool));
		m_font.Draw(batch, label, b.x + (b.w - m_font.MeasureWidth(label)) * 0.5f,
					b.y + (b.h - m_font.Height()) * 0.5f,
					tool == m_tool ? theme.text : theme.textDim);
	}

	// Footer: pan/zoom hint (left) + party cell (right).
	const float pad = std::clamp(panel.w * 0.012f, 4.0f, 16.0f);
	const float footY = panel.y + panel.h - m_font.Height() - pad;
	m_font.Draw(batch, loc::Tr("map.hint"), panel.x + pad, footY, theme.textDim);

	const Party& party = m_world.GetParty();
	const std::string pos =
		loc::Format("map.position", party.GridX(), party.GridZ());
	m_font.Draw(batch, pos, panel.x + panel.w - m_font.MeasureWidth(pos) - pad,
				footY, theme.textDim);
}

} // namespace dungeon::game
