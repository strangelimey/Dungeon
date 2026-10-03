// ============================================================================
// Game/DungeonWorld_LightBudget.cpp - which of a frame's lights are drawn
// (lighting-updates Phase 3, docs/lighting-updates-plan.md).
//
// Every source pushes its light as a CANDIDATE (UpdateLights). This decides
// which reach the renderer, in four steps:
//   1. THE VIEW. A light whose sphere reaches no pixel of the view is dropped.
//      The test is Graphics/LightTiles.h's own LightTiler - the very test the
//      renderer bins with - so "culled" and "in no tile" can never disagree.
//   2. THE REACH. A light in a square the party cannot walk to (a sealed room,
//      the far side of solid rock) is dropped: it could only light the faces
//      of walls the party sees from the other side, which is a LEAK - the
//      shadowless lights shone straight through rock. A BFS from the party's
//      square, re-walked only when that square or the map changes.
//   3. THE RANKING. What remains is ranked by what it adds to the view - its
//      brightness through a falloff of its own reach and its distance from the
//      eye, so a big brazier down the hall can outrank a spark at your feet -
//      and the top Max Lights are kept. A held torch is always kept. Last
//      frame's keepers get a bonus, so two near-equal lights do not trade the
//      last place back and forth.
//   4. THE FADES. A light crossing the budget line FADES (a quarter second)
//      instead of switching. One the budget drops is still drawn while it
//      fades out, as long as there is room under the hard ceiling (64); one
//      that merely left the view keeps its fade, so turning round to a fire
//      does not show it brightening.
//
// Nothing here allocates in a settled frame: the candidate and scratch lists
// are reserved at construction, the fades are a fixed table, the BFS reuses
// its grid and queue.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Graphics/LightTiles.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace dungeon::game {

namespace {
constexpr float kFadeSeconds = 0.25f;
// Last frame's keepers rank this much higher (the slot-incumbent idea the
// shadow scheduler uses, for the same reason).
constexpr float kIncumbentBonus = 1.25f;
constexpr u16 kUnreached = 0xFFFFu;
} // namespace

DungeonWorld::LightFade& DungeonWorld::FadeFor(u32 key, float time, bool& fresh) {
	LightFade* stalest = &m_lightFades[0];
	for (LightFade& f : m_lightFades) {
		if (f.key == key) {
			fresh = false;
			f.lastSeen = time;
			return f;
		}
		if (f.lastSeen < stalest->lastSeen) stalest = &f;
	}
	// New: the entry seen longest ago (an empty one has never been seen).
	fresh = true;
	*stalest = LightFade{key, 0.0f, time, false};
	return *stalest;
}

void DungeonWorld::RefreshReach() {
	const int w = m_map.Width(), h = m_map.Height();
	const int px = m_party.GridX(), pz = m_party.GridZ();
	const size_t cells = static_cast<size_t>(w) * static_cast<size_t>(h);
	if (px == m_reachX && pz == m_reachZ && m_map.Revision() == m_reachRevision &&
		m_reach.size() == cells)
		return;
	m_reachX = px;
	m_reachZ = pz;
	m_reachRevision = m_map.Revision();
	// Same size as before = no allocation (assign keeps the capacity); a new
	// level's size is a level load, not a settled frame.
	m_reach.assign(cells, kUnreached);
	if (m_reachQueue.capacity() < cells) m_reachQueue.reserve(cells);
	m_reachQueue.clear();
	if (px < 0 || pz < 0 || px >= w || pz >= h) return;
	m_reach[static_cast<size_t>(pz) * w + px] = 0;
	m_reachQueue.push_back(pz * w + px);
	for (size_t head = 0; head < m_reachQueue.size(); ++head) {
		const int c = m_reachQueue[head];
		const int cx = c % w, cz = c / w;
		const u16 next = static_cast<u16>(m_reach[static_cast<size_t>(c)] + 1);
		constexpr int kDX[4] = {0, 1, 0, -1}, kDZ[4] = {-1, 0, 1, 0};
		for (int d = 0; d < 4; ++d) {
			const int nx = cx + kDX[d], nz = cz + kDZ[d];
			if (!m_map.IsWalkable(nx, nz)) continue; // also false off the map
			u16& slot = m_reach[static_cast<size_t>(nz) * w + nx];
			if (slot != kUnreached) continue;
			slot = next;
			m_reachQueue.push_back(nz * w + nx);
		}
	}
}

