// ============================================================================
// Game/MapView_Docks.cpp - the editor's docks: their dragged widths, and the
// right dock's two sections, the OVERVIEW and the symbol KEY
// (docs/tool-refinement-plan.md Phase 3; see the "docks" blocks in MapView.h).
//
// WIDTHS. Each dock is dragged at its inner edge and remembered as a share of
// the panel's width (settings.ini map_palette_width / map_legend_width). The
// clamps keep the grid the larger part of the panel whatever is dragged.
// Nothing else needed telling: the grid, the tool strip, the palette's body,
// the category bar's wrapping and the trimmed names are all measured from a
// dock's edge already.
//
// THE OVERVIEW is what the world, the viewed level's DUNGEON or the viewed
// LEVEL holds - summed from DungeonWorld::Census, one row per level, cached per
// edit revision - plus the live checker's findings there. A level or dungeon
// line browses to it; the issues line opens the Check report.
// ============================================================================
#include "Game/MapView.h"

#include "Core/Loc.h"
#include "Game/MapColors.h"
#include "UI/Controls.h" // ui::DrawBorder, DrawButtonFace
#include "UI/Font.h"

#include <algorithm>
#include <format>
#include <utility>

namespace dungeon::game {

namespace {
float RowH(const gfx::Rect& p) { return std::clamp(p.h * 0.040f, 20.0f, 36.0f); }
float GripW(const gfx::Rect& p) { return std::clamp(p.w * 0.004f, 5.0f, 10.0f); }

// The clamps. The floors keep a dock's contents legible; the ceiling is a
// share of the panel, so two docks at their widest still leave the grid the
// larger part of it.
constexpr float kMinLeft = 120.0f, kMinRight = 150.0f, kMaxShare = 0.30f;

float ClampWidth(float px, float min, const gfx::Rect& p) {
	return std::clamp(px, min, std::max(min, p.w * kMaxShare));
}

// The symbol key: a swatch (filled / outlined / triangle) + label per symbol.
// Party and start use the live theme accent, so the table is built per draw.
enum class Sym { Filled, Outline, Triangle };
struct KeySym {
	Sym sym;
	Vec4 color;
	const char* key;
};
constexpr int kKeyRows = 13;
std::array<KeySym, kKeyRows> KeyTable(const ui::Theme& theme) {
	return {{{Sym::Triangle, theme.accent, "map.key.party"},
			 {Sym::Outline, theme.accent, "map.key.start"},
			 {Sym::Filled, kEditorWall, "map.key.wall"},
			 {Sym::Filled, kEditorFloor, "map.key.floor"},
			 {Sym::Filled, kTorch, "map.key.torch"},
			 {Sym::Filled, kBrazier, "map.key.brazier"},
			 {Sym::Filled, kMonster, "map.key.monster"},
			 {Sym::Filled, kItem, "map.key.item"},
			 {Sym::Filled, kButton, "map.key.button"},
			 {Sym::Filled, kDecoration, "map.key.decoration"},
			 {Sym::Filled, kDoor, "map.key.door"},
			 {Sym::Filled, kStair, "map.key.stairs"},
			 {Sym::Triangle, kProjParty, "map.key.projectile"}}};
}

constexpr const char* kScopeKeys[] = {"map.ov.world", "map.ov.dungeon", "map.ov.level"};

// Text trimmed to a width, ".." marking the cut.
std::string Fit(const ui::Font& font, const std::string& text, float room) {
	if (font.MeasureWidth(text) <= room) return text;
	std::string fit = text;
	while (fit.size() > 1 && font.MeasureWidth(fit + "..") > room) fit.pop_back();
	return fit + "..";
}
} // namespace

// --- widths ------------------------------------------------------------------------

float MapView::LeftDockW(const gfx::Rect& p) const {
	const float f = m_settings.mapPaletteWidth;
	return ClampWidth(f > 0.0f ? f * p.w : std::clamp(p.w * 0.16f, 120.0f, 260.0f), kMinLeft, p);
}

float MapView::RightDockW(const gfx::Rect& p) const {
	const float f = m_settings.mapLegendWidth;
	return ClampWidth(f > 0.0f ? f * p.w : std::clamp(p.w * 0.18f, 150.0f, 300.0f), kMinRight, p);
}

float MapView::DockWidth(Dock dock, const gfx::Rect& panel) const {
	return dock == Dock::Left ? LeftDockW(panel) : RightDockW(panel);
}

void MapView::SetDockWidth(Dock dock, float px, const gfx::Rect& panel) {
	if (dock == Dock::None || panel.w <= 0.0f) return;
	const float w = ClampWidth(px, dock == Dock::Left ? kMinLeft : kMinRight, panel);
	(dock == Dock::Left ? m_settings.mapPaletteWidth : m_settings.mapLegendWidth) = w / panel.w;
	m_settings.Save();
}

gfx::Rect MapView::DockGrip(Dock dock, const gfx::Rect& panel) const {
	if (m_mode != Mode::Editor) return {};
	const float g = GripW(panel), pad = DockPad(panel);
	if (dock == Dock::Left && !m_settings.mapPaletteCollapsed) {
		const gfx::Rect d = LeftDockRect(panel);
		const gfx::Rect b = LeftCollapseButton(panel);
		const float top = b.y + b.h + pad;
		return {d.x + d.w - g * 0.5f, top, g, d.y + d.h - top};
	}
	if (dock == Dock::Right && !LegendCollapsed()) {
		const gfx::Rect d = RightDockRect(panel);
		const gfx::Rect b = RightCollapseButton(panel);
		const float top = b.y + b.h + pad;
		return {d.x - g * 0.5f, top, g, d.y + d.h - top};
	}
	return {};
}

// --- the overview's content ------------------------------------------------------

MapView::OverviewScope MapView::Scope() const {
	return static_cast<OverviewScope>(std::clamp(m_settings.mapOverviewScope, 0, 2));
}

void MapView::SetScope(OverviewScope scope) {
	m_settings.mapOverviewScope = static_cast<int>(scope);
	m_settings.Save();
	m_rightScroll = 0.0f;
}

std::vector<MapView::OverviewLine> MapView::OverviewContent(OverviewScope scope) {
	std::vector<OverviewLine> out;
	if (!m_world) return out;
	const Project& proj = m_world->GetProject();
	const std::vector<DungeonWorld::LevelCensus>& census = m_world->Census();
	const std::string viewed = ViewedLevel();
	const std::string dungeon = ViewedDungeon();
	auto line = [&](const char* key, const char* labelKey, std::string value,
					std::string link = {}) {
		out.push_back({key, loc::Tr(labelKey), std::move(value), {}, std::move(link)});
	};

	// The title says what is being counted.
	OverviewLine title;
	title.key = "title";
	title.title = true;
	if (scope == OverviewScope::World) title.label = proj.name;
	else if (scope == OverviewScope::Level) title.label = viewed;
	else if (const CatalogEntry* d = proj.dungeons.Find(dungeon)) title.label = d->Display();
	out.push_back(title);
	if (scope == OverviewScope::Dungeon && dungeon.empty()) {
		out.back().label = loc::Tr("map.ov.nodungeon");
		return out;
	}

	auto counted = [&](const DungeonWorld::LevelCensus& c) {
		return scope == OverviewScope::World     ? true
			   : scope == OverviewScope::Dungeon ? c.dungeon == dungeon
												 : c.stem == viewed;
	};
	DungeonWorld::LevelCensus sum;
	int levels = 0;
	for (const DungeonWorld::LevelCensus& c : census) {
		if (!counted(c)) continue;
		++levels;
		sum.monsters += c.monsters;
		for (size_t b = 0; b < sum.bands.size(); ++b) sum.bands[b] += c.bands[b];
		sum.items += c.items;
		sum.questItems += c.questItems;
		sum.doors += c.doors;
		sum.lockedDoors += c.lockedDoors;
		sum.stairs += c.stairs;
		sum.buttons += c.buttons;
		sum.squares += c.squares;
		if (c.strongestPower > sum.strongestPower) {
			sum.strongestPower = c.strongestPower;
			sum.strongest = c.strongest;
		}
	}
	// The live checker's findings in the same scope: the world's include the
	// ones that belong to no level at all.
	int issues = 0;
	if (m_issues)
		for (const validate::Issue& is : *m_issues) {
			if (scope == OverviewScope::World) ++issues;
			else if (scope == OverviewScope::Level) issues += is.level == viewed;
			else
				for (const DungeonWorld::LevelCensus& c : census)
					if (c.stem == is.level && c.dungeon == dungeon) {
						++issues;
						break;
					}
		}

	// WHAT NEXT (Phase 7): where the viewed level stands in the four stages, as
	// the one line that says what to do about it - a link into that stage. Read
	// in order, so a level is never told to populate before it has a shape.
	if (scope == OverviewScope::Level) {
		// The [+] box is 9 squares; a level no bigger has not been built yet.
		constexpr int kUnbuilt = 9;
		if (sum.squares <= kUnbuilt)
			line("next", "map.ov.next", loc::Tr("map.ov.next.build"), "stage:build");
		else if (sum.monsters == 0)
			line("next", "map.ov.next", loc::Tr("map.ov.next.populate"), "populate");
		else if (issues > 0)
			line("next", "map.ov.next", loc::Format("map.ov.next.check", issues), "check");
		else
			line("next", "map.ov.next", loc::Tr("map.ov.next.ready"));
		line("squares", "map.ov.squares", std::to_string(sum.squares));
	}
	if (scope != OverviewScope::Level)
		line("levels", "map.ov.levels", std::to_string(levels));
	if (scope == OverviewScope::World) {
		int dungeons = 0;
		for (const CatalogEntry& d : proj.dungeons.Entries())
			if (!CatalogBool(&d, "hidden", false)) ++dungeons;
		line("dungeons", "map.ov.dungeons", std::to_string(dungeons));
	}
	line("monsters", "map.ov.monsters", std::to_string(sum.monsters));
	OverviewLine bands{"bands", loc::Tr("map.ov.bands"), {}, sum.bands, {}};
	for (size_t b = 0; b < sum.bands.size(); ++b)
		bands.value += (b ? "," : "") + std::to_string(sum.bands[b]);
	out.push_back(bands);
	if (!sum.strongest.empty()) {
		const CatalogEntry* m = proj.monsters.Find(sum.strongest);
		line("strongest", "map.ov.strongest", m ? m->Display() : sum.strongest);
	}
	line("items", "map.ov.items", std::to_string(sum.items));
	line("quest", "map.ov.quest", std::to_string(sum.questItems));
	// FLAGS belong to a scope rather than to a level: the world's, or a
	// dungeon's own (flags.cat `dungeon`). How many are on is the game being
	// played - the editor is a live view.
	if (scope != OverviewScope::Level) {
		const std::string want = scope == OverviewScope::World ? std::string() : dungeon;
		int flags = 0, on = 0;
		for (const CatalogEntry& f : proj.flags.Entries())
			if (f.Get("dungeon", "") == want) {
				++flags;
				on += m_world->FlagOn(f.id);
			}
		line("flags", "map.ov.flags",
			 on > 0 ? loc::Format("map.ov.flagson", flags, on) : std::to_string(flags));
	}
	line("doors", "map.ov.doors",
		 sum.lockedDoors > 0 ? loc::Format("map.ov.doorslocked", sum.doors, sum.lockedDoors)
							 : std::to_string(sum.doors));
	line("stairs", "map.ov.stairs", std::to_string(sum.stairs));
	if (scope == OverviewScope::Level)
		line("buttons", "map.ov.buttons", std::to_string(sum.buttons));
	line("issues", "map.ov.issues", std::to_string(issues), "check");

	// The way DOWN a level of the tree: the world lists its dungeons, a dungeon
	// its levels - each a link that browses there.
	if (scope == OverviewScope::World) {
		for (const CatalogEntry& d : proj.dungeons.Entries()) {
			if (CatalogBool(&d, "hidden", false)) continue;
			int n = 0;
			std::string first;
			for (const DungeonWorld::LevelCensus& c : census)
				if (c.dungeon == d.id) {
					if (first.empty()) first = c.stem;
					++n;
				}
			if (first.empty()) continue; // nowhere to browse to
			out.push_back({"dungeon:" + d.id, d.Display(),
						   n == 1 ? loc::Tr("map.ov.dungeonlevels.one")
								  : loc::Format("map.ov.dungeonlevels", n),
						   {}, first});
		}
	} else if (scope == OverviewScope::Dungeon) {
		for (const DungeonWorld::LevelCensus& c : census)
			if (c.dungeon == dungeon)
				out.push_back({"level:" + c.stem, c.stem,
							   c.monsters == 1 ? loc::Tr("map.ov.levelmonsters.one")
											   : loc::Format("map.ov.levelmonsters", c.monsters),
							   {}, c.stem});
	}
	return out;
}

void MapView::FollowOverviewLink(const std::string& link) {
	if (link == "check") {
		if (onValidate) onValidate();
	} else if (link == "populate") {
		// The generator dialog, whose Populate button is the stage's action.
		if (onGenerate) onGenerate();
	} else if (link.starts_with("stage:")) {
		// A palette stage, by name, in the Stage grouping (the guided order).
		if (!m_editor) return;
		const std::string_view name = std::string_view(link).substr(6);
		m_editor->SetPaletteGrouping(MapEditor::Grouping::Stage);
		for (int g = 0; g < MapEditor::GroupCount(MapEditor::Grouping::Stage); ++g)
			if (name == MapEditor::GroupName(MapEditor::Grouping::Stage, g)) m_editor->SetActiveGroup(g);
	} else if (!link.empty()) {
		SetViewLevel(link);
	}
}

// --- the right dock's layout ---------------------------------------------------------

gfx::Rect MapView::RightBody(const gfx::Rect& panel) const {
	const gfx::Rect d = RightDockRect(panel);
	const gfx::Rect b = RightCollapseButton(panel);
	const float pad = DockPad(panel);
	const float top = b.y + b.h + pad;
	return {d.x + pad, top, d.w - pad * 2, d.y + d.h - top - pad};
}

void MapView::BuildRightRows(const gfx::Rect& panel, std::vector<DockRow>& out,
							 float& contentH) {
	out.clear();
	const gfx::Rect body = RightBody(panel);
	const float rowH = RowH(panel), pad = DockPad(panel);
	float y = body.y - m_rightScroll;
	out.push_back({DockRow::Kind::Section, {body.x, y, body.w, rowH}, 0});
	y += rowH + pad * 0.5f;
	if (!m_settings.mapOverviewCollapsed) {
		// World / Dungeon / Level: three equal buttons across the dock.
		const float bw = (body.w - pad * 2) / 3.0f;
		for (int s = 0; s < 3; ++s)
			out.push_back({DockRow::Kind::Scope, {body.x + s * (bw + pad), y, bw, rowH}, s});
		y += rowH + pad * 0.5f;
		for (int i = 0; i < static_cast<int>(m_overview.size()); ++i) {
			out.push_back({DockRow::Kind::Line, {body.x, y, body.w, rowH}, i});
			y += rowH;
		}
		y += pad;
	}
	out.push_back({DockRow::Kind::Section, {body.x, y, body.w, rowH}, 1});
	y += rowH + pad * 0.5f;
	if (!m_settings.mapKeyCollapsed)
		for (int k = 0; k < kKeyRows; ++k) {
			out.push_back({DockRow::Kind::Key, {body.x, y, body.w, rowH}, k});
			y += rowH;
		}
	contentH = y + m_rightScroll - body.y;
}

// --- input --------------------------------------------------------------------------------

bool MapView::UpdateDocks(const Input& input, float mx, float my, const gfx::Rect& panel) {
	m_dockUpdated = true;
	// An edge drag in progress owns every frame until the release: the edge
	// follows the pointer (less where it was grabbed), and the release saves.
	if (m_dockDrag != Dock::None) {
		if (input.IsMouseDown(MouseButton::Left)) {
			const float edge = mx - m_dockDragOffset;
			const float frac = [&] {
				if (m_dockDrag == Dock::Left)
					return ClampWidth(edge - LeftDockRect(panel).x, kMinLeft, panel);
				const gfx::Rect rd = RightDockRect(panel);
				return ClampWidth(rd.x + rd.w - edge, kMinRight, panel);
			}() / panel.w;
			(m_dockDrag == Dock::Left ? m_settings.mapPaletteWidth : m_settings.mapLegendWidth) =
				frac;
		} else {
			m_dockDrag = Dock::None;
			m_settings.Save(); // once, on the release - not every frame of a drag
		}
		m_gripHover = m_dockDrag;
		return true;
	}
	m_gripHover = Dock::None;
	if (!m_levelsOpen) {
		if (DockGrip(Dock::Left, panel).Contains(mx, my)) m_gripHover = Dock::Left;
		else if (DockGrip(Dock::Right, panel).Contains(mx, my)) m_gripHover = Dock::Right;
	}
	if (m_gripHover != Dock::None && input.WasMousePressed(MouseButton::Left)) {
		m_dockDrag = m_gripHover;
		const float edge = m_dockDrag == Dock::Left
							   ? LeftDockRect(panel).x + LeftDockRect(panel).w
							   : RightDockRect(panel).x;
		m_dockDragOffset = mx - edge;
		return true;
	}

	// The right dock's body: section headers, the scope buttons, links, wheel.
	m_rightHover = -1;
	if (LegendCollapsed()) return false;
	if (!m_settings.mapOverviewCollapsed) m_overview = OverviewContent(Scope());
	const gfx::Rect body = RightBody(panel);
	if (!body.Contains(mx, my)) return false;
	std::vector<DockRow> rows;
	float contentH = 0.0f;
	BuildRightRows(panel, rows, contentH);
	for (const DockRow& r : rows)
		if (r.kind == DockRow::Kind::Line && r.rect.Contains(mx, my) &&
			!m_overview[static_cast<size_t>(r.index)].link.empty())
			m_rightHover = r.index;
	if (input.WheelDelta() != 0.0f)
		m_rightScroll = std::clamp(m_rightScroll - input.WheelDelta() * 28.0f, 0.0f,
								   std::max(0.0f, contentH - body.h));
	if (!input.WasMousePressed(MouseButton::Left)) return false;
	for (const DockRow& r : rows) {
		if (!r.rect.Contains(mx, my)) continue;
		switch (r.kind) {
		case DockRow::Kind::Section:
			(r.index == 0 ? m_settings.mapOverviewCollapsed : m_settings.mapKeyCollapsed) ^= true;
			m_settings.Save();
			m_rightScroll = 0.0f;
			break;
		case DockRow::Kind::Scope: SetScope(static_cast<OverviewScope>(r.index)); break;
		case DockRow::Kind::Line:
			FollowOverviewLink(m_overview[static_cast<size_t>(r.index)].link);
			break;
		default: break;
		}
		return true;
	}
	return true; // a click anywhere in the dock's body is the dock's
}

// --- drawing --------------------------------------------------------------------------------

void MapView::RenderRightDock(gfx::SpriteBatch& batch, const ui::Theme& theme,
							  const gfx::Rect& panel) {
	const gfx::Rect body = RightBody(panel);
	const float pad = DockPad(panel);
	const ui::Font& font = *m_font;
	std::vector<DockRow> rows;
	float contentH = 0.0f;
	BuildRightRows(panel, rows, contentH);
	const std::array<KeySym, kKeyRows> keys = KeyTable(theme);
	batch.SetScissor(&body);
	for (const DockRow& r : rows) {
		const gfx::Rect& rc = r.rect;
		if (rc.y + rc.h < body.y || rc.y > body.y + body.h) continue; // off-view
		const float ty = rc.y + (rc.h - font.Height()) * 0.5f;
		switch (r.kind) {
		case DockRow::Kind::Section: {
			// A header like the palette's: fill, border, expander, title.
			const bool open = r.index == 0 ? !m_settings.mapOverviewCollapsed
										   : !m_settings.mapKeyCollapsed;
			batch.DrawRect(rc, theme.control);
			ui::DrawBorder(batch, rc, theme.panelBorder);
			const float s = std::min(font.Height(), rc.h - 2.0f);
			if (const gfx::Texture* ico = open ? m_icoBoxMinus : m_icoBoxPlus)
				batch.DrawSprite({rc.x + pad, rc.y + (rc.h - s) * 0.5f, s, s}, {0, 0, 1, 1}, *ico,
								 {0.9f, 0.9f, 0.9f, 1.0f});
			else
				font.Draw(batch, open ? "-" : "+", rc.x + pad, ty, theme.textDim);
			font.Draw(batch, loc::Tr(r.index == 0 ? "map.ov.title" : "map.key"),
					  rc.x + pad * 2 + s, ty, theme.text);
			break;
		}
		case DockRow::Kind::Scope: {
			const std::string label = Fit(font, loc::Tr(kScopeKeys[r.index]), rc.w - 4.0f);
			ui::DrawButtonFace(batch, font, rc, label, theme, false,
							   static_cast<int>(Scope()) == r.index);
			break;
		}
		case DockRow::Kind::Line: {
			const OverviewLine& l = m_overview[static_cast<size_t>(r.index)];
			if (l.title) {
				font.Draw(batch, Fit(font, l.label, rc.w - pad), rc.x + pad * 0.5f, ty,
						  theme.accent);
				break;
			}
			const bool link = !l.link.empty();
			if (link && r.index == m_rightHover) {
				Vec4 wash = theme.controlHot;
				wash.w = 0.45f;
				batch.DrawRect(rc, wash);
			}
			if (l.key == "bands") {
				// A coloured square per power band, its count beside it.
				const float sq = std::max(4.0f, std::round(rc.h * 0.30f));
				const float step = (rc.w - pad) / 5.0f;
				for (size_t b = 0; b < l.bands.size(); ++b) {
					const float x = rc.x + pad * 0.5f + step * static_cast<float>(b);
					batch.DrawRect({x, rc.y + (rc.h - sq) * 0.5f, sq, sq}, kPowerBand[b]);
					font.Draw(batch, std::to_string(l.bands[b]), x + sq + 3.0f, ty,
							  l.bands[b] ? theme.text : theme.textDim);
				}
				break;
			}
			// Label left, value right (a link's value in the accent).
			const float vw = font.MeasureWidth(l.value);
			const float vx = rc.x + rc.w - pad * 0.5f - vw;
			font.Draw(batch, Fit(font, l.label, vx - rc.x - pad * 1.5f), rc.x + pad * 0.5f, ty,
					  link ? theme.text : theme.textDim);
			font.Draw(batch, l.value, vx, ty, link ? theme.accent : theme.text);
			break;
		}
		case DockRow::Kind::Key: {
			const KeySym& k = keys[static_cast<size_t>(r.index)];
			const float sw = rc.h - pad * 2;
			const gfx::Rect box{rc.x + pad * 0.5f, rc.y + pad, sw, sw};
			switch (k.sym) {
			case Sym::Filled: batch.DrawRect(box, k.color); break;
			case Sym::Outline: ui::DrawBorder(batch, box, k.color); break;
			case Sym::Triangle:
				batch.DrawTriangle({box.x + sw * 0.5f, box.y}, {box.x, box.y + sw},
								   {box.x + sw, box.y + sw}, k.color);
				break;
			}
			font.Draw(batch, loc::Tr(k.key), box.x + sw + pad, ty, theme.text);
			break;
		}
		}
	}
	batch.SetScissor(nullptr);
}

void MapView::RenderDockGrips(gfx::SpriteBatch& batch, const ui::Theme& theme,
							  const gfx::Rect& panel) {
	// No Update since the last draw (a modal dialog stops it): the hover this
	// remembers is stale, so let it go - and the resize arrow with it.
	if (!std::exchange(m_dockUpdated, false)) {
		m_gripHover = Dock::None;
		m_rightHover = -1;
	}
	for (const Dock d : {Dock::Left, Dock::Right}) {
		if (m_gripHover != d && m_dockDrag != d) continue;
		const gfx::Rect g = DockGrip(d, panel);
		if (g.w <= 0.0f) continue;
		// The edge itself, lit: a 2px accent line down the middle of the band.
		batch.DrawRect({g.x + g.w * 0.5f - 1.0f, g.y, 2.0f, g.h}, theme.accent);
	}
}

} // namespace dungeon::game
