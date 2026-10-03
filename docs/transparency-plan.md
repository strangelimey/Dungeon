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