bool DungeonWorld::LightReachable(const Vec3& pos, float radius) const {
	const int w = m_map.Width(), h = m_map.Height();
	if (m_reach.size() != static_cast<size_t>(w) * static_cast<size_t>(h)) return true;
	const auto at = [&](int x, int z) {
		return x < 0 || z < 0 || x >= w || z >= h ? kUnreached
												  : m_reach[static_cast<size_t>(z) * w + x];
	};
	const int cx = static_cast<int>(std::floor(pos.x / kCellSize));
	const int cz = static_cast<int>(std::floor(pos.z / kCellSize));
	// A light standing in a solid square (a sconce's flame sits on the wall's
	// face) belongs to the open square beside it it can see.
	u16 steps = at(cx, cz);
	if (steps == kUnreached)
		steps = std::min({at(cx + 1, cz), at(cx - 1, cz), at(cx, cz + 1), at(cx, cz - 1)});
	if (steps == kUnreached) return false;
	// The deepest it could matter: something lit within its reach, seen from
	// as far as the view goes, both measured in grid steps (x1.42, the worst a
	// diagonal costs on a 4-connected grid).
	const float limit = 1.42f * (kFarPlane + radius) / kCellSize + 2.0f;
	return static_cast<float>(steps) <= limit;
}

void DungeonWorld::SelectLights(const Vec3& eye, float time) {
	const float dt = m_lastLightTime < 0.0f ? 0.0f : std::clamp(time - m_lastLightTime, 0.0f, 0.25f);
	m_lastLightTime = time;
	RefreshReach();
	const size_t budget = static_cast<size_t>(
		std::clamp(m_settings.maxPointLights, 1, static_cast<int>(gfx::kMaxPointLights)));
	// The renderer's own binning test (Graphics/LightTiles.h), so a light this
	// keeps is never one the shader would find in no tile.
	const gfx::LightTiler tiler(m_camera.ViewProj());

	m_lightCull = {};
	m_lightCull.candidates = static_cast<u32>(m_lights.points.size());
	m_lightCandidates.clear();
	for (size_t i = 0; i < m_lights.points.size(); ++i) {
		const gfx::PointLight& l = m_lights.points[i];
		const bool torch = (l.id >> 24) == static_cast<u32>(LightKind::Torch);
		gfx::TileRange tiles;
		if (!torch && !tiler.Range(l.position, l.radius, tiles)) {
			++m_lightCull.offscreen; // reaches no pixel: dropped, its fade kept as it was
			continue;
		}
		bool fresh = false;
		LightFade& fade = FadeFor(l.id, time, fresh);
		const bool reachable = torch || LightReachable(l.position, l.radius);
		float score = -1.0f; // unreachable: never kept, though it may fade out
		if (torch) {
			score = 1.0e30f;
		} else if (reachable) {
			const Vec3 d = Sub(l.position, eye);
			const float r2 = l.radius * l.radius;
			score = l.intensity * r2 / (r2 + d.x * d.x + d.y * d.y + d.z * d.z);
			if (fade.kept) score *= kIncumbentBonus;
		}
		m_lightCandidates.push_back({static_cast<u32>(i), score, fresh ? -1.0f : fade.fade,
									 false});
	}

	// Rank (a sort over the reserved list allocates nothing), keep the top.
	std::sort(m_lightCandidates.begin(), m_lightCandidates.end(),
			  [](const LightCandidate& a, const LightCandidate& b) { return a.score > b.score; });
	size_t kept = 0;
	for (LightCandidate& c : m_lightCandidates)
		if (c.score >= 0.0f && kept < budget) {
			c.keep = true;
			++kept;
		}

	// Fades: in for the kept, out for the dropped. A light seen for the first
	// time starts where it belongs (fully in if kept, out if not): a fresh fire
	// on a new level, or a new bolt, should not swell up from nothing.
	size_t drawn = kept;
	for (LightCandidate& c : m_lightCandidates) {
		bool unused = false;
		LightFade& f = FadeFor(m_lights.points[c.index].id, time, unused);
		const bool fresh = c.fade < 0.0f;
		if (c.keep) {
			f.fade = fresh ? 1.0f : std::min(1.0f, f.fade + dt / kFadeSeconds);
			f.kept = true;
		} else {
			f.fade = fresh ? 0.0f : std::max(0.0f, f.fade - dt / kFadeSeconds);
			f.kept = false;
			if (c.score < 0.0f) ++m_lightCull.unreachable;
			else ++m_lightCull.budget;
			// Still fading: drawn on, if the hard ceiling has room for it.
			if (f.fade > 0.0f && drawn < gfx::kMaxPointLights) {
				c.keep = true;
				++drawn;
				++m_lightCull.fadingOut;
			}
		}
		c.fade = f.fade;
	}

	// Out in PUSH order (torches, fires, ...), so the list reads the same frame
	// to frame; the shadow cache keys on ids, not on this order.
	std::sort(m_lightCandidates.begin(), m_lightCandidates.end(),
			  [](const LightCandidate& a, const LightCandidate& b) { return a.index < b.index; });
	m_lightScratch.clear();
	m_lightOriginScratch.clear();
	for (const LightCandidate& c : m_lightCandidates) {
		if (!c.keep) continue;
		gfx::PointLight l = m_lights.points[c.index];
		l.intensity *= c.fade;
		m_lightScratch.push_back(l);
		LightOrigin origin = m_lightOrigins[c.index];
		origin.fade = c.fade;
		m_lightOriginScratch.push_back(origin);
	}
	m_lights.points.swap(m_lightScratch);
	m_lightOrigins.swap(m_lightOriginScratch);
}

