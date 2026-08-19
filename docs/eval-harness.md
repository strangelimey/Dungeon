# The eval harness

**Status:** COMPLETE, P1–P8 built (2026-08-13). A rung can be built, seeded, fought and
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

## P8: the runner

```
.	ools\Eval.ps1              # every suite, with its measurements
.	ools\Eval.ps1 -Table       # the measurements alone
.	ools\Eval.ps1 -Only ladder
.	ools\Eval.ps1 -List
.	ools\Eval.ps1 -SelfTest    # the runner must FAIL on purpose
```

Ten suites in ONE PROCESS, about 40 seconds. `/check-eval` is the command form.

**It is deliberately OUTSIDE `CheckAll`'s tiers.** Every check in that suite
guards a rule; this one guards nothing — a green verdict means the scripts RAN,
not that the numbers are good. Folding it in would put a measurement among
assertions and invite someone to read PASS as "balance is fine".

What IS checked is that the runner still works: a suite that had silently
stopped measuring would read exactly like one whose numbers had not changed.
`-SelfTest` hands it a script with a non-command (must exit 1) and a path that
does not exist (must exit 2).

Measurements are read back from `dungeon.log`, not captured from the process —
the same file a human opens after a run, so the harness cannot show something
the log does not. The match is CASE-SENSITIVE, because `TALLY` is a result and
`tally reset` is the command that begins a rung.

## The `resources` suite (added with the health-and-healing build)

`resources.eval` measures the three pools: the rates, the state gate, and what
recovery TRAINS. It is the standing home for one claim the pure harness cannot
reach — **stamina/sec > mana/sec > health/sec at equal investment** — because
that is a property of the AUTHORED knobs, and RollTest deliberately cannot link
`Balance` (it would drag the file layer in). The dev command `regen` prints it.

Two things it does that are worth copying into any suite measuring a rate:

- **It reads the ordering off REFERENCE rows, not off the party.** The four
  members differ in every stat on purpose, so a per-member verdict flags Brand —
  a brute with INT 8 whose mana rightly crawls — as broken. A claim about knobs
  at equal investment cannot be tested on deliberately unequal characters.
- **It puts a CONTROL in the same run.** The resource practices must creep no
  stat, and an empty creep list proves that only if something in the same run
  does creep. So it casts: `fire` trains and creeps intelligence, `attunement`
  trains the identical 3.0 xp and creeps nothing. One observation, two facts.

`AllocTest.ps1` gained `-Wounded` for the same family of reason: regeneration
only runs BELOW maximum, so the default fresh-party run walks straight past the
whole resource tick and reports a confident PASS for code it never executed.

The `supplies` suite that followed measures food and water the same way, and its
readout is in HOURS REMAINING rather than in units — a meter at 62 means nothing
until you know the drain rate behind it, and that rate depends on the member's
conditioning. It also ends by round-tripping the two new fields through a save,
because a field that does not persist is a bug that surfaces hours later on
somebody else's machine.

## P6 answered at last: the `expedition` suite

The ladder was built to ask *how does the party arrive at the next fight?* and
could only ever answer "worse" — there was no healing, so it measured ATTRITION
and stopped. With the health-and-healing model built, `expedition.eval` asks the
real question: fight, retreat, rest, repeat, until something gives.

Measured (seed 31, novice party, 4x skel_warrior escalating by strength):

| rung | taken | rest | after | food | water |
|---|---|---|---|---|---|
| — | — | — | 42/42 | 100.0 | 100.0 |
| 4x | 24.3 | 39 s | 44/44 | 99.2 | 98.6 |
| 4x ×1.5 | 35.6 | 72 s | 45/45 | 98.1 | 96.6 |
| 4x ×2.0 | 96.7, one down | 151 s | 46/46 | 97.1 | 94.8 |
| 4x ×2.5 | 268.1 | — | **WIPED** | 96.1 | 92.9 |

**THE HEADLINE, and it is a balance finding rather than a bug:** three full
fight-and-recover cycles cost **2.9 food and 5.2 water out of 100**. Water is the
binding meter, at roughly 1.7 per cycle — so a load of supplies buys somewhere
near **fifty-five fights**, and the party died to DIFFICULTY at the fourth having
spent 7% of its water. As tuned today, supplies are not the constraint on a run;
the monsters are. Whether that is right is Michael's to decide, and the knobs are
all in the Balance dialog.

Two smaller things it shows: the rest LENGTH tracks the damage (39 s → 72 s →
151 s), and Brand's maximum health climbs 42 → 46 across the run, because
constitution trains on the recovery. Surviving damage is what makes you harder to
kill, exactly as the model claims.

