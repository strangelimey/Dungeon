// ============================================================================
// Core/ThreadManager.cpp — see ThreadManager.h.
// ============================================================================
#include "Core/ThreadManager.h"

#include "Core/Log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
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

	bool autoRestart = false;        // set once at spawn
	std::atomic<bool> userStopped{false}; // a kill/stop the supervisor must respect
	std::atomic<bool> quarantined{false}; // force-terminated; slot poisoned until reboot
	std::atomic<u32> restarts{0};
	std::atomic<int> priority{0};    // OS thread priority (-2..+2)
	std::atomic<u64> affinity{0};    // CPU affinity mask (0 = any)
	std::mutex controlMx; // serialises lifecycle ops (Stop/Kill/Restart) on this worker

	std::mutex errMx;
	std::string lastError;

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
	auto w = std::make_unique<Worker>();
	Worker* p = w.get();
	p->name = std::move(opt.name);
	p->job = std::move(job);
	p->hz.store(opt.hz);
	p->watchdogMs = opt.watchdogMs;
	p->autoRestart = opt.autoRestart;
	p->priority.store(opt.priority);
	p->affinity.store(opt.affinity);

	std::lock_guard<std::mutex> lk(m_mx);
	p->id = static_cast<WorkerId>(m_workers.size());
	m_workers.push_back(std::move(w));
	// jthread injects the stop_token as the first arg. p is stable (the Worker
	// lives in a unique_ptr; the vector only moves the pointers, never the node).
	p->thread = std::jthread([this, p](std::stop_token st) { Run(p, st); });
	return p->id;
}

void Manager::Run(Worker* w, std::stop_token st) {
	SetOsThreadName(w->name);
#ifdef _WIN32
	SetThreadPriority(GetCurrentThread(), Win32Priority(w->priority.load()));
	if (const u64 mask = w->affinity.load())
		SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(mask));
#endif
	while (!st.stop_requested()) {
		// Paused: hold here (not joined) until resumed or stopped, running no job.
		if (w->paused.load()) {
			w->state.store(State::Paused);
			std::unique_lock<std::mutex> lk(w->sleepMx);
			w->sleepCv.wait(lk, st, [w] { return !w->paused.load(); });
			continue; // re-check stop + paused at the top
		}

		const auto t0 = Clock::now();
		w->beatNs.store(t0.time_since_epoch().count());
		w->state.store(State::Running);

		// Crash capture: one bad tick records its error and the worker keeps
		// running, instead of an unhandled exception calling std::terminate and
		// taking the whole process down. (Restart/quarantine policy is a later step.)
		try {
			w->job(Tick{st, w->iterations.load(), w->id});
		} catch (const std::exception& e) {
			std::lock_guard<std::mutex> lk(w->errMx);
			w->lastError = e.what();
		} catch (...) {
			std::lock_guard<std::mutex> lk(w->errMx);
			w->lastError = "unknown exception";
		}

		const double ms = ToMs(Clock::now() - t0);
		w->lastMs.store(ms);
		if (ms > w->maxMs.load()) w->maxMs.store(ms);
		const double a = w->avgMs.load();
		w->avgMs.store(a <= 0.0 ? ms : a * 0.9 + ms * 0.1); // EMA
		w->iterations.fetch_add(1);

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
	w->state.store(State::Dead);
}

Manager::Worker* Manager::Get(WorkerId id) const {
	std::lock_guard<std::mutex> lk(m_mx);
	return id < m_workers.size() ? m_workers[id].get() : nullptr;
}

void Manager::RequestStop(WorkerId id) {
	Worker* w = Get(id);
	if (!w) return;
	std::lock_guard<std::mutex> ctl(w->controlMx);
	w->userStopped.store(true); // intentional — the supervisor must not revive it
	w->state.store(State::Cancelling);
	w->thread.request_stop(); // also wakes the interruptible sleep
}

void Manager::Stop(WorkerId id) {
	Worker* w = Get(id); // resolve under the lock, then join WITHOUT m_mx held
	if (!w) return;
	std::lock_guard<std::mutex> ctl(w->controlMx);
	w->userStopped.store(true);
	w->state.store(State::Cancelling);
	w->thread.request_stop();
	if (w->thread.joinable()) w->thread.join();
	w->state.store(State::Dead);
}

// Control calls flip the worker's atomics UNDER its sleep mutex, then notify, so
// a worker that is between checking the predicate and waiting can't miss it.
void Manager::Pause(WorkerId id) {
	Worker* w = Get(id);
	if (!w) return;
	{
		std::lock_guard<std::mutex> lk(w->sleepMx);
		w->paused.store(true);
		++w->wakeGen;
	}
	w->sleepCv.notify_all();
}

