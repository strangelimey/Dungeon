# more-ui-updates - notes

Michael's brain dump for the `more-ui-updates` branch (off main 1a55ac3,
2026-10-01). Captured as given; organized and planned only once he says so.

## Raw notes

- Movement buttons: a 3x2 grid of SQUARE stones, with beveled edges.
- Instead of the gold paint for the symbol, ETCH it into the stone, similar to
  the runes. Put gold paint in the BOTTOM of the etched groove.
- They should have the standard click animation: pressed, action, un-pressed.
- The 'sheet' buttons on the character sheet (backpack, stats, etc.) are just
  flat 2D. Build better ones.
- The stone used for the panel backgrounds is a little dark. Find a slightly
  lighter one.
- The pause menu needs a lot of work. We'll need the same cut, beveled stone.
- In a character sheet, pressing "All" always brings up all the backpacks. It
  should bring up whichever tab you were viewing - if you were viewing stats,
  it brings up each character's stats, etc.
- On the "All" screen, when viewing the backpack, we'll need the 'packs' along
  the top, just like in the regular backpack, and the player can switch
  between them.
- In the "All" backpack view, keep the backpack squares the same size as they
  are in the regular backpack.

- (added during planning) Make the stone background a SETTINGS TAB in the
  dialog that shows a FILTERED LIST of the stone textures to choose from.

## Organized

### A. Cut-stone buttons - one look, three places

The core ask is one button style used in three places.

- **The look:** each button is its own square block of stone with bevelled
  edges. Its symbol is etched into the stone the way the runes are, with gold
  paint in the bottom of the groove instead of painted on top.
- **Behaviour:** the standard click animation - pressed, then the action, then
  un-pressed.
- **Where:**
  1. The movement buttons. They are already a 3x2 grid (MovementPad in
     ControlBar.cpp), but drawn as ordinary skinned buttons on the dock's
     panel with a painted glyph. The new version makes each cell its own stone.
  2. The character sheet's tab buttons (backpack, stats, etc.), which are flat
     2D today.
  3. The pause menu, which "needs a lot of work" and gets the same cut,
     bevelled stone.

### B. Panel stone is too dark

- Find a slightly lighter stone for the panel backgrounds.
- **Finding:** the darkness comes from the build script, not from which stone
  is picked. tools/BuildUiStones.py brings every stone to the same mean
  luminance (0.18-0.21, "about 0.2 is panel darkness"), so switching to
  another of the six installed stones would not make the panels lighter.
  Lightening means raising that target, choosing a new stone, or both.

### C. The sheet's "All" view

1. "All" should open whichever tab you were viewing: stats gives every
   member's stats, and so on. Today it always opens the combined backpacks
   (onShowPartyInventory).
2. In the All backpack view, show the pack row along the top, like the regular
   backpack, so the player can switch between packs.
3. In the All backpack view, keep the slots the same size as in the regular
   backpack.

## Answers (Michael, 2026-10-01)

1. **"Similar to the runes"** = the RUNE TABLET item icons (rune_icon_*): the
   symbol carved into a tablet.
2. **Sheet tab buttons** = the SAME etched stone as the movement buttons. The
   CURRENT tab shows BOTH ways: its stone stays sunk in AND its gold is lit.
3. **Pause menu** = cut-stone buttons, plus a STONE PANEL behind the entries,
   a TITLE, and a LAYOUT / SIZING pass. The entries themselves stay as they
   are (Save / Load / Settings / Exit / Back).
4. **Click animation**:
   - The keyboard presses the stones too: WASD/QE animates the matching
     movement stone, exactly as a click does.
   - The action fires ON PRESS, instantly. The animation is purely visual and
     must never delay the action.
5. **Lighter stone** = FIND A NEW stone (survey the archive for a calm,
   naturally lighter one), CLEARLY lighter than today - about 0.3 mean
   luminance against today's ~0.2, still dark enough for text and icons.
6. **"All"** = EVERY tab gets one: Inventory, Stats, Skills, Spells, Effects.
   Layout: ONE CARD PER MEMBER, 2x2 (four compact copies of the tab, like the
   Minimal layout's cards). The Inventory one also gets the pack row and
   full-size slots (C.2, C.3) - and full-size slots in a 2x2 card are a
   space question the plan has to answer.
