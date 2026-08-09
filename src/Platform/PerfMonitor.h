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

	// Moves EVERY OS query — CPU times, the PDH GPU counter, physical and process
	// memory — onto a managed worker. Call once, early; without it those readouts
	// simply stay at their defaults and only FPS is live.
	//
	// WHY NONE OF IT IS ON THE MAIN THREAD: measured with the profiler, the PDH
	// query alone cost ~578 us and fired every 333 ms — a metronome — and it was
	// paid in EVERY build whether the console was open or not, because Tick runs
	// before the console's own is-open check. A dev readout taxing shipping
	// frames. GetSystemTimes went with it at ~78 us; the memory calls are only a
	// few microseconds, but leaving them behind would mean keeping a whole second
	// throttle on the frame to run them, so the rule is simply that OS queries
	// happen on the worker and Tick only folds in what it published.
	void StartOsSampler(threads::Manager& manager);

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

	// The worker's results, published for Tick to fold into m_metrics. Relaxed is
	// enough: a readout one tick stale is not a readout that is wrong, and these
	// are independent numbers rather than a set that has to agree with itself.
	std::atomic<float> m_gpuPercent{-1.0f};
	std::atomic<float> m_cpuPercent{0.0f};
	std::atomic<double> m_sysMemUsedMB{0.0};
	std::atomic<double> m_sysMemTotalMB{0.0};
	std::atomic<double> m_procMemMB{0.0};
	threads::Manager* m_threads = nullptr;
	u32 m_gpuWorker = ~0u;
	// Scratch for the PDH counter array, kept across samples (see SampleGpu).
	std::vector<unsigned char> m_gpuBuffer;
};

} // namespace dungeon
