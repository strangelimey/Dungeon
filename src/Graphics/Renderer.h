#pragma once

#include "Core/MathTypes.h"
#include "Graphics/Camera.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/Lights.h"
#include "Graphics/Mesh.h"
#include "Graphics/Texture.h"
#include "Graphics/UploadAllocator.h"

#include <memory>
#include <span>

namespace dungeon::gfx {

inline constexpr u32 kMaxSkinJoints = 128;

// Forward 3D pass: one pipeline, per-frame light constants, optional texture
// and optional GPU skinning per draw.
class Renderer {
public:
    explicit Renderer(GraphicsDevice& device);

    // Writes the per-frame constants and binds the scene pipeline.
    void BeginScene(ID3D12GraphicsCommandList* list, const Camera& camera,
                    const LightSet& lights);

    // Draws a mesh. `texture` may be null (uses baseColor only); `palette` is
    // empty for static meshes or the skinning palette for skinned ones.
    // `normalMap` (xyz = tangent-space normal, w = height) enables bump
    // mapping, and `heightScale` > 0 adds parallax displacement.
    void DrawMesh(ID3D12GraphicsCommandList* list, const Mesh& mesh, const Mat4& world,
                  const Texture* texture, const Vec4& baseColor,
                  std::span<const Mat4> palette = {},
                  const Texture* normalMap = nullptr, float heightScale = 0.0f);

    // Call when the device frame index advances (resets that frame's allocator).
    void NewFrame(u32 frameIndex);

private:
    GraphicsDevice& m_device;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pso;
    std::unique_ptr<UploadAllocator> m_frameAllocators[kFrameCount];
    std::unique_ptr<Texture> m_whiteTexture;
    std::unique_ptr<Texture> m_flatNormalMap;
    u32 m_frameIndex = 0;
};

} // namespace dungeon::gfx
