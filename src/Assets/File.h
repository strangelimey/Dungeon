#pragma once

#include "Core/Types.h"

#include <expected>
#include <string>
#include <vector>

namespace dungeon::assets {

// The whole file, or why not: a missing file, a DIRECTORY, a short read or one
// that cannot be sized (a pipe, a device) is an error - never a throw or a
// SIZE_MAX buffer. Paths here are UTF-8 (Core/Paths.h).
std::expected<std::vector<u8>, std::string> ReadBinaryFile(const std::string& path);

// Writes (replaces) a file; creates parent directories as needed. False when
// the open, the write or the closing flush fails.
bool WriteBinaryFile(const std::string& path, const void* data, size_t size);

} // namespace dungeon::assets
