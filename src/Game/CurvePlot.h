// ============================================================================
// Game/CurvePlot.h — the contribution curves, drawn.
//
// Michael's reason for choosing a smooth curve over Rolemaster's banded one:
// "the best way would be to express the contribution as a curve that can be
// displayed so it is easier to see the contributions over scale". A banded
// table can be read; a smooth curve has to be SEEN, and a knob you cannot see
// the effect of is a knob you cannot tune.
//
// So this draws the live Balance's two curves — skill level -> roll bonus and
// stat value -> roll bonus — and redraws as the knobs above it move.
//
// THE REFERENCE LINE IS THE POINT. Two opposed d100s deviate by ~41 points
// (measured, tools/RollTest), so a bonus difference much under that is drowned
// by the dice. Drawing it as a horizontal rule turns "is this knob doing
// anything?" from an argument into a glance: a curve that stays under the line
// across its whole useful range is decoration, however impressive its numbers
// look in a text field.
// ============================================================================
#pragma once

#include "Game/Balance.h"
#include "UI/Widget.h"

namespace dungeon::game {

class CurvePlot : public ui::Widget {
public:
	// Borrows the dialog's working copy — the same object its numeric fields
	// edit, so the plot is live by construction rather than by being told.
	// (Leading rect per the control convention: a Stack row supplies it.)
	CurvePlot(const gfx::Rect& rect, const Balance* balance)
		: m_balance(balance) {
		bounds = rect;
	}

	// The widest skill level and stat value plotted. Skill is trained by use
	// and never stops, so the axis shows a generous span rather than a limit.
	float maxSkill = 60.0f;
	float maxStat = 30.0f;

private:
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	const Balance* m_balance;
};

} // namespace dungeon::game