**Three ways this suite lied before it told the truth**, all worth keeping:

1. **Built on `ladder.eval`'s rungs it reported `taken=0.0` everywhere.** A
   novice party is not troubled by skeleton swarms, so every rest had nothing to
   do — a full table measuring none of the thing it was written for. *An
   encounter that does not hurt cannot demonstrate recovery.*
2. **`rest on` followed by `step 900` does not measure a rest.** The auto-stop
   does not depend on `dt`, so it fires on the next ORDINARY frame — and at
   `timescale 0` that frame falls between the two console commands. With nothing
   to recover, rest was over before the step began, and the step then ran its
   whole budget with no rest in progress: fifteen minutes of supplies drained,
   reported as the cost of resting. `rest until` enters the state and runs the
   clock inside ONE command, so no frame can fall in the gap.
3. **It rested in a room that still had monsters in it.** A blow breaks rest and
   a downed member's stabilize clock resets on any monster in aggro, so the party
   sat paying for time and recovering nothing. The protocol is retreat FIRST,
   then rest — which is also what a player does.

It also found a real defect in the rest rules: the auto-stop counted only
STANDING members, so a fight that left one member down and the others unhurt
ended the rest after 0.02 seconds and walked away, reporting `recovered`. The
unconscious are recoverable and waiting for them is one of the things resting is
FOR; only the DEAD are excluded now.

## Recycling the world: one process, many scripts

Built when Michael asked for runs lasting hours. **Measured first**, because the
whole thing turns on one number: a dungeon load is **11,900 ms** and a suite took
14–18 s, so about **80% of an eval run was loading the same level ten times.**

`reset` is the answer — *put the world where `newgame` would, without the load*.
It costs **338 ms**, a 35× saving, because the twelve seconds are models,
textures and icons and those are cached by the time a second script asks. Ten
suites went from ~155 s to **38 s**.

**It is a directive a script CALLS, not something the runner imposes.** Michael's
call and the right shape: a suite testing something specific opens with `reset`;
one measuring PROGRESSION across a series deliberately does not, and inherits
whatever the last one left. `-eval a.eval b.eval …` queues them in one process,
each with its own `RESULT=` line and a `BATCH RESULT=` at the end.

Two failure policies differ on purpose. A script that cannot be READ mid-batch is
counted and the run **carries on** — the rest are still worth measuring, and one
typo should not cost them. A **timeout ends the batch**: something is wedged, the
remaining scripts would inherit it, and one honest failure beats twenty plausible
ones.

### Why the definition is "where a new game would leave it"

Because it is the only definition that can be TESTED. `resettest.eval` takes a
baseline after a real load, wrecks the world every way the harness knows how,
resets, and takes it again; the blocks must match line for line. Anything looser
would be a promise nothing could check — and an incomplete reset is the worst
defect available here: it does not crash, it prints nothing wrong, and every
later suite in the run is quietly contaminated with entirely plausible numbers.
This harness lost twelve ladder rungs to that exact shape once already.

### Three defects the recycling work found, and the one worth remembering

1. **The first `reset` did not restore the MAP — and the equivalence test passed
   anyway.** `arena` walls every cell before carving and strips the map's own
   fixtures, stairs, niches and decorations, so the world afterwards was still an
   empty box with the authored monsters standing in rock. The check printed the
   party, the supplies, the regen rates, the monsters and the skills, and **not
   one of those shows map geometry.** *A test that does not look at the thing
   that differs reports "identical" just as loudly as one that does* — and this
   time it happened inside the check written to prevent it. `mapinfo` now carries
   a static-layer fingerprint whose walkable-cell count is the only readout in
   the harness that can see an arena.
2. **A batch re-ran every earlier script.** `ReadEvalLines` APPENDS (right for
   `include`, its other caller) so the second script ran the first ahead of
   itself and the tenth ran all ten. It came back PASS every time and merely took
   longer — visible only in the cumulative `lines=` counts.
3. **`ResetForEval` reached for `StartNewGame()` directly**, re-introducing the
   crash P2 fixed: from a cold boot the HUD has never been built. It goes through
   `m_ui.onStartNewGame` like the `newgame` command does.

`-SelfTest` now checks four things: a bad script exits 1, a missing one exits 2,
**`reset` equals a new game**, and **a batched suite matches a solo one**. The
last two are the ones that keep the speedup honest — mutation-checked by dropping
the map restore, which they catch.

## What the harness costs the shipping code (audited 2026-08-15)

Michael's review question was the right one: *is this magic flags and special
processing everywhere, and if so should it be behind `#ifdef`?* So here is the
complete inventory, which is the point of writing it down — it should be
auditable in one read rather than trusted.

