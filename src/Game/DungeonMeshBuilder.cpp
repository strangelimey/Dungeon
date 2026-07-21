#include "Game/DungeonMeshBuilder.h"

#include <algorithm>
#include <utility>

using namespace DirectX;

namespace dungeon::game {

// Stable per-cell variant choice so the dungeon looks the same every run.
// (Header-declared: the map overlay resolves the same pick for its cell fill.)
u32 SurfaceVariantFor(int x, int z, u32 salt, u32 count) {
	u32 h = static_cast<u32>(x) * 73856093u ^ static_cast<u32>(z) * 19349663u ^
			salt * 83492791u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return count > 0 ? (h >> 8) % count : 0;
}

namespace {

// Appends `src` transformed by `m` (positions) and its rotation part (normals).
void AppendTransformed(assets::MeshData& dst, const assets::MeshData& src,
					   const XMMATRIX& m) {
	const u32 base = static_cast<u32>(dst.vertices.size());
	dst.vertices.reserve(dst.vertices.size() + src.vertices.size());
	for (const assets::Vertex& v : src.vertices) {
		assets::Vertex out = v;
		const XMVECTOR p = XMVector3Transform(XMLoadFloat3(&v.position), m);
		const XMVECTOR n =
			XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&v.normal), m));
		XMStoreFloat3(&out.position, p);
		XMStoreFloat3(&out.normal, n);
		dst.vertices.push_back(out);
	}
	dst.indices.reserve(dst.indices.size() + src.indices.size());
	for (const u32 i : src.indices) dst.indices.push_back(base + i);
}

} // namespace

namespace {

// Collects the non-empty per-variant buckets of one chunk region into cullable
// GeometryChunks (computing each one's world AABB), tagging them with the
// region's chunk index.
void Collect(std::vector<assets::MeshData>& buckets, int chunkIndex,
			 std::vector<GeometryChunk>& out) {
	for (size_t variant = 0; variant < buckets.size(); ++variant) {
		assets::MeshData& mesh = buckets[variant];
		if (mesh.vertices.empty()) continue;
		GeometryChunk chunk;
		chunk.variant = static_cast<int>(variant);
		chunk.chunk = chunkIndex;
		Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
		for (const assets::Vertex& v : mesh.vertices) {
			lo = {std::min(lo.x, v.position.x), std::min(lo.y, v.position.y),
				  std::min(lo.z, v.position.z)};
			hi = {std::max(hi.x, v.position.x), std::max(hi.y, v.position.y),
				  std::max(hi.z, v.position.z)};
		}
		chunk.boundsMin = lo;
		chunk.boundsMax = hi;
		chunk.mesh = std::move(mesh);
		out.push_back(std::move(chunk));
	}
}

// Instances one floor cell — its floor, ceiling, and the wall blocks on edges
// bordering solid rock — into per-variant bucket meshes (indexed by variant).
// `holes` drops the cell's floor and/or ceiling block (a stair/pit shaft
// opening; the prop mesh brings its own shaft geometry).
void StampCell(const DungeonMap& map, int x, int z, CellHoles holes,
			   std::span<const assets::MeshData> wallBlocks,
			   std::span<const assets::MeshData> floorBlocks,
			   std::span<const assets::MeshData> ceilingBlocks,
			   const NicheMeshFn& niche,
			   std::vector<assets::MeshData>& wallB,
			   std::vector<assets::MeshData>& floorB,
			   std::vector<assets::MeshData>& ceilB) {
	const u32 wallVariants = static_cast<u32>(wallBlocks.size());
	const u32 floorVariants = static_cast<u32>(floorBlocks.size());
	const u32 ceilingVariants = static_cast<u32>(ceilingBlocks.size());
	const Vec3 center = map.CellCenter(x, z);

	// An editor override (>= 0) pins the cell's variant; otherwise the stable
	// position hash chooses it. Clamp to the loaded variant count.
	const auto pick = [](int over, u32 hashed, u32 count) -> u32 {
		if (over < 0) return hashed;
		return count > 0 ? std::min(static_cast<u32>(over), count - 1) : 0u;
	};
	if (!holes.floor) {
		const u32 floorVariant = pick(map.FloorVariant(x, z),
									   SurfaceVariantFor(x, z, 1u, floorVariants), floorVariants);
		AppendTransformed(floorB[floorVariant], floorBlocks[floorVariant],
						  XMMatrixTranslation(center.x, 0, center.z));
	}
	if (!holes.ceiling) {
		const u32 ceilingVariant = pick(map.CeilingVariant(x, z),
										 SurfaceVariantFor(x, z, 2u, ceilingVariants), ceilingVariants);
		AppendTransformed(ceilB[ceilingVariant], ceilingBlocks[ceilingVariant],
						  XMMatrixTranslation(center.x, kWallHeight, center.z));
	}

	// Wall blocks are authored facing +Z; rotate so the face points into the
	// room. Camera convention: forward = (sin yaw, 0, cos yaw).
	struct Edge {
		int dx, dz;
		float yaw;
		Vec3 pos;
	};
	const float s = kCellSize;
	const Edge edges[4] = {
		{0, -1, 0.0f, {center.x, 0, z * s}},              // north
		{0, +1, kPi, {center.x, 0, (z + 1) * s}},         // south
		{-1, 0, kPi * 0.5f, {x * s, 0, center.z}},        // west
		{+1, 0, -kPi * 0.5f, {(x + 1) * s, 0, center.z}}, // east
	};
	// Each wall face takes its texture from the SOLID block it belongs to: the
	// block owns its texture (all faces of one block agree, both sides of a
	// shared wall included), and the editor paints the square that was clicked.
	for (const Edge& e : edges) {
		const int wx = x + e.dx, wz = z + e.dz;
		if (map.IsWalkable(wx, wz)) continue;
		const u32 wallVariant = pick(map.WallVariant(wx, wz),
									  SurfaceVariantFor(wx, wz, 3u, wallVariants), wallVariants);
		const XMMATRIX m =
			XMMatrixRotationY(e.yaw) * XMMatrixTranslation(e.pos.x, e.pos.y, e.pos.z);
		// A niche on this edge stamps its recessed panel in place of the plain
		// wall panel (into the same variant bucket, so it keeps the wall's
		// texture); an unknown type or no niche keeps the normal wall block.
		const assets::MeshData* nicheMesh = nullptr;
		if (niche)
			// A closed (secret) niche renders as the plain wall until it is opened.
			if (const WallNiche* n = map.NicheAt(x, z, e.dx, e.dz); n && n->open)
				nicheMesh = niche(n->type);
		AppendTransformed(wallB[wallVariant],
						  nicheMesh ? *nicheMesh : wallBlocks[wallVariant], m);
	}
}

} // namespace

