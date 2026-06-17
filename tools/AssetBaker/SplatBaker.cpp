// ============================================================================
// SplatBaker.cpp — hit-feedback splat icons for the party bar.
//
// Three blood-impact splats (small / medium / hard) drawn as transparent RGBA:
// an irregular central blob whose radius is modulated by angular noise, ringed
// by a handful of satellite droplets. Severity scales the radius, the jagged
// edge, and the droplet count/spread. The shape lives entirely in the alpha
// channel so the icon overlays a portrait; the RGB is a dark-to-bright red
// gradient. Purely deterministic — same seed, same splat.
//
// Output: hit_splat_<small|med|hard>.png (128x128). NOTE: shipped as PNG only
// (no .dds mip bake) — the icons are tiny and few, so the runtime PNG path
// (TryLoadTextureFile) with its own mips is plenty and keeps the alpha exact.
// ============================================================================
#include "SplatBaker.h"

#include "Core/Log.h"
#include "Core/Types.h"
#include "Noise.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace dungeon::baker {

namespace {

constexpr u32 kSize = 128;
constexpr float kAA = 1.5f / kSize; // antialias width in uv units

struct SplatSpec {
	const char* file;     // hit_splat_<...>.png
	float radius;         // central-blob mean radius (uv)
	float jag;            // edge irregularity (fraction of radius)
	int droplets;         // satellite count
	float dropletSpread;  // max droplet distance from centre (uv)
	float dropletRadius;  // droplet mean radius (uv)
	u32 seed;
};

constexpr SplatSpec kSpecs[] = {
	{"hit_splat_small", 0.17f, 0.045f, 4, 0.30f, 0.030f, 1301u},
	{"hit_splat_med", 0.25f, 0.075f, 7, 0.40f, 0.045f, 1409u},
	{"hit_splat_hard", 0.32f, 0.115f, 11, 0.47f, 0.060f, 1523u},
};

// Smooth 0..1 coverage from a signed edge distance (positive = inside).
float Cover(float edge) { return std::clamp(edge / kAA, 0.0f, 1.0f); }

// Coverage of an irregular blob centred at (cx,cy): the radius wobbles with
// angular noise so the outline reads as a splatter rather than a disc.
float BlobCover(float u, float v, float cx, float cy, float radius, float jag,
				u32 seed) {
	const float dx = u - cx, dy = v - cy;
	const float dist = std::sqrt(dx * dx + dy * dy);
	if (dist < 1e-5f) return 1.0f;
	// Continuous around the wrap: feed cos/sin of the angle into the noise.
	const float c = dx / dist, s = dy / dist;
	const float n = Fbm(c * 2.2f + 3.0f, s * 2.2f + 3.0f, seed);
	const float r = radius * (1.0f + jag / radius * (n - 0.5f) * 2.0f);
	return Cover(r - dist);
}

// rgb gradient: dark crimson core lifting to a brighter rim as coverage drops.
void SplatColor(float coverage, float& r, float& g, float& b) {
	const float t = std::clamp(coverage, 0.0f, 1.0f);
	// t=1 (deep interior) darkest; t→0 (rim) brightest.
	r = 0.74f - 0.40f * t;
	g = 0.10f - 0.07f * t;
	b = 0.07f - 0.05f * t;
}

} // namespace

bool BakeHitSplats(const std::string& dir) {
	bool allOk = true;
	std::vector<u8> rgba(static_cast<size_t>(kSize) * kSize * 4);

	for (const SplatSpec& spec : kSpecs) {
		for (u32 py = 0; py < kSize; ++py) {
			for (u32 px = 0; px < kSize; ++px) {
				const float u = (static_cast<float>(px) + 0.5f) / kSize;
				const float v = (static_cast<float>(py) + 0.5f) / kSize;

				// Central blob plus satellite droplets; coverage is the max so
				// the pieces union into one splatter.
				float coverage = BlobCover(u, v, 0.5f, 0.5f, spec.radius, spec.jag,
										   spec.seed);
				for (int k = 0; k < spec.droplets; ++k) {
					const float ang =
						Hash(static_cast<u32>(k), 0u, spec.seed) * 6.2831853f;
					const float dist =
						spec.radius * 0.7f +
						Hash(static_cast<u32>(k), 1u, spec.seed) *
							(spec.dropletSpread - spec.radius * 0.7f);
					const float dr =
						spec.dropletRadius *
						(0.45f + Hash(static_cast<u32>(k), 2u, spec.seed));
					const float cx = 0.5f + std::cos(ang) * dist;
					const float cy = 0.5f + std::sin(ang) * dist;
					coverage = std::max(
						coverage,
						BlobCover(u, v, cx, cy, dr, dr * 0.6f, spec.seed + 17u + k));
				}

				float r, g, b;
				SplatColor(coverage, r, g, b);
				// A touch of grain breaks up the flat fill.
				const float grain = (Hash(px, py, spec.seed + 91u) - 0.5f) * 0.05f;
				auto pack = [&](float c) {
					return static_cast<u8>(std::clamp(c + grain, 0.0f, 1.0f) * 255.0f);
				};
				const size_t i = (static_cast<size_t>(py) * kSize + px) * 4;
				rgba[i + 0] = pack(r);
				rgba[i + 1] = pack(g);
				rgba[i + 2] = pack(b);
				// Slightly translucent at full coverage so the face reads through.
				rgba[i + 3] = static_cast<u8>(std::clamp(coverage, 0.0f, 1.0f) * 235.0f);
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
