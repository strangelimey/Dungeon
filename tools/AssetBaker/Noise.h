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

inline float ValueNoise(float x, float y, u32 seed) {
	const float fx = x - std::floor(x), fy = y - std::floor(y);
	const u32 xi = static_cast<u32>(std::floor(x)), yi = static_cast<u32>(std::floor(y));
	const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
	const float a = Hash(xi, yi, seed), b = Hash(xi + 1, yi, seed);
	const float c = Hash(xi, yi + 1, seed), d = Hash(xi + 1, yi + 1, seed);
	return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
}

inline float Fbm(float x, float y, u32 seed, int octaves = 4) {
	float value = 0, amplitude = 0.5f;
	for (int i = 0; i < octaves; ++i) {
		value += ValueNoise(x, y, seed + static_cast<u32>(i)) * amplitude;
		x *= 2.0f;
		y *= 2.0f;
		amplitude *= 0.5f;
	}
	return value;
}

} // namespace dungeon::baker
