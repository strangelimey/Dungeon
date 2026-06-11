#include "Graphics/UploadAllocator.h"

namespace dungeon::gfx {

UploadAllocator::UploadAllocator(ID3D12Device* device, u64 capacity)
    : m_capacity(capacity) {
    const D3D12_HEAP_PROPERTIES heap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC desc = BufferDesc(capacity);
    DN_HR(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(&m_buffer)));
    const D3D12_RANGE noRead{0, 0};
    DN_HR(m_buffer->Map(0, &noRead, reinterpret_cast<void**>(&m_mapped)));
}

UploadAllocation UploadAllocator::Allocate(u64 size, u64 alignment) {
    const u64 aligned = (m_offset + alignment - 1) & ~(alignment - 1);
    DN_ASSERT(aligned + size <= m_capacity, "UploadAllocator exhausted for this frame");
    m_offset = aligned + size;
    return {m_mapped + aligned, m_buffer->GetGPUVirtualAddress() + aligned};
}

} // namespace dungeon::gfx
