// ============================================================================
// Game/MapEditor_Shapes.cpp - the SHAPE BRUSHES (docs/tool-refinement-plan.md
// Phase 6): corridor, room, stamp and region, each laid in the current style.
//
// Carve.h decides WHICH squares; this file decides what that means for the
// level: open the rock, raise a stamp's pillars, paint the style's corridor or
// room theme on what opened and on the rock around it - as one undo step and
// one chunk batch, the way every multi-square fill in the editor goes.
// ============================================================================
#include "Game/MapEditor.h"

#include "Core/Loc.h"
#include "Game/Generate.h"
#include "Game/GenerateKnobs.h"
#include "Game/MapView.h"

#include <algorithm>
#include <set>

namespace dungeon::game {

namespace {
// The style's shape knobs, read the way the Generate dialog reads a preset.
generate::Params KnobsOf(const CatalogEntry* style) {
	generate::Params p;
	if (style) generate::Decode(style->Get("knobs", ""), p);
	return p;
}
} // namespace

bool MapEditor::UseShapeRow(const std::string& id) {
	if (!m_world->GetProject().shapes.Contains(id)) return false;
	if (m_stamp == id) {
		m_stamp.clear();
		return true;
	}
	m_stamp = id;
	m_stampTurns = 0;
	SetTool(Tool::Stamp);
	return true;
}

const CatalogEntry* MapEditor::StyleEntry() const {
	return m_style.empty() ? nullptr : m_world->GetProject().styles.Find(m_style);
}

carve::Shape MapEditor::CorridorShape(int ax, int az, int bx, int bz) const {
	const CatalogEntry* style = StyleEntry();
	const int width = style ? static_cast<int>(style->GetFloat("corridor_width", 1.0f) + 0.5f) : 1;
	return carve::Corridor(ax, az, bx, bz, width, KnobsOf(style).winding, m_shapeSeed);
}

carve::Shape MapEditor::RoomShape(int ax, int az, int bx, int bz) const {
	return carve::Room(ax, az, bx, bz);
}

carve::Shape MapEditor::StampShape(int cx, int cz) const {
	const CatalogEntry* e = m_world->GetProject().shapes.Find(m_stamp);
	if (!e) return {};
	return carve::StampAt(carve::ParseStamp(e->Get("rows", "")), cx, cz, m_stampTurns);
}

carve::Shape MapEditor::RegionShape(int ax, int az, int bx, int bz) const {
	const int x0 = std::min(ax, bx), z0 = std::min(az, bz);
	const int w = std::abs(bx - ax) + 1, h = std::abs(bz - az) + 1;
	if (w < 6 || h < 6) return {};
	// THE GENERATOR, sized to the rectangle plus the one-square rock rim it
	// keeps, with the style's recipe - and nothing to populate it with: a region
	// is SHAPE, and populating is its own stage (Phase 7).
	generate::Params p = KnobsOf(StyleEntry());
	p.width = w + 2;
	p.height = h + 2;
	p.seed = m_shapeSeed;
	p.locks = 0;
	p.entryX = p.entryZ = -1;
	p.keepOpen.clear();
	p.monsterIds.clear();
	p.monsterThreat.clear();
	p.lootIds.clear();
	p.keyIds.clear();
	const generate::Level gen = generate::Run(p);
	// The rectangle's own squares of what came back (inside the rim).
	std::vector<u8> floor(static_cast<size_t>(w) * h, 0);
	for (int z = 0; z < h; ++z)
		for (int x = 0; x < w; ++x) floor[static_cast<size_t>(z) * w + x] = gen.At(x + 1, z + 1) ? 1 : 0;
	const DungeonMap& map = m_view.ViewedMap();
	return carve::Region(floor, w, h, x0, z0,
						 [&](int x, int z) { return map.IsWalkable(x, z); }, m_shapeSeed);
}

MapEditor::ShapeResult MapEditor::ApplyShape(const carve::Shape& shape) {
	ShapeResult r;
	if (shape.open.empty() && shape.solid.empty()) return r;
	const DungeonMap& map = m_view.ViewedMap();
	const bool remote = m_view.Browsing();
	const std::string stem = m_view.ViewedLevel(); // a copy: nothing here re-browses
	// Never the map's own edge: the rock rim keeps the level closed.
	const auto inside = [&](int x, int z) {
		return x >= 1 && z >= 1 && x < map.Width() - 1 && z < map.Height() - 1;
	};
	const CatalogEntry* style = StyleEntry();
	const Project& proj = m_world->GetProject();
	const auto themeFor = [&](carve::Role role) -> std::string {
		if (!style) return {};
		const std::string t = style->Get(role == carve::Role::Corridor ? "corridor" : "room", "");
		return proj.themes.Contains(t) ? t : std::string();
	};
	// The squares' state AFTER the edit, read from what was there and what this
	// does to it - a browsed level's snapshot does not show the change yet.
	std::set<std::pair<int, int>> opened, raised;
	for (const carve::Square& q : shape.open)
		if (inside(q.x, q.z)) opened.insert({q.x, q.z});
	const Party& party = m_world->GetParty();
	for (const auto& [x, z] : shape.solid)
		if (inside(x, z) && !opened.count({x, z}) &&
			!(!remote && x == party.GridX() && z == party.GridZ())) // never wall the party in
			raised.insert({x, z});
	const auto openAfter = [&](int x, int z) {
		if (raised.count({x, z})) return false;
		return opened.count({x, z}) > 0 || map.IsWalkable(x, z);
	};

	m_world->BeginUndoStep();
	const u32 rev0 = m_world->Map().Revision();
	m_world->BeginChunkBatch(); // each touched chunk rebuilds once, at the end
	for (const carve::Square& q : shape.open) {
		if (!inside(q.x, q.z)) continue;
		if (!map.IsWalkable(q.x, q.z)) {
			if (remote) m_world->EditCellRemote(stem, q.x, q.z, Cell::Floor);
			else m_world->EditCell(q.x, q.z, Cell::Floor);
			++r.opened;
		}
		if (const std::string t = themeFor(q.role); !t.empty()) {
			PaintThemeAs(t, q.x, q.z, /*open*/ true, remote, stem);
			++r.painted;
		}
	}
	for (const auto& [x, z] : raised) {
		if (!map.IsWalkable(x, z)) continue; // already rock
		if (remote) m_world->EditCellRemote(stem, x, z, Cell::Wall);
		else m_world->EditCell(x, z, Cell::Wall);
		++r.raised;
	}
	// THE WALLS: the rock round what opened - and a stamp's pillars - wear the
	// theme of the square they border.
	std::vector<carve::Square> walls = carve::Rim(shape);
	for (const auto& [x, z] : raised) walls.push_back({x, z, carve::Role::Room});
	for (const carve::Square& w : walls) {
		if (!inside(w.x, w.z) || openAfter(w.x, w.z)) continue;
		if (const std::string t = themeFor(w.role); !t.empty()) {
			PaintThemeAs(t, w.x, w.z, /*open*/ false, remote, stem);
			++r.painted;
		}
	}
	m_world->EndChunkBatch();
	m_world->CommitUndoStep(remote || m_world->Map().Revision() != rev0);
	++m_shapeSeed; // the next winding corridor or region is a new one
	if (m_world->onMessage)
		m_world->onMessage(loc::FormatLine("map.shape.done", r.opened, r.painted));
	return r;
}

} // namespace dungeon::game
