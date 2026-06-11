#include "Assets/Image.h"

#include "Core/Log.h"

#include <stb_image.h>

namespace dungeon::assets {

namespace {
std::optional<ImageData> FromStb(unsigned char* data, int w, int h) {
    if (!data) return std::nullopt;
    ImageData img;
    img.width = static_cast<u32>(w);
    img.height = static_cast<u32>(h);
    img.pixels.assign(data, data + static_cast<size_t>(w) * h * 4);
    stbi_image_free(data);
    return img;
}
} // namespace

std::optional<ImageData> LoadImageFile(const std::string& path) {
    int w = 0, h = 0, comp = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!data) log::Warn("Failed to load image: {}", path);
    return FromStb(data, w, h);
}

std::optional<ImageData> LoadImageMemory(const u8* bytes, size_t size) {
    int w = 0, h = 0, comp = 0;
    unsigned char* data =
        stbi_load_from_memory(bytes, static_cast<int>(size), &w, &h, &comp, 4);
    return FromStb(data, w, h);
}

} // namespace dungeon::assets
