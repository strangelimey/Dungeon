// ============================================================================
// Game/CharacterSheet.cpp — shell: ctor, SetCharacter, Update/Draw, mode strip.
// Tab bodies live in CharacterSheet_Inventory / _Stats / _Lists.
// ============================================================================
#include "Game/CharacterSheet.h"
#include "Game/CharacterSheetLayout.h"
#include "Game/PartyHudDraw.h"
#include "UI/Skin.h"

#include "Core/Loc.h"

namespace dungeon::game {
using namespace sheet;

// The sheet's name heading and bust initial, in rem of the SHEET. Both used to
// come from GameUI's 64px title Font passed down as a raw pointer; that 64 and
// the sheet's 22 were authored against the same 900px design window and scale
// by the same factor, so 64/22 rem is the old size EXACTLY, at any resolution,
// resolved by the widget itself. (Roles carry a face, not a size — see
// UIContext::FontAt.)
constexpr float kHeadingRem = 64.0f / 22.0f;

CharacterSheet::CharacterSheet(const gfx::Rect& rect,
							   std::vector<Character>* roster,
							   const ResourceBarStyle* barStyle,
							   const ItemIconBank* icons,
							   const ItemWeightBank* weights,
							   const ItemIconBank* slotIcons,
							   const ItemCategoryBank* categories,
							   HeldItem* held, bool card)
	: m_roster(roster), m_card(card), m_barStyle(barStyle),
	  m_icons(icons), m_weights(weights), m_slotIcons(slotIcons),
	  m_categories(categories), m_held(held),
	  m_healthLabel(loc::Tr("bar.health")),
	  m_staminaLabel(loc::Tr("bar.stamina")), m_manaLabel(loc::Tr("bar.mana")),
	  m_foodLabel(loc::Tr("bar.food")), m_waterLabel(loc::Tr("bar.water")),
	  m_attributesLabel(loc::Tr("sheet.attributes")),
	  m_skillsLabel(loc::Tr("sheet.skills")), m_noSkills(loc::Tr("sheet.no_skills")),
	  m_effectsLabel(loc::Tr("sheet.effects")),
	  m_noEffects(loc::Tr("sheet.no_effects")),
	  m_spellsLabel(loc::Tr("sheet.spells")),
	  m_noSpells(loc::Tr("sheet.no_spells")) {
	bounds = rect;
	debugName = "CharacterSheet";
	m_attrLabels = {loc::Tr("attr.strength"), loc::Tr("attr.dexterity"),
					loc::Tr("attr.vitality"), loc::Tr("attr.willpower"),
					loc::Tr("attr.intelligence")};
	BuildParts();
	// Room for a member's rows before the first open (RowPool): skills are
	// the schools + weapon classes + the three practices + two headings, and
	// the spells are bounded by the registry (16 in the demo).
	constexpr size_t kSkillRows = 24, kSpellRows = 32, kEffectRows = 16;
	m_skillRows.Warm(kSkillRows);
	m_spellRows.Warm(kSpellRows);
	m_effectRows.Warm(kEffectRows);
	m_spellOrder.reserve(64);
	// And the list widgets that show them (m_lists is in Mode order after
	// Stats: Skills, Spells, Effects).
	const size_t listRows[] = {kSkillRows, kSpellRows, kEffectRows};
	for (size_t n = 0; n < m_lists.size(); ++n)
		if (m_lists[n]) m_lists[n]->Warm(listRows[n]);
}

// The sheet's children. The two non-scrolling bodies (Inventory, Stats) stay
// drawn by the sheet itself against its own rect — they fill it, so "fractions
// of the sheet" is already parent-relative and a container would buy nothing.
// The three LIST tabs each get a SheetList, which is where the shared scroll
// lives (see the header).
void CharacterSheet::BuildParts() {
	// A card has neither: the party window shows the members' names on the
	// cards and keeps one row of tab stones for all four.
	if (!m_card) {
		Add<SheetPortrait>(gfx::Rect{kPortraitX, kPortraitY, kPortraitW, kPortraitH},
						   m_roster, &m_member);
		ui::Button* change = Add<ui::Button>(
			gfx::Rect{kPortraitBtnX, kPortraitBtnY, kPortraitBtnW, kPortraitBtnH},
			loc::Tr("sheet.portrait.change"), [this] {
				if (onChangePortrait) onChangePortrait();
			});
		change->debugName = "change-portrait";

		const float stripW = kModeCount * kModeBtnW + (kModeCount - 1) * kModeBtnGap;
		m_modeStrip = Add<ModeSelector>(gfx::Rect{kModeBtnX, kModeBtnY, stripW, kModeBtnH},
										kModeCount, &m_modeIndex, [this](int i) { SelectMode(i); });
	}

	// One list per scrolling tab, filling the sheet; each positions its heading
	// and scrolling band from the shared layout table.
	const struct {
		const std::string& heading;
		const std::string& empty;
		Mode mode;
	} lists[] = {
		{m_skillsLabel, m_noSkills, Mode::Skills},
		{m_spellsLabel, m_noSpells, Mode::Spells},
		{m_effectsLabel, m_noEffects, Mode::Effects},
	};
	for (size_t n = 0; n < std::size(lists); ++n) {
		SheetList::Counter count;
		SheetList::Measure measure;
		SheetRow::DrawFn draw;
		switch (lists[n].mode) {
		case Mode::Skills:
			count = [this] { return m_skillRows.size(); };
			measure = [this](size_t i, ui::UIContext& c, const ui::Font& f, float w) {
				return MeasureSkillRow(i, c, f, w);
			};
			draw = [this](size_t i, ui::UIContext& c, gfx::SpriteBatch& b,
						  const gfx::Rect& r) { DrawSkillRow(i, c, b, r); };
			break;
		case Mode::Spells:
			count = [this] { return m_spellRows.size(); };
			measure = [this](size_t i, ui::UIContext& c, const ui::Font& f, float w) {
				return MeasureSpellRow(i, c, f, w);
			};
			draw = [this](size_t i, ui::UIContext& c, gfx::SpriteBatch& b,
						  const gfx::Rect& r) { DrawSpellRow(i, c, b, r); };
			break;
		default:
			count = [this] { return m_effectRows.size(); };
			measure = [this](size_t i, ui::UIContext& c, const ui::Font& f, float w) {
				return MeasureEffectRow(i, c, f, w);
			};
			draw = [this](size_t i, ui::UIContext& c, gfx::SpriteBatch& b,
						  const gfx::Rect& r) { DrawEffectRow(i, c, b, r); };
			break;
		}
		// The list takes the BAND it actually occupies — heading line included —
		// not the whole sheet. It used to be {0,0,1,1} with the band expressed
		// as sheet fractions, which meant it lay across the portrait and the
		// mode strip: an area claimed and not drawn in, which is exactly what
		// `uioverlap` is for. Inside its own rect the heading is at the top and
		// the band runs to the bottom.
		constexpr float kListTop = kHeaderY;
		constexpr float kListH = (1.0f - kScrollBottomPad) - kListTop;
		auto* list = Add<SheetList>(gfx::Rect{0, kListTop, 1, kListH},
									lists[n].heading, lists[n].empty,
									std::move(count), std::move(measure),
									std::move(draw));
		list->headingY = 0.0f;
		list->bandBottom = 1.0f;
		m_lists[n] = list;
	}
}

void CharacterSheet::SetCharacter(size_t member) {
	m_member = member;
	m_character = RosterMember(m_roster, m_member);
	for (SheetList* list : m_lists) // a fresh member starts its tabs at the top
		if (list) list->ScrollToTop();
	if (!m_character) return; // out of range — the sheet body just stays empty
	BakeStats();
	BakeSkills();
	BakeSpells();
	BakeEffects();
}

// A mode button, or Tab / Shift+Tab (StepMode): the lists start at the top
// whenever the tab actually changes.
void CharacterSheet::SelectMode(int i) {
	const Mode next = static_cast<Mode>(i);
	if (next != m_mode)
		for (SheetList* list : m_lists)
			if (list) list->ScrollToTop();
	m_mode = next;
	m_modeIndex = i;
}

void CharacterSheet::StepMode(int delta) {
	SelectMode(((static_cast<int>(m_mode) + delta) % kModeCount + kModeCount) % kModeCount);
}

// Only the active tab's list takes part in the walk.
void CharacterSheet::LayoutSelf(ui::UIContext&) {
	m_modeIndex = static_cast<int>(m_mode);
	const Mode listModes[] = {Mode::Skills, Mode::Spells, Mode::Effects};
	for (size_t n = 0; n < m_lists.size(); ++n)
		if (m_lists[n]) m_lists[n]->visible = m_mode == listModes[n] && m_character;
}

// The mode strip and the active list have already had the mouse (they are
// children); this handles the inventory body and then swallows the rest.
void CharacterSheet::UpdateSelf(ui::UIContext& ctx) {
	m_character = RosterMember(m_roster, m_member);
	// The world keeps running under the sheet (it is not a pause), so the
	// tabs that change on their own are re-baked every frame: skills train
	// (regeneration practises the resource skills while hurt) and effects tick
	// down. Cheap and allocation-free - the RowPools only overwrite. Here
	// rather than in a draw so the render pass lays out the fresh rows.
	// Spells change only by learning, which happens through the sheet's own
	// refreshes (RefreshSheet).
	if (m_character) {
		BakeSkills();
		BakeEffects();
	}
	const Input* input = ctx.CurrentInput();
	// Whether the pointer is still ours to read: a child (the close box, a mode
	// button) or a popup in front may already have claimed it.
	const bool pointerFree = input && !ctx.IsMouseConsumed();
	const gfx::Rect px = Body();
	TrackInventoryHover(ctx, px, pointerFree);
	// Before any early return, so a pointer that has left the sheet (or gone
	// behind the close box) clears the bar rather than leaving it stale.
	UpdateStatus(ctx, pointerFree);
	if (!pointerFree) return;
	const float mx = input->MouseX(), my = input->MouseY();
	if (!Pixel().Contains(mx, my)) return;
	// The TITLE BAND - right of the portrait, above the tab icons, where the name
	// sits - acts on nothing, so it is left to the sheet's floating window
	// (ui-panels P3b), which claims it as its own background. (A Ctrl-drag
	// moves the window from anywhere on it.)
	if (mx >= Ax(px, kNameX) && my < Ay(px, kModeBtnY)) return;
	const bool clicked = m_character && input->WasMousePressed(MouseButton::Left);

	if (m_mode == Mode::Inventory)
		UpdateInventory(ctx, px, mx, my, clicked);

	ctx.ConsumeMouse(); // swallow other clicks over the sheet
}

void CharacterSheet::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	m_character = RosterMember(m_roster, m_member);
	const ui::Theme& theme = ctx.GetTheme();
	const gfx::Rect px = Body();

