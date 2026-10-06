#pragma once

#include <string>

namespace dungeon::baker {

// Builds the full box-filtered mip chain for one PNG, BC7-encodes every level
// and writes it as a DDS next to it (same stem, .dds extension). `srgb` is the
// flag the game creates the texture with: an sRGB image's levels are averaged
// in linear light (assets::Downsample), so it must match, or the bake is not
// the chain the game's own fallback would build.
bool BakeMipChain(const std::string& pngPath, const std::string& ddsPath, bool srgb);

// How the files of a folder are sampled, and so how their mips are averaged.
enum class MipColor {
	// assets/textures: a set's ALBEDO (<set>.png) is sRGB; its normal+height
	// (_n) and ORM (_mr) maps are data (DungeonWorld::LoadPbrSet).
	TextureSets,
	// Everything as stored: the portraits, which load linear like all UI art.
	Linear,
};

// Whether a texture-set file is its set's albedo, the one sampled as sRGB.
bool IsTextureSetAlbedo(const std::string& pngPath);

// Runs BakeMipChain for every .png in <texturesDir>. DDS files are derived
// artifacts (gitignored); rerun after importing or rebaking textures.
// skipCurrent leaves a CURRENT .dds alone (assets::BakedIsCurrent) - for a
// folder of thousands (the portraits), where a re-run should cost only what
// changed. The texture sets rebake everything, so an encoder change reaches all
// of them. `prefix` limits it to the .pngs whose names start with it (empty =
// all): the textures folder is ~640 chains, and a `runes` rebake wants `rune_`,
// not an hour.
bool BakeAllMips(const std::string& texturesDir, MipColor color, bool skipCurrent = false,
				 const std::string& prefix = {});

// The same for the images EMBEDDED in every .gltf/.glb in <modelsDir>: one BC7
// chain per image, beside the model as assets::EmbeddedImageSidecar names it,
// which the game loads instead of decoding the PNG/JPEG inside the file. A
// base-colour image is sRGB (assets::SrgbImages). Skips a CURRENT sidecar
// without decoding its image unless `force` (a filter or encoder change, which
// no timestamp can see). Rerun after importing a model.
bool BakeModelImageMips(const std::string& modelsDir, bool force = false);

} // namespace dungeon::baker
