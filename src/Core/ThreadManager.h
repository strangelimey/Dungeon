// ============================================================================
// Core/ThreadManager.h — the engine's managed worker threads.
//
// A central registry of long-lived worker threads with full control: spawn a
// named worker running a repeating job, stop ANY worker (cooperatively), and
// INSPECT any worker's live state (heartbeat, iteration count, timings, last
// error) at any time without blocking it. This is the one home for "lots of
// stuff on lots of threads" — monster AI, pathfinding, asset streaming, etc.
// each just becomes a CLIENT that Spawn()s its workers here, so kill / inspect /
// throttle all work uniformly across the engine.
//
// Model: a worker runs a JOB once per tick in a loop the manager owns. The job
// does one unit of work and returns; the manager handles the loop, the cadence
// (Options::hz), cancellation, timing, and crash capture. Cancellation is
// COOPERATIVE — you cannot safely force-kill a thread, so every job gets a
// std::stop_token it must check (and pass to any blocking wait); the manager's
// interruptible sleep wakes the instant a stop is requested.
//
// Step 1 surface: spawn / stop / inspect + per-worker cadence + OS thread
// naming + per-tick crash capture. Throttle controls (pause, live SetRate,
// global governor), priority/affinity, watchdog/stall detection, and the
// last-resort quarantine Kill() layer in on top of this without reshaping it.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace dungeon::threads {

using WorkerId = u32;
inline constexpr WorkerId kInvalidWorker = ~0u;

// A worker's lifecycle state (see the diagram in the design notes). Stalled is a
// DERIVED view (a tick past its watchdog budget) reported by Inspect; the worker
// keeps running. Quarantined means it was force-terminated by a hard Kill (it
// would not stop cooperatively) — its slot is poisoned until a Restart.
enum class State {
	Starting, Running, Sleeping, Paused, Stalled, Cancelling, Dead, Quarantined
};
const char* StateName(State s);

// Handed to a job each tick. The job does ONE unit of work and returns; the
// manager owns the surrounding loop.
struct Tick {
	std::stop_token stop; // check often, and pass to any blocking wait you do
	u64 iteration;        // 0-based tick counter for this worker
	WorkerId self;        // this worker's id (e.g. to inspect/throttle itself)
};
using JobFn = std::function<void(const Tick&)>;

struct Options {
	std::string name;       // human-readable + set as the OS thread name (debuggers)
	float hz = 0.0f;        // re-run cadence: 0 = flat-out (job should block), >0 = throttle
	unsigned watchdogMs = 0;// flag a tick that runs longer than this as Stalled; 0 = off
	bool autoRestart = false;// if it stalls past the grace window, the supervisor reboots it
	int priority = 0;       // OS thread priority: -2..+2 (lowest..highest), 0 = normal
	u64 affinity = 0;       // CPU affinity mask (bit per core); 0 = any core
};

// A lock-free-read snapshot of one worker's live state. Inspect/SnapshotAll
// copy these out so the caller never blocks a running worker.
struct WorkerInfo {
	WorkerId id = kInvalidWorker;
	std::string name;
	State state = State::Dead;
	u64 iterations = 0;
	double lastMs = 0.0;        // duration of the most recent job tick
	double avgMs = 0.0;         // smoothed average tick duration
	double maxMs = 0.0;         // worst tick duration seen
	double heartbeatAgeMs = 0.0;// time since the current tick began (stall signal)
	float hz = 0.0f;            // configured cadence
	bool paused = false;        // pause requested (may lead the Paused state by a tick)
	u32 restarts = 0;           // times this worker has been rebooted (manual or supervisor)
	int priority = 0;           // OS thread priority (-2..+2)
	std::string lastError;      // message from the last job exception, if any
};

