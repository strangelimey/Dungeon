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
- **The party page** (batch 51, ae8944ea). Title -> Start New Game -> the party
  page. Default party, select Brand, clear his name: his slot should still
  read "Brand" (it read "New member") and Start still be offered. With the
  page open, console `lang de`, close it: the page rebuilds in German (race
  names and traits too) and still edits; `lang en` after (`lang` saves).
  Otherwise it should behave exactly as before; its text now refreshes only
  after a change, not every frame.
- **A quality swap** (batch 64, bbf26422). Enter crypt1, Settings -> Video ->
  Quality Low, then Ultra, then Low: props textured with a tiered set
  (wall_stone props especially) now sharpen and soften with the walls; they
  used to keep the tier they first loaded at. Most prop sets ship 2k only and
  will not change. Console `textures` lists each set's tier.
- **A long id in a dialog title** (batch 18, 61f2997b; optional). The type
  editor's and the Level dialog's title shrink a long id to fit, and cut its
  tail to `..` only past the smallest size. Open the type editor on a type
  with a long id.
- **Stealth after a miss** (batch 29, 9dc818af). A missed shot or
  swing now wakes the monster it was aimed at - an `asleep` one, or a dormant
  lurker beyond its trigger, included - and any attack that reaches the party
  ends a rest as `attacked`: a miss, a bolt the Wind Ward turns, a blow a water
  veil drinks whole. A miss credits no threat, so the woken monster picks its
  victim with no grudge. Creep up on a sleeper, miss it, and watch it wake.
- **The title after a save** (batch 52, ec9f4772). In the pause menu: Save, Esc,
  Return to Main Menu - the title should show Continue and Load at once.
- **The opening lines** (batch 52). Start New Game from crypt2 (or after loading
  a save made on another level) now opens with the "You descend into the
  dungeon... / Something shuffles... / keys" lines, and loading onto a
  different level now shows "You descend" too, which it did not before. Is
  that wanted?
- **A save that will not delete** (batch 52). When a save cannot be deleted
  (another program has it open, say), the Save/Load page shows an accent line
  above the list: "Could not delete <name> (error 32)." - the system's reason
  goes to dungeon.log. Check the wording and the place. (A read-only save still
  deletes; the library ignores the attribute.)
- **Distant textures** (batch 92, 7795ba11). Every mip chain is now averaged in
  linear light and rounded, so distant albedo - fine-detailed brick and paving
  most - may read slightly BRIGHTER than before (the old filter darkened detail
  with distance). Walk crypt1 and crypt2 and judge the far walls and floors;
  anything off is a filter matter, not a texture swap. And the rune tablets,
  potions, rock and torches now load from baked images: check they look the
  same up close.
- **Picker and swatch brightness** (batch 65, 932adb5a). The asset picker's
  texture tiles, the editor palette's surface swatches (theme rows too) and the
  editor map's cell fill under an armed Walls/Floors/Ceilings brush now draw as
  bright as their files - they were much darker (an sRGB view decoding stored
  bytes the sprite pass writes straight out). Open the picker on textures and a
  surface section of the palette.
- **The three bought daggers** (batch 66, b9d23436). french_dagger was drawn
  INSIDE OUT (its glTF node mirrors) and viking_dagger / snake_dagger had skewed
  normals (non-uniform node scales). viking_dagger is the starter `dagger`
  Brand and Sera hold, so its hand icon and floor draw change too. Every other
  picker tile is pixel-identical.
- **An imported weapon's texture** (batch 86, 3c6bde6c). A weapon whose
  single-primitive .gltf named a `texture` set drew white; the set now dresses
  it. `modelfile weapons <id>` prints set= wears= drawn= for any item.
- **The HUD effect strip** (batch 54, 2bf337dc). Each effect icon now sits in a
  stone socket with a time sliver and a border in its school's colour. The
  picture inside the socket's well is small (about 12 px at 900p). The Effects
  tab's icons use the same helper and should look as before.
- **The armor tooltip's avoidance row** (batch 54). Hover a piece of armor (or
  the one in Brand's pack), ideally after `setskill 0 avoid 3`: the avoidance
  row is back, "+8 (lvl 3)" unarmored and "-" armored, red when comparing
  against an unarmored +8; the value columns widen to fit their cells.
