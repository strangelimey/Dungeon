# The eval harness

**Status:** P1–P5 and P7 built (2026-08-13). A rung can be built, seeded, fought and
measured over N seeds, and the blast geometries are measured. What remains is
the PROGRESSION LADDER (P6, blocked below) and the outer runner (P8).

**OPEN DESIGN QUESTION THAT SHAPES P6:** the game has no healing source at all
beyond unconscious self-stabilize. `heal` is a HARNESS FIAT — it restores to full
because a rung needs some starting state, not because a player could. So a ladder
that heals between rungs measures a dungeon nobody can actually run, and "how does
the party arrive at the next fight" is a design call that comes before the ladder
means anything. It is also why the tally records absolute damage rather than
fractions of health: those numbers survive whatever the answer turns out to be.

The damage system has a great many knobs (`balance.cat` alone is fifty-odd) and
almost none of them have been played against. Tuning them by hand means fighting
one encounter, changing a number, and fighting it again — which measures one
lucky afternoon. This is the machinery for measuring them properly instead.

**It is MEASUREMENT, not pass/fail.** The artefact is a table compared across
knob changes, the way `Bc7Test --audit` and the `fireburst` tuning matrix were;
assertions are a thin regression layer on top. The project has already learned
this the expensive way — on the area-blast work, *both bugs were invisible in
the pass/fail checks and obvious in the table.*

## Why the clock had to come first

Three facts about the engine decided the whole design.

**The sim is already dt-driven.** `Game::Update` computes `wdt = dt *
m_timeScale` and `m_time` accumulates from that, not from wall-clock. So
advancing thirty seconds without waiting thirty seconds was always possible.

**The combat RNG had exactly one seed** — `std::mt19937 m_combatRng{0xC0FFEEu}`,
never re-seeded. Every run of the game rolled the identical sequence: perfectly
reproducible, and a single sample forever. Nothing about a 5% fumble or a 6%
critical is visible in one fight.

**Monster AI is paced by WALL-CLOCK.** `ai::AsyncDirector` runs one worker per IQ
bucket at prime millisecond cadences (251/499/997/1999). Simulate thirty seconds
inside a few frames and the monsters think perhaps twice.

That last one is the reason this phase exists, and its failure mode is worse than
it sounds. **Monsters do not freeze — they follow STALE ORDERS.** Execution is
per-frame and unbucketed, so a monster with a cached path keeps walking it
perfectly while never re-planning. Measured: with lockstep off, an eight-second
step still moved a skeleton four cells toward where the party *used to be*. A
harness built on that would produce confident numbers for fights that never
happened, and nothing about the output would look wrong.

## What P1 added

### `lockstep on|off`

Pauses the four bucket workers and drives `AsyncDirector::ComputeInline` from the
host at the same cadences counted in **sim** time (`DungeonWorld::TickLockstepAI`).
It calls the very same `ComputeBucket` the worker calls — not a reimplementation
— so the two modes cannot drift apart in *what* they decide, only in when.

**Be honest about what this is.** It is not a bit-exact reproduction of async
mode and cannot be: async plan latency depends on thread scheduling, so "the same
as async" is not a well-defined target. Lockstep is the *reproducible* version —
the same decisions at the same sim cadence, with the wall-clock jitter removed.

One think per bucket per frame, with the remainder **carried** so the long-run
rate is exactly the bucket's cadence. Deliberately not a catch-up loop: thinking
twice against one frame's world yields two identical plans, because a monster
does not *move* until its executor runs later in that same update.

### `step <seconds>`

Advances the world by sim seconds, now, in **fixed 60Hz sub-steps** — no frames
presented, no input read.

Fixed sub-steps are load-bearing. Nearly everything that paces this game counts
*down* by dt — hand cooldowns, monster attack and move cooldowns, stamina
holdoff, the AI bucket clocks — so a single 30-second dt would let a monster take
one step and swing once. That is also why lockstep needs no catch-up loop: `step`
guarantees the small dt it assumes.

It reports what it **ran**, not what was asked for, and says *why* when it ran
nothing. A silent zero is the worst possible output here.

### `seed <n>`

Reseeds the combat RNG. Every roll comes off this one stream, so this is what
turns a rung into a sample rather than an anecdote.

### `logecho on`, `state`, `party`

The console answers in a *window*, and reading a window means a screenshot — of
whatever happens to be in front of it. `logecho` mirrors every console line into
`dungeon.log`, which is what makes the existing command surface drivable from a
script at all. `party` prints the party's side of an encounter (the other side
has been `monsters` for a while).

`state` exists because of a trap that has now cost two debugging sessions: **dev
commands reach the world from the MENU**, since the world is built at load. So
after a party wipe, `tp` and `monsters` answer perfectly normally while nothing
is being simulated at all. A harness that could not see this would report a whole
suite of encounters that never ran.