	if (m_card) {
		// A card sits on the party window's stone: a darker well, the member's
		// name over it, and a hairline in the member's colour under the name.
		const gfx::Rect& card = Pixel();
		batch.DrawRect(card, {0.0f, 0.0f, 0.0f, 0.22f});
		ui::DrawBorder(batch, card, theme.panelBorder);
		if (!m_character) return;
		const ui::Font& nameFont = ctx.FontAt(ui::FontRole::Display, Em(1.5f));
		const float band = Em(kCardNameEm);
		// On the Inventory tab the name lines up with the squares (em from the
		// card's corner); elsewhere with the tab's own left margin.
		const bool squares = m_mode == Mode::Inventory;
		const float left = squares ? card.x + Em(kCardInvPadEm) : Ax(px, kLeft);
		const float right = squares ? card.x + card.w - Em(kCardInvPadEm) : Ax(px, 1.0f - kLeft);
		nameFont.Draw(batch, m_character->name, left,
					  card.y + (band - nameFont.Height()) * 0.5f, theme.accent);
		const Vec4& c = m_character->portraitColor;
		batch.DrawRect({left, card.y + band - 2.0f, right - left, 1.0f}, {c.x, c.y, c.z, 0.7f});
		switch (m_mode) {
		case Mode::Inventory: DrawInventory(ctx, batch, px); break;
		case Mode::Stats:     DrawStats(ctx, batch, px); break;
		default:              break;
		}
		return;
	}

