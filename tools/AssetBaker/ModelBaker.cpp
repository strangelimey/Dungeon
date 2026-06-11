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

assets::ModelData BuildWallBlock() {
	assets::ModelData model;
	assets::MeshData mesh;

	const float panelZ = -0.10f;     // recess depth
	const float pillarOut = 0.085f;  // pillar protrusion
	const float panelX = 0.80f;      // panel half-width (between pillars)
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

	// Edge pillars (boxes spanning the cell seam, slightly proud of the wall).
	for (const float side : {-1.0f, 1.0f}) {
		const float cx = side * (1.0f - (1.0f - panelX) * 0.5f);
		const float hw = (1.0f - panelX) * 0.5f;
		// Front face + inner side + outer cap; the back is buried in the wall.
		const float x0 = cx - hw, x1 = cx + hw;
		wq({x0, 0, pillarOut}, {x1, 0, pillarOut}, {x1, kWallH, pillarOut},
		   {x0, kWallH, pillarOut}, {0, 0, 1});
		const float inner = side < 0 ? x1 : x0;
		const Vec3 n = side < 0 ? Vec3{1, 0, 0} : Vec3{-1, 0, 0};
		wq({inner, 0, side < 0 ? pillarOut : 0.0f}, {inner, 0, side < 0 ? 0.0f : pillarOut},
		   {inner, kWallH, side < 0 ? 0.0f : pillarOut},
		   {inner, kWallH, side < 0 ? pillarOut : 0.0f}, n);
	}

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
	ok &= WriteGltf(BuildSerpentPillar(), dir + "\\pillar.gltf");
	ok &= WriteGltf(BuildHumanoid({{0.93f, 0.90f, 0.80f, 1.0f}, 0.85f, 3.2f, 0.0f, 0.12f}),
					dir + "\\skeleton.gltf");
	ok &= WriteGltf(BuildHumanoid({{0.72f, 0.65f, 0.48f, 1.0f}, 1.45f, 5.0f, 1.05f, 0.07f}),
					dir + "\\mummy.gltf");
	ok &= WriteGltf(BuildBlob(), dir + "\\blob.gltf");
	return ok;
}

} // namespace dungeon::baker
