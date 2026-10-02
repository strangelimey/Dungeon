# Lighting updates - plan

Notes, organized themes and answers: docs/lighting-updates-notes.md. Six
phases, each shippable and checked on its own. Phase order puts the data
model and the budget BEFORE any new light source, so every new source lands on
a system that already copes with many lights.

## What exists (the starting point)

- `DungeonWorld::UpdateLights` rebuilds `m_lights.points` every frame from:
  carried torches (`AppendCarriedLights`, DungeonWorld_Light.cpp), lit fires,
  burning monsters, floor runes / enchanted weapons, and Ember Sight's fill.
- Colour: torches AND fires share `m_torchColor`, set by the HUD Torchlight
  palette (warm / cold / eerie). Radius and intensity are constants in code
  (a brazier flag picks between two sets). Torch dimming exists
  (`TorchBrightness`).
- Budget: `maxPointLights` (Settings -> Video, 16..64) keeps the lights
  NEAREST THE EYE; `ShadowScheduler` gives 8 cubes to the nearest shadowed ones.
- Cost: scene.hlsl loops over every uploaded light per pixel, and again at each
  of the 12 dust-march steps. No view culling of lights, no tiling.
- Nothing in flight makes light today.

## Phase 1 - the Options panel goes; Rest and Help move beside Log

- Delete the `options` floating panel (GameUI.cpp BuildHud), its
  kHudPanelFields row, its Settings -> UI scale/opacity pair and tray glyph.
  An old ini's `hud_options_*` lines are ignored.
- Rest and Help become small buttons in the bottom-left corner beside the Log
  button, in every state the Log button shows. Rest keeps its action-label
  rule (Rest / Wake, re-labelled each frame from the world).
- Delete the palette: `SetTorchPalette`, `m_torchPalette` (world and GameUI),
  `onTorchPalette`, `SaveData::torchPalette`. The `torch` save line is no
  longer written and is skipped on load (no version bump). Lang keys
  `hud.options`, `hud.torchlight`, `torch.*`, `log.torch_warm/cold/eerie` x5 go.
- `m_torchColor` survives this phase as a constant (warm), so the scene looks
  unchanged until Phase 2 replaces it.
- Checks: `uioverlap hud`, InGameTest (default + minimal), AllocTest default.

## Phase 2 - every light is described by its TYPE (data, not code)

- New catalog `lights.cat` (each project + the world template): named light
  PROFILES. Fields: `color`, `intensity`, `radius`, `pulse` (steady / flicker /
  breathe / strobe / storm), `pulse_rate`, `pulse_depth`, `wander` (how far the
  origin dances - the fires' moving shadows), `shadow` (0/1), `long_fade`
  (the brazier shadow ramp). Authored to reproduce today's look exactly:
  `fire_sconce`, `fire_brazier`, `torch`, `rune_<school>`, `burning`,
  `ember_sight`.
- Sources NAME a profile: fixtures.cat `light = fire_sconce`; items.cat
  `light = torch` on the lit torch; effects.cat `light = burning` on the
  plume effects; the rune / enchanted-weapon floor glow by school. A source may
  override `light_color` (etc.) on itself without a new profile.
- `Game/LightProfile.h` - a PURE TU (in RollTest): parse + evaluate a profile
  at a time and phase into colour / intensity / radius / origin offset. Every
  source goes through it; UpdateLights stops holding light constants.
- Torch: the profile's colour, its brightness times `TorchBrightness`.
- Editor: lights.cat gets a CatalogSchema table (one dialog, the type editor);
  `light` on fixtures/items/effects is a CatalogRef dropdown.
- Dev: `lights` prints this frame's lights - source, profile, colour,
  intensity, radius, distance, shadow slot, kept or culled.
- Check: before/after screenshots of the same squares must match (it is a
  refactor of the look); AllocTest default + `-Hand` (torch lighting).

## Phase 3 - the budget: many lights without overloading the GPU

The answer to the open question, in four parts:

1. CULL WHAT CANNOT BE SEEN. A light whose sphere misses the view frustum
   lights nothing on screen (`ViewCull` already exists for chunks). A light
   whose square the party cannot reach on the grid within its radius (a room
   sealed off by rock) is dropped too - which also stops shadowless fills
   bleeding through walls into view.
2. RANK BY WHAT IT ADDS, NOT BY DISTANCE. Score = the profile's intensity
   through its own falloff at the eye's distance, so a big brazier down the
   hall outranks a dim spark beside you. Carried and party lights are always
   kept. The Max Lights setting keeps its meaning: how many survive.
3. NO POPPING. Each light carries a stable key (its source id); a light
   leaving or entering the budget fades over ~0.25 s instead of switching.
4. TILED LIGHT LISTS IN THE SHADER. The CPU bins the surviving lights into
   screen tiles (a 64-bit mask per tile - one bit per light slot, so the 64-
   light ceiling fits exactly) and uploads it with the frame. A pixel loops
   only its tile's lights. The dust march reuses the SAME mask: every sample
   on the eye-to-surface ray projects to that same pixel, so the tile's list
   is exact for the haze too. That is where most of the per-light cost goes
   today (12 of the 13 evaluations).

- Small transient lights (bolts, sparks) are shadowless by profile, so the 8
  cubes stay with the torch and fires.
- Dev: `lightstress <n>` scatters n test lights; `lights` shows cull reasons
  and tile occupancy.
- Checks: ProfileTest before/after with `lightstress 64` in a fire-dense room
  (the number that proves the tiling earned itself), AllocTest default (the
  per-frame binning allocates nothing), screenshots match at low light counts.

## Phase 4 - lights in flight

- A bolt lights in its SCHOOL'S colour by default (profiles `bolt_fire`,
  `bolt_water`, `bolt_air`, `bolt_earth`); spells.cat `light = <profile>`
  overrides it per spell. A volley is ONE LIGHT PER BOLT (Michael,
  2026-10-02), so each bolt lights its own stretch of corridor as it flies.
  What keeps that affordable is Phase 3, not merging: bolt lights are
  shadowless and small-radius, the tiles keep each one's cost to the pixels
  it reaches, and the ranking drops the dimmest first if a big fight ever
  outruns Max Lights. The stress check casts volleys in a fire-dense room.
  Each bolt is its OWN light source because a volley's timing varies by spell
  (the pending-bolt queue, `spawnBoltAfter`, staggers launches): the light is
  keyed to that projectile's runtime id (Phase 3's stable key), switches on
  when that bolt launches - a bolt still waiting in the queue makes none -
  and goes out when that bolt lands or dies, independent of its siblings.
  Nothing assumes the bolts of one cast fly together.
