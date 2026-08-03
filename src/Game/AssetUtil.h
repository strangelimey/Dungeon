// ============================================================================
// Game/AssetUtil.h — asset-loading helpers shared across the game layer.
//
// Required assets fail hard (DN_ASSERT) with the loader's reason — a missing
// model or texture means the assets/ directory wasn't baked or copied next
// to the exe. Sounds degrade gracefully (the game runs silent), and textures
// prefer the baked .dds mip chains, falling back to PNG + runtime mips so a
// fresh checkout works before `AssetBaker mips` has run.
// ============================================================================
#pragma once

#include "Assets/Model.h"
#include "Assets/Wav.h"
#include "Core/Types.h"
#include "Graphics/Texture.h"

#include <memory>
#include <string>
#include <vector>

namespace dungeon::game {

// Loads models/<name> or aborts with the loader's error.
assets::ModelData LoadModelOrDie(const std::string& name);

// Loads sounds/<name>; a missing file warns and returns silence.
assets::SoundData LoadSound(const std::string& name);

// Loads a texture by stem (no extension), preferring the baked .dds mip
// chain (no runtime filtering); falls back to the PNG + runtime mips.
// Returns null if neither file exists. `srgb` selects an sRGB view (set it for
// albedo/color maps; leave false for normal/height/ORM linear data).
std::unique_ptr<gfx::Texture> TryLoadTextureFile(gfx::GraphicsDevice& device,
												 const std::string& stemPath,
												 bool srgb = false);

// As TryLoadTextureFile, but the texture is required — missing aborts.
std::unique_ptr<gfx::Texture> LoadTextureFile(gfx::GraphicsDevice& device,
											  const std::string& stemPath,
											  bool srgb = false);

// --- what the asset pool actually holds (the editor's dropdowns) ------------
// Installed PBR set names, sorted: the base names behind assets/textures'
// <name>_<res>.dds|png trio, with the _<res>, _n and _mr suffixes folded away
// (so one set appears once, whatever resolutions are installed).
std::vector<std::string> InstalledTextureSets();
// Model names in assets/models, sorted, extension stripped — what a catalog's
// `model` field names. The worn_* block meshes are baked per surface texture,
// not authored types, so they are left out.
std::vector<std::string> InstalledModels();
// Every typeface under assets/fonts, as a path RELATIVE to assets/ and using
// forward slashes — exactly the form fonts.cat's `file` field takes, so a
// listing entry can be handed straight back as a face. Sorted, families first
// (the walk is recursive: one directory per family).
std::vector<std::string> InstalledFonts();

// --- what the pool holds, in detail (the asset picker) ----------------------
// One installed asset as the picker describes it. Everything here comes from
// the DIRECTORY WALK — file names and sizes — so listing hundreds of sets costs
// no decoding. The questions a walk cannot answer (is the height map real or
// flat? where was it imported from?) are answered per SELECTED asset instead.
struct AssetInfo {
	std::string name;   // what a catalog's `texture` / `model` field binds
	std::string file;   // models only: the file, extension included
	u32 resolutions = 0; // texture sets: bit 0/1/2 = 1k/2k/4k installed
	bool normal = false; // <name>_<res>_n exists (normal + height in alpha)
	bool orm = false;    // <name>_<res>_mr exists (occlusion/roughness/metallic)
	bool baked = false;  // a .dds chain exists (else PNG source only)
	u64 bytes = 0;       // every file of the set/model, summed
	// Texture sets: worn block meshes exist for this set, so it can be PAINTED
	// as a surface. Which kind they were baked as isn't knowable from the pool
	// (one worn_<set>_<tier>.gltf per set, the kind is in the geometry) — the
	// project's catalogs answer that, and the picker's owner supplies it.
	bool worn = false;
};

// The installed PBR sets / models with the above filled in, sorted by name.
// One directory walk each (plus one over models/ for the worn-mesh kinds).
std::vector<AssetInfo> InstalledTextureSetInfo();
std::vector<AssetInfo> InstalledModelInfo();

// Resolution bits, so callers don't hand-roll the masks.
constexpr u32 kRes1k = 1u, kRes2k = 2u, kRes4k = 4u;

} // namespace dungeon::game
