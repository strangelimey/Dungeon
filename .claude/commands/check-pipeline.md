---
description: Every source of damage goes through fx::Deal — nothing else writes health
argument-hint: "[selftest]"
allowed-tools: PowerShell, Read, Grep, Glob
---

Drive every source of damage in the game and verify none of them went around the
pipeline (~16 s).

- no argument → `.\tools\CheckAll.ps1 -Only pipeline`
- `selftest` → `.\tools\CheckAll.ps1 -Only pipeline -SelfTest` (must FAIL)

## What it is guarding

docs/effects.md's invariant: **outside the two `fx::ITarget` adapters, nothing
writes health.** Every blow, bolt, blast, DoT tick, ward reprisal, collision and
smashed barrel builds an `fx::DamageEvent` and goes through `fx::Deal`, which is
what makes resists, soak, absorption and wards work everywhere for free instead
of at each of a dozen call sites.

That was verified once, by hand, in July 2026. `Game/DamageLedger.h` is the
machinery that makes it a standing rule: every watched health value carries a
baseline, anything allowed to move it says so, and at each of four checkpoints a
frame the arithmetic has to come out.

**Why it is a runtime check and not a grep.** The obvious cheap version scans the
source for writes to `health`. It was tried against the code as it stands and it
does not work — resource regeneration writes health through a lambda taking
`float&`, so the identifier never appears on the assignment and a scan reports
that file clean.

## Reading a failure

`one-pipeline violation: <who> health moved <x> during "<phase>" ...` — in the
console and in `dungeon.log`. It names the **phase**, not the line: a violation
is found at a checkpoint, by which time the stack that caused it is gone. The
four phases are `outside the world update` / `party movement` / `monsters,
effects and regeneration` / `projectiles and blasts`, and the victim plus the
amount has always been enough to find it from there.

Each `(phase, subject)` is reported once per session, so a standing violation
cannot drown the log.

**If the new write is legitimate**, it needs one of two things, and which one is
a real decision:

- a **sanction** — `ledger::Explained` naming a `Reason` — if it is a rule of the
  game that health moves this way (regeneration, growth, the self-stabilize
  wake, over-exertion). The reason then shows as its own row in the readout.
- a **rebase** — `RebaseDamageLedger()` — if it *replaces* state wholesale (a
  load, a save restore, a respawn, `heal`). There is no route to name there; the
  values it overwrote no longer exist to be reconciled.

## Why the harness demands more than PASS

The suite's first run wiped the party on a T-junction blast, dropped the app to
the title screen, and printed a confident `RESULT=PASS` for four more sections
while nothing simulated at all. So `PipelineTest.ps1` also requires that the app
is still `playing` at the end and that **every sanctioned route moved a non-zero
amount of health**. A zero in the `exertion` row is a section that stopped
working, not a rule that held.

The self-test runs `pipelinepoke`, which moves health with no `DamageEvent`
anywhere near it, and requires the run to come back FAIL.

## Related

`/check-eval` runs the ten **measurement** suites, which have no notion of a
right answer and deliberately sit outside these tiers. This one guards a rule,
so it is in the quick tier with the others.
