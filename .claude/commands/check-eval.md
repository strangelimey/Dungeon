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

Needs a current debug build (a stale one is refused, exit 4); a few minutes for
all of them, in one process.

## THIS IS NOT A PASS/FAIL CHECK

**A green verdict means the scripts RAN, not that the numbers are good.** That is
why the suites are not in `CheckAll`'s tiers and why they have their own
command. (`-SelfTest` is the exception: it checks the RUNNER - reset equals a new
game, batched equals solo, headless equals windowed, a knob moves its number - so
it is CheckAll's full-tier `evalrunner` row.)

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

`Eval.ps1 -List`, as it prints today (name, script, what it measures):

<!-- BEGIN generated: eval-suites (tools\CheckDocs.ps1 -Write rewrites this; CheckAll's `docs` check fails on drift) -->
```text
  smoke      smoke.eval           the runner drives the game unattended, start to finish
  arena      arena.eval           arenas carve, monsters spawn where asked, a fight resolves
  tiers      tiers.eval           the same encounter at two preset tiers, same seed
  ladder     ladder.eval          the progression ladder: walk in, fight, tp away, repeat
  blast      blast-geometry.eval  one detonation in four geometries
  sweep      sweep-novice.eval    one rung over twelve seeds - a distribution, not an anecdote
  resources  resources.eval       the three pools: rates, the state gate, and what recovery trains
  supplies   supplies.eval        food and water: what they cost, and what an empty meter does
  rest       rest.eval            the rest state: what it costs, and the three ways it ends
  expedition expedition.eval      fight, retreat, rest, repeat - how many fights a load of supplies buys
  parties    smallparty.eval      parties of one, three and two: who the formation lets a monster reach
  rootmotion rootmotion.eval      a walking, then dying, kit skeleton: how far its body strays from its square
  spawnrise  spawnrise.eval       a freshly spawned skeleton holds its square until it is up
```
<!-- END generated -->

The `ladder` is cumulative: **no healing between rungs**. `sweep` is one rung
over twelve seeds - a distribution, not an anecdote.

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
- **A stale exe.** If the build failed, the script used to run against the last
  binary and report plausible numbers. It now refuses (exit 4, "STALE") - build
  and run it again.
