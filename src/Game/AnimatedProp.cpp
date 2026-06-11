#include "Game/AnimatedProp.h"

#include <cmath>

namespace dungeon::game {

namespace {
constexpr int kJointCount = 4;
constexpr float kSegmentHeight = 0.55f;
constexpr float kRadius = 0.14f;
constexpr int kRadialSegments = 10;
constexpr int kRings = 16;
constexpr float kTotalHeight = kSegmentHeight * kJointCount;

Mat4 TranslationMatrix(float x, float y, float z) {
    Mat4 m = Mat4Identity();
    m._41 = x;
    m._42 = y;
    m._43 = z;
    return m;
}
} // namespace

assets::ModelData BuildSerpentPillar() {
    assets::ModelData model;

    // --- skeleton: a vertical chain of joints ------------------------------
    for (int j = 0; j < kJointCount; ++j) {
        assets::JointData joint;
        joint.name = "spine" + std::to_string(j);
        joint.parent = j - 1; // -1 for root
        joint.restTranslation = {0, j == 0 ? 0.0f : kSegmentHeight, 0};
        const float globalY = kSegmentHeight * static_cast<float>(j);
        joint.inverseBind = TranslationMatrix(0, -globalY, 0);
        model.skeleton.joints.push_back(joint);
    }

    // --- skinned cylinder ---------------------------------------------------
    assets::MeshData mesh;
    mesh.skinned = true;
    for (int ring = 0; ring <= kRings; ++ring) {
        const float v = static_cast<float>(ring) / kRings;
        const float y = v * kTotalHeight;
        // Slight taper toward the top.
        const float radius = kRadius * (1.0f - 0.35f * v);

        // Blend between the two nearest joints along the chain.
        const float jointPos = v * (kJointCount - 1);
        const u32 j0 = static_cast<u32>(jointPos);
        const u32 j1 = std::min(j0 + 1, static_cast<u32>(kJointCount - 1));
        const float w1 = jointPos - static_cast<float>(j0);

        for (int seg = 0; seg <= kRadialSegments; ++seg) {
            const float a = static_cast<float>(seg) / kRadialSegments * 2.0f * kPi;
            assets::Vertex vert;
            vert.position = {std::cos(a) * radius, y, std::sin(a) * radius};
            vert.normal = {std::cos(a), 0, std::sin(a)};
            vert.uv = {static_cast<float>(seg) / kRadialSegments, v * 4.0f};
            vert.joints[0] = j0;
            vert.joints[1] = j1;
            vert.weights[0] = 1.0f - w1;
            vert.weights[1] = w1;
            mesh.vertices.push_back(vert);
        }
    }
    const u32 stride = kRadialSegments + 1;
    for (u32 ring = 0; ring < kRings; ++ring) {
        for (u32 seg = 0; seg < kRadialSegments; ++seg) {
            const u32 a = ring * stride + seg;
            const u32 b = a + 1;
            const u32 c = a + stride;
            const u32 d = c + 1;
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
    }
    mesh.material = 0;
    model.meshes.push_back(std::move(mesh));
    model.materials.push_back({{0.35f, 0.85f, 0.65f, 1.0f}, -1}); // jade green

    // --- sway animation clip -------------------------------------------------
    assets::AnimationClipData clip;
    clip.name = "sway";
    clip.duration = 4.0f;
    constexpr int kKeys = 33;
    for (int j = 1; j < kJointCount; ++j) { // root stays put
        assets::AnimationChannelData channel;
        channel.joint = j;
        channel.path = assets::ChannelPath::Rotation;
        for (int k = 0; k < kKeys; ++k) {
            const float t = clip.duration * static_cast<float>(k) / (kKeys - 1);
            channel.times.push_back(t);
            // Phase-offset sway per joint, combining two axes.
            const float phase = t / clip.duration * 2.0f * kPi;
            const float angleZ = std::sin(phase + j * 0.9f) * 0.22f;
            const float angleX = std::cos(phase * 2.0f + j * 0.5f) * 0.10f;
            // q = qx * qz (small angles; order is not critical visually).
            const float hz = angleZ * 0.5f, hx = angleX * 0.5f;
            const float cz = std::cos(hz), sz = std::sin(hz);
            const float cx = std::cos(hx), sx = std::sin(hx);
            channel.values.push_back(
                {sx * cz, sx * sz, cx * sz, cx * cz}); // qx*qz composition
        }
        clip.channels.push_back(std::move(channel));
    }
    model.clips.push_back(std::move(clip));
    return model;
}

} // namespace dungeon::game
