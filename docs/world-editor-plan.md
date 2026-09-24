World editor — the plan
=======================

Michael, 2026-09-09: *"We need a way to edit the 'world' properties. Then we
need to be able to create/edit/delete dungeons in that world. Then we need to
pick a dungeon to edit. This latter will be the current editor experience."*

This is P7, which was deferred a day and then asked for with a shape. Written
against what the editor already does, not against a memory of it.

The shape
---------

**The editor should mirror the data.** The world tier gave the project three
levels of nesting; the editor still shows one, a flat list of every level stem
in a dropdown. What he is describing is the same hierarchy, walked downward:

    the WORLD          its terrain, its locations, where the game starts
      a DUNGEON        its levels, its name — created, edited, deleted here
        a LEVEL        the editor exactly as it is today

The last line is the important one: **the level editor does not change.** Every
brush, dialog, inspector and undo step keeps working, and what is added is the
two tiers above it plus a way down.

### Most of this already exists

Three mechanisms carry most of the weight, and the plan is largely a matter of
pointing them at the new tiers:

- **A non-placeable catalog category.** `effects` is already a palette category
  with `placeable = false`: clicking a row opens the TYPE EDITOR instead of
  arming a brush, and it offers no "+ New..." because an effect needs a class.
  `dungeons`, `terrain` and `quests` are the same shape — except they DO want
  "+ New...", because a dungeon is pure data.
- **`TypeEditorDialog` renders from a SCHEMA.** `CatalogSchema.h` is a FieldSpec
  table per catalog; sections become tabs, kinds become widgets, and "?"
  explains them. Exposing a dungeon's `levels` and `tags` is a table, not a
  dialog. Rename and delete come with it, including the reference sweep.
- **`LevelSettingsDialog` is the per-level settings modal**, opened from a
  toolbar button for the level being viewed. The world's own properties want
  exactly that dialog one tier up.

So the genuinely new work is: a WRITER for the world, an EDIT MODE on the world
screen, and a NAVIGATION change from a flat level list to dungeon-then-level.

Phases
------

**W1 — the world can be written.** DONE (2026-09-09). `WorldMap::Serialize`
returns TEXT and touches no file, so it can be diffed and round-trip-checked
without a disk; `Game::SaveWorld` writes it and READS IT BACK, warning if the
two differ — because the failure mode is the nastiest kind, an in-memory world
that stays correct all session with the damage only showing on next launch.

`saveworld` writes the world ALONE, and it exists because using `savemap` to
test the world writer rewrote every level too and stripped the authoring notes
off eval_arena. A command that does one thing can be used to check that one
thing.

The original entry: `world/world.map` has no writer: it is
hand-authored, and every phase below is pointless until an edit can survive the
session. A writer in the shape of the `.map` writer (records, then grid), and
`savemap` learns to include it. `project.ini` already round-trips.

DO THIS FIRST even though it is the least visible: editing without saving is a
demo, and a demo is what gets built when persistence is left till last.

**W2 — dungeons, terrain and quests become catalog categories.** DONE
(2026-09-09), except the DELETE half, which needs W3's mutable world (see
below). Three palette rows, three schema tables, and a third category property:
`authorable`. `placeable` was not enough of a distinction — Effects is
non-placeable AND uncreatable (it needs a class), while these are non-placeable
but pure DATA, so they get "+ New..." and skip the asset dialog entirely. A
dungeon has no texture to import.

Creating one generates an id and opens the TYPE EDITOR, whose title is already a
click-to-rename affordance with the sweep behind it — one naming mechanism
rather than two.

THE SWEEP NOW SEES THE WORLD, and the measurement says why it had to: the crypt
has TWO references and ZERO of them are in any level. A sweep that only walked
levels would have called it safe to delete.

Two things it does NOT do yet, both honest rather than overlooked. RENAMING a
dungeon a location names is REPORTED and refused rather than applied — the
loaded world is const and W3 is what makes it mutable. (UNTRUE, found in W10:
nothing ever refused it — `RenameType` ignored the count, so the rename went
through and the doorways dangled. W11 applies it.) And TERRAIN is not swept
at all, which is a property of the format: the grid names a terrain by its
GLYPH, so renaming the id cannot orphan a cell. That is what the glyph is for.

