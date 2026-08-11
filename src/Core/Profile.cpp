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

#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

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
	g_clock.epochTsc = t0;
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

namespace {

// Copies a name into a slot's fixed storage and claims it for this thread.
void ClaimSlot(Slot& s, std::string_view name) {
	s.used = true;
	s.live = true;
	s.osId = GetCurrentThreadId();
	const size_t n = name.size() < sizeof(s.name) - 1 ? name.size() : sizeof(s.name) - 1;
	std::memcpy(s.name, name.data(), n);
	s.name[n] = '\0';
	t_collector = &s.collector;
}

} // namespace

void RegisterThread(std::string_view name) {
	if (t_collector) return; // already registered; naming it twice is a no-op

	std::lock_guard lock(g_tableMx);

	// A dormant slot of the same name first: this is a worker being rebooted, and
	// reusing its slot is what stops Restart (and the supervisor's automatic
	// reboot of a stalled worker) from exhausting the table. Only a slot whose
	// thread UNREGISTERED is eligible — one still flagged live belongs either to
	// a running thread or to a force-killed one whose tree we are keeping.
	for (u32 i = 0; i < kMaxThreads; ++i) {
		Slot& s = g_slots[i];
		if (!s.used || s.live) continue;
		if (name != s.name) continue;

		std::lock_guard slotLock(s.mx);
		s.collector.Reset();
		s.publishedCount = 0;
		s.publishedRoot = kInvalidNode;
		s.periods = 0;
		s.nodeOverflows = 0;
		s.depthOverflows = 0;
		ClaimSlot(s, name);
		return;
	}

	for (u32 i = 0; i < kMaxThreads; ++i) {
		Slot& s = g_slots[i];
		if (s.used) continue;
		ClaimSlot(s, name);
		return;
	}
	// Past capacity we simply do not measure this thread, the same call this
	// engine's other per-thread registry makes (alloc::RegisterThread).
	log::Write(log::Level::Warn,
			   std::format("Profile: no free slot for thread '{}' ({} max); not measured",
						   name, kMaxThreads));
}

void UnregisterThisThread() {
	Collector* c = t_collector;
	if (!c) return;

	std::lock_guard lock(g_tableMx);
	for (u32 i = 0; i < kMaxThreads; ++i) {
		Slot& s = g_slots[i];
		if (&s.collector != c) continue;
		// `used` stays set and the published tree stays put, so a reader can still
		// see what this thread was doing when it stopped.
		s.live = false;
		break;
	}
	t_collector = nullptr;
}

namespace {

// Copies one slot's live tree into its published copy. Both publish paths — a
// thread publishing its own, and an external source being published for — do
// exactly this once they know WHICH slot.
void PublishSlot(Slot& s) {
	std::lock_guard lock(s.mx);
	s.publishedCount = s.collector.CopyAndReset(s.published, kMaxNodes);
	s.publishedRoot = s.collector.Root();
	s.periods = s.collector.Periods();
	s.nodeOverflows = s.collector.NodeOverflows();
	s.depthOverflows = s.collector.DepthOverflows();
}

// The slot a Collector belongs to. kMaxThreads is 16 and these run once a frame,
// so a scan beats threading an index through every caller.
Slot* SlotFor(const Collector* c) {
	for (u32 i = 0; i < kMaxThreads; ++i)
		if (&g_slots[i].collector == c) return &g_slots[i];
	return nullptr;
}

} // namespace

void PublishThisThread() {
	if (Slot* s = t_collector ? SlotFor(t_collector) : nullptr) PublishSlot(*s);
}

Collector* OpenExternal(std::string_view name) {
	std::lock_guard lock(g_tableMx);

	// By name, so asking twice hands back the same slot rather than opening a
	// second one that then collects half the spans.
	for (u32 i = 0; i < kMaxThreads; ++i)
		if (g_slots[i].used && name == g_slots[i].name) return &g_slots[i].collector;

	for (u32 i = 0; i < kMaxThreads; ++i) {
		Slot& s = g_slots[i];
		if (s.used) continue;
		s.used = true;
		s.live = true;
		s.osId = 0; // no thread owns it
		const size_t n = name.size() < sizeof(s.name) - 1 ? name.size() : sizeof(s.name) - 1;
		std::memcpy(s.name, name.data(), n);
		s.name[n] = 0;
		return &s.collector;
	}
	log::Write(log::Level::Warn,
		       std::format("Profile: no free slot for external source '{}'", name));
	return nullptr;
}

void PublishExternal(Collector* collector) {
	if (!collector) return;
	if (Slot* s = SlotFor(collector)) PublishSlot(*s);
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
		r.slot = i;
		r.nodes = s.published;
		r.nodeCount = s.publishedCount;
		r.root = s.publishedRoot;
		r.periods = s.periods;
		r.nodeOverflows = s.nodeOverflows;
		r.depthOverflows = s.depthOverflows;
		r.baseThreshold = s.collector.BaseThreshold();
		r.live = s.live;
	}
	return written;
}

