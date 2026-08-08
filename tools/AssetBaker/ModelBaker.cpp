// ============================================================================
// ModelBaker.cpp — procedural model construction.
//
// Builds assets::ModelData in code and hands it to WriteGltf:
//   * dungeon blocks — wall (recessed panel + edge pillars, authored facing
//     +Z over x∈[±kCellHalf], y∈[0,2.5]), flat floor, flat ceiling (facing down,
//     placed at wall height by the game); worn variants per surface texture,
//     displaced by that texture's scanned height map (see the worn section)
//   * monsters — skeleton & mummy share a 15-joint humanoid rig (torso +
//     three-joint arms shoulder/elbow/wrist & legs hip/knee/ankle) with
//     segmented tapered-tube bones, ball joints, a skull, and idle/walk/attack/
//     die clips; the blob is a 2-joint lumpy sphere with squash-based idle/
//     walk/attack/die clips
//
// Geometry helpers: AddBox / AddRevolution (vertical axis) / AddStrut (a
// tapered tube between two arbitrary points — splayed legs, forged arms,
// skinned limbs) / AddSphere; TileUvs reprojects to world-aligned tiles.
// Conventions: joints are emitted parent-before-child; inverse binds are
// pure translations (-joint global position); rigid limbs weight fully to
// one joint.
// ============================================================================
#include "ModelBaker.h"

#include "Assets/Image.h"
#include "Assets/WornPanel.h"
#include "Core/Log.h"
#include "Core/MathTypes.h"
#include "GltfWriter.h"
#include "Noise.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <initializer_list>
#include <system_error>
#include <string>
#include <utility>
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

// --- vector helpers (MathTypes ships Add/Sub/Scale/Lerp only) ----------------
float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(const Vec3& a, const Vec3& b) {
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }
Vec3 Normalize(const Vec3& v) {
	const float l = Length(v);
	return l > 1e-6f ? Scale(v, 1.0f / l) : Vec3{0, 1, 0};
}

// Tapered round tube between two arbitrary points (radius ra at a, rb at b),
// `sides` around, with fan caps so the ends read solid. Rigidly bound to
// `joint` if >= 0. Unlike AddRevolution this is not axis-locked, so it builds
// splayed brazier legs, the sconce's forged arm, and skinned limbs. Normals
// fold in the taper slope so cones still light correctly.
void AddStrut(assets::MeshData& mesh, const Vec3& a, const Vec3& b, float ra,
			  float rb, int sides, int joint = -1) {
	const float len = std::max(Length(Sub(b, a)), 1e-6f);
	const Vec3 axis = Scale(Sub(b, a), 1.0f / len);
	const Vec3 ref = std::fabs(axis.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
	const Vec3 u = Normalize(Cross(ref, axis));
	const Vec3 w = Cross(axis, u);
	const float slope = (ra - rb) / len; // d(radius)/d(length) -> axial normal tilt
	const u32 base = static_cast<u32>(mesh.vertices.size());
	auto setSkin = [&](assets::Vertex& v) {
		if (joint >= 0) { v.joints[0] = static_cast<u32>(joint); v.weights[0] = 1.0f; }
	};
	for (int ring = 0; ring < 2; ++ring) {
		const Vec3 c = ring ? b : a;
		const float r = ring ? rb : ra;
		for (int s = 0; s <= sides; ++s) {
			const float ang = static_cast<float>(s) / sides * 2.0f * kPi;
			const Vec3 dir = Add(Scale(u, std::cos(ang)), Scale(w, std::sin(ang)));
			assets::Vertex v;
			v.position = Add(c, Scale(dir, r));
			v.normal = Normalize(Add(dir, Scale(axis, slope)));
			v.uv = {static_cast<float>(s) / sides, static_cast<float>(ring)};
			setSkin(v);
			mesh.vertices.push_back(v);
		}
	}
	const u32 stride = static_cast<u32>(sides) + 1;
	for (int s = 0; s < sides; ++s) {
		const u32 p = base + s, q = p + 1, c = p + stride, d = c + 1;
		mesh.indices.insert(mesh.indices.end(), {p, c, q, q, c, d});
	}
	// Fan caps close each end (legs/limbs hide one end, but tube ends like the
	// torch head or a claw foot show, so cap both).
	auto cap = [&](const Vec3& c, const Vec3& n, float r, bool flip) {
		const u32 cb = static_cast<u32>(mesh.vertices.size());
		assets::Vertex center;
		center.position = c;
		center.normal = n;
		center.uv = {0.5f, 0.5f};
		setSkin(center);
		mesh.vertices.push_back(center);
		for (int s = 0; s <= sides; ++s) {
			const float ang = static_cast<float>(s) / sides * 2.0f * kPi;
			const float ca = std::cos(ang), sa = std::sin(ang);
			const Vec3 dir = Add(Scale(u, ca), Scale(w, sa));
			assets::Vertex v;
			v.position = Add(c, Scale(dir, r));
			v.normal = n;
			v.uv = {0.5f + 0.5f * ca, 0.5f + 0.5f * sa};
			setSkin(v);
			mesh.vertices.push_back(v);
		}
		for (int s = 0; s < sides; ++s)
			if (flip) mesh.indices.insert(mesh.indices.end(), {cb, cb + 2 + s, cb + 1 + s});
			else mesh.indices.insert(mesh.indices.end(), {cb, cb + 1 + s, cb + 2 + s});
	};
	cap(a, Scale(axis, -1.0f), ra, false);
	cap(b, axis, rb, true);
}

// UV sphere centered at `c`, optionally squashed per axis by `scale`, bound to
// `joint` if >= 0. The skeleton's skull and a rounder monster head.
void AddSphere(assets::MeshData& mesh, const Vec3& c, float radius, int lat,
			   int lon, int joint = -1, const Vec3& scale = {1, 1, 1}) {
	const u32 base = static_cast<u32>(mesh.vertices.size());
	for (int i = 0; i <= lat; ++i) {
		const float theta = kPi * static_cast<float>(i) / lat;
		for (int j = 0; j <= lon; ++j) {
			const float phi = 2.0f * kPi * static_cast<float>(j) / lon;
			const Vec3 n{std::sin(theta) * std::cos(phi), std::cos(theta),
						 std::sin(theta) * std::sin(phi)};
			assets::Vertex v;
			v.position = {c.x + n.x * radius * scale.x, c.y + n.y * radius * scale.y,
						  c.z + n.z * radius * scale.z};
			v.normal = Normalize({n.x / scale.x, n.y / scale.y, n.z / scale.z});
			v.uv = {static_cast<float>(j) / lon, static_cast<float>(i) / lat};
			if (joint >= 0) { v.joints[0] = static_cast<u32>(joint); v.weights[0] = 1.0f; }
			mesh.vertices.push_back(v);
		}
	}
	const u32 stride = static_cast<u32>(lon) + 1;
	for (int i = 0; i < lat; ++i)
		for (int j = 0; j < lon; ++j) {
			const u32 a = base + i * stride + j, b = a + 1, cc = a + stride, d = cc + 1;
			mesh.indices.insert(mesh.indices.end(), {a, b, cc, b, d, cc});
		}
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

// EVERYTHING THIS BAKER WRITES IS IN UNIT SPACE: 1.0 = one dungeon square. The
// square is a cube, so a block spans x,z in [-0.5, 0.5] and y in [0, 1]. The
// game multiplies by game::kUnit (src/Game/DungeonMap.h) when it stamps a mesh
// into the world — so changing the metre size of a square needs NO rebake, and
// hand-built Blender assets drop in on exactly these conventions
// (docs/authoring-scale.md).
constexpr float kWallH = 1.0f;    // floor to ceiling — one square
constexpr float kCellHalf = 0.5f; // half a square
constexpr float kUvScale = 1.0f / (2.0f * kCellHalf); // one texture tile per cell

// Real-world detail sizes still read better as metres — a 14 cm border strip, an
// 8.5 cm pillar protrusion, a 1.7 m torch height. U() converts such a dimension
// to units against the REFERENCE square this art was proportioned for. It is an
// AUTHORING convenience only: the baked numbers are units either way, and
// nothing at runtime consults kRefSquare.
constexpr float kRefSquare = 2.5f; // metres per square the art was proportioned for
constexpr float U(float metres) { return metres / kRefSquare; }
// The inverse. Named in full: BuildWallWindow has a local `M` (strip count).
constexpr float Metres(float units) { return units * kRefSquare; }

// PROPS AND CREATURES are proportioned in METRES throughout — a barrel is 0.9 m
// tall, a lever handle 20 cm long, a skeleton 1.7 m — and those numbers carry no
// cell meaning, so they stay authored as metres and are converted here, at the
// single boundary where a finished prop becomes a model (FinishProp and the few
// builders that assemble their own). CELL-RELATIVE geometry — the block family,
// the archway that must meet the flanking walls — is authored directly in units
// instead, off kCellHalf/kWallH. A prop mixing the two expresses its cell
// dimensions as Metres(kCellHalf) so they land exactly on the cell edge after this.
void ScaleMeshToUnits(assets::MeshData& mesh) {
	for (assets::Vertex& v : mesh.vertices)
		v.position = {U(v.position.x), U(v.position.y), U(v.position.z)};
	// Normals are unaffected by a uniform scale.
}

// The rigged counterpart: the mesh, the skeleton's rest translations and inverse
// binds (pure translations here — see the file banner), and every translation
// animation track. Rotations and scales are scale-invariant, so they are left.
void ScaleModelToUnits(assets::ModelData& model) {
	for (assets::MeshData& mesh : model.meshes) ScaleMeshToUnits(mesh);
	for (assets::JointData& joint : model.skeleton.joints) {
		joint.restTranslation = {U(joint.restTranslation.x), U(joint.restTranslation.y),
								 U(joint.restTranslation.z)};
		joint.inverseBind._41 = U(joint.inverseBind._41);
		joint.inverseBind._42 = U(joint.inverseBind._42);
		joint.inverseBind._43 = U(joint.inverseBind._43);
	}
	for (assets::AnimationClipData& clip : model.clips)
		for (assets::AnimationChannelData& ch : clip.channels)
			if (ch.path == assets::ChannelPath::Translation)
				for (Vec4& value : ch.values)
					value = {U(value.x), U(value.y), U(value.z), value.w};
}

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
constexpr float kPillarOut = U(0.085f); // pillar protrusion

// Edge pillars plus the corner seals (outer cap + wall-plane backing strip).
//
// USED ONLY BY THE CLEAN BLOCK NOW. The worn blocks and both niches dropped
// their pillars when the `columns` knob was retired (2026-08-05) — plain walls,
// with pillars placed as decorations instead, so one pillar model serves all 54
// surface types rather than being baked into each. That was safe for them
// because their faces span the FULL cell and the worn displacement is pinned to
// zero at the edges (PinRamp in TextureWallWear), so they already tile
// watertight on their own.
//
// It CANNOT simply go, because the clean block is a different shape: its
// recessed panel stops at kPanelX, so the backing strip below is the only thing
// covering |x| in [kPanelX, kCellHalf] and the outer cap is the only thing
// closing the convex-corner notch. The clean set is currently unbaked/unused
// (see CLAUDE.md), so it keeps its pillars until something actually uses it.
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

	const float panelZ = -U(0.10f);  // recess depth
	const float panelX = kPanelX;
	const float borderY0 = U(0.14f), borderY1 = kWallH - U(0.14f);

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

	// Edge pillars + corner seals. The clean block is the ONE remaining user:
	// its panel stops at kPanelX, so the backing strip is what covers the rest
	// of the wall plane out to the cell edge.
	AddWallPillars(mesh);

	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{1, 1, 1, 1}, -1});
	return model;
}

// A wall NICHE: an alternate full-cell wall panel with a deep rectangular pocket
// carved into the rock. The DungeonMeshBuilder stamps this instead of the plain
// worn panel on a cell edge carrying a niche wall-feature (see wall-details.md
// Phase 2), so it shares the wall's texture/variant and chunk. A flat frame
// surrounds the opening; the pocket is a back wall + four reveals receding to
// z = -kNicheDepth (into the rock). Authored facing +Z (the room side) like
// every wall block. UVs reuse WallFaceUv so the brick/stone tiles continuously
// across the frame and into the pocket.
assets::ModelData BuildWallNiche() {
	assets::ModelData model;
	assets::MeshData mesh;

	constexpr float kNicheDepth = U(0.55f);   // pocket depth into the rock
	constexpr float px = U(0.55f);            // pocket opening half-width
	// py0 is the pocket floor — DungeonWorld::NicheItemPos rests items on it and
	// hardcodes the same 0.30, so the two must move together.
	constexpr float py0 = U(0.75f), py1 = U(1.80f); // pocket opening bottom/top
	constexpr float d = -kNicheDepth;

	auto wq = [&](const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& e,
				  const Vec3& n) {
		AddQuad(mesh, a, b, c, e, n, WallFaceUv(a, n), WallFaceUv(b, n),
				WallFaceUv(c, n), WallFaceUv(e, n));
	};

	// Flat frame around the opening (four border strips at the wall face, +Z).
	wq({-kCellHalf, 0, 0}, {kCellHalf, 0, 0}, {kCellHalf, py0, 0}, {-kCellHalf, py0, 0},
	   {0, 0, 1}); // below
	wq({-kCellHalf, py1, 0}, {kCellHalf, py1, 0}, {kCellHalf, kWallH, 0},
	   {-kCellHalf, kWallH, 0}, {0, 0, 1}); // above
	wq({-kCellHalf, py0, 0}, {-px, py0, 0}, {-px, py1, 0}, {-kCellHalf, py1, 0},
	   {0, 0, 1}); // left
	wq({px, py0, 0}, {kCellHalf, py0, 0}, {kCellHalf, py1, 0}, {px, py1, 0},
	   {0, 0, 1}); // right

	// Pocket: back wall + four reveals connecting the opening (z=0) to the back.
	wq({-px, py0, d}, {px, py0, d}, {px, py1, d}, {-px, py1, d}, {0, 0, 1}); // back
	wq({-px, py0, 0}, {px, py0, 0}, {px, py0, d}, {-px, py0, d}, {0, 1, 0});  // floor
	wq({-px, py1, d}, {px, py1, d}, {px, py1, 0}, {-px, py1, 0}, {0, -1, 0}); // ceiling
	wq({-px, py0, d}, {-px, py1, d}, {-px, py1, 0}, {-px, py0, 0}, {1, 0, 0}); // left
	wq({px, py0, 0}, {px, py1, 0}, {px, py1, d}, {px, py0, d}, {-1, 0, 0});    // right

	// No edge pillars: the frame above already spans the full cell, and the
	// plain wall it sits beside has none either since `columns` was retired.
	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{1, 1, 1, 1}, -1});
	return model;
}

