# Auditing the eval harness

Working notes, branch `harness-evaluation`, started 2026-08-15.

## Why

Michael, on the harness as merged:

> it drives with no, or very little, output, I have no idea if it's measuring
> anything useful or if it's just spinning and then confidently reporting
> meaningless results.

That is two complaints, and they need different answers:

- **Is it honest?** Does the world actually do what the script said, and do the
  printed numbers describe what happened?
- **Is it legible?** Even if every number is right, can a human read a run and
  tell a good one from a bad one?

This document is the catalogue. Findings are numbered and ranked at the bottom;
nothing here is fixed yet, by decision — the walk comes first so we don't
disappear down the first hole.

## The method

The harness measures the game. So the way to audit the harness is to **break
the thing it measures and check the number moves**. A measurement that does not
respond to any knob is either measuring nothing, or measuring something we
cannot name — and the two are indistinguishable from the report.

That is the same rule the rest of this project's checkers already run on
(`Bc7Test -SelfTest`, `AllocTest.ps1 -SelfTest`, `HealthTest.ps1 -SelfTest`,
and the damage-ledger mutation pass). The eval harness has a `-SelfTest`. What
it checks is the subject of F1.

Steps, bottom-up, because a defect in a lower layer voids every number above it:

1. The clock — does `step` advance what it says, is `seed` deterministic
2. The world commands — did `arena`/`spawn`/`forward` do what the line said
3. The tally — does `taken` equal the party's actual health drop
4. Responsiveness — for each printed number, what knob *must* move it
5. The report — do the `measure` regexes catch what the suite prints
6. The verdict — what should PASS refuse to cover

## Baseline

`.\tools\Eval.ps1 -Headless`, debug build, 2026-08-15, commit `13f5b80`.
Ten suites, 38 seconds, ten PASS, `failures=0`.

Log volume against report volume:

| suite | console lines | of which echoed commands | shown on report | shown % |
|---|---|---|---|---|
| smoke | 48 | 16 | 0 | 0 |
| arena | 84 | 35 | 0 | 0 |
| tiers | 143 | 59 | 20 | 14 |
| ladder | 196 | 86 | 16 | 8.2 |
| blast | 170 | 68 | 54 | 31.8 |
| sweep | 250 | 125 | 12 | 4.8 |
| resources | 198 | 59 | 71 | 35.9 |
| supplies | 161 | 57 | 63 | 39.1 |
| rest | 154 | 65 | 26 | 16.9 |
| expedition | 237 | 85 | 30 | 12.7 |
| **total** | **1641** | **655** | **292** | **17.8** |

Discounting echoed commands, which are not results, the report shows about 30%
of what the run produced. The question each finding below asks is whether the
70% it drops contains anything that would have changed the reading.

---

## Step 1 — the clock

### What passed

**Determinism holds, exactly.** Two consecutive runs of `ladder`, separate
processes: 196 console lines each, **zero** differing lines under
`Compare-Object`. The `seed` primitive does what it claims and the harness is
reproducible run to run. This is the foundation the rest sits on and it is
sound.

**No suite silently failed to simulate.** `step` refuses with `step: not
playing (state: X) — nothing stepped` when the app has left `Playing` — the
guard written after a party wipe dropped a run to the title screen and every
subsequent `step` quietly did nothing. Across the full ten-suite run that
message appears **0 times**. Likewise `warning: lockstep is OFF` appears 0
times. Both worth stating plainly: the two failure modes the harness was
already hardened against are not currently occurring.

### F3 — `step 3600` runs 3333.33s, and nothing says so (REAL BUG)

`Game::StepWorld` caps a single call at `kMaxSteps = 200000` ticks. At the
fixed 60 Hz tick that is **3333.33 simulated seconds — 55.6 minutes**.

Two suites ask for an hour:

- `supplies.eval` — section titled **"an hour of standing still"**
- `rest.eval` — section titled **"an hour of world time, for comparison"**

Both get 92.6% of an hour. Both label the result an hour. The supplies suite's
drain figures — the numbers the health-and-healing thread's "a load buys ~55
fights" conclusion rests on — are derived against a duration 4.5 minutes short
of the one written on them.

The irony is that the code anticipated this exactly. `StepWorld`'s comment:

> A ceiling per call, not per second: an eval script asking for an hour by
> mistake should come back and say how far it got

and `step`'s:

> Reports what it actually RAN rather than what was asked for [...] an eval
> that silently measured less time than it believes is worse than one that
> failed outright.

It does come back and say how far it got. **Nothing reads the answer.** See F4.

Measured across the full run: 51 `step` commands, 4 ran short. Two are the
ceiling above; the other two are legitimate early stops (`rest ended:
recovered`, `rest ended: attacked`) which are by design and correctly labelled.

### F4 — `stepped N ticks (X.XXs)` is printed by every step and shown by no report

None of the ten `measure` regexes in `Eval.ps1` match `stepped`. The one line
that would let a reader check *asked* against *ran* is written to
`dungeon.log` on all 51 occasions and filtered out of the report on all 51.

This is the mechanism behind F3, and it generalises: the harness's own
integrity signals are in the 70% the report drops. F3 was invisible not because
the harness failed to notice, but because the report layer discards the notice.

### F2 — the report cannot be captured

`Eval.ps1` writes every line with `Write-Host`, which goes to the host rather
than the success stream. So:

```
.\tools\Eval.ps1 -Headless > baseline.txt
```

produces an **empty file**. Confirmed by doing it.

The script's own header says the numbers exist so that "a knob change can be
diffed against a previous run". You cannot save a previous run. To diff two
runs today you must copy `dungeon.log` aside by hand and re-derive the report
from it — which is what this audit had to do.

### F10 — the report mojibakes its own em-dashes

`skel_warrior â€"`, `rested 38.77s â€" recovered`. UTF-8 bytes rendered through
CP1252. Cosmetic, but it is in the harness's primary output and it makes the
report unpasteable into a document.

---

## Step 2 — the world commands

