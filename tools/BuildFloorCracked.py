# ============================================================================
# tools/BuildFloorCracked.py — authors the `floor_cracked` FLOOR FEATURE.
#
#   blender --background --factory-startup --python tools\BuildFloorCracked.py -- <out.glb>
#   AssetBaker import-model <out.glb> <assets> floor_cracked --raw
#
# Broken paving: a patch of the cell where the slabs have settled, tilted and
# lost a couple of their number, showing the bed beneath. Like floor_recess and
# floor_drain this REPLACES the cell's floor block (floorfeatures.cat) and wears
# the cell's own floor texture — so the broken paving is the SAME STONE as the
# paving around it, which is the whole reason to do this as a feature.
#
# NOT a heightfield, unlike the drain. A crack wants a crisp edge, and a grid
# fine enough to resolve a 2 cm mortar gap over a 2.5 m cell would cost
# thousands of vertices to describe nine flat slabs. Placed boxes give real gaps
# for ~60 verts. The rule of thumb: heightfield for a SMOOTH shape, placed faces
# for a BROKEN one.
#
# EVERYTHING BELOW z = 0. The cell boundary must stay flat to meet the
# neighbouring floor blocks (see BuildFloorRecess's header), so the damage is
# inset behind a flat margin and settles DOWNWARD — a slab standing proud at the
# edge would be a step into the next cell. That constraint is also why it reads
# as "this patch of floor has broken" rather than "the whole tile is rubble",
# which is the more useful thing to be able to place anyway.
#
# EVERYTHING IS IN UNITS: 1.0 = one dungeon square (2.5 m), Z up.
# ============================================================================
import sys

import bmesh
import bpy

HALF = 0.5      # the cell — the floor block's extent, do not change
MOUTH = 0.42    # half-width of the broken patch; outside this the floor is flat
BED_Z = -0.040  # the dirt bed the slabs have settled onto
SLABS = 3       # per side
GAP = 0.022     # mortar gap between slabs — a crisp 5.5 cm crack
MISSING = {(1, 0), (2, 2)}  # slabs that are simply gone, showing the bed

SINK_MIN, SINK_MAX = 0.006, 0.030  # how far a slab has settled below z = 0
TILT = 0.007                       # corner-to-corner lean

OUT = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else "floor_cracked.glb"


def hash01(i, j, salt):
    """Deterministic 0..1 — the asset must be identical on every re-run."""
    h = (i * 73856093) ^ (j * 19349663) ^ (salt * 83492791)
    h &= 0xFFFFFFFF
    h = (h ^ (h >> 13)) * 1274126177 & 0xFFFFFFFF
    return ((h >> 8) & 0xFFFF) / 65535.0


# --- empty the factory scene ------------------------------------------------
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

bm = bmesh.new()

CORNERS = [(-1, -1), (1, -1), (1, 1), (-1, 1)]  # CCW seen from +Z


def add_ring_strips(outer, inner, z):
    """The flat margin: four strips from the cell edge in to the patch, facing up."""
    o = [bm.verts.new((sx * outer, sy * outer, z)) for sx, sy in CORNERS]
    m = [bm.verts.new((sx * inner, sy * inner, z)) for sx, sy in CORNERS]
    for i in range(4):
        j = (i + 1) % 4
        bm.faces.new((o[i], o[j], m[j], m[i]))
    return m


def add_box(x0, x1, y0, y1, zbot, ztop):
    """A box whose TOP corners have individual heights, so a slab can lean.

    Winding is stated corner by corner — these boxes are disjoint islands and
    recalc_face_normals would flip them arbitrarily (CLAUDE.md's normals trap).
    The leaning top makes the side quads slightly non-planar; the exporter
    triangulates them, which is fine at this scale.
    """
    v = [bm.verts.new(p) for p in (
        (x0, y0, zbot), (x1, y0, zbot), (x1, y1, zbot), (x0, y1, zbot),
        (x0, y0, ztop[0]), (x1, y0, ztop[1]), (x1, y1, ztop[2]), (x0, y1, ztop[3]),
    )]
    for face in ((0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
                 (2, 3, 7, 6), (1, 2, 6, 5), (3, 0, 4, 7)):
        bm.faces.new([v[i] for i in face])


# The flat margin, then the lip down to the bed, then the bed itself.
mouth_top = add_ring_strips(HALF, MOUTH, 0.0)
mouth_bed = [bm.verts.new((sx * MOUTH, sy * MOUTH, BED_Z)) for sx, sy in CORNERS]
for i in range(4):
    j = (i + 1) % 4
    bm.faces.new((mouth_top[i], mouth_top[j], mouth_bed[j], mouth_bed[i]))
bm.faces.new(mouth_bed)  # the bed, seen from above

# The slabs. Each is inset by half a gap inside its share of the patch, so the
# gaps are real openings onto the bed rather than painted lines.
pitch = 2.0 * MOUTH / SLABS
for j in range(SLABS):
    for i in range(SLABS):
        if (i, j) in MISSING:
            continue
        cx = -MOUTH + pitch * (i + 0.5)
        cy = -MOUTH + pitch * (j + 0.5)
        half = pitch * 0.5 - GAP * 0.5
        sink = SINK_MIN + (SINK_MAX - SINK_MIN) * hash01(i, j, 1)
        tx = (hash01(i, j, 2) - 0.5) * 2.0 * TILT
        ty = (hash01(i, j, 3) - 0.5) * 2.0 * TILT
        # Corner heights in the CORNERS order: (-,-), (+,-), (+,+), (-,+).
        ztop = [-sink + sx * tx + sy * ty for sx, sy in CORNERS]
        add_box(cx - half, cx + half, cy - half, cy + half, BED_Z, ztop)

# --- UVs --------------------------------------------------------------------
# Up/down faces take the FLOOR BLOCK's mapping, so slab tops, bed and margin all
# continue the same stone. The slab sides are tiny verticals and get an in-plane
# projection (the BuildFloorRecess rule).
uv = bm.loops.layers.uv.verify()
for face in bm.faces:
    nx, ny, nz = (abs(c) for c in face.normal)
    for loop in face.loops:
        co = loop.vert.co
        if nz >= nx and nz >= ny:   p = (co.x + 0.5, -co.y + 0.5)
        elif nx >= ny:              p = (-co.y + 0.5, -co.z)
        else:                       p = (co.x + 0.5, -co.z)
        loop[uv].uv = p

mesh = bpy.data.meshes.new("floor_cracked")
bm.to_mesh(mesh)
bm.free()

obj = bpy.data.objects.new("floor_cracked", mesh)
bpy.context.scene.collection.objects.link(obj)

xs = [v.co.x for v in mesh.vertices]
ys = [v.co.y for v in mesh.vertices]
zs = [v.co.z for v in mesh.vertices]
print(f"BuildFloorCracked: {len(mesh.vertices)} verts, "
      f"x {min(xs):+.3f}..{max(xs):+.3f}, z {min(zs):+.3f}..{max(zs):+.3f} (units)")

assert abs(min(xs) + HALF) < 1e-6 and abs(max(xs) - HALF) < 1e-6, "x extent moved"
assert abs(min(ys) + HALF) < 1e-6 and abs(max(ys) - HALF) < 1e-6, "y extent moved"
assert abs(max(zs)) < 1e-6, "something rose above the floor plane"

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
print(f"BuildFloorCracked: wrote {OUT}")
