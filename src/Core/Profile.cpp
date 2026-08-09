// ============================================================================
// Core/Profile.cpp — the slot table, the clock calibration, and publishing.
//
// The whole file is inside #if DN_PROFILE apart from the stubs at the bottom, so
// a non-profiling build links empty functions and declares none of the storage.
//
// STORAGE: kMaxThreads slots, statically allocated, each holding a Collector and
// the NodeView copy a reader gets. This is the point of the design — see the
// header's note on TerminateThread. A thread holds only a pointer into this
// array, so a worker that is force-killed leaves its last tree intact here
// rather than taking it down with its TLS.
//
// LOCKING: one mutex PER SLOT, covering that slot's published copy only. It is
// taken by the owning thread when it publishes (once a frame or tick) and by a
// reader in SnapshotAll (once a console draw). It is never taken by Enter or
// Exit, so a reader cannot stall a thread mid-scope. The slot TABLE itself is
// only mutated by registration, which is rare and takes the table mutex.
// Lock order, if both are ever wanted: table mutex, then a slot's.
// ============================================================================
#include "Core/Profile.h"

#include "Core/Log.h"

#include <cstring>
#include <mutex>

#if DN_PROFILE
#include <intrin.h>
#include <windows.h>
#endif

namespace dungeon::prof {

#if DN_PROFILE

namespace {

struct Slot {
	Collector collector;

	// The reader's copy. Sized like the pool because a tree is free to fill it.
	NodeView published[kMaxNodes];
	u32 publishedCount = 0;
	u32 publishedRoot = kInvalidNode;
	u64 periods = 0;
	u64 nodeOverflows = 0;
	u64 depthOverflows = 0;

	std::mutex mx; // guards the four `published*` fields and the counts above

