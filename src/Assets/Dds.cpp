#include "Assets/Dds.h"

#include "Assets/File.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace dungeon::assets {

namespace {
constexpr u32 kMagic = 0x20534444;     // "DDS "
constexpr u32 kFourCcDx10 = 0x30315844; // "DX10"
constexpr u32 kDxgiBc7 = 98;            // DXGI_FORMAT_BC7_UNORM
constexpr u32 kHeaderBytes = 4 + 124;
constexpr u32 kDx10HeaderBytes = 20;

u32 ReadU32(const std::vector<u8>& bytes, size_t offset) {
	u32 value;
	std::memcpy(&value, bytes.data() + offset, sizeof(value));
	return value;
}

size_t LevelBytes(TextureFormat format, u32 w, u32 h) {
	if (format == TextureFormat::Bc7)
		return static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4) * 16;
	return static_cast<size_t>(w) * h * 4;
}
} // namespace

std::expected<MipChain, std::string> LoadDdsFile(const std::string& path) {
	auto bytes = ReadBinaryFile(path);
	if (!bytes) return std::unexpected(bytes.error());
	if (bytes->size() < kHeaderBytes || ReadU32(*bytes, 0) != kMagic)
		return std::unexpected(std::format("not a DDS file: {}", path));

	// Header layout offsets (bytes from file start, after the 4-byte magic).
	const u32 height = ReadU32(*bytes, 4 + 8);
	const u32 width = ReadU32(*bytes, 4 + 12);
	const u32 mipCount = std::max(1u, ReadU32(*bytes, 4 + 24));
	const u32 pfFlags = ReadU32(*bytes, 4 + 80);
	const u32 pfFourCc = ReadU32(*bytes, 4 + 84);
	const u32 pfBitCount = ReadU32(*bytes, 4 + 88);
	const u32 pfRMask = ReadU32(*bytes, 4 + 92);
	if (width == 0 || height == 0)
		return std::unexpected(std::format("empty DDS: {}", path));

	TextureFormat format;
	size_t offset;
	if ((pfFlags & 0x4) && pfFourCc == kFourCcDx10) {
		if (bytes->size() < kHeaderBytes + kDx10HeaderBytes)
			return std::unexpected(std::format("truncated DX10 DDS: {}", path));
		const u32 dxgiFormat = ReadU32(*bytes, kHeaderBytes);
		if (dxgiFormat != kDxgiBc7)
			return std::unexpected(
				std::format("unsupported DDS DXGI format {}: {}", dxgiFormat, path));
		format = TextureFormat::Bc7;
		offset = kHeaderBytes + kDx10HeaderBytes;
	} else if (pfBitCount == 32 && pfRMask == 0x000000FF) {
		format = TextureFormat::Rgba8;
		offset = kHeaderBytes;
	} else {
		return std::unexpected(
			std::format("unsupported DDS layout (expect RGBA8 or BC7): {}", path));
	}

	MipChain chain;
	chain.width = width;
	chain.height = height;
	chain.format = format;
	chain.levels.reserve(mipCount);

	u32 w = width, h = height;
	for (u32 level = 0; level < mipCount; ++level) {
		const size_t size = LevelBytes(format, w, h);
		if (offset + size > bytes->size())
			return std::unexpected(std::format("truncated DDS: {}", path));
		TextureLevel out;
		out.width = w;
		out.height = h;
		out.data.assign(bytes->begin() + static_cast<ptrdiff_t>(offset),
						bytes->begin() + static_cast<ptrdiff_t>(offset + size));
		chain.levels.push_back(std::move(out));
		offset += size;
		w = std::max(1u, w / 2);
		h = std::max(1u, h / 2);
	}
	return chain;
}

} // namespace dungeon::assets
