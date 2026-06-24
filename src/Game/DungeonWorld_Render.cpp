// ============================================================================
// Game/DungeonWorld_Render.cpp — the world's draw path (declarations in
// DungeonWorld.h). Split out of DungeonWorld.cpp: frustum / light-sphere culling
// (ViewCull), the shared scene-geometry submission, the surface draw, and the
// shadow-cube + main scene passes. The command list arrives from
// GraphicsDevice::BeginFrame already cleared and bound.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Assets/File.h"
#include "Assets/Image.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/DungeonMeshBuilder.h"
#include "Graphics/Lights.h"
#include "Graphics/ModelPreview.h"
#include "Graphics/Texture.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <queue>

using namespace DirectX;

namespace dungeon::game {

// --- view culling -----------------------------------------------------------

DungeonWorld::ViewCull DungeonWorld::ViewCull::FromFrustum(const Mat4& m) {
	// Gribb-Hartmann from a row-vector view-proj (clip = p * M): the clip
	// components read the COLUMNS of M, so planes are column sums/differences.
	const Vec4 c1{m._11, m._21, m._31, m._41};
	const Vec4 c2{m._12, m._22, m._32, m._42};
	const Vec4 c3{m._13, m._23, m._33, m._43};
	const Vec4 c4{m._14, m._24, m._34, m._44};
	auto add = [](const Vec4& a, const Vec4& b) {
		return Vec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
	};
	auto sub = [](const Vec4& a, const Vec4& b) {
		return Vec4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
	};
	ViewCull cull;
	cull.isSphere = false;
	const Vec4 raw[6] = {add(c4, c1), sub(c4, c1), add(c4, c2),
						 sub(c4, c2), c3,          sub(c4, c3)}; // L R B T near far
	for (int i = 0; i < 6; ++i) {
		Vec4 p = raw[i];
		const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
		if (len > 1e-8f) { p.x /= len; p.y /= len; p.z /= len; p.w /= len; }
		cull.planes[i] = p;
	}
	return cull;
}

DungeonWorld::ViewCull DungeonWorld::ViewCull::FromSphere(const Vec3& center,
														 float radius) {
	ViewCull cull;
	cull.isSphere = true;
	cull.sphereC = center;
	cull.sphereR = radius;
	return cull;
}

bool DungeonWorld::ViewCull::TestSphere(const Vec3& c, float r) const {
	if (isSphere) {
		const Vec3 d = Sub(c, sphereC);
		const float reach = sphereR + r;
		return d.x * d.x + d.y * d.y + d.z * d.z <= reach * reach;
	}
	for (const Vec4& p : planes)
		if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -r) return false; // outside
	return true;
}

bool DungeonWorld::ViewCull::TestAABB(const Vec3& lo, const Vec3& hi) const {
	if (isSphere) {
		// Squared distance from the sphere center to the AABB.
		auto axis = [](float c, float l, float h) {
			const float v = c < l ? l - c : (c > h ? c - h : 0.0f);
			return v * v;
		};
		const float d2 = axis(sphereC.x, lo.x, hi.x) + axis(sphereC.y, lo.y, hi.y) +
						 axis(sphereC.z, lo.z, hi.z);
		return d2 <= sphereR * sphereR;
	}
	for (const Vec4& p : planes) {
		// Positive vertex: the AABB corner farthest along the plane normal. If
		// it is outside, the whole box is (conservative — no false negatives).
		const float px = p.x >= 0 ? hi.x : lo.x;
		const float py = p.y >= 0 ? hi.y : lo.y;
		const float pz = p.z >= 0 ? hi.z : lo.z;
		if (p.x * px + p.y * py + p.z * pz + p.w < 0.0f) return false;
	}
	return true;
}

void DungeonWorld::NewFrame(u32 frameIndex) {
	if (m_particleBatch) m_particleBatch->NewFrame(frameIndex);
}

