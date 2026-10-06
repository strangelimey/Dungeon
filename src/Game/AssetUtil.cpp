// ============================================================================
// Game/AssetUtil.cpp — see AssetUtil.h.
// ============================================================================
#include "Game/AssetUtil.h"

#include "Assets/Dds.h"
#include "Assets/File.h"
#include "Assets/Image.h"
#include "Core/Assert.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/Catalog.h"
#include "UI/ControlIcons.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <map>

namespace dungeon::game {

// The game draws models, so it takes their BAKED embedded images where the
// mip bake has written them (assets::LoadOptions::bakedImages).
static constexpr assets::LoadOptions kGameModel{.bakedImages = true};

assets::ModelData LoadModelOrDie(const std::string& name) {
	auto model = assets::LoadModel(paths::Asset("models\\" + name), kGameModel);
	DN_ASSERT(model.has_value(), model.error() + " — run AssetBaker over assets/");
	return std::move(*model);
}

std::optional<assets::ModelData> LoadModelIfPresent(const std::string& name) {
	auto model = assets::LoadModel(paths::Asset("models\\" + name), kGameModel);
	if (!model) return std::nullopt;
	return std::move(*model);
}

// --- which model FILE a type loads ------------------------------------------
std::span<const ModelCatalog> ModelCatalogs() {
	// DecorationKindFor is handed the decorations, doors, stairs and buttons
	// catalogs; every other model catalog is one loader's.
	static constexpr ModelCatalog kCatalogs[] = {
		{"decorations", ModelFamily::Prop},     {"doors", ModelFamily::Prop},
		{"stairs", ModelFamily::Prop},          {"buttons", ModelFamily::Prop},
		{"monsters", ModelFamily::Monster},     {"fixtures", ModelFamily::Fixture},
		{"wallfeatures", ModelFamily::Feature}, {"surfacefeatures", ModelFamily::Feature},
		{"items", ModelFamily::Item},           {"weapons", ModelFamily::Item},
		{"armor", ModelFamily::Item},
	};
	return kCatalogs;
}

std::optional<ModelFamily> ModelFamilyOf(std::string_view key) {
	for (const ModelCatalog& c : ModelCatalogs())
		if (c.key == key) return c.family;
	return std::nullopt;
}

std::span<const std::string_view> ModelFields(ModelFamily family) {
	static constexpr std::string_view kMain[] = {"model"};
	static constexpr std::string_view kFixture[] = {"model", "empty_model", "part2_model"};
	if (family == ModelFamily::Fixture) return kFixture;
	return kMain;
}

std::string ResolveModelFile(std::string_view name, bool preferGlb) {
	if (name.empty()) return {};
	const std::string preferred = std::string(name) + (preferGlb ? ".glb" : ".gltf");
	const std::string other = std::string(name) + (preferGlb ? ".gltf" : ".glb");
	// The other extension only when it is there and the preferred one is not:
	// with both installed the family's own wins, and with neither the load
	// aborts naming the family's own.
	return !ModelFileInstalled(preferred) && ModelFileInstalled(other) ? other : preferred;
}

std::string ModelFileOf(ModelFamily family, const CatalogEntry* e, const std::string& id,
						std::string_view field) {
	// Only a MAIN model falls back to the id, and an item's not even that (no
	// model = the tablet). A fallback name is used even when an authored field
	// is empty, exactly as the loaders' CatalogGet uses it.
	const bool fallsBack = field == "model" && family != ModelFamily::Item;
	const std::string name =
		CatalogGet(e, field, fallsBack ? std::string_view(id) : std::string_view());
	if (name.empty() && !fallsBack) return {};
	const bool glb = family == ModelFamily::Item ||
					 (family == ModelFamily::Prop && CatalogBool(e, "multimaterial", false));
	// An empty name that falls back still names a file - the bare extension,
	// which no install has - so the load (and levelcheck) report it, as before.
	return name.empty() ? std::string(glb ? ".glb" : ".gltf") : ResolveModelFile(name, glb);
}

bool ModelFileInstalled(const std::string& file) {
	if (file.empty()) return false; // "models\" is the directory itself
	std::error_code ec;
	return std::filesystem::is_regular_file(paths::Asset("models\\" + file), ec);
}

std::optional<UnloadableModel> FirstUnloadableModel(ModelFamily family, const CatalogEntry& e) {
	for (std::string_view field : ModelFields(family)) {
		const std::string file = ModelFileOf(family, &e, e.id, field);
		if (file.empty() || ModelFileInstalled(file)) continue;
		// The name, as the entry gives it: the file less the extension the
		// resolver put on it.
		const size_t dot = file.rfind('.');
		return UnloadableModel{std::string(field), file.substr(0, dot), file};
	}
	return std::nullopt;
}

std::string SurfaceSetOf(const CatalogEntry* e, const std::string& id) {
	return CatalogGet(e, "texture", id);
}

std::string WornBlockFile(std::string_view set, std::string_view tier, std::string_view panel) {
	return std::format("worn_{}_{}{}.gltf", set, tier, panel);
}

assets::SoundData LoadSound(const std::string& name) {
	auto sound = assets::LoadWavFile(paths::Asset("sounds\\" + name));
	if (!sound) log::Warn("{} (running silent)", sound.error());
	return std::move(sound).value_or(assets::SoundData{});
}

namespace {

// The baked chain for `stemPath`, when it may stand in for the PNG: present,
// readable and CURRENT (assets::BakedIsCurrent - no older than the PNG). A
// MISSING .dds is ordinary (UI art ships as PNG only; a fresh checkout has not
// run the mip bake), so that falls back quietly. One that EXISTS but is refused
// is not: the PNG path still draws, so nothing looks wrong. A reader bug hid
// that way for three and a half months while every texture was decoded from
// PNG, uncompressed, with its mips built at runtime - and a STALE one hid the
// same way, since nothing compared the dates: a `runes` re-bake (PNGs only)
// left the game drawing the old tablets (code-review C410).
std::optional<assets::MipChain> CurrentDds(const std::string& stemPath) {
	const std::string dds = stemPath + ".dds";
	const std::string png = stemPath + ".png";
	std::error_code ec;
	if (!std::filesystem::exists(dds, ec)) return std::nullopt;
	if (!assets::BakedIsCurrent(dds, png)) {
		log::Warn("{} is older than its PNG - decoding the PNG instead; rerun "
				  "AssetBaker mips",
				  dds);
		return std::nullopt;
	}
	auto mips = assets::LoadDdsFile(dds);
	if (!mips) {
		log::Warn("{} - loading the PNG instead", mips.error());
		return std::nullopt;
	}
	return std::move(*mips);
}

} // namespace

std::unique_ptr<gfx::Texture> TryLoadTextureFile(gfx::GraphicsDevice& device,
												 const std::string& stemPath, bool srgb) {
	if (auto mips = CurrentDds(stemPath))
		return std::make_unique<gfx::Texture>(device, *mips, srgb);
	if (auto image = assets::LoadImageFile(stemPath + ".png"))
		return std::make_unique<gfx::Texture>(device, *image, srgb);
	return nullptr;
}

std::unique_ptr<gfx::Texture> LoadTextureThumb(gfx::GraphicsDevice& device,
											   const std::string& stemPath, u32 maxPx, bool srgb) {
	// The baked chain, with its big levels dropped: a thumbnail wants maxPx,
	// not the 2048px the set installs at. Same file, a sliver of the memory.
	// Held to the same rule as a full load (CurrentDds).
	if (auto chain = CurrentDds(stemPath)) {
		assets::MipChain thumb;
		thumb.format = chain->format;
		for (const assets::TextureLevel& level : chain->levels) {
			if (level.width > maxPx) continue; // the levels a thumbnail can't use
			if (thumb.levels.empty()) {
				thumb.width = level.width;
				thumb.height = level.height;
			}
			thumb.levels.push_back(level);
		}
		if (!thumb.levels.empty())
			return std::make_unique<gfx::Texture>(device, thumb, srgb);
	}
	// No baked chain (a source-only set): the PNG, at whatever size it is.
	if (auto img = assets::LoadImageFile(stemPath + ".png"))
		return std::make_unique<gfx::Texture>(device, *img, srgb);
	return nullptr;
}

std::optional<Vec4> StoredMeanColor(const std::string& stemPath) {
	const auto img = assets::LoadImageFile(stemPath + ".png");
	if (!img || img->pixels.empty()) return std::nullopt;
	u64 sum[4] = {};
	for (size_t p = 0; p + 3 < img->pixels.size(); p += 4)
		for (size_t c = 0; c < 4; ++c) sum[c] += img->pixels[p + c];
	const double n = static_cast<double>(img->pixels.size() / 4) * 255.0;
	return Vec4{static_cast<float>(sum[0] / n), static_cast<float>(sum[1] / n),
				static_cast<float>(sum[2] / n), static_cast<float>(sum[3] / n)};
}

namespace {
// The one close-box texture, shared by every dialog. A namespace-scope owner
// rather than a function-local static so the lifetime is EXPLICIT:
// ReleaseSharedIcons drops it from ~Game, while the device that owns its SRV
// slot is still alive. A static would destruct at exit, after the device.
std::unique_ptr<gfx::Texture> g_closeIcon;
bool g_closeIconTried = false;
// The control library's own glyphs (ui::ControlIcons) — owned here, borrowed
// there. Same explicit lifetime: ReleaseSharedIcons clears the registry BEFORE
// dropping the texture, so no widget can name a freed SRV slot.
std::unique_ptr<gfx::Texture> g_dropDownIcon;
std::unique_ptr<gfx::Texture> g_dropDownOpenIcon;
// The toolbar discs, by name. A null ENTRY is a name that was tried and whose
// art is missing — kept, so a missing icon costs one failed load rather than
// one per frame the toolbar draws.
std::map<std::string, std::unique_ptr<gfx::Texture>> g_toolbarIcons;
} // namespace

const gfx::Texture* CloseIcon(gfx::GraphicsDevice& device) {
	// Tried-once, not loaded-once: a missing asset must not re-hit the disk for
	// every dialog that opens.
	if (!g_closeIconTried) {
		g_closeIconTried = true;
		const std::string stem = paths::Asset("ui\\icon_close");
		g_closeIcon = TryLoadTextureFile(device, stem);
		// Say so once. This used to fail SILENTLY in ten places at once — every
		// dialog just drew the text "x" fallback and nothing named the cause.
		if (!g_closeIcon) log::Warn("close icon missing: {}(.dds|.png)", stem);
	}
	return g_closeIcon.get();
}

const gfx::Texture* ToolbarIcon(gfx::GraphicsDevice& device,
								const std::string& name) {
	auto it = g_toolbarIcons.find(name);
	if (it == g_toolbarIcons.end()) {
		const std::string stem = paths::Asset("ui\\icon_tb_" + name);
		std::unique_ptr<gfx::Texture> tex = TryLoadTextureFile(device, stem);
		if (!tex) log::Warn("toolbar icon missing: {}(.dds|.png)", stem);
		it = g_toolbarIcons.emplace(name, std::move(tex)).first;
	}
	return it->second.get();
}

void LoadSharedControlIcons(gfx::GraphicsDevice& device) {
	const std::string stem = paths::Asset("ui\\icon_dropdown");
	// Non-sRGB like every other assets/ui image, so its tone matches the rest
	// of the chrome rather than the scene's albedo path.
	g_dropDownIcon = TryLoadTextureFile(device, stem);
	if (!g_dropDownIcon) log::Warn("dropdown icon missing: {}(.dds|.png)", stem);
	const std::string openStem = paths::Asset("ui\\icon_dropdown_open");
	g_dropDownOpenIcon = TryLoadTextureFile(device, openStem);
	if (!g_dropDownOpenIcon) log::Warn("dropdown icon missing: {}(.dds|.png)", openStem);
	ui::ControlIcons icons;
	icons.dropDown = g_dropDownIcon.get();
	icons.dropDownOpen = g_dropDownOpenIcon.get();
	ui::SetControlIcons(icons);
}

void ReleaseSharedIcons() {
	ui::SetControlIcons({}); // before the textures die — the registry borrows
	g_dropDownIcon.reset();
	g_dropDownOpenIcon.reset();
	g_closeIcon.reset();
	g_toolbarIcons.clear(); // borrowed by both toolbars; they are gone by now
	// Re-arm: a later device (the adapter-change relaunch builds a fresh one)
	// must reload rather than be handed the dead texture.
	g_closeIconTried = false;
}

// An 8x8 magenta/black checker texture, built in memory. Stands in for any
// texture that failed to load so a provisioning gap (e.g. a fresh clone or
// worktree without the gitignored .dds sets baked yet) renders glaringly wrong
// but stays PLAYABLE instead of aborting. The warning still names what to bake.
static std::unique_ptr<gfx::Texture> MakePlaceholderTexture(
	gfx::GraphicsDevice& device, bool srgb) {
	constexpr u32 kDim = 8;
	assets::ImageData img;
	img.width = img.height = kDim;
	img.pixels.resize(static_cast<size_t>(kDim) * kDim * 4);
	for (u32 y = 0; y < kDim; ++y)
		for (u32 x = 0; x < kDim; ++x) {
			const bool magenta = (((x / 4) + (y / 4)) & 1u) != 0;
			u8* px = &img.pixels[(static_cast<size_t>(y) * kDim + x) * 4];
			px[0] = magenta ? 255 : 0; // R
			px[1] = 0;                 // G
			px[2] = magenta ? 255 : 0; // B
			px[3] = 255;               // A
		}
	return std::make_unique<gfx::Texture>(device, img, srgb);
}

std::unique_ptr<gfx::Texture> LoadTextureFile(gfx::GraphicsDevice& device,
											  const std::string& stemPath, bool srgb) {
	if (auto texture = TryLoadTextureFile(device, stemPath, srgb)) return texture;
	// Don't abort on a missing texture — it's a provisioning gap, not data
	// corruption. Fall back to an obvious placeholder and flag what to bake.
	log::Warn("Missing texture {} — using placeholder; run AssetBaker over assets/",
			  stemPath);
	return MakePlaceholderTexture(device, srgb);
}

std::unique_ptr<gfx::Texture> LoadNormalMapFile(gfx::GraphicsDevice& device,
												const std::string& albedoStem,
												const std::string& setName, bool* flat) {
	const std::string stem = albedoStem + "_n";
	auto texture = TryLoadTextureFile(device, stem); // linear
	if (flat) *flat = !texture;
	if (texture) return texture;
	// FLAT, not the checker: a normal map is data, and the checker's magenta and
	// black decode to normals tilted half away from every light. 4x4 rather than
	// 1x1 so the runtime mip build has a level to halve.
	log::Warn("texture set '{}' has no normal map ({}) - drawn flat: no relief, no "
			  "parallax; re-import the set to restore it",
			  setName, stem);
	constexpr u32 kDim = 4;
	assets::ImageData img;
	img.width = img.height = kDim;
	img.pixels.resize(static_cast<size_t>(kDim) * kDim * 4);
	for (size_t p = 0; p < img.pixels.size(); p += 4) {
		img.pixels[p + 0] = 128; // x = 0
		img.pixels[p + 1] = 128; // y = 0
		img.pixels[p + 2] = 255; // z = 1: straight out of the surface
		img.pixels[p + 3] = 255; // height 1 = the top: the parallax march stops
	}
	return std::make_unique<gfx::Texture>(device, img, /*srgb*/ false);
}

// --- pool listings (see the header) -----------------------------------------

namespace {
// Directory walk shared by both listings: every file, extension stripped,
// filtered and de-duplicated by the caller's rule. A missing directory yields
// nothing (the editor just shows an empty dropdown).
std::vector<std::string> ScanStems(const std::string& dir,
								   const std::function<bool(std::string&)>& accept) {
	std::vector<std::string> out;
	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
		if (ec || !entry.is_regular_file()) continue;
		std::string stem = entry.path().stem().string();
		if (!accept(stem)) continue;
		if (std::find(out.begin(), out.end(), stem) == out.end())
			out.push_back(std::move(stem));
	}
	std::ranges::sort(out);
	return out;
}
} // namespace

