# ============================================================================
# tools/BuildCeilingBeam.py — authors the `ceiling_beam` prop, a UNIT-SPACE .glb.
#
#   blender --background --factory-startup --python tools\BuildCeilingBeam.py -- <out.glb>
#   AssetBaker import-model <out.glb> <assets> ceiling_beam --raw --texture-set wood_planks
#
# A squared timber spanning the cell just under the ceiling, with a corbel block
# at each end where it beds into the wall.
#
# A DECORATION, NOT A FEATURE, and the distinction is the one CLAUDE.md draws: a
# feature IS the surface (and so must wear the cell's texture), whereas a beam is
# an object that happens to be up there — it is timber against stone, varies
# independently of what it hangs under, and wants its own material. Composing it
# is also what lets one beam model serve all 54 ceiling types.
#
# IT NEEDS NO ENGINE SUPPORT, which is worth knowing before anyone reaches for a
# `mount = ceiling` field. A decoration's transform is a translation to the cell
# centre at y = 0 and nothing else, so the model's OWN geometry decides its
# height; `import-model --raw` preserves that (the floor recess keeps its -4.0
# the same way). A beam authored up at kWallH therefore simply hangs there.
#
# EVERYTHING IS IN UNITS: 1.0 = one dungeon square (2.5 m), Z up — and Blender Z
# becomes the engine's Y, so z = 1.0 here is the ceiling.
# ============================================================================
import sys

import bmesh
import bpy

HALF = 0.5        # the cell
CEIL = 1.0        # kWallH — the ceiling plane
BEAM_HW = 0.055   # half-thickness of the timber — 27 cm square-ish
BEAM_DROP = 0.02  # the beam's top sits this far below the ceiling
CORBEL_HW = 0.085 # the bedding block at each end
CORBEL_LEN = 0.09 # how far it projects from the wall
CORBEL_DROP = 0.11

BEVEL = 0.004     # a chamfer, so torchlight catches the arrises
SEGMENTS = 2

# UV tile rates. TWO of them, and which axis gets which is the whole point — see
# the mapping block near the bottom for why one number could not do this.
TILE_ACROSS = 2.2  # u: one plank (1/20 of a repeat) spans the beam's 0.11 face
TILE_ALONG = 1.1   # v: half of ACROSS, keeping texels square on a 2:1 scan

OUT = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else "ceiling_beam.glb"

# --- empty the factory scene ------------------------------------------------
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

bm = bmesh.new()


def add_box(x0, x1, y0, y1, z0, z1):
    """An axis-aligned box with OUTWARD winding, stated corner by corner.

    Not left to recalc_face_normals: these are disjoint islands and that operator
    needs connectivity, so it would flip them arbitrarily (CLAUDE.md's trap).
    """
    v = [bm.verts.new(p) for p in (
        (x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
        (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1),
    )]
    for face in ((0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
                 (2, 3, 7, 6), (1, 2, 6, 5), (3, 0, 4, 7)):
        bm.faces.new([v[i] for i in face])


# The beam, running along X wall to wall. It spans the FULL cell so a run of
# them in adjacent cells reads as one continuous timber.
top = CEIL - BEAM_DROP
add_box(-HALF, HALF, -BEAM_HW, BEAM_HW, top - 2.0 * BEAM_HW, top)

# A corbel at each end, bedding the beam into the wall.
for sx in (-1.0, 1.0):
    x_out = sx * HALF
    x_in = sx * (HALF - CORBEL_LEN)
    add_box(min(x_out, x_in), max(x_out, x_in), -CORBEL_HW, CORBEL_HW,
            top - CORBEL_DROP, top)

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

# --- UVs: the grain has to run ALONG the timber -----------------------------
# Still dominant-axis projection, valid here because every face is box-ish (the
# CLAUDE.md rule: a swept or revolved surface would seam and wants unrolling
# instead). But that rule only governs whether a projection SEAMS. It says
# nothing about ORIENTATION, and wood is not isotropic.
#
# THE BUG THIS FIXES, in Michael's words: "the texture looks like it's made out
# of vertical matchsticks, side by side". wood_planks runs its planks along the
# image's V, about twenty across its width. The old mapping put the beam's
# LENGTH on u at a single 0.30 tile, so the plank direction lay ACROSS the
# timber and one square carried 3.33 repeats x 20 planks — 67 stripes.
#
# So: length -> v, section -> u, and two tile rates rather than one. They sit in
# 2:1 because the scan is (1024x512) and equal texel density wants the world
# spans in the same ratio; ACROSS is sized so ONE plank covers the beam's face,
# which is what makes it read as a single piece of timber rather than a bundle.
uv = bm.loops.layers.uv.verify()
for face in bm.faces:
    nx, ny, nz = (abs(c) for c in face.normal)
    for loop in face.loops:
        co = loop.vert.co
        if nx >= ny and nx >= nz:
            # An END of the timber. The section IS the face, so both axes cross
            # the grain; only ever seen where a beam is cut, and tiny.
            across, along = co.y, co.z
        elif nz >= ny:
            across, along = co.y, co.x  # top and bottom
        else:
            across, along = co.z, co.x  # the two sides
        loop[uv].uv = (across / TILE_ACROSS, along / TILE_ALONG)

mesh = bpy.data.meshes.new("ceiling_beam")
bm.to_mesh(mesh)
bm.free()

obj = bpy.data.objects.new("ceiling_beam", mesh)
bpy.context.scene.collection.objects.link(obj)

zs = [v.co.z for v in mesh.vertices]
xs = [v.co.x for v in mesh.vertices]
print(f"BuildCeilingBeam: {len(mesh.vertices)} verts, "
      f"x {min(xs):+.3f}..{max(xs):+.3f}, z {min(zs):+.3f}..{max(zs):+.3f} (units)")
assert max(zs) <= CEIL + 1e-6, "the beam pokes through the ceiling"
assert min(zs) > CEIL * 0.75, "the beam hangs too low to read as a ceiling timber"

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
print(f"BuildCeilingBeam: wrote {OUT}")
