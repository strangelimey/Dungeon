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
#include "Core/Utf8.h"
#include "Game/Area.h"               // the area fill's room/corridor
#include "Game/DungeonMeshBuilder.h" // ResolveSurfaceVariant (eyedropper/flood key)
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
	// Created from the palette with nothing but a NAME — no asset dialog,
	// because there is no asset. The world tier's catalogs are pure data.
	bool authorable = false;
};
constexpr CatInfo kCategoryInfo[] = {
	{"map.cat.walls", "walls", true},       {"map.cat.floors", "floors", true},
	{"map.cat.ceilings", "ceilings", true},
	// A brush (placeable) that is pure data (authorable): "+ New..." names one
	// and opens the type editor on it, no asset dialog.
	{"map.cat.themes", "themes", false, /*placeable*/ true, /*authorable*/ true},
	{"map.cat.decorations", "decorations", false},
	{"map.cat.fixtures", "fixtures", false}, {"map.cat.monsters", "monsters", false},
	{"map.cat.buttons", "buttons", false},  {"map.cat.doors", "doors", false},
	{"map.cat.stairs", "stairs", false},    {"map.cat.items", "items", false},
	{"map.cat.weapons", "weapons", false},  {"map.cat.armor", "armor", false},
	{"map.cat.wallfeatures", "wallfeatures", false},
	{"map.cat.surfacefeatures", "surfacefeatures", false},
	{"map.cat.effects", "effects", false, /*placeable*/ false},
	{"map.cat.dungeons", "dungeons", false, /*placeable*/ false, /*authorable*/ true},
	{"map.cat.terrain", "terrain", false, /*placeable*/ false, /*authorable*/ true},
	{"map.cat.quests", "quests", false, /*placeable*/ false, /*authorable*/ true},
	{"map.cat.flags", "flags", false, /*placeable*/ false, /*authorable*/ true},
	{"map.cat.styles", "styles", false, /*placeable*/ false, /*authorable*/ true},
	{"map.cat.shapes", "shapes", false, /*placeable*/ false, /*authorable*/ true},
	{"map.cat.lights", "lights", false, /*placeable*/ false, /*authorable*/ true},
	{"map.cat.trails", "trails", false, /*placeable*/ false, /*authorable*/ true},
};
static_assert(sizeof(kCategoryInfo) / sizeof(kCategoryInfo[0]) ==
				  static_cast<size_t>(MapEditor::PaletteCat::Count),
			  "kCategoryInfo must have one row per PaletteCat");
const CatInfo& CatInfoFor(MapEditor::PaletteCat cat) {
	return kCategoryInfo[static_cast<size_t>(cat)];
}

// The order the palette LISTS its sections in is the category bar's business
// now (MapEditor_Categories.cpp): each group lists its own, and the enum keeps
// its order so nothing indexed by it moves.

// A terrain's swatch is its own authored `color` - the colour the world map
// paints it, read by the one parser for that field - else the floor ink.
Vec4 TerrainSwatch(const CatalogEntry& e) {
	Vec4 c = kFloor;
	CatalogColor(&e, "color", c);
	return c;
}
} // namespace

MapEditor::MapEditor(MapView& view, GameSettings& settings)
	: m_view(view), m_settings(settings) {
	// Open the most-used category by default; the rest start collapsed. And
	// whatever group the bar was left on shows something from the first frame.
	m_catOpen[static_cast<size_t>(PaletteCat::Walls)] = true;
	OpenSomethingInGroup();
	m_drawnSwatches.reserve(kDrawnSwatchRoom); // recorded by the draw, never grown
}

