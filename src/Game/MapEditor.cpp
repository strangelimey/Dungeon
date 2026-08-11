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
#include "Game/DungeonMeshBuilder.h" // SurfaceVariantFor (eyedropper/flood key)
#include "Game/DungeonWorld.h"
#include "Game/Entity.h"
#include "Platform/Input.h" // the filter box consumes TypedChars/VK edges
#include "Game/GameSettings.h"
#include "Game/MapColors.h"
#include "Game/MapView.h"
#include "UI/Controls.h" // ui::DrawBorder
#include "UI/Font.h"

#include <algorithm>
#include <cctype>
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
	// Whether this category's types go INTO the level. Effects don't — they are
	// content you author and tune, not content you place — so their rows open
	// the type editor instead of arming a brush, and they offer no "+ New..."
	// (an effect needs a CLASS behind it, docs/effects.md).
	bool placeable = true;
};
constexpr CatInfo kCategoryInfo[] = {
	{"map.cat.walls", "walls", true},       {"map.cat.floors", "floors", true},
	{"map.cat.ceilings", "ceilings", true},
	{"map.cat.decorations", "decorations", false},
	{"map.cat.fixtures", "fixtures", false}, {"map.cat.monsters", "monsters", false},
	{"map.cat.buttons", "buttons", false},  {"map.cat.doors", "doors", false},
	{"map.cat.stairs", "stairs", false},    {"map.cat.items", "items", false},
	{"map.cat.weapons", "weapons", false},  {"map.cat.armor", "armor", false},
	{"map.cat.wallfeatures", "wallfeatures", false},
	{"map.cat.surfacefeatures", "surfacefeatures", false},
	{"map.cat.effects", "effects", false, /*placeable*/ false},
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
	// Open the most-used category by default; the rest start collapsed.
	m_catOpen[static_cast<size_t>(PaletteCat::Walls)] = true;
}

const char* MapEditor::CategoryNameKey(PaletteCat cat) { return CatInfoFor(cat).nameKey; }
const char* MapEditor::CategoryCatalogKey(PaletteCat cat) { return CatInfoFor(cat).catalogKey; }
bool MapEditor::CategoryTextureSet(PaletteCat cat) { return CatInfoFor(cat).textureSet; }
bool MapEditor::CategoryPlaceable(PaletteCat cat) { return CatInfoFor(cat).placeable; }

MapEditor::PaletteCat MapEditor::CatForCatalogKey(std::string_view catalogKey) {
	for (size_t i = 0; i < static_cast<size_t>(PaletteCat::Count); ++i)
		if (catalogKey == kCategoryInfo[i].catalogKey)
			return static_cast<PaletteCat>(i);
	return PaletteCat::Count;
}

// Resolves a category's items: the level's surface palette (Walls/Floors/
// Ceilings, display names from the project's surface catalogs), or the
// project's entity catalogs.
std::vector<MapEditor::PaletteItem> MapEditor::CategoryItems(PaletteCat cat) const {
	// Surface palettes come from the VIEWED level (level browsing edits any
	// level, and each declares its own palette ids).
	const DungeonMap& map = m_view.ViewedMap();
	const Project& proj = m_world.GetProject();
	// The theme lens, from the VIEWED level for the same reason: browsing a
	// level should rank its palette by ITS theme, not by the party's.
	const std::vector<std::string>& theme = map.Theme();

	// A surface palette (list of catalog ids) resolved to display name + swatch;
	// the entry's `category` groups it under a sub-accordion like the entity
	// catalogs (an id the catalog doesn't know stays ungrouped). The swatch is
	// the entry's loaded albedo texture — the same one the map's cell fill
	// draws — with the flat category color as the not-loaded fallback (a
	// browsed level's foreign palette).
	auto surfaceItems = [&](const std::vector<std::string>& palette,
							const Catalog& catalog, const Vec4& swatch,
							DungeonWorld::SurfaceSel sel) {
		// The "Catalogue" toggle swaps the SOURCE of ids: the whole catalog
		// (minus hidden), or just the level's palette. Everything downstream
		// keys off the id, so the two views paint the same — a catalogue-view id
		// the level lacks is enrolled on first paint (EnsureSurfaceVariant).
		std::vector<std::string> ids;
		if (m_settings.mapShowCatalog)
			for (const CatalogEntry& e : catalog.Entries()) {
				if (CatalogBool(&e, "hidden", false)) continue;
				ids.push_back(e.id);
			}
		const std::vector<std::string>& source =
			m_settings.mapShowCatalog ? ids : palette;
		std::vector<PaletteItem> items;
		for (const std::string& id : source) {
			const CatalogEntry* e = catalog.Find(id);
			items.push_back({e ? e->Display() : id, swatch, id,
							 e ? e->Get("category", "") : std::string(),
							 m_world.SurfaceAlbedoForId(sel, id),
							 CatalogMatchesTags(e, theme)});
		}
		return items;
	};
	// An entity catalog resolved to display name + swatch + id. `hidden = 1`
	// entries are internal (e.g. the door frame the door types share) — they
	// resolve by id but never show as placeable. The `category` field, when an
	// entry carries one, groups it under a palette sub-accordion.
	auto catalogItems = [&](const Catalog& catalog, const Vec4& swatch) {
		std::vector<PaletteItem> items;
		for (const CatalogEntry& e : catalog.Entries()) {
			if (CatalogBool(&e, "hidden", false)) continue;
			items.push_back({e.Display(), swatch, e.id, e.Get("category", ""),
							 /*icon*/ nullptr, CatalogMatchesTags(&e, theme)});
		}
		return items;
	};

	switch (cat) {
	case PaletteCat::Walls:
		return surfaceItems(map.WallPalette(), proj.walls, kWall,
							DungeonWorld::SurfaceSel::Wall);
	case PaletteCat::Floors:
		return surfaceItems(map.FloorPalette(), proj.floors, kFloor,
							DungeonWorld::SurfaceSel::Floor);
	case PaletteCat::Ceilings:
		return surfaceItems(map.CeilingPalette(), proj.ceilings, kCeiling,
							DungeonWorld::SurfaceSel::Ceiling);
	case PaletteCat::Decorations: return catalogItems(proj.decorations, kDecoration);
	case PaletteCat::Fixtures:    return catalogItems(proj.fixtures, kTorch);
	case PaletteCat::Monsters:    return catalogItems(proj.monsters, kMonster);
	case PaletteCat::Buttons:     return catalogItems(proj.buttons, kButton);
	case PaletteCat::Doors:       return catalogItems(proj.doors, kDoor);
	case PaletteCat::Stairs:      return catalogItems(proj.stairs, kStair);
	case PaletteCat::Items:       return catalogItems(proj.items, kItem);
	case PaletteCat::Weapons:     return catalogItems(proj.weapons, kItem);
	case PaletteCat::Armor:       return catalogItems(proj.armor, kItem);
	case PaletteCat::WallFeatures: return catalogItems(proj.wallfeatures, kDecoration);
	case PaletteCat::SurfaceFeatures: return catalogItems(proj.surfacefeatures, kDecoration);
	case PaletteCat::Effects:     return catalogItems(proj.effects, kMonster);
	default:                      return {};
	}
}

