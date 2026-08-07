// ============================================================================
// Game/DungeonMeshBuilder.h — bakes the map into batched render geometry.
//
// Runs once at load. For every floor cell it stamps copies of the block
// models (wall/floor/ceiling .gltf) into big combined vertex buffers — CPU
// "instancing" that turns ~700 block placements into at most
// (#texture variants) draw calls per surface type per frame. Each cell picks
// its texture variant with a position hash, so the mix of brick/stone/mossy
// walls is varied but identical on every run.
// ============================================================================
#pragma once

#include "Assets/Model.h"
#include "Game/DungeonMap.h"

#include <array>
#include <functional>
#include <span>
#include <vector>

namespace dungeon::game {

// One spatial chunk of batched geometry: the cells of a fixed map region that
// share a texture variant, combined into one mesh with its world AABB. Chunking
// (instead of one level-wide mesh per variant) lets the renderer frustum-cull
// off-screen regions in the main pass and sphere-cull out-of-range regions per
// shadow cube, at the cost of a few more draw calls.
struct GeometryChunk {
	int variant = 0; // index into the surface's parallel texture arrays
	int chunk = 0;   // spatial chunk index (cz * chunksX + cx) this came from
	assets::MeshData mesh;
	Vec3 boundsMin{}, boundsMax{};
};

// Batched dungeon geometry as cullable chunks (variant carried per chunk).
struct DungeonGeometry {
	std::vector<GeometryChunk> walls;
	std::vector<GeometryChunk> floors;
	std::vector<GeometryChunk> ceilings;
};

// Map cells per chunk edge (chunk = kChunkCells x kChunkCells cells). Sized so
// a chunk (~9.6 m at kCellSize) is a bit larger than a light radius — small
// enough that a shadow cube touches only a handful of chunks.
inline constexpr int kChunkCells = 4;

// The stable per-cell texture-variant hash (identical every run). `salt`
// separates a cell's three surfaces: 1 = floor, 2 = ceiling, 3 = wall. This is
// the DEFAULT only — an editor override (DungeonMap variant grid >= 0, clamped
// to count-1) wins over it. Exposed so the map overlay's textured cell fill
// resolves the exact variant StampCell bakes.
u32 SurfaceVariantFor(int x, int z, u32 salt, u32 count);

// Which of a cell's horizontal blocks are OPENINGS and must be skipped: the
// floor under a down-stair/pit (its below-grade shaft mesh replaces it), the
// ceiling under a pit's lower half on the level below (its rising shaft mesh
// replaces it). A null function means no holes.
struct CellHoles {
	bool floor = false, ceiling = false;
};
using CellHolesFn = std::function<CellHoles(int x, int z)>;

// Resolves a niche's wallfeatures.cat type to the panel mesh to stamp (null =
// unknown type → the plain wall panel is kept). Lets the builder support any
// number of niche shapes without knowing the catalog.
using NicheMeshFn = std::function<const assets::MeshData*(const std::string& type)>;

// One wall surface's worn panels — every phase in every side-pin combination.
// Assets/WornPanel.h states the naming convention and what the two axes mean.
//
// A worn panel's displacement is normally pinned back to the flat wall plane at
// every edge, because two panels are stamped independently and zero is the only
// value they can agree on sight unseen. That pin is what makes each square read
// as a shallow dish. Two panels of the SAME surface carry the same periodic
// field though, so they meet exactly with no pin at all — hence the OPEN-SIDED
// panels. And a non-square texture needs PHASES on top of that: one square shows
// only part of the image, so consecutive squares have to show consecutive parts
// or nothing joins, however the edges are pinned.
//
// A set that earns neither still bakes its one pinned panel, so an absent
// variant is normal rather than an error and `Panel` degrades to it.
struct WallPanels {
	// byPhase[phase][open bits]; open bits are (open -X ? 1 : 0) | (open +X ? 2).
	// Phase 0 slot 0 is always present — the loader will not build one without it.
	std::vector<std::array<assets::MeshData, 4>> byPhase;

