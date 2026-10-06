// ============================================================================
// Core/ThreadManager.cpp — see ThreadManager.h.
// ============================================================================
#include "Core/ThreadManager.h"

#include "Core/AllocTrack.h"
#include "Core/CrashHandler.h"
#include "Core/Diagnostics.h"
#include "Core/Log.h"
#include "Core/Profile.h"
#include "Core/StackTrace.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <format>
#include <thread>

#ifdef _WIN32
#include <windows.h> // WIN32_LEAN_AND_MEAN / NOMINMAX come from the build defines
#endif

namespace dungeon::threads {

using Clock = std::chrono::steady_clock;
static double ToMs(Clock::duration d) {
	return std::chrono::duration<double, std::milli>(d).count();
}

const char* StateName(State s) {
	switch (s) {
	case State::Starting: return "starting";
	case State::Running: return "running";
	case State::Sleeping: return "sleeping";
	case State::Paused: return "paused";
	case State::Stalled: return "stalled";
	case State::Cancelling: return "cancelling";
	case State::Dead: return "dead";
	case State::Quarantined: return "quarantd";
	}
	return "?";
}

#ifdef _WIN32
// Name the OS thread so it shows by name in the VS debugger's Threads window and
// in profilers (Tracy/Superluminal). Best-effort — ignore failure.
static void SetOsThreadName(const std::string& name) {
	const int n = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
	if (n <= 0) return;
	std::wstring wide(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wide.data(), n);
	SetThreadDescription(GetCurrentThread(), wide.c_str());
}
// Map our -2..+2 priority to the Win32 thread-priority constants.
static int Win32Priority(int p) {
	if (p <= -2) return THREAD_PRIORITY_LOWEST;
	if (p == -1) return THREAD_PRIORITY_BELOW_NORMAL;
	if (p == 0) return THREAD_PRIORITY_NORMAL;
	if (p == 1) return THREAD_PRIORITY_ABOVE_NORMAL;
	return THREAD_PRIORITY_HIGHEST;
}
#else
static void SetOsThreadName(const std::string&) {}
#endif

// ----------------------------------------------------------------------------
// Worker — one managed thread's state. Single-writer (its own thread) for the
// stats, multi-reader (Inspect) — so stats are atomics and reads never block.
// The jthread is declared LAST so it is destroyed (and joined) FIRST, while the
// fields it touches are still alive.
// ----------------------------------------------------------------------------
struct Manager::Worker {
	WorkerId id = kInvalidWorker;
	std::string name;
	JobFn job;
	std::atomic<float> hz{0.0f};
	unsigned watchdogMs = 0; // set once at spawn; 0 = no stall detection

	std::atomic<State> state{State::Starting};
	std::atomic<u64> iterations{0};
	std::atomic<double> lastMs{0.0};
	std::atomic<double> avgMs{0.0};
	std::atomic<double> maxMs{0.0};
	std::atomic<i64> beatNs{0}; // Clock::now() of the current tick's start

	std::atomic<bool> paused{false};
	std::atomic<u64> wakeGen{0}; // bumped by a control call to wake the cadence sleep
	// THE PAUSE HANDSHAKE (WaitPaused), both under sleepMx: every Pause bumps
	// pauseGen, and the worker copies it into pauseAck each time it settles at
	// its pause point - where no tick of its is in flight - then signals quietCv.
	// A generation rather than a "paused" state, because the state can read
	// Paused for an instant AFTER a Resume, while the worker is waking to run a
	// tick; an ack can only name a pause the worker has actually honoured.
	u64 pauseGen = 0;
	u64 pauseAck = 0;
	std::condition_variable quietCv;

	bool autoRestart = false;        // set once at spawn
	// One Stall event per stall EPISODE. Set when the supervisor records one,
	// cleared when the worker gets back under its watchdog — see SupervisorLoop.
	std::atomic<bool> stallReported{false};
	std::atomic<bool> userStopped{false}; // a kill/stop the supervisor must respect
	std::atomic<bool> quarantined{false}; // force-terminated; slot poisoned until reboot
	std::atomic<bool> removed{false};     // Remove or Reap took it out; never restarted again
	std::atomic<u32> restarts{0};
	std::atomic<int> priority{0};    // OS thread priority (-2..+2)
	std::atomic<u64> affinity{0};    // CPU affinity mask (0 = any)
	// Serialises lifecycle ops (Stop/Kill/Restart/Remove) on this worker, and
	// whatever reads `thread` from another thread (the walks, Reap). TIMED so a
	// reader can give up: Stop joins a wedged worker forever, and the supervisor
	// waiting on that to walk a stall would be wedged with it.
	std::timed_mutex controlMx;

