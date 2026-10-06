// ============================================================================
// Game/AssetUtil.h — asset-loading helpers shared across the game layer.
//
// A required MODEL fails hard (DN_ASSERT) with the loader's reason - a missing
// one means the assets/ pool wasn't baked or provisioned, and there is nothing
// to draw in its place. Everything else degrades and SAYS SO: a missing sound
// runs silent, a missing required texture is a magenta checker placeholder, a
// missing normal map is flat (LoadNormalMapFile), each with a warning. Textures
// prefer the baked .dds mip chains, falling back to PNG + runtime mips so a
// fresh checkout works before `AssetBaker mips` has run.
// ============================================================================
#pragma once

#include "Assets/Model.h"
#include "Assets/Wav.h"
#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Graphics/Texture.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon::game {

struct CatalogEntry;

// Loads models/<name> or aborts with the loader's error.
assets::ModelData LoadModelOrDie(const std::string& name);

// --- which model FILE a type loads ------------------------------------------
// THE ONE STATEMENT of how a catalog entry becomes a file under assets/models,
// asked by the loaders (DungeonWorld_Load.cpp) and by `levelcheck` alike.
// levelcheck used to compare the `model` field against the installed file
// STEMS, which passed four kinds of entry that each end in a LoadModelOrDie
// abort (code-review C441): a .glb named where the loader opens .gltf, an
// entry with no `model` (the loader falls back to the id), a fixture's
// `empty_model` / `part2_model`, and a palette's worn block meshes. Each
// family IS a loader, and its rules are the loader's:
enum class ModelFamily : u8 {
	// decorations, doors, stairs, buttons (DecorationKindFor): `model`, else the
	// id; .glb when the entry is `multimaterial`, else .gltf.
	Prop,
	// monsters (MonsterKindFor): `model`, else the id; .gltf.
	Monster,
	// fixtures (FixtureKindFor): `model`, else the id, plus `empty_model` (the
	// bare bracket) and `part2_model` (the coal bed) when named; all .gltf.
	Fixture,
	// wallfeatures, surfacefeatures (LoadFeatureMeshes): `model`, else the id;
	// .gltf.
	Feature,
	// items, weapons, armor (ItemKindFor): `model` only - an item naming none
	// draws as the shared tablet; .glb.
	Item,
};

// The fields `family`'s loader reads a model from, `model` first.
std::span<const std::string_view> ModelFields(ModelFamily family);

// The file the `field` of a type loads - `e` its catalog entry (null when the
// catalog lacks it), `id` its id - extension included, or "" when that field
// loads nothing. A type's MAIN model is never "" outside the Item family: the
// loader opens `<id>.gltf` for an entry with no `model`, and `.gltf` itself for
// one whose `model` is empty, and both are what this returns.
std::string ModelFileOf(ModelFamily family, const CatalogEntry* e, const std::string& id,
						std::string_view field = "model");

// Whether assets/models holds `file`, the name LoadModelOrDie(file) opens.
bool ModelFileInstalled(const std::string& file);

// A surface type's texture SET: its `texture`, else its id - also for an id the
// catalog lacks, so a hand-edited level still loads something
// (ResolveSurfacePalettes).
std::string SurfaceSetOf(const CatalogEntry* e, const std::string& id);

// A surface set's worn block mesh at a mesh tier (GameSettings::MeshSuffix:
// low / med / high). `panel` names a wall panel's phase and open sides
// (assets::WornPanelSuffix); empty is the fully pinned panel, the one file every
// set must have at every tier.
std::string WornBlockFile(std::string_view set, std::string_view tier,
						  std::string_view panel = {});

// Loads models/<name>, or nullopt when it is absent. For an OPTIONAL sibling of
// a required asset, where missing is a legitimate answer rather than a broken
// install: the worn wall panels' open-sided variants exist only for the surface
// sets whose displacement repeats with the cell (BakeWornTiers), and the caller
// falls back to the fully pinned panel for the rest.
std::optional<assets::ModelData> LoadModelIfPresent(const std::string& name);

// Loads sounds/<name>; a missing file warns and returns silence.
assets::SoundData LoadSound(const std::string& name);

// Loads a texture by stem (no extension), preferring the baked .dds mip
// chain (no runtime filtering); falls back to the PNG + runtime mips. A .dds
// older than its PNG (assets::BakedIsCurrent) or one the reader rejects is
// refused with a warning, never drawn in silence. Returns null if neither
// file exists. `srgb` selects an sRGB view (set it for albedo/color maps;
// leave false for normal/height/ORM linear data).
std::unique_ptr<gfx::Texture> TryLoadTextureFile(gfx::GraphicsDevice& device,
												 const std::string& stemPath,
												 bool srgb = false);

