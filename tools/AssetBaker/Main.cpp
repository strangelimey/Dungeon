// AssetBaker — two modes:
//
//   AssetBaker <assets-dir>
//       Regenerates every procedural asset the game ships (textures, sounds,
//       models, rune tablet). UI images (hit splats, title art, rune icons)
//       are NOT baked - they're committed source under assets/ui/. Nor are the
//       party portraits: a bought set in assets/portraits (see portrait-mips).
//
//   AssetBaker import <source-folder> <assets-dir> <output-name>
//                     [--flip-green | --no-flip-green]
//       Packs a downloaded PBR texture set (Poly Haven, ambientCG, Megascans,
//       ...) into the engine format: <name>.png (albedo with AO baked in) and
//       <name>_n.png (normal RGB + height in alpha). Maps are found by
//       filename convention. An OpenGL-style normal map (green up) is flipped
//       when its name ends in the GL token (Assets/PbrMaps.h), and
//       --flip-green / --no-flip-green decide it outright either way - the
//       editor always sends one (code-review C393).
//
//   AssetBaker mips <assets-dir> [<prefix>] [--force]
//       Regenerates the derived .dds mip chains (gitignored) for every PNG in
//       assets/textures, so the game never filters mips at load time - and the
//       same for every image EMBEDDED in a model in assets/models
//       (<model>.img<index>.dds beside it), so it never decodes those either, and
//       for the portraits. A <prefix> bakes only the texture-set PNGs named
//       <prefix>... and nothing else. The texture sets always re-bake; the
//       sidecars and portraits skip a CURRENT file unless --force (after a
//       filter or encoder change, which no timestamp can see).
//
//   AssetBaker model-images <assets-dir> [--force]
//       Only the embedded-image sidecars; current ones are skipped. Run after
//       importing or re-converting a model (the game warns, once per model, on
//       a missing or stale one) - FetchModels and FetchAnimLibrary end with it.
//
//   AssetBaker portrait-mips <assets-dir> [--force]
//       The .dds chains for assets/portraits (the party portrait set,
//       extracted by tools\FetchPortraits.ps1); current ones are skipped.
//       `mips` covers them too.
//
//   AssetBaker models <assets-dir> [--out <models-dir>]
//       Regenerates only the .gltf models (fast — skips the texture, sound,
//       and mip bakes). The worn blocks sample the installed texture height
//       maps, so re-run this after FetchTextures.ps1 or an import. --out writes
//       the models somewhere else (still reading <assets-dir>\textures), which
//       is how tools\WornBakeTest.py compares two bakes without touching the
//       tree.
//
//   AssetBaker wornblock <wall|floor|ceiling> <set> <assets-dir> [--wear <0..1>]
//                        [--relief <metres>] [--out <models-dir>]
//       The worn block meshes of ONE set, at its record (Assets/WornSets.h).
//       With neither --wear nor --relief it writes exactly what `models` does.
//
//   AssetBaker wornsets
//       Prints that record for every shipped set, one `<set> <kind> <relief>
//       <seed>` line each - what tools\WornBakeTest.py walks.
//
//   AssetBaker rescale <assets-dir> <factor> <name> [name...]
//       Uniformly rescales already-imported single-mesh models in place. The
//       fix-up when a model landed at the wrong size — notably the one-time
//       metres-to-units migration (see docs/authoring-scale.md).
//
//   AssetBaker runes <assets-dir>
//       Regenerates the rune tablet model + carved per-element texture sets,
//       and their .dds chains (as `import` does). It used to write the PNGs
//       only, and the game drew the old .dds beside them (code-review C410).
//
//   AssetBaker rig-names <out.gltf> <names-file>
//       A FIXTURE, not an asset: one joint per line of <names-file> (UTF-8; a
//       chain, and a clip of the same name for each), written through the one
//       glTF writer. Every name a bake writes is the baker's own, so this is
//       how tools\BakerWriteTest.py hands the writer quotes, backslashes and
//       control characters to escape (code-review C416).
//
// Every write goes through assets::WriteBinaryFile (code-review C416): a file
// that cannot be written - read-only, locked, a full disk - is an error line
// naming it and saying why, the bake carries on with the rest, and the exit
// code is 1.

