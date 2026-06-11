// ============================================================================
// TitleBaker.cpp — the landing-page background image.
//
// A tiny one-point-perspective raycaster in the spirit of the genre's
// originals: each pixel shoots a ray down a stone corridor and hits the
// floor, ceiling, a side wall, or the far wall, where a teal portal glows.
// Surfaces get the same brick/slab patterns as the game textures (shared
// noise in Noise.h), lit by warm wall torches. The "modern touch" is in the
// grade: angular torch/portal halos (cheap bloom), exponential depth fog,
// filmic-ish tonemap, vignette, and grain.
//
// Output: title_bg.png (1920x1080). Purely deterministic.
// ============================================================================
#include "TitleBaker.h"

#include "Core/Log.h"
#include "Core/Types.h"
#include "Noise.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace dungeon::baker {

namespace {

constexpr u32 kWidth = 1920;
constexpr u32 kHeight = 1080;

// Corridor geometry (world units, matching the game's proportions).
constexpr float kHalfWidth = 1.0f;
constexpr float kCeiling = 2.5f;
constexpr float kEndZ = 13.0f;
constexpr float kEyeHeight = 1.30f;
constexpr float kFov = 0.62f; // tan(half vertical fov)

struct Vec3f {
	float x, y, z;
};

float Dot(const Vec3f& a, const Vec3f& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3f Normalize(const Vec3f& v) {
	const float inv = 1.0f / std::sqrt(Dot(v, v));
	return {v.x * inv, v.y * inv, v.z * inv};
}

// Which surface a ray hit.
enum class Surface { LeftWall, RightWall, Floor, Ceiling, EndWall };

// --- surface patterns (grayscale height-ish value in [0,1]) -------------------

float BrickPattern(float u, float v, u32 seed) {
	// u along the wall, v up the wall; bricks 0.5 x 0.31 world units.
	const float bw = 0.50f, bh = 0.3125f;
	const u32 row = static_cast<u32>(v / bh);
	const float us = u + (row % 2 ? bw * 0.5f : 0.0f);
	const float bx = std::fmod(std::fmod(us, bw) + bw, bw);
	const float by = std::fmod(std::fmod(v, bh) + bh, bh);
	const u32 col = static_cast<u32>(us / bw);

	const float edge = 0.02f;
	const float plateau = std::clamp(std::min({bx, bw - bx, by, bh - by}) / edge, 0.0f, 1.0f);
	const float tint = 0.75f + Hash(col, row, seed) * 0.25f;
	const float grain = (Fbm(u * 6.0f, v * 6.0f, seed + 5) - 0.5f) * 0.25f;
	return std::clamp(plateau * tint + grain, 0.0f, 1.0f);
}

float SlabPattern(float u, float v, u32 seed) {
	const float s = 1.0f;
	const float sx = std::fmod(std::fmod(u, s) + s, s);
	const float sy = std::fmod(std::fmod(v, s) + s, s);
	const float edge = 0.025f;
	const float plateau =
		std::clamp(std::min({sx, s - sx, sy, s - sy}) / edge, 0.0f, 1.0f);
	const u32 col = static_cast<u32>(std::floor(u / s) + 64.0f);
	const u32 row = static_cast<u32>(std::floor(v / s) + 64.0f);
	const float tint = 0.78f + Hash(col, row, seed) * 0.22f;
	const float grain = (Fbm(u * 4.0f, v * 4.0f, seed + 3) - 0.5f) * 0.22f;
	return std::clamp(plateau * tint + grain, 0.0f, 1.0f);
}

// --- lights ---------------------------------------------------------------------

struct Torch {
	Vec3f position;
	float intensity;
};

const Torch kTorches[] = {
	{{-kHalfWidth + 0.04f, 1.85f, 2.6f}, 1.5f},
	{{kHalfWidth - 0.04f, 1.85f, 5.4f}, 1.4f},
	{{-kHalfWidth + 0.04f, 1.85f, 8.2f}, 1.3f},
	{{kHalfWidth - 0.04f, 1.85f, 11.0f}, 1.2f},
};
const Vec3f kTorchColor{1.0f, 0.55f, 0.22f};

const Vec3f kPortalCenter{0.0f, 1.05f, kEndZ - 0.01f};
const Vec3f kPortalColor{0.25f, 0.95f, 0.70f};

// Inside the portal arch on the end wall? Rounded-top doorway.
bool InPortal(float x, float y) {
	constexpr float kW = 0.42f, kTop = 1.55f;
	if (std::fabs(x) > kW || y < 0.06f) return false;
	if (y <= kTop) return true;
	const float dy = y - kTop;
	return dy * dy + x * x <= kW * kW; // rounded cap
}

} // namespace

bool BakeTitleImage(const std::string& dir) {
	std::vector<u8> rgba(static_cast<size_t>(kWidth) * kHeight * 4);
	const float aspect = static_cast<float>(kWidth) / kHeight;
	const Vec3f eye{0.0f, kEyeHeight, 0.0f};

	for (u32 py = 0; py < kHeight; ++py) {
		for (u32 px = 0; px < kWidth; ++px) {
			// --- primary ray --------------------------------------------------
			const float u = (static_cast<float>(px) + 0.5f) / kWidth;
			const float v = (static_cast<float>(py) + 0.5f) / kHeight;
			const Vec3f ray =
				Normalize({(u - 0.5f) * 2.0f * kFov * aspect, (0.5f - v) * 2.0f * kFov,
						   1.0f});

			// Intersect the four corridor planes; nearest positive t wins.
			float t = 1e9f;
			Surface surface = Surface::EndWall;
			if (ray.x < 0) {
				const float tx = (-kHalfWidth - eye.x) / ray.x;
				if (tx < t) { t = tx; surface = Surface::LeftWall; }
			} else if (ray.x > 0) {
				const float tx = (kHalfWidth - eye.x) / ray.x;
				if (tx < t) { t = tx; surface = Surface::RightWall; }
			}
			if (ray.y < 0) {
				const float ty = (0.0f - eye.y) / ray.y;
				if (ty < t) { t = ty; surface = Surface::Floor; }
			} else if (ray.y > 0) {
				const float ty = (kCeiling - eye.y) / ray.y;
				if (ty < t) { t = ty; surface = Surface::Ceiling; }
			}
			// Far wall caps the corridor.
			const float tEnd = (kEndZ - eye.z) / ray.z;
			if (tEnd < t) { t = tEnd; surface = Surface::EndWall; }

			const Vec3f hit{eye.x + ray.x * t, eye.y + ray.y * t, eye.z + ray.z * t};

			// --- material -------------------------------------------------------
			float pattern;
			Vec3f normal;
			Vec3f base{0.42f, 0.38f, 0.33f}; // warm stone
			switch (surface) {
			case Surface::LeftWall:
				pattern = BrickPattern(hit.z, hit.y, 11u);
				normal = {1, 0, 0};
				break;
			case Surface::RightWall:
				pattern = BrickPattern(hit.z, hit.y, 13u);
				normal = {-1, 0, 0};
				break;
			case Surface::Floor:
				pattern = SlabPattern(hit.x, hit.z, 41u);
				normal = {0, 1, 0};
				base = {0.36f, 0.36f, 0.38f};
				break;
			case Surface::Ceiling:
				pattern = 0.55f + (Fbm(hit.x * 2.0f, hit.z * 2.0f, 61u) - 0.5f) * 0.5f;
				normal = {0, -1, 0};
				base = {0.30f, 0.28f, 0.26f};
				break;
			default: // EndWall
				pattern = BrickPattern(hit.x + 8.0f, hit.y, 17u);
				normal = {0, 0, -1};
				break;
			}

			Vec3f albedo{base.x * (0.35f + 0.65f * pattern),
						 base.y * (0.35f + 0.65f * pattern),
						 base.z * (0.35f + 0.65f * pattern)};

			// --- lighting ---------------------------------------------------------
			Vec3f color{albedo.x * 0.05f, albedo.y * 0.05f, albedo.z * 0.06f}; // ambient
			for (const Torch& torch : kTorches) {
				const Vec3f toLight{torch.position.x - hit.x, torch.position.y - hit.y,
									torch.position.z - hit.z};
				const float dist2 = Dot(toLight, toLight);
				const float dist = std::sqrt(dist2);
				const Vec3f l{toLight.x / dist, toLight.y / dist, toLight.z / dist};
				const float ndl = std::max(0.0f, Dot(normal, l));
				const float atten = torch.intensity / (1.0f + dist2 * 0.55f);
				color.x += albedo.x * kTorchColor.x * ndl * atten;
				color.y += albedo.y * kTorchColor.y * ndl * atten;
				color.z += albedo.z * kTorchColor.z * ndl * atten;
			}

			// Portal: emissive surface + light spilling onto nearby stone.
			if (surface == Surface::EndWall && InPortal(hit.x, hit.y)) {
				const float r = std::sqrt(hit.x * hit.x +
										  (hit.y - kPortalCenter.y) * (hit.y - kPortalCenter.y));
				const float swirl =
					0.75f + 0.25f * Fbm(hit.x * 4.0f + 9.0f, hit.y * 4.0f, 77u, 5);
				const float core = std::exp(-r * r * 2.2f) * 2.2f + 0.35f;
				color = {kPortalColor.x * core * swirl, kPortalColor.y * core * swirl,
						 kPortalColor.z * core * swirl};
			} else {
				const Vec3f toPortal{kPortalCenter.x - hit.x, kPortalCenter.y - hit.y,
									 kPortalCenter.z - hit.z};
				const float dist2 = Dot(toPortal, toPortal);
				const float dist = std::sqrt(dist2);
				const Vec3f l{toPortal.x / dist, toPortal.y / dist, toPortal.z / dist};
				const float ndl = std::max(0.0f, Dot(normal, l));
				const float atten = 1.6f / (1.0f + dist2 * 0.30f);
				color.x += albedo.x * kPortalColor.x * ndl * atten;
				color.y += albedo.y * kPortalColor.y * ndl * atten;
				color.z += albedo.z * kPortalColor.z * ndl * atten;
			}

			// --- depth fog (cool, slightly blue) ----------------------------------
			const float fog = 1.0f - std::exp(-hit.z * 0.10f);
			const Vec3f fogColor{0.012f, 0.016f, 0.026f};
			color.x = color.x * (1 - fog) + fogColor.x * fog;
			color.y = color.y * (1 - fog) + fogColor.y * fog;
			color.z = color.z * (1 - fog) + fogColor.z * fog;

			// --- halos: angular glow around each emitter (poor man's bloom) -------
			auto halo = [&](const Vec3f& at, const Vec3f& tint, float size, float gain) {
				const Vec3f toEmitter =
					Normalize({at.x - eye.x, at.y - eye.y, at.z - eye.z});
				const float align = std::max(0.0f, Dot(ray, toEmitter));
				const float g = std::pow(align, size) * gain;
				color.x += tint.x * g;
				color.y += tint.y * g;
				color.z += tint.z * g;
			};
			for (const Torch& torch : kTorches)
				halo(torch.position, kTorchColor, 5000.0f, 0.55f * torch.intensity);
			halo(kPortalCenter, kPortalColor, 700.0f, 0.50f);

			// --- grade: tonemap, vignette, grain ------------------------------------
			color = {color.x / (1 + color.x), color.y / (1 + color.y),
					 color.z / (1 + color.z)};
			const float vx = u - 0.5f, vy = v - 0.5f;
			const float vignette = 1.0f - 1.05f * (vx * vx + vy * vy);
			const float grain = (Hash(px, py, 97u) - 0.5f) * 0.02f;

			auto pack = [&](float c) {
				c = c * std::max(vignette, 0.0f) + grain;
				c = std::pow(std::clamp(c, 0.0f, 1.0f), 1.0f / 2.2f);
				return static_cast<u8>(c * 255.0f);
			};
			const size_t i = (static_cast<size_t>(py) * kWidth + px) * 4;
			rgba[i + 0] = pack(color.x);
			rgba[i + 1] = pack(color.y);
			rgba[i + 2] = pack(color.z);
			rgba[i + 3] = 255;
		}
	}

	const std::string path = dir + "\\title_bg.png";
	const int ok =
		stbi_write_png(path.c_str(), kWidth, kHeight, 4, rgba.data(), kWidth * 4);
	if (ok) log::Info("Wrote {}", path);
	else log::Error("Failed to write {}", path);
	return ok != 0;
}

} // namespace dungeon::baker
