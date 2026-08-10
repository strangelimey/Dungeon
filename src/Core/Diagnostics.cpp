// ============================================================================
// Core/Diagnostics.cpp — the slot table and the event rings.
//
// THE WRITE IS LOCK-FREE, and that is a requirement rather than a preference.
// A mutex here would be taken on the failure path — including by Kill, moments
// before TerminateThread ends a thread that might be holding it — and a leaked
// diagnostic lock would silence the record exactly when it matters. So a writer
// CLAIMS a ring slot with one fetch_add, fills it, and PUBLISHES it with a
// release store to that slot's own sequence number.
//
// THE SEQUENCE NUMBER IS THE ABSOLUTE CLAIM INDEX, not a flag: a reader knows
// slot i holds event n only if seq == n + 1, and it re-checks after copying, so
// a slot recycled mid-copy is discarded instead of returned half-torn. Absolute
// indices are also what makes that check immune to ABA — a full lap of the ring
// lands on seq = n + kEventsPerThread + 1, never back on n + 1.
//
// The table mutex guards REGISTRATION only (who owns which slot), and is never
// taken by Record.
// ============================================================================
#include "Core/Diagnostics.h"

#include "Core/AllocTrack.h"
#include "Core/Log.h"
#include "Core/StackTrace.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <format>
#include <mutex>

#include <intrin.h>
#include <windows.h>

namespace dungeon::diag {

namespace {

// ----------------------------------------------------------------------------
// One recorded event, in registry storage.
struct EventSlot {
	// 0 = never written. Otherwise the claim index + 1, stored with release
	// AFTER the payload — see the file banner.
	std::atomic<u64> seq{0};

	Kind kind = Kind::Exception;
	u32 workerId = 0;
	u64 iteration = 0;
	u64 tsc = 0;
	i64 wallNs = 0;
	char message[kMessageMax] = {};
	void* frames[kStackDepth] = {};
	int frameCount = 0;
};

// One thread's record. `written` is monotonic and never wraps (a u64 of rare
// events outlives the hardware), so `written - kEventsPerThread` is exactly the
// oldest event still present and a reader needs no separate count.
struct Entry {
	EventSlot events[kEventsPerThread];
	std::atomic<u64> written{0};
	std::atomic<u64> counts[kKindCount]{};

	// Log throttling, in two layers, because they catch different failures.
	//
	// IDENTICAL consecutive events collapse to powers of ten: a worker throwing
	// the same thing every tick writes four lines a minute, not four thousand.
	std::atomic<u64> lastHash{0};
	std::atomic<u64> repeat{0};

	// DISTINCT ones are rate-limited per thread. The collapse above is blind to
	// a message carrying a tick number or a coordinate, so a worker failing a
	// slightly different way every tick would still drown the log — and
	// dungeon.log is the surface the crash is meant to be found on, so burying
	// it under a thousand lines of the same bug is its own way of failing.
	// Measured: 16k distinct events wrote a 1.2 MB log before this existed.
	std::atomic<i64> windowStartNs{0};
	std::atomic<u32> windowLogged{0};
	std::atomic<u64> suppressed{0};

