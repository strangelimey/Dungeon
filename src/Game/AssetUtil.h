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
#include <optional>
#include <string>
#include <vector>

namespace dungeon::game {

// Loads models/<name> or aborts with the loader's error.
assets::ModelData LoadModelOrDie(const std::string& name);

// Loads models/<name>, or nullopt when it is absent. For an OPTIONAL sibling of
// a required asset, where missing is a legitimate answer rather than a broken
// install: the worn wall panels' open-sided variants exist only for the surface
// sets whose displacement repeats with the cell (BakeWornTiers), and the caller
// falls back to the fully pinned panel for the rest.
std::optional<assets::ModelData> LoadModelIfPresent(const std::string& name);

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

// --- shared UI icons --------------------------------------------------------
// The close box every dialog draws in its top-right corner (assets/ui/
// icon_close — see ui::AddCloseButton). Loaded on the first ask and owned HERE
// so the ten dialogs that show it share ONE texture: a gfx::Texture holds a
// shader-visible SRV slot for life, and ten copies of one icon spent ten slots
// out of a heap whose exhaustion is an abort. Null if the asset is missing —
// AddCloseButton then falls back to a text "x".
const gfx::Texture* CloseIcon(gfx::GraphicsDevice& device);

// Loads the glyphs the control library draws itself with (assets/ui/
// icon_dropdown — the drop-down's expander box) and installs them in
// ui::SetControlIcons. Owned here for the same reason as the close box: ONE
// texture, one SRV slot, shared by every context — including the editor
// dialogs, which carry no Skin. Call once at startup; a missing asset leaves
// the control on its text glyph.
void LoadSharedControlIcons(gfx::GraphicsDevice& device);

// Drops every shared icon. MUST be called while the GraphicsDevice is still
// alive (~Game does it): the texture returns its SRV slot to the device's free
// list on destruction, so it may not outlive the device the way a plain
// function-local static would.
void ReleaseSharedIcons();

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
