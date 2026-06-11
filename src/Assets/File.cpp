#include "Assets/File.h"

#include <cstdio>
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

} // namespace dungeon::assets
