#pragma once

#include "Core/Types.h"
#include "Graphics/D3DUtil.h"

namespace dungeon::gfx {

struct UploadAllocation {
    void* cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
};

// Per-frame linear allocator over a persistently mapped upload buffer; used
// for transient constant buffers and dynamic vertex data. Reset each frame.
class UploadAllocator {
public:
    UploadAllocator(ID3D12Device* device, u64 capacity);

    UploadAllocation Allocate(u64 size, u64 alignment = 256);
    void Reset() { m_offset = 0; }

    ID3D12Resource* Resource() const { return m_buffer.Get(); }

private:
    ComPtr<ID3D12Resource> m_buffer;
    u8* m_mapped = nullptr;
    u64 m_capacity = 0;
    u64 m_offset = 0;
};

} // namespace dungeon::gfx
