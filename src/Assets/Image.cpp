#include "Assets/Image.h"

#include <stb_image.h>

#include <algorithm>
#include <format>

namespace dungeon::assets {

namespace {
ImageData FromStb(unsigned char* data, int w, int h) {
	ImageData img;
	img.width = static_cast<u32>(w);
	img.height = static_cast<u32>(h);
	img.pixels.assign(data, data + static_cast<size_t>(w) * h * 4);
	stbi_image_free(data);
	return img;
}
} // namespace

std::expected<ImageData, std::string> LoadImageFile(const std::string& path) {
	int w = 0, h = 0, comp = 0;
	unsigned char* data = stbi_load(path.c_str(), &w, &h, &comp, 4);
	if (!data)
		return std::unexpected(
			std::format("failed to load image {}: {}", path, stbi_failure_reason()));
	return FromStb(data, w, h);
}

std::expected<ImageData, std::string> LoadImageMemory(const u8* bytes, size_t size) {
	int w = 0, h = 0, comp = 0;
	unsigned char* data =
		stbi_load_from_memory(bytes, static_cast<int>(size), &w, &h, &comp, 4);
	if (!data)
		return std::unexpected(
			std::format("failed to decode embedded image: {}", stbi_failure_reason()));
	return FromStb(data, w, h);
}

ImageData Downsample(const ImageData& src) {
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
			for (u32 c = 0; c < 4; ++c) {
				const u32 sum =
					src.pixels[(static_cast<size_t>(sy) * src.width + sx) * 4 + c] +
					src.pixels[(static_cast<size_t>(sy) * src.width + sx1) * 4 + c] +
					src.pixels[(static_cast<size_t>(sy1) * src.width + sx) * 4 + c] +
					src.pixels[(static_cast<size_t>(sy1) * src.width + sx1) * 4 + c];
				dst.pixels[(static_cast<size_t>(y) * dst.width + x) * 4 + c] =
					static_cast<u8>(sum / 4);
			}
		}
	}
	return dst;
}

} // namespace dungeon::assets
