---
description: Run the complete regression suite (about an hour) - every check, both builds
allowed-tools: PowerShell, Read, Grep, Glob
---

Run the full regression suite. Use a timeout of at least 90 minutes: it drives
the real game dozens of times (the Python judges alone launch it over fifty),
and each launch waits on a load.

```
.\tools\CheckAll.ps1 -Full
```

Adds the rows marked `full` to the quick tier:

<!-- BEGIN generated: checkall-full (tools\CheckDocs.ps1 -Write rewrites this; CheckAll's `docs` check fails on drift) -->
```text
  build-release  full   no self-test   the release build compiles clean (the config that rots unwatched)
  threads        full   self-testable  ThreadManager + AI buckets under load: no force-terminate, clean reboots
  alloc          full   self-testable  a steady-state frame allocates nothing on the heap
  alloc-hand     full   self-testable  the hand spells (light, douse, flare, fill, pebble) allocate nothing
  alloc-rest     full   self-testable  resting, a monster behind a shut door: its inline searches allocate nothing
  alloc-rest-reach full   self-testable  resting, a frozen monster with a way through: its inline paths allocate nothing
  alloc-lights   full   self-testable  64 lights allocate nothing; a full light list, element floor glows; a door or a walking Firelight re-renders its shadow cube
  typing         full   self-testable  typed console text arrives whole and in order (focus loss, heavy frames)
  health         full   self-testable  crashes, faults and stalls are caught, recorded and explained
  evalrunner     full   no self-test   the eval runner: reset = new game, batched = solo, headless = windowed, knobs move numbers
  editor         full   no self-test   the editor, phase by phase (EditorTest.py, each phase mutation-tested)
  world          full   no self-test   the world tier: saves, worlds, dungeons, the world map (WorldTest.py)
  levelbuild     full   no self-test   the level generator, measured from the files it writes (LevelBuildTest.py)
  quit           full   self-testable  a load can always be quit: `quit` mid-load, Alt+F4 in Borderless
  paths          full   self-testable  the game and an AssetBaker import run from a folder outside ASCII (UTF-8 code page)
  stale          full   no self-test   a harness refuses a stale exe, and CheckAll builds what it runs (StaleTest)
  build-profile  full   no self-test   the release-profile build compiles clean (DN_PROFILE rots unwatched too)
  profile        full   self-testable  the frame budget still adds up, and the verdict still reacts to load
  bc7            full   self-testable  the BC7 encoder error estimate against an independent decoder
```
<!-- END generated -->

This is the one to run before a merge, or after a stretch of work touching
threads, rendering or assets. Report per `/check`'s reporting guidance.

If a check fails, run its own command (`/check-threads`, `/check-health`, ...)
or `.\tools\CheckAll.ps1 -Only <name>` to re-run just that one while
investigating - an hour-long suite is a poor debug loop.
