#include "Assets/Model.h"

#include "Core/Log.h"

#include <cgltf.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <unordered_map>

namespace dungeon::assets {

namespace {

Mat4 ToMat4(const float m[16]) {
    Mat4 out;
    std::memcpy(&out, m, sizeof(out));
    return out;
}

// Maps a cgltf image to an index in ModelData::images, loading on first use.
struct ImageCache {
    const cgltf_data* data;
    std::filesystem::path baseDir;
    ModelData* model;
    std::unordered_map<const cgltf_image*, int> indices;

    int Get(const cgltf_image* image) {
        if (!image) return -1;
        if (auto it = indices.find(image); it != indices.end()) return it->second;

        std::optional<ImageData> loaded;
        if (image->buffer_view && image->buffer_view->buffer->data) {
            const auto* bytes = static_cast<const u8*>(image->buffer_view->buffer->data) +
                                image->buffer_view->offset;
            loaded = LoadImageMemory(bytes, image->buffer_view->size);
        } else if (image->uri && std::strncmp(image->uri, "data:", 5) != 0) {
            loaded = LoadImageFile((baseDir / image->uri).string());
        }

        int index = -1;
        if (loaded) {
            index = static_cast<int>(model->images.size());
            model->images.push_back(std::move(*loaded));
        } else {
            log::Warn("glTF: could not load an image (uri: {})",
                      image->uri ? image->uri : "<embedded>");
        }
        indices[image] = index;
        return index;
    }
};

// Topologically orders the skin's joints parent-before-child and fills
// SkeletonData. Returns node→joint index mapping.
std::unordered_map<const cgltf_node*, int> BuildSkeleton(const cgltf_skin* skin,
                                                         SkeletonData& skeleton) {
    std::unordered_map<const cgltf_node*, int> nodeToSlot; // original slot in skin
    for (cgltf_size i = 0; i < skin->joints_count; ++i)
        nodeToSlot[skin->joints[i]] = static_cast<int>(i);

    // Order parents first.
    std::vector<const cgltf_node*> ordered;
    ordered.reserve(skin->joints_count);
    std::unordered_map<const cgltf_node*, bool> visited;
    auto visit = [&](auto&& self, const cgltf_node* node) -> void {
        if (!node || !nodeToSlot.contains(node) || visited[node]) return;
        visited[node] = true;
        self(self, node->parent);
        ordered.push_back(node);
    };
    for (cgltf_size i = 0; i < skin->joints_count; ++i)
        visit(visit, skin->joints[i]);

    std::unordered_map<const cgltf_node*, int> nodeToJoint;
    for (size_t i = 0; i < ordered.size(); ++i) nodeToJoint[ordered[i]] = static_cast<int>(i);

    skeleton.joints.resize(ordered.size());
    for (size_t i = 0; i < ordered.size(); ++i) {
        const cgltf_node* node = ordered[i];
        JointData& j = skeleton.joints[i];
        j.name = node->name ? node->name : "";
        j.parent = (node->parent && nodeToJoint.contains(node->parent))
                       ? nodeToJoint[node->parent]
                       : -1;
        j.restTranslation = {node->translation[0], node->translation[1], node->translation[2]};
        j.restRotation = {node->rotation[0], node->rotation[1], node->rotation[2],
                          node->rotation[3]};
        j.restScale = {node->scale[0], node->scale[1], node->scale[2]};

        if (skin->inverse_bind_matrices) {
            float m[16];
            cgltf_accessor_read_float(skin->inverse_bind_matrices,
                                      static_cast<cgltf_size>(nodeToSlot[node]), m, 16);
            j.inverseBind = ToMat4(m);
        }
    }
    return nodeToJoint;
}

// `slotRemap` maps glTF skin-slot indices to our topologically ordered joint
// indices (empty for unskinned models).
void ReadPrimitive(const cgltf_primitive* prim, MeshData& mesh,
                   const std::vector<u32>& slotRemap) {
    const cgltf_accessor* position = nullptr;
    const cgltf_accessor* normal = nullptr;
    const cgltf_accessor* texcoord = nullptr;
    const cgltf_accessor* jointsAcc = nullptr;
    const cgltf_accessor* weightsAcc = nullptr;

    for (cgltf_size a = 0; a < prim->attributes_count; ++a) {
        const cgltf_attribute& attr = prim->attributes[a];
        switch (attr.type) {
        case cgltf_attribute_type_position: position = attr.data; break;
        case cgltf_attribute_type_normal:   normal = attr.data; break;
        case cgltf_attribute_type_texcoord: if (attr.index == 0) texcoord = attr.data; break;
        case cgltf_attribute_type_joints:   if (attr.index == 0) jointsAcc = attr.data; break;
        case cgltf_attribute_type_weights:  if (attr.index == 0) weightsAcc = attr.data; break;
        default: break;
        }
    }
    if (!position) return;

    const size_t base = mesh.vertices.size();
    mesh.vertices.resize(base + position->count);
    for (cgltf_size v = 0; v < position->count; ++v) {
        Vertex& vert = mesh.vertices[base + v];
        float tmp[4] = {};
        cgltf_accessor_read_float(position, v, tmp, 3);
        vert.position = {tmp[0], tmp[1], tmp[2]};
        if (normal) {
            cgltf_accessor_read_float(normal, v, tmp, 3);
            vert.normal = {tmp[0], tmp[1], tmp[2]};
        }
        if (texcoord) {
            cgltf_accessor_read_float(texcoord, v, tmp, 2);
            vert.uv = {tmp[0], tmp[1]};
        }
        if (jointsAcc && weightsAcc) {
            cgltf_uint ji[4] = {};
            cgltf_accessor_read_uint(jointsAcc, v, ji, 4);
            for (int k = 0; k < 4; ++k)
                vert.joints[k] = ji[k] < slotRemap.size() ? slotRemap[ji[k]] : 0;
            cgltf_accessor_read_float(weightsAcc, v, tmp, 4);
            for (int k = 0; k < 4; ++k) vert.weights[k] = tmp[k];
            mesh.skinned = true;
        }
    }

    if (prim->indices) {
        mesh.indices.reserve(mesh.indices.size() + prim->indices->count);
        for (cgltf_size i = 0; i < prim->indices->count; ++i)
            mesh.indices.push_back(
                static_cast<u32>(base + cgltf_accessor_read_index(prim->indices, i)));
    } else {
        for (cgltf_size i = 0; i < position->count; ++i)
            mesh.indices.push_back(static_cast<u32>(base + i));
    }
}

} // namespace

std::optional<ModelData> LoadGltf(const std::string& path) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        log::Error("Failed to parse glTF: {}", path);
        return std::nullopt;
    }
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        log::Error("Failed to load glTF buffers: {}", path);
        cgltf_free(data);
        return std::nullopt;
    }

    ModelData model;
    ImageCache imageCache{data, std::filesystem::path(path).parent_path(), &model, {}};

    // Materials (indices must match cgltf's so primitives can look them up).
    for (cgltf_size m = 0; m < data->materials_count; ++m) {
        const cgltf_material& src = data->materials[m];
        MaterialData mat;
        if (src.has_pbr_metallic_roughness) {
            const auto& pbr = src.pbr_metallic_roughness;
            mat.baseColorFactor = {pbr.base_color_factor[0], pbr.base_color_factor[1],
                                   pbr.base_color_factor[2], pbr.base_color_factor[3]};
            if (pbr.base_color_texture.texture)
                mat.baseColorImage = imageCache.Get(pbr.base_color_texture.texture->image);
        }
        model.materials.push_back(mat);
    }

    // Skeleton from the first skin, if any.
    std::unordered_map<const cgltf_node*, int> nodeToJoint;
    std::vector<u32> slotRemap;
    if (data->skins_count > 0) {
        nodeToJoint = BuildSkeleton(&data->skins[0], model.skeleton);
        const cgltf_skin* skin = &data->skins[0];
        slotRemap.resize(skin->joints_count, 0);
        for (cgltf_size i = 0; i < skin->joints_count; ++i)
            slotRemap[i] = static_cast<u32>(nodeToJoint[skin->joints[i]]);
    }

    // Meshes: one MeshData per node-with-mesh per primitive material group.
    for (cgltf_size n = 0; n < data->nodes_count; ++n) {
        const cgltf_node& node = data->nodes[n];
        if (!node.mesh) continue;
        float world[16];
        cgltf_node_transform_world(&node, world);
        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
            const cgltf_primitive& prim = node.mesh->primitives[p];
            MeshData mesh;
            mesh.worldTransform = ToMat4(world);
            mesh.material = prim.material
                                ? static_cast<int>(prim.material - data->materials)
                                : -1;
            ReadPrimitive(&prim, mesh, slotRemap);
            if (!mesh.vertices.empty()) model.meshes.push_back(std::move(mesh));
        }
    }

    // Animation clips.
    for (cgltf_size a = 0; a < data->animations_count; ++a) {
        const cgltf_animation& src = data->animations[a];
        AnimationClipData clip;
        clip.name = src.name ? src.name : std::format("clip{}", a);
        for (cgltf_size c = 0; c < src.channels_count; ++c) {
            const cgltf_animation_channel& ch = src.channels[c];
            if (!ch.target_node || !nodeToJoint.contains(ch.target_node)) continue;

            AnimationChannelData out;
            out.joint = nodeToJoint[ch.target_node];
            switch (ch.target_path) {
            case cgltf_animation_path_type_translation: out.path = ChannelPath::Translation; break;
            case cgltf_animation_path_type_rotation:    out.path = ChannelPath::Rotation; break;
            case cgltf_animation_path_type_scale:       out.path = ChannelPath::Scale; break;
            default: continue;
            }

            const cgltf_accessor* input = ch.sampler->input;
            const cgltf_accessor* output = ch.sampler->output;
            out.times.resize(input->count);
            out.values.resize(output->count);
            for (cgltf_size i = 0; i < input->count; ++i) {
                cgltf_accessor_read_float(input, i, &out.times[i], 1);
                clip.duration = std::max(clip.duration, out.times[i]);
            }
            const int comps = (out.path == ChannelPath::Rotation) ? 4 : 3;
            for (cgltf_size i = 0; i < output->count; ++i) {
                float tmp[4] = {0, 0, 0, 1};
                cgltf_accessor_read_float(output, i, tmp, comps);
                out.values[i] = {tmp[0], tmp[1], tmp[2], tmp[3]};
            }
            clip.channels.push_back(std::move(out));
        }
        if (!clip.channels.empty()) model.clips.push_back(std::move(clip));
    }

    cgltf_free(data);
    log::Info("Loaded glTF '{}': {} meshes, {} materials, {} joints, {} clips", path,
              model.meshes.size(), model.materials.size(), model.skeleton.joints.size(),
              model.clips.size());
    return model;
}

std::optional<ModelData> LoadModel(const std::string& path) {
    auto ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".gltf" || ext == ".glb") return LoadGltf(path);
    if (ext == ".obj") return LoadObj(path);
    log::Error("Unsupported model format: {}", path);
    return std::nullopt;
}

} // namespace dungeon::assets