- **Skill names in their bar's colour** (batch 54). On the sheet's Skills tab
  the status bar names a weapon skill in steel, a defence skill in bronze and a
  reserve in its pool's colour, matching their bars (all were the accent).
- **Script-built models re-run** (batch 95, 610df8a7). In the asset picker
  (neither arch nor the fountain is placed in a level): wall_arch_rustic and
  wall_arch_rough should look as before minus any crack or notch above the
  jambs (the rough arch's per-stone tilts and sizes changed - same seed and
  ranges); fountain_round without the reversed texture column on the spout
  shaft and basin wall; the potion corks and the rock without their seam column.

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
- **Which monster a party swing meets when several share a square** (batch
  28). A swing now goes to the FRONT RANK first, then the swinger's own lane.
  So with one bone swarm back-left (Brand's lane) and another front-right,
  Brand hits the front-right one. A monster's own melee pick works per file
  instead - an empty near slot opens that file - and the mirror of that would
  have Brand hit the back-left one through the gap. Keep front-rank-first, or
  make it per file like the monsters'?
- **A spawn clip still moves timing** (batch 28). Clip choices no longer touch
  the combat dice, but a rising kit skeleton holds still for the length of the
  spawn clip it picked, so authoring another SPAWN clip still changes when that
  monster starts acting in a seeded sweep. Attack, hit and die clips are
  animation only. Fine as it is?
- **What ends a rest as `attacked`** (batch 29). It is now every pipeline event
  but a damage-over-time tick. Two side effects: walking into a wall while
  resting ends the rest even when armour soaks the whole bump (it used to only
  when the bump hurt), and a member's own over-exertion wound no longer ends a
  rest. And "Something is attacking!" still prints before the swing's own
  hit / miss line, because the rest breaks inside the damage pipeline. Should
  either side effect go the other way?
- **Deleting a type a save still names** (batch 52). The plan wanted a type
  delete REFUSED when only a save references the type. Today it deletes and
  warns which saves still name it (the documented design), and the warning now
  finds the saves of the world in hand. Keep the warning, or refuse?
- **A failed write can leave a truncated file** (batch 93, 11f99ae8). The
  baker's writes are now all checked and a failure is reported with a non-zero
  exit, but WriteBinaryFile writes in place, so a full disk can still leave a
  half-written file. Writing to a sibling file and renaming it over the target
  would keep the old one - and would change saves, settings and catalogs too.
  Want that?
- **Walking out onto the world map keeps the undo history** (seen by batch 78,
  f8840928). New game, load, reset and an ambush now clear it, but parking a
  dungeon to walk the world map does not, so Ctrl+Z on the world map can still
  restore the parked dungeon's snapshots. Clear it there too?
- **ThreadStress can HANG under heavy load** (seen by batch 36, c7d67506).
  Twice, while another worktree built debug, release and release-profile at
  once: the AI workers run below normal priority, a starved idle bucket missed
  the 250 ms grace and was force-terminated, and the lock it leaked (a
  condition variable's or the heap's) blocked every other thread for good. The
  existing TerminateThread hazard, reached through starvation. Options: a
  longer grace or a higher priority for the workers under the harness, or
  CheckAll refusing while other builds run. Written up in check-threads.md.
  Seen again by batch 37 (one hang, one run with 7 failures, then a pass at
  half load) and by the integration's threads -SelfTest, which failed two
  extra checks when a bucket was force-terminated under load.
- **An AllocTest -PartyPage row in CheckAll?** (batch 51). The idle party page
  mode is manual for now; a full-tier row would run it every full check.
- **Translations to check** (batch 76, b102f032): twelve new keys' de / es / it
  / ru text, written by the batch (counts, units, "for"); ru uses the existing
  "<word>: {}" count form to avoid its three plural forms.
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
- **Niche walls past four lever names** (batch 39, c9a53727). A chunk holds
  pre-built walls for at most 4 lever names (16 looks); past that, or where the
  looks fill different wall buckets, the load warns and a press there rebuilds
  the old way, which the allocation guard reports (no excuse, per your answer).
  Should the level checker flag it at authoring time instead?
- **Translations to check** (batch 39). The dead-lever warning now says "no
  door or niche": check de / es / it / ru ("Nische", "hornacina", "nicchia",
  "ниши").
