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
	// The glTF node's world transform; the vertices are still in NODE space.
	// Bake it with BakeNodeTransform (below), never by hand.
	Mat4 worldTransform = Mat4Identity();
	bool skinned = false;
};

// --- the node transform (NodeTransform.cpp) -----------------------------------
// One rule for every consumer that wants a mesh in its MODEL's space - the
// world's loaders, the asset picker, import-model (code-review C253: seven hand
// copies of the bake shared two flaws, and three bought daggers carry the
// non-uniform and mirrored nodes they get wrong).
//
// What a mesh's node does to it: worldTransform - or the IDENTITY for a skinned
// mesh, whose node transform glTF says to ignore (its joints place it). A
// positions-only pass (bounds, a measurement) transforms by this.
Mat4 NodeTransform(const MeshData& mesh);
// Bakes NodeTransform into the vertices and resets worldTransform to the
// identity, so a second bake is a no-op: positions through the matrix, NORMALS
// through its inverse-transpose (renormalized - the plain matrix skews them under
// a non-uniform scale), and under a MIRROR (a negative determinant) every
// triangle's winding reversed, or a back-culled draw shows the mesh inside out.
// A skinned mesh is left exactly as it is.
void BakeNodeTransform(MeshData& mesh);

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
	// -1 = no skeleton. The world (MonsterKind::rigRoot) and the Animator's
	// root-travel lock both ask here.
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

// The CPU bytes a model's embedded images hold: the decoded `images` plus the
// baked `imageMips` chains. Once a renderer has uploaded them nothing reads them
// again, so a long-lived cache drops them (ReleaseImages) instead of keeping a
// copy of VRAM in RAM for the life of the world (code-review C222).
inline u64 ImageBytes(const ModelData& model) {
	u64 bytes = 0;
	for (const ImageData& img : model.images) bytes += img.pixels.size();
	for (const MipChain& chain : model.imageMips)
		for (const TextureLevel& level : chain.levels) bytes += level.data.size();
	return bytes;
}

// Frees the image storage and KEEPS THE SLOTS: `images` and `imageMips` keep
// their sizes with every entry emptied, so a material's image index still names
// a slot rather than running off the end of a shorter vector. The model is then
// no longer something textures can be built from (ImagesReleased).
inline void ReleaseImages(ModelData& model) {
	for (ImageData& img : model.images) {
		std::vector<u8>().swap(img.pixels);
		img.width = img.height = 0;
	}
	for (MipChain& chain : model.imageMips) std::vector<TextureLevel>().swap(chain.levels);
}

// Whether the images are gone (ReleaseImages ran): image slots, and not one
// byte behind any of them.
inline bool ImagesReleased(const ModelData& model) {
	return !model.images.empty() && ImageBytes(model) == 0;
}

struct LoadOptions {
	// Use each embedded image's BAKED sidecar (EmbeddedImageSidecar - a BC7 mip
	// chain written by `AssetBaker mips`) when it is at least as new as the
	// model, instead of decoding the PNG/JPEG inside the file. The game asks for
	// this; tools do not, since a tool reading a model wants its real images.
	// Decoding a 2k PNG and building its mips on the CPU cost ~50 ms an image
	// (skel_warrior: ~320 ms of a level change).
	bool bakedImages = false;
	// With bakedImages: say so - one line per model, once a run - when an image
	// had to be decoded because its sidecar is MISSING or OLDER than the model
	// (a missing one used to decode silently, and only a stale one warned, per
	// image: code-review C437). The baker turns it off: it asks for the baked
	// images only to skip the current ones, and is about to bake the rest.
	bool warnUnbaked = true;
};

// Loads .gltf / .glb (full features) or .obj (static geometry only).
std::expected<ModelData, std::string> LoadModel(const std::string& path,
												const LoadOptions& opts = {});

// Where the baked mip chain of a model's embedded image `index` lives: beside
// the model, "<model file>.<index>.dds" (skel_warrior.gltf.3.dds). The index is
// ModelData::images order. The baker writes it, the loader reads it; both call
// this so the name cannot drift.
std::string EmbeddedImageSidecar(const std::string& modelPath, size_t index);

// Which of a model's images are sampled as sRGB: those a material uses as its
// BASE COLOUR (normal and metallic-roughness maps are data). Parallel to
// ModelData::images. The game creates their textures with it and the mip bake
// averages their levels in linear light by it (assets::Downsample), so the two
// ask one function and a sidecar is the chain the game would have built.
std::vector<bool> SrgbImages(const ModelData& model);

// Internal entry points, split by format.
std::expected<ModelData, std::string> LoadGltf(const std::string& path,
											   const LoadOptions& opts = {});
std::expected<ModelData, std::string> LoadObj(const std::string& path);

} // namespace dungeon::assets
