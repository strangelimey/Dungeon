// ============================================================================
// PortraitBaker.cpp — the default party's portraits.
//
// Simple painted busts built from antialiased 2D masks (ellipses and boxes):
// a tinted background, shoulders, head, face details, and one class-defining
// headpiece — Brand's steel helmet, Sera's deep hood, Maren's circlet, and
// Tilo's pointed hat. Shading is a warm key light from the upper-left plus a
// teal rim from the right (the portal's color, tying them to the title art),
// finished with vignette and grain. Purely deterministic.
//
// Output: portrait_<name>.png (256x256), names matching the roster in
// src/Game/Character.cpp.
// ============================================================================
#include "PortraitBaker.h"

#include "Core/Log.h"
#include "Core/Types.h"
#include "Noise.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace dungeon::baker {

namespace {

constexpr u32 kSize = 256;
constexpr float kAA = 1.5f / kSize; // antialias width in uv units

struct Vec3f {
	float x, y, z;
};

Vec3f Mul(const Vec3f& a, float s) { return {a.x * s, a.y * s, a.z * s}; }

void Blend(Vec3f& dst, const Vec3f& src, float a) {
	dst.x += (src.x - dst.x) * a;
	dst.y += (src.y - dst.y) * a;
	dst.z += (src.z - dst.z) * a;
}

// Antialiased ellipse coverage; nx/ny get the interior gradient (0 at the
// center, ±1 at the rim) used as a fake surface normal for shading.
float EllipseMask(float u, float v, float cx, float cy, float rx, float ry,
				  float* nx = nullptr, float* ny = nullptr) {
	const float dx = (u - cx) / rx, dy = (v - cy) / ry;
	const float d = std::sqrt(dx * dx + dy * dy);
	if (nx) {
		*nx = std::clamp(dx, -1.0f, 1.0f);
		*ny = std::clamp(dy, -1.0f, 1.0f);
	}
	const float aa = kAA / std::min(rx, ry);
	return std::clamp((1.0f - d) / aa, 0.0f, 1.0f);
}

float BoxMask(float u, float v, float cx, float cy, float hx, float hy) {
	const float d =
		std::max(std::fabs(u - cx) - hx, std::fabs(v - cy) - hy);
	return std::clamp(-d / kAA, 0.0f, 1.0f);
}

// v above the line (mask fades out below it) — clips headgear to the brow.
float AboveMask(float v, float line) {
	return std::clamp((line - v) / kAA, 0.0f, 1.0f);
}

const Vec3f kRimColor{0.25f, 0.95f, 0.70f}; // the title portal's teal

// Warm key from the upper-left, teal rim from the right.
Vec3f Shade(const Vec3f& base, float nx, float ny) {
	const float key = std::clamp(0.80f - 0.34f * nx - 0.26f * ny, 0.32f, 1.15f);
	Vec3f c = Mul(base, key);
	const float rim = std::pow(std::clamp(nx, 0.0f, 1.0f), 3.0f) * 0.35f;
	c.x += kRimColor.x * rim;
	c.y += kRimColor.y * rim;
	c.z += kRimColor.z * rim;
	return c;
}

enum class Headgear { Helmet, Hood, Circlet, Hat };

struct PortraitSpec {
	const char* file; // portrait_<name>.png
	Headgear headgear;
	Vec3f bg;   // background tint (echoes the character's HUD color)
	Vec3f garb; // shoulders / torso
	Vec3f gear; // headpiece
	Vec3f skin;
	u32 seed;
};

constexpr PortraitSpec kSpecs[] = {
	{"portrait_brand", Headgear::Helmet, {0.30f, 0.16f, 0.11f},
	 {0.30f, 0.22f, 0.16f}, {0.55f, 0.56f, 0.60f}, {0.80f, 0.60f, 0.46f}, 101u},
	{"portrait_sera", Headgear::Hood, {0.12f, 0.21f, 0.12f},
	 {0.25f, 0.20f, 0.14f}, {0.21f, 0.31f, 0.18f}, {0.82f, 0.68f, 0.58f}, 211u},
	{"portrait_maren", Headgear::Circlet, {0.29f, 0.23f, 0.10f},
	 {0.72f, 0.68f, 0.60f}, {0.95f, 0.78f, 0.35f}, {0.78f, 0.58f, 0.45f}, 307u},
	{"portrait_tilo", Headgear::Hat, {0.14f, 0.14f, 0.30f},
	 {0.20f, 0.20f, 0.36f}, {0.27f, 0.27f, 0.48f}, {0.78f, 0.66f, 0.58f}, 401u},
};

Vec3f PaintPixel(float u, float v, const PortraitSpec& spec) {
	// --- background: tinted radial gradient + a soft halo behind the head ----
	const float r2 = (u - 0.5f) * (u - 0.5f) + (v - 0.45f) * (v - 0.45f);
	Vec3f color = Mul(spec.bg, 1.05f - 1.1f * r2 + 0.20f * std::exp(-r2 * 9.0f));
	const float bgGrain = (Fbm(u * 5.0f, v * 5.0f, spec.seed) - 0.5f) * 0.06f;
	color.x += bgGrain;
	color.y += bgGrain;
	color.z += bgGrain;

	// --- bust: shoulders, neck, head -----------------------------------------
	float snx, sny;
	const float shoulders = EllipseMask(u, v, 0.5f, 1.04f, 0.40f, 0.44f, &snx, &sny);
	const float cloth = 0.88f + 0.24f * (Fbm(u * 9.0f, v * 9.0f, spec.seed + 1) - 0.5f);
	Blend(color, Shade(Mul(spec.garb, cloth), snx, sny), shoulders);

	Blend(color, Mul(spec.skin, 0.62f), BoxMask(u, v, 0.5f, 0.60f, 0.055f, 0.10f));

	float hnx, hny;
	const float head = EllipseMask(u, v, 0.5f, 0.42f, 0.150f, 0.185f, &hnx, &hny);
	Blend(color, Shade(spec.skin, hnx, hny), head);

	// --- face: brow shadow, eyes, mouth (headgear may cover these) -----------
	const Vec3f dark{0.10f, 0.08f, 0.07f};
	Blend(color, Mul(spec.skin, 0.70f),
		  head * BoxMask(u, v, 0.5f, 0.40f, 0.11f, 0.012f) * 0.6f);
	const float eyes = std::max(EllipseMask(u, v, 0.448f, 0.438f, 0.017f, 0.013f),
								EllipseMask(u, v, 0.552f, 0.438f, 0.017f, 0.013f));
	Blend(color, dark, eyes * head);
	Blend(color, dark, BoxMask(u, v, 0.5f, 0.545f, 0.034f, 0.005f) * head * 0.45f);

	// --- headgear -------------------------------------------------------------
	switch (spec.headgear) {
	case Headgear::Helmet: {
		// Steel dome to the brow, nasal bar, cheek guards, specular streak.
		float gnx, gny;
		const float dome = EllipseMask(u, v, 0.5f, 0.42f, 0.165f, 0.205f, &gnx, &gny) *
						   AboveMask(v, 0.385f);
		const float nasal = BoxMask(u, v, 0.5f, 0.45f, 0.013f, 0.062f);
		const float cheeks = std::max(BoxMask(u, v, 0.372f, 0.46f, 0.022f, 0.055f),
									  BoxMask(u, v, 0.628f, 0.46f, 0.022f, 0.055f));
		const float metalMask = std::max({dome, nasal, cheeks});
		Vec3f metal = Shade(spec.gear, gnx, gny);
		const float gleam =
			std::pow(std::max(0.0f, -gnx * 0.6f - gny * 0.8f), 5.0f) * 0.5f;
		metal.x += gleam;
		metal.y += gleam;
		metal.z += gleam;
		Blend(color, metal, metalMask);
		break;
	}
	case Headgear::Hood: {
		// Deep hood draped to the shoulders; the face opening falls into
		// shadow with two pale glints for eyes.
		float gnx, gny;
		const float outer = EllipseMask(u, v, 0.5f, 0.45f, 0.21f, 0.27f, &gnx, &gny);
		const float opening = EllipseMask(u, v, 0.5f, 0.465f, 0.105f, 0.145f);
		const float weave =
			0.88f + 0.24f * (Fbm(u * 12.0f, v * 12.0f, spec.seed + 2) - 0.5f);
		Blend(color, Shade(Mul(spec.gear, weave), gnx, gny), outer * (1.0f - opening));
		Blend(color, Mul(color, 0.40f), opening * 0.85f); // shadowed face
		const float glints =
			std::max(EllipseMask(u, v, 0.455f, 0.46f, 0.011f, 0.009f),
					 EllipseMask(u, v, 0.545f, 0.46f, 0.011f, 0.009f));
		Blend(color, {0.78f, 0.86f, 0.80f}, glints * 0.9f);
		break;
	}
	case Headgear::Circlet: {
		// Hair framing the face, side locks, and a thin gold band with a gem.
		float gnx, gny;
		const float outer = EllipseMask(u, v, 0.5f, 0.43f, 0.168f, 0.21f, &gnx, &gny);
		const float face = EllipseMask(u, v, 0.5f, 0.455f, 0.122f, 0.158f);
		const float locks =
			std::max(EllipseMask(u, v, 0.385f, 0.54f, 0.045f, 0.13f),
					 EllipseMask(u, v, 0.615f, 0.54f, 0.045f, 0.13f));
		const float hairMask = std::max(outer * (1.0f - face), locks);
		const float strands =
			0.75f + 0.5f * Fbm(u * 20.0f, v * 6.0f, spec.seed + 3);
		Blend(color, Shade(Mul({0.32f, 0.22f, 0.10f}, strands), gnx, gny), hairMask);
		// Band clipped to the hair silhouette so it doesn't float past it.
		const float band = BoxMask(u, v, 0.5f, 0.352f, 0.118f, 0.009f) * outer;
		Blend(color, Shade(spec.gear, gnx, gny * 0.3f), band);
		Blend(color, kRimColor, EllipseMask(u, v, 0.5f, 0.352f, 0.016f, 0.016f));
		break;
	}
	case Headgear::Hat: {
		// Wide brim and a slightly bent cone, plus a gray beard hanging from
		// the chin (clipped above so it starts under the mouth).
		const float beard = EllipseMask(u, v, 0.5f, 0.60f, 0.062f, 0.105f) *
							std::clamp((v - 0.525f) / kAA, 0.0f, 1.0f);
		const float wisps =
			0.70f + 0.6f * Fbm(u * 24.0f, v * 10.0f, spec.seed + 4);
		Blend(color, Mul({0.60f, 0.58f, 0.56f}, wisps), beard);

		float bnx, bny;
		const float brim = EllipseMask(u, v, 0.5f, 0.315f, 0.27f, 0.042f, &bnx, &bny);
		Blend(color, Shade(Mul(spec.gear, 0.85f), bnx, bny), brim);
		if (v >= 0.08f && v <= 0.318f) {
			const float t = (v - 0.08f) / (0.318f - 0.08f); // apex → brim
			const float cx = 0.55f - 0.05f * t;             // bent tip
			const float w = 0.012f + 0.150f * t;
			const float cone = std::clamp((w - std::fabs(u - cx)) / kAA, 0.0f, 1.0f);
			const float cnx = std::clamp((u - cx) / std::max(w, 0.02f), -1.0f, 1.0f);
			const float folds =
				0.88f + 0.24f * (Fbm(u * 8.0f, v * 8.0f, spec.seed + 5) - 0.5f);
			Blend(color, Shade(Mul(spec.gear, folds), cnx, -0.2f), cone);
		}
		break;
	}
	}
	return color;
}

} // namespace

