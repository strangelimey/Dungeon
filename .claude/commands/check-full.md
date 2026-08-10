---
description: Run the complete regression suite (~20 min) — every check, both builds
allowed-tools: PowerShell, Read, Grep, Glob
---

Run the full regression suite. Use a timeout of at least 30 minutes: it drives
the real game several times and each launch waits on a cold-cache load.

```
.\tools\CheckAll.ps1 -Full
```

Adds to the quick tier: the release build, the thread-system stress run, the
allocation guard, the diagnostics harness, and the BC7 encoder.

This is the one to run before a merge, or after a stretch of work touching
threads, rendering or assets. Report per `/check`'s reporting guidance.

If a check fails, run its own command (`/check-threads`, `/check-health`, …) to
re-run just that one while investigating — a 20-minute suite is a poor debug
loop.
