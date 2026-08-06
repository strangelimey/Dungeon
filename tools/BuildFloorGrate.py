# ============================================================================
# tools/BuildFloorGrate.py — authors the `floor_grate` prop, a UNIT-SPACE .glb.
#
#   blender --background --factory-startup --python tools\BuildFloorGrate.py -- <out.glb>
#   AssetBaker import-model <out.glb> <assets> floor_grate --raw --texture-set rusted_iron
#
# JUST THE BARS. The hole they cover is a FLOOR FEATURE (floorfeatures.cat
# `recess`, tools/BuildFloorRecess.py), which replaces the cell's floor block and
# so wears that cell's floor texture. The bars cannot live in that mesh — it
# rides the floor's variant bucket and would draw them in stone — so a grate is
# the two composed: a recess carved into the floor, iron laid into its mouth.
# That split is the compose rule in CLAUDE.md, and it is also why the bars are
# free to be rusted_iron on any floor in the game.
#
# The first version of this prop was a squashed box sitting ON the floor, which
# is what you get if you try to fake a hole: the floor is a displaced grid and
# nothing below y = 0 is visible, so a flush grate is impossible without cutting
# the floor. Michael rejected it on sight. The recess is the honest fix.
#
# EVERYTHING HERE IS IN UNITS: 1.0 = one dungeon square (game::kUnit, 2.5 m),
# Z up — the exporter's +Y-Up conversion turns Blender Z into the engine's Y.
# The bars hang just below z = 0, so they sit DOWN IN the mouth rather than
# proud of the floor around it.
# ============================================================================
import sys

import bmesh
import bpy

MOUTH = 0.20             # the recess opening's half-width — must match BuildFloorRecess
OVERHANG = 0.008         # bars run into the well wall so no end-gap shows
BAR_Z1 = -0.012          # top of the bars: 3 cm below the floor, set into the mouth
BAR_Z0 = -0.024          # bottom of the bars
BAR_HW = 0.011           # half-width of one bar — 5.5 cm
BARS = 5

BEVEL = 0.0025   # edge break, so torchlight catches the arrises
SEGMENTS = 2
TILE = 0.16      # texture repeat (0.40 m/tile) — iron at a small prop's scale

OUT = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else "floor_grate.glb"

# --- empty the factory scene ------------------------------------------------
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

bm = bmesh.new()


def add_box(x0, x1, y0, y1, z0, z1):
    """An axis-aligned box with OUTWARD winding, stated corner by corner.

    Not left to recalc_face_normals: that operator needs CONNECTIVITY, and these
    bars are disjoint islands, so it would flip them arbitrarily. Winding is the
    contract (CLAUDE.md, and [[blender-mesh-normals-trap]]).
    """
    v = [bm.verts.new(p) for p in (
        (x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
        (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1),
    )]
    for face in ((0, 3, 2, 1),   # bottom (-Z)
                 (4, 5, 6, 7),   # top (+Z)
                 (0, 1, 5, 4),   # -Y
                 (2, 3, 7, 6),   # +Y
                 (1, 2, 6, 5),   # +X
                 (3, 0, 4, 7)):  # -X
        bm.faces.new([v[i] for i in face])


# Bars run along X, spaced across Y, ends buried in the well wall.
span = MOUTH - BAR_HW
for i in range(BARS):
    cy = -span + 2.0 * span * i / (BARS - 1)
    add_box(-MOUTH - OVERHANG, MOUTH + OVERHANG,
            cy - BAR_HW, cy + BAR_HW, BAR_Z0, BAR_Z1)

# --- break the edges --------------------------------------------------------
if BEVEL > 0.0:
    bmesh.ops.bevel(
        bm,
        geom=bm.verts[:] + bm.edges[:] + bm.faces[:],
        offset=BEVEL,
        offset_type="OFFSET",
        segments=SEGMENTS,
        profile=0.5,
        affect="EDGES",
        clamp_overlap=True,
    )

# --- world-aligned tiling UVs ----------------------------------------------
# Dominant-axis projection, valid here because every face is box-ish (the rule
# in CLAUDE.md: a swept or revolved surface would seam and wants unrolling).
uv = bm.loops.layers.uv.verify()
for face in bm.faces:
    nx, ny, nz = (abs(c) for c in face.normal)
    for loop in face.loops:
        co = loop.vert.co
        if nz >= nx and nz >= ny:   p = (co.x, co.y)  # up/down faces
        elif nx >= ny:              p = (co.y, co.z)  # x-facing
        else:                       p = (co.x, co.z)  # y-facing
        loop[uv].uv = (p[0] / TILE, p[1] / TILE)

mesh = bpy.data.meshes.new("floor_grate")
bm.to_mesh(mesh)
bm.free()

obj = bpy.data.objects.new("floor_grate", mesh)
bpy.context.scene.collection.objects.link(obj)

zs = [v.co.z for v in mesh.vertices]
wide = max(abs(v.co.x) for v in mesh.vertices)
print(f"BuildFloorGrate: {len(mesh.vertices)} verts, "
      f"z {min(zs):+.3f}..{max(zs):+.3f}, half-width {wide:.3f} (units)")
assert max(zs) < 0.0, "bars must hang below the floor plane, inside the mouth"

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
print(f"BuildFloorGrate: wrote {OUT}")
