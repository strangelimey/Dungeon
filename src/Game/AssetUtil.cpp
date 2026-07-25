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

std::vector<std::string> InstalledModels() {
	return ScanStems(paths::Asset("models"), [](std::string& stem) {
		// worn_<texture>_<tier> meshes are baked per SURFACE SET, not authored
		// types — they are never what a catalog's `model` field names.
		return !stem.starts_with("worn_");
	});
}

} // namespace dungeon::game
