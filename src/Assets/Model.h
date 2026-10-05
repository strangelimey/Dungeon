// ============================================================================
// Assets/Model.h — CPU-side model data and the loaders that produce it.
//
// ModelData is the engine's one interchange format: the glTF and OBJ loaders
// produce it, the AssetBaker writes it, gfx::Mesh uploads its geometry, and
// anim::Animator plays its skeleton + clips. Everything here is plain data —
// no D3D12, no file handles — so it can be built or inspected anywhere.
//
// Data flow:   file (.gltf/.glb/.obj)
//                └─ LoadModel ──► ModelData
//                                   ├─ meshes[]    ──► gfx::Mesh (GPU upload)
//                                   ├─ images[]    ──► gfx::Texture
//                                   ├─ skeleton    ──┐
//                                   └─ clips[]     ──┴► anim::Animator
// ============================================================================
#pragma once

#include "Assets/Image.h"
#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <expected>
#include <span>
#include <string>
#include <vector>

namespace dungeon::assets {

// One vertex layout for the whole engine; unskinned meshes leave the joint
// weights at zero (weight[0] == 0 means "not skinned" to the shader).
struct Vertex {
	Vec3 position{};
	Vec3 normal{};
	Vec2 uv{};
	u32 joints[4]{};
	float weights[4]{};
};

struct MaterialData {
	Vec4 baseColorFactor{1, 1, 1, 1};
	int baseColorImage = -1;  // index into ModelData::images, -1 = none
	int normalImage = -1;     // tangent-space normal map, -1 = none
	int metalRoughImage = -1; // glTF MR (G=roughness, B=metallic), -1 = none
	float metallic = 1.0f;    // scales the MR map (glTF default 1)
	float roughness = 1.0f;   // scales the MR map (glTF default 1)
	Vec3 emissive{0, 0, 0};   // additive self-lit factor
	bool blend = false;       // glTF alphaMode BLEND: see-through (glass)
};

struct MeshData {
	std::vector<Vertex> vertices;
	std::vector<u32> indices;
	int material = -1;
	Mat4 worldTransform = Mat4Identity(); // baked node transform for static meshes
	bool skinned = false;
};

// --- animation data (pure data; runtime logic lives in the Animation module) ---

struct JointData {
	std::string name;
	int parent = -1;          // index into SkeletonData::joints, -1 = root
	// Maps a bind-pose model-space position into this joint's local space.
	// The skinning palette is inverseBind * jointGlobal (row convention).
	Mat4 inverseBind = Mat4Identity();
	// Local rest transform (used for joints no animation channel touches).
	Vec3 restTranslation{};
	Quat restRotation{0, 0, 0, 1};
	Vec3 restScale{1, 1, 1};
};

// INVARIANT: joints are sorted parent-before-child, so a single forward pass
// can compute global transforms. The glTF loader topologically sorts on load
// and remaps vertex joint indices to match.
struct SkeletonData {
	std::vector<JointData> joints;

