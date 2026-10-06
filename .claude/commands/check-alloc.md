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
  narration of each blow. It is spawned HELD (`freeze hold`), so its first
  notice of the party and its first blow fall INSIDE the window: a fight's first
  blow is paid by every session, so it is not warm-up
- `.\tools\AllocTest.ps1 -Rest` - the party resting with a member down, a
  skeleton that has seen them waiting behind a shut door. Rest forces lockstep,
  so the AI's search runs on the main thread in guarded frames, and fails every
  frame. The rest starts INSIDE the window, from a click on the HUD's Rest
  button (`rest button`), so lockstep's first inline thinks are measured too. It
  refuses a PASS unless the rest outlasted the window, the monster was still
  engaged at its end and `lockstep stats` counts inline thinks (also `CheckAll
  -Only alloc-rest`)
- `.\tools\AllocTest.ps1 -RestReach` - the same with no door and the monster
  frozen (`freeze on`), so every think FINDS a path it never walks: the search's
  output, a plan's path. It refuses a PASS unless `lockstep stats` counts paths
  (also `CheckAll -Only alloc-rest-reach`)
- `.\tools\AllocTest.ps1 -Cast` - a bolt frozen in flight and a spellbook held
  open (the launch itself happens in the console's unguarded frame)
- `.\tools\AllocTest.ps1 -Impact` - the world running: bolts launched by the
  harness (`autocast`), striking a FRESH monster (its first burn is a cost every
  monster pays once, so it must not fall in the warm-up), expiring past it, and
  a Fire Burst detonating - the process's FIRST detonation, since the burst
  caster joins the held rotation only after the warm-up. It refuses a PASS unless
  the window's own tally (logged by the verdict frame) shows a hit, an expiry and
  a blast
- `.\tools\AllocTest.ps1 -Burst` - shots AT the party, which -Impact never
  fires: the skel_magus's burst bolt going off on contact with the party (every
  member hurt and left burning), a Wind Ward turning one, and a gust's repel
  weakening one, flinging one back and spending one so it falls - all fired from
  a world frame by `autocast bolt` (the console's frames are never measured). It
  refuses a PASS unless the window's tally counts each (`partybursts=`,
  `wardturns=`, `repelweakened=`, `repelturned=`, `repelspent=`)
- `.\tools\AllocTest.ps1 -Swing` - the PARTY swinging, which -Melee (a monster
  swinging at the party) never does: the party's swings held (`autoattack hold`)
  until the window opens, so the session's first one is measured, at a frozen
  skeleton, with the die loaded (`fumble severe 1 1`) so Sera's part-burnt torch
  fumbles severely and is knocked to the floor inside the window. It refuses a
  PASS unless the window's tally counts two swings, a severe fumble and a dropped
  item (`swings=`, `severefumbles=`, `fumbledrops=`)
- `.\tools\AllocTest.ps1 -Pause` - Esc into the pause menu and back, three
  times inside the window. The frame that leaves Playing is a transition and is
  disarmed (docs/ARCHITECTURE.md "Checking the rule"); this checks that rule,
  and refuses a PASS unless the verdict line counts a transition
  (`transitions=`). It does NOT cover the frames just after a resume: they fall
  inside the guard's 120-frame warm-up
- `.\tools\AllocTest.ps1 -Exit` - on crypt1, inside the window: a click on the
  log's Help button (its movement-keys line allocates nothing and is excused
  nothing - a line the player reads is not reporting), a step onto the exit
  stair beside the start whose "Leave?" is answered No (the frame that opens a
  prompt over play is disarmed and none is armed while it is up), then a step
  into a pit (`stairadd`) that falls to crypt2 - the step and the plunge are
  armed frames. It refuses a PASS unless the verdict counts the prompt
  (`prompts=`), a Help press (`helps=`) and a pit step (`falls=`) - the last two
  only in MEASURED frames (armed to the end of their Update, inside the window),
  since the harness can only time its sends and a click in the warm-up checks
  nothing
