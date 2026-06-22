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

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
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

// --- scanned worn-stone base -------------------------------------------------
// The tablets are carved from a real worn-limestone scan (installed as the
// `runestone` set — albedo + normal-with-height-in-alpha). The baker reads that
// stone and cuts each element's glyph groove into it, so the runes look like
// ancient pitted stone rather than clean procedural slabs.

struct StoneMaps {
	int w = 0, h = 0;
	std::vector<float> albedo; // w*h*3, sRGB 0..1 (kept encoded — written straight)
	std::vector<float> height; // w*h, 0..1
	bool Valid() const { return w > 0 && h > 0 && !albedo.empty(); }

	// Bilinear sample with wrap (the stone scan tiles; the tablet face crops it).
	void SampleAt(float u, float v, Vec3& alb, float& hgt) const {
		u -= std::floor(u);
		v -= std::floor(v);
		const float fx = u * w - 0.5f, fy = v * h - 0.5f;
		const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
		const float tx = fx - x0, ty = fy - y0;
		auto wrap = [](int a, int n) { return ((a % n) + n) % n; };
		const int xs[2] = {wrap(x0, w), wrap(x0 + 1, w)};
		const int ys[2] = {wrap(y0, h), wrap(y0 + 1, h)};
		Vec3 a{0, 0, 0};
		float hh = 0.0f;
		const float wts[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
		const int cx[4] = {xs[0], xs[1], xs[0], xs[1]};
		const int cy[4] = {ys[0], ys[0], ys[1], ys[1]};
		for (int k = 0; k < 4; ++k) {
			const size_t i = static_cast<size_t>(cy[k]) * w + cx[k];
			a.x += albedo[i * 3 + 0] * wts[k];
			a.y += albedo[i * 3 + 1] * wts[k];
			a.z += albedo[i * 3 + 2] * wts[k];
			hh += height[i] * wts[k];
		}
		alb = a;
		hgt = hh;
	}
};

StoneMaps LoadStoneMaps(const std::string& texturesDir) {
	StoneMaps s;
	int w = 0, h = 0, n = 0;
	const std::string albPath = texturesDir + "\\runestone.png";
	u8* alb = stbi_load(albPath.c_str(), &w, &h, &n, 4);
	if (!alb) {
		log::Warn("RuneBaker: no {} — falling back to procedural stone", albPath);
		return s;
	}
	s.w = w;
	s.h = h;
	s.albedo.resize(static_cast<size_t>(w) * h * 3);
	for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
		for (int k = 0; k < 3; ++k) s.albedo[i * 3 + k] = alb[i * 4 + k] / 255.0f;
	stbi_image_free(alb);

	// Height rides in the normal map's alpha (the importer packs it there).
	int wn = 0, hn = 0, nn = 0;
	u8* nrm = stbi_load((texturesDir + "\\runestone_n.png").c_str(), &wn, &hn, &nn, 4);
	s.height.assign(static_cast<size_t>(w) * h, 0.6f);
	if (nrm && wn == w && hn == h)
		for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
			s.height[i] = nrm[i * 4 + 3] / 255.0f;
	if (nrm) stbi_image_free(nrm);
	log::Info("RuneBaker: carved from worn-stone scan {} ({}x{})", albPath, w, h);
	return s;
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

bool BakeRuneTextureSet(const std::string& texturesDir, const RuneSpec& r, u32 seed,
						const StoneMaps& stone, float uo, float vo) {
	const u32 n = kTexSize;
	std::vector<float> height(static_cast<size_t>(n) * n);
	std::vector<float> carve(static_cast<size_t>(n) * n);   // glyph coverage, reused
	std::vector<Vec3> stoneAlb(static_cast<size_t>(n) * n); // worn-stone albedo
	auto idx = [n](u32 x, u32 y) { return static_cast<size_t>(y) * n + x; };

	// Height field + base colour, sampled from the worn-stone scan (a per-element
	// crop offset so the four tablets aren't identical), with the glyph cut in as
	// a recessed groove. Falls back to gentle procedural stone if the scan is
	// missing.
	for (u32 y = 0; y < n; ++y) {
		for (u32 x = 0; x < n; ++x) {
			const float u = (x + 0.5f) / n;
			const float v = (y + 0.5f) / n;
			const float gy = 1.0f - v; // glyph space is y-up
			float sh;
			Vec3 col;
			if (stone.Valid()) {
				stone.SampleAt(u + uo, v + vo, col, sh);
			} else {
				sh = 0.62f + (Fbm(x * 0.04f, y * 0.04f, seed) - 0.5f) * 0.22f;
				col = {r.stone.x * (0.7f + 0.3f * sh), r.stone.y * (0.7f + 0.3f * sh),
					   r.stone.z * (0.7f + 0.3f * sh)};
			}
			const float c = GlyphCover(u, gy, r);
			carve[idx(x, y)] = c;
			stoneAlb[idx(x, y)] = col;
			height[idx(x, y)] = std::clamp(sh - c * 0.22f, 0.0f, 1.0f); // groove
		}
	}

	// Albedo: the worn stone, the groove darkened (it sits in shadow) and given a
	// faint elemental wash so the carve reads as "this element".
	std::vector<u8> alb(static_cast<size_t>(n) * n * 4);
	for (u32 y = 0; y < n; ++y) {
		for (u32 x = 0; x < n; ++x) {
			const float c = carve[idx(x, y)];
			Vec3 col = stoneAlb[idx(x, y)];
			const float shade = 1.0f - 0.4f * c; // groove in shadow
			col = {col.x * shade, col.y * shade, col.z * shade};
			const float wash = c * 0.5f;
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

// (The rune cursor/inventory icon is no longer baked here — it's a committed
// source image at assets/ui/rune_icon_<elem>.png, like the other UI icons.)

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

float Smoothstep(float a, float b, float t) {
	t = std::clamp((t - a) / (b - a), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

assets::ModelData BuildRuneTablet() {
	// A WORN, hand-sized stone tablet (not a clean 6-face box). We start from a
	// thin slab and erode it: every face is subdivided into a grid, and each
	// vertex is pushed to a "worn" position that depends ONLY on its original
	// location — so the faces stay welded (no cracks). Corners/edges round toward
	// an ellipsoid and the surface is roughened with noise, heavier along the box
	// edges (chipped) than across the broad faces (so the carved glyph still
	// reads). Flat-shaded per quad for a chiselled, rocky look. ~17 cm wide.
	constexpr float hx = 0.085f, y0 = 0.0f, y1 = 0.15f, hz = 0.016f;
	const Vec3 ctr = {0.0f, (y0 + y1) * 0.5f, 0.0f};
	const Vec3 ext = {hx, (y1 - y0) * 0.5f, hz};
	constexpr u32 seed = 4242u;

	// Pristine box point -> worn position. Deterministic in p, so a point shared
	// by two faces (a box edge) maps identically and the seam stays closed.
	auto worn = [&](Vec3 p) -> Vec3 {
		const Vec3 d = {(p.x - ctr.x) / ext.x, (p.y - ctr.y) / ext.y,
						(p.z - ctr.z) / ext.z}; // box coords in [-1,1]
		const float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
		// "Edge-ness": the MEDIAN abs component — ~0 mid-face, ~1 along a box edge
		// (max alone is 1 on every face, so it can't tell faces from edges).
		const float edge = ax + ay + az - std::max({ax, ay, az}) - std::min({ax, ay, az});
		// Round toward the ellipsoid (pulls the slab's edges in).
		const float l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) + 1e-4f;
		const Vec3 sph = {ctr.x + ext.x * d.x / l, ctr.y + ext.y * d.y / l,
						  ctr.z + ext.z * d.z / l};
		constexpr float k = 0.30f;
		Vec3 rp = {p.x + (sph.x - p.x) * k, p.y + (sph.y - p.y) * k,
				   p.z + (sph.z - p.z) * k};
		// Outward direction for the erosion push.
		Vec3 rad = {rp.x - ctr.x, rp.y - ctr.y, rp.z - ctr.z};
		const float rl = std::sqrt(rad.x * rad.x + rad.y * rad.y + rad.z * rad.z) + 1e-4f;
		rad = {rad.x / rl, rad.y / rl, rad.z / rl};
		// Two-scale noise: broad dents + fine grit (centred to [-0.5,0.5]).
		const float big = Fbm(p.x * 13.0f + p.z * 4.0f, p.y * 13.0f + p.x * 2.0f, seed) - 0.5f;
		const float grit = Fbm(p.x * 52.0f + p.y * 9.0f, p.z * 52.0f + p.y * 5.0f, seed + 11u) - 0.5f;
		const float amp = 0.003f + 0.013f * Smoothstep(0.45f, 1.0f, edge); // calm face, chipped edge
		const float disp = big * amp + grit * amp * 0.4f;
		return {rp.x + rad.x * disp, rp.y + rad.y * disp, rp.z + rad.z * disp};
	};

	// Subdivide a face (origin O, span vectors U across / V up) into nu×nv quads,
	// displace each vertex, and flat-shade per quad. UV bilerps the four corners.
	assets::MeshData mesh;
	auto addFace = [&](Vec3 O, Vec3 U, Vec3 V, int nu, int nv, Vec2 uv00, Vec2 uv10,
					   Vec2 uv11, Vec2 uv01) {
		auto P = [&](int i, int j) {
			const float s = float(i) / nu, t = float(j) / nv;
			return worn({O.x + U.x * s + V.x * t, O.y + U.y * s + V.y * t,
						 O.z + U.z * s + V.z * t});
		};
		auto UV = [&](int i, int j) {
			const float s = float(i) / nu, t = float(j) / nv;
			const Vec2 a = {uv00.x + (uv10.x - uv00.x) * s, uv00.y + (uv10.y - uv00.y) * s};
			const Vec2 b = {uv01.x + (uv11.x - uv01.x) * s, uv01.y + (uv11.y - uv01.y) * s};
			return Vec2{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
		};
		for (int j = 0; j < nv; ++j)
			for (int i = 0; i < nu; ++i) {
				const Vec3 c[4] = {P(i, j), P(i + 1, j), P(i + 1, j + 1), P(i, j + 1)};
				const Vec2 uv[4] = {UV(i, j), UV(i + 1, j), UV(i + 1, j + 1), UV(i, j + 1)};
				const Vec3 e1 = {c[1].x - c[0].x, c[1].y - c[0].y, c[1].z - c[0].z};
				const Vec3 e2 = {c[2].x - c[0].x, c[2].y - c[0].y, c[2].z - c[0].z};
				Vec3 nrm = {e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z,
							e1.x * e2.y - e1.y * e2.x};
				const float nl = std::sqrt(nrm.x * nrm.x + nrm.y * nrm.y + nrm.z * nrm.z) + 1e-6f;
				nrm = {nrm.x / nl, nrm.y / nl, nrm.z / nl};
				AddFace(mesh, c, nrm, uv);
			}
	};

	const Vec2 s{0.5f, 0.97f}; // thin edges pin to a stone-only texel
	constexpr int N = 7;       // broad-face subdivisions
	constexpr int M = 2;       // thin-edge subdivisions
	const float h = y1 - y0;
	// Broad faces carry the glyph (full 0..1, rune upright); edges stay stone.
	addFace({-hx, y0, hz}, {2 * hx, 0, 0}, {0, h, 0}, N, N, {0, 1}, {1, 1}, {1, 0}, {0, 0});  // front +Z
	addFace({hx, y0, -hz}, {-2 * hx, 0, 0}, {0, h, 0}, N, N, {0, 1}, {1, 1}, {1, 0}, {0, 0}); // back -Z
	addFace({-hx, y0, -hz}, {0, 0, 2 * hz}, {0, h, 0}, M, N, s, s, s, s);                     // left -X
	addFace({hx, y0, hz}, {0, 0, -2 * hz}, {0, h, 0}, M, N, s, s, s, s);                      // right +X
	addFace({-hx, y1, hz}, {2 * hx, 0, 0}, {0, 0, -2 * hz}, N, M, s, s, s, s);                // top +Y
	addFace({-hx, y0, -hz}, {2 * hx, 0, 0}, {0, 0, 2 * hz}, N, M, s, s, s, s);                // bottom -Y

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
	const StoneMaps stone = LoadStoneMaps(textures);
	u32 seed = 2200u;
	int e = 0;
	// Per-element crop of the stone scan so the four tablets read as distinct
	// pieces of the same ancient rock.
	const float offsets[4][2] = {{0.0f, 0.0f}, {0.5f, 0.13f}, {0.21f, 0.57f}, {0.63f, 0.38f}};
	for (const RuneSpec& r : kRunes) {
		ok &= BakeRuneTextureSet(textures, r, seed, stone, offsets[e][0], offsets[e][1]);
		seed += 31u;
		++e;
	}
	return ok;
}

} // namespace dungeon::baker