	// The rig's ROOT joint: the first parentless one (joints are parent-first),
	// -1 = no skeleton.
	int RootJoint() const {
		for (size_t j = 0; j < joints.size(); ++j)
			if (joints[j].parent < 0) return static_cast<int>(j);
		return -1;
	}
	// Where that root stands at rest, in MODEL space (it has no parent, so its
	// local rest translation is model space); zero without a skeleton. A bought
	// rig need not rest on its file's origin (the skeleton kit stands ~(0.34,
	// 0.43) off it), so the game draws and previews a rigged model centred on
	// this point's XZ - one rule, asked here.
	Vec3 RootRest() const {
		const int r = RootJoint();
		return r >= 0 ? joints[static_cast<size_t>(r)].restTranslation : Vec3{};
	}
};

enum class ChannelPath { Translation, Rotation, Scale };

// One animated property (T, R or S) of one joint. Its keys are RANGES of the
// owning clip's pooled arrays, not vectors of its own: a rigged Mixamo model
// has ~4,000 channels, and two buffers each made its load cost ~8,000
// allocations for what is, per clip, two arrays. Channels that share a key-time
// accessor in the file share one range of `times`. Read keys through
// AnimationClipData::Times / Values.
struct AnimationChannelData {
	int joint = -1;
	ChannelPath path = ChannelPath::Translation;
	u32 timeFirst = 0, timeCount = 0;   // range of AnimationClipData::times
	u32 valueFirst = 0, valueCount = 0; // range of AnimationClipData::values
};

// A channel with key lists of its own - the AUTHORING form (the AssetBaker's
// procedural rigs build these), appended into a clip with AnimationClipData::Add.
struct ChannelKeys {
	int joint = -1;
	ChannelPath path = ChannelPath::Translation;
	std::vector<float> times;
	std::vector<Vec4> values; // xyz for T/S, xyzw quaternion for R
};

struct AnimationClipData {
	std::string name;
	float duration = 0.0f;
	std::vector<AnimationChannelData> channels;
	std::vector<float> times; // every channel's key times, pooled
	std::vector<Vec4> values; // every channel's key values, pooled

	std::span<const float> Times(const AnimationChannelData& ch) const {
		return {times.data() + ch.timeFirst, ch.timeCount};
	}
	std::span<const Vec4> Values(const AnimationChannelData& ch) const {
		return {values.data() + ch.valueFirst, ch.valueCount};
	}
	std::span<Vec4> Values(const AnimationChannelData& ch) {
		return {values.data() + ch.valueFirst, ch.valueCount};
	}
	// Appends an authored channel, copying its keys into the pools.
	void Add(const ChannelKeys& keys) {
		AnimationChannelData ch;
		ch.joint = keys.joint;
		ch.path = keys.path;
		ch.timeFirst = static_cast<u32>(times.size());
		ch.timeCount = static_cast<u32>(keys.times.size());
		ch.valueFirst = static_cast<u32>(values.size());
		ch.valueCount = static_cast<u32>(keys.values.size());
		times.insert(times.end(), keys.times.begin(), keys.times.end());
		values.insert(values.end(), keys.values.begin(), keys.values.end());
		channels.push_back(ch);
	}
};

struct ModelData {
	std::vector<MeshData> meshes;
	std::vector<MaterialData> materials;
	std::vector<ImageData> images;
	// Baked BC7 mip chains for embedded images (LoadOptions::bakedImages).
	// Parallel to `images` when non-empty; an entry WITH levels replaces
	// images[i], which is then left empty. Empty = every image was decoded.
	std::vector<MipChain> imageMips;
	SkeletonData skeleton;                 // empty if not skinned
	std::vector<AnimationClipData> clips;  // empty if no animations
};

struct LoadOptions {
	// Use each embedded image's BAKED sidecar (EmbeddedImageSidecar - a BC7 mip
	// chain written by `AssetBaker mips`) when it is at least as new as the
	// model, instead of decoding the PNG/JPEG inside the file. The game asks for
	// this; tools do not, since a tool reading a model wants its real images.
	// Decoding a 2k PNG and building its mips on the CPU cost ~50 ms an image
	// (skel_warrior: ~320 ms of a level change).
	bool bakedImages = false;
};

// Loads .gltf / .glb (full features) or .obj (static geometry only).
std::expected<ModelData, std::string> LoadModel(const std::string& path,
												const LoadOptions& opts = {});

// Where the baked mip chain of a model's embedded image `index` lives: beside
// the model, "<model file>.<index>.dds" (skel_warrior.gltf.3.dds). The index is
// ModelData::images order. The baker writes it, the loader reads it; both call
// this so the name cannot drift.
std::string EmbeddedImageSidecar(const std::string& modelPath, size_t index);

// Internal entry points, split by format.
std::expected<ModelData, std::string> LoadGltf(const std::string& path,
											   const LoadOptions& opts = {});
std::expected<ModelData, std::string> LoadObj(const std::string& path);

} // namespace dungeon::assets
