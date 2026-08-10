// ============================================================================
// Core/Diagnostics.h — the health record: every way a thread can fail.
//
// docs/diagnostics.md is the design. This header is the RECORD alone — the
// sink that failures are written into and that readouts are read out of. The
// capture sites (the worker catch, the supervisor's stall, the process-level
// fault filter), the symbolizer and the console panel all layer on top without
// reshaping it.
//
// ALWAYS COMPILED IN, unlike Core/Profile. That is the whole point: the crash
// worth reporting happens in a plain debug or release run, so a record gated
// behind a profiling preset would be absent exactly when it is wanted. The cost
// is a fixed ring per thread in BSS and work only on the failure path — a
// healthy frame never touches any of this.
//
// THE REGISTRY OWNS THE STORAGE, and a thread holds only a slot index into it.
// Core/AllocTrack makes the same choice for the same reason, and it is a safety
// property rather than tidiness: ThreadManager::Kill force-terminates a wedged
// worker with TerminateThread, which runs NO destructors and releases the
// thread's TLS block underneath it. A table of pointers INTO TLS would be left
// holding a dangler that the next read follows — in diagnostic code, on the
// path you reach for when something is already wrong. Registry-owned storage
// cannot be freed under a reader, so a force-terminated thread leaves its last
// events behind exactly like one that exited cleanly.
//
// NOTHING HERE ALLOCATES OR LOCKS. Messages are copied into fixed storage and
// stacks into a fixed array, because a report written after heap corruption —
// or from a thread whose termination leaked the CRT heap lock — would be a
// second crash on top of the first. Writes are claimed with one fetch_add and
// published with a release store, so a cross-thread writer (the supervisor
// recording a stall against the worker it watches) needs no mutex that a kill
// could leak.
//
// A REBOOT DOES NOT CLEAR THE RECORD. Core/Profile resets a rebooted worker's
// slot so its predecessor's timings cannot bleed into it; this does the
// opposite on purpose, because "it threw twice, stalled, and was restarted" IS
// the history being asked for. The ring wraps; it is never emptied.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <atomic>
#include <string_view>

namespace dungeon::diag {

// ----------------------------------------------------------------------------
// What can go wrong. One kind per way a thread fails, because the readouts
// count and colour by kind and a catch-all "Error" would collapse a stall and a
// throw into one number.
enum class Kind : u8 {
	Exception, // a C++ throw crossed a managed boundary
	Fault,     // an SEH fault: access violation, divide-by-zero, stack overflow
	Stall,     // a tick outran its watchdog
	Restart,   // a worker was rebooted, by the supervisor or by hand
	Killed,    // a worker was force-terminated and quarantined
	Fatal,     // an assert, a terminate, or a repeat limit reached
};
inline constexpr int kKindCount = 6;
const char* KindName(Kind k);

// ----------------------------------------------------------------------------
// Capacities. Fixed and never grown — see the header note. About 8 KB a thread,
// a quarter of a megabyte of BSS in total, which buys a record that survives a
// force-terminated thread.
inline constexpr int kMaxThreads = 32;      // main + AI buckets + supervisor + stress room
inline constexpr int kEventsPerThread = 16; // ring depth, per thread
inline constexpr int kStackDepth = 32;
inline constexpr int kMessageMax = 192;

static_assert((kEventsPerThread & (kEventsPerThread - 1)) == 0,
			  "ring depth must be a power of two: the write index is masked");

using Slot = u32;
inline constexpr Slot kInvalidSlot = ~0u;

// ----------------------------------------------------------------------------
// What a caller BUILDS. `message` is copied, so a std::string temporary at the
// call site is fine and nothing here retains a pointer to it.
//
// A caller that already holds a better stack than this one could capture passes
// it in `frames` — which is how the throw-time vectored handler (phase 3) beats
// a catch site, where the stack has already unwound and the throw point is gone.
struct Event {
	Kind kind = Kind::Exception;
	u32 workerId = 0;   // ThreadManager WorkerId; 0 = not a managed worker
	u64 iteration = 0;  // the worker's tick count, if it has one
	std::string_view message;

