// ============================================================================
// MipBaker.cpp — moves mip generation AND block compression to bake time.
//
// The game's Texture class can build mip chains at load, but box-filtering a
// 2K texture's pyramid on the CPU is the slowest part of startup. This tool
// does that filtering once per texture, BC7-encodes every level (quarter the
// VRAM and bandwidth of RGBA8 — see Bc7Encoder.cpp), and stores the chain as
// a DX10-header DDS; the game then loads it with a single read
// (Assets/Dds.cpp) and uploads straight to a BC7 resource.
//
// A chain is averaged in the colour space the game SAMPLES it in - linear light
// for an sRGB albedo, the stored values for data - and rounded (code-review
// C414), and a file is skipped only when assets::BakedIsCurrent says so, the
// same rule the game's loaders refuse a stale one by (C410).
// ============================================================================
#include "MipBaker.h"

#include "Assets/File.h"
#include "Assets/Image.h"
#include "Assets/Model.h"
#include "Bc7Encoder.h"
#include "Core/Log.h"
#include "Core/Types.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <format>
#include <vector>

namespace dungeon::baker {

namespace {

using assets::Downsample; // the shared 2x2 box filter (Assets/Image.h) — the
						  // runtime fallback in Graphics/Texture uses the SAME
						  // one, so a chain is identical whichever built it

bool WriteDdsBc7(const std::string& path, u32 width, u32 height,
				 const std::vector<std::vector<u8>>& levels, std::string* why) {
	// Standard DDS: magic + 124-byte header + 20-byte DX10 extension.
	u32 header[31]{};
	header[0] = 124;                                // dwSize
	header[1] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000; // CAPS|HEIGHT|WIDTH|PIXELFORMAT|MIPMAPCOUNT
	header[2] = height;
	header[3] = width;
	header[6] = static_cast<u32>(levels.size());    // mip count
	header[18] = 32;                                // ddspf.dwSize
	header[19] = 0x4;                               // FOURCC
	header[20] = 0x30315844;                        // "DX10"
	header[27] = 0x8 | 0x1000 | 0x400000;           // COMPLEX | TEXTURE | MIPMAP

	const u32 dx10[5] = {98 /*DXGI_FORMAT_BC7_UNORM*/, 3 /*TEXTURE2D*/, 0, 1, 0};

	std::vector<u8> file(4 + sizeof(header) + sizeof(dx10));
	const u32 magic = 0x20534444; // "DDS "
	std::memcpy(file.data(), &magic, 4);
	std::memcpy(file.data() + 4, header, sizeof(header));
	std::memcpy(file.data() + 4 + sizeof(header), dx10, sizeof(dx10));
	for (const auto& level : levels)
		file.insert(file.end(), level.begin(), level.end());

	return assets::WriteBinaryFile(path, file.data(), file.size(), why);
}

// Deletes the sidecars named the OLD way, "<model file>.<n>.dds" with n the
// image's FIRST-USE order (code-review C396 renamed them "<model file>.img<n>
// .dds", n the file's own index). Nothing reads them any more, and for most
// shipped models the two orders differ, so an old file is another image's
// chain: removed rather than left to be mistaken for a current one. Only a
// name of exactly that shape beside a model in this folder is touched.
int RemoveLegacySidecars(const std::string& modelsDir) {
	// Gathered first: removing entries mid-walk leaves the walk unspecified.
	std::vector<std::filesystem::path> old;
	for (const auto& entry : std::filesystem::directory_iterator(modelsDir)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".dds") continue;
		const std::filesystem::path stem = entry.path().stem(); // "<model file>.<n>"
		const std::string digits = stem.extension().string();   // ".<n>"
		if (digits.size() < 2 ||
			!std::all_of(digits.begin() + 1, digits.end(),
						 [](unsigned char c) { return std::isdigit(c) != 0; }))
			continue;
		const std::filesystem::path model = entry.path().parent_path() / stem.stem();
		const std::string modelExt = model.extension().string();
		if (modelExt != ".gltf" && modelExt != ".glb") continue;
		old.push_back(entry.path());
	}
	int removed = 0;
	for (const std::filesystem::path& path : old) {
		std::error_code ec;
		if (std::filesystem::remove(path, ec)) {
			log::Info("Removed old-style sidecar {}", path.string());
			++removed;
		} else {
			log::Warn("Could not remove old-style sidecar {}: {}", path.string(), ec.message());
		}
	}
	return removed;
}

} // namespace