	// The whole card, status band included.
	ui::DrawPanelFace(ctx, batch, Pixel(), opacity ? *opacity : 1.0f);
	if (!m_character) return;
	DrawStatus(ctx, batch);

	// --- header band: the name (the portrait is a child) --------------------
	ctx.FontAt(ui::FontRole::Display, Em(kHeadingRem))
		.Draw(batch, m_character->name, Ax(px, kNameX), Ay(px, kNameY),
			  theme.accent);

	// The two bodies that aren't containers; the list tabs draw as children.
	switch (m_mode) {
	case Mode::Inventory: DrawInventory(ctx, batch, px); break;
	case Mode::Stats:     DrawStats(ctx, batch, px); break;
	default:              break;
	}
}

// The armor tooltip goes in the OVERLAY pass so no slot, icon or child widget
// can paint over it — it is drawn last by definition, which is what a tooltip
// has to be.
void CharacterSheet::DrawOverlaySelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	if (m_mode == Mode::Inventory) DrawArmorTip(ctx, batch, Body());
}

// --- SheetPortrait ---------------------------------------------------------

SheetPortrait::SheetPortrait(const gfx::Rect& rect,
							 const std::vector<Character>* roster,
							 const size_t* member)
	: m_roster(roster), m_member(member) {
	bounds = rect;
	debugName = "SheetPortrait";
}

void SheetPortrait::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	if (const Character* c = RosterMember(m_roster, *m_member))
		DrawPortrait(batch, Pixel(), *c,
					 ctx.FontAt(ui::FontRole::Display, Em(kHeadingRem)),
					 ctx.GetTheme());
}

// --- ModeButton / ModeSelector ---------------------------------------------

