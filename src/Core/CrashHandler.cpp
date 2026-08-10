// ============================================================================
// Core/CrashHandler.cpp — see CrashHandler.h.
//
// Everything below runs on a process that may already be damaged, so the rules
// are stricter than ordinary code: no heap, no locks that a failing thread
// might hold, fixed buffers, and every path ends whether or not the step before
// it worked. Where a choice was available between a richer report and a report
// that definitely arrives, the report that arrives wins.
// ============================================================================
#include "Core/CrashHandler.h"

#include "Core/AllocTrack.h"
#include "Core/Diagnostics.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/StackTrace.h"

#include <atomic>
#include <cstdio>
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

LONG WINAPI FaultFilter(EXCEPTION_POINTERS* info) {
	// Already reporting: let the process die rather than recurse.
	if (g_handling.exchange(true)) return EXCEPTION_EXECUTE_HANDLER;

	char what[256] = "fault (no record)";
	if (info && info->ExceptionRecord) DescribeFault(what, sizeof(what), *info->ExceptionRecord);

	// The record first, the dump second, the log last — in decreasing order of
	// how likely each is to survive a damaged process. The ring is a fixed
	// buffer and a plain store; the dump calls into dbghelp; the log formats and
	// takes a mutex. If only the first works, there is still a record in memory
	// for a debugger to find.
	diag::Record({.kind = diag::Kind::Fault, .message = what, .captureStack = false});
	const bool dumped = WriteDump(info, "fault");

	{
		alloc::Excused excuse;
		log::Error("CRASH: {} — the process is going down.{}", what,
				   dumped ? " A minidump was written beside the exe." : "");
	}

	// The stack LAST, and deliberately so. A fault's real frames are in the
	// CONTEXT_RECORD — the filter's own stack says only that a filter ran — but
	// walking it loads PDBs and takes DbgHelp's lock, which is the riskiest thing
	// done anywhere on this path. By the time it runs the record is in memory and
	// the dump is on disk, so if the walk dies it costs a convenience, not the
	// evidence. (The dump carries the same stack for a debugger regardless; this
	// is so the ANSWER is in dungeon.log without opening one.)
	if (info && info->ContextRecord) {
		void* frames[stack::kMaxFrames];
		const int n = stack::WalkContext(info->ContextRecord, frames, stack::kMaxFrames);
		if (n > 0) {
			{
				alloc::Excused excuse;
				log::Error("  faulting stack:");
			}
			stack::LogStack(frames, n, "    ");
		}
	}

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

	diag::Record({.kind = diag::Kind::Fatal, .message = what});
	const bool dumped = WriteDump(nullptr, "terminate");
	{
		alloc::Excused excuse;
		log::Error("TERMINATE: {}{}", what,
				   dumped ? " — a minidump was written beside the exe." : "");
	}
	std::abort();
}

} // namespace

// ----------------------------------------------------------------------------

void Install() {
	if (g_installed.exchange(true)) return;
	CopyFixed(g_dir, sizeof(g_dir), paths::ExecutableDir());
	CopyFixed(g_exe, sizeof(g_exe), paths::ExecutableName());

	::SetUnhandledExceptionFilter(&FaultFilter);
	std::set_terminate(&TerminateHandler);
	// The throw-time stack capture. Without it every exception in the record
	// carries its CATCH site's frames, which name the handler and never the
	// thrower — see Core/StackTrace.
	stack::InstallThrowCapture();

	log::Info("crash handlers installed (fault filter, terminate handler, throw-time "
			  "stack capture; up to {} minidumps per run)",
			  kMaxDumps);
}

bool WriteDump(void* context, std::string_view tag) {
	const int n = g_dumps.fetch_add(1);
	if (n >= kMaxDumps) return false; // a repeating fault must not fill the disk
	if (!g_dir[0]) return false;      // Install was never called

	char tagBuf[32];
	CopyFixed(tagBuf, sizeof(tagBuf), tag);

	char path[MAX_PATH];
	std::snprintf(path, sizeof(path), "%s\\%s-%s-%lu-%d.dmp", g_dir, g_exe, tagBuf,
				  ::GetCurrentProcessId(), n);

	const HANDLE file = ::CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
									  FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;

	MINIDUMP_EXCEPTION_INFORMATION mei{};
	mei.ThreadId = ::GetCurrentThreadId();
	mei.ExceptionPointers = static_cast<EXCEPTION_POINTERS*>(context);
	mei.ClientPointers = FALSE;

	// WithIndirectlyReferencedMemory pulls in the memory the stacks point AT, not
	// just the stacks — the difference between seeing a pointer parameter and
	// seeing what it pointed to. Costs megabytes; a crash dump is not the place
	// to economise.
	const auto type = static_cast<MINIDUMP_TYPE>(
		MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
		MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules);

	const BOOL ok = ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(),
										file, type, context ? &mei : nullptr, nullptr,
										nullptr);
	::CloseHandle(file);
	return ok != FALSE;
}

int DumpsWritten() {
	const int n = g_dumps.load();
	return n < kMaxDumps ? n : kMaxDumps;
}

void ReportFatal(std::string_view what) {
	// Not re-entrancy-guarded the way the handlers are: an assert firing while a
	// fault is being reported is still worth recording, and this path does not
	// recurse into itself.
	char msg[256];
	CopyFixed(msg, sizeof(msg), what);

	// Record() logs the message itself (at Error, for a Fatal), so this adds only
	// what it cannot know — whether a dump landed. Logging the text twice would
	// double every assertion in the file the log exists to make readable.
	diag::Record({.kind = diag::Kind::Fatal, .message = msg});
	if (WriteDump(nullptr, "fatal")) {
		alloc::Excused excuse;
		log::Error("a minidump was written beside the exe (dump {} of {})",
				   DumpsWritten(), kMaxDumps);
	}
}

} // namespace dungeon::crash
