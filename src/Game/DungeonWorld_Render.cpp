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
			for (const auto& sub : deco.kind->multi->subs)
				m_renderer.DrawMesh(list, *sub.mesh, deco.world, sub.material);
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
			// Non-rune placeholders render scaled UP (kItemPlaceholderScale) — bigger
			// than the rune tablet so they read on a dark floor and are an easy click
			// target; TryPickItem widens the pick radius to match (see kItemPickTopY).
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

} // namespace dungeon::game