DungeonGeometry BuildDungeonRegion(const DungeonMap& map,
								   std::span<const assets::MeshData> wallBlocks,
								   std::span<const assets::MeshData> floorBlocks,
								   std::span<const assets::MeshData> ceilingBlocks,
								   int chunkX, int chunkZ,
								   const CellHolesFn& holes,
								   const NicheMeshFn& niche) {
	const int chunksX = (map.Width() + kChunkCells - 1) / kChunkCells;
	const int chunkIndex = chunkZ * chunksX + chunkX;
	const int x0 = chunkX * kChunkCells, z0 = chunkZ * kChunkCells;
	const int x1 = std::min(x0 + kChunkCells, map.Width());
	const int z1 = std::min(z0 + kChunkCells, map.Height());

	std::vector<assets::MeshData> wallB(wallBlocks.size());
	std::vector<assets::MeshData> floorB(floorBlocks.size());
	std::vector<assets::MeshData> ceilB(ceilingBlocks.size());
	for (int z = z0; z < z1; ++z)
		for (int x = x0; x < x1; ++x)
			if (map.IsWalkable(x, z))
				StampCell(map, x, z, holes ? holes(x, z) : CellHoles{}, wallBlocks,
						  floorBlocks, ceilingBlocks, niche, wallB, floorB, ceilB);

	DungeonGeometry geo;
	Collect(wallB, chunkIndex, geo.walls);
	Collect(floorB, chunkIndex, geo.floors);
	Collect(ceilB, chunkIndex, geo.ceilings);
	return geo;
}

DungeonGeometry BuildDungeonGeometry(const DungeonMap& map,
									 std::span<const assets::MeshData> wallBlocks,
									 std::span<const assets::MeshData> floorBlocks,
									 std::span<const assets::MeshData> ceilingBlocks,
									 const CellHolesFn& holes,
									 const NicheMeshFn& niche) {
	const int chunksX = (map.Width() + kChunkCells - 1) / kChunkCells;
	const int chunksZ = (map.Height() + kChunkCells - 1) / kChunkCells;
	DungeonGeometry geo;
	for (int cz = 0; cz < chunksZ; ++cz)
		for (int cx = 0; cx < chunksX; ++cx) {
			DungeonGeometry r = BuildDungeonRegion(map, wallBlocks, floorBlocks,
												   ceilingBlocks, cx, cz, holes, niche);
			for (GeometryChunk& c : r.walls) geo.walls.push_back(std::move(c));
			for (GeometryChunk& c : r.floors) geo.floors.push_back(std::move(c));
			for (GeometryChunk& c : r.ceilings) geo.ceilings.push_back(std::move(c));
		}
	return geo;
}

} // namespace dungeon::game
