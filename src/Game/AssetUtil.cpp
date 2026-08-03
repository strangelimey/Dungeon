// ============================================================================
// Game/AssetUtil.cpp — see AssetUtil.h.
// ============================================================================
#include "Game/AssetUtil.h"

#include "Assets/Dds.h"
#include "Assets/Image.h"
#include "Core/Assert.h"
#include "Core/Log.h"
#include "Core/Paths.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <functional>

namespace dungeon::game {

assets::ModelData LoadModelOrDie(const std::string& name) {
	auto model = assets::LoadModel(paths::Asset("models\\" + name));
	DN_ASSERT(model.has_value(), model.error() + " — run AssetBaker over assets/");
	return std::move(*model);
}

assets::SoundData LoadSound(const std::string& name) {
	auto sound = assets::LoadWavFile(paths::Asset("sounds\\" + name));
	if (!sound) log::Warn("{} (running silent)", sound.error());
	return std::move(sound).value_or(assets::SoundData{});
}

std::unique_ptr<gfx::Texture> TryLoadTextureFile(gfx::GraphicsDevice& device,
												 const std::string& stemPath, bool srgb) {
	if (auto mips = assets::LoadDdsFile(stemPath + ".dds"))
		return std::make_unique<gfx::Texture>(device, *mips, srgb);
	if (auto image = assets::LoadImageFile(stemPath + ".png"))
		return std::make_unique<gfx::Texture>(device, *image, srgb);
	return nullptr;
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

// Whether a set has worn block meshes baked from it — i.e. whether it can be
// painted as a surface at all. WHICH kind they were baked as is deliberately
// not answered here: the bake writes one worn_<set>_<tier>.gltf per set and the
// kind lives in the geometry, not the name. The project's catalogs are the
// honest source for that (walls.cat/floors.cat/ceilings.cat name their sets),
// and the picker gets it from its owner along with "in use".
bool HasWornMeshes(const std::filesystem::path& modelsDir, const std::string& set) {
	std::error_code ec;
	return std::filesystem::exists(modelsDir / ("worn_" + set + "_med.gltf"), ec);
}
} // namespace

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