Probes: `scratchpad/probe-commands.eval`, `probe-leak.eval`, `probe-bounds.eval`.
Each asks the world for things it cannot do, and watches who objects.

### What passed

**Unfinished `forward` steps do not leak across a rung.** `forward 20` into a
wall, ten seconds of stepping, then a fresh `arena` and 21 more seconds with no
`forward` issued at all: the party stayed at 14,21 throughout. Blocked steps are
consumed by `Party::Act`'s refusal, not queued for later. A ladder rung cannot
inherit the previous rung's walk. Worth recording because the opposite would
have been invisible and would have corrupted every rung after the first.

**Every refusal is at least reported.** `spawn`, `tp`, `blast` and `arena` all
print a specific message naming what they would not do. Nothing fails silently
at the command layer. The problem is entirely what happens to that message.

### F11 — a refused command counts as a PASS

`Game_Eval.cpp` fails a line on `!m_console.RunLine(line)`, and
`DevConsole.h`:

```cpp
bool RunLine(const std::string& line) { return Execute(line); }
// false = no such command
```

`false` means **no such command**. A command that ran and refused returns
`true`. So all of these are successes as far as the harness is concerned:

| line | printed | counted as |
|---|---|---|
| `spawn skeleton 1 1 south` | `spawn: refused 'skeleton' at 1,1` | PASS |
| `spawn definitely_not_a_monster 14 12 south` | `spawn: refused ...` | PASS |
| `tp 1 1` | `1,1 is not walkable` | PASS |
| `blast definitely_not_a_spell 14 12` | `blast: refused ...` | PASS |

A probe script consisting of nothing but these exits **0**.

`DevConsole.h`'s comment on that bool says it exists so a bad line does not
"leave the run reporting a clean pass over an encounter it never set up". It
catches typos in the *command name*. It does not catch a command that named a
real thing and declined to do it — which is the case that actually sets up
encounters.

### F12 — `forward N` reports the request, never the outcome

`forward` prints `forward xN` at the moment the steps are queued, before the
world has moved. The A/B, same party, same script:

```
face s ; pos -> 14,21 ; forward 5 ; step 10 ; pos -> 14,21     (wall: moved 0)
face n ; pos -> 14,21 ; forward 5 ; step 10 ; pos -> 14,16     (open: moved 5)
```

Both print `forward x5`. The output is **identical** whether the party crossed
five cells or none. The only way to tell is a `pos` before and after, which no
suite does.

This matters because the ladder's whole premise is the walk-in: "the party
WALKS in and engages". If a knob change — pace, monster speed, arena size —
ever stopped the party reaching contact, every rung would print a plausible
`TALLY dealt=0 taken=0` and PASS.

### F14 — the `arena` line's "floor" is a bounding box, not floor

`arena room 7 6` prints:

```
arena room 7x13  floor 11,9..17,21  centre 14,12
```

That range is 7 x 13 = **91 cells**. `mapinfo` immediately after counts **55
walkable** — a 7x7 room (49) plus a 6-cell corridor. **36 of the cells inside
the printed "floor" bounds are solid rock.**

The command's own comment states the opposite intent:

> Reported as the extent ACTUALLY carved rather than as the arguments

For `corridor` that is true. For `room` — the shape the ladder and expedition
suites use — it prints the bounding box of room-plus-corridor, and calls it
floor. `arena.eval`'s comment says the line exists "so a log reader can confirm
they were what was assumed", so this is precisely the reader it misleads.

### F15 — the three combine, and a rung measures a third of what it claims

The chain, run as one script (`probe-bounds.eval`), doing exactly what a script
author reading the log would do:

1. `arena room 7 6` reports `floor 11,9..17,21`.
2. Author places three monsters inside those bounds: 11,20 / 17,20 / 14,12.
3. Two are rock (**F14**), so two spawns are refused — and refusals pass (**F11**).
4. `monsters` shows one. Nothing on the report shows `monsters`.
5. The rung runs, prints its header `=== RUNG: 3x skel_swarm (as written) ===`
   and `TALLY dealt=20.7 taken=0.0 swings=2 hits=1 misses=1 slain=1 secs=40.0`.
6. **Exit code 0.**

A rung labelled three monsters measured one, produced an entirely plausible
tally, and reported PASS. This is the failure mode in Michael's words —
"confidently reporting meaningless results" — reproduced on demand in nine
lines.

Note also that the two readouts which would have caught it, `mapinfo` and
`monsters`, are printed and are in **no suite's `measure` regex**. Same shape
as F4: the harness saw everything and the report showed none of it.

---

## Step 3 — the tally

Where the numbers come from, established by reading the counting sites:

| field | counted at | covers |
|---|---|---|
| `dealt` | `MonsterTarget::Wound` | all post-mitigation damage to monsters, before the death check |
| `taken` | `PartyTarget::Wound` | all post-mitigation damage to members |
| `slain` | `MonsterTarget::Wound` | monster deaths |
| `downed` | `PartyTarget::Wound` | **fall events** — see F17 |
| `hits`/`misses`/`crits`/`fumbles` | `ResolveAttack` | **the party's melee swings only** |

Both damage counters sit in the effects pipeline rather than at the attack
sites, which is right and is what makes them catch DoTs, blasts and reprisals.
The dice counters do not — they are on one melee path.

Probes: `scratchpad/probe-tally.eval`, `probe-freeze.eval`, `probe-tiers.eval`,
`probe-heal.eval`.

### What passed

**The control is clean.** An empty room, party alone, 40 seconds: `TALLY
dealt=0.0 taken=0.0 swings=0 slain=0 downed=0 secs=40.0`. Nothing moves the
tally on its own.

**`timescale 0` genuinely freezes the world.** 120 ordinary frames with no
`step`, a live skeleton standing adjacent to the party: party health unchanged,
monster health unchanged, `secs=0.0`. I had expected this to fail — CLAUDE.md
notes the editor's pause skips `Update` entirely because "monster actions fire
off cooldowns not dt", which suggested a zeroed dt might not be enough. It is
enough. The one-line-per-frame pacing does not silently simulate time.

