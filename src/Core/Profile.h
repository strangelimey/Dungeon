// ============================================================================
// Core/Profile.h — hierarchical, per-thread scope timing.
//
// ARCHITECTURE.md's performance work has been guided by whole-frame numbers and
// informed guessing. This is the machinery that replaces the guessing: every
// instrumented scope is timed, nested scopes form a TREE, and each thread keeps
// its own so the AI workers are measured on the same terms as the main loop.
//
// COMPILED OUT ENTIRELY without DN_PROFILE (the debug-profile / release-profile
// presets set it). Every macro below expands to nothing, ScopedZone becomes an
// empty type, and the registry's storage is not declared at all — a plain debug
// or release build carries no instructions, no bytes and no state for any of it.
// That is why the instrumentation is a MACRO rather than a class you construct:
// a call site reads the same in both worlds and leaves nothing behind in one.
//
// WHAT A NODE IS: one per CALL PATH, not per function. `Physics` under `Update`
// and `Physics` under the editor preview are different nodes, because they are
// genuinely different costs. A node accumulates inclusive time and the time its
// children took, so exclusive time falls out by subtraction — which answers
// "is Update slow, or is it just slow because of what it calls".
//
// THE LEVEL GATE, and why it sits BEFORE the clock read: a Zone carries a
// compile-time level (how deep in the detail this scope sits) and every node
// carries a runtime threshold inherited down its subtree. A scope whose level is
// past the threshold in force costs a comparison and a balanced push/pop, and
// nothing else — no timestamp, no lookup. That is what makes it affordable to
// instrument inner loops and pay for them only where you have asked to look.
// Measured (tools note in the profile branch): a scope with both timestamps is
// ~10.6 ns, of which ~9 ns is the two clock reads. Gating first is the whole
// game.
//
// STORAGE LIVES IN THE REGISTRY, not in the thread. A thread holds a POINTER to
// a slot the registry owns. This is deliberate and it is about ThreadManager::
// Kill, which force-terminates a wedged worker with TerminateThread — an API
// that runs NO destructors and frees the thread's TLS out from under anyone
// holding it. A collector held as a true thread_local would leak or dangle at
// exactly the moment its profile is most wanted; owned by the registry, a killed
// worker's last tree is still sitting there to be read.
//
// THE CLOCK is __rdtsc, calibrated against QPC once at Init. Measured on the
// development machine: rdtsc 4.5 ns a read at a 0.41 ns tick, against QPC's
// 9.9 ns at a 100 ns tick. The resolution is what decides it rather than the
// cost — QPC quantizes a 250 ns scope to 200, and no amount of averaging fixes
// a raw timeline. There is NO per-read fallback to QPC: that would put a branch
// on the hottest path in the engine to serve a CPU that has not shipped since
// about 2008. Init checks for an invariant TSC and warns loudly instead, and
// Report::invariantTsc carries the answer so a readout can say so.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <atomic>
#include <string_view>

#if DN_PROFILE
#include <intrin.h>
#endif

