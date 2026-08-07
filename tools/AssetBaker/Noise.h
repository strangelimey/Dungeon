// ============================================================================
// Noise.h — deterministic hash/value/fBm noise shared by the bakers.
// Seeded and repeatable: the same inputs always bake the same assets.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <cmath>

namespace dungeon::baker {

inline float Hash(u32 x, u32 y, u32 seed) {
	u32 h = x * 374761393u + y * 668265263u + seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return static_cast<float>(h & 0xFFFFFF) / 16777216.0f;
}

// `periodX` (0 = off) wraps the LATTICE coordinate in x, making the noise
// exactly periodic with that period — same value AND same derivative either
// side of the wrap, since the two lattice cells are literally the same one.
// That is what lets a per-cell displacement field continue into its neighbour
// without a seam, so a worn wall panel can drop its edge pin.
//
// It must be a POWER OF TWO. A negative x floors to a huge u32 by wraparound,
// which is only consistent under the modulo if the period divides 2^32.
inline float ValueNoise(float x, float y, u32 seed, u32 periodX = 0) {
	const float fx = x - std::floor(x), fy = y - std::floor(y);
	const u32 xi = static_cast<u32>(std::floor(x)), yi = static_cast<u32>(std::floor(y));
	u32 x0 = xi, x1 = xi + 1;
	if (periodX) {
		x0 %= periodX;
		x1 %= periodX;
	}
	const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
	const float a = Hash(x0, yi, seed), b = Hash(x1, yi, seed);
	const float c = Hash(x0, yi + 1, seed), d = Hash(x1, yi + 1, seed);
	return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
}

// `periodX` is the period at the FIRST octave; each octave doubles x, so the
// period doubles with it and every octave stays periodic over the same span.
inline float Fbm(float x, float y, u32 seed, int octaves = 4, u32 periodX = 0) {
	float value = 0, amplitude = 0.5f;
	for (int i = 0; i < octaves; ++i) {
		value += ValueNoise(x, y, seed + static_cast<u32>(i), periodX) * amplitude;
		x *= 2.0f;
		y *= 2.0f;
		periodX *= 2u;
		amplitude *= 0.5f;
	}
	return value;
}

} // namespace dungeon::baker