bool DungeonWorld::AnimatedCasterNear(const gfx::PointLight& light) const {
	auto inReach = [&](const Vec3& c, float r) {
		const Vec3 d = Sub(c, light.position);
		const float reach = light.radius + r;
		return d.x * d.x + d.y * d.y + d.z * d.z <= reach * reach;
	};
	if (m_pillarActive && inReach({m_pillarPos.x, 1.2f, m_pillarPos.z}, 1.2f))
		return true; // sway
	for (const Monster& m : m_monsters) {
		if (!m.Alive() && m.deathAnim <= 0.0f) continue; // a dying monster still animates its cube
		const Vec3 c = m_map.CellCenter(m.x, m.z);
		if (inReach({c.x, 1.0f, c.z}, 1.5f)) return true;
	}
	return false;
}

// Renders the cube shadow maps for every light that holds a slot this frame.
// Runs before the main pass with the shadow pipeline bound. A slot's cube is
// reused from the previous frame (left in its SRV state, the barrier guard
// skips it) unless the light changed/moved, a flicker tick is due, geometry
// changed, or an animating caster sits within the light.
void DungeonWorld::RenderShadowMaps(ID3D12GraphicsCommandList* list) {
	m_shadows.BeginPass();
	const u32 rev = m_map.Revision();

	for (size_t i = 0; i < m_lights.points.size(); ++i) {
		const gfx::PointLight& light = m_lights.points[i];
		if (light.shadowSlot < 0) continue;
		// The scheduler owns the reuse decision (and records the render); we only
		// supply the live map revision and the world's animating-caster verdict.
		if (!m_shadows.ShouldRender(light, i, rev, AnimatedCasterNear(light)))
			continue; // reuse the cube already bound as an SRV

		const ViewCull cull = ViewCull::FromSphere(light.position, light.radius);
		for (u32 face = 0; face < 6; ++face) {
			m_renderer.BeginShadowFace(list, static_cast<u32>(light.shadowSlot), face,
									   light.position, light.radius);
			SubmitSceneGeometry(list, &cull);
		}
	}
	m_renderer.EndShadows(list);
	m_device.BindBackBuffer(list); // the shadow pass redirected the OM
}

void DungeonWorld::RenderScene(ID3D12GraphicsCommandList* list) {
	m_renderer.BeginScene(list, m_camera, m_lights,
						  m_dustEnabled ? m_atmosphere : gfx::Atmosphere{});
	const ViewCull cull = ViewCull::FromFrustum(m_camera.ViewProj());
	SubmitSceneGeometry(list, &cull);
	// Transparent flame/spark/smoke billboards last, over the opaque scene.
	m_particleBatch->Render(list, m_camera, m_particleScratch);
}

void DungeonWorld::DrawMultiMaterial(ID3D12GraphicsCommandList* list,
									const MultiMaterialModel& model, const Mat4& world) {
	for (const MultiMaterialModel::Sub& sub : model.subs)
		m_renderer.DrawMesh(list, *sub.mesh, world, sub.material);
}

