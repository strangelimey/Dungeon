// ============================================================================
// ShaderCompiler.cpp — FXC compilation with an on-disk bytecode cache.
//
// Compiling at launch keeps shader iteration rebuild-free, but FXC is slow,
// so compiled DXBC is cached under <exe>\shadercache. A cache entry stores a
// 64-bit FNV-1a hash of (source bytes, entry point, target, compile flags)
// followed by the bytecode; if the stored hash matches the current source,
// the compiler is skipped entirely. Editing a shader, switching entry
// points, or changing debug/release flags all invalidate naturally.
//
// NOTE: the hash covers only the top-level source file. If shaders ever gain
// #include, the cache key must fold in the included files too.
// ============================================================================
#include "Graphics/ShaderCompiler.h"

#include "Assets/File.h"
#include "Core/Log.h"
#include "Core/Paths.h"

#include <d3dcompiler.h>

#include <cstring>
#include <filesystem>
#include <format>
#include <string_view>
#include <vector>

namespace dungeon::gfx {

namespace {

u64 Fnv1a(const void* data, size_t size, u64 hash = 14695981039346656037ull) {
	const auto* bytes = static_cast<const u8*>(data);
	for (size_t i = 0; i < size; ++i) {
		hash ^= bytes[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

UINT CompileFlags() {
	UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
	flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
	return flags;
}

std::string CachePath(const std::string& sourcePath, const char* entry,
					  const char* target) {
	const std::string stem = std::filesystem::path(sourcePath).stem().string();
	return std::format("{}\\shadercache\\{}_{}_{}.dxbc", paths::ExecutableDir(), stem,
					   entry, target);
}

// Cache layout: [u64 hash][DXBC bytes...]
ComPtr<ID3DBlob> TryLoadCache(const std::string& cachePath, u64 expectedHash) {
	auto cached = assets::ReadBinaryFile(cachePath);
	if (!cached || cached->size() <= sizeof(u64)) return nullptr;

	u64 storedHash = 0;
	std::memcpy(&storedHash, cached->data(), sizeof(storedHash));
	if (storedHash != expectedHash) return nullptr; // source or flags changed

	ComPtr<ID3DBlob> blob;
	const size_t codeSize = cached->size() - sizeof(u64);
	if (FAILED(D3DCreateBlob(codeSize, &blob))) return nullptr;
	std::memcpy(blob->GetBufferPointer(), cached->data() + sizeof(u64), codeSize);
	return blob;
}

void StoreCache(const std::string& cachePath, u64 hash, ID3DBlob* code) {
	std::vector<u8> file(sizeof(u64) + code->GetBufferSize());
	std::memcpy(file.data(), &hash, sizeof(hash));
	std::memcpy(file.data() + sizeof(u64), code->GetBufferPointer(),
				code->GetBufferSize());
	if (!assets::WriteBinaryFile(cachePath, file.data(), file.size()))
		log::Warn("Could not write shader cache: {}", cachePath);
}

} // namespace

ComPtr<ID3DBlob> CompileShader(const std::string& path, const char* entry,
							   const char* target) {
	auto source = assets::ReadBinaryFile(path);
	DN_ASSERT(source.has_value(), source.error());

	// Cache key: source content + entry + target + flags.
	const UINT flags = CompileFlags();
	u64 hash = Fnv1a(source->data(), source->size());
	hash = Fnv1a(entry, std::strlen(entry), hash);
	hash = Fnv1a(target, std::strlen(target), hash);
	hash = Fnv1a(&flags, sizeof(flags), hash);

	const std::string cachePath = CachePath(path, entry, target);
	if (ComPtr<ID3DBlob> cached = TryLoadCache(cachePath, hash)) {
		log::Debug("Shader cache hit: {} ({})", path, entry);
		return cached;
	}

	ComPtr<ID3DBlob> code;
	ComPtr<ID3DBlob> errors;
	const HRESULT hr =
		D3DCompile(source->data(), source->size(), path.c_str(), nullptr, nullptr,
				   entry, target, flags, 0, &code, &errors);
	if (errors && errors->GetBufferSize() > 0)
		log::Warn("Shader compiler output for {} ({}):\n{}", path, entry,
				  static_cast<const char*>(errors->GetBufferPointer()));
	DN_ASSERT(SUCCEEDED(hr), "shader compilation failed");

	StoreCache(cachePath, hash, code.Get());
	log::Info("Compiled shader {} ({}) — cache updated", path, entry);
	return code;
}

} // namespace dungeon::gfx