std::vector<std::string> InstalledTextureSets() {
	// A set installs as <name>_<res>{,_n,_mr}.dds|png — strip the map suffix,
	// then the resolution, and what's left is the name catalogs bind by.
	return ScanStems(paths::Asset("textures"), [](std::string& stem) {
		for (const char* suffix : {"_n", "_mr"})
			if (stem.ends_with(suffix)) stem.resize(stem.size() - std::strlen(suffix));
		for (const char* res : {"_1k", "_2k", "_4k"})
			if (stem.ends_with(res)) {
				stem.resize(stem.size() - std::strlen(res));
				return true;
			}
		return false; // not a resolution-tagged map: not a PBR set
	});
}

namespace {
// Splits a texture file's stem into (set name, resolution bit, map suffix).
// "cobblestone_wall_2k_n" -> {"cobblestone_wall", kRes2k, normal}. False when
// the stem is not a resolution-tagged PBR map (so not part of a set at all).
bool SplitSetStem(std::string stem, std::string& name, u32& res, bool& normal,
				  bool& orm) {
	normal = orm = false;
	if (stem.ends_with("_n")) {
		normal = true;
		stem.resize(stem.size() - 2);
	} else if (stem.ends_with("_mr")) {
		orm = true;
		stem.resize(stem.size() - 3);
	}
	const std::pair<const char*, u32> tags[] = {
		{"_1k", kRes1k}, {"_2k", kRes2k}, {"_4k", kRes4k}};
	for (const auto& [tag, bit] : tags)
		if (stem.ends_with(tag)) {
			stem.resize(stem.size() - std::strlen(tag));
			name = std::move(stem);
			res = bit;
			return true;
		}
	return false;
}
} // namespace

