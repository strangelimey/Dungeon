// ============================================================================
// Core/CrashHandler.cpp — see CrashHandler.h.
//
// Everything below runs on a process that may already be damaged, so the rules
// are stricter than ordinary code: no heap, no locks that a failing thread
// might hold, fixed buffers, and every path ends whether or not the step before
// it worked. Where a choice was available between a richer report and a report
// that definitely arrives, the report that arrives wins.
//
// THE ORDER, in every handler (code-review C385): RECORD the event quietly, then
// write the DUMP, then LOG it once - in decreasing order of how likely each is
// to survive a damaged process. The record is a fixed buffer and a plain store;
// the dump calls into dbghelp; the log formats, allocates, takes the log's mutex
// and symbolizes the stack through DbgHelp's. It used to be record-and-log, then
// dump, then log again: the riskiest step ran first, and every crash said itself
// twice.
// ============================================================================
#include "Core/CrashHandler.h"

#include "Core/AllocTrack.h"
#include "Core/Diagnostics.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/StackTrace.h"

#include <atomic>
#include <crtdbg.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#include <windows.h>
// After windows.h, always.
#include <dbghelp.h>

namespace dungeon::crash {

namespace {

// Snapshotted at Install so the crash path never calls into paths:: (which
// builds std::strings) — see the header note on the heap.
constinit char g_dir[MAX_PATH] = {};
constinit char g_exe[64] = {};
constinit std::atomic<int> g_dumps{0};
constinit std::atomic<bool> g_installed{false};

// Re-entrancy guard. A fault raised INSIDE the handler (a broken stack walk, a
// dump write that faults) must not loop back in — one report is worth having,
// an infinite regress of them is worth nothing and never terminates.
constinit std::atomic<bool> g_handling{false};

// How much of its stack a thread keeps back for reporting its own overflow
// (GuardThreadStack). The faulting thread only records and hands over, but the
// exception dispatch, the filter and a formatted fault description all run on
// what is left - a few KB without this, which is not enough for even that.
constexpr ULONG kStackGuarantee = 64 * 1024;

// How long a failing thread waits for the reporter. A dump with indirectly
// referenced memory takes a few seconds, and the first symbolized stack loads
// the PDBs; past this, the reporter is taken to be stuck - on a lock the failing
// thread holds - and the process ends with the record and, by then, the dump.
constexpr DWORD kReportWaitMs = 30'000;

void CopyFixed(char* dst, size_t cap, std::string_view src) {
	const size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
	if (n) std::memcpy(dst, src.data(), n);
	dst[n] = '\0';
}

// The human name for an SEH code. A switch rather than a table lookup so the
// strings are literals in .rdata and nothing has to be built at crash time.
const char* FaultName(DWORD code) {
	switch (code) {
	case EXCEPTION_ACCESS_VIOLATION: return "access violation";
	case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
	case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
	case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "float divide by zero";
	case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
	case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
	case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
	case EXCEPTION_DATATYPE_MISALIGNMENT: return "datatype misalignment";
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
	case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "noncontinuable exception";
	default: return "unknown fault";
	}
}

// An access violation says in its parameters whether it was a read, a write or
// an execute, and at what address. That trio is often the whole diagnosis, so it
// is worth spelling out rather than printing a bare code.
void DescribeFault(char* out, size_t cap, const EXCEPTION_RECORD& rec) {
	if (rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
		rec.NumberParameters >= 2) {
		const ULONG_PTR kind = rec.ExceptionInformation[0];
		const char* verb = kind == 0 ? "reading" : kind == 1 ? "writing" : "executing";
		std::snprintf(out, cap, "access violation %s 0x%llx at 0x%llx", verb,
					  static_cast<unsigned long long>(rec.ExceptionInformation[1]),
					  reinterpret_cast<unsigned long long>(rec.ExceptionAddress));
		return;
	}
	std::snprintf(out, cap, "%s (code 0x%08lx) at 0x%llx", FaultName(rec.ExceptionCode),
				  static_cast<unsigned long>(rec.ExceptionCode),
				  reinterpret_cast<unsigned long long>(rec.ExceptionAddress));
}

// Writes the dump. `threadId` names the failing thread, which need not be the
// caller: the reporter dumps a thread that is waiting on it. Returns the dump's
// number, 1-based, or 0 when none was written.
int WriteDumpOf(EXCEPTION_POINTERS* info, DWORD threadId, std::string_view tag) {
	const int n = g_dumps.fetch_add(1);
	if (n >= kMaxDumps) return 0; // a repeating fault must not fill the disk
	if (!g_dir[0]) return 0;      // Install was never called

	char tagBuf[32];
	CopyFixed(tagBuf, sizeof(tagBuf), tag);

	char path[MAX_PATH];
	std::snprintf(path, sizeof(path), "%s\\%s-%s-%lu-%d.dmp", g_dir, g_exe, tagBuf,
				  ::GetCurrentProcessId(), n);

	const HANDLE file = ::CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
									  FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return 0;

	// ClientPointers FALSE: the pointers are this process's own, whichever of its
	// threads is writing.
	MINIDUMP_EXCEPTION_INFORMATION mei{};
	mei.ThreadId = threadId;
	mei.ExceptionPointers = info;
	mei.ClientPointers = FALSE;

	// WithIndirectlyReferencedMemory pulls in the memory the stacks point AT, not
	// just the stacks - the difference between seeing a pointer parameter and
	// seeing what it pointed to. Costs megabytes; a crash dump is not the place
	// to economise.
	const auto type = static_cast<MINIDUMP_TYPE>(
		MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
		MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules);

	const BOOL ok = ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(),
										file, type, info ? &mei : nullptr, nullptr,
										nullptr);
	::CloseHandle(file);
	return ok ? n + 1 : 0;
}

// ----------------------------------------------------------------------------
// THE REPORTER. A thread made at Install that does nothing until something
// fails, and then does the expensive half of the report - the dump, the log
// line, the stack walk - on a stack of its own, while the failing thread waits.
//
// It exists for STACK OVERFLOW (code-review C388). A filter runs on the faulting
// thread, and an overflowed thread has only what GuardThreadStack kept back;
// MiniDumpWriteDump, std::format and StackWalk64 over PDBs need far more, so the
// report faulted again inside itself, met the re-entrancy guard and left no
// dump. It helps every other crash too: a thread dumped from OUTSIDE shows its
// real stack in the dump rather than the dumper's, and a failing thread that
// holds the log's lock, DbgHelp's or the heap's no longer deadlocks its own
// report - its wait for the reporter times out instead, with the dump on disk.

// One report. Every pointer is into the failing thread's frame, which stays put
// while that thread waits.
struct Job {
	EXCEPTION_POINTERS* info = nullptr; // a fault's, else null
	DWORD threadId = 0;                 // the failing thread
	diag::Slot slot = diag::kInvalidSlot;
	u64 event = diag::kNoEvent;    // its quiet record (diag::kNoEvent: unrecorded)
	diag::Kind kind = diag::Kind::Fatal;
	const char* what = "";         // the message, for a thread with no record
	const char* tag = "";          // the dump's name: fault / terminate / fatal
	const char* lead = "";         // before the kind in the log line
	const char* fate = "";         // after the message, before the dump's status
	bool walkContext = false;      // a fault: walk its CONTEXT for the log
};

constinit HANDLE g_reporter = nullptr;
constinit DWORD g_reporterId = 0;
constinit HANDLE g_wake = nullptr; // auto-reset: a job is posted
constinit HANDLE g_done = nullptr; // auto-reset: the job is finished
// Claimed by the thread posting a job, so two failing at once cannot both post
// one; the loser reports from its own stack. Left claimed after a wait times
// out, so no later report waits on a reporter that is stuck.
constinit std::atomic<bool> g_reporterBusy{false};
constinit Job g_job{};

// The dump and the log, in that order. Runs on the reporter, or on the failing
// thread when there is no reporter to run it.
void RunJob(const Job& job) {
	const int dump = WriteDumpOf(job.info, job.threadId, job.tag);

	// What became of the dump, after whatever the handler says of the process.
	char status[96];
	if (dump > 0)
		std::snprintf(status, sizeof(status), "minidump %d of %d written beside the exe", dump,
					  kMaxDumps);
	else if (!g_dir[0])
		std::snprintf(status, sizeof(status), "no minidump (crash::Install was not called)");
	else if (g_dumps.load() > kMaxDumps)
		std::snprintf(status, sizeof(status), "no minidump (this run's %d are written)",
					  kMaxDumps);
	else
		std::snprintf(status, sizeof(status), "no minidump (dbghelp would not write one)");
	char note[192];
	std::snprintf(note, sizeof(note), "%s%s%s", job.fate, job.fate[0] ? "; " : "", status);

	// ONE line, the record's: the event and the dump's status together. A thread
	// with no record (one nothing registered) still gets its line, in the same
	// words.
	if (!diag::LogRecorded(job.slot, job.event, job.lead, note)) {
		alloc::Excused excuse;
		log::Error("diag · {}{} on an unregistered thread (tid {}): {} - {}", job.lead,
				   diag::KindName(job.kind), job.threadId, job.what, note);
	}

	// A fault's stack LAST. Its real frames are in the CONTEXT_RECORD - the
	// filter's own stack says only that a filter ran - and walking it loads PDBs
	// under DbgHelp's lock, the riskiest thing done anywhere on this path. By now
	// the record is in memory and the dump on disk, so if the walk dies it costs a
	// convenience, not the evidence; it is here so the ANSWER is in dungeon.log
	// without opening the dump.
	if (job.walkContext && job.info && job.info->ContextRecord) {
		const bool own = job.threadId == ::GetCurrentThreadId();
		const HANDLE thread =
			own ? nullptr
				: ::OpenThread(THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT, FALSE, job.threadId);
		void* frames[stack::kMaxFrames];
		const int n =
			stack::WalkContext(job.info->ContextRecord, frames, stack::kMaxFrames, thread);
		if (thread) ::CloseHandle(thread);
		if (n > 0) {
			{
				alloc::Excused excuse;
				log::Error("  faulting stack:");
			}
			stack::LogStack(frames, n, "    ");
		}
	}
}

DWORD WINAPI ReporterMain(void*) {
	for (;;) {
		if (::WaitForSingleObject(g_wake, INFINITE) != WAIT_OBJECT_0) return 0;
		// A throw out of the log path must not end the reporter with the failing
		// thread still waiting on it.
		try {
			RunJob(g_job);
		} catch (...) {
		}
		::SetEvent(g_done);
	}
}

// Has the report written: by the reporter when it can take it, else here.
// Returns once the report is written or the wait has run out.
void Deliver(const Job& job) {
	const bool viaReporter = g_reporter && ::GetCurrentThreadId() != g_reporterId &&
							 !g_reporterBusy.exchange(true);
	if (!viaReporter) {
		RunJob(job);
		return;
	}
	g_job = job;
	::SetEvent(g_wake);
	if (::WaitForSingleObject(g_done, kReportWaitMs) == WAIT_OBJECT_0)
		g_reporterBusy.store(false);
}

LONG WINAPI FaultFilter(EXCEPTION_POINTERS* info) {
	// Already reporting: let the process die rather than recurse.
	if (g_handling.exchange(true)) return EXCEPTION_EXECUTE_HANDLER;

	char what[256] = "fault (no record)";
	if (info && info->ExceptionRecord) DescribeFault(what, sizeof(what), *info->ExceptionRecord);

	// The record, quietly: everything this thread does is on what may be the
	// last few KB of its stack. The rest is the reporter's.
	const u64 event = diag::Record({.kind = diag::Kind::Fault,
									.message = what,
									.captureStack = false,
									.log = false});
	Deliver({.info = info,
			 .threadId = ::GetCurrentThreadId(),
			 .slot = diag::ThisThread(),
			 .event = event,
			 .kind = diag::Kind::Fault,
			 .what = what,
			 .tag = "fault",
			 .lead = "CRASH: ",
			 .fate = "the process is going down",
			 .walkContext = true});

	// EXECUTE_HANDLER, not CONTINUE_SEARCH: the report is written, and letting
	// it fall through would hand the process to the OS error dialog with nothing
	// gained. The process ends here, deliberately, with evidence on disk.
	return EXCEPTION_EXECUTE_HANDLER;
}

void TerminateHandler() {
	if (g_handling.exchange(true)) std::abort();

	// std::terminate is usually reached WITH an exception in flight — rethrowing
	// it inside the handler is the only way to ask what it was.
	char what[256] = "std::terminate called with no exception in flight";
	if (std::exception_ptr ep = std::current_exception()) {
		try {
			std::rethrow_exception(ep);
		} catch (const std::exception& e) {
			std::snprintf(what, sizeof(what), "unhandled exception: %s", e.what());
		} catch (...) {
			std::snprintf(what, sizeof(what),
						  "unhandled exception not derived from std::exception");
		}
	}

	const u64 event = diag::Record({.kind = diag::Kind::Fatal, .message = what, .log = false});
	Deliver({.threadId = ::GetCurrentThreadId(),
			 .slot = diag::ThisThread(),
			 .event = event,
			 .kind = diag::Kind::Fatal,
			 .what = what,
			 .tag = "terminate",
			 .lead = "TERMINATE: ",
			 .fate = "std::terminate, the process aborts"});
	std::abort();
}

} // namespace

