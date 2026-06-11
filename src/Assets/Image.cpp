#include "Assets/Image.h"

#include <stb_image.h>

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

} // namespace dungeon::assets
