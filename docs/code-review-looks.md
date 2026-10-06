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
