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

// Hard ceiling baked into the frame constant buffer (the array size); must
// match MAX_POINT_LIGHTS in scene.hlsl and the top of GameSettings'
// kLightBudgets (the Ultra tier). The per-frame light count is a runtime
// budget (GameSettings::maxPointLights, Low=16 .. Ultra=64) the game caps to;
// lights beyond it are dropped (nearest-to-camera kept — see UpdateLights).
inline constexpr u32 kMaxPointLights = 64;

struct PointLight {
	Vec3 position{};
	float radius = 8.0f;     // attenuation reaches zero here
	Vec3 color{1, 1, 1};
	float intensity = 1.0f;
	// Shadow cube slot assigned for this frame (-1 = casts no shadows).
	// Slot 0 is highest resolution; the game gives slots to the lights
	// nearest the camera so shadow detail falls off with distance.
	int shadowSlot = -1;
	// When false this light is never considered for a shadow slot — a pure fill
	// light (e.g. the glowing runes), so a clutter of them near the eye can't
	// starve the torch/fires of the few shadow cubes.
	bool castsShadow = true;
	// How strongly this light's shadow applies (0 = unshadowed, 1 = full). The
	// game ramps this 0->1 as the light comes within its shadow range so a
	// shadow dissolves in instead of popping when the light wins a slot; the
	// shader lerps toward "fully lit" by it (see AssignShadowSlots).
	float shadowStrength = 1.0f;
	// When true the shadow fades gradually across most of the light's reach (a
	// long LOD ramp — used by braziers, which carry a large radius for it). When
	// false only the outer edge of the reach softens, so a normal-radius light
	// (a wall sconce) keeps full-strength shadows at typical viewing distance —
	// a caster beside it still reads instead of fading out under the brazier
	// tuning.
	bool longShadowFade = false;
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
	// Density + haze-ambient trimmed in the lighting mood pass (0.09 → 0.075,
	// 1.2 → 0.9): the in-scatter wash was measurably burying surface shadows
	// in fire-dense rooms. Live-tunable from the dev console (`dust <d>`,
	// `haze <h>`) — bake re-tuned values back here.
	float density = 0.075f;            // optical depth per meter at turbidity 1
	float hazeAmbient = 0.9f;          // how much ambient light the dust catches

	// See-through peek (the Sight spell): the wall cell directly ahead the
	// party ghosts in the scene pixel shader while a Sight effect is active.
	// sightCell xy = the cell's world min (X,Z), zw = its max; INACTIVE when
	// zw <= xy (the default). sightTint rgb = the ghost/glow tint (the active
	// school's colour), a = strength (0 = off). Set per-frame by
	// DungeonWorld::RenderScene — a transient, not a level mood knob.
	Vec4 sightCell{0.0f, 0.0f, 0.0f, 0.0f};
	Vec4 sightTint{0.0f, 0.0f, 0.0f, 0.0f};
	// The peek is a round HOLE in the middle of the wall face, not the whole
	// cell: x = the hole's centre world Y (party eye height), y = radius (m),
	// z = whether the face's across-axis is world X (>0.5 for N/S facing, else
	// the across-axis is Z), w unused.
	Vec4 sightHole{0.0f, 0.0f, 0.0f, 0.0f};
};

} // namespace dungeon::gfx
