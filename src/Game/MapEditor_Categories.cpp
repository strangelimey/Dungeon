// ============================================================================
// Game/MapEditor_Categories.cpp - the palette's category bar (see the
// "category bar" block in MapEditor.h; docs/tool-refinement-plan.md Phase 1).
//
// The palette used to be one accordion of every section, so a wall texture sat
// a scroll away from a quest and a dungeon. The bar picks a GROUP and the
// accordion lists only its sections. There are two groupings of the same
// sections - by workflow STAGE and by KIND - and the bar's first button flips
// between them. Each grouping is one table below; the compile-time checks under
// them are what stop a section from going missing from one grouping (it would
// be unreachable except by the filter) or appearing in two groups at once.
// ============================================================================
#include "Game/MapEditor.h"

#include "Core/Loc.h"
#include "Game/DungeonWorld.h"
#include "Game/GameSettings.h"
#include "Game/MapColors.h"
#include "Game/MapView.h"
#include "UI/Controls.h" // ui::DrawBorder, DrawButtonFace
#include "UI/Font.h"

#include <algorithm>
#include <string>

namespace dungeon::game {

namespace {
using PC = MapEditor::PaletteCat;
using Grouping = MapEditor::Grouping;

struct GroupDef {
	const char* name; // console name, icon suffix, map.group.<name>
	const PC* cats;
	size_t count;
};

// --- by STAGE: the workflow's order ------------------------------------------
// World holds what a world is made of above any one level; Build is the shape
// and the look (Themes leads, above the three surfaces it sets at once -
// Michael's call, a whole look is the first thing reached for); Populate is
// what goes into a built level.
constexpr PC kWorld[] = {PC::Dungeons, PC::Quests, PC::Terrain};
constexpr PC kBuild[] = {PC::Themes,       PC::Walls,           PC::Floors,
						 PC::Ceilings,     PC::WallFeatures,    PC::SurfaceFeatures,
						 PC::Doors,        PC::Stairs};
constexpr PC kPopulate[] = {PC::Monsters,    PC::Items,    PC::Weapons, PC::Armor,
							PC::Decorations, PC::Fixtures, PC::Buttons};
constexpr GroupDef kStageGroups[] = {
	{"world", kWorld, std::size(kWorld)},
	{"build", kBuild, std::size(kBuild)},
	{"populate", kPopulate, std::size(kPopulate)},
};

// --- by KIND: what a thing is -------------------------------------------------
constexpr PC kSurfaces[] = {PC::Themes,   PC::Walls,        PC::Floors,
							PC::Ceilings, PC::WallFeatures, PC::SurfaceFeatures};
constexpr PC kStructure[] = {PC::Doors, PC::Stairs, PC::Buttons, PC::Fixtures};
constexpr PC kProps[] = {PC::Decorations};
constexpr PC kCreatures[] = {PC::Monsters};
constexpr PC kItemKinds[] = {PC::Items, PC::Weapons, PC::Armor};
constexpr GroupDef kKindGroups[] = {
	{"surfaces", kSurfaces, std::size(kSurfaces)},
	{"structure", kStructure, std::size(kStructure)},
	{"props", kProps, std::size(kProps)},
	{"creatures", kCreatures, std::size(kCreatures)},
	{"items", kItemKinds, std::size(kItemKinds)},
	{"world", kWorld, std::size(kWorld)},
};

// Every category is in EXACTLY ONE group of a grouping, except the one the
// palette no longer lists at all.
constexpr bool Unlisted(PC c) { return c == PC::Effects; }

template <size_t N>
constexpr bool CoversEachOnce(const GroupDef (&groups)[N]) {
	for (int c = 0; c < static_cast<int>(PC::Count); ++c) {
		int seen = 0;
		for (const GroupDef& g : groups)
			for (size_t i = 0; i < g.count; ++i)
				if (static_cast<int>(g.cats[i]) == c) ++seen;
		if (seen != (Unlisted(static_cast<PC>(c)) ? 0 : 1)) return false;
	}
	return true;
}
static_assert(CoversEachOnce(kStageGroups),
			  "every listed PaletteCat must be in exactly one stage group");
static_assert(CoversEachOnce(kKindGroups),
			  "every listed PaletteCat must be in exactly one kind group");
static_assert(1 + std::size(kKindGroups) <= MapEditor::kMaxBarButtons &&
				  1 + std::size(kStageGroups) <= MapEditor::kMaxBarButtons,
			  "the bar holds the toggle plus every group of a grouping");

std::span<const GroupDef> Groups(Grouping g) {
	if (g == Grouping::Kind) return kKindGroups;
	return kStageGroups;
}

// The icon files, in one order both sides agree on: every stage group, every
// kind group, then the toggle's two faces (it shows the grouping in use).
constexpr const char* kIconNames[] = {
	"cat_world",	 "cat_build", "cat_populate", // stage
	"cat_surfaces", "cat_structure", "cat_props", "cat_creatures",
	"cat_items",	 "cat_world", // kind (World is the same group, same art)
	"cat_bystage",	 "cat_bykind", // the toggle
};
constexpr size_t kToggleIcon = std::size(kStageGroups) + std::size(kKindGroups);
static_assert(std::size(kIconNames) == kToggleIcon + 2, "one icon per group + two");
} // namespace

// --- the tables' accessors ------------------------------------------------------

MapEditor::Grouping MapEditor::PaletteGrouping() const {
	return m_settings.mapPaletteGrouping == 1 ? Grouping::Kind : Grouping::Stage;
}

int MapEditor::GroupCount(Grouping g) { return static_cast<int>(Groups(g).size()); }

const char* MapEditor::GroupName(Grouping g, int group) {
	const std::span<const GroupDef> gs = Groups(g);
	return group >= 0 && group < static_cast<int>(gs.size()) ? gs[group].name : "";
}

std::span<const MapEditor::PaletteCat> MapEditor::GroupCategories(Grouping g, int group) {
	const std::span<const GroupDef> gs = Groups(g);
	if (group < 0 || group >= static_cast<int>(gs.size())) return {};
	return {gs[group].cats, gs[group].count};
}

int MapEditor::ActiveGroup() const {
	const int raw = PaletteGrouping() == Grouping::Kind ? m_settings.mapPaletteKind
														: m_settings.mapPaletteStage;
	return std::clamp(raw, 0, GroupCount(PaletteGrouping()) - 1);
}

bool MapEditor::CategoryListed(PaletteCat cat) { return !Unlisted(cat); }

std::span<const char* const> MapEditor::CategoryIconNames() { return kIconNames; }

void MapEditor::SetCategoryIcons(std::span<const gfx::Texture* const> icons) {
	m_icoCats.fill(nullptr);
	for (size_t i = 0; i < icons.size() && i < m_icoCats.size(); ++i) m_icoCats[i] = icons[i];
}

// --- changing what shows ----------------------------------------------------------

void MapEditor::SetPaletteGrouping(Grouping g) {
	m_settings.mapPaletteGrouping = g == Grouping::Kind ? 1 : 0;
	m_settings.Save();
	m_paletteScroll = 0.0f;
	OpenSomethingInGroup();
}

void MapEditor::SetActiveGroup(int group) {
	group = std::clamp(group, 0, GroupCount(PaletteGrouping()) - 1);
	(PaletteGrouping() == Grouping::Kind ? m_settings.mapPaletteKind
										 : m_settings.mapPaletteStage) = group;
	m_settings.Save();
	m_paletteScroll = 0.0f;
	OpenSomethingInGroup();
}

void MapEditor::OpenSomethingInGroup() {
	const std::span<const PaletteCat> cats = GroupCategories(PaletteGrouping(), ActiveGroup());
	for (const PaletteCat c : cats)
		if (m_catOpen[static_cast<size_t>(c)]) return;
	if (!cats.empty()) m_catOpen[static_cast<size_t>(cats.front())] = true;
}

void MapEditor::RevealCategory(PaletteCat cat) {
	if (!CategoryListed(cat)) return;
	const Grouping g = PaletteGrouping();
	for (int i = 0; i < GroupCount(g); ++i)
		for (const PaletteCat c : GroupCategories(g, i))
			if (c == cat && i != ActiveGroup()) {
				(g == Grouping::Kind ? m_settings.mapPaletteKind
									 : m_settings.mapPaletteStage) = i;
				m_settings.Save();
				m_paletteScroll = 0.0f;
			}
	m_catOpen[static_cast<size_t>(cat)] = true;
}

void MapEditor::SetFilter(std::string_view text) {
	m_filter = std::string(text.substr(0, 24)); // the box's own limit
	m_paletteScroll = 0.0f;
}

std::vector<MapEditor::PaletteCat> MapEditor::CandidateSections() const {
	std::vector<PaletteCat> out;
	if (m_filter.empty()) {
		for (const PaletteCat c : GroupCategories(PaletteGrouping(), ActiveGroup()))
			out.push_back(c);
		return out;
	}
	// Filtering: every group of the grouping in use, in its order.
	const Grouping g = PaletteGrouping();
	for (int i = 0; i < GroupCount(g); ++i)
		for (const PaletteCat c : GroupCategories(g, i))
			if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
	return out;
}

std::vector<MapEditor::ShownSection> MapEditor::ShownSections() const {
	std::vector<ShownSection> out;
	for (const PaletteCat c : CandidateSections()) {
		const std::vector<PaletteItem> items = CategoryItems(c);
		int n = 0;
		for (const PaletteItem& it : items)
			if (MatchesFilter(it.label)) ++n;
		if (!m_filter.empty() && n == 0) continue; // drops out, as in the accordion
		out.push_back({c, n});
	}
	return out;
}

// --- geometry ----------------------------------------------------------------------

MapEditor::BarLayout MapEditor::CategoryBar(const gfx::Rect& panel) const {
	BarLayout out;
	const gfx::Rect body = m_view.PaletteBody(panel);
	const float pad = MapView::DockPad(panel);
	// The buttons are the controls row's height, so the bar and the filter row
	// under it read as one strip of chrome.
	const float s = std::clamp(panel.h * 0.040f, 20.0f, 36.0f);
	const int perRow = std::max(1, static_cast<int>((body.w + pad) / (s + pad)));
	auto rowsFor = [&](Grouping g) { return (1 + GroupCount(g) + perRow - 1) / perRow; };
	// Room for the LARGER grouping's rows, whichever is showing, so flipping the
	// toggle never moves the filter and the accordion under the pointer.
	const int rows = std::max(rowsFor(Grouping::Stage), rowsFor(Grouping::Kind));
	const int n = 1 + GroupCount(PaletteGrouping());
	for (int i = 0; i < n && out.count < out.buttons.size(); ++i) {
		const int r = i / perRow, c = i % perRow;
		out.buttons[out.count++] = {{body.x + c * (s + pad), body.y + r * (s + pad), s, s},
									i - 1};
	}
	out.area = {body.x, body.y, body.w, rows * s + (rows - 1) * pad};
	return out;
}

// --- drawing -------------------------------------------------------------------------

namespace {
// The icon index for a bar button: a group's own art, or the toggle's face for
// the grouping in use.
size_t IconIndex(Grouping g, int group) {
	if (group < 0) return kToggleIcon + (g == Grouping::Kind ? 1 : 0);
	return (g == Grouping::Kind ? std::size(kStageGroups) : 0) + static_cast<size_t>(group);
}

std::string TipFor(Grouping g, int group) {
	if (group < 0)
		return loc::Tr(g == Grouping::Kind ? "map.group.mode.kind" : "map.group.mode.stage");
	return loc::Tr(std::string("map.group.") + MapEditor::GroupName(g, group));
}
} // namespace

void MapEditor::RenderCategoryBar(gfx::SpriteBatch& batch, const ui::Theme& theme,
								  const gfx::Rect& panel) {
	const ui::Font& font = m_view.Font();
	const Grouping g = PaletteGrouping();
	const int active = ActiveGroup();
	const BarLayout bar = CategoryBar(panel);
	for (size_t i = 0; i < bar.count; ++i) {
		const BarButton& b = bar.buttons[i];
		const bool hot = m_hotCtrl == HotCtrl::Bar && m_hotBar == b.group;
		const bool picked = b.group == active;
		// While a filter is set the bar is not what decides the list, so the
		// picked group stops looking picked rather than contradict the rows.
		const bool showPicked = picked && m_filter.empty();
		if (const gfx::Texture* icon = m_icoCats[IconIndex(g, b.group)]) {
			const float f = hot ? 1.15f : showPicked ? 1.0f : 0.72f;
			batch.DrawSprite(b.rect, {0, 0, 1, 1}, *icon, {f, f, f, 1.0f});
		} else {
			// No art: the group's name, trimmed to the box.
			std::string label = b.group < 0 ? std::string(g == Grouping::Kind ? "K" : "S")
											: std::string(GroupName(g, b.group));
			while (label.size() > 1 && font.MeasureWidth(label) > b.rect.w - 4.0f)
				label.pop_back();
			ui::DrawButtonFace(batch, font, b.rect, label, theme, hot, showPicked);
		}
		if (showPicked) {
			// The picked group's ring, two hairlines thick (the tool strip's idiom).
			ui::DrawBorder(batch, b.rect, theme.accent);
			ui::DrawBorder(batch, {b.rect.x + 1, b.rect.y + 1, b.rect.w - 2, b.rect.h - 2},
						   theme.accent);
		}
	}
}

void MapEditor::RenderOverlay(gfx::SpriteBatch& batch, const ui::Theme& theme,
							  const gfx::Rect& panel) {
	if (m_hotCtrl != HotCtrl::Bar || m_hotBar < -1) return;
	const BarLayout bar = CategoryBar(panel);
	for (size_t i = 0; i < bar.count; ++i) {
		const BarButton& b = bar.buttons[i];
		if (b.group != m_hotBar) continue;
		// Under the hovered button (the toolbar's idiom), clamped into the panel:
		// the bar sits at the dock's left edge and a centred tip would run off it.
		const ui::Font& font = m_view.Font();
		const std::string tip = TipFor(PaletteGrouping(), b.group);
		const float dpad = MapView::DockPad(panel);
		const float tw = font.MeasureWidth(tip);
		const float p = dpad * 1.5f;
		gfx::Rect tr{b.rect.x + b.rect.w * 0.5f - tw * 0.5f - p, b.rect.y + b.rect.h + 2.0f,
					 tw + p * 2, font.Height() + p};
		tr.x = std::clamp(tr.x, panel.x + 2.0f, panel.x + panel.w - tr.w - 2.0f);
		batch.DrawRect(tr, kMapBg);
		ui::DrawBorder(batch, tr, theme.panelBorder);
		font.Draw(batch, tip, tr.x + p, tr.y + p * 0.5f, theme.text);
		return;
	}
}

// --- input -----------------------------------------------------------------------------

int MapEditor::BarButtonAt(float mx, float my, const gfx::Rect& panel) const {
	const BarLayout bar = CategoryBar(panel);
	for (size_t i = 0; i < bar.count; ++i)
		if (bar.buttons[i].rect.Contains(mx, my)) return bar.buttons[i].group;
	return -2;
}

bool MapEditor::OnBarClick(float mx, float my, const gfx::Rect& panel) {
	const int hit = BarButtonAt(mx, my, panel);
	if (hit < -1) return CategoryBar(panel).area.Contains(mx, my); // a gap still claims
	if (hit == -1)
		SetPaletteGrouping(PaletteGrouping() == Grouping::Kind ? Grouping::Stage
																: Grouping::Kind);
	else
		SetActiveGroup(hit);
	// Picking a group is a way of asking for its sections: a filter left in the
	// box would keep hiding them, so the pick clears it.
	if (hit >= 0 && !m_filter.empty()) SetFilter("");
	return true;
}

} // namespace dungeon::game
