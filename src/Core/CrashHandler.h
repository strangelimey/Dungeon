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
//     A STACK OVERFLOW needs two things more, or its report faults on the
//     exhausted stack and leaves nothing (code-review C388): every thread the
//     engine starts keeps 64 KB of stack back for it (GuardThreadStack), and the
//     heavy half of every report - the dump, the log line, the stack walk - runs
//     on a REPORTER thread made at Install, while the failing thread waits.
//   • std::terminate — an exception escaping a noexcept function or a thread's
//     top level, and (before this) the way a throwing frame ended the process
//     in silence.
//   • DN_ASSERT — routed through ReportFatal so the record and the dump land
//     BEFORE abort(), which in a debug build otherwise leaves a CRT dialog and
//     a process that looks alive.
//
// THE ORDER, in every handler: RECORD the event quietly, write the DUMP, then
// LOG one line - the event and what became of the dump - with the stack under
// it, symbolized last (code-review C385). Decreasing order of how likely each
// step is to survive: the record is a fixed buffer, the dump calls into
// dbghelp, and the log formats, allocates and takes locks. An SEH fault's real
// stack lives in its CONTEXT_RECORD, so it is walked (StackWalk64 against that
// context) after the line; the MINIDUMP carries the same stack, and every
// thread's, for a debugger.
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

// Installs the fault filter and the terminate handler, starts the reporter
// thread, snapshots the paths the crash path will need, and guards the calling
// thread's stack (GuardThreadStack - the caller is the exe's main thread). Call
// once, early, from an entry point - after diag::Init() so the record exists to
// write into. Idempotent.
void Install();

// Keeps 64 KB of the CALLING thread's stack back for reporting its own stack
// overflow (SetThreadStackGuarantee): an overflow is raised that much earlier,
// so the fault filter has room to record it and hand the report over. Install
// does the main thread; ThreadManager does every worker and its supervisor at
// thread entry. Needs nothing installed.
void GuardThreadStack();

// UNATTENDED: no dialog may wait for a person. Everything that RECORDS a crash
// is untouched (ReportFatal's record, log line and minidump all land first, as
// ever); this only stops the two things that then sit on the desktop waiting
// to be clicked - the debug CRT's "abort() has been called" box and the
// Windows crash box - so the process simply ENDS. `-headless` turns it on
// (Main), and so does `-unattended` for a harness run that must draw: a test
// run that tripped an assert used to hang on a modal dialog until its timeout,
// on the screen of whoever was at the machine, looking exactly like a real
// crash (docs/level-building.md P5).
void SetUnattended();

// Records a Fatal event, writes a dump, then logs one line saying both, with the
// stack - everything that must happen while the process is still able to do it.
// Does NOT abort: the caller decides, because DN_ASSERT wants abort() and a
// repeat-limit shutdown wants an orderly exit.
void ReportFatal(std::string_view what);

// A FATAL NOTE: what one library alone knows about a dying process, added to
// every fatal report - ReportFatal, the fault filter and the terminate handler.
// Graphics installs one (gfx::WatchDevice) that logs a removed GPU device's
// reason and DRED's breadcrumbs (code-review C195), because a TDR surfaces
// wherever the next GPU-touching call happens to be: a failed HRESULT, but as
// often an assert or a fault inside the driver. ONE slot; null removes it.
//
// It runs LAST in each report - after the record, the dump and the log line,
// and after a fault's stack walk - since it calls into a library that may be
// the thing that broke: a note that hangs or faults costs only itself. It must
// not throw.
using FatalNote = void (*)();
void SetFatalNote(FatalNote note);

// The handlers write a minidump beside the exe, <exe>-<tag>-<pid>-<n>.dmp, with
// the failing thread's exception context when there is one (an SEH fault) - the
// faulting register state - and every thread's stack either way. At most this
// many a run: a repeating fault must not fill the disk, and by the third dump of
// the same crash there is nothing new in the fourth. (WriteDump and DumpsWritten
// went with code-review C385: the handlers dump through the reporter, and no
// readout ever asked for the count.)
inline constexpr int kMaxDumps = 3;

} // namespace dungeon::crash
