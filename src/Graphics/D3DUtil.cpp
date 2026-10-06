// ============================================================================
// Graphics/D3DUtil.cpp - the failure half of D3DUtil.h: DN_HR's report, and
// the removed-device evidence every fatal report carries (code-review C195).
//
// WHAT A TDR LOOKS LIKE FROM IN HERE. The GPU stops answering, Windows resets
// it, and the device is REMOVED: every later call that can return an HRESULT
// returns a DXGI_ERROR_DEVICE_* code, and the next one checked is usually the
// frame's Present. Before this, that was the whole report - "Assertion failed:
// SUCCEEDED(hr_) - m_swapchain->Present(...)" - which names the call that
// NOTICED, never the work that killed the GPU, and reads exactly like a call
// handed bad arguments. Three things answer it, and they are logged in this
// order because each narrows the last:
//
//   1. the HRESULT, as hex AND by name (E_OUTOFMEMORY and DEVICE_REMOVED are
//      different bugs, and the old line could not tell them apart);
//   2. GetDeviceRemovedReason - HUNG (a shader looped or ran too long), FAULT
//      (bad memory access), RESET, DRIVER_INTERNAL_ERROR, or S_OK when the call
//      failed on its own and the device is fine;
//   3. DRED (Device Removed Extended Data), switched on before the device is
//      made in EVERY build (GraphicsDevice's constructor - a TDR in a release
//      build is the one most worth explaining): the command lists still in
//      flight with the op each one stopped at, and the GPU virtual address of a
//      page fault with the allocations live and recently freed around it.
//
// Everything here runs on a failure path, so it formats into ordinary strings
// (the process is not presumed damaged - a removed device is a GPU fact, not
// a corrupt heap) but reads each DRED list with a cap, never trusting a count.
// ============================================================================
#include "Graphics/D3DUtil.h"

#include "Core/CrashHandler.h"
#include "Core/Log.h"
#include "Core/StringUtil.h"

#include <dxgi.h>

#include <atomic>
#include <cstdlib>
#include <format>
#include <iterator>
#include <string>