### F16 — `heal` leaves one member wounded when it also resumes from a wipe (REAL BUG)

`heal` does `c.health = c.maxHealth` for every member, unconditionally. It does
not always take:

```
after the wipe:   Brand 0.0/42  Sera 0.0/30  Maren 0.0/34  Tilo 0.0/24   (state menu)
heal ; party ->   Brand  42/42  Sera  30/30  Maren 22.4/34  Tilo  24/24  (state playing)
party      ->     ...unchanged, so it is not a display lag
heal ; party ->   Brand  42/42  Sera  30/30  Maren 34.0/34  Tilo  24/24
```

The **first** heal — the one that also runs `ClearWipeLatch()` and
`ResumeAfterHeal()` to bring the app back from the title screen — leaves member
2 at 22.4 of 34. A **second** heal, from `Playing`, restores her fully. So
`heal` is reliable from `Playing` and unreliable on the frame it resumes from a
wipe. Root cause not yet chased; that is fix-pass work.

**What it costs:** `tiers.eval` is built on exactly this line. Its comment says

> A fresh arena wipes the previous encounter, and the same seed makes the dice
> the same dice — so any difference is the seeding and nothing else.

That is not true as run. The veteran party enters its rung with one of four
members at 64% health, and the two tiers are therefore not being compared on
equal terms. This is the `23.4/35.0` visible in every baseline.

### F17 — `downed` counts fall events, not members

`if (m_fall != Fall::None) ++membersDowned;`. A member who falls unconscious and
is then killed produces two falls. Demonstrated: a bleed applied to **one**
member reports `downed=2`.

So `expedition`'s headline `downed=4` on rung 4 could be four members down, or
two members down and then killed. The report cannot distinguish them, and those
are very different outcomes for a difficulty reading.

### F18 — the `tiers` novice rung wipes to the title screen, every run

`state menu` immediately after rung 1's `step 30`, on every run. This is not a
flake and it is not wrong — a novice party losing to a skeleton is a legitimate
measurement. But:

- the report shows four `DOWN` lines and the word `PASS`, and nothing that says
  "this encounter ended the run";
- the suite is described as "the same encounter at two preset tiers", which
  reads as two comparable fights rather than one wipe and one near-wipe;
- recovery depends entirely on the `heal` on the next line, which is F16.

### F19 — the harness's exit code cannot see a party wipe

A probe script that killed its whole party, ended on the title screen, and
measured a final fight in which the party **never swung once**
(`dealt=0.0 swings=0`, both monsters at full health) exited **0**.

The relevant guard exists — `step` refuses when not `Playing` — but it fires
only on the *next* step. A wipe on the last measured encounter of a script is
completely invisible to the verdict.

### F20 — `monsters` truncates hp, and the blast table is built on it

`monsters` prints `hp 22`, `hp 9`, `hp 0 (dead)` — integers. The `blast` suite's
entire artefact is that table read against `dealt`, and with nine probes the
accumulated rounding is up to ±9 against a `dealt=71.2`. That is 12% of the
measurement, on the suite whose stated job is checking authored blast numbers
("open room 5 at the centre, 3.5 around; T-junction 30"). Those are one-decimal
claims being checked with integer instruments.

This also made it impossible for this audit to settle whether `dealt` includes
overkill — the truncation is wider than the effect. Deliberately left open
rather than guessed at.

### F21 — `taken` is party-wide, but `expedition` shows one member

`expedition`'s measure regex is `... |  \[0\] Brand|...`, so the report prints
Brand's health beside a `TALLY taken=` covering all four members. Rung 1 shows
Brand dropping 9.2 next to `taken=24.3`. Both numbers are correct and they
describe different populations, with nothing on the report saying so.

### A note on `effect` as a probe

`effect bleed 0 10 20` deals **200 damage** — magnitude 10 per second for 20
seconds — against a 42 hp member. Any script reaching for `effect` to wound a
party for a measurement will annihilate it instead. Not a defect, but the
argument order reads as "10 damage over 20 seconds" and means the opposite;
worth a line in the harness docs.

---

## Step 4 — responsiveness

The question this step exists for: **for each number the harness prints, move a
knob that must change it, and see whether it moves.**

Method: A/B arms, six seeds each, everything identical but one line. Offence
arms use a frozen `skel_warrior` at strength 50 (1100 hp) so nothing hits a
ceiling — a first attempt used strength 10 (220 hp), the target sometimes died,
and `dealt` was capped by the target rather than by the party. Per-swing damage
is reported alongside totals because a knob that changes swing *rate* would
otherwise masquerade as one that changes damage.

Probes: `scratchpad/probe-mutate.eval`, `probe-mutate2.eval`, `probe-overkill.eval`.

### The instruments are live

| number | knob moved | from | to | change |
|---|---|---|---|---|
| `hitrate` | DEX 5 -> 25 | 0.307 | 0.630 | **+105%** |
| `dealt` /swing | blade skill 0 -> 10 | 10.58 | 19.34 | **+83%** |
| `dealt` /swing | STR 8 -> 24 | 9.02 | 14.43 | **+60%** |
| `taken` | monster strength x1 -> x3 | 0.0 | 111.3 | **from nothing** |
| `downed` | monster strength x1 -> x3 | 0 | 12 | **from nothing** |

Every one moves in the right direction and by a magnitude that matches the
documented model — accuracy is DEX, damage scales with skill and with the
stat-damage term. **The tally is a live instrument, not a decoration.** That is
the direct answer to "is it measuring anything useful": on these five, yes,
demonstrably.

### `dealt` reconciles, and it counts overkill

Left open in step 3, now settled:

- **Survivor control:** a 110 hp target blasted in a dead end goes to 76 — a
  drop of 34 — and `dealt=33.5`. Agrees to within the integer truncation of
  `monsters` (F20).
