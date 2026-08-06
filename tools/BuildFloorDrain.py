# ============================================================================
# tools/BuildFloorDrain.py — authors the `floor_drain` FLOOR FEATURE.
#
#   blender --background --factory-startup --python tools\BuildFloorDrain.py -- <out.glb>
#   AssetBaker import-model <out.glb> <assets> floor_drain --raw
#
# A dished drain whose throat drops a full storey into darkness. Like
# floor_recess this REPLACES the cell's floor block (floorfeatures.cat), so it
# wears whatever floor texture the cell wears — see that script's header for the
# extent/UV contract, which this asserts too.
#
# BUILT AS A POLAR LOFT, and the reason is worth keeping. The first version was a
# HEIGHTFIELD (a grid with z = f(radius)), which is how the engine builds its own
# worn floor blocks and was the right shape for a shallow basin. It cannot
# survive the throat going deep: a heightfield has ONE z per (x,y), so it can
# never be vertical — asking for a storey-deep shaft just turns the throat into a
# smeared cone, and projecting its UVs from above streaks the texture radially
# down the sides. So: heightfield for a shape that is a SURFACE, a loft once it
# has WALLS.
#
# The awkward part of a polar mesh in a square cell is the skirt out to the
# boundary, and it falls out neatly: the outermost ring puts a vertex ON the
# square perimeter at each of the ring's own angles (d = 0.5 / max(|cos|,|sin|)).
# Consecutive boundary points along one side are collinear, so the mesh edge lies
# exactly on the cell edge — and with N a multiple of 8 the four corners are hit
# exactly. The skirt is then just another quad band.
#
# EVERYTHING IS IN UNITS: 1.0 = one dungeon square (2.5 m), Z up. The exporter
# turns Blender Z into the engine's Y and Blender -Y into the engine's +Z.
# ============================================================================
import math
import sys

import bmesh
import bpy

HALF = 0.5    # the cell — the floor block's extent, do not change
N = 32        # segments; a multiple of 8 so the square's corners are hit exactly

# A DRAIN IS AN OPENING, NOT A DIP. The first shape here was a wide gentle
# saucer — 1.5 cm at the rim easing to 6 cm, spread over 1.8 m — and Michael's
# verdict was "it just looks like a bumpy floor". He was right: at that gradient
# it is indistinguishable from the relief the worn floor blocks already carry.
# What makes a hole read is a CRISP EDGE and a STEEP drop, so the profile is now
# a narrow chamfered lip, a near-vertical funnel, and then the shaft.
R_SHAFT = 0.170    # the throat — 85 cm across
Z_BOTTOM = -4.000  # four storeys — see BuildFloorRecess's DEPTH for why that deep
Z_THROAT = -0.200  # where the vertical shaft meets the funnel
R_FUNNEL, Z_FUNNEL = 0.220, -0.035  # steep: 16.5 cm of drop over 5 cm of radius
R_RIM = 0.250      # the lip meets the flat floor — 1.25 m opening

SQUARE = None      # sentinel: this ring rides the cell perimeter, not a radius

# Inner/lower to outer/upper. The first band is the vertical shaft; then the
# funnel, the lip chamfer, and the flat skirt out to the cell edge.
RINGS = [
    (R_SHAFT, Z_BOTTOM),
    (R_SHAFT, Z_THROAT),
    (R_FUNNEL, Z_FUNNEL),
    (R_RIM, 0.0),
    (SQUARE, 0.0),
]

OUT = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else "floor_drain.glb"

# --- empty the factory scene ------------------------------------------------
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

bm = bmesh.new()
uv = bm.loops.layers.uv.verify()  # before any face, so UVs are set as we build


def ring_pos(radius, z, i):
    theta = 2.0 * math.pi * i / N
    c, s = math.cos(theta), math.sin(theta)
    if radius is SQUARE:
        d = HALF / max(abs(c), abs(s))  # lands ON the cell perimeter
        return (d * c, d * s, z)
    return (radius * c, radius * s, z)


def floor_uv(co):
    """The floor block's mapping — what keeps the cell tiling with its neighbours.
    v is flipped because Blender -Y is the engine's +Z."""
    return (co[0] + 0.5, -co[1] + 0.5)


rings = [[bm.verts.new(ring_pos(r, z, i)) for i in range(N)] for r, z in RINGS]

for k in range(len(RINGS) - 1):
    lower, upper = rings[k], rings[k + 1]
    z_lo, z_hi = RINGS[k][1], RINGS[k + 1][1]
    # The shaft is the one band with no radial extent — it is a wall, and gets an
    # UNROLLED mapping (u = arc length, v = depth) instead of a projection from
    # above, which would collapse it to a line. The CLAUDE.md rule for swept
    # surfaces, applied where it actually bites.
    vertical = RINGS[k][0] is not SQUARE and RINGS[k + 1][0] is not SQUARE \
        and abs(RINGS[k][0] - RINGS[k + 1][0]) < 1e-9
    arc = 2.0 * math.pi * (RINGS[k][0] or 0.0) / N
    for i in range(N):
        j = (i + 1) % N
        # (lower_i, upper_i, upper_j, lower_j) — CCW seen from above for a flat
        # band, and inward-facing for the shaft. Stated, never recalculated.
        face = bm.faces.new((lower[i], upper[i], upper[j], lower[j]))
        if vertical:
            u0, u1 = i * arc, (i + 1) * arc
            uvs = [(u0, -z_lo), (u0, -z_hi), (u1, -z_hi), (u1, -z_lo)]
        else:
            uvs = [floor_uv(loop.vert.co) for loop in face.loops]
        for loop, p in zip(face.loops, uvs):
            loop[uv].uv = p

# The shaft's floor, a fan from the centre. Present rather than left open so the
# mesh never shows the void through it; at a storey down it reads as black.
centre = bm.verts.new((0.0, 0.0, Z_BOTTOM))
for i in range(N):
    j = (i + 1) % N
    face = bm.faces.new((centre, rings[0][i], rings[0][j]))
    for loop in face.loops:
        loop[uv].uv = floor_uv(loop.vert.co)

mesh = bpy.data.meshes.new("floor_drain")
bm.to_mesh(mesh)
bm.free()

obj = bpy.data.objects.new("floor_drain", mesh)
bpy.context.scene.collection.objects.link(obj)

xs = [v.co.x for v in mesh.vertices]
ys = [v.co.y for v in mesh.vertices]
zs = [v.co.z for v in mesh.vertices]
print(f"BuildFloorDrain: {len(mesh.vertices)} verts, "
      f"x {min(xs):+.3f}..{max(xs):+.3f}, z {min(zs):+.3f}..{max(zs):+.3f} (units)")

# The contract, checked rather than trusted (BuildFloorRecess does the same).
assert abs(min(xs) + HALF) < 1e-6 and abs(max(xs) - HALF) < 1e-6, "x extent moved"
assert abs(min(ys) + HALF) < 1e-6 and abs(max(ys) - HALF) < 1e-6, "y extent moved"
edge = [v.co.z for v in mesh.vertices
        if abs(abs(v.co.x) - HALF) < 1e-6 or abs(abs(v.co.y) - HALF) < 1e-6]
assert len(edge) == N, f"expected {N} verts on the cell perimeter, got {len(edge)}"
assert max(abs(z) for z in edge) < 1e-6, "the cell boundary left z = 0"

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
print(f"BuildFloorDrain: wrote {OUT}")
