---
description: Run the regression suite (quick tier by default) and report what drifted
argument-hint: "[quick|full|selftest|list] or -Only <names>"
allowed-tools: PowerShell, Read, Grep, Glob
---

Run the project's regression suite and report the result.

`tools\CheckAll.ps1` is the suite; this command runs it and interprets the
output. The suite exists to catch DRIFT — something that used to hold quietly
ceasing to hold — not to find new bugs.

## What to run

Map the argument (`$ARGUMENTS`, may be empty) to an invocation:

| argument | command | ~time |
|---|---|---|
| *(empty)* or `quick` | `.\tools\CheckAll.ps1` | ~1 min |
| `full` | `.\tools\CheckAll.ps1 -Full` | ~20 min |
| `selftest` | `.\tools\CheckAll.ps1 -Full -SelfTest` | ~15 min |
| `list` | `.\tools\CheckAll.ps1 -List` | instant |
| anything else | pass it through verbatim (e.g. `-Only diag,health`) | — |

Run it with a generous timeout — the full tier drives the real game several
times and each launch waits on a cold-cache load.

## How to report

Lead with the verdict line (`checkall RESULT=...`) and the per-check table. Then:

- **If everything passed**, say so in a sentence. Do not pad it.
- **If something failed**, that is the whole point of the run. For each failed
  check, read its output and say *what* broke, not just that it broke. The
  per-check scripts print a `[FAIL]` line naming the specific expectation, and
  most also write detail to `build\debug\bin\dungeon.log`.
- **Do not fix anything unless asked.** Report first; the user decides whether a
  failure is a regression to fix or an expectation to update.

## Two things worth knowing when reading a failure

**A check that passes is not automatically trustworthy.** `ThreadStress` once
computed every pass/fail condition it existed for, printed them as prose, and
returned 0 regardless — so a whole phase had drifted into measuring an empty
world while printing a confident table. That is why `selftest` exists: it asks
every checker to fail on purpose, and a checker that passes when handed a
failure is broken, however green it looks.

**Coverage gaps are reported, not hidden.** `InGameTest` names the screens it
did *not* sweep, and `HealthTest` names the event kind it cannot drive. If a run
mentions something uncovered, that is deliberate output, not an error.
