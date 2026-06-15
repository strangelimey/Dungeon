// ============================================================================
// ModelBaker.cpp — procedural model construction.
//
// Builds assets::ModelData in code and hands it to WriteGltf:
//   * dungeon blocks — wall (recessed panel + edge pillars, authored facing
//     +Z over x∈[±kCellHalf], y∈[0,2.5]), flat floor, flat ceiling (facing down,
//     placed at wall height by the game); worn variants per surface texture,
//     displaced by that texture's scanned height map (see the worn section)
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

#include "Assets/Image.h"
#include "Core/Log.h"
#include "Core/MathTypes.h"
#include "GltfWriter.h"
#include "Noise.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <string>
#include <vector>

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

// Flat horizontal ring at height y (a fountain rim, or a solid disk cap when
// rInner == 0). Normal faces up or down per `faceUp`.
void AddAnnulus(assets::MeshData& mesh, float cx, float cz, float y, float rInner,
				float rOuter, int sides, bool faceUp) {
	const u32 base = static_cast<u32>(mesh.vertices.size());
	const Vec3 n{0, faceUp ? 1.0f : -1.0f, 0};
	for (int s = 0; s <= sides; ++s) {
		const float a = static_cast<float>(s) / sides * 2.0f * kPi;
		const float ca = std::cos(a), sa = std::sin(a);
		for (const float rad : {rInner, rOuter}) {
			assets::Vertex v;
			v.position = {cx + ca * rad, y, cz + sa * rad};
			v.normal = n;
			v.uv = {0.5f + 0.5f * ca, 0.5f + 0.5f * sa};
			mesh.vertices.push_back(v);
		}
	}
	for (int s = 0; s < sides; ++s) {
		const u32 a = base + s * 2, b = a + 1, c = a + 2, d = a + 3;
		mesh.indices.insert(mesh.indices.end(), {a, b, d, a, d, c});
	}
}

// Surface of revolution about a vertical axis at (cx, cz), from a profile of
// (radius, y) rings ordered bottom to top. Side normals come from the profile
// slope; `inward` flips them for interior walls (a fountain basin). The scene
// pipeline draws double-sided (CULL_MODE_NONE), so winding is cosmetic —
// normals carry the lighting. Optional flat disk caps close the first/last ring.
void AddRevolution(assets::MeshData& mesh, float cx, float cz,
				   const std::vector<Vec2>& profile, int sides, bool capBottom,
				   bool capTop, bool inward = false) {
	const u32 base = static_cast<u32>(mesh.vertices.size());
	const int rings = static_cast<int>(profile.size());
	const float flip = inward ? -1.0f : 1.0f;
	for (int r = 0; r < rings; ++r) {
		const Vec2& cur = profile[r];
		const Vec2& prev = profile[std::max(0, r - 1)];
		const Vec2& next = profile[std::min(rings - 1, r + 1)];
		// Profile tangent (dr, dy); outward surface normal is (dy, -dr).
		const float dr = next.x - prev.x, dy = next.y - prev.y;
		const float nlen = std::sqrt(dr * dr + dy * dy);
		const float nr = nlen > 1e-6f ? dy / nlen : 1.0f;
		const float ny = nlen > 1e-6f ? -dr / nlen : 0.0f;
		for (int s = 0; s <= sides; ++s) {
			const float a = static_cast<float>(s) / sides * 2.0f * kPi;
			const float ca = std::cos(a), sa = std::sin(a);
			assets::Vertex v;
			v.position = {cx + ca * cur.x, cur.y, cz + sa * cur.x};
			v.normal = {ca * nr * flip, ny * flip, sa * nr * flip};
			v.uv = {static_cast<float>(s) / sides, static_cast<float>(r) / (rings - 1)};
			mesh.vertices.push_back(v);
		}
	}
	const u32 stride = static_cast<u32>(sides) + 1;
	for (int r = 0; r < rings - 1; ++r)
		for (int s = 0; s < sides; ++s) {
			const u32 a = base + r * stride + s, b = a + 1, c = a + stride, d = c + 1;
			mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
		}

	// Disk caps so the ends read solid (a barrel lid, a column footing).
	if (capBottom && profile.front().x > 1e-5f)
		AddAnnulus(mesh, cx, cz, profile.front().y, 0.0f, profile.front().x, sides, false);
	if (capTop && profile.back().x > 1e-5f)
		AddAnnulus(mesh, cx, cz, profile.back().y, 0.0f, profile.back().x, sides, true);
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
// Wall block: authored facing +Z (the room side), x in [-kCellHalf, kCellHalf],
// y in [0,2.5]. A recessed center panel framed by edge pillars gives real 3D
// relief that the parallax-mapped textures then deepen.

constexpr float kWallH = 2.5f;    // must match game::kWallHeight (DungeonMap.h)
constexpr float kCellHalf = 1.2f; // half of game::kCellSize (DungeonMap.h)
constexpr float kUvScale = 1.0f / (2.0f * kCellHalf); // one texture tile per cell

// Planar UV projection chosen by the face normal's dominant axis, with a
// consistent texel scale (one texture tile per cell width, so adjacent
// blocks tile seamlessly). Faces that point sideways or up/down (panel
// reveals, pillar flanks) get their own in-plane projection instead of a
// smeared front projection.
Vec2 WallFaceUv(const Vec3& p, const Vec3& n) {
	const float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
	if (az >= ax && az >= ay)
		return {(p.x + kCellHalf) * kUvScale, (kWallH - p.y) * kUvScale};
	if (ay >= ax) return {(p.x + kCellHalf) * kUvScale, (p.z + kCellHalf) * kUvScale};
	return {(p.z + kCellHalf) * kUvScale, (kWallH - p.y) * kUvScale};
}

constexpr float kPanelX = 0.80f * kCellHalf; // panel half-width (between pillars)
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
		const float cx = side * (kCellHalf - (kCellHalf - kPanelX) * 0.5f);
		const float hw = (kCellHalf - kPanelX) * 0.5f;
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
		const float outer = side < 0 ? x0 : x1; // == ±kCellHalf
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
	const float c = kCellHalf;
	AddQuad(mesh, {-c, 0, -c}, {c, 0, -c}, {c, 0, c}, {-c, 0, c}, {0, 1, 0}, {0, 0},
			{1, 0}, {1, 1}, {0, 1});
	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{1, 1, 1, 1}, -1});
	return model;
}

