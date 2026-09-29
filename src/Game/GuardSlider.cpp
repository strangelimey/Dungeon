// ============================================================================
// Game/GuardSlider.cpp — see GuardSlider.h.
// ============================================================================
#include "Game/GuardSlider.h"

#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/UIContext.h"

#include <algorithm>
#include <cmath>

namespace dungeon::game {

namespace {
// A press on a full or over-exerted bar has to travel this far before it
// counts as a drag LEFT or RIGHT - a hand's tremor is not a decision.
constexpr float kDirectionRem = 0.2f;

// THE CHARGE. `effort` runs 0..1 and buys over-exertion 1 - (1 - effort)^2:
// quick at first, then HEAVIER - each further percent costs more effort the
// nearer it is to 100%, which is the "weight" behind the gesture. Effort comes
// from time held AND from rightward travel, so doing both is fastest.
// Slowed from 1.75 s / 1 width (Michael, 2026-09-28: full over-exertion came
// too quickly); the two stay in proportion so dragging speeds it up as before.
constexpr float kChargeSeconds = 2.5f; // held still: 0 -> 100% in this long
constexpr float kChargeWidths = 1.4f;  // or drag right this many bar widths
// A frame hitch must not dump a lump of effort in at once.
constexpr float kMaxChargeStep = 0.1f;

float OverFromEffort(float effort) {
	const float rest = 1.0f - std::clamp(effort, 0.0f, 1.0f);
	return 1.0f - rest * rest;
}
float EffortFromOver(float over) {
	return 1.0f - std::sqrt(1.0f - std::clamp(over, 0.0f, 1.0f));
}

Vec4 Mix(const Vec4& a, const Vec4& b, float t) {
	return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
			a.w + (b.w - a.w) * t};
}
// The angry palette: the committed bar's body darkens toward blood as the
// over-exertion climbs, and the over-exerted stretch over it starts DARK red
// and brightens to a full hot red at 100% (Michael, 2026-09-28: it read bright
// red from the first percent, so a little over-exertion looked like a lot).
constexpr Vec4 kAngryBody{0.45f, 0.08f, 0.06f, 1.0f};
constexpr Vec4 kOverDark{0.40f, 0.04f, 0.03f, 1.0f};
constexpr Vec4 kOverHot{1.00f, 0.10f, 0.06f, 1.0f};
} // namespace

GuardSlider::GuardSlider(const gfx::Rect& rect,
						 const std::vector<Character>* roster, size_t member,
						 std::function<void(size_t, float)> onChange,
						 std::function<float()> exertMax)
	: m_roster(roster), m_member(member), m_onChange(std::move(onChange)),
	  m_exertMax(std::move(exertMax)) {
	bounds = rect;
	debugName = "GuardSlider";
}

float GuardSlider::ExertMax() const { return m_exertMax ? m_exertMax() : 1.0f; }

float GuardSlider::OverOf(float share) const {
	const float max = ExertMax();
	if (share <= 1.0f || max <= 1.0f) return 0.0f;
	return std::clamp((share - 1.0f) / (max - 1.0f), 0.0f, 1.0f);
}

float GuardSlider::HonestAt(float x) const {
	const gfx::Rect& px = Pixel();
	if (px.w <= 0.0f) return 1.0f;
	return std::clamp((x - px.x) / px.w, 0.0f, 1.0f);
}

gfx::Rect GuardSlider::BarRect(float share) const {
	const gfx::Rect& px = Pixel();
	const float top = std::min(Rem(kGapRem), px.h);
	const float angry = std::min(Rem(kAngryRem), px.h - top);
	const float rest = std::min(Rem(kRestRem), angry);
	const float h = rest + (angry - rest) * OverOf(share);
	// CENTRED in the room reserved for the angry swell (Michael, 2026-09-28), so
	// a resting bar sits in the middle of it and an over-exerted one swells out
	// both ways rather than hanging from the top.
	return {px.x, px.y + top + (angry - h) * 0.5f, px.w, h};
}

void GuardSlider::Report(float share) const {
	if (m_onChange) m_onChange(m_member, share);
}

