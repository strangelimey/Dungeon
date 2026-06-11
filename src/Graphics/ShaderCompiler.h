// ============================================================================
// Graphics/ShaderCompiler.h — runtime HLSL compilation (FXC, shader model 5.1).
//
// Shaders compile from assets/shaders/*.hlsl at startup, so iterating on
// them needs no C++ rebuild — edit and relaunch. Compile errors are logged
// verbatim from the compiler, then assert.
// ============================================================================
#pragma once

#include "Graphics/D3DUtil.h"

#include <string>

namespace dungeon::gfx {
ComPtr<ID3DBlob> CompileShader(const std::string& path, const char* entry,
                               const char* target);

} // namespace dungeon::gfx
