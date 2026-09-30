# ui-updates - plan

Built from `docs/ui-updates-notes.md` (Michael's dump + answers, 2026-09-30).
Three features: a status bar on the character sheet, a new item mouse mapping,
and an item details dialog with a spinning 3D model.

## Decisions carried in from the notes

- Status bar: name + one line for whatever is under the pointer, on EVERY sheet
  tab. Blank when nothing is hovered. Items show name + weight; a bag's weight
  includes its contents.
- The armor tooltip stays exactly as it is.
- Item buttons, everywhere an item appears (sheet backpack, doll, bag row, HUD
  hand slots, floor items): LEFT unchanged, RIGHT = details, MIDDLE = use menu.
- In the 3D view: a right-click WITHOUT moving opens details for the floor item
  under the pointer; a right-drag stays mouse look.
- Details dialog: name, category, weight, weapon numbers, armor/resists,
  food/water, a written description, beside the model spinning slowly.
- Nothing pauses: the world keeps running under every sheet tab and under the
  dialog.
- An item with no use: middle-click opens nothing and logs
  "<name> finds no use for that item." in the member's colour.

## Interpretations to confirm (defaults I will build unless told otherwise)

1. HELD ITEM in the status bar: when the cursor carries an item and the pointer
   is over no other item, the bar shows the carried one. Over a slot, the slot
   wins.
2. EMPTY HAND, right-click (HUD or doll): nothing to describe, so nothing
   happens. Middle-click still opens the hand's menu (punch/kick, quick-cast
   spells) exactly as right-click does today.
3. Middle-click on a PACK or non-hand DOLL item offers only the uses that make
   sense off-hand: memorize, eat, drink. A weapon's attack verbs are hand-only,
   so a sword in the backpack "finds no use". Eat/drink from the pack is NEW
   (today only memorize works there).
4. While the details dialog is up it takes the mouse and Esc (Esc closes the
   dialog, not the sheet under it). Movement keys still walk the party when it
   was opened from the 3D view, as the map overlay does.
5. Spells and Effects rows already print their description in the row; the bar
   repeats a one-line form of it (ellipsised to fit) rather than inventing a
   second text.

## Constraint that shapes the build: the allocation guard

The sheet's frames are GUARDED (Game::SteadyStateFrame): opening it, hovering,
and switching tabs must allocate nothing, and so must opening the dialog, since
that is a click in a settled frame. So:
- Status bar text is formatted into fixed buffers (loc::Line / loc::FormatLine),
  the way the carry-load line already is.
- The details dialog is BUILT ONCE (at BuildStaticUi, like the sheet) with
  reserved strings; opening it only fills fields and flips visibility.
- The 3D preview reuses Game's existing `m_modelPreview` render target (the
  editor dialogs that also use it cannot be open at the same time as a play
  dialog); its submesh list fills a reused buffer, not a fresh vector
  (`ItemPreviewSubs` returns a vector today - add a by-TYPE fill variant).
- Checked by a new `AllocTest.ps1 -Sheet` run: open sheet, hover every tab,
  open/spin/close the dialog.

## Phases

### P1 - Status bar
- A bottom band in CharacterSheetLayout.h (rem-sized, below every tab body), and
  a `StatusBar` draw fed by one `m_status` line the sheet sets each Update.
- Inventory: doll cells, pack grid, bag row (bag + contents weight), held item.
  Weight formatted in tenths like the load line (`sheet.status.item` =
  "{} - {} kg").
- Stats: the five bars (health/stamina/mana/food/water) and five attributes
  hit-tested in the direct draw; one-line hints as new lang keys
  (`attr.<x>.hint`, `bar.<x>.hint`).
- Skills/Spells/Effects: SheetList reports the hovered row (ScrollArea-clipped,
  so a scrolled-out row is not hot); skills get `skill.<x>.hint` keys; headers
  show nothing.
- Done when: every tab's hover shows the right line, blank off-target,
  uioverlap clean.

### P2 - Mouse mapping
- CharacterSheet: right-press on doll / pack grid / bag row fires
  `onItemDetails(itemId, member)`; middle-press fires `onItemUse(where)`.
- GameUI: `OpenPackUseMenu` generalises to `OpenSlotUseMenu` (pack or doll
  slot) with memorize/eat/drink; empty -> `log.no_use`. Doll HAND cells use the
  full hand menu (OpenHandUseMenu).
- HUD hands: HandSlot gains a middle-click callback (ControlBar deps
  `onHandMiddle`); right-click becomes details (nothing on an empty hand).
  Left-click unchanged, including "an unset hand with nothing to do opens the
  menu".
- New: eat/drink from a pack slot (ConsumeItem already takes an item id; the
  slot clear moves to the caller).
- Lang: `log.no_use` x5.

### P3 - Item details dialog
- `ItemDetailsDialog` in GameUI's overlay context via BuildDialogChrome: title
  = item name, close box top-right, Esc closes. Body: PreviewPane (left) +
  a Stack of rows (right) that shows only the rows the item has: category,
  weight, damage/speed/skill/reach/element, armor/class/resists/wear slot,
  nutrition/hydration, description (wrapped).
- The data comes from DungeonWorld::ItemKind through one
  `DungeonWorld::ItemDetailsFor(id)` readout, so the UI never reads catalogs.
- Game renders the preview each frame the dialog is open (orbit advances with
  real time, a slow ~20 s turn), then blits it into the pane.
- Dev: `itemdetails <item>|off`, so the harness and uioverlap can reach it.
- Lang: row labels + `itemcat.<category>` x5.

### P4 - Floor items
- Game::Update: remember the right-press point; on release within 3 px, pick
  the floor item under the pointer WITHOUT lifting it (a read-only sibling of
  TryPickItem sharing its pick math) and open details. A drag is untouched
  mouse look.

### P5 - Descriptions
- `item.<id>.desc` for all 34 items (20 items + 8 weapons + 6 armor), English
  written first, then de/es/it/ru. A missing key would render as the key, so the
  dialog is not done until every item has one. The `item.<id>` convention
  already names things by id; a NEW item needs a `.desc` too (add to the
  "Adding a weapon" checklist in CLAUDE.md).

### P6 - Checks and docs
- `/check-ingame` (uioverlap sweep incl. the sheet's status bar and the dialog
  via `itemdetails`), `AllocTest.ps1 -Sheet`, `/check-build` (both configs),
  `/check` quick tier.
- Hand to Michael for the feel pass (spin speed, bar wording, dialog size).
- CLAUDE.md: the item mouse mapping, the status bar, the dialog; docs as needed.

## Out of scope (noticed, not asked)
- Middle-click on a floor item in the 3D view.
- A status bar on the HUD itself.
