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

namespace {
float Distance2(const Vec3& a, const Vec3& b) {
	const Vec3 d = Sub(a, b);
	return d.x * d.x + d.y * d.y + d.z * d.z;
}
} // namespace

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
		m_incumbentCount = 0;
		return;
	}

	// Rank candidate lights by distance to the eye (linear, so the hysteresis
	// margin is in metres). A light that held a slot last frame gets a small
	// discount so two near-equidistant fires don't trade slots — and the
	// resolution tier that rides on the slot — back and forth as the party moves
	// between them; the steadier slot also lets the cube cache reuse more often.
	constexpr float kHysteresis = 0.75f; // metres of slack for a slot incumbent
	constexpr float kReMatch2 = 0.25f;   // (0.5 m)^2: an id-less light "still the same"

	m_candidates.clear();
	const size_t lightCount = std::min<size_t>(lights.size(), gfx::kMaxPointLights);
	for (size_t i = 0; i < lightCount; ++i) {
		const gfx::PointLight& light = lights[i];
		if (!light.castsShadow) continue; // pure fill light (runes)
		float dist = std::sqrt(Distance2(light.position, eye));
		// The incumbent BY ITS ID (code-review C187): the list is rebuilt and
		// re-ranked every frame, but the id is the same light's every frame. Only
		// a light with no id is matched by where it stands.
		for (size_t k = 0; k < m_incumbentCount; ++k) {
			const Incumbent& inc = m_incumbents[k];
			const bool same = light.id != 0
								  ? inc.id == light.id
								  : inc.id == 0 && Distance2(light.position, inc.pos) <= kReMatch2;
			if (same) {
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
	m_incumbentCount = 0;
	for (size_t slot = 0; slot < count; ++slot) {
		gfx::PointLight& light = lights[m_candidates[slot].second];
		light.shadowSlot = static_cast<int>(slot);

		// Fade the shadow in over distance so it dissolves in on approach
		// instead of popping when the light wins a slot.
		{
			// True distance, not the hysteresis-discounted sort key.
			const float dist = std::sqrt(Distance2(light.position, eye));
			const float fadeEnd = light.radius;
			const float fadeStart = light.longShadowFade
										? fadeEnd * kFadeStartFrac
										: std::max(0.0f, fadeEnd - kEdgeFadeBand);
			const float t = (fadeEnd > fadeStart)
								? std::clamp((fadeEnd - dist) / (fadeEnd - fadeStart), 0.0f, 1.0f)
								: 1.0f;
			light.shadowStrength = t * t * (3.0f - 2.0f * t); // smoothstep, gentler
		}

		m_incumbents[m_incumbentCount++] = {light.id, light.position};
	}
}

void ShadowScheduler::BeginPass() {
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
	for (SlotCache& cache : m_cache) cache.visited = false;
	++m_stats.passes;
}

void ShadowScheduler::SetFlickerHz(float hz, int perFrameBudget) {
	m_flickerHz = std::clamp(hz, 0.0f, 240.0f); // 0 = never re-render for flicker
	if (perFrameBudget >= 0) m_flickerBudget = perFrameBudget;
}

void ShadowScheduler::NoteCasterChanged(const Vec3& center, float radius) {
	if (m_ignoreNotes || m_noteOverflow) return;
	// A door leaf notes itself every frame it travels; between two passes that
	// is the same note again, and holding it twice would only fill the list.
	for (u32 i = 0; i < m_noteCount; ++i)
		if (m_notes[i].radius == radius && m_notes[i].center.x == center.x &&
			m_notes[i].center.y == center.y && m_notes[i].center.z == center.z)
			return;
	++m_stats.notes;
	if (m_noteCount < kMaxNotes) m_notes[m_noteCount++] = {center, radius};
	else m_noteOverflow = true;
}

bool ShadowScheduler::NotedNear(const Vec3& center, float radius) const {
	if (m_noteOverflow) return true;
	for (u32 i = 0; i < m_noteCount; ++i) {
		const float reach = radius + m_notes[i].radius;
		if (Distance2(m_notes[i].center, center) <= reach * reach) return true;
	}
	return false;
}

bool ShadowScheduler::ShouldRender(const gfx::PointLight& light, size_t lightIndex,
								   u32 mapRevision, bool casterMoving) {
	constexpr float kPosEps = 0.02f; // 2 cm: a steady light re-renders once it moves

	const int slot = light.shadowSlot;
	SlotCache& cache = m_cache[slot];
	cache.visited = true;

	// A MOVE: past 2 cm for a steady light, and for a wandering one past what its
	// wander alone could have done (PointLight::wander). A wandering light used
	// to ignore moves altogether, so a carried Firelight's shadow kept up with a
	// walking party only on the flicker cadence (code-review C187).
	const bool wanders = light.wander > 0.0f;
	const float slack = std::max(kPosEps, light.wander);
	const bool moved = !(wanders && m_ignoreMoves) &&
					   Distance2(light.position, cache.pos) > slack * slack;
	// A wandering fire cube is due only once its interval has ELAPSED, and only
	// while the frame still has flicker budget left. Everything else below is
	// correctness - a new light, moved geometry, a changed caster, a move - and
	// is never budgeted away.
	const f64 interval = m_flickerHz > 0.0f ? 1.0 / static_cast<f64>(m_flickerHz) : 1.0e9;
	bool flickerDue = false;
	if (wanders && m_flickerLeft > 0 && m_nowSec - cache.lastFlickerSec >= interval) {
		flickerDue = true;
		--m_flickerLeft;
	}
	// Who this is: its stable id, else (high bit set, so the two never collide)
	// its index in this frame's list.
	const u32 identity = light.id != 0 ? light.id : 0x80000000u | static_cast<u32>(lightIndex);
	Reason why = Reason::Count;
	if (cache.lightId != identity) why = Reason::New;
	else if (cache.revision != mapRevision) why = Reason::Geometry;
	else if (casterMoving || NotedNear(light.position, light.radius)) why = Reason::Caster;
	else if (moved) why = Reason::Moved;
	else if (flickerDue) why = Reason::Flicker;
	SlotStats& stats = m_stats.slots[static_cast<size_t>(slot)];
	if (why == Reason::Count) {
		// Reused: how far the light now stands from the pose the cube shows.
		stats.lag = std::max(stats.lag, std::sqrt(Distance2(light.position, cache.pos)));
		return false;
	}

	cache.lightId = identity;
	cache.pos = light.position;
	cache.radius = light.radius;
	cache.revision = mapRevision;
	// Only a FLICKER render re-paces the flicker clock. A cube re-rendered
	// because a monster walked past should not also reset the aesthetic
	// cadence, or a busy room would flicker faster than a quiet one.
	if (flickerDue) cache.lastFlickerSec = m_nowSec;
	if (why == Reason::New) stats.lag = 0.0f; // a new light's lag starts again
	++stats.renders;
	++stats.by[static_cast<size_t>(why)];
	stats.lightId = identity;
	stats.lastPass = m_stats.passes;
	return true;
}

void ShadowScheduler::EndPass() {
	if (m_noteCount > 0 || m_noteOverflow) {
		for (SlotCache& cache : m_cache) {
			if (cache.visited || cache.lightId == kNoLight) continue;
			if (!NotedNear(cache.pos, cache.radius)) continue;
			cache = SlotCache{};
			++m_stats.swept;
		}
	}
	if (m_noteOverflow) ++m_stats.overflows;
	m_noteCount = 0;
	m_noteOverflow = false;
}

void ShadowScheduler::InvalidateCubes() {
	for (SlotCache& cache : m_cache) cache = SlotCache{};
}

const char* ShadowScheduler::ReasonName(Reason reason) {
	switch (reason) {
	case Reason::New: return "new";
	case Reason::Geometry: return "geometry";
	case Reason::Caster: return "caster";
	case Reason::Moved: return "moved";
	case Reason::Flicker: return "flicker";
	default: return "?";
	}
}

} // namespace dungeon::game
