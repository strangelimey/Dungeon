// ============================================================================
// Graphics/GpuProfiler.h — GPU timestamp queries, reported as one more thread.
//
// The CPU profiler kept saying the same thing about this engine: the frame is
// ~4 ms and almost all of it is present/vsync wait, with about 0.2 ms of actual
// CPU work. `scene` costs 8 us on the CPU because that is only COMMAND
// RECORDING — the GPU does the work afterwards, on its own timeline, and nothing
// on the CPU side can see it. This is the instrument for that half.
//
// HOW IT WORKS: a timestamp is written into a query heap before and after each
// marked pass, the pairs are resolved into a readback buffer at end of frame,
// and the results are read kFrameCount frames later, once that slot's fence has
// been waited on and the GPU is known to have finished writing them. A GPU
// timing is therefore always a few frames STALE, which is inherent — you cannot
// read a number the GPU has not written yet — and does not matter for a running
// average.
//
// WHERE THE RESULTS GO: into the profiler as an external slot named "gpu", via
// prof::OpenExternal and Collector::AddSpan. That is the whole reason the seam
// exists — the console's list view, the graph view with its twelve seconds of
// history, and the Perfetto dump all understand a thread, so the GPU becomes one
// and needs no readout of its own.
//
// TWO LIMITS, both deliberate for now:
// * Spans are FLAT. Passes are siblings anyway (shadows, scene, post, ui2d), and
//   flat means AddSpan's root placement is honest rather than double-counting a
//   parent's time. Nesting would need the tree to be rebuilt on the CPU side.
// * Durations are right; absolute PLACEMENT on the trace timeline is not. GPU
//   and CPU timestamps come from different clocks, and pairing them needs
//   ID3D12CommandQueue::GetClockCalibration. Until that lands the GPU rows carry
//   correct lengths accumulated per frame, which is what the console shows.
// ============================================================================
#pragma once

#include "Core/Profile.h"
#include "Core/Types.h"

#include <d3d12.h>
#include <wrl/client.h>

namespace dungeon::gfx {

inline constexpr u32 kMaxGpuZones = 16; // per frame; passes, not draw calls

class GpuProfiler {
public:
	// Mirrors GraphicsDevice's kFrameCount without including it — that header
	// includes THIS one. Init refuses a frameCount larger than this rather than
	// letting the two drift silently apart.
	static constexpr u32 kFrameCount_Max = 4;

public:
	// Safe to skip entirely: without Init every call below is a no-op and the
	// engine simply reports no GPU rows. Returns false if the queue cannot give a
	// timestamp frequency (WARP and some remote adapters).
	bool Init(ID3D12Device* device, ID3D12CommandQueue* queue, u32 frameCount);

	// Called by GraphicsDevice::BeginFrame AFTER the fence wait for `slot` — the
	// point at which the GPU has demonstrably finished the last frame that used
	// it, so its timestamps are readable.
	void BeginFrame(u32 slot);

	// Bracket a pass. Nesting is not supported (see the header note); a Push
	// while one is open is dropped rather than mis-parented.
	void Push(ID3D12GraphicsCommandList* list, const prof::Zone& zone);
	void Pop(ID3D12GraphicsCommandList* list);

	// Called by GraphicsDevice::EndFrame before the list is closed.
	void EndFrame(ID3D12GraphicsCommandList* list);

	bool Enabled() const { return m_enabled; }

private:
	template <typename T>
	using ComPtr = Microsoft::WRL::ComPtr<T>;

	struct Pending {
		const prof::Zone* zone = nullptr;
		u32 first = 0; // index of the start timestamp; the end is first + 1
	};

	ComPtr<ID3D12QueryHeap> m_heap;
	ComPtr<ID3D12Resource> m_readback;
	const u64* m_mapped = nullptr;

	// GPU ticks -> TSC ticks, so everything downstream can keep using one
	// conversion and one TicksToMs.
	f64 m_gpuToTsc = 0.0;

	prof::Collector* m_sink = nullptr;
	u32 m_frameCount = 0;
	u32 m_slot = 0;
	u32 m_used = 0; // queries issued this frame, within the slot's window
	bool m_enabled = false;
	bool m_open = false; // a Push is awaiting its Pop

	// What each slot recorded, so a readback three frames later still knows which
	// zone each pair of timestamps belonged to.
	Pending m_pending[kFrameCount_Max][kMaxGpuZones];
	u32 m_pendingCount[kFrameCount_Max] = {};
};

// RAII bracket for a pass. Does nothing if the profiler never initialised.
class GpuScope {
public:
	GpuScope(GpuProfiler& prof, ID3D12GraphicsCommandList* list, const prof::Zone& zone)
		: m_prof(prof), m_list(list) {
		m_prof.Push(m_list, zone);
	}
	~GpuScope() { m_prof.Pop(m_list); }

	GpuScope(const GpuScope&) = delete;
	GpuScope& operator=(const GpuScope&) = delete;

private:
	GpuProfiler& m_prof;
	ID3D12GraphicsCommandList* m_list;
};

} // namespace dungeon::gfx

// Marks a GPU pass. Mirrors DN_PROFILE_ZONE: the Zone is a function-scope static
// so its address is the identity, and the whole thing vanishes without
// DN_PROFILE.
#if DN_PROFILE
#define DN_GPU_ZONE(profiler_, list_, name_)                                              \
	static constexpr ::dungeon::prof::Zone DN_PROF_CAT(dnGpuZone_, __LINE__){              \
		name_, __FILE__, __LINE__, ::dungeon::prof::kLevelFrame};                          \
	::dungeon::gfx::GpuScope DN_PROF_CAT(dnGpuScope_, __LINE__)                            \
	{                                                                                      \
		profiler_, list_, DN_PROF_CAT(dnGpuZone_, __LINE__)                                \
	}
#else
#define DN_GPU_ZONE(profiler_, list_, name_) ((void)0)
#endif