- **The statue's hood stitch faces** (seen by batch 95). BuildStatue's hood
  stitches (the rims beside the face opening, the hem, the peak - 108
  triangles) are wound INTO the cloth, so with back-face culling they are
  invisible. Flipping the four add_face orders fixes it but makes those
  surfaces appear; `tools\MeshTest.ps1 -Files assets\models\statue_sentinel.gltf
  -Detail` shows it. Flip them?
- **A terrain's glyph change writes world.map at once** (batch 89, 43de2d2b),
  with any other unsaved world edits, and a new terrain saves the catalogs at
  once - so terrain.cat and world.map on disk can never disagree (a mismatch
  aborted the next launch). A terrain delete clears the undo history. Keep?

## Changes you will notice

- **The console and the editor's pause** (batch 53, 3cd4c778). With the dev
  console open, the world now holds while the editor is paused, while an editor
  dialog is up and during the "Leave the crypt?" question; it now runs under the
  character sheet, and a stair stepped onto with the console open is followed at
  once. A bare `editor` (or any `editor ...` subcommand) no longer unpauses a
  paused editor or closes its level drop-down - only a real Player / Editor
  switch does.
- **The Help line's key names** (batch 38, 7d8246d2) are cached when settings
  load and on every rebind, so a keyboard LAYOUT switch while the game runs
  keeps the old names until the next rebind or relaunch.
- **The door inspector's Open** (batch 31, f6999207) now shows and edits the
  door's AUTHORED state, not its state in play (a door the party opened shows
  unticked if it is authored shut). Unticking it with anyone - a monster or the
  party - in the doorway is refused outright: the box ticks itself again, the
  record is unchanged, and the log says "Something is blocking the doorway."
  A wrecked door takes the close in its record while the wreck stays open. And
  the live check warns about a lever standing in the doorway of a door it
  targets, which could never shut it.
- **A stale .dds is refused** (batch 92, 7795ba11). The game now draws a
  texture's PNG, with a warning, when its .dds is OLDER than the PNG, and says
  once a model when its baked images are missing or stale. Your tree in
  C:\Dev\Dungeon had none (checked: all 648 texture .dds are newer than their
  PNGs), but its .dds still carry the OLD mip filter until re-baked: run
  `build\release\bin\AssetBaker.exe mips assets --force` (about an hour), or
  copy the re-baked .dds from the code-review worktree. Seven models had no
  baked images anywhere (the three potions, the rock, the three torches) -
  `AssetBaker model-images` makes them.
- **A bad save index is refused whole** (batch 52). A hand-edited save with a
  member or pack index outside 0..3 is refused with a log line; it still shows
  in the Load list, since only a full read finds the bad index.
- **A curve that names no form** (batch 30). A balance.cat `skill_curve` /
  `stat_curve` that is negative, NaN or above 2 now reads as hyperbolic or
  logarithmic with a warning; an in-range fraction such as 1.7 still truncates
  but warns too, and is written back as 1 at the next save.

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
- **Throws and fumbles** (batch 27, dafd0413). A fumbled torch lands with the
  burn time it had left; a fire or poison flask that bursts on a MONSTER
  trains throwing (on a wall it trains nothing); saving with a flask in the
  air no longer sets it off - on load it lies, whole, in the square it was over
  when you saved (a save cannot know where the flight would have ended).