bool BakePortraits(const std::string& dir) {
	bool allOk = true;
	std::vector<u8> rgba(static_cast<size_t>(kSize) * kSize * 4);

	for (const PortraitSpec& spec : kSpecs) {
		for (u32 py = 0; py < kSize; ++py) {
			for (u32 px = 0; px < kSize; ++px) {
				const float u = (static_cast<float>(px) + 0.5f) / kSize;
				const float v = (static_cast<float>(py) + 0.5f) / kSize;
				Vec3f color = PaintPixel(u, v, spec);

				// Vignette + grain, straight to sRGB bytes (authored in
				// display space — no HDR lighting to tonemap here).
				const float vx = u - 0.5f, vy = v - 0.5f;
				const float vignette = 1.0f - 0.85f * (vx * vx + vy * vy);
				const float grain = (Hash(px, py, spec.seed + 7) - 0.5f) * 0.03f;
				auto pack = [&](float c) {
					return static_cast<u8>(
						std::clamp(c * vignette + grain, 0.0f, 1.0f) * 255.0f);
				};
				const size_t i = (static_cast<size_t>(py) * kSize + px) * 4;
				rgba[i + 0] = pack(color.x);
				rgba[i + 1] = pack(color.y);
				rgba[i + 2] = pack(color.z);
				rgba[i + 3] = 255;
			}
		}

		const std::string path = dir + "\\" + spec.file + ".png";
		if (stbi_write_png(path.c_str(), kSize, kSize, 4, rgba.data(), kSize * 4)) {
			log::Info("Wrote {}", path);
		} else {
			log::Error("Failed to write {}", path);
			allOk = false;
		}
	}
	return allOk;
}

} // namespace dungeon::baker