namespace dungeon::gfx {

namespace {

// The watched device (WatchDevice). A raw pointer in an atomic: GraphicsDevice
// owns the reference and clears this before it releases it.
std::atomic<ID3D12Device*> g_device{nullptr};

// Set once the removal has been logged, so the DN_HR report and the crash
// note that runs inside ReportFatal after it do not say it twice.
std::atomic<bool> g_removalLogged{false};

// Caps on what is read out of DRED's lists. A list in flight carries every op
// it recorded - hundreds of draws a frame - and only the ones around where it
// stopped say anything.
constexpr int kMaxNodes = 8;          // command lists
constexpr u32 kOpsBefore = 4;         // completed ops shown before the stop
constexpr u32 kOpsAfter = 4;          // unfinished ops shown after the stop
constexpr int kMaxAllocations = 8;    // page-fault neighbours, per list

// D3D12_AUTO_BREADCRUMB_OP by value. A table of strings rather than a switch on
// the enum's names, so an SDK that adds or lacks a value still compiles; a value
// past the table prints its number.
constexpr const char* kOpNames[] = {
	"SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced",
	"ExecuteIndirect", "Dispatch", "CopyBufferRegion", "CopyTextureRegion",
	"CopyResource", "CopyTiles", "ResolveSubresource", "ClearRenderTargetView",
	"ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier",
	"ExecuteBundle", "Present", "ResolveQueryData", "BeginSubmission",
	"EndSubmission", "DecodeFrame", "ProcessFrames", "AtomicCopyBufferUint",
	"AtomicCopyBufferUint64", "ResolveSubresourceRegion", "WriteBufferImmediate",
	"DecodeFrame1", "SetProtectedResourceSession", "DecodeFrame2", "ProcessFrames1",
	"BuildRaytracingAccelerationStructure",
	"EmitRaytracingAccelerationStructurePostbuildInfo",
	"CopyRaytracingAccelerationStructure", "DispatchRays", "InitializeMetaCommand",
	"ExecuteMetaCommand", "EstimateMotion", "ResolveMotionVectorHeap",
	"SetPipelineState1", "InitializeExtensionCommand", "ExecuteExtensionCommand",
	"DispatchMesh", "EncodeFrame", "ResolveEncoderOutputMetadata", "Barrier",
	"BeginCommandList", "DispatchGraph", "SetProgram",
};

std::string OpName(D3D12_AUTO_BREADCRUMB_OP op) {
	const auto i = static_cast<size_t>(op);
	if (i < std::size(kOpNames)) return kOpNames[i];
	return std::format("op {}", static_cast<int>(op));
}

// An object's debug name, wide or narrow, or `fallback` when it has none.
std::string NameOf(const char* narrow, const wchar_t* wide, const char* fallback) {
	if (narrow && *narrow) return narrow;
	if (wide && *wide) return str::Narrow(wide);
	return fallback;
}

const char* DeviceStateName(D3D12_DRED_DEVICE_STATE state) {
	switch (state) {
	case D3D12_DRED_DEVICE_STATE_HUNG: return "hung";
	case D3D12_DRED_DEVICE_STATE_FAULT: return "fault";
	case D3D12_DRED_DEVICE_STATE_PAGEFAULT: return "page fault";
	default: return "unknown";
	}
}

const char* AllocationTypeName(D3D12_DRED_ALLOCATION_TYPE type) {
	switch (type) {
	case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE: return "command queue";
	case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return "command allocator";
	case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE: return "pipeline state";
	case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST: return "command list";
	case D3D12_DRED_ALLOCATION_TYPE_FENCE: return "fence";
	case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP: return "descriptor heap";
	case D3D12_DRED_ALLOCATION_TYPE_HEAP: return "heap";
	case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP: return "query heap";
	case D3D12_DRED_ALLOCATION_TYPE_COMMAND_SIGNATURE: return "command signature";
	case D3D12_DRED_ALLOCATION_TYPE_RESOURCE: return "resource";
	default: return "object";
	}
}

// --- the breadcrumbs ----------------------------------------------------------

// One command list DRED still held: where it stopped, and the ops either side.
void LogBreadcrumbNode(const D3D12_AUTO_BREADCRUMB_NODE1& node) {
	const std::string list = NameOf(node.pCommandListDebugNameA,
									node.pCommandListDebugNameW, "(unnamed)");
	const std::string queue = NameOf(node.pCommandQueueDebugNameA,
									 node.pCommandQueueDebugNameW, "(unnamed)");
	const u32 count = node.BreadcrumbCount;
	// pLastBreadcrumbValue is the number of ops the GPU COMPLETED, so it is also
	// the index of the first one it did not.
	const u32 done = node.pLastBreadcrumbValue ? *node.pLastBreadcrumbValue : 0;
	if (!node.pCommandHistory || count == 0) {
		log::Error("  list '{}' on queue '{}': no ops recorded", list, queue);
		return;
	}
	if (done >= count) {
		log::Error("  list '{}' on queue '{}': {} ops, all completed", list, queue, count);
		return;
	}
	// NONE completed says two things DRED cannot tell apart: the GPU never began
	// this list - the usual case, a list queued behind the one that hung (with
	// kFrameCount = 3 the CPU submits a frame or two more before it blocks, every
	// one of them named 'frame') - or it stopped inside the very first op. So no
	// stop marker and no op window: the marker goes only on a list the GPU was
	// PART-way through, or a TDR would point at up to three lists under one name.
	// (UE's DRED dump skips these nodes for the same reason.)
	if (done == 0) {
		log::Error("  list '{}' on queue '{}': {} ops, none completed - not started, or "
				   "stopped at its first op (#0 {})",
				   list, queue, count, OpName(node.pCommandHistory[0]));
		return;
	}
	log::Error("  list '{}' on queue '{}': {} ops, {} completed - stopped at #{} {}", list,
			   queue, count, done, done, OpName(node.pCommandHistory[done]));

	const u32 first = done > kOpsBefore ? done - kOpsBefore : 0;
	const u32 last = done + 1 + kOpsAfter < count ? done + 1 + kOpsAfter : count;
	for (u32 i = first; i < last; ++i) {
		// A breadcrumb CONTEXT is the string a SetMarker / BeginEvent carried, kept
		// when breadcrumb context is on - which pass the op belonged to.
		std::string context;
		for (u32 c = 0; c < node.BreadcrumbContextsCount && node.pBreadcrumbContexts; ++c) {
			const D3D12_DRED_BREADCRUMB_CONTEXT& ctx = node.pBreadcrumbContexts[c];
			if (ctx.BreadcrumbIndex == i && ctx.pContextString)
				context = " \"" + str::Narrow(ctx.pContextString) + "\"";
		}
		// Past the stop is "not completed", never "not run": a breadcrumb counts
		// an op when it FINISHES, so the GPU may have been part-way into any of them.
		log::Error("    #{} {}{}{}", i, OpName(node.pCommandHistory[i]), context,
				   i == done ? "   <- the GPU stopped here" : (i < done ? "" : " (not completed)"));
	}
}

// --- the page fault -----------------------------------------------------------

void LogAllocations(const char* what, const D3D12_DRED_ALLOCATION_NODE1* node) {
	int n = 0;
	for (; node && n < kMaxAllocations; node = node->pNext, ++n)
		log::Error("    {} {} '{}'", what, AllocationTypeName(node->AllocationType),
				   NameOf(node->ObjectNameA, node->ObjectNameW, "(unnamed)"));
	if (node) log::Error("    ... more {} allocations not listed", what);
}

// The readout of a DRED record: the breadcrumb lists and the page fault, each
// with the HRESULT its query returned. Split from the queries (LogDred) so
// LogDredSample can put a made-up record through exactly these lines.
void LogDredRecord(const D3D12_AUTO_BREADCRUMB_NODE1* head, HRESULT crumbsHr,
				   const D3D12_DRED_PAGE_FAULT_OUTPUT1& fault, HRESULT faultHr) {
	if (FAILED(crumbsHr)) {
		log::Error("DRED auto-breadcrumbs: unavailable, 0x{:08X} ({}) - DRED was not on "
				   "when the device was made",
				   static_cast<u32>(crumbsHr), HrName(crumbsHr));
	} else {
		int lists = 0;
		for (const D3D12_AUTO_BREADCRUMB_NODE1* n = head; n && lists <= kMaxNodes; n = n->pNext)
			++lists;
		// A list DRED still holds is one the GPU had not finished. None means the
		// removal came with nothing in flight, not that the record was lost - and
		// an asked-for removal (crashpoke devremoved) always reads none.
		if (lists > kMaxNodes)
			log::Error("DRED auto-breadcrumbs: more than {} command lists in flight (the "
					   "first {} follow)",
					   kMaxNodes, kMaxNodes);
		else
			log::Error("DRED auto-breadcrumbs: {} command list(s) in flight", lists);
		int shown = 0;
		for (const D3D12_AUTO_BREADCRUMB_NODE1* n = head; n && shown < kMaxNodes;
			 n = n->pNext, ++shown)
			LogBreadcrumbNode(*n);
	}

	if (FAILED(faultHr)) {
		log::Error("DRED page fault: unavailable, 0x{:08X} ({})", static_cast<u32>(faultHr),
				   HrName(faultHr));
	} else if (fault.PageFaultVA == 0) {
		log::Error("DRED page fault: none - the GPU did not fault on an address");
	} else {
		log::Error("DRED page fault: GPU virtual address 0x{:016X}", fault.PageFaultVA);
		LogAllocations("live", fault.pHeadExistingAllocationNode);
		LogAllocations("recently freed", fault.pHeadRecentFreedAllocationNode);
	}
}

// Everything DRED holds for a removed device.
void LogDred(ID3D12Device* device) {
	ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
	if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
		log::Error("DRED: this runtime has no ID3D12DeviceRemovedExtendedData1 - no "
				   "breadcrumbs or page fault to report");
		return;
	}
	ComPtr<ID3D12DeviceRemovedExtendedData2> dred2;
	if (SUCCEEDED(dred.As(&dred2)))
		log::Error("DRED device state: {}", DeviceStateName(dred2->GetDeviceState()));

	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
	const HRESULT crumbsHr = dred->GetAutoBreadcrumbsOutput1(&crumbs);
	D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
	const HRESULT faultHr = dred->GetPageFaultAllocationOutput1(&fault);
	LogDredRecord(crumbs.pHeadAutoBreadcrumbNode, crumbsHr, fault, faultHr);
}

// The watched device's state. `always` is DN_HR's question - was it the
// device? - and answers even "no"; the crash note asks only whether there is a
// removal to report. A removal is logged once per process.
void LogDeviceState(bool always) {
	// The names and contexts below are strings built before each log call, on a
	// path that can open inside a guarded frame.
	const alloc::Excused excuse;
	ID3D12Device* device = g_device.load();
	if (!device) {
		if (always) log::Error("gpu device: none yet - the call failed before one was made");
		return;
	}
	const HRESULT reason = device->GetDeviceRemovedReason();
	if (reason == S_OK) {
		if (always) log::Error("gpu device: not removed - the call failed on its own");
		return;
	}
	if (g_removalLogged.exchange(true)) return;
	log::Error("gpu device removed: reason 0x{:08X} ({})", static_cast<u32>(reason),
			   HrName(reason));
	LogDred(device);
}

// The crash note WatchDevice installs (crash::SetFatalNote).
void NoteRemovedDevice() {
	LogDeviceState(false);
}

// A path to its file name: the full source path says nothing a line number
// does not, and widens every report line.
const char* BaseName(const char* path) {
	const char* base = path;
	for (const char* p = path; *p; ++p)
		if (*p == '\\' || *p == '/') base = p + 1;
	return base;
}

} // namespace