assets::ModelData BuildCeilingBlock() {
	// Authored at y=0 facing down; placed at wall height by the game.
	assets::ModelData model;
	assets::MeshData mesh;
	const float c = kCellHalf;
	AddQuad(mesh, {-c, 0, c}, {c, 0, c}, {c, 0, -c}, {-c, 0, -c}, {0, -1, 0}, {0, 0},
			{1, 0}, {1, 1}, {0, 1});
	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{1, 1, 1, 1}, -1});
	return model;
}

// --- worn dungeon blocks -----------------------------------------------------------
// A second block set with real displaced geometry for old, crumbling areas:
// tessellated grids whose vertices are pushed by a wear field, with normals
// derived from the displacement gradient by central differences. All
// displacement is pinned to zero at block edges so adjacent cells (and the
// clean set, if mixed) always meet watertight. The clean blocks remain for
// newer, well-kept areas of the dungeon.
//
// One worn set is baked PER SURFACE TEXTURE (worn_<texture>_<tier>.gltf):
// the wear field samples that texture's scanned height map (packed into the
// alpha of <texture>_1k_n.png by the importer), so the mortar lines, broken
// bricks, and slab joints in the geometry land exactly where the texture
// shows them. When the scanned sets are not installed (fresh checkout before
// tools/FetchTextures.ps1), procedural wear keeps the bake whole.

// A wear field maps block-local surface coordinates to a signed displacement
// (depth into walls/ceilings, height offset for floors), pin ramps included.
using WearField = std::function<float(float, float)>;

