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

## Follow-ups the batches found (not in the plan)

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
