// ============================================================================
// Assets/Model.cpp — glTF 2.0 loader (via cgltf) + format dispatch.
//
// glTF crash course for the reader:
//   * A file holds buffers (raw bytes), bufferViews (slices), and accessors
//     (typed views: "N vec3 floats at offset X"). cgltf resolves all of that;
//     we read through cgltf_accessor_read_* and never touch offsets ourselves.
//   * Meshes hang off NODES, which form a transform hierarchy. We bake each
//     node's world transform into MeshData::worldTransform (static geometry)
//     instead of keeping a scene graph.
//   * A SKIN lists joint nodes + inverse bind matrices. Vertex JOINTS_0
//     attributes index into the skin's joint LIST ORDER ("slots"), not into
//     any sorted order — hence the slot remap below.
//   * ANIMATIONS are channels (target node + path: T/R/S) sampled by paired
//     input (time) / output (value) accessors. We keep them as raw keyframes
//     and interpolate at runtime in anim::Animator.
//
// Matrix layout note: glTF stores matrices column-major for column vectors;
// DirectXMath row-major for row vectors. Those two conventions produce the
// SAME 16-float memory order for the same transform, so plain memcpy is
// correct here (see ToMat4 / the inverse-bind read).
// ============================================================================
#include "Assets/Model.h"

#include "Assets/Dds.h"
#include "Assets/File.h"
#include "Core/AllocTrack.h"
#include "Core/Log.h"

#include <cgltf.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <type_traits>
#include <unordered_map>

namespace dungeon::assets {

namespace {

Mat4 ToMat4(const float m[16]) {
	Mat4 out;
	std::memcpy(&out, m, sizeof(out));
	return out;
}

// Maps a cgltf image to an index in ModelData::images, loading on first use.
// With `bakedImages`, an image whose baked sidecar (EmbeddedImageSidecar) is
// current (assets::BakedIsCurrent against the model file) loads from THAT
// instead - a BC7 mip chain in imageMips, images[i] left empty - and is never
// decoded. The index a sidecar is named by is this same first-use order, which
// the baker gets by running this same loader, so the two cannot disagree.
struct ImageCache {
	const cgltf_data* data;
	std::filesystem::path baseDir;
	ModelData* model;
	std::string modelPath;
	bool bakedImages = false;
	std::unordered_map<const cgltf_image*, int> indices;
	// Images decoded although the bake would have written a sidecar for them,
	// by why: none on disk, or one older than the model (ReportUnbaked).
	int missing = 0;
	int stale = 0;

	enum class Sidecar { Missing, Stale, Rejected };

	// The baked chain for the NEXT index, if there is a usable one; else why not.
	std::optional<MipChain> Baked(Sidecar& why) {
		const std::string sidecar = EmbeddedImageSidecar(modelPath, model->images.size());
		std::error_code ec;
		if (!std::filesystem::exists(sidecar, ec)) {
			why = Sidecar::Missing; // not baked: decode, as before the bake existed
			return std::nullopt;
		}
		if (!BakedIsCurrent(sidecar, modelPath)) {
			// Said (ReportUnbaked), not skipped: a stale sidecar is a model
			// re-imported without a re-bake, and quietly decoding hides that the
			// bake is out of date.
			why = Sidecar::Stale;
			return std::nullopt;
		}
		auto chain = LoadDdsFile(sidecar);
		if (!chain) {
			log::Warn("{} - decoding the embedded image instead", chain.error());
			why = Sidecar::Rejected;
			return std::nullopt;
		}
		return std::move(*chain);
	}

