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

DONE 2026-10-02. Rest / Help are `MessageLog::cornerButtons` (each button as
wide as its longer caption, so Rest -> Wake never moves Help). Minimal's Magic
default now sits under the status plate. The Help click allocated in a guarded
frame (the key names come from the OS layout as strings) - it was the old
button's behaviour too; `MoveKeysHelp` now excuses itself as reporting code.
Checked: `uioverlap hud` + `settings` clean, InGameTest PASS, AllocTest
default / `-Minimal` / `-Panels` PASS, an old save carrying `torch 0` loads.

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

DONE 2026-10-02. `lights.cat` in all three projects (dungeon-demo, Test-World,
the world template); `Game/LightProfile.h` pure and in RollTest (18 checks:
the flicker and the fires' wander are the OLD formulas exactly, the rune
breath within 0.002). Every light goes through `DungeonWorld::PushLight`; the
budget cut keeps each light's ORIGIN beside it (an index order + two reserved
scratch lists). Placed fires keep their own reach (the inspector's Brightness)
- a profile's `radius` serves torches and glows. The palette has a Lights
section (Furnishings, both groupings) whose swatches are the lights' colours.
Dev: `lights` (replaced the old count-only command - the count is its first
line), `lights profiles`, `lights reload` (re-reads lights.cat from disk).
Effects' `light` field takes effect on the next load (the effect registry is
built once); lights.cat, fixtures and items are live.
ADDED (Michael, mid-phase): a placed sconce / brazier's settings dialog has a
FLAME COLOR - one colour for its light AND its flame particles (FireEffect::
SetFlameColor builds the flames from it, since a multiplier over the orange
palette cannot make a bright blue), per placement, `color=r,g,b` on the .map
`fixture` record (written only when set), unset = the kind's profile.

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

DONE 2026-10-03. Built as planned, with two corrections learned on the way:
- The first binning projected each sphere's bounding-box corners, and any box
  straddling the eye's plane counted as the whole screen - in 2.5 m squares with
  7.5-15 m reaches that was nearly every light. It now tests each sphere against
  the tile grid's own planes (`gfx::LightTiler`, 33 column + 19 row planes
  through the eye, built once per view), exact for a sphere and still
  conservative. RollTest samples points inside random spheres and demands none
  is missing from its tile (mutation-checked twice), and checks the game's own
  mirrored camera.
- A sphere that CONTAINS the eye really does reach every pixel (it lights the
  haze in front of each), and with these radii most nearby lights do. So the
  tiles mostly save on DISTANT lights. Measured (release-profile, GPU busy ms,
  1600x900, budget 32): crypt1 view 1.51 -> 1.46; 64 test lights near the
  party 6.24 -> 5.97; 64 spread over the level 5.68 -> 4.98. The budget (Max
  Lights) stays the main control; each drawn nearby light costs ~0.14 ms here.
Tiles on vs off render bit-identically (pixel diff 0 over 100k samples). The
shadow cache now keys on a light's stable id (`gfx::PointLight::id`, kind <<
24 | index), since the ranking reorders the list. The camera updates BEFORE the
lights now, so the cull uses this frame's view. Fades: a budget-dropped light
fades out while the 64 ceiling has room; one that only left the view keeps its
fade; a light seen for the first time starts at its final value. Dev:
`lightstress <n> [near]`, `lighttiles on|off`, and `lights` now prints the cull
counts, tile-light pairs, the camera and each light's tile range. Checked:
RollTest (384), AllocTest default / `-Hand` / new `-Lights -Walk` (64 test
lights, 8 turns inside the window) PASS, InGameTest PASS, no D3D12 validation
messages.

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

DONE 2026-10-03. Every launch goes through `DungeonWorld::Launch`, which DRESSES
the spec (DungeonWorld_Flight.cpp): light and trail from the spell's own
spells.cat `light` / `trail` (a modifier spell lends its own over the bolt it
wraps, `Spell::LendLook`), else the cargo kind's items.cat `light` / `trail`,
else the school's `bolt_<school>` / `trail_<school>`, else `bolt_shot` /
`trail_shot` for a monster's plain shot. A queued volley bolt is dressed when
queued (its names are borrowed from a spell) but lights nothing until it
launches. Each flight is a light keyed `LightKind::Bolt | projectile id`; a lit
bolt's end leaves a 0.3 s FLASH (`ProjectileSystem::ForEachFlash`, 16 slots), so
a hit does not switch the corridor off. Changes from the plan, learned on the way:
- `rate` is particles per SQUARE flown, not per second: the trail is shed by
  distance, and a per-second rate would have needed a reference speed.