const char* HrName(HRESULT hr) {
	switch (hr) {
	case S_OK: return "S_OK";
	case E_FAIL: return "E_FAIL";
	case E_INVALIDARG: return "E_INVALIDARG";
	case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
	case E_NOTIMPL: return "E_NOTIMPL";
	case E_NOINTERFACE: return "E_NOINTERFACE";
	case E_POINTER: return "E_POINTER";
	case E_ACCESSDENIED: return "E_ACCESSDENIED";
	case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
	case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
	case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
	case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
	case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
	case DXGI_ERROR_NOT_FOUND: return "DXGI_ERROR_NOT_FOUND";
	case DXGI_ERROR_MORE_DATA: return "DXGI_ERROR_MORE_DATA";
	case DXGI_ERROR_UNSUPPORTED: return "DXGI_ERROR_UNSUPPORTED";
	case DXGI_ERROR_WAS_STILL_DRAWING: return "DXGI_ERROR_WAS_STILL_DRAWING";
	case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
	case DXGI_ERROR_ACCESS_LOST: return "DXGI_ERROR_ACCESS_LOST";
	case DXGI_ERROR_ACCESS_DENIED: return "DXGI_ERROR_ACCESS_DENIED";
	case DXGI_ERROR_SDK_COMPONENT_MISSING: return "DXGI_ERROR_SDK_COMPONENT_MISSING";
	case D3D12_ERROR_ADAPTER_NOT_FOUND: return "D3D12_ERROR_ADAPTER_NOT_FOUND";
	case D3D12_ERROR_DRIVER_VERSION_MISMATCH: return "D3D12_ERROR_DRIVER_VERSION_MISMATCH";
	default: return "unrecognised";
	}
}