const char* MapEditor::CategoryNameKey(PaletteCat cat) { return CatInfoFor(cat).nameKey; }
const char* MapEditor::CategoryCatalogKey(PaletteCat cat) { return CatInfoFor(cat).catalogKey; }
bool MapEditor::CategoryTextureSet(PaletteCat cat) { return CatInfoFor(cat).textureSet; }
bool MapEditor::CategoryPlaceable(PaletteCat cat) { return CatInfoFor(cat).placeable; }
bool MapEditor::CategoryAuthorable(PaletteCat cat) { return CatInfoFor(cat).authorable; }

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
	const Project& proj = m_world->GetProject();
	// The tags lens, from the VIEWED level for the same reason: browsing a
	// level should rank its palette by ITS tags, not by the party's.
	const std::vector<std::string>& tags = map.Tags();

	// A surface palette (list of catalog ids) resolved to display name + swatch;
	// the entry's `category` groups it under a sub-accordion like the entity
	// catalogs (an id the catalog doesn't know stays ungrouped). The swatch is
	// the entry's loaded albedo texture — the same one the map's cell fill
	// draws — with the flat category color as the not-loaded fallback (a
	// browsed level's foreign palette).
	auto surfaceItems = [&](const std::vector<std::string>& palette, PaletteCat surface,
							const Catalog& catalog) {
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
		for (const std::string& id : source) items.push_back(SurfaceItem(surface, id));
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
							 /*icon*/ nullptr, CatalogMatchesTags(&e, tags)});
		}
		return items;
	};

	switch (cat) {
	case PaletteCat::Walls:    return surfaceItems(map.WallPalette(), cat, proj.walls);
	case PaletteCat::Floors:   return surfaceItems(map.FloorPalette(), cat, proj.floors);
	case PaletteCat::Ceilings: return surfaceItems(map.CeilingPalette(), cat, proj.ceilings);
	case PaletteCat::Themes: {
		// World-wide, so every level lists every theme. The swatch is its
		// floor's, as the Floors section would show it.
		std::vector<PaletteItem> items = catalogItems(proj.themes, kFloor);
		for (PaletteItem& it : items)
			if (const CatalogEntry* e = proj.themes.Find(it.id)) {
				const ThemeMembers m = DungeonWorld::ThemeMembersOf(*e);
				const std::string& floor = m[static_cast<size_t>(Surface::Floor)];
				if (!floor.empty()) it.icon = SurfaceItem(PaletteCat::Floors, floor).icon;
			}
		return items;
	}
	case PaletteCat::Decorations: return catalogItems(proj.decorations, kDecoration);
	case PaletteCat::Fixtures:    return catalogItems(proj.fixtures, kTorch);
	case PaletteCat::Monsters: {
		// Each wears its power band, so a strong kind reads as strong before
		// it is placed (the world caches the powers per edit revision).
		std::vector<PaletteItem> items = catalogItems(proj.monsters, kMonster);
		for (PaletteItem& it : items) it.band = m_world->MonsterBand(it.id);
		// The CURRENT STYLE's lens, in place of the level's tags: its monsters
		// first, then a divider, then the rest - still all clickable.
		const std::vector<std::string> picks = StyleMonsters(m_style);
		if (!picks.empty())
			for (PaletteItem& it : items)
				it.onTags = std::find(picks.begin(), picks.end(), it.id) != picks.end();
		return items;
	}
	case PaletteCat::Buttons:     return catalogItems(proj.buttons, kButton);
	case PaletteCat::Doors:       return catalogItems(proj.doors, kDoor);
	case PaletteCat::Stairs:      return catalogItems(proj.stairs, kStair);
	case PaletteCat::Items:       return catalogItems(proj.items, kItem);
	case PaletteCat::Weapons:     return catalogItems(proj.weapons, kItem);
	case PaletteCat::Armor:       return catalogItems(proj.armor, kItem);
	case PaletteCat::WallFeatures: return catalogItems(proj.wallfeatures, kDecoration);
	case PaletteCat::SurfaceFeatures: return catalogItems(proj.surfacefeatures, kDecoration);
	case PaletteCat::Effects:     return catalogItems(proj.effects, kMonster);
	// The world tier's catalogs: rows that open the type editor (they are
	// never placed). They used to fall to the default below, so each section
	// was a "+ New..." over "(none defined)" however many it held.
	case PaletteCat::Dungeons:    return catalogItems(proj.dungeons, kStair);
	case PaletteCat::Quests:      return catalogItems(proj.quests, kItem);
	case PaletteCat::Flags:       return QuestSectionItems();
	case PaletteCat::Styles:      return StyleSectionItems();
	case PaletteCat::Shapes:      return catalogItems(proj.shapes, kFloor);
	case PaletteCat::Lights: {
		// Each row's swatch is the light's own colour (white for one that takes
		// its source's), so the section reads as a row of lamps.
		std::vector<PaletteItem> items = catalogItems(proj.lights, kTorch);
		for (PaletteItem& it : items) {
			const light::Profile& p = m_world->LightProfileFor(it.id);
			it.swatch = p.sourceColor ? Vec4{0.9f, 0.9f, 0.9f, 1.0f}
									  : Vec4{p.color.x, p.color.y, p.color.z, 1.0f};
		}
		return items;
	}
	case PaletteCat::Trails: {
		// The swatch is the trail's own colour, white for one that takes its
		// light's.
		std::vector<PaletteItem> items = catalogItems(proj.trails, kTorch);
		for (PaletteItem& it : items) {
			const trail::Spec& t = m_world->TrailSpecFor(it.id);
			it.swatch = t.hasColor ? Vec4{t.color.x, t.color.y, t.color.z, 1.0f}
								  : Vec4{0.9f, 0.9f, 0.9f, 1.0f};
		}
		return items;
	}
	case PaletteCat::Terrain: {
		std::vector<PaletteItem> items = catalogItems(proj.terrain, kFloor);
		for (PaletteItem& it : items)
			if (const CatalogEntry* e = proj.terrain.Find(it.id)) it.swatch = TerrainSwatch(*e);
		return items;
	}
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

MapEditor::PaletteItem MapEditor::SurfaceItem(PaletteCat cat, const std::string& id) const {
	// Display name + group from the surface catalog; the swatch is the entry's
	// linear thumbnail - the same one the map's cell fill draws - with the flat
	// category colour as the fallback until it has loaded (asked for by the
	// lookup itself, loaded by the next Update).
	const DungeonWorld::SurfaceSel sel = SelFor(cat);
	const CatalogEntry* e = m_world->SurfaceCatalog(sel).Find(id);
	const Vec4& flat = cat == PaletteCat::Walls ? kWall
					   : cat == PaletteCat::Floors ? kFloor
												   : kCeiling;
	return {e ? e->Display() : id, flat, id, e ? e->Get("category", "") : std::string(),
			m_world->SurfaceSwatchForId(sel, id),
			CatalogMatchesTags(e, m_view.ViewedMap().Tags())};
}

ui::Swatch MapEditor::SurfaceSwatch(PaletteCat cat, const std::string& id) const {
	if (!m_world) return {};
	const Vec4& flat = cat == PaletteCat::Walls ? kWall
					   : cat == PaletteCat::Floors ? kFloor
												   : kCeiling;
	return {m_world->SurfaceSwatchForId(SelFor(cat), id), flat};
}

void MapEditor::LoadSurfaceSwatch(PaletteCat cat, const std::string& id) {
	if (SurfaceCat(cat)) m_world->LoadSurfaceThumb(SelFor(cat), id);
}

void MapEditor::AddToPalette(PaletteCat cat, const std::string& id) {
	if (!SurfaceCat(cat)) return;
	auto log = [&](const std::string& s) {
		if (m_world->onMessage) m_world->onMessage(s);
	};
	const DungeonWorld::SurfaceSel sel = SelFor(cat);
	// The palette lives on the map, which the undo snapshot copies wholesale —
	// so bracketing here is all an undoable palette add needs.
	m_world->BeginUndoStep();
	const bool ok = m_view.Browsing()
						? m_world->AddPaletteEntryRemote(m_view.ViewedLevel(), sel, id)
						: m_world->AddPaletteEntry(sel, id);
	m_world->CommitUndoStep(ok);
	if (!ok) {
		log(loc::Format("map.palette.failed", id));
		return;
	}
	log(loc::Format("map.palette.added", id));
	// Arm the newcomer: it is the last row of its category, and painting it is
	// the reason the user added it - so its section must be the one showing.
	RevealCategory(cat);
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

bool MapEditor::Disarm() {
	if (m_sel.index < 0) return false;
	m_sel.index = -1;
	if (m_world && m_world->onMessage) m_world->onMessage(loc::View("map.brush.off"));
	return true;
}

bool MapEditor::BeginMove(int cx, int cz) {
	m_moving = false;
	if (!m_world) return false;
	auto say = [&](std::string_view line) {
		if (m_world->onMessage) m_world->onMessage(line);
	};
	// A browsed level has records but no live instances, and every move changes
	// the live instance first (the inspectors' rule, for the same reason).
	if (m_view.Browsing()) {
		say(loc::View("map.move.remote"));
		return false;
	}
	m_move = m_world->TopMovableAt(cx, cz);
	if (m_move.kind == DungeonWorld::MoveTarget::Kind::None) {
		say(loc::View("map.move.none"));
		return false;
	}
	m_moving = true;
	say(loc::Format("map.move.pick", m_move.label));
	return true;
}

bool MapEditor::EndMove(int cx, int cz) {
	if (!m_moving || !m_world) return false;
	m_moving = false;
	if (cx == m_move.x && cz == m_move.z) return false; // dropped where it was
	m_world->BeginUndoStep();
	const bool moved = m_world->MoveObject(m_move, cx, cz);
	m_world->CommitUndoStep(moved);
	return moved;
}

bool MapEditor::Arm(PaletteCat cat, const std::string& id) {
	const std::vector<PaletteItem> items = CategoryItems(cat);
	for (int i = 0; i < static_cast<int>(items.size()); ++i)
		if (items[i].id == id) {
			m_sel = {cat, i};
			return true;
		}
	return false;
}

void MapEditor::CollapseAll() {
	m_catOpen.fill(false);
	m_groupOpen.clear(); // groups default collapsed
	m_paletteScroll = 0.0f;
}

void MapEditor::ExpandShown() {
	for (const PaletteCat cat : GroupCategories(PaletteGrouping(), ActiveGroup())) {
		m_catOpen[static_cast<size_t>(cat)] = true;
		for (const PaletteItem& item : CategoryItems(cat))
			if (!item.group.empty()) m_groupOpen[GroupKey(cat, item.group)] = true;
	}
	m_paletteScroll = 0.0f;
}

// --- palette controls row (filter + clear + collapse-all) --------------------

gfx::Rect MapEditor::ControlsRow(const gfx::Rect& panel) const {
	// Under the category bar, which owns the top of the body.
	const gfx::Rect bar = CategoryBar(panel).area;
	const float h = std::clamp(panel.h * 0.040f, 20.0f, 36.0f);
	return {bar.x, bar.y + bar.h + MapView::DockPad(panel), bar.w, h};
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
	// The typed text in order (Input::TypedChars). Esc/Enter release the
	// keyboard back to the game (Game gates the party keys and its own Esc/M on
	// KeyboardCaptured while we hold it), so typing after an Enter is not ours.
	// Whole UTF-8 characters (C383): Backspace takes the last one, and the cap
	// is kFilterMaxChars CHARACTERS, refusing a letter whole rather than keeping
	// half of it.
	bool edited = false;
	const std::string_view typed = input.TypedChars();
	for (size_t i = 0; i < typed.size();) {
		const std::string_view ch = utf8::CharAt(typed, i);
		i += ch.size();
		if (ch[0] == Input::kTypedEnter) {
			m_filterFocused = false;
			break;
		}
		if (ch[0] == Input::kTypedBack) {
			if (!utf8::PopBack(m_filter)) continue;
		} else {
			if (static_cast<unsigned char>(ch[0]) < 0x20) continue; // printable only
			if (utf8::Length(m_filter) >= kFilterMaxChars) continue;
			m_filter.append(ch);
		}
		edited = true;
	}
	if (input.WasKeyPressed(vk::Escape)) m_filterFocused = false;
	if (edited) m_paletteScroll = 0.0f; // a changed filter restarts at the top
}

void MapEditor::TrackMouse(float mx, float my, const gfx::Rect& panel) {
	m_hotBar = BarButtonAt(mx, my, panel);
	m_hotCtrl = m_hotBar >= -1                             ? HotCtrl::Bar
				: FilterBoxRect(panel).Contains(mx, my)     ? HotCtrl::Filter
				: FilterClearRect(panel).Contains(mx, my) ? HotCtrl::Clear
				: CollapseAllRect(panel).Contains(mx, my) ? HotCtrl::Collapse
				: CatalogToggleRect(panel).Contains(mx, my) ? HotCtrl::Catalog
															: HotCtrl::None;
	// The row under the pointer, for the trimmed-name tooltip. Only laid out
	// when the pointer is actually over the accordion.
	m_hoverItem = {PaletteCat::Count, -1};
	if (m_hotCtrl == HotCtrl::None && AccordionBody(panel).Contains(mx, my)) {
		std::vector<PaletteRow> rows;
		float content = 0.0f;
		BuildPaletteRows(panel, rows, content);
		for (const PaletteRow& r : rows)
			if ((r.kind == PaletteRow::Kind::Item || r.kind == PaletteRow::Kind::Header) &&
				r.rect.Contains(mx, my)) {
				// A header is index -2: its name can be trimmed too.
				m_hoverItem = {r.cat, r.kind == PaletteRow::Kind::Header ? -2 : r.index};
				break;
			}
	}
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
	for (const PaletteCat cat : CandidateSections()) {
		const int c = static_cast<int>(cat);
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
				// The TAGS LENS, applied per run (the ungrouped items, and each
				// open group's body): on-tag first, then a divider, then the
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
						[&](int i) { return items[i].onTags; });
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
				// WITHIN a group leaves every group uniformly on- or off-tag
				// and the item divider never fires. A group is on-tag if ANY
				// member is — the question being asked of a collapsed group is
				// "is there anything for me in here".
				const auto offGroup = std::stable_partition(
					groups.begin(), groups.end(), [&](const std::string& g) {
						return std::any_of(items.begin(), items.end(),
										   [&](const PaletteItem& it) {
											   return it.group == g && it.onTags;
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
	// The category bar first, then the controls row: the filter box takes
	// focus, [x] clears, [-] collapses every accordion (and sub-group). Any
	// other palette click releases the filter's keyboard capture.
	if (CategoryBar(panel).area.Contains(mx, my)) {
		m_filterFocused = false;
		return OnBarClick(mx, my, panel);
	}
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
		CollapseAll();
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
		else if (r.kind == PaletteRow::Kind::Item && r.cat == PaletteCat::Flags)
			QuestRowClick(r, mx, my); // rows of two catalogs, with links
		else if (r.kind == PaletteRow::Kind::Item && r.cat == PaletteCat::Styles) {
			// Arm a world style, add a library one (never the type editor: a
			// right-click is that, as everywhere).
			const std::vector<PaletteItem> items = CategoryItems(r.cat);
			if (r.index >= 0 && r.index < static_cast<int>(items.size()))
				UseStyleRow(items[static_cast<size_t>(r.index)].id);
		} else if (r.kind == PaletteRow::Kind::Item && r.cat == PaletteCat::Shapes) {
			// The current stamp, with the Stamp tool picked to lay it.
			const std::vector<PaletteItem> items = CategoryItems(r.cat);
			if (r.index >= 0 && r.index < static_cast<int>(items.size()))
				UseShapeRow(items[static_cast<size_t>(r.index)].id);
		}
		else if (r.kind == PaletteRow::Kind::Item) {
			// A placeable type arms the brush; a non-placeable one has nothing
			// to arm, so a click opens its editor (what right-click does for
			// every row) rather than silently doing nothing.
			// Clicking the ARMED row again puts the brush down - the palette's
			// half of "a way to disarm the brush" (Esc is the other).
			if (CategoryPlaceable(r.cat) && m_sel.index == r.index && m_sel.cat == r.cat)
				Disarm();
			else if (CategoryPlaceable(r.cat)) m_sel = {r.cat, r.index};
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
			// A row standing for another catalog's type edits it THERE; a row
			// with no world catalog behind it (a library style) has nothing to
			// edit until it is added.
			if (r.index >= 0 && r.index < static_cast<int>(items.size()))
				if (const PaletteCat cat = RowCat(r.cat, items[r.index]); cat != PaletteCat::Count)
					onConfigure(cat, items[r.index].id);
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
	const Catalog* cat = m_world->GetProject().CatalogForKey(key);
	return MountFor(key, cat ? cat->Find(items[m_sel.index].id) : nullptr);
}

bool MapEditor::BrushIsWallMounted() const { return BrushMount() == Mount::Wall; }

bool MapEditor::DefaultWallFace(int x, int z, WallFace& out) const {
	// The level the brush EDITS (ApplyBrush below), so a browsed level's own
	// walls and fixtures decide, not the active level's at the same square.
	const DungeonMap& map = m_view.ViewedMap();
	Direction wall = Direction::North;
	bool found = false;
	if (m_sel.cat == PaletteCat::Fixtures)
		found = map.FreeSconceWall(x, z, wall);
	else if (m_sel.cat == PaletteCat::WallFeatures)
		found = map.FreeNicheWall(x, z, wall);
	else
		for (const Direction d : {Direction::North, Direction::East, Direction::South,
								  Direction::West})
			if (!map.IsWalkable(x + DirDX(d), z + DirDZ(d))) {
				wall = d;
				found = true;
				break;
			}
	if (found) out = {x, z, wall, true};
	return found;
}

Placement MapEditor::ResolveBrush(int cx, int cz, const WallFace& face, float fx,
								  float fz) const {
	Placement p = Resolve(m_view.ViewedMap(), BrushMount(), cx, cz, face, fx, fz);
	// The one part the pure resolver cannot answer: WHICH quarter is still free.
	// That is world state, not map state, so it is refined here — through the
	// same search the loader and the player's own drop already use, rather than
	// a second quarter-picking rule that could disagree with them.
	//
	// A BROWSED level deliberately gets no quarter: its items live as records in
	// a stash with no live instances to consult, so any answer here would be a
	// guess, and the placement falls back to the loader's fill order.
	if (p.valid && p.mount == Mount::FloorSlot && !m_view.Browsing())
		p.slot = m_world->FreeItemSlotNear(p.x, p.z, p.subX, p.subZ, -1);
	return p;
}

void MapEditor::ApplyBrush(int cx, int cz, bool dragging, const WallFace& face,
						   const Placement* pre) {
	// Edit target: the VIEWED level. The active level edits live world state;
	// a browsed level routes to DungeonWorld's remote seam (its in-memory
	// stash — see the level-browsing section in MapView.h).
	const bool remote = m_view.Browsing();
	const std::string& stem = m_view.ViewedLevel();

	// Laying a patrol route: a click appends the cell as a waypoint instead of
	// painting the armed brush (a drag doesn't spam duplicates). Routes belong
	// to a LIVE monster, so clicks on a browsed level are ignored.
	if (LayingRoute()) {
		if (!dragging && !remote && onRouteWaypoint) onRouteWaypoint(m_routeId, cx, cz);
		return;
	}
	if (m_sel.index < 0) return; // nothing armed yet
	using SS = DungeonWorld::SurfaceSel;
	auto log = [&](const std::string& s) {
		if (m_world->onMessage) m_world->onMessage(s);
	};

	// Undo bracketing: everything below mutates. Inside a stroke (BeginStroke ..
	// EndStroke) the stroke owns the one undo step and this call only reports
	// whether it changed anything; outside one, it brackets itself. `changed`:
	// live paints compare the map revision, entity edits report success, and
	// remote edits are conservatively treated as changed (a same-value remote
	// paint costs one no-op undo step at worst).
	const bool ownStep = !m_strokeOpen;
	if (ownStep) m_world->BeginUndoStep();
	const u32 rev0 = m_world->Map().Revision();
	bool changed = false;

	switch (m_sel.cat) {
	case PaletteCat::Walls:
	case PaletteCat::Floors:
	case PaletteCat::Ceilings:
	case PaletteCat::Themes: {
		PaintCell(cx, cz, remote, stem);
		// Remote edits are conservatively "changed" (see the bracket note).
		changed = remote || m_world->Map().Revision() != rev0;
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
		const Placement place = pre ? *pre : ResolveBrush(cx, cz, face);
		if (!place.valid) {
			log(loc::Format(place.refusalKey ? place.refusalKey : "map.place.blocked",
							items[m_sel.index].label));
			break;
		}
		const bool wallBrush = place.mount == Mount::Wall;
		const int px = place.x;
		const int pz = place.z;
		if (m_sel.cat == PaletteCat::Monsters)
			ok = remote ? m_world->AddMonsterRemote(stem, id, cx, cz)
						: m_world->AddMonster(id, cx, cz, Direction::South);
		else if (m_sel.cat == PaletteCat::Fixtures)
			ok = wallBrush
					 ? (remote ? m_world->AddFixtureRemote(stem, id, px, pz, face.wall)
							   : m_world->AddFixture(id, px, pz, face.wall))
					 : (remote ? m_world->AddFixtureRemote(stem, id, cx, cz)
							   : m_world->AddFixture(id, cx, cz));
		else if (m_sel.cat == PaletteCat::WallFeatures) {
			// A `bore` (see-through window) tunnels THROUGH the solid block behind
			// the picked face, and that face names the axis it runs along — so a
			// free-standing block can be bored either way instead of always X.
			// Pointing from either side works, as the block is derived from the face.
			if (CatalogBool(m_world->GetProject().wallfeatures.Find(id), "bore", false)) {
				const int bx = face.x + DirDX(face.wall), bz = face.z + DirDZ(face.wall);
				const int axis = (face.wall == Direction::North ||
								  face.wall == Direction::South)
									 ? 1  // through a N/S face -> the bore runs along Z
									 : 0; // through an E/W face -> along X
				ok = m_world->AddBore(id, bx, bz, axis); // active level only for now
			} else
				ok = remote ? m_world->AddNicheRemote(stem, id, px, pz, face.wall)
							: m_world->AddNiche(id, px, pz, face.wall);
		}
		else if (m_sel.cat == PaletteCat::SurfaceFeatures)
			// The plain cell under the pointer — neither surface has a face to
			// pick, and the TYPE decides whether it lands on the floor or the
			// ceiling, so one brush serves both.
			ok = remote ? m_world->AddSurfaceFeatureRemote(stem, id, cx, cz)
						: m_world->AddSurfaceFeature(id, cx, cz);
		else if (m_sel.cat == PaletteCat::Buttons)
			ok = remote ? m_world->AddButtonRemote(stem, id, cx, cz)
						: m_world->AddButton(id, cx, cz);
		else if (m_sel.cat == PaletteCat::Items ||
				 m_sel.cat == PaletteCat::Weapons ||
				 m_sel.cat == PaletteCat::Armor) {
			// Weapons and armor are item entities too — same placement path.
			// A niche on the clicked WALL takes the item (piled in its pocket);
			// a floor cell places on the floor as usual.
			if (!remote)
				if (auto faces = m_world->NicheFacesAt(cx, cz); !faces.empty()) {
					ok = m_world->AddNicheItem(id, faces[0].x, faces[0].z, faces[0].wall);
					log(loc::Format(ok ? "map.place.done" : "map.place.blocked",
									items[m_sel.index].label));
					changed = ok;
					break;
				}
			// The quarter the ghost drew. Remote placement has no live items to
			// pick a free quarter against, so it authors none and the loader
			// fills in order — which is why ResolveBrush leaves `slot` at -1
			// there rather than inventing one.
			ok = remote ? m_world->AddItemRemote(stem, id, cx, cz)
						: m_world->AddItem(id, cx, cz, place.slot);
		}
		else if (wallBrush) // a `mount = wall` decoration hangs on the picked face
			ok = remote ? m_world->AddDecorationRemote(stem, id, px, pz, face.wall)
						: m_world->AddWallDecoration(id, px, pz, face.wall);
		else
			ok = remote ? m_world->AddDecorationRemote(stem, id, cx, cz)
						: m_world->AddDecoration(id, cx, cz, Direction::South);
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
		changed = m_world->AddStairAt(stem, items[m_sel.index].id, cx, cz);
		// A way out leads to a world-map location, which only the owner knows:
		// ask now, while the placement is fresh (play-test #1).
		if (changed && !remote && onExitPlaced &&
			CatalogBool(m_world->GetProject().stairs.Find(items[m_sel.index].id), "exit",
						false))
			onExitPlaced(cx, cz);
		break;
	}
	case PaletteCat::Doors: {
		if (dragging) break; // placement is a single click
		const std::vector<PaletteItem> items = CategoryItems(m_sel.cat);
		if (m_sel.index < 0 || m_sel.index >= static_cast<int>(items.size())) break;
		const std::string& id = items[m_sel.index].id;
		// Doors go through the resolver like everything else now. They used to
		// call AddDoor and let IT find the doorway, which gave the same answer —
		// both bottom out in DungeonMap::DoorwayFacing — but by coincidence
		// rather than by construction, and it left the ghost describing a
		// decision the commit was making again on its own. One refusal path too:
		// the "no doorway here" line is the resolver's, so a door refuses in the
		// same voice as every other brush.
		const Placement place = pre ? *pre : ResolveBrush(cx, cz, face);
		if (!place.valid) {
			log(loc::Format(place.refusalKey ? place.refusalKey : "map.place.blocked",
							items[m_sel.index].label));
			break;
		}
		const bool ok = remote ? m_world->AddDoorRemote(stem, id, place.x, place.z)
							   : m_world->AddDoor(id, place.x, place.z, place.facing);
		if (ok)
			log(loc::Format("map.place.done", items[m_sel.index].label));
		changed = ok;
		break;
	}
	default:
		break;
	}

	if (ownStep) m_world->CommitUndoStep(changed);
	else m_strokeChanged = m_strokeChanged || changed;
}

bool MapEditor::LayingRoute() const {
	return m_routeId != 0 && m_world && m_world->MonsterPatrol(m_routeId) != nullptr;
}

void MapEditor::ResetSession() {
	m_routeId = 0;
	// Dropped, not committed: the step it would close belongs to the world that
	// is going, and dies with it.
	m_strokeOpen = false;
	m_strokeChanged = false;
	m_moving = false;
	m_selX = m_selZ = -1;
	m_selMonster = 0;
	preview = {};
}

void MapEditor::BeginStroke() {
	if (m_strokeOpen) return;
	m_world->BeginUndoStep();
	m_strokeOpen = true;
	m_strokeChanged = false;
}

void MapEditor::EndStroke() {
	if (!m_strokeOpen) return;
	m_strokeOpen = false;
	m_world->CommitUndoStep(m_strokeChanged);
}

void MapEditor::PaintThemeCell(int cx, int cz, bool remote, const std::string& stem) {
	const std::vector<PaletteItem> items = CategoryItems(PaletteCat::Themes);
	if (m_sel.index < 0 || m_sel.index >= static_cast<int>(items.size())) return;
	const DungeonMap& map = m_view.ViewedMap();
	if (cx < 0 || cz < 0 || cx >= map.Width() || cz >= map.Height()) return;
	PaintThemeAs(items[m_sel.index].id, cx, cz, map.IsWalkable(cx, cz), remote, stem);
}

void MapEditor::PaintThemeAs(const std::string& id, int cx, int cz, bool open, bool remote,
							 const std::string& stem) {
	using SS = DungeonWorld::SurfaceSel;
	const CatalogEntry* def = m_world->GetProject().themes.Find(id);
	if (!def) return;
	const ThemeMembers members = DungeonWorld::ThemeMembersOf(*def);
	// The surfaces this square shows, and only those the theme speaks
	// for: an empty member list leaves that surface exactly as it is.
	const SS surfaces[2] = {open ? SS::Floor : SS::Wall, SS::Ceiling};
	const int count = open ? 2 : 1;
	bool any = false;
	for (int i = 0; i < count; ++i) any = any || !members[static_cast<size_t>(surfaces[i])].empty();
	if (!any) return;
	const int variant = m_world->EnsureThemeVariant(stem, id);
	if (variant == -1) return;
	for (int i = 0; i < count; ++i) {
		if (members[static_cast<size_t>(surfaces[i])].empty()) continue;
		if (remote) m_world->EditVariantRemote(stem, cx, cz, surfaces[i], variant);
		else m_world->EditVariant(cx, cz, surfaces[i], variant);
	}
}

std::string MapEditor::ArmedId() const {
	if (m_sel.index < 0) return {};
	const std::vector<PaletteItem> items = CategoryItems(m_sel.cat);
	return m_sel.index < static_cast<int>(items.size()) ? items[m_sel.index].id : std::string();
}

void MapEditor::PaintCell(int cx, int cz, bool remote, const std::string& stem) {
	using SS = DungeonWorld::SurfaceSel;
	if (!PaintableCat(m_sel.cat)) return; // placement never reaches here
	if (m_sel.cat == PaletteCat::Themes) { // a theme sets a whole look
		PaintThemeCell(cx, cz, remote, stem);
		return;
	}
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
	const int variant = m_world->EnsureSurfaceVariant(stem, sel, items[m_sel.index].id);
	if (variant < 0) return;
	// The texture brush owns the CELL TYPE too: painting a wall texture on a
	// floor square raises the wall, a floor/ceiling texture carves solid rock
	// walkable, then the variant lands on the converted square — these ARE
	// the structural brushes (the old Structure Wall/Floor rows folded in).
	const Cell want = sel == SS::Wall ? Cell::Wall : Cell::Floor;
	if (remote) {
		m_world->EditCellRemote(stem, cx, cz, want); // no-op when already right
		m_world->EditVariantRemote(stem, cx, cz, sel, variant);
		return;
	}
	if (m_world->Map().At(cx, cz) != want) {
		const Party& party = m_world->GetParty();
		if (want == Cell::Wall && cx == party.GridX() && cz == party.GridZ())
			return; // never wall the party in (skip; a fill keeps going)
		m_world->BeginChunkBatch(); // the type change and the variant: one rebuild
		m_world->EditCell(cx, cz, want);
		m_world->EditVariant(cx, cz, sel, variant);
		m_world->EndChunkBatch();
		return;
	}
	m_world->EditVariant(cx, cz, sel, variant);
}

void MapEditor::PaintRect(int cx, int cz) {
	if (m_sel.index < 0) return;
	// Placement categories (and a rect with no anchor yet) act as a plain click.
	if (!PaintableCat(m_sel.cat) || m_lastX < 0) {
		ApplyBrush(cx, cz, /*dragging*/ false);
		return;
	}
	PaintRectBetween(m_lastX, m_lastZ, cx, cz);
}

void MapEditor::PaintRectBetween(int ax, int az, int bx, int bz) {
	if (m_sel.index < 0) return;
	if (!PaintableCat(m_sel.cat)) { // placement acts as a plain click
		ApplyBrush(bx, bz, /*dragging*/ false);
		return;
	}
	const DungeonMap& map = m_view.ViewedMap();
	const int x0 = std::max(0, std::min(ax, bx)), x1 = std::min(map.Width() - 1, std::max(ax, bx));
	const int z0 = std::max(0, std::min(az, bz)), z1 = std::min(map.Height() - 1, std::max(az, bz));
	std::vector<std::pair<int, int>> cells;
	for (int z = z0; z <= z1; ++z)
		for (int x = x0; x <= x1; ++x) cells.push_back({x, z});
	PaintCells(cells);
	m_lastX = bx; // chainable: the far corner anchors the next rectangle
	m_lastZ = bz;
	if (m_world->onMessage) m_world->onMessage(loc::FormatLine("map.fill.done", cells.size()));
}

MapEditor::Tool MapEditor::ActiveTool() const {
	const int t = m_settings.mapTool;
	return t >= 0 && t < static_cast<int>(Tool::Count) ? static_cast<Tool>(t) : Tool::Paint;
}

void MapEditor::SetTool(Tool t) {
	if (static_cast<int>(t) == m_settings.mapTool) return;
	m_settings.mapTool = static_cast<int>(t);
	m_settings.Save();
}

const char* MapEditor::ToolName(Tool t) {
	switch (t) {
	case Tool::Rect:  return "rect";
	case Tool::Flood: return "flood";
	case Tool::Area:  return "area";
	case Tool::Pick:  return "pick";
	case Tool::Corridor: return "corridor";
	case Tool::Room:     return "room";
	case Tool::Stamp:    return "stamp";
	case Tool::Region:   return "region";
	default:          return "paint";
	}
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
	// A theme floods over whichever surface the clicked square shows
	// (its wall on a block, its floor on open ground) - it paints both kinds.
	const SS sel = m_sel.cat == PaletteCat::Themes
					   ? (baseCell == Cell::Wall ? SS::Wall : SS::Floor)
				   : m_sel.cat == PaletteCat::Walls  ? SS::Wall
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
	m_world->BeginUndoStep();
	const u32 rev0 = m_world->Map().Revision();
	m_world->BeginChunkBatch(); // each touched chunk rebuilds once, at the end
	for (const auto& [x, z] : region) PaintCell(x, z, remote, stem);
	m_world->EndChunkBatch();
	m_world->CommitUndoStep(remote || m_world->Map().Revision() != rev0);
	m_lastX = cx;
	m_lastZ = cz;
	if (m_world->onMessage)
		m_world->onMessage(loc::FormatLine("map.fill.done", region.size()));
}

void MapEditor::PaintCells(std::span<const std::pair<int, int>> cells) {
	const bool remote = m_view.Browsing();
	const std::string stem = m_view.ViewedLevel(); // a copy: nothing here re-browses
	m_world->BeginUndoStep();
	const u32 rev0 = m_world->Map().Revision();
	m_world->BeginChunkBatch(); // each touched chunk rebuilds once, at the end
	for (const auto& [x, z] : cells) PaintCell(x, z, remote, stem);
	m_world->EndChunkBatch();
	m_world->CommitUndoStep(remote || m_world->Map().Revision() != rev0);
}

void MapEditor::AreaFill(int cx, int cz) {
	if (m_sel.index < 0) return;
	if (!PaintableCat(m_sel.cat)) { // placement acts as a plain click
		ApplyBrush(cx, cz, /*dragging*/ false);
		return;
	}
	const DungeonMap& map = m_view.ViewedMap();
	std::vector<area::CellXZ> region = area::Region(map, cx, cz);
	if (region.empty()) {
		if (m_world->onMessage) m_world->onMessage(loc::View("map.area.solid"));
		return;
	}
	// A wall brush paints the blocks the area SEES; floor and ceiling brushes
	// the area itself. Either way PaintCell's type rule leaves the cell types
	// alone, since each square already is the kind its brush wants.
	// A THEME takes both - the room and its walls - which is the whole
	// point of one: "make this room marble hall" in a single click.
	if (m_sel.cat == PaletteCat::Walls) region = area::Walls(map, region);
	else if (m_sel.cat == PaletteCat::Themes) {
		const std::vector<area::CellXZ> walls = area::Walls(map, region);
		region.insert(region.end(), walls.begin(), walls.end());
	}
	PaintCells(region);
	m_lastX = cx;
	m_lastZ = cz;
	if (m_world->onMessage)
		m_world->onMessage(loc::FormatLine("map.area.done", region.size()));
}

void MapEditor::FillLevel() {
	if (m_sel.index < 0 || !PaintableCat(m_sel.cat)) return;
	const DungeonMap& map = m_view.ViewedMap();
	const bool walls = m_sel.cat == PaletteCat::Walls;
	const bool themed = m_sel.cat == PaletteCat::Themes; // every square, both kinds
	std::vector<area::CellXZ> cells;
	for (int z = 0; z < map.Height(); ++z)
		for (int x = 0; x < map.Width(); ++x)
			if (themed || map.IsWalkable(x, z) != walls) cells.push_back({x, z});
	PaintCells(cells);
	if (m_world->onMessage)
		m_world->onMessage(loc::FormatLine("map.level.filled", cells.size()));
}

void MapEditor::PickAt(int cx, int cz) {
	const DungeonMap& map = m_view.ViewedMap();
	if (cx < 0 || cz < 0 || cx >= map.Width() || cz >= map.Height()) return;
	using SS = DungeonWorld::SurfaceSel;
	// A solid square arms its wall texture, a floor square its floor texture;
	// ceilings (sharing the floor square) are picked while already on the
	// Ceilings brush.
	const bool solid = map.At(cx, cz) == Cell::Wall;
	// A square painted with a THEME picks up the theme - that is
	// the thing to paint elsewhere to make it match - not the one member the
	// hash happened to show here.
	if (const int slot = DungeonMap::ThemeSlotOf(
			map.Variant(solid ? SS::Wall : SS::Floor, cx, cz));
		slot >= 0 && slot < static_cast<int>(map.ThemeCount())) {
		const std::string& id = map.ThemeId(slot);
		const std::vector<PaletteItem> items = CategoryItems(PaletteCat::Themes);
		for (int i = 0; i < static_cast<int>(items.size()); ++i)
			if (items[i].id == id) {
				m_sel = {PaletteCat::Themes, i};
				if (m_world->onMessage)
					m_world->onMessage(loc::FormatLine("map.pick.done", items[i].label));
				return;
			}
	}
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
			if (m_world->onMessage)
				m_world->onMessage(loc::FormatLine("map.pick.done", items[i].label));
			return;
		}
}

int MapEditor::ResolvedVariant(int cx, int cz, int selRaw) const {
	using SS = DungeonWorld::SurfaceSel;
	const SS sel = static_cast<SS>(selRaw);
	const DungeonMap& map = m_view.ViewedMap();
	const int count = static_cast<int>(map.Palette(sel).size());
	if (count == 0) return -1;
	// StampCell's exact answer, the same one the map's textured fill draws.
	return static_cast<int>(
		ResolveSurfaceVariant(map, cx, cz, sel, static_cast<u32>(count)));
}

void MapEditor::InspectAt(int cx, int cz) {
	const bool remote = m_view.Browsing();
	const DungeonMap& map = m_view.ViewedMap();
	auto log = [&](const std::string& s) {
		if (m_world->onMessage) m_world->onMessage(s);
	};
	// EVERY WORD of the line is the language's (code-review C107): the base and
	// the counts used to be English spliced into a localised pattern, so a German
	// editor read "Zelle 3, 4: wall, 2 monsters". A count takes its `.one` key
	// for one, the overview's convention (map.ov.levelmonsters), since how a
	// count is said is the language's rule; the list joins through a key too.
	const std::string_view base =
		loc::View(map.At(cx, cz) == Cell::Wall ? "map.select.wall" : "map.select.floor");
	if (remote) {
		// No live instances on a browsed level — report the static base only;
		// the inspectors need the level active.
		log(loc::Format("map.select.contents", cx, cz, base));
		return;
	}
	int props = 0;
	for (const auto& m : m_world->DecorationMarkers())
		if (m.x == cx && m.z == cz) ++props;
	int mons = 0;
	for (const auto& m : m_world->MonsterMarkers())
		if (m.x == cx && m.z == cz) ++mons;
	const auto counted = [](int n, std::string_view many, std::string_view one) {
		return n == 1 ? loc::Tr(one) : loc::Format(many, n);
	};
	std::string details(base);
	if (mons)
		details = loc::Format("map.joined", details,
							  counted(mons, "map.select.monsters", "map.select.monsters.one"));
	if (props)
		details = loc::Format("map.joined", details,
							  counted(props, "map.select.props", "map.select.props.one"));
	log(loc::Format("map.select.contents", cx, cz, details));
	// Select the square (highlight + patrol-route overlay) and open the
	// inspector right away when it holds an editable object — the owner
	// (onInspect) picks the dialog, via the chooser when several share it.
	m_selX = cx;
	m_selZ = cz;
	m_selMonster = m_world->MonsterRuntimeIdAt(cx, cz);
	if (m_world->AnyInspectableAt(cx, cz) && onInspect) onInspect(cx, cz);
}

bool MapEditor::EraseAt(int cx, int cz, const WallFace& face) {
	using SS = DungeonWorld::SurfaceSel;
	const bool remote = m_view.Browsing();
	const std::string& stem = m_view.ViewedLevel();
	auto log = [&](const std::string& s) {
		if (m_world->onMessage) m_world->onMessage(s);
	};
	m_world->BeginUndoStep();
	bool changed = true; // every rung but the last removes something
	if (remote) { // the stash-side ladder messages for itself
		changed = m_world->EraseRemote(stem, cx, cz);
	} else if (m_world->RemoveStairAt(cx, cz)) {
		// stairs message themselves (they name the paired level's cleanup)
	} else if (face.valid &&
			   (m_world->RemoveFixtureAtFace(face.x, face.z, face.wall) ||
				m_world->RemoveNicheAtFace(face.x, face.z, face.wall))) {
		// Wall things are placed per FACE, so one cell/block can carry several:
		// erase the one being POINTED at before the cell-wide rungs below (which
		// take whichever they find first). Sconce before niche, matching the
		// order of the cell-wide ladder.
		log(loc::Tr("map.erase.removed"));
	} else if (m_world->RemoveEntityAt(cx, cz) || m_world->RemoveFixtureAt(cx, cz) ||
			   m_world->RemoveNicheAtWall(cx, cz) || m_world->RemoveBoreAt(cx, cz) ||
			   // Below the wall rungs: a floor recess is the cell's own floor, so
			   // erasing it should not beat anything STANDING on that floor.
			   m_world->RemoveFeatureAt(cx, cz)) {
		log(loc::Tr("map.erase.removed"));
	} else {
		// The last rung resets the surface overrides - and a square that had
		// none is unchanged. It used to take an undo step regardless ("the
		// ladder always acts"), so a Ctrl+Z after it took back nothing, the
		// paint rule's mistake (a no-op is not an edit).
		const u32 rev = m_world->Map().Revision();
		m_world->EditVariant(cx, cz, SS::Wall, -1);
		m_world->EditVariant(cx, cz, SS::Floor, -1);
		m_world->EditVariant(cx, cz, SS::Ceiling, -1);
		changed = m_world->Map().Revision() != rev;
		log(loc::Format("map.erase.reset", cx, cz));
	}
	m_world->CommitUndoStep(changed);
	return changed;
}

void MapEditor::RenderBody(gfx::SpriteBatch& batch, const ui::Theme& theme,
						   const gfx::Rect& panel) {
	const ui::Font& font = m_view.Font();
	const float dpad = MapView::DockPad(panel);

	RenderCategoryBar(batch, theme, panel);
	m_rowTip.clear(); // the row drawing below sets it again if still hovered
	m_drawnSwatches.clear(); // ...and the swatches it draws, for the survey

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
		// The square box icons, brightened on hover and dimmed when disabled
		// (the toolbar's idiom); the text face only when the art is missing.
		auto iconBox = [&](const gfx::Rect& r, const gfx::Texture* icon, const char* text,
					   bool hot, bool enabled) {
			if (!icon) {
				ui::DrawButtonFace(batch, font, r, text, theme, hot, false, enabled);
				return;
			}
			const float f = !enabled ? 0.32f : hot ? 1.15f : 0.9f;
			batch.DrawSprite(r, {0, 0, 1, 1}, *icon, {f, f, f, 1.0f});
		};
		iconBox(FilterClearRect(panel), m_icoClear, "x",
				m_hotCtrl == HotCtrl::Clear && !m_filter.empty(), !m_filter.empty());
		iconBox(CollapseAllRect(panel), m_icoCollapse, "-",
				m_hotCtrl == HotCtrl::Collapse, true);
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
	// A header's expand/collapse mark: the square box at text height, or the
	// "+"/"-" glyph without the art. arrowW is the width it takes, so the label
	// after it lands the same distance away either way.
	const bool boxes = m_icoExpand && m_icoCollapse;
	const float arrowW = boxes ? font.Height() : font.MeasureWidth("+");
	auto expander = [&](bool open, float x, const gfx::Rect& rc, float ty) {
		if (boxes) {
			const float s = std::min(arrowW, rc.h - 2.0f);
			batch.DrawSprite({x, rc.y + (rc.h - s) * 0.5f, s, s}, {0, 0, 1, 1},
							 *(open ? m_icoCollapse : m_icoExpand),
							 {0.9f, 0.9f, 0.9f, 1.0f});
		} else {
			font.Draw(batch, open ? "-" : "+", x, ty, theme.textDim);
		}
	};
	std::vector<PaletteItem> items; // the current category's items
	// A row standing for another section's type (PaletteItem::ref) lights when
	// THAT type is armed - once per draw, since ArmedId resolves a section.
	const PaletteCat armedCat = ArmedCat();
	const std::string armedId = ArmedId();
	// A name that does not fit is trimmed with ".." - back to a whole UTF-8
	// character, never mid-way through one - and, when its row is the hovered
	// one (`hoverIndex`), says itself in full in a tooltip (RenderOverlay).
	auto drawFitted = [&](const PaletteRow& r, int hoverIndex, const std::string& name,
						  float x, float room, float ty, const Vec4& ink) {
		if (font.MeasureWidth(name) <= room) {
			font.Draw(batch, name, x, ty, ink);
			return;
		}
		ui::DrawFittedText(batch, font, name, x, ty, room, ink);
		if (r.cat == m_hoverItem.cat && hoverIndex == m_hoverItem.index) {
			m_rowTip = name;
			m_rowTipAt = r.rect;
		}
	};
	for (const PaletteRow& r : rows) {
		const gfx::Rect& rc = r.rect;
		if (r.kind == PaletteRow::Kind::Header) items = CategoryItems(r.cat);
		if (rc.y + rc.h < body.y || rc.y > body.y + body.h) continue; // off-view
		const float ty = rc.y + (rc.h - font.Height()) * 0.5f;
		switch (r.kind) {
		case PaletteRow::Kind::Header: {
			batch.DrawRect(rc, theme.control);
			ui::DrawBorder(batch, rc, theme.panelBorder);
			expander(m_catOpen[static_cast<size_t>(r.cat)], rc.x + dpad, rc, ty);
			const float hx = rc.x + dpad * 2 + arrowW;
			drawFitted(r, -2, loc::Tr(CategoryNameKey(r.cat)), hx, rc.x + rc.w - dpad - hx, ty,
					   theme.text);
			break;
		}
		case PaletteRow::Kind::NewButton:
			font.Draw(batch, loc::Tr("map.cat.new"), rc.x + dpad * 3, ty, theme.accent);
			break;
		case PaletteRow::Kind::Empty:
			font.Draw(batch, loc::Tr("map.cat.empty"), rc.x + dpad * 3, ty, theme.textDim);
			break;
		case PaletteRow::Kind::Divider: {
			// The tags boundary: a hairline across the run's width, inset to the
			// items' indent. Deliberately a RULE and not a labelled "off-tag"
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
			int n = 0;
			for (const PaletteItem& it : items)
				if (it.group == r.group) ++n;
			std::string label = r.group;
			label[0] = static_cast<char>(
				std::toupper(static_cast<unsigned char>(label[0])));
			label += std::format(" ({})", n);
			expander(GroupOpen(r.cat, r.group), rc.x + dpad * 3, rc, ty);
			const float sx = rc.x + dpad * 4 + arrowW;
			// Trimmed, without a tooltip: hover never resolves to a sub-header
			// (-3 is no row's index), and its full name is one click away.
			drawFitted(r, -3, label, sx, rc.x + rc.w - dpad - sx, ty, theme.text);
			break;
		}
		case PaletteRow::Kind::Item: {
			if (r.index < 0 || r.index >= static_cast<int>(items.size())) break;
			const PaletteItem& item = items[r.index];
			const bool active = (m_sel.cat == r.cat && m_sel.index == r.index) ||
								(!item.ref.empty() && armedCat == RowCat(r.cat, item) &&
								 armedId == item.id) ||
								(r.cat == PaletteCat::Styles && item.ref.empty() &&
								 item.id == m_style) ||
								(r.cat == PaletteCat::Shapes && item.id == m_stamp);
			if (active) {
				batch.DrawRect(rc, theme.controlActive);
				ui::DrawBorder(batch, rc, theme.panelBorder);
			}
			// Grouped items indent one level past their sub-header.
			const float indent = dpad * (r.group.empty() ? 3.0f : 5.0f);
			const float sw = rc.h - dpad * 2;
			const gfx::Rect swatchAt{rc.x + indent, rc.y + dpad, sw, sw};
			ui::DrawSwatch(batch, swatchAt, items[r.index].Swatch());
			// Recorded only when the scissor left it whole: a part-clipped one
			// would be measured against the rows round it.
			if (item.icon && m_drawnSwatches.size() < kDrawnSwatchRoom &&
				swatchAt.y >= body.y && swatchAt.y + swatchAt.h <= body.y + body.h)
				m_drawnSwatches.push_back({item.icon, swatchAt});
			const float labelX = rc.x + indent + sw + dpad;
			const int band = std::clamp(items[r.index].band, 0, power::kBands);
			// The power band: five small pips at the row's end, `band` of them
			// lit in the band's colour (green, feeble, to red, the strongest).
			const float pip = std::max(3.0f, std::round(rc.h * 0.16f));
			const float gap = std::max(1.0f, std::round(pip * 0.4f));
			const float pipsW = band > 0 ? power::kBands * pip + (power::kBands - 1) * gap : 0.0f;
			const float pipsX = rc.x + rc.w - dpad - pipsW;
			// A GO-TO link at the row's end, where the thing lies somewhere the
			// editor can take you (a placed quest item).
			const bool link = !item.gotoLevel.empty();
			const gfx::Rect linkAt = GoToRect(rc);
			if (link) {
				const float gw = font.MeasureWidth(">");
				font.Draw(batch, ">", linkAt.x + (linkAt.w - gw) * 0.5f, ty, theme.accent);
			}
			// Any name that does not fit - before the pips or the link, or before
			// the dock's edge - is trimmed with ".." and the hovered one says itself
			// in full in a tooltip (RenderOverlay).
			const std::string& name = item.label;
			const float end = band > 0 ? pipsX : link ? linkAt.x : rc.x + rc.w;
			const float room = end - dpad - labelX;
			const Vec4& ink = active ? theme.text : theme.textDim;
			drawFitted(r, r.index, name, labelX, room, ty, ink);
			for (int i = 0; band > 0 && i < power::kBands; ++i) {
				const gfx::Rect p{pipsX + i * (pip + gap), rc.y + (rc.h - pip) * 0.5f, pip, pip};
				if (i < band) batch.DrawRect(p, kPowerBand[band - 1]);
				else ui::DrawBorder(batch, p, theme.panelBorder);
			}
			break;
		}
		}
	}
	batch.SetScissor(nullptr);
}

} // namespace dungeon::game