void GuardSlider::UpdateSelf(ui::UIContext& ctx) {
	const Character* c = RosterMember(m_roster, m_member);
	m_hot = false;
	if (!c) { // short roster, or the member went away mid-drag - inert
		m_drag = Drag::None;
		return;
	}
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const float mx = static_cast<float>(input->MouseX());
	const float share = c->offenseShare;

	// The GRAB ZONE is the whole band - the gap under the hands, the room the
	// swell grows into and the slack below - not just the bar, which at rest is
	// a sliver too thin to start a drag on. DrawSelf lights the band while the
	// pointer is over it, so every pixel claimed here is one painted there.
	const bool hot = !ctx.IsMouseConsumed() &&
					 Pixel().Contains(mx, static_cast<float>(input->MouseY()));
	m_hot = hot;
	if (hot) {
		ctx.ConsumeMouse();
		if (input->WasMousePressed(MouseButton::Left)) {
			m_pressX = m_lastX = mx;
			// Below 100% a press places the stance at once, as a slider does. On a
			// full or over-exerted bar it waits to see which way it goes, because
			// the two directions mean different things there.
			if (share < 1.0f) {
				m_drag = Drag::Honest;
				Report(HonestAt(mx));
			} else {
				m_drag = Drag::Pending;
			}
		}
	}
	if (m_drag == Drag::None) return;

	// The drag continues OUTSIDE the widget once begun - a slider you lose the
	// moment the cursor strays off a band this thin would be unusable.
	if (input->WasMouseReleased(MouseButton::Left)) {
		// A plain click on a full bar still places the stance where it landed.
		// On an over-exerted bar it does nothing: leaving over-exertion takes a
		// deliberate drag left, not a stray click.
		if (m_drag == Drag::Pending && share <= 1.0f) Report(HonestAt(mx));
		m_drag = Drag::None;
		return;
	}

	switch (m_drag) {
	case Drag::Pending: {
		const float dx = mx - m_pressX;
		if (dx >= Rem(kDirectionRem) && ExertMax() > 1.0f) {
			// Right from a full (or already over-exerted) bar: the charge, carrying
			// on from wherever the over-exertion already stands.
			m_drag = Drag::Charge;
			m_effort = EffortFromOver(OverOf(share));
			m_lastTick = std::chrono::steady_clock::now();
			m_lastX = mx;
		} else if (dx <= -Rem(kDirectionRem)) {
			if (share > 1.0f) {
				// Left from over-exertion: back to 100% attack / 0% over, and this
				// press is spent - defense takes another drag.
				Report(1.0f);
				m_drag = Drag::Held;
			} else {
				m_drag = Drag::Honest;
				Report(HonestAt(mx));
			}
		}
		break;
	}
	case Drag::Honest:
		// HonestAt never passes 1.0: the detent. Pressing on past it does
		// nothing; over-exertion is a separate press.
		Report(HonestAt(mx));
		break;
	case Drag::Charge: {
		const auto now = std::chrono::steady_clock::now();
		const float dt = std::min(
			kMaxChargeStep, std::chrono::duration<float>(now - m_lastTick).count());
		m_lastTick = now;
		// Only travel RIGHT pushes. Drifting left mid-charge neither helps nor
		// cancels; backing out is a new drag left.
		const float moved = std::max(0.0f, mx - m_lastX);
		m_lastX = mx;
		const float width = std::max(1.0f, Pixel().w * kChargeWidths);
		m_effort = std::min(1.0f, m_effort + dt / kChargeSeconds + moved / width);
		Report(1.0f + OverFromEffort(m_effort) * (ExertMax() - 1.0f));
		break;
	}
	case Drag::Held:
	case Drag::None:
		break;
	}
}

void GuardSlider::DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const Character* c = RosterMember(m_roster, m_member);
	if (!c) return; // roster shorter than this slot — draw nothing
	const ui::Theme& theme = ctx.GetTheme();
	const float share = c->offenseShare;
	const float over = OverOf(share);
	const gfx::Rect bar = BarRect(share);

	// The grab zone, lit while it is hot or held, so the band the pointer can
	// start a drag on is visible rather than guessed at.
	if (m_hot || m_drag != Drag::None) {
		const Vec4& a = theme.accent;
		batch.DrawRect(Pixel(), {a.x, a.y, a.z, 0.12f});
	}

	batch.DrawRect(bar, theme.control);

	if (over <= 0.0f) {
		// THE STANCE: the offense portion fills from the left, and what is left
		// of the track is what the character guards with, so the split is
		// legible without a number - the bar IS the stance.
		const float fill = bar.w * std::clamp(share, 0.0f, 1.0f);
		if (fill > 0.0f)
			batch.DrawRect({bar.x, bar.y, fill, bar.h},
						   m_drag == Drag::Honest ? theme.controlActive : theme.accent);
	} else {
		// OVER-EXERTED: every point is already in the swing, so the whole bar is
		// the committed body, darkening toward blood; the over-exertion itself
		// burns across it from the left, hotter as it climbs. Swelling (BarRect)
		// and reddening together are the "angrier" look.
		// Mostly blood from the first percent, so a little over-exertion reads as
		// DARK red rather than orange; the stretch below brings the brightness.
		batch.DrawRect(bar, Mix(theme.accent, kAngryBody, 0.85f + 0.15f * over));
		batch.DrawRect({bar.x, bar.y, bar.w * over, bar.h},
					   Mix(kOverDark, kOverHot, over));
	}

	// A hairline under the bar: 1px, the one place raw pixels are allowed
	// (UI/Units.h), because a fractional hairline blurs or vanishes.
	batch.DrawRect({bar.x, bar.y + bar.h - 1.0f, bar.w, 1.0f}, theme.panelBorder);
}

} // namespace dungeon::game
