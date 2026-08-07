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


def height_r(r):
    """The arch profile, as a function of the SQUARE radius alone."""
    if r >= SPRING:
        return 0.0
    # Flat-topped at the crown (zero slope at r = 0) rather than pointed. Note
    # it does NOT meet the flat ceiling tangentially at the springing — the
    # slope there is RISE * pi, about 47 degrees, which is the steepest point on
    # the whole surface. (An earlier comment here claimed the opposite.) That is
    # a deliberate spring line rather than a defect: a vault that eased into the
    # ceiling would have no visible impost at all.
    return RISE * math.cos(0.5 * math.pi * r / SPRING)


def height(x, y):
    """A cloister vault: one arch profile swept over the SQUARE radius.

    Using max(|x|,|y|) rather than sqrt(x^2+y^2) is the whole difference between
    a cloister vault and a dome. The square metric's level sets are squares, so
    the surface meets all four edges at z = 0 simultaneously, and the diagonals —
    where the metric switches from |x| to |y| — become the creases that read as
    the GROINS. Both the flat boundary and the groins fall out of the maths
    instead of being modelled.
    """
    return height_r(max(abs(x), abs(y)))


# --- the UV remap -----------------------------------------------------------
# THE DEFECT THIS FIXES: the vault's texture streaked radially up the webs. The
# mapping is projected from ABOVE, which is what keeps it seamless with the flat
# ceiling blocks around it — but a planar projection is the surface's SHADOW,
# and on a web climbing at 47 degrees a centimetre of shadow is a centimetre and
# a half of stone. The texture gets stretched along the slope by exactly that
# ratio.
#
# THE REALISATION THAT MAKES IT FIXABLE: the tiling contract binds only the
# BOUNDARY. A feature tile has to carry the block's own UVs where it meets its
# neighbours (u = x + 0.5, v = z + 0.5) or the stone jumps at the cell edge —
# but nothing constrains the interior at all. So the interior can be
# redistributed by SURFACE ARC LENGTH while the edge stays pinned to the block
# mapping, and both properties hold at once.
#
# The remap scales the (x, y) UV vector by s(r)/s(0.5), the cumulative arc
# length along the profile normalised to the cell edge. At r = 0.5 that is 1 by
# construction, so the boundary is untouched; inside, the texture advances
# fastest where the surface is steepest, which is what evens out the density.
# It is exact along the radial direction (the direction of the stretch, since
# the height depends only on r) and leaves a second-order tangential error,
# which is invisible next to what it removes.
_ARC_N = 512
_ARC = [0.0]
for _i in range(1, _ARC_N + 1):
    _r0, _r1 = HALF * (_i - 1) / _ARC_N, HALF * _i / _ARC_N
    _ARC.append(_ARC[-1] + math.hypot(_r1 - _r0, height_r(_r1) - height_r(_r0)))


def uv_radius(r):
    """The radius the texture should sit at, so density follows the stone."""
    t = min(max(r / HALF, 0.0), 1.0) * _ARC_N
    i = min(int(t), _ARC_N - 1)
    s = _ARC[i] + (_ARC[i + 1] - _ARC[i]) * (t - i)
    return HALF * s / _ARC[-1]


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
        r = max(abs(co.x), abs(co.y))
        # Scale the UV vector, not each axis: k depends on r alone, so this stays
        # continuous across the diagonals where the square metric switches axis.
        # Correcting only the dominant axis would be exact but would tear along
        # every groin.
        k = uv_radius(r) / r if r > 1e-9 else 1.0
        loop[uv].uv = (co.x * k + 0.5, -co.y * k + 0.5)

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

# THE TILING CONTRACT, checked on the UVs rather than assumed. The arc-length
# remap is free to do what it likes inside, but every vertex ON the cell
# boundary must still carry the plain block mapping or the stone jumps against
# the flat ceiling next door — which is the whole reason the projection is from
# above in the first place.
uv_layer = mesh.uv_layers.active.data
for poly in mesh.polygons:
    for li in poly.loop_indices:
        v = mesh.vertices[mesh.loops[li].vertex_index].co
        if abs(abs(v.x) - HALF) < 1e-6 or abs(abs(v.y) - HALF) < 1e-6:
            want = (v.x + 0.5, -v.y + 0.5)
            got = uv_layer[li].uv
            assert abs(got[0] - want[0]) < 1e-5 and abs(got[1] - want[1]) < 1e-5, (
                f"boundary UV drifted off the block mapping at {tuple(v)}")

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
print(f"BuildCeilingVault: wrote {OUT}")
