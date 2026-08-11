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

// ----------------------------------------------------------------------------
// The frame's LANDMARKS, named once. A reader that wants to say what a frame was
// spent on has to find these in the tree, and it can only find them BY NAME —
// there is no other identity a zone carries. Declared here so the sites that
// plant them and the readout that looks for them cannot drift apart in a rename.
//
// All at kLevelFrame, and that is load-bearing rather than a default: the
// console's frame-budget verdict is computed FROM them, so a level that could be
// gated out would leave the verdict unable to answer in exactly the default
// configuration everyone reads it in.
inline constexpr const char* kZoneFrame = "frame";
inline constexpr const char* kZoneUpdate = "update";
inline constexpr const char* kZoneRender = "render";
inline constexpr const char* kZoneWaitGpu = "wait.gpu"; // blocked on the frame fence
inline constexpr const char* kZoneRecord = "record";    // building command lists
inline constexpr const char* kZonePresent = "present";  // blocked in Present
inline constexpr const char* kZoneWaitCap = "wait.cap"; // held back by the frame cap

// Source names, for the same reason: the readout pairs the main thread's frame
// against the GPU's busy time and has to find both.
inline constexpr const char* kThreadMain = "main";
inline constexpr const char* kSourceGpu = "gpu";

// Declared here and DEFINED only in a profiling build, so the external-slot
// functions below can name it either way and a caller needs no #if of its own.
class Collector;

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
// The event ring: the second sink, and the one that can see a HITCH. The tree
// aggregates, so it can say the average frame spends 4 ms in render and can
// never say that frame 8412 spent 40. This keeps the raw timeline.
//
// ALWAYS RECORDING, and it WRAPS rather than growing — a flight recorder, not a
// press-record button, because by the time a stutter is noticed it has already
// happened. Both sinks share the one level gate, so raising a subtree's detail
// deepens the tree and fattens the ring together.
//
// AN EVENT IS 16 BYTES and holds no kind field: a null zone MEANS "exit of the
// innermost open scope", which the reader is already tracking a stack to know.
// That buys the packing for free and needs no interning of zone names — the
// Zones are function-scope statics, so a pointer stays valid for the process and
// the dump reads the name straight off it.
struct Event {
	u64 tsc = 0;
	const Zone* zone = nullptr; // null = the matching exit
};

