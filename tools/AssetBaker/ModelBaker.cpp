// ============================================================================
// ModelBaker.cpp — procedural model construction.
//
// Builds assets::ModelData in code and hands it to WriteGltf:
//   * dungeon blocks — wall (recessed panel + edge pillars, authored facing
//     +Z over x∈[-1,1], y∈[0,2.5]), flat floor, flat ceiling (facing down,
//     placed at wall height by the game)
//   * serpent pillar — skinned cylinder, 4-joint chain, looping sway clip
//   * monsters — skeleton & mummy share a 7-joint humanoid rig (root→spine→
//     head/arms, root→legs) with box limbs and an idle clip; the blob is a
//     2-joint sphere with a squash-and-stretch scale clip
//
// Conventions: joints are emitted parent-before-child; inverse binds are
// pure translations (-joint global position); rigid box limbs weight fully
// to one joint, while cylinders/spheres blend between neighboring joints.
// ============================================================================
#include "ModelBaker.h"

#include "Core/MathTypes.h"
#include "GltfWriter.h"
#include "Noise.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace DirectX;

namespace dungeon::baker {

namespace {

// --- mesh construction helpers ----------------------------------------------

void AddQuad(assets::MeshData& mesh, const Vec3& a, const Vec3& b, const Vec3& c,
			 const Vec3& d, const Vec3& normal, const Vec2& uvA, const Vec2& uvB,
			 const Vec2& uvC, const Vec2& uvD, int joint = -1) {
	const u32 base = static_cast<u32>(mesh.vertices.size());
	const Vec3 corners[4] = {a, b, c, d};
	const Vec2 uvs[4] = {uvA, uvB, uvC, uvD};
	for (int i = 0; i < 4; ++i) {
		assets::Vertex v;
		v.position = corners[i];
		v.normal = normal;
		v.uv = uvs[i];
		if (joint >= 0) {
			v.joints[0] = static_cast<u32>(joint);
			v.weights[0] = 1.0f;
		}
		mesh.vertices.push_back(v);
	}
	mesh.indices.insert(mesh.indices.end(),
						{base, base + 1, base + 2, base, base + 2, base + 3});
}

// Axis-aligned box with per-face normals; rigidly bound to `joint` if >= 0.
void AddBox(assets::MeshData& mesh, const Vec3& center, const Vec3& halfExtents,
			int joint = -1) {
	const float x0 = center.x - halfExtents.x, x1 = center.x + halfExtents.x;
	const float y0 = center.y - halfExtents.y, y1 = center.y + halfExtents.y;
	const float z0 = center.z - halfExtents.z, z1 = center.z + halfExtents.z;
	const Vec2 u0{0, 0}, u1{1, 0}, u2{1, 1}, u3{0, 1};
	AddQuad(mesh, {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, {0, 0, 1},
			u0, u1, u2, u3, joint); // front
	AddQuad(mesh, {x1, y0, z0}, {x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {0, 0, -1},
			u0, u1, u2, u3, joint); // back
	AddQuad(mesh, {x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, {-1, 0, 0},
			u0, u1, u2, u3, joint); // left
	AddQuad(mesh, {x1, y0, z1}, {x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {1, 0, 0},
			u0, u1, u2, u3, joint); // right
	AddQuad(mesh, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {x0, y1, z0}, {0, 1, 0},
			u0, u1, u2, u3, joint); // top
	AddQuad(mesh, {x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, {0, -1, 0},
			u0, u1, u2, u3, joint); // bottom
}

Mat4 InverseBindForGlobal(const Vec3& globalPos) {
	Mat4 m = Mat4Identity();
	m._41 = -globalPos.x;
	m._42 = -globalPos.y;
	m._43 = -globalPos.z;
	return m;
}

Quat QuatFromEuler(float pitch, float yaw, float roll) {
	Quat q;
	XMStoreFloat4(&q, XMQuaternionRotationRollPitchYaw(pitch, yaw, roll));
	return q;
}

// --- dungeon blocks --------------------------------------------------------------
// Wall block: authored facing +Z (the room side), x in [-1,1], y in [0,2.5].
// A recessed center panel framed by edge pillars gives real 3D relief that
// the parallax-mapped textures then deepen.

constexpr float kWallH = 2.5f;

// Planar UV projection chosen by the face normal's dominant axis, with a
// consistent texel scale (one texture tile per 2 world units). Faces that
// point sideways or up/down (panel reveals, pillar flanks) get their own
// in-plane projection instead of a smeared front projection.
Vec2 WallFaceUv(const Vec3& p, const Vec3& n) {
	const float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
	if (az >= ax && az >= ay) return {(p.x + 1.0f) * 0.5f, (kWallH - p.y) * 0.5f};
	if (ay >= ax) return {(p.x + 1.0f) * 0.5f, (p.z + 1.0f) * 0.5f};
	return {(p.z + 1.0f) * 0.5f, (kWallH - p.y) * 0.5f};
}

constexpr float kPanelX = 0.80f;     // panel half-width (between pillars)
constexpr float kPillarOut = 0.085f; // pillar protrusion

// Edge pillars plus the corner seals (outer cap + wall-plane backing strip).
// Shared by the clean and worn wall blocks so both stay watertight at convex
// corners — see the corner-notch fix notes on the cap below.
void AddWallPillars(assets::MeshData& mesh) {
	auto wq = [&](const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
				  const Vec3& n) {
		AddQuad(mesh, a, b, c, d, n, WallFaceUv(a, n), WallFaceUv(b, n),
				WallFaceUv(c, n), WallFaceUv(d, n));
	};

	for (const float side : {-1.0f, 1.0f}) {
		const float cx = side * (1.0f - (1.0f - kPanelX) * 0.5f);
		const float hw = (1.0f - kPanelX) * 0.5f;
		const float x0 = cx - hw, x1 = cx + hw;

		// Front face.
		wq({x0, 0, kPillarOut}, {x1, 0, kPillarOut}, {x1, kWallH, kPillarOut},
		   {x0, kWallH, kPillarOut}, {0, 0, 1});

		// Inner side, toward the panel.
		const float inner = side < 0 ? x1 : x0;
		const Vec3 n = side < 0 ? Vec3{1, 0, 0} : Vec3{-1, 0, 0};
		wq({inner, 0, side < 0 ? kPillarOut : 0.0f}, {inner, 0, side < 0 ? 0.0f : kPillarOut},
		   {inner, kWallH, side < 0 ? 0.0f : kPillarOut},
		   {inner, kWallH, side < 0 ? kPillarOut : 0.0f}, n);

		// Outer side cap at the block edge. Along a straight wall the next
		// block's pillar hides it, but at an outside corner of a solid block
		// nothing else covers this strip — without it there is a see-through
		// notch at every convex wall corner.
		const float outer = side < 0 ? x0 : x1; // == ±1
		wq({outer, 0, 0}, {outer, 0, kPillarOut}, {outer, kWallH, kPillarOut},
		   {outer, kWallH, 0}, {side, 0, 0});

		// Backing strip on the wall plane behind the pillar, sealing the
		// gap between the panel borders (|x| <= panelX) and the block edge.
		wq({x0, 0, 0}, {x1, 0, 0}, {x1, kWallH, 0}, {x0, kWallH, 0}, {0, 0, 1});
	}
}

assets::ModelData BuildWallBlock() {
	assets::ModelData model;
	assets::MeshData mesh;

	const float panelZ = -0.10f;     // recess depth
	const float panelX = kPanelX;
	const float borderY0 = 0.14f, borderY1 = kWallH - 0.14f;

	auto wq = [&](const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
				  const Vec3& n) {
		AddQuad(mesh, a, b, c, d, n, WallFaceUv(a, n), WallFaceUv(b, n),
				WallFaceUv(c, n), WallFaceUv(d, n));
	};

	// Recessed center panel.
	wq({-panelX, borderY0, panelZ}, {panelX, borderY0, panelZ},
	   {panelX, borderY1, panelZ}, {-panelX, borderY1, panelZ}, {0, 0, 1});
	// Flush top/bottom border strips.
	wq({-panelX, 0, 0}, {panelX, 0, 0}, {panelX, borderY0, 0}, {-panelX, borderY0, 0},
	   {0, 0, 1});
	wq({-panelX, borderY1, 0}, {panelX, borderY1, 0}, {panelX, kWallH, 0},
	   {-panelX, kWallH, 0}, {0, 0, 1});
	// Reveals connecting the borders to the recessed panel.
	wq({-panelX, borderY0, 0}, {panelX, borderY0, 0}, {panelX, borderY0, panelZ},
	   {-panelX, borderY0, panelZ}, {0, 1, 0});
	wq({-panelX, borderY1, panelZ}, {panelX, borderY1, panelZ}, {panelX, borderY1, 0},
	   {-panelX, borderY1, 0}, {0, -1, 0});
	wq({-panelX, borderY0, panelZ}, {-panelX, borderY1, panelZ}, {-panelX, borderY1, 0},
	   {-panelX, borderY0, 0}, {1, 0, 0});
	wq({panelX, borderY0, 0}, {panelX, borderY1, 0}, {panelX, borderY1, panelZ},
	   {panelX, borderY0, panelZ}, {-1, 0, 0});

	// Edge pillars + corner seals (shared with the worn wall).
	AddWallPillars(mesh);

	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{1, 1, 1, 1}, -1});
	return model;
}

assets::ModelData BuildFloorBlock() {
	assets::ModelData model;
	assets::MeshData mesh;
	AddQuad(mesh, {-1, 0, -1}, {1, 0, -1}, {1, 0, 1}, {-1, 0, 1}, {0, 1, 0}, {0, 0},
			{1, 0}, {1, 1}, {0, 1});
	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{1, 1, 1, 1}, -1});
	return model;
}

assets::ModelData BuildCeilingBlock() {
	// Authored at y=0 facing down; placed at wall height by the game.
	assets::ModelData model;
	assets::MeshData mesh;
	AddQuad(mesh, {-1, 0, 1}, {1, 0, 1}, {1, 0, -1}, {-1, 0, -1}, {0, -1, 0}, {0, 0},
			{1, 0}, {1, 1}, {0, 1});
	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{1, 1, 1, 1}, -1});
	return model;
}

// --- worn dungeon blocks -----------------------------------------------------------
// A second block set with real displaced geometry for old, crumbling areas:
// tessellated grids whose vertices are pushed by brick/slab-aware wear noise,
// with normals derived analytically from the displacement gradient. All
// displacement is pinned to zero at block edges so adjacent cells (and the
// clean set, if mixed) always meet watertight. The clean blocks remain for
// newer, well-kept areas of the dungeon.

// Smooth 0→1 ramp within `width` of a boundary at 0.
float PinRamp(float distance, float width) {
	const float t = std::clamp(distance / width, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

// Erosion depth (into the rock) for the worn wall surface, in wall-local
// coordinates (x across [-1,1], y up [0,kWallH]).
float WallWearDepth(float x, float y) {
	const float u = x + 1.0f;       // 0..2 along the wall
	const float v = kWallH - y;     // 0..2.5 down the wall

	// Brick grid matching the texture proportions (0.5 x 0.3125).
	const float bw = 0.50f, bh = 0.3125f;
	const u32 row = static_cast<u32>(v / bh);
	const float us = u + (row % 2 ? bw * 0.5f : 0.0f);
	const u32 col = static_cast<u32>(us / bw);
	const float bx = std::fmod(us, bw), by = std::fmod(v, bh);

	// Eroded mortar joints.
	const float joint = std::min({bx, bw - bx, by, bh - by});
	const float mortar = std::clamp(1.0f - joint / 0.05f, 0.0f, 1.0f) * 0.035f;

	// Some bricks are recessed or broken; most just vary slightly.
	const float r = Hash(col, row, 101u);
	const float brick = r > 0.78f ? (r - 0.78f) * 0.32f : r * 0.02f;

	// Large undulation (bowed masonry) + fine roughness + ground-level wear.
	const float undulation = (Fbm(u * 0.9f, v * 0.9f, 103u) - 0.5f) * 0.05f;
	const float rough = (Fbm(u * 4.0f, v * 4.0f, 105u) - 0.5f) * 0.022f;
	const float low = std::clamp(1.0f - y, 0.0f, 1.0f) * 0.02f;

	const float depth = mortar + brick + undulation + rough + low;
	// Pin to the flat plane at every block edge so seams stay closed.
	const float pin = PinRamp(1.0f - std::fabs(x), 0.12f) * PinRamp(y, 0.10f) *
					  PinRamp(kWallH - y, 0.10f);
	return std::clamp(depth, 0.0f, 0.12f) * pin;
}

// Height offset for the worn floor: sunken, tilted slabs with eroded joints.
float FloorWearHeight(float x, float z) {
	const float u = x + 1.0f, v = z + 1.0f; // 0..2, slabs are 1m
	const u32 col = static_cast<u32>(u), row = static_cast<u32>(v);

	const float sink = -Hash(col + 17u, row + 9u, 201u) * 0.035f;
	const float tiltX = (Hash(col, row, 203u) - 0.5f) * 0.06f;
	const float tiltZ = (Hash(col, row, 205u) - 0.5f) * 0.06f;
	const float lx = std::fmod(u, 1.0f) - 0.5f, lz = std::fmod(v, 1.0f) - 0.5f;
	float h = sink + tiltX * lx + tiltZ * lz;

	// Joint gaps between slabs + general unevenness.
	const float ju = std::min(std::fmod(u, 1.0f), 1.0f - std::fmod(u, 1.0f));
	const float jv = std::min(std::fmod(v, 1.0f), 1.0f - std::fmod(v, 1.0f));
	h -= std::clamp(1.0f - std::min(ju, jv) / 0.06f, 0.0f, 1.0f) * 0.02f;
	h += (Fbm(u * 2.2f, v * 2.2f, 207u) - 0.5f) * 0.03f;

	const float pin = PinRamp(1.0f - std::fabs(x), 0.10f) * PinRamp(1.0f - std::fabs(z), 0.10f);
	return std::clamp(h, -0.07f, 0.035f) * pin;
}

// Erosion pockets (upward, into the rock) for the worn ceiling.
float CeilingWearDepth(float x, float z) {
	const float u = x + 1.0f, v = z + 1.0f;
	float d = std::max(0.0f, Fbm(u * 1.5f, v * 1.5f, 301u) - 0.42f) * 0.20f;
	d += (Fbm(u * 4.0f, v * 4.0f, 303u) - 0.5f) * 0.015f;
	const float pin = PinRamp(1.0f - std::fabs(x), 0.10f) * PinRamp(1.0f - std::fabs(z), 0.10f);
	return std::clamp(d, 0.0f, 0.10f) * pin;
}

assets::ModelData BuildWornWallBlock() {
	assets::ModelData model;
	assets::MeshData mesh;

	// Displaced surface replacing the clean block's panel/border relief.
	constexpr int kNx = 28, kNy = 36;
	constexpr float kEps = 0.02f; // finite-difference step for normals
	for (int j = 0; j <= kNy; ++j) {
		const float y = kWallH * static_cast<float>(j) / kNy;
		for (int i = 0; i <= kNx; ++i) {
			const float x = -1.0f + 2.0f * static_cast<float>(i) / kNx;
			const float d = WallWearDepth(x, y);
			// Surface z = -d; tangent cross product gives (dd/dx, dd/dy, 1).
			const float ddx = (WallWearDepth(x + kEps, y) - WallWearDepth(x - kEps, y)) / (2 * kEps);
			const float ddy = (WallWearDepth(x, y + kEps) - WallWearDepth(x, y - kEps)) / (2 * kEps);
			const float inv = 1.0f / std::sqrt(ddx * ddx + ddy * ddy + 1.0f);

			assets::Vertex vert;
			vert.position = {x, y, -d};
			vert.normal = {ddx * inv, ddy * inv, inv};
			vert.uv = {(x + 1.0f) * 0.5f, (kWallH - y) * 0.5f};
			mesh.vertices.push_back(vert);
		}
	}
	const u32 stride = kNx + 1;
	for (u32 j = 0; j < kNy; ++j)
		for (u32 i = 0; i < kNx; ++i) {
			const u32 a = j * stride + i, b = a + 1, c = a + stride, d2 = c + 1;
			mesh.indices.insert(mesh.indices.end(), {a, b, d2, a, d2, c});
		}

	AddWallPillars(mesh);
	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{1, 1, 1, 1}, -1});
	return model;
}

assets::ModelData BuildWornFloorBlock() {
	assets::ModelData model;
	assets::MeshData mesh;

	constexpr int kN = 28;
	constexpr float kEps = 0.02f;
	for (int j = 0; j <= kN; ++j) {
		const float z = -1.0f + 2.0f * static_cast<float>(j) / kN;
		for (int i = 0; i <= kN; ++i) {
			const float x = -1.0f + 2.0f * static_cast<float>(i) / kN;
			const float h = FloorWearHeight(x, z);
			const float hx = (FloorWearHeight(x + kEps, z) - FloorWearHeight(x - kEps, z)) / (2 * kEps);
			const float hz = (FloorWearHeight(x, z + kEps) - FloorWearHeight(x, z - kEps)) / (2 * kEps);
			const float inv = 1.0f / std::sqrt(hx * hx + hz * hz + 1.0f);

			assets::Vertex vert;
			vert.position = {x, h, z};
			vert.normal = {-hx * inv, inv, -hz * inv};
			vert.uv = {(x + 1.0f) * 0.5f, (z + 1.0f) * 0.5f};
			mesh.vertices.push_back(vert);
		}
	}
	const u32 stride = kN + 1;
	for (u32 j = 0; j < kN; ++j)
		for (u32 i = 0; i < kN; ++i) {
			const u32 a = j * stride + i, b = a + 1, c = a + stride, d2 = c + 1;
			mesh.indices.insert(mesh.indices.end(), {a, d2, b, a, c, d2});
		}

	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{1, 1, 1, 1}, -1});
	return model;
}