	std::mutex errMx;
	std::string lastError;

	// This worker's slot in the health record, published by Run at thread entry
	// so ANOTHER thread can record against it — the supervisor logging a stall,
	// Kill logging a termination. Those events belong on the worker's timeline,
	// which is where a reader looks for them, not on the reporter's.
	std::atomic<diag::Slot> diagSlot{diag::kInvalidSlot};

	// Interruptible cadence/pause sleep. Control calls flip the atomics under this
	// mutex then notify, so a sleeping worker can't miss the wake.
	std::mutex sleepMx;
	std::condition_variable_any sleepCv;

	std::jthread thread; // MUST be last (see comment above)
};

Manager::Manager() {
	m_supervisor = std::jthread([this](std::stop_token st) { SupervisorLoop(st); });
}

Manager::~Manager() {
	// Stop the supervisor first so it can't try to reboot a worker mid-teardown.
	m_supervisor.request_stop();
	if (m_supervisor.joinable()) m_supervisor.join();
	// Stop every worker; force-terminate any that won't cooperate so shutdown
	// can't hang on a wedged thread. (Teardown is single-threaded: no lock.)
	for (auto& w : m_workers) StopOrTerminate(w.get());
}

WorkerId Manager::Spawn(JobFn job, Options opt) {
	auto w = std::make_shared<Worker>();
	Worker* p = w.get();
	p->name = std::move(opt.name);
	p->job = std::move(job);
	p->hz.store(opt.hz);
	p->watchdogMs = opt.watchdogMs;
	p->autoRestart = opt.autoRestart;
	p->priority.store(opt.priority);
	p->affinity.store(opt.affinity);

	std::lock_guard<std::mutex> lk(m_mx);
	p->id = m_nextId++; // stable id, not the array index
	m_workers.push_back(std::move(w));
	// jthread injects the stop_token as the first arg. p is stable (the Worker
	// lives in a shared_ptr; the vector only moves the pointers, never the node),
	// and it outlives the thread: ~Manager and Remove join (or terminate) it
	// before the registry lets go, and Reap only drops a slot whose thread is gone.
	p->thread = std::jthread([this, p](std::stop_token st) { Run(p, st); });
	return p->id;
}

void Manager::Run(Worker* w, std::stop_token st) {
	SetOsThreadName(w->name);
	// Room kept back on this thread's stack for reporting its own overflow
	// (Core/CrashHandler.h): without it the fault filter runs on the last few KB
	// and faults again inside the report. Here, at thread entry, for the reason
	// the registrations below are here - every managed thread gets it.
	crash::GuardThreadStack();
	// Same name to the allocation counters: "allocates nothing per tick" is the
	// steady-state rule on this side too, and the AI's snapshot/plan pools exist
	// precisely to hold it. Registering here rather than inside the job keeps the
	// one-time atexit bookkeeping out of the tick.
	alloc::RegisterThread(w->name);
	// And to the profiler, for the same reason and in the same place. Doing it
	// HERE rather than in any particular client is what makes every managed
	// thread measured - the AI buckets, a stress worker, and whatever spawns
	// next - instead of only the ones someone remembered to wire. (Not the
	// supervisor: it is the Manager's own std::jthread, never a Worker, so it
	// never comes through here.)
	prof::RegisterThread(w->name);
	// And to the health record, in the same place and for the same reason: being
	// MANAGED is what gets a thread covered, rather than somebody having
	// remembered to wire it up. A worker rebooting adopts the slot it had, so the
	// events that got it rebooted are still sitting there when it comes back.
	w->diagSlot.store(diag::RegisterThread(w->name));
#ifdef _WIN32
	SetThreadPriority(GetCurrentThread(), Win32Priority(w->priority.load()));
	if (const u64 mask = w->affinity.load())
		SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(mask));
#endif
	// One tick's failure, kept two ways because they answer different questions.
	// lastError is "is this worker unhappy RIGHT NOW", which the console panel
	// shows; the health record is "what happened, when, how often, and on what
	// tick", which is what you read afterwards. Before the record existed this
	// string was the whole story — overwritten by the next failure and never
	// logged, so a worker that threw a thousand times was indistinguishable from
	// one that threw once, and neither left a trace in dungeon.log.
	const auto noteFailure = [w](const char* what) {
		{
			std::lock_guard<std::mutex> lk(w->errMx);
			w->lastError = what;
		}
		// The stack the VECTORED handler took at the moment of the throw. By the
		// time control reaches this catch the frames between throw and catch have
		// unwound, so capturing here would name the catch site — which we already
		// know — and never the code that threw. Falls back to a catch-site
		// capture if the throw capture is not installed.
		void* frames[stack::kMaxFrames];
		const int n = stack::ThrowFrames(frames, stack::kMaxFrames);
		diag::Record({.kind = diag::Kind::Exception,
					  .workerId = w->id,
					  .iteration = w->iterations.load(),
					  .message = what,
					  .frames = n > 0 ? frames : nullptr,
					  .frameCount = n});
	};

