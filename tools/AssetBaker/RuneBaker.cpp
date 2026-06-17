// ============================================================================
// RuneBaker.cpp — rune-item assets (the carved-stone tablets the party finds).
//
// One shared tablet MODEL plus four per-element TEXTURE sets and four ICONS:
//   * rune_tablet.gltf — a small standing stone slab. Only the broad front and
//     back faces carry the carved-rune UVs (full 0..1); the thin edges pin to a
//     stone-only corner of the texture, so the glyph shows once on each face.
//   * rune_<elem>_2k.png (+ _n + _mr) — a stone HEIGHT FIELD with an Elder
//     Futhark glyph carved in as a recessed groove (lower height). Albedo, the
//     normal+height map (parallax in scene.hlsl), and the ORM map all derive
//     from that one field, so the carve lights coherently. Baked at _2k so the
//     res→2k prop fallback (DungeonWorld::LoadPbrSet) finds it at every tier.
//   * rune_icon_<elem>.png — a small element-tinted tile with the glyph, for
//     the held cursor + inventory slot (PNG only, like the hit splats).
//
// Glyph → element: Fire=Kenaz, Water=Laguz, Air=Ansuz, Earth=Berkano. Each is a
// short list of straight strokes (Elder Futhark is all straight lines), shared
// by the tablet textures and the icons. Purely deterministic.
// ============================================================================
#include "RuneBaker.h"

#include "Assets/Model.h"
#include "Core/Log.h"
#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "GltfWriter.h"
#include "Noise.h"

#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace dungeon::baker {

