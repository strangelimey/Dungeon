// ============================================================================
// Game/DungeonWorld_Models.cpp - the model cache: one parse and one GPU upload
// per model FILE, however many catalog kinds use it (declarations in
// DungeonWorld.h, "THE MODEL CACHE").
//
// Why it exists: the kind caches (MonsterKindFor, ItemKindFor, DecorationKindFor,
// FixtureKindFor) key by CATALOG ID, and content deliberately shares files - six
// skeleton variants on skeleton.gltf, five armours on leather_armor.glb, every
// enchanted blade on its plain twin's mesh, marble and stone columns on one
// column.gltf. Each of those kinds used to parse the file and upload its own
// meshes and textures, so the armour's textures sat in VRAM five times over.
//
// What is shared and what is not: the parsed ModelData, meshes[0]'s gfx::Mesh,
// and a multi-material model's GPU textures and submesh geometry. NOT shared: a
// multi-material model's MATERIALS - every kind gets its own copy of the
// template (MultiMaterialModel is copyable for exactly this), because a kind may
// bake catalog overrides into them (BakeCatalogMaterial) that must not repaint
// the others. Per-kind icon targets stay per kind for the same reason.
//
// Lifetime: everything is shared_ptr, so the cache holding a reference only ever
// DELAYS a free, never causes one early. The one path that drops kinds while
// the GPU may still read them (ReloadTypeKind) drains first, as it always did.
// ============================================================================
#include "Game/AssetUtil.h"
#include "Game/DungeonWorld.h"

using namespace DirectX;

