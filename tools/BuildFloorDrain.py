# ============================================================================
# tools/BuildFloorDrain.py — authors the `floor_drain` FLOOR FEATURE.
#
#   blender --background --factory-startup --python tools\BuildFloorDrain.py -- <out.glb>
#   AssetBaker import-model <out.glb> <assets> floor_drain --raw
#
# A dished drain sunk into a cell: gentle basin, steep throat, flat bottom. Like
# floor_recess this REPLACES the cell's floor block (floorfeatures.cat), so it
# wears whatever floor texture the cell wears — see that script's header for the
# extent/UV contract, which this asserts too.
#
# BUILT AS A HEIGHTFIELD, not as placed faces, because the shape is radial: a
# grid over the cell with z = a function of radius. That is exactly how the
# engine builds its own worn floor blocks (ModelBaker's BuildWornFloorBlock over
# a wear field), and it buys three things at once — the outer boundary is flat
# by construction, the floor UVs fall straight out of x and y, and the normals
# come from a finite difference rather than needing planar faces.
#
# It also sidesteps the UV trap in CLAUDE.md. Dominant-axis projection is only
# valid on box-ish geometry; on a dish the normal rotates through 90 degrees and
# the dominant axis would FLIP mid-surface and seam. Here every vertex is
# projected from ABOVE, the same mapping the flat part uses, so the basin is
# continuous with the floor around it. A shallow dish stretches only slightly;
# the throat stretches more, but it is a small dark hole.
#
# EVERYTHING IS IN UNITS: 1.0 = one dungeon square (2.5 m), Z up. The exporter
# turns Blender Z into the engine's Y and Blender -Y into the engine's +Z.
# ============================================================================
import sys

import bmesh
import bpy

HALF = 0.5   # the cell — the floor block's extent, do not change
GRID = 40    # quads per side; 41x41 verts

# (radius, z) stations, inner to outer, smoothstepped between. The last two put
# the surface back at 0 well inside the cell edge, so the whole boundary is flat
# and meets the neighbouring floor blocks exactly.
PROFILE = [
    (0.000, -0.220),  # throat floor
    (0.100, -0.220),
    (0.140, -0.070),  # throat wall — steep, reads as a hole
    (0.320, -0.015),  # the basin, gentle
    (0.360, 0.000),
    (1.000, 0.000),   # flat, out to the corners (r = 0.707 at a corner)
]

OUT = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else "floor_drain.glb"


def smoothstep(t):
    t = min(max(t, 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


def height(x, y):
    r = (x * x + y * y) ** 0.5
    for (r0, z0), (r1, z1) in zip(PROFILE, PROFILE[1:]):
        if r <= r1:
            if r1 - r0 <= 0.0:
                return z1
            return z0 + (z1 - z0) * smoothstep((r - r0) / (r1 - r0))
    return PROFILE[-1][1]


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

# CCW seen from +Z, so every face comes out pointing up. Stated, not recalculated
# (recalc_face_normals needs connectivity and flips hand-built shapes).
for j in range(GRID):
    for i in range(GRID):
        bm.faces.new((verts[j][i], verts[j][i + 1],
                      verts[j + 1][i + 1], verts[j + 1][i]))

bm.normal_update()  # a grid IS connected, so smooth vertex normals are honest here

# --- UVs --------------------------------------------------------------------
# The floor block's mapping, for every vertex — flat part and basin alike, which
# is what keeps the two continuous. v is flipped because Blender -Y is engine +Z.
uv = bm.loops.layers.uv.verify()
for face in bm.faces:
    for loop in face.loops:
        co = loop.vert.co
        loop[uv].uv = (co.x + 0.5, -co.y + 0.5)

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
assert max(abs(z) for z in edge) < 1e-6, "the cell boundary left z = 0"

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
print(f"BuildFloorDrain: wrote {OUT}")