assets::ModelData BuildWornCeilingBlock() {
	assets::ModelData model;
	assets::MeshData mesh;

	// Authored at y=0 facing down (like the clean block); erosion goes up.
	constexpr int kN = 24;
	constexpr float kEps = 0.02f;
	for (int j = 0; j <= kN; ++j) {
		const float z = -1.0f + 2.0f * static_cast<float>(j) / kN;
		for (int i = 0; i <= kN; ++i) {
			const float x = -1.0f + 2.0f * static_cast<float>(i) / kN;
			const float d = CeilingWearDepth(x, z);
			const float dx = (CeilingWearDepth(x + kEps, z) - CeilingWearDepth(x - kEps, z)) / (2 * kEps);
			const float dz = (CeilingWearDepth(x, z + kEps) - CeilingWearDepth(x, z - kEps)) / (2 * kEps);
			const float inv = 1.0f / std::sqrt(dx * dx + dz * dz + 1.0f);

			assets::Vertex vert;
			vert.position = {x, d, z};
			vert.normal = {dx * inv, -inv, dz * inv};
			vert.uv = {(x + 1.0f) * 0.5f, (z + 1.0f) * 0.5f};
			mesh.vertices.push_back(vert);
		}
	}
	const u32 stride = kN + 1;
	for (u32 j = 0; j < kN; ++j)
		for (u32 i = 0; i < kN; ++i) {
			const u32 a = j * stride + i, b = a + 1, c = a + stride, d2 = c + 1;
			mesh.indices.insert(mesh.indices.end(), {a, b, d2, a, d2, c});
		}

	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{1, 1, 1, 1}, -1});
	return model;
}

