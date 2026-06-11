// ============================================================================
// Graphics/UploadAllocator.h — per-frame linear (arena) GPU allocator.
//
// The cornerstone of the engine's "no heap traffic per frame" rule: a bump
// pointer over a persistently mapped upload-heap buffer. Allocate() is a few
// instructions; Reset() reclaims everything at frame start. Owners keep one
// allocator per frame-in-flight (kFrameCount), so the GPU can still be
// reading frame N's data while the CPU writes frame N+1's.
//
// SAFETY: an allocation is valid for one frame only — never cache the
// returned pointers. Default alignment is 256 (D3D12's CBV requirement).
// ============================================================================
#pragma once

#include "Core/Types.h"
#include "Graphics/D3DUtil.h"

namespace dungeon::gfx {

struct UploadAllocation {
	void* cpu = nullptr;                   // write your data here...
	D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;     // ...and bind this address
};

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