// An arched wall niche: like BuildWallNiche but the pocket's top is a
// semicircular arch springing from vertical sides, with a proud KEYSTONE wedge
// at the crown. The opening for a column at x is [py0 .. archTop(x)], where
// archTop is flat-topped vertical sides below the springline and a semicircle
// above. Walls render backface-culling OFF, so only the authored normals matter.
assets::ModelData BuildWallNicheArch() {
	assets::ModelData model;
	assets::MeshData mesh;

	constexpr float kDepth = U(0.55f); // pocket depth into the rock
	constexpr float px = U(0.52f);     // opening half-width = arch radius
	// Like BuildWallNiche's py0, this pocket floor is mirrored in NicheItemPos (0.20).
	constexpr float py0 = U(0.50f);      // opening bottom
	constexpr float springY = U(1.25f);  // where the vertical sides meet the arch
	constexpr float R = px;           // semicircular arch
	constexpr float crown = springY + R;
	constexpr int N = 14;             // arch segments across the width
	const float zb = -kDepth;

	auto wq = [&](const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& e,
				  const Vec3& n) {
		AddQuad(mesh, a, b, c, e, n, WallFaceUv(a, n), WallFaceUv(b, n),
				WallFaceUv(c, n), WallFaceUv(e, n));
	};
	const auto archTop = [&](float x) {
		return springY + std::sqrt(std::max(0.0f, R * R - x * x));
	};
	const auto xAt = [&](int j) { return -px + 2.0f * px * static_cast<float>(j) / N; };

	// Frame at the wall face (+Z): the panel minus the arched opening.
	wq({-kCellHalf, 0, 0}, {kCellHalf, 0, 0}, {kCellHalf, py0, 0}, {-kCellHalf, py0, 0},
	   {0, 0, 1}); // below the opening
	wq({-kCellHalf, py0, 0}, {-px, py0, 0}, {-px, kWallH, 0}, {-kCellHalf, kWallH, 0},
	   {0, 0, 1}); // left of the opening
	wq({px, py0, 0}, {kCellHalf, py0, 0}, {kCellHalf, kWallH, 0}, {px, kWallH, 0},
	   {0, 0, 1}); // right of the opening
	for (int j = 0; j < N; ++j) {
		const float x0 = xAt(j), x1 = xAt(j + 1), a0 = archTop(x0), a1 = archTop(x1);
		// Frame above the arch, and the pocket back below it (per x-strip).
		wq({x0, a0, 0}, {x1, a1, 0}, {x1, kWallH, 0}, {x0, kWallH, 0}, {0, 0, 1});
		wq({x0, py0, zb}, {x1, py0, zb}, {x1, a1, zb}, {x0, a0, zb}, {0, 0, 1});
		// Arch soffit: the curved reveal from the front rim into the pocket. Normal
		// points inward (toward the arch centre) so it lights like a ceiling.
		const float mx = (x0 + x1) * 0.5f, my = (a0 + a1) * 0.5f;
		const float nx = -mx, ny = springY - my;
		const float inv = 1.0f / std::sqrt(nx * nx + ny * ny + 1e-4f);
		wq({x0, a0, 0}, {x1, a1, 0}, {x1, a1, zb}, {x0, a0, zb}, {nx * inv, ny * inv, 0});
	}

	// Straight reveals: the opening bottom and the two vertical sides (py0..spring).
	wq({-px, py0, 0}, {px, py0, 0}, {px, py0, zb}, {-px, py0, zb}, {0, 1, 0}); // floor
	wq({-px, py0, zb}, {-px, springY, zb}, {-px, springY, 0}, {-px, py0, 0}, {1, 0, 0});
	wq({px, py0, 0}, {px, springY, 0}, {px, springY, zb}, {px, py0, zb}, {-1, 0, 0});

	// Keystone: a proud wedge (wider at the top) centred on the crown.
	constexpr float kwb = U(0.10f), kwt = U(0.15f), ky0 = crown - U(0.12f),
					ky1 = crown + U(0.16f), kp = U(0.07f);
	wq({-kwb, ky0, kp}, {kwb, ky0, kp}, {kwt, ky1, kp}, {-kwt, ky1, kp}, {0, 0, 1}); // face
	wq({-kwb, ky0, 0}, {-kwb, ky0, kp}, {-kwt, ky1, kp}, {-kwt, ky1, 0}, {-1, 0, 0}); // left
	wq({kwb, ky0, kp}, {kwb, ky0, 0}, {kwt, ky1, 0}, {kwt, ky1, kp}, {1, 0, 0});      // right
	wq({-kwt, ky1, 0}, {-kwt, ky1, kp}, {kwt, ky1, kp}, {kwt, ky1, 0}, {0, 1, 0});    // top
	wq({-kwb, ky0, kp}, {-kwb, ky0, 0}, {kwb, ky0, 0}, {kwb, ky0, kp}, {0, -1, 0});   // bottom

	// No edge pillars — see BuildWallNiche.
	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{1, 1, 1, 1}, -1});
	return model;
}

// A wall WINDOW: a circular bore THROUGH the wall block (see-through, Phase 3).
// Unlike a niche it has NO back — the tunnel recedes to the block centre
// (kCellHalf), so the two flanking floor cells' bore panels meet into one tunnel
// through the block. A frame (the cell face minus the circle) surrounds a circular
// hole; the tunnel walls (top/bottom reveals per x-strip, closing at the sides)
// are the "sides of the hole" the see-through spell will also want. Authored
// facing +Z like every wall block; the game grants LoS/fire through it separately.
assets::ModelData BuildWallWindow() {
	assets::ModelData model;
	assets::MeshData mesh;

	constexpr float r = U(0.55f);      // hole radius
	constexpr float cy = U(1.25f);     // hole centre height (roughly eye level)
	constexpr float depth = kCellHalf; // tunnel to the block centre
	constexpr int M = 40;              // x-strips across the hole (smoother rim)
	const float zb = -depth;

	auto wq = [&](const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& e,
				  const Vec3& n) {
		AddQuad(mesh, a, b, c, e, n, WallFaceUv(a, n), WallFaceUv(b, n),
				WallFaceUv(c, n), WallFaceUv(e, n));
	};
	const auto arc = [&](float x, float sign) { // circle y at |x| (sign ±1)
		return cy + sign * std::sqrt(std::max(0.0f, r * r - x * x));
	};
	const auto xAt = [&](int j) { return -r + 2.0f * r * static_cast<float>(j) / M; };

	// Frame outside the hole's width: full-height strips left and right of it.
	wq({-kCellHalf, 0, 0}, {-r, 0, 0}, {-r, kWallH, 0}, {-kCellHalf, kWallH, 0},
	   {0, 0, 1});
	wq({r, 0, 0}, {kCellHalf, 0, 0}, {kCellHalf, kWallH, 0}, {r, kWallH, 0}, {0, 0, 1});
	for (int j = 0; j < M; ++j) {
		const float x0 = xAt(j), x1 = xAt(j + 1);
		const float b0 = arc(x0, -1.0f), b1 = arc(x1, -1.0f); // bottom of circle
		const float t0 = arc(x0, 1.0f), t1 = arc(x1, 1.0f);   // top of circle
		// Frame below and above the circle (z=0, facing the room).
		wq({x0, 0, 0}, {x1, 0, 0}, {x1, b1, 0}, {x0, b0, 0}, {0, 0, 1});
		wq({x0, t0, 0}, {x1, t1, 0}, {x1, kWallH, 0}, {x0, kWallH, 0}, {0, 0, 1});
		// Tunnel walls: the hole rim (z=0) extruded into the block (zb). Normal
		// points RADIALLY inward (toward the bore axis) so the cylinder lights
		// smoothly — a fixed up/down normal would flip hard at the sides. Uses the
		// strip midpoint's rim direction: (-xm, ±sq)/r for the bottom/top arc.
		const float xm = (x0 + x1) * 0.5f;
		const float sq = std::sqrt(std::max(0.0f, r * r - xm * xm)); // circle half-height
		const float inv = 1.0f / r;
		wq({x0, b0, 0}, {x1, b1, 0}, {x1, b1, zb}, {x0, b0, zb}, {-xm * inv, sq * inv, 0});
		wq({x0, t0, zb}, {x1, t1, zb}, {x1, t1, 0}, {x0, t0, 0}, {-xm * inv, -sq * inv, 0});
	}

	// No edge pillars — the strips above already reach both cell edges.
	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{1, 1, 1, 1}, -1});
	return model;
}

