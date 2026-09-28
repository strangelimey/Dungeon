# Play-test notes

Branch `level-tuning` (a92ee5f), release build. Started 2026-09-28.

Issues as reported, in Michael's words, one per entry. Nothing here is fixed or
designed yet.

## Iteration 1 summary (2026-09-28)

One bug, seven requests, in four groups. The order is a SUGGESTION for Michael to
confirm, not a decision.

| Order | Group | Items | Size | Why here |
|---|---|---|---|---|
| 1 | Editor bug | #1 stair brush does nothing | small? | The only bug. Blocks moving stairs today, and a silent click is exactly the failure that goes unreported. Find why before building #2 on top of it. |
| 2 | Character sheet keys | #4 movement keys page members, #5 Tab / Shift+Tab page tabs | small | Two input handlers on one screen, one commit. |
| 3 | HUD right-edge panels | #6 Magic split + auto-hide, #7 movement split, #8 minimize (movement and Magic) | medium | One rework of GameUI::BuildHud. Build the panels with independent positions now so "resize and move with the mouse" can be added later. |
| 4 | Editor map tools | #2 drag-and-drop move (dig through a square), #3 resize a level | medium / large | Both need design answers first (see each entry). #3 is the riskiest: code sized to the cell grid has crashed on a size change before. |

## Issues

1. **Can't move the "stairs up" in the editor.** "I'm in the 'crypt1' level, in
   the level editor. I'm trying to move the "stairs up". When I select it on the
   palette and then click on the map, nothing happens."
   - Repro: editor on crypt1, pick Stairs > stairs up in the palette, then
     left-click a floor square.
   - Observed: no change on the map and nothing in dungeon.log. The click fails
     silently, with no refusal message.
   - Likely area: the stair brush path in MapEditor -> `DungeonWorld::AddStairAt`.
     There is also no "move" for a stair at all; the only way is place-new plus
     erase-old.
   - FIXED (uncommitted, 2026-09-28). Three causes:
     1. Every editor message went to the HUD log, which the full-screen editor
        does not draw. Now shown on an editor message line (bottom of the grid,
        5 s) and written to dungeon.log as `editor: ...`.
     2. The thing at 7,7 is the WAY OUT (stairs_exit), not stairs up. The Stairs
        Up brush on a top floor is refused ("No level above"), correctly, and the
        brush could not place a Way Out at all. It can now; placing one opens the
        stair inspector with a "Leads out to" dropdown of world-map locations
        (Michael's pick: ask in a dialog), defaulting to the doorway that lands
        on this level.
     3. Erasing a Way Out (middle-click, inspector Delete, or painting a wall on
        it) ABORTED the game: the pair cleanup read the location name as a
        level and asserted on the missing .map. Guarded; mutation-tested.
     - Arrival square following a moved exit: deferred to #2 (Michael).
   - FOLLOW-UP (Michael, same session): "The 'way out' at 7,8 has the correct
     facing (N), but the in-game object is the wrong way around. The facing
     means 'direction the player is facing when they enter the level on this
     stair/doorway'." FIXED: StairPropWorld turned every stair prop to the
     OPPOSITE of its facing, on the false belief that the meshes rise toward +Z
     (their FOOT is at +Z). c530853 had chosen that to keep stairs looking as
     before, and before was already inverted. Now the foot faces the facing.
     Data fixed with it: both 1,1 stairs (crypt1 down, crypt2 up) said north,
     into rock, and now say south (how they always looked). A new stair faces
     its first open side (DungeonMap::OpenFacing), not always north.
   - OPEN, from Michael's own crypt1 edit (exit moved to 7,8, skeleton moved to
     1,1): world.map's crypt_gate still lands at 7,7, off the exit, and the
     skeleton stands on the stair down. dungeon-demo is the harness world, so
     LevelBuildTest phase 8 fails with that edit in place (passes without it).
     - Harness: `editor place <cat> <id> <x> <z>` and `editor erase <x> <z>`.

2. **Feature request: drag-and-drop to move things on the editor map.** "We need
   to add drag-and-drop to the editor map so something can be moved. If there is
   more than one thing on the square, just grab the top one and the user can keep
   doing it to dig through the square's contents."
   - Came out of #1: the goal there was to MOVE the stair, not place a second one.
   - Needs deciding later: what "top" means on a square (the draw order? the
     middle-click erase ladder: stair pair, then entity, then fixture?), which
     mouse button drags (left paints, right-drag pans, middle erases), and
     whether moving a stair also moves its paired half on the other level.

