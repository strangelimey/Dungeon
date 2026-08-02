// ============================================================================
// Game/PartyBar.h — the top party bar: the row of CharacterPanel slots.
//
// The bar owns the slots, so the scale slider moves ONE rect (this widget's
// bounds) and every panel — and everything inside a panel — follows. The bar
// always reserves `kSlots` columns, so a party of one keeps a full-size slot
// rather than stretching across the window; a roster shorter than that simply
// has fewer children and the spare columns stay empty.
// ============================================================================
#pragma once

#include "UI/Widget.h"

namespace dungeon::game {

class PartyBar : public ui::Widget {
public:
	// Columns the bar always reserves, whatever the roster size.
	static constexpr size_t kSlots = 4;

	explicit PartyBar(const gfx::Rect& rect) {
		bounds = rect;
		debugName = "PartyBar";
	}

	// Gap between slots, as a fraction of the bar's width.
	float gap = 0.006f;

private:
	// Splits the bar into kSlots even columns and gives each child its own.
	void LayoutSelf(ui::UIContext& ctx) override;
};

} // namespace dungeon::game
