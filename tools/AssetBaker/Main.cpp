// AssetBaker: regenerates every binary asset the game ships. Usage:
//   AssetBaker.exe <path-to-assets-dir>

#include "Core/Log.h"
#include "ModelBaker.h"
#include "SoundBaker.h"
#include "TextureBaker.h"
#include "TitleBaker.h"

#include <filesystem>
#include <string>

int main(int argc, char** argv) {
	using namespace dungeon;
	if (argc < 2) {
		log::Error("usage: AssetBaker <assets-dir>");
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
