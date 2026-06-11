#include "Game/DungeonMeshBuilder.h"

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

DungeonGeometry BuildDungeonGeometry(const DungeonMap& map,
									 const assets::MeshData& wallBlock,
									 const assets::MeshData& floorBlock,
									 const assets::MeshData& ceilingBlock,
									 u32 wallVariants, u32 floorVariants,
									 u32 ceilingVariants) {
	DungeonGeometry geo;
	geo.walls.resize(std::max(wallVariants, 1u));
	geo.floors.resize(std::max(floorVariants, 1u));
	geo.ceilings.resize(std::max(ceilingVariants, 1u));

	for (int z = 0; z < map.Height(); ++z) {
		for (int x = 0; x < map.Width(); ++x) {
			if (!map.IsWalkable(x, z)) continue;
			const Vec3 center = map.CellCenter(x, z);

			AppendTransformed(geo.floors[VariantFor(x, z, 1u, floorVariants)],
							  floorBlock,
							  XMMatrixTranslation(center.x, 0, center.z));
			AppendTransformed(geo.ceilings[VariantFor(x, z, 2u, ceilingVariants)],
							  ceilingBlock,
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
			const u32 wallVariant = VariantFor(x, z, 3u, wallVariants);
			for (const Edge& e : edges) {
				if (map.IsWalkable(x + e.dx, z + e.dz)) continue;
				const XMMATRIX m = XMMatrixRotationY(e.yaw) *
								   XMMatrixTranslation(e.pos.x, e.pos.y, e.pos.z);
				AppendTransformed(geo.walls[wallVariant], wallBlock, m);
			}
		}
	}
	return geo;
}

} // namespace dungeon::game
