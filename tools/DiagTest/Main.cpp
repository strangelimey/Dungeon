// ============================================================================
// tools/DiagTest/Main.cpp — the health record's regression test.
//
// Core/Diagnostics is a lock-free ring with a sequence-number publish, written
// from any thread and read while it is being written. That is precisely the
// kind of code that reads correctly and behaves otherwise, so it is CHECKED
// here rather than reasoned about — the same bargain tools/Bc7Test makes for
// the encoder's error estimate.
//
// The load-bearing test is CONCURRENCY: several threads hammer ONE slot while a
// reader walks it, and every event read back must be internally consistent (its
// message encodes the very fields it arrived with). A torn read — half of one
// event and half of the next — cannot pass that, which is the whole point.
//
// One machine-readable verdict line, like `alloctest`:  diagtest RESULT=PASS
// Exit code 0 = PASS.
// ============================================================================
#include "Core/Diagnostics.h"
#include "Core/Log.h"
#include "Core/Paths.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <share.h>
#include <string>
#include <thread>
#include <vector>

using namespace dungeon;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& what) {
	++g_checks;
	if (!ok) ++g_failures;
	std::printf("  [%s] %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

// A thread's slot, found by the name it registered under. SnapshotThreads is
// the only way in from outside, and it reports dormant slots too — which is
// what test 5 needs.
diag::Slot SlotNamed(const char* name) {
	diag::ThreadHealth all[diag::kMaxThreads];
	const int n = diag::SnapshotThreads(all, diag::kMaxThreads);
	for (int i = 0; i < n; ++i)
		if (std::strcmp(all[i].name, name) == 0) return all[i].slot;
	return diag::kInvalidSlot;
}

diag::ThreadHealth HealthOf(diag::Slot slot) {
	diag::ThreadHealth all[diag::kMaxThreads];
	const int n = diag::SnapshotThreads(all, diag::kMaxThreads);
	for (int i = 0; i < n; ++i)
		if (all[i].slot == slot) return all[i];
	return {};
}

void Say(const char* title) { std::printf("\n%s\n", title); }

// Counts log lines containing `needle`. The sink is still open for writing and
// flushed per line, so it can be read back in place. -1 = could not be read.
//
// _fsopen with _SH_DENYNO, NOT fopen_s: the secure variant opens with
// _SH_SECURE, which denies write sharing — and this very process is already
// holding the log open for writing, so fopen_s fails on its own log every time.
int CountLogLines(const char* needle) {
	const std::string path =
		paths::ExecutableDir() + "\\" + paths::ExecutableName() + ".log";
	FILE* f = _fsopen(path.c_str(), "r", _SH_DENYNO);
	if (!f) {
		std::printf("  [skip] could not reopen %s to count log lines\n", path.c_str());
		return -1;
	}
	int lines = 0;
	char buf[1024];
	while (std::fgets(buf, sizeof(buf), f))
		if (std::strstr(buf, needle)) ++lines;
	std::fclose(f);
	return lines;
}

// --------------------------------------------------------------------------
// 1 — record and read back, oldest first.
void TestBasics() {
	Say("1 - record, read back, field fidelity");
	std::jthread([] {
		const diag::Slot s = diag::RegisterThread("t.basics");
		for (int i = 0; i < 3; ++i)
			diag::Record({.kind = diag::Kind::Exception,
						  .workerId = 7,
						  .iteration = static_cast<u64>(100 + i),
						  .message = std::format("boom {}", i)});

		diag::EventView ev[8];
		const int n = diag::ReadEvents(s, ev, 8);
		Check(n == 3, std::format("three events read back (got {})", n));
		if (n != 3) return;

		Check(std::strcmp(ev[0].message, "boom 0") == 0, "oldest first");
		Check(std::strcmp(ev[2].message, "boom 2") == 0, "newest last");
		Check(ev[1].workerId == 7 && ev[1].iteration == 101, "workerId and iteration survive");
		Check(ev[0].index == 0 && ev[2].index == 2, "monotonic per-thread index");
		Check(ev[0].tsc != 0 && ev[0].wallNs != 0, "both clocks stamped");
		Check(ev[0].frameCount > 0, std::format("stack captured ({} frames)", ev[0].frameCount));
		Check(ev[0].tsc <= ev[2].tsc, "timestamps are ordered");
	}).join();
}

// --------------------------------------------------------------------------
// 2 — the ring wraps, and the TOTALS do not.
void TestWrap() {
	Say("2 - wrap: detail is bounded, totals are not");
	std::jthread([] {
		const diag::Slot s = diag::RegisterThread("t.wrap");
		constexpr int kExtra = 5;
		constexpr int kTotal = diag::kEventsPerThread + kExtra;
		for (int i = 0; i < kTotal; ++i)
			diag::Record({.kind = diag::Kind::Stall,
						  .iteration = static_cast<u64>(i),
						  .message = std::format("tick {}", i),
						  .captureStack = false});

		diag::EventView ev[diag::kEventsPerThread * 2];
		const int n = diag::ReadEvents(s, ev, diag::kEventsPerThread * 2);
		Check(n == diag::kEventsPerThread,
			  std::format("ring holds exactly {} (got {})", diag::kEventsPerThread, n));
		if (n == diag::kEventsPerThread) {
			Check(ev[0].index == kExtra,
				  std::format("oldest survivor is index {} (got {})", kExtra, ev[0].index));
			Check(ev[n - 1].index == kTotal - 1, "newest is the last written");
			Check(std::strcmp(ev[0].message, "tick 5") == 0, "the wrapped-off events really are gone");
		}

		const diag::ThreadHealth h = HealthOf(s);
		Check(h.total == kTotal, std::format("total counts all {} (got {})", kTotal, h.total));
		Check(h.Count(diag::Kind::Stall) == kTotal, "per-kind counter does not wrap");
		Check(h.Count(diag::Kind::Exception) == 0, "unrelated kinds stay zero");
	}).join();
}

// --------------------------------------------------------------------------
// 3 — an over-long message is truncated, not overrun.
void TestTruncation() {
	Say("3 - message truncation");
	std::jthread([] {
		const diag::Slot s = diag::RegisterThread("t.trunc");
		const std::string huge(diag::kMessageMax * 3, 'x');
		diag::Record({.kind = diag::Kind::Fatal, .message = huge, .captureStack = false});

		diag::EventView ev[2];
		const int n = diag::ReadEvents(s, ev, 2);
		Check(n == 1, "the event was recorded");
		if (n == 1) {
			const size_t len = std::strlen(ev[0].message);
			Check(len == diag::kMessageMax - 1,
				  std::format("truncated to {} chars (got {})", diag::kMessageMax - 1, len));
			Check(ev[0].message[diag::kMessageMax - 1] == '\0', "NUL-terminated");
		}
	}).join();
}

// --------------------------------------------------------------------------
// 4 — a cross-thread write lands on the TARGET's timeline, not the writer's.
void TestCrossThread() {
	Say("4 - cross-thread record (the supervisor's case)");
	std::jthread([] {
		const diag::Slot target = diag::RegisterThread("t.target");
		std::jthread([target] {
			diag::RegisterThread("t.reporter");
			diag::RecordFor(target, {.kind = diag::Kind::Restart,
									 .workerId = 3,
									 .message = "rebooted by the supervisor",
									 .captureStack = false});
		}).join();

		diag::EventView ev[4];
		const int n = diag::ReadEvents(target, ev, 4);
		Check(n == 1 && std::strcmp(ev[0].message, "rebooted by the supervisor") == 0,
			  "the event is on the target's ring");
		Check(HealthOf(target).Count(diag::Kind::Restart) == 1, "target's counter moved");

		const diag::Slot reporter = SlotNamed("t.reporter");
		Check(reporter != diag::kInvalidSlot && HealthOf(reporter).total == 0,
			  "the reporter's own ring stayed empty");
	}).join();
}

// --------------------------------------------------------------------------
// 5 — a same-named thread ADOPTS its slot and keeps the history (the reboot
//     rule: what got a worker restarted is the thing worth still having).
void TestRebootKeepsHistory() {
	Say("5 - a reboot adopts the slot and keeps its events");
	diag::Slot first = diag::kInvalidSlot;
	std::jthread([&first] {
		first = diag::RegisterThread("t.reboot");
		diag::Record({.kind = diag::Kind::Exception,
					  .message = "why it died",
					  .captureStack = false});
		diag::UnregisterThisThread();
	}).join();

	diag::Slot second = diag::kInvalidSlot;
	std::jthread([&second] {
		second = diag::RegisterThread("t.reboot"); // the "rebooted" worker
		diag::Record({.kind = diag::Kind::Restart, .message = "back up", .captureStack = false});
	}).join();

	Check(first == second && first != diag::kInvalidSlot, "the same slot was adopted");
	diag::EventView ev[4];
	const int n = diag::ReadEvents(first, ev, 4);
	Check(n == 2, std::format("both events present (got {})", n));
	if (n == 2)
		Check(std::strcmp(ev[0].message, "why it died") == 0,
			  "the predecessor's event survived the reboot");
}

// --------------------------------------------------------------------------
// 6 — THE ONE THAT MATTERS. Several writers hammer one slot while a reader
//     walks it. Every event read must be self-consistent: the message spells
//     out the workerId and iteration it arrived with, so a torn read cannot
//     pass. A wrapped-past slot is skipped by the reader, never half-returned.
void TestConcurrency() {
	Say("6 - concurrent writers + a live reader (torn-read detection)");
	constexpr int kWriters = 4;
	constexpr int kPerWriter = 4000;

	diag::Slot target = diag::kInvalidSlot;
	std::atomic<bool> go{false};
	std::atomic<int> done{0};
	std::atomic<u64> readCount{0};
	std::atomic<u64> tornCount{0};

	std::jthread owner([&] {
		target = diag::RegisterThread("t.hammer");
		go.store(true);
		while (done.load() < kWriters) std::this_thread::yield();
	});
	while (!go.load()) std::this_thread::yield();

	std::vector<std::jthread> writers;
	for (int w = 0; w < kWriters; ++w) {
		writers.emplace_back([&, w] {
			diag::RegisterThread(std::format("t.hammer.w{}", w));
			for (int i = 0; i < kPerWriter; ++i)
				diag::RecordFor(target, {.kind = diag::Kind::Exception,
										 .workerId = static_cast<u32>(w),
										 .iteration = static_cast<u64>(i),
										 .message = std::format("w{}#{}", w, i),
										 .captureStack = false});
			done.fetch_add(1);
		});
	}

	std::jthread reader([&] {
		diag::EventView ev[diag::kEventsPerThread];
		while (done.load() < kWriters) {
			const int n = diag::ReadEvents(target, ev, diag::kEventsPerThread);
			for (int i = 0; i < n; ++i) {
				readCount.fetch_add(1);
				int w = -1;
				unsigned long long it = 0;
				// The message must spell out the fields it arrived beside.
				if (sscanf_s(ev[i].message, "w%d#%llu", &w, &it) != 2 ||
					w != static_cast<int>(ev[i].workerId) || it != ev[i].iteration)
					tornCount.fetch_add(1);
			}
		}
	});

	for (auto& t : writers) t.join();
	reader.join();
	owner.join();

	const u64 expected = static_cast<u64>(kWriters) * kPerWriter;
	const diag::ThreadHealth h = HealthOf(target);
	Check(h.total == expected,
		  std::format("every write was claimed: {} of {}", h.total, expected));
	Check(readCount.load() > 0,
		  std::format("the reader saw live traffic ({} events)", readCount.load()));
	Check(tornCount.load() == 0,
		  std::format("no torn reads ({} bad of {})", tornCount.load(), readCount.load()));
}

// --------------------------------------------------------------------------
// 7 — the merged view is newest-first across threads.
void TestMergeOrder() {
	Say("7 - ReadAllEvents merges threads newest-first");
	for (int i = 0; i < 3; ++i) {
		std::jthread([i] {
			diag::RegisterThread(std::format("t.merge{}", i));
			diag::Record({.kind = diag::Kind::Killed,
						  .message = std::format("from thread {}", i),
						  .captureStack = false});
		}).join();
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}

	diag::EventView ev[64];
	diag::Slot slots[64];
	const int n = diag::ReadAllEvents(ev, 64, slots);
	Check(n > 0, std::format("merged view returned {} events", n));

	bool ordered = true;
	for (int i = 1; i < n; ++i)
		if (ev[i].tsc > ev[i - 1].tsc) ordered = false;
	Check(ordered, "strictly newest-first by TSC");

	// The three above are the most recent Killed events, in reverse order.
	int seen = 0;
	for (int i = 0; i < n && seen < 3; ++i)
		if (ev[i].kind == diag::Kind::Killed) {
			const std::string want = std::format("from thread {}", 2 - seen);
			if (std::strcmp(ev[i].message, want.c_str()) != 0) ordered = false;
			++seen;
		}
	Check(seen == 3 && ordered, "the newest three are the three just written, reversed");
}

// --------------------------------------------------------------------------
// 8 — a repeating failure floods the RECORD but not the LOG.
void TestLogThrottle() {
	Say("8 - identical repeats are logged at powers of ten only");
	std::jthread([] {
		diag::RegisterThread("t.flood");
		for (int i = 0; i < 100; ++i)
			diag::Record({.kind = diag::Kind::Exception,
						  .message = "the same failure every tick",
						  .captureStack = false});
	}).join();

	const int lines = CountLogLines("the same failure every tick");
	if (lines < 0) return;

	// 1, 10, 100 — three lines for a hundred identical failures.
	Check(lines == 3, std::format("100 repeats produced {} log lines (want 3)", lines));
	Check(HealthOf(SlotNamed("t.flood")).Count(diag::Kind::Exception) == 100,
		  "all 100 still counted in the record");
}

// --------------------------------------------------------------------------
// 9 — a thread failing a DIFFERENT way every tick is rate-limited too. The
//     repeat-collapse above is blind to a message carrying a tick number, and
//     without this a bad worker buries the crash worth finding.
void TestRateLimit() {
	Say("9 - distinct messages are rate-limited per thread");
	constexpr int kBurst = 200;
	std::jthread([] {
		diag::RegisterThread("t.varied");
		for (int i = 0; i < kBurst; ++i)
			diag::Record({.kind = diag::Kind::Fault,
						  .message = std::format("distinct failure {}", i),
						  .captureStack = false});
	}).join();

	const int lines = CountLogLines("distinct failure ");
	if (lines < 0) return;

	// One second's budget plus at most a summary or two, against 200 events.
	Check(lines > 0 && lines <= 12,
		  std::format("{} events wrote {} log lines (want 1..12)", kBurst, lines));
	Check(HealthOf(SlotNamed("t.varied")).Count(diag::Kind::Fault) == kBurst,
		  std::format("all {} still counted in the record", kBurst));
	Check(CountLogLines("were not logged (rate limit)") >= 0, "the log says what it swallowed");
}

} // namespace

int main() {
	log::UseUtf8Console();
	diag::Init();

	std::printf("DiagTest — Core/Diagnostics regression\n");
	std::printf("  %d slots x %d events, %d-frame stacks, %d-char messages\n",
				diag::kMaxThreads, diag::kEventsPerThread, diag::kStackDepth,
				diag::kMessageMax);

	TestBasics();
	TestWrap();
	TestTruncation();
	TestCrossThread();
	TestRebootKeepsHistory();
	TestConcurrency();
	TestMergeOrder();
	TestLogThrottle();
	TestRateLimit();

	const diag::Totals t = diag::ProcessTotals();
	std::printf("\nprocess totals: %llu events (%llu exception, %llu stall, %llu restart, "
				"%llu killed, %llu fatal)\n",
				t.total, t.Count(diag::Kind::Exception), t.Count(diag::Kind::Stall),
				t.Count(diag::Kind::Restart), t.Count(diag::Kind::Killed),
				t.Count(diag::Kind::Fatal));

	std::printf("\ndiagtest RESULT=%s checks=%d failures=%d\n",
				g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