**All harness STATE the world holds is one member**, `DungeonWorld::m_harness`
(`struct Harness`): the encounter `tally`, `autoAttack`, `frozen`, and
`pendingSteps`. It is touched in six places in the simulation — the two
`fx::ITarget` adapters and `ResolveAttack` (the tally), one `continue` in the
monster loop (`frozen`), one guard on `TickAutoAttack`, and three lines feeding
queued steps. Every one of them reads `m_harness.something` and says what it is.
`ResetForEval` is `m_harness = {}`, so a field added to the struct is reset for
free — the four loose bools this replaced were four chances to forget one.

**The script runner is its own TU**, `Game_Eval.cpp`: `PumpEvalScript`,
`LoadEvalScript`, `ReadEvalLines`, `ResolveEvalPath`, `StepWorld`,
`ResetForEval`. `Game::Update` calls the pump, which returns on its first line
when no script is loaded. Its ten `m_eval*` members stay on `Game` (they are
already prefixed and under one banner, so they self-label without a struct).

**Headless is one branch** in Main's frame loop plus `Game::EndHeadlessFrame`.

**Three things that look like harness machinery and are not:**

- **Lockstep AI.** `SetResting` turns it on, because rest runs the world at 60x
  and lockstep is what makes the monsters think honestly through a fast-forward.
  It would have to exist if the harness never had.
- **The dev console.** 90 commands, of which `allocguard`, `allocpoke`,
  `crashpoke`, `threadwedge` and `uioverlap` predate eval entirely. The console
  is the designated home for dev facilities and ships in every build by design.
- **The damage ledger** (`Game/DamageLedger.h`) is a shipping invariant check
  like `Core/AllocTrack`, not harness machinery. Its sanction sites are the
  documentation of a rule, not scaffolding for a test.

### Why none of it is behind `#ifdef`

It was considered and rejected on 2026-08-15. **The harness's entire value is
that it measures the shipping binary** — the rule `tools/RollTest`'s CMakeLists
states for the roll engine: the real thing linked straight in, never a copy.
Compile these hooks out of release and the suites, and `/check-pipeline`, would
be measuring a build that is not what ships. It would also add a fourth build
configuration to a project that already runs `build-release` and `build-profile`
checks *because* a configuration nobody builds rots.

The consolidation was proven behaviour-preserving the way the `Defense.h`
refactor was: the pipeline suite is deterministic, and every number came back
identical across it (816,146 values checked over 33,048 checkpoints, all five
route totals unchanged).

## Headless (`-headless`)

`Dungeon.exe -headless -eval <script...>` runs with **no window on screen and no
drawing**. The simulation, the dev console and the log are unchanged;
`Eval.ps1 -Headless` and `PipelineTest.ps1 -Headless` pass it through.

**It is not really a speed switch, and it is worth saying so before somebody
measures it hopefully.** Ten suites go **42 s → 37 s**, about 12%. The time is
the asset load plus the `step` loops, and a `step` runs many simulated seconds
inside ONE frame — there are simply not many frames to save. What it buys is a
run that does not steal focus, that survives being launched over RDP or from a
scheduled task, and that can be run several at a time without contending for the
GPU. It also retires `docs/drive.ps1`'s worst failure, `Shot` grabbing whatever
window happened to be in front.

**What it does NOT do is remove the graphics device.** The swapchain is bound to
an HWND, so the window still exists — it is merely never shown. Prising the
device out would mean a null path at every `gfx` call site (mesh building,
texture upload, icon bakes, font atlases) for no gain, because what a headless
run saves is the per-frame cost, not the once-per-process cost of owning a
device. A machine with no GPU is already handled a layer down: `GraphicsDevice`
falls back to WARP.

### The one thing that made it non-trivial

`Game::Render` is pure drawing — except for its last line, `++m_framesRendered`,
and `RunLoadTasks` gates on it: a staged task runs only once the state's screen
has been *presented* at least once, so a multi-second bake never lands on a frame
nobody has seen. Skip rendering naively and that counter never moves, the load
queue never advances, and the run sits on the loading screen until the 600-second
script timeout — a hang, reported as a timeout, ten minutes later.

So `Game::EndHeadlessFrame` does that bookkeeping in its own words. The increment
stays where it is for the normal path: moving it would change what "presented"
means for everybody in order to fix a case that has no screen.

**The equivalence is CHECKED, not assumed.** `Eval.ps1 -SelfTest` now runs a
suite both ways and requires the console output to match line for line (161 lines,
identical). Without that, a headless mode that quietly measured something else
would look exactly like one that worked — the same argument as the
batched-matches-solo check beside it, and the frame counter is the proof that the
risk is real rather than theoretical, since something the simulation depended on
genuinely was living in the render pass.

