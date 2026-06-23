// ============================================================================
// ImportTextures.cpp — packs downloaded PBR sets into the engine format.
//
// Download sites ship one image per map (albedo, normal, displacement, AO,
// roughness, opacity); the engine wants three packed RGBA8 files (albedo with
// any opacity mask in its alpha, normal+height in alpha, ORM). This importer
// bridges the two:
//   * discovers maps by the common naming conventions (ambientCG "_Color" /
//     "_NormalGL", Poly Haven "_diff" / "_nor_gl" / "_disp", generic
//     "albedo" / "normal" / "height" / "opacity"...)
//   * packs a separate opacity/mask map into the albedo's alpha channel
//   * flips the normal green channel when the source is OpenGL-flavored
//   * resamples height/AO bilinearly when their resolution differs
//   * warns about found-but-unused maps (roughness) and missing height
//     (parallax becomes flat until a displacement map is provided)
// ============================================================================
#include "ImportTextures.h"

#include "Assets/Image.h"
#include "Core/Log.h"
#include "Core/Types.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <initializer_list>
#include <optional>
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

// Height (displacement) maps need special handling: download sites ship them
// as 16-bit grayscale PNGs whose useful range may occupy a tiny slice of
// [0,1] — an 8-bit load can flatten such a map to a constant (Poly Haven's
// medieval_blocks_03 does exactly that). Load at full depth and normalize the
// actual min..max range to 0..1; absolute displacement scale is applied
// downstream anyway (heightScale for parallax, the relief amplitudes in
// ModelBaker for the worn block meshes).
struct HeightMap {
	u32 width = 0, height = 0;
	std::vector<float> values; // normalized 0..1
};

std::optional<HeightMap> LoadHeightMap(const std::string& path) {
	int w = 0, h = 0, comp = 0;
	stbi_us* data = stbi_load_16(path.c_str(), &w, &h, &comp, 1);
	if (!data) return std::nullopt;

	const size_t count = static_cast<size_t>(w) * h;
	const auto [minIt, maxIt] = std::minmax_element(data, data + count);
	const float lo = *minIt;
	const float range = static_cast<float>(*maxIt) - lo;
	if (range < 656.0f) { // < 1% of 16-bit: a flat export, no usable relief
		stbi_image_free(data);
		log::Warn("Height map is (near-)constant, treating as absent: {}", path);
		return std::nullopt;
	}

	HeightMap map;
	map.width = static_cast<u32>(w);
	map.height = static_cast<u32>(h);
	map.values.resize(count);
	for (size_t i = 0; i < count; ++i) map.values[i] = (data[i] - lo) / range;
	stbi_image_free(data);
	return map;
}

