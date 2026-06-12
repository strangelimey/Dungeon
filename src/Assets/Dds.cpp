#include "Assets/Dds.h"

#include "Assets/File.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace dungeon::assets {

namespace {
constexpr u32 kMagic = 0x20534444; // "DDS "
constexpr u32 kHeaderBytes = 4 + 124;

u32 ReadU32(const std::vector<u8>& bytes, size_t offset) {
	u32 value;
	std::memcpy(&value, bytes.data() + offset, sizeof(value));
	return value;
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
	const u32 pfBitCount = ReadU32(*bytes, 4 + 84);
	const u32 pfRMask = ReadU32(*bytes, 4 + 88);

	if (pfBitCount != 32 || pfRMask != 0x000000FF)
		return std::unexpected(
			std::format("unsupported DDS layout (expect RGBA8): {}", path));
	if (width == 0 || height == 0)
		return std::unexpected(std::format("empty DDS: {}", path));

	MipChain chain;
	chain.width = width;
	chain.height = height;
	chain.levels.reserve(mipCount);

	size_t offset = kHeaderBytes;
	u32 w = width, h = height;
	for (u32 level = 0; level < mipCount; ++level) {
		const size_t size = static_cast<size_t>(w) * h * 4;
		if (offset + size > bytes->size())
			return std::unexpected(std::format("truncated DDS: {}", path));
		ImageData image;
		image.width = w;
		image.height = h;
		image.pixels.assign(bytes->begin() + static_cast<ptrdiff_t>(offset),
							bytes->begin() + static_cast<ptrdiff_t>(offset + size));
		chain.levels.push_back(std::move(image));
		offset += size;
		w = std::max(1u, w / 2);
		h = std::max(1u, h / 2);
	}
	return chain;
}

} // namespace dungeon::assets
