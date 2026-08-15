---
description: Run the damage-system eval suites and report what they measured
argument-hint: "[list|selftest|table|<suite>]"
allowed-tools: PowerShell, Read, Grep, Glob
---

Drive the eval harness (docs/eval-harness.md). The game runs its own console
scripts unattended — no clicks, no screenshots.

- no argument → `.\tools\Eval.ps1` — every suite, with its measurements
- `list` → `.\tools\Eval.ps1 -List` — the suites and the fragments
- `table` → `.\tools\Eval.ps1 -Table` — the measurements alone, nothing else
- `selftest` → `.\tools\Eval.ps1 -SelfTest` (must PASS by making the runner FAIL)
- anything else → `.\tools\Eval.ps1 -Only <name>`

Needs a debug build; ~2 minutes for all six.

## THIS IS NOT A PASS/FAIL CHECK

**A green verdict means the scripts RAN, not that the numbers are good.** That is
why it is not in `CheckAll`'s tiers and why it has its own command.

The numbers are the artefact — `TALLY` lines and blast tables, to be **compared
against a previous run** after a knob changes. Nothing asserts that a balance
figure is correct, because nobody has decided what correct is. The same lesson
the area-blast work paid for: *both bugs were invisible in the pass/fail checks
and obvious in the table.*

**Report the measurements, not the verdict.** "eval RESULT=PASS" on its own is
almost content-free. Give the table, and say what moved.

## Do not propose rebalancing

Michael tunes by playtesting; the harness reports. Surface what a measurement
shows and stop there unless he asks. He has already said he *likes* how weak the
starting party is against the starting content — a lopsided table is not
automatically a defect.

## What each suite is for

| suite | what it measures |
|---|---|
| `smoke` | the runner drives the game start to finish — machinery only |
| `arena` | arenas carve, monsters spawn where asked, a fight resolves |
| `tiers` | one encounter at two preset tiers, same seed |
| `ladder` | the progression ladder: walk in, fight, tp away, repeat, **no healing between rungs** |
| `blast` | one detonation in four geometries |
| `sweep` | one rung over twelve seeds — a distribution, not an anecdote |

## Reading them

**One encounter is an anecdote.** Combat is dice: a 5% fumble or a 6% critical
cannot show honestly in a single fight. Prefer `sweep` for anything you intend to
act on, and treat a single `ladder` rung as a sample of one.

**Damage is in absolute points, never a fraction of health** — the healing model
is still undesigned (docs/health-and-healing.md), and fractions would change
meaning the day it lands.

**A rung that "timed out" is a result, not an error.** `slain` below the number
spawned means the encounter outlasted its `step` window. Say so rather than
reporting it as a failure.

**The ladder is cumulative by design.** No healing between rungs, so `party`
health falls down the file; the per-rung `TALLY` is reset each time. Attrition
lives in the party line, throughput in the tally.

## If a suite FAILS

That is the runner breaking, not balance moving. Exit codes: **1** a script line
matched no command or the run timed out; **2** the script could not be read.
`dungeon.log` beside the exe has every console line (`logecho` is forced on for
scripted runs), and a crash leaves a symbolized stack and a minidump there too.

Two traps that have already bitten, both of which look like a broken build:

- **Dev commands work from the MENU.** After a party wipe the app returns to the
  title screen and every `step` is correctly refused — `state` is how a script
  sees it, and `step` says so rather than reporting a bare zero.
- **A stale exe.** If the build failed, the script still runs against the last
  binary and reports plausible numbers. Check the build succeeded first.
