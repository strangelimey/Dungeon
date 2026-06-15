// ============================================================================
// ModelImport.cpp — brings authored/bought models into the engine's
// conventions (see ModelImport.h). The runtime loader (assets::LoadModel) reads
// .gltf/.glb/.obj; this tool re-emits a single, normalized, grounded glTF that
// drops onto a cell floor like the procedural props, and imports the model's
// PBR maps as a same-named texture set.
// ============================================================================
#include "ModelImport.h"

#include "Assets/Model.h"
#include "Core/Log.h"
#include "Core/MathTypes.h"
#include "GltfWriter.h"
#include "ImportTextures.h"
#include "MipBaker.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>

using namespace DirectX;

namespace dungeon::baker {

namespace {

namespace fs = std::filesystem;

// Finds the first model file in a folder (or returns the path unchanged if it
// already points at a file).
std::string ResolveModelFile(const std::string& sourcePath) {
	if (fs::is_regular_file(sourcePath)) return sourcePath;
	if (fs::is_directory(sourcePath)) {
		std::vector<fs::path> files;
		for (const auto& e : fs::directory_iterator(sourcePath))
			if (e.is_regular_file()) files.push_back(e.path());
		std::ranges::sort(files);
		for (const auto& f : files) {
			std::string ext = f.extension().string();
			std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			if (ext == ".gltf" || ext == ".glb" || ext == ".obj") return f.string();
		}
	}
	return {};
}

} // namespace

bool ImportModel(const std::string& sourcePath, const std::string& assetsDir,
				 const std::string& name, float targetHeight, float yawDegrees,
				 char upAxis) {
	const std::string modelFile = ResolveModelFile(sourcePath);
	if (modelFile.empty()) {
		log::Error("No .gltf/.glb/.obj model found at {}", sourcePath);
		return false;
	}

	auto loaded = assets::LoadModel(modelFile);
	if (!loaded) {
		log::Error("{}", loaded.error());
		return false;
	}
	const assets::ModelData& src = *loaded;

	// --- merge every mesh/primitive into one, baking node transforms ---------
	assets::MeshData merged;
	for (const assets::MeshData& m : src.meshes) {
		const XMMATRIX node = XMLoadFloat4x4(&m.worldTransform);
		const u32 base = static_cast<u32>(merged.vertices.size());
		for (const assets::Vertex& v : m.vertices) {
			assets::Vertex out = v;
			out.joints[0] = out.joints[1] = out.joints[2] = out.joints[3] = 0;
			out.weights[0] = out.weights[1] = out.weights[2] = out.weights[3] = 0;
			const XMVECTOR p = XMVector3Transform(
				XMVectorSet(v.position.x, v.position.y, v.position.z, 1.0f), node);
			const XMVECTOR n = XMVector3Normalize(XMVector3TransformNormal(
				XMVectorSet(v.normal.x, v.normal.y, v.normal.z, 0.0f), node));
			XMFLOAT3 pf, nf;
			XMStoreFloat3(&pf, p);
			XMStoreFloat3(&nf, n);
			out.position = {pf.x, pf.y, pf.z};
			out.normal = {nf.x, nf.y, nf.z};
			merged.vertices.push_back(out);
		}
		for (u32 i : m.indices) merged.indices.push_back(base + i);
	}
	if (merged.vertices.empty()) {
		log::Error("Model has no geometry: {}", modelFile);
		return false;
	}

	// --- orient: optional Z-up -> Y-up, then yaw about Y ----------------------
	XMMATRIX orient = XMMatrixIdentity();
	if (upAxis == 'z' || upAxis == 'Z')
		orient = XMMatrixRotationX(-XM_PIDIV2); // Z-up source to the engine's Y-up
	if (yawDegrees != 0.0f)
		orient = orient * XMMatrixRotationY(yawDegrees * (XM_PI / 180.0f));
	for (assets::Vertex& v : merged.vertices) {
		const XMVECTOR p = XMVector3Transform(
			XMVectorSet(v.position.x, v.position.y, v.position.z, 1.0f), orient);
		const XMVECTOR n = XMVector3Normalize(XMVector3TransformNormal(
			XMVectorSet(v.normal.x, v.normal.y, v.normal.z, 0.0f), orient));
		XMFLOAT3 pf, nf;
		XMStoreFloat3(&pf, p);
		XMStoreFloat3(&nf, n);
		v.position = {pf.x, pf.y, pf.z};
		v.normal = {nf.x, nf.y, nf.z};
	}

	// --- normalize scale + ground (min y = 0) + center XZ --------------------
	Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
	for (const assets::Vertex& v : merged.vertices) {
		lo = {std::min(lo.x, v.position.x), std::min(lo.y, v.position.y),
			  std::min(lo.z, v.position.z)};
		hi = {std::max(hi.x, v.position.x), std::max(hi.y, v.position.y),
			  std::max(hi.z, v.position.z)};
	}
	const float ext[3] = {hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
	const float maxExt = std::max({ext[0], ext[1], ext[2], 1e-4f});
	// targetHeight scales the model's height to that value; auto-fit puts the
	// largest extent at ~2.0 m (comfortably inside a 2.4 m cell).
	const float scale = targetHeight > 0.0f ? targetHeight / std::max(ext[1], 1e-4f)
											: 2.0f / maxExt;
	const float cx = (lo.x + hi.x) * 0.5f, cz = (lo.z + hi.z) * 0.5f;
	for (assets::Vertex& v : merged.vertices) {
		v.position = {(v.position.x - cx) * scale, (v.position.y - lo.y) * scale,
					  (v.position.z - cz) * scale};
	}
	log::Info("Imported model '{}': {} verts, source extent {:.2f}x{:.2f}x{:.2f} m, "
			  "scaled x{:.3f}",
			  name, merged.vertices.size(), ext[0], ext[1], ext[2], scale);

	// --- write the normalized single-mesh glTF (white material; the texture
	// set named after the model carries its color) ---------------------------
	merged.material = 0;
	assets::ModelData out;
	out.meshes.push_back(std::move(merged));
	out.materials.push_back({{1, 1, 1, 1}, -1});
	const std::string modelsDir = assetsDir + "\\models";
	std::error_code ec;
	fs::create_directories(modelsDir, ec);
	if (!WriteGltf(out, modelsDir + "\\" + name + ".gltf")) return false;

	// --- import the source folder's PBR maps as the texture set <name>_2k -----
	const std::string folder =
		fs::is_directory(sourcePath) ? sourcePath : fs::path(modelFile).parent_path().string();
	const std::string texturesDir = assetsDir + "\\textures";
	if (ImportPbrTextureSet(folder, texturesDir, name + "_2k", false)) {
		bool ok = BakeMipChain(texturesDir + "\\" + name + "_2k.png",
							   texturesDir + "\\" + name + "_2k.dds");
		ok &= BakeMipChain(texturesDir + "\\" + name + "_2k_n.png",
						   texturesDir + "\\" + name + "_2k_n.dds");
		ok &= BakeMipChain(texturesDir + "\\" + name + "_2k_mr.png",
						   texturesDir + "\\" + name + "_2k_mr.dds");
		if (!ok) log::Warn("Model textures imported but a mip bake failed");
	} else {
		log::Warn("No PBR maps imported for '{}' — it will draw with a flat color",
				  name);
	}
	return true;
}

} // namespace dungeon::baker