	while (!st.stop_requested()) {
		// Paused: hold here (not joined) until resumed or stopped, running no job.
		// Holding here is QUIET - no tick of this worker is in flight - so every
		// pause asked so far is acknowledged on the way in, and again on each
		// wake while still paused: a Resume and a fresh Pause can both land
		// while it sleeps, and that second pause must be acknowledged too, or a
		// WaitPaused for it would wait out its timeout on a worker already still.
		if (w->paused.load()) {
			w->state.store(State::Paused);
			std::unique_lock<std::mutex> lk(w->sleepMx);
			while (w->paused.load() && !st.stop_requested()) {
				const u64 gen = w->pauseGen;
				w->pauseAck = gen;
				w->quietCv.notify_all();
				w->sleepCv.wait(lk, st, [w, gen] {
					return !w->paused.load() || w->pauseGen != gen;
				});
			}
			continue; // re-check stop + paused at the top
		}

		const auto t0 = Clock::now();
		w->beatNs.store(t0.time_since_epoch().count());
		w->state.store(State::Running);

		// Crash capture: one bad tick records its error and the worker keeps
		// running, instead of an unhandled exception calling std::terminate and
		// taking the whole process down. (Restart/quarantine policy is a later step.)
		{
			// The root of this thread's tree. Named for what it is rather than
			// for the worker — the thread's name rides the report, and every
			// worker's tree is its own, so there is nothing to disambiguate.
			// ScopedZone unwinds correctly if the job throws below.
			DN_PROFILE_ZONE("tick");
			try {
				w->job(Tick{st, w->iterations.load(), w->id});
			} catch (const std::exception& e) {
				noteFailure(e.what());
			} catch (...) {
				noteFailure("unknown exception (not derived from std::exception)");
			}
		}

		const double ms = ToMs(Clock::now() - t0);
		w->lastMs.store(ms);
		if (ms > w->maxMs.load()) w->maxMs.store(ms);
		const double a = w->avgMs.load();
		w->avgMs.store(a <= 0.0 ? ms : a * 0.9 + ms * 0.1); // EMA
		w->iterations.fetch_add(1);

		// This worker's publish boundary. A worker has no frames — the AI buckets
		// tick at 251/499/997/1999 ms — so its TICK is the period, and publishing
		// before the cadence sleep means a console draw sees the tick that just
		// ran rather than one a second stale.
		prof::PublishThisThread();

		// Effective cadence = configured hz scaled by the global governor.
		const float eff = w->hz.load() * m_globalScale.load();
		if (eff > 0.0f && !st.stop_requested() && !w->paused.load()) {
			w->state.store(State::Sleeping);
			const std::chrono::duration<double> interval(1.0 / eff);
			const u64 gen = w->wakeGen.load();
			std::unique_lock<std::mutex> lk(w->sleepMx);
			// Wakes on timeout, a stop request, or any control call (pause / new
			// rate bumps wakeGen) so the change takes effect at once.
			w->sleepCv.wait_for(lk, st, interval, [w, gen] {
				return w->paused.load() || w->wakeGen.load() != gen;
			});
		}
	}
	// Frees the profiler slot for a same-named successor (Restart, or the
	// supervisor rebooting a stalled worker) while leaving its last published
	// tree readable. A worker that never reaches this line because it was hard
	// Kill()ed keeps its slot on purpose — see prof::UnregisterThisThread.
	prof::UnregisterThisThread();
	// Frees the health slot for a same-named successor too — which is what makes
	// a reboot ADOPT its own history rather than start blank. A force-terminated
	// worker never reaches this line, so its slot stays live and its successor
	// takes a fresh one: the two lives are then recorded separately, which is the
	// honest answer when one of them was killed mid-tick.
	diag::UnregisterThisThread();
	{
		// Under the handshake's lock, then signalled: a thread that has ended is
		// as quiet as one holding at its pause point, and a WaitPaused begun
		// while it was stopping must hear so rather than wait out its timeout.
		std::lock_guard<std::mutex> lk(w->sleepMx);
		w->state.store(State::Dead);
	}
	w->quietCv.notify_all();
}