namespace dungeon::prof {

#if DN_PROFILE
inline constexpr bool kEnabled = true;
#else
inline constexpr bool kEnabled = false;
#endif

// ----------------------------------------------------------------------------
// Detail levels. A Zone names the level it belongs to; a subtree's threshold
// says how deep to record there. These are conventions, not a closed set — the
// gate is a plain `level > threshold` test — but three tiers is what the console
// will offer and what call sites should stick to.
inline constexpr u8 kLevelFrame = 0;  // frame landmarks: always recorded
inline constexpr u8 kLevelSystem = 1; // a subsystem's phases
inline constexpr u8 kLevelDetail = 2; // inner loops, per-item work

inline constexpr i8 kDefaultThreshold = kLevelFrame;

// A call site's identity, planted as a function-scope static by the macros
// below. Its ADDRESS is the key — comparing zones is a pointer compare, and the
// strings are never copied or hashed at runtime.
struct Zone {
	const char* name;
	const char* file;
	u32 line;
	u8 level;
};

// ----------------------------------------------------------------------------
// Capacities. Per-thread and fixed: the pool NEVER grows, because a profiler
// that allocates mid-frame would violate the steady-state rule it exists to
// help enforce (docs/ARCHITECTURE.md, "Memory strategy"). Exhaustion drops the
// scope and bumps a counter the readout shows, the same way alloc::kMaxThreads
// drops a registration rather than going fatal.
inline constexpr u32 kMaxThreads = 16; // main + the AI buckets + supervisor + room
inline constexpr u32 kMaxNodes = 2048; // per thread
inline constexpr u32 kMaxDepth = 64;   // nesting, per thread
inline constexpr u32 kInvalidNode = ~0u;

// ----------------------------------------------------------------------------
// One node of a thread's call tree. Children hang off firstChild/nextSibling as
// indices rather than pointers so the whole pool is relocatable and a published
// copy needs no fix-ups.
//
// Counters are RAW TSC TICKS. Converting to milliseconds on the hot path would
// be a multiply per exit for a number nothing reads until the console draws;
// Report carries the calibration so a reader converts once.
struct Node {
	const Zone* zone = nullptr;
	u32 parent = kInvalidNode;
	u32 firstChild = kInvalidNode;
	u32 nextSibling = kInvalidNode;

	u64 inclusive = 0; // this publish period, including children
	u64 childTime = 0; // the part of `inclusive` spent inside children
	u64 calls = 0;
	u64 maxTicks = 0; // worst single call this period

	// Detail override for this node and everything beneath it; -1 = inherit.
	// ATOMIC because the console (main thread) may raise the level on a node an
	// AI worker owns. Relaxed is right: a worker picking the change up a tick
	// late is not a race, it is the intended behaviour.
	std::atomic<i8> detail{-1};
};

// The POD form a reader gets. Same fields minus the atomic, so a snapshot is
// trivially copyable and the console never touches a live node.
struct NodeView {
	const Zone* zone = nullptr;
	u32 parent = kInvalidNode;
	u32 firstChild = kInvalidNode;
	u32 nextSibling = kInvalidNode;
	u64 inclusive = 0;
	u64 childTime = 0;
	u64 calls = 0;
	u64 maxTicks = 0;
	i8 detail = -1;

	// Time in this node itself. Children can only ever account for time this
	// node was also inside, so the subtraction cannot legitimately go negative —
	// but a node whose child overflowed the pool has children it never recorded,
	// so clamp rather than underflow into a vast u64.
	u64 Exclusive() const { return inclusive > childTime ? inclusive - childTime : 0; }
};

#if DN_PROFILE

// ----------------------------------------------------------------------------
// One thread's collector. Owned by the registry (see the header note); a thread
// reaches its own through the thread_local pointer below.
//
// Everything on the hot path is inline and touches only this object, so Enter
// and Exit take no lock and contend with nothing. The only shared word is a
// node's `detail`, read relaxed.
class Collector {
public:
	// --- the hot path -------------------------------------------------------
	void Enter(const Zone& zone) {
		// Past the depth limit we still push, so that Exit stays balanced and
		// the tree does not silently re-root itself mid-frame.
		if (m_depth >= kMaxDepth) {
			++m_depthOverflows;
			return;
		}

		const i8 threshold = m_depth > 0 ? m_stack[m_depth - 1].threshold : m_threshold;
		Frame& f = m_stack[m_depth++];
		f.threshold = threshold;

		// THE GATE, before any clock read.
		if (static_cast<i8>(zone.level) > threshold) {
			f.node = kInvalidNode;
			return;
		}

		const u32 node = FindOrCreateChild(zone);
		f.node = node;
		if (node == kInvalidNode) return; // pool exhausted; counted, not fatal

		// A node's own override wins over what it inherited, and applies to
		// everything nested inside it.
		const i8 own = m_nodes[node].detail.load(std::memory_order_relaxed);
		if (own >= 0) f.threshold = own;

		m_current = node;
		f.start = __rdtsc();
	}

