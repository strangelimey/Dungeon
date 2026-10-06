// ============================================================================
// Assets/ImportFiles.cpp - see ImportFiles.h. Each test PARSES the file name
// against the shape its writer gives it, end to end, rather than matching a
// prefix: a name that merely starts like the import's ("pottery" beside "pot")
// fails at the first character that is not the next piece of the shape.
// ============================================================================
#include "Assets/ImportFiles.h"

#include <algorithm>
#include <initializer_list>

namespace dungeon::assets {

namespace {

// Takes `piece` off the front of `s`: false, with `s` left as it was, when `s`
// does not start with it.
bool Take(std::string_view& s, std::string_view piece) {
	if (!s.starts_with(piece)) return false;
	s.remove_prefix(piece.size());
	return true;
}

// Takes a run of one or more digits off the front of `s`.
bool TakeDigits(std::string_view& s) {
	const auto digit = [](char c) { return c >= '0' && c <= '9'; };
	const size_t n = static_cast<size_t>(std::find_if_not(s.begin(), s.end(), digit) - s.begin());
	if (n == 0) return false;
	s.remove_prefix(n);
	return true;
}

// What follows a texture set's name in one of its files: the albedo, the
// normal+height map (_n) or the ORM map (_mr), each as its source PNG and its
// baked chain.
bool SetFileTail(std::string_view rest) {
	(void)(Take(rest, "_n") || Take(rest, "_mr"));
	return rest == ".png" || rest == ".dds";
}

// worn_<base>_<tier>[_<phase>[l][r]].gltf - the bare name is phase 0 with both
// sides pinned, the only panel a floor, a ceiling or a procedural wall has.
bool WornFile(std::string_view base, std::string_view file) {
	if (!Take(file, "worn_") || !Take(file, base) || !Take(file, "_")) return false;
	if (!Take(file, "low") && !Take(file, "med") && !Take(file, "high")) return false;
	if (Take(file, "_")) {
		if (!TakeDigits(file)) return false;
		Take(file, "l");
		Take(file, "r");
	}
	return file == ".gltf";
}

// <name>.gltf / <name>.glb, or one of its sidecars <name>.gltf.<n>.dds.
bool ModelFile(std::string_view name, std::string_view file) {
	if (!Take(file, name)) return false;
	if (!Take(file, ".gltf") && !Take(file, ".glb")) return false;
	if (file.empty()) return true;
	return Take(file, ".") && TakeDigits(file) && file == ".dds";
}

} // namespace

std::string_view TextureSetBase(std::string_view poolName) {
	for (const std::string_view tag : {"_1k", "_2k", "_4k", "_8k"})
		if (poolName.size() > tag.size() && poolName.ends_with(tag))
			return poolName.substr(0, poolName.size() - tag.size());
	return poolName;
}

bool ImportOwnsFile(std::string_view poolName, bool model, PoolDir dir, std::string_view file) {
	if (poolName.empty() || file.empty()) return false;
	if (dir == PoolDir::Models)
		return model ? ModelFile(poolName, file) : WornFile(TextureSetBase(poolName), file);
	// A model's own maps came in as the texture set <name>_2k (import-model).
	return Take(file, poolName) && (!model || Take(file, "_2k")) && SetFileTail(file);
}

} // namespace dungeon::assets