## P2: the game runs its own script

```
Dungeon.exe -eval tools\EvalScripts\smoke.eval
```

A script is console commands, one per line, `;` or `#` to comment. The game
queues it and runs **one line per frame**, then emits a verdict and exits with
it:

```
eval RESULT=PASS script=smoke.eval lines=16 unknown=0
```

Exit codes are the harness's actual interface: **0** every line matched a command
and the queue emptied · **1** a line matched nothing, or the run timed out · **2**
the script could not be read at all. That last one is deliberately distinct — a
"test run" that silently sat at the title screen would report whatever the
harness assumed rather than what happened.

**Waiting is free and there is no `wait` directive.** While a staged load is in
flight the console's commands are disabled, and the pump respects the same gate,
so `newgame` is simply followed by the next line once the world actually exists.
Nothing to tune, and nothing that can be tuned wrong.

**One line per frame**, because a command that changes state — `newgame`, a level
transition, a quality swap — needs its frame to land before the next line reasons
about the result.

`logecho` is forced ON for a scripted run rather than left to the script: a run
whose author forgot the line would leave no readable record of itself, which is
the one outcome a harness must not have.

### It is checked against failure, like everything else here

`tools/EvalScripts/selftest-bad.eval` contains one line that is not a command. It
must exit **1**; if it passes, the runner's verdict means nothing. A missing
script file must exit **2**. Both verified.

### Determinism, measured

The same script run twice produces **byte-identical** console output (48 lines).
That is the promise P1 made and could not yet test, and it is what makes a knob
change show up as an exact diff rather than as noise — the same technique that
proved the `Defense.h` refactor behaviour-preserving.

### What the first unattended run found

It crashed the process. `newgame` called `StartNewGame()` directly, but from a
cold boot `m_gameLoaded` is false and **the HUD has never been built — it is a
load task** — so setting `AppState::Playing` left the same frame's state machine
dereferencing a HUD with no widgets. The menu entry had always handled this;
`onStartNewGame` is where the "already loaded, or load first?" decision lives.
The command now calls that callback instead of reimplementing it.

*A dev command that duplicates a UI action will drift from it, and this one
drifted immediately.* Worth noting the diagnostics work paid for itself here: the
fault filter caught it, symbolized `GameUI::SetHudStatus`, and wrote a minidump,
so a crash in an unattended headless-ish run was a two-minute diagnosis.

## P3: the arena

```
arena <open|corridor|deadend|tjunction> [w] [h]
spawn <type> <x> <z> [n|e|s|w]
```

Every attempt to verify P1 against the **showcase** level fought back: a monster
already standing adjacent so it had nothing to walk toward, a target cell that
turned out not to be walkable, one creature that would not engage in either AI
mode. None of those are bugs — they are a hand-authored level being
hand-authored — but they make it impossible to say what a measurement measured.
An arena is a room whose entire contents you chose.

**It writes no files.** The editor's new-level button authors a `.map`/`.ent`
pair and appends the project manifest, which in a dev build lands straight in the
git tree (`paths::Asset` *is* the source tree). An eval that dirtied the working
copy every run would be intolerable, so `arena` carves into the **loaded** map
and empties the world into it. Nothing persists unless someone types `savemap`,
which a script never does.

The shapes are the geometries a propagating blast has to be measured in: an open
room lets a wavefront ring outward; a corridor reflects it off both ends; a dead
end reflects it back onto the caster; a **T-junction** is where arms converge and
multiply, which is where the worst number in the game lives.

The arena is **centred on the map**, so its cells are derivable from the map size
and a script can hardcode them — a script cannot read a command's answer. The
command prints the extent **actually carved** (not the arguments) so a log reader
can confirm they were the cells assumed. A corridor ignores `h`; echoing the
request would have had `arena corridor 11` claim an 11×11 room in the one record
anybody reads.

`deadend` is not a different shape from `corridor` — it is a different place to
stand in one, so its reported centre is the **closed end** rather than the middle.

### His blast case, in nine lines

```
arena open 9 9
spawn skel_swarm 13 11 south      ; ...and the other eight, 13..15 x 11..13
```

### What it finally proved

A skeleton spawned at 14,8 **pathfound across an empty room to 14,11** to reach
the party at 14,12, inside a single `step 30`, and took three of the four members
down. That is lockstep AI demonstrated end to end — a monster that had to think,
path and close the distance — which the showcase level had prevented four times
over.

## P4: seeding a rung

```
setstat  <member> <str|dex|vit|wil|int> <n>
setskill <member> <skill id> <level>
heal     [member]
char     <member>
include  <path>          ; script-level, resolved against the root script's folder
```