std::shared_ptr<Manager::Worker> Manager::Get(WorkerId id) const {
	std::lock_guard<std::mutex> lk(m_mx);
	for (const auto& w : m_workers)
		if (w->id == id) return w;
	return nullptr;
}

void Manager::RequestStop(WorkerId id) {
	const std::shared_ptr<Worker> w = Get(id);
	if (!w) return;
	std::lock_guard ctl(w->controlMx);
	w->userStopped.store(true); // intentional — the supervisor must not revive it
	w->state.store(State::Cancelling);
	w->thread.request_stop(); // also wakes the interruptible sleep
}

void Manager::Stop(WorkerId id) {
	// Resolved under the lock, then joined WITHOUT m_mx held.
	const std::shared_ptr<Worker> w = Get(id);
	if (!w) return;
	std::lock_guard ctl(w->controlMx);
	w->userStopped.store(true);
	w->state.store(State::Cancelling);
	w->thread.request_stop();
	if (w->thread.joinable()) w->thread.join();
	w->state.store(State::Dead);
}

void Manager::Remove(WorkerId id) {
	const std::shared_ptr<Worker> w = Get(id);
	if (!w) return;
	{
		std::lock_guard ctl(w->controlMx);
		// Marked FIRST, under the lock every lifecycle op takes, so a Restart
		// already waiting on it - the supervisor's, which looked this worker up
		// before it was asked to go - finds it removed and gives up rather than
		// relaunching a job whose owner is being destroyed.
		w->removed.store(true);
		w->userStopped.store(true);
		w->state.store(State::Cancelling);
		w->thread.request_stop();
		if (w->thread.joinable()) w->thread.join();
		w->state.store(State::Dead);
		// The job goes NOW, on the caller's thread: what it captured belongs to
		// the caller, who is about to destroy it, and nothing may run it again.
		w->job = nullptr;
	}
	// Out of the registry: no row in the THREADS panel to `boot`, no id for
	// anything to find. A copy the supervisor still holds keeps the Worker
	// itself alive until its check is done - it can neither run nor restart it.
	std::lock_guard<std::mutex> lk(m_mx);
	std::erase_if(m_workers, [&w](const std::shared_ptr<Worker>& p) { return p == w; });
}

// Control calls flip the worker's atomics UNDER its sleep mutex, then notify, so
// a worker that is between checking the predicate and waiting can't miss it.
void Manager::Pause(WorkerId id) {
	const std::shared_ptr<Worker> w = Get(id);
	if (!w) return;
	{
		std::lock_guard<std::mutex> lk(w->sleepMx);
		w->paused.store(true);
		++w->pauseGen; // the pause WaitPaused waits to see honoured
		++w->wakeGen;
	}
	w->sleepCv.notify_all();
}