// --- fire props --------------------------------------------------------------------
// Iron sconce (wall torch holder) and floor brazier. Both are simple box
// assemblies — the drama comes from the particle flames and the point light
// the game attaches at the flame origin (sconce: local (0, 1.78, 0.22);
// brazier: local (0, 0.72, 0)).

assets::ModelData BuildSconce() {
	// Authored against a wall at z=0, arm reaching into the room (+Z).
	assets::ModelData model;
	assets::MeshData mesh;
	AddBox(mesh, {0, 1.45f, 0.02f}, {0.07f, 0.16f, 0.02f});   // mounting plate
	AddBox(mesh, {0, 1.52f, 0.12f}, {0.025f, 0.025f, 0.10f}); // arm
	AddBox(mesh, {0, 1.58f, 0.22f}, {0.06f, 0.05f, 0.06f});   // cup
	AddBox(mesh, {0, 1.70f, 0.22f}, {0.022f, 0.10f, 0.022f}); // torch shaft
	mesh.material = 0;
	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{0.35f, 0.32f, 0.30f, 1.0f}, -1}); // dark iron
	return model;
}

assets::ModelData BuildBrazier() {
	assets::ModelData model;
	assets::MeshData mesh;
	AddBox(mesh, {0, 0.05f, 0}, {0.26f, 0.05f, 0.26f}); // base slab
	AddBox(mesh, {0, 0.25f, 0}, {0.07f, 0.16f, 0.07f}); // stem
	AddBox(mesh, {0, 0.47f, 0}, {0.20f, 0.07f, 0.20f}); // bowl underside
	AddBox(mesh, {0, 0.58f, 0}, {0.26f, 0.06f, 0.26f}); // bowl rim
	AddBox(mesh, {0, 0.65f, 0}, {0.18f, 0.025f, 0.18f}); // coal bed
	mesh.material = 0;
	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{0.38f, 0.33f, 0.29f, 1.0f}, -1}); // bronzed iron
	return model;
}

