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

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

#if DN_ALLOC_TRACK
#include <windows.h>
#endif

namespace dungeon::alloc {

namespace {

struct Slot {
	Counters counters;
	int excusedDepth = 0;
	int tableIndex = -1;
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
	if (s.excusedDepth > 0) ++s.counters.excused;
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
#if DN_ALLOC_TRACK
		e.osId = static_cast<u32>(::GetCurrentThreadId());
#endif
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
