// ============================================================================
// Game/GuardSlider.h — the offense/defense stance, under a member's hands.
//
// One slider PER CHARACTER, spanning the full width of both hand boxes: how
// much of their skill goes into attacking, and how much is held back to guard
// with (docs/damage-system.md). Full right is all-out — every point in the
// swing, nothing kept back.
//
// Deliberately NOT ui::Slider: that control carries a label line above its
// track and lays out by its own box, which is right on a settings page and far
// too tall for the HUD, where this has to fit in the sliver under two hand
// boxes. What it keeps from the house style is everything that matters — rem
// units, the theme colours, and claiming the pointer only where it paints.
//
// It holds no Character pointer across frames (PartyHudTypes RosterMember, the
// roster-resize rule) and mutates nothing itself: dragging fires a callback the
// owner routes, like every other HUD action.
// ============================================================================
#pragma once

#include "Game/PartyHudTypes.h"
#include "UI/Widget.h"

#include <functional>

namespace dungeon::game {

class GuardSlider : public ui::Widget {
public:
	GuardSlider(const gfx::Rect& rect, const std::vector<Character>* roster,
				size_t member, std::function<void(size_t, float)> onChange);

	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	// Where the live share sits along the track, and the inverse pick.
	float ShareAt(float x) const;

	const std::vector<Character>* m_roster;
	size_t m_member;
	std::function<void(size_t, float)> m_onChange;
	bool m_dragging = false;
};

} // namespace dungeon::game
