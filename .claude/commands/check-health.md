---
description: Crashes, faults and stalls are caught, recorded and explained
argument-hint: "[selftest|<case>]"
allowed-tools: PowerShell, Read, Grep, Glob
---

Break the real game eighteen ways and read `dungeon.log` (~16 min). Seven of
them END the process on purpose (`fault`, `overflow`, `assert`, `devremoved`,
`devassert`, `devfault`, `devterminate`), each in a game of its own that the
harness launched.

- no argument → `.\tools\CheckAll.ps1 -Only health`
- `selftest` → `.\tools\CheckAll.ps1 -Only health -SelfTest` (every case must
  fail on each of its expectations, and for no other reason - a harness error
  or a death with nothing injected fails the self-test)
- a case name → `.\tools\HealthTest.ps1 -Only <case>` for one case, much faster

Cases: `throw` - `uiclip` - `uinest` - `uiscroll` - `worker` - `stall` - `probe` -
`kill` - `restart` - `healthmark` - `panelclick` - `fault` - `overflow` -
`assert` - `devremoved` - `devassert` - `devfault` - `devterminate`. Every event
kind is covered; `kill` drives the
Killed kind through `threadkill`, the THREADS panel's kill button as a command.
`devremoved` removes the D3D12 device on purpose (`crashpoke devremoved`, a
TDR's shape without hanging a GPU) and demands the failed call's HRESULT by
name, then the removal reason and DRED's record BEFORE the FATAL line
(code-review C195); `dredpoke` puts a made-up DRED record through the same
readout first, since an asked-for removal leaves DRED no command list in flight
to print. `devassert` / `devfault` / `devterminate` (`crashpoke devremoved
assert|fault|terminate`) remove the device and die at once, before any call
fails, so the removal can only arrive through the crash handler's fatal note -
after that report's own lines.
`uiclip` and `uinest` read what a caught throw leaves BEHIND in the UI walk's
clip (code-review C208): a throw from inside a scroll area's walk, then a click
outside it that must land; and a scroll area nested in a tab page whose
siblings must still take their clicks and draw (`clippoke`). `uiscroll`
(`scrollpoke`, code-review C127) drives the one scrollbar, ui::ScrollBar: a
120-row drop-down's open list and a scroll area, each by wheel and thumb drag,
in a scratch tree.
`healthmark` and `panelclick` click the console's readout panel for real
(code-review C380 / C379): a HEALTH mark holding a stall and its restart
(`crashpoke stallpair`, located by `health strip`) must name the stall it is
drawn as, and a click on a THREADS button laid out unseen under the panel's foot
(`consolepanel`, THREADS expanded unsaved over `threadspawn 0 8` batches) must
change no worker - only the count of presses the panel turned away moves. It
clicks a worker's row, never a section expander (none is laid out below the
panel in this case), so it does NOT check that such a click writes no
settings.ini: the expanders sit behind the same gate, but no click here reaches
one.

## What it is guarding

That the game never again dies without saying why. Before this existed, an
access violation vanished the process silently; a worker that threw a thousand
times looked exactly like one that threw once.

The harness deliberately reads **only `dungeon.log`** — never engine internals.
If the answer is not in the file you open after a crash, it does not count.

## Reading a failure

The `[FAIL]` line prints the exact pattern that was missing. Then look at
`build\debug\bin\dungeon.log` for what *did* happen:

- **`fault` / `overflow` / `assert` failing** - check whether a `.dmp` was written
  beside the exe. Report plus dump missing usually means `crash::Install()` is
  not running. Each wants ONE line naming the dump's status (`minidump 1 of 3
  written`), written after the dump (code-review C385): the line present but met
  twice means a handler logs before the dump again. `overflow` alone failing,
  with no line and no dump, means the report ran out of stack - check
  `crash::GuardThreadStack` and the reporter thread in Core/CrashHandler.cpp
  (C388; dungeon.log's `crash handlers installed` line says whether the reporter
  started).
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
- **`uiscroll` failing** - its line names every step that did not hold (the
  wheel's step, the drag's end or half way, a row picked under the thumb). The
  bar is src/UI/ScrollBar.cpp; the list's use of it DropDown::UpdateSelf
  (Controls_Popups.cpp), the page's ScrollArea::UpdateSelf
  (Controls_Containers.cpp).
- **`stall` or `kill` failing on a frame pattern** (`DelayExecution`, the
  `Game_DevDiagnostics.cpp` line) - those frames must sit UNDER the event's own
  line: the supervisor walks a stalled worker, and `StopOrTerminate` a wedged
  one before the terminate (code-review C387), and the log prints a walked
  stack whole. Missing frames under a stall line usually means the walk could
  not take the worker's `controlMx` within 50 ms, or the tick ended mid-walk.
  The patterns are anchored to their line on purpose: the stall, the kill and
  the probe of one worker all log the same frames.
- **`devremoved` failing** - the `dredpoke` lines missing or misread means the
  readout (Graphics/D3DUtil.cpp LogDredRecord) changed its wording or its op
  window (the queued list, 0 completed, must be one line with no stop marker);
  the removal's lines missing between `D3D12 call failed at` and the FATAL line
  means FailHr no longer asks the device itself (lines AFTER FATAL are the fatal
  note's), or the device was never handed to WatchDevice. A `DRED
  auto-breadcrumbs: unavailable` or `DRED page fault: unavailable` line means
  DRED, or its page faults, was not switched on before the device was made
  (GraphicsDevice::EnableDred).
- **`devassert` / `devfault` / `devterminate` failing** on the removal lines -
  the fatal note did not run in that report (Core/CrashHandler.cpp RunNote:
  ReportFatal, the fault filter, the terminate handler), or ran before the
  report's own last line - the one line each report writes after its dump
  (C385), or a fault's walked stack under it.
- **A case timing out at startup** - that is the harness, not the product. The
  new game is started through the console's `newgame` (tools\HarnessGame.ps1
  Start-NewGame) and waited for on `Level ready:`; a case is not retried, so a
  launch that never got there fails that case as a harness error.

`Killed` had no scripted coverage until code-review batch 36 - a hard kill was
only a THREADS panel button. The `kill` case covers it now, so the run no longer
prints a note about it.