void DungeonWorld::SubmitSceneGeometry(ID3D12GraphicsCommandList* list,
									  const ViewCull* cull) {
	// A discrete mesh draws only if its bounding sphere passes the cull (camera
	// frustum in the main pass, light reach in the shadow pass).
	auto visible = [&](const Vec3& c, float r) {
		return !cull || cull->TestSphere(c, r);
	};

	DrawSurface(list, m_walls, cull);
	DrawSurface(list, m_floors, cull);
	DrawSurface(list, m_ceilings, cull);

	// Pillar — carved jade. The color is the model's flat baseColorFactor (a deep,
	// muted jade); the stone set contributes ONLY its normal + ORM maps, giving the
	// surface real micro-relief and roughness variation so it reads as polished
	// mineral rather than a smooth wet tube. The peacock-ore albedo (purple) is
	// deliberately dropped. Satin roughness, fully non-metallic.
	if (m_pillarActive && visible({m_pillarPos.x, 1.2f, m_pillarPos.z}, 1.8f)) {
		Mat4 pillarWorld = Mat4Identity();
		pillarWorld._41 = m_pillarPos.x;
		pillarWorld._43 = m_pillarPos.z;
		gfx::MaterialParams pillarMaterial;
		pillarMaterial.baseColor = m_pillarModel.materials[0].baseColorFactor;
		pillarMaterial.metallic = 0.0f;
		pillarMaterial.roughness = 0.55f;
		if (m_pillarTex) {
			pillarMaterial.normalMap = m_pillarTex->normal.get();
			pillarMaterial.metalRough = m_pillarTex->mr.get(); // modulates rough/metal
		}
		m_renderer.DrawMesh(list, *m_pillarMesh, pillarWorld, pillarMaterial,
							m_pillarAnimator.Palette());
	}

	// Static architecture decorations (columns, archways, fountains, ...):
	// textured stone/wood with bump + parallax + ORM, falling back to the flat
	// glTF material color if the texture set is missing.
	for (const Decoration& deco : m_decorations) {
		// radius 2.0 covers the widest prop (the archway spans the full cell).
		if (!visible({deco.world._41, 1.2f, deco.world._43}, 2.0f)) continue;
		// Authored multi-material model: draw each submesh with its own glTF
		// material (steel blade, brass guard, leather grip, ...). Shared by the
		// shadow + main passes, so these also cast shadows.
		if (deco.kind->multi) {
			DrawMultiMaterial(list, *deco.kind->multi, deco.world);
			continue;
		}
		gfx::MaterialParams material;
		material.doubleSided = !deco.kind->authored; // authored meshes back-cull
		ApplyPropMaterial(material, deco.kind->tex, deco.kind->color, 0.85f);
		material.alphaCutoff = deco.kind->alphaCutoff; // > 0: render the mask's gaps
		m_renderer.DrawMesh(list, *deco.kind->mesh, deco.world, material);
	}

	// Floor items: the shared carved-stone tablet. RUNES draw per element with
	// their PBR set (parallax cuts the glyph in) and an element AURA — a faint
	// emissive the shader concentrates at the silhouette (Fresnel) plus the
	// pulsing point light it casts (UpdateLights), both on the same RunePulse.
	// Other categories are PLACEHOLDERS: the same slab, flat-tinted by category
	// with a soft steady self-glow so they read on a dark floor (no cast light).
	if (m_runeMesh) {
		for (const Item& item : m_items) {
			if (item.collected) continue;
			const Vec3 c = SlotCenter(item.x, item.z, SizeClass::Medium, item.slot);
			if (!visible({c.x, 0.3f, c.z}, 0.8f)) continue;
			// A model item (e.g. a weapon) draws as its actual 3D model on the floor
			// (grounded via GroundOffsetY, real-size), each part with its own
			// material — not the tablet.
			if (item.kind->model) {
				Mat4 w = Mat4Identity();
				w._41 = c.x;
				w._42 = item.kind->model->GroundOffsetY();
				w._43 = c.z;
				DrawMultiMaterial(list, *item.kind->model, w);
				continue;
			}
			// Non-rune placeholders render scaled UP (kItemPlaceholderScale) — bigger
			// than the rune tablet so they read on a dark floor (pickup is a
			// floor-quarter click test, independent of the rendered size).
			const float scale = item.kind->isRune ? 1.0f : kItemPlaceholderScale;
			Mat4 world = Mat4Identity();
			world._11 = world._22 = world._33 = scale;
			world._41 = c.x;
			world._43 = c.z;
			gfx::MaterialParams material;
			material.doubleSided = false; // authored slab: back-cull
			const Vec4& g = item.kind->glow;
			if (item.kind->isRune) {
				ApplyPropMaterial(material, item.kind->tex,
								  m_runeModel.materials[0].baseColorFactor, 0.85f);
				// Emissive scaled well below the old internal glow — the shader's
				// Fresnel turns it into a rim aura, and the cast light (UpdateLights,
				// same RunePulse) does the surrounding glow, so they breathe together.
				const float pulse = RunePulse(m_time, item.id);
				constexpr float kAura = 0.9f; // soft aura, not a glowing panel
				material.emissive = {g.x * pulse * kAura, g.y * pulse * kAura,
									 g.z * pulse * kAura};
			} else {
				// Placeholder: flat category tint as base colour + a steady self-glow
				// of the same hue so the tablet reads clearly on an unlit floor (these
				// items cast no light of their own, unlike runes).
				ApplyPropMaterial(material, nullptr, g, 0.7f);
				constexpr float kSelf = 0.55f;
				material.emissive = {g.x * kSelf, g.y * kSelf, g.z * kSelf};
			}
			m_renderer.DrawMesh(list, *m_runeMesh, world, material);
		}
	}

	// Monsters: bone/bandage/slime PBR sets, bound by type name. The flat-color
	// fallback keeps the old look if a set is missing (blob glistens wetly).
	for (const Monster& monster : m_monsters) {
		if (!monster.Alive() && monster.deathAnim <= 0.0f) continue; // gone once death anim ends
		const MonsterKind& kind = *monster.kind;
		const Vec3 pos = monster.visualPos; // glides between cells while chasing
		if (!visible({pos.x, 1.0f, pos.z}, 1.5f)) continue;
		Mat4 world;
		XMStoreFloat4x4(&world, XMMatrixRotationY(monster.yaw) *
									XMMatrixTranslation(pos.x, 0, pos.z));
		gfx::MaterialParams material;
		const float fallbackRough = kind.fallbackRoughness;
		ApplyPropMaterial(material, kind.tex,
						  kind.model.materials[0].baseColorFactor, fallbackRough);
		m_renderer.DrawMesh(list, *kind.mesh, world, material,
							monster.animator.Palette());
	}

	// Fire props: worn iron sconce + bronze brazier (bump + parallax + ORM),
	// falling back to flat metallic iron if the sets are missing.
	for (const Fire& fire : m_fires) {
		if (!visible(fire.flamePos, 1.2f)) continue;
		gfx::MaterialParams metal;
		const Vec4 fallback = fire.brazier ? m_brazierColor : m_sconceColor;
		ApplyPropMaterial(metal, fire.brazier ? m_brazierTex : m_sconceTex,
						  fallback, 0.5f);
		if (!metal.albedo) metal.metallic = 1.0f; // flat fallback reads as metal
		m_renderer.DrawMesh(list, fire.brazier ? *m_brazierMesh : *m_sconceMesh,
							fire.world, metal);
	}
}

