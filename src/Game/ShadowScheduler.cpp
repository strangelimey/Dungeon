// ============================================================================
// Game/ShadowScheduler.cpp — see ShadowScheduler.h.
// ============================================================================
#include "Game/ShadowScheduler.h"

#include "Game/DungeonMap.h" // kCellSize

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ranges>

namespace dungeon::game {

ShadowScheduler::ShadowScheduler() {
	// Rebuilt every frame into retained capacity — no steady-state allocation.
	m_candidates.reserve(gfx::kMaxPointLights);
}

void ShadowScheduler::AssignSlots(std::span<gfx::PointLight> lights, const Vec3& eye,
								  bool shadowsEnabled) {
	static_assert(gfx::kShadowSlots <= gfx::kMaxPointLights);

	for (gfx::PointLight& light : lights) {
		light.shadowSlot = -1;
		light.shadowStrength = 1.0f;
	}
	if (!shadowsEnabled) { // dev console: lights stay lit, just unshadowed
		m_prevPos.clear();
		return;
	}

	// Rank candidate lights by distance to the eye (linear, so the hysteresis
	// margin is in metres). A light that held a slot last frame gets a small
	// discount so two near-equidistant fires don't trade slots — and the
	// resolution tier that rides on the slot — back and forth as the party moves
	// between them; the steadier slot also lets the cube cache reuse more often.
	constexpr float kHysteresis = 0.75f; // metres of slack for a slot incumbent
	constexpr float kReMatch2 = 0.25f;   // (0.5 m)²: "still the same light"

	m_candidates.clear();
	const size_t lightCount = std::min<size_t>(lights.size(), gfx::kMaxPointLights);
	for (size_t i = 0; i < lightCount; ++i) {
		if (!lights[i].castsShadow) continue; // pure fill light (runes)
		const Vec3 d = Sub(lights[i].position, eye);
		float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
		for (const Vec3& prev : m_prevPos) {
			const Vec3 e = Sub(lights[i].position, prev);
			if (e.x * e.x + e.y * e.y + e.z * e.z <= kReMatch2) {
				dist -= kHysteresis; // incumbent: bias toward keeping its slot
				break;
			}
		}
		m_candidates.emplace_back(dist, i);
	}
	std::ranges::sort(m_candidates);

	// Two fade profiles, both ending at the light's radius and smoothstepped
	// (softer than linear), anchored to per-light distance (a STABLE quantity,
	// unlike the rank cutoff that drifts with how many lights are near):
	//   - longShadowFade (braziers): fade across most of the reach, from 12% of
	//     the radius out — a long LOD ramp the big brazier radius is sized for.
	//   - default (sconces, glows): keep full strength except in the outer band,
	//     so a caster beside a normal-radius light still casts a visible shadow
	//     at viewing distance instead of fading out under the brazier tuning.
	constexpr float kFadeStartFrac = 0.12f;           // long ramp: inner edge = 12% of radius
	constexpr float kEdgeFadeBand = 1.5f * kCellSize; // default: soften the outer ~1.5 cells

	const size_t count = std::min<size_t>(m_candidates.size(), gfx::kShadowSlots);
	m_prevPos.clear();
	for (size_t slot = 0; slot < count; ++slot) {
		gfx::PointLight& light = lights[m_candidates[slot].second];
		light.shadowSlot = static_cast<int>(slot);

		// Fade the shadow in over distance so it dissolves in on approach
		// instead of popping when the light wins a slot.
		{
			// True distance, not the hysteresis-discounted sort key.
			const Vec3 d = Sub(light.position, eye);
			const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
			const float fadeEnd = light.radius;
			const float fadeStart = light.longShadowFade
										? fadeEnd * kFadeStartFrac
										: std::max(0.0f, fadeEnd - kEdgeFadeBand);
			const float t = (fadeEnd > fadeStart)
								? std::clamp((fadeEnd - dist) / (fadeEnd - fadeStart), 0.0f, 1.0f)
								: 1.0f;
			light.shadowStrength = t * t * (3.0f - 2.0f * t); // smoothstep, gentler
		}

		m_prevPos.push_back(light.position);
	}
}

void ShadowScheduler::BeginPass() {
	++m_frameCounter;

	// The scheduler's own wall clock. A flicker cadence is about how fast the
	// fire LOOKS like it is moving, which is a property of seconds, not of frames
	// or of simulation time.
	const auto now = std::chrono::steady_clock::now();
	if (!m_haveEpoch) {
		m_epoch = now;
		m_haveEpoch = true;
	}
	m_nowSec = std::chrono::duration<f64>(now - m_epoch).count();

	// Refilled per pass. This is what stops every fire coming due on the same
	// frame and spiking it — they end up naturally staggered instead.
	m_flickerLeft = m_flickerBudget;
}

void ShadowScheduler::SetFlickerHz(float hz, int perFrameBudget) {
	m_flickerHz = std::clamp(hz, 0.0f, 240.0f); // 0 = never re-render for flicker
	if (perFrameBudget >= 0) m_flickerBudget = perFrameBudget;
}

bool ShadowScheduler::ShouldRender(const gfx::PointLight& light, size_t lightIndex,
								   u32 mapRevision, bool animatedCasterNear) {
	constexpr float kPosEps2 = 0.0004f; // 2 cm: a steady light re-renders once it moves

	const int slot = light.shadowSlot;
	SlotCache& cache = m_cache[slot];

	const Vec3 d = Sub(light.position, cache.pos);
	const bool moved = d.x * d.x + d.y * d.y + d.z * d.z > kPosEps2;
	// A wandering fire cube is due only once its interval has ELAPSED, and only
	// while the frame still has flicker budget left. Everything else in
	// needsRender below is correctness — a new light, moved geometry, an animating
	// caster — and is never budgeted away.
	const f64 interval = m_flickerHz > 0.0f ? 1.0 / static_cast<f64>(m_flickerHz) : 1.0e9;
	bool flickerDue = false;
	if (light.flickerShadow && m_flickerLeft > 0 &&
		m_nowSec - cache.lastFlickerSec >= interval) {
		flickerDue = true;
		--m_flickerLeft;
	}
	const bool needsRender = cache.lightId != static_cast<int>(lightIndex) ||
							 cache.revision != mapRevision || animatedCasterNear ||
							 (light.flickerShadow ? flickerDue : moved);
	if (needsRender) {
		cache.lightId = static_cast<int>(lightIndex);
		cache.pos = light.position;
		cache.revision = mapRevision;
		// Only a FLICKER render re-paces the flicker clock. A cube re-rendered
		// because a monster walked past should not also reset the aesthetic
		// cadence, or a busy room would flicker faster than a quiet one.
		if (flickerDue) cache.lastFlickerSec = m_nowSec;
	}
	return needsRender;
}

void ShadowScheduler::InvalidateCubes() {
	for (SlotCache& cache : m_cache) cache = SlotCache{};
}

} // namespace dungeon::game
