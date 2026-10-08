---
description: Run the quick regression tier (a few minutes) and report what drifted
argument-hint: "[extra CheckAll.ps1 flags]"
allowed-tools: PowerShell, Read, Grep, Glob
---

Run the quick regression tier and report the result.

```
.\tools\CheckAll.ps1 $ARGUMENTS
```

Quick is the everyday tier - the rows marked `quick` below, a few minutes. It is
meant to be run often enough that drift is caught while you still remember what
you changed. Whatever is selected brings its build rows with it, first, and every
harness refuses a stale exe on its own (exit 4).

## What the tiers run

`CheckAll.ps1 -List`, as it prints today:

<!-- BEGIN generated: checkall-list (tools\CheckDocs.ps1 -Write rewrites this; CheckAll's `docs` check fails on drift) -->
```text
  build-debug    quick  no self-test   the debug build compiles clean
  build-release  full   no self-test   the release build compiles clean (the config that rots unwatched)
  diag           quick  no self-test   the health record: ring, wrap, cross-thread writes, torn reads
  rolls          quick  self-testable  the pure rules: dice, strike, armour, blasts, resources, ledger, carve, party
  anim           quick  self-testable  the Animator keeps Play's promises: a held clip, mid-fade too, never restarts
  docs           quick  self-testable  the /check-* commands list what CheckAll -List and Eval -List print
  verdict        quick  self-testable  the native judges' last-line reader refuses every bad or contradictory line
  lang           quick  self-testable  every language key the code names is in every .lang file, holes matching
  template       quick  self-testable  the new-world template is what BuildTemplate.py makes of dungeon-demo: its picks, its items, byte for byte
  threads        full   self-testable  ThreadManager + AI buckets under load: no force-terminate, clean reboots
  ingame         quick  self-testable  level files + installed models, and a uioverlap sweep of every screen
  pipeline       quick  self-testable  every source of damage goes through fx::Deal; nothing else writes health
  spells         quick  self-testable  every spell tier does what it says: hand spells, bolts, modifiers, wards; descriptions shown whole
  combat         quick  self-testable  the combat rules the code review fixed, judged from combat.eval
  ai             quick  self-testable  monsters stand where they can (sides, kiters, pits); a new game or load forgets the last fight; a world switch leaves no dead AI worker; a resting chaser walks and thinks as an awake one, and a 60x shot skips neither the party nor a wall
  alloc          full   self-testable  a steady-state frame allocates nothing on the heap
  alloc-hand     full   self-testable  the hand spells (light, douse, flare, fill, pebble), and Rock past 64 drops, allocate nothing
  alloc-rest     full   self-testable  resting, a monster behind a shut door: its inline searches allocate nothing
  alloc-rest-reach full   self-testable  resting, a frozen monster with a way through: its inline paths allocate nothing
  alloc-lights   full   self-testable  64 lights allocate nothing; a full light list, element floor glows; a door or a walking Firelight re-renders its shadow cube
  alloc-items    full   self-testable  moving an item allocates nothing; no glow from a shut niche; every click target hits where it is drawn
  typing         full   self-testable  typed console text arrives whole and in order (focus loss, heavy frames)
  health         full   self-testable  crashes, faults and stalls are caught, recorded and explained
  evalrunner     full   no self-test   the eval runner: reset = new game, batched = solo, headless = windowed, knobs move numbers; load paths leave a clean device on WARP
  editor         full   no self-test   the editor, phase by phase (EditorTest.py, each phase mutation-tested)
  world          full   no self-test   the world tier: saves, worlds, dungeons, the world map (WorldTest.py)
  levelbuild     full   no self-test   the level generator, measured from the files it writes (LevelBuildTest.py)
  quit           full   self-testable  a load can always be quit: `quit` mid-load, Alt+F4 in Borderless
  display        full   self-testable  a Windowed Apply lands centred in the chosen monitor's work area, unsaved from a script; on WARP the monitors are still listed, a display change re-reads them; a relaunch keeps -project and the parent's log
  paths          full   self-testable  the game and an AssetBaker import run from a folder outside ASCII (UTF-8 code page)
  baked          full   self-testable  a .dds older than its PNG, or a model sidecar missing or stale, is refused and said (once a model)
  modelload      full   no self-test   the model loaders read what a file says: v//n OBJ normals, a glTF with no material, sidecars by image index
  stale          full   no self-test   a harness refuses a stale exe, and CheckAll builds what it runs (StaleTest)
  build-profile  full   no self-test   the release-profile build compiles clean (DN_PROFILE rots unwatched too)
  profile        full   self-testable  the frame budget still adds up, and the verdict still reacts to load
  worn           full   self-testable  the worn-block bake has one authority: models = wornblock = the committed files; wear 0 is flat
  bakerwrites    full   self-testable  the asset baker fails loudly: a read-only target is an error saying why, names are escaped, a bad map is said; an import flips green as asked
  convertmesh    full   self-testable  ConvertMesh --keep-rig keeps the skeletal one of a take's two actions, and FetchModels hears a traceback
  meshes         full   self-testable  the script-built arches, fountains, potions, rock and door frames are closed, face out and run their u one way
  bc7            full   self-testable  the BC7 encoder error estimate against an independent decoder
```
<!-- END generated -->

## The family

<!-- BEGIN generated: family (each command file's own description; tools\CheckDocs.ps1) -->
| command | covers |
|---|---|
| `/check` | Run the quick regression tier (a few minutes) and report what drifted |
| `/check-alloc` | A steady-state frame allocates nothing on the heap |
| `/check-bc7` | The BC7 encoder's error estimate against an independent decoder |
| `/check-build` | Both configs compile clean — including release, which rots unwatched |
| `/check-diag` | The health record's ring — wrap, cross-thread writes, torn reads |
| `/check-eval` | Run the damage-system eval suites and report what they measured |
| `/check-full` | Run the complete regression suite (about an hour) - every check, both builds |
| `/check-health` | Crashes, faults and stalls are caught, recorded and explained |
| `/check-ingame` | Level files, installed models, and a UI overlap sweep of every screen |
| `/check-pipeline` | Every source of damage goes through fx::Deal — nothing else writes health |
| `/check-profile` | The frame budget still adds up, and the verdict still reacts to load |
| `/check-selftest` | Check the checkers — every harness is handed a failure and must report it |
| `/check-threads` | Thread system under load — no force-terminate, clean supervised reboots |
<!-- END generated -->

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
