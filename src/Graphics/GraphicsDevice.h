#pragma once

#include "Core/Types.h"
#include "Graphics/D3DUtil.h"

#include <dxgi1_6.h>

#include <functional>

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
    GraphicsDevice(HWND__* hwnd, u32 width, u32 height);
    ~GraphicsDevice();

    GraphicsDevice(const GraphicsDevice&) = delete;
    GraphicsDevice& operator=(const GraphicsDevice&) = delete;

    ID3D12Device* Device() const { return m_device.Get(); }
    u32 Width() const { return m_width; }
    u32 Height() const { return m_height; }
    u32 FrameIndex() const { return m_frameIndex; } // 0..kFrameCount-1

    // Begins the frame: waits for this slot's previous use, resets the command
    // list, transitions the back buffer, clears, and binds RT + viewport.
    ID3D12GraphicsCommandList* BeginFrame(const float clearColor[4]);
    void EndFrame(); // close, execute, present, signal

    void Resize(u32 width, u32 height);
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

    u32 m_width = 0;
    u32 m_height = 0;

    ComPtr<IDXGIFactory6> m_factory;
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
};

} // namespace dungeon::gfx