ModeButton::ModeButton(const gfx::Rect& rect, int index, const int* activeIndex,
					   std::function<void(int)> onSelect)
	: ui::Button(rect, std::string(),
				 [onSelect = std::move(onSelect), index] {
					 if (onSelect) onSelect(index);
				 }),
	  m_index(index), m_active(activeIndex) {
	debugName = "ModeButton";
	fireOnPress = true; // a tab changes the page on the press (Michael)
}

void ModeButton::UpdateSelf(ui::UIContext& ctx) {
	ui::Button::UpdateSelf(ctx);
	// The current tab is held down and lit - however it got there: a click, or
	// Tab / Shift+Tab stepping onto it.
	active = *m_active == m_index;
}

void ModeButton::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	if (const ui::Skin* skin = ctx.GetSkin(); etch && skin && skin->block.texture) {
		ui::Button::DrawSelf(ctx, batch); // the cut stone
		return;
	}
	// The flat look (no skin, or uiskin=0): the hand-drawn glyph.
	const ui::Theme& theme = ctx.GetTheme();
	const gfx::Rect& r = Pixel();
	const int i = m_index;
	batch.DrawRect(r, active ? theme.controlActive
							 : (Hot() ? theme.controlHot : theme.control));
	ui::DrawBorder(batch, r, active ? theme.accent : theme.panelBorder);
	const Vec4 ink = active ? theme.text : theme.textDim;
	const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
	if (i == 0) { // Inventory: 2x2 grid of squares
		const float sq = r.w * 0.17f, g = r.w * 0.09f;
		const float x0 = cx - sq - g * 0.5f, y0 = cy - sq - g * 0.5f;
		for (int gx = 0; gx < 2; ++gx)
			for (int gy = 0; gy < 2; ++gy)
				batch.DrawRect({x0 + gx * (sq + g), y0 + gy * (sq + g), sq, sq}, ink);
	} else if (i == 1) { // Stats: three ascending bars
		const float bw = r.w * 0.13f, g = r.w * 0.07f;
		const float x0 = cx - (3 * bw + 2 * g) * 0.5f, baseY = cy + r.h * 0.24f;
		const float h[3] = {r.h * 0.22f, r.h * 0.34f, r.h * 0.46f};
		for (int k = 0; k < 3; ++k)
			batch.DrawRect({x0 + k * (bw + g), baseY - h[k], bw, h[k]}, ink);
	} else if (i == 2) { // Skills: a six-point star (two overlaid triangles)
		const float rad = r.w * 0.26f, dx = rad * 0.866f, dy = rad * 0.5f;
		batch.DrawTriangle({cx, cy - rad}, {cx - dx, cy + dy}, {cx + dx, cy + dy}, ink);
		batch.DrawTriangle({cx, cy + rad}, {cx - dx, cy - dy}, {cx + dx, cy - dy}, ink);
	} else if (i == 3) { // Spells: a rune diamond/gem
		const float hw = r.w * 0.20f, hh = r.h * 0.28f;
		batch.DrawTriangle({cx, cy - hh}, {cx - hw, cy}, {cx + hw, cy}, ink);
		batch.DrawTriangle({cx, cy + hh}, {cx - hw, cy}, {cx + hw, cy}, ink);
	} else { // Effects: an hourglass
		const float hw = r.w * 0.22f, hh = r.h * 0.26f;
		batch.DrawTriangle({cx - hw, cy - hh}, {cx + hw, cy - hh}, {cx, cy}, ink);
		batch.DrawTriangle({cx, cy}, {cx - hw, cy + hh}, {cx + hw, cy + hh}, ink);
	}
}

ModeSelector::ModeSelector(const gfx::Rect& rect, int count,
						   const int* activeIndex,
						   std::function<void(int)> onSelect) {
	bounds = rect;
	debugName = "ModeSelector";
	// Even columns with the authored gap, as fractions of the strip.
	const float span = static_cast<float>(count);
	const float gap = sheet::kModeBtnGap / (span * sheet::kModeBtnW +
										   (span - 1.0f) * sheet::kModeBtnGap);
	const float w = (1.0f - gap * (span - 1.0f)) / span;
	for (int i = 0; i < count; ++i)
		m_buttons.push_back(
			Add<ModeButton>(gfx::Rect{(w + gap) * static_cast<float>(i), 0.0f, w, 1.0f}, i,
							activeIndex, onSelect));
}

void ModeSelector::SetEtches(std::span<const gfx::Texture* const> etch,
							 std::span<const gfx::Texture* const> lit) {
	for (size_t i = 0; i < m_buttons.size(); ++i) {
		m_buttons[i]->etch = i < etch.size() ? etch[i] : nullptr;
		m_buttons[i]->etchLit = i < lit.size() ? lit[i] : nullptr;
	}
}

} // namespace dungeon::game