// --- serpent pillar (moved from the old in-game AnimatedProp) ---------------------

assets::ModelData BuildSerpentPillar() {
	constexpr int kJoints = 4;
	constexpr float kSegment = 0.55f;
	constexpr float kRadius = 0.14f;
	constexpr int kRadial = 10, kRings = 16;
	constexpr float kTotal = kSegment * kJoints;

	assets::ModelData model;
	for (int j = 0; j < kJoints; ++j) {
		assets::JointData joint;
		joint.name = "spine" + std::to_string(j);
		joint.parent = j - 1;
		joint.restTranslation = {0, j == 0 ? 0.0f : kSegment, 0};
		joint.inverseBind = InverseBindForGlobal({0, kSegment * j, 0});
		model.skeleton.joints.push_back(joint);
	}

	assets::MeshData mesh;
	mesh.skinned = true;
	for (int ring = 0; ring <= kRings; ++ring) {
		const float v = static_cast<float>(ring) / kRings;
		const float y = v * kTotal;
		const float radius = kRadius * (1.0f - 0.35f * v);
		const float jointPos = v * (kJoints - 1);
		const u32 j0 = static_cast<u32>(jointPos);
		const u32 j1 = std::min(j0 + 1, static_cast<u32>(kJoints - 1));
		const float w1 = jointPos - static_cast<float>(j0);
		for (int seg = 0; seg <= kRadial; ++seg) {
			const float a = static_cast<float>(seg) / kRadial * 2.0f * kPi;
			assets::Vertex vert;
			vert.position = {std::cos(a) * radius, y, std::sin(a) * radius};
			vert.normal = {std::cos(a), 0, std::sin(a)};
			vert.uv = {static_cast<float>(seg) / kRadial, v * 4.0f};
			vert.joints[0] = j0;
			vert.joints[1] = j1;
			vert.weights[0] = 1.0f - w1;
			vert.weights[1] = w1;
			mesh.vertices.push_back(vert);
		}
	}
	const u32 stride = kRadial + 1;
	for (u32 ring = 0; ring < kRings; ++ring)
		for (u32 seg = 0; seg < kRadial; ++seg) {
			const u32 a = ring * stride + seg, b = a + 1, c = a + stride, d = c + 1;
			mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
		}
	mesh.material = 0;
	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{0.35f, 0.85f, 0.65f, 1.0f}, -1});

	assets::AnimationClipData clip;
	clip.name = "sway";
	clip.duration = 4.0f;
	constexpr int kKeys = 33;
	for (int j = 1; j < kJoints; ++j) {
		assets::AnimationChannelData channel;
		channel.joint = j;
		channel.path = assets::ChannelPath::Rotation;
		for (int k = 0; k < kKeys; ++k) {
			const float t = clip.duration * static_cast<float>(k) / (kKeys - 1);
			channel.times.push_back(t);
			const float phase = t / clip.duration * 2.0f * kPi;
			const Quat q = QuatFromEuler(std::cos(phase * 2.0f + j * 0.5f) * 0.10f, 0,
										 std::sin(phase + j * 0.9f) * 0.22f);
			channel.values.push_back({q.x, q.y, q.z, q.w});
		}
		clip.channels.push_back(std::move(channel));
	}
	model.clips.push_back(std::move(clip));
	return model;
}