void Manager::Resume(WorkerId id) {
	Worker* w = Get(id);
	if (!w) return;
	{
		std::lock_guard<std::mutex> lk(w->sleepMx);
		w->paused.store(false);
		++w->wakeGen;
	}
	w->sleepCv.notify_all();
}

void Manager::SetRate(WorkerId id, float hz) {
	Worker* w = Get(id);
	if (!w) return;
	{
		std::lock_guard<std::mutex> lk(w->sleepMx);
		w->hz.store(hz);
		++w->wakeGen; // wake the cadence sleep so the new rate applies now
	}
	w->sleepCv.notify_all();
}

void Manager::SetPriority(WorkerId id, int priority) {
	Worker* w = Get(id);
	if (!w) return;
	std::lock_guard<std::mutex> ctl(w->controlMx); // don't race Restart's relaunch
	w->priority.store(priority);
#ifdef _WIN32
	if (w->thread.joinable())
		SetThreadPriority(static_cast<HANDLE>(w->thread.native_handle()),
						  Win32Priority(priority));
#endif
}

void Manager::SetAffinity(WorkerId id, u64 mask) {
	Worker* w = Get(id);
	if (!w) return;
	std::lock_guard<std::mutex> ctl(w->controlMx);
	w->affinity.store(mask);
#ifdef _WIN32
	if (mask && w->thread.joinable())
		SetThreadAffinityMask(static_cast<HANDLE>(w->thread.native_handle()),
							  static_cast<DWORD_PTR>(mask));
#endif
}

void Manager::SetGlobalThrottle(float scale) {
	m_globalScale.store(std::clamp(scale, 0.05f, 4.0f));
	// Wake every worker's cadence sleep so the new scale takes effect at once.
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
	// Wedged: force-terminate. This can leak whatever locks the job held — last
	// resort only. Abandon (detach) the object so nothing tries to join it.
#ifdef _WIN32
	TerminateThread(static_cast<HANDLE>(w->thread.native_handle()), 1);
#endif
	w->thread.detach();
	w->quarantined.store(true);
	log::Warn("thread '{}' would not stop — force-terminated (locks may have leaked)",
			  w->name);
}

void Manager::Kill(WorkerId id) {
	Worker* w = Get(id);
	if (!w) return;
	std::lock_guard<std::mutex> ctl(w->controlMx);
	w->userStopped.store(true); // intentional — the supervisor must not revive it
	w->state.store(State::Cancelling);
	StopOrTerminate(w);
	w->state.store(w->quarantined.load() ? State::Quarantined : State::Dead);
}

void Manager::Restart(WorkerId id) {
	Worker* w = Get(id);
	if (!w) return;
	std::lock_guard<std::mutex> ctl(w->controlMx);
	// Stop the current thread (force-terminating a wedged one) so the old thread
	// is entirely gone before the new one touches this Worker — no shared-state
	// race, and a stuck worker can't block the reboot.
	w->state.store(State::Cancelling);
	StopOrTerminate(w);

	// Fresh slate; keep the stored job + config. Booting clears the user-stopped
	// and quarantine flags so the worker runs again and is supervised again.
	w->quarantined.store(false);
	w->userStopped.store(false);
	w->paused.store(false);
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
	w->thread = std::jthread([this, w](std::stop_token st) { Run(w, st); });
}

void Manager::SupervisorLoop(std::stop_token st) {
	using namespace std::chrono_literals;
	while (!st.stop_requested()) {
		std::vector<WorkerId> ids;
		{
			std::lock_guard<std::mutex> lk(m_mx);
			ids.reserve(m_workers.size());
			for (const auto& w : m_workers) ids.push_back(w->id);
		}
		for (WorkerId id : ids) {
			Worker* w = Get(id);
			if (!w || !w->autoRestart || w->userStopped.load() || w->watchdogMs == 0)
				continue;
			if (w->state.load() != State::Running) continue; // only a stuck live tick
			const i64 beat = w->beatNs.load();
			if (!beat) continue;
			const double age =
				ToMs(Clock::now() - Clock::time_point(Clock::duration(beat)));
			if (age > static_cast<double>(w->watchdogMs) * 5.0)
				Restart(id); // stalled well past the watchdog: reboot it
		}
		// Coarse poll; checks the stop flag often so shutdown is prompt.
		for (int i = 0; i < 10 && !st.stop_requested(); ++i)
			std::this_thread::sleep_for(10ms);
	}
}

WorkerInfo Manager::Inspect(WorkerId id) const {
	Worker* w = Get(id);
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

} // namespace dungeon::threads
