#include "Platform/PerfMonitor.h"

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
	if (m_pdhQuery) PdhCloseQuery(static_cast<PDH_HQUERY>(m_pdhQuery));
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

	m_sampleTimer += dt;
	if (m_sampleTimer >= kSampleInterval) {
		m_sampleTimer = 0.0f;
		Sample();
	}
}

void PerfMonitor::Sample() {
	SampleCpu();
	SampleGpu();

	MEMORYSTATUSEX mem{};
	mem.dwLength = sizeof(mem);
	if (GlobalMemoryStatusEx(&mem)) {
		m_metrics.sysMemTotalMB = ToMB(mem.ullTotalPhys);
		m_metrics.sysMemUsedMB = ToMB(mem.ullTotalPhys - mem.ullAvailPhys);
	}

	PROCESS_MEMORY_COUNTERS pmc{};
	if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
		m_metrics.procMemMB = ToMB(pmc.WorkingSetSize);
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
			m_metrics.cpuPercent =
				static_cast<float>(std::clamp(busy, 0.0, 1.0) * 100.0);
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

	std::vector<unsigned char> buffer(bufferSize);
	auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
	if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufferSize,
									 &itemCount, items) != ERROR_SUCCESS)
		return;

	double sum = 0.0;
	for (DWORD n = 0; n < itemCount; ++n) {
		if (items[n].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA &&
			items[n].szName && wcsstr(items[n].szName, L"engtype_3D"))
			sum += items[n].FmtValue.doubleValue;
	}
	m_metrics.gpuPercent = static_cast<float>(std::clamp(sum, 0.0, 100.0));
}

} // namespace dungeon