namespace {

// --- rune glyphs -------------------------------------------------------------
// Strokes in a unit cell, y UP (0 bottom, 1 top). One element per row.

struct Stroke {
	float x0, y0, x1, y1;
};

struct RuneSpec {
	const char* elem;     // "fire" / "water" / "air" / "earth" (rune_<elem>...)
	Vec3 stone;           // base stone tint
	Vec3 accent;          // element colour shown in the groove / icon glyph
	std::vector<Stroke> strokes;
};

// Coordinates picked to read as the named Futhark rune at a glance.
const std::array<RuneSpec, 4> kRunes = {{
	// Fire — Kenaz "<" (beacon/torch).
	{"fire", {0.55f, 0.42f, 0.36f}, {0.95f, 0.45f, 0.18f},
	 {{0.58f, 0.84f, 0.40f, 0.50f}, {0.40f, 0.50f, 0.58f, 0.16f}}},
	// Water — Laguz: stave with a short branch off the top, down-right.
	{"water", {0.40f, 0.46f, 0.55f}, {0.30f, 0.55f, 0.95f},
	 {{0.42f, 0.86f, 0.42f, 0.14f}, {0.42f, 0.86f, 0.66f, 0.66f}}},
	// Air — Ansuz: stave with two parallel branches, down-right.
	{"air", {0.52f, 0.55f, 0.58f}, {0.80f, 0.92f, 1.00f},
	 {{0.42f, 0.86f, 0.42f, 0.14f},
	  {0.42f, 0.84f, 0.66f, 0.64f},
	  {0.42f, 0.62f, 0.66f, 0.42f}}},
	// Earth — Berkano: stave with two right-side triangular bows (a "B").
	{"earth", {0.44f, 0.50f, 0.40f}, {0.45f, 0.80f, 0.32f},
	 {{0.40f, 0.86f, 0.40f, 0.14f},
	  {0.40f, 0.86f, 0.64f, 0.70f}, {0.64f, 0.70f, 0.40f, 0.54f},
	  {0.40f, 0.54f, 0.64f, 0.38f}, {0.64f, 0.38f, 0.40f, 0.14f}}},
}};

// Distance from point (px,py) to segment (a→b), all in uv space.
float SegDist(float px, float py, const Stroke& s) {
	const float vx = s.x1 - s.x0, vy = s.y1 - s.y0;
	const float wx = px - s.x0, wy = py - s.y0;
	const float c1 = vx * wx + vy * wy;
	if (c1 <= 0.0f) return std::sqrt(wx * wx + wy * wy);
	const float c2 = vx * vx + vy * vy;
	if (c2 <= c1) return std::hypot(px - s.x1, py - s.y1);
	const float t = c1 / c2;
	return std::hypot(px - (s.x0 + t * vx), py - (s.y0 + t * vy));
}

// Carve coverage 0..1 at glyph-space (gx,gy): 1 deep in a stroke, 0 outside.
float GlyphCover(float gx, float gy, const RuneSpec& r) {
	constexpr float kHalfWidth = 0.040f; // stroke half-width (uv)
	constexpr float kAA = 0.013f;
	float best = 1e9f;
	for (const Stroke& s : r.strokes) best = std::min(best, SegDist(gx, gy, s));
	return std::clamp((kHalfWidth - best) / kAA, 0.0f, 1.0f);
}

// --- image plumbing ----------------------------------------------------------

bool SaveRgba(const std::string& path, u32 size, const std::vector<u8>& rgba) {
	const int ok = stbi_write_png(path.c_str(), size, size, 4, rgba.data(), size * 4);
	if (ok) log::Info("Wrote {}", path);
	else log::Error("Failed to write {}", path);
	return ok != 0;
}

u8 ToU8(float v) { return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

// --- texture set (albedo + normal/height + ORM) ------------------------------

constexpr u32 kTexSize = 512;

bool BakeRuneTextureSet(const std::string& texturesDir, const RuneSpec& r, u32 seed) {
	const u32 n = kTexSize;
	std::vector<float> height(static_cast<size_t>(n) * n);
	std::vector<float> carve(static_cast<size_t>(n) * n); // glyph coverage, reused
	auto idx = [n](u32 x, u32 y) { return static_cast<size_t>(y) * n + x; };

	// Height field: gently varied stone, with the glyph cut in as a groove.
	for (u32 y = 0; y < n; ++y) {
		for (u32 x = 0; x < n; ++x) {
			const float u = (x + 0.5f) / n;
			const float gy = 1.0f - (y + 0.5f) / n; // glyph space is y-up
			const float stone = 0.62f + (Fbm(x * 0.04f, y * 0.04f, seed) - 0.5f) * 0.22f;
			const float c = GlyphCover(u, gy, r);
			carve[idx(x, y)] = c;
			height[idx(x, y)] = std::clamp(stone * (1.0f - c) + 0.10f * c, 0.0f, 1.0f);
		}
	}

	// Albedo: stone shaded by height; the groove darkens and takes a faint
	// elemental wash so the carve reads as "this element".
	std::vector<u8> alb(static_cast<size_t>(n) * n * 4);
	for (u32 y = 0; y < n; ++y) {
		for (u32 x = 0; x < n; ++x) {
			const float h = height[idx(x, y)];
			const float c = carve[idx(x, y)];
			const float grain = (Hash(x, y, seed + 5u) - 0.5f) * 0.10f;
			const float shade = 0.55f + 0.45f * h + grain;
			Vec3 col = {r.stone.x * shade, r.stone.y * shade, r.stone.z * shade};
			// Blend the accent into the recess (darkened — it sits in shadow).
			const float wash = c * 0.7f;
			col.x = col.x * (1 - wash) + r.accent.x * 0.35f * wash;
			col.y = col.y * (1 - wash) + r.accent.y * 0.35f * wash;
			col.z = col.z * (1 - wash) + r.accent.z * 0.35f * wash;
			const size_t i = idx(x, y) * 4;
			alb[i + 0] = ToU8(col.x);
			alb[i + 1] = ToU8(col.y);
			alb[i + 2] = ToU8(col.z);
			alb[i + 3] = 255;
		}
	}

	// Normal + height (alpha). Clamped edge sampling — the tablet face does not
	// tile, so wrapping would seam the border.
	auto at = [&](int x, int y) {
		return height[idx(static_cast<u32>(std::clamp(x, 0, int(n) - 1)),
						  static_cast<u32>(std::clamp(y, 0, int(n) - 1)))];
	};
	std::vector<u8> nrm(static_cast<size_t>(n) * n * 4);
	constexpr float kStrength = 4.5f;
	for (u32 y = 0; y < n; ++y) {
		for (u32 x = 0; x < n; ++x) {
			const float dx = (at(x + 1, y) - at(x - 1, y)) * kStrength;
			const float dy = (at(x, y + 1) - at(x, y - 1)) * kStrength;
			const float inv = 1.0f / std::sqrt(dx * dx + dy * dy + 1.0f);
			const size_t i = idx(x, y) * 4;
			nrm[i + 0] = ToU8(-dx * inv * 0.5f + 0.5f);
			nrm[i + 1] = ToU8(-dy * inv * 0.5f + 0.5f);
			nrm[i + 2] = ToU8(inv * 0.5f + 0.5f);
			nrm[i + 3] = ToU8(height[idx(x, y)]);
		}
	}

	// ORM: occlusion (R) dips in the groove, roughness (G) high matte stone,
	// metallic (B) zero.
	std::vector<u8> mr(static_cast<size_t>(n) * n * 4);
	for (u32 y = 0; y < n; ++y) {
		for (u32 x = 0; x < n; ++x) {
			const float c = carve[idx(x, y)];
			const size_t i = idx(x, y) * 4;
			mr[i + 0] = ToU8(1.0f - c * 0.45f); // occlusion
			mr[i + 1] = ToU8(0.82f + c * 0.10f); // roughness
			mr[i + 2] = 0;                       // metallic
			mr[i + 3] = 255;
		}
	}

	const std::string base = texturesDir + "\\rune_" + r.elem + "_2k";
	bool ok = SaveRgba(base + ".png", n, alb);
	ok &= SaveRgba(base + "_n.png", n, nrm);
	ok &= SaveRgba(base + "_mr.png", n, mr);
	return ok;
}

// --- icons (held cursor / inventory slot) ------------------------------------

constexpr u32 kIconSize = 128;

// Rounded-square coverage (1 inside, ramps to 0 at the rounded border).
float TileMask(float u, float v) {
	const float dx = std::fabs(u - 0.5f) - 0.36f;
	const float dy = std::fabs(v - 0.5f) - 0.36f;
	const float outside = std::hypot(std::max(dx, 0.0f), std::max(dy, 0.0f)) +
						   std::min(std::max(dx, dy), 0.0f) - 0.08f; // rounded corners
	return std::clamp(-outside / (1.5f / kIconSize), 0.0f, 1.0f);
}

bool BakeRuneIcon(const std::string& texturesDir, const RuneSpec& r) {
	const u32 n = kIconSize;
	std::vector<u8> rgba(static_cast<size_t>(n) * n * 4);
	for (u32 y = 0; y < n; ++y) {
		for (u32 x = 0; x < n; ++x) {
			const float u = (x + 0.5f) / n;
			const float v = (y + 0.5f) / n;
			const float gy = 1.0f - v;
			const float mask = TileMask(u, v);
			const float c = GlyphCover(u, gy, r);
			// Element-tinted stone tile; the glyph glows in the accent colour.
			Vec3 col = {r.stone.x * 0.85f, r.stone.y * 0.85f, r.stone.z * 0.85f};
			col.x = col.x * (1 - c) + r.accent.x * c;
			col.y = col.y * (1 - c) + r.accent.y * c;
			col.z = col.z * (1 - c) + r.accent.z * c;
			const size_t i = (static_cast<size_t>(y) * n + x) * 4;
			rgba[i + 0] = ToU8(col.x);
			rgba[i + 1] = ToU8(col.y);
			rgba[i + 2] = ToU8(col.z);
			rgba[i + 3] = ToU8(mask);
		}
	}
	return SaveRgba(texturesDir + "\\rune_icon_" + r.elem + ".png", n, rgba);
}

// --- tablet model ------------------------------------------------------------

// One quad (two tris), CCW from the four corners, with explicit per-corner UVs.
void AddFace(assets::MeshData& m, const Vec3 c[4], const Vec3& nrm, const Vec2 uv[4]) {
	const u32 base = static_cast<u32>(m.vertices.size());
	for (int i = 0; i < 4; ++i) {
		assets::Vertex v;
		v.position = c[i];
		v.normal = nrm;
		v.uv = uv[i];
		m.vertices.push_back(v);
	}
	m.indices.insert(m.indices.end(),
					 {base, base + 1, base + 2, base, base + 2, base + 3});
}

assets::ModelData BuildRuneTablet() {
	// A small standing slab resting on the floor (y 0..0.46).
	constexpr float hx = 0.16f, y0 = 0.0f, y1 = 0.46f, hz = 0.04f;
	// Broad faces map the whole texture (rune centred); thin edges pin to a
	// stone-only corner so no partial glyph bleeds onto them.
	const Vec2 full[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
	const Vec2 stone[4] = {{0.5f, 0.97f}, {0.5f, 0.97f}, {0.5f, 0.97f}, {0.5f, 0.97f}};

	assets::MeshData mesh;
	{ // front (+Z): rune upright
		const Vec3 c[4] = {{-hx, y0, hz}, {hx, y0, hz}, {hx, y1, hz}, {-hx, y1, hz}};
		AddFace(mesh, c, {0, 0, 1}, full);
	}
	{ // back (-Z): rune again (mirrored in x — acceptable for a stone slab)
		const Vec3 c[4] = {{hx, y0, -hz}, {-hx, y0, -hz}, {-hx, y1, -hz}, {hx, y1, -hz}};
		AddFace(mesh, c, {0, 0, -1}, full);
	}
	{ // left (-X)
		const Vec3 c[4] = {{-hx, y0, -hz}, {-hx, y0, hz}, {-hx, y1, hz}, {-hx, y1, -hz}};
		AddFace(mesh, c, {-1, 0, 0}, stone);
	}
	{ // right (+X)
		const Vec3 c[4] = {{hx, y0, hz}, {hx, y0, -hz}, {hx, y1, -hz}, {hx, y1, hz}};
		AddFace(mesh, c, {1, 0, 0}, stone);
	}
	{ // top (+Y)
		const Vec3 c[4] = {{-hx, y1, hz}, {hx, y1, hz}, {hx, y1, -hz}, {-hx, y1, -hz}};
		AddFace(mesh, c, {0, 1, 0}, stone);
	}
	{ // bottom (-Y)
		const Vec3 c[4] = {{-hx, y0, -hz}, {hx, y0, -hz}, {hx, y0, hz}, {-hx, y0, hz}};
		AddFace(mesh, c, {0, -1, 0}, stone);
	}

	assets::ModelData model;
	mesh.material = 0;
	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({{0.50f, 0.50f, 0.52f, 1.0f}, -1}); // stone fallback
	return model;
}

} // namespace

bool BakeRunes(const std::string& assetsDir) {
	const std::string models = assetsDir + "\\models";
	const std::string textures = assetsDir + "\\textures";
	bool ok = WriteGltf(BuildRuneTablet(), models + "\\rune_tablet.gltf");
	u32 seed = 2200u;
	for (const RuneSpec& r : kRunes) {
		ok &= BakeRuneTextureSet(textures, r, seed);
		ok &= BakeRuneIcon(textures, r);
		seed += 31u;
	}
	return ok;
}

} // namespace dungeon::baker