// The chain for one decoded image: box-filtered levels (in linear light for an
// sRGB image), each BC7-encoded. `source` names it in the log.
static bool BakeImageChain(assets::ImageData image, bool srgb, const std::string& source,
						   const std::string& ddsPath) {
	if (image.width % 4 != 0 || image.height % 4 != 0) {
		// D3D12 requires BC top-level dimensions to be multiples of 4.
		log::Warn("{}: {}x{} is not block-aligned — skipped (PNG fallback applies)",
				  source, image.width, image.height);
		return true;
	}

	const u32 width = image.width, height = image.height;
	std::vector<std::vector<u8>> levels;
	assets::ImageData level = std::move(image);
	while (true) {
		levels.push_back(EncodeBc7(level));
		if (level.width == 1 && level.height == 1) break;
		level = Downsample(level, srgb);
	}

	std::string why;
	if (!WriteDdsBc7(ddsPath, width, height, levels, &why)) {
		log::Error("Cannot write the mip chain: {}", why); // why names it (C416)
		return false;
	}
	log::Info("Wrote {} ({} BC7 mips)", ddsPath, levels.size());
	return true;
}

bool BakeMipChain(const std::string& pngPath, const std::string& ddsPath, bool srgb) {
	auto image = assets::LoadImageFile(pngPath);
	if (!image) {
		log::Error("{}", image.error());
		return false;
	}
	return BakeImageChain(std::move(*image), srgb, pngPath, ddsPath);
}

bool IsTextureSetAlbedo(const std::string& pngPath) {
	const std::string stem = std::filesystem::path(pngPath).stem().string();
	return !stem.ends_with("_n") && !stem.ends_with("_mr");
}

bool BakeModelImageMips(const std::string& modelsDir, bool force) {
	bool ok = true;
	int models = 0, written = 0, fresh = 0;
	for (const auto& entry : std::filesystem::directory_iterator(modelsDir)) {
		const std::string ext = entry.path().extension().string();
		if (!entry.is_regular_file() || (ext != ".gltf" && ext != ".glb")) continue;
		const std::string path = entry.path().string();
		// Loaded as the GAME loads it, with bakedImages, unless forced: an image
		// whose sidecar is current (the loader's own test, assets::BakedIsCurrent)
		// arrives as that chain and is never decoded - a 2k decode was paid for
		// every image of every model before, current or not, only to be thrown
		// away (code-review C410). Either way each image carries its index in the
		// file (imageSources), which is what a sidecar is named by. warnUnbaked off: the
		// images it would report are the ones about to be baked.
		const assets::LoadOptions opts{.bakedImages = !force, .warnUnbaked = false};
		auto model = assets::LoadModel(path, opts);
		if (!model) {
			log::Error("{}", model.error());
			ok = false;
			continue;
		}
		if (model->images.empty()) continue;
		++models;
		const std::vector<bool> srgb = assets::SrgbImages(*model);
		for (size_t i = 0; i < model->images.size(); ++i) {
			if (i < model->imageMips.size() && !model->imageMips[i].levels.empty()) {
				++fresh; // its sidecar is current
				continue;
			}
			if (model->images[i].pixels.empty()) continue; // nothing decoded to bake
			// Named by the image's index in the FILE, as the loader looks it up
			// (code-review C396) - not `i`, its place among the images loaded.
			const u32 source = model->imageSources[i];
			ok &= BakeImageChain(std::move(model->images[i]), srgb[i],
								 std::format("{} image {}", path, source),
								 assets::EmbeddedImageSidecar(path, source));
			++written;
		}
	}
	const int legacy = RemoveLegacySidecars(modelsDir);
	log::Info("Model image bake: {} models with embedded images, {} images baked, "
			  "{} already current, {} old-style sidecars removed",
			  models, written, fresh, legacy);
	return ok;
}

bool BakeAllMips(const std::string& texturesDir, MipColor color, bool skipCurrent,
				 const std::string& prefix) {
	bool ok = true;
	int count = 0, fresh = 0;
	for (const auto& entry : std::filesystem::directory_iterator(texturesDir)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".png") continue;
		if (!entry.path().filename().string().starts_with(prefix)) continue;
		const std::string png = entry.path().string();
		std::filesystem::path dds = entry.path();
		dds.replace_extension(".dds");
		if (skipCurrent && assets::BakedIsCurrent(dds.string(), png)) {
			++fresh;
			continue;
		}
		const bool srgb = color == MipColor::TextureSets && IsTextureSetAlbedo(png);
		ok &= BakeMipChain(png, dds.string(), srgb);
		++count;
	}
	log::Info("Mip bake: {} textures processed, {} already current", count, fresh);
	return ok;
}

} // namespace dungeon::baker
