// ============================================================================
// Core/ThreadManager.cpp — see ThreadManager.h.
// ============================================================================
#include "Core/ThreadManager.h"

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
	case State::Cancelling: return "cancelling";
	case State::Dead: return "dead";
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

	std::atomic<State> state{State::Starting};
	std::atomic<u64> iterations{0};
	std::atomic<double> lastMs{0.0};
	std::atomic<double> avgMs{0.0};
	std::atomic<double> maxMs{0.0};
	std::atomic<i64> beatNs{0}; // Clock::now() of the current tick's start

	std::mutex errMx;
	std::string lastError;

	// Interruptible cadence sleep (also the seam Pause/SetRate will reuse).
	std::mutex sleepMx;
	std::condition_variable_any sleepCv;

	std::jthread thread; // MUST be last (see comment above)
};

Manager::Manager() = default;

Manager::~Manager() {
	// Ask everyone to stop (wakes any interruptible sleep), then join. The
	// jthread destructor would do this too, but doing it explicitly controls the
	// order and wakes sleepers before we block on a join.
	for (auto& w : m_workers) w->thread.request_stop();
	for (auto& w : m_workers)
		if (w->thread.joinable()) w->thread.join();
}

WorkerId Manager::Spawn(JobFn job, Options opt) {
	auto w = std::make_unique<Worker>();
	Worker* p = w.get();
	p->name = std::move(opt.name);
	p->job = std::move(job);
	p->hz.store(opt.hz);

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
	while (!st.stop_requested()) {
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

		const float hz = w->hz.load();
		if (hz > 0.0f && !st.stop_requested()) {
			w->state.store(State::Sleeping);
			const std::chrono::duration<double> interval(1.0 / hz);
			std::unique_lock<std::mutex> lk(w->sleepMx);
			// Wakes on timeout OR a stop request (condition_variable_any's
			// stop_token overload registers the wake for us).
			w->sleepCv.wait_for(lk, st, interval, [] { return false; });
		}
	}
	w->state.store(State::Dead);
}

Manager::Worker* Manager::Get(WorkerId id) const {
	std::lock_guard<std::mutex> lk(m_mx);
	return id < m_workers.size() ? m_workers[id].get() : nullptr;
}

void Manager::RequestStop(WorkerId id) {
	if (Worker* w = Get(id)) {
		w->state.store(State::Cancelling);
		w->thread.request_stop(); // also wakes the interruptible sleep
	}
}

void Manager::Stop(WorkerId id) {
	Worker* w = Get(id); // resolve under the lock, then join WITHOUT it held
	if (!w) return;
	w->state.store(State::Cancelling);
	w->thread.request_stop();
	if (w->thread.joinable()) w->thread.join();
	w->state.store(State::Dead);
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