bool Manager::WaitPaused(WorkerId id, std::chrono::milliseconds timeout) {
	const std::shared_ptr<Worker> w = Get(id);
	if (!w) return true; // gone from the registry: nothing of it can be running
	std::unique_lock<std::mutex> lk(w->sleepMx);
	if (!w->paused.load()) return false; // nobody asked it to pause
	const u64 want = w->pauseGen;
	// Quiet once it has honoured this pause - or has no thread left to run a
	// tick on. A quarantined one ended without signalling (it was terminated),
	// so it is seen at the timeout's final look, not before.
	return w->quietCv.wait_for(lk, timeout, [&w, want] {
		return w->pauseAck >= want || w->state.load() == State::Dead ||
			   w->quarantined.load();
	});
}

void Manager::Resume(WorkerId id) {
	const std::shared_ptr<Worker> w = Get(id);
	if (!w) return;
	{
		std::lock_guard<std::mutex> lk(w->sleepMx);
		w->paused.store(false);
		++w->wakeGen;
	}
	w->sleepCv.notify_all();
}

void Manager::SetRate(WorkerId id, float hz) {
	const std::shared_ptr<Worker> w = Get(id);
	if (!w) return;
	{
		std::lock_guard<std::mutex> lk(w->sleepMx);
		w->hz.store(hz);
		++w->wakeGen; // wake the cadence sleep so the new rate applies now
	}
	w->sleepCv.notify_all();
}

void Manager::SetPriority(WorkerId id, int priority) {
	const std::shared_ptr<Worker> w = Get(id);
	if (!w) return;
	std::lock_guard ctl(w->controlMx); // don't race Restart's relaunch
	w->priority.store(priority);
#ifdef _WIN32
	if (w->thread.joinable())
		SetThreadPriority(static_cast<HANDLE>(w->thread.native_handle()),
						  Win32Priority(priority));
#endif
}

void Manager::SetAffinity(WorkerId id, u64 mask) {
	const std::shared_ptr<Worker> w = Get(id);
	if (!w) return;
	std::lock_guard ctl(w->controlMx);
	w->affinity.store(mask);
#ifdef _WIN32
	if (mask && w->thread.joinable())
		SetThreadAffinityMask(static_cast<HANDLE>(w->thread.native_handle()),
							  static_cast<DWORD_PTR>(mask));
#endif
}

void Manager::SetGlobalThrottle(float scale, bool wakeNow) {
	m_globalScale.store(std::clamp(scale, 0.05f, 4.0f));
	// Workers read m_globalScale at the top of each cadence sleep, so a change
	// always applies by their next natural wake. wakeNow forces it to take effect
	// THIS instant by waking them — right for a one-shot manual `throttle`, but
	// NOT for the per-frame adaptive governor: waking every worker every frame
	// would make them tick at frame rate (a burst) during the easing, defeating
	// the very throttle being applied. The governor passes wakeNow = false.
	if (!wakeNow) return;
	std::lock_guard<std::mutex> lk(m_mx);
	for (auto& w : m_workers) {
		{
			std::lock_guard<std::mutex> s(w->sleepMx);
			++w->wakeGen;
		}
		w->sleepCv.notify_all();
	}
}

// Stop a worker, force-terminating it if it won't go cooperatively. On return
// w->thread is either joined (clean) or detached (quarantined). Caller serialises
// via controlMx (except the dtor, which runs single-threaded).
void Manager::StopOrTerminate(Worker* w) {
	if (!w->thread.joinable()) return; // already joined/detached
	w->thread.request_stop();          // wakes any interruptible sleep
	// Grace: a cooperative worker reaches Dead at the end of its loop quickly.
	const auto deadline = Clock::now() + std::chrono::milliseconds(250);
	while (Clock::now() < deadline && w->state.load() != State::Dead)
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	if (w->state.load() == State::Dead) {
		w->thread.join(); // clean exit
		return;
	}
	// WHERE it is stuck, walked from outside while there is still a thread to
	// walk: the terminate below ends the evidence, and "it would not stop" says
	// nothing without where (code-review C387). Not the caller's own stack -
	// that belongs to whoever called Kill, and says nothing about the victim.
	void* frames[diag::kStackDepth];
	const int n = WalkWorker(w, frames, diag::kStackDepth);
	// Wedged: force-terminate. DANGEROUS last resort — TerminateThread runs no
	// unwinding, so any lock the job held is leaked forever. If it was mid-malloc
	// (the AI BFS allocates), the leaked lock is the CRT HEAP lock, which
	// deadlocks the whole process on the next allocation. Acceptable only because
	// Kill tries cooperative stop first, and the realistic trigger is a genuine
	// bug (an infinite loop that never checks its token). Abandon (detach) the
	// object so nothing tries to join it.
#ifdef _WIN32
	const HANDLE h = static_cast<HANDLE>(w->thread.native_handle());
	TerminateThread(h, 1);
	// TerminateThread only STARTS the termination and returns. Wait for it to
	// land, so that by the time the slot reads quarantined - and Reap may free
	// the Worker - no thread is left running with a pointer to it.
	WaitForSingleObject(h, 1000);
#endif
	w->thread.detach();
	w->quarantined.store(true);
	// On the VICTIM's timeline, not the killer's, with the victim's own frames.
	diag::RecordFor(w->diagSlot.load(),
					{.kind = diag::Kind::Killed,
					 .workerId = w->id,
					 .iteration = w->iterations.load(),
					 .message = "force-terminated: would not stop cooperatively; any "
								"lock it held is leaked",
					 .frames = n > 0 ? frames : nullptr,
					 .frameCount = n,
					 .captureStack = false,
					 .walked = n > 0});
	log::Warn("thread '{}' would not stop — FORCE-TERMINATED; process may be unstable "
			  "(leaked locks). Restart soon.",
			  w->name);
}