// --- monsters --------------------------------------------------------------------
// Humanoid rig shared by skeleton and mummy:
//   0 root (hips) -> 1 spine -> 2 head, 3 armL, 4 armR; root -> 5 legL, 6 legR

struct HumanoidStyle {
	Vec4 color;
	float bulk;       // limb thickness multiplier
	float duration;   // idle clip length
	float armRaise;   // base forward arm pitch (mummy shamble pose)
	float swing;      // arm swing amplitude
};

assets::ModelData BuildHumanoid(const HumanoidStyle& style) {
	assets::ModelData model;

	const Vec3 globals[7] = {{0, 1.00f, 0}, {0, 1.30f, 0},      {0, 1.70f, 0},
							 {-0.28f, 1.55f, 0}, {0.28f, 1.55f, 0},
							 {-0.12f, 0.95f, 0}, {0.12f, 0.95f, 0}};
	const int parents[7] = {-1, 0, 1, 1, 1, 0, 0};
	const char* names[7] = {"root", "spine", "head", "armL", "armR", "legL", "legR"};
	for (int j = 0; j < 7; ++j) {
		assets::JointData joint;
		joint.name = names[j];
		joint.parent = parents[j];
		const Vec3 parentPos = parents[j] >= 0 ? globals[parents[j]] : Vec3{0, 0, 0};
		joint.restTranslation = Sub(globals[j], parentPos);
		joint.inverseBind = InverseBindForGlobal(globals[j]);
		model.skeleton.joints.push_back(joint);
	}

	const float b = style.bulk;
	assets::MeshData mesh;
	mesh.skinned = true;
	AddBox(mesh, {0, 1.02f, 0}, {0.16f * b, 0.06f, 0.085f * b}, 0);        // pelvis
	AddBox(mesh, {0, 1.40f, 0}, {0.17f * b, 0.15f, 0.085f * b}, 1);        // torso
	AddBox(mesh, {0, 1.80f, 0}, {0.095f * b, 0.11f, 0.095f * b}, 2);       // head
	AddBox(mesh, {-0.33f, 1.30f, 0}, {0.05f * b, 0.26f, 0.05f * b}, 3);    // armL
	AddBox(mesh, {0.33f, 1.30f, 0}, {0.05f * b, 0.26f, 0.05f * b}, 4);     // armR
	AddBox(mesh, {-0.12f, 0.50f, 0}, {0.06f * b, 0.45f, 0.06f * b}, 5);    // legL
	AddBox(mesh, {0.12f, 0.50f, 0}, {0.06f * b, 0.45f, 0.06f * b}, 6);     // legR
	mesh.material = 0;
	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({style.color, -1});

	// Idle clip: breathing bob, head scan, arm sway around the base pose.
	assets::AnimationClipData clip;
	clip.name = "idle";
	clip.duration = style.duration;
	constexpr int kKeys = 25;
	auto times = [&](assets::AnimationChannelData& ch) {
		for (int k = 0; k < kKeys; ++k)
			ch.times.push_back(clip.duration * static_cast<float>(k) / (kKeys - 1));
	};
	auto phase = [&](int k) {
		return 2.0f * kPi * static_cast<float>(k) / (kKeys - 1);
	};

	{ // root bob (translation, around rest height 1.0)
		assets::AnimationChannelData ch;
		ch.joint = 0;
		ch.path = assets::ChannelPath::Translation;
		times(ch);
		for (int k = 0; k < kKeys; ++k)
			ch.values.push_back({0, 1.0f + 0.025f * std::sin(phase(k) * 2.0f), 0, 0});
		clip.channels.push_back(std::move(ch));
	}
	{ // spine sway
		assets::AnimationChannelData ch;
		ch.joint = 1;
		ch.path = assets::ChannelPath::Rotation;
		times(ch);
		for (int k = 0; k < kKeys; ++k) {
			const Quat q = QuatFromEuler(style.armRaise * 0.08f, 0,
										 0.05f * std::sin(phase(k)));
			ch.values.push_back({q.x, q.y, q.z, q.w});
		}
		clip.channels.push_back(std::move(ch));
	}
	{ // head scan
		assets::AnimationChannelData ch;
		ch.joint = 2;
		ch.path = assets::ChannelPath::Rotation;
		times(ch);
		for (int k = 0; k < kKeys; ++k) {
			const Quat q = QuatFromEuler(0, 0.18f * std::sin(phase(k) + 0.7f), 0);
			ch.values.push_back({q.x, q.y, q.z, q.w});
		}
		clip.channels.push_back(std::move(ch));
	}
	for (int arm = 3; arm <= 4; ++arm) { // arm sway (anti-phase)
		assets::AnimationChannelData ch;
		ch.joint = arm;
		ch.path = assets::ChannelPath::Rotation;
		times(ch);
		const float sign = arm == 3 ? 1.0f : -1.0f;
		for (int k = 0; k < kKeys; ++k) {
			const Quat q = QuatFromEuler(
				-style.armRaise + sign * style.swing * std::sin(phase(k)), 0, 0);
			ch.values.push_back({q.x, q.y, q.z, q.w});
		}
		clip.channels.push_back(std::move(ch));
	}
	model.clips.push_back(std::move(clip));
	return model;
}

