// ============================================================================
// Graphics/LightTiles.cpp - see LightTiles.h.
// ============================================================================
#include "Graphics/LightTiles.h"

#include <algorithm>
#include <cmath>

namespace dungeon::gfx {

namespace {
// Column `k` of a row-vector matrix: the coefficients of one clip component
// as a function of (p, 1).
struct Coeffs {
	float x, y, z, w;
};
Coeffs Column(const Mat4& m, int k) {
	switch (k) {
	case 0: return {m._11, m._21, m._31, m._41};
	case 1: return {m._12, m._22, m._32, m._42};
	case 2: return {m._13, m._23, m._33, m._43};
	default: return {m._14, m._24, m._34, m._44};
	}
}
} // namespace

LightTiler::LightTiler(const Mat4& vp) {
	const Coeffs cx = Column(vp, 0), cy = Column(vp, 1), cw = Column(vp, 3);
	// The plane clip.v - a * clip.w = 0 (sign s turns it to face the way wanted),
	// scaled to unit normal so its value at a point is a distance in metres.
	const auto plane = [](const Coeffs& v, const Coeffs& w, float a, float s) {
		Plane p{s * (v.x - a * w.x), s * (v.y - a * w.y), s * (v.z - a * w.z),
				s * (v.w - a * w.w)};
		const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
		const float inv = len > 0.0f ? 1.0f / len : 0.0f;
		return Plane{p.x * inv, p.y * inv, p.z * inv, p.w * inv};
	};
	for (u32 i = 0; i <= kLightTilesX; ++i) {
		const float a = -1.0f + 2.0f * static_cast<float>(i) / kLightTilesX;
		m_cols[i] = plane(cx, cw, a, 1.0f); // positive to the RIGHT of x = a
	}
	for (u32 i = 0; i <= kLightTilesY; ++i) {
		const float a = 1.0f - 2.0f * static_cast<float>(i) / kLightTilesY;
		m_rows[i] = plane(cy, cw, a, -1.0f); // positive BELOW y = a
	}
	m_front = plane(cw, {0, 0, 0, 0}, 0.0f, 1.0f); // positive in front of the eye
}

bool LightTiler::Range(const Vec3& c, float r, TileRange& out) const {
	if (m_front.Distance(c) < -r) return false; // wholly behind the eye
	// A wedge between planes k and k+1 is touched when the sphere reaches the
	// positive side of k and the negative side of k+1. The touched wedges are a
	// contiguous run, so the first and last bound it.
	const auto run = [&](const Plane* planes, int count, int& lo, int& hi) {
		lo = -1;
		hi = -1;
		for (int k = 0; k < count; ++k) {
			if (planes[k].Distance(c) < -r) continue;    // wholly before this one
			if (planes[k + 1].Distance(c) > r) continue; // wholly past the next
			if (lo < 0) lo = k;
			hi = k;
		}
		return lo >= 0;
	};
	return run(m_cols, static_cast<int>(kLightTilesX), out.c0, out.c1) &&
		   run(m_rows, static_cast<int>(kLightTilesY), out.r0, out.r1);
}

u32 TileOf(float ndcX, float ndcY) {
	const int col = std::clamp(static_cast<int>(std::floor((ndcX * 0.5f + 0.5f) * kLightTilesX)),
							   0, static_cast<int>(kLightTilesX) - 1);
	const int row = std::clamp(static_cast<int>(std::floor((0.5f - ndcY * 0.5f) * kLightTilesY)),
							   0, static_cast<int>(kLightTilesY) - 1);
	return static_cast<u32>(row) * kLightTilesX + static_cast<u32>(col);
}

void BinLights(const Mat4& viewProj, std::span<const LightSphere> spheres,
			   std::span<u64> masks) {
	std::fill(masks.begin(), masks.end(), u64{0});
	const LightTiler tiler(viewProj);
	const size_t n = std::min<size_t>(spheres.size(), 64);
	for (size_t i = 0; i < n; ++i) {
		TileRange t;
		if (!tiler.Range(spheres[i].center, spheres[i].radius, t)) continue;
		const u64 bit = u64{1} << i;
		for (int row = t.r0; row <= t.r1; ++row)
			for (int col = t.c0; col <= t.c1; ++col)
				masks[static_cast<size_t>(row) * kLightTilesX + static_cast<size_t>(col)] |= bit;
	}
}

} // namespace dungeon::gfx
