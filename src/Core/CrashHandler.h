// ============================================================================
// Core/CrashHandler.h — the process-level end of the health record.
//
// Core/Diagnostics is the RECORD; this is what feeds it the failures no catch
// clause can see, and what turns "the game vanished" into a report. Three
// sources, all installed by one call:
//
//   • SEH FAULTS — access violation, divide-by-zero, stack overflow. These are
//     not C++ exceptions and no `catch` anywhere will ever see them. They are
//     also the majority of what actually kills a game, which is why the plan
//     scoped them in from the start rather than settling for `catch (...)`.
//   • std::terminate — an exception escaping a noexcept function or a thread's
//     top level, and (before this) the way a throwing frame ended the process
//     in silence.
//   • DN_ASSERT — routed through ReportFatal so the record and the dump land
//     BEFORE abort(), which in a debug build otherwise leaves a CRT dialog and
//     a process that looks alive.
//
// WHAT IT DELIBERATELY DOES NOT DO: symbolize. An SEH fault's real stack lives
// in the CONTEXT_RECORD, not on the handler's own stack, so walking it needs
// StackWalk64 against that context — that is phase 3. What phase 2 gives is the
// exception code, the faulting address, and a MINIDUMP, which carries the full
// stack for a debugger to open afterwards. The dump is the honest answer until
// the walker exists, and stays useful after it.
//
// PATHS ARE SNAPSHOTTED AT INSTALL. paths::ExecutableDir() builds a std::string,
// and a crash path must not touch the heap — it may be running because the heap
// is already broken, or on a thread whose termination leaked the CRT heap lock.
// Install copies what it needs into fixed buffers so the failing path formats
// only into the stack.
// ============================================================================
#pragma once

#include <string_view>

namespace dungeon::crash {

// Installs the fault filter and the terminate handler, and snapshots the paths
// the crash path will need. Call once, early, from an entry point — after
// diag::Init() so the record exists to write into. Idempotent.
void Install();

// Records a Fatal event, logs it, writes a dump and flushes — everything that
// must happen while the process is still able to do it. Does NOT abort: the
// caller decides, because DN_ASSERT wants abort() and a repeat-limit shutdown
// wants an orderly exit.
void ReportFatal(std::string_view what);

// Writes a minidump beside the exe, named <exe>-<tag>-<pid>-<n>.dmp. `context`
// is an EXCEPTION_POINTERS* when one is available (an SEH fault) and null
// otherwise — without it the dump still carries every thread's stack, just not
// the faulting register state. Returns false if dbghelp would not write it.
//
// Bounded per run (kMaxDumps): a repeating fault must not fill the disk, and by
// the third dump of the same crash there is nothing new in the fourth.
bool WriteDump(void* context, std::string_view tag);

inline constexpr int kMaxDumps = 3;

// How many dumps this run has written, for a readout to report.
int DumpsWritten();

} // namespace dungeon::crash
