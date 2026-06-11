#include "Assets/File.h"

#include <cstdio>
#include <filesystem>
#include <format>

namespace dungeon::assets {

std::expected<std::vector<u8>, std::string> ReadBinaryFile(const std::string& path) {
	std::FILE* f = nullptr;
	if (fopen_s(&f, path.c_str(), "rb") != 0 || !f)
		return std::unexpected(std::format("could not open file: {}", path));
	std::fseek(f, 0, SEEK_END);
	const long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::vector<u8> data(static_cast<size_t>(size));
	if (size > 0 && std::fread(data.data(), 1, data.size(), f) != data.size()) {
		std::fclose(f);
		return std::unexpected(std::format("short read on file: {}", path));
	}
	std::fclose(f);
	return data;
}

bool WriteBinaryFile(const std::string& path, const void* data, size_t size) {
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

	std::FILE* f = nullptr;
	if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
	const bool ok = std::fwrite(data, 1, size, f) == size;
	std::fclose(f);
	return ok;
}

} // namespace dungeon::assets
