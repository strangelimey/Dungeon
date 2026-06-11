#include "Graphics/Mesh.h"

#include "Graphics/GraphicsDevice.h"

#include <cstring>

namespace dungeon::gfx {

namespace {

// Creates a default-heap buffer filled from `data` via a transient upload buffer.
ComPtr<ID3D12Resource> CreateFilledBuffer(GraphicsDevice& device, const void* data,
                                          u64 size, D3D12_RESOURCE_STATES finalState) {
    ComPtr<ID3D12Resource> result;
    ComPtr<ID3D12Resource> staging;

    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC desc = BufferDesc(size);

    DN_HR(device.Device()->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&result)));
    DN_HR(device.Device()->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&staging)));

    void* mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    DN_HR(staging->Map(0, &noRead, &mapped));
    std::memcpy(mapped, data, size);
    staging->Unmap(0, nullptr);

    device.ExecuteImmediate([&](ID3D12GraphicsCommandList* list) {
        list->CopyBufferRegion(result.Get(), 0, staging.Get(), 0, size);
        const auto barrier =
            Transition(result.Get(), D3D12_RESOURCE_STATE_COPY_DEST, finalState);
        list->ResourceBarrier(1, &barrier);
    });
    return result;
}

} // namespace

Mesh::Mesh(GraphicsDevice& device, const assets::MeshData& data)
    : m_indexCount(static_cast<u32>(data.indices.size())) {
    const u64 vbSize = data.vertices.size() * sizeof(assets::Vertex);
    const u64 ibSize = data.indices.size() * sizeof(u32);

    m_vertexBuffer = CreateFilledBuffer(
        device, data.vertices.data(), vbSize,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    m_indexBuffer = CreateFilledBuffer(device, data.indices.data(), ibSize,
                                       D3D12_RESOURCE_STATE_INDEX_BUFFER);

    m_vbv.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vbv.SizeInBytes = static_cast<UINT>(vbSize);
    m_vbv.StrideInBytes = sizeof(assets::Vertex);

    m_ibv.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
    m_ibv.SizeInBytes = static_cast<UINT>(ibSize);
    m_ibv.Format = DXGI_FORMAT_R32_UINT;
}

void Mesh::Bind(ID3D12GraphicsCommandList* list) const {
    list->IASetVertexBuffers(0, 1, &m_vbv);
    list->IASetIndexBuffer(&m_ibv);
}

} // namespace dungeon::gfx