	// Table bookkeeping, written under g_mx. Read unlocked by Record and by the
	// readouts: `used` only ever goes false→true, and a stale `live` costs a
	// diagnostic one wrong flag, never a bad pointer, because the storage is
	// ours and outlives every thread that writes to it.
	std::atomic<bool> used{false};
	std::atomic<bool> live{false};
	char name[32] = {};
	std::atomic<u32> osId{0};
};

std::mutex g_mx;
// constinit is the checked form of "no dynamic initialization". This table is
// reachable from a fault filter and from a thread being torn down, so an
// initializer that ran lazily on first use would be a hazard on exactly the
// path this exists to serve.
constinit Entry g_entries[kMaxThreads]{};
constinit bool g_inited = false;

thread_local constinit Slot t_slot = kInvalidSlot;

// FNV-1a over the kind and message: the identity used to collapse a repeat.
u64 HashEvent(Kind kind, const char* msg) {
	u64 h = 1469598103934665603ull;
	h = (h ^ static_cast<u64>(kind)) * 1099511628211ull;
	for (const char* p = msg; *p; ++p) h = (h ^ static_cast<u8>(*p)) * 1099511628211ull;
	return h;
}

// 1, 10, 100, 1000, ... — the only repeat counts that get a log line.
bool IsLogPoint(u64 n) {
	while (n >= 10 && n % 10 == 0) n /= 10;
	return n == 1;
}

// The per-thread log budget: kLogBurst lines a window, everything past that
// counted and dropped. Deliberately generous — the first few lines of a new
// failure are the ones worth having, and a burst that fits under the limit is
// never delayed.
constexpr u32 kLogBurst = 8;
constexpr i64 kLogWindowNs = 1'000'000'000;

// Claims one line of this thread's budget. On rolling into a new window it
// hands back however many were suppressed in the last one, so the log says how
// much it swallowed instead of quietly losing it. Racy between concurrent
// writers on one slot, and deliberately so: the cost of losing that race is a
// spare log line, and a lock here is the thing a Kill could leak.
bool TakeLogBudget(auto& e, i64 nowNs, u64& flushed) {
	const i64 start = e.windowStartNs.load(std::memory_order_relaxed);
	if (start == 0 || nowNs - start > kLogWindowNs) {
		e.windowStartNs.store(nowNs, std::memory_order_relaxed);
		e.windowLogged.store(0, std::memory_order_relaxed);
		flushed = e.suppressed.exchange(0, std::memory_order_relaxed);
	}
	if (e.windowLogged.load(std::memory_order_relaxed) >= kLogBurst) return false;
	e.windowLogged.fetch_add(1, std::memory_order_relaxed);
	return true;
}

bool NameMatches(const Entry& e, std::string_view name) {
	const size_t n = std::strlen(e.name);
	return n == name.size() && std::memcmp(e.name, name.data(), n) == 0;
}

void SetName(Entry& e, std::string_view name) {
	const size_t n =
		name.size() < sizeof(e.name) - 1 ? name.size() : sizeof(e.name) - 1;
	if (n) std::memcpy(e.name, name.data(), n);
	e.name[n] = '\0';
}

// Wipes a slot being handed to a thread it did not belong to. NOT used when a
// same-named worker reboots into its own slot: keeping that history is the
// point (see the header note on reboots).
void ResetEntry(Entry& e) {
	for (EventSlot& s : e.events) s.seq.store(0, std::memory_order_relaxed);
	e.written.store(0, std::memory_order_relaxed);
	for (std::atomic<u64>& c : e.counts) c.store(0, std::memory_order_relaxed);
	e.lastHash.store(0, std::memory_order_relaxed);
	e.repeat.store(0, std::memory_order_relaxed);
}

// The log line. Excuses its own allocations: log::Write formats a std::string,
// and an un-excused report inside a guarded frame would be its own violation
// (the rule Core/AllocTrack sets out).
void LogEvent(const Entry& e, const EventSlot& s, u64 repeat) {
	alloc::Excused excuse;

	// system_clock's own duration is not nanoseconds (100 ns ticks on MSVC), so
	// the stored figure has to be cast back into the clock's units, not just
	// wrapped in a time_point.
	const auto tp = std::chrono::system_clock::time_point(
		std::chrono::duration_cast<std::chrono::system_clock::duration>(
			std::chrono::nanoseconds(s.wallNs)));
	const std::time_t t = std::chrono::system_clock::to_time_t(tp);
	std::tm local{};
	localtime_s(&local, &t);
	const long long ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::nanoseconds(s.wallNs))
			.count() %
		1000;

