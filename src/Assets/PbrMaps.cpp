// ============================================================================
// Assets/PbrMaps.cpp — see PbrMaps.h. Moved out of AssetBaker's ImportTextures
// so the editor's asset dialog reports exactly what the baker will import.
// ============================================================================
#include "Assets/PbrMaps.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <initializer_list>
#include <vector>

namespace dungeon::assets {

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

// Unreal-style exports abbreviate the map kind to a single-letter suffix
// (T_Torch_R / T_Torch_M). Those are too short to match as substrings — a
// stem like "wood_render" contains "_r" — so they anchor to the stem's end.
bool EndsWithAny(const std::string& haystack, std::initializer_list<const char*> needles) {
	for (const char* needle : needles)
		if (haystack.ends_with(needle)) return true;
	return false;
}

} // namespace

PbrMapSet DiscoverPbrMaps(const std::string& sourceDir) {
	PbrMapSet found;
	std::error_code ec;
	std::vector<std::filesystem::path> files;
	for (const auto& entry : std::filesystem::directory_iterator(sourceDir, ec))
		if (!ec && entry.is_regular_file()) files.push_back(entry.path());
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
		} else if (found.normal.empty() &&
				   (ContainsAny(stem, {"normal", "_nor", "_nrm"}) ||
					EndsWithAny(stem, {"_n"}))) {
			found.normal = path;
			found.normalLooksGl = ContainsAny(stem, {"gl"});
		} else if (found.height.empty() &&
				   ContainsAny(stem, {"height", "displacement", "_disp", "bump"})) {
			found.height = path;
		} else if (found.roughness.empty() &&
				   (ContainsAny(stem, {"rough"}) || EndsWithAny(stem, {"_r"}))) {
			found.roughness = path;
		} else if (found.metallic.empty() &&
				   (ContainsAny(stem, {"metallic", "metalness", "metal", "_met"}) ||
					EndsWithAny(stem, {"_m"}))) {
			found.metallic = path;
		} else if (found.albedo.empty() &&
				   ContainsAny(stem, {"albedo", "basecolor", "base_color", "diffuse",
									  "color", "_col", "_diff", "_alb"})) {
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

} // namespace dungeon::assets