3. **Feature request: resize a level from the editor.** "I need a way to adjust a
   level's width and height easily from within the editor screen."
   - Today the size is only chosen when a level is generated (the [+] generator's
     width/height knobs). The Level settings dialog has no size fields.
   - Needs deciding later: which edge(s) grow or shrink (right/bottom only, or an
     anchor), and what happens to content cut off by a shrink. Stairs are pinned
     to the same (x,z) on both floors, so a resize that shifts coordinates must
     move the paired stairs too. Remember the fog-mask crash: things sized to the
     cell grid assumed the size never changes.

4. **Character sheet: movement keys should page members.** "When on a character
   sheet, hitting the movement keys ('a' and 'd' in this configuration) should
   move to the prev and next character, just like hitting '<' and '>' do."
   - His current binding: 'a' = previous, 'd' = next. These are his strafe-left /
     strafe-right keys, so the sheet should read the BOUND keys (MoveKeys), not
     hardcoded letters, and a rebind should carry over.
   - Fits a92ee5f: the sheet already owns the input and the party doesn't move
     while it is open, so these keys are free there.
   - Likely area: the sheet's input handling in GameUI, next to the < > buttons'
     prev/next callbacks.
   - DONE with #5: GameUI::UpdateSheet reads the BOUND strafe keys and goes
     through onOpenSheet, the < > buttons' own path. Not while the sheet's item
     menu is open (it names a slot of the member it opened on). Driven with real
     keystrokes; `sheet status` is the readout.

5. **Character sheet: Tab / Shift+Tab cycle the sheet's tabs.** "When on a
   character sheet, hitting tab or shift-tab should go to the next and prev tabs
   on the character sheet (inv, attributes, etc.)"
   - Presumably wrapping at the ends, like the member paging in #4.
   - Pairs with #4: movement keys page members, Tab pages tabs.
   - Likely area: the sheet's input handling in GameUI. Could live on
     ui::TabControl itself, so every tabbed page gets it (the Settings page and
     the editor dialogs are tabbed too).
   - DONE on the sheet only: its tabs are the sheet's own mode strip, not a
     ui::TabControl, so CharacterSheet::StepMode wraps through the five modes.
     Tab on the other tabbed pages is NOT done (not asked for).

6. **HUD: split Magic into its own bar, hidden until someone knows a symbol.**
   "On the right-hand control bar, split the 'magic' section out of the main bar
   and make it a separate bar below the controls bar. Also, it should not show if
   NO character has any symbols memorized - it will appear as soon as a character
   learns one."
   - Two parts: (a) a separate panel below the controls bar, not a section inside
     it; (b) visible only while at least one member knows at least one symbol.
   - Hidden means the controls bar ends at the hands, not an empty gap.
   - Likely area: GameUI::BuildHud (the right-edge panel: move arrows, hand
     pairs, Magic area). Visibility must be derived each frame (or on learn and
     roster change), not latched, since learning happens mid-game and a load can
     change it.

7. **HUD: split the movement controls from the hands too.** "Also, split the
   movement controls area from the hand controls area. so, there will be 3 areas
   docked on the right of the screen, below the portrait bar"
   - Extends #6. The right edge becomes three separate panels stacked below the
     portrait bar: movement arrows, then hands, then Magic (Magic only when
     someone knows a symbol).
   - The order above follows today's top-to-bottom layout; confirm it at design
     time.
   - Scaling: the party-bar scale slider shifts the panels beneath it
     (GameUI::ApplyPartyBarScale), so all three must follow it.

8. **HUD: the movement panel can be minimized.** "The 'movement' icons panel can
   be minimized as most players will move with the keyboard."
   - Builds on #7 (movement as its own panel).
   - Likely shape: a collapse toggle like the editor docks' flip-arrow strip, with
     the state persisted in settings.ini the way map_palette_collapsed is.
   - ANSWERED: "no, leave it on screen by default." Expanded on a new install.
   - ANSWERED: "when the movement arrows panel is minimized ... do not move the
     hands panel." The other panels keep their positions and the space stays
     empty. (This reverses my guess that they would move up.)
   - ADDED: "add a minimize button to the magic panel, too." Minimizing Magic is
     separate from #6's auto-hide (no symbols known): it is the player's choice
     and only applies once the panel would show.
   - LATER (not this pass): "we'll make each panel resizable and moveable with
     the mouse." Panel positions shouldn't depend on each other, so this is
     easy to add later. It also explains why minimizing doesn't reflow.