- **Overkill:** the same blast on a **6 hp** target reports `dealt=15.3`.

So `dealt` is full post-mitigation damage delivered, including the part that
overshoots a kill, and it reconciles exactly against monster health when the
target survives. Well-defined and correct — it is simply nowhere written down,
so a reader comparing a blast table against `dealt` will find a discrepancy
they cannot explain.

### F22 — `echo` text is silently truncated at `;` or `#`

`ReadEvalLines` strips from the first `;` **or `#`**, so:

```
echo === if dealt is ~6 the counter CLAMPS; if ~17 it counts overkill ===
```

reaches the log as `=== if dealt is ~6 the counter CLAMPS`. It cost this audit
one wasted 36-encounter run — arm markers written as `##### ARM x #####` were
stripped to bare `echo`, and every sample came back unlabelled. A suite's own
section headers can lose their second half with no warning of any kind.

### F23 — the harness cannot tell an easy fight from no fight

The x1 arm of the monster-strength mutation ran six 20-second encounters
against a live, unfrozen `skel_warrior` standing adjacent to the party, and
produced: `dealt` 3.6, `taken` 0.0, **0.3 swings**, `downed` 0.

Over twenty seconds, next to a monster, the party swung roughly once in three
encounters and was never struck. Whatever the cause — aggro never triggering
after a `tp`, the monster not noticing — the output is a small, entirely
plausible tally that reads exactly like a trivially easy fight. Nothing
distinguishes it from one.

This is F12's problem one layer up: the harness measures the *outcome* of an
encounter without ever establishing that the encounter *joined*.

### The one number that did not respond — and it is not the harness's fault

Putting a weapon in every member's hands did **not** raise damage:

| arm | dealt/swing | swings | hitrate |
|---|---|---|---|
| bare hands | **11.86** | 55.3 | 0.563 |
| `serrated_blade` x4 | **10.58** | 60.0 | 0.527 |

Armed is **11% worse per swing** than unarmed. The equip was verified in the
same run (`char 0` reports `hand0 = serrated_blade`), so this is not a probe
that failed to take.

The explanation is visible in the skill arm above: the same blade at skill 10
deals 19.34 per swing. **The blade scales with a skill the party does not have,
and at skill 0 it lands below the unarmed knobs.** That is a coherent content
story, not a bug in the instrument.

Recording it here because it is exactly what the balance pass is for, and
deliberately not proposing a change to it — this harness reports, it does not
judge. It is also the strongest single endorsement of the harness in this
document: **the instrument worked, and the first thing it found when pointed at
the content was real.**

---

## Step 5 — the report layer

### What passed

**`-SelfTest` is green.** All five checks: bad script exits 1, missing script
exits 2, `reset` equals a new game (27 lines identical), batched equals solo
(14 lines identical), headless equals windowed (161 lines identical). Whatever
it checks, it checks correctly.

**`reset` is complete, beyond what the shipped check covers.** The shipped
baseline prints `char 0`, while the wrecking block modifies members 1, 2 and 3
(`effect bleed 1`, `setstat 2`, `setskill 3`, `give waterskin 1`, `learn 3
fire`). I widened the baseline to `pos` plus `char 0..3` and re-ran it: **33
lines, zero differences.** The recycling that the whole batch form rests on is
sound. This is a coverage observation about the check, not a defect in `reset`.

### F24 — the report cannot show a warning or an error at all

The measure filter is `^\[info \] console: `. Every other line in the log is
invisible to the report **by construction** — including all of `[warn ]` and
`[ERROR]`.

The ten-suite baseline contains **192** such lines. Stripped of stack frames,
the distinct messages are:

```
steady-state frame allocated 379 times (19215 bytes) - new call site(s):
steady-state frame allocated 202 times (4769 bytes)  - new call site(s):
steady-state frame allocated  81 times (2244 bytes)  - new call site(s):
steady-state frame allocated  77 times (2997 bytes)  - new call site(s):
steady-state frame allocated  34 times (1449 bytes)  - new call site(s):
steady-state frame allocated  33 times  (843 bytes)  - new call site(s):
No audio output device - running silent
```

Six steady-state allocation violations during an eval run, and the report says
`PASS` ten times. (The stacks name `PumpEvalScript`, `DevConsole::Execute` and
`log::Write`, so these are largely the harness's own console printing — the
documented "anything reporting from inside a guarded frame must excuse itself"
case. That makes them expected; it does not make them *invisible* a good idea.)

The general form is what matters: if the game logged a failed model load, a
missing catalog entry or an assert warning mid-suite, the report would show the
numbers and the word PASS and nothing else.

### F25 — three different situations render as the same empty box

The report prints a suite's header and then whatever matches its `measure`
regex. Nothing distinguishes:

1. **no measure defined** — `smoke` and `arena`, `measure = $null` (F5);
2. **a regex that matched nothing** — e.g. after someone edits an `echo`;
3. **a suite that genuinely printed nothing**.

All three produce a header followed by blank space. Since case 1 is *normal*
and visible on every run, a reader is trained to skim past exactly the shape
that case 2 would take. There is no guard: `Eval.ps1` never checks that a
non-null `measure` matched at least one line.

### F26 — the batch-equivalence check is far narrower than the headless one

Both exist for the same reason, and they are not the same strength:

| check | lines compared |
|---|---|
| headless vs windowed | **161** (every console line of the probe suite) |
| batched vs solo | **14** (`  [0] Brand` lines only) |

The batch check greps `^\[info \] console:   \[0\] Brand`, so it compares one
member's supply readouts. Contamination that reached members 1-3, the tally,
the map or the skills would pass it. Given that batching is the feature the
whole 155s -> 38s speedup rests on, it is checked on 8.7% of what headless is.

---

## Step 6 — the verdict

`PASS` means, precisely: **every line of every script matched a registered
command name, and the queue emptied without hitting the 600s timeout.**

That is a real and useful property — it catches typos, renamed commands, and
wedged runs, and `-SelfTest` proves it can fail. It is also very much less than
a reader assumes.

