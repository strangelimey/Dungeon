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
#include <string_view>

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
	EaseInCubic, // cubic: very slow start, strong fast finish
	EaseInQuart, // quartic: even slower start, harder fast finish
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
	case Easing::EaseInCubic:
		return t * t * t;
	case Easing::EaseInQuart:
		return t * t * t * t;
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

// ============================================================================
// Shapes: the two ENDS of a motion, chosen independently
// ============================================================================
// `Easing` above is a set of WHOLE curves, and that is why it cannot express
// "slow start, bouncy finish": some of its entries are in-shaped (EaseIn,
// EaseInQuart), some out-shaped (Bounce, Elastic, Back) and some complete S
// curves (EaseInOut). Asking for one of them as "the ease-in" and another as
// "the ease-out" only means something if every option can be had in either
// orientation.
//
// So a SHAPE is orientation-free — a family of acceleration — and its two forms
// are derived: the out form is the in form reflected through the diagonal,
// Out(t) = 1 - In(1 - t). That identity is what makes an arbitrary pair
// composable, and it is why this is a separate enum rather than more entries in
// `Easing`: the existing curves are used by the party tweens and the settings
// page, where a whole curve is exactly what is wanted.
enum class EaseShape {
	Linear,  // no shaping: constant rate
	Quad,    // t^2 — the gentle default
	Cubic,   // t^3
	Quart,   // t^4 — noticeably heavy
	Sine,    // a quarter cosine: the softest of the smooth ones
	Expo,    // very slow then very fast; reads as mass
	Back,    // pulls the other way first, then goes
	Elastic, // springs past and oscillates in
	Bounce,  // strikes and rebounds, decaying
};

// The ACCELERATING form of a shape: 0 at t=0, 1 at t=1, starting slow.
inline float EaseIn(EaseShape s, float t) {
	if (t <= 0.0f) return 0.0f;
	if (t >= 1.0f) return 1.0f;
	switch (s) {
	case EaseShape::Linear:
		return t;
	case EaseShape::Quad:
		return t * t;
	case EaseShape::Cubic:
		return t * t * t;
	case EaseShape::Quart:
		return t * t * t * t;
	case EaseShape::Sine:
		return 1.0f - std::cos(t * kPi * 0.5f);
	case EaseShape::Expo:
		return std::pow(2.0f, 10.0f * (t - 1.0f));
	case EaseShape::Back: {
		constexpr float c1 = 1.70158f, c3 = c1 + 1.0f;
		return c3 * t * t * t - c1 * t * t;
	}
	case EaseShape::Elastic: {
		constexpr float c4 = (2.0f * kPi) / 3.0f;
		return -std::pow(2.0f, 10.0f * t - 10.0f) *
			   std::sin((t * 10.0f - 10.75f) * c4);
	}
	case EaseShape::Bounce:
		return 1.0f - detail::BounceOut(1.0f - t);
	}
	return t;
}

// The DECELERATING form, by reflection — see the note above.
inline float EaseOut(EaseShape s, float t) { return 1.0f - EaseIn(s, 1.0f - t); }

// A motion's two ends. Defaults are the gentle quadratic both ways, which is
// what an unshaped tween should feel like.
struct EaseSpan {
	EaseShape in = EaseShape::Quad;
	EaseShape out = EaseShape::Quad;
};

// Progress t through a span: the first half accelerates on `in`, the second
// decelerates on `out`, each mapped onto its own half of the output. Splitting
// at the midpoint is what lets the two be chosen independently — the curves
// meet at (0.5, 0.5) whatever they are, so no pair can produce a discontinuity.
// The value may leave [0,1] near the ends when a springy shape is chosen, which
// is the point of those shapes; clamp at the call site if that matters.
inline float Ease(const EaseSpan& span, float t) {
	if (t <= 0.0f) return 0.0f;
	if (t >= 1.0f) return 1.0f;
	return t < 0.5f ? 0.5f * EaseIn(span.in, t * 2.0f)
					: 0.5f + 0.5f * EaseOut(span.out, t * 2.0f - 1.0f);
}

// The catalog/ini spelling of each shape, indexed by EaseShape. One table so a
// value written by the editor is read back by the loader — and so the schema's
// dropdown, the .cat token and this enum cannot drift apart.
inline constexpr const char* kEaseShapeNames[] = {
	"linear", "quad", "cubic", "quart", "sine", "expo", "back", "elastic", "bounce",
};
inline constexpr int kEaseShapeCount = 9;

inline const char* EaseShapeName(EaseShape s) {
	const int i = static_cast<int>(s);
	return i >= 0 && i < kEaseShapeCount ? kEaseShapeNames[i] : kEaseShapeNames[1];
}

// Parses a shape name; anything unrecognised (including empty) takes `fallback`,
// so a hand-edited catalog with a typo animates rather than refusing to load.
inline EaseShape EaseShapeFromName(std::string_view name,
								   EaseShape fallback = EaseShape::Quad) {
	for (int i = 0; i < kEaseShapeCount; ++i)
		if (name == kEaseShapeNames[i]) return static_cast<EaseShape>(i);
	return fallback;
}

} // namespace dungeon
