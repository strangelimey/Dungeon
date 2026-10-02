# Lighting updates - notes

Michael's brain dump for the `lighting-updates` branch, captured as it came
in (his words, in order). Organizing and planning come after he says he is done.

## Raw notes

- Let's work on the lighting.
- Completely remove the "Options" panel that shows torchlight, etc.
- Light comes from these places: level ambience and turbidity set in the
  lever dlg [level dialog?]. A lit wall sconce or brazier (and other dungeon
  light sources later), any carried torch, any active light spell.
- The color of the light is set by its type. A sconce or brazier is
  orange-red, a torch is the same but it gets dimmer as it runs out. Then
  there are various light spells, each with their own color and pulse, etc.
  Some worn items might illuminate.
- A spell in flight has a color, set by its school (but it might be
  overridden by any spell).
- So does a magic arrow.
- QUESTION: This might be a lot of lights. How do we manage them without
  overloading the GPU?

## Organized

### A. Remove the Options panel
The HUD's left-column "Options" plate goes entirely: the Torchlight dropdown
(warm / cold / eerie), and with it the palette behind it
(`DungeonWorld::SetTorchPalette`, `m_torchColor`, the `torch` save line, the
`torch.*` / `log.torch_*` / `hud.torchlight` lang keys, the `options` floating
panel in kHudPanelFields and its Settings -> UI scale/opacity pair).
GAP: the plate also holds the REST and HELP buttons. They need a new home (Q1).

### B. Where light comes from (the whole list)
1. The LEVEL: ambient + turbidity (dust / haze / ambient scale), already
   authored in the Level settings dialog (`atmosphere` record). No change asked.
2. DUNGEON LIGHT SOURCES: lit sconces and braziers today; more kinds later.
3. CARRIED TORCHES: any lit torch in a hand (or on the cursor) - exists.
4. ACTIVE LIGHT SPELLS: none exist yet (Q2).
5. WORN ITEMS that illuminate: new.
6. THINGS IN FLIGHT: a spell bolt; a magic arrow (no bows/arrows exist yet).
Not on his list but lit today, assumed KEPT: a floor rune / enchanted weapon's
glow, a burning monster's glow, Ember Sight's fill light (Q3).

### C. Colour (and behaviour) belong to the light's TYPE
- Sconce and brazier: orange-red.
- Torch: the same colour, dimming as it runs out (the dimming exists:
  `TorchBrightness`, last tenth of `burn_time`).
- Light spells: each its own colour and PULSE.
- Worn items: their own.
- A spell in flight: its SCHOOL'S colour by default, overridable per spell.
- A magic arrow: likewise its own colour.
So no global palette: every light is described by its source's catalog entry.

### D. Budget (his question)
Many small lights (volleys, glowing gear, several spells) on top of the fires.
The cost today: the scene shader loops over EVERY uploaded light for every
pixel, and again at each of the 12 dust-march steps, so a light costs ~13
evaluations a pixel wherever it reaches. Today's only control is the
nearest-to-eye budget cut (Settings -> Video -> Max Lights, 16..64) plus 8
shadow cubes. Answered in the plan.

## Answers (2026-10-02)

- Q1 Rest + Help: small buttons in the bottom-left corner BESIDE the Log button.
- Q2 Light spells: a new tier-2 FORM rune, SOWILO (the Futhark sun rune):
  school + Sowilo = a light. "But we might change more than the light color
  for each school" - so each school's light is its own spell class with room
  for its own behaviour, not one spell recoloured.
- Q3 The unlisted glows (floor rune / enchanted weapon, burning monster, Ember
  Sight's fill): KEEP all three, moved onto the same per-type light data.