Evidenced in this audit, `PASS` does **not** mean:

| PASS survives... | shown by |
|---|---|
| a command that ran and refused | F11 |
| a monster that never spawned | F15 |
| a rung measuring one third of what its header claims | F15 |
| the party never reaching the fight | F12, F23 |
| an encounter in which nobody swung | F23 |
| `step` running 92.6% of the time asked | F3 |
| the party being wiped | F19 |
| the run ending on the title screen | F19 |
| six steady-state allocation violations | F24 |
| a suite whose measurement matched nothing | F25 |

Every one of those was reproduced, and in each case the harness had already
printed the evidence into `dungeon.log`.

### F9, resolved

`expedition` rung 4 ending with `rested 0.00s — still resting (hit the cap)`
and the party down is **not a defect** — it is the suite measuring the point
where the expedition fails, which is what it exists to do. The defect is that
it reads identically to a successful rung: same header shape, same TALLY line,
same `PASS`. It belongs to F19 and F25, not to itself.

---

## Findings

| # | Finding | Kind | Severity |
|---|---|---|---|
| F1 | `-SelfTest` checks the runner, never the measurement | honest | **high** |
| F3 | `step 3600` silently runs 3333.33s; two suites call it an hour | honest | **high** |
| F4 | `stepped N ticks` printed 51 times, shown 0 times | legible | **high** |
| F2 | Report is `Write-Host`, cannot be redirected or saved | legible | medium |
| F5 | `smoke` and `arena` print nothing at all | legible | medium |
| F6 | `tiers` wipes the novice party and reports PASS | honest | **high** |
| F7 | `tiers` veteran block starts already wounded — state leak | honest | **high** |
| F8 | `blast` prints `hitrate=0.000` when `swings=0` | legible | low |
| F9 | `expedition` rung 4 ends party-down, `rested 0.00s`, PASS | honest | **high** |
| F10 | Report mojibakes em-dashes | legible | low |
| F11 | A refused command (`spawn`/`tp`/`blast`) counts as a PASS | honest | **high** |
| F12 | `forward N` reports the request, never the outcome | honest | **high** |
| F14 | `arena` prints a bounding box labelled "floor"; 36 of 91 cells are rock | honest | medium |
| F15 | F14 + F11 together let a rung measure 1 of 3 monsters and PASS | honest | **high** |
| F16 | `heal` leaves a member wounded when it also resumes from a wipe | honest | **high** |
| F17 | `downed` counts fall events, not members | legible | medium |
| F18 | `tiers` novice rung wipes to the title screen every run, unflagged | legible | medium |
| F19 | Exit code cannot see a party wipe on the last encounter | honest | **high** |
| F20 | `monsters` truncates hp; the blast table is ±9 on a 71.2 reading | honest | medium |
| F21 | `taken` is party-wide; `expedition` shows one member beside it | legible | low |
| F22 | `echo` text silently truncated at `;` or `#` | legible | medium |
| F23 | Cannot tell an easy fight from no fight — contact is never established | honest | **high** |
| F24 | Report cannot show a `[warn ]` or `[ERROR]` line at all | honest | **high** |
| F25 | "No measure", "regex matched nothing" and "printed nothing" look identical | honest | **high** |
| F26 | Batch-equivalence check compares 14 lines; headless compares 161 | honest | medium |
| F27 | A `^` inside a measure regex can never match — `expedition`'s `^state ` never printed a line | legible | medium |

**Withdrawn:** F6 (`tiers` measures nothing after the wipe) — `tiers.eval` has
a `heal` on the next line with a comment explaining precisely this, and it
works. I recorded it too quickly from the baseline. The real defect in that
seam is F16, which is narrower and worse.

**Superseded:** F7 by F16 (its root cause). F9 by F19 + F25 — see step 6; the
expedition's failing rung is the suite working, not failing.

**Not a defect after testing:** `timescale 0` freezes completely;
unfinished `forward` steps do not leak; `reset` is complete under a widened
check; determinism is exact; `dealt` reconciles against monster health and
counts overkill; `-SelfTest` passes all five of its checks.

---

## What it would take to fix

Ranked by how much trust each buys per unit of work. Grouped because several
share one change.

### Tier 1 — connect the signals that already exist — **DONE**

These needed no new measurement. The game already printed all of it. Landed
2026-08-17; what each change did, and what it caught the first time it ran, is
recorded under "Tier 1, as built" below.

1. **Fail a script on a refusal, not just on an unknown command** (F11, F15).
   `RunLine` returns whether a command *matched*; it needs a second signal for
   whether it *succeeded*. Cheapest version: have `Print` calls on refusal
   paths route through a `Refused()` helper that the eval pump counts, same as
   `m_evalUnknown`. Kills the entire F15 chain.
2. **Show `stepped` when it ran short, and fail on it** (F3, F4). The line
   exists; compare it against the ask. Also fixes the "an hour" mislabel by
   making it impossible to miss.
3. **Surface `[warn ]` and `[ERROR]` on the report** (F24). One extra pass over
   the log section, unconditional, no per-suite regex.
4. **Fail a suite whose non-null `measure` matched zero lines** (F25). Four
   lines of PowerShell, and it makes silent measurement loss impossible.
5. **Assert the party is alive and `state playing` at the end of each script**
   (F19). One line per suite, or better, checked by the runner.

### Tier 2 — make the report readable — **DONE** (see "Tier 2, as built")

6. **Make the report capturable** (F2) — `Write-Output`, or a `-OutFile`. This
   is what unlocks diffing a knob change, which is the harness's stated purpose.
7. **Print a per-suite line count and what was suppressed** (F4, F25) — "142
   console lines, 20 shown" tells a reader there is a log worth opening.
8. **Give `smoke` and `arena` a measure** (F5) — `mapinfo` and `monsters` are
   the obvious ones, and they are precisely the geometry readouts F15 needs.
9. **Fix the mojibake** (F10) and the `hitrate` on zero swings (F8) —
   cosmetic, cheap, and both are in the primary output.

