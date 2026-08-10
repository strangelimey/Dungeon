---
description: Run the quick regression tier (~1 min) and report what drifted
argument-hint: "[extra CheckAll.ps1 flags]"
allowed-tools: PowerShell, Read, Grep, Glob
---

Run the quick regression tier and report the result.

```
.\tools\CheckAll.ps1 $ARGUMENTS
```

Quick is the everyday tier — debug build, the health record's unit test, and the
in-game audits — about a minute. It is meant to be run often enough that drift
is caught while you still remember what you changed.

## The family

| command | covers |
|---|---|
| `/check` | quick tier (this one) |
| `/check-full` | everything, ~20 min |
| `/check-selftest` | every checker must FAIL on purpose |
| `/check-build` | debug + release compile |
| `/check-diag` | the health record's ring |
| `/check-threads` | ThreadManager under load |
| `/check-health` | crashes are caught and explained |
| `/check-alloc` | steady-state frames allocate nothing |
| `/check-ingame` | levels, models, and a UI overlap sweep |
| `/check-bc7` | the BC7 encoder |

## How to report — applies to every command in this family

Lead with the `checkall RESULT=` line and the per-check table. Then:

- **Everything passed** — say so in a sentence. Don't pad it.
- **Something failed** — that is the entire point of the run. Read that check's
  output and say *what* broke, not just that it broke. Each script prints a
  `[FAIL]` line naming the specific expectation, and the game-driving ones leave
  detail in `build\debug\bin\dungeon.log`.
- **Don't fix anything unless asked.** Report first; the user decides whether a
  failure is a regression to fix or an expectation that should be updated.

## Two things worth knowing when reading any result

**A check that passes is not automatically trustworthy.** `ThreadStress` once
computed every pass/fail condition it existed for, printed them as prose, and
returned 0 regardless — so a whole phase had drifted into measuring an empty
world behind a confident-looking table. That is what `/check-selftest` is for.

**Coverage gaps are printed, not hidden.** `/check-ingame` names the screens it
could not sweep and `/check-health` names the event kind it cannot drive. If a
run mentions something uncovered, that is deliberate output, not an error.