// Smooth 0→1 ramp within `width` of a boundary at 0.
float PinRamp(float distance, float width) {
	const float t = std::clamp(distance / width, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

// Samples the height channel (alpha) of a packed normal+height texture:
// bilinear, wrapping, and box-filtered to the mesh grid spacing so coarse
// tiers don't alias detail finer than their vertices.
class TextureHeight {
public:
	explicit TextureHeight(const std::string& packedNormalPath) {
		if (auto image = assets::LoadImageFile(packedNormalPath))
			m_image = std::move(*image);
		// A (near-)constant alpha means the set shipped no real displacement
		// (the importer packs 255 for those) — report invalid so the bake
		// falls back to procedural wear instead of a featureless recess.
		u8 lo = 255, hi = 0;
		for (size_t i = 3; i < m_image.pixels.size(); i += 4) {
			lo = std::min(lo, m_image.pixels[i]);
			hi = std::max(hi, m_image.pixels[i]);
		}
		m_valid = m_image.width > 0 && hi - lo >= 8;
	}

	bool IsValid() const { return m_valid; }

	float Sample(float u, float v) const {
		const int w = static_cast<int>(m_image.width);
		const int h = static_cast<int>(m_image.height);
		const float x = (u - std::floor(u)) * w - 0.5f;
		const float y = (v - std::floor(v)) * h - 0.5f;
		const int x0 = static_cast<int>(std::floor(x));
		const int y0 = static_cast<int>(std::floor(y));
		const float fx = x - x0, fy = y - y0;
		auto at = [&](int px, int py) {
			px = (px % w + w) % w;
			py = (py % h + h) % h;
			return m_image.pixels[(static_cast<size_t>(py) * w + px) * 4 + 3] / 255.0f;
		};
		const float top = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * fx;
		const float bottom = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * fx;
		return top + (bottom - top) * fy;
	}

	// Averages kTaps² samples over one (du, dv) grid-cell footprint.
	float SampleBox(float u, float v, float du, float dv) const {
		constexpr int kTaps = 4;
		float sum = 0.0f;
		for (int j = 0; j < kTaps; ++j)
			for (int i = 0; i < kTaps; ++i)
				sum += Sample(u + du * ((i + 0.5f) / kTaps - 0.5f),
							  v + dv * ((j + 0.5f) / kTaps - 0.5f));
		return sum / (kTaps * kTaps);
	}

private:
	assets::ImageData m_image;
	bool m_valid = false;
};

// --- procedural wear (fallback when the scanned sets are not installed) ----

// Erosion depth (into the rock) for the worn wall surface, in wall-local
// coordinates (x across [-kCellHalf, kCellHalf], y up [0,kWallH]).
float WallWearDepth(float x, float y) {
	const float u = x + kCellHalf;  // 0..cell width along the wall, in meters
	const float v = kWallH - y;     // 0..2.5 down the wall

	// Generic brick grid (0.5 x 0.3125) — only an approximation of the
	// scanned textures; the texture-driven fields below replace this.
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
	const float pin = PinRamp(kCellHalf - std::fabs(x), 0.12f) * PinRamp(y, 0.10f) *
					  PinRamp(kWallH - y, 0.10f);
	return std::clamp(depth, 0.0f, 0.12f) * pin;
}

// Height offset for the worn floor: sunken, tilted slabs with eroded joints.
float FloorWearHeight(float x, float z) {
	const float u = x + kCellHalf, v = z + kCellHalf; // meters, slabs are 1m
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

	const float pin = PinRamp(kCellHalf - std::fabs(x), 0.10f) *
					  PinRamp(kCellHalf - std::fabs(z), 0.10f);
	return std::clamp(h, -0.07f, 0.035f) * pin;
}

// Erosion pockets (upward, into the rock) for the worn ceiling.
float CeilingWearDepth(float x, float z) {
	const float u = x + kCellHalf, v = z + kCellHalf;
	float d = std::max(0.0f, Fbm(u * 1.5f, v * 1.5f, 301u) - 0.42f) * 0.20f;
	d += (Fbm(u * 4.0f, v * 4.0f, 303u) - 0.5f) * 0.015f;
	const float pin = PinRamp(kCellHalf - std::fabs(x), 0.10f) *
					  PinRamp(kCellHalf - std::fabs(z), 0.10f);
	return std::clamp(d, 0.0f, 0.10f) * pin;
}

// --- texture-driven wear ----------------------------------------------------
// Each field samples the matching texture's height map with the SAME UV
// mapping the mesh (and therefore the renderer) uses, so geometric relief
// lines up with the painted bricks/slabs. `relief` is the world-space
// displacement amplitude; du/dv pass the grid footprint to SampleBox.

WearField TextureWallWear(const TextureHeight& height, float relief, int gridX,
						  int gridY, u32 seed) {
	const float du = 1.0f / gridX;
	const float dv = (kWallH / gridY) * kUvScale;
	return [&height, relief, du, dv, seed](float x, float y) {
		const float u = (x + kCellHalf) * kUvScale;
		const float v = (kWallH - y) * kUvScale;
		// Low texture height = recessed surface (mortar, broken bricks).
		float d = (1.0f - height.SampleBox(u, v, du, dv)) * relief;
		d += (Fbm(u * 1.8f, v * 1.8f, seed) - 0.5f) * 0.045f; // bowed masonry
		d += std::clamp(1.0f - y, 0.0f, 1.0f) * 0.018f;       // ground-level wear
		const float pin = PinRamp(kCellHalf - std::fabs(x), 0.12f) * PinRamp(y, 0.10f) *
						  PinRamp(kWallH - y, 0.10f);
		return std::clamp(d, 0.0f, relief + 0.05f) * pin;
	};
}

WearField TextureFloorWear(const TextureHeight& height, float relief, int grid,
						   u32 seed) {
	const float du = 0.5f / grid, dv = 0.5f / grid;
	return [&height, relief, du, dv, seed](float x, float z) {
		const float u = (x + kCellHalf) * kUvScale, v = (z + kCellHalf) * kUvScale;
		float h = (height.SampleBox(u, v, du, dv) - 0.5f) * relief;
		h += (Fbm(u * 2.2f, v * 2.2f, seed) - 0.5f) * 0.02f; // general unevenness
		const float pin = PinRamp(kCellHalf - std::fabs(x), 0.10f) *
						  PinRamp(kCellHalf - std::fabs(z), 0.10f);
		return std::clamp(h, -0.07f, 0.05f) * pin;
	};
}

WearField TextureCeilingWear(const TextureHeight& height, float relief, int grid,
							 u32 seed) {
	const float du = 0.5f / grid, dv = 0.5f / grid;
	return [&height, relief, du, dv, seed](float x, float z) {
		const float u = (x + kCellHalf) * kUvScale, v = (z + kCellHalf) * kUvScale;
		// Low texture height = deeper erosion pocket (upward, into the rock).
		float d = (1.0f - height.SampleBox(u, v, du, dv)) * relief;
		d += (Fbm(u * 3.0f, v * 3.0f, seed) - 0.5f) * 0.015f;
		const float pin = PinRamp(kCellHalf - std::fabs(x), 0.10f) *
						  PinRamp(kCellHalf - std::fabs(z), 0.10f);
		return std::clamp(d, 0.0f, relief) * pin;
	};
}

// Grid resolutions are parameters: the baker emits each worn block at three
// complexity tiers (low/med/high) so the game can trade geometric detail for
// performance via the Settings menu.
assets::ModelData BuildWornWallBlock(int kNx, int kNy, const WearField& wear) {
	assets::ModelData model;
	assets::MeshData mesh;

	// Displaced surface replacing the clean block's panel/border relief.
	constexpr float kEps = 0.02f; // finite-difference step for normals
	for (int j = 0; j <= kNy; ++j) {
		const float y = kWallH * static_cast<float>(j) / kNy;
		for (int i = 0; i <= kNx; ++i) {
			const float x = kCellHalf * (2.0f * static_cast<float>(i) / kNx - 1.0f);
			const float d = wear(x, y);
			// Surface z = -d; tangent cross product gives (dd/dx, dd/dy, 1).
			const float ddx = (wear(x + kEps, y) - wear(x - kEps, y)) / (2 * kEps);
			const float ddy = (wear(x, y + kEps) - wear(x, y - kEps)) / (2 * kEps);
			const float inv = 1.0f / std::sqrt(ddx * ddx + ddy * ddy + 1.0f);

			assets::Vertex vert;
			vert.position = {x, y, -d};
			vert.normal = {ddx * inv, ddy * inv, inv};
			vert.uv = {(x + kCellHalf) * kUvScale, (kWallH - y) * kUvScale};
			mesh.vertices.push_back(vert);
		}
	}
	const u32 stride = static_cast<u32>(kNx) + 1;
	for (u32 j = 0; j < static_cast<u32>(kNy); ++j)
		for (u32 i = 0; i < static_cast<u32>(kNx); ++i) {
			const u32 a = j * stride + i, b = a + 1, c = a + stride, d2 = c + 1;
			mesh.indices.insert(mesh.indices.end(), {a, b, d2, a, d2, c});
		}

	AddWallPillars(mesh);
	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{1, 1, 1, 1}, -1});
	return model;
}

