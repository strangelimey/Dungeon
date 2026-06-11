// ============================================================================
// Graphics/Lights.h — the light set the game hands to the renderer each frame.
//
// Plain data; the renderer packs it into the frame constant buffer. Point
// lights use inverse-square falloff windowed to zero at `radius`. The
// directional light is disabled by a black color (no sun underground).
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <vector>

namespace dungeon::gfx {

// Hard cap baked into the frame constant buffer; must match MAX_POINT_LIGHTS
// in scene.hlsl. Lights beyond the cap are silently dropped.
inline constexpr u32 kMaxPointLights = 16;

struct PointLight {
	Vec3 position{};
	float radius = 8.0f;     // attenuation reaches zero here
	Vec3 color{1, 1, 1};
	float intensity = 1.0f;
};

struct DirectionalLight {
	Vec3 direction{0, -1, 0};
	Vec3 color{0, 0, 0}; // black = disabled
};

struct LightSet {
	Vec3 ambient{0.05f, 0.05f, 0.07f};
	DirectionalLight directional;
	std::vector<PointLight> points; // first kMaxPointLights are used
};

} // namespace dungeon::gfx
