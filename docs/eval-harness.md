# The eval harness

**Status:** P1–P4 built (2026-08-13). The suites do not exist yet — this is the
clock they run on, the runner that drives them, the room they happen in and the
way a rung is seeded.

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

**P5 first has to make the party fight back** (see above) — an auto-attack mode,
or rungs are measuring defence only. Then the encounter summary and the N-seed
sweep; P6 the progression ladder; P7 blast geometry; P8 `tools/Eval.ps1`
and a `/check-eval` skill, outside the quick tier.

**A balance signal already, unasked for:** one skeleton — 16 hp, the second-
weakest thing in the game — took three of four fresh members down in thirty
seconds. An unarmoured low-level party being about three swings from death is
Michael's explicit design decision and must not be "fixed", but it is now a
number rather than an impression, which is the entire point of this.