assets::ModelData BuildWornFloorBlock(int kN, const WearField& wear) {
	assets::ModelData model;
	assets::MeshData mesh;

	constexpr float kEps = 0.02f;
	for (int j = 0; j <= kN; ++j) {
		const float z = kCellHalf * (2.0f * static_cast<float>(j) / kN - 1.0f);
		for (int i = 0; i <= kN; ++i) {
			const float x = kCellHalf * (2.0f * static_cast<float>(i) / kN - 1.0f);
			const float h = wear(x, z);
			const float hx = (wear(x + kEps, z) - wear(x - kEps, z)) / (2 * kEps);
			const float hz = (wear(x, z + kEps) - wear(x, z - kEps)) / (2 * kEps);
			const float inv = 1.0f / std::sqrt(hx * hx + hz * hz + 1.0f);

			assets::Vertex vert;
			vert.position = {x, h, z};
			vert.normal = {-hx * inv, inv, -hz * inv};
			vert.uv = {(x + kCellHalf) * kUvScale, (z + kCellHalf) * kUvScale};
			mesh.vertices.push_back(vert);
		}
	}
	const u32 stride = static_cast<u32>(kN) + 1;
	for (u32 j = 0; j < static_cast<u32>(kN); ++j)
		for (u32 i = 0; i < static_cast<u32>(kN); ++i) {
			const u32 a = j * stride + i, b = a + 1, c = a + stride, d2 = c + 1;
			mesh.indices.insert(mesh.indices.end(), {a, d2, b, a, c, d2});
		}

	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{1, 1, 1, 1}, -1});
	return model;
}

assets::ModelData BuildWornCeilingBlock(int kN, const WearField& wear) {
	assets::ModelData model;
	assets::MeshData mesh;

	// Authored at y=0 facing down (like the clean block); erosion goes up.
	constexpr float kEps = 0.02f;
	for (int j = 0; j <= kN; ++j) {
		const float z = kCellHalf * (2.0f * static_cast<float>(j) / kN - 1.0f);
		for (int i = 0; i <= kN; ++i) {
			const float x = kCellHalf * (2.0f * static_cast<float>(i) / kN - 1.0f);
			const float d = wear(x, z);
			const float dx = (wear(x + kEps, z) - wear(x - kEps, z)) / (2 * kEps);
			const float dz = (wear(x, z + kEps) - wear(x, z - kEps)) / (2 * kEps);
			const float inv = 1.0f / std::sqrt(dx * dx + dz * dz + 1.0f);

			assets::Vertex vert;
			vert.position = {x, d, z};
			vert.normal = {dx * inv, -inv, dz * inv};
			vert.uv = {(x + kCellHalf) * kUvScale, (z + kCellHalf) * kUvScale};
			mesh.vertices.push_back(vert);
		}
	}
	const u32 stride = static_cast<u32>(kN) + 1;
	for (u32 j = 0; j < static_cast<u32>(kN); ++j)
		for (u32 i = 0; i < static_cast<u32>(kN); ++i) {
			const u32 a = j * stride + i, b = a + 1, c = a + stride, d2 = c + 1;
			mesh.indices.insert(mesh.indices.end(), {a, b, d2, a, d2, c});
		}

	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{1, 1, 1, 1}, -1});
	return model;
}

// Reprojects a mesh's UVs to world-aligned tiles so a texture repeats at a
// fixed real-world size (tileMeters per tile) regardless of the prop's overall
// shape — each vertex projects onto the plane of its dominant normal axis (the
// same triplanar-by-dominant-axis idea WallFaceUv uses). This keeps the texel
// density even across a blocky prop's differently-sized faces, instead of the
// one-stretched-tile-per-face that AddBox emits. Good for box assemblies;
// curved surfaces (the pillar, the blob) keep their own natural UVs.
void TileUvs(assets::MeshData& mesh, float tileMeters) {
	const float inv = 1.0f / tileMeters;
	for (assets::Vertex& v : mesh.vertices) {
		const float ax = std::fabs(v.normal.x), ay = std::fabs(v.normal.y),
					az = std::fabs(v.normal.z);
		Vec2 p;
		if (ay >= ax && ay >= az) p = {v.position.x, v.position.z};  // up/down faces
		else if (ax >= az)        p = {v.position.z, v.position.y};  // x-facing
		else                      p = {v.position.x, v.position.y};  // z-facing
		v.uv = {p.x * inv, p.y * inv};
	}
}