namespace dungeon::game {

std::shared_ptr<const assets::ModelData> DungeonWorld::ModelFile(const std::string& file) {
	CachedModel& entry = m_modelCache[file];
	if (!entry.data)
		entry.data = std::make_shared<const assets::ModelData>(LoadModelOrDie(file));
	return entry.data;
}

std::shared_ptr<gfx::Mesh> DungeonWorld::ModelMesh(const std::string& file) {
	const std::shared_ptr<const assets::ModelData> data = ModelFile(file);
	CachedModel& entry = m_modelCache[file];
	if (!entry.mesh) entry.mesh = std::make_shared<gfx::Mesh>(m_device, data->meshes[0]);
	return entry.mesh;
}

std::unique_ptr<DungeonWorld::MultiMaterialModel> DungeonWorld::ModelMulti(
	const std::string& file) {
	const std::shared_ptr<const assets::ModelData> data = ModelFile(file);
	CachedModel& entry = m_modelCache[file];
	if (!entry.multi) entry.multi = BuildMultiMaterialModel(m_device, *data);
	return std::make_unique<MultiMaterialModel>(*entry.multi);
}

void DungeonWorld::ForgetModelFile(const std::string& file) {
	m_modelCache.erase(file);
}

// --- the asset picker's view of a pool file ---------------------------------
// Deliberately NOT through the cache: the picker browses every file in the pool,
// most of which no level uses, and caching them would keep each one's textures
// in VRAM for the session. The picker owns the result and drops it.
std::unique_ptr<DungeonWorld::PoolModelLook> DungeonWorld::LoadPoolModelLook(
	gfx::GraphicsDevice& device, const std::string& modelPath, const std::string& setStem,
	u32 thumbPx, const std::string& idleHint, PoolModelLook::Mount mount) {
	auto data = assets::LoadModel(modelPath, {.bakedImages = true});
	if (!data || data->meshes.empty()) return nullptr;
	std::unique_ptr<MultiMaterialModel> multi = BuildMultiMaterialModel(device, *data);

	auto look = std::make_unique<PoolModelLook>();
	look->textures = std::move(multi->textures);
	look->lo = multi->boundsMin;
	look->hi = multi->boundsMax;
	look->sinks = look->lo.y < -0.01f;
	look->FrameAboveFloor();
	look->rigged = data->skeleton.RootJoint() >= 0;
	if (look->rigged && !data->clips.empty()) {
		// The idle: the catalog's, else the library's `idle__` clips or a plain
		// `idle` (the baker's rigs). Never just the FIRST clip - the kit's first
		// is a spawn that starts lying on the floor.
		auto has = [&](const std::string& name) {
			return std::ranges::any_of(data->clips, [&](const assets::AnimationClipData& c) {
				return c.name == name;
			});
		};
		if (!idleHint.empty() && has(idleHint)) {
			look->idleClip = idleHint;
		} else {
			for (const assets::AnimationClipData& c : data->clips)
				if (c.name == "idle" || c.name.starts_with("idle__")) {
					look->idleClip = c.name;
					break;
				}
		}
		look->data = std::make_shared<const assets::ModelData>(std::move(*data));
	}
	// The CPU copy, wherever it now lives (a rig's moved into the look).
	const assets::ModelData& source = look->data ? *look->data : *data;

	// Which material wins, by the world's own split: a single-primitive .gltf is
	// drawn by its catalog set (MonsterKindFor takes the multi-material path only
	// past one primitive, which is why the giant spider is orange in play and
	// its embedded map is pale), while a .glb or a many-part model wears its own
	// materials (the item and decoration loaders' .glb path) and the set only
	// fills a part with no image. Loaded only if something will wear it.
	const bool setWins = multi->subs.size() == 1 && modelPath.ends_with(".gltf");
	std::shared_ptr<gfx::Texture> albedo, normal, mr;
	const bool wantsSet = !setStem.empty() &&
						  (setWins || std::ranges::any_of(multi->subs,
														  [](const MultiMaterialModel::Sub& s) {
															  return s.material.albedo == nullptr;
														  }));
	if (wantsSet) {
		auto load = [&](const std::string& stem, bool srgb) -> std::shared_ptr<gfx::Texture> {
			return thumbPx > 0 ? LoadTextureThumb(device, stem, thumbPx, srgb)
							   : TryLoadTextureFile(device, stem, srgb);
		};
		albedo = load(setStem, /*srgb*/ true);
		if (albedo) {
			normal = load(setStem + "_n", false);
			mr = load(setStem + "_mr", false);
			for (const auto& t : {albedo, normal, mr})
				if (t) look->textures.push_back(t);
		}
	}

	for (size_t i = 0; i < multi->subs.size(); ++i) {
		MultiMaterialModel::Sub& sub = multi->subs[i];
		if (setWins && albedo) {
			// As the world's single-mesh draw builds it: a fresh material with
			// the set on it (ApplyPropMaterial), none of the file's factors.
			gfx::MaterialParams m;
			m.doubleSided = true;
			ApplyPbr(m, albedo.get(), normal.get(), mr.get(), 0.0f, {1, 1, 1, 1}, 0.9f);
			sub.material = m;
		} else if (!sub.material.albedo) {
			// A part the model does not texture: the set as a prop draw applies
			// it (ApplyPbr), else the glTF's flat colour. Double-sided, since
			// such a part is usually hand-built geometry (the scene's default
			// PSO for those is CULL_NONE too).
			sub.material.doubleSided = true;
			if (albedo)
				ApplyPbr(sub.material, albedo.get(), normal.get(), mr.get(), 0.0f,
						 sub.material.baseColor, sub.material.roughness);
		}
		// A feature's inside drawn as its own, darker part (Mount). The split
		// is by triangle centroid against the mounting plane, on the vertices
		// as BuildMultiMaterialModel uploaded them (node transform baked).
		if (mount != PoolModelLook::Mount::Free && i < source.meshes.size() &&
			!source.meshes[i].skinned) {
			assets::MeshData outside = source.meshes[i];
			const XMMATRIX node = XMLoadFloat4x4(&outside.worldTransform);
			for (assets::Vertex& v : outside.vertices) {
				XMFLOAT3 p, n;
				XMStoreFloat3(&p, XMVector3Transform(
									  XMVectorSet(v.position.x, v.position.y, v.position.z, 1.0f),
									  node));
				XMStoreFloat3(&n, XMVector3Normalize(XMVector3TransformNormal(
									  XMVectorSet(v.normal.x, v.normal.y, v.normal.z, 0.0f),
									  node)));
				v.position = {p.x, p.y, p.z};
				v.normal = {n.x, n.y, n.z};
			}
			assets::MeshData inside = outside;
			outside.indices.clear();
			inside.indices.clear();
			const auto& src = source.meshes[i].indices;
			// How far behind the plane (units) a triangle must sit to be INSIDE.
			// Past surface relief - cracked paving sinks its slabs 0.04, and at
			// -0.01 whole stones went dark - but short of every real hole: the
			// drain's throat starts at 0.035, a niche is 0.22 deep.
			constexpr float kBehind = -0.05f;
			for (size_t t = 0; t + 2 < src.size(); t += 3) {
				const Vec3& a = outside.vertices[src[t]].position;
				const Vec3& b = outside.vertices[src[t + 1]].position;
				const Vec3& c = outside.vertices[src[t + 2]].position;
				const float depth = mount == PoolModelLook::Mount::Floor
										? (a.y + b.y + c.y) / 3.0f
										: (a.z + b.z + c.z) / 3.0f;
				auto& into = depth < kBehind ? inside.indices : outside.indices;
				into.insert(into.end(), {src[t], src[t + 1], src[t + 2]});
			}
			if (!inside.indices.empty() && !outside.indices.empty()) {
				auto outMesh = std::make_shared<gfx::Mesh>(device, outside);
				auto inMesh = std::make_shared<gfx::Mesh>(device, inside);
				gfx::MaterialParams shaded = sub.material;
				shaded.baseColor = {shaded.baseColor.x * PoolModelLook::kInsideShade,
									shaded.baseColor.y * PoolModelLook::kInsideShade,
									shaded.baseColor.z * PoolModelLook::kInsideShade,
									shaded.baseColor.w};
				look->meshes.push_back(outMesh);
				look->meshes.push_back(inMesh);
				look->parts.push_back({outMesh.get(), sub.material});
				look->parts.push_back({inMesh.get(), shaded});
				continue;
			}
		}
		look->meshes.push_back(sub.mesh);
		look->parts.push_back({sub.mesh.get(), sub.material});
	}
	return look;
}

void DungeonWorld::PoolModelLook::FitToPose(std::span<const Mat4> palette) {
	if (!data || palette.empty()) return;
	Vec3 l{1e9f, 1e9f, 1e9f}, h{-1e9f, -1e9f, -1e9f};
	for (const assets::MeshData& mesh : data->meshes) {
		// The node transform first, as BuildMultiMaterialModel baked it into the
		// uploaded vertices, then the skinning sum the scene shader does.
		const XMMATRIX node = XMLoadFloat4x4(&mesh.worldTransform);
		for (const assets::Vertex& v : mesh.vertices) {
			const XMVECTOR p = XMVector3Transform(
				XMVectorSet(v.position.x, v.position.y, v.position.z, 1.0f), node);
			XMVECTOR s = XMVectorZero();
			float total = 0.0f;
			for (int i = 0; i < 4; ++i) {
				if (v.weights[i] <= 0.0f || v.joints[i] >= palette.size()) continue;
				s = XMVectorAdd(s, XMVectorScale(XMVector3Transform(
													 p, XMLoadFloat4x4(&palette[v.joints[i]])),
												 v.weights[i]));
				total += v.weights[i];
			}
			if (total <= 0.0f) s = p; // an unweighted vertex stays put
			XMFLOAT3 f;
			XMStoreFloat3(&f, s);
			l = {std::min(l.x, f.x), std::min(l.y, f.y), std::min(l.z, f.z)};
			h = {std::max(h.x, f.x), std::max(h.y, f.y), std::max(h.z, f.z)};
		}
	}
	if (h.x >= l.x) {
		lo = l;
		hi = h;
		FrameAboveFloor();
	}
}

void DungeonWorld::PoolModelLook::FrameAboveFloor() {
	const float footprint = std::max(hi.x - lo.x, hi.z - lo.z);
	lo.y = std::max(lo.y, std::min(0.0f, hi.y) - 0.5f * footprint);
}

} // namespace dungeon::game