	int Get(const cgltf_image* image) {
		if (!image) return -1;
		if (auto it = indices.find(image); it != indices.end()) return it->second;

		std::optional<Sidecar> unbaked;
		if (bakedImages) {
			Sidecar why = Sidecar::Missing;
			if (std::optional<MipChain> chain = Baked(why)) {
				const int index = static_cast<int>(model->images.size());
				model->images.emplace_back(); // placeholder: imageMips carries it
				model->imageMips.resize(model->images.size());
				model->imageMips.back() = std::move(*chain);
				indices[image] = index;
				return index;
			}
			unbaked = why;
		}

		std::expected<ImageData, std::string> loaded =
			std::unexpected(std::string("image has no data source"));
		if (image->buffer_view && image->buffer_view->buffer->data) {
			const auto* bytes = static_cast<const u8*>(image->buffer_view->buffer->data) +
								image->buffer_view->offset;
			loaded = LoadImageMemory(bytes, image->buffer_view->size);
		} else if (image->uri && std::strncmp(image->uri, "data:", 5) != 0) {
			loaded = LoadImageFile((baseDir / image->uri).string());
		}

		int index = -1;
		if (loaded) {
			// Counted only where the bake WOULD have written a sidecar: it skips an
			// image whose sides are not a multiple of 4 (BC7's block), which then
			// decodes on every load by design and is nothing to report.
			if (unbaked && loaded->width % 4 == 0 && loaded->height % 4 == 0) {
				if (*unbaked == Sidecar::Missing) ++missing;
				else if (*unbaked == Sidecar::Stale) ++stale;
			}
			index = static_cast<int>(model->images.size());
			model->images.push_back(std::move(*loaded));
			if (!model->imageMips.empty()) model->imageMips.resize(model->images.size());
		} else {
			log::Warn("glTF image skipped: {}", loaded.error());
		}
		indices[image] = index;
		return index;
	}
};

// One line per MODEL, once a run, for the images it had to decode: each costs
// ~50 ms at every load (a 2k decode plus its mips on the CPU), and nothing said
// so for a missing sidecar - the case a fresh fetch left behind (code-review
// C437). Once a run because a model loads again on every level that uses it,
// and the cause is the same each time. Model loading may run off the main
// thread, so the set of models already reported is locked.
void ReportUnbaked(const std::string& path, int missing, int stale, size_t images) {
	static std::mutex mx;
	static std::set<std::string> reported;
	{
		const std::scoped_lock lock(mx);
		if (!reported.insert(path).second) return;
	}
	log::Warn("{}: {} of its {} embedded images decoded at load - {} with no baked "
			  "sidecar, {} older than the model; run AssetBaker model-images",
			  path, missing + stale, images, missing, stale);
}

// ----------------------------------------------------------------------------
// Skeleton extraction.
// Topologically orders the skin's joints parent-before-child (the order the
// Animator's single-pass global-transform computation requires) and fills
// SkeletonData. Returns the node→joint index mapping; LoadGltf derives the
// slot→joint remap for vertex weights from it.
// ----------------------------------------------------------------------------
std::unordered_map<const cgltf_node*, int> BuildSkeleton(const cgltf_skin* skin,
														 SkeletonData& skeleton) {
	std::unordered_map<const cgltf_node*, int> nodeToSlot; // original slot in skin
	for (cgltf_size i = 0; i < skin->joints_count; ++i)
		nodeToSlot[skin->joints[i]] = static_cast<int>(i);

	// Order parents first.
	std::vector<const cgltf_node*> ordered;
	ordered.reserve(skin->joints_count);
	std::unordered_map<const cgltf_node*, bool> visited;
	auto visit = [&](this auto&& self, const cgltf_node* node) -> void {
		if (!node || !nodeToSlot.contains(node) || visited[node]) return;
		visited[node] = true;
		self(node->parent);
		ordered.push_back(node);
	};
	for (cgltf_size i = 0; i < skin->joints_count; ++i)
		visit(skin->joints[i]);

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

// ----------------------------------------------------------------------------
// Geometry extraction. One glTF primitive appends into one MeshData:
// positions/normals/uvs always, joints/weights when skinned. `slotRemap`
// maps glTF skin-slot indices to our topologically ordered joint indices
// (empty for unskinned models).
// ----------------------------------------------------------------------------
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

std::string EmbeddedImageSidecar(const std::string& modelPath, size_t index) {
	return std::format("{}.{}.dds", modelPath, index);
}

std::vector<bool> SrgbImages(const ModelData& model) {
	std::vector<bool> srgb(model.images.size(), false);
	for (const MaterialData& m : model.materials)
		if (m.baseColorImage >= 0 && static_cast<size_t>(m.baseColorImage) < srgb.size())
			srgb[static_cast<size_t>(m.baseColorImage)] = true;
	return srgb;
}

std::expected<ModelData, std::string> LoadGltf(const std::string& path,
											   const LoadOptions& opts) {
	const alloc::Counters before = alloc::ThisThread();
	cgltf_options options{};
	cgltf_data* data = nullptr;
	if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success)
		return std::unexpected(std::format("failed to parse glTF: {}", path));
	// Own the parse immediately: everything below allocates, so any exception
	// (or early return) must still reach cgltf_free.
	const std::unique_ptr<cgltf_data, decltype(&cgltf_free)> owned(data, &cgltf_free);
	if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success)
		return std::unexpected(std::format("failed to load glTF buffers: {}", path));

