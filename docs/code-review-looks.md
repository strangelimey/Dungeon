# code-review - for Michael: looks and decisions

What the code-review batches leave for you, kept as they land on the
`code-review` branch. A **look** is something a batch changed that only your
eyes can judge (the plan's "Look: yes"). A **decision** is a question a batch
raised that is yours, not a fix. Each line names the batch and its commit.

## Looks

- **Alt+F4 in Borderless** (batch 16, 9d505911). Settings -> Video -> display
  mode Borderless, Apply, then Alt+F4: the game should close at once - on the
  title screen and during a level load. (`quit` / `exit` in the console also
  work during a load now.) Exclusive the same way, if you use it.
- **A door's shadow** (batch 60, dc1d27c9). With the held torch and the party
  standing still, open a door: the space beyond should lose the leaf's shadow
  as it opens, not on your next step. One way to set it up from the console:
  `goto eval_arena`, `arena corridor 11`, `tp 13 12`, `face east`, `editor
  place doors wooden_door 15 12`, `mappage close`, then click the chain (or
  `opendoor 15 12`). Also watch a skeleton's body vanish at the end of its
  death clip, and a smashed crate: neither should leave a shadow behind.
- **A magus fight** (batch 24, b8c506ce). A burst bolt that reaches the party
  now goes off IN the party's square: about 5 to each member in an open room
  (more in a corridor, from reflections), and all four are left burning, where
  before it struck one member for about 21.7. In a scratch fight (a magus three
  squares off for 6 s) the party took 38.7 before, 52.0 after. Nothing tuned.
- **A monster coming round a brazier** (batch 33, 45049ebe). Start a game, go
  to crypt1's room and stand at 8,4 facing east, the brazier at 9,4 between you
  and the skeleton at 10,4. Bolt it (Maren's Fire Bolt): it should come round
  the brazier to 8,3 or 8,5 and fight within a couple of seconds. Before, it
  stood still behind the fire.
- **The editor map's monster icons** (batch 61, 93c9da37). The kit skeletons
  (warrior, bare, berserker, spearman) are head shots in their breathing idle
  now, not the T-pose. A before/after survey is in
  `%TEMP%\cr61\mapicon_survey_before_after.png`; in the game, `mapicons all`
  then `mapicons survey on` shows every icon beside its picker tile.
- **Enchanted blades on the floor** (batch 62, 673aace6; optional). Drop a
  flamebrand and a frostbrand in a dark spot (`editor place weapons flamebrand
  <x> <z>`, then `mappage close`, or carry one and drop it): the floor light
  should be red and blue, where both were steel grey.
- **The world map's overlay in a world being edited** (batch 73, 2d4f0294).
  Set a world to Editor mode (`worldedit on`, or the title's Editor entry on a
  world that starts outside), go into a dungeon, press M and flip to the world
  page: no toolbar, the fog kept, a left click does not paint, and
  right-clicking a doorway opens nothing. On the travel screen (`worldmap on`)
  the toolbar, fog-off, paint and doorway right-click all still work.
- **Pause and the sheet over the world map** (batch 73). On the world map,
  press Esc, and open a portrait's sheet: the world map shows behind both,
  where the parked dungeon used to. In a level nothing changed.
- **World settings from a doorway** (batch 73). Right-click a doorway: the
  Doorways tab's status row is empty (it used to show the World tab's
  "Standing on ..."), and water coordinates typed into the World tab's start
  explain themselves on the World tab.
- **New fade lines** (batch 25, b1d1c5ad; optional). A carried Firelight /
  Tidelight / Skylight ends with "Maren's Firelight fades.", and a dazzled
  monster recovering says "The skeleton can see again." The de / es / it / ru
  wordings were written by the batch and are worth a glance.
- **The Hagalaz flare and the Earth stone** (batch 25; optional). The flare no
  longer reaches through a shut door, no longer scorches a monster still
  dazzled from an earlier flare elsewhere, and no longer lights a wall torch
  through rock; an Earth stone's mapping and tracks stop at a shut door.
- **Resting** (batch 35, a8fecd90). Rest now runs its 60x as 1/60 s ticks (at
  most 90 a frame, so full speed holds down to 40 fps). Rest in play, ideally
  with a monster awake nearby: does it feel the same speed, and do monsters
  now arrive faster and smarter while you rest? They think and walk at awake
  rates - about 2x the squares and 2x the thinks a simulated second.
- **The gold in the grooves on light materials** (batch 49, 7757aa02). The
  movement pad and the sheet's tab stones on `uimaterial snow_packed`, then
  `limestone_pale`, `marble_white`, `desert_rock`, `granite_grey` (`uimaterial
  off` after; `sheet 0` shows the tabs, the current one's gold lit). Pictures:
  `build\debug\bin\shots\ingametest-movepad-snow_packed.png` (an InGameTest run
  writes it). On the snows the etched gold went from 1.25:1 to 2.29:1.
- **Clicking small floor items** (batch 63, b087b70d). A laid-flat torch, a rune
  tablet, a key, an apple and an upright potion: click near the near and far
  edges of each one's quarter of the square. The pick is measured at the item's
  drawn middle now (a key should be easy to click where it is drawn).
- **A glow in a wall niche** (batch 63). A rune or enchanted blade in an OPEN
  niche glows in the pocket, not at the foot of the wall; in a SHUT niche it
  does not glow. `editor place wallfeatures niche <x> <z> <dir>`, then `niche
  <x> <z> <n|e|s|w> put rune_fire`, and `niche ... shut|open`.
- **Esc in the editor's dialogs** (batch 75, 151604ec; optional). With a
  drop-down open in an inspector or Level settings, Esc closes only the list;
  the dialog and its edits stay. Esc closes the create dialog ("+ New..."); on
  the editor map, Esc with the level list open closes only the list.
- **A long id in a dialog title** (batch 18, 61f2997b; optional). The type
  editor's and the Level dialog's title shrink a long id to fit, and cut its
  tail to `..` only past the smallest size. Open the type editor on a type
  with a long id.

## Decisions

- **Eval rungs and the spawn rise** (aed9b5a8). Since spawn-rise, a kit
  skeleton spawned beside the party lies still for 9.5-14 s while it gets up,
  and every eval rung that spawns one there measures a free beating before the
  fight. respond.eval's two defence arms read `taken` 0 because of it, so they
  now spawn with the new `spawn ... up` (stands at once) and the runner's
  self-test passes again. The measuring suites (arena, ladder, tiers, ...) were
  left alone: whether they should say `up` too is a measurement choice, not a
  fix. Their numbers moved when spawn-rise landed, not with this change.
- **Seeded combat sequences moved once** (aed9b5a8, C73 brought forward):
  animation clip choice now draws from its own cosmetic random stream, so every
  seeded eval sequence shifted once. Nothing was tuned back.

- **The live-blast ceiling** (batch 21). Live blasts are now a fixed table of 32
  (a first fight allocated nothing after this). A 33rd blast while 32 are still
  spreading lands WHOLE at once, with no linger, and logs a warning - normal play
  should never get there. Say if you would rather it replace the oldest.
- **A rest with a member down** (batch 32, 9a430726). A rest with a DOWNED
  member never ends by itself while a monster is within its aggro distance
  (Manhattan, through walls): that monster keeps resetting the stabilize clock,
  `recovered` waits for the downed member, and only hunger ends it (at 60x).
  AllocTest -Rest relies on exactly that to keep its rest going. Is a rest that
  runs until the food is gone what you want, or should rest refuse, or break,
  when a downed member cannot stabilize?
- **balance.cat is closed to rename and delete** (batch 72, dd116ca1), like
  effects, attacks and spells, because the code reads its `[formula]` id by
  name. Say if you would rather it stayed open.
- **Translations to check** (batch 72). `map.type.classbacked` ("The game's
  code defines this one - its entry only tunes it, so it cannot be renamed or
  deleted.") has new de / es / it / ru text written by the batch, unchecked.
- **expedition.eval no longer measures what it was written for** (seen by
  batch 11). Since spawn-rise, rung 4 (4x skel_warrior x2.5) does not wipe the
  party (taken 8.0, downed 0): the skeletons spend 10-14 s of each 40 s rung
  getting up. The script's "THE PARTY IS DEAD BY HERE", the eval-harness.md
  expedition table and CLAUDE.md's "the party dies to DIFFICULTY long first"
  are out of date for that reason. Part of the `spawn ... up` question above.
- **Pale gold or dark bronze on snow** (batch 49). On snow_packed the ink solve
  moves the gold toward PALE gold (0.99, 0.93, 0.77), so the grooves read as a
  pale cream line. Dark bronze instead would be a ResolveInks change (it moves
  the carved words too). To make the etches read harder on light materials,
  the levers are the groove's floor opacity and shading (FLOOR_A / TONE_MIN in
  tools\BuildEtchGlyphs.py), not the colour.
- **A Caster with no spell** (batch 74, c14a2e38). Picked as Caster, a monster
  with no spell now defaults to spells.cat's first entry, `flame` - a hand
  spell with no thrown form, so it throws its plain shot until a bolt is
  picked. Should the Caster list (and so the default) offer only spells a
  monster can throw, such as firebolt?
- **The rest of C19** (batch 25). Warn on spells.cat keys nothing reads, and
  drop the 15 dead `effect =` / `element =` lines? Not done yet.
- **A carried Skylight's shock and a shut door** (batch 25). The flare and the
  stone now stop at a shut door, but the Skylight's shock (CrackleNearest)
  still uses a line of sight that passes one. Should it stop too?
- **Monsters on an up-stair square** (batch 33). A monster may still stand on
  an UP stair's square (a flight, with a floor under it); only floor holes
  (pits, down stairwells) are refused. A monster parked there blocks the
  stair. Should every traversable stair square be refused too?
- **Things over a pit** (batch 33). A drop onto a pit is refused, and a throw
  that ends over a pit comes down short of it; neither falls to the level
  below. Do you want "falls down the shaft" instead? That needs a cross-level
  drop at run time.
- **Long, low creatures' map icons** (batch 61). The head-shot rule frames the
  top quarter of the model, centred on its box, which for the giant spider
  and the centipede is the middle of the back (the centipede's icon is mostly
  empty halo). Unchanged by the batch; worth a rule for low creatures (frame
  the front end) if you agree.
- **A smashed bracket drops its torch unlit** (batch 19, 2828ae7f). The bracket
  remembers its torch by the UNLIT id and the smash puts the fire out first, so
  the dropped torch is dark; a torch set down or thrown while lit keeps
  burning. The comment now says so (it claimed "anything set down goes out").
  Is an unlit drop what you want there?
- **What docs/ai.md says is not built** (batch 19). ai.md now lists it:
  call-for-help and a group leader (ProvokeMonster wakes only the monster that
  was hit), hearing, an authorable sight cone, Patrol and Cast as intents, and
  patrol pathing that is greedy only. Which of these are still wanted?
- **UsageLinesTest.ps1** (batch 20, ab47ca4c). The check that every Fetch*
  usage line still selects what it names is a standalone script for now (it
  needs the OneDrive archive and says SKIP without it). Should it join
  CheckAll's full tier?
- **`readfile <path>`** (batch 47, 049936cd). A new dev command (Diagnostics
  group) that PathsTest uses to read a file through the game's own UTF-8 path
  code. Keep it there, or name a different home for that check.

## Changes you will notice

- **The log after a worker exits** (batch 9, 99eb0cf7). A worker that
  unregisters now writes what the log still owed it: a "N further events ...
  were not logged (rate limit)" line, and a closing line for the tail of a run
  of identical repeats ("47 further repeats ... (57 in the run)"). Before, both
  were lost if the thread ended first. A run still going on the main thread at
  a clean exit is the one case with no closing line.

- **A burst that breaks on a shut door** (batch 23, 3a37fd96). A flight now
  ends in front of the wall or door it meets, never inside it, so a Fire Burst
  (or a skel_magus bolt) breaking on a shut door plays out wholly on the
  caster's side. In the corridor test the party two squares back took 91.4,
  against 73.0 when half the burst went through the door. Nothing was tuned.
  Also: a bolt that flies past a lone member and breaks behind him now leaves
  its burn on him (only reachable when the column a caster aimed at falls
  while the bolt is in flight).
- **A killing blow wakes nothing** (batch 26, a3a24f7d). A monster's killing
  blow no longer adds threat or provokes it, so a one-blow kill cannot print
  "The X turns on <member>!" as it falls. With a key, a tablet or any skill-less
  item in hand, swings and parries train `unarmed`, as Punch and Kick do.
- **`timescale` past about 6 runs in fixed ticks** (batch 35): capped at 90
  ticks (1.5 s of world) a frame, so `timescale 100` no longer gives 100x.
- **A step that ends on a wipe stops the clock** (batch 11, a61264c3).
  tiers.eval's novice rung now reads "stopped: the party was wiped" after 23.68
  s, where it ran a full 30 s; no measured number moved.
- **A surface's Relief row** (batch 90, cf80b8d7). Unset by default now, a
  checkbox reading e.g. "Relief (derived 0.060)" - the texture set's own depth,
  from one record the baker and the editor share - where it was a slider at
  the per-kind 0.055 / 0.045 / 0.08. Every committed worn mesh re-bakes
  byte-identical, so nothing in the dungeon changed. The editor can no longer
  create a type that re-bakes a shipped set as the wrong surface kind.
- **The adept's threat** (batch 25). skel_mage_adept's volley bolts now carry
  their own spells.cat burn (1 4, was firebolt's 2 4), so its threat went from
  16.92 to 15.43 (shot 5.94 -> 4.94): still band 3, now just below
  skel_lurker (15.74). No other kind moved. Not tuned back.
- **Every declined dev command refuses** (batch 10, 556e014f). A command that
  declines (bad arguments, nothing to act on) now REFUSES, and an eval script
  that meets an unexpected refusal fails, so a script cannot report on a setup
  line the world turned down. Scripts that probe a refusal say
  `expect-refuse`. Also: a middle-click erase with nothing to erase no longer
  pushes an empty undo step, and `profile panel` toggles like bare `profile`.
- **The power bands re-cut** (batch 24). skel_magus's derived threat went from
  23.14 to 30.70 (its burst now lands on the whole party). The ranking order
  is unchanged, so the generator picks the same monsters, but the palette's
  bands re-cut against the higher top: skeleton, skel_bare, skel_spearman and
  centipede drop from band 3 to 2; warrior, berserker, lurker, adept and mage
  from 4 to 3; band 4 is now empty.
- **Starting a rest waits for the AI** (batch 34, f95726ff). Rest (and
  `lockstep on`) now blocks the main thread until any AI tick already running
  finishes - about 40 ms with a deliberately heavy bucket in ThreadStress, a
  few ms on a real level.
- **Menus with a group row are a few pixels wider** (batch 48, 3c9df68c), so
  the bare hand's group marker sits clear of "Combat" / "Magic" at 720p.
- **A press during an alt-tab** (batch 8, 2be14913). Losing focus no longer
  wipes the frame's key PRESS edges, so a key or click that lands in the same
  frame as a focus change still counts once (held keys are still released, so
  nothing sticks down). It fixed a harness's lost console toggle while other
  sessions' games took the foreground.
- **InGameTest is slower** (batch 8): about 9.5 minutes instead of under one,
  because every screen now gets a marker and a status probe and must prove it
  opened before its audit counts.
- **More effects per bearer** (batch 19): the ceiling went from 16 to 24, and a
  static_assert now counts the worst case from the named constants.
- **FetchTextures / FetchModels -WhatIf** (batch 20): lists what would be
  imported and bakes nothing.
- **A carried Firelight's shadow cube** (batch 60). It now re-renders while the
  party walks (about every other frame at the walk's pace here), at about the
  cost the held torch's cube already paid. Slot 0 is documented as "the
  nearest shadow-casting light" (torch and Firelight compete by distance), not
  "the carried torch always wins"; the behaviour was already that.

## Follow-ups the batches found (not in the plan)

- **Spell bolts fly 8 m** (found by batch 23). spells.cat `range = 8` is in
  METRES, about 3.2 squares, so a firebolt cast from 4 squares fizzles before
  it reaches a door. It reads like a number from before the scale change. Left
  alone (a balance-pass question, not a fix).
- **Test-World lost its catalog headers** (found by batch 72). Before the
  comment fix, a write had already dropped the template's header text from
  that world's catalogs; its quests.cat is now just the generated one-line
  header. Copying the headers from assets/templates/default/catalog brings
  them back. Also: deleting a catalog's FIRST entry now moves its whole lead
  comment to the next entry (or the file header would go with it), so a
  comment only about the deleted entry may need removing by hand.
- **No allocation check puts a monster's burst on the party** (batch 24). No
  AllocTest mode lands a monster's burst bolt on the party, or a gust on a
  shot, inside the window. The new paths are a fixed table and two erase_ifs
  that keep capacity, so they should allocate nothing, but nothing proves it.
- **Esc on the Settings page and in the portrait picker** (seen by batch 75).
  Both still take an Esc meant for one of their own open lists as "leave the
  page" / "close the picker" (GameUI::CloseSettingsPage, DismissPopup). The fix
  is the editor dialogs' (ask PopupOpen first). Schedule it?
- **`rest on <anything>`** (seen by batch 11) still turns rest on for any word
  but `off`; batch 10's refusal pass did not reach it.
- **InGameTest's sweep_stair can lose a command** (seen by batch 49): `goto
  crypt1` can outlast Run-Cmd's fixed 2 s, and the next `editor inspect 1 1`
  is dropped (once in three runs there). It wants a wait for "Level ready".
- **CheckAll's new rows** add time to the full tier: `alloc-items` (batch 63)
  about 3-4 minutes, `worn` (batch 90) its release-baker re-bakes.
- **Rear-rank members spam "no reach"** (seen by batch 15, not changed). With
  autoattack on, a rear-rank member without a polearm takes the no-reach path
  every frame (PartyAttack returns before setting the hand's cooldown), so the
  HUD log fills with that line. It does not allocate.
- **An eval script that ends on a load** (seen by batch 10). A script whose
  LAST line starts a load (a cold `reset` or `newgame`) ends mid-load and
  fails with endstate=loadinggame, since only the next line waits for a load.
  renameworld.eval got a trailing `state`; the runner was not changed.
- **QuitTest's Alt+F4 case once timed out** (the pre-merge full run, 423bfe1b).
  "still running 20s after Alt+F4 in Borderless", under the lanes' build load;
  it passed alone straight after and in the same run's self-test pass. Worth
  watching for a repeat before reading it as a real regression.
- **Back-to-back HealthTest runs** (seen by batch 48). A HealthTest started
  while the previous run's last game (the `assert` case) is still exiting is
  refused (exit 3); a moment later it runs. The race predates the batch.
- **AITest's save** (batch 34). It leaves `aitest_async.dsav` in the shared
  Documents\DungeonSaves, overwritten each run - one more fixed name for the
  "fixed save names" follow-up above.
- **/check-selftest and other sessions' games** (batch 8). ProfileTest refuses
  beside ANY running game (on purpose: a second game on the GPU would be
  measured), so the `profile` row of a full self-test run fails whenever
  another session has a game up. Run it alone.
- **`partypage status` on the default four** (seen by batch 8, not touched).
  On the title screen, after `partypage default`, it printed "start: refused -
  Brand cannot be made: " with an empty reason, and race=- for the four
  premade members. Possibly a real oddity on the party page.
- **CheckAll's alloc-lights row** (batch 60) adds about 1.5 minutes to the full
  tier. Say if you would rather keep its checks as a manual `AllocTest
  -Lights` run.

- **Fixed save names in the shared save folder** (batch 4, cde991c8). The
  Python judges now name their saves per worktree, but other eval scripts
  (supplytest, spells_douse, partycreation, portraits, ...) still save under
  fixed names in the one Documents\DungeonSaves your play uses. The same helpers
  could rename them; a later batch.
- **A killed harness leaves the volume muted** (batch 4). The mute is restored
  by the harness's own finally/atexit, which a kill skips, so a killed run
  leaves `volume=0` in that build's settings.ini. A run could clear a mute a
  killed run left, as EditorTest now recovers the library backup.
- **EditorTest phase 16 still writes the real style library** (batch 4),
  behind a backup that is now kill-safe. A `-library <dir>` switch would let it
  use a scratch copy like everything else (a C++ change).
