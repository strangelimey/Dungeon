// ============================================================================
// Game/CharacterSheet_Stats.cpp — attributes + resource bars tab.
// ============================================================================
#include "Game/CharacterSheet.h"
#include "Game/CharacterSheetLayout.h"
#include "Game/PartyHudDraw.h"

#include "Core/Loc.h"

#include <algorithm>
#include <format>
#include <span>

namespace dungeon::game {
using namespace sheet;

namespace {

// "value / max" into `buf`, truncating the way the sheet always has.
std::string_view FormatPool(std::span<char> buf, float value, float max) {
	const auto end = std::format_to_n(buf.data(), static_cast<std::ptrdiff_t>(buf.size()),
									  "{} / {}", static_cast<int>(value),
									  static_cast<int>(max))
						 .out;
	return {buf.data(), static_cast<size_t>(end - buf.data())};
}

} // namespace

// Nothing to bake any more: the numbers are formatted as they draw (see the
// header). Kept so SetCharacter's list of bakes still names every tab.
void CharacterSheet::BakeStats() {}

// The rows start a line BELOW the heading, measured, rather than at an
// authored fraction: the heading grew with kStatRem and the old fixed start
// left the two almost touching. Deriving it means the gap survives any
// future retune of the scale — and both columns share it, so the bars stay
// on the same baselines as the attributes beside them. One function because
// the status bar hit-tests the same rows the draw lays out.
CharacterSheet::StatRows CharacterSheet::StatRowsFor(ui::UIContext& ctx,
													 const gfx::Rect& px) const {
	const ui::Font& font = ctx.FontAt(ui::FontRole::Body, Rem(kStatRem));
	return {Ay(px, kHeaderY) + font.LineAdvance() + Rem(0.4f), kStatRowH * px.h};
}

void CharacterSheet::DrawStats(ui::UIContext& ctx, gfx::SpriteBatch& batch,
							   const gfx::Rect& px) {
	const ui::Theme& theme = ctx.GetTheme();
	// Enlarged (kStatRem) with the row pitch and bar height to match, so the
	// values still sit inside the bars they label.
	const ui::Font& font = ctx.FontAt(ui::FontRole::Body, Rem(kStatRem));

	// --- attributes (left column) -------------------------------------------
	font.Draw(batch, m_attributesLabel, Ax(px, kLeft), Ay(px, kHeaderY), theme.accent);

	const StatRows rows = StatRowsFor(ctx, px);
	const float rowTop = rows.top;
	const float rowStep = rows.step;

	const int attrs[] = {m_character->strength, m_character->dexterity,
						 m_character->vitality, m_character->willpower,
						 m_character->intelligence};
	for (size_t i = 0; i < m_attrLabels.size(); ++i) {
		const float y = rowTop + static_cast<float>(i) * rowStep;
		font.Draw(batch, m_attrLabels[i], Ax(px, kLabelX), y, theme.textDim);
		char buf[16];
		const auto end = std::format_to_n(buf, sizeof(buf), "{}", attrs[i]).out;
		const std::string_view value(buf, static_cast<size_t>(end - buf));
		const float vw = font.MeasureWidth(value);
		font.Draw(batch, value, Ax(px, kValueRight) - vw, y, theme.text);
	}

	// --- health / stamina / mana bars (right column) ------------------------
	const struct {
		const std::string& label;
		float value, max;
		ResourceBar which;
	} bars[] = {
		{m_healthLabel, m_character->health, m_character->maxHealth, ResourceBar::Health},
		{m_staminaLabel, m_character->stamina, m_character->maxStamina,
		 ResourceBar::Stamina},
		{m_manaLabel, m_character->mana, m_character->maxMana, ResourceBar::Mana},
		// The two SUPPLIES, below the three pools they pay for
		// (docs/health-and-healing.md). Five bars against the five attributes in
		// the left column, which is how the two halves of the tab now line up.
		{m_foodLabel, m_character->food, m_character->maxFood, ResourceBar::Food},
		{m_waterLabel, m_character->water, m_character->maxWater, ResourceBar::Water},
	};
	// The bar's box is the GLASS less the end caps' reach, so the iron frame
	// stays inside the column the layout gave it across, and sticks out only up
	// and down. Three passes - fills, frames, then the text - so no frame
	// covers its neighbour's fill and no frame covers a number.
	const bool framed = m_barStyle->framed && m_barStyle->frame;
	auto tubeOf = [&](size_t i) {
		gfx::Rect bar{Ax(px, kBarX), rowTop + static_cast<float>(i) * rowStep,
					  kBarW * px.w, kStatBarH * px.h};
		if (framed) {
			const BarFrameReach reach = FrameReach(bar.h);
			bar.x += reach.left;
			bar.w = std::max(bar.w - reach.left - reach.right, 0.0f);
		}
		return bar;
	};
	for (size_t i = 0; i < std::size(bars); ++i)
		DrawResourceBarFill(batch, tubeOf(i), bars[i].which,
							bars[i].value / std::max(bars[i].max, 1.0f), m_member,
							*m_barStyle, theme);
	for (size_t i = 0; i < std::size(bars); ++i)
		DrawResourceBarFrame(batch, tubeOf(i), *m_barStyle);
	for (size_t i = 0; i < std::size(bars); ++i) {
		const auto& b = bars[i];
		const gfx::Rect bar = tubeOf(i);
		const float textY = bar.y + (bar.h - font.Height()) * 0.5f;
		font.Draw(batch, b.label, Ax(px, kBarLabelX), textY, theme.textDim);
		char buf[32];
		const std::string_view text = FormatPool(buf, b.value, b.max);
		const float tw = font.MeasureWidth(text);
		const float tx = bar.x + (bar.w - tw) * 0.5f;
		// A shadow under the number, so it reads over a bright, moving fill.
		if (framed) font.Draw(batch, text, tx + 1.0f, textY + 1.0f, {0, 0, 0, 0.85f});
		font.Draw(batch, text, tx, textY, theme.text);
	}
}

} // namespace dungeon::game