	int PhaseCount() const {
		return byPhase.empty() ? 1 : static_cast<int>(byPhase.size());
	}

	const assets::MeshData& Panel(int phase, bool openLeft, bool openRight) const {
		const size_t p = static_cast<size_t>(phase) % byPhase.size();
		const size_t i = (openLeft ? 1u : 0u) | (openRight ? 2u : 0u);
		const std::array<assets::MeshData, 4>& row = byPhase[p];
		if (!row[i].vertices.empty()) return row[i];
		if (!row[0].vertices.empty()) return row[0];
		return byPhase[0][0];
	}
};

// Instances the baked block models over every floor cell: floor + ceiling
// per cell and a wall block on each edge that borders solid rock. Each block
// span holds one mesh per texture variant; a cell picks its variant by a
// stable hash and stamps the MATCHING mesh into that variant's bucket, so
// geometric relief always pairs with the texture drawn over it. `niche` (a
// type→mesh resolver, nullable) stamps a niche panel in place of the plain wall
// panel on an edge carrying a niche (DungeonMap::NicheAt) — it rides the same
// variant bucket, so it keeps the wall's texture.
// `bore` (a type→mesh resolver, nullable) is stamped on a face whose solid
// neighbour is bored see-through (DungeonMap::BoreAlong), same variant bucket.
//
// `wallUAspect` is each wall variant's texture aspect (width/height; 1 for a
// square scan), parallel to `wallBlocks`. It scales the U of a stamped wall
// FEATURE only. A feature mesh is ONE mesh shared by all 54 surfaces, so unlike
// the worn blocks it cannot carry a per-texture aspect in its baked UVs — and
// on a 2:1 set (ten of ours are) a niche would then tile at a different rate
// from the wall it is cut into. Here is the one place both are known, since the
// feature rides the wall's variant bucket. Empty = every surface square, which
// is what the code assumed before ten of them turned out not to be.
//
// `floorFeature` / `ceilingFeature` are the same substitution turned to face down
// and up: a type→mesh resolver whose tile is stamped IN PLACE OF the cell's floor
// or ceiling block (DungeonMap::FeatureAt), into that surface's variant bucket,
// so a recess sunk into the floor or a vault raised into the ceiling wears the
// cell's own texture for it. The matching `*UAspect` span corrects each exactly
// as `wallUAspect` corrects a niche. A cell may carry one of each.
DungeonGeometry BuildDungeonGeometry(const DungeonMap& map,
									 std::span<const WallPanels> wallBlocks,
									 std::span<const assets::MeshData> floorBlocks,
									 std::span<const assets::MeshData> ceilingBlocks,
									 std::span<const float> wallUAspect = {},
									 std::span<const float> floorUAspect = {},
									 std::span<const float> ceilingUAspect = {},
									 const CellHolesFn& holes = {},
									 const NicheMeshFn& niche = {},
									 const NicheMeshFn& bore = {},
									 const NicheMeshFn& floorFeature = {},
									 const NicheMeshFn& ceilingFeature = {});

// Builds just the geometry for one spatial chunk region (chunk coords
// chunkX/chunkZ, each covering kChunkCells cells), with every returned chunk
// tagged with its chunk index. The editor uses this to rebuild only the chunks
// an edit touched instead of the whole map. BuildDungeonGeometry is this run
// over every region.
DungeonGeometry BuildDungeonRegion(const DungeonMap& map,
								   std::span<const WallPanels> wallBlocks,
								   std::span<const assets::MeshData> floorBlocks,
								   std::span<const assets::MeshData> ceilingBlocks,
								   std::span<const float> wallUAspect,
								   std::span<const float> floorUAspect,
								   std::span<const float> ceilingUAspect,
								   int chunkX, int chunkZ,
								   const CellHolesFn& holes = {},
								   const NicheMeshFn& niche = {},
								   const NicheMeshFn& bore = {},
								   const NicheMeshFn& floorFeature = {},
								   const NicheMeshFn& ceilingFeature = {});

} // namespace dungeon::game