The original entry: One
`kCategoryInfo` row and one `CatalogSchema` table each, following `effects`.
That buys create, edit, rename and delete through machinery that already exists
and is already checked.

THE ONE PIECE THAT IS NOT FREE is the reference sweep. `SweepTypeRefs` walks
levels and the cross-catalog fields; it knows nothing about world LOCATIONS,
which now name a dungeon (`dungeon=`), a level (`level=`) and — for an exit
stair — a location. Renaming or deleting a dungeon has to reach them, or the
sweep's promise ("a delete REFUSES while anything still references the type")
quietly stops being true at the tier that matters most.

**W3 — the world screen gets an edit mode.** DONE (2026-09-09) for the MODE,
the TERRAIN BRUSH and the UNDO. Locations and areas move to W4: areas are a
LIST (his answer), which is dialog work rather than brush work, and a location
wants the same dialog to edit what it points at.

The world joins the editor's ONE history by the same borrowing `m_roster` does
— `DungeonWorld::SetWorldForUndo` takes a pointer, the snapshot copies it, the
restore puts it back — so the world stays Game's and a step spanning tiers
undoes as one thing. Nothing else in DungeonWorld reads it, and the comment at
the setter says that a second use is the moment to ask a harder question.

TWO THINGS THE BUILDING TAUGHT. `SetTerrainAt` returns "I found that terrain and
set it", NOT "something changed" — so trusting it put NO-OP UNDO STEPS on the
stack, and the next Ctrl+Z spent itself taking back nothing. The mouse path had
the guard and the console path did not, which is exactly the shape of bug two
paths to one action produce. And the snapshot still has to be AGGREGATE
INITIALIZED: DungeonMap's default constructor is private (it exists for
FromText), so it cannot be default-built and filled in field by field.

