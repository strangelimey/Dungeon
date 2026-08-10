---
description: Crashes, faults and stalls are caught, recorded and explained
argument-hint: "[selftest|<case>]"
allowed-tools: PowerShell, Read, Grep, Glob
---

Break the real game seven ways and read `dungeon.log` (~7 min).

- no argument → `.\tools\CheckAll.ps1 -Only health`
- `selftest` → `.\tools\CheckAll.ps1 -Only health -SelfTest` (must FAIL)
- a case name → `.\tools\HealthTest.ps1 -Only <case>` for one case, much faster

Cases: `throw` · `worker` · `stall` · `probe` · `restart` · `fault` · `assert`.

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
  `Game_DevCommands.cpp` (the throw) means the vectored handler did not fire.
- **`stall` / `restart` failing** — stall detection must not ride the reboot
  path; a worker with no `autoRestart` still has to be recorded.
- **A case timing out at startup** — that is the harness, not the product. It
  retries `Start New Game` three times; if all three miss, the window never took
  focus.

`Killed` has **no scripted coverage** — a hard kill is a THREADS panel button,
not a console command. The run says so every time; that line is expected output,
not a warning.
