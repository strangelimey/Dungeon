#include "Assets/Image.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>

namespace dungeon::assets {

void StbImageFree::operator()(void* pixels) const { stbi_image_free(pixels); }

namespace {
using StbPixels = std::unique_ptr<unsigned char, StbImageFree>;

ImageData FromStb(const StbPixels& data, int w, int h) {
	ImageData img;
	img.width = static_cast<u32>(w);
	img.height = static_cast<u32>(h);
	img.pixels.assign(data.get(), data.get() + static_cast<size_t>(w) * h * 4);
	return img;
}
} // namespace

std::expected<ImageData, std::string> LoadImageFile(const std::string& path) {
	int w = 0, h = 0, comp = 0;
	// stb opens `path` with the narrow fopen, which reads UTF-8 (Core/Paths.h).
	const StbPixels data(stbi_load(path.c_str(), &w, &h, &comp, 4));
	if (!data)
		return std::unexpected(
			std::format("failed to load image {}: {}", path, stbi_failure_reason()));
	return FromStb(data, w, h);
}

std::expected<ImageData, std::string> LoadImageMemory(const u8* bytes, size_t size) {
	int w = 0, h = 0, comp = 0;
	const StbPixels data(
		stbi_load_from_memory(bytes, static_cast<int>(size), &w, &h, &comp, 4));
	if (!data)
		return std::unexpected(
			std::format("failed to decode embedded image: {}", stbi_failure_reason()));
	return FromStb(data, w, h);
}

// ---- The sRGB curve, for the mip filter ---------------------------------------

namespace {

// IEC 61966-2-1's decode - what a *_SRGB format's sampler applies.
double SrgbToLinear(double c) {
	return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

// Both directions as tables, so a 2k chain costs four lookups and a short
// binary search a texel rather than a pow() each: `linear` is every code's
// light, and `roundUp[k]` the light at the MIDPOINT of codes k and k+1 (in code
// space), past which the nearest code is k+1. So re-encoding is "how many
// midpoints has this light reached" - exact, with no inverse curve to drift.
struct SrgbTables {
	float linear[256];
	float roundUp[255];

	SrgbTables() {
		for (int i = 0; i < 256; ++i)
			linear[i] = static_cast<float>(SrgbToLinear(i / 255.0));
		for (int k = 0; k < 255; ++k)
			roundUp[k] = static_cast<float>(SrgbToLinear((k + 0.5) / 255.0));
	}

	u8 Encode(float light) const {
		return static_cast<u8>(std::upper_bound(roundUp, roundUp + 255, light) - roundUp);
	}
};

// Built on first use; a function-local static is thread-safe to initialise,
// and the runtime fallback can run on a load task.
const SrgbTables& Srgb() {
	static const SrgbTables tables;
	return tables;
}

} // namespace

ImageData Downsample(const ImageData& src, bool srgb) {
	const SrgbTables& curve = Srgb();
	ImageData dst;
	dst.width = std::max(1u, src.width / 2);
	dst.height = std::max(1u, src.height / 2);
	dst.pixels.resize(static_cast<size_t>(dst.width) * dst.height * 4);
	for (u32 y = 0; y < dst.height; ++y) {
		for (u32 x = 0; x < dst.width; ++x) {
			const u32 sx = std::min(x * 2, src.width - 1);
			const u32 sy = std::min(y * 2, src.height - 1);
			const u32 sx1 = std::min(sx + 1, src.width - 1);
			const u32 sy1 = std::min(sy + 1, src.height - 1);
			const u8* p00 = &src.pixels[(static_cast<size_t>(sy) * src.width + sx) * 4];
			const u8* p01 = &src.pixels[(static_cast<size_t>(sy) * src.width + sx1) * 4];
			const u8* p10 = &src.pixels[(static_cast<size_t>(sy1) * src.width + sx) * 4];
			const u8* p11 = &src.pixels[(static_cast<size_t>(sy1) * src.width + sx1) * 4];
			u8* out = &dst.pixels[(static_cast<size_t>(y) * dst.width + x) * 4];
			for (u32 c = 0; c < 4; ++c) {
				if (srgb && c < 3) {
					const float light = (curve.linear[p00[c]] + curve.linear[p01[c]] +
										 curve.linear[p10[c]] + curve.linear[p11[c]]) *
										0.25f;
					out[c] = curve.Encode(light);
				} else {
					// Rounded: + 2 is half of the divisor.
					out[c] = static_cast<u8>((p00[c] + p01[c] + p10[c] + p11[c] + 2u) / 4u);
				}
			}
		}
	}
	return dst;
}

} // namespace dungeon::assets
