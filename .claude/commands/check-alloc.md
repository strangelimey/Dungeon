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
- `.\tools\AllocTest.ps1 -Effects` - the party bar's effect strips GROWING: the
  four schools' party wards, held (`autocast hold`) until the window's first
  armed frame, land on every member inside it, each strip going from no icon to
  four. A panel builds its whole icon pool when it is made (code-review C219);
  it used to build an icon the first time a member showed N effects, and again
  after every HUD rebuild. It refuses a PASS unless the verdict's
  `effectsrose=` shows every member's count rose in the window. `-Minimal` runs
  it on the party cards; `-Party 'premade=0' -GrowRoster` starts with a party of
  one and then `newparty default`, so the roster GROWS before the window - a
  grown member's effect list is reserved again after the copy (code-review C228).
  -GrowRoster is refused without -Effects, the one mode that uses those lists
  inside the window and demands it
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
- `.\tools\AllocTest.ps1 -Lever` - eval_arena's levers' corner, inside the
  window: crypt_token lifted off 1,21, put down, lifted and put down again (the
  first lift moves the sunken_relic quest on and reveals crypt_back - new world
  state; the second changes nothing), two turns to face the levers, a click on
  the lever at 1,22 - wired to no niche, `sets=relic_lifted` by `flagwire` (a
  new flag) - a step east and a click on the one at 2,22, whose press reveals
  the secret niche at 3,22. The world state's lists keep spare entries with
  room, sized from the catalogs, and the hooks read views (code-review C212). A
  press used to build a vector of the niches it touched, and a reveal rebuilt
  the walls round it in play (a drain, a build, an upload); the press now counts
  and the reveal swaps in walls pre-built at the load (code-review C211). It
  refuses a PASS unless the verdict counts, in measured frames, a press that
  flipped no niche and one that flipped one (`levers=`, `niches=`), and fails as
  WALLS unless afterwards the niche is open, the walls on show match a fresh
  bake's layout (`geomhash`) and moved, and both chunks the niche reaches have
  the revealed look on show with no press rebuilt in play (`niche looks`). The
  layout sees the niche's own chunk; the one east of it changes only a panel's
  pin, so its swap is judged by the look `niche looks` says is on show. It also
  refuses a PASS unless the window's tally counts two lifts and two puts of the
  token, and fails as HOOKS unless afterwards the quest is at `heard`, crypt_back
  is discovered and relic_lifted is on (each off before the window)
- `.\tools\AllocTest.ps1 -Sheet` - the character sheet: hover (the status bar),
  every tab, a right-click opening the item details dialog, the use menu. It
  refuses a PASS unless `itemdetails status` counts an open made in the window
- `.\tools\AllocTest.ps1 -Sheet -AllSpells` - the LONGEST Known Spells list
  (code-review C220): Sera learns every spell before the window (`learn 1 all`),
  and each cycle pages the sheet to her on Known Spells, opens the party window
  there (her card with it), and comes back through Brand's portrait. The sheet
  warmed 32 spell rows for a registry of 44; it warms the registry's size now.
  Only the FIRST bake of her list on each can grow it, so it refuses a PASS
  unless the verdict's `spellrows=` says the sheet's and her card's first bake
  of every spell taught both ran in MEASURED frames (armed to the end of their
  Update, inside the window) - a first bake in the warm-up checks nothing
- `.\tools\AllocTest.ps1 -Items` - an item moved pack -> cursor -> floor ->
  cursor -> pack through the party inventory window, in eval_arena. The
  measured item is a kind never dropped before the window (a kind's first drop
  is paid by every kind, so it is not warm-up). It refuses a PASS unless the
  window's tally counts a drop and a lift for EVERY cycle sent (`drops=`/
  `lifts=`), so the first cycle cannot have beaten the window. Before
  the game it runs `itempose.eval` headless: no floor glow from a rune in a
  shut niche and, in an open one, a glow over the rune in the pocket; and
  `pickprobe` hits every click target where it is drawn (a failure is the
  result PICKS)
- `.\tools\AllocTest.ps1 -Items -LongId` - an id LONGER than MSVC's 15-character
  small-string buffer (code-review C218): inside the window the runes are
  SWAPPED through the cursor into a third, free slot, and then
  potion_antidote_minor (21 characters, laid ahead before the window) is lifted
  off the floor and put back. Only the FIRST cycle measures: its swap leaves the
  cursor holding a buffer no long id ever passed through, while the later ones
  hand it buffers the setup or that first lift already grew. Every slot, the
  cursor and the scratch ids are born with room for 31 (`kItemIdCapacity`). It
  refuses a PASS unless the window's tally counts a lift and a drop for EVERY
  cycle sent (only the potion touches the floor), which puts the first lift
  inside the window, and the runes stand where the cycles that ran leave them
- `.\tools\AllocTest.ps1 -Packs` - a 4-slot and an 8-slot bag swapped in the
  sheet's pack row, so a bag GROWS inside the window. It refuses a PASS unless
  `sheet status` counts two equips made during it (`equips=`)
- `.\tools\AllocTest.ps1 -OnHitTypo` - the party swinging its starting daggers
  at a frozen skeleton, the dagger's `on_hit` naming no effect (`onhit dagger
  brun 3 6`, in memory), so every landed blow WARNS inside the window - the
  warning must excuse its own formatting - and a severe fumble may knock a
  dagger or the torch to the floor there, which copies nothing (code-review C10,
  C212). The swinging is held (`autoattack hold`) until the window's first armed
  frame; it refuses a PASS unless a warning landed inside

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