void DungeonWorld::DrawSurface(ID3D12GraphicsCommandList* list,
							   const Surface& surface, const ViewCull* cull) {
	const Mat4 identity = Mat4Identity();
	for (const SurfaceChunk& chunk : surface.chunks) {
		if (cull && !cull->TestAABB(chunk.boundsMin, chunk.boundsMax)) continue;
		const int v = chunk.variant;
		// Surfaces always carry an albedo, so the fallback color/roughness are
		// unused here; the ORM map (when present) drives roughness/metallic.
		gfx::MaterialParams material;
		ApplyPbr(material, surface.albedo[v].get(), surface.normal[v].get(),
				 surface.mr[v].get(), surface.heightScale, {}, 0.0f);
		m_renderer.DrawMesh(list, *chunk.mesh, identity, material);
	}
}

// --- baked 3D item icons ----------------------------------------------------

namespace {
// A soft round disc: white RGB with a radial-falloff alpha (1 at the core, 0 by
// the edge). The bake tints + alpha-scales it into a translucent halo behind the
// item so a thin weapon reads on any slot colour.
std::unique_ptr<gfx::Texture> MakeHaloTexture(gfx::GraphicsDevice& device, u32 size) {
	assets::ImageData img;
	img.width = img.height = size;
	img.pixels.resize(static_cast<size_t>(size) * size * 4);
	const float c = (size - 1) * 0.5f;
	const float inner = size * 0.18f; // solid core
	const float outer = size * 0.70f; // big soft glow reaching toward the corners
	for (u32 y = 0; y < size; ++y)
		for (u32 x = 0; x < size; ++x) {
			const float dx = static_cast<float>(x) - c, dy = static_cast<float>(y) - c;
			const float d = std::sqrt(dx * dx + dy * dy);
			float a = std::clamp(1.0f - (d - inner) / (outer - inner), 0.0f, 1.0f);
			a *= a; // ease for a softer edge
			u8* px = &img.pixels[(static_cast<size_t>(y) * size + x) * 4];
			px[0] = px[1] = px[2] = 255; // white; tinted at draw time
			px[3] = static_cast<u8>(a * 255.0f + 0.5f);
		}
	return std::make_unique<gfx::Texture>(device, img, /*srgb=*/false);
}
} // namespace