// ----------------------------------------------------------------------------
namespace {

// Next '/'-separated segment of `path`, advancing it past the separator.
std::string_view NextSegment(std::string_view& path) {
	const size_t slash = path.find('/');
	std::string_view seg = path.substr(0, slash);
	path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
	return seg;
}

// Walks `path` down one thread's PUBLISHED tree and returns the node index, or
// kInvalidNode. Resolving against the published copy rather than the live tree
// is what makes this safe to do from the console while the owning thread runs:
// the copy is stable under the slot lock, and node INDICES match the live pool
// one-for-one because publishing copies node i to slot i and the tree only ever
// grows.
u32 ResolvePath(const Slot& s, std::string_view path) {
	u32 node = kInvalidNode;
	u32 candidates = s.publishedRoot;

	while (!path.empty()) {
		const std::string_view seg = NextSegment(path);
		if (seg.empty()) continue;

		u32 found = kInvalidNode;
		for (u32 c = candidates; c != kInvalidNode; c = s.published[c].nextSibling) {
			const Zone* z = s.published[c].zone;
			if (z && seg == z->name) {
				found = c;
				break;
			}
		}
		if (found == kInvalidNode) return kInvalidNode;
		node = found;
		candidates = s.published[found].firstChild;
	}
	return node;
}

} // namespace

int SetDetail(std::string_view path, i8 level) {
	int matches = 0;
	std::lock_guard table(g_tableMx);
	for (u32 i = 0; i < kMaxThreads; ++i) {
		Slot& s = g_slots[i];
		if (!s.used) continue;

		std::lock_guard lock(s.mx);
		const u32 node = ResolvePath(s, path);
		if (node == kInvalidNode) continue;

		// Relaxed: the owning thread reading this a tick late is the intended
		// behaviour, not a race.
		s.collector.SetDetail(node, level);
		++matches;
	}
	return matches;
}

bool SetDetailNode(u32 slot, u32 node, i8 level) {
	if (slot >= kMaxThreads) return false;

	std::lock_guard table(g_tableMx);
	Slot& s = g_slots[slot];
	if (!s.used) return false;

	// Bounded against the PUBLISHED count, which is what the caller's snapshot
	// showed it. Collector::SetDetail bounds against the live count as well, so a
	// node published and then lost to a Reset is refused there rather than
	// writing into a stale entry.
	std::lock_guard lock(s.mx);
	if (node >= s.publishedCount) return false;

	s.collector.SetDetail(node, level);
	return true;
}

int ListDetails(DetailEntry* out, int capacity) {
	int count = 0;
	std::lock_guard table(g_tableMx);
	for (u32 i = 0; i < kMaxThreads && count < capacity; ++i) {
		Slot& s = g_slots[i];
		if (!s.used) continue;

		std::lock_guard lock(s.mx);
		for (u32 n = 0; n < s.publishedCount && count < capacity; ++n) {
			if (s.published[n].detail < 0) continue;

			DetailEntry& e = out[count++];
			std::memcpy(e.thread, s.name, sizeof(e.thread));
			e.level = s.published[n].detail;

			// The path is rebuilt by walking UP to the root and then reversing —
			// a node knows its parent, not its children's names.
			const char* parts[kMaxDepth];
			int depth = 0;
			for (u32 at = n; at != kInvalidNode && depth < static_cast<int>(kMaxDepth);
				 at = s.published[at].parent)
				parts[depth++] = s.published[at].zone ? s.published[at].zone->name : "?";

			size_t w = 0;
			for (int d = depth - 1; d >= 0 && w + 1 < sizeof(e.path); --d) {
				if (w > 0 && w + 1 < sizeof(e.path)) e.path[w++] = '/';
				for (const char* c = parts[d]; *c && w + 1 < sizeof(e.path); ++c)
					e.path[w++] = *c;
			}
			e.path[w] = '\0';
		}
	}
	return count;
}