#include "Assets/File.h"
#include "Assets/Model.h"
#include "Assets/WornSets.h"
#include "Core/Log.h"
#include "GltfWriter.h"
#include "ImportTextures.h"
#include "MipBaker.h"
#include "ModelBaker.h"
#include "ModelImport.h"
#include "RuneBaker.h"
#include "SoundBaker.h"
#include "TextureBaker.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

// `rig-names`: the joint chain and clips named by the lines of a file (see the
// header). A triangle skinned to the root makes it a model the writer takes.
bool WriteNamedRig(const std::string& out, const std::string& namesFile) {
	using namespace dungeon;
	const auto text = assets::ReadBinaryFile(namesFile);
	if (!text) {
		log::Error("rig-names: {}", text.error());
		return false;
	}
	std::vector<std::string> names;
	std::string line;
	for (const u8 b : *text) {
		if (b != '\n') {
			line.push_back(static_cast<char>(b));
			continue;
		}
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (!line.empty()) names.push_back(line);
		line.clear();
	}
	if (!line.empty() && line.back() == '\r') line.pop_back();
	if (!line.empty()) names.push_back(line);
	if (names.empty()) {
		log::Error("rig-names: {} names nothing", namesFile);
		return false;
	}

	assets::ModelData model;
	for (size_t i = 0; i < names.size(); ++i) {
		assets::JointData joint;
		joint.name = names[i];
		joint.parent = static_cast<int>(i) - 1;
		joint.restTranslation = {0.0f, i ? 0.1f : 0.0f, 0.0f};
		model.skeleton.joints.push_back(joint);

		assets::AnimationClipData clip;
		clip.name = names[i];
		clip.duration = 1.0f;
		assets::ChannelKeys keys;
		keys.joint = static_cast<int>(i);
		keys.path = assets::ChannelPath::Rotation;
		keys.times = {0.0f, 1.0f};
		keys.values = {{0, 0, 0, 1}, {0, 0.7071068f, 0, 0.7071068f}};
		clip.Add(keys);
		model.clips.push_back(std::move(clip));
	}
	assets::MeshData mesh;
	mesh.skinned = true;
	mesh.material = 0;
	for (const Vec3 p : {Vec3{0, 0, 0}, Vec3{0.1f, 0, 0}, Vec3{0, 0.1f, 0}}) {
		assets::Vertex v;
		v.position = p;
		v.normal = {0, 0, 1};
		v.weights[0] = 1.0f;
		mesh.vertices.push_back(v);
	}
	mesh.indices = {0, 1, 2};
	model.meshes.push_back(std::move(mesh));
	model.materials.push_back({});
	return baker::WriteGltf(model, out);
}

} // namespace

