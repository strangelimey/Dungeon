---
description: Thread system under load — no force-terminate, clean supervised reboots
argument-hint: "[selftest]"
allowed-tools: PowerShell, Read, Grep, Glob
---

Drive the real `ThreadManager` + AI buckets under synthetic load (~45 s).

- no argument → `.\tools\CheckAll.ps1 -Only threads`
- `selftest` → `.\tools\CheckAll.ps1 -Only threads -SelfTest` (must FAIL)

Eight phases. RUN FIRST, phase H races `threadreap` against the supervisor
rebooting three stalled workers and one wedged one, with this thread reaping
flat out (code-review C386: a Restart reads Dead and not joinable between its
join and its relaunch, and a reap there lost the worker). Nothing may leave the
registry mid-reboot, no tick may begin on a reaped worker, and Reap must still
take a worker stopped for good and one killed. It also reads the health record:
every stall and every forced kill must carry the worker's own WALKED stack,
through the job's code and not the supervisor's or the killer's (C387). It runs first because a force-terminated thread never gives
its health-record slot back, and the self-test's planted worker fills the table
by the end of the run. Then: baseline, asymmetric per-bucket load, heavy
full-BFS, a ramp into the supervisor's reboot zone, the global governor,
cooperative kill/restart, and lockstep entered while bucket 0 is caught mid-tick
(G, code-review C63: it must return only once every worker holds at its pause
point, nothing may publish after it, and a worker rebooted under lockstep must
come back still paused).
The synthetic world is built the way `DungeonWorld::BuildAISnapshot` builds the
real one: each monster chases the party's cell, monsters crowd through `occ`
(not `blocked`), and each bucket's IQ is derived from `Scheduler::BucketForIq`.
Every plan batch is AUDITED against its snapshot, so each phase also checks the
work it claims to do, with two checks. The batch check: every bucket that has
monsters publishes a whole batch, and every batch of this snapshot plans each
monster of its bucket exactly once and nothing else. The path check: in a
reachable phase every plan is an engage with a real path to the party, and in a
walled-off phase none finds one. Together they say it of every monster.

The self-test injects two faults: a worker that ignores its stop token, and
every chase target put back at (0,0). It passes only when exactly the six checks
listed in `kSelfTestFails` fail and every other check passes. Those six are the
health-record check and the five reachable-path checks. The batch checks must
stay green. A self-test that fails something else, or misses one of the six, is
itself a FAIL, and the run names each mismatch.

## What it is really guarding

That an overloaded worker stops **cooperatively**. The fallback is
`TerminateThread`, which runs no unwinding and leaks any lock the job held — and
if it was mid-allocation that lock is the CRT heap lock, which deadlocks the
whole process on the next `new`.

## Reading a failure

**The check that matters is the last one**, `nothing was force-terminated all
run (from the health record)`. The per-phase `State::Quarantined` scans are
nearly decorative: `Restart` sets that flag via `StopOrTerminate` and then
*clears* it before relaunching, so a force-terminated worker reads as `Running`
moments later. Measured in the self-test: 26 force-terminates, every state scan
still green. If the record check fires, it names the worker and the reason. It
leaves out phase H's `stress.reapwedge`, which H force-terminates on purpose.

**If an H check fails**, read its two lines. `threadreap never took a worker the
supervisor was rebooting` failing, with `fewest seen` below the registry size,
is C386 back: `Manager::Reap` reaping by state alone instead of passing over a
slot whose `controlMx` it cannot take. `ticks begun on a reaped worker` above 0
is the same bug seen from inside. `still drops a worker stopped for good` failing
means Reap was made safe by never reaping. The `record:` line counts stalls and
kills recorded with the WORKER's own walked stack: one through its job
(`StallTick` / `WedgeTick`, symbolized) that names nothing only the supervisor
or the killer runs (`SupervisorLoop`, `RestartWorker`, `StopOrTerminate`,
`Kill`, `WalkWorker`, `PhaseReap`, `main`). "A frame in the exe" would not do:
Core is a static lib, so the supervisor's own stack is in the exe too. A
shortfall is C387 - the supervisor's stall walk or `StopOrTerminate`'s
pre-terminate walk not attaching its frames, or attaching the wrong thread's -
and `first other:` under it shows the first stack that failed, top 8 frames.

**If `D: the ramp reached the reboot zone and the supervisor rebooted it`
fails**, suspect the workload rather than the thread system. That is exactly how
the earlier drift presented: the harness sets `aggroRange = 1e9f` to force
engagement, `ai::Agent` later grew a perception model (`aware`, `directional`,
sight cones), and the monsters quietly stopped engaging. Check `avgMs` in the
phase D table - if thousands of monsters cost fractions of a millisecond,
nothing is pathing.

**If a path check fails**, read its `paths:` line. In a reachable phase, `empty`
means the monsters are chasing something they cannot reach. That is how C418
presented: the targets were never set, so every monster chased (0,0), a border
wall, and every "reachable" phase measured a failed full-map search. `malformed`
counts paths that are not a real walk: they leave the walkable grid, pass
through the party's cell or another monster's square, or do not end on the
party.

**If a batch check fails**, read the `batches:` line under it. It counts whole
batches per bucket, names any bucket that has monsters and published none, and
quotes the first bad batch: empty while its bucket has monsters, planning a
monster twice or another bucket's, missing some, or MIXING this snapshot's plans
with an earlier one's (a pooled batch that kept a bigger tick's tail - drop
`out->resize(used)` in `AsyncDirector::ComputeBucket` and phases C to F show
it). `stale skipped` is not a failure: the first batch after a new snapshot may
come from a tick that was already running on the old one. Only that first one
may.

**If a G check fails**, read its `caught=` line. `G: lockstep began while
bucket0 was mid-tick` failing means the setup missed (a tick too short to catch,
or the sizing never reached ~40 ms) and the rest of G proved nothing.
`returned only once every worker held` and `no worker published` failing
together is C63 back: `AsyncDirector::SetLockstep` no longer waits
(`Manager::WaitPaused`). `comes back paused` failing is `Manager::Restart`
clearing `paused` again.

**Timing checks are loose on purpose** (the governor ones use 0.7x / 0.6x
margins) because these are real threads on a shared machine. A flaky check gets
ignored, which is worse than no check. The governor phase counts each window's
ticks from before its `SetGlobalThrottle` call, because that call wakes every
worker for one immediate tick. Counted from after the call, that tick fell
inside the window or outside it depending on a race, and bucket 3 makes only
one or two ticks a window.

**The self-test is load-sensitive.** Its target fault makes every reachable
phase a full-map search, so its ticks run several times longer than a normal
run's. On a machine already at full CPU (another session's build), an AI bucket
can then miss the supervisor's 250 ms grace and be force-terminated, and the
self-test also fails F's checks. Rerun it on a quiet machine before suspecting
the harness.

**So is the ordinary run, under enough load** (measured 2026-10-06, while
another worktree built debug, release and release-profile at once). The AI
workers run below normal priority, so a machine saturated at normal priority
starves them: a two-monster tick took 200 ms, F's idle bucket could not wake
within the 250 ms grace and was force-terminated, and the run then HUNG in G - a
thread terminated mid-wake leaves its condition variable's lock held, and the
next Pause blocks on it forever. That is the TerminateThread hazard this harness
exists to keep away from, reached by starvation rather than by a bug. If a run
hangs, look for `killed on 'ai.bucket` in `build\debug\bin\threadstress.log`
and for other builds on the machine, and kill the stuck ThreadStress by its PID.