// --- fire props --------------------------------------------------------------------
// Iron sconce (wall torch holder) and floor brazier. Both are simple box
// assemblies — the drama comes from the particle flames and the point light
// the game attaches at the flame origin (sconce: local (0, 1.78, 0.22);
// brazier: local (0, 0.72, 0)). The game binds a worn-metal PBR set by name
// (sconce_<res>, brazier_<res>), so the meshes carry world-aligned tiling UVs
// for the albedo/normal/height/ORM maps; the glTF baseColor is the fallback.

assets::ModelData BuildSconce() {
	// Authored against a wall at z=0, arm reaching into the room (+Z).
	assets::ModelData model;
	assets::MeshData mesh;
	AddBox(mesh, {0, 1.45f, 0.02f}, {0.07f, 0.16f, 0.02f});   // mounting plate
	AddBox(mesh, {0, 1.52f, 0.12f}, {0.025f, 0.025f, 0.10f}); // arm
	AddBox(mesh, {0, 1.58f, 0.22f}, {0.06f, 0.05f, 0.06f});   // cup
	AddBox(mesh, {0, 1.70f, 0.22f}, {0.022f, 0.10f, 0.022f}); // torch shaft
	TileUvs(mesh, 0.30f); // worn-iron grain repeats every 30 cm
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
	TileUvs(mesh, 0.40f); // bronze pattern repeats every 40 cm
	mesh.material = 0;
	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{0.38f, 0.33f, 0.29f, 1.0f}, -1}); // bronzed iron
	return model;
}

// --- architecture decorations ------------------------------------------------
// Static, single-material props placed by the .map "decoration <type> x z
// [facing]" records (DungeonWorld loads <type>.gltf). All are authored facing
// +Z (the default Direction::South), centered on the cell, resting on the
// floor (y up from 0), and sized to sit inside one 2.4 m cell. One combined
// mesh + one baseColorFactor each — WriteGltf takes a single mesh, and the
// decoration renderer reads the color straight off the glTF material.

// Finalizes a prop: reprojects UVs to world-aligned tiles so textures repeat
// at a fixed real-world size (instead of one stretched tile per prop), then
// wraps the mesh in a single-material model. The projection picks the plane
// from each vertex's dominant normal axis (same idea as the wall blocks'
// WallFaceUv) — good enough for stone/wood tiling on these simple shapes.
assets::ModelData FinishProp(assets::MeshData&& mesh, const Vec4& color,
							 float tileMeters = 0.6f) {
	TileUvs(mesh, tileMeters);
	assets::ModelData model;
	mesh.material = 0;
	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({color, -1});
	return model;
}

// Doric-ish stone column: plinth, swelling shaft (entasis), flared capital.
// Stands ~2.45 m, just shy of the 2.5 m ceiling.
assets::ModelData BuildColumn() {
	const std::vector<Vec2> profile = {
		{0.36f, 0.00f}, {0.36f, 0.10f}, {0.30f, 0.14f}, {0.27f, 0.20f},
		{0.225f, 0.30f}, {0.23f, 1.10f}, {0.22f, 1.90f}, {0.205f, 2.10f},
		{0.255f, 2.24f}, {0.32f, 2.34f}, {0.34f, 2.38f}, {0.34f, 2.45f}};
	assets::MeshData mesh;
	AddRevolution(mesh, 0, 0, profile, 18, true, true);
	return FinishProp(std::move(mesh), {0.66f, 0.64f, 0.60f, 1.0f});
}

