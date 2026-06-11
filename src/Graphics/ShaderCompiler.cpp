#include "Graphics/ShaderCompiler.h"

#include "Core/Log.h"
#include "Core/StringUtil.h"

#include <d3dcompiler.h>

namespace dungeon::gfx {

ComPtr<ID3DBlob> CompileShader(const std::string& path, const char* entry,
                               const char* target) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr =
        D3DCompileFromFile(str::Widen(path).c_str(), nullptr,
                           D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target, flags, 0,
                           &code, &errors);
    if (errors && errors->GetBufferSize() > 0)
        log::Warn("Shader compiler output for {} ({}):\n{}", path, entry,
                  static_cast<const char*>(errors->GetBufferPointer()));
    DN_ASSERT(SUCCEEDED(hr), "shader compilation failed");
    return code;
}

} // namespace dungeon::gfx