	void Exit() {
		if (m_depth == 0) return; // unbalanced; the macros make this unreachable
		const Frame& f = m_stack[--m_depth];
		if (f.node == kInvalidNode) return; // gated out or dropped

		const u64 elapsed = __rdtsc() - f.start;
		Node& n = m_nodes[f.node];
		n.inclusive += elapsed;
		++n.calls;
		if (elapsed > n.maxTicks) n.maxTicks = elapsed;

		m_current = n.parent;
		if (m_current != kInvalidNode) m_nodes[m_current].childTime += elapsed;
	}

	// --- boundaries ---------------------------------------------------------
	// Copies the used nodes out and zeroes their counters, ending one period and
	// starting the next. The main thread does this per FRAME and a worker per
	// TICK, because a worker has no frames — the AI buckets run at
	// 251/499/997/1999 ms, so forcing them into frame buckets would show empty
	// trees and one false spike.
	//
	// THE TREE SURVIVES: only the counters reset. Nodes keep their identity
	// across periods, which is what lets a detail override placed on one stay
	// put, and what stops every frame re-walking the pool to rebuild a structure
	// that has not changed. Cost is O(nodes actually used) — a few hundred, not
	// the whole pool.
	u32 CopyAndReset(NodeView* out, u32 capacity);

	// Raises (or clears, with -1) the detail threshold on one node and, by
	// inheritance, everything beneath it. Safe to call from another thread.
	void SetDetail(u32 node, i8 level) {
		if (node < m_nodeCount) m_nodes[node].detail.store(level, std::memory_order_relaxed);
	}

	void SetBaseThreshold(i8 level) { m_threshold = level; }
	i8 BaseThreshold() const { return m_threshold; }

	u32 Root() const { return m_root; }
	u64 Periods() const { return m_periods; }
	u64 NodeOverflows() const { return m_nodeOverflows; }
	u64 DepthOverflows() const { return m_depthOverflows; }

private:
	struct Frame {
		u32 node = kInvalidNode;
		u64 start = 0;
		i8 threshold = kDefaultThreshold;
	};

	// Children are a short list and a linear scan over them is cache-friendly,
	// which is the whole reason the tree is shaped this way rather than hashed.
	u32 FindOrCreateChild(const Zone& zone) {
		const u32 parent = m_current;
		u32 child = parent == kInvalidNode ? m_root : m_nodes[parent].firstChild;
		while (child != kInvalidNode) {
			if (m_nodes[child].zone == &zone) return child;
			child = m_nodes[child].nextSibling;
		}

		if (m_nodeCount >= kMaxNodes) {
			++m_nodeOverflows;
			return kInvalidNode;
		}
		const u32 fresh = m_nodeCount++;
		Node& n = m_nodes[fresh];
		n.zone = &zone;
		n.parent = parent;
		n.firstChild = kInvalidNode;
		if (parent == kInvalidNode) {
			n.nextSibling = m_root;
			m_root = fresh;
		} else {
			n.nextSibling = m_nodes[parent].firstChild;
			m_nodes[parent].firstChild = fresh;
		}
		return fresh;
	}

	Node m_nodes[kMaxNodes];
	Frame m_stack[kMaxDepth];

	u32 m_nodeCount = 0;
	u32 m_root = kInvalidNode; // first top-level node; siblings chain from it
	u32 m_current = kInvalidNode;
	u32 m_depth = 0;
	i8 m_threshold = kDefaultThreshold;

