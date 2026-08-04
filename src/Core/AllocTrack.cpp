// ============================================================================
// Core/AllocTrack.cpp — the ::operator new replacements and the slot table.
//
// Counting is LOCK-FREE: every allocation touches only the calling thread's
// own thread_local Slot, which is constant-initialized (no dynamic init, so
// first touch cannot re-enter the allocator). The mutex here guards the slot
// TABLE — a registry of who to report — and is taken by registration and by
// SnapshotAll, never by Note().
//
// The table holds POINTERS to live thread_local slots plus a copy of the final
// counters, so a worker that exits (or is rebooted by ThreadManager) leaves its
// totals behind instead of a dangling read.
// ============================================================================
#include "Core/AllocTrack.h"

#include "Core/Assert.h"
#include "Core/Log.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <new>
#include <string>

#include <windows.h>
// After windows.h, always.
#include <dbghelp.h>

namespace dungeon::alloc {

namespace {

struct Slot {
	Counters counters;
	int excusedDepth = 0;
	int tableIndex = -1;

	// Frame guard (main thread today; per-thread so a worker tick can use it).
	Counters frameStart;
	bool armed = false;
	bool capturing = false;
	int stackCount = 0;
	int stackDepth[kMaxFrameStacks] = {};
	void* stacks[kMaxFrameStacks][kStackDepth] = {};
};

// Constant-initialized POD: touching it never allocates, which is what makes
// it safe to read from inside operator new.
thread_local Slot t_slot{};

struct Entry {
	Slot* live = nullptr; // null once that thread exited
	Counters last;        // its counters at exit, so a dead worker still reports
	char name[32] = {};
	u32 osId = 0;
	bool used = false;
};

std::mutex g_mx;
Entry g_entries[kMaxThreads];
bool g_inited = false;

// Clears this thread's table entry at thread exit, keeping its final numbers.
struct ExitHook {
	~ExitHook() {
		if (t_slot.tableIndex < 0) return;
		std::lock_guard lock(g_mx);
		Entry& e = g_entries[t_slot.tableIndex];
		e.last = t_slot.counters;
		e.live = nullptr;
	}
};

} // namespace

// Called from the operator new/delete replacements below. Never allocates,
// never locks, never throws.
void Note(size_t bytes) noexcept {
	Slot& s = t_slot;
	++s.counters.allocs;
	s.counters.bytes += bytes;
	if (s.excusedDepth > 0) {
		++s.counters.excused;
		return;
	}
	// Only reached in a frame the guard already expects to be clean, and only
	// for the first few — CaptureStackBackTrace does not allocate, so it is
	// safe here, but it is not free.
	if (s.capturing && s.stackCount < kMaxFrameStacks) {
		const int n = static_cast<int>(::RtlCaptureStackBackTrace(
			1, kStackDepth, s.stacks[s.stackCount], nullptr));
		s.stackDepth[s.stackCount] = n;
		++s.stackCount;
	}
}

void NoteFree() noexcept { ++t_slot.counters.frees; }

void RegisterThread(std::string_view name) {
	// The ExitHook's dynamic init may itself allocate (atexit bookkeeping), so
	// register from thread startup, not from a hot path.
	static thread_local ExitHook hook;
	(void)&hook;

	std::lock_guard lock(g_mx);
	if (t_slot.tableIndex >= 0) return; // already registered
	for (int i = 0; i < kMaxThreads; ++i) {
		Entry& e = g_entries[i];
		if (e.used && e.live) continue;
		// A free slot, or a dead thread's slot being reused (a rebooted worker
		// takes a fresh one rather than inheriting the old totals).
		e = Entry{};
		e.used = true;
		e.live = &t_slot;
		e.osId = static_cast<u32>(::GetCurrentThreadId());
		const size_t n = name.size() < sizeof(e.name) - 1 ? name.size() : sizeof(e.name) - 1;
		std::memcpy(e.name, name.data(), n);
		e.name[n] = '\0';
		t_slot.tableIndex = i;
		return;
	}
	// Table full: the thread still counts, it just cannot be reported.
}

void Init() {
	{
		std::lock_guard lock(g_mx);
		if (g_inited) return;
		g_inited = true;
	}
	RegisterThread("main");
}

Counters ThisThread() { return t_slot.counters; }

void ResetThisThread() { t_slot.counters = Counters{}; }

int SnapshotAll(ThreadReport* out, int capacity) {
	std::lock_guard lock(g_mx);
	int n = 0;
	for (int i = 0; i < kMaxThreads && n < capacity; ++i) {
		const Entry& e = g_entries[i];
		if (!e.used) continue;
		ThreadReport& r = out[n++];
		std::memcpy(r.name, e.name, sizeof(r.name));
		r.osThreadId = e.osId;
		// A live thread's counters are read without synchronization: they are
		// monotonic per-thread totals and this is a diagnostic, so a torn read
		// costs at worst one stale figure on a 32-bit boundary.
		r.counters = e.live ? e.live->counters : e.last;
	}
	return n;
}

// --- the frame guard --------------------------------------------------------

namespace {

GuardStats g_stats;
bool g_strict = false;

// Stacks already reported, by hash. Fixed storage: a standing violation must
// not grow anything, and 64 distinct call sites in one session is already far
// past the point where the log has made its case.
constexpr int kMaxSeenStacks = 64;
u64 g_seen[kMaxSeenStacks] = {};
int g_seenCount = 0;
bool g_symInited = false;

u64 HashStack(void* const* frames, int depth) {
	u64 h = 1469598103934665603ull; // FNV-1a
	for (int i = 0; i < depth; ++i) {
		h ^= reinterpret_cast<u64>(frames[i]);
		h *= 1099511628211ull;
	}
	return h;
}

// True the first time a stack is seen (and records it).
bool FirstSighting(u64 hash) {
	for (int i = 0; i < g_seenCount; ++i)
		if (g_seen[i] == hash) return false;
	if (g_seenCount < kMaxSeenStacks) g_seen[g_seenCount++] = hash;
	return true;
}

// Symbolizes one frame address as "func (file:line)", best effort — a release
// build without a PDB falls back to the module-relative address.
std::string Describe(void* address) {
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

} // namespace

void BeginFrame() {
	Slot& s = t_slot;
	s.frameStart = s.counters;
	// Last frame's verdict decides this frame's capture — see the header.
	s.capturing = s.armed;
	s.armed = false;
	s.stackCount = 0;
}

void ArmFrame(bool steady) { t_slot.armed = steady; }

FrameResult EndFrame() {
	Slot& s = t_slot;
	FrameResult result;
	result.armed = s.armed;
	if (!s.armed) return result;

	const u64 allocs = s.counters.allocs - s.frameStart.allocs;
	const u64 excused = s.counters.excused - s.frameStart.excused;
	result.violations = allocs - excused;
	result.bytes = s.counters.bytes - s.frameStart.bytes;
	result.stacks = s.stackCount;

	++g_stats.framesArmed;
	if (result.violations > 0) {
		++g_stats.framesViolating;
		g_stats.violations += result.violations;
	}
	return result;
}

void ReportFrame(const FrameResult& result) {
	if (!result.armed || result.violations == 0) return;
	Excused excused; // symbolizing and logging both allocate

	Slot& s = t_slot;
	int fresh = 0;
	for (int i = 0; i < s.stackCount; ++i) {
		const u64 hash = HashStack(s.stacks[i], s.stackDepth[i]);
		if (!FirstSighting(hash)) continue;
		if (!g_symInited) {
			SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
			SymInitialize(GetCurrentProcess(), nullptr, TRUE);
			g_symInited = true;
		}
		if (fresh == 0)
			log::Warn("steady-state frame allocated {} times ({} bytes) — new call site(s):",
					  result.violations, result.bytes);
		++fresh;
		++g_stats.stacksReported;
		log::Warn("  [{}]", fresh);
		for (int f = 0; f < s.stackDepth[i]; ++f) {
			const std::string frame = Describe(s.stacks[i][f]);
			// Skip the plumbing. A dozen frames of std::_Allocate / _Buy_raw /
			// _Container_proxy_ptr12 say only "a container grew", which is
			// already known — the answer is the first OUR-code frame under it.
			if (!frame.starts_with("std::") && !frame.starts_with("operator new"))
				log::Warn("      {}", frame);
			// Stop at the frame loop: above it is Main/CRT scaffolding that says
			// nothing about which system allocated.
			if (frame.starts_with("wWinMain") || frame.starts_with("main")) break;
		}
	}
	// A frame whose stacks are all known stays silent: the log has said it.
	DN_ASSERT(!g_strict || result.violations == 0,
			  std::format("steady-state frame allocated {} times", result.violations));
}

void SetStrict(bool strict) { g_strict = strict; }
bool Strict() { return g_strict; }
GuardStats Stats() { return g_stats; }
void ResetStats() {
	g_stats = GuardStats{};
	g_seenCount = 0;
}

Excused::Excused() { ++t_slot.excusedDepth; }
Excused::~Excused() { --t_slot.excusedDepth; }

} // namespace dungeon::alloc

#if DN_ALLOC_TRACK

// --- global operator new/delete replacements --------------------------------
// Every form, because a missing one silently falls back to the CRT's and goes
// uncounted. Plain forms pair malloc/free, over-aligned forms pair
// _aligned_malloc/_aligned_free — mixing them corrupts the heap.

void* operator new(size_t n) {
	void* p = std::malloc(n ? n : 1);
	if (!p) throw std::bad_alloc();
	dungeon::alloc::Note(n);
	return p;
}

void* operator new[](size_t n) { return ::operator new(n); }

void* operator new(size_t n, const std::nothrow_t&) noexcept {
	void* p = std::malloc(n ? n : 1);
	if (p) dungeon::alloc::Note(n);
	return p;
}

void* operator new[](size_t n, const std::nothrow_t& tag) noexcept {
	return ::operator new(n, tag);
}

void* operator new(size_t n, std::align_val_t align) {
	void* p = _aligned_malloc(n ? n : 1, static_cast<size_t>(align));
	if (!p) throw std::bad_alloc();
	dungeon::alloc::Note(n);
	return p;
}

void* operator new[](size_t n, std::align_val_t align) { return ::operator new(n, align); }

void* operator new(size_t n, std::align_val_t align, const std::nothrow_t&) noexcept {
	void* p = _aligned_malloc(n ? n : 1, static_cast<size_t>(align));
	if (p) dungeon::alloc::Note(n);
	return p;
}

void* operator new[](size_t n, std::align_val_t align, const std::nothrow_t& tag) noexcept {
	return ::operator new(n, align, tag);
}

void operator delete(void* p) noexcept {
	if (!p) return;
	dungeon::alloc::NoteFree();
	std::free(p);
}

void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, size_t) noexcept { ::operator delete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }

void operator delete(void* p, std::align_val_t) noexcept {
	if (!p) return;
	dungeon::alloc::NoteFree();
	_aligned_free(p);
}

void operator delete[](void* p, std::align_val_t align) noexcept { ::operator delete(p, align); }

void operator delete(void* p, size_t, std::align_val_t align) noexcept {
	::operator delete(p, align);
}

void operator delete[](void* p, size_t, std::align_val_t align) noexcept {
	::operator delete(p, align);
}

void operator delete(void* p, std::align_val_t align, const std::nothrow_t&) noexcept {
	::operator delete(p, align);
}

void operator delete[](void* p, std::align_val_t align, const std::nothrow_t&) noexcept {
	::operator delete(p, align);
}

#endif // DN_ALLOC_TRACK
