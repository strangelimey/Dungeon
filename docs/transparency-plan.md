# transparency - plan

From docs/transparency-notes.md and Michael's answers (2026-10-02):
- Looks AND drinking are in this branch.
- The liquid is GENERATED from the bottle's own shape, never authored per model.
- A potion is drunk all at once and leaves its empty container behind.
- Potions: health, stamina, mana, antidote. Plus the fire and poison BOMBS
  (thrown, not drunk).
- Three strength tiers, three containers: vial (minor), small bottle
  (standard), flask (greater). The bombs come in three sizes too.
- Containers are SCRIPT-BUILT first; bought models may replace them later.

Six phases, each checked on its own. Phase 1 is the renderer and stands
alone; everything after it is content riding on it.

Overlap with `lighting-updates` (in progress): its Phase 3 rewrites the light
loop in scene.hlsl and the frame constants in Renderer. This branch adds
pipeline states and a glass function APPLIED TO THE LIT COLOUR, outside that
loop, and one object-constant field. Whichever branch merges second gets a
small mechanical conflict, not an interleaved one.

## Phase 1 - the transparent pass

AS BUILT (2026-10-02). Two things changed from the list below after Michael
looked at it, and both matter for Phase 3:
- THE GLASS FILTERS, IT DOES NOT PAINT. Dual-source blending (`PSGlass`):
  result = added + behind x filter, per channel. The material's RGB is the TINT
  (white = clear) and its alpha the DENSITY. Plain premultiplied alpha read as
  a milky veil ("a ghost"), and could never have tinted the room red through a
  red potion. Scatter is density SQUARED, so a thin pane adds almost nothing.
- THE OPACITY IS FLAT AT EVERY ANGLE. Raising it toward 1 at grazing angles
  (Fresnel) needs a picture of the room to fill the rim with; the dark body
  outlined every silhouette in black, an estimate of the arriving light in
  white. The edge now gains only a faint additive sheen (`kRimSheen`).
- Checked: AllocTest `-Glass` (new: a glass decoration in view for the whole
  window, refuses a PASS otherwise; mutation-checked both ways) and `-Sheet`
  PASS, the D3D12 validation layer silent, no shipped model declares alphaMode
  BLEND. `glass_test` in decorations.cat is the temporary subject until Phase 2.

1. MATERIAL: `MaterialParams::transparent` (bool). Off = today's draw, byte
   for byte.
2. PIPELINES: blended PSOs - premultiplied alpha, depth TEST on, depth WRITE
   off - in two cull modes (front-culled and back-culled), each for the LDR
   and HDR targets. Four new PSOs.
3. THE QUEUE: `Renderer::DrawMesh` with a transparent material does not draw.
   - In the SHADOW pass it is skipped: glass casts no shadow (yet).
   - In the scene pass it is queued: mesh, world, material, palette view and
     the view depth of its centre.
   - The queue is a FIXED-CAPACITY array sized at startup. Recording and
     sorting allocate nothing (std::sort on it does not allocate); an
     overflow draws the extra opaque-sorted at the end with a logged warning
     rather than growing.
4. THE FLUSH: `Renderer::FlushTransparent(list)` sorts back to front and
   draws each entry TWICE - back faces, then front faces - so a bottle's far
   wall lands before its near one. Every pass that draws items calls it:
   RenderScene (before the particles), the icon bake (BakeIconFor / map
   icons), ModelPreview (editor dialogs) and the item details dialog.
5. THE GLASS LOOK (scene.hlsl, its own function after the light loop):
   - Opacity = albedo alpha x baseColor alpha, raised toward 1 at grazing
     angles (Schlick Fresnel on N.V), so the rim reads and the face is clear.
   - The specular highlight is added at full strength, not faded with the
     opacity, so wet glass catches the torch.
   - Output premultiplied. Fog and dust already apply per fragment, so glass
     in a dusty room hazes like everything else.
6. HOW A MESH IS MARKED:
   - glTF `alphaMode = BLEND` on a material marks that submesh (bought glass
     usually ships that way; the scripts in Phase 2 write it).
   - Catalog `transparent = 1` on any model entry marks the whole model.
   - A cork or label stays opaque: marking is per submesh, which the
     multi-material path already carries.
7. Dev: `glass [status]` - queued this frame, peak, overflows.
8. Checks: AllocTest default and `-Sheet` (the details dialog flushes too)
   PASS; screenshots judged by Michael on a test decoration marked
   `transparent = 1`.

Not in this phase: refraction (the scene bending through glass - needs a copy
of the opaque scene sampled with an offset; a later phase if the look wants
it), tinted shadows, per-triangle sorting (objects crossing each other can
sort wrong; fine for bottles).

## Phase 2 - containers by script

