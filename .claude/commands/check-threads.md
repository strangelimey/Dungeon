---
description: Thread system under load — no force-terminate, clean supervised reboots
argument-hint: "[selftest]"
allowed-tools: PowerShell, Read, Grep, Glob
---

Drive the real `ThreadManager` + AI buckets under synthetic load (~40 s).

- no argument → `.\tools\CheckAll.ps1 -Only threads`
- `selftest` → `.\tools\CheckAll.ps1 -Only threads -SelfTest` (must FAIL)

Seven phases: baseline, asymmetric per-bucket load, heavy full-BFS, a ramp into
the supervisor's reboot zone, the global governor, cooperative kill/restart, and
lockstep entered while bucket 0 is caught mid-tick (G, code-review C63: it must
return only once every worker holds at its pause point, nothing may publish
after it, and a worker rebooted under lockstep must come back still paused).
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
still green. If the record check fires, it names the worker and the reason.

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