### Tier 3 — new checks (real work, real value)

10. **A responsiveness self-test** (F1) — the gap this whole audit turned on.
    Two arms, one knob, assert the number moved: `hitrate` under DEX is the
    cleanest (0.307 -> 0.630 is not a subtle signal). Without it the harness
    can go dead and report PASS forever.
11. **Establish contact before measuring an encounter** (F12, F23) — `forward`
    should report cells actually crossed, and a rung should be able to assert
    "the party is adjacent to a monster" before it starts the clock.
12. **Root-cause `heal` on the wipe-resume frame** (F16) — a real game bug, and
    it silently biases `tiers`.
13. **Widen the batch-equivalence check** (F26) to the same line set headless
    uses.

### Tier 4 — precision — **DONE** (see "Tier 4, as built")

14. **`monsters` should print hp to one decimal** (F20) so the blast suite can
    check one-decimal claims.
15. **`downed` should count members, not falls** (F17) — or be renamed.
16. **Document what `dealt` counts** (overkill included) and the `effect`
    argument order.

---

## The answer to the question

> I have no idea if it's measuring anything useful or if it's just spinning and
> then confidently reporting meaningless results.

**It is measuring, and the measurements are real.** Step 4 moved five knobs and
all five numbers responded correctly and proportionately; `dealt` reconciles
against monster health to within the display's own rounding; determinism is
exact; `reset`, headless and batching are all genuinely equivalent. The
instrument is sound, and the first thing it found when aimed at the content —
a blade that is worse than bare hands at skill 0 — is a real result.

**And it will report PASS over almost any failure of setup.** Not because it
fails to notice: it noticed every single defect in this document and wrote each
one into `dungeon.log`. The runner reads one bit of that (did the command name
exist?) and the report reads about 30% of the rest through per-suite regexes
that nobody wrote to include the integrity signals.

So the trust to place in it today is specific rather than general: **trust the
numbers, verify the setup.** A run's TALLY is honest about what happened. It is
not evidence that what happened was what the script asked for.

Tier 1 closes that gap, and it is mostly plumbing rather than new machinery.

---

## Tier 1, as built (2026-08-17)

Five changes. None adds a measurement; every one connects a signal the game was
already writing to the verdict or the report.

### 1. A refusal is a failure — `DevConsole::Refuse`

`RunLine`'s bool answers "was there a command by that name", which catches typos
and nothing else. `DevConsole` gains a second signal:

```cpp
void Refuse(std::string line);   // printed like any other line, and COUNTED
bool ConsumeRefusal();           // read once per line by the eval pump
```

