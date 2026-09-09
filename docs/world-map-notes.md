World map — brain dump
======================

Michael's raw notes, `world-map` branch, 2026-09-09. Captured verbatim in
substance during the dump; NOT yet organized into themes and NOT yet a design.
This file is the record the eventual plan gets checked against, so it stays as
he said it — including the half-formed bits ("might", "or") and the open
questions. Edits here should only ever ADD later dump material; the organizing
and the design belong in their own sections/files below and after.

Raw notes
---------

- We'll need an overall game file(s) that holds everything for a game.
- This will contain details on the world (game).
- The world map will contain many dungeons.
- Each dungeon will contain one or more levels.
- Saves for a dungeon will contain local differences. However, some things will
  need to be saved at the global level. For instance, you might find a quest
  item in a dungeon. This will update some quest flags for the overall world.
- The party's position in the world will be saved globally too.
- Dungeons will be discovered on the world map as you explore - or you might
  find a map or a clue that will add them (like in Skyrim).
- Travel between dungeons happens on the world map view. However, random
  encounters might happen and then the player will be in a "constructed on the
  fly" dungeon/area for the encounter.
- Random encounter areas won't be saved - they're throwaway.
- The world map itself needs to be data-driven, like levels. This means a random
  encounter can be generated based on the level/difficulty of the particular
  area of the map where the encounter occurs.
- Towns and shops might live on the world map too.
- Quests will be tracked globally with their own state.

Status
------

Dump closed 2026-09-09 ("that's it for now"). Next steps, in order and only
when asked: ORGANIZE these into themes/groups (flagging contradictions and
gaps, which were deliberately not raised mid-dump), then PLAN against the
organized set.

Organized
---------

Grouped 2026-09-09 from the raw notes above, and checked against what the tree
already has (`Project.h`, `SaveGame.h`, `Generate.h`, `DungeonWorld_Arena.cpp`).
Still NOT a design — this only sorts what he said and names what he did not.

### A. The containment hierarchy

  game file(s) -> world -> many dungeons -> one or more levels each

- We'll need an overall game file(s) that holds everything for a game.
- This will contain details on the world (game).
- The world map will contain many dungeons.
- Each dungeon will contain one or more levels.

What exists: a PROJECT (`assets/projects/<name>/project.ini`) with a FLAT level
list — `levels = stem stem stem`, in menu order. There is no dungeon tier at
all; a dungeon is only implied by which levels happen to link to each other via
stairs (`dest=` names a stem). So this theme adds a MIDDLE tier that nothing
currently models, and the level dropdown, the stair `dest=` sweep and the
manifest all currently assume the flat list.

### B. The save split — global vs local

- Saves for a dungeon will contain local differences.
- Some things need saving at the GLOBAL level (a quest item found in a dungeon
  updates world quest flags).
- The party's position in the world will be saved globally too.
- Random encounter areas won't be saved - they're throwaway.

What exists: ONE save file (`.dsav`, currently v25) already holding BOTH tiers
without a boundary between them — `currentLevel` + a `levels[]` of per-level
`LevelState` (fog `seen`, entity diffs/spawns, niches, broken props) + party
pose + full character state. So the local half is largely built; what the notes
add is a GLOBAL tier above it and a line between the two.

### C. The world map itself — data, discovery, contents

- Dungeons discovered by exploring, or added by finding a map or a clue
  (like in Skyrim).
- The world map itself needs to be data-driven, like levels.
- An area of the map has a level/difficulty.
- Towns and shops might live on the world map too.
- Travel between dungeons happens on the world map view.

Precedent that fits: fog of war is already a per-cell bitset of revealed cells
(`m_seen`) that is dynamic save state and never baked into the static layer —
"discovered dungeons" is the same shape one tier up. A found map/clue granting
discovery is the same thing reveal items were always going to do to `MarkSeen`.

### D. Random encounters — throwaway constructed areas

- Random encounters may happen while travelling.
- The player lands in a "constructed on the fly" dungeon/area for the encounter.
- Not saved; throwaway.
- Generated from the level/difficulty of the map area the encounter occurred in.