	const std::string when = std::format("{:02}:{:02}:{:02}.{:03}", local.tm_hour,
										 local.tm_min, local.tm_sec, ms);
	const std::string who =
		s.workerId ? std::format("'{}' (worker {}, tick {})", e.name, s.workerId,
								 s.iteration)
				   : std::format("'{}'", e.name);
	const std::string again =
		repeat > 1 ? std::format(" — repeated {} times", repeat) : std::string{};

	log::Write(s.kind == Kind::Fatal ? log::Level::Error : log::Level::Warn,
			   std::format("diag {} · {} on {}: {}{}", when, KindName(s.kind), who,
						   s.message, again));

	// The stack, ONCE per distinct site. A failure that repeats is the ordinary
	// case and its stack is identical every time, so printing thirty frames on
	// every occurrence would undo the throttling above and bury the next problem.
	if (s.frameCount > 0) {
		// Function-local so the set is created on first use rather than at load,
		// and guarded because this can be reached from any thread — unlike the
		// RECORD above, the LOG path is allowed a lock (log::Write takes one
		// anyway).
		static std::mutex seenMx;
		static stack::SeenSet seen;
		bool first = false;
		{
			std::lock_guard lk(seenMx);
			first = seen.FirstSighting(stack::Hash(s.frames, s.frameCount));
		}
		if (first) stack::LogStack(s.frames, s.frameCount);
	}
}

// Reads one ring position, if it still holds the event asked for. `out` is
// filled only on success, and the sequence is re-checked afterwards so a slot
// recycled during the copy is rejected.
bool ReadSlot(const Entry& e, u64 index, EventView& out) {
	const EventSlot& s = e.events[index & (kEventsPerThread - 1)];
	if (s.seq.load(std::memory_order_acquire) != index + 1) return false;

	out.kind = s.kind;
	out.workerId = s.workerId;
	out.iteration = s.iteration;
	out.tsc = s.tsc;
	out.wallNs = s.wallNs;
	out.index = index;
	std::memcpy(out.message, s.message, sizeof(out.message));
	out.frameCount = s.frameCount;
	std::memcpy(out.frames, s.frames, sizeof(out.frames));

	return s.seq.load(std::memory_order_acquire) == index + 1;
}

// The oldest index still resident in a ring that has written `w` events.
u64 OldestIndex(u64 w) { return w > kEventsPerThread ? w - kEventsPerThread : 0; }

} // namespace

// ----------------------------------------------------------------------------

const char* KindName(Kind k) {
	switch (k) {
	case Kind::Exception: return "exception";
	case Kind::Fault: return "fault";
	case Kind::Stall: return "stall";
	case Kind::Restart: return "restart";
	case Kind::Killed: return "killed";
	case Kind::Fatal: return "FATAL";
	}
	return "?";
}

void Init() {
	{
		std::lock_guard lock(g_mx);
		if (g_inited) return;
		g_inited = true;
	}
	RegisterThread("main");
}

Slot RegisterThread(std::string_view name) {
	std::lock_guard lock(g_mx);
	if (t_slot != kInvalidSlot) return t_slot;

	// 1. A dormant slot this NAME already owns — a rebooted worker coming back.
	//    Adopted as-is, so the events that got it rebooted are still there.
	for (int i = 0; i < kMaxThreads; ++i) {
		Entry& e = g_entries[i];
		if (!e.used.load() || e.live.load()) continue;
		if (!NameMatches(e, name)) continue;
		e.live.store(true);
		e.osId.store(static_cast<u32>(::GetCurrentThreadId()));
		t_slot = static_cast<Slot>(i);
		return t_slot;
	}

	// 2. A slot nobody has ever used.
	for (int i = 0; i < kMaxThreads; ++i) {
		Entry& e = g_entries[i];
		if (e.used.load()) continue;
		SetName(e, name);
		e.osId.store(static_cast<u32>(::GetCurrentThreadId()));
		e.live.store(true);
		e.used.store(true);
		t_slot = static_cast<Slot>(i);
		return t_slot;
	}

	// 3. A dormant slot belonging to some other name. Its events are not this
	//    thread's history, so they go.
	for (int i = 0; i < kMaxThreads; ++i) {
		Entry& e = g_entries[i];
		if (!e.used.load() || e.live.load()) continue;
		ResetEntry(e);
		SetName(e, name);
		e.osId.store(static_cast<u32>(::GetCurrentThreadId()));
		e.live.store(true);
		t_slot = static_cast<Slot>(i);
		return t_slot;
	}

	// Table full. The thread runs unrecorded rather than the process dying over
	// a diagnostic — the same bargain Core/AllocTrack makes. Reaching here needs
	// 32 live-or-dormant names, and a force-terminated worker never releasing
	// its slot is the only way to get there in practice.
	return kInvalidSlot;
}

