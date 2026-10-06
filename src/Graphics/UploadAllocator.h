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
//
// RUNNING OUT: Allocate() asserts, for owners whose use is bounded by
// construction (the renderer's constants, the particle batch). An owner whose
// use follows the CONTENT - the sprite batch, which draws a quad per map square
// in the editor - calls TryAllocate() instead, which hands back an empty
// allocation and leaves the arena as it was, so the owner can drop that one
// piece of work, count it, and carry on with the next (code-review C163).
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
	// As Allocate, but a request that does not fit returns {} (cpu == nullptr)
	// and takes nothing - a smaller request after it can still succeed.
	UploadAllocation TryAllocate(u64 size, u64 alignment = 256);
	void Reset() { m_offset = 0; }

	// Bytes handed out since the last Reset (alignment padding included), and
	// the arena's size: the two halves of a gauge.
	u64 Used() const { return m_offset; }
	u64 Capacity() const { return m_capacity; }

	ID3D12Resource* Resource() const { return m_buffer.Get(); }

private:
	ComPtr<ID3D12Resource> m_buffer;
	u8* m_mapped = nullptr;
	u64 m_capacity = 0;
	u64 m_offset = 0;
};

} // namespace dungeon::gfx
