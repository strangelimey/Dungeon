# The eval harness

**Status:** P1 built (2026-08-13). The harness itself does not exist yet — this
is the clock it will run on.

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

P2 the `-eval <script>` batch runner and structured `RESULT=` output; P3 `arena`
+ `spawn` (an empty room with a monster where you want it — the showcase level
fought every attempt to verify P1, which is exactly what generated arenas are
for); P4 `setstat`/`setskill`/`preset`; P5 the encounter summary and the N-seed
sweep; P6 the progression ladder; P7 blast geometry; P8 `tools/Eval.ps1`.
