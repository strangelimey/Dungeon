#include "Graphics/GraphicsDevice.h"

#include "Core/AllocTrack.h"
#include "Core/Log.h"
#include "Core/Profile.h"
#include "Core/StringUtil.h"
#include "Graphics/DisplayEnum.h" // PackLuid

#include <Windows.h>

#include <algorithm>
#include <format>
#include <mutex>

namespace dungeon::gfx {

GraphicsDevice::GraphicsDevice(HWND__* hwnd, u32 width, u32 height,
							   u64 preferredAdapterLuid)
	: m_width(width), m_height(height), m_hwnd(hwnd) {
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

	// Creates the device on `adapter` and records its identity; returns false
	// for software adapters or a failed device create (caller tries the next).
	ComPtr<IDXGIAdapter1> adapter;
	auto tryAdapter = [&](IDXGIAdapter1* a) -> bool {
		DXGI_ADAPTER_DESC1 desc;
		a->GetDesc1(&desc);
		if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) return false;
		if (SUCCEEDED(D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0,
										IID_PPV_ARGS(&m_device)))) {
			m_adapterName = str::Narrow(desc.Description);
			m_adapterLuid = PackLuid(desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
			ComPtr<IDXGIAdapter1>(a).As(&m_adapter); // IDXGIAdapter3 for queries/outputs
			log::Info("GPU: {}", m_adapterName);
			return true;
		}
		return false;
	};

	// A specific adapter was requested (Settings → Video): match it by LUID.
	if (preferredAdapterLuid != 0) {
		for (UINT i = 0; m_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND;
			 ++i) {
			DXGI_ADAPTER_DESC1 desc;
			adapter->GetDesc1(&desc);
			if (PackLuid(desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart) ==
				preferredAdapterLuid) {
				tryAdapter(adapter.Get());
				break;
			}
			adapter.Reset();
		}
		if (!m_device) log::Warn("Requested adapter not found; using default");
	}

