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

// The one PNG writer (RGBA8, `width * 4` bytes a row): encoded in memory and
// written through WriteBinaryFile (Assets/File.h), so a failed open, a short
// write or a failed closing flush is a false with `why` set - stbi_write_png
// checked none of them, and the asset baker's three PNG writers each called it
// (code-review C416). Only the baker writes PNGs; the game reads them.
bool WritePngFile(const std::string& path, u32 width, u32 height, const u8* rgba,
				  std::string* why = nullptr);

// Frees a buffer stb_image returned (stbi_load, _16, _from_memory). Held in a
// std::unique_ptr from the moment it comes back, so a throw while copying out
// of it - the copy allocates - cannot leak it (code-review C231).
struct StbImageFree {
	void operator()(void* pixels) const;
};

// Halve an RGBA8 image with a 2x2 box filter (odd sizes clamp, so a 1px
// dimension stays 1), every average ROUNDED to the nearest code. Good enough
// for normal maps too, since the shader renormalizes after sampling.
//
// `srgb` is the flag the texture is CREATED with (its *_SRGB format). With it
// the colour channels are averaged in LINEAR light - decoded through the sRGB
// curve, averaged, re-encoded to the nearest code - the light the sampler
// blends. Averaging the stored bytes instead darkens fine albedo detail with
// distance (a black-and-white checker went to 127, where its light is 188), and
// the old truncating `sum / 4` lost about half a code a level, some four codes
// over a 2k chain, parallax height included (code-review C414). Alpha is
// coverage, never gamma-encoded, so it is averaged as stored either way.
//
// Lives HERE, in the lib that owns ImageData, because BOTH mip producers need
// exactly this and each used to carry its own byte-identical copy: the baker
// (AssetBaker/MipBaker, writing the .dds chains) and the runtime fallback
// (Graphics/Texture, for images generated in memory or loaded from a PNG that
// was never baked). The two have to agree - a mip chain that differs by which
// path built it is a bug nobody would think to look for - and two copies can
// only agree by luck. The flag has no default for the same reason: a caller
// that forgot it would build a chain the other path does not.
ImageData Downsample(const ImageData& src, bool srgb);

} // namespace dungeon::assets