// ----------------------------------------------------------------------------
// Chrome Trace Event JSON. "B"/"E" are begin/end of a duration on one thread,
// `ts` is microseconds on a shared timeline, and a "M" metadata event names each
// thread so the viewer shows "ai.bucket0" instead of a number.
TraceStats DumpTrace(const char* path) {
	TraceStats stats;

	std::FILE* f = nullptr;
	if (fopen_s(&f, path, "wb") != 0 || !f) {
		log::Write(log::Level::Warn, std::format("Profile: cannot write trace to {}", path));
		return stats;
	}
	std::fputs("{\"traceEvents\":[\n", f);

	bool first = true;
	u64 minTsc = ~0ull, maxTsc = 0;

	std::lock_guard table(g_tableMx);
	for (u32 i = 0; i < kMaxThreads; ++i) {
		Slot& s = g_slots[i];
		if (!s.used) continue;
		++stats.threads;

		// The window still in the ring: everything from `written - capacity` up,
		// or from zero if it has not wrapped yet.
		const u64 written = s.collector.EventsWritten();
		const u64 oldest = written > kRingEvents ? written - kRingEvents : 0;

		std::fprintf(f,
					 "%s{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":%u,"
					 "\"args\":{\"name\":\"%s\"}}",
					 first ? "" : ",\n", s.osId, s.name);
		first = false;

		// Replaying needs a stack because an exit event carries no zone — it
		// closes whatever is innermost. An exit with nothing open is the head of
		// a wrapped ring: its enter is simply gone, so it is counted and dropped
		// rather than attributed to the wrong scope.
		const Zone* open[kMaxDepth];
		u32 depth = 0;

		for (u64 at = oldest; at < written; ++at) {
			const Event e = s.collector.EventAt(at);
			if (e.tsc < minTsc) minTsc = e.tsc;
			if (e.tsc > maxTsc) maxTsc = e.tsc;

			const f64 us = static_cast<f64>(e.tsc - g_clock.epochTsc) / g_clock.ticksPerNs /
						   1000.0;
			if (e.zone) {
				if (depth < kMaxDepth) open[depth] = e.zone;
				++depth;
				std::fprintf(f,
							 ",\n{\"name\":\"%s\",\"ph\":\"B\",\"ts\":%.3f,\"pid\":1,"
							 "\"tid\":%u}",
							 e.zone->name, us, s.osId);
				++stats.events;
			} else if (depth > 0) {
				--depth;
				std::fprintf(f, ",\n{\"ph\":\"E\",\"ts\":%.3f,\"pid\":1,\"tid\":%u}", us,
							 s.osId);
				++stats.events;
			} else {
				++stats.discarded; // an exit whose enter had already been overwritten
			}
		}

		// Any scope still open at the newest event is mid-flight — the dump is
		// running INSIDE the frame's own scopes, so on the main thread there are
		// always a couple. They are closed at the last timestamp seen, because a
		// bare "B" with no "E" makes the viewer draw a scope running to infinity
		// and a well-formed file matters more than the last microsecond. Their
		// durations are therefore a LOWER BOUND, which is what `truncated` says.
		stats.truncated += depth;
		for (u32 d = 0; d < depth && d < kMaxDepth; ++d)
			std::fprintf(f, ",\n{\"ph\":\"E\",\"ts\":%.3f,\"pid\":1,\"tid\":%u}",
						 static_cast<f64>(maxTsc - g_clock.epochTsc) / g_clock.ticksPerNs /
							 1000.0,
						 s.osId);

		// Did that thread lap us while we read? The ring is large and this is
		// fast, but "probably not" is not a thing to leave unchecked in a tool
		// whose whole purpose is telling you what really happened.
		const u64 after = s.collector.EventsWritten();
		if (after - written >= kRingEvents) stats.overrun += after - written;
	}

	std::fputs("\n]}\n", f);
	std::fclose(f);

	stats.ok = true;
	stats.spanMs = maxTsc > minTsc
					   ? static_cast<f64>(maxTsc - minTsc) / g_clock.ticksPerNs / 1e6
					   : 0.0;
	log::Write(log::Level::Info,
			   std::format("Profile: wrote {} events from {} threads spanning {:.1f} ms to {}"
						   "{}{}{}",
						   stats.events, stats.threads, stats.spanMs, path,
						   stats.truncated ? std::format(" ({} truncated)", stats.truncated)
										   : std::string{},
						   stats.discarded ? std::format(" ({} discarded)", stats.discarded)
										   : std::string{},
						   stats.overrun ? std::format(" (OVERRUN {})", stats.overrun)
										 : std::string{}));
	return stats;
}

Clock ClockInfo() { return g_clock; }

#else // !DN_PROFILE

void Init() {}
void RegisterThread(std::string_view) {}
void UnregisterThisThread() {}
void PublishThisThread() {}
int SnapshotAll(ThreadReport*, int) { return 0; }
Collector* OpenExternal(std::string_view) { return nullptr; }
void PublishExternal(Collector*) {}
int SetDetail(std::string_view, i8) { return 0; }
bool SetDetailNode(u32, u32, i8) { return false; }
int ListDetails(DetailEntry*, int) { return 0; }
TraceStats DumpTrace(const char*) { return {}; }
Clock ClockInfo() { return {}; }

#endif // DN_PROFILE

} // namespace dungeon::prof
