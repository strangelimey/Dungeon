---
description: Level files, installed models, and a UI overlap sweep of every screen
argument-hint: "[selftest]"
allowed-tools: PowerShell, Read, Grep, Glob
---

Audits that need a running game, sharing one launch (a few minutes).

- no argument → `.\tools\CheckAll.ps1 -Only ingame`
- `selftest` → `.\tools\CheckAll.ps1 -Only ingame -SelfTest` (a real sweep with
  two named faults - a refused open step, a missing label - and exactly those
  two checks must fail)

## What it is guarding

**`levelcheck`** — every level file present, every model a catalog type names
installed. The baked pool is gitignored, so a fresh clone or a stale worktree
provision has entries whose assets are absent. Scoped to **models** on purpose:
a missing texture renders magenta and is survivable, a missing model is a
`LoadModelOrDie` that takes the process down at level load — possibly on a level
nobody has visited in weeks.

**`uioverlap`** — CLAUDE.md says run it after touching any screen, and the one
manual sweep found four defects nobody had reported. It sweeps every screen a
console command can open - the HUD, pause, both maps, the editor and its dialogs,
the sheet and the party window, the party creation page and its picker, the
generator and new-world dialogs, the pause menu and the sheet over the world map,
short parties of three and one, and a hand box's use menu with its Combat /
Magic groups in a 720p window (code-review C382) - each under its own
`sweep_<name>` label (InGameTest.ps1 lists them).

**`backdrop`** - the pause menu and the sheet opened from the WORLD MAP draw the
world map behind them and skip the 3D pass; they used to draw the parked dungeon
(code-review C365). `backdrop` logs what Render drew - recorded in the switch
case that drew the picture, so a frame that drew none reads `over nothing` - and
it is those sweeps' status: `paused over worldmap - scene skipped`, `sheet over
worldmap - scene skipped`, and the control, `sheet over playing - scene drawn`.
It is one more reason this check runs WINDOWED: a headless run renders nothing.

## Reading a failure

**`MISSING MODEL '<x>' named by type '<y>'`** — the pool is incomplete. Usually
means a worktree was provisioned from a stale file list; re-run
`tools\FetchModels.ps1`, or robocopy `assets\models` from a populated sibling.

**`<label>: never reached the log`** - the sweep never audited that screen. This
is a *coverage* failure, and it is the more serious kind: it caught its own first
version sending `Esc`/`M` while the console was open, which ate the keystrokes
and audited the HUD three times while reporting four clean screens. Labels match
EXACTLY (`uioverlap [<label>] ---`), so a longer sibling cannot stand in.

**`<label>: its open step never logged ...`** - the audit ran, but the screen's
own status line (`generate dialog: regenerate tab 2`, `sheet: open member 0`...)
never landed before it: the open step was refused or opened something else, and
the audit saw the screen beneath. **`audited in state 'x'`** is the same failure
seen from the app state (a party wiped mid-sweep audits the title from then on).
Every screen in the harness's tables must name a status line; a row without one
is refused before the game launches (`screen '<label>' needs ... at least one
status pattern`), since it would otherwise be judged on its label alone. For
`sweep_handmenu` the status is `handmenu status` seeing the menu LAID OUT (two
groups or more, a submenu with rows, a box with an area, a window under 900
high): the HUD sizes an open menu only while the console and the map are shut,
and an unsized menu is 0x0, which the audit skips.

**`<label>: found overlaps`** - a real layout defect. The findings name both
widgets; `uitree dump <context>` in the dev console gives the pixel rects.

The **settings page is not swept** - it needs a mouse click, and a scripted
click against a moving layout is how a sweep starts silently auditing the wrong
screen. Nor are the sheet's hand-drawn bars: uioverlap audits widgets, not
direct draws. The run prints both every time.