// ----------------------------------------------------------------------------

void Install() {
	if (g_installed.exchange(true)) return;
	CopyFixed(g_dir, sizeof(g_dir), paths::ExecutableDir());
	CopyFixed(g_exe, sizeof(g_exe), paths::ExecutableName());

	// The reporter, before any handler can need it. Without it every report runs
	// on the failing thread, as it always used to - so a failure here costs the
	// overflow case, not the rest.
	g_wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
	g_done = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (g_wake && g_done) {
		DWORD id = 0;
		const HANDLE thread = ::CreateThread(nullptr, 0, &ReporterMain, nullptr, 0, &id);
		if (thread) {
			::SetThreadDescription(thread, L"crash.reporter");
			g_reporterId = id;
			g_reporter = thread;
		}
	}
	// The calling thread is the exe's main thread; ThreadManager guards every
	// worker it starts.
	GuardThreadStack();

	::SetUnhandledExceptionFilter(&FaultFilter);
	std::set_terminate(&TerminateHandler);
	// The throw-time stack capture. Without it every exception in the record
	// carries its CATCH site's frames, which name the handler and never the
	// thrower — see Core/StackTrace.
	stack::InstallThrowCapture();

	log::Info("crash handlers installed (fault filter, terminate handler, throw-time "
			  "stack capture, {}; {} KB of stack kept back for an overflow; up to {} "
			  "minidumps per run)",
			  g_reporter ? "a reporter thread" : "NO reporter thread - reports run on the "
												 "failing thread",
			  kStackGuarantee / 1024, kMaxDumps);
}

