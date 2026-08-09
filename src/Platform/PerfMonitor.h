// ============================================================================
// Platform/PerfMonitor.h — lightweight performance sampler for the dev console.
//
// FPS updates every Tick (cheap, a smoothed frame rate); the OS-level metrics
// (system-wide CPU %, physical memory, this process's working set, and GPU 3D
// utilization via PDH) are sampled on a throttle (~3 Hz) since each involves
// syscalls or a perf-counter query. GPU memory and the adapter name are NOT
// sampled here — they come from the Graphics device and the console merges
// them into the display.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <atomic>
#include <vector>

namespace dungeon {

namespace threads {
class Manager;
}


// This process's working set right now, and the highest it has ever been, in
// MB. The peak is the OS's own high-water mark (it survives a drop back), which
// is what makes it worth reporting after a staged load: it says what the load
// actually demanded, not what it kept.
struct ProcessMemory {
	double workingSetMB = 0.0;
	double peakWorkingSetMB = 0.0;
};
ProcessMemory QueryProcessMemory();

class PerfMonitor {
public:
	PerfMonitor();
	~PerfMonitor();

	PerfMonitor(const PerfMonitor&) = delete;
	PerfMonitor& operator=(const PerfMonitor&) = delete;

	struct Metrics {
		float fps = 0.0f;
		float cpuPercent = 0.0f;  // system-wide, 0..100
		float gpuPercent = -1.0f; // 3D engine, 0..100; < 0 means unavailable
		double sysMemUsedMB = 0.0;
		double sysMemTotalMB = 0.0;
		double procMemMB = 0.0; // this process's working set
	};

	// Moves the GPU-engine query onto a managed worker. Call once, early; without
	// it the GPU readout simply stays unavailable.
	//
	// WHY IT IS NOT ON THE MAIN THREAD: measured with the profiler, the PDH query
	// costs ~578 us and fires every 333 ms — a metronome, and paid in EVERY build
	// whether the console is open or not, because Tick runs before the console's
	// own is-open check. It is a dev readout taxing shipping frames. PDH has to
	// enumerate every GPU engine instance to find the 3D ones, so the cost is
	// inherent to the counter rather than something to tune away; moving it off
	// the frame is the fix.
	void StartGpuWorker(threads::Manager& manager);

	// dt in seconds; advances the FPS average every call and runs the throttled
	// OS queries when enough time has elapsed.
	void Tick(float dt);
	const Metrics& Get() const { return m_metrics; }

private:
	void Sample();    // the throttled OS queries (CPU/RAM/GPU)
	void SampleCpu(); // GetSystemTimes deltas
	void SampleGpu(); // PDH GPU-engine counter

	Metrics m_metrics;

	// FPS over a fixed display window (frames / elapsed), latched at the end of
	// each window so the shown number is readable instead of churning per frame.
	int m_fpsFrames = 0;
	float m_fpsElapsed = 0.0f;
	float m_sampleTimer = 1.0f;  // forces a sample on the first Tick

	// CPU: previous GetSystemTimes snapshot, as 100-ns tick counts.
	unsigned long long m_prevIdle = 0;
	unsigned long long m_prevKernel = 0;
	unsigned long long m_prevUser = 0;
	bool m_haveCpuBaseline = false;

	// PDH GPU query handles (void* keeps <pdh.h> out of the header). Touched by
	// the worker after StartGpuWorker, and by nothing else — the destructor stops
	// the worker before closing the query.
	void* m_pdhQuery = nullptr;
	void* m_pdhCounter = nullptr;

	// The worker's result, published for Tick to fold into m_metrics. Relaxed is
	// enough: a readout one tick stale is not a readout that is wrong.
	std::atomic<float> m_gpuPercent{-1.0f};
	threads::Manager* m_threads = nullptr;
	u32 m_gpuWorker = ~0u;
	// Scratch for the PDH counter array, kept across samples (see SampleGpu).
	std::vector<unsigned char> m_gpuBuffer;
};

} // namespace dungeon