int main(int argc, char** argv) {
	using namespace dungeon;

	// Log lines are UTF-8; a console decodes in its own code page unless told
	// (Core/Log.h). The baker's output is full of em-dashes and asset names.
	log::UseUtf8Console();

	if (argc >= 2 && std::string(argv[1]) == "import") {
		const auto usage = [] {
			log::Error("usage: AssetBaker import <source-folder> <assets-dir> "
					   "<output-name> [--flip-green | --no-flip-green]");
			return 1;
		};
		if (argc < 5) return usage();
		// Tri-state: asked on, asked off, or (neither) by the normal map's name.
		// An unknown word or both flags is refused, not read as "neither".
		std::optional<bool> flipGreen;
		for (int i = 5; i < argc; ++i) {
			const std::string flag = argv[i];
			const std::optional<bool> asked = flag == "--flip-green"      ? std::optional(true)
											  : flag == "--no-flip-green" ? std::optional(false)
																		  : std::nullopt;
			if (!asked || (flipGreen && *flipGreen != *asked)) return usage();
			flipGreen = asked;
		}
		const std::string texturesDir = std::string(argv[3]) + "\\textures";
		const std::string name = argv[4];
		if (!baker::ImportPbrTextureSet(argv[2], texturesDir, name, flipGreen)) return 1;
		// Bake mip chains for the freshly imported set right away (albedo,
		// normal+height, and the ORM occlusion/roughness/metallic map). Only the
		// albedo is sampled as sRGB.
		bool ok = baker::BakeMipChain(texturesDir + "\\" + name + ".png",
									  texturesDir + "\\" + name + ".dds", /*srgb*/ true);
		ok &= baker::BakeMipChain(texturesDir + "\\" + name + "_n.png",
								  texturesDir + "\\" + name + "_n.dds", /*srgb*/ false);
		ok &= baker::BakeMipChain(texturesDir + "\\" + name + "_mr.png",
								  texturesDir + "\\" + name + "_mr.dds", /*srgb*/ false);
		return ok ? 0 : 1;
	}

	if (argc >= 2 && std::string(argv[1]) == "import-model") {
		if (argc < 5) {
			log::Error("usage: AssetBaker import-model <model-file|folder> "
					   "<assets-dir> <name> [--height M] [--yaw deg] [--up y|z] "
					   "[--texture-set name] [--lift M] [--wall] [--raw]");
			return 1;
		}
		float height = 0.0f, yaw = 0.0f, lift = 0.0f;
		char up = 'y';
		bool wall = false, raw = false;
		std::string textureSet;
		for (int i = 5; i < argc; ++i) {
			const std::string a = argv[i];
			if (a == "--height" && i + 1 < argc) height = std::stof(argv[++i]);
			else if (a == "--yaw" && i + 1 < argc) yaw = std::stof(argv[++i]);
			else if (a == "--up" && i + 1 < argc) up = argv[++i][0];
			else if (a == "--texture-set" && i + 1 < argc) textureSet = argv[++i];
			else if (a == "--lift" && i + 1 < argc) lift = std::stof(argv[++i]);
			else if (a == "--wall") wall = true;
			else if (a == "--raw") raw = true;
		}
		return baker::ImportModel(argv[2], argv[3], argv[4], height, yaw, up,
								  textureSet, lift, wall, raw)
				   ? 0
				   : 1;
	}

	if (argc >= 2 && std::string(argv[1]) == "rescale") {
		if (argc < 5) {
			log::Error("usage: AssetBaker rescale <assets-dir> <factor> <name> [name...]");
			return 1;
		}
		const float factor = std::stof(argv[3]);
		bool ok = true;
		for (int i = 4; i < argc; ++i) ok &= baker::RescaleModel(argv[2], argv[i], factor);
		return ok ? 0 : 1;
	}

	// The three mip commands share their tail: an optional <prefix> word (mips
	// only) and --force, which re-bakes a file its timestamp calls current - the
	// way a filter or encoder change reaches the sidecars and the portraits.
	const auto mipArgs = [&](std::string& prefix, bool& force) {
		for (int i = 3; i < argc; ++i) {
			const std::string a = argv[i];
			if (a == "--force") force = true;
			else if (a.starts_with("--")) log::Warn("{}: unknown flag '{}' ignored", argv[1], a);
			else prefix = a;
		}
	};

	if (argc >= 3 && std::string(argv[1]) == "mips") {
		// Texture sets AND the images embedded in bought models - both are BC7
		// chains the game loads instead of decoding PNGs. `mips <assets>
		// <prefix>` bakes only the texture sets named <prefix>... and leaves the
		// models and portraits alone.
		std::string prefix;
		bool force = false;
		mipArgs(prefix, force);
		const std::string assets = argv[2];
		if (!prefix.empty())
			return baker::BakeAllMips(assets + "\\textures", baker::MipColor::TextureSets, false,
									  prefix)
					   ? 0
					   : 1;
		bool ok = baker::BakeAllMips(assets + "\\textures", baker::MipColor::TextureSets);
		ok &= baker::BakeModelImageMips(assets + "\\models", force);
		if (std::filesystem::is_directory(assets + "\\portraits"))
			ok &= baker::BakeAllMips(assets + "\\portraits", baker::MipColor::Linear, !force);
		return ok ? 0 : 1;
	}

	if (argc >= 3 && std::string(argv[1]) == "portrait-mips") {
		std::string prefix;
		bool force = false;
		mipArgs(prefix, force);
		return baker::BakeAllMips(std::string(argv[2]) + "\\portraits", baker::MipColor::Linear,
								  !force, prefix)
				   ? 0
				   : 1;
	}

	if (argc >= 3 && std::string(argv[1]) == "model-images") {
		// Only the embedded-image sidecars (fast when nothing changed: current
		// sidecars are skipped). Run after importing or re-converting a model.
		std::string prefix;
		bool force = false;
		mipArgs(prefix, force);
		if (!prefix.empty()) log::Warn("model-images takes no prefix; '{}' ignored", prefix);
		return baker::BakeModelImageMips(std::string(argv[2]) + "\\models", force) ? 0 : 1;
	}

	if (argc >= 3 && std::string(argv[1]) == "models") {
		const std::string assets = argv[2];
		std::string out = assets + "\\models";
		for (int i = 3; i + 1 < argc; i += 2)
			if (std::string(argv[i]) == "--out") out = argv[i + 1];
		std::error_code ec;
		std::filesystem::create_directories(out, ec);
		if (baker::BakeModels(out, assets + "\\textures")) return 0;
		log::Error("Model bake FAILED - a file above could not be written or baked.");
		return 1;
	}

	if (argc >= 4 && std::string(argv[1]) == "rig-names")
		return WriteNamedRig(argv[2], argv[3]) ? 0 : 1;

	if (argc >= 2 && std::string(argv[1]) == "wornsets") {
		// Plain stdout, not the log: this is read by a script.
		for (const assets::WornSet& set : assets::ShippedWornSets()) {
			const std::string_view kind = assets::WornKindName(set.kind);
			std::printf("%.*s %.*s %.4f %u\n", static_cast<int>(set.texture.size()),
						set.texture.data(), static_cast<int>(kind.size()), kind.data(),
						static_cast<double>(set.relief), static_cast<unsigned>(set.seed));
		}
		return 0;
	}

	if (argc >= 2 && std::string(argv[1]) == "wornblock") {
		if (argc < 5) {
			log::Error("usage: AssetBaker wornblock <wall|floor|ceiling> <name> "
					   "<assets-dir> [--wear <0..1>] [--relief <metres>] "
					   "[--out <models-dir>]");
			return 1;
		}
		// Optional surface-look flags (the editor's type dialog): relief is the
		// displacement amplitude in metres and wear scales it (0 = flat). Relief
		// unset (-1) keeps the set's own (Assets/WornSets.h). (`--columns`
		// retired 2026-08-05 with the wall pillars; pillars are decorations now.)
		const std::string assets = argv[4];
		std::string out = assets + "\\models";
		float wear = 1.0f;
		float relief = -1.0f;
		for (int i = 5; i + 1 < argc; i += 2) {
			const std::string flag = argv[i];
			if (flag == "--wear")
				wear = std::strtof(argv[i + 1], nullptr);
			else if (flag == "--relief")
				relief = std::strtof(argv[i + 1], nullptr);
			else if (flag == "--out")
				out = argv[i + 1];
			else
				log::Warn("wornblock: unknown flag '{}' ignored", flag);
		}
		return baker::BakeWornBlocks(argv[2], argv[3], out, assets + "\\textures", wear,
									 relief)
				   ? 0
				   : 1;
	}

	if (argc >= 3 && std::string(argv[1]) == "runes") {
		// Tablet model + carved per-element texture sets, then their .dds chains,
		// as `import` does. It used to stop at the PNGs and leave `mips <assets>
		// rune_` to the user - and the game, which loads a .dds over its PNG,
		// went on drawing the old tablets until it was run (code-review C410; the
		// loader now refuses a .dds older than its PNG, and says so).
		const std::string assets = argv[2];
		bool ok = baker::BakeRunes(assets);
		ok &= baker::BakeAllMips(assets + "\\textures", baker::MipColor::TextureSets, false,
								 "rune_");
		return ok ? 0 : 1;
	}

	if (argc >= 3 && std::string(argv[1]) == "sounds") {
		// Synthesized WAVs only (no mip/texture work) - fast. WriteBinaryFile makes
		// the folders.
		if (baker::BakeSounds(std::string(argv[2]) + "\\sounds")) return 0;
		log::Error("Sound bake FAILED - a file above could not be written.");
		return 1;
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
	ok &= baker::BakeSounds(assets + "\\sounds");
	ok &= baker::BakeModels(assets + "\\models", assets + "\\textures");
	ok &= baker::BakeRunes(assets); // tablet + carved per-element texture sets
	// Every texture set's chain, the runes' included (BakeRunes writes PNGs; the
	// `runes` command adds the chain pass this one already is).
	ok &= baker::BakeAllMips(assets + "\\textures", baker::MipColor::TextureSets);
	ok &= baker::BakeModelImageMips(assets + "\\models");
	if (ok) log::Info("Asset bake complete.");
	else log::Error("Asset bake FAILED.");
	return ok ? 0 : 1;
}
