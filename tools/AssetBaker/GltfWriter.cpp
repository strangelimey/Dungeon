// ============================================================================
// GltfWriter.cpp — serializes a ModelData to a self-contained .gltf file.
//
// Output structure (single mesh, optional skin + animations):
//   * one buffer holding every accessor's bytes, embedded as a base64 data
//     URI so the file has no sidecar .bin
//   * node 0 is the mesh node; nodes 1..N are the joints (parent-before-
//     child, matching SkeletonData order, so JOINTS_0 needs no remap on load)
//   * accessors: POSITION (with required min/max), NORMAL, TEXCOORD_0,
//     JOINTS_0 (u16), WEIGHTS_0, indices (u32), inverse binds (MAT4),
//     and per-channel time/value pairs for animations
//
// Matrix layout: our row-major Mat4 bytes equal glTF's column-major layout
// for the same transform, so inverse binds are written verbatim.
// ============================================================================
#include "GltfWriter.h"

#include "Core/Log.h"

#include <algorithm>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

namespace dungeon::baker {

namespace {

std::string Base64(const std::vector<u8>& data) {
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        const u32 b0 = data[i];
        const u32 b1 = i + 1 < data.size() ? data[i + 1] : 0;
        const u32 b2 = i + 2 < data.size() ? data[i + 2] : 0;
        const u32 triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(table[(triple >> 18) & 63]);
        out.push_back(table[(triple >> 12) & 63]);
        out.push_back(i + 1 < data.size() ? table[(triple >> 6) & 63] : '=');
        out.push_back(i + 2 < data.size() ? table[triple & 63] : '=');
    }
    return out;
}

// Accumulates the binary buffer and accessor/bufferView JSON fragments.
struct BufferBuilder {
    std::vector<u8> bytes;
    std::vector<std::string> bufferViews;
    std::vector<std::string> accessors;

    // Returns the accessor index for `count` elements of raw data.
    int Add(const void* data, size_t byteLength, int componentType, const char* type,
            size_t count, const std::string& minMax = {}) {
        while (bytes.size() % 4) bytes.push_back(0);
        const size_t offset = bytes.size();
        const auto* src = static_cast<const u8*>(data);
        bytes.insert(bytes.end(), src, src + byteLength);

        const int viewIndex = static_cast<int>(bufferViews.size());
        bufferViews.push_back(std::format(
            R"({{"buffer":0,"byteOffset":{},"byteLength":{}}})", offset, byteLength));
        const int accessorIndex = static_cast<int>(accessors.size());
        accessors.push_back(std::format(
            R"({{"bufferView":{},"componentType":{},"count":{},"type":"{}"{}}})",
            viewIndex, componentType, count, type,
            minMax.empty() ? "" : "," + minMax));
        return accessorIndex;
    }
};

std::string Join(const std::vector<std::string>& parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out.push_back(',');
        out += parts[i];
    }
    return out;
}

} // namespace