// WHICH kind the meshes were baked as is deliberately not answered here: the
// bake writes one worn_<set>_<tier>.gltf per set and the kind lives in the
// geometry, not the name. The catalogs that paint with the set (and a shipped
// set's own record, Assets/WornSets.h) are the honest source for that - see
// Game::AdoptSurfaceSet.
bool HasWornMeshes(const std::filesystem::path& modelsDir, const std::string& set) {
	std::error_code ec;
	return std::filesystem::exists(modelsDir / WornBlockFile(set, "med"), ec);
}

bool HasWornMeshes(const std::string& set) {
	return HasWornMeshes(std::filesystem::path(paths::Asset("models")), set);
}

std::vector<AssetInfo> InstalledTextureSetInfo() {
	namespace fs = std::filesystem;
	const fs::path dir = paths::Asset("textures");
	const fs::path models = paths::Asset("models");
	std::vector<AssetInfo> out;
	std::error_code ec;
	for (const auto& entry : fs::directory_iterator(dir, ec)) {
		if (ec || !entry.is_regular_file()) continue;
		const std::string ext = entry.path().extension().string();
		const bool dds = ext == ".dds";
		if (!dds && ext != ".png") continue;
		std::string name;
		u32 res = 0;
		bool normal = false, orm = false;
		if (!SplitSetStem(entry.path().stem().string(), name, res, normal, orm))
			continue;
		// One record per SET: the files fold into it as they are met.
		auto it = std::ranges::find_if(out, [&](const AssetInfo& a) { return a.name == name; });
		if (it == out.end()) {
			out.push_back(AssetInfo{.name = name});
			it = out.end() - 1;
			it->worn = HasWornMeshes(models, name);
		}
		it->resolutions |= res;
		it->normal |= normal;
		it->orm |= orm;
		it->baked |= dds;
		it->bytes += entry.file_size(ec);
	}
	std::ranges::sort(out, {}, &AssetInfo::name);
	return out;
}

