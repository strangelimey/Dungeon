# ui-panels - plan (2026-09-30)

From docs/ui-panels-notes.md ("Organized" + Michael's answers to Q1-Q4). Q5
(Lock / Reset layout) was not asked and is still marked (assumed).

## The shape

Five phases. The look first (it touches every screen but moves no geometry),
then the layout (floating panels), then the second layout (minimal) built on
top of the floating machinery. Each phase ends with its checks and a commit.

Commit 0 (before P1): the carried per-dock scale / opacity work, as it stands -
it is built and checked, and P3 then says what happens to the scale half.

## P1 - Stone chrome in layers

Today a skin part is ONE baked 9-slice image per chrome kind (panel, button,
slot). A stone look that can be switched live needs the stone and the shape
drawn separately:

- STONE FILL: a seamless stone tile, tiled across the rect at a fixed texel
  scale (so grain density is the same on a button and a window-wide panel).
  The tile is already toned to UI darkness by the bake, so the runtime does no
  tone maths - it only tiles.
- FRAME OVERLAYS, stone-independent, 9-slice, mostly transparent: white
  highlight top/left, black shadow bottom/right, a thin dark outline.
  One per kind: `panel` (raised, soft), `button` (raised, tighter),
  `button_down` (the same inverted - pressed), `slot` (sunken, dark centre).
- SHEEN: one soft polish highlight, stretched over panels only.

Skin grows a `stone` texture + those overlay parts; ui::DrawPanelFace,
DrawButtonFace and a NEW ui::DrawSlotFace compose fill + overlay. Hot / active
keep washing the theme's control colours over the face, disabled keeps
dimming. The flat look stays whole as the debug mode (uiskin=0). The wooden
kit parts (skin_button / skin_slot) are retired once stone buttons and slots
are in ("the stone should also apply to the buttons and slots").

Assets, all committed source like the other UI images, all script-made:
- tools/BuildUiStones.py: the curated list (start: armani-marble,
  granite-gray-white, gray-granite-flecks, blackrock, plus 2-3 more from the
  contact sheet) -> assets/ui/stones/<name>.png, 512px, seamless (the archive
  tiles are seamless at full size; the script only resizes and tones), toned
  to one mean luminance so any stone lands at panel darkness.
- tools/BuildUiBevels.py: the overlay parts -> assets/ui/frame_*.png.

Slots: every item socket goes through DrawSlotFace - HUD hand boxes, the party
bar's portrait-adjacent slots, the sheet's backpack / doll / bag row / lists,
the party inventory. Five kSlotBg DrawRect sites today (CharacterSheet_Inventory
x3, _Lists, InventoryWindow, CharacterPanel) plus HandSlot's own ring.

Checks: uioverlap sweep (a socket's inset may move a pixel - content must stay
inside), AllocTest plain + -Sheet (draw only, no allocation expected), SRV live
count (one stone + a handful of overlays loaded, not every stone), release
build.

## P2 - The stone picker

Settings -> UI gets a "Stone" dropdown listing assets/ui/stones/*.png, scanned
ONCE at startup (BuildStaticUi, never per frame). Picking swaps the Skin's
stone texture (load the new one, then drop the old - the SRV drain rule:
WaitIdle before the old texture is freed, since in-flight frames still sample
it). Live, like the Textured UI checkbox. ini `ui_stone=<name>`; a missing
name falls back to the first stone, logged.

Adding a stone later = one line in BuildUiStones.py and a re-run.

(Q4) The stone reaches the game screens: HUD, menus, settings, pause, saves,
sheet, item dialog. Editor dialogs keep their current look - check at P1 which
of them draw through the skinned helpers today and give those an explicit
"no stone" path rather than letting the skin reach them by accident.

## P3 - Floating HUD panels

(Q1) Floating: the party bar, the status plate, the options plate, Movement,
Hands, Magic - AND the character sheet and the party inventory, which become
floating windows like Grimrock's inventory. The message log stays
screen-anchored along the bottom (it already sizes itself to the footer and has
its own hide/show).

The sheet scales differently from the HUD panels, and more cheaply: it has its
own UIContext (m_sheetUi), so its scale is that context's ROOT font size
(UpdateFonts' kSheetFontH x scale) - rem itself moves and every rem-based
detail on every tab follows, with no em conversion. The party inventory lives
in the HUD context, so it takes the dock route (fontScale + em). Both keep
their current default size and position until moved.

A HUD panel becomes a ui::FloatingPanel (new, UI lib): a top-level child of the
HUD root whose bounds are a SAVED rect instead of a layout result.
- MOVE: left-drag on the panel's own background (not on a control) past a
  small threshold. Left, because right-drag is mouse look in the 3D view and a
  panel's pixels are not the view's.
- RESIZE: a corner grip, UNIFORM scale about the opposite corner. The docks
  are aspect-locked (square cells and boxes), so a free resize would only leave
  dead space. The scale drives the panel's fontScale exactly as the commit-0
  sliders do - the em-based layout inside the docks is already there. The party
  bar and the plates move to em the same way.
- (Q2) BOTH: the drag and the Settings -> UI sliders edit the SAME saved
  scale, so dragging moves the slider and the slider resizes the panel. Every
  floating panel gets a scale + opacity pair (the party bar's existing pair and
  the three dock pairs carry over; the plates, the sheet and the inventory gain
  one). Settings -> UI groups them per panel under one "HUD panels" section.
- Clamped inside the window; stored as window FRACTIONS + a scale, so a
  resolution change keeps the layout (ini `hud_panel_<id>=x,y,scale`, absent =
  today's default position).
- (Q5 assumed) Settings -> UI "Lock HUD layout" (default OFF while this is new)
  and "Reset HUD layout".
- The pointer shows the move / resize arrow over a grip: tool-refinement added
  Window::SetCursorShape for its dock edges (35bd46e, unmerged). Port that one
  function here, or merge tool-refinement first - decide at P3.
- ApplyPartyBarScale's "slide everything under the bar down" goes away: with
  every panel placed by its own rect nothing is below anything. The party-bar
  scale slider stays but now scales the bar uniformly, like every other panel,
  instead of growing it taller at a pinned width.
- Panels may overlap each other when the player puts them there, so uioverlap
  treats top-level floating panels as overlapOk with each other - but still
  audits everything INSIDE each panel.

P3 is the biggest phase; it splits into P3a (the FloatingPanel widget, the HUD
panels, persistence, Lock / Reset) and P3b (the sheet and the inventory as
floating windows), each its own commit.

Checks: uioverlap at default layout and at a dragged / scaled layout; AllocTest
with a scripted drag + resize inside the window (dev command `hudpanel <id>
<x> <y> [scale]` so the harness moves one without scripting clicks); settings
round-trip; minimize still leaves the other docks where they are.

## P4 - Minimal mode

Settings -> UI "Layout: Standard / Minimal" (ini `hud_layout=`). Minimal
replaces the party bar AND the Hands dock with one CARD per member: portrait +
name + the three bars + effect pips across the top, the two hand boxes and the
stance slider below - Grimrock's card.

(Q3) The four cards form ONE floating block, 2x2 in formation order
(Brand front-L, Sera front-R, Maren rear-L, Tilo rear-R) - the layout the hands
dock already uses. Movement and Magic stay separate floating panels, as in
Standard. A short roster fills the block the way HandsArea does today (2 wide,
2+1 for three).

Built by REUSE, not copies: CharacterPanel's portrait / bars / effects drawing
and HandPair move into pieces both layouts compose, so a card is those two
stacked. Switching layout changes the widget tree, so it is a DEFERRED HUD
rebuild (the RebuildForRoster / m_pendingLanguage pattern), never from the
dropdown's own callback. Each layout keeps its own saved panel positions.

Checks: uioverlap in both layouts, the sheet / portrait clicks and hand clicks
all reachable from a card, AllocTest in Minimal, a 1-, 3- and 4-member roster.

## P5 - Wrap-up

InGameTest sweeps gain the Minimal layout and a moved-panel layout; CLAUDE.md
section; hand the release build over for the feel pass (stone choice, bevel
strength, grip size, card proportions).

## Risks

- Busy stone: the first stone skin (799b0a6) was replaced because it fought the
  wooden buttons. Stone everywhere removes that clash, but a busy stone behind
  item icons still hurts - the curated list keeps the calm ones, and the bake's
  tone step also eases contrast.
- The HUD rebuild on a layout switch is the same deferred path the roster uses;
  a panel pointer cached across it would dangle (the UIContext Clear rule).
- Scale x font: every panel size maps to a font size; FontLibrary rounds to
  whole pixels, so a drag bakes at most ~17 sizes per role - well under its
  64-font warning, but a drag should update the font only on a settled size if
  the bake shows as a hitch.
