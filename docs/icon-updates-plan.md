# icon-updates - plan

STATUS (2026-09-30): built, judged in game, merged. Changes after the first look are
in the notes ("First look in game"): taller party bar, frames kept inside the member's
slot, calmer fills (kPace / kSubdue), and an integer noise hash. "Noticed" became a
live, aware, non-Idle monster instead of the stabilize clock's distance check.

Source: docs/icon-updates-notes.md (Michael's notes, the organized list, his answers).
Goal: the resource bars become kit bar #1's iron frame around a procedural, animated,
emissive fill - health, stamina and mana on the party bar AND the sheet; food and water
on the sheet get the same frame with a solid fill for now.

## What exists today

- `DrawStatBar` (Game/PartyHudDraw.cpp) is the whole bar: a theme-coloured rect, a
  coloured rect `fraction` wide, a 1px border. Callers: `StatsArea::DrawSelf` (party bar,
  3 bars) and `CharacterSheet::DrawStats` (sheet, 5 bars, value text centred on top).
  `CharacterSheet_Lists.cpp` also uses it for the skill XP bars - OUT of scope, stays flat.
- Colours come from `ResourceBarColors` (PartyHudTypes.h) via `GameSettings::barColors`,
  driven by `kBarFields` (ini `bar_<name>`, the Settings > UI "Resource Bars" picker grid).
- The 2D pass is `gfx::SpriteBatch` + `sprite.hlsl`: one PSO, straight alpha blend,
  vertex = position / uv / colour, flush on texture change. No time, no per-quad params.
- "Noticed" already exists as `danger` in `DungeonWorld::UpdateMonsters` (any live
  monster within its own aggro range of the party) - computed and passed to TickParty,
  not stored.
- The kit was cut before (skin_button / skin_slot, commit d12bc9e) by a scratchpad
  script that was never committed.

Kit bar #1, measured (4096x1024 source): the bar is 3662x612 px; the glass tube inside
it is 3050x280 - 83% of the frame's width, 46% of its height. End caps ~306 px each;
scrollwork reaches 184 px above the tube and 148 below (at the ends only).

## Phase 1 - cut the frame (tools/CutBarFrame.py)

A COMMITTED script, per the script-is-the-asset rule (the last kit cut lived in a
scratchpad and is gone). Reads the OneDrive original, writes `assets/ui/bar_frame.png`.
- Background: key the black field to transparent by FLOOD FILL FROM THE IMAGE EDGE, not
  by a global threshold - the frame is dark iron, and a threshold would eat it.
- Interior: find the tube's inner rim and make everything inside it transparent. The
  tube is a CAPSULE (rounded ends), so the hole follows the rim, not a rectangle. The
  fill draws as a plain rect UNDER the frame; the opaque caps hide its corners.
- Crop to the bar, downscale to ~1024 wide (a bar is never more than a few hundred px
  on screen; mips via the usual bake).
- Print the TUBE INSETS (left / right / top / bottom as fractions of the frame). Code
  holds them as named constants beside the load, citing the script; the script asserts
  them so a re-cut that moves the tube fails loudly instead of misaligning the fill.
- Glass highlights painted over the old fill are lost with it - the shader puts a
  highlight back (Phase 3).
Check: render the cut over a mid-grey and a checker and look at it before going on.

## Phase 2 - the renderer path (SpriteBatch fill mode)

The fills need time, a kind, the fill fraction and a heartbeat phase per bar - things
the sprite vertex cannot carry. Add ONE new draw to SpriteBatch:

    void DrawBarFill(const Rect& tube, const BarFill& fill);
    struct BarFill { BarKind kind; float fraction; float phase; float intensity; Vec4 tint; };

- A second PSO from a new `bar.hlsl` (same root signature plus a time constant), with
  its own vertex carrying `params` (kind, fraction, phase, intensity). Switching between
  sprites and fills FLUSHES, exactly as a texture change does today, so draw order is
  still submission order and the frame drawn after its fill lands on top.
- PREMULTIPLIED blend in that PSO, so one shader does both the body of the fluid
  (alpha 1) and an additive glow (alpha 0) - "emissive" in a 2D pass that runs after
  bloom/tonemap.
- `time` = wall-clock seconds from Game, wrapped so float precision never degrades.
- No allocation: the fill vertices go in a second reserved pending list, like the
  first (8k reserved, flushed per draw).
- `bar.hlsl` compiles at launch through the shader cache like the others, so the look
  iterates with edit + relaunch, no rebuild.

## Phase 3 - the fills (bar.hlsl)

Each kind has its own surface motion (answer 1). All three:
- fill to `fraction` with a soft bright MENISCUS at the leading edge, not a hard cut;
  the empty part of the tube is dark glass;
- EMISSIVE: brightness = lerp(floor, 1, fraction) - a full bar blazes, a low one
  smoulders (floor ~0.35, tunable);
- a glass highlight streak along the top of the tube, over the fill.

| kind    | colour                     | motion |
|---------|----------------------------|--------|
| health  | blood red                  | the #1 look: a fluid that EBBS AND FLOWS - slow layered noise, a sloshing surface line, rising bubbles - and a HEARTBEAT: a lub-dub swell of brightness that travels along the tube on each beat |
| stamina | green, the XP-bar glow     | a GLOW: slow breathing luminance, soft drifting motes, a heat-shimmer ripple |
| mana    | blue, the mana-bar wisps   | a LIGHTNING-like PULSE: bright filaments arc along the tube at intervals with flicker between, blue wisps drifting underneath |
| solid   | food / water flat colours  | none - flat fill, framed (answer 3, to be replaced later) |

## Phase 4 - the heartbeat and the state behind it

- `DungeonWorld::PartyNoticed()` - the `danger` already computed in UpdateMonsters,
  kept in a member instead of thrown away. False while travelling (no level).
- A per-member `BarPulse` (heart phase + current rate), owned by GameUI and ticked once
  per frame with real dt. The PHASE is integrated on the CPU, never `time * rate` in the
  shader, so a rate change speeds the beat up or slows it down instead of jumping it.
  The party bar and the sheet read the same pulse, so both beat in step.
- Rate (bpm), all named constants for tuning:
  - resting ~60; NOTICED ~120;
  - NEAR DEATH (health under ~30%) slides down toward ~35 - near death WINS over
    noticed (decided);
  - eased toward the target over ~1.5 s, so the change is felt, not stepped;
  - DOWN (unconscious or dead): no beat (answer 4). The fill is empty at 0 HP anyway,
    so the bar simply goes dark.
- Not saved - presentation state, re-derived on load.

## Phase 5 - wire it in, remove the settings

- `ResourceBarColors` becomes `ResourceBarStyle` (PartyHudTypes.h): the frame texture,
  the tube insets, food/water solid colours, a pointer to the pulses. No user colours.
- `DrawStatBar` grows a framed sibling, `DrawResourceBar(batch, tube, kind, fraction,
  pulse, style)`: fill, then the frame placed around the tube. The FRAME STICKS OUT past
  the bar's rect (answer 5): the bar rect is the TUBE; caps run past both ends and the
  scrollwork above and below. Horizontally the frame is drawn 3-slice (caps at their
  aspect, the rim stretched) so it fits any bar width without squashing the caps.
- Party bar (`StatsArea`) and sheet (`DrawStats`, all five bars) switch to it. The
  sheet's centred value text stays on top, with a shadow so it reads over a moving fill.
- uiskin=0 (the flat debug look) keeps today's `DrawStatBar`, with fixed colours.
- REMOVE: `kBarFields`, `GameSettings::barColors`, its ini load/save, the Settings > UI
  "Resource Bars" grid, and the `bar.*` lang keys if nothing else uses them. An old
  settings.ini's `bar_*` lines are ignored on load.
- CLAUDE.md: every kBarFields / barColors / Resource Bars mention updated.
- docs/costs.md: the kit's itch.io row gains "bar_frame (bar #1)".

## Phase 6 - dev console + checks

- `hudbars [status]` - each member's bpm, noticed, fractions; `hudbars demo on|off`
  sweeps every bar 0..1..0 so dimming and the meniscus can be judged without a fight;
  `hudbars rate <bpm|auto>` pins the heartbeat for judging.
- `/check-alloc` - the party bar draws every guarded frame, so the new path must
  allocate nothing; plus `AllocTest.ps1 -Sheet` for the sheet's five.
- `/check-build` - release too.
- `/check-ingame` - the uioverlap sweep (the bars are direct draws, so the sweep proves
  the widgets around them did not move; the frame sticking out is intended).
- Then hand the game to Michael to JUDGE the look - colour, speed, brightness are his
  call, and the shader iterates with a relaunch.

## Decisions (Michael, 2026-09-30)

1. Near death IN combat: NEAR DEATH WINS - the heart labours slowly even mid-fight.
2. Stacking: SPACE THE BARS A LITTLE FARTHER APART (the full frame, scrollwork kept).
   StatsArea's bar gap grows from 0.25 rem; the tubes get thinner to pay for it, and
   the scrollwork may still touch its neighbour's - judge on screen.
3. Merge risk, not a question: ui-panels (unmerged) also edits the Settings > UI page
   (its stone picker), where this removes the Resource Bars grid. Whichever lands second
   resolves a small conflict in GameUI.cpp.