	// Otherwise (or on miss) prefer the highest-performance hardware adapter.
	if (!m_device) {
		for (UINT i = 0; m_factory->EnumAdapterByGpuPreference(
							 i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
							 IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
			 ++i) {
			if (tryAdapter(adapter.Get())) break;
			adapter.Reset();
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

	InstallDebugMessageLog();

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
	// ALLOW_MODE_SWITCH lets exclusive full-screen change the display mode; the
	// same flag must be passed to every ResizeBuffers call afterwards.
	m_swapFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	scDesc.Flags = m_swapFlags;
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
	srvDesc.NumDescriptors = kSrvHeapCapacity;
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

	// GPU timestamps. Not fatal if the queue cannot do them (WARP) — the engine
	// just reports no GPU rows.
	m_gpu.Init(m_device.Get(), m_queue.Get(), kFrameCount);
	DN_ASSERT(m_fenceEvent, "CreateEvent failed");

	CreateSizeDependentResources();
	log::Info("D3D12 device ready ({}x{}, {} frames in flight)", width, height,
			  kFrameCount);
}

GraphicsDevice::~GraphicsDevice() {
	WaitIdle();
	// DXGI requires leaving exclusive full-screen before the swapchain is
	// released, or Release warns/asserts.
	if (m_swapchain) {
		BOOL fs = FALSE;
		m_swapchain->GetFullscreenState(&fs, nullptr);
		if (fs) m_swapchain->SetFullscreenState(FALSE, nullptr);
	}
	if (m_fenceEvent) CloseHandle(m_fenceEvent);
	if (m_capTimer) CloseHandle(m_capTimer);
	// Before the device drops, so a message raised by D3D's own teardown cannot
	// arrive after the log has been finalized.
	if (m_msgCookie) {
		ComPtr<ID3D12InfoQueue1> q1;
		if (SUCCEEDED(m_device.As(&q1))) q1->UnregisterMessageCallback(m_msgCookie);
		m_msgCookie = 0;
	}
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
	DN_HR(m_swapchain->ResizeBuffers(kFrameCount, width, height, kBackBufferFormat,
									 m_swapFlags));
	CreateSizeDependentResources();
}

// Flip-model swap chains REQUIRE a ResizeBuffers after every fullscreen<->
// windowed transition, even when the pixel size is unchanged — otherwise the
// next Present fails (DXGI MISCELLANEOUS ERROR #117). The transition's own
// WM_SIZE can early-out of Resize() when the dimensions match, so SetFullscreen
// calls this unconditionally after each SetFullscreenState.
void GraphicsDevice::RecreateSwapChainBuffers() {
	WaitIdle();
	ReleaseSizeDependentResources();
	DN_HR(m_swapchain->ResizeBuffers(kFrameCount, m_width, m_height, kBackBufferFormat,
									 m_swapFlags));
	CreateSizeDependentResources();
}

// ----------------------------------------------------------------------------
// Full-screen control. Exclusive mode targets a specific output (monitor) of
// the active adapter and optionally requests a display mode; the SetFullscreen
// state change provokes a WM_SIZE → Resize, which rebuilds the back buffers.
// Windowed/Borderless just drop any exclusive state — the window's geometry
// (Window::SetWindowed / SetBorderless) does the rest.
// ----------------------------------------------------------------------------
void GraphicsDevice::SetFullscreen(bool exclusive, u32 outputIndex, u32 width,
								   u32 height) {
	WaitIdle();

	BOOL currentlyFs = FALSE;
	m_swapchain->GetFullscreenState(&currentlyFs, nullptr);

	if (!exclusive) {
		if (currentlyFs) {
			m_swapchain->SetFullscreenState(FALSE, nullptr);
			// A WM_SIZE follows from the restored window, but it may report the
			// same size and early-out of Resize(); the flip model still demands a
			// ResizeBuffers post-transition. The caller resizes the window next.
			RecreateSwapChainBuffers();
		}
		return;
	}

	// Exclusive: pick the target output and (optionally) the display mode.
	ComPtr<IDXGIOutput> output;
	if (m_adapter) m_adapter->EnumOutputs(outputIndex, &output);

	if (width > 0 && height > 0) {
		DXGI_MODE_DESC mode{};
		mode.Width = width;
		mode.Height = height;
		mode.Format = kBackBufferFormat;
		m_swapchain->ResizeTarget(&mode); // size the window to the mode first
	}
	if (FAILED(m_swapchain->SetFullscreenState(TRUE, output.Get()))) {
		log::Warn("Exclusive full-screen failed; staying windowed");
		return;
	}
	// Re-issue the mode after the transition (recommended DXGI pattern) so the
	// resolution actually takes; the resulting WM_SIZE rebuilds the buffers.
	if (width > 0 && height > 0) {
		DXGI_MODE_DESC mode{};
		mode.Width = width;
		mode.Height = height;
		mode.Format = kBackBufferFormat;
		m_swapchain->ResizeTarget(&mode);
	}
	// Mandatory post-transition rebuild: the WM_SIZE above early-outs of Resize()
	// when the fullscreen size matches the prior window size, so do it here or the
	// next Present aborts (DXGI #117).
	RecreateSwapChainBuffers();
}

// ----------------------------------------------------------------------------
// Frame loop. BeginFrame/EndFrame bracket all rendering for one frame:
//   BeginFrame: throttle on the slot's fence → reset allocator/list →
//               back buffer to RENDER_TARGET → clear → bind RT/viewport/heap
//   EndFrame:   back buffer to PRESENT → execute → Present → signal
//               (Present's sync interval divides the refresh; SetPresentInterval)
// ----------------------------------------------------------------------------
ID3D12GraphicsCommandList* GraphicsDevice::BeginFrame(const float clearColor[4]) {
	// Wait until the GPU has finished the previous frame that used this slot.
	//
	// ZONED, and this is the point of it: time spent here is the main thread
	// STOPPED because the GPU is kFrameCount frames behind, which is the signature
	// of a GPU-bound frame and nothing else. Folded into `render` — as it was —
	// it read as expensive rendering, which is the opposite conclusion. The zone
	// brackets the test as well as the wait so a frame that does not block still
	// reports the (near-zero) cost of asking.
	{
		DN_PROFILE_ZONE(prof::kZoneWaitGpu);
		if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex]) {
			DN_HR(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
			WaitForSingleObject(m_fenceEvent, INFINITE);
		}
	}

	// The fence above is exactly the guarantee the GPU profiler needs: the GPU has
	// finished the last frame that used this slot, so its timestamps are readable.
	m_gpu.BeginFrame(m_frameIndex);

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

// ============================================================================
// Debug-layer validation messages -> dungeon.log
//
// EnableDebugLayer (in the constructor) turns validation ON, but the layer on
// its own only talks to OutputDebugString: run the game outside a debugger and
// its diagnosis is written to nobody. That is backwards for the failure it
// exists to catch — an object released while a command list still references it
// surfaces as an access violation INSIDE D3D12SDKLayers at submit time, and the
// message naming the object is the whole answer. A fault of exactly that shape
// (2026-08-18, in ExecuteCommandLists) is what this was added for; the layer had
// almost certainly already said why, into a stream nothing was reading.
//
// THE CALLBACK IS THE POINT, not the queue. ID3D12InfoQueue1 delivers a message
// synchronously from inside the offending D3D call, so log::Write — which
// flushes per line — has it on disk BEFORE that call returns. A queue polled at
// the end of the frame loses precisely the message that mattered, because the
// process dies inside the call the drain was going to follow. Polling is the
// fallback for a runtime without InfoQueue1, and it says so at startup.
//
// INFO/MESSAGE severities are dropped: the layer narrates every resource
// creation at those levels, and a log too noisy to read is the failure being
// fixed, not a milder version of it.
// ============================================================================
namespace {

log::Level LevelFor(D3D12_MESSAGE_SEVERITY sev) {
	switch (sev) {
	case D3D12_MESSAGE_SEVERITY_CORRUPTION:
	case D3D12_MESSAGE_SEVERITY_ERROR:   return log::Level::Error;
	case D3D12_MESSAGE_SEVERITY_WARNING: return log::Level::Warn;
	default:                             return log::Level::Debug;
	}
}

const char* SeverityName(D3D12_MESSAGE_SEVERITY sev) {
	switch (sev) {
	case D3D12_MESSAGE_SEVERITY_CORRUPTION: return "CORRUPTION";
	case D3D12_MESSAGE_SEVERITY_ERROR:      return "error";
	case D3D12_MESSAGE_SEVERITY_WARNING:    return "warning";
	case D3D12_MESSAGE_SEVERITY_INFO:       return "info";
	default:                                return "message";
	}
}

// IDENTICAL CONSECUTIVE messages collapse to powers of ten, the same throttle
// Core/Diagnostics applies to a worker throwing the same thing every tick — and
// for the same reason, sharpened by a measurement. The back-buffer clear warns
// ONCE A FRAME for good (id 820: a swapchain buffer is created by DXGI with no
// optimized clear value, so no clear can ever match it, and there is nothing to
// fix at the call site). Unthrottled that is 3096 lines a minute, which would
// bury the one message this whole path exists to deliver. The count is kept and
// printed, so nothing is hidden — only repeated.
bool IsLogPoint(u64 n) { // 1, 10, 100, 1000, ...
	while (n >= 10 && n % 10 == 0) n /= 10;
	return n == 1;
}

// One formatting site for both collection paths, so a message reads identically
// however it arrived — the same reason IsPlumbingFrame is one rule for every
// stack readout. Callable from any thread: InfoQueue1 delivers on whichever
// thread made the D3D call.
std::mutex g_msgMutex;
D3D12_MESSAGE_ID g_lastId = static_cast<D3D12_MESSAGE_ID>(-1);
u64 g_repeat = 0;

void ReportMessage(D3D12_MESSAGE_SEVERITY sev, D3D12_MESSAGE_ID id, const char* desc) {
	if (sev == D3D12_MESSAGE_SEVERITY_INFO || sev == D3D12_MESSAGE_SEVERITY_MESSAGE)
		return;
	// This reports from inside a frame the allocation guard may be bracketing,
	// and formatting the line allocates — so it excuses itself, like every other
	// reporter that can fire mid-frame.
	alloc::Excused excuse;

	u64 repeat = 1;
	{
		std::lock_guard lock(g_msgMutex);
		g_repeat = (id == g_lastId) ? g_repeat + 1 : 1;
		g_lastId = id;
		repeat = g_repeat;
	}
	if (repeat > 1 && !IsLogPoint(repeat)) return;

	const std::string again =
		repeat > 1 ? std::format(" — repeated {} times", repeat) : std::string{};
	log::Write(LevelFor(sev),
			   std::format("d3d12 {} [{}]: {}{}", SeverityName(sev), static_cast<int>(id),
						   desc ? desc : "(no description)", again));
}

void CALLBACK OnD3D12Message(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
							 D3D12_MESSAGE_ID id, LPCSTR description, void*) {
	ReportMessage(severity, id, description);
}

} // namespace

void GraphicsDevice::InstallDebugMessageLog() {
	if (!m_device) return;

	// WHICH PATH INSTALLED IS LOGGED, because silence here is ambiguous: "the
	// layer found nothing wrong" and "nothing was ever listening" read the same
	// in a log file, and the second is the state this code exists to end.
	ComPtr<ID3D12InfoQueue1> queue1;
	if (SUCCEEDED(m_device.As(&queue1))) {
		DWORD cookie = 0;
		if (SUCCEEDED(queue1->RegisterMessageCallback(
				OnD3D12Message, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie))) {
			m_msgCookie = static_cast<u32>(cookie);
			log::Info("D3D12 validation -> dungeon.log (callback)");
			return;
		}
	}

	if (SUCCEEDED(m_device.As(&m_infoQueue))) {
		log::Info("D3D12 validation -> dungeon.log (polled per frame; a message "
				  "raised by the call that crashes will be lost)");
		return;
	}
	// Neither interface = no debug layer, which is the normal release build.
	// Nothing to say: validation was never asked for.
}

void GraphicsDevice::DrainDebugMessages() {
	if (!m_infoQueue) return;
	const u64 count = m_infoQueue->GetNumStoredMessages();
	if (count == 0) return; // the steady-state case: no work, no allocation

	alloc::Excused excuse;
	for (u64 i = 0; i < count; ++i) {
		SIZE_T bytes = 0;
		if (FAILED(m_infoQueue->GetMessage(i, nullptr, &bytes)) || bytes == 0) continue;
		m_msgScratch.resize(bytes); // grows to the longest message, then stays
		auto* msg = reinterpret_cast<D3D12_MESSAGE*>(m_msgScratch.data());
		if (FAILED(m_infoQueue->GetMessage(i, msg, &bytes))) continue;
		ReportMessage(msg->Severity, msg->ID, msg->pDescription);
	}
	m_infoQueue->ClearStoredMessages();
}

void GraphicsDevice::EndFrame() {
	// Ahead of Close/Execute, not after: on the polling path this is the last
	// chance to write out what recording produced before the submit that a
	// corrupt list faults inside of.
	DrainDebugMessages();
	const auto barrier = Transition(m_backBuffers[m_frameIndex].Get(),
									D3D12_RESOURCE_STATE_RENDER_TARGET,
									D3D12_RESOURCE_STATE_PRESENT);
	m_commandList->ResourceBarrier(1, &barrier);
	m_gpu.EndFrame(m_commandList.Get()); // resolve the frame's timestamps
	DN_HR(m_commandList->Close());

	ID3D12CommandList* lists[] = {m_commandList.Get()};
	m_queue->ExecuteCommandLists(1, lists);
	// Sync interval 1 = present every vblank (full refresh); 2/3/4 present every
	// Nth vblank, dividing the frame rate while staying vblank-aligned (tear-
	// free). See SetPresentInterval / the Video tab's Frame Rate dropdown.
	//
	// ZONED because this blocks, but read it with the caveat that it is the one
	// AMBIGUOUS wait of the three: Present parks here either waiting for the
	// vblank it is synced to — being ahead of the display, which is healthy — or
	// waiting for a back buffer the GPU has not released yet, which is not.
	// Nothing in the call distinguishes those. What disambiguates them is GPU BUSY
	// TIME measured elsewhere (the gpu source's spans): a saturated GPU means the
	// second reading, a mostly idle one means the first. The console's verdict is
	// built that way round for exactly this reason, and a waitable swapchain is
	// what would separate them at the source.
	{
		DN_PROFILE_ZONE(prof::kZonePresent);
		DN_HR(m_swapchain->Present(m_presentInterval, 0));
	}

	m_fenceValues[m_frameIndex] = m_nextFenceValue;
	DN_HR(m_queue->Signal(m_fence.Get(), m_nextFenceValue));
	++m_nextFenceValue;

	m_frameIndex = m_swapchain->GetCurrentBackBufferIndex();
}

void GraphicsDevice::SetPresentInterval(u32 interval) {
	m_presentInterval = std::clamp<u32>(interval, 1, 4);
}

int GraphicsDevice::RefreshHz() const {
	// The refresh rate of the monitor the window currently sits on, read from
	// the OS (DXGI has no direct query). Drives the Frame Rate dropdown's labels
	// — the actual cap is the present interval, so a stale value is cosmetic.
	HMONITOR mon = MonitorFromWindow(reinterpret_cast<HWND>(m_hwnd),
									 MONITOR_DEFAULTTONEAREST);
	MONITORINFOEXW info{};
	info.cbSize = sizeof(info);
	if (GetMonitorInfoW(mon, &info)) {
		DEVMODEW dm{};
		dm.dmSize = sizeof(dm);
		if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
			dm.dmDisplayFrequency > 1)
			return static_cast<int>(dm.dmDisplayFrequency);
	}
	return 60; // safe default when the OS reports a placeholder (0/1) or fails
}

int GraphicsDevice::FrameCapHz() const {
	if (!m_frameCap) return 0;
	// Cached: re-read at most a few times a second. RefreshHz is three Win32
	// calls including EnumDisplaySettings, which is not something to do on every
	// frame for a number that changes when a window is dragged between monitors.
	LARGE_INTEGER now{};
	QueryPerformanceCounter(&now);
	if (m_capHzCached == 0 || m_qpcFreq <= 0 ||
		now.QuadPart - m_capHzCheckedQpc > m_qpcFreq / 2) {
		m_capHzCached = RefreshHz();
		m_capHzCheckedQpc = now.QuadPart;
	}
	const int interval = static_cast<int>(m_presentInterval < 1 ? 1 : m_presentInterval);
	return m_capHzCached > 0 ? m_capHzCached / interval : 0;
}

void GraphicsDevice::WaitFrameCap() {
	const int hz = FrameCapHz();
	if (hz <= 0) {
		m_capDeadlineQpc = 0;
		return;
	}
	if (m_qpcFreq <= 0) {
		LARGE_INTEGER f{};
		QueryPerformanceFrequency(&f);
		m_qpcFreq = f.QuadPart;
		if (m_qpcFreq <= 0) return;
	}
	if (!m_capTimer) {
		// HIGH_RESOLUTION (Win10 1803+) gets sub-millisecond granularity without
		// timeBeginPeriod, which is a PROCESS-WIDE setting that slows every other
		// timer in the process and is rude to do for one loop's benefit. If the
		// flag is unsupported the call fails and we fall back to spinning, which
		// is accurate but burns a core — so the failure is worth not hiding.
		m_capTimer = CreateWaitableTimerExW(nullptr, nullptr,
											CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
											TIMER_ALL_ACCESS);
		if (!m_capTimer)
			log::Warn("frame cap: no high-resolution timer; falling back to spin-wait");
	}

	const i64 slice = m_qpcFreq / hz;
	LARGE_INTEGER now{};
	QueryPerformanceCounter(&now);

	// RESYNC rather than catch up. After a hitch, a load screen or a paused
	// debugger the deadline can be many slices in the past, and honouring it
	// would run a burst of uncapped frames trying to make the time back — the
	// one moment the cap is most visible and least wanted.
	if (m_capDeadlineQpc == 0 || now.QuadPart - m_capDeadlineQpc > slice * 4)
		m_capDeadlineQpc = now.QuadPart;
	m_capDeadlineQpc += slice;

	for (;;) {
		QueryPerformanceCounter(&now);
		const i64 left = m_capDeadlineQpc - now.QuadPart;
		if (left <= 0) break;

		const double leftMs = static_cast<double>(left) * 1000.0 / static_cast<double>(m_qpcFreq);
		// Sleep the bulk, SPIN the last stretch. A timer is only accurate to a
		// fraction of a millisecond and overshooting the deadline is what the cap
		// exists to prevent, so the tail is spun; a quarter of a millisecond of
		// spinning is cheap next to sleeping through the vblank we are aiming at.
		if (m_capTimer && leftMs > 0.75) {
			LARGE_INTEGER due{};
			due.QuadPart = -static_cast<LONGLONG>((leftMs - 0.5) * 10000.0); // 100ns, relative
			if (SetWaitableTimer(m_capTimer, &due, 0, nullptr, nullptr, FALSE))
				WaitForSingleObject(m_capTimer, INFINITE);
			else
				YieldProcessor();
		} else {
			YieldProcessor();
		}
	}
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
	SrvHandle handle;
	if (!m_srvFree.empty()) {
		handle.index = m_srvFree.back();
		m_srvFree.pop_back();
	} else {
		// The free list recycles, so this bounds LIVE textures — but it is still
		// a ceiling, and one that was invisible right up to the abort. Say what
		// the number was when it blew, so the message points at a leak rather
		// than just a limit.
		DN_ASSERT(m_srvNext < kSrvHeapCapacity,
				  std::format("SRV heap exhausted: {} slots, all live (peak {}). "
							  "Something is holding textures it should have freed.",
							  kSrvHeapCapacity, m_srvHighWater));
		handle.index = m_srvNext++;
	}
	++m_srvLive;
	if (m_srvLive > m_srvHighWater) {
		m_srvHighWater = m_srvLive;
		// On the crossing only, never per allocation.
		for (const u32 pct : {75u, 90u})
			if (m_srvHighWater == kSrvHeapCapacity * pct / 100)
				log::Warn("SRV heap {}% full ({} of {} slots live)", pct, m_srvHighWater,
						  kSrvHeapCapacity);
	}
	handle.cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
	handle.cpu.ptr += static_cast<size_t>(handle.index) * m_srvSize;
	handle.gpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
	handle.gpu.ptr += static_cast<u64>(handle.index) * m_srvSize;
	return handle;
}

void GraphicsDevice::FreeSrv(u32 index) {
	m_srvFree.push_back(index);
	if (m_srvLive > 0) --m_srvLive;
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