std::vector<std::string> TextureStemsMissingNormals() {
	namespace fs = std::filesystem;
	// Every map's stem (.dds and .png alike: either one loads), then the albedos
	// among them asked whether their `_n` is there too.
	std::vector<std::string> stems;
	std::error_code ec;
	for (const auto& entry : fs::directory_iterator(paths::Asset("textures"), ec)) {
		if (ec || !entry.is_regular_file()) continue;
		const std::string ext = entry.path().extension().string();
		if (ext == ".dds" || ext == ".png") stems.push_back(entry.path().stem().string());
	}
	std::ranges::sort(stems);
	std::vector<std::string> out;
	for (const std::string& stem : stems) {
		std::string name;
		u32 res = 0;
		bool normal = false, orm = false;
		if (!SplitSetStem(stem, name, res, normal, orm) || normal || orm) continue;
		if (!std::ranges::binary_search(stems, stem + "_n") &&
			(out.empty() || out.back() != stem)) // a .dds and a .png: one stem
			out.push_back(stem);
	}
	return out;
}

std::vector<AssetInfo> InstalledModelInfo() {
	namespace fs = std::filesystem;
	std::vector<AssetInfo> out;
	std::error_code ec;
	for (const auto& entry : fs::directory_iterator(paths::Asset("models"), ec)) {
		if (ec || !entry.is_regular_file()) continue;
		const std::string ext = entry.path().extension().string();
		if (ext != ".gltf" && ext != ".glb") continue;
		std::string stem = entry.path().stem().string();
		if (stem.starts_with("worn_")) continue; // baked per surface set, not a type
		out.push_back(AssetInfo{.name = stem,
								.file = entry.path().filename().string(),
								.bytes = entry.file_size(ec)});
	}
	std::ranges::sort(out, {}, &AssetInfo::name);
	return out;
}

