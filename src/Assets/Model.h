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
    int baseColorImage = -1; // index into ModelData::images, -1 = none
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
};

enum class ChannelPath { Translation, Rotation, Scale };

struct AnimationChannelData {
    int joint = -1;
    ChannelPath path = ChannelPath::Translation;
    std::vector<float> times;
    std::vector<Vec4> values; // xyz for T/S, xyzw quaternion for R
};

struct AnimationClipData {
    std::string name;
    float duration = 0.0f;
    std::vector<AnimationChannelData> channels;
};

struct ModelData {
    std::vector<MeshData> meshes;
    std::vector<MaterialData> materials;
    std::vector<ImageData> images;
    SkeletonData skeleton;                 // empty if not skinned
    std::vector<AnimationClipData> clips;  // empty if no animations
};

// Loads .gltf / .glb (full features) or .obj (static geometry only).
std::expected<ModelData, std::string> LoadModel(const std::string& path);

// Internal entry points, split by format.
std::expected<ModelData, std::string> LoadGltf(const std::string& path);
std::expected<ModelData, std::string> LoadObj(const std::string& path);

} // namespace dungeon::assets