- No trail within half a square of the eye: a bolt leaves from beside it, and
  the first screenshots showed its first embers as blurred orbs filling a corner.
- A thrown lit torch needs no `trail_torch`: it trails its OWN flame, a
  FireEffect from the torch-flame pool whose particles keep their course as the
  emitter tumbles on. The same pool (8, reserved) burns the floor torches.
- Bolt brightness came down a step after the first look (fire 2.2 -> 1.7): a
  volley beside a wall blew it out.
Added on the way, Michael 2026-10-03 (see the items below): floor torches stay
lit; Put out and Light in the hand menu; magical torches refuse Flame. The
spark pool never grows now (`AddSpark`): full, it recycles the oldest trail
particle, else refuses. `trails.cat` sits in the palette beside Lights (both
groupings), with a type-editor schema; items.cat gained a `trail` row. Dev:
`trails [reload]`. Checked: RollTest 396 (a Trails section), SpellTest 39,
EditorTest (the Trails section in both palette groupings), AllocTest default,
`-Hand`, `-Lights -Walk`, `-Cast`, `-Impact`, `-Throw` and the new `-Throw
-ThrowItem torch_lit` (a lit
torch carried, thrown, landing lit and lifted again, six times in the window)
PASS; seen in the window: each school's bolt lights the corridor its colour and
sheds its trail, a thrown torch lights the far wall and keeps burning where it
lands, Put out / Light / the out-of-mana refusal / Flame's refusal all read.

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
- FLOOR TORCHES (Michael, 2026-10-03): a lit torch on the floor STAYS LIT -
  thrown or set down - burns its charge there, becomes its stub when spent,
  and is a light in its square (its kind's profile, dimmed by its charge, keyed
  by the floor item's id) with its flame drawn. PlaceDrop no longer puts it
  out; a pack still does.
- TORCH COMMANDS (Michael, 2026-10-03): the hand menu offers PUT OUT for any
  lit torch in the hand (renames it to `unlit_as`, keeping its charge) and,
  for a MAGICAL torch (its lit kind has `power_level` > 0), LIGHT. A magical
  torch will not take Flame's fire (the spell says so); its Light costs the
  holder mana, balance.cat `torch_light_mana` per power level, refused with a
  line when they lack it. Both are menu-only one-shots, never a hand's left-
  click default. use.light / use.putout + log lines x5.
- Checks: AllocTest `-Cast` and `-Impact` (a launch inside a guarded frame),
  `-Throw`; SpellTest.py quick.

## Phase 5 - worn items that illuminate

DONE 2026-10-03. Any item on the doll - or in a hand - whose kind names a
`light` and does not burn gives that light, steady at full strength, from its
member's side (`AppendCarriedLights`, `LightKind::Worn`, key member x slots +
slot): in a hand at the torch's height, worn at the chest. Torches keep their
own path (charge, dimming). The weapons and armor schemas gained the `light`
row items already had. The test item is the MOONSTONE AMULET (armor.cat, `wear
= amulet`, a new `jewelry` category), lighting with the new `moonstone`
profile - a cool blue-white breathing slowly, shadowless, 2.2 squares. Its
model is script-built (tools/BuildAmulet.py: a silver wire bezel, a domed
emissive stone, a cord loop through a bail), committed beside the rock.
Fixed on the way: the four lit-torch descriptions still said "set it down and
it goes out", untrue since Phase 4 (x5). AllocTest gained `-Wear <item>`
(member 0 wears it before the window; refuses to run unless the game says it
was worn and a `worn` light shows).
Found by `-Walk` once the debug build was current: ShadowScheduler's
`m_prevPos` was never reserved, so the first frame with more shadowed lights in
view than ever before grew it mid-walk (latent on main too; now reserved to
kShadowSlots). Checked, on a debug build rebuilt for the purpose: AllocTest
default, `-Hand`, `-Lights -Walk`, `-Walk`, `-Cast`, `-Impact`, `-Throw`, `-Throw
-ThrowItem torch_lit`, and `-Wear moonstone_amulet` alone and with `-Sheet`,
`-Items -MeasureItem moonstone_amulet` and `-Walk` - all PASS; SpellTest 39 and
EditorTest PASS; `uioverlap` clean over the amulet's details dialog; RollTest
396; seen in the window (the amulet lights a dark corridor blue-white, the
`lights` readout lists it as `worn`).

CORRECTION to Phase 4's checks: its AllocTest runs launched a STALE debug exe
(the harness runs build\debug; only release had been rebuilt), so those passes
measured nothing new. The reruns above, on a current build, are the real ones -
and the Phase 4 code passed them as it stood.

