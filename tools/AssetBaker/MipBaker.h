#pragma once

#include <string>

namespace dungeon::baker {

// Builds the full box-filtered mip chain for one PNG and writes it as an
// uncompressed RGBA8 DDS next to it (same stem, .dds extension).
bool BakeMipChain(const std::string& pngPath, const std::string& ddsPath);

// Runs BakeMipChain for every .png in <texturesDir>. DDS files are derived
// artifacts (gitignored); rerun after importing or rebaking textures.
// skipCurrent leaves a .dds already newer than its .png alone - for a folder of
// thousands (the portraits), where a re-run should cost only what changed. The
// texture sets rebake everything, so an encoder change reaches all of them.
bool BakeAllMips(const std::string& texturesDir, bool skipCurrent = false);

// The same for the images EMBEDDED in every .gltf/.glb in <modelsDir>: one BC7
// chain per image, beside the model as assets::EmbeddedImageSidecar names it,
// which the game loads instead of decoding the PNG/JPEG inside the file.
// Skips a sidecar already newer than its model. Rerun after importing a model.
bool BakeModelImageMips(const std::string& modelsDir);

} // namespace dungeon::baker
