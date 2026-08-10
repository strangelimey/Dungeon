---
description: Thread system under load — no force-terminate, clean supervised reboots
argument-hint: "[selftest]"
allowed-tools: PowerShell, Read, Grep, Glob
---

Drive the real `ThreadManager` + AI buckets under synthetic load (~36 s).

- no argument → `.\tools\CheckAll.ps1 -Only threads`
- `selftest` → `.\tools\CheckAll.ps1 -Only threads -SelfTest` (must FAIL)

Six phases: baseline, asymmetric per-bucket load, heavy full-BFS, a ramp into
the supervisor's reboot zone, the global governor, and cooperative kill/restart.

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

**If the ramp reports `never reached the reboot zone`**, suspect the workload
rather than the thread system. That is exactly how the earlier drift presented:
the harness sets `aggroRange = 1e9f` to force engagement, `ai::Agent` later grew
a perception model (`aware`, `directional`, sight cones), and the monsters
quietly stopped engaging. Check `avgMs` in the phase D table — if thousands of
monsters cost fractions of a millisecond, nothing is pathing.

**Timing checks are loose on purpose** (the governor ones use 0.7x / 0.6x
margins) because these are real threads on a shared machine. A flaky check gets
ignored, which is worse than no check.