- items / weapons / armor .cat `light = <profile>`: an item WORN on the doll
  (or held, for a held item) is a light at its member's side, like a torch.
  One test item to see it with (e.g. a glowing amulet), with item + .desc
  lang keys x5.
- Checks: AllocTest `-Sheet` / `-Items` (equipping it inside the window),
  `uioverlap`.

## Phase 6 - light spells: the Sowilo form rune

Michael's answers are in the notes ("Phase 6 answers"). Built in steps, each
committed and checked on its own:

6a. THE RUNE. A fourth tier-2 FORM rune, id `light`, shown as SOWILO
  (`rune.light`): the SpellSymbol, the grammar (TierOf / SymbolMayFollow /
  WellFormedRecipe), its glyph, glow and icon images and its baked tablet, the
  `rune_light` item in all three projects, lang x5, and every table sized by
  the symbol count. Ends with: learnable, memorizable, shown in the Magic panel.
  DONE: SpellSymbol::Light APPENDED after Explode (bit 9 of knownSymbols, so
  old saves read unchanged); the four name tables; ElementColor's form gold;
  RuneBaker's 10th rune (Sowilo as a three-stroke zig-zag) - re-baking left
  every other rune byte-identical; BuildRuneIcons / BuildRuneGlow; `rune_light`
  x3; lang x5. Found on the way: SpellIdList held exactly 32 ids for exactly 32
  spells, so the next spell would never have been LEARNED - now 64, and
  SpellBook::Build warns if the registry outgrows it; and the items `symbol`
  schema row lacked multiple / explode. MERGE NOTE: the tablet textures are
  gitignored (assets/textures), so after this lands on main run `AssetBaker
  runes assets` then `AssetBaker mips assets rune_` there - a missing tablet
  makes EVERY rune fall back to its flat icon.

