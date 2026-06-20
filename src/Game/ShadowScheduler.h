// ============================================================================
// Game/ShadowScheduler.h — point-light shadow-cube budgeting.
//
// Split out of DungeonWorld. Given the frame's point lights and the camera eye
// it does two jobs and owns no rendering or world state:
//   1. AssignSlots — hands the kShadowSlots cube-shadow slots to the lights
//      NEAREST the eye, slot 0 (highest resolution + PCF) to the closest and
//      coarser slots outward. Assignment is HYSTERETIC (a slot holder resists
//      being bumped by a marginally-closer rival, matched by position since the
//      light list is rebuilt every frame) and each slotted light gets a
//      shadowStrength that fades in over distance, so a shadow dissolves in
//      instead of popping the instant the light wins a slot.
//   2. ShouldRender — decides, per slot, whether the slot's CACHED cube can be
//      reused this frame or must be re-rendered (the light changed/moved, a
//      flicker tick is due, the map geometry changed, or an animating caster is
//      within the light). The caller does the actual cube draws when it says so.
//
// The carried torch sits at the eye, so it always wins slot 0. The world (a)
// builds the light list, (b) supplies the live map revision + an animated-caster
// verdict, and (c) renders the cubes ShouldRender selects.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Graphics/Lights.h"
#include "Graphics/Renderer.h" // gfx::kShadowSlots

#include <span>
#include <utility>
#include <vector>

namespace dungeon::game {

class ShadowScheduler {
public:
	ShadowScheduler();

	// Assigns shadow slots to the lights nearest `eye`, setting each light's
	// shadowSlot (-1 = none) and shadowStrength fade in place. shadowsEnabled
	// false clears every slot (dev-console `shadows off`: lights stay lit).
	void AssignSlots(std::span<gfx::PointLight> lights, const Vec3& eye,
					 bool shadowsEnabled);

	// Call once at the start of the shadow pass (advances the flicker clock that
	// throttles wandering fire cubes to half rate).
	void BeginPass();

	// Whether slot `light.shadowSlot`'s cube must be re-rendered this frame for
	// the light at list index `lightIndex`. `mapRevision` is the live geometry
	// revision; `animatedCasterNear` is the world's verdict that a monster/pillar
	// animates within the light. Returns true AND records the render in the slot
	// cache; false means the cube (still bound as an SRV) is reused.
	bool ShouldRender(const gfx::PointLight& light, size_t lightIndex,
					  u32 mapRevision, bool animatedCasterNear);

	// Forces every cached cube to re-render next frame (quality swap, level load).
	void InvalidateCubes();

private:
	// Scratch for AssignSlots: (distance, light index), sorted nearest-first into
	// retained capacity each frame.
	std::vector<std::pair<float, size_t>> m_candidates;
	// Positions of the lights that held slots last frame — the hysteresis anchor
	// (matched by position, not index: the light list is rebuilt every frame and
	// budget culling can shuffle indices, but a fire only wanders a few cm).
	std::vector<Vec3> m_prevPos;

	// Per-slot cube cache: the slot's cube is reused unless a ShouldRender
	// condition trips. Keyed by the light's index in the frame's light list.
	struct SlotCache {
		int lightId = -1;           // list index that last rendered this slot
		Vec3 pos{};                 // light position at that render
		u32 revision = 0xFFFFFFFFu; // map geometry revision at that render
	};
	SlotCache m_cache[gfx::kShadowSlots];
	u64 m_frameCounter = 0;
};

} // namespace dungeon::game
