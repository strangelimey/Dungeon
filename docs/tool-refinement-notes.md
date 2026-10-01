# Tool refinement - notes

Michael's brain dump, captured as given (2026-09-30). Organize and plan come after.

## Raw notes

- Streamline the workflow: create new world -> add level -> build level -> populate level.
- We need to work on the palette.
- Too many things in the palette of completely different types - e.g. a selection for wall, then a selection for world or effect, etc.
- We need categories. Probably a row or two of icons to switch between the categories.
- While building a new world/dungeon, we want to reduce repetition.
- You can create a STYLE, such as 'small crypt', 'dirt tunnels', 'guard barracks' etc., that creates the things you'll need - e.g. a brush that lays a stone corridor, or a dirt room, or a winding marble tunnel, etc.
- We'll also need a catalog of the likely monsters found in the dungeon/level, and their power levels.
- We'll need some sort of LOCAL 'quest item' area and a GLOBAL 'quest item' area. These will include items and/or flags.
- An OVERVIEW readout, a bit like the status bar at the bottom, listing things like: # of levels, # of monsters, # of monsters on this level, # of quest items, etc.
- "There will be a lot more but this is a good start."

## Organized (2026-09-30)

The workflow is the spine; everything else hangs off one of its stages.

### 1. The workflow: new world -> add level -> build level -> populate level
The organizing idea for the branch. Each note below serves one stage.
What exists: NewWorldDialog (blank / copy / level / wizard), the [+] level
generator with genpresets, the paint tools, placement brushes.
Open: what does "streamline" mean concretely - fewer dialogs, a guided
sequence, or the stages simply being where you expect them?

### 2. Palette: categories, switched by icon rows
Today it is ONE accordion of 19 sections (kCategoryInfo) mixing brushes you
paint (walls, floors, monsters, doors) with world-level definitions you only
edit (dungeons, terrain, quests, effects - not placeable).
Want: categories, with a row or two of icons to switch between them.
Open: what are the categories? The workflow stages would be one natural cut
(World / Build / Populate), but that is a proposal, not his words.

### 3. Styles, to cut repetition
A style ('small crypt', 'dirt tunnels', 'guard barracks') creates the brushes
you will need: a stone corridor, a dirt room, a winding marble tunnel.
Existing things that each hold a PIECE of a style today:
- themes.cat - a look (one wall + floor + ceiling)
- genpresets.cat - a shape recipe (room size, winding, branches...)
- tags - which content fits (undead, stone, cave)
What does not exist: a brush that lays SHAPE (a corridor, a room) rather
than one square.
Open: is a style the bundle of those three plus a monster list? Does it
live world-wide (like themes) or per project? How is a corridor/room brush
used - drag a path, stamp a room?

### 4. Monster catalog per dungeon/level, with power levels
Likely monsters for a dungeon or level, and how strong each is.
Existing: monsters carry tags; the generator ranks its pool by a derived
threat. There is no authored "power level" and no per-dungeon roster.
Open: an authored list (part of the style?) or a readout of what fits the
tags? Is power level authored or derived from stats?

### 5. Quest items: local and global
A local and a global quest-item area, holding items and/or flags.
Existing: quests.cat (quest = ordered stages); an item advances one via
`quest = <id>:<stage>`; progress is save state. Global flags DO exist at
runtime (WorldState::flags, key=value strings, set by an item's `flag =`,
saved as `flag` lines) but are not authored or listed anywhere in the
editor. No local scope.
Open: does local mean level or dungeon? Are flags separate from quest
stages (a lever pulled, a door once opened)?

### 6. Overview readout
Like the sheet's status bar: # levels, # monsters, # monsters on this level,
# quest items, etc.
Open: where does it sit (the editor's foot?), and at what scope (world /
dungeon / level, or all three)?

### Answers (2026-09-30)
- Streamline: ALL FOUR hurt - too many dialogs, no guided order, things hard
  to find, too much repetition.
- Palette categories: BOTH cuts - by workflow stage AND by kind of thing,
  with a toggle to switch back and forth.
- Style bundles: look + shape + content tags + monster list (so the monster
  catalog, note 4, is part of a style).
- Style scope: BOTH - a shared library across worlds plus world-local styles.
- Shape brushes: ALL FOUR - drag a path (corridor), drag a room, stamp a
  pre-made shape, generate a marked region.
- Monster power level: DERIVED from the monster's numbers, overridable by an
  authored value.
- Quest scope: local = the DUNGEON (global = the world).
- Flags: SEPARATE named on/off flags alongside quest stages.
- Overview: a COLLAPSIBLE PANEL (like the legend dock), covering world /
  dungeon / level, chosen by a 3-way toggle.

### Gaps noticed
- Styles (3) and the monster catalog (4) may be one thing - a style names
  its likely monsters. Worth deciding together.
- Palette categories (2) are easiest to settle after styles, since a style
  may itself be a palette category or the thing a category filters by.
