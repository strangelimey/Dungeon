// ============================================================================
// Graphics/D3DUtil.h — small D3D12 conveniences shared by the module:
// ComPtr alias, the DN_HR fatal-HRESULT check, and one-line builders for the
// verbose descriptor structs (heap properties, buffer descs, barriers).
// ============================================================================
#pragma once

#include "Core/Assert.h"
#include "Core/Types.h"

#include <d3d12.h>
#include <wrl/client.h>

namespace dungeon::gfx {

using Microsoft::WRL::ComPtr;

#define DN_HR(expr)                                                              \
	do {                                                                         \
		const HRESULT hr_ = (expr);                                              \
		DN_ASSERT(SUCCEEDED(hr_), #expr);                                        \
	} while (0)

inline D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type) {
	D3D12_HEAP_PROPERTIES props{};
	props.Type = type;
	return props;
}

inline D3D12_RESOURCE_DESC BufferDesc(u64 size) {
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = size;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_UNKNOWN;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	return desc;
}

inline D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource,
										 D3D12_RESOURCE_STATES before,
										 D3D12_RESOURCE_STATES after) {
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	return barrier;
}

} // namespace dungeon::gfx
