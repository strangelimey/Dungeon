#include "Assets/Model.h"

#include "Core/Log.h"

#include <cstdio>
#include <format>
#include <sstream>
#include <string>

namespace dungeon::assets {

// Minimal Wavefront OBJ loader: v / vn / vt / f (triangles or fans).
// Materials are ignored; the result is a single mesh with the default material.
std::expected<ModelData, std::string> LoadObj(const std::string& path) {
	std::FILE* f = nullptr;
	if (fopen_s(&f, path.c_str(), "r") != 0 || !f)
		return std::unexpected(std::format("could not open OBJ: {}", path));

	std::vector<Vec3> positions;
	std::vector<Vec3> normals;
	std::vector<Vec2> uvs;
	MeshData mesh;

	auto addVertex = [&](const std::string& spec) {
		int pi = 0, ti = 0, ni = 0;
		// Formats: v, v/t, v//n, v/t/n (1-based, negatives = relative).
		if (sscanf_s(spec.c_str(), "%d/%d/%d", &pi, &ti, &ni) < 1) {
			if (sscanf_s(spec.c_str(), "%d//%d", &pi, &ni) < 1) return u32(0);
		}
		auto resolve = [](int idx, size_t count) -> int {
			if (idx > 0) return idx - 1;
			if (idx < 0) return static_cast<int>(count) + idx;
			return -1;
		};
		Vertex v;
		const int p = resolve(pi, positions.size());
		if (p >= 0 && p < static_cast<int>(positions.size())) v.position = positions[p];
		const int t = resolve(ti, uvs.size());
		if (t >= 0 && t < static_cast<int>(uvs.size())) v.uv = uvs[t];
		const int n = resolve(ni, normals.size());
		if (n >= 0 && n < static_cast<int>(normals.size())) v.normal = normals[n];
		mesh.vertices.push_back(v);
		return static_cast<u32>(mesh.vertices.size() - 1);
	};

	char line[1024];
	while (std::fgets(line, sizeof(line), f)) {
		std::istringstream ss(line);
		std::string tag;
		ss >> tag;
		if (tag == "v") {
			Vec3 p{};
			ss >> p.x >> p.y >> p.z;
			positions.push_back(p);
		} else if (tag == "vn") {
			Vec3 n{};
			ss >> n.x >> n.y >> n.z;
			normals.push_back(n);
		} else if (tag == "vt") {
			Vec2 t{};
			ss >> t.x >> t.y;
			t.y = 1.0f - t.y; // OBJ uses bottom-left origin
			uvs.push_back(t);
		} else if (tag == "f") {
			std::vector<u32> face;
			std::string spec;
			while (ss >> spec) face.push_back(addVertex(spec));
			for (size_t i = 2; i < face.size(); ++i) { // triangle fan
				mesh.indices.push_back(face[0]);
				mesh.indices.push_back(face[i - 1]);
				mesh.indices.push_back(face[i]);
			}
		}
	}
	std::fclose(f);

	if (mesh.vertices.empty())
		return std::unexpected(std::format("OBJ contained no geometry: {}", path));

	ModelData model;
	model.materials.push_back({});
	mesh.material = 0;
	model.meshes.push_back(std::move(mesh));
	log::Info("Loaded OBJ '{}': {} vertices", path, model.meshes[0].vertices.size());
	return model;
}

} // namespace dungeon::assets