// A RECTANGULAR see-through bore — like BuildWallWindow but a straight-sided
// opening (a framed window). Frame around a rectangle + a four-walled tunnel to
// the block centre, no back. Axis-aligned normals, so no smooth-normal concerns.
assets::ModelData BuildWallWindowRect() {
	assets::ModelData model;
	assets::MeshData mesh;

	constexpr float hw = U(0.55f), hh = U(0.60f); // opening half-width / half-height
	constexpr float cy = U(1.25f), depth = kCellHalf;
	const float y0 = cy - hh, y1 = cy + hh, zb = -depth;

	auto wq = [&](const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& e,
				  const Vec3& n) {
		AddQuad(mesh, a, b, c, e, n, WallFaceUv(a, n), WallFaceUv(b, n),
				WallFaceUv(c, n), WallFaceUv(e, n));
	};
	// Frame around the opening (z=0, facing the room).
	wq({-kCellHalf, 0, 0}, {kCellHalf, 0, 0}, {kCellHalf, y0, 0}, {-kCellHalf, y0, 0},
	   {0, 0, 1}); // below
	wq({-kCellHalf, y1, 0}, {kCellHalf, y1, 0}, {kCellHalf, kWallH, 0},
	   {-kCellHalf, kWallH, 0}, {0, 0, 1}); // above
	wq({-kCellHalf, y0, 0}, {-hw, y0, 0}, {-hw, y1, 0}, {-kCellHalf, y1, 0}, {0, 0, 1}); // left
	wq({hw, y0, 0}, {kCellHalf, y0, 0}, {kCellHalf, y1, 0}, {hw, y1, 0}, {0, 0, 1});     // right
	// Tunnel: four walls from the opening edge (z=0) into the block (zb), no back.
	wq({-hw, y0, 0}, {hw, y0, 0}, {hw, y0, zb}, {-hw, y0, zb}, {0, 1, 0});  // floor (up)
	wq({-hw, y1, zb}, {hw, y1, zb}, {hw, y1, 0}, {-hw, y1, 0}, {0, -1, 0}); // ceiling (down)
	wq({-hw, y0, zb}, {-hw, y1, zb}, {-hw, y1, 0}, {-hw, y0, 0}, {1, 0, 0}); // left (+X)
	wq({hw, y0, 0}, {hw, y1, 0}, {hw, y1, zb}, {hw, y0, zb}, {-1, 0, 0});    // right (-X)

	// No edge pillars — the frame above already reaches both cell edges.
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

// How far from a block edge the displacement takes to return to the flat wall
// plane. ZERO AT THE EDGE IS NON-NEGOTIABLE — it is the one value two
// independently stamped blocks can agree on without knowing each other, and it
// is what keeps a seam closed when two different surface types meet, or a wall
// meets a floor block, or a convex corner exposes a block's side. The DISTANCE
// it takes to get there is pure aesthetics, and it trades two defects against
// each other: wide, and every cell reads as a shallow dish (a cove up each side
// and along the floor); narrow, and the return becomes a crease that risks
// reading as a grid down a long corridor.
//
// The side ramp used to be invisible. AddWallPillars covered |x| in
// [0.40, 0.50] units and stood U(0.085) proud, while the ramp only acts past
// |x| = 0.452 — so the pillars contained it entirely, and retiring the
// `columns` knob (8574f3a) exposed a curve that had always been there.
constexpr float kWallPinSide = U(0.045f); // was U(0.12f)
constexpr float kWallPinVert = U(0.040f); // was U(0.10f)

// Which of a wall panel's two SIDE edges are pinned. `left` is the panel's -X
// edge and `right` its +X (the panel is authored facing +Z; DungeonMeshBuilder
// maps those to world axes when it rotates the panel onto an edge).
//
// A side may be left OPEN only when the neighbouring panel is the same surface,
// because then the two carry the same displacement field and meet exactly — see
// the periodic-noise note in TextureWallWear. Top and bottom are ALWAYS pinned:
// the floor and ceiling blocks are different meshes displaced by different
// rules, so they can never agree with a wall no matter who its neighbours are.
struct SidePins {
	bool left = true;
	bool right = true;
};

// The bowed-masonry noise's frequency across a panel. It must be a POWER OF TWO
// so every fBm octave lands on an integer lattice period and the term can be
// made exactly periodic (Fbm's periodX) — that periodicity is what lets a side
// go unpinned. It was 1.8 while every edge was pinned and the discontinuity was
// hidden; a non-integer frequency cannot tile.
constexpr float kBowFreqU = 2.0f;
constexpr u32 kBowPeriodU = 2u; // lattice cells per panel at the first octave

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

	// Width / height of the scan. NOT always 1: eleven of the installed sets are
	// 2:1 tiles (a 4096x2048 scan holds two squares' worth of stone across and
	// one down), and the UV mapping below is otherwise isotropic, so they were
	// being squeezed into a square footprint and rendered half as wide as the
	// material actually is. Valid even when IsValid() is false — a flat height
	// map still has dimensions, and the aspect has to correct the painted
	// texture whether the relief came from the scan or from procedural wear.
	float Aspect() const {
		return m_image.width > 0 && m_image.height > 0
				   ? static_cast<float>(m_image.width) /
						 static_cast<float>(m_image.height)
				   : 1.0f;
	}

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

// Erosion depth (into the rock) for the worn wall surface, in wall-local UNIT
// coordinates (x across [-kCellHalf, kCellHalf], y up [0,kWallH]). The masonry
// PATTERN is physical — bricks are bricks whatever a square measures — so the
// field samples in metres (kRefSquare) and converts its displacement back to
// units on the way out.
float WallWearDepth(float x, float y) {
	const float u = (x + kCellHalf) * kRefSquare; // metres along the wall
	const float v = (kWallH - y) * kRefSquare;    // metres down the wall

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
	const float low = std::clamp(1.0f - y * kRefSquare, 0.0f, 1.0f) * 0.02f;

	const float depth = mortar + brick + undulation + rough + low; // metres
	// Pin to the flat plane at every block edge so seams stay closed (block-local
	// distances, so the ramp widths are units).
	const float pin = PinRamp(kCellHalf - std::fabs(x), kWallPinSide) *
					  PinRamp(y, kWallPinVert) * PinRamp(kWallH - y, kWallPinVert);
	return U(std::clamp(depth, 0.0f, 0.12f)) * pin;
}

// Height offset for the worn floor: sunken, tilted slabs with eroded joints.
float FloorWearHeight(float x, float z) {
	// Physical pattern (slabs are 1 m), so sample in metres — see WallWearDepth.
	const float u = (x + kCellHalf) * kRefSquare, v = (z + kCellHalf) * kRefSquare;
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
	h += (Fbm(u * 2.2f, v * 2.2f, 207u) - 0.5f) * 0.03f; // metres

	const float pin = PinRamp(kCellHalf - std::fabs(x), U(0.10f)) *
					  PinRamp(kCellHalf - std::fabs(z), U(0.10f));
	return U(std::clamp(h, -0.07f, 0.035f)) * pin;
}

// Erosion pockets (upward, into the rock) for the worn ceiling.
float CeilingWearDepth(float x, float z) {
	const float u = (x + kCellHalf) * kRefSquare, v = (z + kCellHalf) * kRefSquare;
	float d = std::max(0.0f, Fbm(u * 1.5f, v * 1.5f, 301u) - 0.42f) * 0.20f;
	d += (Fbm(u * 4.0f, v * 4.0f, 303u) - 0.5f) * 0.015f; // metres
	const float pin = PinRamp(kCellHalf - std::fabs(x), U(0.10f)) *
					  PinRamp(kCellHalf - std::fabs(z), U(0.10f));
	return U(std::clamp(d, 0.0f, 0.10f)) * pin;
}

// --- texture-driven wear ----------------------------------------------------
// Each field samples the matching texture's height map with the SAME UV
// mapping the mesh (and therefore the renderer) uses, so geometric relief
// lines up with the painted bricks/slabs. `relief` is the displacement
// amplitude IN METRES (it describes how deep real masonry is eroded); each
// field converts its result to units on return. du/dv pass the grid footprint
// to SampleBox. The u,v here are UV space (0..1 per cell), unaffected by the
// unit change.
//
// U IS DIVIDED BY THE TEXTURE'S ASPECT (TextureHeight::Aspect) so a non-square
// scan covers proportionally more world width per repeat instead of being
// squashed into one square. Both the mesh UVs and these fields take the same
// correction, which is the whole reason it can be applied at all — they share
// kUvScale precisely so the relief lands on the painted stones, and correcting
// only one of them would slide the two apart. `du`, the SampleBox footprint in
// u, is scaled with it.

// WHY THIS FIELD IS PERIODIC IN U, which is what makes an unpinned side legal.
// Two neighbouring panels of the same surface must agree exactly at the edge
// they share, and neither knows the other exists — so the field has to repeat
// with the cell. Each term either already did or was made to:
//   * the height-map term wraps, because SampleBox wraps (u = 1 IS u = 0);
//   * the ground-wear term depends on y alone;
//   * the bowed-masonry noise did NOT, and was the whole obstacle — a ±2.2 cm
//     step at every seam, comfortably visible against 6 cm of relief. Fbm's
//     periodX now wraps its lattice, so u = 1 and u = 0 are the same lattice
//     cell and the value AND its derivative match.
// The derivative mattering is the part worth remembering: BuildWornWallBlock
// takes its normals from this field by central difference, sampling PAST the
// panel edge. A field that is periodic hands the neighbour's own samples back,
// so the normals come out continuous across the seam for free — with a merely
// continuous field the geometry would close but the lighting would still crease.
//
// A NON-SQUARE scan reaches the same place by a different route. At aspect 2 a
// cell spans only half the image, so the map does not wrap WITHIN a cell — but
// two consecutive PHASES span u = 0..0.5 and 0.5..1.0, meeting at the same u
// and closing the loop over the pair. The noise needs nothing extra: its period
// is 1 in u and the phases partition exactly that, so a phase boundary lands
// inside the period and the final phase's far edge is the wrap. The one thing
// ruled out is a FRACTIONAL aspect, which cannot divide a repeat into whole
// cells at all — BakeWornTiers refuses those.
// `uOffset` slides the whole panel along the texture — the PHASE. A 2:1 scan
// shows only half the image per square, so if every cell used offset 0 they
// would all show the same half and nothing would join; phase p shows the slice
// starting at p/aspect, and the caller lays consecutive phases along the wall.
// It MUST be the same value BuildWornWallBlock puts in the UVs (BakeWornTiers
// computes it once and hands it to both), or the relief stops landing on the
// stones it was sampled from.
WearField TextureWallWear(const TextureHeight& height, float relief, int gridX,
						  int gridY, u32 seed, SidePins pins = {},
						  float uOffset = 0.0f) {
	const float uScale = kUvScale / height.Aspect();
	const float du = 1.0f / (gridX * height.Aspect());
	const float dv = (kWallH / gridY) * kUvScale;
	return [&height, relief, du, dv, uScale, uOffset, seed, pins](float x, float y) {
		const float u = (x + kCellHalf) * uScale + uOffset;
		const float v = (kWallH - y) * kUvScale;
		// Low texture height = recessed surface (mortar, broken bricks).
		float d = (1.0f - height.SampleBox(u, v, du, dv)) * relief; // metres
		d += (Fbm(u * kBowFreqU, v * 1.8f, seed, 4, kBowPeriodU) - 0.5f) * 0.045f;
		// Ground-level wear over the lowest metre (y is units, so scale it).
		d += std::clamp(1.0f - y * kRefSquare, 0.0f, 1.0f) * 0.018f;
		float pin = PinRamp(y, kWallPinVert) * PinRamp(kWallH - y, kWallPinVert);
		if (pins.left) pin *= PinRamp(x + kCellHalf, kWallPinSide);
		if (pins.right) pin *= PinRamp(kCellHalf - x, kWallPinSide);
		return U(std::clamp(d, 0.0f, relief + 0.05f)) * pin;
	};
}

WearField TextureFloorWear(const TextureHeight& height, float relief, int grid,
						   u32 seed) {
	const float uScale = kUvScale / height.Aspect();
	const float du = 0.5f / (grid * height.Aspect()), dv = 0.5f / grid;
	return [&height, relief, du, dv, uScale, seed](float x, float z) {
		const float u = (x + kCellHalf) * uScale, v = (z + kCellHalf) * kUvScale;
		float h = (height.SampleBox(u, v, du, dv) - 0.5f) * relief; // metres
		h += (Fbm(u * 2.2f, v * 2.2f, seed) - 0.5f) * 0.02f; // general unevenness
		const float pin = PinRamp(kCellHalf - std::fabs(x), U(0.10f)) *
						  PinRamp(kCellHalf - std::fabs(z), U(0.10f));
		return U(std::clamp(h, -0.07f, 0.05f)) * pin;
	};
}

WearField TextureCeilingWear(const TextureHeight& height, float relief, int grid,
							 u32 seed) {
	const float uScale = kUvScale / height.Aspect();
	const float du = 0.5f / (grid * height.Aspect()), dv = 0.5f / grid;
	return [&height, relief, du, dv, uScale, seed](float x, float z) {
		const float u = (x + kCellHalf) * uScale, v = (z + kCellHalf) * kUvScale;
		// Low texture height = deeper erosion pocket (upward, into the rock).
		float d = (1.0f - height.SampleBox(u, v, du, dv)) * relief; // metres
		d += (Fbm(u * 3.0f, v * 3.0f, seed) - 0.5f) * 0.015f;
		const float pin = PinRamp(kCellHalf - std::fabs(x), U(0.10f)) *
						  PinRamp(kCellHalf - std::fabs(z), U(0.10f));
		return U(std::clamp(d, 0.0f, relief)) * pin;
	};
}

// Grid resolutions are parameters: the baker emits each worn block at three
// complexity tiers (low/med/high) so the game can trade geometric detail for
// performance via the Settings menu. A flat wall (wear == 0, see BakeWornTiers)
// passes kNx = kNy = 1 so it is a bare quad, not a dense flat grid.
//
// The surface spans the FULL cell and its displacement is pinned to zero at
// every edge, so the block is watertight by itself — nothing decorative is
// added on top (the `columns` edge pillars were retired 2026-08-05; place a
// pillar decoration instead).
// `uAspect` is the texture's width/height (1 for a square scan) — the U axis is
// divided by it so a 2:1 tile spans two squares of world width per repeat
// instead of being squashed into one. It MUST match the wear field's own
// correction: the two share this mapping so the displacement lands on the
// painted stones.
assets::ModelData BuildWornWallBlock(int kNx, int kNy, const WearField& wear,
									 float uAspect, float uOffset = 0.0f) {
	assets::ModelData model;
	assets::MeshData mesh;
	const float uScale = kUvScale / uAspect;

	// Displaced surface replacing the clean block's panel/border relief.
	constexpr float kEps = U(0.02f); // finite-difference step for normals
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
			// uOffset is the panel's PHASE — the same slide the wear field above
			// was sampled through, so relief still lands on the painted stones.
			vert.uv = {(x + kCellHalf) * uScale + uOffset, (kWallH - y) * kUvScale};
			mesh.vertices.push_back(vert);
		}
	}
	const u32 stride = static_cast<u32>(kNx) + 1;
	for (u32 j = 0; j < static_cast<u32>(kNy); ++j)
		for (u32 i = 0; i < static_cast<u32>(kNx); ++i) {
			const u32 a = j * stride + i, b = a + 1, c = a + stride, d2 = c + 1;
			mesh.indices.insert(mesh.indices.end(), {a, b, d2, a, d2, c});
		}

	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{1, 1, 1, 1}, -1});
	return model;
}

assets::ModelData BuildWornFloorBlock(int kN, const WearField& wear,
									  float uAspect) {
	assets::ModelData model;
	assets::MeshData mesh;
	const float uScale = kUvScale / uAspect; // see BuildWornWallBlock

	constexpr float kEps = U(0.02f);
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
			vert.uv = {(x + kCellHalf) * uScale, (z + kCellHalf) * kUvScale};
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

assets::ModelData BuildWornCeilingBlock(int kN, const WearField& wear,
										float uAspect) {
	assets::ModelData model;
	assets::MeshData mesh;
	const float uScale = kUvScale / uAspect; // see BuildWornWallBlock

	// Authored at y=0 facing down (like the clean block); erosion goes up.
	constexpr float kEps = U(0.02f);
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
			vert.uv = {(x + kCellHalf) * uScale, (z + kCellHalf) * kUvScale};
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
// curved surfaces (the blob) keep their own natural UVs.
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
	// Authored against a wall at z=0, arm reaching into the room (+Z). Flame
	// origin (the game's light/particles) is local (0, 1.78, 0.22).
	assets::ModelData model;
	assets::MeshData mesh;
	// Iron escutcheon: a slab against the wall with a raised top/bottom frame
	// and four corner rivets, so the mount reads forged rather than a flat box.
	AddBox(mesh, {0, 1.45f, 0.015f}, {0.075f, 0.17f, 0.015f});  // back plate
	AddBox(mesh, {0, 1.615f, 0.03f}, {0.08f, 0.014f, 0.014f});  // top frame bar
	AddBox(mesh, {0, 1.285f, 0.03f}, {0.08f, 0.014f, 0.014f});  // bottom frame bar
	for (float sx : {-0.062f, 0.062f})
		for (float sy : {1.355f, 1.545f}) AddSphere(mesh, {sx, sy, 0.035f}, 0.013f, 5, 6);
	// Forged bracket arm into the room, braced by a diagonal stay underneath.
	AddStrut(mesh, {0, 1.50f, 0.03f}, {0, 1.55f, 0.20f}, 0.024f, 0.018f, 8); // arm
	AddStrut(mesh, {0, 1.30f, 0.04f}, {0, 1.52f, 0.185f}, 0.015f, 0.010f, 6); // stay
	// Flared cup cradling the torch, revolved about the cup center (z=0.22).
	AddRevolution(mesh, 0, 0.22f,
				  {{0.034f, 1.55f}, {0.05f, 1.59f}, {0.07f, 1.65f}, {0.072f, 1.67f},
				   {0.052f, 1.66f}},
				  12, true, false);
	// Torch: a tapered shaft bound in cloth at the head (the flame caps it).
	AddStrut(mesh, {0, 1.55f, 0.22f}, {0, 1.73f, 0.22f}, 0.017f, 0.021f, 8); // shaft
	AddStrut(mesh, {0, 1.71f, 0.22f}, {0, 1.79f, 0.22f}, 0.046f, 0.034f, 8); // cloth head
	TileUvs(mesh, 0.30f); // worn-iron grain repeats every 30 cm
	ScaleMeshToUnits(mesh); // metre-authored prop -> unit space
	mesh.material = 0;
	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{0.35f, 0.32f, 0.30f, 1.0f}, -1}); // dark iron
	return model;
}

