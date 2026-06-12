// ============================================================================
// MipBaker.cpp — moves mip generation to bake time.
//
// The game's Texture class can build mip chains at load, but box-filtering a
// 2K texture's pyramid on the CPU is the slowest part of startup. This tool
// does that filtering once per texture and stores the result as an
// uncompressed RGBA8 DDS (mip levels tightly packed after a standard 124-byte
// header); the game then loads the chain with a single read (Assets/Dds.cpp).
// ============================================================================
#include "MipBaker.h"

#include "Assets/File.h"
#include "Assets/Image.h"
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

bool WriteDds(const std::string& path, const std::vector<assets::ImageData>& mips) {
	// Standard DDS: magic + 124-byte header, uncompressed 32-bit RGBA masks.
	u32 header[31]{};
	header[0] = 124;                                  // dwSize
	header[1] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000;   // CAPS|HEIGHT|WIDTH|PIXELFORMAT|MIPMAPCOUNT
	header[2] = mips[0].height;
	header[3] = mips[0].width;
	header[4] = mips[0].width * 4;                    // pitch
	header[6] = static_cast<u32>(mips.size());        // mip count
	header[18] = 32;                                  // ddspf.dwSize
	header[19] = 0x41;                                // RGB | ALPHAPIXELS
	header[21] = 32;                                  // bit count
	header[22] = 0x000000FF;                          // R mask
	header[23] = 0x0000FF00;                          // G mask
	header[24] = 0x00FF0000;                          // B mask
	header[25] = 0xFF000000;                          // A mask
	header[27] = 0x8 | 0x1000 | 0x400000;             // COMPLEX | TEXTURE | MIPMAP

	std::vector<u8> file;
	size_t total = 4 + sizeof(header);
	for (const auto& mip : mips) total += mip.pixels.size();
	file.reserve(total);

	const u32 magic = 0x20534444; // "DDS "
	file.resize(4 + sizeof(header));
	std::memcpy(file.data(), &magic, 4);
	std::memcpy(file.data() + 4, header, sizeof(header));
	for (const auto& mip : mips)
		file.insert(file.end(), mip.pixels.begin(), mip.pixels.end());

	return assets::WriteBinaryFile(path, file.data(), file.size());
}

} // namespace

bool BakeMipChain(const std::string& pngPath, const std::string& ddsPath) {
	auto image = assets::LoadImageFile(pngPath);
	if (!image) {
		log::Error("{}", image.error());
		return false;
	}
	std::vector<assets::ImageData> mips;
	mips.push_back(std::move(*image));
	while (mips.back().width > 1 || mips.back().height > 1)
		mips.push_back(Downsample(mips.back()));

	if (!WriteDds(ddsPath, mips)) {
		log::Error("Failed to write {}", ddsPath);
		return false;
	}
	log::Info("Wrote {} ({} mips)", ddsPath, mips.size());
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
