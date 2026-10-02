# More UI updates - plan

Branch `more-ui-updates`, worktree `C:\Dev\Dungeon-more-ui-updates`, off main
1a55ac3. Michael's notes, the organized groups and his answers are in
docs/more-ui-updates-notes.md. Six phases. The order is chosen so each look is
judged on the surface it will finally sit on: the STONE first (everything else
is drawn on it), then the cut-stone BUTTON and its first user (the movement
pad), then its other two users (sheet tabs, pause menu), then the sheet's "All"
view, which is the largest piece and touches the most code.

Standing rule: every phase ends with both configs built, `uioverlap` over the
screens it touched, the AllocTest modes named, and the game HANDED TO MICHAEL
fresh for a look. One commit per phase.

## Phase 1 - A lighter panel stone (B, answer 5)

DONE, and grown in play (2026-10-01): the tab became MATERIAL, with wood /
forest / snow / rock families beside 8 light stones; the PLACE picks the
material (a level's `uistone`, else its dungeon's `ui_stone`; "follow" is the
default, any tile pins one); the Level settings dialog authors it with
thumbnails, category buttons and a live preview that Esc reverts. See CLAUDE.md
"Stone chrome" for how it is built.

Today: tools/BuildUiStones.py tones every stone to a mean luminance of
0.18-0.21 ("about 0.2 is panel darkness"), so the darkness is the TARGET, not
the stone choice. Default `uiStone = granite_grey` (GameSettings.h:142).

- Survey the archive for calm, naturally light stones. Candidates the search
  found (continuous surfaces, no block joints): 2k/countertops white-marble,
  cloudy-veined-quartz-light, marble-speckled, stringy-marble, granitesmooth1,
  speckled-countertop1, pebbled-counter, fleshy-granite1; 2k/rocks limestone5,
  limestone6, limestone_flat_textured, limestone, marble_white, rock_smooth,
  sandstonecliff, speckled-granite1, granite5. Tile sets with joints (floor
  tiles, temple blocks) are out - a panel face must be seamless.
- `find_albedo` reads png/jpg only; teach it .tif (PIL reads 8-bit TIFF), or
  limestone / marble_white / rock_smooth are skipped as "NO ALBEDO".
- A STONE TAB in Settings (Michael, during planning) replaces the UI tab's
  "Stone" dropdown: a FILTERED LIST of the stone textures to choose from,
  each a THUMBNAIL of the stone (small - the stones are 1024px PNGs, so a
  tile is downscaled at load or baked beside it by BuildUiStones.py, never
  the full texture per tile), a filter box, and the current one marked.
  Picking applies live (LoadStone already WaitIdles) and saves `ui_stone=`.
  Built once at page build like the other tabs; the tiles load when the tab
  is first shown, not per frame. It lists the BAKED UI STONES only (plan
  answer 6), filtered by name and light/dark.
- The light candidates join STONES at ~0.30 (with `--check` seam scores in
  the commit), get `stone.<name>` keys x5, and Michael's pick from the tab
  becomes the default. The existing dark stones stay in the list.
- Watch for: the bevel overlays (frame_*.png) and the theme's text colours
  were tuned on a 0.2 stone. A lighter face may want a lighter `textDim` or a
  stronger bevel shadow - judged on screen, adjusted in BuildUiFrames.py /
  the theme defaults, not per call site.

Checks: `BuildUiStones.py --check` (seam), uioverlap hud + sheet, screenshots
of HUD / sheet / pause / settings on the new stone. No code allocation change.

## Phase 2 - The cut-stone button, on the movement pad (A, answers 1, 4)

DONE (2026-10-01). Built as planned, with two lessons from tuning the etch: a
blurred-mask depth is flat-topped (the gold hid the walls), and a distance
depth normalised by its deepest texel pools the gold in the joints - so the
depth is the distance from the cut's edge scaled by the stroke's half-width.
Checked by the new AllocTest `-Walk` (mutation-checked).

Today: MovementPad (ControlBar.cpp:52-106) is six `ui::Button`s whose face is
a round Wenrexa disc with a baked gold chevron (icon_chevron / icon_chevron2,
no script made them). No stone, no bevel. A click fires AFTER release, at the
bottom of the sink (Button::UpdateSelf, Controls.cpp:305-353). Keyboard moves
(Party::HandleInput) never touch the pad.

"Like the rune tablets": the 2D rune_icon_*.png are actually flat two-colour
tiles. The real carve is the 3D tablet texture (RuneBaker.cpp
BakeRuneTextureSet: glyph coverage -> recessed height -> normal -> lit groove,
occlusion in the groove, accent wash). That is the recipe to reuse.

