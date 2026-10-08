#include "Assets/Model.h"

#include "Core/Log.h"

#include <cmath>
#include <cstdio>
#include <format>
#include <memory>
#include <sstream>
#include <string>

namespace dungeon::assets {

// Minimal Wavefront OBJ loader: v / vn / vt / f (triangles or fans).
// Materials are ignored; the result is a single mesh with the default material.
std::expected<ModelData, std::string> LoadObj(const std::string& path) {
	std::FILE* raw = nullptr;
	if (fopen_s(&raw, path.c_str(), "r") != 0 || !raw)
		return std::unexpected(std::format("could not open OBJ: {}", path));
	// The parse loop below allocates; own the handle so an exception still
	// closes the file.
	const std::unique_ptr<std::FILE, decltype(&std::fclose)> f(raw, &std::fclose);

	std::vector<Vec3> positions;
	std::vector<Vec3> normals;
	std::vector<Vec2> uvs;
	MeshData mesh;

	// Whether the vertex just added took a normal from the file.
	bool hadNormal = false;
	auto addVertex = [&](const std::string& spec) {
		int pi = 0, ti = 0, ni = 0;
		hadNormal = false;
		// Formats: v, v/t, v//n, v/t/n (1-based, negatives = relative). `v//n`
		// is tested FIRST: "%d/%d/%d" reads "5//3" as just the 5 (it stops at
		// the second '/') and returns 1, so the old fallback behind it never
		// ran and an OBJ exported with normals but no UVs - a common exporter
		// option - came in with every normal zero (code-review C394).
		if (spec.find("//") != std::string::npos) {
			if (sscanf_s(spec.c_str(), "%d//%d", &pi, &ni) < 1) return u32(0);
		} else if (sscanf_s(spec.c_str(), "%d/%d/%d", &pi, &ti, &ni) < 1) {
			return u32(0);
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
		if (n >= 0 && n < static_cast<int>(normals.size())) {
			v.normal = normals[n];
			hadNormal = v.normal.x != 0.0f || v.normal.y != 0.0f || v.normal.z != 0.0f;
		}
		mesh.vertices.push_back(v);
		return static_cast<u32>(mesh.vertices.size() - 1);
	};

	int flatFaces = 0; // faces given a flat normal for want of the file's
	char line[1024];
	while (std::fgets(line, sizeof(line), f.get())) {
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
			bool allNormals = true;
			while (ss >> spec) {
				face.push_back(addVertex(spec));
				allNormals &= hadNormal;
			}
			// A face the file gives no normal (none written, or an index that
			// resolves to nothing) would light black: it gets its own FLAT
			// normal instead, from its corners (Newell's method, so a polygon
			// that is not quite planar still gets its average facing), and the
			// load says how many it had to make.
			if (!allNormals && face.size() >= 3) {
				Vec3 flat{};
				for (size_t i = 0; i < face.size(); ++i) {
					const Vec3& a = mesh.vertices[face[i]].position;
					const Vec3& b = mesh.vertices[face[(i + 1) % face.size()]].position;
					flat.x += (a.y - b.y) * (a.z + b.z);
					flat.y += (a.z - b.z) * (a.x + b.x);
					flat.z += (a.x - b.x) * (a.y + b.y);
				}
				const float len = std::sqrt(flat.x * flat.x + flat.y * flat.y + flat.z * flat.z);
				if (len > 0.0f) flat = {flat.x / len, flat.y / len, flat.z / len};
				// Every corner is its own vertex here (addVertex never shares),
				// so a corner the file DID give a normal keeps it.
				for (const u32 vi : face) {
					Vec3& n = mesh.vertices[vi].normal;
					if (n.x == 0.0f && n.y == 0.0f && n.z == 0.0f) n = flat;
				}
				++flatFaces;
			}
			for (size_t i = 2; i < face.size(); ++i) { // triangle fan
				mesh.indices.push_back(face[0]);
				mesh.indices.push_back(face[i - 1]);
				mesh.indices.push_back(face[i]);
			}
		}
	}

	if (mesh.vertices.empty())
		return std::unexpected(std::format("OBJ contained no geometry: {}", path));
	if (flatFaces > 0)
		log::Warn("OBJ '{}': {} faces had no normals - given flat ones", path, flatFaces);

	ModelData model;
	model.materials.push_back({});
	mesh.material = 0;
	model.meshes.push_back(std::move(mesh));
	log::Info("Loaded OBJ '{}': {} vertices", path, model.meshes[0].vertices.size());
	return model;
}

} // namespace dungeon::assets