void WatchDevice(ID3D12Device* device) {
	if (!device) return;
	g_device.store(device);
	crash::SetFatalNote(&NoteRemovedDevice);
}

void UnwatchDevice(ID3D12Device* device) {
	ID3D12Device* expected = device;
	if (device && g_device.compare_exchange_strong(expected, nullptr))
		crash::SetFatalNote(nullptr);
}

void LogDredSample() {
	const alloc::Excused excuse;
	log::Info("dredpoke: a made-up DRED record through the removal readout - the "
			  "breadcrumbs a real TDR leaves");
	// The frame list stopped mid-scene at #7, a marker naming the pass just
	// before; the window shows #3 to #11, four ops either side, and leaves the
	// first three and the last two out. Behind it the NEXT frame's recording of
	// the same list, queued and never begun (0 completed: one line, no marker),
	// then an immediate list that finished.
	static constexpr D3D12_AUTO_BREADCRUMB_OP kFrameOps[] = {
		D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER,
		D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW,
		D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW,
		D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED,
		D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER,
		D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED,
		D3D12_AUTO_BREADCRUMB_OP_SETMARKER,
		D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED,
		D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED,
		D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER,
		D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED,
		D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER,
		D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED,
		D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER,
	};
	static constexpr D3D12_AUTO_BREADCRUMB_OP kImmediateOps[] = {
		D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER,
		D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION,
		D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER,
	};
	static constexpr UINT kFrameDone = 7;
	static constexpr UINT kQueuedDone = 0;
	static constexpr UINT kImmediateDone = 3;
	D3D12_DRED_BREADCRUMB_CONTEXT contexts[] = {{6, L"scene"}};

	D3D12_AUTO_BREADCRUMB_NODE1 immediate{};
	immediate.pCommandListDebugNameW = L"immediate";
	immediate.pCommandQueueDebugNameW = L"direct queue";
	immediate.BreadcrumbCount = static_cast<UINT>(std::size(kImmediateOps));
	immediate.pLastBreadcrumbValue = &kImmediateDone;
	immediate.pCommandHistory = kImmediateOps;

	D3D12_AUTO_BREADCRUMB_NODE1 queued{};
	queued.pCommandListDebugNameW = L"frame";
	queued.pCommandQueueDebugNameW = L"direct queue";
	queued.BreadcrumbCount = static_cast<UINT>(std::size(kFrameOps));
	queued.pLastBreadcrumbValue = &kQueuedDone;
	queued.pCommandHistory = kFrameOps;
	queued.pNext = &immediate;

	D3D12_AUTO_BREADCRUMB_NODE1 frame{};
	frame.pCommandListDebugNameW = L"frame";
	frame.pCommandQueueDebugNameA = "direct queue";
	frame.BreadcrumbCount = static_cast<UINT>(std::size(kFrameOps));
	frame.pLastBreadcrumbValue = &kFrameDone;
	frame.pCommandHistory = kFrameOps;
	frame.pNext = &queued;
	frame.BreadcrumbContextsCount = static_cast<UINT>(std::size(contexts));
	frame.pBreadcrumbContexts = contexts;

	D3D12_DRED_ALLOCATION_NODE1 heap{};
	heap.AllocationType = D3D12_DRED_ALLOCATION_TYPE_HEAP;
	D3D12_DRED_ALLOCATION_NODE1 live{};
	live.ObjectNameW = L"scene color";
	live.AllocationType = D3D12_DRED_ALLOCATION_TYPE_RESOURCE;
	live.pNext = &heap;
	D3D12_DRED_ALLOCATION_NODE1 freed{};
	freed.ObjectNameA = "shadow cube 3";
	freed.AllocationType = D3D12_DRED_ALLOCATION_TYPE_RESOURCE;

	D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
	fault.PageFaultVA = 0x12340000;
	fault.pHeadExistingAllocationNode = &live;
	fault.pHeadRecentFreedAllocationNode = &freed;

	LogDredRecord(&frame, S_OK, fault, S_OK);
}

[[noreturn]] void FailHr(HRESULT hr, const char* expr, const char* file, int line) {
	const char* base = BaseName(file);
	// The whole expression here - the record's message below is capped at 192
	// characters and a D3D call's argument list easily fills it.
	log::Error("D3D12 call failed at {}:{}: {} returned 0x{:08X} ({})", base, line, expr,
			   static_cast<u32>(hr), HrName(hr));
	LogDeviceState(true);
	{
		const alloc::Excused excuse;
		crash::ReportFatal(std::format("D3D12 call failed: 0x{:08X} {} ({}:{})",
									   static_cast<u32>(hr), HrName(hr), base, line));
	}
	std::abort();
}

} // namespace dungeon::gfx
