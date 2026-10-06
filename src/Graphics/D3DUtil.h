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

// ----------------------------------------------------------------------------
// DN_HR - a failed HRESULT is FATAL, and it says WHY (code-review C195).
//
// It used to be a bare DN_ASSERT on the expression text, so an out-of-memory
// and a device removal read the same in the log, and a TDR - the most common
// way a D3D12 game dies - left no evidence at all: the next Present failed and
// the report named only the call. FailHr logs, in order, the call with its
// HRESULT and the code's name, the device's removal state (GetDeviceRemoved-
// Reason) and, for a removed device, what DRED recorded - the command lists
// still in flight with the op each stopped at, and the GPU address of a page
// fault with the allocations around it - and only THEN goes through
// crash::ReportFatal (record, minidump) and aborts. Evidence before the abort,
// the DN_ASSERT rule.
// ----------------------------------------------------------------------------
[[noreturn]] void FailHr(HRESULT hr, const char* expr, const char* file, int line);

#define DN_HR(expr)                                                              \
	do {                                                                         \
		const HRESULT hr_ = (expr);                                              \
		if (FAILED(hr_)) ::dungeon::gfx::FailHr(hr_, #expr, __FILE__, __LINE__); \
	} while (0)

// The symbolic name of an HRESULT the D3D12 / DXGI path returns
// ("DXGI_ERROR_DEVICE_HUNG"), or "unrecognised" - the hex beside it is still
// the answer then.
const char* HrName(HRESULT hr);

// The device a fatal report describes. GraphicsDevice hands over its own once
// it is made and takes it back on destruction (Unwatch clears only the device
// it is given, so a stale one never wipes a newer one). Watching also installs
// a crash::SetFatalNote, so an assert, a fault in the driver or a terminate
// that lands AFTER a removal reports the removal too - a TDR surfaces wherever
// the next GPU-touching call happens to be, and that is not always a DN_HR. A
// removal is logged once per process.
void WatchDevice(ID3D12Device* device);
void UnwatchDevice(ID3D12Device* device);

// `dredpoke`: puts a MADE-UP DRED record - three command lists (one stopped
// mid-draw beside a marker, one queued behind it and never begun, one
// finished) and a page fault with a live and a freed allocation - through the
// readout a removal logs. The readout's list walking
// is otherwise unreachable on purpose: an asked-for removal (crashpoke
// devremoved) leaves DRED no list in flight, and only a real TDR - a hung GPU,
// never something a harness may do on a shared machine - leaves one.
void LogDredSample();

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
