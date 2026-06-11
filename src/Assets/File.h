#pragma once

#include "Core/Types.h"

#include <optional>
#include <string>
#include <vector>

namespace dungeon::assets {

std::optional<std::vector<u8>> ReadBinaryFile(const std::string& path);

} // namespace dungeon::assets
