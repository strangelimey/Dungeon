---
description: Check the checkers — every harness is handed a failure and must report it
allowed-tools: PowerShell, Read, Grep, Glob
---

Run every self-testable check in its fail-on-purpose mode. **A `FAIL` from each
individual check is the PASS condition here**, and the runner inverts the
verdict accordingly — exit 0 means every checker correctly caught the failure it
was handed.

```
.\tools\CheckAll.ps1 -Full -SelfTest
```

## Why this exists

A checker that has never been watched fail is a checker nobody should trust.
This is not hypothetical in this repo: `ThreadStress` computed every pass/fail
condition it existed for, printed them as prose, and returned 0 unconditionally.
A whole phase had drifted into driving 4488 monsters that paid no pathing at all
— 0.4 ms a tick — while printing a table that looked entirely healthy. Nothing
noticed, because nothing could.

Writing its self-test then exposed a second layer: the per-phase checks read
`State::Quarantined`, a flag `Restart` clears on its way to relaunching. In the
self-test run a worker was force-terminated **26 times** and every one of those
checks still passed. Only the check that reads the health record caught it.

## Reading the result

- **`checkall RESULT=PASS`** — every checker reported the failure it was given.
- **A check that "passes" its self-test run** (i.e. reports success when handed a
  failure) is broken, however green it normally looks. Say which one, plainly.
- **Checks with no self-test mode are named and skipped**, never counted as
  passing. Currently `build-debug`, `build-release` and `diag` — all
  compile-or-run gates whose failure mode is not silent.