void DungeonWorld::BakeItemIconsIfNeeded(ID3D12GraphicsCommandList* list,
										 gfx::SpriteBatch& sprites) {
	if (m_itemIconsBaked) return;
	m_itemIconsBaked = true;
	if (!m_iconHalo) m_iconHalo = MakeHaloTexture(m_device, kIconSize);

	// Shared depth target for the bakes (created once, icon-sized).
	if (!m_iconDepth) {
		ID3D12Device* d = m_device.Device();
		D3D12_RESOURCE_DESC depth{};
		depth.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		depth.Width = kIconSize;
		depth.Height = kIconSize;
		depth.DepthOrArraySize = 1;
		depth.MipLevels = 1;
		depth.Format = gfx::kDepthFormat;
		depth.SampleDesc.Count = 1;
		depth.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		D3D12_CLEAR_VALUE clear{};
		clear.Format = gfx::kDepthFormat;
		clear.DepthStencil.Depth = 1.0f;
		const D3D12_HEAP_PROPERTIES heap = gfx::HeapProps(D3D12_HEAP_TYPE_DEFAULT);
		DN_HR(d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depth,
										 D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
										 IID_PPV_ARGS(&m_iconDepth)));
		D3D12_DESCRIPTOR_HEAP_DESC dsv{};
		dsv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		dsv.NumDescriptors = 1;
		DN_HR(d->CreateDescriptorHeap(&dsv, IID_PPV_ARGS(&m_iconDsvHeap)));
		d->CreateDepthStencilView(m_iconDepth.Get(), nullptr,
								  m_iconDsvHeap->GetCPUDescriptorHandleForHeapStart());
	}

	bool any = false;
	for (auto&& [id, kind] : m_itemKinds) {
		if (!kind->model || !kind->iconTarget) continue;
		BakeIcon(list, sprites, *kind->model, *kind->iconTarget);
		any = true;
	}
	// The bakes redirected the output merger; hand the back buffer back for the
	// scene + 2D passes (mirrors RenderShadowMaps).
	if (any) m_device.BindBackBuffer(list);
}

