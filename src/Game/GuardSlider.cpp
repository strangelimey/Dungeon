// ============================================================================
// Game/GuardSlider.cpp — see GuardSlider.h.
// ============================================================================
#include "Game/GuardSlider.h"

#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/UIContext.h"

#include <algorithm>

namespace dungeon::game {

namespace {
// The visible span of the track. Over-exertion pushes the share past 1, so the
// bar is drawn against this ceiling rather than against 1.0 — otherwise the
// one state worth SEEING (spent past everything you have) would look identical
// to an ordinary all-out swing.
//
// This is the DRAG's travel, not the rule. Balance::exertMax is the authority on
// how far a stance may go and Game's onGuardChange clamps to it; the two are
// deliberately not plumbed together, because the widget is built once while the
// knob is editable live in the Balance dialog, so a captured copy would go stale
// the moment it was tuned. Setting exert_max below this only makes the last
// stretch of track unreachable — self-correcting, since the fill is drawn from
// the CLAMPED share the character actually holds.
constexpr float kTrackMax = 2.0f;

// Where an honest full commitment sits on that track: everything right of this
// is borrowed against stamina and hide.
constexpr float kFullCommit = 1.0f;
} // namespace

GuardSlider::GuardSlider(const gfx::Rect& rect,
						 const std::vector<Character>* roster, size_t member,
						 std::function<void(size_t, float)> onChange)
	: m_roster(roster), m_member(member), m_onChange(std::move(onChange)) {
	bounds = rect;
	debugName = "GuardSlider";
}

float GuardSlider::ShareAt(float x) const {
	const gfx::Rect& px = Pixel();
	if (px.w <= 0.0f) return kTrackMax;
	return std::clamp((x - px.x) / px.w, 0.0f, kTrackMax);
}

void GuardSlider::UpdateSelf(ui::UIContext& ctx) {
	if (!RosterMember(m_roster, m_member)) return; // short roster — inert
	const Input* input = ctx.CurrentInput();
	if (!input) return;

	const bool hot = !ctx.IsMouseConsumed() &&
					 Pixel().Contains(input->MouseX(), input->MouseY());
	if (hot) {
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left)) m_dragging = true;
	}
	// The drag continues OUTSIDE the widget once begun — a slider you lose the
	// moment the cursor strays off a band this thin would be unusable.
	if (m_dragging) {
		if (m_onChange)
			m_onChange(m_member, ShareAt(static_cast<float>(input->MouseX())));
		if (input->WasMouseReleased(MouseButton::Left)) m_dragging = false;
	}
}

void GuardSlider::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const Character* c = RosterMember(m_roster, m_member);
	if (!c) return; // roster shorter than this slot — draw nothing
	const ui::Theme& theme = ctx.GetTheme();
	const gfx::Rect& px = Pixel();

	batch.DrawRect(px, theme.control);

	// The OFFENSE portion fills from the left; what is left of the track up to
	// the full-commit mark is what the character is guarding with, so the split
	// is legible without a number — the bar IS the stance.
	const float share = std::clamp(c->offenseShare, 0.0f, kTrackMax);
	const float honest = std::min(share, kFullCommit);
	const float fill = px.w * (honest / kTrackMax);
	if (fill > 0.0f)
		batch.DrawRect({px.x, px.y, fill, px.h},
					   m_dragging ? theme.controlActive : theme.accent);

	// OVER-EXERTION is the stretch past the mark, and it is drawn in its own
	// alarming colour rather than as more of the same fill. It is a different
	// KIND of spending — everything left of the mark comes out of skill you
	// have, everything right of it out of stamina and then hide — so it must not
	// read as simply more attack.
	if (share > kFullCommit) {
		const float over = px.w * ((share - kFullCommit) / kTrackMax);
		batch.DrawRect({px.x + fill, px.y, over, px.h}, {0.85f, 0.25f, 0.20f, 0.9f});
	}

	// The full-commit mark: a hairline at 1.0, so the point where the cost
	// changes character is visible BEFORE you drag past it rather than only
	// after. 1px — the one place raw pixels are allowed (UI/Units.h).
	batch.DrawRect({px.x + px.w * (kFullCommit / kTrackMax), px.y, 1.0f, px.h},
				   theme.panelBorder);

	// A hairline under the track: 1px, the one place raw pixels are allowed
	// (UI/Units.h), because a fractional hairline blurs or vanishes.
	batch.DrawRect({px.x, px.y + px.h - 1.0f, px.w, 1.0f}, theme.panelBorder);
}

} // namespace dungeon::game
