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

// GPU texture formats the asset pipeline produces. Bc7 is block-compressed
// (16 bytes per 4x4 block — a quarter of RGBA8) and decodes in the sampler.
enum class TextureFormat { Rgba8, Bc7 };

// One mip level as raw bytes in the chain's format (RGBA8: width*height*4;
// BC7: ceil(w/4)*ceil(h/4)*16).
struct TextureLevel {
	u32 width = 0;
	u32 height = 0;
	std::vector<u8> data;
};

// A full pre-built mip pyramid (level 0 = full resolution). Produced at bake
// time by AssetBaker (BC7 DDS files) so the game neither filters mips nor
// pays full RGBA bandwidth at load.
struct MipChain {
	u32 width = 0;
	u32 height = 0;
	TextureFormat format = TextureFormat::Rgba8;
	std::vector<TextureLevel> levels;
};

std::expected<ImageData, std::string> LoadImageFile(const std::string& path);
std::expected<ImageData, std::string> LoadImageMemory(const u8* bytes, size_t size);

} // namespace dungeon::assets