// Stone archway: a full-cell wall slab with an arched opening cut through it,
// so the jambs run out to the flanking walls and a spandrel fills the corners
// above the curve up to the ceiling — no gaps to see around. A shallow voussoir
// ring framing the opening keeps the arch relief. Authored facing +Z (passage
// along Z); placed facing East/West it spans the corridor between its walls.
assets::ModelData BuildArchway() {
	constexpr float kIn = 0.62f;       // opening half-width / soffit radius
	constexpr float kOut = 0.92f;      // voussoir ring outer radius
	constexpr float kSpring = 1.55f;   // springline height
	constexpr float kD = 0.20f;        // wall half-thickness
	constexpr float kProud = 0.06f;    // ring relief proud of the wall face
	constexpr float kDf = kD + kProud; // ring face / opening reveal half-depth
	constexpr int kN = 22;             // arch segments
	assets::MeshData mesh;

	auto quad = [&](const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
					const Vec3& n) {
		AddQuad(mesh, a, b, c, d, n, {0, 0}, {1, 0}, {1, 1}, {0, 1});
	};
	// Arch outline point at radius r, angle theta (0 = +x spring, pi = -x).
	auto arc = [&](float r, float th) {
		return Vec2{r * std::cos(th), kSpring + r * std::sin(th)};
	};
	// Radial projection from the springline center onto the cell-top box
	// (|x| <= kOut, y in [kSpring, kWallH]) — the outer edge of the spandrel.
	auto box = [&](float th) {
		const float c = std::cos(th), s = std::sin(th);
		float t = 1e9f;
		if (s > 1e-4f) t = std::min(t, (kWallH - kSpring) / s);
		if (c > 1e-4f) t = std::min(t, kOut / c);
		else if (c < -1e-4f) t = std::min(t, -kOut / c);
		return Vec2{c * t, kSpring + s * t};
	};

	for (const float side : {1.0f, -1.0f}) {
		const Vec3 nz{0, 0, side};
		const float zw = side * kD, zr = side * kDf;
		// Flat wall: full-height panels beyond the ring + the top spandrel.
		quad({-kCellHalf, 0, zw}, {-kOut, 0, zw}, {-kOut, kWallH, zw},
			 {-kCellHalf, kWallH, zw}, nz);
		quad({kOut, 0, zw}, {kCellHalf, 0, zw}, {kCellHalf, kWallH, zw},
			 {kOut, kWallH, zw}, nz);
		for (int k = 0; k < kN; ++k) {
			const float t0 = kPi * k / kN, t1 = kPi * (k + 1) / kN;
			const Vec2 o0 = arc(kOut, t0), o1 = arc(kOut, t1);
			const Vec2 p0 = box(t0), p1 = box(t1);
			quad({o0.x, o0.y, zw}, {o1.x, o1.y, zw}, {p1.x, p1.y, zw}, {p0.x, p0.y, zw}, nz);
		}
		// Voussoir ring face (proud of the wall): straight jambs + arch band.
		quad({kIn, 0, zr}, {kOut, 0, zr}, {kOut, kSpring, zr}, {kIn, kSpring, zr}, nz);
		quad({-kOut, 0, zr}, {-kIn, 0, zr}, {-kIn, kSpring, zr}, {-kOut, kSpring, zr}, nz);
		for (int k = 0; k < kN; ++k) {
			const float t0 = kPi * k / kN, t1 = kPi * (k + 1) / kN;
			const Vec2 i0 = arc(kIn, t0), i1 = arc(kIn, t1);
			const Vec2 o0 = arc(kOut, t0), o1 = arc(kOut, t1);
			quad({i0.x, i0.y, zr}, {o0.x, o0.y, zr}, {o1.x, o1.y, zr}, {i1.x, i1.y, zr}, nz);
		}
		// Ring outer side wall: steps from the wall plane up to the proud face.
		quad({kOut, 0, zw}, {kOut, kSpring, zw}, {kOut, kSpring, zr}, {kOut, 0, zr}, {1, 0, 0});
		quad({-kOut, 0, zw}, {-kOut, kSpring, zw}, {-kOut, kSpring, zr}, {-kOut, 0, zr}, {-1, 0, 0});
		for (int k = 0; k < kN; ++k) {
			const float t0 = kPi * k / kN, t1 = kPi * (k + 1) / kN;
			const Vec2 o0 = arc(kOut, t0), o1 = arc(kOut, t1);
			const Vec3 rn{std::cos(t0), std::sin(t0), 0};
			quad({o0.x, o0.y, zw}, {o1.x, o1.y, zw}, {o1.x, o1.y, zr}, {o0.x, o0.y, zr}, rn);
		}
	}

	// Opening interior (through the full ±kDf depth): soffit + jamb reveals.
	for (int k = 0; k < kN; ++k) {
		const float t0 = kPi * k / kN, t1 = kPi * (k + 1) / kN;
		const Vec2 a0 = arc(kIn, t0), a1 = arc(kIn, t1);
		const Vec3 n{-std::cos(t0), -std::sin(t0), 0};
		quad({a0.x, a0.y, kDf}, {a0.x, a0.y, -kDf}, {a1.x, a1.y, -kDf}, {a1.x, a1.y, kDf}, n);
	}
	quad({kIn, 0, kDf}, {kIn, 0, -kDf}, {kIn, kSpring, -kDf}, {kIn, kSpring, kDf}, {-1, 0, 0});
	quad({-kIn, 0, -kDf}, {-kIn, 0, kDf}, {-kIn, kSpring, kDf}, {-kIn, kSpring, -kDf}, {1, 0, 0});

	// Slab outer edges so the sides meet the walls and the top meets the ceiling.
	quad({-kCellHalf, 0, kD}, {-kCellHalf, kWallH, kD}, {-kCellHalf, kWallH, -kD}, {-kCellHalf, 0, -kD}, {-1, 0, 0});
	quad({kCellHalf, 0, -kD}, {kCellHalf, kWallH, -kD}, {kCellHalf, kWallH, kD}, {kCellHalf, 0, kD}, {1, 0, 0});
	quad({-kCellHalf, kWallH, kD}, {-kCellHalf, kWallH, -kD}, {kCellHalf, kWallH, -kD}, {kCellHalf, kWallH, kD}, {0, 1, 0});

	return FinishProp(std::move(mesh), {0.60f, 0.58f, 0.55f, 1.0f});
}

// Closed wooden door in a timber frame (facing +Z blocks Z-passage): jambs,
// lintel, a plank leaf with battens, two iron straps, and a ring handle.
assets::ModelData BuildDoor() {
	assets::MeshData mesh;
	AddBox(mesh, {-0.55f, 1.05f, 0}, {0.12f, 1.05f, 0.14f}); // left jamb
	AddBox(mesh, {0.55f, 1.05f, 0}, {0.12f, 1.05f, 0.14f});  // right jamb
	AddBox(mesh, {0, 2.16f, 0}, {0.67f, 0.10f, 0.14f});      // lintel
	AddBox(mesh, {0, 1.02f, 0}, {0.42f, 1.02f, 0.05f});      // leaf
	for (const float px : {-0.28f, 0.0f, 0.28f})             // plank grooves
		AddBox(mesh, {px, 1.02f, 0.055f}, {0.015f, 1.0f, 0.01f});
	for (const float py : {0.45f, 1.6f})                     // iron straps
		AddBox(mesh, {0, py, 0.06f}, {0.40f, 0.05f, 0.012f});
	AddBox(mesh, {0.30f, 1.02f, 0.075f}, {0.05f, 0.05f, 0.02f}); // handle
	return FinishProp(std::move(mesh), {0.40f, 0.27f, 0.16f, 1.0f});
}