AS BUILT (2026-10-03):
- `tools/BuildPotion.py` writes potion_vial / potion_bottle / potion_flask.glb
  straight into assets/models (an item loads only .glb; committed by name like
  rock.glb). The glass is a revolved CLOSED section whose inside is the outside
  offset by the wall; every face is checked against the normal its profile
  segment demands and flipped if not (no recalc). The glass material's
  alphaMode / colour are patched into the .glb after export and read back.
  `--tint` / `--density` / `--roughness` build a variant without an edit.
- SIZE: at true size a clear bottle vanished on a 2.5 m square; `GAME_SIZE`
  1.5 (Michael picked it over 2): vial 12 cm, bottle 18 cm, flask 27 cm.
- EMPTY GLASS IS FROSTED (density 0.25, roughness 0.35) - Michael chose it
  from four side-by-sides (clear / light green / frosted / deep green).
- `upright = 1` (items.cat, type-editor row): the floor pose and the icon pose
  keep a bottle standing instead of laying it along its length. Phase 3 needs
  that too - liquid in a lying bottle would have to level itself.
- Items `vial_empty` / `bottle_empty` / `flask_empty`, category `potion`, with
  names and descriptions x5. `glass_test` removed; AllocTest -Glass now places
  `flask_empty` (-GlassCategory / -GlassKind) and passes.