// --- surface palette membership ---------------------------------------------

namespace {
// The surface selector behind a surface palette category.
DungeonWorld::SurfaceSel SelFor(MapEditor::PaletteCat cat) {
	return cat == MapEditor::PaletteCat::Walls	  ? DungeonWorld::SurfaceSel::Wall
		   : cat == MapEditor::PaletteCat::Floors ? DungeonWorld::SurfaceSel::Floor
												  : DungeonWorld::SurfaceSel::Ceiling;
}
} // namespace

void MapEditor::AddToPalette(PaletteCat cat, const std::string& id) {
	if (!SurfaceCat(cat)) return;
	auto log = [&](const std::string& s) {
		if (m_world.onMessage) m_world.onMessage(s);
	};
	const DungeonWorld::SurfaceSel sel = SelFor(cat);
	// The palette lives on the map, which the undo snapshot copies wholesale —
	// so bracketing here is all an undoable palette add needs.
	m_world.BeginUndoStep();
	const bool ok = m_view.Browsing()
						? m_world.AddPaletteEntryRemote(m_view.ViewedLevel(), sel, id)
						: m_world.AddPaletteEntry(sel, id);
	m_world.CommitUndoStep(ok);
	if (!ok) {
		log(loc::Format("map.palette.failed", id));
		return;
	}
	log(loc::Format("map.palette.added", id));
	// Arm the newcomer: it is the last row of its category, and painting it is
	// the reason the user added it.
	m_catOpen[static_cast<size_t>(cat)] = true;
	const std::vector<PaletteItem> items = CategoryItems(cat);
	for (int i = 0; i < static_cast<int>(items.size()); ++i)
		if (items[i].id == id) {
			m_sel = {cat, i};
			// A grouped item hides inside a collapsed sub-accordion; open it so
			// the armed row is visible.
			if (!items[i].group.empty())
				m_groupOpen[GroupKey(cat, items[i].group)] = true;
			break;
		}
}

// --- palette controls row (filter + clear + collapse-all) --------------------

gfx::Rect MapEditor::ControlsRow(const gfx::Rect& panel) const {
	const gfx::Rect body = m_view.PaletteBody(panel);
	const float h = std::clamp(panel.h * 0.040f, 20.0f, 36.0f);
	return {body.x, body.y, body.w, h};
}

gfx::Rect MapEditor::CollapseAllRect(const gfx::Rect& panel) const {
	const gfx::Rect row = ControlsRow(panel);
	return {row.x + row.w - row.h, row.y, row.h, row.h}; // square, right end
}

gfx::Rect MapEditor::FilterClearRect(const gfx::Rect& panel) const {
	const gfx::Rect c = CollapseAllRect(panel);
	const float pad = MapView::DockPad(panel);
	return {c.x - pad - c.h, c.y, c.h, c.h}; // square, left of collapse-all
}

gfx::Rect MapEditor::FilterBoxRect(const gfx::Rect& panel) const {
	const gfx::Rect row = ControlsRow(panel);
	const gfx::Rect clear = FilterClearRect(panel);
	const float pad = MapView::DockPad(panel);
	return {row.x, row.y, clear.x - pad - row.x, row.h};
}

gfx::Rect MapEditor::CatalogToggleRect(const gfx::Rect& panel) const {
	const gfx::Rect row = ControlsRow(panel);
	const float pad = MapView::DockPad(panel);
	return {row.x, row.y + row.h + pad, row.w, row.h};
}

gfx::Rect MapEditor::AccordionBody(const gfx::Rect& panel) const {
	const gfx::Rect body = m_view.PaletteBody(panel);
	const gfx::Rect toggle = CatalogToggleRect(panel);
	const float used = (toggle.y + toggle.h + MapView::DockPad(panel)) - body.y;
	return {body.x, body.y + used, body.w, body.h - used};
}

bool MapEditor::MatchesFilter(const std::string& label) const {
	if (m_filter.empty()) return true;
	auto lower = [](const std::string& s) {
		std::string out = s;
		for (char& ch : out)
			ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		return out;
	};
	return lower(label).find(lower(m_filter)) != std::string::npos;
}

void MapEditor::HandleTyping(const Input& input) {
	if (!m_filterFocused) return;
	bool edited = false;
	for (const char c : input.TypedChars()) {
		if (static_cast<unsigned char>(c) < 0x20) continue; // printable only
		if (m_filter.size() >= 24) break;
		m_filter.push_back(c);
		edited = true;
	}
	if (input.WasKeyPressed(vk::Back) && !m_filter.empty()) {
		m_filter.pop_back();
		edited = true;
	}
	// Esc/Enter release the keyboard back to the game (Game gates the party
	// keys and its own Esc/M on KeyboardCaptured while we hold it).
	if (input.WasKeyPressed(vk::Escape) || input.WasKeyPressed(vk::Return))
		m_filterFocused = false;
	if (edited) m_paletteScroll = 0.0f; // a changed filter restarts at the top
}

