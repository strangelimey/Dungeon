# A world is loaded when a game starts, not when the process does

Michael, 2026-09-24: *"change things so the world is only loaded when
'continue' or 'start' are selected."*

## Why it was the other way

W7 made a world a PROJECT and chose, deliberately, to read it once at process
start: `m_project(Project::Load(ChooseProjectFolder()))` in `Game`'s initialiser
list, and everything downstream built from it and holding on to it —
`DungeonWorld` keeps `const Project&`, loads a level in its constructor, builds
damage types / balance / effects / magic there and nowhere else, and caches
monster / item / decoration / fixture kinds by catalog id for the life of the
process; `MapView` and `MapEditor` hold `DungeonWorld&`. Switching therefore
RELAUNCHED (`SwitchWorld` / `RestartApp`, and W12's `-newgame`), which a fresh
process makes correct by construction. It costs a visible restart.

## The decisions (Michael, 2026-09-24)

1. **Old saves are refused.** Saves gain the world they belong to; the save
   version goes to 2 and the floor with it — early saves are work in progress,
   the policy already on record.
2. **Continue loads the newest save from ANY world**, and that world with it.
3. **The Load page lists every save**, each labelled with its world.
4. **The title screen has no world**, so the editor and every console command
   that needs one refuse until a game is started or continued. Global commands
   (quit, fps, lang, worlds, logecho, and the harness's reset / newgame) work.

## The approach: rebuild, do not reset

A world switch DESTROYS the old `DungeonWorld` and constructs a new one from the
new project, rather than clearing it in place. The map of what an in-place reset
would have to forget runs to a dozen caches keyed by catalog id — and a new
world copies its catalogs from the one it was made from, so the ids COLLIDE and
a missed cache shows the old world's content under the new world's name. A new
object has no stale state to miss. The price is mechanical: `m_world` becomes an
owned pointer, and the two editor objects that hold a reference to it rebind.

What survives a switch is exactly what is not the world's: settings, fonts,
sounds, the thread manager, title art, the UI's contexts — the HUD is rebuilt
(`BuildHud` appends, so it must clear first), and the item-icon banks are
cleared, since they are keyed by the old world's ids.

## Phases

- **P1 — saves know their world.** A `world=` header line; `SaveSlot::world`;
  version 2, floor 2. The Load page shows the world beside the time. A
  `-project` run sees only that world's saves (it is how a harness opens a
  world, and a Continue must not carry it off into another).
- **P2 — the world is built on demand.** `m_world` an owned pointer, created by
  `Game::LoadWorld(folder)` and destroyed by `UnloadWorld()` (GPU drained first:
  in-flight frames still reference its buffers). `MapView` / `MapEditor` take a
  pointer and rebind. The title screen runs with none: per-frame calls guarded,
  settings pushed into the party on creation instead of in `Game`'s constructor,
  and ONE console gate — a command either needs a world or is listed as not.
- **P3 — the menus drive it.** Start New Game: the world list, then load that
  world if it is not the one resident, then a new game. Continue / Load: the
  save's world, likewise. Return to Main Menu keeps the world resident, so
  going back in costs nothing. The Worlds dialog's Open becomes a new game in
  that world. `-newgame` and the relaunching `SwitchWorld` go.
- **P4 — checked.** A swap A -> B -> A must return the SRV heap and the heap
  to where they were (a switch that leaks is the failure this design invites,
  and the SRV gauge already exists to show it); WorldTest learns the save's
  world and the refusal of v1; the eval suites, InGameTest, AllocTest.

## Built (2026-09-24)

All four phases, in one pass. What the doing turned up:

- **The version-floor check had been passing on the wrong save.** WorldTest
  phase 3 downgraded the save by replacing the literal `save version=1`, so
  the day the format became v2 the replace did nothing and the load went
  through. Its first check still PASSED, because it looked for the refusal
  sentence anywhere in the log, and every old v1 save in the folder, all now
  refused, logged it. It reads the version off the file now, asserts the
  file changed, and demands the refusal name THIS save.
- **The leak check is real, not assumed.** Title screen 48 descriptor slots,
  Dungeon Demo 250, a second world 228, back to Dungeon Demo 250. With the
  unload's icon-texture frees deleted it reads 296 on the way back, so the
  phase fails on exactly the leak this design invites.
- A new world made on the title screen copies its catalogs from the DEFAULT
  world (there is no loaded one to copy); `worlds new` from the console there
  is how a harness makes one.
- `worlds status` prints what is resident and the SRV count, the number the
  check reads. `worlds load` switches in the process and no longer ends a
  script, so it may appear in an eval script now.
- Seen, not caused here: pausing on the world map draws the dungeon's frozen
  3D scene behind the menu rather than the map.

Verified: WorldTest (19 phases, the swap mutation-tested), the ten eval
suites, CheckAll quick tier, a release build, and the menus driven by hand:
title -> list -> a second world (same process) -> Return to Main Menu -> the
demo (same process) -> save -> another world -> title -> Continue, which
switched back to the save's world and loaded it.
