#pragma once

#include "Assets/Model.h"
#include "Graphics/D3DUtil.h"

namespace dungeon::gfx {

class GraphicsDevice;

// GPU mesh: immutable vertex/index buffers on the default heap.
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
