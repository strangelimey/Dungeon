// ============================================================================
// Graphics/GpuProfiler.cpp — see GpuProfiler.h.
// ============================================================================
#include "Graphics/GpuProfiler.h"

#include "Core/Assert.h"
#include "Core/Log.h"

#include <format>

namespace dungeon::gfx {

// The whole implementation is a profiling feature, so it splits on the
// preprocessor like the rest: without DN_PROFILE these are empty stubs and
// prof::Collector is not even a complete type to call AddSpan on.
#if DN_PROFILE

namespace {
// Two timestamps per zone: one before the pass, one after.
constexpr u32 kQueriesPerZone = 2;
} // namespace

bool GpuProfiler::Init(ID3D12Device* device, ID3D12CommandQueue* queue, u32 frameCount) {
	if (!device || !queue || frameCount == 0 || frameCount > kFrameCount_Max) return false;

	// A queue that cannot timestamp is not an error worth failing over — the
	// engine simply has no GPU rows. WARP reports this.
	u64 gpuHz = 0;
	if (FAILED(queue->GetTimestampFrequency(&gpuHz)) || gpuHz == 0) {
		log::Write(log::Level::Warn,
				   "GpuProfiler: queue has no timestamp frequency; GPU timings off");
		return false;
	}

	m_frameCount = frameCount;
	const u32 perSlot = kMaxGpuZones * kQueriesPerZone;
	const u32 total = perSlot * frameCount;

	D3D12_QUERY_HEAP_DESC hd{};
	hd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	hd.Count = total;
	if (FAILED(device->CreateQueryHeap(&hd, IID_PPV_ARGS(&m_heap)))) return false;

	D3D12_HEAP_PROPERTIES hp{};
	hp.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC rd{};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rd.Width = static_cast<u64>(total) * sizeof(u64);
	rd.Height = 1;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.Format = DXGI_FORMAT_UNKNOWN;
	rd.SampleDesc.Count = 1;
	rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
											   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
											   IID_PPV_ARGS(&m_readback))))
		return false;

	// Mapped once and left mapped: a readback buffer is CPU-visible, and
	// re-mapping every frame to read eight bytes per pass would cost more than
	// the numbers are worth.
	void* p = nullptr;
	if (FAILED(m_readback->Map(0, nullptr, &p))) return false;
	m_mapped = static_cast<const u64*>(p);

	// The conversion that lets everything downstream keep ONE unit. A GPU tick is
	// worth (tscHz / gpuHz) TSC ticks, so a span converted here reads in
	// milliseconds through the same TicksToMs as every CPU zone.
	const prof::Clock clock = prof::ClockInfo();
	const f64 tscHz = clock.ticksPerNs * 1e9;
	m_gpuToTsc = tscHz / static_cast<f64>(gpuHz);

	m_sink = prof::OpenExternal("gpu");
	m_enabled = m_sink != nullptr;
	if (m_enabled)
		log::Write(log::Level::Info,
				   std::format("GpuProfiler: {:.1f} MHz timestamps, {} zones/frame",
							   static_cast<f64>(gpuHz) / 1e6, kMaxGpuZones));
	return m_enabled;
}

void GpuProfiler::BeginFrame(u32 slot) {
	if (!m_enabled) return;
	DN_ASSERT(slot < m_frameCount, "GPU profiler frame slot out of range");

	// Read what this slot recorded LAST time round. Its fence has just been
	// waited on, so the GPU has finished writing these; that is the whole reason
	// the read happens here and not at the end of the frame that issued them.
	const u32 count = m_pendingCount[slot];
	const u32 base = slot * kMaxGpuZones * kQueriesPerZone;
	for (u32 i = 0; i < count; ++i) {
		const Pending& p = m_pending[slot][i];
		const u64 t0 = m_mapped[base + p.first];
		const u64 t1 = m_mapped[base + p.first + 1];
		if (t1 <= t0 || !p.zone) continue; // a slot that never ran, or a wrapped counter

		const u64 tsc = static_cast<u64>(static_cast<f64>(t1 - t0) * m_gpuToTsc);
		m_sink->AddSpan(*p.zone, tsc);
	}

	// One publish per frame, matching what the main thread does for itself, so
	// the GPU rows advance on the same cadence as everything beside them.
	if (count > 0) prof::PublishExternal(m_sink);

	m_slot = slot;
	m_used = 0;
	m_pendingCount[slot] = 0;
	m_open = false;
}

void GpuProfiler::Push(ID3D12GraphicsCommandList* list, const prof::Zone& zone) {
	if (!m_enabled || !list) return;
	// Nesting is not supported, and a dropped Push is better than a span
	// attributed to the wrong pass. Same for running out of zones.
	if (m_open || m_used >= kMaxGpuZones) return;

	const u32 base = m_slot * kMaxGpuZones * kQueriesPerZone;
	const u32 first = m_used * kQueriesPerZone;
	list->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base + first);

	Pending& p = m_pending[m_slot][m_used];
	p.zone = &zone;
	p.first = first;
	m_open = true;
}

void GpuProfiler::Pop(ID3D12GraphicsCommandList* list) {
	if (!m_enabled || !list || !m_open) return;

	const u32 base = m_slot * kMaxGpuZones * kQueriesPerZone;
	const u32 first = m_pending[m_slot][m_used].first;
	list->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base + first + 1);

	++m_used;
	m_pendingCount[m_slot] = m_used;
	m_open = false;
}

void GpuProfiler::EndFrame(ID3D12GraphicsCommandList* list) {
	if (!m_enabled || !list || m_used == 0) return;

	// Resolve only what this frame actually used. The destination offset keeps
	// each slot's results in its own window, so a frame in flight cannot
	// overwrite the numbers another frame is about to be read for.
	const u32 base = m_slot * kMaxGpuZones * kQueriesPerZone;
	list->ResolveQueryData(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base,
						   m_used * kQueriesPerZone, m_readback.Get(),
						   static_cast<u64>(base) * sizeof(u64));
}

#else

bool GpuProfiler::Init(ID3D12Device*, ID3D12CommandQueue*, u32) { return false; }
void GpuProfiler::BeginFrame(u32) {}
void GpuProfiler::Push(ID3D12GraphicsCommandList*, const prof::Zone&) {}
void GpuProfiler::Pop(ID3D12GraphicsCommandList*) {}
void GpuProfiler::EndFrame(ID3D12GraphicsCommandList*) {}

#endif // DN_PROFILE

} // namespace dungeon::gfx