The original entry: `WorldMapView` already draws the
world; this gives it brushes, on the MapView/MapEditor pattern: a terrain paint
(the palette's Terrain category arms it), location place/move/delete, and area
rectangles. Undo through the existing snapshot idiom.

It belongs on `WorldMapView` rather than as a third `MapView` mode. MapView is
built around a `DungeonMap` — its docks, variant grids, stair pairing, fog and
level browsing are all level concepts — and P3 kept the two apart deliberately.
Bending it to a grid whose cells are terrain kinds would cost more than the
drawing code it would save.

**W4 — the world's own properties.** The MODEL and its rules are DONE
(2026-09-09): the world start, the game's opening, the harness level, areas
(add/delete/REORDER) and locations (add/delete/move/retarget), each bracketed
as its own undo step and each refusing what the LOADER would refuse — a
duplicate id, an occupied cell, impassable ground. An editor must not be able
to author a world the checker rejects a moment later.

`worldarea at <x> <z>` reports which area OWNS a cell. That readout exists
because the ordering rule was otherwise unobservable: a list can show the
order, but only this shows that the order DID something — the same cell reading
0.12 from lowlands, then 0.90 from a newer overlapping row, then 0.12 again
once that row is moved to the front.

THE MANIFEST IS NOT UNDOABLE and the command says so rather than pretending.
The editor's history snapshots the world and the levels, not project.ini, and a
half-undoable dialog would be worse than an honest one.

THE DIALOG IS DONE TOO (2026-09-22). `WorldSettingsDialog` is three tabs, and
they are the three console command families wearing a face: World (the start
cell, the game's opening, the harness level), Areas (the table, in FILE ORDER,
with arrows that reorder it) and Doorways (the list, and a form for the selected
one). It is opened by the world screen's new toolbar band — Settings / Undo /
Redo / Save, the level editor's one-list idiom with four tools instead of ten —
and by RIGHT-CLICKING a doorway, which opens it on that doorway.

THE DIALOG PROPOSES AND THE OWNER DISPOSES, the split WorldMapView's onPaint
already made: it holds a borrowed `const WorldMap*` and re-reads it on every
rebuild, and every change leaves through a callback that returns whether it was
allowed. A refused edit is therefore one the table visibly does not show, and no
working copy exists that could disagree with the world about what happened.

THE REFUSALS MOVED INTO `WorldMap` to make that true — `SetStart`, `AddArea`,
`RemoveArea`, `MoveArea` — because the dialog is a SECOND way in and a rule
living in only one of them would be two editors wearing one name. The console
commands now report those decisions rather than making them.

FOUR THINGS THE BUILDING TAUGHT, none of them foreseen:

- A TOOLTIP POINTED INTO A TEMPORARY. `for (const ToolButton& b :
  ToolbarButtons(panel))` keeps the returned vector alive only to the end of the
  loop, and the hovered entry was read after it — the first hover faulted. The
  level editor's band holds the list in a named local, which is why it never
  did.
- A REBUILD TAKES THE FOCUS WITH IT. Showing the status note by rebuilding the
  form meant the field being typed in stopped existing after its first digit, so
  a two-digit cell could not be entered — and worse, the digits were judged
  separately, so typing 12 offered 1 first and a refusal bounced the field back
  before the 2 arrived. The note is written into its Label in place now, and
  nothing mid-edit is corrected.
- AN ID THAT EDITS ITSELF CANNOT BE CAPTURED BY VALUE. Every callback in an area
  row looked the row up by the id it was built with; renaming "a" to "moors" is
  five renames, and the second looked up a name that no longer existed. The row
  shares one `shared_ptr<std::string>` that each accepted rename updates.
- ONE SENTENCE FOR TWO REFUSALS HID ONE OF THEM. `worldarea add` printed
  "refused: duplicate id, or no extent" for both rules, and the harness check
  for the duplicate rule PASSED with that rule deleted — the case beside it
  printed the same words. Each refusal names its own rule now. The reporting is
  the decision's alibi, so it has to be as specific as the decision.

Also found and fixed here, both older than this phase: a batch scissored out to
nothing was still submitted, so a dialog left open wrote ten thousand identical
D3D12 warnings into `dungeon.log` (`SpriteBatch::Flush` drops it now — it can
write nothing by definition); and `worldmap on` answered from the TITLE SCREEN,
where the way back out set Playing and the first frame dereferenced a HUD that
was never built.

The original entry: A `WorldSettingsDialog`, the LevelSettings
one tier up, holding what belongs to the world rather than to any square: the
`start` cell, and the manifest's game-opening fields (`start_dungeon`,
`start_level`, `start_x`, `start_z`) plus `eval_level`.

That dialog is where "where does a new game begin" becomes editable rather than
a hand-edited ini — and it is the natural home for the answer to a question the
world tier raised and never surfaced in a UI.

**W5 — pick a dungeon, then a level.** DONE (2026-09-22). The toolbar's level
dropdown is two-tier: a row per dungeon, expanding to its levels, and the `[+]`
button creates the new level INSIDE the dungeon being viewed — named after it
too (crypt1, crypt2, crypt3), so the grouping is legible in the filename the
way the demo's levels were already hand-named.

`project.levels` still holds every stem — it is the editor's universe and the
checker's — but it has stopped being the thing the UI presents. The grouping is
`Project::DungeonLevels` / `DungeonOfLevel` / `OrphanLevels`, one home, because
the picker, the world-settings dialog and the checker adapter all ask the same
question and three answers could disagree.

THE ORPHANS ARE LISTED, last, under a group called "no dungeon". A stem no
dungeon claims is a checker WARNING, not a level that should become unreachable
by being forgotten — and the picker is the only way to open one.

The popup is ONE ROW LIST that hover, click and render all walk, which is the
toolbar's own idiom brought inside it: before this each of the three walked the
flat stem vector separately, and a two-tier list with three copies of the tier
logic would be three chances to disagree about which row is which.

### What it cost, which was not the picker