// A THUMBNAIL of a texture: the stem's baked .dds chain with every level wider
// than `maxPx` dropped (a 128px tile of a 2k set is ~16 KB instead of
// megabytes), else the source PNG at whatever size it is when no CURRENT chain
// is baked (the TryLoadTextureFile rule). Null if neither loads. It UPLOADS,
// which drains the GPU: call it from Update, never while a frame is being
// recorded. The asset picker's tiles, the editor's surface swatches and the
// portrait picker all load here.
//
// LINEAR BY DEFAULT, and that is a rule, not a preference: ANYTHING THE SPRITE
// BATCH DRAWS LOADS LINEAR. sprite.hlsl writes its sample straight into the
// UNORM back buffer, so the stored (already sRGB-encoded) bytes must reach it
// as they are; an sRGB view decodes them to linear light on the way and the
// image draws far darker than it is (code-review C158: every asset picker tile
// and editor swatch did, an albedo shown much darker than the material looks
// in the world). `srgb` = true is for a texture a LIT 3D pass samples - the
// icon bake and the picker's model tiles, which shade it like the scene does.
std::unique_ptr<gfx::Texture> LoadTextureThumb(gfx::GraphicsDevice& device,
											   const std::string& stemPath, u32 maxPx,
											   bool srgb = false);

// The mean of a texture's STORED values, 0..1 per channel with no sRGB decode -
// what a sprite of it correctly drawn (linear, above) averages to on screen.
// Read off the source `<stemPath>.png` (the baked mips are box-filtered in that
// same space, so a thumbnail's mean is the image's); nullopt when there is none.
// The thumbnail survey's yardstick (`assetpicker survey`, `editor palette
// swatches`): a decode of the whole image, so a dev readout, never per frame.
std::optional<Vec4> StoredMeanColor(const std::string& stemPath);

// As TryLoadTextureFile, but the texture is required: a missing one does NOT
// abort - it warns (naming what to bake) and returns a magenta/black checker
// placeholder, so a provisioning gap renders glaringly wrong but stays playable.
std::unique_ptr<gfx::Texture> LoadTextureFile(gfx::GraphicsDevice& device,
											  const std::string& stemPath,
											  bool srgb = false);

// A set's NORMAL map, `<albedoStem>_n` (linear; xyz the tangent-space normal,
// alpha the height the parallax marches). A missing one is a FLAT placeholder -
// (128, 128, 255) with height 255, so the lighting is the geometry's and the
// parallax march stops at once - and ONE warning naming the set (`setName`),
// never the magenta checker LoadTextureFile would hand back, which read as a
// normal tilts the surface in a checkerboard (code-review C471). `flat`, when
// given, says which one came back.
std::unique_ptr<gfx::Texture> LoadNormalMapFile(gfx::GraphicsDevice& device,
												const std::string& albedoStem,
												const std::string& setName,
												bool* flat = nullptr);

// --- shared UI icons --------------------------------------------------------
// The close box every dialog draws in its top-right corner (assets/ui/
// icon_close — see ui::AddCloseButton). Loaded on the first ask and owned HERE
// so the ten dialogs that show it share ONE texture: a gfx::Texture holds a
// shader-visible SRV slot for life, and ten copies of one icon spent ten slots
// out of a heap whose exhaustion is an abort. Null if the asset is missing —
// AddCloseButton then falls back to a text "x".
const gfx::Texture* CloseIcon(gfx::GraphicsDevice& device);

// An editor TOOLBAR icon disc by name (assets/ui/icon_tb_<name>), owned here
// for the same reason the close box is: TWO toolbars draw from this set now —
// the level editor's and the world screen's — and a per-view copy would spend
// a second SRV slot on a byte-identical image. Tried once per name, null when
// the art is missing (the caller falls back to the label).
const gfx::Texture* ToolbarIcon(gfx::GraphicsDevice& device,
								const std::string& name);

// Loads the glyphs the control library draws itself with (assets/ui/
// icon_dropdown + icon_dropdown_open - the drop-down's expander box, closed and
// open) and installs them in ui::SetControlIcons. Owned here for the same
// reason as the close box: one texture each, shared by every context — including the editor
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
// not authored types, so they are left out. A NAME, not a file: whether a type
// can load it is ModelFileOf + ModelFileInstalled's question, since the loader
// appends one extension and a stem installed with the other does not load.
std::vector<std::string> InstalledModels();
// The UI materials in assets/ui/stones (tools/BuildUiStones.py), sorted, as
// stems - what settings' ui_stone, dungeons.cat's `ui_stone` and a level's
// `uistone` record name.
std::vector<std::string> InstalledUiStones();
// Every typeface under assets/fonts, as a path RELATIVE to assets/ and using
// forward slashes — exactly the form fonts.cat's `file` field takes, so a
// listing entry can be handed straight back as a face. Sorted, families first
// (the walk is recursive: one directory per family).
std::vector<std::string> InstalledFonts();
// Whether texture set `set` has worn block meshes baked from it (its
// worn_<set>_med.gltf is in the pool) - whether it can be painted as a surface
// without a bake. The first form takes the models folder, for a caller already
// walking it.
bool HasWornMeshes(const std::filesystem::path& modelsDir, const std::string& set);
bool HasWornMeshes(const std::string& set);

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

// Every installed albedo with NO normal map beside it AT ITS RESOLUTION, as the
// stem the loader asks for ("rusted_iron_4k"), sorted. Per resolution because
// that is how a set loads: a 4k albedo with only a 2k `_n` still draws flat at
// Ultra (LoadNormalMapFile). `levelcheck` lists them (code-review C471).
std::vector<std::string> TextureStemsMissingNormals();

// Resolution bits, so callers don't hand-roll the masks.
constexpr u32 kRes1k = 1u, kRes2k = 2u, kRes4k = 4u;

} // namespace dungeon::game
