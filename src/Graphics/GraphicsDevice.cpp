#include "Graphics/GraphicsDevice.h"

#include "Core/Log.h"
#include "Core/StringUtil.h"

#include <Windows.h>

namespace dungeon::gfx {

GraphicsDevice::GraphicsDevice(HWND__* hwnd, u32 width, u32 height)
	: m_width(width), m_height(height) {
	UINT factoryFlags = 0;
#ifdef _DEBUG
	{
		ComPtr<ID3D12Debug> debug;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
			debug->EnableDebugLayer();
			factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
			log::Info("D3D12 debug layer enabled");
		}
	}
#endif
	DN_HR(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory)));

	// Prefer the highest-performance hardware adapter.
	ComPtr<IDXGIAdapter1> adapter;
	for (UINT i = 0; m_factory->EnumAdapterByGpuPreference(
						 i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
						 IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
		 ++i) {
		DXGI_ADAPTER_DESC1 desc;
		adapter->GetDesc1(&desc);
		if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
		if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
										IID_PPV_ARGS(&m_device)))) {
			m_adapterName = str::Narrow(desc.Description);
			adapter.As(&m_adapter); // IDXGIAdapter3 for QueryVideoMemoryInfo
			log::Info("GPU: {}", m_adapterName);
			break;
		}
	}
	if (!m_device) { // WARP fallback so the game still runs without a GPU
		ComPtr<IDXGIAdapter> warp;
		DN_HR(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
		DN_HR(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0,
								IID_PPV_ARGS(&m_device)));
		warp.As(&m_adapter);
		m_adapterName = "WARP (software)";
		log::Warn("Using WARP software rasterizer");
	}

	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	DN_HR(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue)));

	DXGI_SWAP_CHAIN_DESC1 scDesc{};
	scDesc.Width = width;
	scDesc.Height = height;
	scDesc.Format = kBackBufferFormat;
	scDesc.SampleDesc.Count = 1;
	scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scDesc.BufferCount = kFrameCount;
	scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	ComPtr<IDXGISwapChain1> swap1;
	DN_HR(m_factory->CreateSwapChainForHwnd(m_queue.Get(), reinterpret_cast<HWND>(hwnd),
											&scDesc, nullptr, nullptr, &swap1));
	DN_HR(swap1.As(&m_swapchain));
	m_factory->MakeWindowAssociation(reinterpret_cast<HWND>(hwnd), DXGI_MWA_NO_ALT_ENTER);

	// Descriptor heaps.
	D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
	rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvDesc.NumDescriptors = kFrameCount;
	DN_HR(m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)));
	m_rtvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
	dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvDesc.NumDescriptors = 1;
	DN_HR(m_device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&m_dsvHeap)));

	D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
	srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvDesc.NumDescriptors = 1024;
	srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	DN_HR(m_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_srvHeap)));
	m_srvSize = m_device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// Per-frame command allocators + one reusable command list.
	for (u32 i = 0; i < kFrameCount; ++i)
		DN_HR(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
											   IID_PPV_ARGS(&m_allocators[i])));
	DN_HR(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
									  m_allocators[0].Get(), nullptr,
									  IID_PPV_ARGS(&m_commandList)));
	DN_HR(m_commandList->Close());

	DN_HR(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
	m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	DN_ASSERT(m_fenceEvent, "CreateEvent failed");

	CreateSizeDependentResources();
	log::Info("D3D12 device ready ({}x{}, {} frames in flight)", width, height,
			  kFrameCount);
}

GraphicsDevice::~GraphicsDevice() {
	WaitIdle();
	if (m_fenceEvent) CloseHandle(m_fenceEvent);
}

void GraphicsDevice::CreateSizeDependentResources() {
	// Back-buffer RTVs.
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	for (u32 i = 0; i < kFrameCount; ++i) {
		DN_HR(m_swapchain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])));
		m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtv);
		rtv.ptr += m_rtvSize;
	}

	// Depth buffer.
	D3D12_RESOURCE_DESC depthDesc{};
	depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthDesc.Width = m_width;
	depthDesc.Height = m_height;
	depthDesc.DepthOrArraySize = 1;
	depthDesc.MipLevels = 1;
	depthDesc.Format = kDepthFormat;
	depthDesc.SampleDesc.Count = 1;
	depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE clear{};
	clear.Format = kDepthFormat;
	clear.DepthStencil.Depth = 1.0f;

	const D3D12_HEAP_PROPERTIES heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
	DN_HR(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &depthDesc,
											D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
											IID_PPV_ARGS(&m_depthBuffer)));
	m_device->CreateDepthStencilView(m_depthBuffer.Get(), nullptr,
									 m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

	m_frameIndex = m_swapchain->GetCurrentBackBufferIndex();
}

void GraphicsDevice::ReleaseSizeDependentResources() {
	for (auto& buffer : m_backBuffers) buffer.Reset();
	m_depthBuffer.Reset();
}

