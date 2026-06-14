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

namespace dungeon {

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

	// PDH GPU query handles (void* keeps <pdh.h> out of the header).
	void* m_pdhQuery = nullptr;
	void* m_pdhCounter = nullptr;
};

} // namespace dungeon
