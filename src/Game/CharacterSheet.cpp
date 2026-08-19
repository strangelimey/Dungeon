// ============================================================================
// Game/CharacterSheet.cpp — shell: ctor, SetCharacter, Update/Draw, mode strip.
// Tab bodies live in CharacterSheet_Inventory / _Stats / _Lists.
// ============================================================================
#include "Game/CharacterSheet.h"
#include "Game/CharacterSheetLayout.h"
#include "Game/PartyHudDraw.h"

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
							   const ResourceBarColors* barColors,
							   const ItemIconBank* icons,
							   const ItemWeightBank* weights,
							   const ItemIconBank* slotIcons,
							   const ItemCategoryBank* categories,
							   std::optional<std::string>* held)
	: m_roster(roster), m_barColors(barColors),
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
}

// The sheet's children. The two non-scrolling bodies (Inventory, Stats) stay
// drawn by the sheet itself against its own rect — they fill it, so "fractions
// of the sheet" is already parent-relative and a container would buy nothing.
// The three LIST tabs each get a SheetList, which is where the shared scroll
// lives (see the header).
void CharacterSheet::BuildParts() {
	Add<SheetPortrait>(gfx::Rect{kPortraitX, kPortraitY, kPortraitW, kPortraitH},
					   m_roster, &m_member);

	const float stripW = kModeCount * kModeBtnW + (kModeCount - 1) * kModeBtnGap;
	Add<ModeSelector>(gfx::Rect{kModeBtnX, kModeBtnY, stripW, kModeBtnH},
					  kModeCount, &m_modeIndex, [this](int i) {
						  const Mode next = static_cast<Mode>(i);
						  if (next != m_mode)
							  for (SheetList* list : m_lists)
								  if (list) list->ScrollToTop();
						  m_mode = next;
						  m_modeIndex = i;
					  });

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
	const Input* input = ctx.CurrentInput();
	if (!input || ctx.IsMouseConsumed()) return;
	const gfx::Rect& px = Pixel();
	const float mx = input->MouseX(), my = input->MouseY();
	if (!px.Contains(mx, my)) return;
	const bool clicked = m_character && input->WasMousePressed(MouseButton::Left);

	if (m_mode == Mode::Inventory)
		UpdateInventory(ctx, px, mx, my, clicked);

	ctx.ConsumeMouse(); // swallow other clicks over the sheet
}

void CharacterSheet::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	m_character = RosterMember(m_roster, m_member);
	const ui::Theme& theme = ctx.GetTheme();
	const gfx::Rect& px = Pixel();

	ui::DrawPanelFace(ctx, batch, px);
	if (!m_character) return;

	// --- header band: the name (the portrait is a child) --------------------
	ctx.FontAt(ui::FontRole::Display, Rem(kHeadingRem))
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
	if (m_mode == Mode::Inventory) DrawArmorTip(ctx, batch, Pixel());
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
					 ctx.FontAt(ui::FontRole::Display, Rem(kHeadingRem)),
					 ctx.GetTheme());
}

// --- ModeButton / ModeSelector ---------------------------------------------

ModeButton::ModeButton(const gfx::Rect& rect, int index, const int* activeIndex,
					   std::function<void(int)> onSelect)
	: m_index(index), m_active(activeIndex), m_onSelect(std::move(onSelect)) {
	bounds = rect;
	debugName = "ModeButton";
}

void ModeButton::UpdateSelf(ui::UIContext& ctx) {
	m_hot = false;
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	m_hot = !ctx.IsMouseConsumed() &&
			Pixel().Contains(input->MouseX(), input->MouseY());
	if (!m_hot) return;
	ctx.ConsumeMouse();
	if (input->WasMousePressed(MouseButton::Left) && m_onSelect) m_onSelect(m_index);
}

void ModeButton::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const ui::Theme& theme = ctx.GetTheme();
	const gfx::Rect& r = Pixel();
	const int i = m_index;
	const bool active = *m_active == i;
	batch.DrawRect(r, active ? theme.controlActive
							 : (m_hot ? theme.controlHot : theme.control));
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
		Add<ModeButton>(gfx::Rect{(w + gap) * static_cast<float>(i), 0.0f, w, 1.0f},
						i, activeIndex, onSelect);
}

} // namespace dungeon::game