void Manager::Kill(WorkerId id) {
	const std::shared_ptr<Worker> w = Get(id);
	if (!w) return;
	std::lock_guard ctl(w->controlMx);
	w->userStopped.store(true); // intentional — the supervisor must not revive it
	w->state.store(State::Cancelling);
	StopOrTerminate(w.get());
	w->state.store(w->quarantined.load() ? State::Quarantined : State::Dead);
}

void Manager::Restart(WorkerId id) { RestartWorker(id, /*bySupervisor=*/false); }

void Manager::RestartWorker(WorkerId id, bool bySupervisor) {
	const std::shared_ptr<Worker> w = Get(id);
	if (!w) return;
	std::lock_guard ctl(w->controlMx);
	// Taken out of the registry (Remove, or Reap) while this call waited for the
	// lock: nothing may run its job again. Nothing to relaunch.
	if (w->removed.load()) return;
	// The supervisor chose this reboot from a look taken WITHOUT the lock. A Stop
	// or Kill that landed since is the user's word and wins: rebooting would
	// clear userStopped and set running again a worker just stopped on purpose.
	if (bySupervisor && w->userStopped.load()) return;
	// Stop the current thread (force-terminating a wedged one) so the old thread
	// is entirely gone before the new one touches this Worker — no shared-state
	// race, and a stuck worker can't block the reboot.
	w->state.store(State::Cancelling);
	StopOrTerminate(w.get());

	// Recorded BEFORE the counters are cleared, so the event carries the tick the
	// worker died on rather than the zero it is about to be reset to. Covers both
	// callers — the supervisor's automatic reboot and a manual one from the
	// console — because both come through here.
	diag::RecordFor(w->diagSlot.load(),
					{.kind = diag::Kind::Restart,
					 .workerId = w->id,
					 .iteration = w->iterations.load(),
					 .message = std::format("rebooted (restart #{}) after {} ticks",
											w->restarts.load() + 1, w->iterations.load()),
					 .captureStack = false});

	// Fresh slate; keep the stored job + config. Booting clears the user-stopped
	// and quarantine flags so the worker runs again and is supervised again.
	// NOT `paused`: a pause belongs to whoever asked for it, not to this thread's
	// life. It used to be cleared here, so the supervisor rebooting a stalled AI
	// worker while lockstep held it paused set it running again - beside the
	// inline compute lockstep promises is the only thinking (code-review C63).
	// The new thread finds the flag at the top of its loop and holds there.
	w->quarantined.store(false);
	w->userStopped.store(false);
	// A new life's stall is a new episode. Left set, the supervisor cleared it
	// only if it happened to look while the first tick was still under its
	// watchdog - a 100 ms poll against a 100 ms watchdog - so a rebooted AI
	// bucket that stalled again was mostly never recorded at all.
	w->stallReported.store(false);
	w->iterations.store(0);
	w->lastMs.store(0.0);
	w->avgMs.store(0.0);
	w->maxMs.store(0.0);
	w->beatNs.store(0);
	{
		std::lock_guard<std::mutex> lk(w->errMx);
		w->lastError.clear();
	}
	w->restarts.fetch_add(1);
	w->state.store(State::Starting);
	// A RAW pointer into the thread, as Spawn passes: a thread holding its own
	// Worker's shared_ptr could end up destroying that Worker - and so joining
	// itself - when the registry lets go first.
	Worker* p = w.get();
	w->thread = std::jthread([this, p](std::stop_token st) { Run(p, st); });
}

