#pragma once

#include "Assets/Image.h"

#include <vector>

namespace dungeon::baker {

// Encodes an RGBA8 image as BC7 blocks (mode 6 only: one subset, 7-bit
// endpoints + p-bit, 4-bit indices — the workhorse mode for photographic
// content). Returns ceil(w/4) * ceil(h/4) * 16 bytes.
std::vector<u8> EncodeBc7(const assets::ImageData& image);

} // namespace dungeon::baker
