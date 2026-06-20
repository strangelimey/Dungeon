// ============================================================================
// Game/AssetUtil.cpp — see AssetUtil.h.
// ============================================================================
#include "Game/AssetUtil.h"

#include "Assets/Dds.h"
#include "Assets/Image.h"
#include "Core/Assert.h"
#include "Core/Log.h"
#include "Core/Paths.h"

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

} // namespace dungeon::game