- ASSETS, by script (`tools/BuildCutStone.py`, committed with its output):
  - `frame_block.png` / `frame_block_down.png`: a cut block's bevel - a wider,
    chamfered band than frame_button (a block, not a plate), light only, the
    BuildUiFrames.py approach, so ANY stone shows through.
  - `etch_<name>.png`: one per glyph. The glyph's coverage is blurred into a
    HEIGHT field (a V or U groove), lit from the top-left like the bevels:
    the upper-left wall in shadow, the lower-right wall catching light, and
    GOLD only on the groove's FLOOR (the deepest band), not the whole stroke.
    Light/shadow only plus the gold, so it is stone-independent too.
  - `etch_<name>_lit.png`: the same with the gold brightened and a faint glow
    in the groove (the "current tab" state, Phase 3).
  - Movement glyphs reuse BuildToolIcons.py's arrows (chevron, double chevron,
    turn arrow) drawn as masks - one file per direction, never one rotated,
    since rotating would turn the lighting with it (the same rule the box
    glyphs follow).
- DRAWING: `ui::DrawCutStone(batch, rect, skin, glyph, pressedDepth, lit)` in
  UI/Skin: the stone tile (screen-anchored as now), the block bevel (up or
  down by depth), the etch, the content sinking by depth like a label does.
  Every cut-stone site goes through it.
- PRESS: a `Button` option `fireOnPress`: the action runs on the press, the
  animation (sink, hold, rise; eased with Core/Easing.h) plays on its own and
  never delays it. The movement stones use it.
- KEYBOARD: Party gets an act counter + last MoveAction (bumped in `Act`,
  whatever the source). MovementPad reads it each Update and presses the
  matching stone visually (a public `Button::PressVisual()`). A held W
  repeats steps, so the stone re-presses per step. Clicks also go through
  Act, so a click and a key look identical.
- The pad stays 3x2, cells stay square; each cell is now its own stone with
  a small gap of panel between them.

Checks: AllocTest default (the pad and the keyboard path in a guarded frame),
uioverlap hud, `/check-ingame`. Screenshot at rest, pressed, and mid-walk.

## Phase 3 - Sheet tabs in cut stone (A, answer 2)

DONE (2026-10-01). ModeButton is now a ui::Button subclass (the push, fire on
press and the cut-stone draw come with it; the hand-drawn glyphs stay as the
flat fallback). The tabs grew 30 -> 43 px: at the old size no etch read.

Today: ModeButton (CharacterSheet.cpp:259-312) draws each glyph from
DrawRect / DrawTriangle primitives (grid, bars, star, diamond, hourglass) on
a flat theme fill; active = controlActive fill + accent border.

- Five etch glyphs from BuildCutStone.py (backpack, stats, skills, spells,
  effects), redrawn properly rather than copying the primitive shapes.
- ModeButton draws through `DrawCutStone`. The CURRENT tab is BOTH sunk (held
  at full depth) AND lit (`etch_*_lit`). A click fires on press (it only
  switches the tab).
- Tab / Shift+Tab press the stone they land on, as the keyboard does on the
  pad.

Checks: AllocTest -Sheet, uioverlap sheet (`sheet <m>` puts it in the sweep).

## Phase 4 - The pause and title menus (A, answer 3; plan answers 1, 2, 5)

DONE. Plus, at Michael's ask after seeing it: the world selection, load and
save pages on the same stone (a PageCard with its title carved; slots are cut
stones that push; Save and Back carved; the delete mark a carved cross, red
only under the pointer). The carved text's lit edge was cut to a 1 px whisper
after "a pale outline behind the text that makes things blurry".

Today: BuildPauseMenu (GameUI.cpp:859-902) is a bare `ui::MenuList` - text
only, the selected row an accent fill + border with `>` `<` markers - over a
0.55 black wash, the title drawn raw above it (RenderPauseOverlay,
GameUI.cpp:2296). Nothing uses the stone. The landing menu (BuildMenuList,
GameUI.cpp:319) is the same widget, and BOTH get the new look.

- A STONE PANEL (`DrawPanelFace`) holding a TITLE (pause: "Paused"; landing:
  the game title stays over the art, the panel holds the entries) in the
  Display font, and the entries.
- Each entry is a cut-stone button: a wide block (not square), its WORD
  CARVED at runtime (words cannot be pre-baked per language): the text drawn
  as a dark offset up-left, a light offset down-right and the gold fill
  between - the same lighting as the baked etches, so a word and an arrow
  read as cut by the same hand.
- Keyboard selection (Up/Down) and hover show the way a current tab does: lit
  gold. Enter presses the stone.
- TIMING: a menu entry acts ON RELEASE over it (drag off to cancel); the
  stone sinks on the press and the animation still plays. Only the movement
  stones and the tabs fire on press.