assets::ModelData BuildBlob() {
	assets::ModelData model;

	const Vec3 globals[2] = {{0, 0.18f, 0}, {0, 0.63f, 0}};
	for (int j = 0; j < 2; ++j) {
		assets::JointData joint;
		joint.name = j == 0 ? "base" : "top";
		joint.parent = j - 1;
		joint.restTranslation = j == 0 ? globals[0] : Sub(globals[1], globals[0]);
		joint.inverseBind = InverseBindForGlobal(globals[j]);
		model.skeleton.joints.push_back(joint);
	}

	// Sphere sitting on the floor, weights blending base->top by height.
	constexpr float kRadius = 0.42f;
	constexpr int kLat = 12, kLon = 16;
	assets::MeshData mesh;
	mesh.skinned = true;
	for (int lat = 0; lat <= kLat; ++lat) {
		const float theta = kPi * static_cast<float>(lat) / kLat; // 0 = top pole
		for (int lon = 0; lon <= kLon; ++lon) {
			const float phi = 2.0f * kPi * static_cast<float>(lon) / kLon;
			const Vec3 n{std::sin(theta) * std::cos(phi), std::cos(theta),
						 std::sin(theta) * std::sin(phi)};
			assets::Vertex v;
			v.position = {n.x * kRadius, kRadius + n.y * kRadius, n.z * kRadius};
			v.normal = n;
			v.uv = {static_cast<float>(lon) / kLon, static_cast<float>(lat) / kLat};
			const float w1 =
				std::clamp((v.position.y - 0.15f) / 0.5f, 0.0f, 1.0f); // top weight
			v.joints[0] = 0;
			v.joints[1] = 1;
			v.weights[0] = 1.0f - w1;
			v.weights[1] = w1;
			mesh.vertices.push_back(v);
		}
	}
	const u32 stride = kLon + 1;
	for (u32 lat = 0; lat < kLat; ++lat)
		for (u32 lon = 0; lon < kLon; ++lon) {
			const u32 a = lat * stride + lon, b = a + 1, c = a + stride, d = c + 1;
			mesh.indices.insert(mesh.indices.end(), {a, b, c, b, d, c});
		}
	mesh.material = 0;
	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{0.38f, 0.78f, 0.32f, 1.0f}, -1});

	// Squash-and-stretch wobble.
	assets::AnimationClipData clip;
	clip.name = "idle";
	clip.duration = 2.8f;
	constexpr int kKeys = 25;
	for (int j = 0; j < 2; ++j) {
		assets::AnimationChannelData ch;
		ch.joint = j;
		ch.path = assets::ChannelPath::Scale;
		for (int k = 0; k < kKeys; ++k) {
			const float t = clip.duration * static_cast<float>(k) / (kKeys - 1);
			ch.times.push_back(t);
			const float s =
				std::sin(2.0f * kPi * t / clip.duration + (j == 0 ? 0.0f : 0.9f));
			ch.values.push_back({1.0f + 0.09f * s, 1.0f - 0.11f * s, 1.0f + 0.09f * s, 0});
		}
		clip.channels.push_back(std::move(ch));
	}
	model.clips.push_back(std::move(clip));
	return model;
}

} // namespace

