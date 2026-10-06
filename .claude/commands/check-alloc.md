---
description: A steady-state frame allocates nothing on the heap
argument-hint: "[selftest]"
allowed-tools: PowerShell, Read, Grep, Glob
---

Measure a window of genuinely steady frames in the running game (~2 min).

- no argument → `.\tools\CheckAll.ps1 -Only alloc`
- `selftest` → `.\tools\CheckAll.ps1 -Only alloc -SelfTest` (must FAIL)

## What it is guarding

ARCHITECTURE.md's memory strategy: a steady-state frame allocates nothing. An
allocation in a settled frame is a bug, **events included** - a bump message, a
level line and a monster's swing all print without allocating
(docs/message-allocation.md), so the guard has no notion of an event and no
exception list.

The party stands still in this run because that is the **baseline**, not because
events are excused: a still party keeps the event paths out of the window, and a
path outside the window passes whether it allocates or not. The event paths are
put inside the window by `AllocTest.ps1`'s modes, run by hand (this command runs
only the default):

- `.\tools\AllocTest.ps1 -Wounded` - the regeneration tick (a full-health party
  never runs it)
- `.\tools\AllocTest.ps1 -Melee` - a monster swinging at the party, and the
  narration of each blow
- `.\tools\AllocTest.ps1 -Cast` - a bolt frozen in flight and a spellbook held
  open (the launch itself happens in the console's unguarded frame)
- `.\tools\AllocTest.ps1 -Impact` - the world running: bolts launched by the
  harness (`autocast`), striking a FRESH monster (its first burn is a cost every
  monster pays once, so it must not fall in the warm-up), expiring past it, and
  a Fire Burst detonating. It refuses a PASS unless the window's own tally
  (logged by the verdict frame) shows a hit, an expiry and a blast
- `.\tools\AllocTest.ps1 -Pause` - Esc into the pause menu and back, three
  times inside the window. The frame that leaves Playing is a transition and is
  disarmed (docs/ARCHITECTURE.md "Checking the rule"); this checks that rule,
  and refuses a PASS unless the verdict line counts a transition
  (`transitions=`). It does NOT cover the frames just after a resume: they fall
  inside the guard's 120-frame warm-up
- `.\tools\AllocTest.ps1 -Sheet` - the character sheet: hover (the status bar),
  every tab, a right-click opening the item details dialog, the use menu. It
  refuses a PASS unless `itemdetails status` counts an open made in the window
- `.\tools\AllocTest.ps1 -Items` - an item moved pack -> cursor -> floor ->
  cursor -> pack through the party inventory window, in eval_arena. The
  measured item is a kind never dropped before the window (a kind's first drop
  is paid by every kind, so it is not warm-up). It refuses a PASS unless the
  window's tally counts two drops and two lifts (`drops=`/`lifts=`)
- `.\tools\AllocTest.ps1 -Packs` - a 4-slot and an 8-slot bag swapped in the
  sheet's pack row, so a bag GROWS inside the window. It refuses a PASS unless
  `sheet status` counts two equips made during it (`equips=`)

## Reading a failure

`alloctest RESULT=FAIL` prints the violating call sites with symbolized stacks,
also in `dungeon.log`. Each **unique** stack is reported once per session, so a
standing violation cannot drown the log — a frame repeating a known stack stays
silent.

A violation during an event is a violation. There used to be a policy here that
event frames were reported but not asserted on; it was a rationalisation of a
defect (`loc::Tr` copying text the table already owned), and its real cost was
that a guard firing during ordinary play teaches you to ignore it. If an event
allocates, fix the event. Something firing events every frame is a separate,
MESSAGE-RATE problem, visible in the log on its own terms.

`alloc::Excused` is not a way out for gameplay. It is for paths that are allowed
to allocate inside an otherwise steady frame - a dev-console command, an editor
dialog, a first-time bake - and for reporting code, which must excuse itself
because `log::Write` formats a string.

One thing that looks like a bug and is not:

- **Debug allocation counts are not release counts.** MSVC iterator debugging
  makes `vector`'s move constructor allocate, so growth copies rather than
  moves. Do not compare a debug number against a release one.

The self-test arms `allocpoke`, which allocates every frame on purpose, and
requires the run to come back FAIL.
