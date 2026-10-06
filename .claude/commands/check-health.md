---
description: Crashes, faults and stalls are caught, recorded and explained
argument-hint: "[selftest|<case>]"
allowed-tools: PowerShell, Read, Grep, Glob
---

Break the real game ten ways and read `dungeon.log` (~10 min).

- no argument → `.\tools\CheckAll.ps1 -Only health`
- `selftest` → `.\tools\CheckAll.ps1 -Only health -SelfTest` (every case must
  fail on each of its expectations, and for no other reason - a harness error
  or a death with nothing injected fails the self-test)
- a case name → `.\tools\HealthTest.ps1 -Only <case>` for one case, much faster

Cases: `throw` - `uiclip` - `uinest` - `worker` - `stall` - `probe` - `kill` -
`restart` - `fault` - `assert`. Every event kind is covered; `kill` drives the
Killed kind through `threadkill`, the THREADS panel's kill button as a command.
`uiclip` and `uinest` read what a caught throw leaves BEHIND in the UI walk's
clip (code-review C208): a throw from inside a scroll area's walk, then a click
outside it that must land; and a scroll area nested in a tab page whose
siblings must still take their clicks and draw (`clippoke`).

## What it is guarding

That the game never again dies without saying why. Before this existed, an
access violation vanished the process silently; a worker that threw a thousand
times looked exactly like one that threw once.

The harness deliberately reads **only `dungeon.log`** — never engine internals.
If the answer is not in the file you open after a crash, it does not count.

## Reading a failure

The `[FAIL]` line prints the exact pattern that was missing. Then look at
`build\debug\bin\dungeon.log` for what *did* happen:

- **`fault` / `assert` failing** — check whether a `.dmp` was written beside the
  exe. Report plus dump missing usually means `crash::Install()` is not running.
- **`throw` failing on the stack pattern** — the throw-time capture is the
  fragile part. A stack naming `Main.cpp` (the catch site) instead of
  `Game_DevDiagnostics.cpp` (the throw) means the vectored handler did not fire.
- **`stall` / `restart` failing** — stall detection must not ride the reboot
  path; a worker with no `autoRestart` still has to be recorded.
- **`uiclip` failing only on the click** - the throw was recorded but the clip
  it was thrown under outlived it: look at ui::ScopedClip (src/UI/Widget.cpp)
  and the reset at the top of UIContext::Update / Render. `uiclip` failing on
  the THROW's line, with "the case tests nothing" in the log instead, means the
  poke lost its premise (no clip in force, or the button inside it): look at
  Game/ClipPoke.cpp, not at the walk. `uinest` failing names which sibling
  missed its click or was clipped.
- **`stall` or `kill` failing on a frame pattern** (`DelayExecution`, the
  `Game_DevDiagnostics.cpp` line) - those frames must sit UNDER the event's own
  line: the supervisor walks a stalled worker, and `StopOrTerminate` a wedged
  one before the terminate (code-review C387), and the log prints a walked
  stack whole. Missing frames under a stall line usually means the walk could
  not take the worker's `controlMx` within 50 ms, or the tick ended mid-walk.
  The patterns are anchored to their line on purpose: the stall, the kill and
  the probe of one worker all log the same frames.
- **A case timing out at startup** - that is the harness, not the product. The
  new game is started through the console's `newgame` (tools\HarnessGame.ps1
  Start-NewGame) and waited for on `Level ready:`; a case is not retried, so a
  launch that never got there fails that case as a harness error.

`Killed` had no scripted coverage until code-review batch 36 - a hard kill was
only a THREADS panel button. The `kill` case covers it now, so the run no longer
prints a note about it.
