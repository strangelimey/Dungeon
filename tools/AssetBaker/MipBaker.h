#pragma once

#include <string>

namespace dungeon::baker {

// Builds the full box-filtered mip chain for one PNG and writes it as an
// uncompressed RGBA8 DDS next to it (same stem, .dds extension).
bool BakeMipChain(const std::string& pngPath, const std::string& ddsPath);

// Runs BakeMipChain for every .png in <texturesDir> whose name starts with
// `prefix` (empty = all). DDS files are derived artifacts (gitignored); rerun
// after importing or rebaking textures. The prefix exists because the whole
// folder is ~640 chains: a `runes` rebake wants `rune_`, not an hour.
bool BakeAllMips(const std::string& texturesDir, const std::string& prefix = {});

// The same for the images EMBEDDED in every .gltf/.glb in <modelsDir>: one BC7
// chain per image, beside the model as assets::EmbeddedImageSidecar names it,
// which the game loads instead of decoding the PNG/JPEG inside the file.
// Skips a sidecar already newer than its model. Rerun after importing a model.
bool BakeModelImageMips(const std::string& modelsDir);

} // namespace dungeon::baker
