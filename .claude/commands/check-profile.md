---
description: The frame budget still adds up, and the verdict still reacts to load
argument-hint: "[selftest]"
allowed-tools: PowerShell, Read, Grep, Glob
---

Snapshot, change, snapshot — the loop the profile panel exists to serve, run
against a real game (~2 min, one launch).

- no argument → `.\tools\CheckAll.ps1 -Only profile`
- `selftest` → `.\tools\CheckAll.ps1 -Only profile -SelfTest` (must FAIL)

Needs a **release-profile** build; without `DN_PROFILE` every zone compiles to
nothing and there is no budget to check. The suite's `build-profile` check covers
that config compiling at all.

## What it is guarding

**The partition.** `cpu` is *defined* as the frame minus every block
(`wait.gpu`, `present`, `wait.cap`), so those four must sum to the frame. They
stop summing the moment someone adds a blocking call — a fence, a sleep, a lock —
without giving it a zone. Nothing crashes: the unaccounted time lands in `cpu`
by elimination and an idle engine starts reading as CPU-bound, which is the one
direction that sends you optimising the wrong half of the engine.

**That the verdict still reacts.** A hard-wired `bound by display` is
indistinguishable from a correct one on a display-bound machine, and this
instrument was nearly shipped having only ever produced that single reading. The
run forces two different changes — quality low→ultra for the GPU term, framecap
off→on for the cap term — so a readout that has stopped reporting cannot pass by
accident on one of them.

**That the cap holds.** Capped frame time against `1000/hz`, where `hz` is parsed
from the game's own `framecap` report rather than hardcoded, so the check is not
pinned to the monitor of whoever wrote it.

## Reading a failure

**`cpu+wait+present+cap = X but frame = Y`** — an unaccounted block. Something
new is blocking inside the frame with no zone around it. Find it by raising
detail on `frame` in the panel and looking for the gap; the fix is a
`DN_PROFILE_ZONE` at the blocking call and, if it is a *wait*, a term in
`MeasureFrameBudget` so it is subtracted from CPU rather than counted as work.

**`ultra barely moved GPU work`** — the GPU timestamp path has stopped
reporting. Check `GpuProfiler::Init` succeeded (it refuses on adapters with no
timestamp frequency and logs so) and that the `gpu` source still appears in the
tree.

**`cap missed`** — the frame limiter is not holding its target. Most likely the
high-resolution waitable timer failed to create (logged as a warning at first
use) and it fell back to spinning, or the deadline resync threshold is swallowing
the wait.

**`snapshot '<x>' never reached the log`** — a *coverage* failure and the more
serious kind: the run recorded nothing and would otherwise have passed on an
empty result. Usually a dropped keystroke or the console not open.

## What a green run does not prove

`-SelfTest` inverts only the **coverage** assertion. The partition and reaction
checks have each failed for real during development, which is weaker evidence
than a harness that can produce the failure on demand. The run says so on every
invocation rather than leaving it to be assumed.

Two assertions **skip** rather than fail when the machine cannot exercise them:
no GPU timestamps (WARP), and a single-monitor desktop where the compositor
already paces at the cap target so the cap has nothing to hold back. Both are
named in the output.
