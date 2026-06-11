#include "Game/DungeonMeshBuilder.h"

namespace dungeon::game {

namespace {

// Appends a quad (two triangles) given four corners in CCW order when viewed
// from the direction the normal points, with per-corner UVs.
void AddQuad(assets::MeshData& mesh, const Vec3& a, const Vec3& b, const Vec3& c,
             const Vec3& d, const Vec3& normal, float uScale, float vScale) {
    const u32 base = static_cast<u32>(mesh.vertices.size());
    const Vec2 uvs[4] = {{0, 0}, {uScale, 0}, {uScale, vScale}, {0, vScale}};
    const Vec3 corners[4] = {a, b, c, d};
    for (int i = 0; i < 4; ++i) {
        assets::Vertex v;
        v.position = corners[i];
        v.normal = normal;
        v.uv = uvs[i];
        mesh.vertices.push_back(v);
    }
    mesh.indices.insert(mesh.indices.end(),
                        {base, base + 1, base + 2, base, base + 2, base + 3});
}

} // namespace

DungeonGeometry BuildDungeonGeometry(const DungeonMap& map) {
    DungeonGeometry geo;
    const float s = kCellSize;
    const float h = kWallHeight;

    for (int z = 0; z < map.Height(); ++z) {
        for (int x = 0; x < map.Width(); ++x) {
            if (!map.IsWalkable(x, z)) continue;
            const float x0 = x * s, x1 = (x + 1) * s;
            const float z0 = z * s, z1 = (z + 1) * s;

            // Floor (normal up) and ceiling (normal down).
            AddQuad(geo.floors, {x0, 0, z0}, {x1, 0, z0}, {x1, 0, z1}, {x0, 0, z1},
                    {0, 1, 0}, 1, 1);
            AddQuad(geo.ceilings, {x0, h, z1}, {x1, h, z1}, {x1, h, z0}, {x0, h, z0},
                    {0, -1, 0}, 1, 1);

            // Walls on sides bordering solid cells; normals point into the room.
            if (!map.IsWalkable(x, z - 1)) // north wall (z0 edge)
                AddQuad(geo.walls, {x0, 0, z0}, {x1, 0, z0}, {x1, h, z0}, {x0, h, z0},
                        {0, 0, 1}, 1, h / s);
            if (!map.IsWalkable(x, z + 1)) // south wall (z1 edge)
                AddQuad(geo.walls, {x1, 0, z1}, {x0, 0, z1}, {x0, h, z1}, {x1, h, z1},
                        {0, 0, -1}, 1, h / s);
            if (!map.IsWalkable(x - 1, z)) // west wall (x0 edge)
                AddQuad(geo.walls, {x0, 0, z1}, {x0, 0, z0}, {x0, h, z0}, {x0, h, z1},
                        {1, 0, 0}, 1, h / s);
            if (!map.IsWalkable(x + 1, z)) // east wall (x1 edge)
                AddQuad(geo.walls, {x1, 0, z0}, {x1, 0, z1}, {x1, h, z1}, {x1, h, z0},
                        {-1, 0, 0}, 1, h / s);
        }
    }
    return geo;
}

} // namespace dungeon::game
