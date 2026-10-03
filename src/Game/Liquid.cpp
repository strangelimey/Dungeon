// ============================================================================
// Game/Liquid.cpp - see Liquid.h.
// ============================================================================
#include "Game/Liquid.h"

#include <algorithm>
#include <cmath>

namespace dungeon::game::liquid {

namespace {

Vec3 Normalized(Vec3 v) {
	const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
	return l > 1e-12f ? Vec3{v.x / l, v.y / l, v.z / l} : Vec3{0.0f, 1.0f, 0.0f};
}

// How far the liquid sits inside the inner wall, as a share of the glass's
// height: enough that the two surfaces never fight for the same depth, too
// little to see a gap.
constexpr float kInset = 0.004f;
// The single-wall fallback's shrink toward the axis.
constexpr float kFallbackShrink = 0.92f;
// Below this share of the glass's triangles facing into the cavity, the model
// is taken to have no inner wall at all.
constexpr float kMinInnerShare = 0.2f;

} // namespace

Shell Build(const assets::MeshData& glass) {
	Shell out;
	const auto& vs = glass.vertices;
	const auto& is = glass.indices;
	if (vs.empty() || is.size() < 3) return out;

	Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
	for (const auto& v : vs) {
		lo = {std::min(lo.x, v.position.x), std::min(lo.y, v.position.y),
			  std::min(lo.z, v.position.z)};
		hi = {std::max(hi.x, v.position.x), std::max(hi.y, v.position.y),
			  std::max(hi.z, v.position.z)};
	}
	const float ax = 0.5f * (lo.x + hi.x), az = 0.5f * (lo.z + hi.z); // the vertical axis
	const float height = std::max(hi.y - lo.y, 1e-6f);

	// A triangle of the INNER wall points into the cavity: toward the axis on a
	// wall, straight up on the cavity floor. The outside points away from the
	// axis, or down under the base; the lip's flat top points up but sits at the
	// very top, which is what the height test keeps out.
	const size_t triCount = is.size() / 3;
	std::vector<bool> inner(triCount, false);
	size_t innerCount = 0;
	for (size_t t = 0; t < triCount; ++t) {
		const auto& a = vs[is[t * 3]];
		const auto& b = vs[is[t * 3 + 1]];
		const auto& c = vs[is[t * 3 + 2]];
		const Vec3 n = Normalized({a.normal.x + b.normal.x + c.normal.x,
								   a.normal.y + b.normal.y + c.normal.y,
								   a.normal.z + b.normal.z + c.normal.z});
		const float cx = (a.position.x + b.position.x + c.position.x) / 3.0f - ax;
		const float cy = (a.position.y + b.position.y + c.position.y) / 3.0f;
		const float cz = (a.position.z + b.position.z + c.position.z) / 3.0f - az;
		const float rl = std::sqrt(cx * cx + cz * cz);
		const float radial = rl > 1e-6f ? (n.x * cx + n.z * cz) / rl : 0.0f;
		const bool wall = radial < -0.2f;
		const bool floor = n.y > 0.5f && radial <= 0.1f && cy < hi.y - 0.02f * height;
		if (wall || floor) {
			inner[t] = true;
			++innerCount;
		}
	}
	out.fromInnerWall = innerCount >= static_cast<size_t>(kMinInnerShare * triCount);

	auto& mesh = out.mesh;
	mesh.material = -1;
	mesh.skinned = false;
	mesh.vertices.reserve(triCount * 3);
	mesh.indices.reserve(triCount * 3);
	const float inset = kInset * height;
	float bottom = 1e9f, top = -1e9f;
	for (size_t t = 0; t < triCount; ++t) {
		if (out.fromInnerWall && !inner[t]) continue;
		const u32 corner[3] = {is[t * 3], is[t * 3 + 1], is[t * 3 + 2]};
		const u32 base = static_cast<u32>(mesh.vertices.size());
		for (const u32 k : corner) {
			assets::Vertex v = vs[k];
			const Vec3 n = Normalized(v.normal);
			if (out.fromInnerWall) {
				// Into the cavity by the inset, then turned to face the glass.
				v.position = {v.position.x + n.x * inset, v.position.y + n.y * inset,
							  v.position.z + n.z * inset};
				v.normal = {-n.x, -n.y, -n.z};
			} else {
				v.position = {ax + (v.position.x - ax) * kFallbackShrink, v.position.y,
							  az + (v.position.z - az) * kFallbackShrink};
				v.normal = n;
			}
			bottom = std::min(bottom, v.position.y);
			top = std::max(top, v.position.y);
			mesh.vertices.push_back(v);
		}
		// An inner-wall triangle is reversed with its normal; the fallback keeps
		// the glass's own winding, which already faces out.
		if (out.fromInnerWall)
			mesh.indices.insert(mesh.indices.end(), {base, base + 2, base + 1});
		else
			mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
		++out.triangles;
	}
	out.bottom = bottom;
	out.top = top;
	return out;
}

} // namespace dungeon::game::liquid
