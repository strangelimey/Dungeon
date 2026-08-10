// ============================================================================
// Core/AllocTrack.cpp — the ::operator new replacements and the slot table.
//
// Counting is LOCK-FREE: every allocation touches only the calling thread's
// own Slot, reached through a constant-initialized thread_local pointer (no
// dynamic init, so first touch cannot re-enter the allocator). The mutex here
// guards the slot TABLE — a registry of who to report — and is taken by
// registration and by SnapshotAll, never by Note().
//
// THE REGISTRY OWNS THE COUNTERS. The slots live in the module-level table and
// the thread_local holds only a POINTER into it, rather than the other way
// round. That is a safety property, not tidiness: ThreadManager::Kill
// force-terminates a wedged worker with TerminateThread, which runs NO
// destructors — no thread_local is destroyed and no exit hook fires — and the
// thread's TLS block is released underneath it anyway. A table of pointers INTO
// TLS would therefore be left holding a dangler that the next SnapshotAll reads,
// which is the worst place for a silent bad read: diagnostic code, on the path
// you reach for when something is already wrong. Registry-owned storage cannot
// be freed under a reader, so a killed thread simply leaves its last counters
// behind, exactly like one that exited cleanly.
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

	// Frame guard (main thread today; per-thread so a worker tick can use it).
	Counters frameStart;
	bool armed = false;
	bool capturing = false;
	int stackCount = 0;
	int stackDepth[kMaxFrameStacks] = {};
	void* stacks[kMaxFrameStacks][kStackDepth] = {};
};

struct Entry {
	Slot slot;         // the counters themselves — see the file banner
	bool live = false; // is a thread currently writing to this slot
	char name[32] = {};
	u32 osId = 0;
	bool used = false;
};

std::mutex g_mx;
// constinit is the checked form of "no dynamic initialization": this table is
// touched from inside operator new, so an initializer that ran lazily on first
// use could re-enter the allocator. ~1.6 KB a slot (the captured stacks
// dominate) x kMaxThreads is ~100 KB of BSS, which buys the kill-safety above.
constinit Entry g_entries[kMaxThreads]{};
bool g_inited = false;

// Where an UNREGISTERED thread counts. Its numbers are never reported (nothing
// in the table points here), so it can safely be thread_local: the dangling
// risk only exists for storage a reader can reach. Constant-initialized POD, so
// touching it never allocates — which is what makes it safe inside operator new.
thread_local constinit Slot t_fallback{};

// This thread's slot: a registry entry once registered, else null. Constant-
// initialized (a null pointer literal) for the same re-entrancy reason.
thread_local constinit Slot* t_slot = nullptr;
thread_local constinit int t_index = -1; // its index in g_entries, or -1

// Never allocates, never locks. The null test is the unregistered case.
inline Slot& Mine() noexcept { return t_slot ? *t_slot : t_fallback; }

// Detaches this thread from its table entry at thread exit, leaving the final
// counters behind in registry-owned storage. Nulling t_slot under the same lock
// that clears `live` is what makes the entry safe to hand to a later thread:
// afterwards this thread writes only to its fallback, so any allocation during
// the rest of CRT teardown (other thread_locals being destroyed) can no longer
// land in a slot somebody else now owns. Those last few are lost from the
// report, which is the same as before and not worth a lock in Note().
struct ExitHook {
	~ExitHook() {
		if (t_index < 0) return;
		std::lock_guard lock(g_mx);
		g_entries[t_index].live = false;
		t_slot = nullptr;
		t_index = -1;
	}
};

} // namespace

// Called from the operator new/delete replacements below. Never allocates,
// never locks, never throws.
void Note(size_t bytes) noexcept {
	Slot& s = Mine();
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

void NoteFree() noexcept { ++Mine().counters.frees; }

void RegisterThread(std::string_view name) {
	// The ExitHook's dynamic init may itself allocate (atexit bookkeeping), so
	// register from thread startup, not from a hot path.
	static thread_local ExitHook hook;
	(void)&hook;

	std::lock_guard lock(g_mx);
	if (t_index >= 0) return; // already registered
	for (int i = 0; i < kMaxThreads; ++i) {
		Entry& e = g_entries[i];
		// A slot still marked live belongs to a thread that never released it.
		// Usually that thread is simply running; it can also be one Kill()
		// force-terminated, whose ExitHook never fired. Both are skipped, so a
		// killed thread's totals stay readable instead of being overwritten by
		// the worker that replaces it. That leaks a slot per hard kill, bounded
		// by kMaxThreads and by Kill being a genuine last resort.
		if (e.used && e.live) continue;
		// A free slot, or a dead thread's slot being reused (a rebooted worker
		// takes a fresh one rather than inheriting the old totals).
		e = Entry{};
		e.used = true;
		e.live = true;
		e.osId = static_cast<u32>(::GetCurrentThreadId());
		const size_t n = name.size() < sizeof(e.name) - 1 ? name.size() : sizeof(e.name) - 1;
		std::memcpy(e.name, name.data(), n);
		e.name[n] = '\0';
		// Whatever this thread counted before it registered came out of its
		// fallback; carry it over so the totals stay honest across the switch.
		e.slot.counters = t_fallback.counters;
		t_fallback = Slot{};
		t_index = i;
		t_slot = &e.slot;
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

Counters ThisThread() { return Mine().counters; }

void ResetThisThread() { Mine().counters = Counters{}; }

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
		// costs at worst one stale figure on a 32-bit boundary. Reading a DEAD
		// thread's is likewise safe with no special case — the storage is ours,
		// so it does not matter whether that thread exited or was terminated.
		r.counters = e.slot.counters;
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
	Slot& s = Mine();
	s.frameStart = s.counters;
	// Last frame's verdict decides this frame's capture — see the header.
	s.capturing = s.armed;
	s.armed = false;
	s.stackCount = 0;
}

void ArmFrame(bool steady) { Mine().armed = steady; }

FrameResult EndFrame() {
	Slot& s = Mine();
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

	Slot& s = Mine();
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

Excused::Excused() { ++Mine().excusedDepth; }
Excused::~Excused() { --Mine().excusedDepth; }

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