Slot ThisThread() { return t_slot; }

Slot FindThread(std::string_view name) {
	std::lock_guard lock(g_mx);
	for (int i = 0; i < kMaxThreads; ++i) {
		const Entry& e = g_entries[i];
		if (e.used.load() && e.live.load() && NameMatches(e, name))
			return static_cast<Slot>(i);
	}
	return kInvalidSlot;
}

void UnregisterThisThread() {
	std::lock_guard lock(g_mx);
	if (t_slot == kInvalidSlot) return;
	g_entries[t_slot].live.store(false);
	t_slot = kInvalidSlot;
}

// ----------------------------------------------------------------------------

void RecordFor(Slot slot, const Event& ev) {
	if (slot >= static_cast<Slot>(kMaxThreads)) return;
	Entry& e = g_entries[slot];
	if (!e.used.load(std::memory_order_relaxed)) return;

	const u64 at = e.written.fetch_add(1, std::memory_order_relaxed);
	EventSlot& s = e.events[at & (kEventsPerThread - 1)];

	// Invalidate before touching the payload, so a reader mid-copy of the event
	// this slot USED to hold rejects it rather than returning a mix of the two.
	s.seq.store(0, std::memory_order_release);

	s.kind = ev.kind;
	s.workerId = ev.workerId;
	s.iteration = ev.iteration;
	s.tsc = __rdtsc();
	s.wallNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
				   std::chrono::system_clock::now().time_since_epoch())
				   .count();

	const size_t n = ev.message.size() < kMessageMax - 1 ? ev.message.size()
														 : kMessageMax - 1;
	if (n) std::memcpy(s.message, ev.message.data(), n);
	s.message[n] = '\0';

	// A stack the caller already holds beats one captured here — at a catch site
	// the throw point has already unwound away (phase 3 supplies the better one).
	if (ev.frames && ev.frameCount > 0) {
		const int fc = ev.frameCount < kStackDepth ? ev.frameCount : kStackDepth;
		std::memcpy(s.frames, ev.frames, static_cast<size_t>(fc) * sizeof(void*));
		s.frameCount = fc;
	} else if (ev.captureStack) {
		// Does not allocate, which is what makes it safe on this path. Skips
		// this frame so the report starts at the caller.
		s.frameCount = static_cast<int>(
			::RtlCaptureStackBackTrace(1, kStackDepth, s.frames, nullptr));
	} else {
		s.frameCount = 0;
	}

	s.seq.store(at + 1, std::memory_order_release);

	e.counts[static_cast<int>(ev.kind)].fetch_add(1, std::memory_order_relaxed);

	// The record is complete at this point; everything below only decides
	// whether this event also reaches the log.
	const u64 h = HashEvent(ev.kind, s.message);
	u64 repeat = 1;
	bool identical = false;
	if (e.lastHash.load(std::memory_order_relaxed) == h) {
		repeat = e.repeat.fetch_add(1, std::memory_order_relaxed) + 1;
		identical = true;
	} else {
		e.lastHash.store(h, std::memory_order_relaxed);
		e.repeat.store(1, std::memory_order_relaxed);
	}
	if (identical && !IsLogPoint(repeat)) {
		e.suppressed.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	u64 flushed = 0;
	if (!TakeLogBudget(e, s.wallNs, flushed)) {
		e.suppressed.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if (flushed) {
		alloc::Excused excuse;
		log::Warn("diag · {} further events on '{}' were not logged (rate limit); the "
				  "record kept them all",
				  flushed, e.name);
	}
	LogEvent(e, s, repeat);
}

void Record(const Event& ev) {
	if (t_slot == kInvalidSlot) return; // an unregistered thread is not recorded
	RecordFor(t_slot, ev);
}

// ----------------------------------------------------------------------------

int SnapshotThreads(ThreadHealth* out, int capacity) {
	std::lock_guard lock(g_mx);
	int n = 0;
	for (int i = 0; i < kMaxThreads && n < capacity; ++i) {
		const Entry& e = g_entries[i];
		if (!e.used.load()) continue;
		ThreadHealth& h = out[n++];
		std::memcpy(h.name, e.name, sizeof(h.name));
		h.osThreadId = e.osId.load();
		h.slot = static_cast<Slot>(i);
		h.live = e.live.load();
		h.total = e.written.load(std::memory_order_acquire);
		for (int k = 0; k < kKindCount; ++k)
			h.counts[k] = e.counts[k].load(std::memory_order_relaxed);
	}
	return n;
}

int ReadEvents(Slot slot, EventView* out, int capacity) {
	if (slot >= static_cast<Slot>(kMaxThreads) || capacity <= 0) return 0;
	const Entry& e = g_entries[slot];
	if (!e.used.load()) return 0;

	const u64 w = e.written.load(std::memory_order_acquire);
	u64 first = OldestIndex(w);
	// Oldest first, so a caller printing them in order reads a timeline.
	if (w - first > static_cast<u64>(capacity)) first = w - static_cast<u64>(capacity);

	int n = 0;
	for (u64 i = first; i < w && n < capacity; ++i)
		if (ReadSlot(e, i, out[n])) ++n;
	return n;
}

int ReadAllEvents(EventView* out, int capacity, Slot* slotOut) {
	if (capacity <= 0) return 0;

	// A k-way merge over the per-thread rings, newest first. Done with a cursor
	// per thread rather than by gathering everything and sorting, because the
	// gather buffer would be a quarter of a megabyte on the stack — on a path
	// that may be walking a process already in trouble.
	u64 cursor[kMaxThreads];  // one past the next index to consider
	u64 floors[kMaxThreads];  // oldest index still resident
	for (int i = 0; i < kMaxThreads; ++i) {
		const u64 w = g_entries[i].used.load()
						  ? g_entries[i].written.load(std::memory_order_acquire)
						  : 0;
		cursor[i] = w;
		floors[i] = OldestIndex(w);
	}

	int n = 0;
	while (n < capacity) {
		int best = -1;
		u64 bestTsc = 0;
		EventView bestView{};
		for (int i = 0; i < kMaxThreads; ++i) {
			// Walk back past any slot that has been recycled since we looked.
			EventView v{};
			while (cursor[i] > floors[i] && !ReadSlot(g_entries[i], cursor[i] - 1, v))
				--cursor[i];
			if (cursor[i] <= floors[i]) continue;
			if (best < 0 || v.tsc > bestTsc) {
				best = i;
				bestTsc = v.tsc;
				bestView = v;
			}
		}
		if (best < 0) break;
		out[n] = bestView;
		if (slotOut) slotOut[n] = static_cast<Slot>(best);
		++n;
		--cursor[best];
	}
	return n;
}

Totals ProcessTotals() {
	Totals t;
	for (int i = 0; i < kMaxThreads; ++i) {
		const Entry& e = g_entries[i];
		if (!e.used.load()) continue;
		t.total += e.written.load(std::memory_order_acquire);
		for (int k = 0; k < kKindCount; ++k)
			t.counts[k] += e.counts[k].load(std::memory_order_relaxed);
	}
	return t;
}

} // namespace dungeon::diag