- **Readouts every eval diff will show once** (batch 28). Every TALLY line now
  ends `clipdraws=N`, and each `monsters` line for a Medium / Small / Tiny
  monster shows `slot N`. New console tools: `spawn ... share` (join a square
  others hold) and `monsterclips` (a kind's clip table, live and unsaved).
- **`reset` after a level change is a load** (batch 12, 997868a3). A reset after
  a script left the harness level loads back to eval_arena ("reset: switched
  in N ms ..."), and reset now clears every level's stashed edits and the undo
  history - typing `reset` in a dev session throws away unsaved edits to
  other levels.
- **Load paths** (batch 64): the model cache no longer pins CPU images (48.1 MB
  in 13 files on crypt1, now 0); a set missing its `_n` normal map draws flat
  with one warning, not the magenta checker, and `levelcheck` lists such sets;
  `Dungeon.exe -warp` draws on the software rasterizer, and every Eval.ps1 run
  now fails on a D3D12 debug-layer error.
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
- **assets\maps is gone** (batch 94, 3c88329e). FetchTextures now builds its
  list from every catalog's `texture` fields (the worlds', the library's, the
  template's) and ends by carving the rune sets (`AssetBaker runes`); a name
  nothing covers stops it before any bake. A fresh clone gets all 12 crypt sets.
- **The template follows dungeon-demo** (batch 94). TemplateTest (quick tier)
  holds assets\templates\default against a fresh BuildTemplate run, so after a
  change to dungeon-demo's catalogs or project.ini, run `python
  tools\BuildTemplate.py` and commit the template, or the quick tier fails.
- **A world switch under a bake is refused** (batch 79, 3e879299), and a
  question still up (an exit's "Leave the crypt?") goes with the game: a new
  game, a load or a world switch takes it down unanswered, as it does the item
  on the cursor. New dev commands `confirm` and `bake [status|wait]`.
- **A lever press allocates nothing** (batch 39). A secret niche's reveal swaps
  in walls built at load instead of rebuilding chunks in play. eval_arena gained
  a levers' corner (a hidden niche at 3,22, its lever at 2,22, an unwired lever
  at 1,22), out of every suite's way.
- **Models load as .gltf or .glb** (batch 86), and a type whose model cannot
  load is refused at its creation and its Save, with the reason.
- **Visiting or reading a level no longer stashes it** (batch 80, 6f289839), so
  `savemap` rewrites only what was edited (a level still in the old stair form
  is rewritten once after a visit; the ACTIVE level is always written from live
  state, its monsters where they stand - batch 81's C326). Unsaved edits now
  survive a save loaded on another level.
- **A GPU failure explains itself** (batch 67, 684586ac). A failed D3D12 call
  logs its HRESULT by name, the device's removal reason and DRED's record (the
  command lists in flight, the op each stopped at, a page fault's address);
  DRED is on in release builds too. If you ever hit a real TDR, look under "D3D12
  call failed at" in dungeon.log. Its GPU cost is unprofiled - worth one
  /check-profile on a quiet machine.
- **Terrain glyphs are checked** (batch 89): a "+ New" terrain gets an unused
  glyph, a bad or duplicate glyph and a delete of a painted terrain are refused,
  and `worldloc set <id> entry <x> <z>|none` sets both halves of an entry.

## Follow-ups the batches found (not in the plan)

- **glTF names are never unescaped** (found by batch 93). cgltf never calls
  cgltf_decode_string and Model.cpp copies node and animation names raw, so a
  name with an escape would load as `a\"b`. No shipped name needs one; a
  two-line decode in Model.cpp would make the round trip exact.
- **AllocTest -Exit has no CheckAll row** (batch 38): run it by hand (about 3
  minutes).
- **No judge checks the baker's sRGB flag per file** (found by batch 92):
  albedo vs `_n` / `_mr` vs a model's base-colour images. The logic is small
  (IsTextureSetAlbedo, SrgbImages), but nothing decodes a baked chain to check.
- **Undo can lose a broken fixture** (found by batch 77). RestoreEditorState
  (undo) applies the level's saved dynamic state before RebuildFiresAndDust
  re-seeds the fixture table, so if the undo step changed the map's fixtures
  (undoing a regenerate, say), its broken or damaged fixtures are matched
  against the OLD table's squares and some can be lost. Seeding the table
  fresh before ApplyActiveSnapshot there would make it exact.
- **HealthTest does not pass -unattended** (found by batch 37): its assert case
  still flashes the debug CRT abort box before the harness kills it.

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
- **The type editor's `texture` field and shared sets** (seen by batch 91,
  85e5a498). "Use installed" refuses a set painted as another surface kind, but
  the type editor's `texture` field can still point a surface type at one; its
  restyle bake would then reshape that set (the baker refuses a shipped set, not
  an editor-imported one). Gating onSave with Game::AdoptSurfaceSet is small.
  Also: a same-kind "Use installed" uses the set's existing worn meshes as they
  are (tuned ones included), and the create dialog's footer line does not wrap,
  so a long name may overrun the Create icon (no windowed uioverlap sweep of it).
- **A killed worker never gives its health slot back** (seen by batch 36): a
  force-terminated worker's next life takes a new slot, so the threads
  self-test fills the 32-slot table. A rebooted worker could keep its own.
- **LangTest checks literal keys only** (batch 76): keys built at run time
  (`monster.` + id, `skill.` + id) and catalog `name = item.<id>` fields are not
  checked; a catalog-name check would follow naturally. `map.select.empty` is
  an orphan key in all five files.
- **Eval.ps1's other self-test runs are windowed with no timeout** (seen by
  batch 12): a debug assert in one parks the self-test on a CRT dialog.