std::vector<std::string> InstalledModels() {
	return ScanStems(paths::Asset("models"), [](std::string& stem) {
		// worn_<texture>_<tier> meshes are baked per SURFACE SET, not authored
		// types — they are never what a catalog's `model` field names.
		return !stem.starts_with("worn_");
	});
}

std::vector<std::string> InstalledUiStones() {
	std::vector<std::string> out;
	std::error_code ec;
	// Not recursive: thumbs/ beside the tiles holds a same-named copy of each.
	for (const auto& entry :
		 std::filesystem::directory_iterator(paths::Asset("ui\\stones"), ec))
		if (entry.is_regular_file() && entry.path().extension() == ".png")
			out.push_back(entry.path().stem().string());
	std::ranges::sort(out);
	return out;
}

std::vector<std::string> InstalledFonts() {
	std::vector<std::string> out;
	std::error_code ec;
	const std::filesystem::path root = paths::Asset("fonts");
	// Recursive, because assets/fonts holds one directory per family.
	for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
		if (ec || !entry.is_regular_file()) continue;
		std::string ext = entry.path().extension().string();
		std::ranges::transform(ext, ext.begin(),
							   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (ext != ".ttf" && ext != ".otf") continue;
		// Relative to assets/, forward slashes — what fonts.cat's `file` takes,
		// so a listed entry can be handed straight back as a face.
		std::string rel =
			std::filesystem::relative(entry.path(), root.parent_path(), ec).string();
		std::ranges::replace(rel, '\\', '/');
		out.push_back(std::move(rel));
	}
	std::ranges::sort(out);
	return out;
}

} // namespace dungeon::game
