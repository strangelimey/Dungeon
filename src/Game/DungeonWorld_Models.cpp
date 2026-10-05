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
namespace {
// Clips `mesh` to the half-space where coordinate `axis` (0 x, 1 y, 2 z) is >=
// `cutAt`: whole triangles below are dropped, a straddling one is cut along the
// plane (one or two triangles, new vertices interpolated on the crossing edges),
// winding kept. Returns whether anything was cut. For a picture only (the asset
// picker's floor features and window bores) - the vertices it adds are never
// welded.
bool ClipBelow(assets::MeshData& mesh, float cutAt, int axis = 1) {
	const std::vector<u32> tris = std::move(mesh.indices);
	mesh.indices.clear();
	bool cut = false;
	auto along = [axis](const Vec3& p) { return axis == 0 ? p.x : axis == 1 ? p.y : p.z; };
	auto crossing = [&](u32 a, u32 b) {
		const assets::Vertex va = mesh.vertices[a], vb = mesh.vertices[b];
		const float t = (cutAt - along(va.position)) / (along(vb.position) - along(va.position));
		assets::Vertex v = t < 0.5f ? va : vb; // joints/weights from the nearer end
		v.position = Lerp(va.position, vb.position, t);
		(axis == 0 ? v.position.x : axis == 1 ? v.position.y : v.position.z) = cutAt;
		XMStoreFloat3(&v.normal,
					  XMVector3Normalize(XMLoadFloat3(&va.normal) * (1.0f - t) +
										 XMLoadFloat3(&vb.normal) * t));
		v.uv = {va.uv.x + (vb.uv.x - va.uv.x) * t, va.uv.y + (vb.uv.y - va.uv.y) * t};
		mesh.vertices.push_back(v);
		return static_cast<u32>(mesh.vertices.size() - 1);
	};
	for (size_t t = 0; t + 2 < tris.size(); t += 3) {
		const u32 v[3] = {tris[t], tris[t + 1], tris[t + 2]};
		bool keep[3];
		int kept = 0;
		for (int k = 0; k < 3; ++k) kept += (keep[k] = along(mesh.vertices[v[k]].position) >= cutAt);
		if (kept == 3) {
			mesh.indices.insert(mesh.indices.end(), {v[0], v[1], v[2]});
			continue;
		}
		cut = true;
		if (kept == 0) continue;
		// Rotate so the odd one out comes first, keeping the winding.
		int r = 0;
		for (int k = 0; k < 3; ++k)
			if (keep[k] == (kept == 1)) r = k;
		const u32 a = v[r], b = v[(r + 1) % 3], c = v[(r + 2) % 3];
		const u32 ab = crossing(a, b), ac = crossing(a, c);
		if (kept == 1) { // a alone survives: a, ab, ac
			mesh.indices.insert(mesh.indices.end(), {a, ab, ac});
		} else { // a alone is cut away: ab, b, c and ab, c, ac
			mesh.indices.insert(mesh.indices.end(), {ab, b, c, ab, c, ac});
		}
	}
	return cut;
}

// Drops the OUTER SKIN of a well's inside: the faces on its outer hull - the
// cell's box (|x| or |z| at its half-extent) or the outermost cylinder (the
// largest radius any inside vertex reaches) - that face outward. The pit's and
// stairwells' walls are solid boxes and drums, so culling back faces leaves
// their outsides, which then hang under the tile; in play that side is rock and
// never seen. Only whole triangles on the hull and facing out go, so a stair's
// risers and the walls' inner faces stay.
void DropWellSkin(assets::MeshData& mesh) {
	if (mesh.indices.empty()) return;
	float hx = 0.0f, hz = 0.0f, rmax = 0.0f;
	for (const u32 i : mesh.indices) {
		const Vec3& p = mesh.vertices[i].position;
		hx = std::max(hx, std::abs(p.x));
		hz = std::max(hz, std::abs(p.z));
		rmax = std::max(rmax, std::sqrt(p.x * p.x + p.z * p.z));
	}
	constexpr float kOnHull = 0.01f;
	auto onHull = [&](const Vec3& p) {
		return std::abs(p.x) >= hx - kOnHull || std::abs(p.z) >= hz - kOnHull ||
			   std::sqrt(p.x * p.x + p.z * p.z) >= rmax - kOnHull;
	};
	const std::vector<u32> tris = std::move(mesh.indices);
	mesh.indices.clear();
	for (size_t t = 0; t + 2 < tris.size(); t += 3) {
		const assets::Vertex& a = mesh.vertices[tris[t]];
		const assets::Vertex& b = mesh.vertices[tris[t + 1]];
		const assets::Vertex& c = mesh.vertices[tris[t + 2]];
		const float cx = (a.position.x + b.position.x + c.position.x) / 3.0f;
		const float cz = (a.position.z + b.position.z + c.position.z) / 3.0f;
		const float nx = a.normal.x + b.normal.x + c.normal.x;
		const float nz = a.normal.z + b.normal.z + c.normal.z;
		const bool outward = nx * cx + nz * cz > 0.0f;
		if (outward && onHull(a.position) && onHull(b.position) && onHull(c.position))
			continue;
		mesh.indices.insert(mesh.indices.end(), {tris[t], tris[t + 1], tris[t + 2]});
	}
}
} // namespace

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
	// A real depth, not a sliver: the door chain dips 0.027 below its origin,
	// and at -0.01 that counted as sinking and the tile looked straight down
	// the length of the chain - a dot.
	look->sinks = look->lo.y < -0.1f;
	look->mount = mount;
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

	// A wall feature BORED through the wall (a window: its tube runs half a
	// square back, out to the other side) is cut a little behind its face and
	// the dark beyond set at the cut. Whole, the tube was a thin notch at the
	// tile's angle and its far end poked out past the panel's edge. A niche
	// (0.22, closed at the back) is shallower and kept.
	constexpr float kBoreDeeper = 0.3f, kBoreCut = -0.15f;
	const bool bored =
		mount == PoolModelLook::Mount::Wall && -multi->boundsMin.z > kBoreDeeper;
	if (bored) look->lo.z = std::max(look->lo.z, kBoreCut);

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
		const bool hasInside = mount == PoolModelLook::Mount::Floor ||
							   mount == PoolModelLook::Mount::Wall ||
							   mount == PoolModelLook::Mount::CeilingWell;
		if (hasInside && i < source.meshes.size() && !source.meshes[i].skinned) {
			// How far a point sits BEHIND the mounting plane (positive = inside):
			// below the floor at y = 0, behind the wall face at z = 0, or above a
			// ceiling hole's rim - its lowest point, where the ceiling is.
			const float rimY = multi->boundsMin.y;
			auto behind = [&](const Vec3& a, const Vec3& b, const Vec3& c) {
				switch (mount) {
				case PoolModelLook::Mount::Floor: return -(a.y + b.y + c.y) / 3.0f;
				case PoolModelLook::Mount::Wall: return -(a.z + b.z + c.z) / 3.0f;
				default: return (a.y + b.y + c.y) / 3.0f - rimY;
				}
			};
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
			// A floor feature is CUT at its framed depth (FrameAboveFloor): the
			// drain's and recess's shafts run four squares down so that no one in
			// play can find the bottom, which a picture of the tile has no use
			// for - with them gone it can be shown at a three-quarter view
			// (kFloorFeatureTilt) instead of straight down a well.
			const bool clipped =
				(mount == PoolModelLook::Mount::Floor && ClipBelow(outside, look->lo.y)) ||
				(bored && ClipBelow(outside, kBoreCut, /*z*/ 2));
			look->cutAway = look->cutAway || clipped;
			const std::vector<u32> src = std::move(outside.indices);
			assets::MeshData inside = outside;
			outside.indices.clear();
			inside.indices.clear();
			// How far behind the plane (units) a triangle must sit to be INSIDE.
			// Past surface relief - cracked paving sinks its slabs 0.04, and at
			// -0.01 whole stones went dark - but short of every real hole: the
			// drain's throat starts at 0.035, a niche is 0.22 deep.
			constexpr float kBehind = 0.05f;
			for (size_t t = 0; t + 2 < src.size(); t += 3) {
				const Vec3& a = outside.vertices[src[t]].position;
				const Vec3& b = outside.vertices[src[t + 1]].position;
				const Vec3& c = outside.vertices[src[t + 2]].position;
				auto& into = behind(a, b, c) > kBehind ? inside.indices : outside.indices;
				into.insert(into.end(), {src[t], src[t + 1], src[t + 2]});
			}
			if (mount != PoolModelLook::Mount::Wall) DropWellSkin(inside);
			if (!inside.indices.empty() && !outside.indices.empty()) {
				auto outMesh = std::make_shared<gfx::Mesh>(device, outside);
				look->meshes.push_back(outMesh);
				look->parts.push_back({outMesh.get(), sub.material});
				// The inside in DEPTH BANDS, lighter at the mouth and darker
				// down (kInsideShadeTop -> kInsideShade): one flat shade made a
				// stairwell's treads a single dark sheet, and a gradient is what
				// says they go DOWN. Banded by triangle centroid, since the mesh
				// carries no vertex colour to grade it smoothly.
				auto depthOf = [&](size_t t) {
					return behind(inside.vertices[inside.indices[t]].position,
								  inside.vertices[inside.indices[t + 1]].position,
								  inside.vertices[inside.indices[t + 2]].position);
				};
				float deepest = 0.0f;
				for (size_t t = 0; t + 2 < inside.indices.size(); t += 3)
					deepest = std::max(deepest, depthOf(t));
				constexpr int kBands = PoolModelLook::kInsideBands;
				std::array<assets::MeshData, kBands> bands;
				for (assets::MeshData& band : bands) band.vertices = inside.vertices;
				for (size_t t = 0; t + 2 < inside.indices.size(); t += 3) {
					const float f = deepest > 0.0f ? depthOf(t) / deepest : 1.0f;
					const int k = std::clamp(static_cast<int>(f * kBands), 0, kBands - 1);
					bands[k].indices.insert(bands[k].indices.end(),
											{inside.indices[t], inside.indices[t + 1],
											 inside.indices[t + 2]});
				}
				for (int k = 0; k < kBands; ++k) {
					if (bands[k].indices.empty()) continue;
					const float s = PoolModelLook::kInsideShadeTop +
									(PoolModelLook::kInsideShade - PoolModelLook::kInsideShadeTop) *
										static_cast<float>(k) / (kBands - 1);
					gfx::MaterialParams shaded = sub.material;
					shaded.baseColor = {shaded.baseColor.x * s, shaded.baseColor.y * s,
										shaded.baseColor.z * s, shaded.baseColor.w};
					// An inside is only ever met from WITHIN - in play there is
					// rock behind it - so its back faces are culled: seen from
					// outside the tile a well's walls vanish rather than hang
					// under it as a box.
					shaded.doubleSided = false;
					auto bandMesh = std::make_shared<gfx::Mesh>(device, bands[k]);
					look->meshes.push_back(bandMesh);
					look->parts.push_back({bandMesh.get(), shaded});
				}
				continue;
			}
			if (clipped) { // all on one side, but cut: draw the cut mesh, never the whole
				assets::MeshData& only = inside.indices.empty() ? outside : inside;
				auto mesh = std::make_shared<gfx::Mesh>(device, only);
				look->meshes.push_back(mesh);
				look->parts.push_back({mesh.get(), sub.material});
				continue;
			}
		}
		look->meshes.push_back(sub.mesh);
		look->parts.push_back({sub.mesh.get(), sub.material});
	}

	// THE DARK BEYOND. A wall-sized panel (a square wide and tall, a fraction
	// deep - an arch, the archway, a door frame) stands IN a wall, and what
	// shows through its opening in play is the dark passage or room beyond. In
	// a tile it was the icon's light halo, so an arch's opening read as a grey
	// blob and the stonework round it as nothing much - the three arches tiled
	// alike. A near-black backdrop just behind the back face fixes that: hidden
	// wherever the panel is solid, seen only through holes. Not for a plain wall
	// feature (its inside is already shaded, and a niche is closed) - but for a
	// BORED one, at the bore's cut: set at the far end of a window's half-square
	// tube it stood out past the panel as a slab.
	const Vec3& bmin = multi->boundsMin;
	const Vec3& bmax = multi->boundsMax;
	const bool panel = bmax.x - bmin.x >= 0.9f && bmax.y - bmin.y >= 0.9f &&
					   bmax.z - bmin.z <= 0.35f;
	if (!look->rigged && ((panel && mount != PoolModelLook::Mount::Wall) || bored)) {
		assets::MeshData back;
		const float z = (bored ? kBoreCut : bmin.z) - 0.005f;
		back.vertices.resize(4);
		// Inset from the sides and top - a frame's back can be narrower than its
		// bounds, and an edge of black peeked past the door frame's jamb - but
		// not the bottom: an arch's opening runs down to the floor.
		// A bore's backdrop sits 0.15 behind the face, and the preview's swing
		// (+-0.7 rad) slides it ~0.1 sideways - so it is inset past that, on
		// every side (a window does not reach the floor).
		const float inset = bored ? 0.12f : 0.04f;
		const float insetBottom = bored ? inset : 0.0f;
		const float xs[4] = {bmin.x + inset, bmax.x - inset, bmax.x - inset, bmin.x + inset};
		const float ys[4] = {bmin.y + insetBottom, bmin.y + insetBottom, bmax.y - inset,
							 bmax.y - inset};
		for (int k = 0; k < 4; ++k) {
			back.vertices[k].position = {xs[k], ys[k], z};
			back.vertices[k].normal = {0.0f, 0.0f, 1.0f}; // toward the room
			back.vertices[k].uv = {k == 1 || k == 2 ? 1.0f : 0.0f, k >= 2 ? 0.0f : 1.0f};
		}
		back.indices = {0, 1, 2, 0, 2, 3};
		gfx::MaterialParams dark;
		dark.baseColor = {0.025f, 0.025f, 0.03f, 1.0f};
		dark.metallic = 0.0f;
		dark.roughness = 1.0f;
		dark.doubleSided = true;
		auto mesh = std::make_shared<gfx::Mesh>(device, back);
		look->meshes.push_back(mesh);
		look->parts.push_back({mesh.get(), dark});
		look->backed = true;
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

void DungeonWorld::PoolModelLook::AddContext(PoolModelLook&& context) {
	for (gfx::PreviewSubmesh part : context.parts) {
		const float s = kContextShade;
		part.material.baseColor = {part.material.baseColor.x * s, part.material.baseColor.y * s,
								   part.material.baseColor.z * s, part.material.baseColor.w};
		parts.push_back(part);
	}
	textures.insert(textures.end(), context.textures.begin(), context.textures.end());
	meshes.insert(meshes.end(), context.meshes.begin(), context.meshes.end());
	lo = {std::min(lo.x, context.lo.x), std::min(lo.y, context.lo.y),
		  std::min(lo.z, context.lo.z)};
	hi = {std::max(hi.x, context.hi.x), std::max(hi.y, context.hi.y),
		  std::max(hi.z, context.hi.z)};
}

void DungeonWorld::PoolModelLook::FrameSlenderTop() {
	const float h = hi.y - lo.y, w = std::max(hi.x - lo.x, hi.z - lo.z);
	if (rigged || w <= 0.0f || h < kSlender * w) return;
	lo.y = hi.y - kSlenderTop * h;
}

void DungeonWorld::PoolModelLook::FrameAboveFloor() {
	// A WELL within a square of depth (the pit, a stairwell) is framed whole -
	// its steps are the point. Only a SHAFT deeper than that (the drain's and
	// recess's four squares) is framed, and for a floor feature cut, at half a
	// square: deep enough to show the throat, never the stick.
	const float footprint = std::max(hi.x - lo.x, hi.z - lo.z);
	const float top = std::min(0.0f, hi.y);
	if (lo.y < top - footprint) lo.y = top - 0.5f * footprint;
}

} // namespace dungeon::game
