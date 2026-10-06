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
#include <chrono>
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
// worker. Workers are addressed by a stable WorkerId (an id that is never
// reused - a stopped worker stays in the registry as Dead until Reap or Remove
// takes it out, and an id that is gone just answers as unknown).
// ----------------------------------------------------------------------------
class Manager {
public:
	// Both defined out-of-line in the .cpp, where Worker is complete.
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

	// Request stop AND block until the worker has finished and joined. The slot
	// stays, Dead, holding its job - which a Restart (the console's `boot`) runs
	// again. So a client whose job captures the client itself must not Stop its
	// workers in its destructor: it must Remove them.
	void Stop(WorkerId id);

	// Stop (and join) a worker, then take it OUT of the registry and drop its
	// job, so nothing can ever run that job again: a Restart already waiting on
	// it gives up, and the id answers as unknown from here on. This is what a
	// client's destructor calls when its job captures `this` (AsyncDirector).
	// Blocks like Stop, so a worker that never checks its token blocks it too.
	void Remove(WorkerId id);

	// Throttle controls — all take effect immediately (they wake the worker's
	// cadence sleep). Pause holds the worker after its current tick without
	// joining it; Resume releases it; SetRate changes the re-run cadence (hz<=0
	// means run flat-out). No-ops for an unknown or dead worker.
	//
	// PAUSE ONLY ASKS. It sets a flag and returns: a worker in the middle of a
	// tick finishes it first, publishing whatever that tick makes. WaitPaused is
	// the other half - it blocks until the worker is HOLDING at its pause point,
	// so no tick of its is in flight, and answers false after `timeout` (or when
	// no pause is pending). A worker with no thread left (Dead, or quarantined)
	// counts as quiet.
	void Pause(WorkerId id);
	bool WaitPaused(WorkerId id, std::chrono::milliseconds timeout);
	void Resume(WorkerId id);
	void SetRate(WorkerId id, float hz);
	void SetPriority(WorkerId id, int priority); // -2..+2, applied to the live thread
	void SetAffinity(WorkerId id, u64 mask);     // CPU mask (0 = any core)

	// Hard, last-resort stop: ask the worker to stop, and if it won't within a
	// short grace (a job ignoring its token / wedged in a loop), FORCE-terminate
	// the OS thread and mark the slot Quarantined. Force-termination can leak
	// locks the job held — only for a genuinely stuck thread. A cooperative
	// worker exits cleanly here instead (no termination). Restart recovers the
	// slot. Use Stop/RequestStop for the normal cooperative path. A forced kill
	// is recorded (diag Killed) with the stack the thread was stuck in, walked
	// from outside just before the terminate (code-review C387).
	void Kill(WorkerId id);

	// Global throttle governor: scales EVERY worker's cadence by `scale`
	// (1 = normal, 0.5 = half rate, >1 = faster). Lets the frame loop ease all
	// background work when it is over budget. wakeNow=true applies it this instant
	// (one-shot manual use); false lets it land on each worker's next natural wake
	// (the per-frame adaptive governor, so it doesn't wake everyone every frame).
	void SetGlobalThrottle(float scale, bool wakeNow = true);
	float GlobalThrottle() const { return m_globalScale.load(); }

	// Reboot a worker: cooperatively stop the current thread (request stop, let
	// the in-flight tick finish, JOIN), reset its stats, and relaunch it on the
	// same id with its original job. The join-before-relaunch makes it race-free;
	// a worker truly wedged in an infinite loop (never checking its token) is
	// force-terminated after Kill's grace instead, and recorded as Kill records.
	// Clears the user-stopped flag, so a killed worker booted here runs again and
	// is supervised again. KEEPS a pause: a worker paused when it is rebooted
	// comes back paused, so the supervisor rebooting a stalled worker cannot
	// release one that lockstep is holding (code-review C63). A removed worker
	// (Remove) is not restarted.
	void Restart(WorkerId id);

	// THE PROBE: what is this worker doing RIGHT NOW. Suspends it, walks its
	// stack, resumes it — the only way to answer "what is it stuck on" for a
	// thread that is stalled rather than crashed, since a stalled thread is still
	// running and has thrown nothing to record.
	//
	// Costs the worker a pause of microseconds. Returns 0 for an unknown, dead or
	// self-referencing id, and for one a lifecycle op (a Restart, a Kill) holds
	// past half a second - its thread is being replaced. See stack::WalkThread
	// for why the walk uses the PE unwind tables rather than DbgHelp. The
	// supervisor walks a worker the same way at the start of each stall, so the
	// record says where it was stuck even after the reboot has ended that thread.
	int CaptureStack(WorkerId id, void** out, int max) const;

	// Lock-free reads of live worker state. Inspect returns a Dead-stated default
	// for an unknown id.
	WorkerInfo Inspect(WorkerId id) const;
	std::vector<WorkerInfo> SnapshotAll() const;
	size_t Count() const;

	// Drop fully-stopped workers (Dead or Quarantined, thread gone) from the
	// registry so it doesn't grow without bound as short-lived workers come and
	// go. WorkerIds are stable, so survivors keep theirs. Safe from any thread.
	//
	// It takes only a slot NO LIFECYCLE OP HOLDS (it try-locks each worker's
	// controlMx and passes over one it cannot get). A Restart, between its join
	// and its relaunch, leaves a slot that reads Dead and not joinable - and the
	// supervisor's reboot of a stalled worker is exactly that - so a reap that
	// went by state alone dropped a worker part-way through its reboot, which
	// then relaunched on a slot nothing could see (code-review C386). A slot it
	// does take is marked removed under that lock, so a Restart that looked it
	// up first and is waiting its turn finds it gone and gives up. It drops only
	// the REGISTRY'S hold on a Worker: one the supervisor is looking at lives on
	// in its copy (Get hands out shared ownership).
	void Reap();

private:
	struct Worker; // opaque (holds atomics + the jthread); defined in the .cpp
	void Run(Worker* w, std::stop_token st);
	void SupervisorLoop(std::stop_token st); // reboots stalled autoRestart workers
	// Restart's body. The supervisor's reboot passes bySupervisor: it decided on
	// the reboot from a look taken without the worker's lock, so a Stop or Kill
	// that landed since wins and the reboot gives up. A manual Restart (`boot`)
	// is meant to revive a stopped worker, so it does not ask.
	void RestartWorker(WorkerId id, bool bySupervisor);
	// Stop a worker, force-terminating it if it won't stop cooperatively. Leaves
	// w->thread joined (clean) or detached (quarantined). Caller serialises.
	void StopOrTerminate(Worker* w);
	// Walks a worker's live thread from outside (stack::WalkThread). The caller
	// holds w->controlMx, so no Restart can be swapping the thread underneath.
	// 0 for a worker with no thread to walk.
	static int WalkWorker(Worker* w, void** out, int max);
	// Null if unknown. SHARED ownership, so a Worker found here outlives a Reap
	// or a Remove that drops it from the registry while the caller still holds
	// it - the supervisor, between its lookup and its checks, is that caller.
	std::shared_ptr<Worker> Get(WorkerId id) const;

	mutable std::mutex m_mx; // guards the m_workers vector structure only
	std::vector<std::shared_ptr<Worker>> m_workers;
	WorkerId m_nextId = 1; // stable id source (not the array index, so Reap is safe)
	std::atomic<float> m_globalScale{1.0f}; // governor: multiplies every cadence
	std::jthread m_supervisor; // monitors heartbeats; last member = stopped first
};

} // namespace dungeon::threads