- **The guard's first armed frame** (seen by batch 27): an allocation on the
  window's very first armed frame is counted but left no stack in one
  mutation run, though batch 15 starts capture in ArmFrame for exactly that.
  Worth re-checking against -Swing.
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
- **HealthTest's `kill` case once missed its top frame** (the second merge's
  full run, e7792f40). The kill's stack had the `sleep_for` and
  Game_DevDiagnostics frames but not `DelayExecution`: the wedged worker sleeps
  50 ms at a time, so under the lanes' load the kill can catch it between two
  sleeps (in SleepEx, before the syscall). It passed alone straight after.
  Accepting SleepEx / Sleep as the top frame would close it; not done.
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
- **A general clip skip in Widget::Draw** (seen by batch 65). Widget::Update
  skips a widget the clip removes, Widget::Draw does not; the asset picker now
  skips its own tiles out of view, but a skip in the walk would save draw work
  in every scroll area. It changes the UI library's draw rule, so not done.
- **A huge window's thumbnail cap** (batch 65). The cap (twice what is on
  screen) was run at 1600x900 only; a forced cap stands in for a 5K or
  ultrawide window, which was not tried on real hardware.
- **Renaming a type that has no `model`** (seen by batch 86) changes the file
  its id falls back to, and is not refused; the same for a `texture` id
  fallback. No dungeon-demo entry relies on either today.
- **A multi-part .glb decoration not marked multimaterial** (batch 86) now
  loads instead of aborting, but draws only its first part in its `texture`
  set - until batch 8c retires `multimaterial`.
- **Five fetched textures differ from the installed ones** (seen by batch 94):
  burlap_1k / 2k, wood_planks_1k, mossy_rock_2k_n and runestone_n come out of a
  fresh fetch unlike your June imports (older inputs or importer options).
  pillar_2k and the wood_planks_clean / _old3 sets are installed but named by
  nothing, so a fresh fetch leaves them out.
- **Two batches each green alone, red together** (the r9 round, cb2c953d).
  Batch 86's EditorTest phase 50 assumed eval_arena had no lever; batch 39 gave
  it two. The phase's scratch copy now drops them. Worth knowing for the merges
  still to come: a judge's premise about shared content is where lanes collide.
- **The other Build*.py usage lines** (batch 95) still lack
  `--python-exit-code 1` (about 20 untouched scripts), so a failing assert in
  one exits 0 and the next step imports the last run's .glb.
- **A stale comment** (batch 95): decorations.cat still credits wall_arch_rough
  to tools/RoughenArch.py; it is BuildWallArch.py --rough (C403's).
- **A party ward the default casters cannot afford** (seen by batch 41): with
  the default party, Brand (8 max mana) and Sera (12) can never cast a party ward
  (20 mana). Game data, noted for the balance pass.
- **Re-bake sidecars after batch 95** in your tree at the merge: the potions'
  and the rock's `.glb.0.dds` (`AssetBaker model-images assets`, release), or
  the loader reports them stale and decodes instead.
