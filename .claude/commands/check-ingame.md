---
description: Level files, installed models, and a UI overlap sweep of every screen
argument-hint: "[selftest]"
allowed-tools: PowerShell, Read, Grep, Glob
---

Audits that need a running game, sharing one launch (a few minutes).

- no argument → `.\tools\CheckAll.ps1 -Only ingame`
- `selftest` → `.\tools\CheckAll.ps1 -Only ingame -SelfTest` (a real sweep with
  three named faults - a refused open step, a missing label, the world map's
  word faces switched off before their audit - and exactly those three checks
  must fail)

## What it is guarding

**`levelcheck`** - every level file present, every model a catalog type or a
level's palette loads installed. The baked pool is gitignored, so a fresh clone
or a stale worktree provision has entries whose assets are absent. Scoped to
**models** on purpose: a missing texture renders magenta and is survivable, a
missing model is a `LoadModelOrDie` that takes the process down at level load -
possibly on a level nobody has visited in weeks. A model is checked as the FILE
its loader opens, through the loaders' own resolver (`ModelFileOf` /
`WornBlockFile` in AssetUtil): whichever of .gltf / .glb is installed (the
category's own first - code-review C301), the id when an entry names no `model`,
a fixture's `empty_model` / `part2_model`, a door's `trim` naming no doors.cat
entry (its loader opens the name as a file), and every palette's worn blocks at
every mesh tier (code-review C441). Then five MUTATIONS (`levelcheck mutate
empty_model|part2_model|id|trim|worn`) each plant one of those faults in what
the check reads - never in the files - and each must FAIL with exactly one more
model missing, naming the planted file. The command picks what it plants on its
own, apart from the check's walk, so a check that stops reading a field or a
tier comes back PASSED rather than refused. Two CONTROLS (`levelcheck mutate
glb|gltf`) plant an entry that LOADS - a decoration naming a .glb-only model, an
item a .gltf-only one, each under the extension its loader does not prefer - and
must come back as the real run did, with the planted file the one the walk
resolved (`resolved=`). It also NAMES every
albedo in the pool with no `_n` normal map at its resolution (`missing_normals=`
on the verdict line): such a set draws flat with one warning, so it is counted,
not failed.

**`uioverlap`** — CLAUDE.md says run it after touching any screen, and the one
manual sweep found four defects nobody had reported. It sweeps every screen a
console command can open - the HUD, pause, both maps, the editor and its dialogs,
the sheet and the party window, the party creation page and its picker, the
generator and new-world dialogs, the pause menu and the sheet over the world map,
short parties of three and one, and a hand box's use menu with its Combat /
Magic groups in a 720p window (code-review C382) - each under its own
`sweep_<name>` label (InGameTest.ps1 lists them). One row's audit must NOT be
clean: `sweep_worldwords` draws the world map's buttons as their no-art word
faces (`worldview words on`) under ru.lang, and that hand-drawn chrome reports
each face it cut (`inspect::NoteChromeTrim`, code-review C224) - only such trims,
at least one, each quoting whole UTF-8 characters and the "..".

**`backdrop`** - the pause menu and the sheet opened from the WORLD MAP draw the
world map behind them and skip the 3D pass; they used to draw the parked dungeon
(code-review C365). `backdrop` logs what Render drew - recorded in the switch
case that drew the picture, so a frame that drew none reads `over nothing` - and
it is those sweeps' status: `paused over worldmap - scene skipped`, `sheet over
worldmap - scene skipped`, and the control, `sheet over playing - scene drawn`.
It is one more reason this check runs WINDOWED: a headless run renders nothing.

**`thumbnails`** - the asset picker's texture tiles and the editor palette's
surface swatches draw as bright as their files (code-review C158): an image the
sprite batch draws through an sRGB view comes out far darker, and every tile and
swatch once did. `assetpicker survey` / `editor palette swatches` print where
each image was drawn and its file's STORED mean; the run photographs the window
(PrintWindow, by PID) and sets each one's drawn mean beside it (within 0.05
luminance; at least 4 measured, 2 of them 0.2 or brighter so a darkening could
not hide). And no tile ON SCREEN is evicted (C111): each picker runs under a cap
FORCED to 8 (`thumbcap 8`), below what it shows, scrolled half way so eviction
must run - nothing in view blank, no key loaded twice, something evicted (or the
cap proved nothing), and NOTHING MORE evicted after a settle with the view still.
A tile whose load found no image (`missing=`: a portrait the worktree lacks - the
bought pack is gitignored, and that is a valid state) is a `[note]`, not a
failure. And THE HEAP LINE holds: the asset picker under a line forced 4 SRVs
above what is live (`thumbcap heap <n>`), scrolled to a screen it must load
again, stops at the line (`heaptop=` never past it, `refused=` at least 1) and
still loads every tile in view by evicting what is off screen.

## Reading a failure

**`MISSING MODEL '<file>' (<field>) of type '<y>'`** or **`MISSING WORN MESH
'<file>'`** - the pool is incomplete. Usually means a worktree was provisioned
from a stale file list; re-run `tools\FetchModels.ps1`, or robocopy
`assets\models` from a populated sibling. A missing worn mesh is the texture
import's bake (`AssetBaker models`). The name is missing under BOTH extensions
(the loader takes either, so the file named is only the one it would try
first). A `(trim)` miss is usually the catalog entry, not the pool: the door
type's `trim` names no doors.cat entry (a typo, or a trim entry renamed away),
and the loader opens the name as a file.

**`NO LOADER RULE for '<field>' of type '<y>'`** - a catalog no loader reads a
model from carries a model field: a new loader the check's table lacks (add it,
with its family), or a field that does nothing.

**`levelcheck mutate <case>: PASSED`** - the check no longer reads what that
mutation planted (a field, the id fallback, a door's trim, a tier), so the kind
of entry it stands for would pass and then abort a load. The command chooses
what to plant BEFORE the check walks and apart from it, so a check that stops
reading a field, a tier or the worn pass lands here, never in the refusal below.
**`never reported (refused?)`** - the world had nothing to plant it in (no
fixture with that field, no door with a trim, no .glb-only or .gltf-only model,
no level palette, no tier other than the live one), or the file it chose is not
as the case needs it (a catalog case's file installed, a control's or the worn
case's file not installed); the refusal says which.

**`levelcheck control <case>: the check resolved '<x>', not the planted <y>`**
or **`... the planted <y> loads`** - the check no longer opens a model under the
extension its loader does not prefer, the rule C301 brought in: it would fail a
type the game loads (an editor-imported weapon is a .gltf). The check and the
loaders share `ModelFileOf`, so this usually means that resolver changed.

**`NO NORMAL MAP <set>_<res> - it loads flat`** (not a failure) - the set was
copied or imported without its `_n` maps, so it draws with no relief or
parallax. Copy the set's `_n` files from a populated sibling, or re-import it.

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

**`sweep_worldwords: the word faces were not cut and reported`** - no trim came
back (the faces drew whole, or a cut went unreported), a finding other than a
chrome trim landed, or a quoted face holds U+FFFD: it was cut part-way through a
UTF-8 character, the old byte-at-a-time trim's defect (WorldMapView must fit
through ui::FitText).

**`<label>: found overlaps`** - a real layout defect. The findings name both
widgets; `uitree dump <context>` in the dev console gives the pixel rects.

**`<tiles|swatches> did not draw as bright as their files`** - a texture the
sprite batch draws was loaded sRGB (`LoadTextureThumb`'s default is linear for
that reason; AssetUtil.h). The pictures are under `build\<cfg>\bin\shots`
(`ingametest-assetpicker.png`, `ingametest-palette.png`). **`... evicted what
it showed`** - a `reloads=` or `blank=` past 0 under the forced cap: the owner
no longer `Keep`s the tiles in view before `Evict` (ThumbCache.h). **`... more
evicted with the view standing still`** - a draw marks tiles it does not show
(the asset picker's tiles draw only `InView`), so the cache makes and drops the
off-screen ones every frame, each time a GPU drain. **`the heap line did not
hold`** - a load went past `ThumbCacheKnobs::kHeapLine` (`heaptop=` over the
line: running the SRV heap out is an abort), or a starved cache made no room
from off screen (`blank=` past 0: Evict no longer bounds the cap by the room
under the line).

The **settings page is not swept** - it needs a mouse click, and a scripted
click against a moving layout is how a sweep starts silently auditing the wrong
screen. Nor are the sheet's hand-drawn bars: uioverlap audits widgets, not
direct draws. The run prints both every time.