The `[+]` button saves the project, and that made an old defect routine: the
manifest WRITER rebuilt project.ini from scratch, so **every comment in it was
deleted on every save** — what the level list is, why eval_arena is on it, what
removing the four opening lines does. The catalogs had been fixed for this long
ago (`serialize::Block::lead`); the manifest never was, and CLAUDE.md's
"project.ini already round-trips" was simply untrue. Project now keeps the block
it parsed and updates fields inside it, `serialize::Remove` exists so a field can
be UNSET rather than blanked (an absent `start_x` is -1; a blank one is 0, which
would land a new game on a row it was never sent to), and blank lines between
field groups survive the trip.

THE FIX IS CHECKED, and the check is the interesting part. `catround` asks the
REAL WRITERS — `Project::ManifestText`, `Catalog::Serialize`, both split out as
pure text (the `WorldMap::Serialize` shape W1 established) — for what they would
write, and diffs it against what is on disk. Three things it taught while being
built, each of which had made it report a clean run it had not earned:

- CHECKING THE PRIMITIVE IS NOT CHECKING THE WRITER. The first version diffed
  ParseBlocks→WriteBlocks, which does not include the header line each writer
  prepends — so it called an empty catalog broken and would have missed a
  writer-level bug entirely.
- A SKIPPED FILE IS NOT A PASSED FILE. `project.ini`'s path had lost a
  backslash, the read failed, and the early return said nothing: it reported
  "23 of 23" while never once looking at the file the whole fix was about.
  Absences are counted and named now.
- A FIDELITY CHECK MUST RUN BEFORE A SAVE, not after. Placed after `levels new`
  it compared the writer with its own output, which agrees with itself however
  much it drops — deleting the blank-line rule was invisible from there.

**W6 — the player's map gets the world too.** DONE (2026-09-23), and it is the
first of these phases that is not about the editor at all. Michael: *"when the
player brings up the regular map (in a dungeon), there needs to be a button to
toggle between world map and dungeon map."*

So the M-map has two PAGES. A fresh open shows the dungeon — where you are —
and one button flips to the overworld with its fog, the party's world position
and the locations it has found. Outside there is no second page and no toggle:
the world map is already the whole screen out there, which is what his "that
only makes sense when they are in a dungeon" amounts to once the travel screen
is the thing you are standing on.

NEITHER VIEW LEARNED TO DRAW THE OTHER. Each offers the way ACROSS and Game
does the flip — MapView's own header already argues for that split ("bending it
to a grid whose cells are terrain kinds would cost more than the drawing code
it would save"), and this is that argument cashed. The two buttons sit on the
SAME PIXELS, top-left of the grid, so the pair reads as one control that stays
put rather than two that swap places; the right edge could not do it, because
the key dock lives there and its width follows a persisted collapse flag.

THE PAGE IS DERIVED, NOT LATCHED — `ShowingWorldPage()` asks whether the
overlay is up rather than trusting something to have said so. A stair fires
`Close()` from code that knows nothing about pages, and a remembered page would
have left the TRAVEL screen offering a way back to a dungeon the party had
already left.

TWO THINGS THAT WOULD HAVE BEEN LIES. The world page's hint still read "move
with your movement keys" — which from inside a dungeon you cannot; it takes the
dungeon map's "drag to pan" instead. And the same pass removed the KEY dock
from the player's map (Michael, same session: it is for building, not for
playing), which gave the grid the whole panel and took `mapPlayerKeyCollapsed`
with it — a setting nothing reads is worse than no setting, because it sits in
everyone's settings.ini looking like it still does something.

Dev: `mappage | open | close | dungeon | world` — the M key and the toggle
without a keyboard, reporting the page, whether the toggle is OFFERED, and
whether the map is open, because those are three different facts.

**W7 — several worlds, and a way between them.** DONE (2026-09-23). Michael:
*"the reason I wanted to add this world map stuff is so I can create a whole
new world, add dungeons, etc., and then switch back and forth between them.
This will be used in future for the test harnesses so we can have specific
scenarios for the test."*