// ----------------------------------------------------------------------------
// The manager. Owns every worker for its lifetime; destruction stops and joins
// them all. Spawning is the only structural mutation and is rare, so it takes a
// brief lock; per-worker stats are atomics, so Inspect/SnapshotAll never block a
// worker. Workers are addressed by a stable WorkerId (an index that is never
// reused or invalidated — a stopped worker stays in the registry as Dead).
// ----------------------------------------------------------------------------
class Manager {
public:
	// Both defined out-of-line in the .cpp: Worker is incomplete here, so the
	// vector<unique_ptr<Worker>> destruction these would emit (ctor unwind, dtor)
	// can't be instantiated in a client TU.
	Manager();
	~Manager(); // requests stop on every worker and joins them
	Manager(const Manager&) = delete;
	Manager& operator=(const Manager&) = delete;

	// Launch a named worker running `job` once per tick at Options::hz. Returns
	// its stable id.
	WorkerId Spawn(JobFn job, Options opt);

	// Cooperatively ask a worker to stop (non-blocking): wakes it from any
	// interruptible sleep; it exits after its current tick. The job sees this via
	// its Tick::stop token.
	void RequestStop(WorkerId id);

	// Request stop AND block until the worker has finished and joined. A client
	// MUST Stop its own workers in its destructor before its captured state dies,
	// since the job closure typically references that client.
	void Stop(WorkerId id);

	// Throttle controls — all take effect immediately (they wake the worker's
	// cadence sleep). Pause holds the worker after its current tick without
	// joining it; Resume releases it; SetRate changes the re-run cadence (hz<=0
	// means run flat-out). No-ops for an unknown or dead worker.
	void Pause(WorkerId id);
	void Resume(WorkerId id);
	void SetRate(WorkerId id, float hz);
	void SetPriority(WorkerId id, int priority); // -2..+2, applied to the live thread
	void SetAffinity(WorkerId id, u64 mask);     // CPU mask (0 = any core)

	// Hard, last-resort stop: ask the worker to stop, and if it won't within a
	// short grace (a job ignoring its token / wedged in a loop), FORCE-terminate
	// the OS thread and mark the slot Quarantined. Force-termination can leak
	// locks the job held — only for a genuinely stuck thread. A cooperative
	// worker exits cleanly here instead (no termination). Restart recovers the
	// slot. Use Stop/RequestStop for the normal cooperative path.
	void Kill(WorkerId id);

	// Global throttle governor: scales EVERY worker's cadence by `scale`
	// (1 = normal, 0.5 = half rate, >1 = faster). Lets the frame loop ease all
	// background work when it is over budget. Takes effect immediately.
	void SetGlobalThrottle(float scale);
	float GlobalThrottle() const { return m_globalScale.load(); }

	// Reboot a worker: cooperatively stop the current thread (request stop, let
	// the in-flight tick finish, JOIN), reset its stats, and relaunch it on the
	// same id with its original job. The join-before-relaunch makes it race-free,
	// but a worker truly wedged in an infinite loop (never checking its token)
	// will block here — that case needs the hard-quarantine kill (a later step).
	// Clears the user-stopped flag, so a killed worker booted here runs again and
	// is supervised again.
	void Restart(WorkerId id);

	// Lock-free reads of live worker state. Inspect returns a Dead-stated default
	// for an unknown id.
	WorkerInfo Inspect(WorkerId id) const;
	std::vector<WorkerInfo> SnapshotAll() const;
	size_t Count() const;

private:
	struct Worker; // opaque (holds atomics + the jthread); defined in the .cpp
	void Run(Worker* w, std::stop_token st);
	void SupervisorLoop(std::stop_token st); // reboots stalled autoRestart workers
	// Stop a worker, force-terminating it if it won't stop cooperatively. Leaves
	// w->thread joined (clean) or detached (quarantined). Caller serialises.
	void StopOrTerminate(Worker* w);
	Worker* Get(WorkerId id) const; // null if out of range

	mutable std::mutex m_mx; // guards the m_workers vector structure only
	std::vector<std::unique_ptr<Worker>> m_workers;
	std::atomic<float> m_globalScale{1.0f}; // governor: multiplies every cadence
	std::jthread m_supervisor; // monitors heartbeats; last member = stopped first
};

} // namespace dungeon::threads