	ModelData model;
	// Reserved so pushing the clips never regrows the vector (see the clip loop
	// below). Were it to regrow, the clips would still MOVE, not copy, in a debug
	// build as in release: MSVC's iterator debugging allocates a proxy inside
	// each container's move constructor, but that constructor stays noexcept.
	// Checked here, so a member that would make a clip copy on growth - one with
	// a throwing move - fails to compile instead of quietly copying every
	// clip's key pools each time the vector grows.
	static_assert(std::is_nothrow_move_constructible_v<AnimationClipData>);
	model.clips.reserve(data->animations_count);
	ImageCache imageCache{data, std::filesystem::path(path).parent_path(), &model, path,
						  opts.bakedImages};

	// Materials (indices must match cgltf's so primitives can look them up).
	for (cgltf_size m = 0; m < data->materials_count; ++m) {
		const cgltf_material& src = data->materials[m];
		MaterialData mat;
		if (src.has_pbr_metallic_roughness) {
			const auto& pbr = src.pbr_metallic_roughness;
			mat.baseColorFactor = {pbr.base_color_factor[0], pbr.base_color_factor[1],
								   pbr.base_color_factor[2], pbr.base_color_factor[3]};
			mat.metallic = pbr.metallic_factor;
			mat.roughness = pbr.roughness_factor;
			if (pbr.base_color_texture.texture)
				mat.baseColorImage = imageCache.Get(pbr.base_color_texture.texture->image);
			// glTF packs roughness in G and metallic in B — the same layout our
			// MaterialParams::metalRough (ORM) slot samples, so it binds directly.
			if (pbr.metallic_roughness_texture.texture)
				mat.metalRoughImage =
					imageCache.Get(pbr.metallic_roughness_texture.texture->image);
		}
		if (src.normal_texture.texture)
			mat.normalImage = imageCache.Get(src.normal_texture.texture->image);
		mat.emissive = {src.emissive_factor[0], src.emissive_factor[1],
						src.emissive_factor[2]};
		mat.blend = src.alpha_mode == cgltf_alpha_mode_blend;
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

	// Animation clips. Each clip's keys land in TWO pooled arrays (times, values)
	// that its channels index into - see AnimationChannelData. A rigged Mixamo
	// model is 40 clips x 33 joints x T/R/S = 3,960 channels, and when each owned
	// two vectors its load cost ~8,000 allocations (~24,000 in debug, where MSVC's
	// iterator debugging makes every vector move allocate a proxy). Pooled, it is
	// a handful per clip, however many channels the clip has.
	//
	// Two things the files make cheap:
	//  * SHARED KEY TIMES. An exporter writes one time accessor for every channel
	//    sampled on the same frames (the Mixamo rigs: 3,960 channels, 74 distinct
	//    inputs), so each distinct input is read into `times` once and every
	//    channel using it points at the same range. Exact.
	//  * CONSTANT CHANNELS. Exporters key every joint's T, R and S whether it
	//    moves or not, and most do not. A channel whose values never change is
	//    stored as ONE key, and dropped outright when that key IS the joint's rest
	//    pose, since the Animator starts every sample from the rest pose anyway -
	//    which also spares the Animator sampling it every frame (the Mixamo rigs
	//    sample ~1,100 channels a frame instead of ~3,900).
	//    "Never change" and "is the rest pose" are within kConstantEpsilon, NOT
	//    bitwise, and that is what makes it pay: the exporter's rounding wobbles
	//    idle channels in the last digits (a scale of 0.99999994), so a bitwise
	//    test caught 25 of the Mixamo rigs' 3,960 channels and this catches
	//    ~2,900. It is LOSSY by at most that much in a joint's local T/R/S -
	//    a millionth of a unit, ~2.5 um at kUnit - and tools/AnimTest measures
	//    the resulting palette difference rather than assuming it.
	constexpr float kConstantEpsilon = 1e-6f;
	std::vector<std::pair<const cgltf_accessor*, u32>> timeRanges; // input -> first key
	std::vector<Vec4> scratch; // one channel's values while it is classified
	const auto wanted = [&](const cgltf_animation_channel& ch, ChannelPath& path) {
		if (!ch.target_node || !nodeToJoint.contains(ch.target_node)) return false;
		if (!ch.sampler->input->count || !ch.sampler->output->count) return false;
		switch (ch.target_path) {
		case cgltf_animation_path_type_translation: path = ChannelPath::Translation; return true;
		case cgltf_animation_path_type_rotation:    path = ChannelPath::Rotation; return true;
		case cgltf_animation_path_type_scale:       path = ChannelPath::Scale; return true;
		default: return false;
		}
	};
	// Component-wise, so a quaternion and its negation (the same rotation) count
	// as different - which only ever keeps a channel that could have gone.
	const auto same = [](const Vec4& a, const Vec4& b, int comps) {
		const float d[4] = {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
		for (int i = 0; i < comps; ++i)
			if (!(std::fabs(d[i]) <= kConstantEpsilon)) return false; // NaN: not same
		return true;
	};
	for (cgltf_size a = 0; a < data->animations_count; ++a) {
		const cgltf_animation& src = data->animations[a];
		AnimationClipData clip;
		clip.name = src.name ? src.name : std::format("clip{}", a);

		// Size the pools first, so neither grows: times EXACTLY (one range per
		// distinct input), values to the most they could need (every channel
		// kept whole), trimmed once the constant channels have been collapsed.
		size_t timeKeys = 0, valueKeys = 0, channelCount = 0;
		timeRanges.clear();
		for (cgltf_size c = 0; c < src.channels_count; ++c) {
			ChannelPath path;
			if (!wanted(src.channels[c], path)) continue;
			const cgltf_animation_sampler& s = *src.channels[c].sampler;
			++channelCount;
			valueKeys += s.output->count;
			if (std::ranges::find(timeRanges, s.input, &std::pair<const cgltf_accessor*, u32>::first) ==
				timeRanges.end()) {
				timeRanges.push_back({s.input, 0});
				timeKeys += s.input->count;
			}
		}
		if (channelCount == 0) continue; // nothing this skeleton can play
		clip.channels.reserve(channelCount);
		clip.times.reserve(timeKeys);
		clip.values.reserve(valueKeys);
		timeRanges.clear();

		for (cgltf_size c = 0; c < src.channels_count; ++c) {
			const cgltf_animation_channel& ch = src.channels[c];
			AnimationChannelData out;
			if (!wanted(ch, out.path)) continue;
			out.joint = nodeToJoint[ch.target_node];
			const cgltf_accessor* input = ch.sampler->input;
			const cgltf_accessor* output = ch.sampler->output;

			// Key times: read each distinct input once; the duration is the
			// latest key of any of them, exactly as when every channel had a copy.
			auto range = std::ranges::find(timeRanges, input,
										   &std::pair<const cgltf_accessor*, u32>::first);
			if (range == timeRanges.end()) {
				const u32 first = static_cast<u32>(clip.times.size());
				clip.times.resize(first + input->count);
				for (cgltf_size i = 0; i < input->count; ++i) {
					cgltf_accessor_read_float(input, i, &clip.times[first + i], 1);
					clip.duration = std::max(clip.duration, clip.times[first + i]);
				}
				range = timeRanges.insert(timeRanges.end(), {input, first});
			}

			const int comps = (out.path == ChannelPath::Rotation) ? 4 : 3;
			scratch.resize(output->count);
			for (cgltf_size i = 0; i < output->count; ++i) {
				float tmp[4] = {0, 0, 0, 1};
				cgltf_accessor_read_float(output, i, tmp, comps);
				scratch[i] = {tmp[0], tmp[1], tmp[2], tmp[3]};
			}
			// Every key within kConstantEpsilon of the first; the first is kept.
			const bool constant = std::ranges::all_of(
				scratch, [&](const Vec4& v) { return same(v, scratch[0], comps); });
			if (constant) {
				const JointData& j = model.skeleton.joints[out.joint];
				const Vec4 rest = out.path == ChannelPath::Translation
									  ? Vec4{j.restTranslation.x, j.restTranslation.y,
											 j.restTranslation.z, 0}
								  : out.path == ChannelPath::Scale
									  ? Vec4{j.restScale.x, j.restScale.y, j.restScale.z, 0}
									  : Vec4{j.restRotation.x, j.restRotation.y,
											 j.restRotation.z, j.restRotation.w};
				if (same(scratch[0], rest, comps)) continue; // the rest pose already says it
			}
			const u32 keys = constant ? 1u : static_cast<u32>(output->count);
			out.timeFirst = range->second;
			out.timeCount = constant ? 1u : static_cast<u32>(input->count);
			out.valueFirst = static_cast<u32>(clip.values.size());
			out.valueCount = keys;
			clip.values.insert(clip.values.end(), scratch.begin(), scratch.begin() + keys);
			clip.channels.push_back(out);
		}
		// The reserve assumed nothing collapsed; give back what it did not use.
		// One reallocation per clip, against the clip's whole value set otherwise
		// sitting there for the model's lifetime.
		clip.values.shrink_to_fit();
		// A clip whose every channel restates the rest pose is still a clip: it
		// plays (as the rest pose), which is what it did before any were dropped.
		model.clips.push_back(std::move(clip));
	}

	// Allocation cost rides the existing line: model loading is the heaviest
	// allocator in the game and the per-model number is what turns "loading
	// allocates a lot" into a name (see LoadQueue's table).
	const alloc::Counters after = alloc::ThisThread();
	const size_t baked = std::ranges::count_if(
		model.imageMips, [](const MipChain& c) { return !c.levels.empty(); });
	log::Info("Loaded glTF '{}': {} meshes, {} materials, {} joints, {} clips, "
			  "{} images ({} baked) [{} allocs, {:.1f} MB]",
			  path, model.meshes.size(), model.materials.size(),
			  model.skeleton.joints.size(), model.clips.size(), model.images.size(), baked,
			  after.allocs - before.allocs,
			  static_cast<double>(after.bytes - before.bytes) / (1024.0 * 1024.0));
	if (opts.bakedImages && opts.warnUnbaked && imageCache.missing + imageCache.stale > 0)
		ReportUnbaked(path, imageCache.missing, imageCache.stale, model.images.size());
	return model;
}

std::expected<ModelData, std::string> LoadModel(const std::string& path,
												const LoadOptions& opts) {
	auto ext = std::filesystem::path(path).extension().string();
	std::ranges::transform(ext, ext.begin(),
						   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (ext == ".gltf" || ext == ".glb") return LoadGltf(path, opts);
	if (ext == ".obj") return LoadObj(path);
	return std::unexpected(std::format("unsupported model format: {}", path));
}

} // namespace dungeon::assets
