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
//
//   AssetBaker mips <assets-dir>
//       Regenerates the derived .dds mip chains (gitignored) for every PNG in
//       assets/textures, so the game never filters mips at load time.
//
//   AssetBaker models <assets-dir>
//       Regenerates only the .gltf models (fast — skips the texture, sound,
//       and mip bakes). The worn blocks sample the installed texture height
//       maps, so re-run this after FetchTextures.ps1 or an import.
//
//   AssetBaker portraits <assets-dir>
//       Regenerates only the party portraits and their mip chains (fast).
//
//   AssetBaker splats <assets-dir>
//       Regenerates only the hit-feedback splat icons (fast; PNG only).
//
//   AssetBaker runes <assets-dir>
//       Regenerates the rune tablet model + carved per-element texture sets +
//       icons (fast; PNG only — run `mips` after to derive the .dds).

#include "Core/Log.h"
#include "ImportTextures.h"
#include "MipBaker.h"
#include "ModelBaker.h"
#include "ModelImport.h"
#include "PortraitBaker.h"
#include "RuneBaker.h"
#include "SoundBaker.h"
#include "SplatBaker.h"
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
		const std::string name = argv[4];
		if (!baker::ImportPbrTextureSet(argv[2], texturesDir, name, flipGreen)) return 1;
		// Bake mip chains for the freshly imported set right away (albedo,
		// normal+height, and the ORM occlusion/roughness/metallic map).
		bool ok = baker::BakeMipChain(texturesDir + "\\" + name + ".png",
									  texturesDir + "\\" + name + ".dds");
		ok &= baker::BakeMipChain(texturesDir + "\\" + name + "_n.png",
								  texturesDir + "\\" + name + "_n.dds");
		ok &= baker::BakeMipChain(texturesDir + "\\" + name + "_mr.png",
								  texturesDir + "\\" + name + "_mr.dds");
		return ok ? 0 : 1;
	}

	if (argc >= 2 && std::string(argv[1]) == "import-model") {
		if (argc < 5) {
			log::Error("usage: AssetBaker import-model <model-file|folder> "
					   "<assets-dir> <name> [--height M] [--yaw deg] [--up y|z]");
			return 1;
		}
		float height = 0.0f, yaw = 0.0f;
		char up = 'y';
		for (int i = 5; i < argc; ++i) {
			const std::string a = argv[i];
			if (a == "--height" && i + 1 < argc) height = std::stof(argv[++i]);
			else if (a == "--yaw" && i + 1 < argc) yaw = std::stof(argv[++i]);
			else if (a == "--up" && i + 1 < argc) up = argv[++i][0];
		}
		return baker::ImportModel(argv[2], argv[3], argv[4], height, yaw, up) ? 0 : 1;
	}

	if (argc >= 3 && std::string(argv[1]) == "mips")
		return baker::BakeAllMips(std::string(argv[2]) + "\\textures") ? 0 : 1;

	if (argc >= 3 && std::string(argv[1]) == "models") {
		const std::string assets = argv[2];
		return baker::BakeModels(assets + "\\models", assets + "\\textures") ? 0 : 1;
	}

	if (argc >= 2 && std::string(argv[1]) == "wornblock") {
		if (argc < 5) {
			log::Error("usage: AssetBaker wornblock <wall|floor|ceiling> <name> "
					   "<assets-dir>");
			return 1;
		}
		return baker::BakeWornBlocks(argv[2], argv[3], argv[4]) ? 0 : 1;
	}

	if (argc >= 3 && std::string(argv[1]) == "portraits") {
		const std::string texturesDir = std::string(argv[2]) + "\\textures";
		if (!baker::BakePortraits(texturesDir)) return 1;
		bool ok = true;
		for (const char* name : {"portrait_brand", "portrait_sera",
								 "portrait_maren", "portrait_tilo"})
			ok &= baker::BakeMipChain(texturesDir + "\\" + name + ".png",
									  texturesDir + "\\" + name + ".dds");
		return ok ? 0 : 1;
	}

	if (argc >= 3 && std::string(argv[1]) == "splats") {
		// PNG only — no mip bake (see SplatBaker.cpp).
		return baker::BakeHitSplats(std::string(argv[2]) + "\\textures") ? 0 : 1;
	}

	if (argc >= 3 && std::string(argv[1]) == "runes") {
		// Tablet model + carved per-element texture sets + icons. PNG only — the
		// _2k set loads fine without a .dds; run `mips` afterward to derive them.
		return baker::BakeRunes(argv[2]) ? 0 : 1;
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
	ok &= baker::BakePortraits(assets + "\\textures");
	ok &= baker::BakeHitSplats(assets + "\\textures");
	ok &= baker::BakeSounds(assets + "\\sounds");
	ok &= baker::BakeModels(assets + "\\models", assets + "\\textures");
	ok &= baker::BakeRunes(assets); // tablet + carved textures + icons (before mips)
	ok &= baker::BakeAllMips(assets + "\\textures");
	if (ok) log::Info("Asset bake complete.");
	else log::Error("Asset bake FAILED.");
	return ok ? 0 : 1;
}