A single run cannot play from fresh characters to end-game, so a rung has to
**start** where it wants to measure.

`setstat` calls `RecomputePartyMaxima` afterwards, because health, stamina and
mana all **derive** from stats — a stat set without it leaves a level-20 fighter
with a novice's hit points and every number after it measured wrong.

`setskill` squares what it is given, since levels derive from `floor(sqrt(xp))`.
Squaring is the honest inverse, so a seeded skill trains onward from exactly
where a played one would have.

### A preset is a script fragment, not a table

`include` is handled by the runner rather than as a console command — it edits
the queue the pump is walking, which a command handler has no business reaching
into. Included lines are **spliced in place**, so they are indistinguishable from
inline ones: same one-per-frame pacing, same load gate, same counting. Nesting
works for free.

That makes a preset a file (`tools/EvalScripts/presets/*.eval`) instead of a
hardcoded loadout in C++ — editable, diffable and composable like the rest of the
suite. `presets/novice.eval` deliberately sets almost nothing: a fresh roster *is*
the novice tier, and writing its numbers out would create a second definition
that drifts from `CreateDefaultParty` the first time that changes.

### `heal`, and why a ladder cannot run without it

**A wipe returns to the title screen.** So the first rung that wipes ends the run
— every later `step` is correctly refused and every later rung measures nothing.
The first two-tier script did exactly that: rung 2 never ran, and only the
`state` guard made it obvious rather than silently plausible.

`heal` restores in place (a `newgame` per rung would cost a full staged reload
each time) and deliberately does **not** touch stats, skills, gear or stance —
those are what the preset seeded, and a heal that undid them would make rung 2
measure rung 1's party.

### What two tiers look like

Same seed, same room, same monster, thirty seconds:

| | novice | veteran |
|---|---|---|
| party after | all four **DOWN** | all four **untouched** |

### The gap this exposed: the party does not fight back

The skeleton finishes on 16 hp in **both** runs. Nothing swings for the party —
`PartyAttack` is driven by a hand-slot click or the `swing` command, so a rung as
written measures the party's **defence** and nothing else. A real two-way rung
needs the party attacking on its own; that is P5's problem, and it changes what
every ladder number means.

## P5: measuring an encounter

```
autoattack on|off
tally [reset]
sweep <count> <script>      ; runner-level
```

### The party has to fight back

`PartyAttack` is driven by a hand-slot click or `swing`, so before this a
measured encounter was **the party standing still being hit** — and it did not
look wrong. `autoattack` swings every hand that is off cooldown, and **turns the
party to face an adjacent monster**: a rung should not have to predict which side
something will approach from, and the first sweep measured twelve fights in which
the party stared at a wall while a skeleton chewed on them from behind.

It swings only when something is actually adjacent. A whiff at air costs the
attack's pace *and* its full stamina bill (`PartyAttack` pays both before it
looks for a target), so a party auto-swinging into an empty corridor would arrive
at the fight exhausted and every number after would describe an exhausted party.

### The tally is counted in the pipeline, not at the attack sites

`dealt` and `taken` are counted inside `PartyTarget::Wound` and
`MonsterTarget::Wound` — the two `fx::ITarget` adapters. **Every** source of
damage goes through the one pipeline (`docs/effects.md`), so a blast, a DoT tick,
a ward's reprisal and an ordinary sword blow are all caught by the same two
lines. A tally hung off the attack sites would have missed four of those five.

Damage is recorded in **absolute points**, never as a fraction of health: the
healing model is still to be designed, and fractions would silently change
meaning the day it lands.

Output is one grep-able key=value line per rung:

```
TALLY dealt=19.9 taken=0.0 swings=2 hits=1 misses=1 hitrate=0.500 crits=0
      fumbles=0 slain=1 downed=0 secs=45.0
```

### `sweep` — one rung, N seeds, a distribution

`sweep 12 rungs/novice-1skeleton.eval` splices the rung twelve times, each
preceded by its own `seed` (1..count, so two sweeps are comparable
sample-for-sample). Combat is dice: one encounter is an anecdote, and a 5% fumble
cannot show up honestly in a single fight.

A **rung** assumes nothing about what ran before — `heal`, fresh arena, fresh
spawn, `tally reset` — and deliberately does *not* set its own seed; the sweep
owns that, or a distribution silently becomes one number repeated N times.

**Include paths are always relative to the ROOT script's folder**, never to the
including file's. The queue is spliced flat, so by the time a line runs there is
no "including file" left to be relative to.

### What the first working sweep found

Twelve seeds, novice party, one skeleton, 45 seconds:

| | result |
|---|---|
| skeleton slain | **12 / 12** |
| party members downed | **0** |
| damage taken | 0 in eleven runs, 9.6 in one |
| swings to kill | 1–9, hit rate 0.22–1.00 |

**This inverts P4's tier comparison entirely.** That earlier "novice: all four
DOWN" was the party *never swinging back* — a defence-only measurement that made
the game look far harder than it is. The same encounter, fought properly, is a
comfortable win. Nothing about the earlier output looked wrong, which is the
whole argument for the tally and the auto-attack existing before any ladder does.

### Two latches a harness has to know about

Neither is a bug; both are game rules that a *repeated* encounter runs into and a
played one never does.

- **`m_partyWiped` gates every monster attack** and only `ResetForNewGame` clears
  it. So a ladder that healed between rungs ran rung 2 onward against monsters
  that had permanently stopped swinging — and reported forty-five simulated
  seconds of nothing, twelve times, as a result. `heal` now clears it.
- **A wipe returns to the title screen** (see P4's `heal`).

## P7: the blast geometries

```
blast  <spell id> <x> <z>
freeze on|off
```

`blast` detonates a spell's **authored** rules at a cell — no caster, no mana, no
skill roll, no bolt flight. It reads the spell's own `BlastSpec` and procs, so
what a measurement describes is the content that ships rather than numbers the
harness invented. It refuses a spell with no `blast_force` rather than detonating
a nothing, because "not an area effect" and "reached nobody" produce the same
empty table and mean opposite things.

**The monsters are the instrument.** Each is a fixed-hp probe parked on a known
cell, so the hp left after the blast reads the falloff and the convergence
straight off. `freeze` is what makes that honest: the first run had two of nine
warriors walk out of the squares being measured and then maul the party, so the
table described where they ended up rather than what the blast did to where they
were. Frozen monsters still burn, still take the blast and still die — they
simply do not act.

A blast plays out over **ticks**, so a script must `step` after detonating;
reading `monsters` in the same breath measures the moment before it went off.

### The table

`fireburst` (`blast_force = 7`), `skel_warrior` probes at 22 hp:

| geometry | detonation cell | ring | outer | total dealt |
|---|---|---|---|---|
| open 9×9 | 13 | 12 (orthogonal only) | — | 71.2 |
| corridor 11 | **dead** | 19 | 14 | 90.4 |
| dead end 7 (at the closed end) | **dead** | **dead** | 14 | 71.3 |
| T-junction | **dead** | 15 (arms) | 10 (stem) | 76.9 |

This **reproduces the tuning** recorded in `spells.cat` — open is mild, the dead
end and the junction are lethal — but it does so in the real world, with real
monsters, through the whole effects pipeline (resists, soak, on-hit procs),
where the original tuning ran a throwaway harness over pure geometry.

Two things visible in the data rather than asserted by a comment: the open case
leaves the **diagonals untouched**, which is the 4-cardinal propagation rule; and
the seven squares it does fill are exactly the authored `blast_force`.

## Driving it: two rules learned the hard way

**Set `timescale 0` first.** Otherwise real time passes between console commands
and contaminates the measurement — an early A/B "showed" monsters moving during a
step when in fact they had moved during the sleeps between the commands. With
timescale 0 normal frames advance the world by nothing, while `step` (which uses
its own fixed tick, unscaled) still works. Only the step advances the world.

**Check `state` before and after.** See above.

## Proven

A real fight, simulated inside one `step`, twenty sim-seconds in well under a
second of wall clock:

```
step 20   (lockstep on, timescale 0, seed 7, party at 12,8)

           before          after
  Brand    42.0/42.0       25.9/42.0
  Sera     30.0/30.0        8.7/30.0
  Maren    34.0/34.0        0.0/34.0  DOWN
  Tilo     24.0/24.0        0.0/24.0  DOWN
```

That is the capability everything else is built on.

## Next

**P6, the progression ladder, is BLOCKED on the healing question at the top of
this file** — not on machinery. A rung can be built, seeded, fought and swept
today; what is undecided is how the party is supposed to *arrive* at the next
fight, and until that is answered a ladder's rungs are joined by a `heal` that
the game itself cannot perform.

~~P7 blast geometry~~ **DONE** — it was not blocked by the healing question (a
blast is one detonation, not a sequence of fights), so it was built first.

What is left is **P8**: `tools/Eval.ps1` and a `/check-eval` skill, outside the
quick tier — and P6 whenever the healing model is settled.

**A balance signal already, unasked for:** one skeleton — 16 hp, the second-
weakest thing in the game — took three of four fresh members down in thirty
seconds. An unarmoured low-level party being about three swings from death is
Michael's explicit design decision and must not be "fixed", but it is now a
number rather than an impression, which is the entire point of this.
