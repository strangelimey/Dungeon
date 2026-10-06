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
