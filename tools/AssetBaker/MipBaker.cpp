// ============================================================================
// MipBaker.cpp — moves mip generation AND block compression to bake time.
//
// The game's Texture class can build mip chains at load, but box-filtering a
// 2K texture's pyramid on the CPU is the slowest part of startup. This tool
// does that filtering once per texture, BC7-encodes every level (quarter the
// VRAM and bandwidth of RGBA8 — see Bc7Encoder.cpp), and stores the chain as
// a DX10-header DDS; the game then loads it with a single read
// (Assets/Dds.cpp) and uploads straight to a BC7 resource.
// ============================================================================
#include "MipBaker.h"

#include "Assets/File.h"
#include "Assets/Image.h"
#include "Bc7Encoder.h"
#include "Core/Log.h"
#include "Core/Types.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

namespace dungeon::baker {

namespace {

// Mirrors the runtime fallback in Graphics/Texture.cpp: 2x2 box filter, good
// enough for albedo and for normal maps (the shader renormalizes).
assets::ImageData Downsample(const assets::ImageData& src) {
	assets::ImageData dst;
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

bool WriteDdsBc7(const std::string& path, u32 width, u32 height,
				 const std::vector<std::vector<u8>>& levels) {
	// Standard DDS: magic + 124-byte header + 20-byte DX10 extension.
	u32 header[31]{};
	header[0] = 124;                                // dwSize
	header[1] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000; // CAPS|HEIGHT|WIDTH|PIXELFORMAT|MIPMAPCOUNT
	header[2] = height;
	header[3] = width;
	header[6] = static_cast<u32>(levels.size());    // mip count
	header[18] = 32;                                // ddspf.dwSize
	header[19] = 0x4;                               // FOURCC
	header[20] = 0x30315844;                        // "DX10"
	header[27] = 0x8 | 0x1000 | 0x400000;           // COMPLEX | TEXTURE | MIPMAP

	const u32 dx10[5] = {98 /*DXGI_FORMAT_BC7_UNORM*/, 3 /*TEXTURE2D*/, 0, 1, 0};

	std::vector<u8> file(4 + sizeof(header) + sizeof(dx10));
	const u32 magic = 0x20534444; // "DDS "
	std::memcpy(file.data(), &magic, 4);
	std::memcpy(file.data() + 4, header, sizeof(header));
	std::memcpy(file.data() + 4 + sizeof(header), dx10, sizeof(dx10));
	for (const auto& level : levels)
		file.insert(file.end(), level.begin(), level.end());

	return assets::WriteBinaryFile(path, file.data(), file.size());
}

} // namespace

bool BakeMipChain(const std::string& pngPath, const std::string& ddsPath) {
	auto image = assets::LoadImageFile(pngPath);
	if (!image) {
		log::Error("{}", image.error());
		return false;
	}
	if (image->width % 4 != 0 || image->height % 4 != 0) {
		// D3D12 requires BC top-level dimensions to be multiples of 4.
		log::Warn("{}: {}x{} is not block-aligned — skipped (PNG fallback applies)",
				  pngPath, image->width, image->height);
		return true;
	}

	const u32 width = image->width, height = image->height;
	std::vector<std::vector<u8>> levels;
	assets::ImageData level = std::move(*image);
	while (true) {
		levels.push_back(EncodeBc7(level));
		if (level.width == 1 && level.height == 1) break;
		level = Downsample(level);
	}

	if (!WriteDdsBc7(ddsPath, width, height, levels)) {
		log::Error("Failed to write {}", ddsPath);
		return false;
	}
	log::Info("Wrote {} ({} BC7 mips)", ddsPath, levels.size());
	return true;
}

bool BakeAllMips(const std::string& texturesDir) {
	bool ok = true;
	int count = 0;
	for (const auto& entry : std::filesystem::directory_iterator(texturesDir)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".png") continue;
		std::filesystem::path dds = entry.path();
		dds.replace_extension(".dds");
		ok &= BakeMipChain(entry.path().string(), dds.string());
		++count;
	}
	log::Info("Mip bake: {} textures processed", count);
	return ok;
}

} // namespace dungeon::baker
