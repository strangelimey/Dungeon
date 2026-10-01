# icon-updates - notes

Michael's brain dump, captured as it comes in. Organize and plan come after.

- Source kit: C:\Users\stran\OneDrive\DungeonAssets\ui\medieval-rpg-ui-kit\extracted\upscayl_png_upscayl-standard-4x_4x
  (181 PNGs, 4x upscaled: Action Buttons, Inventory Slots, Life / Mana / XP Status Bars,
  Menu Buttons, Minimap Borders, Portrait Frames, System Buttons - ~20 each)
- Several images show red, blue and green bars. Wants something like this for the
  health, stamina and mana bars on the party bar.
- Need to split them up so the progress % can be shown by the bar "filling up".
- So we need the containing CHROME and the textured CENTRE (fluid, energy, whatever)
  as separate pieces.
- Q: do we cut up the images, or can Claude generate suitable ones from the examples?
  (Answer given: Claude can't paint art like this. Cut the frame from the kit; the fill can be
  cut too, or made procedurally - possibly animated in a shader.)
- DECIDED: ONE frame for all three bars, with PROCEDURAL ANIMATED fills. Likes the animating idea.
  - Mana: a lightning-like pulse. BLUE, like the kit's mana bars (the blue wisps in the silver set).
  - Health: a heart beat - faster in combat, much slower near death. Stays RED, like the blood.
  - Stamina: a glow. GREEN, glowing like the XP bars (the green energy in the gold-framed set).
  - All three EMISSIVE, and the brightness diminishes as the stat diminishes.
- The frame comes from the IRON (red / "Life Status Bars") set.
  - PICKED: #1 ("Life Status Bars (1).png") - rounded glass tube, scrollwork at both ends
    plus small ones above/below the ends. (Palette: scratchpad iron_frames.png.)
  - Also likes how #1's CONTENTS EBB AND FLOW LIKE BLOOD (the wavy, bubbly surface of the fluid).

Observed while looking (facts, not decisions):
- Bars are 4096x1024; buttons / slots / frames / borders are 3072x3072.
- The PNGs are RGBA but fully OPAQUE - the bars sit on solid black, slots on dark grey.
  No transparency to separate chrome from fill; it has to be cut.
- "Life" = red fluid in dark iron (21 variants), "Mana" = blue wisps in silver (20),
  "XP" = green glow in gold (20). Three different frame styles, one per colour.

## Organized (2026-09-30)

A. THE FRAME - one shared iron frame, cut from kit bar #1.
   - Cut the chrome out: interior and black field made transparent.
   - The same frame serves health, stamina and mana.

B. THE FILLS - procedural, animated, emissive; the bar FILLS UP to the stat's %.
   | Bar     | Colour                      | Motion                                         |
   |---------|-----------------------------|------------------------------------------------|
   | Health  | red, blood                  | heartbeat: faster in combat, much slower near death |
   | Stamina | green, glowing like XP bars | glow                                           |
   | Mana    | blue, like the mana bars    | lightning-like pulse                           |
   - All three: brightness falls as the stat falls (on top of the fill length).
   - Liked: kit #1's blood ebbing and flowing (wavy, bubbly surface).

C. WHERE - the party bar's three resource bars.

Gaps / questions to settle before planning:
1. Ebb-and-flow: health only, or the surface motion for all three (each in its own character)?
2. Colours are USER-SETTABLE today (Settings > UI > Resource Bars, kBarFields). Keep the
   pickers as a tint over the procedural fill, or retire them?
3. The character sheet's Stats tab draws five bars (+ food, water). Same frame there?
   What do food/water fill with?
4. Heartbeat states: "in combat" = any monster in aggro (the existing stabilize signal)?
   At 0 HP (unconscious) and dead - does the beat stop, flatline?
5. Fit: kit #1's end caps are wide; on the party bar does the frame's track shrink to make
   room, or does the frame overrun the current bar's width?
6. The branch is icon-updates: anything wanted from the other kit categories (action / menu /
   system buttons, inventory slots, portrait frames, minimap borders)?
7. ui-panels (unmerged) is putting STONE chrome on panels, buttons and item slots. Iron bars
   inside stone panels - intended?
8. Costs ledger: where did the kit come from and what did it cost?

First look in game (Michael, 2026-09-30):
- "Make the party bar tubes taller" -> party bar height 0.107 -> 0.140.
- The bar CHROME OVERLAPS THE CHARACTER CONTAINER (the member's slot) - keep it inside.
- The animations are TOO BUSY - they draw the eye. SLOW THEM DOWN and SUBDUE the effect a little.
- Vertical bars in the fill (the noise's float hash disagreeing across cells) - fixed
  with an integer hash before he finished saying it.

Answers (Michael, 2026-09-30):
1. EACH bar gets its OWN surface motion.
2. REMOVE the resource-bar colour settings.
3. YES, the same frame on the sheet's bars too. Food/water: a SOLID colour for now - will
   need changing later.
4. YES, "in combat" = noticed (any monster in aggro). NO BEAT when out (unconscious).
5. The frame STICKS OUT past the current bar's width.
6. NO - nothing else from the kit on this branch.
7. YES, iron bars inside stone panels - but that might need to change.
8. Already downloaded; if not in the ledger it was free. -> It IS in the ledger: docs/costs.md
   itch.io row 2026-07-12, "UI Medieval RPG" by Vill8tion, $2.70. Already the source of
   skin_button (slot #17) and skin_slot (slot #12); the earlier cut was a scripted bbox-detect +
   crop + centre-punch (commit d12bc9e). Update that row's status when the bar frame lands.