Converted sites are the ones that change the world and can decline to: `Need()`
(one change covering every command's arity error), `tp`, `spawn`, `blast`,
`arena`, `effect ... ahead`, `step`, `forward`, `face`, `speed`, `timescale`,
and the whole `no such member` family.

**The rule for future sites**, which is the part worth keeping: `Refuse` means
"you asked me to change the world and I did not". A query with nothing to say
stays a `Print` — `monsters` printing `no monsters` is an ANSWER. The test is
whether a script that carried on regardless would be measuring something other
than what it wrote.

Deliberately left as `Print`: `cast` reporting a fizzle (a fizzle is a game
outcome, not a refusal), "that hand cannot swing now" (a cooldown), and the
editor/UI commands (`savemap`, `font`, `uitree`, `quality`) which no measurement
script drives.

### 2. `step` refuses a truncated call — `Game::StepStop`

`StepWorld` now says WHY it stopped rather than leaving a caller to subtract two
numbers: `Complete` / `Ceiling` / `LevelChange` / `RestEnded` / `NotPlaying`. A
`Ceiling` stop is a refusal quoting the shortfall, and the ceiling constants live
in `Game.h` so `step` can name them instead of restating them.

### 3. Warnings and errors reach the report

`Eval.ps1`'s filter only ever looked at `[info ] console:` lines, so `[warn ]`
and `[ERROR]` were invisible by construction. Now surfaced per suite,
deduplicated, capped at six, with stack frames dropped (they are indented under
their message, so requiring a non-space after the tag keeps the message and
drops the forty symbol lines beneath it).

### 4. A measure that matched nothing is a failure

New verdict `NOMEASURE`, plus the regex printed so it can be repaired. This
closes the case where editing an `echo` silently kills a suite's measurement and
leaves it looking exactly like `smoke` and `arena`, which legitimately print
nothing on every run.

### 5. A script must end in play

Checked in `PumpEvalScript`'s completion branch. The existing guard — `step`
refusing when not `Playing` — only ever fires on the NEXT step, and a script's
last encounter has no next step.

### What it caught on its first run

Three suites failed immediately, and all three failures were real:

| suite | what the new check said |
|---|---|
| `supplies` | `line 14 REFUSED: 'step 3600'` |
| `rest` | `line 33 REFUSED: 'step 3600'` |
| `expedition` | `ended in state 'menu', not playing` |

`rest.eval` turned out to have known about the ceiling all along — an inline
comment reading "so an hour arrives as 3333 seconds" — while still titling the
section "an hour of world time". `supplies.eval` did not know at all. **The same
fact was documented in one of the two files that needed it**, which is the
argument for a runtime check over a comment, made by the code itself.

### And one more, in the suite least likely to have it

`PipelineTest.ps1` drives the same runner, so it inherited the checks. It failed
too, on two refusals:

```
eval: line 73 REFUSED: 'tp 14 14'
eval: line 75 REFUSED: 'spawn skel_warrior 14 13 south'
```

`pipeline.eval`'s section 7 — over-exertion, the DECLARED exception to the
one-pipeline rule — has **no `arena` of its own**. It inherited section 6's
`arena corridor 11`, which is one cell tall at z=12, so 14,14 and 14,13 are
solid rock. The party never moved there and the monster never spawned. That
section has been swinging at empty air, from wherever the wall bump left the
party, for the life of the file.

It passed every time, and for a defensible reason: `PipelineTest` asks whether
each route MOVED HEALTH, and over-exertion bills stamina for a swing whether or
not it connects. Giving section 7 its own `arena open 9 9`:

| route | before | after |
|---|---|---|
| `exertion` | -42.96 | **-42.96** |
| `pipeline` | -226.36 | **-248.36** |

`exertion` unchanged to the penny — confirming the swings always billed — and
`pipeline` up by 22, which is the monster that was never there trading blows.

This is the audit's thesis restated by a third party: a suite can check its own
invariant rigorously and still never ask whether the setup was what the script
said.

### The corrected numbers

Both hour-long steps became two calls of 1800s. The drain figures moved by the
ratio that predicts:

| reading | before (3333.3s) | after (3600.0s) | change |
|---|---|---|---|
| supplies, Brand water drop | 29.0 | 31.3 | +7.9% |
| supplies, Sera food drop | 12.2 | 13.2 | +8.2% |
| rest, Brand food drop | 12.2 | 13.2 | +8.2% |

Duration correction is +8.0%. The measurements moved with it, which is the
check that the fix was a correction rather than a change.

`expedition` gains a `heal` before its closing section, with the reasoning at
the call site: the party dying at rung 4 IS the suite's answer, and what was
wrong was reading the closing figures off the corpse. `heal` restores health,
stamina and mana only — food and water are untouched, so the supply bill is
still the real one, and the wipe stays on the record in rung 4's `party` line
and its `downed=4`.

### Still open after Tier 1

The six steady-state allocation warnings are now VISIBLE on every run rather
than fixed — they come from the harness's own console printing inside a guarded
frame, which is the documented "must excuse itself" case. Visible and expected
beats invisible; whether to excuse them is a separate question from this audit.

All four tiers are done; each is written up below.

---

## Tier 3 item 10 — the responsiveness self-test (2026-08-17)

The gap the whole audit turned on (F1). Everything in `-SelfTest` was plumbing:
a bad script exits 1, a missing one exits 2, `reset` equals a new game, batching
and headless change nothing. **Not one of them asked whether the tally still
describes the fight**, so a counting site could be deleted and the harness would
report ten green suites forever.

`tools/EvalScripts/respond.eval` runs three pairs of arms, each identical but
for one knob the combat model says must move one of the headline numbers. It is
run by `-SelfTest` and is deliberately NOT a suite — nothing it prints is a
number to compare across knob changes, the same argument `resettest.eval` makes.

| number | knob | counting site | measured |
|---|---|---|---|
| `hitrate` | DEX 5 -> 25 | `ResolveAttack` | 0.336 -> 0.668 (2.0x) |
| `dealt`/swing | blade skill 0 -> 10 | `MonsterTarget::Wound` | 11.02 -> 17.84 (1.6x) |
| `taken` | monster x2 -> x4 | `PartyTarget::Wound` | 19.2 -> 234.4 (12x) |
| `downed` | monster x2 -> x4 | `PartyTarget::Wound` | 2 -> 17 |

Three design decisions worth keeping:

- **The encounter is a shared fragment** (`rungs/respond-offence.eval`) so the
  two arms of a pair cannot drift apart. The defence pair is the exception and
  needs two files, because its knob is on the MONSTER and the monster is spawned
  inside the encounter; both files say so at the top.
- **`dealt` is measured PER SWING.** A knob that changed the swing rate would
  otherwise masquerade as one that changed damage.
- **The offence target is a frozen 1100 hp punchbag.** A first attempt used 220
  hp, it sometimes died, and both arms converged because they were measuring the
  monster rather than the party.

**The knob that was tried and rejected:** armour for the `taken` pair, which
would have shared the offence fragment and read better. Measured, `plate_cuirass`
+ `avoid 8` moved `taken` by 17% against an x3 monster, and at x1.5 and x2 the
effect was smaller than the sample noise and sometimes inverted — soak is flat,
so it matters least against exactly the heavy blows a reliable check needs. A
subtle knob makes a flaky check.

### Non-vacuous by mutation

A check that cannot fail is worth nothing, so each counting site was broken in
turn and the suite re-run. **Four mutations, four failures, each exactly the
right one and no collateral:**

| mutation | hitrate | dealt/swing | taken | downed |
|---|---|---|---|---|
| `tally.dealt += 1.0f` (constant) | ok | **FAIL** 17.44 vs 16.41 | ok | ok |
| `tally.taken += 0.0f` (dead) | ok | ok | **FAIL** 0.00 vs 0.00 | ok |
| `++tally.hits` unconditionally | **FAIL** 1.00 vs 1.00 | ok | ok | ok |
| `if (false) ++membersDowned` | ok | ok | ok | **FAIL** 0.00 |

The second and fourth are the interesting pair: `taken` and `membersDowned` are
incremented two lines apart in the same function, and killing one leaves the
other's check green. The four rows are genuinely independent.

`DungeonWorld_Combat.cpp` was confirmed byte-identical to HEAD afterwards and
all four rows returned to ok.

---

## Tier 2, as built (2026-08-17)

### 6. The report can be saved — `-OutFile`

Every report line now goes through one `Say` helper that writes to the host
(keeping the colour that makes a FAIL findable in 300 lines of numbers) **and**
appends to a buffer that `-OutFile` writes out. `Write-Output` alone would have
lost the colour; `Write-Host` alone was F2.

The workflow this exists for, verified end to end:

```
.\tools\Eval.ps1 -Headless -Table -OutFile before.txt
...edit balance.cat...
.\tools\Eval.ps1 -Headless -Table -OutFile after.txt
Compare-Object (gc before.txt) (gc after.txt)
```

Two identical runs diff to **exactly one line** — `seconds=`, which changes run
to run on the same build and is documented at the call site as the line a
reader should expect to differ.

Written with `[IO.File]::WriteAllLines`, not `Set-Content`: PS 5.1's
`-Encoding utf8` prepends a BOM, and a BOM makes the first line of every diff
spurious. Verified — the file starts `13,10`, not `239,187,191`.

### 7. Every suite says how much it suppressed

`[16 of 110 lines shown; the rest is in dungeon.log]`. Across the ten suites the
report shows roughly 30% of what the run produced, and that is where every
integrity signal in this audit was hiding. One line per suite is what tells a
reader there is a log worth opening.

### 8. `smoke` and `arena` measure something

Both had plenty to show and nobody had said so. `arena` now calls `mapinfo`
after every carve, and **that immediately put F14 on the report**:

| arena | printed "floor" bounds | actually walkable |
|---|---|---|
| `open 9x9` | 81 cells | 81 |
| `corridor 11x1` | 11 | 11 |
| `deadend 7x1` | 7 | 7 |
| `tjunction 9x5` | 45 | **13** |

So the bounding-box-labelled-floor problem is not confined to `room`: the
t-junction names 45 cells as floor and carves 13.

### 9. The two cosmetic defects

`Get-Content -Encoding UTF8` (via one `ReadLog` helper) fixes the mojibake at
the source — PS 5.1 defaults to the ANSI code page and the game writes UTF-8, so
every em-dash was arriving as three characters. This fixes the DATA; how a
console then draws it is the console's code page and not this script's business.

`hitrate=n/a` replaces `hitrate=0.000` when nothing swung. A rate over no trials
is undefined, not zero, and `0.000` made "never swung" identical to "missed
every time" — which is the pair a blast table (`swings=0` by nature) sits right
next to. Parsers should read `[0-9.]+|n/a`; the responsiveness check excludes
such samples from its average rather than counting them as zero.

### F27 — a `^` inside a measure can never match (found while doing Tier 2)

The filter is `^\[info \] console: (<measure>)`, so a caret inside the
alternation asserts start-of-STRING in the middle of the pattern.

**`expedition` has carried `|^state ` since the day it was written and has never
printed one `state` line.** With it fixed, the very first run shows `state menu`
immediately after rung 4 — the wipe that F19 was about, now visible on the report
instead of only in the verdict.

The NOMEASURE check from Tier 1 cannot catch this, and that is worth
understanding: the suite's *other* alternatives still match, so the measure is
not empty — only one branch of it is dead. A per-alternative check would be the
fix; a comment at the table saying "the line is already anchored, just write the
text" is what is there instead.

---

## Tier 4, as built (2026-08-17)

### 14. `monsters` prints hp to one decimal

The blast suite's probes ARE its instrument, and `spells.cat` states the numbers
being checked to one decimal ("3.5 around, 17.5 at the feet"). They were being
read with `{:.0f}`.

The open-room case is the one that should reconcile exactly, since nothing dies
there and so no overkill is in play:

| | before | after |
|---|---|---|
| nine probes summed | 125 (as displayed) | **127.0** |
| implied drop from 198 | 73, ±9 | **71.0** |
| `dealt` | 71.2 | 71.2 |
| disagreement | up to 9 (12%) | **0.2** |

Four of the nine probes were sitting at `10.5` and displaying as `10`. The
other three geometries still differ from `dealt` — by 3.4, 13.3 and 13.4 — and
that is now readable as exactly what it is: overkill on the 1, 2 and 1 probes
that died in each.

### 15. `downed` counts members, not falls

`Tally::downedMask`, one bit per roster slot, cleared with the rest of the
struct by `tally reset`. A member who drops unconscious and is then killed sets
the same bit twice and counts once.

Demonstrated: one member bled to death reported `downed=2` before and reports
`downed=1` now; the whole party reports `downed=4`, which is now bounded by the
roster size and therefore interpretable at a glance.

The roster index comes from `DungeonWorld::MemberIndex` — pointer arithmetic
into the one `std::vector<Character>` both sides already share, so it needs no
extra bookkeeping and cannot drift from the roster's order. No container, no
allocation: the steady-state rule applies to this code path like any other.

### 16. What the fields mean, written down

At the `tally` print site, because two of them were guessed wrong by the audit
that checked them: `dealt` includes overkill, `taken` is party-wide, `swings` is
melee only, `downed` is members. And `effect`'s usage now says
`[magnitude, PER SECOND for a DoT] [seconds]` — the argument order reads as "10
damage over 20 seconds" and means 200.

### What did not change, and why that is the answer

The ten suites' numbers are otherwise identical across this change, and the
`downed` figures in the current run happen to be the same because no member fell
twice in them. That is the correct outcome: Tier 4 is a precision and naming
pass, not a behaviour change. The probe is what proves the semantics moved.

### The pattern, three steps in

Every high finding so far has the same shape, and it is not "the harness is
careless" — it is nearly the opposite:

1. The game **notices** the problem and prints a specific, well-worded message.
2. A comment near that message explains why noticing matters.
3. Nothing **reads** the message: the runner only fails on unknown commands
   (F11), and the report only shows lines matching a per-suite regex that
   nobody wrote to include the integrity signals (F4, F15).

`step` says how far it really got, and no regex matches `stepped` (F3/F4).
`spawn` says it refused, and `RunLine` returns true anyway (F11). `mapinfo`
counts the walkable cells specifically so a wrecked map is visible, and no
suite shows it (F15). `forward` is the one exception — it genuinely does not
know its own outcome (F12).

So the fix is unlikely to be "make the harness check more things". It is
mostly **connecting signals that already exist to the verdict and the
report**.

### What passed, in one place

Not everything is broken, and a list of only defects would misrepresent the
harness:

- Determinism is exact — two processes, 196 lines, zero differences.
- `step` refuses to run when not `Playing`, and that never fired across the
  full suite (0 occurrences). The party-wipe-to-title-screen failure is real,
  guarded, and not currently happening.
- `lockstep is OFF` never fired either.
- Unfinished `forward` steps do not leak across an arena rebuild.
- Every refusal is at least *printed*, specifically and by name.
- The world recycling (`reset`) and headless equivalence are both checked by
  `-SelfTest` and both pass.
