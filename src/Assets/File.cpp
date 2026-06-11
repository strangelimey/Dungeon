#include "Assets/File.h"

#include "Core/Log.h"

#include <cstdio>

namespace dungeon::assets {

std::optional<std::vector<u8>> ReadBinaryFile(const std::string& path) {
    std::FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) {
        log::Warn("Could not open file: {}", path);
        return std::nullopt;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<u8> data(static_cast<size_t>(size));
    if (size > 0 && std::fread(data.data(), 1, data.size(), f) != data.size()) {
        std::fclose(f);
        log::Warn("Short read on file: {}", path);
        return std::nullopt;
    }
    std::fclose(f);
    return data;
}

} // namespace dungeon::assets