assets::ModelData BuildBrazier() {
	// Tripod fire bowl, flame origin local (0, 0.72, 0).
	assets::ModelData model;
	assets::MeshData mesh;
	// Three splayed legs with out-turned claw feet, set at 120° (offset so a leg
	// doesn't sit dead-center-front).
	for (int i = 0; i < 3; ++i) {
		const float a = static_cast<float>(i) / 3.0f * 2.0f * kPi + 0.52f;
		const float ca = std::cos(a), sa = std::sin(a);
		const Vec3 foot{ca * 0.22f, 0.0f, sa * 0.22f};
		AddStrut(mesh, foot, {ca * 0.10f, 0.42f, sa * 0.10f}, 0.022f, 0.034f, 7); // leg
		AddStrut(mesh, {ca * 0.20f, 0.02f, sa * 0.20f},
				 {ca * 0.30f, 0.05f, sa * 0.30f}, 0.03f, 0.018f, 6); // claw toe
	}
	// Binding collar where the legs gather under the bowl.
	AddRevolution(mesh, 0, 0,
				  {{0.11f, 0.40f}, {0.135f, 0.43f}, {0.115f, 0.46f}}, 14, false, false);
	// Flared bowl with a thick rim.
	AddRevolution(mesh, 0, 0,
				  {{0.10f, 0.45f}, {0.17f, 0.52f}, {0.25f, 0.62f}, {0.27f, 0.66f},
				   {0.255f, 0.665f}, {0.22f, 0.64f}},
				  18, true, false);
	// Domed coal bed sitting inside the bowl.
	AddRevolution(mesh, 0, 0, {{0.0f, 0.605f}, {0.12f, 0.625f}, {0.205f, 0.635f}}, 18,
				  false, true);
	TileUvs(mesh, 0.40f); // bronze pattern repeats every 40 cm
	ScaleMeshToUnits(mesh); // metre-authored prop -> unit space
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
// `authoredInUnits` opts out of the metre conversion for a prop already built
// off kCellHalf/kWallH (the archway).
assets::ModelData FinishProp(assets::MeshData&& mesh, const Vec4& color,
							 float tileMeters = 0.6f,
							 bool authoredInUnits = false) {
	// UVs project from the authored positions, so tile BEFORE converting —
	// tileMeters then means what it says.
	TileUvs(mesh, authoredInUnits ? U(tileMeters) : tileMeters);
	if (!authoredInUnits) ScaleMeshToUnits(mesh);
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
	// AUTHORED IN UNITS: the slab has to meet the flanking walls at ±kCellHalf and
	// the ceiling at kWallH, so this prop is cell-relative and skips FinishProp's
	// metre conversion (hence the U() on the real-world dimensions).
	constexpr float kIn = U(0.62f);    // opening half-width / soffit radius
	constexpr float kOut = U(0.92f);   // voussoir ring outer radius
	constexpr float kSpring = U(1.55f); // springline height
	constexpr float kD = U(0.20f);     // wall half-thickness
	constexpr float kProud = U(0.06f); // ring relief proud of the wall face
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

	return FinishProp(std::move(mesh), {0.60f, 0.58f, 0.55f, 1.0f}, 0.6f,
					  /*authoredInUnits*/ true);
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

// Iron portcullis: thick vertical bars crossed by horizontal bands, hung from a
// top header, each bar ending in a downward spike at the floor — the classic
// dungeon gate. Authored facing +Z (blocks Z-passage like the door). Built from
// REAL bars (not an alpha cutout) so torchlight rakes true shadows through the
// gaps and the grille keeps its depth up close; procedural box build, so it
// renders CULL_NONE like the other box props (catalog authored=0). The game
// binds a worn-iron PBR set by the catalog texture id (rusted_iron).
assets::ModelData BuildPortcullis() {
	assets::MeshData mesh;
	constexpr float kHalfW = 1.05f;  // bar-field half-width (inside the 1.2 cell half)
	constexpr float kTopY = 2.32f;   // header height (just under the 2.5 ceiling)
	constexpr float kBar = 0.05f;    // vertical bar half-thickness (10 cm square iron)
	constexpr float kBarZ = 0.06f;   // bar half-depth (sits proud of the bands)
	constexpr float kSpikeY = 0.20f; // spikes occupy y in [0, kSpikeY]
	constexpr float kBandZ = -0.02f; // bands set back so the verticals read in front
	constexpr int kVerts = 8;        // vertical bars
	const float bandY[] = {0.58f, 1.28f, 1.98f};

	// Top header beam the bars hang from (spans a touch beyond the field).
	AddBox(mesh, {0, kTopY, 0}, {kHalfW + 0.06f, 0.09f, 0.075f});
	// Horizontal bands.
	for (const float by : bandY)
		AddBox(mesh, {0, by, kBandZ}, {kHalfW, 0.05f, 0.045f});
	// Vertical bars, each with a spiked foot and a rivet stud at every band.
	for (int i = 0; i < kVerts; ++i) {
		const float x = -kHalfW + static_cast<float>(i) / (kVerts - 1) * (2.0f * kHalfW);
		AddBox(mesh, {x, (kSpikeY + kTopY) * 0.5f, 0},
			   {kBar, (kTopY - kSpikeY) * 0.5f, kBarZ});            // shaft
		AddStrut(mesh, {x, kSpikeY, 0}, {x, 0.0f, 0}, kBar, 0.0f, 4); // spike
		for (const float by : bandY)
			AddBox(mesh, {x, by, kBandZ + 0.06f}, {0.03f, 0.03f, 0.02f}); // rivet
	}
	return FinishProp(std::move(mesh), {0.34f, 0.33f, 0.34f, 1.0f}, 0.30f);
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
// Coil of rope: a torus lying flat on the floor (axis = Y). The rope-strand
// texture wraps the tube once (v) and tiles many times along the loop (u) so it
// reads as twisted cord. Unlike the other props it keeps its own cylindrical
// UVs, so it does NOT pass through FinishProp's world-tile reprojection.
assets::ModelData BuildRope() {
	assets::MeshData mesh;
	constexpr float R = 0.30f;  // coil radius
	constexpr float r = 0.055f; // rope half-thickness
	constexpr int major = 56, minor = 12;
	constexpr float uRepeats = 9.0f; // strand pattern tiles around the loop
	for (int i = 0; i <= major; ++i) {
		const float phi = static_cast<float>(i) / major * 2.0f * kPi;
		const float cp = std::cos(phi), sp = std::sin(phi);
		for (int j = 0; j <= minor; ++j) {
			const float th = static_cast<float>(j) / minor * 2.0f * kPi;
			const float ct = std::cos(th), st = std::sin(th);
			assets::Vertex v;
			v.position = {(R + r * ct) * cp, r + r * st, (R + r * ct) * sp};
			v.normal = {ct * cp, st, ct * sp};
			v.uv = {static_cast<float>(i) / major * uRepeats,
					static_cast<float>(j) / minor};
			mesh.vertices.push_back(v);
		}
	}
	const u32 stride = minor + 1;
	for (u32 i = 0; i < major; ++i)
		for (u32 j = 0; j < minor; ++j) {
			const u32 a = i * stride + j, b = a + stride;
			mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
		}
	assets::ModelData model;
	ScaleMeshToUnits(mesh); // metre-authored prop -> unit space (keeps its own UVs)
	mesh.material = 0;
	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({Vec4{0.62f, 0.49f, 0.30f, 1.0f}, -1});
	return model;
}

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

// Wall-mounted cloth banner: a hanging panel on a top rail, authored
// back-against the wall (z~0) and reaching into the room (+Z) like the sconce,
// so the game's wall-mount transform hangs it on a wall (decoration record
// "wall=<dir>"). A swallowtail notch at the hem reads as heraldic cloth.
assets::ModelData BuildBanner() {
	assets::MeshData mesh;
	AddBox(mesh, {0, 2.18f, 0.05f}, {0.40f, 0.035f, 0.05f}); // top rail
	AddBox(mesh, {0, 1.55f, 0.03f}, {0.32f, 0.60f, 0.018f}); // cloth field
	for (const float xs : {-0.16f, 0.16f})                   // two tails below
		AddBox(mesh, {xs, 0.82f, 0.03f}, {0.13f, 0.13f, 0.018f});
	return FinishProp(std::move(mesh), {0.55f, 0.11f, 0.13f, 1.0f}); // crimson
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

	// 15-joint humanoid: torso (root/spine/head) plus three-joint limbs so they
	// bend — each arm is shoulder -> elbow -> wrist, each leg hip -> knee ->
	// ankle, with the hand hanging off the wrist and the foot off the ankle.
	// Indices 0/1/2 stay root/spine/head (the idle clip keys those by number).
	enum J {
		J_ROOT, J_SPINE, J_HEAD,
		J_SHL, J_ELL, J_WRL, // left arm:  shoulder, elbow, wrist
		J_SHR, J_ELR, J_WRR, // right arm
		J_HIPL, J_KNL, J_ANL, // left leg: hip, knee, ankle
		J_HIPR, J_KNR, J_ANR, // right leg
		J_COUNT
	};
	const Vec3 G[J_COUNT] = {
		{0, 1.00f, 0}, {0, 1.30f, 0}, {0, 1.70f, 0},
		{-0.20f, 1.55f, 0}, {-0.255f, 1.30f, 0}, {-0.31f, 1.04f, 0},
		{0.20f, 1.55f, 0}, {0.255f, 1.30f, 0}, {0.31f, 1.04f, 0},
		{-0.12f, 0.95f, 0}, {-0.125f, 0.50f, 0}, {-0.13f, 0.06f, 0},
		{0.12f, 0.95f, 0}, {0.125f, 0.50f, 0}, {0.13f, 0.06f, 0},
	};
	const int parent[J_COUNT] = {
		-1, J_ROOT, J_SPINE,
		J_SPINE, J_SHL, J_ELL,
		J_SPINE, J_SHR, J_ELR,
		J_ROOT, J_HIPL, J_KNL,
		J_ROOT, J_HIPR, J_KNR,
	};
	const char* names[J_COUNT] = {
		"root", "spine", "head",
		"shoulderL", "elbowL", "wristL",
		"shoulderR", "elbowR", "wristR",
		"hipL", "kneeL", "ankleL",
		"hipR", "kneeR", "ankleR",
	};
	for (int j = 0; j < J_COUNT; ++j) {
		assets::JointData joint;
		joint.name = names[j];
		joint.parent = parent[j];
		const Vec3 parentPos = parent[j] >= 0 ? G[parent[j]] : Vec3{0, 0, 0};
		joint.restTranslation = Sub(G[j], parentPos);
		joint.inverseBind = InverseBindForGlobal(G[j]);
		model.skeleton.joints.push_back(joint);
	}

	const float b = style.bulk;
	assets::MeshData mesh;
	mesh.skinned = true;
	// Torso: hips, a ribcage tapering to the shoulders, neck + skull + jaw.
	AddStrut(mesh, {0, 0.94f, 0}, {0, 1.12f, 0}, 0.15f * b, 0.135f * b, 10, J_ROOT);
	AddStrut(mesh, {0, 1.12f, 0}, {0, 1.56f, 0}, 0.135f * b, 0.175f * b, 12, J_SPINE);
	AddStrut(mesh, {0, 1.56f, 0}, {0, 1.70f, 0}, 0.05f * b, 0.06f * b, 8, J_HEAD);
	AddSphere(mesh, {0, 1.80f, 0.005f}, 0.105f * b, 10, 12, J_HEAD, {0.92f, 1.0f, 1.02f});
	AddStrut(mesh, {0, 1.73f, 0.05f}, {0, 1.69f, 0.08f}, 0.055f * b, 0.04f * b, 6, J_HEAD);
	// Segmented limbs: each bone is a tube bound to its own joint, with a small
	// ball at the elbow/wrist and knee/ankle to mask the seam where bones meet
	// (reads as a knuckle / wrapped joint and hides the gap when the joint bends).
	auto arm = [&](int sh, int el, int wr, float sx) {
		AddStrut(mesh, G[sh], G[el], 0.058f * b, 0.05f * b, 8, sh);  // upper arm
		AddStrut(mesh, G[el], G[wr], 0.05f * b, 0.038f * b, 8, el);  // forearm
		AddStrut(mesh, G[wr], {G[wr].x + sx * 0.02f, G[wr].y - 0.12f, 0.03f},
				 0.042f * b, 0.018f * b, 6, wr);                     // hand
		AddSphere(mesh, G[el], 0.05f * b, 6, 8, el);                 // elbow
		AddSphere(mesh, G[wr], 0.04f * b, 6, 8, wr);                 // wrist
	};
	arm(J_SHL, J_ELL, J_WRL, -1.0f);
	arm(J_SHR, J_ELR, J_WRR, 1.0f);
	auto leg = [&](int hip, int kn, int an) {
		AddStrut(mesh, G[hip], G[kn], 0.078f * b, 0.062f * b, 8, hip); // thigh
		AddStrut(mesh, G[kn], G[an], 0.062f * b, 0.045f * b, 8, kn);   // shin
		AddStrut(mesh, {G[an].x, 0.05f, -0.02f}, {G[an].x, 0.035f, 0.13f},
				 0.052f, 0.04f, 6, an);                               // foot
		AddSphere(mesh, G[kn], 0.064f * b, 6, 8, kn);                 // knee
		AddSphere(mesh, G[an], 0.05f * b, 6, 8, an);                  // ankle
	};
	leg(J_HIPL, J_KNL, J_ANL);
	leg(J_HIPR, J_KNR, J_ANR);
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
	{ // shoulder sway (anti-phase) around the base raise
		const int sh[2] = {J_SHL, J_SHR};
		for (int s = 0; s < 2; ++s) {
			assets::AnimationChannelData ch;
			ch.joint = sh[s];
			ch.path = assets::ChannelPath::Rotation;
			times(ch);
			const float sign = s == 0 ? 1.0f : -1.0f;
			for (int k = 0; k < kKeys; ++k) {
				const Quat q = QuatFromEuler(
					-style.armRaise + sign * style.swing * std::sin(phase(k)), 0, 0);
				ch.values.push_back({q.x, q.y, q.z, q.w});
			}
			clip.channels.push_back(std::move(ch));
		}
	}
	{ // a constant slight elbow bend so the arms aren't ramrod straight
		const int el[2] = {J_ELL, J_ELR};
		for (int s = 0; s < 2; ++s) {
			assets::AnimationChannelData ch;
			ch.joint = el[s];
			ch.path = assets::ChannelPath::Rotation;
			times(ch);
			for (int k = 0; k < kKeys; ++k) {
				const Quat q = QuatFromEuler(0.25f + 0.06f * std::sin(phase(k)), 0, 0);
				ch.values.push_back({q.x, q.y, q.z, q.w});
			}
			clip.channels.push_back(std::move(ch));
		}
	}
	model.clips.push_back(std::move(clip));

	// walk / attack / die clips. Authored on the 15-joint rig and consumed by
	// DungeonWorld's monster animation state machine (walk loops over the chase
	// glide; attack fires per swing; die plays once on slay, then the corpse
	// vanishes). Two small builders sample a 0..1 phase across the clip.
	auto rotChan = [](assets::AnimationClipData& c, int joint, int keys, auto&& f) {
		assets::AnimationChannelData ch;
		ch.joint = joint;
		ch.path = assets::ChannelPath::Rotation;
		for (int k = 0; k < keys; ++k) {
			const float u = static_cast<float>(k) / (keys - 1);
			ch.times.push_back(c.duration * u);
			const Quat q = f(u);
			ch.values.push_back({q.x, q.y, q.z, q.w});
		}
		c.channels.push_back(std::move(ch));
	};
	auto rootY = [](assets::AnimationClipData& c, int keys, auto&& f) {
		assets::AnimationChannelData ch;
		ch.joint = 0;
		ch.path = assets::ChannelPath::Translation;
		for (int k = 0; k < keys; ++k) {
			const float u = static_cast<float>(k) / (keys - 1);
			ch.times.push_back(c.duration * u);
			ch.values.push_back({0, f(u), 0, 0});
		}
		c.channels.push_back(std::move(ch));
	};
	const float raise = style.armRaise;

	{ // walk: hips stride anti-phase, knees flex through the swing, arms counter-
	  // swing at the shoulders with the elbows bent, body double-bob. Loops.
		assets::AnimationClipData walk;
		walk.name = "walk";
		walk.duration = 0.72f;
		constexpr int K = 21;
		const float tau = 2.0f * kPi;
		rotChan(walk, J_HIPL, K, [&](float u) { return QuatFromEuler(0.62f * std::sin(tau * u), 0, 0); });
		rotChan(walk, J_HIPR, K, [&](float u) { return QuatFromEuler(0.62f * std::sin(tau * u + kPi), 0, 0); });
		rotChan(walk, J_KNL, K, [&](float u) { return QuatFromEuler(1.05f * (0.5f - 0.5f * std::cos(tau * u)), 0, 0); });
		rotChan(walk, J_KNR, K, [&](float u) { return QuatFromEuler(1.05f * (0.5f - 0.5f * std::cos(tau * u + kPi)), 0, 0); });
		rotChan(walk, J_SHL, K, [&](float u) { return QuatFromEuler(-raise + 0.5f * std::sin(tau * u + kPi), 0, 0); });
		rotChan(walk, J_SHR, K, [&](float u) { return QuatFromEuler(-raise + 0.5f * std::sin(tau * u), 0, 0); });
		rotChan(walk, J_ELL, K, [&](float u) { return QuatFromEuler(0.4f + 0.25f * std::sin(tau * u + kPi), 0, 0); });
		rotChan(walk, J_ELR, K, [&](float u) { return QuatFromEuler(0.4f + 0.25f * std::sin(tau * u), 0, 0); });
		rootY(walk, K, [&](float u) { return 1.0f + 0.045f * std::sin(2.0f * tau * u); });
		model.clips.push_back(std::move(walk));
	}

	{ // attack: rear the right arm overhead (wind-up), then drive a big forward-
	  // reaching overhand strike — the arm arcs down THROUGH forward (+Z, toward
	  // the party) to a forward-down reach, body leaning in. One-shot. Shoulder
	  // pitch about X: 0 = arm down, ~-1.57 = arm forward, ~-2.6 = overhead, so
	  // the swing sweeps -2.6 -> -0.5 right past the target. Slower (0.65s) so the
	  // commit reads instead of a twitch.
		assets::AnimationClipData atk;
		atk.name = "attack";
		atk.duration = 0.65f;
		constexpr int K = 17;
		auto L = [](float a, float b, float t) { return a + (b - a) * t; };
		rotChan(atk, J_SHR, K, [&](float u) {
			float p;
			if (u < 0.35f)      p = L(-raise, -2.6f, u / 0.35f);             // rear overhead
			else if (u < 0.55f) p = L(-2.6f, -0.5f, (u - 0.35f) / 0.20f);    // strike down through forward
			else                p = L(-0.5f, -raise, (u - 0.55f) / 0.45f);   // settle
			return QuatFromEuler(p, 0, 0);
		});
		rotChan(atk, J_ELR, K, [&](float u) {
			float b;
			if (u < 0.35f)      b = L(0.3f, 1.5f, u / 0.35f);            // cock the elbow on the wind-up
			else if (u < 0.55f) b = L(1.5f, 0.15f, (u - 0.35f) / 0.20f); // snap straight, reaching, through the strike
			else                b = L(0.15f, 0.3f, (u - 0.55f) / 0.45f); // settle
			return QuatFromEuler(b, 0, 0);
		});
		// spine leans forward into the strike (peaks at the chop, recovers).
		rotChan(atk, J_SPINE, K, [&](float u) { return QuatFromEuler(0.5f * std::sin(u * kPi), 0, 0); });
		{ // root lunges forward into the strike — model +Z is the facing dir, so
		  // the body steps toward the party as the arm comes down, then recovers.
			assets::AnimationChannelData ch;
			ch.joint = J_ROOT;
			ch.path = assets::ChannelPath::Translation;
			for (int k = 0; k < K; ++k) {
				const float u = static_cast<float>(k) / (K - 1);
				ch.times.push_back(atk.duration * u);
				float z;
				if (u < 0.35f)      z = L(0.0f, -0.05f, u / 0.35f);          // tiny anticipation back
				else if (u < 0.55f) z = L(-0.05f, 0.22f, (u - 0.35f) / 0.20f); // drive forward
				else                z = L(0.22f, 0.0f, (u - 0.55f) / 0.45f);   // recover
				ch.values.push_back({0, 1.0f, z, 0});
			}
			atk.channels.push_back(std::move(ch));
		}
		model.clips.push_back(std::move(atk));
	}

	{ // die: root sinks and topples forward, spine slumps, arms go limp at the
	  // shoulders + elbows, knees buckle. One-shot, holds the heap.
		assets::AnimationClipData die;
		die.name = "die";
		die.duration = 0.9f;
		constexpr int K = 15;
		rootY(die, K, [&](float u) { return 1.0f - 0.78f * (u * u); });                   // sink (accelerating)
		rotChan(die, J_ROOT, K, [&](float u) { return QuatFromEuler(1.45f * (u * u), 0, 0); });  // topple forward
		rotChan(die, J_SPINE, K, [&](float u) { return QuatFromEuler(0.5f * (u * u), 0, 0); });  // spine slump
		rotChan(die, J_SHL, K, [&](float u) { return QuatFromEuler(-raise * (1.0f - u * u) + 0.3f * (u * u), 0, 0); });
		rotChan(die, J_SHR, K, [&](float u) { return QuatFromEuler(-raise * (1.0f - u * u) + 0.3f * (u * u), 0, 0); });
		rotChan(die, J_ELL, K, [&](float u) { return QuatFromEuler(0.3f + 0.6f * (u * u), 0, 0); }); // elbows go limp
		rotChan(die, J_ELR, K, [&](float u) { return QuatFromEuler(0.3f + 0.6f * (u * u), 0, 0); });
		rotChan(die, J_KNL, K, [&](float u) { return QuatFromEuler(0.9f * (u * u), 0, 0); });        // knees buckle
		rotChan(die, J_KNR, K, [&](float u) { return QuatFromEuler(0.9f * (u * u), 0, 0); });
		model.clips.push_back(std::move(die));
	}

	// Metre-authored creature (a ~1.7 m humanoid) -> unit space, rig and clips
	// included. Done last so the joint globals above stay readable in metres.
	ScaleModelToUnits(model);
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

	// Lumpy droplet sitting on the floor, weights blending base->top by height.
	// A few low-frequency lobes perturb the radius so the silhouette reads as a
	// gelatinous blob rather than a perfect ball (the slime normal map carries
	// the fine detail, so the base sphere normal is kept).
	constexpr float kRadius = 0.42f;
	constexpr int kLat = 16, kLon = 22;
	auto lump = [](const Vec3& n) {
		return 1.0f + 0.10f * std::sin(n.x * 4.0f + 1.3f) * std::cos(n.z * 3.0f) +
			   0.06f * std::sin(n.y * 5.0f + 2.1f);
	};
	assets::MeshData mesh;
	mesh.skinned = true;
	for (int lat = 0; lat <= kLat; ++lat) {
		const float theta = kPi * static_cast<float>(lat) / kLat; // 0 = top pole
		for (int lon = 0; lon <= kLon; ++lon) {
			const float phi = 2.0f * kPi * static_cast<float>(lon) / kLon;
			const Vec3 n{std::sin(theta) * std::cos(phi), std::cos(theta),
						 std::sin(theta) * std::sin(phi)};
			const float r = kRadius * lump(n);
			assets::Vertex v;
			v.position = {n.x * r, kRadius + n.y * r, n.z * r};
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

	// walk / attack / die for the blob — pure squash-and-stretch (+ a base hop),
	// since the rig is just base/top and the blob never turns to face. Consumed
	// by the monster animation state machine the same way as the humanoid clips.
	const float tau = 2.0f * kPi;
	// Volume-ish squash from a single width signal a (a>0 = wide + short).
	auto squash = [](float a) -> Vec4 { return {1.0f + a, 1.0f - 1.25f * a, 1.0f + a, 0}; };
	// Explicit-keyframe channel (non-uniform times are fine; the sampler brackets).
	auto chanKeys = [](assets::AnimationClipData& c, int joint, assets::ChannelPath path,
					   std::initializer_list<std::pair<float, Vec4>> keys) {
		assets::AnimationChannelData ch;
		ch.joint = joint;
		ch.path = path;
		for (const auto& [t, v] : keys) { ch.times.push_back(c.duration * t); ch.values.push_back(v); }
		c.channels.push_back(std::move(ch));
	};

	{ // walk: a bouncing ooze — squashed + low, then tall + hopped up. Loops.
		assets::AnimationClipData walk;
		walk.name = "walk";
		walk.duration = 0.6f;
		constexpr int K = 21;
		auto scaleCh = [&](int joint, float amp, float ph) {
			assets::AnimationChannelData ch;
			ch.joint = joint;
			ch.path = assets::ChannelPath::Scale;
			for (int k = 0; k < K; ++k) {
				const float u = static_cast<float>(k) / (K - 1);
				ch.times.push_back(walk.duration * u);
				ch.values.push_back(squash(amp * std::cos(tau * u + ph)));
			}
			walk.channels.push_back(std::move(ch));
		};
		scaleCh(0, 0.20f, 0.0f);
		scaleCh(1, 0.17f, -0.6f); // top lags the base -> jelly follow-through
		{ // base hop around the 0.18 rest height; peaks when stretched (u=0.5)
			assets::AnimationChannelData ch;
			ch.joint = 0;
			ch.path = assets::ChannelPath::Translation;
			for (int k = 0; k < K; ++k) {
				const float u = static_cast<float>(k) / (K - 1);
				ch.times.push_back(walk.duration * u);
				ch.values.push_back({0, 0.18f + 0.08f * (0.5f - 0.5f * std::cos(tau * u)), 0, 0});
			}
			walk.channels.push_back(std::move(ch));
		}
		model.clips.push_back(std::move(walk));
	}

	{ // attack: gather, rear up tall, then slam flat — a vertical pounce. One-shot.
		assets::AnimationClipData atk;
		atk.name = "attack";
		atk.duration = 0.5f;
		chanKeys(atk, 0, assets::ChannelPath::Scale,
				 {{0.0f, {1, 1, 1, 0}}, {0.18f, {1.25f, 0.70f, 1.25f, 0}},
				  {0.40f, {0.72f, 1.5f, 0.72f, 0}}, {0.60f, {1.45f, 0.55f, 1.45f, 0}},
				  {0.80f, {0.94f, 1.08f, 0.94f, 0}}, {1.0f, {1, 1, 1, 0}}});
		chanKeys(atk, 0, assets::ChannelPath::Translation,
				 {{0.0f, {0, 0.18f, 0, 0}}, {0.18f, {0, 0.12f, 0, 0}},
				  {0.40f, {0, 0.34f, 0, 0}}, {0.60f, {0, 0.07f, 0, 0}},
				  {0.80f, {0, 0.20f, 0, 0}}, {1.0f, {0, 0.18f, 0, 0}}});
		chanKeys(atk, 1, assets::ChannelPath::Scale,
				 {{0.0f, {1, 1, 1, 0}}, {0.45f, {0.85f, 1.25f, 0.85f, 0}},
				  {0.65f, {1.25f, 0.72f, 1.25f, 0}}, {1.0f, {1, 1, 1, 0}}});
		model.clips.push_back(std::move(atk));
	}

	{ // die: deflate — spread wide and flat, the top collapsing into the base,
	  // sinking to a puddle. One-shot, holds the puddle.
		assets::AnimationClipData die;
		die.name = "die";
		die.duration = 0.85f;
		chanKeys(die, 0, assets::ChannelPath::Scale,
				 {{0.0f, {1, 1, 1, 0}}, {0.30f, {1.18f, 0.66f, 1.18f, 0}},
				  {0.65f, {1.5f, 0.30f, 1.5f, 0}}, {1.0f, {1.7f, 0.16f, 1.7f, 0}}});
		chanKeys(die, 0, assets::ChannelPath::Translation,
				 {{0.0f, {0, 0.18f, 0, 0}}, {1.0f, {0, 0.05f, 0, 0}}});
		chanKeys(die, 1, assets::ChannelPath::Scale,
				 {{0.0f, {1, 1, 1, 0}}, {1.0f, {1.4f, 0.25f, 1.4f, 0}}});
		chanKeys(die, 1, assets::ChannelPath::Translation, // top sinks onto the base
				 {{0.0f, {0, 0.45f, 0, 0}}, {1.0f, {0, 0.10f, 0, 0}}});
		model.clips.push_back(std::move(die));
	}

	ScaleModelToUnits(model); // metre-authored creature -> unit space
	return model;
}

// A stone staircase climbing a FULL STOREY and continuing up through a hole in
// the ceiling — the exit to the level above, and the mirror of BuildStairsDown.
//
// It used to be a 7-step stub 1.19 m tall in a 2.5 m room, and stairs_up
// carried `hole = none`, so a solid ceiling capped it: steps that went nowhere.
// The catalog now gives it `hole = ceiling` and this mesh brings the shaft, the
// same way BuildPitCeiling does for a pit's upper half.
//
// THE PITCH IS FORCED BY THE GRID. Climbing kWallH inside ONE cell means
// rise == run == cell/steps — 45 degrees. Steep for a real building, normal for
// the genre, and unavoidable here: a gentler flight needs two cells, and the
// other way to buy height in a single square is to turn it into a spiral.
assets::ModelData BuildStairs() {
	assets::MeshData mesh;
	constexpr int kSteps = 10;
	constexpr float kHalfCell = Metres(kCellHalf), kCeil = Metres(kWallH);
	constexpr float kRise = kCeil / kSteps;             // 0.25 m — tops out flush
	constexpr float kRun = (2.0f * kHalfCell) / kSteps; // 0.25 m — fills the cell
	constexpr float kHalfX = 0.85f;  // flight half-width
	constexpr float kShaft = 1.30f;  // stairwell height above the ceiling
	constexpr float kSlab = 0.04f;   // collar thickness

	// Rising flight, front (+Z, the room side) to back. Each step is a column
	// down to the floor, so the flight is solid underneath rather than a
	// floating ribbon — same construction as the descending twin.
	for (int i = 0; i < kSteps; ++i) {
		const float top = (i + 1) * kRise;
		const float zc = kHalfCell - kRun * 0.5f - i * kRun;
		AddBox(mesh, {0.0f, top * 0.5f, zc}, {kHalfX, top * 0.5f, kRun * 0.5f});
	}

	// The ceiling block is skipped for the whole cell, so this mesh closes
	// everything the stairwell does NOT open. HEADROOM sets the opening: a
	// climber has kCeil minus the tread height, so from the third step (0.75 m)
	// on there is under 2 m of clearance and the ceiling has to be gone. The
	// hole therefore runs from two treads in, all the way to the back wall,
	// leaving a narrow front lip and a strip either side of the flight.
	constexpr float zOpen = kHalfCell - 2.0f * kRun; // front edge of the opening
	constexpr float openMidZ = (zOpen - kHalfCell) * 0.5f;
	constexpr float openHalfZ = (zOpen + kHalfCell) * 0.5f;
	const float collarY = kCeil + kSlab;
	AddBox(mesh, {0.0f, collarY, (zOpen + kHalfCell) * 0.5f},
		   {kHalfCell, kSlab, (kHalfCell - zOpen) * 0.5f}); // lip at the front
	for (const float side : {-1.0f, 1.0f})
		AddBox(mesh, {side * (kHalfX + kHalfCell) * 0.5f, collarY, openMidZ},
			   {(kHalfCell - kHalfX) * 0.5f, kSlab, openHalfZ}); // beside the flight

	// The stairwell above: four walls around the opening and a cap, standing in
	// for the underside of the next level's stairwell.
	//
	// NO FURTHER TREADS. The flight reaches the ceiling hard against the back
	// wall, so there is nowhere left in THIS cell for the climb to carry on —
	// the next flight belongs to the cell on the level above. An earlier cut put
	// three treads in the middle of the shaft, which read as a second flight
	// floating free of the first, because that is exactly what it was.
	const float wallY = kCeil + kShaft * 0.5f;
	for (const float side : {-1.0f, 1.0f})
		AddBox(mesh, {side * (kHalfX + 0.05f), wallY, openMidZ},
			   {0.05f, kShaft * 0.5f, openHalfZ});
	AddBox(mesh, {0.0f, wallY, -(kHalfCell - 0.05f)},
		   {kHalfX + 0.1f, kShaft * 0.5f, 0.05f}); // back
	AddBox(mesh, {0.0f, wallY, zOpen + 0.05f},
		   {kHalfX + 0.1f, kShaft * 0.5f, 0.05f}); // front, over the lip
	AddBox(mesh, {0.0f, kCeil + kShaft - kSlab, openMidZ},
		   {kHalfX + 0.1f, kSlab, openHalfZ});     // cap, so you cannot see out

	return FinishProp(std::move(mesh), {0.55f, 0.53f, 0.50f, 1.0f});
}

// The DOWN counterpart: a flight descending BELOW GRADE into a stone stairwell
// shaft. The game skips the floor block on a down-stair's cell (DungeonWorld's
// floor-hole predicate), so this mesh is self-contained: flush collar slabs
// beside the narrower opening, shaft side/back walls, the descending column
// flight (the mirror of BuildStairs' rising columns, entering flush at the
// cell's front edge), and a landing at the bottom. Everything hangs from the
// prop origin at y=0 = floor level.
assets::ModelData BuildStairsDown() {
	assets::MeshData mesh;
	constexpr int kSteps = 7;
	constexpr float kRise = 0.17f, kRun = 0.24f, kHalfX = 0.85f;
	// Metre-authored like every prop, but this dimension is CELL-derived — take it
	// from kCellHalf so the collar lands exactly on the cell edge after FinishProp
	// converts back to units.
	constexpr float kHalfCell = Metres(kCellHalf); // the prop fills its cell
	const float depth = kSteps * kRise + 0.08f; // shaft bottom below grade

	// Descending flight: step i's tread at -i*kRise, each a full column down to
	// the shaft bottom. Step 0 sits flush with the floor at the front edge, so
	// the party appears to walk straight onto the top of the flight.
	for (int i = 0; i < kSteps; ++i) {
		const float top = -i * kRise;
		const float zc = kHalfCell - kRun * 0.5f - i * kRun;
		AddBox(mesh, {0.0f, (top - depth) * 0.5f, zc},
			   {kHalfX, (top + depth) * 0.5f, kRun * 0.5f});
	}
	// Landing past the flight, at the bottom of the shaft.
	const float zLandFront = kHalfCell - kSteps * kRun;
	AddBox(mesh, {0.0f, -depth + 0.04f, (zLandFront - kHalfCell) * 0.5f},
		   {kHalfX, 0.04f, (zLandFront + kHalfCell) * 0.5f});
	// Shaft walls: the back, and the two sides with their inner faces at the
	// opening's edges (under the collar).
	AddBox(mesh, {0.0f, -depth * 0.5f, -kHalfCell + 0.05f},
		   {kHalfCell, depth * 0.5f, 0.05f});
	AddBox(mesh, {-(kHalfX + 0.05f), -depth * 0.5f, 0.0f},
		   {0.05f, depth * 0.5f, kHalfCell});
	AddBox(mesh, {kHalfX + 0.05f, -depth * 0.5f, 0.0f},
		   {0.05f, depth * 0.5f, kHalfCell});
	// Collar: flush slabs covering the cell strips beside the opening, so the
	// skipped floor block reads as a neat stairwell narrower than the cell.
	AddBox(mesh, {(kHalfX + kHalfCell) * 0.5f, -0.04f, 0.0f},
		   {(kHalfCell - kHalfX) * 0.5f, 0.04f, kHalfCell});
	AddBox(mesh, {-(kHalfX + kHalfCell) * 0.5f, -0.04f, 0.0f},
		   {(kHalfCell - kHalfX) * 0.5f, 0.04f, kHalfCell});
	return FinishProp(std::move(mesh), {0.50f, 0.48f, 0.45f, 1.0f});
}

// A pit: an open shaft dropping a full storey to the level below (which gets
// the paired ceiling hole). Same self-contained construction as
// BuildStairsDown — flush collar slabs around a narrower opening, shaft walls
// with their inner faces at the opening's edges — but no flight: a sheer drop
// to a floor slab ~one wall-height down (the level below's floor, faked).
assets::ModelData BuildPit() {
	assets::MeshData mesh;
	// kHalfCell / kDepth are cell-derived (see BuildStairsDown); the rest is metres.
	constexpr float kOpen = 0.85f, kHalfCell = Metres(kCellHalf), kDepth = Metres(kWallH);
	// Collar: two full-length x strips, two z strips between them.
	AddBox(mesh, {(kOpen + kHalfCell) * 0.5f, -0.04f, 0.0f},
		   {(kHalfCell - kOpen) * 0.5f, 0.04f, kHalfCell});
	AddBox(mesh, {-(kOpen + kHalfCell) * 0.5f, -0.04f, 0.0f},
		   {(kHalfCell - kOpen) * 0.5f, 0.04f, kHalfCell});
	AddBox(mesh, {0.0f, -0.04f, (kOpen + kHalfCell) * 0.5f},
		   {kOpen, 0.04f, (kHalfCell - kOpen) * 0.5f});
	AddBox(mesh, {0.0f, -0.04f, -(kOpen + kHalfCell) * 0.5f},
		   {kOpen, 0.04f, (kHalfCell - kOpen) * 0.5f});
	// Shaft walls + the floor a storey down.
	AddBox(mesh, {-(kOpen + 0.05f), -kDepth * 0.5f, 0.0f},
		   {0.05f, kDepth * 0.5f, kOpen + 0.1f});
	AddBox(mesh, {kOpen + 0.05f, -kDepth * 0.5f, 0.0f},
		   {0.05f, kDepth * 0.5f, kOpen + 0.1f});
	AddBox(mesh, {0.0f, -kDepth * 0.5f, -(kOpen + 0.05f)},
		   {kOpen + 0.1f, kDepth * 0.5f, 0.05f});
	AddBox(mesh, {0.0f, -kDepth * 0.5f, kOpen + 0.05f},
		   {kOpen + 0.1f, kDepth * 0.5f, 0.05f});
	AddBox(mesh, {0.0f, -kDepth + 0.04f, 0.0f}, {kOpen, 0.04f, kOpen});
	return FinishProp(std::move(mesh), {0.42f, 0.40f, 0.38f, 1.0f});
}

// The pit's lower half, placed on the level BELOW: a hole in the ceiling. The
// ceiling block on its cell is skipped (CellHolesFn), and this mesh brings the
// mirrored shaft: collar slabs seated on the ceiling plane, walls rising a
// storey, and a cap (the faked underside of the level above). Origin stays at
// y=0 like every prop — the geometry lives up at ceiling height.
assets::ModelData BuildPitCeiling() {
	assets::MeshData mesh;
	constexpr float kOpen = 0.85f, kHalfCell = Metres(kCellHalf), kCeil = Metres(kWallH),
					kRise = 2.3f;
	const float collarY = kCeil + 0.04f; // slab undersides flush with the ceiling
	AddBox(mesh, {(kOpen + kHalfCell) * 0.5f, collarY, 0.0f},
		   {(kHalfCell - kOpen) * 0.5f, 0.04f, kHalfCell});
	AddBox(mesh, {-(kOpen + kHalfCell) * 0.5f, collarY, 0.0f},
		   {(kHalfCell - kOpen) * 0.5f, 0.04f, kHalfCell});
	AddBox(mesh, {0.0f, collarY, (kOpen + kHalfCell) * 0.5f},
		   {kOpen, 0.04f, (kHalfCell - kOpen) * 0.5f});
	AddBox(mesh, {0.0f, collarY, -(kOpen + kHalfCell) * 0.5f},
		   {kOpen, 0.04f, (kHalfCell - kOpen) * 0.5f});
	const float wallY = kCeil + kRise * 0.5f;
	AddBox(mesh, {-(kOpen + 0.05f), wallY, 0.0f}, {0.05f, kRise * 0.5f, kOpen + 0.1f});
	AddBox(mesh, {kOpen + 0.05f, wallY, 0.0f}, {0.05f, kRise * 0.5f, kOpen + 0.1f});
	AddBox(mesh, {0.0f, wallY, -(kOpen + 0.05f)}, {kOpen + 0.1f, kRise * 0.5f, 0.05f});
	AddBox(mesh, {0.0f, wallY, kOpen + 0.05f}, {kOpen + 0.1f, kRise * 0.5f, 0.05f});
	AddBox(mesh, {0.0f, kCeil + kRise - 0.04f, 0.0f}, {kOpen, 0.04f, kOpen});
	return FinishProp(std::move(mesh), {0.42f, 0.40f, 0.38f, 1.0f});
}

// RETIRED (2026-08-07): the doorway frame is authored in tools/BuildDoorFrame.py
// now and imported, so it must NOT be baked here — `AssetBaker models` would
// overwrite the imported door_frame.gltf with three boxes again. It was two
// posts and a lintel; the authored one is BuildWallArch's construction with a
// square head: a slab whose opening is built from panels rather than booleaned,
// jamb courses laid as separate stones with real mortar gaps, and a monolithic
// lintel oversailing both jambs. See BuildLever's note above for the same move.
//
// THE OPENING IS A CONTRACT between that script and the leaves below: the
// script asserts OPEN 0.34 x DOOR_H 0.84 units, which is kLeafHalfW 0.85 m and
// kLeafH 2.1 m over kUnit. Change a leaf's size and the frame must follow.

// UV tile for the door leaves, and it is NOT the 0.6 m the other props take.
// A prop's tile has to be read off the TEXTURE, not chosen for the prop: the
// scans here are wall-scale, of a whole boarded or panelled surface, so 0.6 m
// crammed a full fence into two thirds of a metre and the door came out striped
// in 1.3 cm slivers — "vertical lollipop sticks". Any future leaf texture wants
// the same sum done.
//
// The sum was done by COUNTING and got the count wrong: `wood_planks_old8` was
// read as 16 boards across a square scan, which made 2.7 m sound like 17 cm
// boards. It has ten (measured — column minima, confirmed by autocorrelation at
// 101.5 px on 1024), so this laid 27 cm ones. The wooden leaf that mattered has
// since moved to tools/BuildDoorLeaf.py, which reads the seam table off the
// scan instead of assuming a pitch, and what is left here wears metal or
// granite rather than wood — so the number stands, but not the reasoning.
constexpr float kDoorTile = 2.7f;

// The leaf, and the pull sunk into its meeting edge. The pull now belongs
// entirely to the wooden leaf — tools/BuildDoorLeaf.py cuts it into the meeting
// stile — so these are the SOURCE of that script's unit-space PULL_* copies
// rather than a shape cut in two places. kPullY is still read here, by the
// middle strap, which is level with the hand the pull is for.
constexpr float kLeafHalfW = 0.85f, kLeafH = 2.1f, kLeafHalfT = 0.05f;
constexpr float kPullY = 1.05f;        // hand height on a 2.1 m leaf
[[maybe_unused]]
constexpr float kPullHalfH = 0.075f;   // 15 cm tall — a hand, not a fingertip
constexpr float kPullHalfW = 0.045f;   // half a band bar's width

// NO SHIPPED DOOR WEARS THIS ANY MORE (2026-08-08). It was the stand-in leaf
// for every type before any was authored, and the last of them — the stone door
// — moved to tools/BuildStoneSlab.py. It is kept deliberately, because a door
// type created in the editor needs SOME model on the day it is made, and a
// plain slab at exactly the frame's opening is the right thing to hand it. Its
// old comment follows, and still explains why it carries nothing.
//
// The door panel filling the frame's opening. A PLAIN slab: it used to carry
// two cross braces "so it reads as a built door rather than a plain slab", and
// they were the wrong answer twice over. A brace in the leaf's own wood texture
// reads as a scar across the boards, not as a strap — and a strap is IRON, so
// it belongs in the `trim` model that carries its own texture, not here. With
// the tile read off the texture (kDoorTile) the boards carry the door on their
// own. Slides sideways (+X in authored space, into the neighbouring wall).
assets::ModelData BuildDoorPanel() {
	assets::MeshData mesh;
	constexpr float kOpen = 0.85f, kH = 2.1f;
	AddBox(mesh, {0.0f, kH * 0.5f, 0.0f}, {kOpen, kH * 0.5f, 0.05f});
	return FinishProp(std::move(mesh), {0.45f, 0.33f, 0.20f, 1.0f}, kDoorTile);
}

// RETIRED (2026-08-08): the LEFT HALF of a split door's leaf is authored in
// tools/BuildDoorLeaf.py now and imported, so it must NOT be baked here —
// `AssetBaker models` would overwrite the imported door_panel_half.gltf with
// four boxes again. It was a flat slab with a pocket in it; the authored one is
// five real boards with grooves between them, each crowned and set a little
// proud of its neighbours, and each wearing ONE board of the scan so the
// painted seam falls in the modelled groove. Same move as BuildDoorFrame and
// BuildLever above.
//
// The kPull* constants below are still live — the trim band's pocket is cut to
// the same rect as the leaf's, and a disagreement between them would show as a
// ledge — and the Python holds their unit-space copies with that noted.
//
// TWO THINGS THAT SCRIPT INHERITED and must keep, both learned here:
// A HALF LEAF MUST REACH THE CENTRE EXACTLY. The braces this once carried were
// inset to 96% of the half width, which on a single panel is an unremarkable
// margin — but on a split door the two insets meet, leaving a 3.4 cm notch at
// the centre line with the leaf face recessed behind it. It read as a small
// downward arrow stamped on the door. Anything a split leaf carries has to run
// out to x = 0 or stop well short of it; a near miss is what shows.
// THE PULL HAS TO CUT THE LEAF, not just the band. Cut into the band alone it
// was 1.5 cm deep — the band's proud height and nothing more — and invisible:
// the meeting stile is the nearest surface to a torch carried at the eye, so it
// is the BRIGHTEST thing on the door, and a 1.5 cm step throws no shadow
// against a hotspot. A recess has to be deep enough to go dark. Cutting the
// leaf too takes it to 4.7 cm.

// --- door trim ---------------------------------------------------------------
// A leaf's IRONWORK, drawn with the leaf through the door type's `trim` field.
// A trim exists for exactly ONE reason: a second MATERIAL. The import path
// binds one texture set per model, so a wooden door's straps cannot be iron
// inside a wood-textured mesh. A leaf that is all one material needs no trim
// at all — which is why the portcullis lost its.
//
// The TEXTURE these wear matters as much as the tile (doors.cat carries the
// reasoning): a strap is a 9 cm bar, so its set must have no feature bigger
// than that, or every bar picks up a different part of the scan and the strap
// looks assembled from offcuts.
//
// RETIRED (2026-08-08): AddBandRect and BuildDoorBand laid a rectangular band —
// uprights down both edges, rails top and bottom — around a WHOLE leaf. Its
// only user was the portcullis's `trim`, and a portcullis is now one authored
// model (tools/BuildPortcullis.py) carrying its own beam, rails and rivets.

// --- the plank door's ironwork: HINGE STRAPS ---------------------------------
// This was a picture frame too — uprights down both edges, rails top and
// bottom — and its INNER upright was the problem. Nine centimetres of iron down
// each half's closing edge made an 18 cm bar at the centre when the two halves
// met, standing 1.5 cm proud of everything around it. Against the old flat leaf
// that was invisible, because a flat leaf has no relief for a proud bar to be
// brighter than. Against the authored leaf it lit brighter than the wood and
// read as a LIT SLOT STRAIGHT THROUGH THE DOOR — convincingly enough that
// finding it took a travel = 0 run, a motion = slide run and finally dropping
// `trim` altogether to prove the leaf was whole.
//
// So the centre is left to the wood: BuildDoorLeaf.py gives the leaf a proud
// meeting STILE and the pull is cut into that. What iron is left is what a
// plank door actually wears — hinge straps running from the hanging edge across
// the boards, stopping well clear of the closing edge, clench-nailed through
// each board they cross. A strap that reached the centre would not be a hinge
// strap; that is why it never looked like one.
//
// The nails are pitched on the LEAF'S BOARDS, so the three numbers below mirror
// BuildDoorLeaf.py's layout (its units x kUnit). A nail landing a centimetre
// off a board's middle is exactly the kind of near miss this thread keeps
// paying for, so the pitch is derived rather than eyeballed — and the strap
// ends ON a groove rather than somewhere inside a board.
constexpr float kStileW = 0.205f;      // BuildDoorLeaf.py STILE_W
constexpr float kBoardGap = 0.010f;    // ... GROOVE_W
constexpr float kBoardW = (kLeafHalfW - kStileW - 4.0f * kBoardGap) / 4.0f;
constexpr float kBoardPitch = kBoardW + kBoardGap;

constexpr float kStrapT = 0.056f;      // half-thickness — BuildDoorLeaf.py
                                       // asserts its boards stay inside this,
                                       // so a strap always sits ON the door
constexpr float kStrapRootH = 0.048f;  // 9.6 cm at the hanging edge...
constexpr float kStrapTipH = 0.026f;   // ... tapering to 5.2 cm at the tip
constexpr float kNailR = 0.014f;

// A forged strap: a slab TAPERING along x, which AddBox cannot express. Root at
// x0 with half-height h0, tip at x1 with h1, symmetric through the leaf so it
// reads from both faces (a door is walked through in both directions).
void AddStrap(assets::MeshData& mesh, float y, float x0, float x1, float h0,
			  float h1, float t) {
	const Vec3 a{x0, y - h0, t}, b{x1, y - h1, t}, c{x1, y + h1, t}, d{x0, y + h0, t};
	const Vec3 A{x0, y - h0, -t}, B{x1, y - h1, -t}, C{x1, y + h1, -t},
		D{x0, y + h0, -t};
	const Vec2 q0{0, 0}, q1{1, 0}, q2{1, 1}, q3{0, 1}; // FinishProp re-tiles these
	// The taper tilts the long edges' normals; flat when h0 == h1.
	const float run = x1 - x0, drop = h0 - h1;
	const float n = std::sqrt(run * run + drop * drop);
	const Vec3 up{drop / n, run / n, 0.0f}, down{drop / n, -run / n, 0.0f};
	AddQuad(mesh, a, b, c, d, {0, 0, 1}, q0, q1, q2, q3);
	AddQuad(mesh, B, A, D, C, {0, 0, -1}, q0, q1, q2, q3);
	AddQuad(mesh, A, a, d, D, {-1, 0, 0}, q0, q1, q2, q3);
	AddQuad(mesh, b, B, C, c, {1, 0, 0}, q0, q1, q2, q3);
	AddQuad(mesh, d, c, C, D, up, q0, q1, q2, q3);
	AddQuad(mesh, A, B, b, a, down, q0, q1, q2, q3);
}

assets::ModelData BuildDoorBandHalf() {
	assets::MeshData mesh;
	// The tip lands on the groove between the last BOARD and the STILE: iron
	// crosses every board and stops where the joinery starts, which is both what
	// a hinge strap does and a stop on a modelled joint rather than half way
	// across a board. It was first cut a board shorter, and the 70 cm of blank
	// wood that left across the middle of the door read as ironwork that had
	// given up early.
	constexpr float kTipX = -kLeafHalfW + 4.0f * kBoardPitch - kBoardGap;
	// Low, hand height, high. The middle one takes kPullY on purpose: the strap
	// a hand reaches past should be the one level with what it reaches for.
	for (const float y : {0.30f, kPullY, 1.80f}) {
		AddStrap(mesh, y, -kLeafHalfW, kTipX, kStrapRootH, kStrapTipH, kStrapT);
		// A clench nail through the middle of every board the strap crosses,
		// on both faces. Squashed to a dome like the stone door's bosses: a
		// full sphere reads as a ball sitting on the iron, where a nail head is
		// a swelling of it.
		for (int b = 0; b < 4; ++b) {
			const float x =
				-kLeafHalfW + static_cast<float>(b) * kBoardPitch + kBoardW * 0.5f;
			for (const float sz : {-1.0f, 1.0f})
				AddSphere(mesh, {x, y, sz * kStrapT}, kNailR, 6, 10, -1,
						  {1.0f, 1.0f, 0.45f});
		}
	}
	return FinishProp(std::move(mesh), {0.30f, 0.28f, 0.27f, 1.0f}, 0.35f);
}

// Bronze bosses and a ring pull — the STONE door's answer to the same problem.
// Iron banding on a stone slab reads as ironwork bolted to rock; a sealed vault
// door instead marks its corners and hangs one heavy ring. Both faces get the
// full set, because a door is walked through in both directions. And on a slab
// that is now COURSED MASONRY (tools/BuildStoneSlab.py — the door reads as a
// section of wall that slides), the bronze is the only thing that says door at
// all, which makes it worth more than it was.
//
// THE THICKNESS BUDGET IS THE HARD PART, and it is why the slab has a pocket.
// The frame's jamb mortice is 8 cm half-high and everything that slides into it
// has to fit: 5.5 cm of stone leaves 2.5 cm, and a ring standing off its face by
// its own 2.2 cm thickness needs 4.4. So the ring hangs in a POCKET sunk into
// the slab and measures from the pocket's FLOOR, while the studs measure from
// the face. Both numbers live in that script too and are named there.
constexpr float kSlabHalfT = 0.055f;   // BuildStoneSlab.py T (0.022 units)
constexpr float kSlabPocket = 0.030f;  // ... its pocket floor, T - POCKET_D

assets::ModelData BuildDoorBosses() {
	assets::MeshData mesh;
	// 4 cm, not the 5.5 it was: a stud on the face has only the 2.5 cm the slab
	// leaves, and a squashed sphere stands half its radius proud.
	constexpr float kBossR = 0.040f, kInset = 0.16f;
	for (const float sx : {-1.0f, 1.0f})
		for (const float sy : {0.0f, 1.0f})
			for (const float sz : {-1.0f, 1.0f}) {
				const Vec3 c{sx * (kLeafHalfW - kInset),
							 sy ? kLeafH - kInset : kInset, sz * kSlabHalfT};
				// Squashed to a dome: a full sphere would float half-buried and
				// read as a ball, where a boss is a swelling of the surface.
				AddSphere(mesh, c, kBossR, 8, 12, -1, {1.0f, 1.0f, 0.5f});
			}
	// The ring, hanging in the POCKET's plane — see the budget above. Built from
	// chords rather than a swept torus: AddStrut is the only round primitive
	// here, and at this size 20 segments is round enough that nobody counts them.
	constexpr float kRingR = 0.15f, kTubeR = 0.022f;
	constexpr int kSeg = 20;
	for (const float sz : {-1.0f, 1.0f}) {
		const float z = sz * (kSlabPocket + kTubeR);
		const float cy = kLeafH * 0.46f;
		auto at = [&](int i) {
			const float a = 2.0f * kPi * static_cast<float>(i) / kSeg;
			return Vec3{kRingR * std::sin(a), cy + kRingR * std::cos(a) - kRingR, z};
		};
		for (int i = 0; i < kSeg; ++i) AddStrut(mesh, at(i), at(i + 1), kTubeR, kTubeR, 8);
		// The mount the ring hangs from, at the ring's top — on the pocket floor
		// with it, or the ring would hang off thin air.
		AddBox(mesh, {0.0f, cy + 0.02f, sz * (kSlabPocket + 0.02f)},
			   {0.05f, 0.045f, 0.02f});
	}
	// 0.45 m over the hammered-brass set puts a planished dimple at about 1.7 cm
	// — hammer marks at arm's length, rather than the fine speckle 0.30 gave.
	return FinishProp(std::move(mesh), {0.55f, 0.42f, 0.20f, 1.0f}, 0.45f);
}

// --- door openers ------------------------------------------------------------
// RETIRED (2026-08-08): all four are authored now and imported, so none may be
// baked here — `AssetBaker models` would overwrite the imported meshes with
// boxes again. The jamb pad and its surround are tools/BuildDoorPad.py; the
// chain and its socket are parts of tools/BuildHangingChain.py, which is where a
// chain belongs whatever it hangs from.
//
// What they were: four boxes, deliberately, to judge PLACEMENT and HEIGHT before
// any shape existed — the door_panel_half move, and it worked. Three numbers
// they settled are now in the scripts and worth keeping visible here, because
// each was found by looking at the screen rather than by reasoning:
//
//  * A pad's face must stand further off its surround than kPadPress, or a press
//    swallows it. The first cut pressed 3.5 cm, deeper than the whole fitting,
//    and the button vanished into the jamb instead of being pushed.
//  * A chain's socket must conceal more length than kChainDrop, or the chain's
//    top end walks out of the bottom of its own anchor.
//  * An opener is authored the WALL PROP way — +Z out of the wall, origin ON the
//    wall face at hand height — because the render translates to the jamb and
//    turns the far copy about Y, exactly as the sconce and the lever are placed.
//    That origin is also the point the hand meets, which is what the pull moves.

// RETIRED (2026-08-07): the wall lever is authored in tools/BuildLever.py now
// and imported as TWO models — lever_plate (static) and lever_handle (tilts) —
// which is what lets the mount stay put while the handle swings, and lets the
// two carry different textures. Two bugs went with the old four-box version: it
// put its pivot boss 5.5 cm proud of the origin the render actually rotates
// about, and it tilted the back plate along with the handle.
// Bakes the three worn-block tiers (low/med/high) for one surface texture set,
// displaced by that texture's packed height map (procedural wear when absent).
// kind: 0 = wall, 1 = floor, 2 = ceiling. Shared by the full bake and the
// editor's per-set import (so a newly imported set gets its worn meshes).
// `wearScale` scales the block's displacement (walls.cat `wear`; 0 = flat).
bool BakeWornTiers(int kind, const std::string& texture, float relief, u32 seed,
				   const std::string& modelsDir, const std::string& texturesDir,
				   float wearScale = 1.0f) {
	struct Tier {
		const char* suffix;
		int wallX, wallY, floor, ceiling;
	};
	static const Tier tiers[] = {
		{"low", 14, 16, 14, 12}, {"med", 34, 36, 34, 29}, {"high", 53, 56, 53, 43}};

	relief *= wearScale;
	const bool flat = wearScale <= 0.0f; // no displacement — bake a bare quad
	// The displacement source is the set's packed normal+height map at ANY
	// installed resolution: 1k is the cheapest to sample, but a set imported
	// from the editor only installs at _2k (and a 4k-only set at _4k), and
	// falling back beats silently baking procedural wear over a scanned texture.
	TextureHeight height(std::format("{}\\{}_1k_n.png", texturesDir, texture));
	for (const char* res : {"_2k", "_4k"}) {
		if (height.IsValid()) break;
		height = TextureHeight(std::format("{}\\{}{}_n.png", texturesDir, texture, res));
	}
	if (!flat && !height.IsValid())
		log::Warn("{}: no packed height map — baking procedural wear "
				  "(run tools/FetchTextures.ps1, then rebake)", texture);
	// A non-square scan tiles across proportionally more world width. Read from
	// the image rather than authored, so a set cannot drift from its own texture
	// — and note this holds even for a set baking PROCEDURAL wear, since it is
	// the painted texture being corrected, not the displacement.
	const float uAspect = height.Aspect();
	if (uAspect != 1.0f)
		log::Info("{}: {:.2f}:1 texture — one tile spans {:.2f} squares across",
				  texture, uAspect, uAspect);
	// PHASES: how many squares one repeat of the texture is spread over. A square
	// scan is one, and a 2:1 scan is two — a cell shows half the image, so
	// consecutive cells must show CONSECUTIVE halves or the pattern (and the
	// height map beneath it) restarts at every boundary. A fractional aspect
	// could not divide a repeat into whole cells at all and is refused.
	//
	// Each phase is baked in all four side-pin combinations, because a panel of
	// any phase can meet a matching neighbour on either side. So a set emits
	// phases x 4 panels per tier: 4 for a square scan, 8 for a 2:1 one.
	const float phasesF = std::round(uAspect);
	const bool wholeAspect = std::fabs(uAspect - phasesF) < 1e-3f && phasesF >= 1.0f;
	const int phases = static_cast<int>(phasesF);
	// The remaining exclusion is a wall with no per-cell field to continue at
	// all: WallWearDepth samples a METRE lattice, which does not divide a 2.5 m
	// square, so a procedurally worn set stays pinned however square it is.
	const bool seamless = kind == 0 && !flat && height.IsValid() && wholeAspect;
	if (kind == 0 && !flat && !seamless)
		log::Info("{}: wall seams stay pinned ({})", texture,
				  height.IsValid() ? "aspect is not a whole number of squares"
								   : "no height map");
	else if (seamless && phases > 1)
		log::Info("{}: {} phases — one repeat walks across {} squares", texture,
				  phases, phases);
	bool ok = true;
	for (const Tier& tier : tiers) {
		const std::string out =
			std::format("{}\\worn_{}_{}.gltf", modelsDir, texture, tier.suffix);
		if (kind == 0) {
			// Flat: a single quad spanning the panel (kNx=kNy=1) with a zero wear
			// field, regardless of tier. Worn: the tier grid, height-map- or
			// procedurally-displaced.
			const int nx = flat ? 1 : tier.wallX, ny = flat ? 1 : tier.wallY;
			// Phase 0 fully pinned is written even when the set earns nothing
			// else, because it is what every fallback lands on: a set that loses
			// its right to the siblings (a re-import at a fractional aspect, a
			// height map going missing) degrades to today's behaviour rather than
			// to a hole. A STALE sibling would be silently preferred over it
			// though, so the ones not written this run are removed.
			for (int phase = 0; phase < assets::kMaxWornPhases; ++phase)
				for (int open = 0; open < 4; ++open) {
					const std::string path =
						std::format("{}\\worn_{}_{}{}.gltf", modelsDir, texture,
									tier.suffix, assets::WornPanelSuffix(phase, open));
					const bool wanted =
						phase == 0 && open == 0 ? true : seamless && phase < phases;
					if (!wanted) {
						if (phase != 0 || open != 0) {
							std::error_code ec;
							std::filesystem::remove(path, ec);
						}
						continue;
					}
					// ONE uOffset feeds both the UVs and the height sampling, so
					// the two cannot drift apart and slide the relief off its
					// stones. Phase p starts p/aspect of the way through the image.
					const float uOffset =
						static_cast<float>(phase) * kUvScale / uAspect;
					const SidePins pins{!(open & 1), !(open & 2)};
					WearField field =
						flat ? WearField([](float, float) { return 0.0f; })
						: height.IsValid()
							? TextureWallWear(height, relief, tier.wallX, tier.wallY,
											  seed, pins, uOffset)
							: WearField(WallWearDepth);
					ok &= WriteGltf(
						BuildWornWallBlock(nx, ny, field, uAspect, uOffset), path);
				}
		}
		else if (kind == 1)
			ok &= WriteGltf(BuildWornFloorBlock(tier.floor,
												height.IsValid()
													? TextureFloorWear(height, relief,
																	   tier.floor, seed)
													: WearField(FloorWearHeight),
												uAspect),
							out);
		else
			ok &= WriteGltf(BuildWornCeilingBlock(tier.ceiling,
												  height.IsValid()
													  ? TextureCeilingWear(height, relief,
																		   tier.ceiling, seed)
													  : WearField(CeilingWearDepth),
												  uAspect),
							out);
	}
	return ok;
}

} // namespace

bool BakeModels(const std::string& dir, const std::string& texturesDir) {
	bool ok = true;
	ok &= WriteGltf(BuildWallBlock(), dir + "\\wall_block.gltf");
	ok &= WriteGltf(BuildWallNiche(), dir + "\\wall_niche.gltf");
	ok &= WriteGltf(BuildWallNicheArch(), dir + "\\wall_niche_arch.gltf");
	ok &= WriteGltf(BuildWallWindow(), dir + "\\wall_window.gltf");
	ok &= WriteGltf(BuildWallWindowRect(), dir + "\\wall_window_rect.gltf");
	ok &= WriteGltf(BuildFloorBlock(), dir + "\\floor_block.gltf");
	ok &= WriteGltf(BuildCeilingBlock(), dir + "\\ceiling_block.gltf");

	// Worn blocks: one set per surface texture (0=wall/1=floor/2=ceiling), each
	// at three complexity tiers — see BakeWornTiers. The texture names and their
	// order must match the surface sets a level's palette references.
	struct WornSpec {
		int kind;
		const char* texture;
		float relief; // world-space displacement amplitude (meters)
		u32 seed;
	};
	const WornSpec specs[] = {
		{0, "wall_brick", 0.060f, 911u},   {0, "wall_stone", 0.055f, 921u},
		{0, "wall_moss", 0.040f, 931u},    {1, "floor_slabs", 0.050f, 941u},
		{1, "floor_cobble", 0.045f, 951u}, {2, "ceiling_rough", 0.100f, 961u},
		{2, "ceiling_cracked", 0.080f, 971u},
		// Scanned textures.com sets (each belongs to exactly one surface kind so
		// its worn_<set>_<tier>.gltf is unambiguous). Polished marble has no
		// height map -> procedural wear; relief is its parallax amplitude.
		{0, "cobblestone_wall", 0.060f, 981u}, {0, "stacked_stone", 0.050f, 991u},
		{0, "brick_red", 0.055f, 1001u},       {0, "plaster", 0.030f, 1011u},
		{0, "rock_cliff", 0.070f, 1021u},      {0, "marble_white", 0.020f, 1031u},
		{1, "cobblestone_floor", 0.050f, 1041u}, {1, "broken_tile", 0.040f, 1051u},
		{1, "rubble", 0.060f, 1061u},          {1, "rock_smooth", 0.045f, 1071u},
		{2, "limestone", 0.080f, 1081u},

		// --- batch 2 (2026-08-03): 36 scanned sets ---------------------------
		// Relief is chosen by what the stone DOES, not by a flat per-kind
		// default: round cobbles stand proud, dressed temple ashlar barely
		// moves, and a carved wall keeps its detail in the map rather than the
		// mesh (displacing it would smear the carving). Floors sit LOWER than
		// the walls throughout — relief you walk over reads as lumpy long
		// before the same amplitude looks wrong on a wall — and ceilings sit
		// highest, since nothing ever gets close enough to betray the silhouette.
		{0, "wall_cobble_mixed", 0.070f, 1101u},
		{0, "wall_cobble_mixed4", 0.065f, 1111u},
		{0, "wall_cobble_mossy", 0.070f, 1121u},
		{0, "wall_cobble_round", 0.075f, 1131u},
		{0, "wall_stone_plain", 0.055f, 1141u},
		{0, "wall_stone_granite", 0.050f, 1151u},
		{0, "wall_stone_28", 0.060f, 1161u},
		{0, "wall_stone_30", 0.060f, 1171u},
		{0, "wall_stone_34", 0.055f, 1181u},
		{0, "wall_brick_weathered", 0.050f, 1191u},
		{0, "wall_brick_coarse", 0.055f, 1201u},
		{0, "wall_brick_plaster", 0.040f, 1211u}, // plaster skins the courses
		{0, "wall_brick_distorted", 0.050f, 1221u},
		{0, "wall_brick_old", 0.045f, 1231u},
		{0, "wall_sandstone_blocks", 0.045f, 1241u},
		{0, "wall_sandstone_block2", 0.045f, 1251u},
		{0, "wall_temple_sandstone", 0.040f, 1261u}, // dressed: nearly flush
		{0, "wall_temple_ancient", 0.045f, 1271u},
		{0, "wall_carved", 0.035f, 1281u},        // keep the carving in the map
		{1, "floor_medieval", 0.045f, 1291u},
		{1, "floor_cobble_path", 0.050f, 1301u},
		{1, "floor_cobble_medieval", 0.050f, 1311u},
		{1, "floor_cobble_mossy", 0.050f, 1321u},
		{1, "floor_stone_pavement", 0.040f, 1331u},
		{1, "floor_temple", 0.035f, 1341u},
		{1, "floor_ancient_stone", 0.040f, 1351u},
		{1, "floor_paving_mossy", 0.050f, 1361u},
		{1, "floor_slate", 0.035f, 1371u},
		// A stair TREAD surface first, but worn as a floor too so the brush can
		// place it — the mesh can still bind the plain texture either way.
		{1, "floor_stairs", 0.045f, 1381u},
		{1, "ground_rockbed", 0.055f, 1391u},
		{1, "ground_soil_dusty", 0.030f, 1401u}, // soil slumps; it does not jut
		{1, "ground_soil_rocky", 0.050f, 1411u},
		{1, "ground_gravel", 0.050f, 1421u},
		{2, "ceiling_rock", 0.100f, 1431u},
		{2, "ceiling_rock_layered", 0.090f, 1441u},
		{2, "ceiling_rock_porous", 0.070f, 1451u},
		// The three wood sets are PROP textures (door panel, crate, barrel), not
		// cell surfaces, so they get no worn block: a worn mesh would commit
		// them to one surface kind for nothing.
	};
	for (const WornSpec& spec : specs)
		ok &= BakeWornTiers(spec.kind, spec.texture, spec.relief, spec.seed, dir,
							texturesDir);

	ok &= WriteGltf(BuildSconce(), dir + "\\sconce.gltf");
	ok &= WriteGltf(BuildBrazier(), dir + "\\brazier.gltf");

	// Static architecture decorations (placed by .map "decoration" records).
	ok &= WriteGltf(BuildColumn(), dir + "\\column.gltf");
	ok &= WriteGltf(BuildArchway(), dir + "\\archway.gltf");
	ok &= WriteGltf(BuildDoor(), dir + "\\door.gltf");
	ok &= WriteGltf(BuildPortcullis(), dir + "\\portcullis.gltf");
	ok &= WriteGltf(BuildFountain(), dir + "\\fountain.gltf");
	ok &= WriteGltf(BuildStatue(), dir + "\\statue.gltf");
	ok &= WriteGltf(BuildBarrel(), dir + "\\barrel.gltf");
	ok &= WriteGltf(BuildCrate(), dir + "\\crate.gltf");
	ok &= WriteGltf(BuildChest(), dir + "\\chest.gltf");
	ok &= WriteGltf(BuildBanner(), dir + "\\banner.gltf");
	ok &= WriteGltf(BuildRope(), dir + "\\rope.gltf");
	ok &= WriteGltf(BuildStairs(), dir + "\\stairs.gltf");
	ok &= WriteGltf(BuildStairsDown(), dir + "\\stairs_down.gltf");
	ok &= WriteGltf(BuildPit(), dir + "\\pit.gltf");
	ok &= WriteGltf(BuildPitCeiling(), dir + "\\pit_ceiling.gltf");
	// (door_frame moved to tools/BuildDoorFrame.py — see the note at its old
	// site; baking it here would overwrite the imported mesh)
	// door_panel, NOT door: door.gltf is the cosmetic wood_door decoration.
	ok &= WriteGltf(BuildDoorPanel(), dir + "\\door_panel.gltf");
	// (door_panel_half moved to tools/BuildDoorLeaf.py — see the note at its old
	// site; baking it here would overwrite the imported mesh)
	// Leaf trim (doors.cat `trim`): iron bands over wood, bronze over stone.
	ok &= WriteGltf(BuildDoorBandHalf(), dir + "\\door_band_half.gltf");
	// (the portcullis moved to tools/BuildPortcullis.py and took door_band with
	// it — a lattice that is iron all through needs no separate trim)
	ok &= WriteGltf(BuildDoorBosses(), dir + "\\door_bosses.gltf");
	// (the openers moved too — door_pad/door_pad_mount to tools/BuildDoorPad.py,
	// door_chain/door_chain_socket to tools/BuildHangingChain.py's parts)
	// (the lever moved to tools/BuildLever.py — see the note at BuildLever's
	// old site; lever_plate.gltf and lever_handle.gltf are import-model output)

	ok &= WriteGltf(BuildHumanoid({{0.93f, 0.90f, 0.80f, 1.0f}, 0.85f, 3.2f, 0.0f, 0.12f}),
					dir + "\\skeleton.gltf");
	ok &= WriteGltf(BuildHumanoid({{0.72f, 0.65f, 0.48f, 1.0f}, 1.45f, 5.0f, 1.05f, 0.07f}),
					dir + "\\mummy.gltf");
	ok &= WriteGltf(BuildBlob(), dir + "\\blob.gltf");
	return ok;
}

bool BakeWornBlocks(const std::string& kind, const std::string& name,
					const std::string& assetsDir, float wearScale, float relief) {
	const int k = kind == "floor" ? 1 : (kind == "ceiling" ? 2 : 0);
	// Negative = unspecified: keep the per-kind default this command has always
	// baked at, so an unset `relief` field changes nothing.
	if (relief < 0.0f) relief = k == 2 ? 0.08f : (k == 1 ? 0.045f : 0.055f);
	const u32 seed = static_cast<u32>(std::hash<std::string>{}(name)) | 1u;
	return BakeWornTiers(k, name, relief, seed, assetsDir + "\\models",
						 assetsDir + "\\textures", wearScale);
}

} // namespace dungeon::baker
