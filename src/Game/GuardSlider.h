// ============================================================================
// Game/GuardSlider.h — the offense/defense stance, under a member's hands.
//
// One slider PER CHARACTER, spanning the full width of both hand boxes: how
// much of their skill goes into attacking, and how much is held back to guard
// with (docs/damage-system.md). The WHOLE bar is the honest range: full right
// is 100% attack, 0% defense - every point in the swing, nothing kept back.
//
// OVER-EXERTION (a share past 1.0, bought with stamina and then health) is not
// a place on the track but a separate, deliberate gesture (Michael,
// 2026-09-28), because it is the one setting that can drop the character:
//   - 100% is a DETENT. A drag stops there and cannot cross it either way;
//     crossing always takes a NEW press.
//   - From a full bar, a new press that moves RIGHT starts a CHARGE. It climbs
//     while held AND while the pointer moves right (both together is fastest),
//     and it gets HEAVIER as it climbs: about 1.75 s held still takes it from
//     0 to 100% over-exertion. Releasing keeps what it reached; a later
//     right-drag charges on from there.
//   - From an over-exerted bar, ANY press that moves LEFT snaps back to 100%
//     attack / 0% over-exertion, and that press is spent. A further left drag
//     is the ordinary stance again.
//   - An over-exerted bar is ANGRY: red, and swelling toward kAngryRem thick as
//     it nears 100%. The swell is inside this widget's own bounds - the band
//     HandPair reserves includes the angry size, and the bar sits CENTRED in
//     that room, swelling out both ways - so it never paints over the hands
//     above it.
//   - The GRAB ZONE is the whole band, not the bar (Michael, 2026-09-28: the
//     resting bar alone was too fiddly to start a drag on). The band is the gap
//     under the hands, the room the swell grows into, and a little slack below,
//     and while the pointer is over it the whole band lights faintly, so the
//     pixels it claims are pixels it paints.
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

#include <chrono>
#include <functional>

namespace dungeon::game {

class GuardSlider : public ui::Widget {
public:
	// The band, top to bottom, in rem: the gap under the hand boxes, the bar
	// (kRestRem thick at rest, swelling toward kAngryRem), and slack below.
	// HandPair reserves kBandRem for the widget; all of it is the grab zone.
	static constexpr float kGapRem = 0.25f;
	static constexpr float kRestRem = 0.25f;
	static constexpr float kAngryRem = 0.6f;
	static constexpr float kSlackRem = 0.3f;
	static constexpr float kBandRem = kGapRem + kAngryRem + kSlackRem;

	// `exertMax` is the live Balance::exertMax (the share a full over-exertion
	// means); asked every frame, because it is editable in the Balance dialog
	// and a captured copy would go stale.
	GuardSlider(const gfx::Rect& rect, const std::vector<Character>* roster,
				size_t member, std::function<void(size_t, float)> onChange,
				std::function<float()> exertMax);

	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	// What a press is doing. PENDING is a press on a full or over-exerted bar
	// that has not yet moved far enough to say which way it is going; HELD is
	// a press that snapped back to 100% and is spent until release.
	enum class Drag { None, Pending, Honest, Charge, Held };

	float ExertMax() const;
	// How far past 1.0 the share is, as 0..1 of the way to ExertMax().
	float OverOf(float share) const;
	// The honest share under a pointer x: 0..1 across the bar, never past it.
	float HonestAt(float x) const;
	// The rect the bar PAINTS for this share (the resting strip, or the angry
	// swell), centred in the kAngryRem room below the kGapRem gap.
	gfx::Rect BarRect(float share) const;
	void Report(float share) const;

	const std::vector<Character>* m_roster;
	size_t m_member;
	std::function<void(size_t, float)> m_onChange;
	std::function<float()> m_exertMax;
	Drag m_drag = Drag::None;
	bool m_hot = false; // the pointer is over the grab zone (lights the band)
	float m_pressX = 0.0f;
	float m_lastX = 0.0f;
	float m_effort = 0.0f; // the charge's progress; over-exertion = f(effort)
	std::chrono::steady_clock::time_point m_lastTick{};
};

} // namespace dungeon::game