- TRAILS (Michael, 2026-10-02): anything in flight can shed particles -
  sparks, magic embers, motes, drips - through the projectile system's
  existing SPARK pool (Projectiles.h `Spark`: pos/vel/colour/life/size/`fall`/
  `swell`, already drawn as additive billboards by the particle batch for
  impact bursts). New catalog `trails.cat` of named TRAIL profiles: `shape`
  (spark / ember / mote / puff / drip), `rate` (per second, emitted by
  distance flown so a fast bolt does not thin out), `life`, `size`, `spread`,
  `fall` (embers rise, drips and grit fall), `swell`, `color` (default: the
  source's light colour, so trail and light always agree). Defaults by school
  (`trail_fire` rising embers, `trail_water` falling droplets, `trail_air`
  swirling motes, `trail_earth` falling grit); spells.cat `trail = <id>`
  overrides per spell, and an item kind's `trail` covers thrown items and,
  later, magic arrows. A thrown LIT torch trails its own flame (the fixture
  fire's particle look). Each bolt in a volley trails independently, like its
  light.
  Cost rules: the pool is RESERVED and never grows in a frame (the
  allocation rule) - when it is full the oldest trail particle is recycled
  before an impact spark is refused, since a hit must always read; a trail's
  rate falls off with distance from the eye; a monster's shot trails too.
  Dev: `trails` (live count, pool use, recycled this second).
- A thrown item lights if its kind has a `light` (a thrown lit torch keeps
  lighting the corridor it flies down, and lands lit).
- MAGIC ARROWS: no bow or arrow exists yet, so this phase builds the hook only -
  any projectile whose cargo kind names a `light` glows in flight. An arrow
  authored later needs only a catalog line.
- Checks: AllocTest `-Cast` and `-Impact` (a launch inside a guarded frame),
  `-Throw`; SpellTest.py quick.

## Phase 5 - worn items that illuminate

- items / weapons / armor .cat `light = <profile>`: an item WORN on the doll
  (or held, for a held item) is a light at its member's side, like a torch.
  One test item to see it with (e.g. a glowing amulet), with item + .desc
  lang keys x5.
- Checks: AllocTest `-Sheet` / `-Items` (equipping it inside the window),
  `uioverlap`.

## Phase 6 - light spells: the Sowilo form rune

- A fourth tier-2 FORM rune, `light` id, shown as SOWILO (`rune.light` ->
  Sowilo). `Spells.h` grammar: TierOf / SymbolMayFollow / WellFormedRecipe
  learn it. The rune tablet + glyph (RuneBaker, BuildEtchGlyphs.py).
- A `LightSpell` form base (beside BoltSpell / WardSpell / SightSpell) and
  FOUR classes, one per school, each its own file pair - because Michael wants
  the schools to differ in more than colour. The shared part: an effect on the
  caster (a `light` effect kind) carrying a profile, lasting the cast's power
  in seconds, following the party, dimming at the end like a torch. What each
  school ADDS beyond its colour and pulse is decided with Michael when the
  phase opens (one question at a time) - not guessed here.
- Modifiers: whether Ingwaz (party-wide, as for wards) / Hagalaz apply to a
  light is part of that same conversation.
- Content: spells.cat entries, effects.cat entries, `spell.<id>` + `.desc` and
  `rune.light` lang keys x5, AllSpells.cpp + CMakeLists.
- Checks: SpellTest.py (a light cast, its duration, its end), AllocTest
  `-Cast` with a light spell, the spells eval suite.

## Docs to update as phases land

CLAUDE.md (the FIRE AND LIGHT bullet, the HUD description, Renderer features
for the tiled lists), docs/torches-and-fire.md, docs/magic system.md.
