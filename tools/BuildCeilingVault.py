# ============================================================================
# tools/BuildCeilingVault.py — authors the `ceiling_vault` SURFACE FEATURE.
#
#   blender --background --factory-startup --python tools\BuildCeilingVault.py -- <out.glb>
#   AssetBaker import-model <out.glb> <assets> ceiling_vault --raw
#
# A CLOISTER VAULT (a domical vault): four curved webs springing from the four
# edges of the bay and meeting at a crown, with groins running up the diagonals.
# It REPLACES the cell's ceiling block (surfacefeatures.cat, `surface =
# ceiling`), riding the ceiling's variant bucket, so it wears whatever ceiling
# texture the cell wears.
#
# WHY NOT A GROIN VAULT, which is the more obvious choice over a square bay. A
# groin vault is two barrel vaults crossing, and a barrel vault is OPEN AT ITS
# ENDS — that opening is the arch. Its boundary is therefore at full crown height
# along two of the four edges, which is exactly what a per-cell tile may not do:
# the edges have to sit at z = 0 or the cell steps against the flat ceiling block
# beside it. The first version here was a groin vault and the boundary assert
# below caught it immediately. A cloister vault springs from ALL FOUR edges, so
# it satisfies the contract by construction — and it is the right vault for a bay
# enclosed by walls anyway, which is what a dungeon cell usually is.
#
# (A true groin vault would want a run of adjacent vaulted cells sharing their
# open arches — a thing the feature system could express only if a tile could see
# its neighbours, which it deliberately cannot.)
#
# THE CEILING BLOCK'S CONVENTION, which this must match exactly (ModelBaker's
# BuildWornCeilingBlock):
#   * authored around z = 0 and stamped by the game at y = kWallHeight, so z = 0
#     here IS the ceiling plane;
#   * faces point DOWN — you look up at it from inside the room;
#   * the vault rises in +z, i.e. UP into the rock above the cell;
#   * UVs are the same u = x + 0.5, v = z + 0.5 mapping the floor uses.
# The cell edges must stay at z = 0 for the same reason a floor tile's do: the
# worn blocks pin their displacement to zero there, so a feature that keeps the
# boundary flat meets its neighbours seamlessly.
#
# A HEIGHTFIELD is right here, unlike the drain. A groin vault is a SURFACE — one
# height per (x,y), no walls — so the grid gives it smooth normals, the ceiling
# UVs for free, and a flat boundary by construction. (The rule that cost a
# rewrite: heightfield for a surface, loft once it has walls.)
#
# EVERYTHING IS IN UNITS: 1.0 = one dungeon square (2.5 m), Z up.
# ============================================================================
import math
import sys

import bmesh
import bpy

HALF = 0.5    # the cell — the ceiling block's extent, do not change
GRID = 32     # quads per side; 33x33 verts

RISE = 0.34    # crown height above the ceiling plane — 85 cm
SPRING = 0.50  # springs from the cell edge itself, so the whole bay is vaulted

OUT = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else "ceiling_vault.glb"


def height(x, y):
    """A cloister vault: one arch profile swept over the SQUARE radius.

    Using max(|x|,|y|) rather than sqrt(x^2+y^2) is the whole difference between
    a cloister vault and a dome. The square metric's level sets are squares, so
    the surface meets all four edges at z = 0 simultaneously, and the diagonals —
    where the metric switches from |x| to |y| — become the creases that read as
    the GROINS. Both the flat boundary and the groins fall out of the maths
    instead of being modelled.
    """
    r = max(abs(x), abs(y))
    if r >= SPRING:
        return 0.0
    # cos meets the flat ceiling tangentially at the springing rather than at a
    # crease, and reaches the crown flat-topped rather than pointed.
    return RISE * math.cos(0.5 * math.pi * r / SPRING)


# --- empty the factory scene ------------------------------------------------
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

bm = bmesh.new()

verts = []
for j in range(GRID + 1):
    y = -HALF + 2.0 * HALF * j / GRID
    row = []
    for i in range(GRID + 1):
        x = -HALF + 2.0 * HALF * i / GRID
        row.append(bm.verts.new((x, y, height(x, y))))
    verts.append(row)

# Wound to face DOWN (-Z) — the ceiling is seen from below. That is the reverse
# of the floor tile's winding, and it is the one thing most likely to be wrong on
# a ceiling asset: a vault wound the floor's way is invisible from the room.
for j in range(GRID):
    for i in range(GRID):
        bm.faces.new((verts[j][i], verts[j + 1][i],
                      verts[j + 1][i + 1], verts[j][i + 1]))

bm.normal_update()  # a grid IS connected, so smooth vertex normals are honest

# --- UVs --------------------------------------------------------------------
# The ceiling block's mapping, which is the floor block's. v is flipped because
# Blender -Y is the engine's +Z.
uv = bm.loops.layers.uv.verify()
for face in bm.faces:
    for loop in face.loops:
        co = loop.vert.co
        loop[uv].uv = (co.x + 0.5, -co.y + 0.5)

mesh = bpy.data.meshes.new("ceiling_vault")
bm.to_mesh(mesh)
bm.free()

obj = bpy.data.objects.new("ceiling_vault", mesh)
bpy.context.scene.collection.objects.link(obj)

xs = [v.co.x for v in mesh.vertices]
ys = [v.co.y for v in mesh.vertices]
zs = [v.co.z for v in mesh.vertices]
print(f"BuildCeilingVault: {len(mesh.vertices)} verts, "
      f"x {min(xs):+.3f}..{max(xs):+.3f}, z {min(zs):+.3f}..{max(zs):+.3f} (units)")

# The contract, checked rather than trusted.
assert abs(min(xs) + HALF) < 1e-6 and abs(max(xs) - HALF) < 1e-6, "x extent moved"
assert abs(min(ys) + HALF) < 1e-6 and abs(max(ys) - HALF) < 1e-6, "y extent moved"
assert min(zs) >= -1e-6, "a vault rises into the rock; nothing may hang below z = 0"
edge = [v.co.z for v in mesh.vertices
        if abs(abs(v.co.x) - HALF) < 1e-6 or abs(abs(v.co.y) - HALF) < 1e-6]
assert max(abs(z) for z in edge) < 1e-6, "the cell boundary left z = 0"
# And it must actually be a vault, not a flat plate.
assert max(zs) > RISE * 0.9, "the crown did not rise"

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
print(f"BuildCeilingVault: wrote {OUT}")
