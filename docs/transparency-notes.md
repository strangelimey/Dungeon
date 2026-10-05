# transparency - brain dump (raw, verbatim in substance)

## Context (how the branch started, 2026-10-02)

Michael gathered fab.com listings for new objects. All six have a usable
format (fbx, some glb/obj). Four are glass, and the renderer has no
transparency (only alpha-test cutout), so the branch opened to add a
transparent pass before buying them.

1. Apothecary poison and potion jars (HorusZ) - max, fbx; 2k PBR incl. an
   opacity map; refraction-led look
   https://www.fab.com/listings/e9cea16e-67c8-4047-81e9-077d3ec11d48
2. Chemical Flask (Crop3d) - blend, max, fbx, glb, obj; flat materials, modern
   https://www.fab.com/listings/dac6fe37-9966-48f7-a7a9-aaf11293061c
3. Lab Vial (Crop3d) - blend, max, fbx, glb, obj; flat materials, modern
   https://www.fab.com/listings/8c67ab37-aeb8-44df-9a01-11ab726794d1
4. Small Glass Bottle With Cork (HQ3DMOD) - blend, fbx; 4k PBR, OpenGL normals
   ("converted files, not textured" contradicts that)
   https://www.fab.com/listings/bacc470e-4b21-43b6-b7ad-5fdce367c75b
5. 40 Medieval Maces Vol. 1 (Faraz CGA) - max, blend, fbx, obj; unwrapped, no
   textures mentioned
   https://www.fab.com/listings/5b691946-0c23-4112-a8b1-649f35abed1c
6. 100+ Upgradable Swords (CodePhase Games) - Unreal, fbx; 2k textures
   https://www.fab.com/listings/1771dfd2-87ad-44ab-9ab8-9d9cd49aca5b

Overlap to watch: `lighting-updates` (in progress) Phase 3 rewrites the light
loop in scene.hlsl and the frame constants in Renderer. Keep this branch's
shader change outside that loop.

## Dump

- Glass bottles need to show the LIQUID INSIDE.
- A different colour for a different potion.
- Weaker potions in SMALLER containers: vial, small bottle, flask, etc.
- (end of dump: "the biggest task this session is adding transparency to
  support these")

---

# Organized

## A. A transparent render pass (the main task)
- Glass has to read as glass: you see through it to what is behind and inside.
- Today: alpha-test cutout only (`alpha_test`, opaque PSO, no blending).
  scene.hlsl already returns albedo alpha; ParticleBatch already blends.
- Needed: blended PSOs (depth test, no depth write), a per-material flag from
  the catalog, transparent draws queued after the opaque ones and sorted back
  to front, before the particles. Not in the shadow pass at first.
- Opacity maps (listings 1 and 4) go into the albedo's alpha at import.

## B. Liquid inside the glass
- A bottle shows its contents.
- The COLOUR is per potion, so it is DATA on the item, not baked into the
  texture - one bottle model serves many potions.
- Gap: none of the listings says it ships a liquid mesh. Either the liquid is
  generated from the bottle's own shape (an inner shell cut at a fill line) or
  each bottle needs one authored.
- Possible extra (not asked for): the fill line could follow the item's charge,
  the way the waterskin has full / half / empty.

## C. Potions sized by strength
- Weaker = smaller: vial < small bottle < flask (< larger).
- So a potion is (container model) x (liquid colour) x (strength).
- Gap: NO potion item exists yet. The medicine pouch accepts category
  `potion`; the flasks that exist are THROWN (fire, poison); CLAUDE.md lists
  potions and healing as still to build. Is drinking one part of this branch?

## D. Buying
- The container shapes come from the listings: 1 (apothecary set), 4 (small
  corked bottle), 2/3 (flask, vial - but modern lab glass, flat materials).
- 5 (maces) and 6 (swords) are unrelated to this branch.

