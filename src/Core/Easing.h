// ============================================================================
// Core/Easing.h — generic interpolation easing curves.
//
// An easing remaps a normalized progress t in [0,1] onto a shaped value used
// to drive an interpolation (a Lerp factor, a fade, a tween). The input is the
// linear fraction of an animation's elapsed time; the output is the eased
// fraction. Keep call sites simple:
//
//     const float k = Ease(Easing::EaseInOut, t);   // t is your raw 0..1 time
//     pos = Lerp(from, to, k);
//
// Most curves stay within [0,1]; the springy ones (Bounce/Elastic/Back)
// deliberately overshoot, which is fine for position/scale tweens but should
// be avoided where the eased value must stay bounded. Designed to be generic:
// nothing here knows about the party, the UI, or any particular animation.
// ============================================================================
#pragma once

#include <cmath>

#include "Core/MathTypes.h" // kPi

namespace dungeon {

// Easing curves, ordered roughly by aggressiveness. Linear is the identity and
// the default everywhere — pass it to get plain interpolation.
enum class Easing {
	Linear,      // identity: constant rate
	EaseIn,      // quadratic: slow start, accelerates into the target
	EaseOut,     // quadratic: fast start, decelerates into the target
	EaseInOut,   // smoothstep: slow start AND slow stop (the gentle default)
	LinearStart, // constant velocity at t=0 (slope 1), eases out to a stop
	LinearEnd,   // eases in from rest, constant velocity at t=1 (slope 1)
	Bounce,      // settles onto the target with a few decaying bounces
	Elastic,     // overshoots and springs back like a damped oscillator
	Back,        // small anticipation pull-back, overshoots, then settles
};

namespace detail {
// Penner-style bounce-out: t in [0,1] -> [0,1], a series of decaying parabolas.
inline float BounceOut(float t) {
	constexpr float n1 = 7.5625f;
	constexpr float d1 = 2.75f;
	if (t < 1.0f / d1) {
		return n1 * t * t;
	} else if (t < 2.0f / d1) {
		t -= 1.5f / d1;
		return n1 * t * t + 0.75f;
	} else if (t < 2.5f / d1) {
		t -= 2.25f / d1;
		return n1 * t * t + 0.9375f;
	}
	t -= 2.625f / d1;
	return n1 * t * t + 0.984375f;
}
} // namespace detail

// Maps raw progress t (clamped to [0,1]) through the chosen curve. The springy
// curves may return values slightly outside [0,1] near the ends.
inline float Ease(Easing type, float t) {
	if (t <= 0.0f) return 0.0f;
	if (t >= 1.0f) return 1.0f;
	switch (type) {
	case Easing::Linear:
		return t;
	case Easing::EaseIn:
		return t * t;
	case Easing::EaseOut:
		return t * (2.0f - t); // 1 - (1-t)^2
	case Easing::EaseInOut:
		return t * t * (3.0f - 2.0f * t); // smoothstep
	// Half-eased cubics for chaining tweens. The "linear" end has slope 1 (the
	// segment's average velocity), so a LinearEnd segment meets a following
	// LinearStart segment of equal duration with matched velocity — no
	// brake-then-relaunch at the seam. The eased end has slope 0 (rest).
	case Easing::LinearStart:
		return t + t * t - t * t * t; // f'(0)=1, f'(1)=0
	case Easing::LinearEnd:
		return t * t * (2.0f - t); // 2t^2 - t^3: f'(0)=0, f'(1)=1
	case Easing::Bounce:
		return detail::BounceOut(t);
	case Easing::Elastic: {
		constexpr float c4 = (2.0f * kPi) / 3.0f;
		return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
	}
	case Easing::Back: {
		constexpr float c1 = 1.70158f;
		constexpr float c3 = c1 + 1.0f;
		const float u = t - 1.0f;
		return 1.0f + c3 * u * u * u + c1 * u * u; // back-out: overshoot then settle
	}
	}
	return t;
}

// Convenience: ease t and Lerp between two scalars / vectors in one call.
inline float EaseLerp(Easing type, float a, float b, float t) {
	return Lerp(a, b, Ease(type, t));
}
inline Vec3 EaseLerp(Easing type, const Vec3& a, const Vec3& b, float t) {
	return Lerp(a, b, Ease(type, t));
}

} // namespace dungeon
