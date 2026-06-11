// ============================================================================
// Graphics/Mesh.h — immutable GPU mesh.
//
// Uploads an assets::MeshData's vertex/index arrays to default-heap buffers
// at construction (blocking; load-time only). The single engine-wide vertex
// layout (position/normal/uv/joints/weights) is declared in the Renderer's
// input layout — change assets::Vertex and that layout together.
// ============================================================================
#pragma once

#include "Assets/Model.h"
#include "Graphics/D3DUtil.h"

namespace dungeon::gfx {

class GraphicsDevice;
class Mesh {
public:
    Mesh(GraphicsDevice& device, const assets::MeshData& data);

    void Bind(ID3D12GraphicsCommandList* list) const;
    u32 IndexCount() const { return m_indexCount; }

private:
    ComPtr<ID3D12Resource> m_vertexBuffer;
    ComPtr<ID3D12Resource> m_indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vbv{};
    D3D12_INDEX_BUFFER_VIEW m_ibv{};
    u32 m_indexCount = 0;
};

} // namespace dungeon::gfx