void MapEditor::TrackMouse(float mx, float my, const gfx::Rect& panel) {
	m_hotCtrl = FilterBoxRect(panel).Contains(mx, my)     ? HotCtrl::Filter
				: FilterClearRect(panel).Contains(mx, my) ? HotCtrl::Clear
				: CollapseAllRect(panel).Contains(mx, my) ? HotCtrl::Collapse
				: CatalogToggleRect(panel).Contains(mx, my) ? HotCtrl::Catalog
															: HotCtrl::None;
}

void MapEditor::BuildPaletteRows(const gfx::Rect& panel, std::vector<PaletteRow>& out,
								 float& contentHeight) const {
	out.clear();
	const gfx::Rect body = AccordionBody(panel);
	const float pad = MapView::DockPad(panel);
	const float headerH = std::clamp(panel.h * 0.045f, 22.0f, 42.0f);
	const float itemH = std::clamp(panel.h * 0.040f, 20.0f, 36.0f);
	// While a filter is set, matching items list FLAT under their category
	// header regardless of accordion/group state (a filter over collapsed
	// accordions would otherwise show nothing), "+ New..." rows hide, and a
	// category with no matches drops out entirely.
	const bool filtering = !m_filter.empty();

	float y = body.y - m_paletteScroll;
	for (int c = 0; c < static_cast<int>(PaletteCat::Count); ++c) {
		const PaletteCat cat = static_cast<PaletteCat>(c);
		const std::vector<PaletteItem> items = CategoryItems(cat);
		if (filtering) {
			std::vector<int> matches;
			for (int i = 0; i < static_cast<int>(items.size()); ++i)
				if (MatchesFilter(items[i].label)) matches.push_back(i);
			if (matches.empty()) continue; // category drops out
			out.push_back({PaletteRow::Kind::Header, cat, -1,
						   {body.x, y, body.w, headerH}});
			y += headerH;
			for (const int i : matches) {
				out.push_back({PaletteRow::Kind::Item, cat, i,
							   {body.x, y, body.w, itemH}, std::string()});
				y += itemH;
			}
			y += pad;
			continue;
		}
		out.push_back({PaletteRow::Kind::Header, cat, -1, {body.x, y, body.w, headerH}});
		y += headerH;
		if (m_catOpen[c]) {
			if (Creatable(cat)) {
				out.push_back({PaletteRow::Kind::NewButton, cat, -1,
							   {body.x, y, body.w, itemH}});
				y += itemH;
			}
			if (items.empty()) {
				out.push_back({PaletteRow::Kind::Empty, cat, -1, {body.x, y, body.w, itemH}});
				y += itemH;
			} else {
				// Sub-accordions: ungrouped items list first, then each group
				// (first-appearance order) under a collapsible sub-header whose
				// body only lays out while open. Item indices stay CategoryItems
				// positions, so the armed selection and dispatch are untouched
				// by the display grouping.
				auto itemRow = [&](int i) {
					out.push_back({PaletteRow::Kind::Item, cat, i,
								   {body.x, y, body.w, itemH}, items[i].group});
					y += itemH;
				};
				// The THEME LENS, applied per run (the ungrouped items, and each
				// open group's body): on-theme first, then a divider, then the
				// rest. Indices are what get reordered, never `items` — the armed
				// selection and every dispatch below address CategoryItems
				// POSITIONS, so sorting the vector itself would silently re-point
				// the brush at whatever slid into its index.
				auto run = [&](auto&& belongs) {
					std::vector<int> idx;
					for (int i = 0; i < static_cast<int>(items.size()); ++i)
						if (belongs(items[i])) idx.push_back(i);
					const auto off = std::stable_partition(
						idx.begin(), idx.end(),
						[&](int i) { return items[i].onTheme; });
					for (auto it = idx.begin(); it != idx.end(); ++it) {
						// Only between the two groups, and only when there ARE two.
						if (it == off && it != idx.begin()) {
							out.push_back({PaletteRow::Kind::Divider, cat, -1,
										   {body.x, y, body.w, itemH * 0.5f}});
							y += itemH * 0.5f;
						}
						itemRow(*it);
					}
				};
				std::vector<std::string> groups;
				for (const PaletteItem& it : items)
					if (!it.group.empty() &&
						std::find(groups.begin(), groups.end(), it.group) == groups.end())
						groups.push_back(it.group);
				// GROUPS take the lens too, and this is the half that does the
				// work: `category` and `tags` correlate hard on real content (the
				// Skeleton group is exactly the undead ones), so ranking only
				// WITHIN a group leaves every group uniformly on- or off-theme
				// and the item divider never fires. A group is on-theme if ANY
				// member is — the question being asked of a collapsed group is
				// "is there anything for me in here".
				const auto offGroup = std::stable_partition(
					groups.begin(), groups.end(), [&](const std::string& g) {
						return std::any_of(items.begin(), items.end(),
										   [&](const PaletteItem& it) {
											   return it.group == g && it.onTheme;
										   });
					});
				run([](const PaletteItem& it) { return it.group.empty(); });
				for (auto g = groups.begin(); g != groups.end(); ++g) {
					if (g == offGroup && g != groups.begin()) {
						out.push_back({PaletteRow::Kind::Divider, cat, -1,
									   {body.x, y, body.w, itemH * 0.5f}});
						y += itemH * 0.5f;
					}
					out.push_back({PaletteRow::Kind::SubHeader, cat, -1,
								   {body.x, y, body.w, itemH}, *g});
					y += itemH;
					if (GroupOpen(cat, *g))
						run([&](const PaletteItem& it) { return it.group == *g; });
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
	const float maxScroll = std::max(0.0f, content - AccordionBody(panel).h);
	m_paletteScroll = std::clamp(m_paletteScroll - delta * 28.0f, 0.0f, maxScroll);
}

bool MapEditor::OnClick(float mx, float my, const gfx::Rect& panel) {
	// Controls row first: the filter box takes focus, [x] clears, [-]
	// collapses every accordion (and sub-group). Any other palette click
	// releases the filter's keyboard capture.
	if (FilterBoxRect(panel).Contains(mx, my)) {
		m_filterFocused = true;
		return true;
	}
	m_filterFocused = false;
	if (FilterClearRect(panel).Contains(mx, my)) {
		m_filter.clear();
		m_paletteScroll = 0.0f;
		return true;
	}
	if (CollapseAllRect(panel).Contains(mx, my)) {
		m_catOpen.fill(false);
		m_groupOpen.clear(); // groups default collapsed
		m_paletteScroll = 0.0f;
		return true;
	}
	// The "Catalogue" checkbox: surfaces show the whole catalog vs the level's
	// palette. The armed selection is a ROW index, and the row set differs
	// between the two views, so keep the same TYPE armed across the toggle
	// (or disarm if it isn't shown in the new view).
	if (CatalogToggleRect(panel).Contains(mx, my)) {
		std::string armedId;
		if (m_sel.index >= 0 && SurfaceCat(m_sel.cat)) {
			const std::vector<PaletteItem> before = CategoryItems(m_sel.cat);
			if (m_sel.index < static_cast<int>(before.size()))
				armedId = before[m_sel.index].id;
		}
		m_settings.mapShowCatalog = !m_settings.mapShowCatalog;
		m_settings.Save(); // a workflow preference, persisted like the dock flags
		m_paletteScroll = 0.0f;
		if (!armedId.empty()) {
			const std::vector<PaletteItem> after = CategoryItems(m_sel.cat);
			m_sel.index = -1;
			for (int i = 0; i < static_cast<int>(after.size()); ++i)
				if (after[i].id == armedId) { m_sel.index = i; break; }
		}
		return true;
	}
	std::vector<PaletteRow> rows;
	float content = 0.0f;
	BuildPaletteRows(panel, rows, content);
	for (const PaletteRow& r : rows) {
		if (!r.rect.Contains(mx, my)) continue;
		if (r.kind == PaletteRow::Kind::Header)
			m_catOpen[static_cast<size_t>(r.cat)] = !m_catOpen[static_cast<size_t>(r.cat)];
		else if (r.kind == PaletteRow::Kind::SubHeader)
			m_groupOpen[GroupKey(r.cat, r.group)] = !GroupOpen(r.cat, r.group);
		else if (r.kind == PaletteRow::Kind::Item) {
			// A placeable type arms the brush; a non-placeable one has nothing
			// to arm, so a click opens its editor (what right-click does for
			// every row) rather than silently doing nothing.
			if (CategoryPlaceable(r.cat)) m_sel = {r.cat, r.index};
			else if (onConfigure) {
				const std::vector<PaletteItem> items = CategoryItems(r.cat);
				if (r.index >= 0 && r.index < static_cast<int>(items.size()))
					onConfigure(r.cat, items[r.index].id);
			}
		}
		else if (r.kind == PaletteRow::Kind::NewButton && onNewAsset)
			onNewAsset(r.cat);
		return true;
	}
	return false;
}

bool MapEditor::OnRightClick(float mx, float my, const gfx::Rect& panel) {
	std::vector<PaletteRow> rows;
	float content = 0.0f;
	BuildPaletteRows(panel, rows, content);
	for (const PaletteRow& r : rows) {
		if (!r.rect.Contains(mx, my)) continue;
		// Any item row opens its per-type editor (the owner routes it through the
		// schema-driven TypeEditorDialog — EVERY category is configurable now, so
		// there is no per-category allowlist here any more).
		if (r.kind == PaletteRow::Kind::Item && onConfigure) {
			const std::vector<PaletteItem> items = CategoryItems(r.cat);
			if (r.index >= 0 && r.index < static_cast<int>(items.size()))
				onConfigure(r.cat, items[r.index].id);
		}
		return true; // any row in the dock body consumes the right-click
	}
	return false;
}

// The armed brush's mount, from the type's own `mount` field or its category's
// default (Placement.h). Floor when nothing is armed — the caller then has no
// placement to make anyway.
Mount MapEditor::BrushMount() const {
	if (m_sel.index < 0 || !CategoryPlaceable(m_sel.cat)) return Mount::Floor;
	const std::vector<PaletteItem> items = CategoryItems(m_sel.cat);
	if (m_sel.index >= static_cast<int>(items.size())) return Mount::Floor;
	const char* key = CategoryCatalogKey(m_sel.cat);
	const Catalog* cat = m_world.GetProject().CatalogForKey(key);
	return MountFor(key, cat ? cat->Find(items[m_sel.index].id) : nullptr);
}

bool MapEditor::BrushIsWallMounted() const { return BrushMount() == Mount::Wall; }

Placement MapEditor::ResolveBrush(int cx, int cz, const WallFace& face) const {
	return Resolve(m_view.ViewedMap(), BrushMount(), cx, cz, face);
}

void MapEditor::ApplyBrush(int cx, int cz, bool dragging, const WallFace& face) {
	// Edit target: the VIEWED level. The active level edits live world state;
	// a browsed level routes to DungeonWorld's remote seam (its in-memory
	// stash — see the level-browsing section in MapView.h).
	const bool remote = m_view.Browsing();
	const std::string& stem = m_view.ViewedLevel();

	// Laying a patrol route: a click appends the cell as a waypoint instead of
	// painting the armed brush (a drag doesn't spam duplicates). Routes belong
	// to a LIVE monster, so clicks on a browsed level are ignored.
	if (m_routeId != 0) {
		if (!dragging && !remote && onRouteWaypoint) onRouteWaypoint(m_routeId, cx, cz);
		return;
	}
	if (m_sel.index < 0) return; // nothing armed yet
	using SS = DungeonWorld::SurfaceSel;
	auto log = [&](const std::string& s) {
		if (m_world.onMessage) m_world.onMessage(s);
	};

	// Undo bracketing: everything below mutates. A drag stroke is ONE undo
	// step — the snapshot is taken before the stroke's first cell; later
	// stroke cells fold into it. `changed` decides whether the pending
	// snapshot is kept: live paints compare the map revision, entity edits
	// report success, and remote edits are conservatively treated as changed
	// (a same-value remote paint costs one no-op undo step at worst).
	const bool strokeStart = !dragging;
	if (strokeStart) m_world.BeginUndoStep();
	const u32 rev0 = m_world.Map().Revision();
	bool changed = false;

	switch (m_sel.cat) {
	case PaletteCat::Walls:
	case PaletteCat::Floors:
	case PaletteCat::Ceilings: {
		PaintCell(cx, cz, remote, stem);
		// Remote edits are conservatively "changed" (see the bracket note).
		changed = remote || m_world.Map().Revision() != rev0;
		m_lastX = cx; // the shift-rectangle gesture anchors on the last paint
		m_lastZ = cz;
		break;
	}
	case PaletteCat::Decorations:
	case PaletteCat::Monsters:
	case PaletteCat::Buttons:
	case PaletteCat::Items:
	case PaletteCat::Weapons:
	case PaletteCat::Armor:
	case PaletteCat::WallFeatures:
	case PaletteCat::SurfaceFeatures:
	case PaletteCat::Fixtures: {
		if (dragging) break; // placement is a single click
		const std::vector<PaletteItem> items = CategoryItems(m_sel.cat);
		if (m_sel.index < 0 || m_sel.index >= static_cast<int>(items.size())) break;
		const std::string& id = items[m_sel.index].id;
		bool ok = false;
		// THE SAME resolver the hover ghost drew from, so the click lands exactly
		// where the preview said it would (Placement.h). A refusal carries its own
		// reason — "nothing to hang this on" is a different problem from "that
		// square is rock", and the ghost has already been saying which.
		const Placement place = ResolveBrush(cx, cz, face);
		if (!place.valid) {
			log(loc::Format(place.refusalKey ? place.refusalKey : "map.place.blocked",
							items[m_sel.index].label));
			break;
		}
		const bool wallBrush = place.mount == Mount::Wall;
		const int px = place.x;
		const int pz = place.z;
		if (m_sel.cat == PaletteCat::Monsters)
			ok = remote ? m_world.AddMonsterRemote(stem, id, cx, cz)
						: m_world.AddMonster(id, cx, cz, Direction::South);
		else if (m_sel.cat == PaletteCat::Fixtures)
			ok = wallBrush
					 ? (remote ? m_world.AddFixtureRemote(stem, id, px, pz, face.wall)
							   : m_world.AddFixture(id, px, pz, face.wall))
					 : (remote ? m_world.AddFixtureRemote(stem, id, cx, cz)
							   : m_world.AddFixture(id, cx, cz));
		else if (m_sel.cat == PaletteCat::WallFeatures) {
			// A `bore` (see-through window) tunnels THROUGH the solid block behind
			// the picked face, and that face names the axis it runs along — so a
			// free-standing block can be bored either way instead of always X.
			// Pointing from either side works, as the block is derived from the face.
			if (CatalogBool(m_world.GetProject().wallfeatures.Find(id), "bore", false)) {
				const int bx = face.x + DirDX(face.wall), bz = face.z + DirDZ(face.wall);
				const int axis = (face.wall == Direction::North ||
								  face.wall == Direction::South)
									 ? 1  // through a N/S face -> the bore runs along Z
									 : 0; // through an E/W face -> along X
				ok = m_world.AddBore(id, bx, bz, axis); // active level only for now
			} else
				ok = remote ? m_world.AddNicheRemote(stem, id, px, pz, face.wall)
							: m_world.AddNiche(id, px, pz, face.wall);
		}
		else if (m_sel.cat == PaletteCat::SurfaceFeatures)
			// The plain cell under the pointer — neither surface has a face to
			// pick, and the TYPE decides whether it lands on the floor or the
			// ceiling, so one brush serves both.
			ok = remote ? m_world.AddSurfaceFeatureRemote(stem, id, cx, cz)
						: m_world.AddSurfaceFeature(id, cx, cz);
		else if (m_sel.cat == PaletteCat::Buttons)
			ok = remote ? m_world.AddButtonRemote(stem, id, cx, cz)
						: m_world.AddButton(id, cx, cz);
		else if (m_sel.cat == PaletteCat::Items ||
				 m_sel.cat == PaletteCat::Weapons ||
				 m_sel.cat == PaletteCat::Armor) {
			// Weapons and armor are item entities too — same placement path.
			// A niche on the clicked WALL takes the item (piled in its pocket);
			// a floor cell places on the floor as usual.
			if (!remote)
				if (auto faces = m_world.NicheFacesAt(cx, cz); !faces.empty()) {
					ok = m_world.AddNicheItem(id, faces[0].x, faces[0].z, faces[0].wall);
					log(loc::Format(ok ? "map.place.done" : "map.place.blocked",
									items[m_sel.index].label));
					changed = ok;
					break;
				}
			ok = remote ? m_world.AddItemRemote(stem, id, cx, cz)
						: m_world.AddItem(id, cx, cz);
		}
		else if (wallBrush) // a `mount = wall` decoration hangs on the picked face
			ok = remote ? m_world.AddDecorationRemote(stem, id, px, pz, face.wall)
						: m_world.AddWallDecoration(id, px, pz, face.wall);
		else
			ok = remote ? m_world.AddDecorationRemote(stem, id, cx, cz)
						: m_world.AddDecoration(id, cx, cz, Direction::South);
		log(loc::Format(ok ? "map.place.done" : "map.place.blocked",
						items[m_sel.index].label));
		changed = ok;
		break;
	}
	case PaletteCat::Stairs: {
		if (dragging) break; // placement is a single click
		const std::vector<PaletteItem> items = CategoryItems(m_sel.cat);
		if (m_sel.index < 0 || m_sel.index >= static_cast<int>(items.size())) break;
		// One entry for any viewed level (each side lands live or in a stash);
		// it does all the messaging itself (success names the paired level;
		// each failure mode has its own specific line).
		changed = m_world.AddStairAt(stem, items[m_sel.index].id, cx, cz);
		break;
	}
	case PaletteCat::Doors: {
		if (dragging) break; // placement is a single click
		const std::vector<PaletteItem> items = CategoryItems(m_sel.cat);
		if (m_sel.index < 0 || m_sel.index >= static_cast<int>(items.size())) break;
		const std::string& id = items[m_sel.index].id;
		// AddDoor messages the doorway failure itself (auto-orientation needs
		// flanking walls); the generic done/blocked line covers the rest.
		const bool ok = remote ? m_world.AddDoorRemote(stem, id, cx, cz)
							   : m_world.AddDoor(id, cx, cz);
		if (ok)
			log(loc::Format("map.place.done", items[m_sel.index].label));
		changed = ok;
		break;
	}
	default:
		break;
	}

	if (strokeStart) m_world.CommitUndoStep(changed);
}

void MapEditor::PaintCell(int cx, int cz, bool remote, const std::string& stem) {
	using SS = DungeonWorld::SurfaceSel;
	if (!PaintableCat(m_sel.cat)) return; // placement never reaches here
	const SS sel = m_sel.cat == PaletteCat::Walls    ? SS::Wall
				   : m_sel.cat == PaletteCat::Floors ? SS::Floor
													 : SS::Ceiling;
	// Resolve the armed ROW to a catalog id, then to the level's VARIANT INDEX.
	// The row index is a position in the displayed list (which the "Catalogue"
	// toggle changes), NOT the variant index — so we key off the id and let the
	// world enrol a catalogue-view type the level lacks (append-only; a type
	// already present keeps its index). -1 = its baked assets are missing.
	const std::vector<PaletteItem> items = CategoryItems(m_sel.cat);
	if (m_sel.index < 0 || m_sel.index >= static_cast<int>(items.size())) return;
	const int variant = m_world.EnsureSurfaceVariant(stem, sel, items[m_sel.index].id);
	if (variant < 0) return;
	// The texture brush owns the CELL TYPE too: painting a wall texture on a
	// floor square raises the wall, a floor/ceiling texture carves solid rock
	// walkable, then the variant lands on the converted square — these ARE
	// the structural brushes (the old Structure Wall/Floor rows folded in).
	const Cell want = sel == SS::Wall ? Cell::Wall : Cell::Floor;
	if (remote) {
		m_world.EditCellRemote(stem, cx, cz, want); // no-op when already right
		m_world.EditVariantRemote(stem, cx, cz, sel, variant);
		return;
	}
	if (m_world.Map().At(cx, cz) != want) {
		const Party& party = m_world.GetParty();
		if (want == Cell::Wall && cx == party.GridX() && cz == party.GridZ())
			return; // never wall the party in (skip; a fill keeps going)
		m_world.EditCell(cx, cz, want);
	}
	m_world.EditVariant(cx, cz, sel, variant);
}

void MapEditor::PaintRect(int cx, int cz) {
	if (m_sel.index < 0) return;
	// Placement categories (and a rect with no anchor yet) act as a plain click.
	if (!PaintableCat(m_sel.cat) || m_lastX < 0) {
		ApplyBrush(cx, cz, /*dragging*/ false);
		return;
	}
	const bool remote = m_view.Browsing();
	const std::string& stem = m_view.ViewedLevel();
	const int x0 = std::min(m_lastX, cx), x1 = std::max(m_lastX, cx);
	const int z0 = std::min(m_lastZ, cz), z1 = std::max(m_lastZ, cz);
	m_world.BeginUndoStep();
	const u32 rev0 = m_world.Map().Revision();
	for (int z = z0; z <= z1; ++z)
		for (int x = x0; x <= x1; ++x) PaintCell(x, z, remote, stem);
	m_world.CommitUndoStep(remote || m_world.Map().Revision() != rev0);
	m_lastX = cx; // chainable: the far corner anchors the next rectangle
	m_lastZ = cz;
	if (m_world.onMessage)
		m_world.onMessage(loc::Format("map.fill.done",
									  (x1 - x0 + 1) * (z1 - z0 + 1)));
}

void MapEditor::FloodFill(int cx, int cz) {
	if (m_sel.index < 0) return;
	if (!PaintableCat(m_sel.cat)) { // placement acts as a plain click
		ApplyBrush(cx, cz, /*dragging*/ false);
		return;
	}
	const DungeonMap& map = m_view.ViewedMap();
	if (cx < 0 || cz < 0 || cx >= map.Width() || cz >= map.Height()) return;
	using SS = DungeonWorld::SurfaceSel;
	const Cell baseCell = map.At(cx, cz);
	const SS sel = m_sel.cat == PaletteCat::Walls    ? SS::Wall
				   : m_sel.cat == PaletteCat::Floors ? SS::Floor
													 : SS::Ceiling;
	// FLOOD stays a recolor: the region keys on the brush surface's
	// RESOLVED variant, which the wrong square type doesn't have — so a
	// fill started there is a no-op. (A plain click or a shift-rect DOES
	// convert the cell type; flood converting a whole room to solid on a
	// misclick would be a foot-gun.)
	if ((baseCell == Cell::Wall) != (sel == SS::Wall)) return;
	const int baseVar = ResolvedVariant(cx, cz, static_cast<int>(sel));
	// 4-connected region of same cell type (+ same resolved variant for the
	// surface brushes, so the fill stops where the visible texture changes).
	std::vector<std::pair<int, int>> region, stack{{cx, cz}};
	std::vector<u8> seen(static_cast<size_t>(map.Width()) * map.Height(), 0);
	auto idx = [&](int x, int z) {
		return static_cast<size_t>(z) * map.Width() + x;
	};
	seen[idx(cx, cz)] = 1;
	while (!stack.empty()) {
		const auto [x, z] = stack.back();
		stack.pop_back();
		region.push_back({x, z});
		const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
		for (const auto& d : nb) {
			const int nx = x + d[0], nz = z + d[1];
			if (nx < 0 || nz < 0 || nx >= map.Width() || nz >= map.Height())
				continue;
			if (seen[idx(nx, nz)]) continue;
			if (map.At(nx, nz) != baseCell) continue;
			if (ResolvedVariant(nx, nz, static_cast<int>(sel)) != baseVar)
				continue;
			seen[idx(nx, nz)] = 1;
			stack.push_back({nx, nz});
		}
	}
	const bool remote = m_view.Browsing();
	const std::string& stem = m_view.ViewedLevel();
	m_world.BeginUndoStep();
	const u32 rev0 = m_world.Map().Revision();
	for (const auto& [x, z] : region) PaintCell(x, z, remote, stem);
	m_world.CommitUndoStep(remote || m_world.Map().Revision() != rev0);
	m_lastX = cx;
	m_lastZ = cz;
	if (m_world.onMessage)
		m_world.onMessage(loc::Format("map.fill.done", region.size()));
}

void MapEditor::PickAt(int cx, int cz) {
	const DungeonMap& map = m_view.ViewedMap();
	if (cx < 0 || cz < 0 || cx >= map.Width() || cz >= map.Height()) return;
	using SS = DungeonWorld::SurfaceSel;
	// A solid square arms its wall texture, a floor square its floor texture;
	// ceilings (sharing the floor square) are picked while already on the
	// Ceilings brush.
	const bool solid = map.At(cx, cz) == Cell::Wall;
	const PaletteCat cat =
		solid ? PaletteCat::Walls
			  : (m_sel.cat == PaletteCat::Ceilings ? PaletteCat::Ceilings
												   : PaletteCat::Floors);
	const SS sel = cat == PaletteCat::Walls    ? SS::Wall
				   : cat == PaletteCat::Floors ? SS::Floor
											   : SS::Ceiling;
	const int v = ResolvedVariant(cx, cz, static_cast<int>(sel));
	if (v < 0) return; // empty palette
	// v is the picked cell's VARIANT index (into the level palette); the armed
	// selection is a ROW index into the displayed list, which the "Catalogue"
	// view reorders. Map variant → id → the row showing that id.
	const std::vector<std::string>& pal = sel == SS::Wall	? map.WallPalette()
										  : sel == SS::Floor ? map.FloorPalette()
															 : map.CeilingPalette();
	if (v >= static_cast<int>(pal.size())) return;
	const std::string& id = pal[v];
	const std::vector<PaletteItem> items = CategoryItems(cat);
	for (int i = 0; i < static_cast<int>(items.size()); ++i)
		if (items[i].id == id) {
			m_sel = {cat, i};
			if (m_world.onMessage)
				m_world.onMessage(loc::Format("map.pick.done", items[i].label));
			return;
		}
}

int MapEditor::ResolvedVariant(int cx, int cz, int selRaw) const {
	using SS = DungeonWorld::SurfaceSel;
	const SS sel = static_cast<SS>(selRaw);
	const DungeonMap& map = m_view.ViewedMap();
	const std::vector<std::string>& pal =
		sel == SS::Wall    ? map.WallPalette()
		: sel == SS::Floor ? map.FloorPalette()
						   : map.CeilingPalette();
	const int count = static_cast<int>(pal.size());
	if (count == 0) return -1;
	// Override else the mesh builder's hash — StampCell's exact pick, the same
	// resolution the map's textured fill uses.
	const int over = sel == SS::Wall    ? map.WallVariant(cx, cz)
					 : sel == SS::Floor ? map.FloorVariant(cx, cz)
										: map.CeilingVariant(cx, cz);
	if (over >= 0) return std::min(over, count - 1);
	const u32 salt = sel == SS::Wall ? 3u : sel == SS::Floor ? 1u : 2u;
	return static_cast<int>(
		SurfaceVariantFor(cx, cz, salt, static_cast<u32>(count)));
}

void MapEditor::InspectAt(int cx, int cz) {
	const bool remote = m_view.Browsing();
	const DungeonMap& map = m_view.ViewedMap();
	auto log = [&](const std::string& s) {
		if (m_world.onMessage) m_world.onMessage(s);
	};
	if (remote) {
		// No live instances on a browsed level — report the static base only;
		// the inspectors need the level active.
		log(loc::Format("map.select.contents", cx, cz,
						map.At(cx, cz) == Cell::Wall ? "wall" : "floor"));
		return;
	}
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
	// Select the square (highlight + patrol-route overlay) and open the
	// inspector right away when it holds an editable object — the owner
	// (onInspect) picks the dialog, via the chooser when several share it.
	m_selX = cx;
	m_selZ = cz;
	m_selMonster = m_world.MonsterRuntimeIdAt(cx, cz);
	if (m_world.AnyInspectableAt(cx, cz) && onInspect) onInspect(cx, cz);
}

void MapEditor::EraseAt(int cx, int cz, const WallFace& face) {
	using SS = DungeonWorld::SurfaceSel;
	const bool remote = m_view.Browsing();
	const std::string& stem = m_view.ViewedLevel();
	auto log = [&](const std::string& s) {
		if (m_world.onMessage) m_world.onMessage(s);
	};
	m_world.BeginUndoStep();
	if (remote) { // the stash-side ladder messages for itself
		m_world.EraseRemote(stem, cx, cz);
	} else if (m_world.RemoveStairAt(cx, cz)) {
		// stairs message themselves (they name the paired level's cleanup)
	} else if (face.valid &&
			   (m_world.RemoveFixtureAtFace(face.x, face.z, face.wall) ||
				m_world.RemoveNicheAtFace(face.x, face.z, face.wall))) {
		// Wall things are placed per FACE, so one cell/block can carry several:
		// erase the one being POINTED at before the cell-wide rungs below (which
		// take whichever they find first). Sconce before niche, matching the
		// order of the cell-wide ladder.
		log(loc::Tr("map.erase.removed"));
	} else if (m_world.RemoveEntityAt(cx, cz) || m_world.RemoveFixtureAt(cx, cz) ||
			   m_world.RemoveNicheAtWall(cx, cz) || m_world.RemoveBoreAt(cx, cz) ||
			   // Below the wall rungs: a floor recess is the cell's own floor, so
			   // erasing it should not beat anything STANDING on that floor.
			   m_world.RemoveFeatureAt(cx, cz)) {
		log(loc::Tr("map.erase.removed"));
	} else {
		m_world.EditVariant(cx, cz, SS::Wall, -1);
		m_world.EditVariant(cx, cz, SS::Floor, -1);
		m_world.EditVariant(cx, cz, SS::Ceiling, -1);
		log(loc::Format("map.erase.reset", cx, cz));
	}
	m_world.CommitUndoStep(true); // the ladder always acts (last rung resets)
}

void MapEditor::RenderBody(gfx::SpriteBatch& batch, const ui::Theme& theme,
						   const gfx::Rect& panel) {
	const ui::Font& font = m_view.Font();
	const float dpad = MapView::DockPad(panel);

	// Controls row (fixed above the scrolled accordion): filter box with
	// placeholder/caret, [x] clear, [-] collapse-all.
	{
		const gfx::Rect box = FilterBoxRect(panel);
		batch.DrawRect(box, theme.control);
		ui::DrawBorder(batch, box,
					   m_filterFocused ? theme.accent : theme.panelBorder);
		const float ty = box.y + (box.h - font.Height()) * 0.5f;
		if (m_filter.empty() && !m_filterFocused) {
			font.Draw(batch, loc::Tr("map.filter.hint"), box.x + dpad, ty,
					  theme.textDim);
		} else {
			font.Draw(batch, m_filter, box.x + dpad, ty, theme.text);
			if (m_filterFocused) { // caret at the text end
				const float cx = box.x + dpad + font.MeasureWidth(m_filter) + 1.0f;
				batch.DrawRect({cx, box.y + 4.0f, 1.0f, box.h - 8.0f}, theme.text);
			}
		}
		ui::DrawButtonFace(batch, font, FilterClearRect(panel), "x", theme,
						   m_hotCtrl == HotCtrl::Clear && !m_filter.empty(),
						   false, !m_filter.empty());
		ui::DrawButtonFace(batch, font, CollapseAllRect(panel), "-", theme,
						   m_hotCtrl == HotCtrl::Collapse, false, true);
	}

	// "Catalogue" checkbox (second controls line): a small box + label. Checked
	// shows the whole surface catalog; unchecked, only the level's palette.
	{
		const gfx::Rect row = CatalogToggleRect(panel);
		const float bs = row.h - dpad * 2; // the box side
		const gfx::Rect box{row.x, row.y + dpad, bs, bs};
		batch.DrawRect(box, m_settings.mapShowCatalog ? theme.accent : theme.control);
		ui::DrawBorder(batch, box,
					   m_hotCtrl == HotCtrl::Catalog ? theme.accent : theme.panelBorder);
		font.Draw(batch, loc::Tr("map.cat.catalogue"), box.x + bs + dpad,
				  row.y + (row.h - font.Height()) * 0.5f,
				  m_hotCtrl == HotCtrl::Catalog ? theme.text : theme.textDim);
	}

	const gfx::Rect body = AccordionBody(panel);
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
		case PaletteRow::Kind::Divider: {
			// The theme boundary: a hairline across the run's width, inset to the
			// items' indent. Deliberately a RULE and not a labelled "off-theme"
			// header — the rows below it are ordinary, clickable types, and a
			// header would read as a section you are not supposed to use.
			const float inset = dpad * 3;
			batch.DrawRect({rc.x + inset, rc.y + rc.h * 0.5f, rc.w - inset * 2, 1.0f},
						   theme.panelBorder);
			break;
		}
		case PaletteRow::Kind::SubHeader: {
			// A group sub-header: indented +/- toggle, the free-form category
			// token (first letter up-cased) and the member count. Tokens are
			// data ids, so no loc lookup — like the item labels themselves.
			const char* arrow = GroupOpen(r.cat, r.group) ? "-" : "+";
			int n = 0;
			for (const PaletteItem& it : items)
				if (it.group == r.group) ++n;
			std::string label = r.group;
			label[0] = static_cast<char>(
				std::toupper(static_cast<unsigned char>(label[0])));
			label += std::format(" ({})", n);
			font.Draw(batch, arrow, rc.x + dpad * 3, ty, theme.textDim);
			font.Draw(batch, label, rc.x + dpad * 4 + arrowW, ty, theme.text);
			break;
		}
		case PaletteRow::Kind::Item: {
			if (r.index < 0 || r.index >= static_cast<int>(items.size())) break;
			const bool active = m_sel.cat == r.cat && m_sel.index == r.index;
			if (active) {
				batch.DrawRect(rc, theme.controlActive);
				ui::DrawBorder(batch, rc, theme.panelBorder);
			}
			// Grouped items indent one level past their sub-header.
			const float indent = dpad * (r.group.empty() ? 3.0f : 5.0f);
			const float sw = rc.h - dpad * 2;
			const gfx::Rect swRect{rc.x + indent, rc.y + dpad, sw, sw};
			if (items[r.index].icon)
				batch.DrawSprite(swRect, {0, 0, 1, 1}, *items[r.index].icon,
								 {1, 1, 1, 1});
			else
				batch.DrawRect(swRect, items[r.index].swatch);
			font.Draw(batch, items[r.index].label, rc.x + indent + sw + dpad, ty,
					  active ? theme.text : theme.textDim);
			break;
		}
		}
	}
	batch.SetScissor(nullptr);
}

} // namespace dungeon::game
