#include "Assets/File.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
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

// The C runtime's words for an errno ("Permission denied", "No space left on
// device") - what a write's error line ends with.
std::string Reason(int err) {
	char text[128] = {};
	if (err == 0 || strerror_s(text, sizeof text, err) != 0 || !text[0])
		return "no reason given";
	return text;
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

bool WriteBinaryFile(const std::string& path, const void* data, size_t size,
					 std::string* why) {
	const auto fail = [&](std::string reason) {
		if (why) *why = std::move(reason);
		return false;
	};
	// The folder first: an fopen into a folder that could not be made says only
	// "No such file or directory", which hides the real cause.
	std::error_code ec;
	const std::filesystem::path folder = std::filesystem::path(path).parent_path();
	if (!folder.empty() && !std::filesystem::create_directories(folder, ec) && ec)
		return fail(std::format("could not make the folder of {}: {}", path, ec.message()));

	std::FILE* raw = nullptr;
	if (const errno_t err = fopen_s(&raw, path.c_str(), "wb"); err != 0 || !raw)
		return fail(std::format("could not open {} for writing: {}", path, Reason(err)));
	FilePtr f(raw);
	if (std::fwrite(data, 1, size, f.get()) != size)
		return fail(std::format("short write on {}: {}", path, Reason(errno)));
	// Closed by hand to read the result: buffered bytes are written HERE, so a
	// full disk shows at the close, not at the fwrite. Nothing between the two
	// can throw, so taking the handle back out of its owner is safe.
	if (std::fclose(f.release()) != 0)
		return fail(std::format("could not finish writing {} (the closing flush): {}", path,
								Reason(errno)));
	return true;
}

bool BakedIsCurrent(const std::string& baked, const std::string& source) {
	std::error_code ec;
	const auto bakedTime = std::filesystem::last_write_time(baked, ec);
	if (ec) return false; // not baked
	const auto sourceTime = std::filesystem::last_write_time(source, ec);
	if (ec) return true; // no source for it to be older than
	return bakedTime >= sourceTime;
}

} // namespace dungeon::assets
