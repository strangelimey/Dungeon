#include "Platform/PerfMonitor.h"

#include "Core/Profile.h"
#include "Core/ThreadManager.h"

#include <Windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>

#include <algorithm>
#include <vector>

namespace dungeon {

namespace {
constexpr float kSampleInterval = 0.33f; // ~3 Hz for the OS queries
constexpr float kFpsWindow = 0.5f;       // FPS averaging/refresh window (2 Hz)

double ToMB(unsigned long long bytes) {
	return static_cast<double>(bytes) / (1024.0 * 1024.0);
}
unsigned long long FtToU64(const FILETIME& ft) {
	return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) |
		   ft.dwLowDateTime;
}
} // namespace

ProcessMemory QueryProcessMemory() {
	PROCESS_MEMORY_COUNTERS pmc{};
	if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return {};
	return {ToMB(pmc.WorkingSetSize), ToMB(pmc.PeakWorkingSetSize)};
}

PerfMonitor::PerfMonitor() {
	// GPU utilization via the PDH "GPU Engine" counter set (WDDM 2.x+). The
	// instance wildcard expands to one entry per (process, GPU engine); we sum
	// the 3D-engine instances at sample time. If any step fails the GPU figure
	// stays unavailable (gpuPercent < 0).
	PDH_HQUERY query = nullptr;
	if (PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS) {
		PDH_HCOUNTER counter = nullptr;
		if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage",
								  0, &counter) == ERROR_SUCCESS) {
			PdhCollectQueryData(query); // prime; first real read is next sample
			m_pdhQuery = query;
			m_pdhCounter = counter;
		} else {
			PdhCloseQuery(query);
		}
	}
}

PerfMonitor::~PerfMonitor() {
	// Stop BEFORE closing the query: the job holds `this` and touches the PDH
	// handles, so it has to be joined while they are still valid. Stop blocks
	// until the worker has finished its tick and been joined.
	if (m_threads && m_gpuWorker != ~0u) m_threads->Stop(m_gpuWorker);
	if (m_pdhQuery) PdhCloseQuery(static_cast<PDH_HQUERY>(m_pdhQuery));
}

void PerfMonitor::StartOsSampler(threads::Manager& manager) {
	if (m_threads) return; // already started
	m_threads = &manager;

	threads::Options opt;
	opt.name = "perf.os";
	opt.hz = 1.0f / kSampleInterval; // the same ~3 Hz, just not on the frame
	m_gpuWorker = manager.Spawn([this](const threads::Tick&) { Sample(); }, opt);
}

void PerfMonitor::Tick(float dt) {
	// FPS: average over a fixed window and latch the result once per window, so
	// the displayed number is steady rather than recomputed every frame.
	++m_fpsFrames;
	m_fpsElapsed += dt;
	if (m_fpsElapsed >= kFpsWindow) {
		m_metrics.fps = static_cast<float>(m_fpsFrames) / m_fpsElapsed;
		m_fpsFrames = 0;
		m_fpsElapsed = 0.0f;
	}

	// Whatever the worker last published. All of Tick is now FPS arithmetic and
	// five relaxed loads, where it used to carry every OS query in the readout.
	m_metrics.gpuPercent = m_gpuPercent.load(std::memory_order_relaxed);
	m_metrics.cpuPercent = m_cpuPercent.load(std::memory_order_relaxed);
	m_metrics.sysMemUsedMB = m_sysMemUsedMB.load(std::memory_order_relaxed);
	m_metrics.sysMemTotalMB = m_sysMemTotalMB.load(std::memory_order_relaxed);
	m_metrics.procMemMB = m_procMemMB.load(std::memory_order_relaxed);
}

// Runs on the perf.os WORKER, never on the frame. Nothing here touches
// m_metrics: that belongs to whoever calls Tick, and each result is published
// through its own atomic instead.
//
// Level-2 zones, so they cost nothing until someone raises detail on this
// worker. They are what found the cost in the first place — a periodic hitch is
// invisible in an average and only shows as a RHYTHM.
void PerfMonitor::Sample() {
	{
		DN_PROFILE_ZONE_L(prof::kLevelDetail, "os.cpu");
		SampleCpu();
	}
	{
		DN_PROFILE_ZONE_L(prof::kLevelDetail, "os.gpu");
		SampleGpu();
	}
	DN_PROFILE_ZONE_L(prof::kLevelDetail, "os.mem");

	MEMORYSTATUSEX mem{};
	mem.dwLength = sizeof(mem);
	if (GlobalMemoryStatusEx(&mem)) {
		m_sysMemTotalMB.store(ToMB(mem.ullTotalPhys), std::memory_order_relaxed);
		m_sysMemUsedMB.store(ToMB(mem.ullTotalPhys - mem.ullAvailPhys),
							 std::memory_order_relaxed);
	}

	PROCESS_MEMORY_COUNTERS pmc{};
	if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
		m_procMemMB.store(ToMB(pmc.WorkingSetSize), std::memory_order_relaxed);
}

void PerfMonitor::SampleCpu() {
	FILETIME idle, kernel, user;
	if (!GetSystemTimes(&idle, &kernel, &user)) return;
	const unsigned long long i = FtToU64(idle);
	const unsigned long long k = FtToU64(kernel); // kernel time includes idle
	const unsigned long long u = FtToU64(user);
	if (m_haveCpuBaseline) {
		const unsigned long long idleD = i - m_prevIdle;
		const unsigned long long total = (k - m_prevKernel) + (u - m_prevUser);
		if (total > 0) {
			const double busy = static_cast<double>(total - idleD) /
								static_cast<double>(total);
			m_cpuPercent.store(static_cast<float>(std::clamp(busy, 0.0, 1.0) * 100.0),
							   std::memory_order_relaxed);
		}
	}
	m_prevIdle = i;
	m_prevKernel = k;
	m_prevUser = u;
	m_haveCpuBaseline = true;
}

void PerfMonitor::SampleGpu() {
	if (!m_pdhQuery) return;
	auto query = static_cast<PDH_HQUERY>(m_pdhQuery);
	auto counter = static_cast<PDH_HCOUNTER>(m_pdhCounter);
	if (PdhCollectQueryData(query) != ERROR_SUCCESS) return;

	DWORD bufferSize = 0, itemCount = 0;
	if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufferSize,
									 &itemCount, nullptr) != PDH_MORE_DATA ||
		bufferSize == 0)
		return;

	// Retained capacity: PDH wants a byte buffer whose size it dictates, and the
	// instance list barely changes, so the member vector settles after the first
	// sample instead of allocating one per sample inside a steady frame.
	if (m_gpuBuffer.size() < bufferSize) m_gpuBuffer.resize(bufferSize);
	auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(m_gpuBuffer.data());
	if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufferSize,
									 &itemCount, items) != ERROR_SUCCESS)
		return;

	double sum = 0.0;
	for (DWORD n = 0; n < itemCount; ++n) {
		if (items[n].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA &&
			items[n].szName && wcsstr(items[n].szName, L"engtype_3D"))
			sum += items[n].FmtValue.doubleValue;
	}
	// Published rather than written into m_metrics: this runs on the worker, and
	// m_metrics belongs to whoever calls Tick.
	m_gpuPercent.store(static_cast<float>(std::clamp(sum, 0.0, 100.0)),
					   std::memory_order_relaxed);
}

} // namespace dungeon
