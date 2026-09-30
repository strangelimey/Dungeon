# ui-updates - raw notes

Michael's brain dump for the `ui-updates` branch, captured as it comes in.
Verbatim in substance; not yet organized or planned.

## Notes

- Need a 'status bar' type thing at the bottom of the character sheet. Hovering
  over an item in the backpack, ragdoll, etc. will show the name and weight of
  the item in the status bar.
- Currently, right-click on a rune in the backpack brings up 'memorize'. Want
  to change this. Right-click should bring up a dialog with the item and its
  details. The item will be shown 3D, slowly spinning next to the details.
- Left-click continues to be pick up / put down / swap. Middle-click will be
  where the item's use menu (such as memorize) goes.
- The status bar should work the same on the other tabs (attributes, skills,
  etc.).

(End of dump 1, 2026-09-30: "that's it for now.")

## Organized

### A. Character sheet status bar
- A strip along the bottom of the character sheet.
- Inventory tab: hovering an item (backpack grid, paper doll, "etc.") shows
  the item's NAME and WEIGHT.
- Every other tab (Stats/attributes, Skills, Spells, Effects) uses the same bar
  for whatever is under the pointer.

### B. Item mouse buttons, re-mapped
- LEFT: unchanged - pick up / put down / swap.
- RIGHT: opens an item DETAILS dialog (replaces today's right-click use menu).
- MIDDLE: the item's USE menu (Memorize and the like) moves here.

### C. Item details dialog
- Opened by right-clicking an item.
- The item is shown in 3D, slowly spinning, beside its details.

### Open questions (for the planning step)
1. Status bar on the other tabs: what does it say for an attribute, a skill,
   a spell, an effect? Its name plus a one-line description? What is in the
   bar when nothing is hovered (blank, carry load, a hint)?
2. Hovering an item on the Inventory tab: which targets count? The backpack
   grid and the doll for sure; what about the pack row (the bags) and the item
   held on the cursor? Does a bag's weight include what is in it?
3. The armor tooltip already shows on hover. Keep it alongside the status bar,
   or fold some of it into the bar?
4. Where does the new mapping apply? Only the sheet, or the HUD hand slots
   too? (Right-clicking a hand slot opens today's use menu, which also
   carries attack uses.) What about floor items in the 3D view?
5. What goes in the details dialog? Items have NO descriptions today (no
   item.<id>.desc keys). Candidates: name, category, weight, weapon numbers,
   armor/resists, food/water, a written description. Does the world keep
   running while it is open (the sheet is not a pause)?
6. An item with no use: middle-click does nothing, or shows an empty menu?

### Answers (Michael, 2026-09-30)
1. Yes: name + one line. Show NOTHING when nothing is hovered.
2. Yes (backpack, doll, pack row, held item all count). A bag's weight
   INCLUDES its contents.
3. Keep the armor tooltip as-is.
4. Same as backpack.
5. Yes, those sound good (name, category, weight, weapon numbers,
   armor/resists, food/water, a written description). NO PAUSE in any
   character sheet tab.
6. Nothing (no menu). Add a character-specific log line, like: "Fizban finds
   no use for that item."