// Bilinear sample of a normalized height map.
float SampleHeight(const HeightMap& map, float u, float v) {
	const float x = u * (map.width - 1), y = v * (map.height - 1);
	const u32 x0 = static_cast<u32>(x), y0 = static_cast<u32>(y);
	const u32 x1 = std::min(x0 + 1, map.width - 1), y1 = std::min(y0 + 1, map.height - 1);
	const float fx = x - x0, fy = y - y0;
	auto at = [&](u32 px, u32 py) {
		return map.values[static_cast<size_t>(py) * map.width + px];
	};
	const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
	const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
	return top + (bottom - top) * fy;
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
	std::string albedo, normal, height, ao, roughness, metallic, opacity;
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
		} else if (found.metallic.empty() &&
				   ContainsAny(stem, {"metallic", "metalness", "metal", "_met"})) {
			found.metallic = path;
		} else if (found.albedo.empty() &&
				   ContainsAny(stem, {"albedo", "basecolor", "base_color", "diffuse",
									  "color", "_col", "_diff"})) {
			found.albedo = path;
		} else if (found.opacity.empty() &&
				   ContainsAny(stem, {"opacity", "alpha", "opac", "transp", "mask"})) {
			// Checked after albedo so a combined "basecolor_alpha" name binds as
			// the albedo, not stolen here as the mask.
			found.opacity = path;
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

	// --- albedo (pure base color; AO now lives in the ORM map) --------------
	auto albedo = assets::LoadImageFile(found.albedo);
	if (!albedo) {
		log::Error("{}", albedo.error());
		return false;
	}
	log::Info("Albedo: {} ({}x{})", found.albedo, albedo->width, albedo->height);

	// --- opacity → albedo alpha ---------------------------------------------
	// A separate opacity/mask map (some textures.com sets ship one — e.g. wood
	// planks with cut-out gaps between boards) packs into the albedo's alpha,
	// the same trick as height-into-normal-alpha. The shader multiplies it into
	// albedo.a (gBaseTexture), ready for an alpha-test clip or a blend pass.
	// Resampled in case the mask differs in resolution. Absent → the alpha is
	// left as the source basecolor's own (255 for an opaque download).
	if (!found.opacity.empty()) {
		if (auto opacity = assets::LoadImageFile(found.opacity)) {
			log::Info("Opacity: {} (packed into albedo alpha)", found.opacity);
			for (u32 y = 0; y < albedo->height; ++y) {
				for (u32 x = 0; x < albedo->width; ++x) {
					const float u = static_cast<float>(x) / (albedo->width - 1);
					const float v = static_cast<float>(y) / (albedo->height - 1);
					albedo->pixels[(static_cast<size_t>(y) * albedo->width + x) * 4 + 3] =
						static_cast<u8>(SampleChannel(*opacity, 0, u, v) + 0.5f);
				}
			}
		} else {
			log::Warn("Opacity map failed to load, albedo left opaque: {}", opacity.error());
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

	// Height into the alpha channel, normalized to the map's real range and
	// resampled to the normal map's size (see LoadHeightMap).
	if (!found.height.empty()) {
		auto height = LoadHeightMap(found.height);
		if (height) {
			log::Info("Height: {} (normalized, packed into normal alpha)", found.height);
			for (u32 y = 0; y < normal.height; ++y) {
				for (u32 x = 0; x < normal.width; ++x) {
					const float u = static_cast<float>(x) / (normal.width - 1);
					const float v = static_cast<float>(y) / (normal.height - 1);
					normal.pixels[(static_cast<size_t>(y) * normal.width + x) * 4 + 3] =
						static_cast<u8>(SampleHeight(*height, u, v) * 255.0f + 0.5f);
				}
			}
		} else {
			// LoadHeightMap already logged why; render the material flat.
			log::Warn("No usable height data — parallax will be flat");
			for (size_t i = 3; i < normal.pixels.size(); i += 4) normal.pixels[i] = 255;
		}
	} else {
		// 255 = fully proud surface: the parallax march exits immediately, so
		// the material renders flat (0 would smear the whole tile sideways).
		for (size_t i = 3; i < normal.pixels.size(); i += 4) normal.pixels[i] = 255;
	}

	// --- ORM map: R = occlusion, G = roughness, B = metallic (glTF order) -----
	// Built at the albedo's resolution; each source channel is resampled and
	// defaults to a neutral value when its map is absent (AO 1, rough 1, metal 0).
	assets::ImageData orm;
	orm.width = albedo->width;
	orm.height = albedo->height;
	orm.pixels.assign(static_cast<size_t>(orm.width) * orm.height * 4, 255);
	auto loadOpt = [](const std::string& p) -> std::optional<assets::ImageData> {
		if (p.empty()) return std::nullopt;
		if (auto img = assets::LoadImageFile(p)) return std::move(*img);
		return std::nullopt;
	};
	const auto ao = loadOpt(found.ao);
	const auto rough = loadOpt(found.roughness);
	const auto metal = loadOpt(found.metallic);
	if (ao) log::Info("Occlusion: {} (ORM.r)", found.ao);
	if (rough) log::Info("Roughness: {} (ORM.g)", found.roughness);
	if (metal) log::Info("Metallic: {} (ORM.b)", found.metallic);
	for (u32 y = 0; y < orm.height; ++y) {
		for (u32 x = 0; x < orm.width; ++x) {
			const float u = static_cast<float>(x) / (orm.width - 1);
			const float v = static_cast<float>(y) / (orm.height - 1);
			const size_t i = (static_cast<size_t>(y) * orm.width + x) * 4;
			orm.pixels[i + 0] = ao ? static_cast<u8>(SampleChannel(*ao, 0, u, v)) : 255;
			orm.pixels[i + 1] = rough ? static_cast<u8>(SampleChannel(*rough, 0, u, v)) : 255;
			orm.pixels[i + 2] = metal ? static_cast<u8>(SampleChannel(*metal, 0, u, v)) : 0;
			orm.pixels[i + 3] = 255;
		}
	}

	// --- write the packed set (albedo / normal+height / ORM) ------------------
	std::error_code ec;
	std::filesystem::create_directories(texturesDir, ec);
	bool ok = SavePng(texturesDir + "\\" + outputName + ".png", *albedo);
	ok &= SavePng(texturesDir + "\\" + outputName + "_n.png", normal);
	ok &= SavePng(texturesDir + "\\" + outputName + "_mr.png", orm);
	if (ok)
		log::Info("Imported '{}' — reference it from the game's texture sets or "
				  "overwrite an existing name to replace a procedural material.",
				  outputName);
	return ok;
}

} // namespace dungeon::baker