A WORLD IS A PROJECT (his answer, asked because the two readings led to very
different work): a self-contained game under `assets/projects/<name>` with its
own overworld, dungeons, levels, catalogs and opening. The alternative — several
world maps sharing one project's content — was rejected because scenarios would
share a content pool: every scenario's levels piling into one manifest, two
worlds fighting over "where the game begins", and a dungeon reachable only from
world B reading as unreachable while world A is loaded.

SWITCHING RELAUNCHES, which is the adapter change's bargain for the adapter
change's reason: the level meshes, the surface textures, the catalogs and the
world are all built from the choice at startup, and tearing that down in place
would be a long tail of stale caches for a saving of ten seconds. The choice
comes from three places, each answering a different question — `-project <name>`
for ONE run (the scenario interface: the harness opens a world without touching
your settings), `settings.ini project=` for the one you switched to, and
`dungeon-demo` otherwise. A name that no longer resolves falls back rather than
aborting: a settings file naming a deleted world must not make the game
unlaunchable.

A NEW WORLD COPIES CONTENT AND NOT PLACES (his answer again): every catalog
comes across, so you can build in it at once, while dungeons and quests start
empty and the overworld is blank. Three things that only showed up in the doing:

- IT NEEDS SOMEWHERE, because `DungeonWorld`'s constructor loads a level. A
  world with nowhere at all in it cannot stand up, so a new one gets one room,
  in one dungeon, behind one doorway — the smallest starter that both loads and
  passes the checker.
- AND IT NEEDS A HARNESS LEVEL. A world with no `eval_level` opens the harness
  on the WORLD MAP, where half the dev commands refuse because the party is not
  in a level. Creating one names its starter room, so a new world is usable as
  a scenario the moment it exists.
- THE CHECKER CAUGHT THE COPY ON ITS FIRST RUN. Copied items carry `quest` and
  `reveals` hooks, which name a quest stage and a world location — progress and
  places, both of which had just been cleared. Three errors, in a world that had
  existed for four seconds. Content comes across; what content POINTS AT does
  not.

Dev: `worlds | new <name> | load <name>`. Note that `worlds load` must not
appear in an eval script — it relaunches, which would end the run mid-script;
opening a world for a test is the `-project` flag's job.

**W8 — the worlds have a face.** DONE (2026-09-23). W7's three verbs were
console-only; they are now the world toolbar's LEFTMOST disc (a globe — apart
from the other four in meaning, since every other disc acts on THIS world and
this one is the way to the others). `WorldsDialog` lists the worlds on disk, the
running one marked and offering no button, with a name field and Create below.

It calls the SAME `CreateWorld` / `SwitchWorld` the console does, so the two
cannot disagree about what a name may be or what switching means. Two things
were decided rather than inherited:

- OPEN IS ARMED. The first click on a row's Open turns it into Relaunch and says
  what the second click will do (and that anything unsaved is lost); a click on
  a different row moves the arm instead of switching; Esc disarms before it
  closes. It is one of only two buttons in the editor undo cannot reach — the
  type editor's Delete makes the same bargain.