	char name[32] = {};
	u32 osId = 0;
	bool used = false;
	bool live = false;
};

// ~2 MB in a profiling build, and not declared at all in any other. Deliberately
// static rather than heap: the profiler must not be the thing that allocates.
Slot g_slots[kMaxThreads];
std::mutex g_tableMx;
bool g_inited = false;

Clock g_clock;

bool InvariantTsc() {
	int regs[4] = {};
	__cpuid(regs, 0x80000000);
	if (static_cast<unsigned>(regs[0]) < 0x80000007u) return false;
	__cpuid(regs, 0x80000007);
	return (regs[3] & (1 << 8)) != 0; // EDX bit 8
}

// Watches both clocks over a short window to learn the TSC's rate. QPC's own
// tick is coarse (100 ns on the development machine) but over 50 ms that is a
// rounding error, and this runs once.
void Calibrate() {
	LARGE_INTEGER freq{}, q0{}, q1{};
	QueryPerformanceFrequency(&freq);

	QueryPerformanceCounter(&q0);
	const u64 t0 = __rdtsc();
	Sleep(50);
	const u64 t1 = __rdtsc();
	QueryPerformanceCounter(&q1);

	const f64 seconds =
		static_cast<f64>(q1.QuadPart - q0.QuadPart) / static_cast<f64>(freq.QuadPart);
	const f64 ticks = static_cast<f64>(t1 - t0);

	g_clock.invariantTsc = InvariantTsc();
	g_clock.ticksPerNs = seconds > 0.0 ? ticks / (seconds * 1e9) : 1.0;
	g_clock.mhz = g_clock.ticksPerNs * 1000.0;
}

} // namespace

thread_local Collector* t_collector = nullptr;

// ----------------------------------------------------------------------------
u32 Collector::CopyAndReset(NodeView* out, u32 capacity) {
	const u32 n = m_nodeCount < capacity ? m_nodeCount : capacity;
	for (u32 i = 0; i < n; ++i) {
		Node& src = m_nodes[i];
		NodeView& dst = out[i];
		dst.zone = src.zone;
		dst.parent = src.parent;
		dst.firstChild = src.firstChild;
		dst.nextSibling = src.nextSibling;
		dst.inclusive = src.inclusive;
		dst.childTime = src.childTime;
		dst.calls = src.calls;
		dst.maxTicks = src.maxTicks;
		dst.detail = src.detail.load(std::memory_order_relaxed);

		// Structure and identity stay; only the period's numbers reset.
		src.inclusive = 0;
		src.childTime = 0;
		src.calls = 0;
		src.maxTicks = 0;
	}
	++m_periods;
	return n;
}

// ----------------------------------------------------------------------------
void Init() {
	{
		std::lock_guard lock(g_tableMx);
		if (g_inited) return;
		g_inited = true;
	}

	Calibrate();
	if (!g_clock.invariantTsc) {
		// Not fatal: the numbers are still self-consistent within a run, they
		// just drift with the core clock. Worth shouting about, because every
		// figure the profiler reports downstream inherits the doubt.
		log::Write(log::Level::Warn,
				   "Profile: CPU reports NO invariant TSC. Timings may drift with clock "
				   "speed or across cores.");
	}
	log::Write(log::Level::Info,
			   std::format("Profile: TSC {:.1f} MHz, invariant {}", g_clock.mhz,
						   g_clock.invariantTsc ? "yes" : "NO"));

	RegisterThread("main");
}

void RegisterThread(std::string_view name) {
	if (t_collector) return; // already registered; naming it twice is a no-op

	std::lock_guard lock(g_tableMx);
	for (u32 i = 0; i < kMaxThreads; ++i) {
		Slot& s = g_slots[i];
		if (s.used) continue;

		s.used = true;
		s.live = true;
		s.osId = GetCurrentThreadId();
		const size_t n = name.size() < sizeof(s.name) - 1 ? name.size() : sizeof(s.name) - 1;
		std::memcpy(s.name, name.data(), n);
		s.name[n] = '\0';

		t_collector = &s.collector;
		return;
	}
	// Past capacity we simply do not measure this thread, the same call this
	// engine's other per-thread registry makes (alloc::RegisterThread).
	log::Write(log::Level::Warn,
			   std::format("Profile: no free slot for thread '{}' ({} max); not measured",
						   name, kMaxThreads));
}

void PublishThisThread() {
	Collector* c = t_collector;
	if (!c) return;

	// Find our slot. kMaxThreads is 16 and this runs once a frame, so a scan is
	// cheaper than carrying an index through the thread_local.
	for (u32 i = 0; i < kMaxThreads; ++i) {
		Slot& s = g_slots[i];
		if (&s.collector != c) continue;

		std::lock_guard lock(s.mx);
		s.publishedCount = c->CopyAndReset(s.published, kMaxNodes);
		s.publishedRoot = c->Root();
		s.periods = c->Periods();
		s.nodeOverflows = c->NodeOverflows();
		s.depthOverflows = c->DepthOverflows();
		return;
	}
}

int SnapshotAll(ThreadReport* out, int capacity) {
	int written = 0;
	std::lock_guard table(g_tableMx);
	for (u32 i = 0; i < kMaxThreads && written < capacity; ++i) {
		Slot& s = g_slots[i];
		if (!s.used) continue;

		std::lock_guard lock(s.mx);
		ThreadReport& r = out[written++];
		std::memcpy(r.name, s.name, sizeof(r.name));
		r.osThreadId = s.osId;
		r.nodes = s.published;
		r.nodeCount = s.publishedCount;
		r.root = s.publishedRoot;
		r.periods = s.periods;
		r.nodeOverflows = s.nodeOverflows;
		r.depthOverflows = s.depthOverflows;
		r.live = s.live;
	}
	return written;
}

Clock ClockInfo() { return g_clock; }

#else // !DN_PROFILE

void Init() {}
void RegisterThread(std::string_view) {}
void PublishThisThread() {}
int SnapshotAll(ThreadReport*, int) { return 0; }
Clock ClockInfo() { return {}; }

#endif // DN_PROFILE

} // namespace dungeon::prof
