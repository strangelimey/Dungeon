# ui-panels - Michael's notes (brain dump, 2026-09-30)

Raw notes, his words in substance. Organize and plan only when he says so.

## Before the branch opened (same conversation)

- Reference: Legend of Grimrock 2 UI screenshot
  (https://www.rpgfan.com/wp-content/uploads/2020/10/Legend-of-Grimrock-II-Screenshot-001.jpg).
- "I like the polished stone background to the UI elements. We have a bunch of
  textures. Have a look through them and see what we can find to improve the
  current UI look."
  - Survey done: 27 continuous-surface stones out of the installed sets + the
    OneDrive archive (contact sheet + six mock panels shown). Calm candidates:
    armani-marble, granite-gray-white, gray-granite-flecks, blackrock. Busy:
    cloudy-veined-quartz, gray-polished-granite.
- "Also, I like how each bar/panel is a floating panel that can be independently
  moved and resized."
- "Can we have a settings dialog section where we pick from a filtered list of
  stone textures or do we need to decide and re-bake the elements?"
  - Answered: yes, by drawing the look in layers (stone fill tiled + toned at
    runtime, a stone-independent bevel overlay, a sheen overlay); a curated
    assets/ui/stones/ folder is the filtered list.
- Carried in from main, uncommitted: Settings -> UI scale + background opacity
  for the Movement / Hands / Magic docks (built and checked, see CLAUDE.md
  Quality system). Open: dragging to resize may replace the scale half.

## Brain dump

- "I like the way the Grimrock UI combines the party bar with the hand
  controls. We need to add a UI setting called 'minimal' that does this."
- "the stone should also apply to the buttons and slots"

## Organized (2026-09-30, after "continue")

Three themes, which the plan takes in this order because each later one draws
on the earlier:

A. STONE CHROME (the look)
   1. Polished stone behind the UI panels, Grimrock-style.
   2. The same stone on BUTTONS and SLOTS - not just panels. Today buttons are
      the wooden kit face (skin_button.png), HUD hand boxes use the kit ring
      (skin_slot.png), and the sheet / inventory / bag / party-bar item slots are
      flat kSlotBg rects with no skin at all.
   3. The stone is PICKED in Settings from a filtered list, switchable live,
      rather than chosen once and baked in.

B. FLOATING HUD PANELS (the layout)
   4. Each bar / panel is its own floating panel, moved and resized
      independently.
   5. (Carried) per-dock scale + opacity sliders. Resizing by drag covers what
      the scale slider does; opacity has no drag equivalent.

C. "MINIMAL" (a second layout)
   6. A UI setting named "minimal" that combines the party bar with the hand
      controls, as Grimrock does: one card per member (portrait + bars over the
      two hands).

Gaps and open questions (for Michael - none of these was said either way):
- Q1 Which HUD panels float? Candidates: party bar, status plate (compass),
  options plate (torch / Rest / Help), Movement, Hands, Magic, the message log.
  And the character sheet / party inventory (Grimrock's inventory is a floating
  window) - in or out?
- Q2 Resizing: the docks' contents are aspect-locked (square cells and hand
  boxes), so a resize is naturally a UNIFORM scale by a corner grip. Keep the
  scale sliders as well, or drop them for the drag? (Opacity stays either way.)
- Q3 Minimal: what else goes? Grimrock has no movement pad and hangs the rune
  panel off the caster's card. Does minimal keep Movement and Magic as they are
  and only fold party bar + hands into cards? Cards as one 2x2 floating block
  (formation order: Brand front-L, Sera front-R, Maren rear-L, Tilo rear-R) or
  four separately floating cards?
- Q4 Where does the stone reach? The skin is shared by every UI context, so by
  default the menus, settings and pause pages and the sheet go stone too. The
  editor's dialogs as well, or do they keep today's quiet look?
- Q5 Guarding against an accidental drag: a "Lock HUD layout" toggle, and a
  "Reset HUD layout" button?

Answers (Michael, 2026-09-30):
- Q1: all the HUD panels except the message log, AND the character sheet /
  party inventory become floating windows too.
- Q2: BOTH - drag to resize, and keep the scale sliders as a precise
  alternative (per panel), alongside the opacity sliders.
- Q3: the four cards form ONE floating 2x2 block in formation order; Movement
  and Magic stay their own floating panels.
- Q4: stone on the game screens only (HUD, menus, settings, pause, saves,
  sheet, item dialog); editor dialogs keep their current look.
- Q5: not asked; the plan assumes Lock + Reset.
