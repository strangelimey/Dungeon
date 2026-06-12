// ============================================================================
// Assets/Dds.h — loader for the engine's baked mip-chain textures.
//
// AssetBaker writes uncompressed RGBA8 DDS files with full mip pyramids
// (one per PNG, derived artifacts — see `AssetBaker mips`). Loading one is a
// single read + split into levels: no decode, no runtime mip filtering.
// Only the subset of DDS the baker emits is supported (RGBA8, 2D, mipped).
// ============================================================================
#pragma once

#include "Assets/Image.h"

#include <expected>
#include <string>

namespace dungeon::assets {

std::expected<MipChain, std::string> LoadDdsFile(const std::string& path);

} // namespace dungeon::assets
