// ============================================================================
// Graphics/GraphicsDevice.h — D3D12 device ownership and frame plumbing.
//
// This class owns everything that exists once per application:
//   * the device, the direct command queue, and the swapchain
//     (kFrameCount = 3 back buffers, flip-discard, vsync'd Present)
//   * descriptor heaps: RTV (back buffers), DSV (one depth buffer), and one
//     shader-visible CBV/SRV heap that all textures allocate slots from
//   * frame synchronization (see BeginFrame) and a blocking one-shot command
//     path for load-time uploads (ExecuteImmediate)
//
// FRAME SYNC MODEL: there are kFrameCount "frame slots", each with its own
// command allocator and fence value. EndFrame signals the fence; BeginFrame
// waits until the GPU finished the *previous* use of the new slot before
// resetting its allocator. CPU and GPU therefore overlap up to kFrameCount-1
// frames, and any per-frame resource (see UploadAllocator) must exist once
// per slot.
//
// Renderer and SpriteBatch build passes on top of this; nothing above the
// Graphics module ever touches D3D12 directly.
// ============================================================================
#pragma once

#include "Core/Types.h"
#include "Graphics/D3DUtil.h"
#include "Graphics/DisplayEnum.h" // FullscreenMode

#include <dxgi1_6.h>

#include <functional>
#include <string>

namespace dungeon::gfx {

inline constexpr u32 kFrameCount = 3;
inline constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
inline constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

struct SrvHandle {
	u32 index = 0;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
	D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
};

// Owns the D3D12 device, swapchain, frame synchronization, and descriptor
// heaps. The Renderer drives passes on top of this.
class GraphicsDevice {
public:
	// preferredAdapterLuid selects a specific GPU by packed LUID (see
	// DisplayEnum::PackLuid); 0 = auto-pick the highest-performance adapter.
	GraphicsDevice(HWND__* hwnd, u32 width, u32 height, u64 preferredAdapterLuid = 0);
	~GraphicsDevice();

	GraphicsDevice(const GraphicsDevice&) = delete;
	GraphicsDevice& operator=(const GraphicsDevice&) = delete;

	ID3D12Device* Device() const { return m_device.Get(); }
	u32 Width() const { return m_width; }
	u32 Height() const { return m_height; }
	u32 FrameIndex() const { return m_frameIndex; } // 0..kFrameCount-1

	// Diagnostics for the dev console. Adapter name from DXGI_ADAPTER_DESC1;
	// local (dedicated) video memory current usage vs. the OS-provided budget,
	// both in bytes (zero when no adapter was retained).
	const std::string& AdapterName() const { return m_adapterName; }
	// Packed LUID of the adapter the device is actually running on (the Video
	// tab compares this to a staged choice to decide whether a restart is
	// needed — switching adapters requires recreating the device).
	u64 AdapterLuid() const { return m_adapterLuid; }
	struct GpuMemoryInfo {
		u64 usedBytes = 0;
		u64 budgetBytes = 0;
	};
	GpuMemoryInfo QueryGpuMemory() const;

	// Begins the frame: waits for this slot's previous use, resets the command
	// list, transitions the back buffer, clears, and binds RT + viewport.
	ID3D12GraphicsCommandList* BeginFrame(const float clearColor[4]);
	void EndFrame(); // close, execute, present, signal

	// Present sync interval (1..4): present every Nth vblank, so 1 = full refresh
	// and 2/3/4 divide the frame rate while staying vblank-aligned (tear-free).
	// This is how the Video tab's Frame Rate dropdown caps GPU load on a high-
	// refresh display. Live; safe to call any frame.
	void SetPresentInterval(u32 interval);
	// The current monitor's refresh rate in Hz (read from the OS) — the dropdown
	// labels the intervals with the resulting frame rates (refresh / interval).
	int RefreshHz() const;

	// Re-binds the back buffer RT/DSV + full viewport after an offscreen pass
	// (e.g. shadow rendering) redirected the output merger. No clear.
	void BindBackBuffer(ID3D12GraphicsCommandList* list);

	void Resize(u32 width, u32 height);

	// Enters/leaves DXGI exclusive full-screen on the given output index of the
	// active adapter. Windowed and Borderless modes leave exclusive state and
	// are driven by the window's geometry instead (the ensuing WM_SIZE calls
	// Resize). For Exclusive, width/height request a display mode; 0,0 keeps the
	// output's current mode. No-op transitions are cheap.
	void SetFullscreen(bool exclusive, u32 outputIndex, u32 width, u32 height);

	void WaitIdle();

	// Allocates a slot in the shader-visible CBV/SRV heap (never freed; the
	// game's texture count is small and bounded).
	SrvHandle AllocateSrv();
	ID3D12DescriptorHeap* SrvHeap() const { return m_srvHeap.Get(); }

	// Records `record` into a one-shot command list and blocks until the GPU
	// finishes it. Used for resource uploads at load time.
	void ExecuteImmediate(const std::function<void(ID3D12GraphicsCommandList*)>& record);

private:
	void CreateSizeDependentResources();
	void ReleaseSizeDependentResources();
	// Unconditional back-buffer rebuild at the current size — required by
	// flip-model swap chains after every fullscreen<->windowed transition.
	void RecreateSwapChainBuffers();

	u32 m_width = 0;
	u32 m_height = 0;

	ComPtr<IDXGIFactory6> m_factory;
	ComPtr<IDXGIAdapter3> m_adapter; // retained for video-memory queries + outputs
	std::string m_adapterName;
	u64 m_adapterLuid = 0;
	HWND__* m_hwnd = nullptr;       // for full-screen window association
	u32 m_swapFlags = 0;           // swapchain create + ResizeBuffers flags
	ComPtr<ID3D12Device> m_device;
	ComPtr<ID3D12CommandQueue> m_queue;
	ComPtr<IDXGISwapChain3> m_swapchain;

	ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
	ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
	ComPtr<ID3D12DescriptorHeap> m_srvHeap;
	u32 m_rtvSize = 0;
	u32 m_srvSize = 0;
	u32 m_srvNext = 0;

	ComPtr<ID3D12Resource> m_backBuffers[kFrameCount];
	ComPtr<ID3D12Resource> m_depthBuffer;

	ComPtr<ID3D12CommandAllocator> m_allocators[kFrameCount];
	ComPtr<ID3D12GraphicsCommandList> m_commandList;

	ComPtr<ID3D12Fence> m_fence;
	u64 m_fenceValues[kFrameCount]{};
	u64 m_nextFenceValue = 1;
	void* m_fenceEvent = nullptr;

	u32 m_frameIndex = 0;

	// Present sync interval (Settings → Video Frame Rate). 1 = every vblank
	// (full refresh); 2/3/4 divide the rate tear-free. See SetPresentInterval.
	u32 m_presentInterval = 1;
};

} // namespace dungeon::gfx
