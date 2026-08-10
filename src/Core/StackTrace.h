// ============================================================================
// Core/StackTrace.h — capturing, walking and symbolizing call stacks.
//
// Lifted out of Core/AllocTrack, which grew a private symbolizer for its
// violating-frame report. The health record wants the same thing, and two
// copies of a DbgHelp wrapper is how they drift.
//
// THE HARD PART IS GETTING A USEFUL STACK FOR A C++ THROW. At a `catch` site the
// stack has ALREADY UNWOUND: the frames between the throw and the catch are
// gone, so capturing there tells you where the exception was handled, which you
// knew, and not where it came from, which you wanted. The fix is to look
// EARLIER — a vectored exception handler runs on the throwing thread at the
// moment of the throw, before any unwinding, and stashes the stack for the catch
// site to pick up. That is what InstallThrowCapture / ThrowFrames are for.
//
// An SEH FAULT is the mirror image: nothing unwinds, but the interesting stack
// is not the handler's — it is in the CONTEXT_RECORD the filter is handed.
// WalkContext walks that instead of the caller's own frames.
//
// DBGHELP IS NOT THREAD-SAFE. Every entry point here that touches it serializes
// on one mutex. That is fine for the log path (which already takes the log's own
// lock) and is exactly why the RECORD in Core/Diagnostics does not go through
// here: recording must stay lock-free, and symbolizing is a thing done later,
// for a human, on a thread that can afford to wait.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <string>

namespace dungeon::stack {

// RtlCaptureStackBackTrace's own ceiling on the frames it will walk.
inline constexpr int kMaxFrames = 62;

// Loads symbols. Idempotent and thread-safe; called lazily by Describe, so a
// run that never symbolizes never pays for it.
void Init();

// This thread's current call stack. `skip` drops the innermost frames (1 = the
// caller of Capture). Does not allocate — safe on a failure path.
int Capture(void** out, int max, int skip = 1);

// The stack described by an SEH CONTEXT record, which is where a fault's real
// frames live — the filter's own stack says only that a filter ran. `context`
// is a CONTEXT*. Serializes on the DbgHelp lock.
int WalkContext(void* context, void** out, int max);

// One frame as "func (file:line)", best effort: a build without a PDB falls
// back to the bare address. Serializes on the DbgHelp lock.
std::string Describe(void* address);

// Identity of a stack, for "have I already reported this one".
u64 Hash(void* const* frames, int depth);

// A bounded set of stacks already reported. Fixed storage: a standing failure
// must not grow anything, and 64 distinct sites in one session is long past the
// point where the log has made its case. Each consumer owns its own, so the
// allocation guard's sites and the health record's cannot mask each other.
class SeenSet {
public:
	// True the first time this hash is offered (and remembers it).
	bool FirstSighting(u64 hash);
	void Reset() { m_count = 0; }

private:
	static constexpr int kMax = 64;
	u64 m_hashes[kMax] = {};
	int m_count = 0;
};

// Logs a symbolized stack, one frame a line, at Warn. Skips the std:: plumbing
// and stops at the entry point: a dozen frames of container internals say only
// "something grew", and everything above wWinMain is CRT scaffolding.
void LogStack(void* const* frames, int depth, const char* indent = "      ");

// --- throw-time capture -----------------------------------------------------

// Installs a vectored exception handler that records the stack of every C++
// throw, on the throwing thread, BEFORE unwinding. Costs one stack capture per
// throw in the process — this engine throws rarely, and the alternative is a
// catch-site stack that cannot name the thrower. Idempotent.
void InstallThrowCapture();

// The stack of the most recent throw seen on THIS thread; 0 if none, or if the
// capture is not installed. Meant to be read from a catch site, where the most
// recent throw is the one being handled.
//
// CAVEAT: it is the LAST throw, not "the throw of the exception you hold". An
// exception thrown DURING unwinding overwrites it, so a nested failure can
// leave the outer catch looking at the inner one's stack. That is rare and
// visible (the frames will not match the message), and the alternative — keying
// captures to exception objects — needs machinery the failure path should not
// have.
int ThrowFrames(void** out, int max);

// How many throws the handler has seen this run, for a readout to report.
u64 ThrowsSeen();

} // namespace dungeon::stack
