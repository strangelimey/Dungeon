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
	// Shadow cube slot assigned for this frame (-1 = casts no shadows).
	// Slot 0 is highest resolution; the game gives slots to the lights
	// nearest the camera so shadow detail falls off with distance.
	int shadowSlot = -1;
	// This light's position jitters every frame for flicker (fires). The shadow
	// cache throttles such cubes on a frame interval instead of re-rendering on
	// every sub-pixel wander; steady lights (torch, glow) cache until they move.
	bool flickerShadow = false;
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

class Texture;

// Air turbidity for the scene. `turbidityMap` is a top-down density grid
// (R channel, one texel per dungeon cell, bilinear-blended); the scene
// shader raymarches it for extinction and torch-lit in-scattering. A null
// map means perfectly clear air.
struct Atmosphere {
	const Texture* turbidityMap = nullptr;
	Vec2 worldExtent{1.0f, 1.0f};      // world size the map covers (x, z)
	// Dust albedo tint, deliberately well below 1: it also acts as the
	// global in-scatter scale so torches make dust glow without washing
	// the whole frame out.
	Vec3 hazeColor{0.50f, 0.42f, 0.30f};
	float density = 0.09f;             // optical depth per meter at turbidity 1
	float hazeAmbient = 1.2f;          // how much ambient light the dust catches
};

} // namespace dungeon::gfx
