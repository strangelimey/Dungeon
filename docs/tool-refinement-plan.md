# Tool refinement - plan

Built from docs/tool-refinement-notes.md (Michael's dump, the organized
themes and his answers). The goal is the workflow in one line:

    new world -> add level -> build level -> populate level

with less repetition, fewer dialogs, a guided order and tools where you
expect them. Every phase below serves one of those four stages.

The judge is `tools\EditorTest.py` (each phase adds its own mutation-tested
phase there); `uioverlap` runs after every phase that touches a screen.

## What exists (the starting point)

- Palette: ONE accordion of 19 sections (`kCategoryInfo` +
  `kDisplayOrder`, MapEditor.cpp:47-86), hand-drawn with SpriteBatch.
  Brushes you paint (walls, monsters) sit beside definitions you only edit
  (dungeons, terrain, quests, effects). Dungeons/Terrain/Quests show NO rows
  at all: `CategoryItems` has no case for them and returns `{}`
  (MapEditor.cpp:186), so each is just "+ New..." and "(empty)".
- The pieces of a "style" exist, scattered: themes.cat (a look),
  genpresets.cat (a shape recipe), tags (what content fits). All catalogs
  are per project; there is no shared library, only the one-time
  assets/templates/default copy at world creation.
- The generator is whole-level only (`generate::Run`, Generate.cpp:480). Its
  carving helpers (Carve, CarveCorridor, Room shapes, Grower) are private to
  Generate.cpp's anonymous namespace.
- Monster strength is DERIVED already: `threat::Of` (Threat.cpp:64), from the
  catalog stats against a reference party; the generator ranks its pool by
  it. No authored override, and nothing in the editor shows it except the
  `threat` console command.
- Quests: quests.cat (ordered stages), an item's `quest = id:stage`,
  `flag = key[=value]` and `reveals = loc` fire on pickup
  (Game::OnItemFound, Game_World.cpp:444). Global flags already live in
  `WorldState::flags` and the save (`flag` lines). None of the three item
  fields is in the items schema, so they are hand-authored. No dungeon scope.
- Docks: palette LEFT, a static symbol key RIGHT (no Update side, no clicks).
  MapView.cpp is 1925 lines - new dock code goes in its own file.
- Dialogs on the way through the workflow: NewWorldDialog, GenerateDialog
  (create + regenerate), LevelSettingsDialog, TypeEditorDialog,
  AssetDialog, ValidateDialog. Tags are set in Level settings, the palette
  donor in Generate, the look by painting - three places for one decision.

## Phase 1 - Palette categories and the icon rows

The foundation: every later phase adds a category, so the switcher comes
first.

- A CATEGORY BAR at the top of the left dock, above the filter row: one or
  two rows of square icon buttons (the `box_*` family from
  tools\BuildToolIcons.py, tooltipped). Clicking one shows ONLY that group's
  sections in the accordion below.
- Two groupings, with a toggle at the bar's start to flip between them
  (Michael: "both, with a toggle"). One grouping shows at a time: the toggle
  then one button per group, wrapping to a second row when the dock is
  narrow (the bar reserves the larger grouping's rows, so flipping never
  moves the filter under the pointer).
  - BY STAGE: World / Build / Furnishings / Populate (Furnishings split out
    of Populate after Michael tried it).
    - World: Styles (Phase 5), Dungeons, Quests & flags, Terrain
    - Build: Themes, Walls, Floors, Ceilings, Wall features, Surface
      features, Doors, Stairs
    - Furnishings: Decorations, Fixtures, Buttons
    - Populate: Monsters, Items, Weapons, Armor
  - BY KIND: one row of kinds, all visible at once - Surfaces (themes +
    walls/floors/ceilings + features), Structure (doors, stairs),
    Furnishings (decorations, fixtures, buttons - Michael split them out of
    Structure after trying it), Creatures, Items (items, weapons, armor),
    World (dungeons, quests & flags, terrain, styles).
  Both are one data table (category -> stage, category -> kind), the
  kCategoryInfo idiom, so regrouping is a table edit. Michael kept these
  groupings as proposed.
- EFFECTS LEAVE THE PALETTE (Michael: they are tuning, not building). They
  move to an Effects tab in the Balance dialog beside Formula and Attacks,
  still edited through the same schema-driven form.
- The FILTER box searches EVERY category regardless of the bar, and says so
  (a hit outside the current group shows under its own header).
- Dungeons / Terrain / Quests get real rows (`CategoryItems` cases), so the
  World stage is not a column of empty sections.
- Persist: grouping mode + the selected group per mode (settings.ini
  `map_palette_group`, `map_palette_stage`, `map_palette_kind`).
