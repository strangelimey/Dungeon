#pragma once

#include "Core/Types.h"

#include <expected>
#include <string>
#include <vector>

namespace dungeon::assets {

std::expected<std::vector<u8>, std::string> ReadBinaryFile(const std::string& path);

} // namespace dungeon::assets
