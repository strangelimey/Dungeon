#pragma once

#include <string>

namespace dungeon::baker {

// Builds the full box-filtered mip chain for one PNG and writes it as an
// uncompressed RGBA8 DDS next to it (same stem, .dds extension).
bool BakeMipChain(const std::string& pngPath, const std::string& ddsPath);

// Runs BakeMipChain for every .png in <texturesDir>. DDS files are derived
// artifacts (gitignored); rerun after importing or rebaking textures.
bool BakeAllMips(const std::string& texturesDir);

} // namespace dungeon::baker
