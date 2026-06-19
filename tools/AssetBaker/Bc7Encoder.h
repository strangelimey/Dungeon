#pragma once

#include "Assets/Image.h"

#include <vector>

namespace dungeon::baker {

// Encodes an RGBA8 image as BC7 blocks. Each block trials mode 6 (one subset,
// RGBA) and — for fully-opaque blocks — mode 1 (two subsets, RGB) and keeps the
// lower-error result, so material edges get a colour line per region instead of
// one smeared line. Returns ceil(w/4) * ceil(h/4) * 16 bytes.
std::vector<u8> EncodeBc7(const assets::ImageData& image);

} // namespace dungeon::baker