int DungeonWorld::SetStressLights(int count, bool nearby) {
	m_stressLights.clear();
	count = std::clamp(count, 0, static_cast<int>(m_stressLights.capacity()));
	if (count == 0) return 0;
	// Scattered over the open squares the party can reach - within 6 steps
	// (`nearby`: the worst case, the eye inside nearly every light) or anywhere
	// on the level (a level's many fires, mostly far off) - a few per square
	// at random heights and spots, each its own colour. The seed is fixed, so
	// a measurement is repeatable.
	RefreshReach();
	std::mt19937 rng(0x11647u);
	std::uniform_real_distribution<float> unit(0.0f, 1.0f);
	const int w = m_map.Width();
	size_t open = 0;
	for (u16 steps : m_reach)
		if (steps != kUnreached && (!nearby || steps <= 6)) ++open;
	// The chance a square takes a light on a pass, so the load spreads over
	// all of them rather than filling the first rows.
	const float take = open > 0 ? std::min(1.0f, 1.5f * static_cast<float>(count) / open) : 1.0f;
	for (int pass = 0; pass < 64 && static_cast<int>(m_stressLights.size()) < count; ++pass)
		for (size_t c = 0; c < m_reach.size() && static_cast<int>(m_stressLights.size()) < count;
			 ++c) {
			if (m_reach[c] == kUnreached || (nearby && m_reach[c] > 6) || unit(rng) > take)
				continue;
			const int x = static_cast<int>(c) % w, z = static_cast<int>(c) / w;
			const Vec3 at = m_map.CellCenter(x, z);
			const float hue = unit(rng) * 6.0f;
			const auto channel = [&](float shift) {
				const float k = std::fmod(hue + shift, 6.0f);
				return std::clamp(std::min(k, 4.0f - k), 0.0f, 1.0f) * 0.8f + 0.2f;
			};
			m_stressLights.push_back(
				{{at.x + (unit(rng) - 0.5f) * kCellSize * 0.8f, 0.4f + unit(rng) * 1.6f,
				  at.z + (unit(rng) - 0.5f) * kCellSize * 0.8f},
				 {channel(5.0f), channel(3.0f), channel(1.0f)}});
		}
	return static_cast<int>(m_stressLights.size());
}

} // namespace dungeon::game