// Per thread. Power of two: the write index is masked, never compared. 64k
// events is 1 MB a thread — at the frame-landmark level that is a window of
// tens of seconds, and it SHRINKS as detail is turned up, which is why
// TraceStats reports the span it actually captured rather than promising one.
inline constexpr u32 kRingEvents = 65536;
static_assert((kRingEvents & (kRingEvents - 1)) == 0, "ring capacity must be a power of two");

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
		// Past the depth limit there is no frame to push into, so the matching
		// Exit is REMEMBERED instead. Letting it fall through to the normal pop
		// would consume the parent's frame — the tree would re-root itself
		// mid-period and every subsequent Exit would be off by one. The count is
		// exact because these are always the innermost scopes: nothing can nest
		// below the limit and come back, so their exits arrive first.
		if (m_depth >= kMaxDepth) {
			++m_depthOverflows;
			++m_unpushed;
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
		Record(&zone, f.start);
	}

	void Exit() {
		if (m_unpushed > 0) { // matches an Enter the stack had no room for
			--m_unpushed;
			return;
		}
		if (m_depth == 0) return; // unbalanced; the macros make this unreachable
		const Frame& f = m_stack[--m_depth];
		if (f.node == kInvalidNode) return; // gated out or dropped

		const u64 now = __rdtsc();
		Record(nullptr, now);
		const u64 elapsed = now - f.start;
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

	// Records a span timed SOMEWHERE ELSE — on the GPU — rather than by this
	// thread's clock. The zone becomes a ROOT of the tree rather than a child of
	// whatever happens to be open here, because the work it describes did not
	// nest inside anything on this side.
	//
	// `ticks` must already be in TSC units, so TicksToMs means the same thing for
	// these as for everything else. The caller owns that conversion: only it
	// knows what clock the span was measured against.
	void AddSpan(const Zone& zone, u64 ticks) {
		const u32 save = m_current;
		m_current = kInvalidNode;
		const u32 node = FindOrCreateChild(zone);
		m_current = save;
		if (node == kInvalidNode) return;

		Node& n = m_nodes[node];
		n.inclusive += ticks;
		++n.calls;
		if (ticks > n.maxTicks) n.maxTicks = ticks;
	}

	// Raises (or clears, with -1) the detail threshold on one node and, by
	// inheritance, everything beneath it. Safe to call from another thread.
	void SetDetail(u32 node, i8 level) {
		if (node < m_nodeCount) m_nodes[node].detail.store(level, std::memory_order_relaxed);
	}

	void SetBaseThreshold(i8 level) { m_threshold = level; }
	i8 BaseThreshold() const { return m_threshold; }

	// Drops the tree and every counter. Used when a slot is handed to a rebooted
	// worker, so its predecessor's numbers do not bleed into the new one. Only
	// the bookkeeping is cleared: the node array keeps whatever it held, which
	// nothing can reach with m_nodeCount back at zero.
	void Reset() {
		m_nodeCount = 0;
		m_root = kInvalidNode;
		m_current = kInvalidNode;
		m_depth = 0;
		m_unpushed = 0;
		m_written.store(0, std::memory_order_relaxed);
		m_threshold = kDefaultThreshold;
		m_nodeOverflows = 0;
		m_depthOverflows = 0;
		m_periods = 0;
	}

	// --- ring readout (the dump; see DumpTrace) -----------------------------
	// Acquire pairs with Record's release store: every event below the returned
	// index is fully written. Reading is otherwise unsynchronized on purpose —
	// stopping a thread to read its ring would change the very timings the ring
	// exists to capture.
	u64 EventsWritten() const { return m_written.load(std::memory_order_acquire); }
	const Event& EventAt(u64 index) const { return m_events[index & (kRingEvents - 1)]; }

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

	// Appends to the ring. Only the owning thread ever writes, so no lock — but
	// the index is PUBLISHED with a release store so a reader that acquires it
	// knows every event below it is fully written. On x86 that is a plain store;
	// it costs nothing and it is the difference between a dump reading whole
	// events and reading half of one.
	void Record(const Zone* zone, u64 tsc) {
		const u64 at = m_written.load(std::memory_order_relaxed);
		Event& e = m_events[at & (kRingEvents - 1)];
		e.tsc = tsc;
		e.zone = zone;
		m_written.store(at + 1, std::memory_order_release);
	}

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

	// The ring, and a monotonic count of everything ever written to it. The count
	// never wraps (a u64 at even a billion events a second outlives the hardware),
	// so `written - kRingEvents` is exactly the oldest event still present and a
	// reader can tell how much history it has without a separate flag.
	Event m_events[kRingEvents];
	std::atomic<u64> m_written{0};

	u32 m_nodeCount = 0;
	u32 m_root = kInvalidNode; // first top-level node; siblings chain from it
	u32 m_current = kInvalidNode;
	u32 m_depth = 0;
	u32 m_unpushed = 0; // Enters past kMaxDepth, awaiting their Exits
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
//
// A slot left behind by a thread that unregistered is REUSED if the name
// matches, which is what keeps ThreadManager::Restart from burning a slot per
// reboot (the supervisor reboots a stalled worker on its own, so this is not a
// rare path). The reused slot is reset, so a predecessor's numbers cannot bleed
// into its replacement.
void RegisterThread(std::string_view name);

// Marks the calling thread's slot dormant, keeping its last published tree
// readable, and makes the slot available for a same-named successor. Call at
// the end of a thread that will exit cleanly.
//
// A thread that does NOT get here — one force-terminated by ThreadManager::Kill
// — leaves its slot occupied and still flagged live. That is deliberate: the
// tree it died holding is the evidence the kill was worth looking at, and with
// kMaxThreads slots against an engine that runs about six threads there is room
// to lose a few to quarantine.
void UnregisterThisThread();

// Publishes the calling thread's tree and starts a new period (see
// Collector::Publish — frame for the main thread, tick for a worker).
void PublishThisThread();

// --- externally-timed sources -----------------------------------------------
// A slot belonging to no thread, for work measured on another clock entirely and
// fed in afterwards with Collector::AddSpan.
//
// It exists so such work appears as one more row in the same tree, the same
// panel, the same graph view and the same trace, instead of growing a parallel
// readout beside them. Everything downstream of the registry already knows how
// to draw a thread; the GPU is one more.
//
// Idempotent by name; returns null without DN_PROFILE or if the table is full.
Collector* OpenExternal(std::string_view name);
void PublishExternal(Collector* collector);

// --- reading ----------------------------------------------------------------

// One thread's published tree. `nodes` points into registry storage that stays
// valid until that thread's next publish, so a reader should walk it and be
// done rather than hold it across frames.
struct ThreadReport {
	char name[32] = {};
	u32 osThreadId = 0;
	// The registry slot. UNLIKE osThreadId this is unique for every source: an
	// external one (the GPU) owns no thread and reports id 0, so two of them
	// would be indistinguishable. Anything addressing a source rather than
	// merely labelling it — SetDetailNode below — keys on this.
	u32 slot = 0;
	const NodeView* nodes = nullptr;
	u32 nodeCount = 0;
	u32 root = kInvalidNode;
	u64 periods = 0;        // frames (or ticks) published
	u64 nodeOverflows = 0;  // scopes dropped because the pool was full
	u64 depthOverflows = 0; // scopes dropped because nesting was too deep
	// What the roots are gated against, and so the level the whole tree inherits
	// before any per-node override. Reported rather than assumed to be
	// kDefaultThreshold, since a reader computing a node's EFFECTIVE level has to
	// start the inheritance chain somewhere.
	i8 baseThreshold = kDefaultThreshold;
	bool live = false; // false once that thread has exited or been killed
};

// Copies every registered thread's latest published tree into `out` (capacity
// kMaxThreads); returns how many were written. Takes the per-thread publish
// lock briefly and never blocks a thread mid-scope.
int SnapshotAll(ThreadReport* out, int capacity);

// --- detail overrides -------------------------------------------------------
// Turning one branch up to investigate a slowdown, while everything else stays
// at the default level. The gate and the inheritance are in Collector; these are
// how a path gets named from outside.
//
// `path` is slash-separated zone names from a ROOT, e.g. "frame/render". It is
// matched against EVERY thread's tree and applied to every match, which is what
// makes "tick" reach all four AI workers in one go. Level -1 clears the override
// and returns that subtree to inheriting.
//
// A path can only name a node that ALREADY EXISTS, because that is the only way
// a name is known to have been reached. Level-0 landmarks always record, so the
// top of every tree is there to aim at and you drill down from what you can see:
// raise a branch, look again, raise the branch that appeared.
//
// Returns how many nodes matched (0 = the path names nothing recorded yet).
int SetDetail(std::string_view path, i8 level);

// The same override, aimed at ONE node of ONE source — what a click on a row of
// the console's tree wants, where a path does not say enough. The four AI
// workers run identical trees, so "tick/think" names a node in all four and the
// path form deliberately hits them all; a click lands on one row and must move
// only that row's thread.
//
// `slot` is ThreadReport::slot and `node` an index into that report's `nodes`.
// Both come straight from a snapshot, and indices are stable across publishes
// (node i is copied to slot i and the tree only ever grows), so a report read
// last frame still addresses the right node this one. Returns false if the slot
// is unused or the index is past that source's tree — a node that vanished
// (its thread was rebooted and Reset the tree) simply misses.
bool SetDetailNode(u32 slot, u32 node, i8 level);

struct DetailEntry {
	char thread[32] = {};
	char path[128] = {};
	i8 level = -1;
};

// Every override currently in force, for the console to list back.
int ListDetails(DetailEntry* out, int capacity);

// --- the trace dump ---------------------------------------------------------

struct TraceStats {
	int threads = 0;
	u64 events = 0;    // written to the file
	u64 discarded = 0; // head exits whose opening enter had already been overwritten
	u64 truncated = 0; // scopes still open at the dump, closed at the last timestamp
	u64 overrun = 0;   // events a thread overwrote WHILE being read (see below)
	f64 spanMs = 0.0;  // wall time the trace actually covers
	bool ok = false;
};

// Writes every thread's ring to `path` as Chrome Trace Event JSON — the format
// Perfetto and speedscope both read, so a flame graph and a searchable timeline
// come for free instead of being a viewer project the size of the profiler.
//
// TWO HONEST LIMITS, both reported in TraceStats rather than hidden:
//
// A wrapped ring starts MID-FLIGHT, so its oldest events are exits whose enters
// were overwritten. Those are discarded — an exit arriving with nothing open is
// counted and dropped, not guessed at. At the other end, the dump runs INSIDE
// the frame's own scopes, so a few are always still open; those are closed at
// the last timestamp seen and counted as `truncated`, their durations being a
// lower bound rather than a fiction.
//
// The world is NOT stopped while this reads. A thread writing fast enough could
// lap the reader; the read is bracketed by the same published index it started
// from, so an overrun is DETECTED and counted rather than silently producing a
// trace with a plausible-looking hole in it. Freezing the editor (its pause
// button) before dumping avoids it entirely for the main thread.
TraceStats DumpTrace(const char* path);

// Clock calibration, so a reader converts raw ticks once.
struct Clock {
	f64 ticksPerNs = 1.0;
	f64 mhz = 0.0;
	bool invariantTsc = false;
	// TSC at Init. Every thread's events are stamped from the same counter, so
	// subtracting one epoch is what puts them all on ONE timeline in the trace
	// rather than four that happen to overlap.
	u64 epochTsc = 0;
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