	u64 m_nodeOverflows = 0;
	u64 m_depthOverflows = 0;
	u64 m_periods = 0;
};

// The calling thread's collector, or null on a thread that never registered.
// The null check in the macros is one predictable branch; pointing unregistered
// threads at a dummy would trade it for a dynamic-init ordering question.
extern thread_local Collector* t_collector;

#endif // DN_PROFILE

// ----------------------------------------------------------------------------
// Registry surface. All of these are no-ops (or return nothing) without
// DN_PROFILE, so callers need no #if of their own.

// Calibrates the clock, checks for an invariant TSC and names the calling
// thread "main". Call once, early, from the exe. Idempotent.
void Init();

// Gives the calling thread a collector. A thread that never registers is simply
// not measured. Names are copied into fixed storage; registration past
// kMaxThreads is dropped rather than fatal.
void RegisterThread(std::string_view name);

// Publishes the calling thread's tree and starts a new period (see
// Collector::Publish — frame for the main thread, tick for a worker).
void PublishThisThread();

// --- reading ----------------------------------------------------------------

// One thread's published tree. `nodes` points into registry storage that stays
// valid until that thread's next publish, so a reader should walk it and be
// done rather than hold it across frames.
struct ThreadReport {
	char name[32] = {};
	u32 osThreadId = 0;
	const NodeView* nodes = nullptr;
	u32 nodeCount = 0;
	u32 root = kInvalidNode;
	u64 periods = 0;        // frames (or ticks) published
	u64 nodeOverflows = 0;  // scopes dropped because the pool was full
	u64 depthOverflows = 0; // scopes dropped because nesting was too deep
	bool live = false;      // false once that thread has exited or been killed
};

// Copies every registered thread's latest published tree into `out` (capacity
// kMaxThreads); returns how many were written. Takes the per-thread publish
// lock briefly and never blocks a thread mid-scope.
int SnapshotAll(ThreadReport* out, int capacity);

// Clock calibration, so a reader converts raw ticks once.
struct Clock {
	f64 ticksPerNs = 1.0;
	f64 mhz = 0.0;
	bool invariantTsc = false;
};
Clock ClockInfo();

inline f64 TicksToMs(u64 ticks, const Clock& c) {
	return static_cast<f64>(ticks) / c.ticksPerNs / 1e6;
}

} // namespace dungeon::prof

// ----------------------------------------------------------------------------
// Instrumentation macros. DN_PROFILE_ZONE takes the frame level (always
// recorded); DN_PROFILE_ZONE_L names a deeper one that only records once its
// subtree's threshold has been raised.
//
// The Zone is a function-scope static so it is constructed once and its address
// is the call site's identity. Pasting __LINE__ into the names lets two zones
// share a scope without colliding.
#define DN_PROF_CAT2(a, b) a##b
#define DN_PROF_CAT(a, b) DN_PROF_CAT2(a, b)

#if DN_PROFILE

namespace dungeon::prof {
// RAII around one scope. Holds nothing but the collector it entered on, so a
// thread that registers mid-scope (or never registers) cannot mismatch its
// Enter and Exit.
class ScopedZone {
public:
	explicit ScopedZone(const Zone& zone) : m_collector(t_collector) {
		if (m_collector) m_collector->Enter(zone);
	}
	~ScopedZone() {
		if (m_collector) m_collector->Exit();
	}

	ScopedZone(const ScopedZone&) = delete;
	ScopedZone& operator=(const ScopedZone&) = delete;

private:
	Collector* m_collector;
};
} // namespace dungeon::prof

#define DN_PROFILE_ZONE_L(level_, name_)                                                  \
	static constexpr ::dungeon::prof::Zone DN_PROF_CAT(dnZone_, __LINE__){                 \
		name_, __FILE__, __LINE__, static_cast<::dungeon::u8>(level_)};                    \
	::dungeon::prof::ScopedZone DN_PROF_CAT(dnScope_, __LINE__)                            \
	{                                                                                      \
		DN_PROF_CAT(dnZone_, __LINE__)                                                     \
	}

#define DN_PROFILE_ZONE(name_) DN_PROFILE_ZONE_L(::dungeon::prof::kLevelFrame, name_)

#else

#define DN_PROFILE_ZONE_L(level_, name_) ((void)0)
#define DN_PROFILE_ZONE(name_) ((void)0)

#endif // DN_PROFILE