- Files: the bar goes in a new MapEditor_Categories.cpp; MapEditor.cpp stays
  the accordion.
- Checked: EditorTest phase - each group shows exactly its categories, the
  filter reaches across groups, the mode round-trips settings.ini; mutation:
  drop a category from the table and the test must fail.

## Phase 2 - Monster power level

Small, and the overview (Phase 3) and styles (Phase 5) both read it.

- `power` = the derived `threat::Of` value, shown as a readable number and a
  BAND (1-5, cut from the project's own monster spread so the bands mean
  something in every world). BUILT as: Game/Power.h (pure, in RollTest) -
  Resolve (override > 0 wins) and Band (which fifth of the range, linear, not
  by rank, so a runaway strongest kind stands alone); DungeonWorld_Census.cpp
  caches every kind's power per edit revision (the palette asks every frame).
- An authored `power = <n>` on a monsters.cat entry OVERRIDES the derived
  value (Michael: "derived, overridable"). One function answers "how strong
  is this monster" (`Game::PowerOf`), and the generator's ranking goes
  through it too, so the override changes what the generator picks.
- Shown: a band badge on each Monsters palette row, the derived value beside
  the override slider in the type editor (so an override is visibly an
  override), and in the overview.
- Checked: RollTest (threat is a pure TU) - the override wins, absent means
  derived; EditorTest - the palette badge follows an override.

## Phase 3 - The overview panel (and resizable docks)

Michael, 2026-09-30: fold dock resizing into this phase - drag a dock's
inner edge, the width remembered as a share of the panel, the pointer the
left-right arrow over it (a small Window addition: WM_SETCURSOR).

- A collapsible panel in the RIGHT dock, ABOVE the symbol key, each with its
  own collapse header (the key becomes the lower section). Persisted:
  `map_overview_collapsed`.
- A 3-way toggle: WORLD / DUNGEON / LEVEL, where dungeon and level follow the
  VIEWED level.
  - World: levels, dungeons, monsters (total, by power band), items, quest
    items, flags, open issues.
  - Dungeon: its levels, monsters by band, dungeon-local quest items and
    flags (Phase 4), keyed doors.
  - Level: monsters (by band, strongest named), items, doors / keys, stairs,
    issues on this level.
- The counting walk is the Validate one (active level live, stashed level
  from its stash, else `ReadOnlyLevelOf` - NEVER stash to read), exposed as
  one `DungeonWorld::Census()` that returns plain counts. It recounts when
  `EditRevision()` moves and no button is held, exactly like
  `RefreshLiveIssues`, so it costs nothing while you paint.
- Rows are clickable where it helps: an issue count opens Check, a level
  name browses to it.
- Files: MapView_Overview.cpp (+ the key moves to MapView_Docks.cpp, which
  also takes MapView.cpp away from its 2000-line ceiling).
- Checked: EditorTest - the counts match a hand-built fixture world at all
  three scopes and move after a paint / undo; uioverlap with the panel open
  and collapsed.

## Phase 4 - Quest items and flags, dungeon and world

- FLAGS become authored things with a SCOPE (Michael: separate named on/off
  flags; local = the dungeon). A new `flags.cat`:
  `[id] display = ... scope = world | dungeon <dungeon id>`.
  Runtime keeps the existing `WorldState::flags` store; a dungeon-scoped flag
  is stored under `<dungeon>/<flag>`, so the SAVE FORMAT DOES NOT CHANGE.
  On/off is what the editor shows; the store stays key=value so nothing
  already written breaks.
- QUEST ITEMS = any item carrying `quest`, `flag` or `reveals`. Those three
  join the items schema (a "Quest" tab), with dropdowns for quest:stage,
  flag and location instead of hand-typed text.
- The World stage's "Quests & flags" category lists two sections, THIS
  DUNGEON and WORLD: its flags, its quests, and the quest items that touch
  each, with where each item is placed ("crypt2 14,6" - click to browse).
- Quest editing gets its missing half: a row per stage with its text line
  (today `text_<stage>` is hand-authored).
- Validation: a flag nothing sets, a quest stage no item reaches (that one
  exists), an item setting a flag of another dungeon's scope.
- Dev: `flag <id> [on|off]`, `flags [dungeon]`.
- Checked: EditorTest (author a flag + item through the dialogs, the
  sections list them); an eval script picks the item up and reads the flag
  at the right scope; save round-trip.