// Two-tier stone fountain: a low octagon-smooth basin with a recessed pool, a
// central pedestal, and a small upper bowl.
assets::ModelData BuildFountain() {
	constexpr int kS = 20;
	assets::MeshData mesh;
	// Outer wall + plinth.
	AddRevolution(mesh, 0, 0, {{0.98f, 0.0f}, {0.98f, 0.10f}, {0.92f, 0.14f},
							   {0.92f, 0.46f}, {0.96f, 0.50f}},
				  kS, true, false);
	AddAnnulus(mesh, 0, 0, 0.50f, 0.80f, 0.96f, kS, true);  // rim
	// Inner basin wall (inward-facing) down to the pool floor.
	AddRevolution(mesh, 0, 0, {{0.80f, 0.50f}, {0.80f, 0.18f}}, kS, false, false, true);
	AddAnnulus(mesh, 0, 0, 0.18f, 0.18f, 0.80f, kS, true);  // pool floor
	// Central pedestal + upper bowl + finial.
	AddRevolution(mesh, 0, 0, {{0.18f, 0.18f}, {0.13f, 0.30f}, {0.12f, 0.72f}}, kS,
				  false, false);
	AddRevolution(mesh, 0, 0, {{0.12f, 0.72f}, {0.34f, 0.80f}, {0.34f, 0.86f},
							   {0.30f, 0.88f}, {0.10f, 0.84f}, {0.06f, 0.96f}},
				  kS, false, true);
	return FinishProp(std::move(mesh), {0.52f, 0.55f, 0.58f, 1.0f});
}

// Pedestal statue of a robed figure, pale marble. Built from a square plinth
// and blocked-out body so it reads at dungeon scale.
assets::ModelData BuildStatue() {
	assets::MeshData mesh;
	AddBox(mesh, {0, 0.18f, 0}, {0.36f, 0.18f, 0.36f});  // plinth
	AddBox(mesh, {0, 0.40f, 0}, {0.30f, 0.04f, 0.30f});  // cap slab
	AddBox(mesh, {0, 0.86f, 0}, {0.21f, 0.42f, 0.16f});  // robe / legs
	AddBox(mesh, {0, 1.34f, 0}, {0.23f, 0.22f, 0.16f});  // torso
	AddBox(mesh, {-0.27f, 1.24f, 0.02f}, {0.07f, 0.30f, 0.08f}); // arm L
	AddBox(mesh, {0.27f, 1.24f, 0.02f}, {0.07f, 0.30f, 0.08f});  // arm R
	AddRevolution(mesh, 0, 0,
				  {{0.0f, 1.52f}, {0.10f, 1.56f}, {0.13f, 1.66f}, {0.10f, 1.76f},
				   {0.0f, 1.80f}},
				  14, false, false); // head
	return FinishProp(std::move(mesh), {0.76f, 0.74f, 0.70f, 1.0f});
}

// Wooden barrel: a bulged body of revolution with two proud iron hoops.
assets::ModelData BuildBarrel() {
	assets::MeshData mesh;
	AddRevolution(mesh, 0, 0,
				  {{0.24f, 0.0f}, {0.30f, 0.05f}, {0.34f, 0.22f}, {0.355f, 0.43f},
				   {0.34f, 0.64f}, {0.30f, 0.81f}, {0.24f, 0.86f}},
				  16, true, true);
	for (const float y : {0.20f, 0.62f}) // hoops
		AddRevolution(mesh, 0, 0, {{0.365f, y}, {0.365f, y + 0.05f}}, 16, false, false);
	return FinishProp(std::move(mesh), {0.45f, 0.31f, 0.18f, 1.0f});
}

// Wooden crate: a solid box wrapped by battens along all twelve edges.
assets::ModelData BuildCrate() {
	constexpr float s = 0.36f, t = 0.05f;
	assets::MeshData mesh;
	AddBox(mesh, {0, s, 0}, {s, s, s}); // body (sits 0..2s)
	for (const float zside : {-s, s})   // X-edge battens (top & bottom)
		for (const float yside : {0.0f, 2 * s})
			AddBox(mesh, {0, yside, zside}, {s + 0.02f, t, t});
	for (const float xside : {-s, s})   // Z-edge battens
		for (const float yside : {0.0f, 2 * s})
			AddBox(mesh, {xside, yside, 0}, {t, t, s + 0.02f});
	for (const float xside : {-s, s})   // vertical corner battens
		for (const float zside : {-s, s})
			AddBox(mesh, {xside, s, zside}, {t, s + 0.02f, t});
	return FinishProp(std::move(mesh), {0.50f, 0.36f, 0.22f, 1.0f});
}

// Treasure chest: a box body, a flat lid, two iron straps, and a lock plate.
assets::ModelData BuildChest() {
	assets::MeshData mesh;
	AddBox(mesh, {0, 0.22f, 0}, {0.42f, 0.22f, 0.28f});  // body
	AddBox(mesh, {0, 0.50f, 0}, {0.42f, 0.08f, 0.28f});  // lid
	for (const float xs : {-0.20f, 0.20f}) {             // straps over the front
		AddBox(mesh, {xs, 0.22f, 0.285f}, {0.04f, 0.22f, 0.012f});
		AddBox(mesh, {xs, 0.585f, 0}, {0.04f, 0.012f, 0.28f});
	}
	AddBox(mesh, {0, 0.44f, 0.29f}, {0.07f, 0.07f, 0.02f}); // lock plate
	return FinishProp(std::move(mesh), {0.43f, 0.30f, 0.18f, 1.0f});
}

