// ============================================================================
// Core/AllocTrack.h — per-thread heap allocation counters.
//
// ARCHITECTURE.md's memory strategy says a steady-state frame allocates
// nothing on the heap. That has been a CONVENTION verified by hand; these
// counters are the machinery that turns it into something a build can check.
// This header is only the meter — the frame guard that reads it and the
// dev-console readout are layered on top.
//
// WHAT IS COUNTED: every allocation through the global ::operator new family,
// which is all of ours (the engine is one statically-linked exe, and std
// containers route through std::allocator -> ::operator new).
// WHAT IS NOT: raw malloc/HeapAlloc, and anything a DLL allocates inside
// itself (D3D12, DXGI, XAudio2, PDH). That boundary is deliberate — the rule
// is about OUR containers, and driver allocations are not ours to remove.
//
// Counters are THREAD-LOCAL: the main thread and each ThreadManager worker are
// measured separately, because "allocates nothing per tick" is the same
// invariant on both sides and a shared counter would blur them together.
// Note() itself never allocates and never locks, so it is safe to leave on.
//
// EXCUSED SCOPES: a path that is ALLOWED to allocate inside an otherwise
// steady frame (a dev-console command, an editor dialog, a first-time bake)
// wraps itself in alloc::Excused. That does not stop the counting — the
// allocation still lands in `allocs`, it also lands in `excused` — so the raw
// number stays honest and only the VIOLATION count (allocs - excused) forgives
// it. Reporting code must excuse itself too: log::Write formats a std::string,
// so an un-excused report would be its own violation.
//
// LINKER TRAP: replacing ::operator new only takes effect if the linker pulls
// this object file in, and it will not pull an object nothing references out
// of a static lib. alloc::Init() exists to be that reference — Main calls it
// once at startup. Without such a call the CRT's own operator new stays and
// every counter silently reads zero.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <string_view>

namespace dungeon::alloc {

#if DN_ALLOC_TRACK
inline constexpr bool kEnabled = true;
#else
inline constexpr bool kEnabled = false;
#endif

// Cumulative since process start (or the last Reset) for one thread. There is
// no live-bytes figure on purpose: free() does not carry a size on every path,
// and the invariant this exists for only needs "did anything allocate".
struct Counters {
	u64 allocs = 0;  // every ::operator new, excused or not
	u64 excused = 0; // the subset made inside an Excused scope
	u64 frees = 0;
	u64 bytes = 0; // cumulative bytes REQUESTED (not the allocator's rounding)

	u64 Violations() const { return allocs - excused; }
};

// Links the operator-new replacements in and names the calling thread "main".
// Call once, early, from the exe. Idempotent.
void Init();

// Registers the calling thread under `name` so SnapshotAll can report it.
// Names are copied into fixed storage (no allocation); a thread that never
// registers is still counted, just reported as unnamed. Slots are limited
// (kMaxThreads); registration past that is dropped rather than fatal.
void RegisterThread(std::string_view name);

// The calling thread's live counters.
Counters ThisThread();

// Resets the calling thread's counters to zero.
void ResetThisThread();

inline constexpr int kMaxThreads = 64;

struct ThreadReport {
	char name[32] = {};
	u32 osThreadId = 0;
	Counters counters;
};

// Copies every registered thread's counters into `out` (capacity kMaxThreads);
// returns how many were written. Takes a lock only over the slot table, never
// over a counting thread, so it cannot stall a worker mid-tick.
int SnapshotAll(ThreadReport* out, int capacity);

// --- the frame guard --------------------------------------------------------
// BeginFrame latches this thread's counters, ArmFrame says whether the frame
// now running counts as steady state, EndFrame returns the verdict.
//
// STACK CAPTURE is decided at BeginFrame from the PREVIOUS frame's arming, not
// the current one: a frame is armed a few lines into Game::Update, and steady
// state is a RUN of frames rather than a single one, so last frame's answer is
// both available in time and correct in practice. Capture is bounded per frame
// (kMaxFrameStacks) and only happens in a frame that is already broken.
inline constexpr int kMaxFrameStacks = 8;
inline constexpr int kStackDepth = 24;

struct FrameResult {
	bool armed = false;
	u64 violations = 0; // allocations that were not excused
	u64 bytes = 0;
	int stacks = 0; // how many were captured for ReportFrame to symbolize
};

void BeginFrame();
void ArmFrame(bool steady);
FrameResult EndFrame();

// Logs a violating frame: each UNIQUE stack is symbolized once per session
// (DbgHelp), and a frame that only repeats known stacks stays silent so a
// standing violation cannot drown the log. Excuses its own allocations.
// In strict mode a violation asserts instead — off by default, because an
// abort in a debug build leaves a CRT dialog and a process that looks alive.
void ReportFrame(const FrameResult& result);

void SetStrict(bool strict);
bool Strict();

struct GuardStats {
	u64 framesArmed = 0;
	u64 framesViolating = 0;
	u64 violations = 0;
	u64 stacksReported = 0;
};
GuardStats Stats();
void ResetStats();

// RAII: allocations on this thread while alive are counted AND excused. Nests.
class Excused {
public:
	Excused();
	~Excused();

	Excused(const Excused&) = delete;
	Excused& operator=(const Excused&) = delete;
};

} // namespace dungeon::alloc