- FLAG CONSUMERS (Michael: doors, buttons/levers, stairs/exits). Today
  nothing reads a flag; three do after this phase, all as .ent / .map
  record params authored in their inspectors with a flag dropdown:
  - DOOR `flag = <id>`: stays shut until the flag is on (the `key =` rule's
    shape - a wired button still bypasses, as it does a lock).
  - BUTTON `sets = <id>` / `clears = <id>` / `toggles = <id>` when pressed,
    and `flag = <id>` to refuse to work until it is on.
  - STAIR / EXIT `flag = <id>`: the transition refuses (with a message) until
    the flag is on; a world-map location likewise.
  Validation learns them: a door or stair waiting on a flag nothing can set
  is an error (the `doorlocked` check's shape).

BUILT (2026-09-30), and where it differs from the above:
- Flags are stored in WorldState::flags BY ID, not as `<dungeon>/<flag>`:
  flags.cat ids are already unique, so the prefix bought nothing, and the scope
  lives where the editor and the checker read it (flags.cat `dungeon`, absent =
  world). The save format still does not change. WorldState::FlagOn /
  SetFlagOn are the on/off view (on = set to anything but "0"), so a
  hand-written `flag = seal=broken` still reads as on.
- The palette keeps Quests (the quest definitions) and adds a "Quest items &
  flags" section beside it, in both World groups: this dungeon's group and the
  World's, each listing its flags and then its quest items. An item's scope is
  the scope of the flag it sets (the world's when it sets none - a quest and a
  reveal are world-tier). A flag row opens its editor; an item row arms that
  item's brush and ends in a ">" link to where it lies; "+ New..." makes a flag,
  local to the viewed dungeon. Flags of OTHER dungeons are not listed.
- Quest stages: a Stages tab (FieldKind::QuestStages) - one row per stage,
  its id and its log line together, a remove box, an add button.
- The items' Quest tab is on items, weapons and armor alike (OnItemFound reads
  all three).
- Consumers as planned; the door's wait is checked BEFORE its key. A world
  location's `flag=` is honoured (EnterLocation, OfferEntrance) but authored by
  hand - the world editor has no row for it yet.
- Validation (Validate_Flags.cpp): `flagwaits` (error, at the door / lever /
  stair), `flagunknown`, `flagscope`, `flagunused`. Not done: the reachability
  flood still treats a flag-waiting door as passable; "nothing sets it" names
  the unusable case.
