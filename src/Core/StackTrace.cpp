// ============================================================================
// Core/StackTrace.cpp — see StackTrace.h.
// ============================================================================
#include "Core/StackTrace.h"

#include "Core/AllocTrack.h"
#include "Core/Log.h"

#include <atomic>
#include <cstring>
#include <format>
#include <mutex>

#include <windows.h>
// After windows.h, always.
#include <dbghelp.h>

namespace dungeon::stack {

namespace {

// DbgHelp is single-threaded by contract: every Sym* call in this file is under
// this. See the header — the record deliberately does not come through here.
std::mutex g_symMx;
bool g_symInited = false;

void EnsureSymbols() { // caller holds g_symMx
	if (g_symInited) return;
	SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
	SymInitialize(GetCurrentProcess(), nullptr, TRUE);
	g_symInited = true;
}

// MSVC's C++ exception code — the 'msc' magic in the high bytes. Every `throw`
// in a Microsoft-toolchain binary raises this, which is what makes a vectored
// handler able to see throws at all.
constexpr DWORD kCppExceptionCode = 0xE06D7363;

// The throwing thread's own stash. Constant-initialized POD: the handler runs
// on an arbitrary thread at an arbitrary moment, so nothing here may have a
// dynamic initializer or a destructor.
thread_local constinit void* t_throwFrames[kMaxFrames] = {};
thread_local constinit int t_throwCount = 0;

std::atomic<u64> g_throws{0};
std::atomic<bool> g_captureInstalled{false};

LONG CALLBACK ThrowCapture(EXCEPTION_POINTERS* info) {
	// Only C++ throws. Everything else that passes through here — breakpoints, a
	// first-chance fault the debugger will handle, the DBG_PRINTEXCEPTION a
	// library raises for OutputDebugString — would cost a stack walk and tell us
	// nothing, and this handler sees EVERY exception in the process.
	if (info && info->ExceptionRecord &&
		info->ExceptionRecord->ExceptionCode == kCppExceptionCode) {
		// Skip this frame and the RaiseException plumbing under it, so the top of
		// the stored stack is the code that actually wrote `throw`.
		t_throwCount = static_cast<int>(
			::RtlCaptureStackBackTrace(2, kMaxFrames, t_throwFrames, nullptr));
		g_throws.fetch_add(1, std::memory_order_relaxed);
	}
	// ALWAYS continue the search: this is an observer. Returning
	// EXCEPTION_CONTINUE_EXECUTION here would swallow every throw in the engine.
	return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

// ----------------------------------------------------------------------------

void Init() {
	std::lock_guard lock(g_symMx);
	EnsureSymbols();
}

int Capture(void** out, int max, int skip) {
	if (max > kMaxFrames) max = kMaxFrames;
	return static_cast<int>(::RtlCaptureStackBackTrace(
		static_cast<DWORD>(skip), static_cast<DWORD>(max), out, nullptr));
}

int WalkContext(void* context, void** out, int max) {
	if (!context || max <= 0) return 0;
	std::lock_guard lock(g_symMx);
	EnsureSymbols();

	// StackWalk64 MUTATES the context it walks, so it gets a copy — the original
	// belongs to the exception record and the dump writer still needs it intact.
	CONTEXT ctx = *static_cast<CONTEXT*>(context);

	STACKFRAME64 frame{};
#if defined(_M_X64)
	constexpr DWORD machine = IMAGE_FILE_MACHINE_AMD64;
	frame.AddrPC.Offset = ctx.Rip;
	frame.AddrFrame.Offset = ctx.Rbp;
	frame.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_ARM64)
	constexpr DWORD machine = IMAGE_FILE_MACHINE_ARM64;
	frame.AddrPC.Offset = ctx.Pc;
	frame.AddrFrame.Offset = ctx.Fp;
	frame.AddrStack.Offset = ctx.Sp;
#else
#error "StackTrace::WalkContext needs a register mapping for this architecture"
#endif
	frame.AddrPC.Mode = AddrModeFlat;
	frame.AddrFrame.Mode = AddrModeFlat;
	frame.AddrStack.Mode = AddrModeFlat;

	int n = 0;
	while (n < max &&
		   StackWalk64(machine, GetCurrentProcess(), GetCurrentThread(), &frame, &ctx,
					   nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
		if (frame.AddrPC.Offset == 0) break;
		out[n++] = reinterpret_cast<void*>(frame.AddrPC.Offset);
	}
	return n;
}

std::string Describe(void* address) {
	std::lock_guard lock(g_symMx);
	EnsureSymbols();

	char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
	auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
	symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
	symbol->MaxNameLen = MAX_SYM_NAME;
	const DWORD64 addr = reinterpret_cast<DWORD64>(address);

	DWORD64 displacement = 0;
	if (!SymFromAddr(GetCurrentProcess(), addr, &displacement, symbol))
		return std::format("0x{:016x}", addr);

	IMAGEHLP_LINE64 line{};
	line.SizeOfStruct = sizeof(line);
	DWORD lineDisplacement = 0;
	if (SymGetLineFromAddr64(GetCurrentProcess(), addr, &lineDisplacement, &line)) {
		const char* file = std::strrchr(line.FileName, '\\');
		return std::format("{} ({}:{})", symbol->Name, file ? file + 1 : line.FileName,
						   line.LineNumber);
	}
	return symbol->Name;
}

u64 Hash(void* const* frames, int depth) {
	u64 h = 1469598103934665603ull; // FNV-1a
	for (int i = 0; i < depth; ++i) {
		h ^= reinterpret_cast<u64>(frames[i]);
		h *= 1099511628211ull;
	}
	return h;
}

bool SeenSet::FirstSighting(u64 hash) {
	for (int i = 0; i < m_count; ++i)
		if (m_hashes[i] == hash) return false;
	if (m_count < kMax) m_hashes[m_count++] = hash;
	return true;
}

void LogStack(void* const* frames, int depth, const char* indent) {
	alloc::Excused excuse; // symbolizing and logging both allocate
	for (int i = 0; i < depth; ++i) {
		const std::string frame = Describe(frames[i]);
		// Skip the plumbing. A dozen frames of std::_Allocate / _Buy_raw say only
		// "a container grew", which is already known — the answer is the first
		// frame of OUR code under it. Likewise the throw machinery: a throw-time
		// capture always starts inside ntdll's exception dispatcher (measured:
		// RtlLocateExtendedFeature / RtlRaiseException / CxxThrowException sit on
		// top of every one), and no Rtl* frame is ever ours. Filtered BY NAME
		// rather than by a fixed skip count, because the dispatcher's depth is
		// not ours to depend on.
		const bool plumbing =
			frame.starts_with("std::") || frame.starts_with("operator new") ||
			frame.starts_with("Rtl") || frame.starts_with("Ki") ||
			frame.contains("CxxThrowException") || frame.starts_with("RaiseException");
		if (!plumbing) log::Warn("{}{}", indent, frame);
		// Stop at the entry point: above it is CRT scaffolding that says nothing
		// about which system failed.
		if (frame.starts_with("wWinMain") || frame.starts_with("main")) break;
	}
}

void InstallThrowCapture() {
	if (g_captureInstalled.exchange(true)) return;
	// FIRST in the chain (1), so the stack is taken before anything else has a
	// chance to handle, translate or re-raise the exception.
	::AddVectoredExceptionHandler(1, &ThrowCapture);
}

int ThrowFrames(void** out, int max) {
	const int n = t_throwCount < max ? t_throwCount : max;
	if (n > 0) std::memcpy(out, t_throwFrames, static_cast<size_t>(n) * sizeof(void*));
	return n;
}

u64 ThrowsSeen() { return g_throws.load(std::memory_order_relaxed); }

} // namespace dungeon::stack
