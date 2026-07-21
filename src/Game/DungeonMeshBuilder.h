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

// Instances the baked block models over every floor cell: floor + ceiling
// per cell and a wall block on each edge that borders solid rock. Each block
// span holds one mesh per texture variant; a cell picks its variant by a
// stable hash and stamps the MATCHING mesh into that variant's bucket, so
// geometric relief always pairs with the texture drawn over it. `niche` (a
// type→mesh resolver, nullable) stamps a niche panel in place of the plain wall
// panel on an edge carrying a niche (DungeonMap::NicheAt) — it rides the same
// variant bucket, so it keeps the wall's texture.
DungeonGeometry BuildDungeonGeometry(const DungeonMap& map,
									 std::span<const assets::MeshData> wallBlocks,
									 std::span<const assets::MeshData> floorBlocks,
									 std::span<const assets::MeshData> ceilingBlocks,
									 const CellHolesFn& holes = {},
									 const NicheMeshFn& niche = {});

// Builds just the geometry for one spatial chunk region (chunk coords
// chunkX/chunkZ, each covering kChunkCells cells), with every returned chunk
// tagged with its chunk index. The editor uses this to rebuild only the chunks
// an edit touched instead of the whole map. BuildDungeonGeometry is this run
// over every region.
DungeonGeometry BuildDungeonRegion(const DungeonMap& map,
								   std::span<const assets::MeshData> wallBlocks,
								   std::span<const assets::MeshData> floorBlocks,
								   std::span<const assets::MeshData> ceilingBlocks,
								   int chunkX, int chunkZ,
								   const CellHolesFn& holes = {},
								   const NicheMeshFn& niche = {});

} // namespace dungeon::game