- The overview counts flags per scope (World / Dungeon) and how many are on.
- Dev: `flag`, `flags`, `opendoor` (the party's hand on a door), `press ...
  party`, `flagwire` (the inspectors' setters), `editor palette use`.
- Checked: EditorTest 15 (18 checks) on a fixture written into eval_arena.

## Phase 5 - Styles

A STYLE is look + shape + content tags + a monster list (Michael: all four),
in a shared library AND per world.

- `styles.cat` entry:
  - look: a room theme and a corridor theme (themes.cat ids - a winding
    marble tunnel and the chamber it opens into can differ)
  - shape: generator knobs (the genpresets line format, reused as is) plus
    `corridor_width` (Michael: per style - 1 for dirt tunnels, 2 for a
    grand hall)
  - tags: what content fits
  - monsters: `id [weight]` list; power comes from Phase 2
- THE LIBRARY: `assets/library/styles.cat` (plus the themes / genpresets its
  styles use), outside projects/ like the template. The palette's Styles
  section shows the world's own styles first, then library styles greyed
  with "Add to this world". ADDING COPIES the style AND whatever it points at
  that the world lacks (its themes; its surface types); monsters the world
  does not have are listed as missing rather than silently dropped. After
  that the world's copy is its own - no live link, the template rule.
  "Save to library" goes the other way.
- Starter library: small crypt, dirt tunnels, guard barracks, marble halls -
  authored from what is installed.
- Arming a style makes it the CURRENT style: Build's shape brushes (Phase 6)
  paint in it, and Populate's Monsters section filters to its list with
  power bands.
- A dungeon can name a default style (`dungeons.cat style =`); a new level in
  that dungeon starts in it (Phase 7).
- Checked: EditorTest - add a library style into a fixture world and check
  the themes it needed arrived and nothing else did; a second add is a
  no-op; the monster filter follows the armed style.

BUILT (2026-09-30), and where it differs from the above:
- `styles.cat` per world: `room`, `corridor` (themes), `knobs`, `corridor_width`,
  `tags`, `monsters` (`<id> [weight], ...`, Game/Style.h - pure, in RollTest).
  The type editor's style form has Look / Shape / Monsters tabs; the monster
  list is rows (a dropdown naming each monster with its power, a weight, a
  remove box) through a new FieldKind::WeightedRefs. `knobs` is the plain
  settings line for now (Phase 7 hands it to the generator).
- THE LIBRARY is assets/library: styles.cat, themes.cat and the walls /
  floors / ceilings those themes are made of (Game/StyleLibrary.h). Starter:
  Small Crypt, Dirt Tunnels, Guard Barracks, Marble Halls. The demo world
  owns Small Crypt (and its crypt dungeon names it as `style`); the other
  three are offered from the library.
- The palette's Styles section (World group, first): This world, then the
  library's the world lacks. A world style's row ARMS it (again = off); a
  library row ADDS it. Right-click a world style for its editor, whose footer
  has "Save to library" (the style as edited, written to the world first).
- The armed style RANKS the Monsters section rather than hiding the rest: its
  monsters first, a divider, the others - the tags lens's rule, since the odd
  one out is often the memorable one. Not saved: a session's working choice.
- The checker warns when a style names a theme or monster the world lacks
  (an add from the library reports missing monsters and does not copy them)
  and when a dungeon names a style it lacks.
- Rename sweeps: a theme rename rewrites styles' room / corridor, a style
  rename dungeons' `style`, a monster rename styles' lists (weights kept).
  And the Phase 4 gap this exposed: a FLAG rename now rewrites items' `flag`,
  locations' `flag=`, and the door / lever / stair records naming it.
- New worlds: the template carries styles.cat (styles are reusable content,
  like themes).
- Dev: `styles`, `style use|add|save|row <id>`.

## Phase 6 - Shape brushes

Four ways to lay shape in the current style (Michael: all four). Each is one
undo step and one chunk batch; each previews the cells it will change while
dragging, before anything is committed.

- CORRIDOR: drag A -> B. Routes a corridor between them - straight/L for a
  low `winding`, a seeded meandering walk for a high one; the style decides.
  Carves floor at the style's `corridor_width`, walls what it borders,
  paints the corridor theme.
- ROOM: drag a rectangle. Carves the room, walls around it, room theme.
  Joins any corridor it overlaps.
- STAMP: a pre-made shape (round chamber, cross hall, pillared hall,
  L-room...) from a `shapes.cat` of small ASCII grids. Click to place; wheel
  or R rotates.
- REGION: mark a rectangle; the generator fills it with rooms and corridors
  from the style's shape knobs and joins them to whatever open squares touch
  its edge.
- The enabling refactor: lift the carving helpers out of Generate.cpp's
  anonymous namespace into a pure `Game/Carve.h` (grid in, cells out, no
  world) used by BOTH the generator and the brushes, and teach
  `generate::Run` to work inside a bounds rectangle with a mask of existing
  squares (it already honours `keepOpen` / entry, which is the join).
  Whole-level generation must come out byte-identical after the lift - that
  is the check the refactor is a no-op.
- Tools: the four join the tool strip (MapView_Tools.cpp), with icons from
  BuildToolIcons.py.
- Checked: RollTest-style pure tests for Carve.h (a corridor connects its
  ends; a room is closed; a region joins its edge); `generate` regression on
  a fixed seed before/after the lift; EditorTest for each brush + undo.

## Phase 7 - The workflow, wired through

Now the pieces exist, make the four stages one path.

- NEW WORLD: the dialog gains a style pick (from the library); the starter
  floor is built in it and the world receives that style.
- ADD LEVEL ([+]): opens on the dungeon's default style, which fills tags,
  palette and shape knobs in one go - no separate Level settings visit, no
  palette donor stem. Create lands you in the Build stage with the style
  armed.
- BUILD: the palette's Stage view IS the guided order; Build shows the
  shape brushes first, then themes and surfaces.
- POPULATE: the generator's "populate only" (monsters/items by the style's
  list, difficulty, density) on an already-built level - so hand-built
  shape can be populated automatically too, then adjusted by hand.
- The overview's Level view shows where the level stands (built? populated?
  issues?) - the "what next" readout.
- Dialog audit: after this phase, list every dialog the four stages still
  open and fold the ones that exist only because a decision had nowhere else
  to live (tags and the palette donor are the two known ones).
- Checked: an EditorTest script walks the whole path - new world from a
  library style, add a level, corridor + room + stamp, populate, overview
  counts - with no dialog opened that the plan does not name.

## Order and size

1 palette -> 2 power -> 3 overview -> 4 quests/flags -> 5 styles -> 6 shape
brushes -> 7 workflow. Phases 1-3 are small-to-medium and independent of the
rest; 5 and 6 are the large ones; 6's Carve.h lift should land as its own
commit before any brush uses it.

## Decisions (2026-09-30)

- Flags are read by doors, buttons/levers and stairs/exits (Phase 4).
- Corridor width is per style (Phases 5, 6).
- Effects leave the palette for a Balance dialog tab (Phase 1).
- The stage and kind groupings stay as proposed (Phase 1).