bool BakeModels(const std::string& dir) {
	bool ok = true;
	ok &= WriteGltf(BuildWallBlock(), dir + "\\wall_block.gltf");
	ok &= WriteGltf(BuildFloorBlock(), dir + "\\floor_block.gltf");
	ok &= WriteGltf(BuildCeilingBlock(), dir + "\\ceiling_block.gltf");
	ok &= WriteGltf(BuildWornWallBlock(), dir + "\\wall_block_worn.gltf");
	ok &= WriteGltf(BuildWornFloorBlock(), dir + "\\floor_block_worn.gltf");
	ok &= WriteGltf(BuildWornCeilingBlock(), dir + "\\ceiling_block_worn.gltf");
	ok &= WriteGltf(BuildSconce(), dir + "\\sconce.gltf");
	ok &= WriteGltf(BuildBrazier(), dir + "\\brazier.gltf");
	ok &= WriteGltf(BuildSerpentPillar(), dir + "\\pillar.gltf");
	ok &= WriteGltf(BuildHumanoid({{0.93f, 0.90f, 0.80f, 1.0f}, 0.85f, 3.2f, 0.0f, 0.12f}),
					dir + "\\skeleton.gltf");
	ok &= WriteGltf(BuildHumanoid({{0.72f, 0.65f, 0.48f, 1.0f}, 1.45f, 5.0f, 1.05f, 0.07f}),
					dir + "\\mummy.gltf");
	ok &= WriteGltf(BuildBlob(), dir + "\\blob.gltf");
	return ok;
}

} // namespace dungeon::baker