bool WriteGltf(const assets::ModelData& model, const std::string& path) {
    if (model.meshes.size() != 1) {
        log::Error("WriteGltf supports exactly one mesh ({})", path);
        return false;
    }
    const assets::MeshData& mesh = model.meshes[0];
    const bool skinned = !model.skeleton.joints.empty();
    const size_t vertexCount = mesh.vertices.size();

    BufferBuilder buffer;

    // --- vertex attributes (deinterleaved) ----------------------------------
    std::vector<float> positions, normals, uvs, weights;
    std::vector<u16> joints;
    Vec3 posMin{1e9f, 1e9f, 1e9f}, posMax{-1e9f, -1e9f, -1e9f};
    for (const assets::Vertex& v : mesh.vertices) {
        positions.insert(positions.end(), {v.position.x, v.position.y, v.position.z});
        normals.insert(normals.end(), {v.normal.x, v.normal.y, v.normal.z});
        uvs.insert(uvs.end(), {v.uv.x, v.uv.y});
        posMin = {std::min(posMin.x, v.position.x), std::min(posMin.y, v.position.y),
                  std::min(posMin.z, v.position.z)};
        posMax = {std::max(posMax.x, v.position.x), std::max(posMax.y, v.position.y),
                  std::max(posMax.z, v.position.z)};
        if (skinned) {
            for (int k = 0; k < 4; ++k) joints.push_back(static_cast<u16>(v.joints[k]));
            weights.insert(weights.end(),
                           {v.weights[0], v.weights[1], v.weights[2], v.weights[3]});
        }
    }

    const std::string posMinMax = std::format(
        R"("min":[{},{},{}],"max":[{},{},{}])", posMin.x, posMin.y, posMin.z, posMax.x,
        posMax.y, posMax.z);
    const int accPos = buffer.Add(positions.data(), positions.size() * 4, 5126, "VEC3",
                                  vertexCount, posMinMax);
    const int accNorm =
        buffer.Add(normals.data(), normals.size() * 4, 5126, "VEC3", vertexCount);
    const int accUv = buffer.Add(uvs.data(), uvs.size() * 4, 5126, "VEC2", vertexCount);
    int accJoints = -1, accWeights = -1;
    if (skinned) {
        accJoints =
            buffer.Add(joints.data(), joints.size() * 2, 5123, "VEC4", vertexCount);
        accWeights =
            buffer.Add(weights.data(), weights.size() * 4, 5126, "VEC4", vertexCount);
    }
    const int accIndices = buffer.Add(mesh.indices.data(), mesh.indices.size() * 4,
                                      5125, "SCALAR", mesh.indices.size());

    // --- nodes: 0 = mesh node, 1..N = joints --------------------------------
    std::vector<std::string> nodes;
    {
        std::string meshNode = R"({"mesh":0)";
        if (skinned) meshNode += R"(,"skin":0)";
        meshNode += "}";
        nodes.push_back(meshNode);
    }
    std::string skinJson;
    if (skinned) {
        const auto& skel = model.skeleton.joints;
        // children lists per joint
        std::vector<std::vector<int>> children(skel.size());
        for (size_t j = 0; j < skel.size(); ++j)
            if (skel[j].parent >= 0)
                children[static_cast<size_t>(skel[j].parent)].push_back(
                    static_cast<int>(j) + 1); // node index
        for (size_t j = 0; j < skel.size(); ++j) {
            const auto& joint = skel[j];
            std::string node = std::format(
                R"({{"name":"{}","translation":[{},{},{}],"rotation":[{},{},{},{}],"scale":[{},{},{}])",
                joint.name, joint.restTranslation.x, joint.restTranslation.y,
                joint.restTranslation.z, joint.restRotation.x, joint.restRotation.y,
                joint.restRotation.z, joint.restRotation.w, joint.restScale.x,
                joint.restScale.y, joint.restScale.z);
            if (!children[j].empty()) {
                node += R"(,"children":[)";
                for (size_t c = 0; c < children[j].size(); ++c)
                    node += (c ? "," : "") + std::to_string(children[j][c]);
                node += "]";
            }
            node += "}";
            nodes.push_back(node);
        }

        // Inverse bind matrices: our row-major Mat4 bytes are exactly glTF's
        // column-major layout for the equivalent transform.
        std::vector<float> ibm;
        for (const auto& joint : skel) {
            const float* m = &joint.inverseBind._11;
            ibm.insert(ibm.end(), m, m + 16);
        }
        const int accIbm = buffer.Add(ibm.data(), ibm.size() * 4, 5126, "MAT4",
                                      skel.size());

        std::string jointList;
        for (size_t j = 0; j < skel.size(); ++j)
            jointList += (j ? "," : "") + std::to_string(j + 1);
        skinJson = std::format(
            R"("skins":[{{"joints":[{}],"inverseBindMatrices":{},"skeleton":1}}],)",
            jointList, accIbm);
    }

    // --- animations ----------------------------------------------------------
    std::string animJson;
    if (!model.clips.empty()) {
        std::vector<std::string> anims;
        for (const auto& clip : model.clips) {
            std::vector<std::string> samplers, channels;
            for (const auto& ch : clip.channels) {
                const std::string inMinMax = std::format(
                    R"("min":[{}],"max":[{}])", ch.times.front(), ch.times.back());
                const int accIn = buffer.Add(ch.times.data(), ch.times.size() * 4,
                                             5126, "SCALAR", ch.times.size(), inMinMax);
                int accOut;
                const char* pathName;
                if (ch.path == assets::ChannelPath::Rotation) {
                    accOut = buffer.Add(ch.values.data(), ch.values.size() * 16, 5126,
                                        "VEC4", ch.values.size());
                    pathName = "rotation";
                } else {
                    std::vector<float> v3;
                    for (const Vec4& v : ch.values)
                        v3.insert(v3.end(), {v.x, v.y, v.z});
                    accOut = buffer.Add(v3.data(), v3.size() * 4, 5126, "VEC3",
                                        ch.values.size());
                    pathName = ch.path == assets::ChannelPath::Translation ? "translation"
                                                                           : "scale";
                }
                const int samplerIndex = static_cast<int>(samplers.size());
                samplers.push_back(std::format(
                    R"({{"input":{},"output":{},"interpolation":"LINEAR"}})", accIn,
                    accOut));
                channels.push_back(std::format(
                    R"({{"sampler":{},"target":{{"node":{},"path":"{}"}}}})",
                    samplerIndex, ch.joint + 1, pathName));
            }
            anims.push_back(std::format(
                R"({{"name":"{}","samplers":[{}],"channels":[{}]}})", clip.name,
                Join(samplers), Join(channels)));
        }
        animJson = std::format(R"("animations":[{}],)", Join(anims));
    }

    // --- material -------------------------------------------------------------
    Vec4 color{1, 1, 1, 1};
    if (!model.materials.empty()) color = model.materials[0].baseColorFactor;
    const std::string materialJson = std::format(
        R"("materials":[{{"pbrMetallicRoughness":{{"baseColorFactor":[{},{},{},{}],"metallicFactor":0.0,"roughnessFactor":1.0}}}}],)",
        color.x, color.y, color.z, color.w);

    // --- primitive --------------------------------------------------------------
    std::string attributes = std::format(
        R"("POSITION":{},"NORMAL":{},"TEXCOORD_0":{})", accPos, accNorm, accUv);
    if (skinned)
        attributes += std::format(R"(,"JOINTS_0":{},"WEIGHTS_0":{})", accJoints,
                                  accWeights);

    // --- assemble ------------------------------------------------------------
    std::string sceneNodes = "0";
    if (skinned) sceneNodes += ",1"; // root joint participates in the scene

    const std::string json = std::format(
        R"({{"asset":{{"version":"2.0","generator":"Dungeon AssetBaker"}},)"
        R"("scene":0,"scenes":[{{"nodes":[{}]}}],)"
        R"("nodes":[{}],)"
        R"({}{}{})"
        R"("meshes":[{{"primitives":[{{"attributes":{{{}}},"indices":{},"material":0}}]}}],)"
        R"("buffers":[{{"byteLength":{},"uri":"data:application/octet-stream;base64,{}"}}],)"
        R"("bufferViews":[{}],"accessors":[{}]}})",
        sceneNodes, Join(nodes), materialJson, skinJson, animJson, attributes,
        accIndices, buffer.bytes.size(), Base64(buffer.bytes),
        Join(buffer.bufferViews), Join(buffer.accessors));

    std::FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) {
        log::Error("Cannot write {}", path);
        return false;
    }
    std::fwrite(json.data(), 1, json.size(), f);
    std::fclose(f);
    log::Info("Wrote {} ({} verts, {} joints, {} clips)", path, vertexCount,
              model.skeleton.joints.size(), model.clips.size());
    return true;
}

} // namespace dungeon::baker