Machinery that already exists and should be reused rather than reinvented:
`generate::Params`/`Level` is PURE and DETERMINISTIC (same seed => same level)
and emits ORDINARY content with no "generated" flag, so every existing tool
works on it; `DungeonWorld_Arena` is the precedent for a space carved into the
LOADED map that writes no files; and catalog `tags` ("what WORLD it belongs to
— undead, ...") is already the pool-matching axis a difficulty-scaled encounter
would draw from, resolved by the caller so the generator stays pure.

### E. Quests

- A quest item found in a dungeon updates quest flags for the overall world.
- Quests will be tracked globally with their own state.

Nothing exists today: no quest catalog, no flags, no journal. The dump names the
STATE ("tracked globally with their own state") but not the DEFINITION side.

Open questions and gaps
-----------------------

Held back during the dump, raised here rather than answered:

1. IS THE "GAME FILE" CONTENT OR SAVE? The notes use it both ways — "holds
   everything for a game" and "details on the world" sound authored (the
   project's job), while quest flags and party position are plainly save state.
   Two different files, or one thing with two halves?
2. WHERE IS THE PARTY WHEN IT IS ON THE WORLD MAP? Today `AppState::Playing`
   implies a loaded level and a `DungeonWorld` with a camera in it. "The party's
   position in the world" is a second, coarser position that exists when no
   level is active. Is the world map a new app state, an overlay like MapView,
   or a level of its own kind?
3. IS THE WORLD MAP A GRID? "Data-driven like levels" says authored-as-data, but
   not whether it shares the cell grid, and "travel happens on the world map
   VIEW" suggests a screen rather than a walked 3D space.
4. WHAT HAPPENS IF YOU SAVE DURING A RANDOM ENCOUNTER? "Throwaway, never saved"
   and "save any time" cannot both hold. Options exist (refuse, drop you back
   to the map, or store the seed) but the dump does not choose.
5. DOES TRAVEL COST TIME? Food and water drain by time and rest multiplies it
   (docs/health-and-healing.md). Crossing a world map is exactly where that
   would bite, and the dump says nothing about it.
6. IS THERE A WORLD-MAP EDITOR? Levels are authored in the editor; "data-driven
   like levels" implies the world map is authored too, but no editing surface
   was mentioned.
7. TOWNS AND SHOPS are a "might" — and a shop implies money, trade and prices,
   none of which exist. Scope boundary needed, not a design.
8. DOES THE DUNGEON TIER CHANGE EXISTING CONTENT? `showcase`/`level1` are
   currently loose stems in a flat list. Whether they become a dungeon, and
   whether old saves (which name a level stem) still load, is undecided.

Answers
-------

Michael's answers to the open questions, 2026-09-09. Numbering matches above.

1. BOTH — like a level, there's content and then a diff for the save.
   (So the world takes the same two-layer split every level already has: an
   authored static layer that ships with the project, and dynamic state that
   only ever lives in the save as a diff on top of it.)
2. The party will be "in the world, at coordinates x,y". The view will be the
   world map with some sort of icon for the party and locations, etc.
3. (Is the world map a grid?) Probably. Not sure yet. — OPEN.
4. (Saving during a random encounter?) Yeah, this will need to be handled
   differently. — GAP, deliberately parked. Not decided; do not design around
   an assumed answer.
5. (Does travel cost time?) Yes.
6. (A world-map editor?) Not sure yet. — OPEN.
7. (Towns and shops?) Yes, that needs money and trade. That's a "to do" — a
   known follow-on, NOT part of this branch's scope.
8. (Does the dungeon tier change existing content?) We won't need to keep
   existing content. — So no migration path is owed: existing levels may be
   re-homed or discarded, and OLD SAVES NEED NOT LOAD. That frees the save
   format from back-compat for this change (a clean break rather than another
   version rung).

Later decisions (after the plan)
--------------------------------

- `reset` (the eval harness) STILL MEANS A LEVEL for now — so P4 must give the
  harness its own explicit entry rather than inheriting whatever a new game
  starts as.
- REFUSE old saves on the version bump. "Early saves are WIP only."
- SUPPLIES ON TRAVEL: don't worry about consuming food etc. continuously while
  travelling. In a dungeon it stays a continual tick as now; on the world map we
  determine JOURNEY DURATION and then move and tick differently. The costs
  portion of the tick may need refactoring out of where it is now, but THAT CAN
  WAIT. (2026-09-09 — supersedes the plan's original party-tick extraction.)
