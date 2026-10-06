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
// The LOG checks (8 to 12) read this process's real log back, from the path the
// sink itself opened (log::FilePath), and an unreadable log FAILS them: a check
// that skips when its evidence is missing passes on nothing. Tests 9 and 11
// each wait one log window out, which is most of the run's few seconds. Test 12
// is the stack seen-set's (Core/StackTrace), which both the record's log path and
// the allocation guard use to log a stack once.
//
// One machine-readable verdict line, the shared one (tools/Common/Verdict.h):
//   diagtest RESULT=PASS checks=N failures=0 self_test=0
// Exit code 0 = PASS.
// ============================================================================
#include "Common/Verdict.h"
#include "Core/Diagnostics.h"
#include "Core/Log.h"
#include "Core/StackTrace.h"

#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <share.h>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

using namespace dungeon;

namespace {

// One check, tallied by the shared verdict (tools/Common/Verdict.h).
void Check(bool ok, const std::string& what) { verdict::Check(ok, what, "  "); }

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

// Every log line containing `needle`. The sink is still open for writing and
// flushed per line, so it can be read back in place.
//
// An unreadable log is a FAIL, recorded here, and returns nothing so the
// caller's own checks stop: every one of them would otherwise pass, or fail
// confusingly, on no evidence at all.
//
// _fsopen with _SH_DENYNO, NOT fopen_s: the secure variant opens with
// _SH_SECURE, which denies write sharing - and this very process is already
// holding the log open for writing, so fopen_s fails on its own log every time.
std::optional<std::vector<std::string>> LogLines(const char* needle) {
	const std::string& path = log::FilePath();
	// Owned from the open, like every FILE* in the codebase (code-review C231).
	const std::unique_ptr<FILE, decltype(&std::fclose)> f(
		_fsopen(path.c_str(), "r", _SH_DENYNO), &std::fclose);
	if (!f) {
		Check(false, std::format("the log can be read back ({})", path));
		return std::nullopt;
	}
	std::vector<std::string> lines;
	char buf[1024];
	while (std::fgets(buf, sizeof(buf), f.get()))
		if (std::strstr(buf, needle)) lines.emplace_back(buf);
	return lines;
}

// How many log lines contain `needle`; -1 when the log could not be read (and
// that has already FAILED).
int CountLogLines(const char* needle) {
	const auto lines = LogLines(needle);
	return lines ? static_cast<int>(lines->size()) : -1;
}

// The count on every rate-limit line naming `thread`, oldest first:
//   diag · 192 further events on 't.varied' were not logged (rate limit); ...
// A line whose count cannot be read gives 0, which no check here wants.
std::optional<std::vector<u64>> RateLimitCounts(const char* thread) {
	const std::string tail = std::format(" further events on '{}' were not logged", thread);
	const auto lines = LogLines(tail.c_str());
	if (!lines) return std::nullopt;
	std::vector<u64> counts;
	for (const std::string& line : *lines) {
		const size_t at = line.find(tail);
		size_t from = at;
		while (from > 0 && std::isdigit(static_cast<unsigned char>(line[from - 1]))) --from;
		counts.push_back(from < at ? std::stoull(line.substr(from, at - from)) : 0);
	}
	return counts;
}

std::string Join(const std::vector<u64>& values) {
	std::string s;
	for (const u64 v : values) s += (s.empty() ? "" : ", ") + std::to_string(v);
	return s.empty() ? "none" : s;
}

// The digits of `text`, or 0 when it is not wholly digits (no check wants 0).
u64 Number(std::string_view text) {
	u64 value = 0;
	const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
	return ec == std::errc{} && end == text.data() + text.size() ? value : 0;
}

// One closing line of a repeat run, the collapse's own accounting:
//   diag · 47 further repeats on 't.run57' were not logged (57 in the run;
//   the thread unregistered); the record kept them all
struct RunTail {
	u64 tail = 0;    // the repeats after the run's last line
	u64 run = 0;     // the run's whole length
	std::string why; // "the run ended" or "the thread unregistered"
};

// Every closing line naming `thread`, oldest first. A field that cannot be read
// stays 0 / empty, which no check here wants.
std::optional<std::vector<RunTail>> RunTails(const char* thread) {
	const std::string mid = std::format(" further repeats on '{}' were not logged (", thread);
	const auto lines = LogLines(mid.c_str());
	if (!lines) return std::nullopt;
	std::vector<RunTail> tails;
	for (const std::string& line : *lines) {
		RunTail t;
		const size_t at = line.find(mid);
		size_t from = at;
		while (from > 0 && std::isdigit(static_cast<unsigned char>(line[from - 1]))) --from;
		t.tail = Number(std::string_view(line).substr(from, at - from));
		const size_t open = at + mid.size();
		const size_t sep = line.find(" in the run; ", open);
		const size_t close = line.find(')', open);
		if (sep != std::string::npos && close != std::string::npos && sep < close) {
			t.run = Number(std::string_view(line).substr(open, sep - open));
			const size_t whyAt = sep + std::strlen(" in the run; ");
			t.why = line.substr(whyAt, close - whyAt);
		}
		tails.push_back(t);
	}
	return tails;
}

std::string Join(const std::vector<RunTail>& tails) {
	std::string s;
	for (const RunTail& t : tails)
		s += std::format("{}{} of {} ({})", s.empty() ? "" : ", ", t.tail, t.run, t.why);
	return s.empty() ? "none" : s;
}

// Exactly one closing line, with these numbers and this reason.
bool OneRunTail(const std::vector<RunTail>& tails, u64 tail, u64 run, const char* why) {
	return tails.size() == 1 && tails[0].tail == tail && tails[0].run == run &&
		   tails[0].why == why;
}

// Sleeps until the log throttle's window has certainly rolled over.
void WaitOutLogWindow() {
	std::this_thread::sleep_for(std::chrono::nanoseconds(diag::kLogWindowNs) +
								std::chrono::milliseconds(200));
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
// 8 — a repeating failure floods the RECORD but not the LOG. A run that stops
//     BETWEEN two powers of ten still accounts for its tail: a run of 57 has
//     lines at 1 and 10, and the 47 after are written as one closing line when
//     the run ends (a different event) or its thread unregisters. 100 is the
//     one shape that needs no closing line, so it cannot be the only one tried.
void TestLogThrottle() {
	Say("8 - identical repeats are logged at powers of ten, and a run's tail when it ends");
	std::jthread([] {
		diag::RegisterThread("t.flood");
		for (int i = 0; i < 100; ++i)
			diag::Record({.kind = diag::Kind::Exception,
						  .message = "the same failure every tick",
						  .captureStack = false});
		// Writes whatever the log still owes the thread - and for a run that
		// stopped ON a power of ten that is nothing: "repeated 100 times" said
		// them, and the collapse kept none from the rate limit.
		diag::UnregisterThisThread();
	}).join();

	const int lines = CountLogLines("the same failure every tick");
	if (lines < 0) return;

	// 1, 10, 100 — three lines for a hundred identical failures.
	Check(lines == 3, std::format("100 repeats produced {} log lines (want 3)", lines));
	Check(HealthOf(SlotNamed("t.flood")).Count(diag::Kind::Exception) == 100,
		  "all 100 still counted in the record");

	const auto limited = RateLimitCounts("t.flood");
	if (!limited) return;
	Check(limited->empty(),
		  std::format("the collapsed repeats are not reported as rate-limited "
					  "(rate-limit lines: {})",
					  Join(*limited)));
	const auto floodTails = RunTails("t.flood");
	if (!floodTails) return;
	Check(floodTails->empty(),
		  std::format("a run that stopped on a power of ten writes no closing line "
					  "(closing lines: {})",
					  Join(*floodTails)));

	// 57, then the thread goes: the exit writes the tail.
	std::jthread([] {
		diag::RegisterThread("t.run57");
		for (int i = 0; i < 57; ++i)
			diag::Record({.kind = diag::Kind::Exception,
						  .message = "a run of fifty-seven",
						  .captureStack = false});
		diag::UnregisterThisThread();
	}).join();
	const int lines57 = CountLogLines("a run of fifty-seven");
	const auto tails57 = RunTails("t.run57");
	const auto limited57 = RateLimitCounts("t.run57");
	if (lines57 < 0 || !tails57 || !limited57) return;
	Check(lines57 == 2, std::format("57 repeats produced {} log lines (want 2: 1, 10)", lines57));
	Check(OneRunTail(*tails57, 47, 57, "the thread unregistered"),
		  std::format("the exit wrote one closing line, 47 after the 10th of 57 (got: {})",
					  Join(*tails57)));
	Check(limited57->empty(),
		  std::format("and none of it as rate-limited (rate-limit lines: {})", Join(*limited57)));

	// 150, then a different event: the event that ends the run writes the tail,
	// and the exit after it owes nothing.
	std::jthread([] {
		diag::RegisterThread("t.run150");
		for (int i = 0; i < 150; ++i)
			diag::Record({.kind = diag::Kind::Exception,
						  .message = "a run of a hundred and fifty",
						  .captureStack = false});
		diag::Record({.kind = diag::Kind::Exception,
					  .message = "the event that ends the run",
					  .captureStack = false});
		diag::UnregisterThisThread();
	}).join();
	const int lines150 = CountLogLines("a run of a hundred and fifty");
	const int ender = CountLogLines("the event that ends the run");
	const auto tails150 = RunTails("t.run150");
	if (lines150 < 0 || ender < 0 || !tails150) return;
	Check(lines150 == 3 && ender == 1,
		  std::format("150 repeats produced {} log lines (want 3), the ending event {} (want 1)",
					  lines150, ender));
	Check(OneRunTail(*tails150, 50, 150, "the run ended"),
		  std::format("the run's end wrote one closing line, 50 after the 100th of 150, and "
					  "the exit none (got: {})",
					  Join(*tails150)));
}

// --------------------------------------------------------------------------
// 9 — a thread failing a DIFFERENT way every tick is rate-limited too. The
//     repeat-collapse above is blind to a message carrying a tick number, and
//     without this a bad worker buries the crash worth finding. What it held
//     back is written by the NEXT window's first event, so the test waits the
//     window out and records one more.
void TestRateLimit() {
	Say("9 - distinct messages are rate-limited per thread");
	constexpr int kBurst = 200;
	int burstLines = -1;
	std::jthread([&burstLines] {
		diag::RegisterThread("t.varied");
		for (int i = 0; i < kBurst; ++i)
			diag::Record({.kind = diag::Kind::Fault,
						  .message = std::format("distinct failure {}", i),
						  .captureStack = false});
		burstLines = CountLogLines("distinct failure ");
		WaitOutLogWindow();
		diag::Record({.kind = diag::Kind::Fault,
					  .message = "the first failure of the next window",
					  .captureStack = false});
	}).join();
	if (burstLines < 0) return;

	// One window's budget, exactly, against 200 events.
	Check(burstLines == static_cast<int>(diag::kLogBurst),
		  std::format("{} events in one window wrote {} log lines (want the budget, {})",
					  kBurst, burstLines, diag::kLogBurst));
	Check(HealthOf(SlotNamed("t.varied")).Count(diag::Kind::Fault) == kBurst + 1,
		  std::format("all {} still counted in the record", kBurst + 1));

	const int next = CountLogLines("the first failure of the next window");
	if (next < 0) return;
	Check(next == 1, "the event after the window was logged");

	const auto limited = RateLimitCounts("t.varied");
	if (!limited) return;
	const u64 want = static_cast<u64>(kBurst - burstLines);
	Check(limited->size() == 1 && (*limited)[0] == want,
		  std::format("one rate-limit line, counting the {} swallowed (got: {})", want,
					  Join(*limited)));
}

// --------------------------------------------------------------------------
// 10 - a burst that ends with its thread is still accounted for. No later
//      window opens on a thread that has gone, so unregistering writes the
//      held-back count itself. And a repeat run in a SPENT window loses nothing
//      either: a refused "repeated 10 times" takes the repeats it would have
//      said into the rate limit's count, and so does a refused closing line.
void TestExitFlush() {
	Say("10 - a thread's exit writes what the rate limit held back");
	constexpr int kBurst = 50;
	std::jthread([] {
		diag::RegisterThread("t.parting");
		for (int i = 0; i < kBurst; ++i)
			diag::Record({.kind = diag::Kind::Exception,
						  .message = std::format("parting failure {}", i),
						  .captureStack = false});
		diag::UnregisterThisThread();
	}).join();

	const int lines = CountLogLines("parting failure ");
	if (lines < 0) return;
	const auto limited = RateLimitCounts("t.parting");
	if (!limited) return;
	const u64 want = static_cast<u64>(kBurst - lines);
	Check(want > 0 && limited->size() == 1 && (*limited)[0] == want,
		  std::format("one rate-limit line at exit, counting the {} swallowed (got: {})",
					  want, Join(*limited)));

	// The budget spent on distinct events, then a run of 15 the budget refuses
	// at 1 and at 10, ended by one more event. Every one of the 24 is either a
	// line or in the one count: 8 lines, 16 counted, no closing line (refused,
	// its 5 joined the count), and the exit owes no tail (the last run is 1).
	constexpr int kRun = 15;
	std::jthread([] {
		diag::RegisterThread("t.cutshort");
		for (u32 i = 0; i < diag::kLogBurst; ++i)
			diag::Record({.kind = diag::Kind::Exception,
						  .message = std::format("spending the budget {}", i),
						  .captureStack = false});
		for (int i = 0; i < kRun; ++i)
			diag::Record({.kind = diag::Kind::Exception,
						  .message = "a run past a spent budget",
						  .captureStack = false});
		diag::Record({.kind = diag::Kind::Exception,
					  .message = "the event after the cut-short run",
					  .captureStack = false});
		diag::UnregisterThisThread();
	}).join();
	const int spent = CountLogLines("spending the budget ");
	const int runLines = CountLogLines("a run past a spent budget");
	const auto cutLimited = RateLimitCounts("t.cutshort");
	const auto cutTails = RunTails("t.cutshort");
	if (spent < 0 || runLines < 0 || !cutLimited || !cutTails) return;
	const u64 total = diag::kLogBurst + kRun + 1;
	Check(spent == static_cast<int>(diag::kLogBurst) && runLines == 0,
		  std::format("the budget took {} lines (want {}) and the run {} (want 0)", spent,
					  diag::kLogBurst, runLines));
	Check(cutLimited->size() == 1 && (*cutLimited)[0] == total - diag::kLogBurst,
		  std::format("one rate-limit line counting the other {} of {} (got: {})",
					  total - diag::kLogBurst, total, Join(*cutLimited)));
	Check(cutTails->empty(),
		  std::format("the refused closing line was counted, not written (closing lines: {})",
					  Join(*cutTails)));
}

// --------------------------------------------------------------------------
// 11 - a slot handed to a NEW name starts with a clean log throttle. Once all
//      32 slots have been used, a 33rd name takes a dormant slot of another
//      name. The predecessor here leaves its window spent and a count held
//      back; the new owner must inherit neither - its first event is logged at
//      once, and no rate-limit line ever counts events it never had. This
//      fills the table, so it runs LAST.
void TestSlotReuse() {
	Say("11 - a 33rd name takes a dormant slot with a clean log throttle");

	// Every slot but the predecessor's must be live, so the new name has exactly
	// one place to go. Earlier tests left some slots dormant; their own names
	// adopt them again and keep them (a thread that never unregisters).
	diag::ThreadHealth all[diag::kMaxThreads];
	int used = diag::SnapshotThreads(all, diag::kMaxThreads);
	for (int i = 0; i < used; ++i)
		if (!all[i].live) {
			const std::string name = all[i].name;
			std::jthread([&name] { diag::RegisterThread(name); }).join();
		}
	Check(used < diag::kMaxThreads,
		  std::format("setup: the table has room ({} of {} slots used)", used,
					  diag::kMaxThreads));
	if (used >= diag::kMaxThreads) return;
	// Then every never-used slot but one, which the predecessor takes.
	for (int i = used; i < diag::kMaxThreads - 1; ++i)
		std::jthread([i] { diag::RegisterThread(std::format("t.fill{}", i)); }).join();

	// The predecessor spends its window and unregisters (writing what it held
	// back). Then a cross-thread report lands on its DORMANT slot - RecordFor
	// does not ask whether the owner still runs, which is the supervisor's case
	// - and is held back in that still-spent window, pending.
	constexpr int kBurst = 20;
	constexpr int kLate = 5;
	diag::Slot old = diag::kInvalidSlot;
	std::jthread([&old] {
		old = diag::RegisterThread("t.oldowner");
		for (int i = 0; i < kBurst; ++i)
			diag::Record({.kind = diag::Kind::Exception,
						  .message = std::format("old owner failure {}", i),
						  .captureStack = false});
		diag::UnregisterThisThread();
	}).join();
	for (int i = 0; i < kLate; ++i)
		diag::RecordFor(old, {.kind = diag::Kind::Stall,
							  .message = std::format("late report {}", i),
							  .captureStack = false});
	const int late = CountLogLines("late report ");
	if (late < 0) return;
	Check(old != diag::kInvalidSlot && late == 0,
		  std::format("setup: the late reports on the dormant slot were held back "
					  "({} of {} logged)",
					  late, kLate));

	used = diag::SnapshotThreads(all, diag::kMaxThreads);
	int dormant = 0;
	for (int i = 0; i < used; ++i)
		if (!all[i].live) ++dormant;
	Check(used == diag::kMaxThreads && dormant == 1,
		  std::format("setup: all {} slots used, one dormant ({} used, {} dormant)",
					  diag::kMaxThreads, used, dormant));

	diag::Slot fresh = diag::kInvalidSlot;
	int firstLogged = -1;
	std::jthread([&fresh, &firstLogged] {
		fresh = diag::RegisterThread("t.newowner");
		diag::Record({.kind = diag::Kind::Exception,
					  .message = "the first event of the new owner",
					  .captureStack = false});
		firstLogged = CountLogLines("the first event of the new owner");
		// The predecessor's held-back count would surface when a new window
		// opens, so open one.
		WaitOutLogWindow();
		diag::Record({.kind = diag::Kind::Exception,
					  .message = "the second event of the new owner",
					  .captureStack = false});
	}).join();

	Check(fresh == old && fresh != diag::kInvalidSlot,
		  "the 33rd name took the predecessor's dormant slot");
	Check(firstLogged == 1,
		  std::format("its first event was logged at once: a fresh window, not the "
					  "predecessor's spent one ({} lines)",
					  firstLogged));
	const auto limited = RateLimitCounts("t.newowner");
	if (!limited) return;
	Check(limited->empty(),
		  std::format("no rate-limit line for events the new owner never had "
					  "(rate-limit lines: {})",
					  Join(*limited)));
	Check(HealthOf(fresh).total == 2,
		  std::format("its record holds its own 2 events only (got {})", HealthOf(fresh).total));
}

// --------------------------------------------------------------------------
// 12 - a FULL seen-set reports nothing new. The allocation guard and the health
//      record each log a stack once, remembering it in a bounded stack::SeenSet.
//      Once its capacity is taken, a site it cannot remember must answer "seen" -
//      it used to answer "new" on every offer, so the guard logged and
//      symbolized a stack on every violating frame from then on (code-review
//      C226) - be counted instead, and say so in ONE line. The count is of
//      OFFERS turned away, not of distinct sites (a repeat counts again), which
//      is what every readout of it must say. It touches no slot of the record,
//      so it can follow test 11's full table.
void TestSeenSetFull() {
	Say("12 - a full seen-set reports no further site, counts the offers, and says so once");
	constexpr int kCap = stack::SeenSet::kCapacity;
	// Distinct, non-zero, and nothing like a real stack's hash.
	const auto site = [](int i) { return 0x9E3779B97F4A7C15ull * static_cast<u64>(i + 1); };
	const char* kFull = "t.seenset: the stack set is full";
	stack::SeenSet set("t.seenset");

	int firsts = 0;
	for (int i = 0; i < kCap; ++i) firsts += set.FirstSighting(site(i)) ? 1 : 0;
	Check(firsts == kCap, std::format("{} distinct sites are each new once (got {})", kCap, firsts));
	Check(!set.FirstSighting(site(3)) && set.TurnedAway() == 0,
		  "a site the full set holds is seen, and is not turned away");

	constexpr int kPast = 10;
	int newPast = 0;
	for (int i = kCap; i < kCap + kPast; ++i) newPast += set.FirstSighting(site(i)) ? 1 : 0;
	Check(newPast == 0, std::format("{} sites past the {}th are none of them new (got {})", kPast,
									kCap, newPast));
	Check(set.TurnedAway() == kPast,
		  std::format("...and each offer is counted as turned away ({}, want {})",
					  set.TurnedAway(), kPast));
	Check(!set.FirstSighting(site(kCap)) && set.TurnedAway() == kPast + 1,
		  "a turned-away site offered again stays unreported, and counts again (no room "
		  "to remember it - the count is of offers, not sites)");

	const int lines = CountLogLines(kFull);
	if (lines < 0) return;
	Check(lines == 1, std::format("{} offers turned away wrote {} 'set is full' lines (want 1)",
								  set.TurnedAway(), lines));

	// A reset starts a new episode: forgotten sites, a fresh count, and a set that
	// fills again says so again.
	set.Reset();
	Check(set.FirstSighting(site(3)) && set.TurnedAway() == 0,
		  "after Reset a held site is new again and the count restarts");
	for (int i = 0; i < kCap + 1; ++i) set.FirstSighting(site(100 + i));
	const int again = CountLogLines(kFull);
	if (again < 0) return;
	Check(again == 2 && set.TurnedAway() == 2,
		  std::format("refilled, it says so again: {} lines (want 2), {} turned away (want 2)",
					  again, set.TurnedAway()));
}

// --------------------------------------------------------------------------
// 10 - WALKED stacks (a stall's, a forced kill's) cannot use up the log's
//      memory of which stacks it has already shown. That memory is a SeenSet,
//      which once full logs no further site (test 12), and a walk lands
//      somewhere new almost every stall: sharing the exceptions' set, a
//      session's stall walks would fill it and every later exception site would
//      log no stack at all (code-review batch 36's review; before batch 15 a
//      full set called every stack new, and logged it on every event).

// The frame an exception's stack is told apart by in the log. noinline, so it
// is a frame of its own whatever the build.
__declspec(noinline) void SiteMarkerThrows(int i) {
	diag::Record({.kind = diag::Kind::Exception, .message = std::format("site check {}", i)});
}

void TestWalkedStacksKeepTheirOwnSet() {
	Say("10 - walked stacks do not crowd out the exceptions' once-per-site rule");
	// The SET must actually fill, or a walk sharing the exceptions' set would pass
	// too. A thread offers a seen-set at most kWalkedLogged + 1 walked stacks of a
	// KIND (past that it does not offer them), and logs at most kLogBurst lines a
	// window: so each of nine threads walks four stalls and four kills - eight
	// lines, eight offers, 72 in all, past the set's 64. (Nine threads and not
	// ten: with the site thread that is ten names, and the slot table must keep
	// one never-used slot for test 11. One kind alone offers only 36, and that
	// version of this test passed with the walks pointed at the shared set.)
	constexpr int kThreads = 9;
	constexpr int kPerKind = static_cast<int>(diag::kWalkedLogged) + 1;
	constexpr diag::Kind kKinds[] = {diag::Kind::Stall, diag::Kind::Killed};
	constexpr int kPerThread = kPerKind * 2;
	static_assert(kPerThread <= diag::kLogBurst, "a thread's walks must all reach the log");
	static_assert(kThreads * kPerThread > stack::SeenSet::kCapacity,
				  "the walks must be able to fill a seen-set");
	for (int t = 0; t < kThreads; ++t) {
		std::jthread([t, kKinds] {
			diag::RegisterThread(std::format("t.walk{}", t));
			for (int i = 0; i < kPerThread; ++i) {
				void* frame = reinterpret_cast<void*>(0x10000ull + t * 0x100ull + i);
				diag::Record({.kind = kKinds[i / kPerKind],
							  .message = std::format("walked {}.{}", t, i),
							  .frames = &frame,
							  .frameCount = 1,
							  .captureStack = false,
							  .walked = true});
			}
		}).join();
	}
	// One exception site, hit twice with different messages (so the repeat
	// collapse does not hide the second): its stack is one site, logged once.
	std::jthread([] {
		diag::RegisterThread("t.sites");
		for (int i = 0; i < 2; ++i) SiteMarkerThrows(i);
	}).join();

	const int stacks = CountLogLines("SiteMarkerThrows");
	const int notes = CountLogLines("are in the record only");
	if (stacks < 0 || notes < 0) return;
	Check(stacks == 1, std::format("after {} walked stacks, an exception site hit twice logged its "
								   "stack {} times (want 1)",
								   kThreads * kPerThread, stacks));
	// The walked set fills too (72 offers, 64 places), and full it takes no
	// further site (test 12): the threads that fit log three stacks of each kind
	// and a note a kind, and the last one's walks log nothing.
	constexpr int kFit = stack::SeenSet::kCapacity / kPerThread;
	Check(notes == kFit * 2,
		  std::format("each thread the walked set holds logged 3 walked stacks of each kind, "
					  "then one note a kind ({} notes, want {})",
					  notes, kFit * 2));
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
	TestExitFlush();
	// Ten names of its own, so it runs before the slot fill below, which leaves
	// no slot for anyone (it needs one never-used slot of its own).
	TestWalkedStacksKeepTheirOwnSet();
	TestSlotReuse(); // it fills the slot table, so only slot-free tests follow
	TestSeenSetFull();

	const diag::Totals t = diag::ProcessTotals();
	std::printf("\nprocess totals: %llu events (%llu exception, %llu stall, %llu restart, "
				"%llu killed, %llu fatal)\n",
				t.total, t.Count(diag::Kind::Exception), t.Count(diag::Kind::Stall),
				t.Count(diag::Kind::Restart), t.Count(diag::Kind::Killed),
				t.Count(diag::Kind::Fatal));

	std::printf("\n");
	return verdict::Finish("diagtest", false, false);
}
