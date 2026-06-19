// ============================================================================
// Game/MapEditor.cpp — see MapEditor.h.
//
// The editor-only half of the map overlay: the left-dock brush palette and the
// brush-apply logic. MapView (the shared viewport) drives this while in Editor
// mode — it draws the dock frame and does the grid hit-test, then calls in here
// to fill the palette body, resolve clicks, and paint cells.
// ============================================================================
#include "Game/MapEditor.h"

#include "Core/Loc.h"
#include "Game/DungeonWorld.h"
#include "Game/Entity.h"
#include "Game/GameSettings.h"
#include "Game/MapColors.h"
#include "Game/MapView.h"
#include "UI/Controls.h" // ui::DrawBorder
#include "UI/Font.h"

#include <algorithm>
#include <format>

namespace dungeon::game {

namespace {
// One source of truth for each palette category: its display loc key, the
// project catalog it authors into ("" = not creatable), and whether that catalog
// is a texture set (folder import) vs a model. Indexed by PaletteCat, in enum
// order (the static_assert guards drift).
struct CatInfo {
	const char* nameKey;
	const char* catalogKey;
	bool textureSet;
};
constexpr CatInfo kCategoryInfo[] = {
	{"map.cat.tools", "", false},           {"map.cat.structure", "", false},
	{"map.cat.walls", "walls", true},       {"map.cat.floors", "floors", true},
	{"map.cat.ceilings", "ceilings", true}, {"map.cat.decorations", "decorations", false},
	{"map.cat.fixtures", "fixtures", false}, {"map.cat.monsters", "monsters", false},
	{"map.cat.doors", "doors", false},      {"map.cat.stairs", "stairs", false},
	{"map.cat.items", "items", false},
};
static_assert(sizeof(kCategoryInfo) / sizeof(kCategoryInfo[0]) ==
				  static_cast<size_t>(MapEditor::PaletteCat::Count),
			  "kCategoryInfo must have one row per PaletteCat");
const CatInfo& CatInfoFor(MapEditor::PaletteCat cat) {
	return kCategoryInfo[static_cast<size_t>(cat)];
}
} // namespace

MapEditor::MapEditor(MapView& view, DungeonWorld& world, GameSettings& settings)
	: m_view(view), m_world(world), m_settings(settings) {
	// Open the most-used categories by default; the rest start collapsed.
	m_catOpen[static_cast<size_t>(PaletteCat::Tools)] = true;
	m_catOpen[static_cast<size_t>(PaletteCat::Structure)] = true;
	m_catOpen[static_cast<size_t>(PaletteCat::Walls)] = true;
}

const char* MapEditor::CategoryNameKey(PaletteCat cat) { return CatInfoFor(cat).nameKey; }
const char* MapEditor::CategoryCatalogKey(PaletteCat cat) { return CatInfoFor(cat).catalogKey; }
bool MapEditor::CategoryTextureSet(PaletteCat cat) { return CatInfoFor(cat).textureSet; }

// Resolves a category's items: built-in tools/structure, the level's surface
// palette (Walls/Floors/Ceilings, display names from the project's surface
// catalogs), or the project's entity catalogs.
std::vector<MapEditor::PaletteItem> MapEditor::CategoryItems(PaletteCat cat) const {
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

void MapEditor::BuildPaletteRows(const gfx::Rect& panel, std::vector<PaletteRow>& out,
								 float& contentHeight) const {
	out.clear();
	const gfx::Rect body = m_view.PaletteBody(panel);
	const float pad = MapView::DockPad(panel);
	const float headerH = std::clamp(panel.h * 0.045f, 22.0f, 42.0f);
	const float itemH = std::clamp(panel.h * 0.040f, 20.0f, 36.0f);

	float y = body.y - m_paletteScroll;
	for (int c = 0; c < static_cast<int>(PaletteCat::Count); ++c) {
		const PaletteCat cat = static_cast<PaletteCat>(c);
		out.push_back({PaletteRow::Kind::Header, cat, -1, {body.x, y, body.w, headerH}});
		y += headerH;
		if (m_catOpen[c]) {
			if (Creatable(cat)) {
				out.push_back({PaletteRow::Kind::NewButton, cat, -1,
							   {body.x, y, body.w, itemH}});
				y += itemH;
			}
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

void MapEditor::OnWheel(float delta, const gfx::Rect& panel) {
	std::vector<PaletteRow> rows;
	float content = 0.0f;
	BuildPaletteRows(panel, rows, content);
	const float maxScroll = std::max(0.0f, content - m_view.PaletteBody(panel).h);
	m_paletteScroll = std::clamp(m_paletteScroll - delta * 28.0f, 0.0f, maxScroll);
}

bool MapEditor::OnClick(float mx, float my, const gfx::Rect& panel) {
	std::vector<PaletteRow> rows;
	float content = 0.0f;
	BuildPaletteRows(panel, rows, content);
	for (const PaletteRow& r : rows) {
		if (!r.rect.Contains(mx, my)) continue;
		if (r.kind == PaletteRow::Kind::Header)
			m_catOpen[static_cast<size_t>(r.cat)] = !m_catOpen[static_cast<size_t>(r.cat)];
		else if (r.kind == PaletteRow::Kind::Item)
			m_sel = {r.cat, r.index};
		else if (r.kind == PaletteRow::Kind::NewButton && onNewAsset)
			onNewAsset(r.cat);
		return true;
	}
	return false;
}

void MapEditor::ApplyBrush(int cx, int cz, bool dragging) {
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
			for (const auto& m : m_world.DecorationMarkers())
				if (m.x == cx && m.z == cz) ++props;
			int mons = 0;
			for (const auto& m : m_world.MonsterMarkers())
				if (m.x == cx && m.z == cz) ++mons;
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
	case PaletteCat::Monsters:
	case PaletteCat::Fixtures: {
		if (dragging) break; // placement is a single click
		const std::vector<PaletteItem> items = CategoryItems(m_sel.cat);
		if (m_sel.index < 0 || m_sel.index >= static_cast<int>(items.size())) break;
		const std::string& id = items[m_sel.index].id;
		bool ok = false;
		if (m_sel.cat == PaletteCat::Monsters)
			ok = m_world.AddMonster(id, cx, cz, Direction::South);
		else if (m_sel.cat == PaletteCat::Fixtures)
			ok = m_world.AddFixture(id, cx, cz);
		else
			ok = m_world.AddDecoration(id, cx, cz, Direction::South);
		log(loc::Format(ok ? "map.place.done" : "map.place.blocked",
						items[m_sel.index].label));
		break;
	}
	default: { // Doors/Stairs/Items — placement wiring lands later
		if (dragging) break;
		const std::vector<PaletteItem> items = CategoryItems(m_sel.cat);
		if (m_sel.index >= 0 && m_sel.index < static_cast<int>(items.size()))
			log(loc::Format("map.place.todo", items[m_sel.index].label));
		break;
	}
	}
}

void MapEditor::RenderBody(gfx::SpriteBatch& batch, const ui::Theme& theme,
						   const gfx::Rect& panel) {
	ui::Font& font = m_view.Font();
	const float dpad = MapView::DockPad(panel);
	const gfx::Rect body = m_view.PaletteBody(panel);
	batch.SetScissor(&body);

	std::vector<PaletteRow> rows;
	float content = 0.0f;
	BuildPaletteRows(panel, rows, content);
	const float arrowW = font.MeasureWidth("+");
	std::vector<PaletteItem> items; // the current category's items
	for (const PaletteRow& r : rows) {
		const gfx::Rect& rc = r.rect;
		if (r.kind == PaletteRow::Kind::Header) items = CategoryItems(r.cat);
		if (rc.y + rc.h < body.y || rc.y > body.y + body.h) continue; // off-view
		const float ty = rc.y + (rc.h - font.Height()) * 0.5f;
		switch (r.kind) {
		case PaletteRow::Kind::Header: {
			batch.DrawRect(rc, theme.control);
			ui::DrawBorder(batch, rc, theme.panelBorder);
			const char* arrow = m_catOpen[static_cast<size_t>(r.cat)] ? "-" : "+";
			font.Draw(batch, arrow, rc.x + dpad, ty, theme.textDim);
			font.Draw(batch, loc::Tr(CategoryNameKey(r.cat)),
					  rc.x + dpad * 2 + arrowW, ty, theme.text);
			break;
		}
		case PaletteRow::Kind::NewButton:
			font.Draw(batch, loc::Tr("map.cat.new"), rc.x + dpad * 3, ty, theme.accent);
			break;
		case PaletteRow::Kind::Empty:
			font.Draw(batch, loc::Tr("map.cat.empty"), rc.x + dpad * 3, ty, theme.textDim);
			break;
		case PaletteRow::Kind::Item: {
			if (r.index < 0 || r.index >= static_cast<int>(items.size())) break;
			const bool active = m_sel.cat == r.cat && m_sel.index == r.index;
			if (active) {
				batch.DrawRect(rc, theme.controlActive);
				ui::DrawBorder(batch, rc, theme.panelBorder);
			}
			const float indent = dpad * 3, sw = rc.h - dpad * 2;
			batch.DrawRect({rc.x + indent, rc.y + dpad, sw, sw}, items[r.index].swatch);
			font.Draw(batch, items[r.index].label, rc.x + indent + sw + dpad, ty,
					  active ? theme.text : theme.textDim);
			break;
		}
		}
	}
	batch.SetScissor(nullptr);
}

} // namespace dungeon::game
