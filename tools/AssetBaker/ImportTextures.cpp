// ============================================================================
// ImportTextures.cpp — packs downloaded PBR sets into the engine format.
//
// Download sites ship one image per map (albedo, normal, displacement, AO,
// roughness); the engine wants two packed RGBA8 files (albedo, normal+height
// in alpha). This importer bridges the two:
//   * discovers maps by the common naming conventions (ambientCG "_Color" /
//     "_NormalGL", Poly Haven "_diff" / "_nor_gl" / "_disp", generic
//     "albedo" / "normal" / "height"...)
//   * multiplies AO into the albedo (the shader has no separate AO slot)
//   * flips the normal green channel when the source is OpenGL-flavored
//   * resamples height/AO bilinearly when their resolution differs
//   * warns about found-but-unused maps (roughness) and missing height
//     (parallax becomes flat until a displacement map is provided)
// ============================================================================
#include "ImportTextures.h"

#include "Assets/Image.h"
#include "Core/Log.h"
#include "Core/Types.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <initializer_list>
#include <vector>

namespace dungeon::baker {

namespace {

std::string Lower(std::string s) {
	std::ranges::transform(s, s.begin(),
						   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

bool ContainsAny(const std::string& haystack, std::initializer_list<const char*> needles) {
	for (const char* needle : needles)
		if (haystack.find(needle) != std::string::npos) return true;
	return false;
}

// Bilinear sample of one channel, with the source possibly a different size.
float SampleChannel(const assets::ImageData& image, u32 channel, float u, float v) {
	const float x = u * (image.width - 1), y = v * (image.height - 1);
	const u32 x0 = static_cast<u32>(x), y0 = static_cast<u32>(y);
	const u32 x1 = std::min(x0 + 1, image.width - 1), y1 = std::min(y0 + 1, image.height - 1);
	const float fx = x - x0, fy = y - y0;
	auto at = [&](u32 px, u32 py) {
		return static_cast<float>(
			image.pixels[(static_cast<size_t>(py) * image.width + px) * 4 + channel]);
	};
	const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
	const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
	return top + (bottom - top) * fy;
}

bool SavePng(const std::string& path, const assets::ImageData& image) {
	const int ok = stbi_write_png(path.c_str(), static_cast<int>(image.width),
								  static_cast<int>(image.height), 4,
								  image.pixels.data(),
								  static_cast<int>(image.width) * 4);
	if (ok) log::Info("Wrote {}", path);
	else log::Error("Failed to write {}", path);
	return ok != 0;
}

// What we managed to find in the source folder.
struct FoundMaps {
	std::string albedo, normal, height, ao, roughness;
	bool normalLooksGl = false;
};

FoundMaps DiscoverMaps(const std::string& sourceDir) {
	FoundMaps found;
	std::vector<std::filesystem::path> files;
	for (const auto& entry : std::filesystem::directory_iterator(sourceDir))
		if (entry.is_regular_file()) files.push_back(entry.path());
	std::ranges::sort(files); // deterministic pick when several match

	for (const auto& file : files) {
		const std::string ext = Lower(file.extension().string());
		if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".tga" &&
			ext != ".bmp")
			continue;
		const std::string stem = Lower(file.stem().string());
		const std::string path = file.string();

		// Order matters: "ambientocclusion" must not be mistaken for albedo.
		if (found.ao.empty() &&
			ContainsAny(stem, {"ambientocclusion", "ambient_occlusion", "_ao", "occ"})) {
			found.ao = path;
		} else if (found.normal.empty() && ContainsAny(stem, {"normal", "_nor"})) {
			found.normal = path;
			found.normalLooksGl = ContainsAny(stem, {"gl"});
		} else if (found.height.empty() &&
				   ContainsAny(stem, {"height", "displacement", "_disp", "bump"})) {
			found.height = path;
		} else if (found.roughness.empty() && ContainsAny(stem, {"rough"})) {
			found.roughness = path;
		} else if (found.albedo.empty() &&
				   ContainsAny(stem, {"albedo", "basecolor", "base_color", "diffuse",
									  "color", "_col", "_diff"})) {
			found.albedo = path;
		}
	}
	return found;
}

} // namespace

bool ImportPbrTextureSet(const std::string& sourceDir, const std::string& texturesDir,
						 const std::string& outputName, bool forceFlipGreen) {
	if (!std::filesystem::is_directory(sourceDir)) {
		log::Error("Import source is not a directory: {}", sourceDir);
		return false;
	}
	const FoundMaps found = DiscoverMaps(sourceDir);
	if (found.albedo.empty()) {
		log::Error("No albedo/basecolor map found in {}", sourceDir);
		return false;
	}
	if (found.normal.empty())
		log::Warn("No normal map found — a flat normal will be generated");
	if (found.height.empty())
		log::Warn("No height/displacement map found — parallax will be flat");
	if (!found.roughness.empty())
		log::Warn("Roughness map found but not used yet (specular is per-material): {}",
				  found.roughness);

	// --- albedo, with AO baked in ------------------------------------------
	auto albedo = assets::LoadImageFile(found.albedo);
	if (!albedo) {
		log::Error("{}", albedo.error());
		return false;
	}
	log::Info("Albedo: {} ({}x{})", found.albedo, albedo->width, albedo->height);

	if (!found.ao.empty()) {
		auto ao = assets::LoadImageFile(found.ao);
		if (ao) {
			log::Info("AO: {} (multiplied into albedo)", found.ao);
			for (u32 y = 0; y < albedo->height; ++y) {
				for (u32 x = 0; x < albedo->width; ++x) {
					const float u = static_cast<float>(x) / (albedo->width - 1);
					const float v = static_cast<float>(y) / (albedo->height - 1);
					const float occlusion = SampleChannel(*ao, 0, u, v) / 255.0f;
					const size_t i = (static_cast<size_t>(y) * albedo->width + x) * 4;
					for (int c = 0; c < 3; ++c)
						albedo->pixels[i + c] =
							static_cast<u8>(albedo->pixels[i + c] * occlusion);
				}
			}
		} else {
			log::Warn("Could not load AO map: {}", ao.error());
		}
	}

	// --- normal + height ---------------------------------------------------------
	assets::ImageData normal;
	bool flipGreen = forceFlipGreen;
	if (!found.normal.empty()) {
		auto loaded = assets::LoadImageFile(found.normal);
		if (!loaded) {
			log::Error("{}", loaded.error());
			return false;
		}
		normal = std::move(*loaded);
		if (found.normalLooksGl && !forceFlipGreen) {
			flipGreen = true;
			log::Info("Normal: {} (OpenGL convention detected — flipping green)",
					  found.normal);
		} else {
			log::Info("Normal: {}{}", found.normal,
					  flipGreen ? " (green flip forced)" : "");
		}
	} else {
		// Flat normal at the albedo's resolution.
		normal.width = albedo->width;
		normal.height = albedo->height;
		normal.pixels.assign(static_cast<size_t>(normal.width) * normal.height * 4, 0);
		for (size_t i = 0; i < normal.pixels.size(); i += 4) {
			normal.pixels[i + 0] = 128;
			normal.pixels[i + 1] = 128;
			normal.pixels[i + 2] = 255;
		}
	}
	if (flipGreen)
		for (size_t i = 1; i < normal.pixels.size(); i += 4)
			normal.pixels[i] = static_cast<u8>(255 - normal.pixels[i]);

	// Height into the alpha channel (resampled to the normal map's size).
	if (!found.height.empty()) {
		auto height = assets::LoadImageFile(found.height);
		if (height) {
			log::Info("Height: {} (packed into normal alpha)", found.height);
			for (u32 y = 0; y < normal.height; ++y) {
				for (u32 x = 0; x < normal.width; ++x) {
					const float u = static_cast<float>(x) / (normal.width - 1);
					const float v = static_cast<float>(y) / (normal.height - 1);
					normal.pixels[(static_cast<size_t>(y) * normal.width + x) * 4 + 3] =
						static_cast<u8>(SampleChannel(*height, 0, u, v));
				}
			}
		} else {
			log::Warn("Could not load height map: {}", height.error());
		}
	} else {
		for (size_t i = 3; i < normal.pixels.size(); i += 4) normal.pixels[i] = 0;
	}

	// --- write the packed pair ------------------------------------------------
	std::error_code ec;
	std::filesystem::create_directories(texturesDir, ec);
	bool ok = SavePng(texturesDir + "\\" + outputName + ".png", *albedo);
	ok &= SavePng(texturesDir + "\\" + outputName + "_n.png", normal);
	if (ok)
		log::Info("Imported '{}' — reference it from the game's texture sets or "
				  "overwrite an existing name to replace a procedural material.",
				  outputName);
	return ok;
}

} // namespace dungeon::baker