void DungeonWorld::BakeIcon(ID3D12GraphicsCommandList* list, gfx::SpriteBatch& sprites,
							const MultiMaterialModel& model, const gfx::Texture& target) {
	D3D12_RESOURCE_BARRIER toRT = gfx::Transition(
		target.Resource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_RENDER_TARGET);
	list->ResourceBarrier(1, &toRT);

	const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // transparent corners
	gfx::BeginOffscreen(list, target.Rtv(),
						m_iconDsvHeap->GetCPUDescriptorHandleForHeapStart(), kIconSize,
						clear);

	// Composite a soft round halo first (the 3D item draws over it). Tint sets the
	// halo colour + translucency — tweak kHaloColor to taste.
	// NOTE on alpha: the halo is straight-alpha blended over a TRANSPARENT clear,
	// so the icon stores premultiplied-ish RGB with straight alpha; the UI then
	// blends the icon straight-alpha again, so the halo reads at ~alpha^2 (dimmer
	// than the nominal tint). kHaloColor is therefore the AS-COMPOSITED value that
	// was tuned visually, not a literal target — a true fix would need a
	// premultiplied-alpha path through SpriteBatch (a broad UI change). The opaque
	// model itself is unaffected (alpha 1).
	constexpr Vec4 kHaloColor{0.90f, 0.92f, 0.97f, 0.55f}; // soft light glow
	sprites.Begin(list, kIconSize, kIconSize);
	sprites.DrawSprite({0.0f, 0.0f, static_cast<float>(kIconSize),
						static_cast<float>(kIconSize)},
					   {0, 0, 1, 1}, *m_iconHalo, kHaloColor);
	sprites.End();

	// Centre the model at the origin and scale its LONGEST extent to fill the icon
	// frame's DIAGONAL — a long thin weapon laid corner-to-corner (the classic RPG
	// look) reads big in a square, where filling only the height leaves a thin
	// sliver. Roll about the view axis (world Z, since the camera has no roll) for
	// the diagonal, plus a gentle yaw/tilt for a 3/4 view. Item models vary in real
	// size (a ~0.5 m dagger), so fit by bounds, not a fixed scale.
	const Vec3 lo = model.boundsMin, hi = model.boundsMax;
	const Vec3 c{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
	const Vec3 ext{hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
	const float longest = std::max({ext.x, ext.y, ext.z, 1e-3f});
	// Camera at 2.2 / 35° FOV frames ~1.39 units tall (diagonal ~1.97); fill ~95%.
	const float s = 1.9f / longest;
	// Turn the model's FLATTEST face toward the camera — align its thinnest axis
	// with the view (+Z) so we see the broad face, not the thin edge, whatever
	// orientation the item shipped in (a blade modelled flat-on-Z, lying on Y, or
	// standing on X all resolve here). Then a gentle 3/4 tilt + a diagonal roll so
	// a long item fills the square corner-to-corner.
	XMMATRIX align = XMMatrixIdentity();
	if (ext.x <= ext.y && ext.x <= ext.z) align = XMMatrixRotationY(kPi * 0.5f);
	else if (ext.y <= ext.x && ext.y <= ext.z) align = XMMatrixRotationX(kPi * 0.5f);
	// A long thin item (a blade) reads best laid DIAGONALLY corner-to-corner; a
	// bulkier/upright item (armour, a shield) reads best UPRIGHT with its front to
	// the camera — no roll. Decide from elongation: longest extent vs the next.
	float sorted[3] = {ext.x, ext.y, ext.z};
	std::sort(sorted, sorted + 3);
	const bool elongated = sorted[2] > sorted[1] * 2.0f;
	const XMMATRIX present =
		elongated ? XMMatrixRotationY(0.3f) * XMMatrixRotationX(-0.15f) *
						XMMatrixRotationZ(0.7f)
				  : XMMatrixRotationY(0.35f) * XMMatrixRotationX(-0.12f);
	const XMMATRIX worldX = XMMatrixTranslation(-c.x, -c.y, -c.z) *
							XMMatrixScaling(s, s, s) * align * present;
	Mat4 world;
	XMStoreFloat4x4(&world, worldX);

	gfx::Camera cam;
	cam.SetLens(35.0f * kPi / 180.0f, 1.0f, 0.02f, 10.0f);
	cam.SetPosition({0.0f, 0.0f, -2.2f});
	cam.SetYawPitch(0.0f, 0.0f);

	// Punchy studio lighting so a thin dark weapon reads clearly on a dark slot
	// (background stays transparent — the item just floats, lit bright): high
	// ambient, a strong key (upper front-right) + a cool fill (lower front-left),
	// and TWO strong RIM lights behind the item that halo its silhouette toward the
	// camera — the rim is what keeps a thin blade legible on any background.
	gfx::LightSet lights;
	lights.ambient = {0.62f, 0.62f, 0.68f};
	lights.points.push_back(
		{{1.8f, 2.0f, -1.8f}, 12.0f, {1.0f, 0.97f, 0.92f}, 6.5f, -1, false});
	lights.points.push_back(
		{{-1.8f, 0.6f, -1.6f}, 12.0f, {0.82f, 0.88f, 1.0f}, 3.4f, -1, false});
	lights.points.push_back(
		{{1.3f, 1.5f, 2.4f}, 12.0f, {1.0f, 1.0f, 1.0f}, 7.5f, -1, false});
	lights.points.push_back(
		{{-1.3f, 1.5f, 2.4f}, 12.0f, {1.0f, 1.0f, 1.0f}, 7.5f, -1, false});

	m_renderer.BeginScene(list, cam, lights);
	DrawMultiMaterial(list, model, world);

	D3D12_RESOURCE_BARRIER toSRV = gfx::Transition(
		target.Resource(), D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	list->ResourceBarrier(1, &toSRV);
}

} // namespace dungeon::game