void GraphicsDevice::Resize(u32 width, u32 height) {
	if (width == 0 || height == 0 || (width == m_width && height == m_height)) return;
	WaitIdle();
	ReleaseSizeDependentResources();
	m_width = width;
	m_height = height;
	DN_HR(m_swapchain->ResizeBuffers(kFrameCount, width, height, kBackBufferFormat, 0));
	CreateSizeDependentResources();
}

// ----------------------------------------------------------------------------
// Frame loop. BeginFrame/EndFrame bracket all rendering for one frame:
//   BeginFrame: throttle on the slot's fence → reset allocator/list →
//               back buffer to RENDER_TARGET → clear → bind RT/viewport/heap
//   EndFrame:   back buffer to PRESENT → execute → Present(vsync) → signal
// ----------------------------------------------------------------------------
ID3D12GraphicsCommandList* GraphicsDevice::BeginFrame(const float clearColor[4]) {
	// Wait until the GPU has finished the previous frame that used this slot.
	if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex]) {
		DN_HR(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}

	DN_HR(m_allocators[m_frameIndex]->Reset());
	DN_HR(m_commandList->Reset(m_allocators[m_frameIndex].Get(), nullptr));

	const auto barrier = Transition(m_backBuffers[m_frameIndex].Get(),
									D3D12_RESOURCE_STATE_PRESENT,
									D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_commandList->ResourceBarrier(1, &barrier);

	D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += static_cast<size_t>(m_frameIndex) * m_rtvSize;
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
		m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
	m_commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
	m_commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
	m_commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0,
										 nullptr);

	const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(m_width),
								  static_cast<float>(m_height), 0.0f, 1.0f};
	const D3D12_RECT scissor{0, 0, static_cast<LONG>(m_width),
							 static_cast<LONG>(m_height)};
	m_commandList->RSSetViewports(1, &viewport);
	m_commandList->RSSetScissorRects(1, &scissor);

	ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Get()};
	m_commandList->SetDescriptorHeaps(1, heaps);
	return m_commandList.Get();
}

void GraphicsDevice::BindBackBuffer(ID3D12GraphicsCommandList* list) {
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += static_cast<size_t>(m_frameIndex) * m_rtvSize;
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
		m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
	list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

	const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(m_width),
								  static_cast<float>(m_height), 0.0f, 1.0f};
	const D3D12_RECT scissor{0, 0, static_cast<LONG>(m_width),
							 static_cast<LONG>(m_height)};
	list->RSSetViewports(1, &viewport);
	list->RSSetScissorRects(1, &scissor);
}

void GraphicsDevice::EndFrame() {
	const auto barrier = Transition(m_backBuffers[m_frameIndex].Get(),
									D3D12_RESOURCE_STATE_RENDER_TARGET,
									D3D12_RESOURCE_STATE_PRESENT);
	m_commandList->ResourceBarrier(1, &barrier);
	DN_HR(m_commandList->Close());

	ID3D12CommandList* lists[] = {m_commandList.Get()};
	m_queue->ExecuteCommandLists(1, lists);
	DN_HR(m_swapchain->Present(1, 0)); // vsync

	m_fenceValues[m_frameIndex] = m_nextFenceValue;
	DN_HR(m_queue->Signal(m_fence.Get(), m_nextFenceValue));
	++m_nextFenceValue;

	m_frameIndex = m_swapchain->GetCurrentBackBufferIndex();
}

void GraphicsDevice::WaitIdle() {
	DN_HR(m_queue->Signal(m_fence.Get(), m_nextFenceValue));
	if (m_fence->GetCompletedValue() < m_nextFenceValue) {
		DN_HR(m_fence->SetEventOnCompletion(m_nextFenceValue, m_fenceEvent));
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}
	++m_nextFenceValue;
}

SrvHandle GraphicsDevice::AllocateSrv() {
	DN_ASSERT(m_srvNext < 1024, "SRV heap exhausted");
	SrvHandle handle;
	handle.index = m_srvNext++;
	handle.cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
	handle.cpu.ptr += static_cast<size_t>(handle.index) * m_srvSize;
	handle.gpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
	handle.gpu.ptr += static_cast<u64>(handle.index) * m_srvSize;
	return handle;
}

void GraphicsDevice::ExecuteImmediate(
	const std::function<void(ID3D12GraphicsCommandList*)>& record) {
	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
	DN_HR(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
										   IID_PPV_ARGS(&allocator)));
	DN_HR(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
									  nullptr, IID_PPV_ARGS(&list)));
	record(list.Get());
	DN_HR(list->Close());
	ID3D12CommandList* lists[] = {list.Get()};
	m_queue->ExecuteCommandLists(1, lists);
	WaitIdle();
}

GraphicsDevice::GpuMemoryInfo GraphicsDevice::QueryGpuMemory() const {
	GpuMemoryInfo info{};
	if (m_adapter) {
		DXGI_QUERY_VIDEO_MEMORY_INFO vram{};
		if (SUCCEEDED(m_adapter->QueryVideoMemoryInfo(
				0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vram))) {
			info.usedBytes = vram.CurrentUsage;
			info.budgetBytes = vram.Budget;
		}
	}
	return info;
}

} // namespace dungeon::gfx
