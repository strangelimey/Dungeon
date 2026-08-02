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

CharacterSheet::CharacterSheet(const gfx::Rect& rect,
							   std::vector<Character>* roster,
							   const ui::Font* portraitFont,
							   const ResourceBarColors* barColors,
							   const ItemIconBank* icons,
							   const ItemWeightBank* weights,
							   const ItemIconBank* slotIcons,
							   const ItemCategoryBank* categories,
							   std::optional<std::string>* held)
	: m_roster(roster), m_portraitFont(portraitFont), m_barColors(barColors),
	  m_icons(icons), m_weights(weights), m_slotIcons(slotIcons),
	  m_categories(categories), m_held(held),
	  m_healthLabel(loc::Tr("bar.health")),
	  m_staminaLabel(loc::Tr("bar.stamina")), m_manaLabel(loc::Tr("bar.mana")),
	  m_attributesLabel(loc::Tr("sheet.attributes")),
	  m_skillsLabel(loc::Tr("sheet.skills")), m_noSkills(loc::Tr("sheet.no_skills")),
	  m_effectsLabel(loc::Tr("sheet.effects")),
	  m_noEffects(loc::Tr("sheet.no_effects")),
	  m_spellsLabel(loc::Tr("sheet.spells")),
	  m_noSpells(loc::Tr("sheet.no_spells")) {
	bounds = rect;
	m_attrLabels = {loc::Tr("attr.strength"), loc::Tr("attr.dexterity"),
					loc::Tr("attr.vitality"), loc::Tr("attr.willpower"),
					loc::Tr("attr.intelligence")};
}

void CharacterSheet::SetCharacter(size_t member) {
	m_member = member;
	m_character = RosterMember(m_roster, m_member);
	m_scroll = 0.0f; // a fresh member starts its list tabs at the top
	m_scrollDragging = false;
	if (!m_character) return; // out of range — the sheet body just stays empty
	BakeStats();
	BakeSkills();
	BakeSpells();
	BakeEffects();
}

gfx::Rect CharacterSheet::ModeButtonRect(const gfx::Rect& px, int i) const {
	const float x = kModeBtnX + static_cast<float>(i) * (kModeBtnW + kModeBtnGap);
	return At(px, x, kModeBtnY, kModeBtnW, kModeBtnH);
}

void CharacterSheet::Update(ui::UIContext& ctx) {
	m_character = RosterMember(m_roster, m_member);
	m_hotMode = -1;
	const Input* input = ctx.CurrentInput();
	if (!input || ctx.IsMouseConsumed()) return; // sheet buttons update first
	const gfx::Rect& px = Pixel();
	const float mx = input->MouseX(), my = input->MouseY();
	if (!px.Contains(mx, my)) return;
	const bool clicked = m_character && input->WasMousePressed(MouseButton::Left);

	// Mode toggle buttons (always present, every mode).
	for (int i = 0; i < kModeCount; ++i)
		if (ModeButtonRect(px, i).Contains(mx, my)) {
			m_hotMode = i;
			if (clicked) {
				const Mode next = static_cast<Mode>(i);
				if (next != m_mode) { // a fresh tab starts at the top
					m_scroll = 0.0f;
					m_scrollDragging = false;
				}
				m_mode = next;
				ctx.ConsumeMouse();
			}
			break;
		}

	// The list tabs (Skills / Spells / Effects) scroll — wheel + thumb drag.
	if (m_mode == Mode::Skills || m_mode == Mode::Spells || m_mode == Mode::Effects)
		UpdateScroll(ctx, px);

	if (m_mode == Mode::Inventory)
		UpdateInventory(ctx, px, mx, my, clicked);

	ctx.ConsumeMouse(); // swallow other clicks over the sheet
}

void CharacterSheet::Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	m_character = RosterMember(m_roster, m_member);
	const ui::Theme& theme = ctx.GetTheme();
	const gfx::Rect& px = Pixel();

	ui::DrawPanelFace(ctx, batch, px);
	if (!m_character) return;

	// --- header band (portrait, name) — the bars live on the Stats page ------
	DrawPortrait(batch, At(px, kPortraitX, kPortraitY, kPortraitW, kPortraitH),
				 *m_character, *m_portraitFont, theme);
	m_portraitFont->Draw(batch, m_character->name, Ax(px, kNameX), Ay(px, kNameY),
						 theme.accent);

	// --- mode toggle + the active mode's body -------------------------------
	DrawModeButtons(ctx, batch, px);
	switch (m_mode) {
	case Mode::Inventory: DrawInventory(ctx, batch, px); break;
	case Mode::Stats:     DrawStats(ctx, batch, px); break;
	case Mode::Skills:    DrawSkills(ctx, batch, px); break;
	case Mode::Spells:    DrawSpells(ctx, batch, px); break;
	case Mode::Effects:   DrawEffects(ctx, batch, px); break;
	}
}

void CharacterSheet::DrawModeButtons(ui::UIContext& ctx, gfx::SpriteBatch& batch,
									 const gfx::Rect& px) {
	const ui::Theme& theme = ctx.GetTheme();
	for (int i = 0; i < kModeCount; ++i) {
		const gfx::Rect r = ModeButtonRect(px, i);
		const bool active = static_cast<int>(m_mode) == i;
		batch.DrawRect(r, active ? theme.controlActive
								 : (m_hotMode == i ? theme.controlHot : theme.control));
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
}

} // namespace dungeon::game
