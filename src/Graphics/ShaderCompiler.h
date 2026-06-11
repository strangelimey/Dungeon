#pragma once

#include "Graphics/D3DUtil.h"

#include <string>

namespace dungeon::gfx {

// Compiles an HLSL entry point from a file with FXC (shader model 5.x).
// Asserts on compile errors after logging the compiler output.
ComPtr<ID3DBlob> CompileShader(const std::string& path, const char* entry,
                               const char* target);

} // namespace dungeon::gfx
