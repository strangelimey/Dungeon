// ============================================================================
// Game/CharacterSheet_Stats.cpp — attributes + resource bars tab.
// ============================================================================
#include "Game/CharacterSheet.h"
#include "Game/CharacterSheetLayout.h"
#include "Game/PartyHudDraw.h"

#include "Core/Loc.h"

#include <algorithm>
#include <format>

namespace dungeon::game {
using namespace sheet;

void CharacterSheet::BakeStats() {
	if (!m_character) return;
	const Character& character = *m_character;
	m_healthText = std::format("{} / {}", static_cast<int>(character.health),
							   static_cast<int>(character.maxHealth));
	m_staminaText = std::format("{} / {}", static_cast<int>(character.stamina),
								static_cast<int>(character.maxStamina));
	m_manaText = std::format("{} / {}", static_cast<int>(character.mana),
							 static_cast<int>(character.maxMana));
	m_attrValues = {std::to_string(character.strength),
					std::to_string(character.dexterity),
					std::to_string(character.vitality),
					std::to_string(character.willpower),
					std::to_string(character.intelligence)};
}

void CharacterSheet::DrawStats(ui::UIContext& ctx, gfx::SpriteBatch& batch,
							   const gfx::Rect& px) {
	const ui::Theme& theme = ctx.GetTheme();
	ui::Font& font = ctx.GetFont();

	// --- attributes (left column) -------------------------------------------
	font.Draw(batch, m_attributesLabel, Ax(px, kLeft), Ay(px, kHeaderY), theme.accent);
	for (size_t i = 0; i < m_attrLabels.size(); ++i) {
		const float y = kFirstRowY + static_cast<float>(i) * kRowH;
		font.Draw(batch, m_attrLabels[i], Ax(px, kLabelX), Ay(px, y), theme.textDim);
		const float vw = font.MeasureWidth(m_attrValues[i]);
		font.Draw(batch, m_attrValues[i], Ax(px, kValueRight) - vw, Ay(px, y),
				  theme.text);
	}

	// --- health / stamina / mana bars (right column) ------------------------
	const struct {
		const std::string& label;
		float value, max;
		const Vec4& color;
		const std::string& text;
	} bars[] = {
		{m_healthLabel, m_character->health, m_character->maxHealth,
		 m_barColors->health, m_healthText},
		{m_staminaLabel, m_character->stamina, m_character->maxStamina,
		 m_barColors->stamina, m_staminaText},
		{m_manaLabel, m_character->mana, m_character->maxMana, m_barColors->mana,
		 m_manaText},
	};
	for (size_t i = 0; i < std::size(bars); ++i) {
		const auto& b = bars[i];
		const float by = kFirstRowY + static_cast<float>(i) * kRowH;
		const gfx::Rect bar = At(px, kBarX, by, kBarW, kBarH);
		font.Draw(batch, b.label, Ax(px, kBarLabelX),
				  bar.y + (bar.h - font.Height()) * 0.5f, theme.textDim);
		DrawStatBar(batch, bar, b.value / std::max(b.max, 1.0f), b.color, theme);
		const float tw = font.MeasureWidth(b.text);
		font.Draw(batch, b.text, bar.x + (bar.w - tw) * 0.5f,
				  bar.y + (bar.h - font.Height()) * 0.5f, theme.text);
	}
}

} // namespace dungeon::game
