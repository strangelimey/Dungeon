#pragma once

#include "Core/Types.h"

#include <expected>
#include <string>
#include <vector>

namespace dungeon::assets {

std::expected<std::vector<u8>, std::string> ReadBinaryFile(const std::string& path);

// Writes (replaces) a file; creates parent directories as needed.
bool WriteBinaryFile(const std::string& path, const void* data, size_t size);

} // namespace dungeon::assets
