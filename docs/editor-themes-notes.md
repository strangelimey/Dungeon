# editor-themes - raw notes

Michael's brain dump, captured as given. Not organized yet.

- We just created "combinations" in the editor. They are NOT for drawing on the
  map directly.
- Within each combination, the user sets the wall, floor, and ceiling.
- When a combination is armed in the editor, clicking / filling / drag-and-drop
  updates whatever is ALREADY on the square with the relevant contents of the
  combination:
  - a floor square: its floor and ceiling are set from the combination.
  - a wall square: its wall is set from the combination.

## Organized (2026-09-29)

What main already does (MapEditor::PaintComboCell and the fill tools):
- A combination never changes a cell's TYPE (recolour only). Matches.
- Floor square -> floor + ceiling; wall square -> wall. Matches.
- Click, drag stroke, Rectangle, Flood, Area (room + its walls) and Fill level
  all route through that one per-square rule. Matches.

Where the notes DIFFER from what exists:
1. "Not for drawing on the map directly." Today a painted square REFERENCES
   the combination (`surfacemix` records, variant <= -2): editing the
   combination later repaints every square using it, on every level. Read
   literally, the notes describe a PRESET that stamps concrete textures, so the
   square keeps no link.
2. "The user sets THE wall, floor and ceiling." Today each surface is a LIST
   (a mix, varied per square by hash, e.g. marble_hall floor = floor_slabs
   floor_temple). The notes read as one texture per surface.

Open questions for Michael:
- Q1: preset (stamp textures, no link) or reference (keep today's live link)?
- Q2: one texture per surface, or keep the mix lists?
- Q3: is "themes" (the branch name) the new name for combinations, or
  something beyond them?

Answers: Q1 keep the live link. Q2 one texture each. Q3 rename to themes.

## Plan

P1 - Rename combinations to THEMES, all the way down.
- Editor label "Themes" (map.cat.* key x5 langs).
- combos.cat -> themes.cat (Project::combos -> themes; the template too).
- `surfacemix` record -> `theme`. No level uses surfacemix yet, so no alias
  or migration is needed.
- Code identifiers Combo* -> Theme* (DungeonMap slots, MapEditor
  PaletteCat, DungeonWorld Ensure/Refresh, dev commands), eval scripts
  combos/comboedit, EditorTest phase, CLAUDE.md bullet.

P2 - One texture per surface.
- themes.cat `wall` / `floor` / `ceiling` each name ONE id; the type editor
  shows a dropdown per surface (with None) instead of checkbox lists.
- A missing surface still means "leave that surface as it is".
- The per-square hash pick among members goes away; the resolver reads the
  one member directly.
- marble_hall's two floors trimmed to one.
- The live link is unchanged: editing a theme still repaints every square.

Checks: debug + release build, EditorTest, the theme eval suites, uioverlap
on the type editor with a theme open.