`tools/BuildPotion.py`, using the profile lofter (BuildFountain.py's pattern):
- Three containers from (radius, height) station tables: VIAL (narrow tube),
  BOTTLE (small, round-shouldered), FLASK (wide, round-bellied, long neck).
- Two submeshes each: GLASS (both walls, closed over the lip, `alphaMode =
  BLEND`, a pale tint, low roughness) and a CORK (opaque).
- Winding is the contract (CLAUDE.md's Blender trap): emit every quad
  counter-clockwise from outside, no recalc.
- Unit space; real sizes (vial ~8 cm, bottle ~12 cm, flask ~18 cm).
- Imported with `import-model --raw`; the .glb is a build artifact.
- Michael judges the three shapes in-game (the bridge if he wants to tweak
  live).

## Phase 3 - the liquid inside

AS BUILT (2026-10-03):
- `Game/Liquid.h` (pure, MeshData in and out): the shell is the glass's INNER
  wall - triangles facing into the cavity (toward the axis, or up on the cavity
  floor below the lip) - inset a hair, reversed and turned outward. A model with
  no inner wall falls back to the glass shrunk toward its axis.
- The level is a CLIP PLANE, not a cut: `MaterialParams::liquid` /
  `liquidLevel` (object space), carried into world space through the world
  matrix's Y row so it tilts with the bottle. A back face seen through the cut
  is lit with the plane's normal - the surface.
- `FlushTransparent` draws one object at a time (a run at the same distance):
  every far wall, the liquid both ways, every near wall.
- items.cat `liquid_color` (r, g, b[, density], default density 0.85) and
  `liquid_fill` (0..1, default 0.6); `DungeonWorld::AddLiquid` appends the
  shell to the kind's own model copy, so floor, niche, flight, icon and the
  details dialog all draw it with no code of their own.
- A FILLED bottle's glass goes CLEAR (density / roughness 0.08): the frosting is
  for empty glass, and over a liquid it laid a white veil (health read pink).
- COLOUR RULE (Michael: "more red", "more blue", "more green"): a DARK tint at
  density 0.95. A light tint lets the white behind through and the tonemap lifts
  it to pastel. Health 0.50/0.01/0.015, stamina 0.06/0.42/0.04, mana
  0.04/0.10/0.60, antidote 0.62/0.34/0.02.
- The twelve potion items exist already (look, names x5, `drink_as`); Phase 4
  adds what drinking does and their descriptions. `itemcat.potion` x5 added.
- AllocTest -Glass now places `potion_health_greater` (glass AND liquid): PASS.

Generated at item-kind load for any kind with a `liquid_color`:
- THE SHAPE: the glass submesh pulled in toward the model's vertical axis by
  the wall thickness (import-model centres XZ, so the axis is known). Measured
  on the scripted bottles first, then on a bought one before trusting it.
- THE LEVEL: one object-constant field, a fill height in object space; the
  shader clips the liquid above it. `liquid_fill` (0..1, default 0.8).
- THE SURFACE: the liquid draws double-sided; a back face seen through the cut
  top shades flat as the surface, so the cut reads as a meniscus, not a hole.
- DRAW ORDER inside one object: glass back faces, liquid, glass front faces.
- Shows the same everywhere the item draws: the floor, a niche, in flight,
  the HUD icon, the details dialog.
- An EMPTY container is the same model with no `liquid_color`.

## Phase 4 - drinkable potions

AS BUILT (2026-10-03). Michael's answers: a potion does NOT wake the
unconscious (later spells / items will); potions turn up in `start_items`, the
crypt levels and generated loot.
- items.cat `restore_health` / `restore_stamina` / `restore_mana` (at once, never
  past the maximum) and `cures = poison 0.5, bleed` (each an effect id and the
  share of its bite taken away; absent = lifted). Parsed at load; a drink
  allocates nothing. `ConsumeItem` refuses the unconscious before anything
  moves (`log.consume_downed`), and the log says "drinks" for a `command =
  drink` item (the waterskin too, which used to be "eaten").
- Health moves under a new ledger reason, `drink`; PipelineTest demands that
  route be non-zero and pipeline.eval section 10 drives it (an antidote first).
- Numbers, first cut: health / mana 10 / 25 / 50, stamina 15 / 30 / 60;
  antidotes halve poison / lift poison / lift poison and bleeding.
- The details dialog shows Heals / Restores stamina / Restores mana / Cures; a
  description x5 for each of the twelve.
- PLACEMENT: `potion_health_minor` and `potion_mana_minor` in start_items (the
  world template too - tools/BuildTemplate.py's hardcoded list had lost
  `torch_lit`, now restored); three potions in each crypt level; generated loot
  already took any untagged item, and the new `loot = 0` keeps the EMPTIES out.
- `party` now lists each member's effects. `tools/EvalScripts/potions.eval`
  measures refusal, both antidotes, each pool, the downed refusal and the
  ledger row. NOT covered by AllocTest: a drink from the pack in an armed frame
  (no mode clicks the use menu's Drink row).

Items (items.cat, category `potion` - the medicine pouch already accepts it,
`command = drink`, `drink_as` = the empty container):

| | vial (minor) | small bottle (standard) | flask (greater) |
|---|---|---|---|
| health (red) | potion_health_minor | potion_health | potion_health_greater |
| stamina (green) | potion_stamina_minor | potion_stamina | potion_stamina_greater |
| mana (blue) | potion_mana_minor | potion_mana | potion_mana_greater |
| antidote (amber) | potion_antidote_minor | potion_antidote | potion_antidote_greater |

Plus the empties: `vial_empty`, `bottle_empty`, `flask_empty`.

- New fields `restore_health`, `restore_stamina`, `restore_mana` (instant)
  and `cures = poison[, bleed]`, read by the ONE consume handler
  (`ConsumeItem`), which already refuses an item that would do nothing.
- Health moves under a NEW damage-ledger reason, `drink`, a declared
  sanctioned write like `regen` - PipelineTest must still pass and must see it.
- Antidote first cut: minor halves a poison's remaining bite, standard cures
  poison, greater cures poison and bleeding.
- All numbers are a first cut, in items.cat.
- Lang: item.<id> and item.<id>.desc x5 each; the details dialog gains rows
  for what a potion restores and cures.
- Checks: a `potions` eval suite (each kind does what it says, the empty is
  left behind, a full member is refused and keeps the potion); AllocTest
  `-Sheet` drinking from the pack.

## Phase 5 - the bombs in three sizes

AS BUILT (2026-10-03). Michael: no shattering effect - the blast is enough.
- items.cat `throw_scale` (type-editor row), applied in ItemKindFor AFTER a
  borrowed spell payload: blast damage and linger x s, blast_force (squares)
  rounded and never below 1, on-hit effect magnitudes x s.
- fire_flask_small / fire_flask / fire_flask_large and the poison trio, in the
  vial / small bottle / flask (throw_scale 0.6 / 1 / 1.5, weight 0.3 / 0.6 /
  1.2). Fire 0.75/0.22/0.01, poison a murky olive 0.35/0.38/0.05 (stamina owns
  clean green). The flasks' old "clay flask" descriptions are glass now; names
  and descriptions x5 for the four new sizes.
- Measured by tools/EvalScripts/bombs.eval (a row of sturdy skeletons in the
  open arena): fire dealt 7.8 / 24.5 / 37.5, poison 22.5 / 82.3 / 145.3 with
  poison 1.2 / 2 / 3 per second.
- NOT measured by AllocTest: a glass bomb in FLIGHT (-Throw throws a rock, and a
  bomb breaks so the loop could not lift it again); its parts are covered
  separately (-Throw the flight and landing, -Glass the transparent queue).

- `fire_flask` and `poison_flask` KEEP their ids as the standard size
  (crypt1.ent and dungeon-demo's `start_items` name them), in the small
  bottle. Adding `_small` (vial) and `_large` (flask) beside them.
- Fire orange, poison a murky yellow-green (stamina owns clean green).
- A size scales the blast: a new `throw_scale` multiplier over the blast
  damage, reach and linger (the fire flask's comes from firebolt_burst, so it
  must scale a borrowed payload too).
- Checks: AllocTest `-Throw` with a bomb.

## Phase 6 - docs and close

CLAUDE.md (Renderer features: the transparent pass and the queue; the item
fields), docs/costs.md if anything was bought, and the regression tier
(`/check`).

## Open questions (asked one at a time when their phase opens)

- Phase 4: can a health potion poured into a DOWNED member wake them?
- Phase 4: where do potions appear - the starter kit, `start_items`, placed in
  the crypt levels?
- Phase 5: does a bomb shatter visibly (glass shards) or is the blast enough?
- After Phase 3: does the look want refraction?