- `.\tools\AllocTest.ps1 -Sheet` - the character sheet: hover (the status bar),
  every tab, a right-click opening the item details dialog, the use menu. It
  refuses a PASS unless `itemdetails status` counts an open made in the window
- `.\tools\AllocTest.ps1 -Items` - an item moved pack -> cursor -> floor ->
  cursor -> pack through the party inventory window, in eval_arena. The
  measured item is a kind never dropped before the window (a kind's first drop
  is paid by every kind, so it is not warm-up). It refuses a PASS unless the
  window's tally counts two drops and two lifts (`drops=`/`lifts=`). Before
  the game it runs `itempose.eval` headless: no floor glow from a rune in a
  shut niche and, in an open one, a glow over the rune in the pocket; and
  `pickprobe` hits every click target where it is drawn (a failure is the
  result PICKS)
- `.\tools\AllocTest.ps1 -Packs` - a 4-slot and an 8-slot bag swapped in the
  sheet's pack row, so a bag GROWS inside the window. It refuses a PASS unless
  `sheet status` counts two equips made during it (`equips=`)
- `.\tools\AllocTest.ps1 -OnHitTypo` - the party swinging clubs at a frozen
  skeleton, the club's `on_hit` naming no effect (`onhit club brun 3 6`, in
  memory), so every landed blow WARNS inside the window - the warning must
  excuse its own formatting. The swinging is held (`autoattack hold`) until the
  window's first armed frame; it refuses a PASS unless a warning landed inside

Every mode also fails on an `AI pool grew:` line anywhere in `dungeon.log`. The
AI's snapshot and walkability-grid pools are filled at level load to as many
buffers as can be in use at once, so a pool that grows in play is a defect even
outside the window; it used to grow in whichever guarded frame the thread
scheduler happened to pick. The same line reports any growth of what lockstep's
inline compute uses - its brain's search scratch, its plan batches and their
paths - which is sized at level load (and when a monster is added) as well.

## Reading a failure

`alloctest RESULT=FAIL` prints the violating call sites with symbolized stacks,
also in `dungeon.log`. Each **unique** stack is reported once per session, so a
standing violation cannot drown the log — a frame repeating a known stack stays
silent. Two lines say what a stack cannot: `... no stacks captured` (a
violation with nothing to show, at 1, 10, 100... such frames), and, after 64
distinct sites, one `allocation guard: the stack set is full` - a stack it does
not hold is then counted (`allocguard`, the shutdown totals), not logged. That
count is of CAPTURES, not sites: one site still allocating every frame adds one
a frame, so a big number there is not a big number of sites.

A violation during an event is a violation. There used to be a policy here that
event frames were reported but not asserted on; it was a rationalisation of a
defect (`loc::Tr` copying text the table already owned), and its real cost was
that a guard firing during ordinary play teaches you to ignore it. If an event
allocates, fix the event. Something firing events every frame is a separate,
MESSAGE-RATE problem, visible in the log on its own terms.

`alloc::Excused` is not a way out for gameplay. It is for paths that are allowed
to allocate inside an otherwise steady frame - a dev-console command, an editor
dialog, a first-time bake - and for reporting code. `log::Write` and its
templates excuse their own formatting; a reporter excuses only what it builds
before the call (a formatted argument, a console `Print`). Reporting is
DEV-facing output: a line the player reads never is (code-review C217).

One thing that looks like a bug and is not:

- **Debug allocation counts are not release counts.** MSVC iterator debugging
  makes `vector`'s move constructor allocate, so growth copies rather than
  moves. Do not compare a debug number against a release one.

The self-test arms `allocpoke once`, which allocates exactly once, on the
window's first armed frame (the first after a disarm, which captured no stacks
until code-review C214). It requires the run to come back FAIL AND
`dungeon.log` to name the poke's call site (`Game::AllocPokeOnce`).