void GuardThreadStack() {
	ULONG size = kStackGuarantee;
	::SetThreadStackGuarantee(&size);
}

void SetUnattended() {
	// abort()'s own message box, and its hand-off to Windows Error Reporting.
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	// The debug CRT's report windows (its own asserts and errors) go to the
	// debugger's output instead of a window. A no-op in release.
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
	// And the system's own "stopped working" box for a fault the filter passes on.
	::SetErrorMode(::GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	log::Info("crash: unattended - a fatal error records, dumps and EXITS; no dialog waits");
}

void ReportFatal(std::string_view what) {
	// Not re-entrancy-guarded the way the handlers are: an assert firing while a
	// fault is being reported is still worth recording, and this path does not
	// recurse into itself (on the reporter it runs in place - Deliver).
	char msg[256];
	CopyFixed(msg, sizeof(msg), what);

	// Recorded quietly WITH its stack (a plain RtlCaptureStackBackTrace - nothing
	// to symbolize yet), dumped, then ONE line saying both and the stack under it.
	const u64 event = diag::Record({.kind = diag::Kind::Fatal, .message = msg, .log = false});
	Deliver({.threadId = ::GetCurrentThreadId(),
			 .slot = diag::ThisThread(),
			 .event = event,
			 .kind = diag::Kind::Fatal,
			 .what = msg,
			 .tag = "fatal"});
}

} // namespace dungeon::crash
