#include "Assets/File.h"

#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>

namespace dungeon::assets {

namespace {

// The handle is owned from the moment it opens, so a throw anywhere after it -
// sizing the vector can - still closes the file (code-review C231).
struct FileCloser {
	void operator()(std::FILE* f) const { std::fclose(f); }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

// `path` is UTF-8, and the narrow fopen reads it as UTF-8 because every exe
// declares that its code page (src/Core/Utf8CodePage.manifest).
FilePtr Open(const std::string& path, const char* mode) {
	std::FILE* raw = nullptr;
	if (fopen_s(&raw, path.c_str(), mode) != 0) return nullptr;
	return FilePtr(raw);
}

} // namespace

std::expected<std::vector<u8>, std::string> ReadBinaryFile(const std::string& path) {
	const FilePtr f = Open(path, "rb");
	if (!f) {
		// A directory does not open on Windows; say so, since "could not open"
		// reads as a missing file.
		std::error_code ec;
		if (std::filesystem::is_directory(path, ec))
			return std::unexpected(std::format("not a file, a directory: {}", path));
		return std::unexpected(std::format("could not open file: {}", path));
	}
	// 64-bit and CHECKED. ftell's `long` is 32 bits here, so a file past 2 GB -
	// or anything that cannot seek, a pipe or a device - came back as -1, which
	// became a SIZE_MAX vector and a length_error with the handle left open.
	if (_fseeki64(f.get(), 0, SEEK_END) != 0)
		return std::unexpected(std::format("cannot seek in file: {}", path));
	const long long size = _ftelli64(f.get());
	if (size < 0 || _fseeki64(f.get(), 0, SEEK_SET) != 0)
		return std::unexpected(std::format("cannot size file: {}", path));
	std::vector<u8> data(static_cast<size_t>(size));
	if (size > 0 && std::fread(data.data(), 1, data.size(), f.get()) != data.size())
		return std::unexpected(std::format("short read on file: {}", path));
	return data;
}

bool WriteBinaryFile(const std::string& path, const void* data, size_t size) {
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

	FilePtr f = Open(path, "wb");
	if (!f) return false;
	const bool wrote = std::fwrite(data, 1, size, f.get()) == size;
	// Closed by hand to read the result: buffered bytes are written HERE, so a
	// full disk shows at the close, not at the fwrite. Nothing between the two
	// can throw, so taking the handle back out of its owner is safe.
	return std::fclose(f.release()) == 0 && wrote;
}

} // namespace dungeon::assets