	void* const* frames = nullptr; // a stack captured elsewhere
	int frameCount = 0;
	bool captureStack = true; // capture here when `frames` is null
};

// The POD a reader gets: the same event with its storage owned rather than
// borrowed, so a snapshot can be walked at leisure while the thread runs on.
struct EventView {
	Kind kind = Kind::Exception;
	u32 workerId = 0;
	u64 iteration = 0;
	u64 tsc = 0;    // __rdtsc at the moment of record — puts it on the profiler's timeline
	i64 wallNs = 0; // system_clock nanoseconds, for a human-readable stamp
	u64 index = 0;  // this thread's monotonic event number (survives the wrap)
	char message[kMessageMax] = {};
	void* frames[kStackDepth] = {};
	int frameCount = 0;
};

// One thread's standing health. The ring holds recent DETAIL and wraps; these
// counters hold TOTALS and do not, so "has this worker ever thrown" stays
// answerable after the ring has turned over.
struct ThreadHealth {
	char name[32] = {};
	u32 osThreadId = 0;
	Slot slot = kInvalidSlot;
	bool live = false; // false once the thread exited (or was killed)
	u64 counts[kKindCount] = {};
	u64 total = 0; // every event ever recorded by this thread

	u64 Count(Kind k) const { return counts[static_cast<int>(k)]; }
};

// ----------------------------------------------------------------------------
// Registration. Mirrors Core/AllocTrack and Core/Profile: ThreadManager::Run
// registers every managed worker at thread entry, so coverage is a property of
// being managed rather than of somebody having remembered to wire it.

// Names the calling thread "main" and prepares the table. Call once, early,
// from the exe. Idempotent.
void Init();

// Gives the calling thread a slot and returns it. Registering past kMaxThreads
// is dropped rather than fatal — the thread simply cannot be reported, which is
// the same bargain the allocation counters make. A slot left by a thread that
// unregistered is reused by a same-named successor, and the successor KEEPS the
// predecessor's events (see the header note on reboots).
Slot RegisterThread(std::string_view name);

// The calling thread's slot, or kInvalidSlot if it never registered.
Slot ThisThread();

// Look a slot up by the name it registered under, for a caller recording an
// event ABOUT another thread. Returns the LIVE slot of that name if there is
// one. kInvalidSlot if nothing matches.
Slot FindThread(std::string_view name);

// Marks the calling thread's slot dormant, keeping its events readable and
// freeing the name for a successor. A thread that is force-terminated never
// reaches this, so its slot stays live — deliberately, since the events it died
// holding are the reason to look.
void UnregisterThisThread();

// ----------------------------------------------------------------------------
// Recording. Both forms are safe from any thread, allocate nothing, take no
// lock and never throw.

// Records against the CALLING thread's slot.
void Record(const Event& e);

// Records against ANOTHER thread's slot — the supervisor logging a stall
// against the worker it watches, or Kill logging a termination against the
// thread it just ended. The event lands on that thread's timeline, which is
// where a reader looks for it, rather than on the reporter's.
void RecordFor(Slot slot, const Event& e);

// ----------------------------------------------------------------------------
// Reading.

// Copies every registered thread's standing health into `out` (capacity
// kMaxThreads); returns how many were written.
int SnapshotThreads(ThreadHealth* out, int capacity);

// Copies one thread's recent events, OLDEST FIRST, into `out`. Returns how many
// were written — at most min(capacity, kEventsPerThread), fewer if the thread
// has recorded fewer or if a slot was being overwritten as it was read (a
// recycled slot is skipped rather than returned half-torn).
int ReadEvents(Slot slot, EventView* out, int capacity);

// Everything recorded by anyone, newest first, across all threads — what a
// console command prints when asked "what has gone wrong". Returns how many
// were written.
int ReadAllEvents(EventView* out, int capacity, Slot* slotOut = nullptr);

// Process-wide totals, for a one-line "3 exceptions, 1 stall" summary.
struct Totals {
	u64 counts[kKindCount] = {};
	u64 total = 0;
	u64 Count(Kind k) const { return counts[static_cast<int>(k)]; }
};
Totals ProcessTotals();

} // namespace dungeon::diag
