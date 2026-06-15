#include "Game/DungeonMeshBuilder.h"

#include <algorithm>
#include <utility>

using namespace DirectX;

namespace dungeon::game {

namespace {

// Stable per-cell variant choice so the dungeon looks the same every run.
u32 VariantFor(int x, int z, u32 salt, u32 count) {
	u32 h = static_cast<u32>(x) * 73856093u ^ static_cast<u32>(z) * 19349663u ^
			salt * 83492791u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return count > 0 ? (h >> 8) % count : 0;
}

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

// Collects the non-empty (chunk, variant) buckets into cullable GeometryChunks,
// computing each one's world AABB from its vertices.
void Collect(std::vector<assets::MeshData>& buckets, u32 variants,
			 std::vector<GeometryChunk>& out) {
	for (size_t b = 0; b < buckets.size(); ++b) {
		assets::MeshData& mesh = buckets[b];
		if (mesh.vertices.empty()) continue;
		GeometryChunk chunk;
		chunk.variant = static_cast<int>(b % variants);
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

} // namespace

DungeonGeometry BuildDungeonGeometry(const DungeonMap& map,
									 std::span<const assets::MeshData> wallBlocks,
									 std::span<const assets::MeshData> floorBlocks,
									 std::span<const assets::MeshData> ceilingBlocks) {
	const u32 wallVariants = static_cast<u32>(wallBlocks.size());
	const u32 floorVariants = static_cast<u32>(floorBlocks.size());
	const u32 ceilingVariants = static_cast<u32>(ceilingBlocks.size());

	const int chunksX = (map.Width() + kChunkCells - 1) / kChunkCells;
	const int chunksZ = (map.Height() + kChunkCells - 1) / kChunkCells;
	const int chunkCount = chunksX * chunksZ;
	const auto chunkOf = [&](int x, int z) {
		return (z / kChunkCells) * chunksX + (x / kChunkCells);
	};

	// Temp buckets indexed [chunk * variants + variant]; collected at the end.
	std::vector<assets::MeshData> wallB(chunkCount * wallVariants);
	std::vector<assets::MeshData> floorB(chunkCount * floorVariants);
	std::vector<assets::MeshData> ceilB(chunkCount * ceilingVariants);

	for (int z = 0; z < map.Height(); ++z) {
		for (int x = 0; x < map.Width(); ++x) {
			if (!map.IsWalkable(x, z)) continue;
			const Vec3 center = map.CellCenter(x, z);
			const int chunk = chunkOf(x, z);

			// An editor override (>= 0) pins the cell's variant; otherwise the
			// stable position hash chooses it. Clamp to the loaded variant count.
			const auto pick = [](int over, u32 hashed, u32 count) -> u32 {
				if (over < 0) return hashed;
				return count > 0 ? std::min(static_cast<u32>(over), count - 1) : 0u;
			};
			const u32 floorVariant = pick(map.FloorVariant(x, z),
										   VariantFor(x, z, 1u, floorVariants), floorVariants);
			AppendTransformed(floorB[chunk * floorVariants + floorVariant],
							  floorBlocks[floorVariant],
							  XMMatrixTranslation(center.x, 0, center.z));
			const u32 ceilingVariant = pick(map.CeilingVariant(x, z),
											 VariantFor(x, z, 2u, ceilingVariants),
											 ceilingVariants);
			AppendTransformed(ceilB[chunk * ceilingVariants + ceilingVariant],
							  ceilingBlocks[ceilingVariant],
							  XMMatrixTranslation(center.x, kWallHeight, center.z));

			// Wall blocks are authored facing +Z; rotate so the face points
			// into the room. Camera convention: forward = (sin yaw, 0, cos yaw).
			struct Edge {
				int dx, dz;
				float yaw;
				Vec3 pos;
			};
			const float s = kCellSize;
			const Edge edges[4] = {
				{0, -1, 0.0f, {center.x, 0, z * s}},                 // north
				{0, +1, kPi, {center.x, 0, (z + 1) * s}},            // south
				{-1, 0, kPi * 0.5f, {x * s, 0, center.z}},           // west
				{+1, 0, -kPi * 0.5f, {(x + 1) * s, 0, center.z}},    // east
			};
			const u32 wallVariant = pick(map.WallVariant(x, z),
										  VariantFor(x, z, 3u, wallVariants), wallVariants);
			for (const Edge& e : edges) {
				if (map.IsWalkable(x + e.dx, z + e.dz)) continue;
				const XMMATRIX m = XMMatrixRotationY(e.yaw) *
								   XMMatrixTranslation(e.pos.x, e.pos.y, e.pos.z);
				AppendTransformed(wallB[chunk * wallVariants + wallVariant],
								  wallBlocks[wallVariant], m);
			}
		}
	}

	DungeonGeometry geo;
	Collect(wallB, wallVariants, geo.walls);
	Collect(floorB, floorVariants, geo.floors);
	Collect(ceilB, ceilingVariants, geo.ceilings);
	return geo;
}

} // namespace dungeon::game