void Manager::SupervisorLoop(std::stop_token st) {
	using namespace std::chrono_literals;
	// Room on its own stack to report its own overflow, as every worker has (Run).
	crash::GuardThreadStack();
	while (!st.stop_requested()) {
		std::vector<WorkerId> ids;
		{
			std::lock_guard<std::mutex> lk(m_mx);
			ids.reserve(m_workers.size());
			for (const auto& w : m_workers) ids.push_back(w->id);
		}
		for (WorkerId id : ids) {
			const std::shared_ptr<Worker> w = Get(id);
			if (!w || w->watchdogMs == 0) continue;

			// DETECTION is separate from the reboot, and deliberately so. It used
			// to ride the reboot path, which meant a stall on a worker with no
			// autoRestart — demo.wedged, say — was never recorded at all: the
			// panel showed `stalled` live and the history showed nothing, so it
			// vanished the moment you looked away.
			if (w->state.load() != State::Running) { // only a stuck LIVE tick
				w->stallReported.store(false);
				continue;
			}
			const i64 beat = w->beatNs.load();
			if (!beat) continue;
			const double age =
				ToMs(Clock::now() - Clock::time_point(Clock::duration(beat)));
			if (age <= static_cast<double>(w->watchdogMs)) {
				w->stallReported.store(false); // it finished; arm for the next one
				continue;
			}

			// Once per STALL EPISODE, not once per poll: the supervisor looks
			// every 100 ms, and a tick wedged for a minute would otherwise write
			// six hundred identical events and push everything else out of the
			// ring. The flag clears above, when the worker gets back under its
			// watchdog or leaves Running.
			if (!w->stallReported.exchange(true)) {
				// WHERE it is stuck, walked from outside now: an autoRestart
				// worker is rebooted at 5x its watchdog (half a second for an AI
				// bucket), which ends the thread and the evidence with it, long
				// before anyone could type `health probe` (code-review C387).
				// Under the worker's lock, so no Restart swaps the thread under
				// the walk - but only waited for briefly: a Stop joining a wedged
				// worker holds it forever, and the record goes without frames
				// rather than the supervisor wedging too.
				void* frames[diag::kStackDepth];
				int n = 0;
				{
					std::unique_lock ctl(w->controlMx, std::chrono::milliseconds(50));
					if (ctl.owns_lock()) n = WalkWorker(w.get(), frames, diag::kStackDepth);
				}
				// A tick that ended while we looked was walked in whatever came
				// next - the cadence sleep - which is not where it stalled.
				if (w->beatNs.load() != beat || w->state.load() != State::Running) n = 0;
				diag::RecordFor(w->diagSlot.load(),
								{.kind = diag::Kind::Stall,
								 .workerId = id,
								 .iteration = w->iterations.load(),
								 .message = std::format(
									 "tick has run {:.0f} ms — past its {} ms watchdog",
									 age, w->watchdogMs),
								 .frames = n > 0 ? frames : nullptr,
								 .frameCount = n,
								 .captureStack = false,
								 .walked = n > 0});
			}

			// The reboot is the separate fact, and Restart records it itself.
			if (w->autoRestart && !w->userStopped.load() &&
				age > static_cast<double>(w->watchdogMs) * 5.0)
				RestartWorker(id, /*bySupervisor=*/true);
		}
		// Coarse poll; checks the stop flag often so shutdown is prompt.
		for (int i = 0; i < 10 && !st.stop_requested(); ++i)
			std::this_thread::sleep_for(10ms);
	}
}