6b. THE LIGHT FORM. A `LightSpell` form base (beside Bolt / Ward / Sight) and
  four classes, one file pair each (`light_fire` ... `light_earth`). The shared
  part: a `light` EFFECT on the caster (effects.cat; an Effect class) carrying
  the school, lasting the cast's power x `duration` seconds, its light pushed
  every frame at the party (`LightKind::Spell`) from the school's lights.cat
  profile (`spell_fire` ...), dimming over its last tenth like a torch. Effects
  of different schools stack; a recast of the same school replaces it.
  Ingwaz = ONE BIGGER LIGHT (brighter and further, as long); Hagalaz = a
  DAZZLING FLARE (no lasting light; a flash, the monsters near DAZZLED for a few
  seconds, and the school's effect once). ModifiedSpell learns to wrap a light.
  DONE: Spell/LightSpell + Firelight / Tidelight / Skylight / Stonelight (ids
  firelight ...; Stonelight 150 s, the others 60 s at power 8, in proportion
  past it); Effect/LightEffect.h holds the `light` kind (per-school names,
  `scale_power` 8: size = sqrt(power / 8) within 0.7..1.8) and `dazzle`. The
  world side is DungeonWorld_SpellLight.cpp: a light per (member, school) above
  the party a fifth of a square ahead (`LightKind::Spell`), dimming over its
  last tenth; `LightFlare` (a new cast service, at the END of CastServices) a
  0.7 s `spell_flare` flash in a hand-glow slot, a mote cloud, and `dazzle` on
  every monster within 3 walking steps (the light budget's reach map) for
  1.5 + power/4 s (2..8); a dazzled monster skips its turn at the harness
  `freeze` seam. ModifiedSpell: `<id>_bright` (Ingwaz, `grow` 2: cast at twice
  the power, lasting what the plain power buys) and `<id>_flare` (no blast).
  Measured headless: a mummy two squares off sat dazzled 6.2 s where an
  undazzled one walked up; a level-10 Firelight is power 18.2 for 137 s. NOTE
  the console `heal` CLEARS EFFECTS - a test that heals between casts sees one
  light. Found: de/es/it/ru have no strings for the four Sight spells or
  `log.sight_up` (older than this branch; they show as keys). Checked: AllocTest
  `-Light` (new: each member casts a different light from a world frame, the
  flare at a frozen mummy; refuses a PASS unless all four cast again in the
  window and the mummy carries a dazzle), SpellTest 39, RollTest 396.

6c. FIRE. Its profile casts shadows (the brightest). KINDLES: an unlit sconce
  or brazier within a square of the party catches as it passes (a brazier still
  needs `brazier_power`). SCORCHES: every `scorch_rate` seconds a monster in an
  adjacent square takes a small fire burst. With it, the data Michael asked
  for: every skeleton kind `fire 0.75` in monsters.cat, and a new monsters.cat
  `flammable` - ANY fire landing on a flammable monster (a bolt, a lit torch's
  blow, the scorch) sets it burning every time. Mummies are flammable.
  DONE: TickSpellLights (every frame): KindleNear(1, power) each quarter second
  (party square or one beside; a brazier at effects.cat [light]
  `kindle_brazier_power` 14), and per member a scorch every `scorch_every` 1.5 s
  of `scorch_damage` 1.5 x power/8 fire (a Burst: resisted, not soaked) on each
  monster orthogonally beside the party, with a puff. The flare's fire once:
  every dazzled monster scorched, every fire within 3 steps kindled. FLAMMABLE
  lives in MonsterTarget::Wound - the one seam every hit on a monster crosses -
  so a bolt, a torch's blow and the scorch all ignite: a non-tick fire hit on a
  flammable monster not already burning applies `burn` at balance.cat
  `ignite_burn` 2 / `ignite_seconds` 6. All 12 skeleton kinds `fire 0.75`;
  `[mummy] flammable = 1`; a monsters schema row. Measured headless: a doused
  wall torch beside the party relit within a second of Firelight; a mummy
  beside a level-10 caster caught and died in 4 s (double fire damage plus the
  burn); a skeleton lost 1.7 of 16. SpellTest's ward-burst section burst FIRE at
  four skeletons, which now shrug it off - its probes are mummies now.
  Checked: AllocTest `-Light` (its mummy beside the party now, x400 hp, so the
  scorch and the ignite land in the window) PASS, SpellTest 39, PipelineTest.

6d. WATER. CUTS THE HAZE: a NEGATIVE dust puff centred on the party (the
  shader's DustDensity clamps at zero), the light's reach in radius. SOOTHES:
  stamina regenerates faster (`soothe` x) for the party while it lasts.
  QUENCHES: a member set alight stops burning, and none can burn while it lasts.
  DONE: the bubble is a NEGATIVE dust puff (`clear_haze` 2) at the eye, the
  light's reach in radius, taking a free puff slot or the weakest smoke's;
  scene.hlsl's DustDensity now clamps at zero. Soothe scales stamina's regen
  call by 1 + `soothe` (1: twice as fast; the exerting gate still wins), shown
  by `regen` as "stamina x2.00 in a Tidelight". Quench erases every plume
  effect from the party each frame a Tidelight is up (with a line per member
  put out). Its flare: the party quenched and `power` stamina back each.
  Checked headless: a burning Tilo put out within a second of the cast; in the
  window, a corridor at `dust 0.6` cleared round the party. AllocTest `-Light`
  and SpellTest 39 PASS.

6e. AIR. REACHES FURTHEST (its profile: wide and dim). CRACKLES: every
  `crackle_rate` seconds a monster within its reach and the party's line of
  sight takes a small air burst, with a spark. WARNS: while a monster near has
  noticed the party (`PartyNoticed`), its flicker runs faster (a pulse clock
  integrated per frame, so the change never jumps).
  DONE: CrackleNearest every `crackle_every` 2 s per member: the nearest
  living monster within the light's reach in squares (`spell_air` radius x
  the power's scale) and the party's orthogonal line of sight takes
  `crackle_damage` 2 x power/8 of air (a Burst), with a spit of white sparks.
  WARN: m_airPulseClock advances at a rate eased toward `warn_rate` 3 while
  PartyNoticed(), back to 1 otherwise; the Skylight's pulse reads that clock.
  Its flare: a shock at every monster it dazzled. Measured headless: a
  skeleton three squares off in sight went 16 -> 2.1 hp in 5 s from a level-10
  caster (three shocks of 4.6) while one out of sight was untouched - STRONG
  for a passive light; it is two knobs for the balance pass. AllocTest
  `-Light` and SpellTest 39 PASS.

6f. EARTH. SET DOWN: no effect on the caster - a glowing STONE left in the
  square it was cast in, part of that level's saved state (`LevelState`), its
  light pushed from there (`LightKind::Stone`), lasting longest (`duration`).
  MAPS: the squares within its reach are marked seen. SHOWS TRACKS (below).
  DONE: Stonelight overrides LightOn - nothing lands on the caster; the new
  `placeLightStone` cast service (at the END of CastServices) sets a stone in
  the party's square. The world holds a FIXED pool of 8 a level
  (`m_lightStones`: square, power, time left, duration): a stone already in that
  square gives way, past the pool the one nearest its end. Each pushes
  `LightKind::Stone` from `spell_earth` a little above the floor (dimming over
  its last tenth), sheds the odd amber mote while the party is within 8 squares,
  and draws as effects.cat [light] `stone_item` (the rock; resolved ONCE in
  PreloadItemKinds, since a lookup by name builds a string) tinted and glowing
  amber. MAPS: every square within the light's reach in WALKING steps (the light
  budget's reach map, so a wall stops it) is marked seen with the walls round
  it - also the Earth flare's once-effect. SAVED: SaveData::LevelState::stones,
  a `lightstone x z power left duration` line, captured in SnapshotActive and
  restored in ApplyActiveSnapshot, cleared with the pending volleys; a level
  left behind is not simulated, so its stones wait. Dev `lightstones [clear]`
  (with how many squares the level has mapped). `autocast` holds 6 entries now.
  Measured headless: a level-10 stone (power 18.7, 351 s, reach 4.9 squares)
  took crypt1 from 9 squares mapped to 58; it survived save -> reset -> load and
  a trip to crypt2 and back. Seen in the window: a small amber stone in its own
  pool of light, motes rising, the room mapped. Checked: AllocTest `-Light` (a
  Stonelight now in the rotation, 6 casts in the window), default, `-Cast`,
  `-Walk`, `-Light -Walk`; SpellTest 39, PipelineTest, RollTest 396; release
  builds. NOT YET: a stone has no map marker.

6g. MONSTER TRACKS. Every monster step records its square and direction and
  the world time on the level (a per-level grid, saved in `LevelState` as ages,
  so a load restores them), fading over `track_life` seconds. An Earth stone
  shows the tracks within its reach as faint amber motes low on the floor.
  FOR LATER (Michael): the PARTY leaving tracks, scent and noise that some
  monsters can follow - the grid is built so the party can write to it too.
  DONE: `m_tracks`, one cell per square (sized with the fog mask by
  FitTracksToMap at every one of its five sites, so a step never allocates):
  the track clock's time (`m_trackClock`, simulated seconds), the way it went
  and its MAKER (Monster now; Party is there for later). Written in
  StepMonsterTo, the one place a monster's step commits, onto the square it
  steps to; a slot shuffle in its own square leaves none. Fades over balance.cat
  `track_life` 300 s. SAVED per level as ages: one `tracks x,z,dir,maker,age
  ...` line (`seen`'s shape), captured in SnapshotActive, restored against the
  clock in ApplyActiveSnapshot, cleared for a new game. SHOWN by a stone every
  0.3 s while the party is within 8 squares: for each track in its reach (now a
  walk in a fixed 17x17 window round the STONE - StoneReachCells - which also
  does the mapping, replacing the party-centred reach map), a faint amber
  ProjectileSystem::Mote at a footprint - along the line it walked, a little to
  one side - drifting the way it went, fewer and dimmer as it ages. A mote is
  TRAIL-flagged, the first thing the pool gives up. The Earth flare shows every
  track in its reach at once. Dev `tracks [clear] | add <x> <z> <n|e|s|w>`.
  Measured headless: a skeleton walking to the party left 4 tracks going east;
  they came back a second older after save -> reset -> load, and unchanged
  from crypt2. Seen in the window: a double row of amber footprints from the
  party to the skeleton that made them. Checked: AllocTest `-Light` plants three
  tracks in the stone's reach and refuses a PASS without track motes in the pool
  (mutation-checked: ShowTracks cut -> "0 motes", UNMEASURED); default, -Walk,
  -Cast, -Impact (one run measured only 304 frames and exited without its
  closing line; two reruns PASS); SpellTest 39, PipelineTest, RollTest 396,
  EditorTest; release builds.

- Content: spells.cat (4 lights + their Ingwaz and Hagalaz forms), effects.cat
  `light`, lights.cat `spell_<school>`, `spell.<id>` + `.desc` and the rune's
  keys x5, AllSpells.cpp + CMakeLists.
- Checks: SpellTest.py (each light cast, its duration, its end; the flare's
  dazzle), AllocTest `-Cast` with a light spell and `-Walk` under a light,
  RollTest for anything pure, a save/load round trip of a stone and tracks,
  each behaviour seen in the window - all on a CURRENT debug build.

## Docs to update as phases land

CLAUDE.md (the FIRE AND LIGHT bullet, the HUD description, Renderer features
for the tiled lists), docs/torches-and-fire.md, docs/magic system.md.
