// ============================================================================
// Assets/Image.h — CPU-side image loading (stb_image: PNG, JPG, TGA, ...).
// Everything is normalized to RGBA8 on load; gfx::Texture uploads it.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <expected>
#include <string>
#include <vector>

namespace dungeon::assets {

// CPU-side image, always RGBA8.
struct ImageData {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> pixels; // width * height * 4
};

std::expected<ImageData, std::string> LoadImageFile(const std::string& path);
std::expected<ImageData, std::string> LoadImageMemory(const u8* bytes, size_t size);

} // namespace dungeon::assets
