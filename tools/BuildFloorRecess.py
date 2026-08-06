# ============================================================================
# tools/BuildFloorRecess.py — authors the `floor_recess` FLOOR FEATURE.
#
#   blender --background --factory-startup --python tools\BuildFloorRecess.py -- <out.glb>
#   AssetBaker import-model <out.glb> <assets> floor_recess --raw
#
# This is not a prop. It is a floor FEATURE: the mesh builder stamps it IN PLACE
# OF the cell's floor block, into the same variant bucket, so it wears whatever
# floor texture that cell wears — exactly how a wall niche replaces a wall panel
# (wallfeatures.cat, DungeonMeshBuilder). That is what lets a grate sit flush and
# actually carve a hole, instead of being a box parked on top of the floor.
#
# TWO THINGS IT MUST MATCH EXACTLY, or the cell stops tiling with its neighbours:
#
#   EXTENT — the floor block spans local x,z in [-0.5, 0.5] with its surface at
#   y = 0 (ModelBaker's BuildWornFloorBlock). The worn block is DISPLACED, but
#   its displacement is pinned to zero at the cell edges (PinRamp), which is what
#   makes a flat tile meet it seamlessly. So the outer ring here sits at z = 0
#   and MUST NOT MOVE — note the bevel below is restricted to the mouth for
#   precisely this reason.
#
#   UVs — the floor block uses u = x + 0.5, v = z + 0.5 (kUvScale is 1.0, one
#   texture repeat per cell). Anything else and the stone jumps at the boundary.
#
# EVERYTHING IS IN UNITS: 1.0 = one dungeon square (game::kUnit, 2.5 m), Z up.
# The exporter's +Y-Up conversion turns Blender Z into the engine's Y and Blender
# -Y into the engine's +Z — hence the v flip (v = -by + 0.5) below.
# ============================================================================
import sys

import bmesh
import bpy

HALF = 0.5      # the cell — do not change, it is the floor block's extent
MOUTH = 0.20    # half-width of the opening — 1.0 m across
DEPTH = 0.10    # 25 cm deep, enough to read as a hole and still show its floor

BEVEL = 0.006   # MOUTH RIM ONLY (see above)
SEGMENTS = 2

OUT = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else "floor_recess.glb"

# --- empty the factory scene ------------------------------------------------
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

bm = bmesh.new()

# Three rings, each in the same CCW-from-above corner order, so a strip
# (outer[i], outer[i+1], inner[i+1], inner[i]) always comes out facing +Z and a
# wall (mouth[i], mouth[i+1], bottom[i+1], bottom[i]) always faces INTO the well.
# Winding is stated, never recalculated: recalc_face_normals needs connectivity
# and flips hand-built shapes arbitrarily (the trap in CLAUDE.md).
CORNERS = [(-1, -1), (1, -1), (1, 1), (-1, 1)]

outer = [bm.verts.new((sx * HALF, sy * HALF, 0.0)) for sx, sy in CORNERS]
mouth = [bm.verts.new((sx * MOUTH, sy * MOUTH, 0.0)) for sx, sy in CORNERS]
floor = [bm.verts.new((sx * MOUTH, sy * MOUTH, -DEPTH)) for sx, sy in CORNERS]

for i in range(4):
    j = (i + 1) % 4
    bm.faces.new((outer[i], outer[j], mouth[j], mouth[i]))  # top, around the hole
    bm.faces.new((mouth[i], mouth[j], floor[j], floor[i]))  # well wall, inward
bm.faces.new(floor)  # the bottom, seen from above

# --- break the mouth rim ----------------------------------------------------
# ONLY the rim. Bevelling everything would drag the outer ring off z = 0 and
# off +-0.5, and the cell would no longer meet the floor blocks beside it.
if BEVEL > 0.0:
    rim = [e for e in bm.edges
           if all(v in mouth for v in e.verts)]
    bmesh.ops.bevel(
        bm,
        geom=rim,
        offset=BEVEL,
        offset_type="OFFSET",
        segments=SEGMENTS,
        profile=0.5,
        affect="EDGES",
        clamp_overlap=True,
    )

# --- UVs --------------------------------------------------------------------
# Up/down faces take the FLOOR BLOCK's mapping exactly (see the header). The
# well's walls get their own in-plane projection so the stone does not smear
# down them — the same dominant-axis idea as WallFaceUv, with v running down.
uv = bm.loops.layers.uv.verify()
for face in bm.faces:
    nx, ny, nz = (abs(c) for c in face.normal)
    for loop in face.loops:
        co = loop.vert.co
        if nz >= nx and nz >= ny:   p = (co.x + 0.5, -co.y + 0.5)
        elif nx >= ny:              p = (-co.y + 0.5, -co.z)
        else:                       p = (co.x + 0.5, -co.z)
        loop[uv].uv = p

mesh = bpy.data.meshes.new("floor_recess")
bm.to_mesh(mesh)
bm.free()

obj = bpy.data.objects.new("floor_recess", mesh)
bpy.context.scene.collection.objects.link(obj)

xs = [v.co.x for v in mesh.vertices]
ys = [v.co.y for v in mesh.vertices]
zs = [v.co.z for v in mesh.vertices]
print(f"BuildFloorRecess: {len(mesh.vertices)} verts, "
      f"x {min(xs):+.3f}..{max(xs):+.3f}, y {min(ys):+.3f}..{max(ys):+.3f}, "
      f"z {min(zs):+.3f}..{max(zs):+.3f} (units)")
# The check that matters: the outer ring must still be exactly the cell.
assert abs(min(xs) + HALF) < 1e-6 and abs(max(xs) - HALF) < 1e-6, "x extent moved"
assert abs(min(ys) + HALF) < 1e-6 and abs(max(ys) - HALF) < 1e-6, "y extent moved"
assert abs(max(zs)) < 1e-6, "top surface left z = 0"

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
print(f"BuildFloorRecess: wrote {OUT}")