// --- serpent pillar ----------------------------------------------------------------

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
	// World-aligned tiling so the bone/bandage set (skeleton_<res>, mummy_<res>)
	// keeps an even grain across the limbs; the game binds the set by type name.
	TileUvs(mesh, 0.55f);
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

bool BakeModels(const std::string& dir, const std::string& texturesDir) {
	bool ok = true;
	ok &= WriteGltf(BuildWallBlock(), dir + "\\wall_block.gltf");
	ok &= WriteGltf(BuildFloorBlock(), dir + "\\floor_block.gltf");
	ok &= WriteGltf(BuildCeilingBlock(), dir + "\\ceiling_block.gltf");

	// Worn blocks: one set per surface texture, each at three complexity
	// tiers (selectable in-game). The texture names and their order must
	// match the surface sets in Game::LoadAllSurfaceTextures — the game
	// pairs worn_<texture>_<tier>.gltf with <texture>_<res> by position.
	struct Tier {
		const char* suffix;
		int wallX, wallY, floor, ceiling;
	};
	// In-plane counts cover kCellSize (2.4m), wallY covers kWallH (2.5m);
	// all chosen for roughly equal vertex spacing per tier.
	const Tier tiers[] = {
		{"low", 14, 16, 14, 12},
		{"med", 34, 36, 34, 29},
		{"high", 53, 56, 53, 43},
	};
	enum class Kind { Wall, Floor, Ceiling };
	struct WornSpec {
		Kind kind;
		const char* texture;
		float relief; // world-space displacement amplitude (meters)
		u32 seed;
	};
	const WornSpec specs[] = {
		{Kind::Wall, "wall_brick", 0.060f, 911u},
		{Kind::Wall, "wall_stone", 0.055f, 921u},
		{Kind::Wall, "wall_moss", 0.040f, 931u},
		{Kind::Floor, "floor_slabs", 0.050f, 941u},
		{Kind::Floor, "floor_cobble", 0.045f, 951u},
		{Kind::Ceiling, "ceiling_rough", 0.100f, 961u},
		{Kind::Ceiling, "ceiling_cracked", 0.080f, 971u},
	};
	for (const WornSpec& spec : specs) {
		const TextureHeight height(
			std::format("{}\\{}_1k_n.png", texturesDir, spec.texture));
		if (!height.IsValid())
			log::Warn("{}: no packed height map — baking procedural wear "
					  "(run tools/FetchTextures.ps1, then rebake models)",
					  spec.texture);
		for (const Tier& tier : tiers) {
			const std::string out =
				std::format("{}\\worn_{}_{}.gltf", dir, spec.texture, tier.suffix);
			switch (spec.kind) {
			case Kind::Wall:
				ok &= WriteGltf(
					BuildWornWallBlock(
						tier.wallX, tier.wallY,
						height.IsValid()
							? TextureWallWear(height, spec.relief, tier.wallX,
											  tier.wallY, spec.seed)
							: WearField(WallWearDepth)),
					out);
				break;
			case Kind::Floor:
				ok &= WriteGltf(
					BuildWornFloorBlock(
						tier.floor,
						height.IsValid()
							? TextureFloorWear(height, spec.relief, tier.floor, spec.seed)
							: WearField(FloorWearHeight)),
					out);
				break;
			case Kind::Ceiling:
				ok &= WriteGltf(
					BuildWornCeilingBlock(
						tier.ceiling,
						height.IsValid()
							? TextureCeilingWear(height, spec.relief, tier.ceiling,
												 spec.seed)
							: WearField(CeilingWearDepth)),
					out);
				break;
			}
		}
	}

	ok &= WriteGltf(BuildSconce(), dir + "\\sconce.gltf");
	ok &= WriteGltf(BuildBrazier(), dir + "\\brazier.gltf");
	ok &= WriteGltf(BuildSerpentPillar(), dir + "\\pillar.gltf");

	// Static architecture decorations (placed by .map "decoration" records).
	ok &= WriteGltf(BuildColumn(), dir + "\\column.gltf");
	ok &= WriteGltf(BuildArchway(), dir + "\\archway.gltf");
	ok &= WriteGltf(BuildDoor(), dir + "\\door.gltf");
	ok &= WriteGltf(BuildFountain(), dir + "\\fountain.gltf");
	ok &= WriteGltf(BuildStatue(), dir + "\\statue.gltf");
	ok &= WriteGltf(BuildBarrel(), dir + "\\barrel.gltf");
	ok &= WriteGltf(BuildCrate(), dir + "\\crate.gltf");
	ok &= WriteGltf(BuildChest(), dir + "\\chest.gltf");

	ok &= WriteGltf(BuildHumanoid({{0.93f, 0.90f, 0.80f, 1.0f}, 0.85f, 3.2f, 0.0f, 0.12f}),
					dir + "\\skeleton.gltf");
	ok &= WriteGltf(BuildHumanoid({{0.72f, 0.65f, 0.48f, 1.0f}, 1.45f, 5.0f, 1.05f, 0.07f}),
					dir + "\\mummy.gltf");
	ok &= WriteGltf(BuildBlob(), dir + "\\blob.gltf");
	return ok;
}

} // namespace dungeon::baker