- LAYOUT: entries in a `ui::Stack` on the panel, sized in rem; the panel sized
  to its content and centred. The entries themselves do not change.

Checks: InGameTest `sweep_paused` (uioverlap) plus the landing screen,
AllocTest -Pause, screenshots of both menus.

## Phase 5 - The All window follows the tab (C.1, answer 6; plan answers 3, 4)

DONE, one change from the plan: no separate SheetBody class. CharacterSheet
already owned exactly what SheetBody would have (a member index, the pools,
the lists, the hover and status), so a card is a CharacterSheet in CARD MODE
(Game/PartyWindow.h; CLAUDE.md "THE PARTY WINDOW"). The Inventory tab shows
the sheet's whole tab at card size until Phase 6 reworks it.

Today: "All" (GameUI.cpp:1209) closes the sheet and opens InventoryWindow, a
separate, NON-MODAL four-COLUMN window over the running world, backpacks
only. The sheet is built for ONE member: CharacterSheet holds one m_member,
its row pools are baked per member (SetCharacter) and re-baked every frame,
and GameUI's sheet callbacks resolve through the global m_sheetIndex.

- The All window STAYS A SEPARATE WINDOW over the world and stays NON-MODAL
  (the world is clickable around it; its corner box or Esc closes it). It
  REPLACES InventoryWindow and shows EVERY tab: it opens on the tab the sheet
  was on, and has ITS OWN ROW of the five cut-stone tabs along its top -
  switching changes all four cards.
- Layout: one CARD per member, 2x2 (the MemberCards CardGrid arrangement),
  each card that member's tab at card size, the member's name at its head.
- Implementation: split CharacterSheet's tab bodies out into a `SheetBody`
  that owns a MEMBER INDEX and its own pools (Stats rows, the three
  SheetLists, hover, status lines). The sheet holds one at full size; the All
  window holds four at card size, all BUILT AND WARMED AT CONSTRUCTION
  (nothing added when the window opens - the allocation rule). Callbacks take
  the body's member, never m_sheetIndex. Both windows draw the same code, so
  a stat or a skill row cannot differ between them.
- Compact cards: Stats' bars and attributes; a skill / spell / effect list
  that scrolls within its card.
- The window keeps the `inventory` panel slot (kHudPanelFields: its spot,
  scale and opacity carry over), and gets a status line naming what the
  pointer is over.

Checks: AllocTest -Sheet plus a new -All mode (open, each tab, hover,
close), uioverlap over the All window on every tab, `inventory status` gains
`tab <name>`.

## Phase 6 - The party backpacks (C.2, C.3, answer 6)

DONE. A card's Inventory tab is its own em layout (no doll; the carry load
beside the name; pack row over the contents, six across), at the SHEET'S text
size times the window's scale, so a square is the sheet's. The window's size
follows the tab (PartyWindow::PanelSize, rows = the most any shown member's
bag needs) and its default spot is centred at the other tabs' size, so the tab
stones stay put. The three defects went with InventoryWindow. -Items aims by
`inventory slot`; -Packs still works the SHEET (the same EquipOrSelectPack).
A 16-slot bag (three rows) would not fit a 900-px window - none is authored.

- The All window's Inventory tab: each card shows that member's PACK ROW
  (four bag squares, select / equip exactly as the sheet's - the same code
  now, EquipOrSelectPack) over the selected bag's contents at the SAME slot
  size as the single-member backpack (6 columns).
- Size: at the sheet's slot size a card needs ~0.28 of the window wide and
  four rows (pack row + three content rows for a 16-slot bag) tall, so the
  2x2 is about 0.6W x 0.8H at scale 1 - TALLER than the other tabs need. The
  window GROWS to fit while its Inventory tab is showing (its FloatingPanel
  size follows the tab) and shrinks back after.
- Replacing InventoryWindow fixes three defects for free, since the shared
  code already handles them: no PackAccepts check (a bag could go in a bag),
  no container equip, and a bag past 6 slots drew off the panel.
- AllocTest's `Get-InventorySlotPoint` hard-codes InventoryWindow's geometry;
  `-Items` / `-Packs` read slot points from a dev readout instead of
  hard-coded fractions.

Checks: AllocTest -Items, -Packs, -All; uioverlap; a hand-off for cross-
member swaps.

## Answers to the plan's questions (Michael, 2026-10-01)

1. Menu entries are CARVED at runtime (dark/light offsets, gold fill).
2. BOTH menus - the pause menu and the title screen's - get the cut stone.
3. "All" stays a SEPARATE WINDOW over the world, with ITS OWN ROW of tab
   stones.
4. It stays NON-MODAL, like today's backpack window, which it replaces.
5. Menu entries act ON RELEASE; the movement stones and tabs act on press.
6. The Stone tab lists the BAKED UI STONES only (filter: name + light/dark).