**Two things deliberately NOT run headless:** `/check-ingame`, because
`uioverlap` measures what widgets PAINT and nothing paints; and `/check-alloc`,
because the allocation guard brackets update *and* render, so a headless frame is
a different frame from the one the rule is about.

## The one suite that is not a measurement

`pipeline.eval` lives in `tools\EvalScripts\` and is deliberately **not** in
`Eval.ps1`'s suite list. It borrows the machinery — `-eval` scripting, `arena`,
`reset`, `step`, lockstep AI — but it is a CHECK: it guards docs/effects.md's
one-pipeline invariant, so it has a right answer, its own harness
(`tools\PipelineTest.ps1`), and a place in CheckAll's quick tier with the other
checked rules. Everything in `Eval.ps1` produces tables nobody has decided the
right answer for, which is exactly why that runner stays outside those tiers.

Worth carrying forward from building it: **its own first run demonstrated the
failure this project keeps meeting.** The party was wiped by a blast, the app
dropped to the title screen, and the next four sections printed a confident
`RESULT=PASS` while nothing simulated at all. `PipelineTest.ps1` therefore
demands the app is still `playing` at the end and that every sanctioned route
moved a non-zero amount of health — and that second rule immediately caught a
*different* empty section, over-exertion billing nothing because an untrained
fighter cannot over-exert by design. Two empty passes, in the suite written to
be immune to empty passes. **Assume the run stopped early until something in the
output proves it did not.**

## What PASS means (tightened 2026-08-17)

The harness was audited end to end because Michael could not tell, from a run
that prints little and reports ten greens, whether it was measuring anything.
The full findings are docs/eval-audit.md; the short version is that the
MEASUREMENTS were sound — five knobs moved, five numbers responded, `dealt`
reconciles against monster health — and the VERDICT was not.

`PASS` used to mean one thing: every line matched a registered command name and
the queue emptied. It now means four:

1. every line named a command (unchanged — this is what catches a typo);
2. **every line DID what it said.** `DevConsole::Refuse` is the second signal
   `RunLine`'s bool never carried. A `spawn` onto rock, a `tp` into a wall, an
   unknown spell and a mis-called command all returned "true, a command by that
   name exists" and the run reported PASS over an encounter it never set up;
3. **every `step` ran the time it was given.** `StepWorld` reports a `StepStop`
   reason, and `Ceiling` is a refusal. It always printed the seconds it really
   ran and nothing read them;
4. **every suite measured something,** and the script ended in play.

THE RULE FOR A NEW REFUSAL SITE: `Refuse` means "you asked me to change the
world and I did not". A query with nothing to say stays a `Print` — `monsters`
answering `no monsters` is an ANSWER, not a refusal. The test is whether a
script that carried on regardless would be measuring something other than what
it wrote.

### What it caught, immediately

Three of the ten suites failed on the first run and all three were real. Two —
`supplies` and `rest` — had been asking for `step 3600` and getting 3333.33s,
the per-call ceiling, while calling the result "an hour"; both are now two calls
of 1800 and their drain figures rose by the 8% the correction predicts.
`expedition` had been ending at the MENU, printing "what the expedition cost,
and what it earned" off a party that was already dead.

**The lesson is not that somebody was careless.** `rest.eval` carried an inline
comment saying "`step` caps at 200000 ticks, so an hour arrives as 3333
seconds" — it KNEW — and still titled the section "an hour of world time".
`supplies.eval` did not know at all. The same fact was documented in one of the
two files that needed it, which is the whole argument for a runtime check over a
comment, made by this codebase about itself.

The end-in-play rule is also not new to the project: `PipelineTest.ps1` learned
it the same way one section up on this page, after a T-junction blast wiped its
party and four sections printed a confident PASS over nothing. The eval runner
simply had not adopted it. **When one harness here learns that a run can die
quietly, check whether the others have been told.**

## Next

The harness is done and so is the system it was waiting for
(docs/health-and-healing.md). What is left is the BALANCE PASS — and for the
first time the questions are numeric rather than rhetorical: recovery is cheap,
exertion is a small drain beside time, and a run ends to monsters long before it
ends to hunger.

**A balance signal already, unasked for:** one skeleton — 16 hp, the second-
weakest thing in the game — took three of four fresh members down in thirty
seconds. An unarmoured low-level party being about three swings from death is
Michael's explicit design decision and must not be "fixed", but it is now a
number rather than an impression, which is the entire point of this.