int Manager::WalkWorker(Worker* w, void** out, int max) {
	if (!w->thread.joinable()) return 0;
#ifdef _WIN32
	// A quarantined slot's thread was force-terminated and detached; the handle
	// may name nothing, or worse, something reused. Never walk one.
	if (w->quarantined.load()) return 0;
	return stack::WalkThread(static_cast<void*>(w->thread.native_handle()), out, max);
#else
	(void)out;
	(void)max;
	return 0;
#endif
}

int Manager::CaptureStack(WorkerId id, void** out, int max) const {
	const std::shared_ptr<Worker> w = Get(id);
	if (!w) return 0;
	// The thread object is read under the worker's lock, so a Restart cannot be
	// swapping it mid-read. Bounded: a Stop joining a wedged worker holds the
	// lock forever, and the probe is meant for exactly that worker.
	std::unique_lock ctl(w->controlMx, std::chrono::milliseconds(500));
	if (!ctl.owns_lock()) return 0;
	return WalkWorker(w.get(), out, max);
}

WorkerInfo Manager::Inspect(WorkerId id) const {
	const std::shared_ptr<Worker> w = Get(id);
	if (!w) return {};
	WorkerInfo info;
	info.id = w->id;
	info.name = w->name;
	info.state = w->state.load();
	info.iterations = w->iterations.load();
	info.lastMs = w->lastMs.load();
	info.avgMs = w->avgMs.load();
	info.maxMs = w->maxMs.load();
	const i64 beat = w->beatNs.load();
	info.heartbeatAgeMs =
		beat ? ToMs(Clock::now() - Clock::time_point(Clock::duration(beat))) : 0.0;
	info.hz = w->hz.load();
	info.paused = w->paused.load();
	info.restarts = w->restarts.load();
	info.priority = w->priority.load();
	// Watchdog: a tick still Running past its budget is reported as Stalled (the
	// worker keeps going — this is a detection overlay, not a stored transition).
	// Sleeping/Paused don't count: their heartbeat is old by design.
	if (w->watchdogMs > 0 && info.state == State::Running &&
		info.heartbeatAgeMs > static_cast<double>(w->watchdogMs))
		info.state = State::Stalled;
	// A force-terminated slot reads Quarantined regardless of the frozen atomics.
	if (w->quarantined.load()) info.state = State::Quarantined;
	{
		std::lock_guard<std::mutex> lk(w->errMx);
		info.lastError = w->lastError;
	}
	return info;
}

std::vector<WorkerInfo> Manager::SnapshotAll() const {
	std::vector<WorkerId> ids;
	{
		std::lock_guard<std::mutex> lk(m_mx);
		ids.reserve(m_workers.size());
		for (const auto& w : m_workers) ids.push_back(w->id);
	}
	std::vector<WorkerInfo> out;
	out.reserve(ids.size());
	for (WorkerId id : ids) out.push_back(Inspect(id));
	return out;
}

size_t Manager::Count() const {
	std::lock_guard<std::mutex> lk(m_mx);
	return m_workers.size();
}

void Manager::Reap() {
	std::lock_guard<std::mutex> lk(m_mx);
	// Remove only fully-stopped slots: Dead = cleanly joined, Quarantined = force-
	// terminated + detached. Either way the thread is gone (not joinable), so the
	// Worker's destruction joins nothing and frees no in-use state.
	//
	// And only one NO LIFECYCLE OP HOLDS. A Restart reads exactly like a dead
	// slot between its join and its relaunch - Dead, not joinable - and the
	// supervisor's reboot of a stalled worker is one; reaped there, the reboot
	// went on to launch a thread on a Worker the registry had let go of, and a
	// stalled AI bucket left the registry for good (code-review C386). A try-lock,
	// so a reap never waits on a 250 ms grace (lock order m_mx -> controlMx, and
	// a try never blocks). The lock is released before the erase destroys
	// anything, and a slot taken is marked removed under it, so a Restart that
	// looked it up first and is queued on the lock gives up.
	std::erase_if(m_workers, [](const std::shared_ptr<Worker>& w) {
		std::unique_lock ctl(w->controlMx, std::try_to_lock);
		if (!ctl.owns_lock()) return false; // mid-op: not stopped, whatever it reads
		const State s = w->state.load();
		if ((s != State::Dead && s != State::Quarantined) || w->thread.joinable())
			return false;
		w->removed.store(true);
		return true;
	});
}

} // namespace dungeon::threads
