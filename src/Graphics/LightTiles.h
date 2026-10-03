// ============================================================================
// Graphics/LightTiles.h - which lights can touch which part of the screen
// (lighting-updates Phase 3, docs/lighting-updates-plan.md).
//
// The screen is cut into a fixed grid of TILES in normalized device
// coordinates (kLightTilesX x kLightTilesY, whatever the resolution, so the
// same grid serves the main view, the editor previews and the icon bakes).
// Each tile gets a 64-bit MASK, one bit per uploaded point light, set when the
// light's sphere can reach any pixel in that tile. The scene shader loops over
// its tile's bits instead of over every light.
//
// THE DUST MARCH IS WHY THIS IS ENOUGH. Every sample the haze raymarch takes
// lies on the eye-to-surface ray, and every point on that ray projects to the
// same pixel - so the pixel's tile mask is exact for the march too, not just
// for the surface.
//
// HOW A SPHERE IS BINNED. Every tile column is the wedge between two planes
// through the eye (NDC x = a and x = b), every row likewise, so a sphere's
// columns are the run of wedges it touches - a signed distance per plane, 33
// planes across and 19 down, built once per view (LightTiler). A sphere wholly
// behind the eye reaches nothing. A sphere that CONTAINS the eye touches every
// wedge and so every tile, and that is right, not loose: each pixel's ray
// starts inside it, so it lights the haze in front of every pixel.
//
// CONSERVATIVE BY CONSTRUCTION: the wedge test admits a sphere that touches a
// column's slab and a row's slab without touching the tile where they cross,
// and a wedge plane extends behind the eye. Both only ever ADD a light to a
// tile; a light is never missing from a tile it reaches. RollTest checks that
// direction by sampling points inside spheres.
//
// Pure (DirectXMath storage types only), so RollTest links it.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <span>

namespace dungeon::gfx {

inline constexpr u32 kLightTilesX = 32;
inline constexpr u32 kLightTilesY = 18;
inline constexpr u32 kLightTileCount = kLightTilesX * kLightTilesY;

// The tiles a sphere reaches: columns [c0, c1] by rows [r0, r1], row 0 at the
// TOP of the screen.
struct TileRange {
	int c0 = 0, c1 = 0, r0 = 0, r1 = 0;
};

// The tile grid's planes for one view (row-vector convention: clip = p * M).
// Build once per view and ask it about as many spheres as there are.
class LightTiler {
public:
	explicit LightTiler(const Mat4& viewProj);
	// False = the sphere reaches no pixel of this view (behind the eye, or off
	// the screen's edges).
	bool Range(const Vec3& center, float radius, TileRange& out) const;

private:
	struct Plane {
		float x, y, z, w; // normalized: (x, y, z) . p + w = signed distance
		float Distance(const Vec3& p) const { return x * p.x + y * p.y + z * p.z + w; }
	};
	Plane m_cols[kLightTilesX + 1]; // NDC x = -1 .. 1, facing +x
	Plane m_rows[kLightTilesY + 1]; // NDC y = 1 .. -1 (top first), facing down
	Plane m_front;                  // w = 0 through the eye, facing forward
};

// The tile a point in NDC falls in (clamped to the grid) - the shader's
// LightMask does the same arithmetic.
u32 TileOf(float ndcX, float ndcY);

// Bins spheres (position + radius, up to 64) into `masks` (kLightTileCount).
// Bit i of a tile's mask = sphere i reaches it.
struct LightSphere {
	Vec3 center;
	float radius;
};
void BinLights(const Mat4& viewProj, std::span<const LightSphere> spheres,
			   std::span<u64> masks);

} // namespace dungeon::gfx
