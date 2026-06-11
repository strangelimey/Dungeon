#pragma once

#include "Core/Types.h"

#include <optional>
#include <string>
#include <vector>

namespace dungeon::assets {

// CPU-side image, always RGBA8.
struct ImageData {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> pixels; // width * height * 4
};

std::optional<ImageData> LoadImageFile(const std::string& path);
std::optional<ImageData> LoadImageMemory(const u8* bytes, size_t size);

} // namespace dungeon::assets
