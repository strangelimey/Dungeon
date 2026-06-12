// AssetBaker — two modes:
//
//   AssetBaker <assets-dir>
//       Regenerates every procedural asset the game ships (textures, sounds,
//       models, title art).
//
//   AssetBaker import <source-folder> <assets-dir> <output-name> [--flip-green]
//       Packs a downloaded PBR texture set (Poly Haven, ambientCG, Megascans,
//       ...) into the engine format: <name>.png (albedo with AO baked in) and
//       <name>_n.png (normal RGB + height in alpha). Maps are found by
//       filename convention; OpenGL-style normals are flipped automatically
//       when detectable, or forced with --flip-green.

#include "Core/Log.h"
#include "ImportTextures.h"
#include "ModelBaker.h"
#include "SoundBaker.h"
#include "TextureBaker.h"
#include "TitleBaker.h"

#include <filesystem>
#include <string>

int main(int argc, char** argv) {
	using namespace dungeon;

	if (argc >= 2 && std::string(argv[1]) == "import") {
		if (argc < 5) {
			log::Error("usage: AssetBaker import <source-folder> <assets-dir> "
					   "<output-name> [--flip-green]");
			return 1;
		}
		const bool flipGreen = argc >= 6 && std::string(argv[5]) == "--flip-green";
		const std::string texturesDir = std::string(argv[3]) + "\\textures";
		return baker::ImportPbrTextureSet(argv[2], texturesDir, argv[4], flipGreen)
				   ? 0
				   : 1;
	}

	if (argc < 2) {
		log::Error("usage: AssetBaker <assets-dir>  |  AssetBaker import ...");
		return 1;
	}
	const std::string assets = argv[1];
	std::error_code ec;
	std::filesystem::create_directories(assets + "\\textures", ec);
	std::filesystem::create_directories(assets + "\\sounds", ec);
	std::filesystem::create_directories(assets + "\\models", ec);

	bool ok = true;
	ok &= baker::BakeTextures(assets + "\\textures");
	ok &= baker::BakeTitleImage(assets + "\\textures");
	ok &= baker::BakeSounds(assets + "\\sounds");
	ok &= baker::BakeModels(assets + "\\models");
	if (ok) log::Info("Asset bake complete.");
	else log::Error("Asset bake FAILED.");
	return ok ? 0 : 1;
}