- A CREATE ARMS THE NEW WORLD. Making one is nearly always the first half of
  going there, so its row comes back as Relaunch. The duplicate and the empty
  name are refused by the dialog with their OWN sentences before the owner is
  asked (the owner's single "" would have been one sentence for two mistakes —
  W4's lesson).

It also fixed a W7 defect the dialog made reachable: `SwitchWorld`'s "already
there" compared against the SETTING, not the world RUNNING, so a `-project`
scenario could not switch to the world settings.ini already named — it returned
success and did nothing.

Dev: `worlds dialog [open|create <name>|off]` — the rows' own calls, for a
harness. The same rule as `worlds load` applies: a SECOND `open` of one row
relaunches, so a script checks the arm and never makes it. WorldTest phase 15.

**W9 — deleting a world.** DONE (2026-09-23). Michael: it should work *"like
deleting a repo in GitHub: you have to enter the name of the world (case
sensitive) as confirmation."* A row's Delete opens a CONFIRMATION VIEW inside the
Worlds dialog: what goes (read off DISK — its levels, its dungeons, the
overworld, every catalog), that it cannot be undone, the name to type, and a
Delete button that stays DISABLED until the typed text equals the name exactly —
not trimmed, not case-folded. Esc / Cancel go back to the list. `ui::Button`
gained `enabled` for it (a disabled button still claims the pixels it paints).

The RULES live in `Game` (`WorldDeleteRefusal`), not the dialog, because the
console reaches them too (`worlds delete <name> <name>` — the typed confirmation
in its console form). Two refusals, each its own sentence, and asked BEFORE the
confirmation opens so it never asks you to type out a name it will then refuse:
the world RUNNING (its files are the ones on screen), and `dungeon-demo`, THE
FALLBACK — a launch whose world is missing lands there, so without it a launch
cannot stand up. The fallback row offers a space, not a dead button. The delete
itself checks the path it is about to `remove_all` (directly under the projects
root, holding a project.ini) rather than trusting how the path was built, and a
settings.ini naming the deleted world is pointed back at the fallback.

**WHAT THIS PHASE FOUND THAT WAS NOT DELETION.** Michael made a world in W8 and
switched into it, so settings.ini named it — and every harness launched without
`-project` then measured HIS world. Its `eval_level` is its starter room, so the
eval suites would have run somewhere other than eval_arena and could have passed
doing it. Now a `-eval` run skips settings.ini (a harness measures a scenario,
not the developer's last choice; one that wants another world says `-project`),
and the four keystroke-driven harnesses (Alloc/Health/InGame/Profile) pass
`-project dungeon-demo`. WorldTest phase 16 carries the CONTROL: it points
settings.ini at a scratch world and demands a harness run still opens
dungeon-demo, restoring the file after.

And the sweep lied once on the way: `uioverlap` after a scripted `delete` click
audited the LIST, because the dialog defers its rebuild (a button fires inside
the tree walk) and its Update does not run while the console is up.
`ApplyPending()` is the console's way to apply it. The same sweep then found a
real overlap — the new Delete column squeezed a world's name under Open.

Dev: `worlds dialog delete <name>` (a row's Delete) and `worlds dialog confirm
<text>` (type it and press the button). WorldTest phase 16; InGameTest sweeps the
confirmation with a scratch world it makes and deletes itself.

**W10 — deleting a dungeon, and its levels with it.** DONE (2026-09-24). The
original plan item, answered on day one (open question 1 below): *delete the
levels too, after a confirmation dialog.* It is the type editor's own Delete on a
dungeon — right-click the dungeon's palette row, as for any type — but where every
other category keeps its two-click arm, a dungeon's gives way to W9's TYPED
confirmation: what goes, BY NAME AND BY COUNT (the dungeon, then one line per
level), that it cannot be undone, and a Delete that wakes only when the id is typed
exactly. The builder is now SHARED (`game::BuildTypedConfirm`, DialogLayout.h) and
the Worlds dialog uses it too — two copies of "enabled when it matches" are two
places for one of them to start trimming.

THE RULES, each its own sentence and all asked BEFORE the confirmation opens
(`Game::DungeonDeleteRefusal`): the dungeon holds the PARTY's level; it is the
game's OPENING (`start_dungeon`, or `start_level` is one of its levels); one of
its levels is the HARNESS's (`eval_level`); a DOORWAY on the world map leads in
(by its dungeon, or by naming one of these levels outright); a level ANOTHER
dungeon also claims; a STAIR from a surviving level leads in (exits skipped —
their dest is a location). The last is the refusal the plan asked for in so many
words: better than deleting and reporting the wreckage afterwards. Run against
the demo it says something true of each dungeon: the crypt is the opening, the
proving ground holds the party, and once the opening moves, the crypt's two
doorways stand in the way.

THE DELETE (`Game::DeleteDungeon`) writes the manifest and the catalog FIRST and
removes files after, so a failure between them leaves stray files nothing names
rather than a manifest naming files that are gone. `DungeonWorld::DeleteLevel`
forgets the level's stashes and dynamic state BEFORE its files — and that order
is the one failure no message would show: a stash left behind is written straight
back to disk by the next `savemap`. The undo history is dropped (it holds copies
of these levels), a viewport browsing one snaps back to the party's level, and
saves that stand in or remember a deleted level are named, as a type delete names
the saves that use a type.

WHAT BUILDING IT TURNED UP:

- **THE STASH CHECK WAS VACUOUS ON ITS FIRST RUN.** With the stash erase deleted
  from `DeleteLevel`, the scenario still passed — because nothing had ever pulled
  the doomed level into memory, so there was no stash to leave behind. The
  scenario now runs a type sweep first (it parses every level), and the mutant
  then reads `saved levels: room1, dungeon11`. The check is that line, not the
  directory: the script's second delete cycle removes the file again anyway.
- A control per rule: every obstacle is put in place, must be named, and is then
  lifted, and the delete must read `allowed` again — the whole sequence of answers
  is compared at once, so a rule firing in another's place shifts it.
- The sweep of the confirmation LOGS that the confirmation opened, because a
  sweep's label alone cannot say which view it audited (W9's lesson); it was then
  shown to catch a planted overlap inside it.

Two things met on the way and left for W11, since they are RENAMES, not deletes:
renaming a dungeon a location names is NOT refused as this doc and W2 said it was
(`RenameType` ignores the location count, so the doorways dangle), and renaming a
LEVEL updates stairs but not `dungeons.cat levels`, `start_level`, `eval_level`
or a location's `level=`.

Level-file operations (save-all, rename, delete) moved to their own file,
`DungeonWorld_Levels.cpp` — `_Editing.cpp` was past 2,200 lines. The dungeon
tier's console commands start one too, `Game_DevDungeons.cpp`. Dev: `dungeons`
(list) / `what <id>` (the answer and the account, doing nothing) / `delete <id>
<id>` / `dialog [<id>|delete|confirm <text>|off]`, and `stairadd <type> <x> <z>
[level]` (a stair pair as one undo step — the only way a script can author the
stair case). WorldTest phase 17 (15 checks); InGameTest sweeps the confirmation.

**W11 — a rename reaches everything that names it.** DONE (2026-09-24). The two
defects W10 turned up, both the same shape: a rename that fixed the references it
knew about and left the rest pointing at a name that no longer existed.

- A DUNGEON rename (`RenameType`, through `SweepCatalogRefs`) now rewrites every
  doorway to it and the game's opening (`start_dungeon`). A doorway named the
  SHORT way — its id is the dungeon's and it has no `dungeon=` — gets the field
  written out, since the location keeps its own id, which names a different
  thing. The world is written at once when the sweep changed it (compared whole,
  not by count).
- A LEVEL rename (`Game::RenameLevel`) now follows the level into its dungeon's
  `levels` (in its place), `start_level`, `eval_level` and every doorway's
  `level=`, and the WORLD is written at once.
- Two more of the same kind, found building the check. OTHER LEVELS' STAIRS were
  repointed only in memory and reached disk at the next `savemap` — but the files
  had already moved, so a session ended in between left every stair into the
  level naming a file that was gone (measured: a fresh launch then reports
  `stairnolevel`). The levels a stair changed in are written now. And an EXIT
  stair's dest is a world LOCATION, not a level, yet the stair rename treated it
  as one: a location spelled like the old stem was silently retargeted to a
  location that did not exist. `RenameStairDest` takes the exit types to skip.
- Each level-rename refusal has its own sentence now (not a stem / no such level
  / taken / the move failed) — the first console form printed one sentence for
  three rules, the W4 mistake.

Checked by WorldTest phase 18 (14 checks) in a scratch world, `wt_ren`, built to
hold one of EVERY kind of reference — a short-way doorway, an explicit `level=`,
the opening, the harness level, a stair from another level, and an exit naming a
location spelled like the level. It reads the files straight after, with no
`savemap` in the script. Both subtle halves were mutation-tested: without the
immediate write the stair on disk still names `room1`; without the exit skip the
exit reads `dest=hall` and the checker reports it. Phase 11's reference count
for the crypt went from 2 to 3 — the opening is a reference, and now counted.
Dev: `dungeons rename <id> <new>`, `levelrename <old> <new>`.

**W12 — a new game asks which world.** DONE (2026-09-24). Michael: *"Start New
Game should show a list of worlds that the user can pick from. If there is only
one, just go straight into that."* The landing page's Start New Game counts the
worlds on disk AT THE CLICK (a world made in the editor since the menu was built
must be offered): one, and it starts as it always has; more, and a world page
opens — the Load page's own list control, a row per world by its manifest
`name`, the folder beside it only when it says something the title does not,
sorted by title ignoring case, Back and Esc to the menu. No delete: that is the
Worlds dialog's, behind a typed name.

A PICK OF THE RUNNING WORLD starts at once. ANY OTHER is the W7 switch — the
choice persisted, so Continue and the next launch follow it — plus `-newgame` on
the relaunch, which the fresh process honours on its first menu frame through the
same callback the entry uses, so it goes on to the game it was started for
instead of stopping at the title. Verified by driving the real menu both ways
(the relaunch arrived in the crypt; the in-place pick kept its process id).

A `-project` LAUNCH OFFERS ONLY ITS OWN WORLD, so the page never appears: the
world is chosen for that run already, and the four keystroke harnesses press
Enter on Start New Game expecting to be playing — a page between them and the
game would have broken every one, and the first world the developer made would
have been the day it happened.

What this does not change
-------------------------

The LEVEL editor. Brushes, inspectors, the type editor, undo, level browsing,
the check button, generate, balance — all of it keeps working on the level you
have picked. If any of that has to change to accommodate the tiers above it,
that is a sign the seam is in the wrong place.

Open questions
--------------

1. **Deleting a dungeon: what happens to its levels?** ANSWERED (Michael,
   2026-09-09): **delete the levels too, after a confirmation dialog.**

   Two consequences to build in rather than discover. First, this is the one
   editor action with NO UNDO — the snapshot history is in memory and the files
   are not — so the confirmation has to say what it is about to destroy by
   NAME and by count, not ask "are you sure?". Second, the REFERENCE SWEEP runs
   first and can refuse: a level of this dungeon may be the far side of a stair
   from a level in another one, and deleting it would leave that stair pointing
   at nothing. Refusing with the reason beats deleting and reporting the
   wreckage afterwards.
2. **Is the world screen reachable in play, or editor-only?** ANSWERED
   (Michael, 2026-09-09): **both.** So `WorldMapView` takes the same shape
   `MapView` did — one view, two MODES — which is the answer the dungeon map
   already arrived at and is worth copying rather than re-deriving.

   Three things follow. FOG is the mode's main difference, exactly as it is
   below ground: Player mode draws undiscovered ground as unknown, Editor mode
   draws the whole world and every location whether the party knows of it or
   not. The WAY IN mirrors it too — the `editor` console command, and Esc backs
   out. And unlike the dungeon map there is no "while it is open the world
   keeps simulating" problem to solve: the world map is an app state that
   simulates nothing, so an edit mode on it is a mode of that state and not an
   overlay over a running game.
3. **Areas: painted, or a list of rectangles?** ANSWERED (Michael,
   2026-09-09): **a list.** So no fifth brush, and no implied "which area owns
   this cell" — a small table of rows (id, x, z, w, h, difficulty) that edits
   the records as they already are.

   Worth keeping honest in the UI: areas OVERLAP legally and the LAST match
   wins, which is a fact about file order. So the table must show them IN ORDER
   and let that order be changed, or the one rule the format has becomes
   invisible in the only place anyone would edit it.
4. **Does the world need its own undo stack**, or does it join the editor's
   existing snapshot history? ANSWERED (Michael, 2026-09-09): **join the
   editor's.** One history, one Ctrl+Z, and a step that spans tiers undoes as
   one thing — which matters because some edits genuinely do span them (adding
   a level to a dungeon touches the manifest, the catalog and the files).

   The cost is that every snapshot grows by the world's records, and the
   existing history already copies every level's editable state per step. That
   is worth watching but not worth pre-solving: the world is a few dozen
   records against maps of hundreds of cells.

All four answered. Nothing above is blocking W1, which is the writer.
