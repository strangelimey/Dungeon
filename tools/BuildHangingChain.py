# ============================================================================
# tools/BuildHangingChain.py — authors the `hanging_chain` prop, UNIT-SPACE .glb.
#
#   blender --background --factory-startup --python tools\BuildHangingChain.py -- <out.glb>
#   AssetBaker import-model <out.glb> <assets> hanging_chain --raw --texture-set rusted_iron
#
# A chain hanging from the ceiling, with a ring plate where it is fixed and a
# hook at the bottom. A DECORATION authored high — see BuildCeilingBeam's header
# for why that needs no engine support.
#
# LINKS ARE BOXES, NOT TORI. A real chain link is a torus, and a torus needs a
# ring of rings — at 12 x 8 segments that is ~100 verts a link and ~1400 for the
# chain, for something a couple of centimetres across in a dark room. Each link
# here is instead a thin rectangular loop of four boxes, alternating flat and
# upright the way a real chain does, which reads identically at any distance the
# player can get to and costs a tenth as much. The alternation is the whole tell:
# a chain drawn all in one plane looks like a ladder.
#
# EVERYTHING IS IN UNITS: 1.0 = one dungeon square (2.5 m), Z up — Blender Z
# becomes the engine's Y, so z = 1.0 is the ceiling.
# ============================================================================
import sys

import bmesh
import bpy

CEIL = 1.0          # kWallH — the ceiling plane
PLATE_HW = 0.045    # the fixing plate at the top
PLATE_T = 0.012
LINKS = 9           # how many links hang
LINK_H = 0.055      # vertical pitch of one link
LINK_HW = 0.024     # half-width of a link's loop
WIRE = 0.006        # half-thickness of the metal itself
HOOK_R = 0.030      # the hook's bend radius
HOOK_SEG = 7

BEVEL = 0.002
SEGMENTS = 1
TILE = 0.12         # a small repeat — iron at this scale

OUT = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else "hanging_chain.glb"

# --- empty the factory scene ------------------------------------------------
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

bm = bmesh.new()


def add_box(x0, x1, y0, y1, z0, z1):
    """Outward winding, stated corner by corner (the normals trap in CLAUDE.md:
    recalc_face_normals needs connectivity and these are disjoint islands)."""
    v = [bm.verts.new(p) for p in (
        (x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
        (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1),
    )]
    for face in ((0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
                 (2, 3, 7, 6), (1, 2, 6, 5), (3, 0, 4, 7)):
        bm.faces.new([v[i] for i in face])


def add_link(z_top, upright):
    """One link: a rectangular loop of four bars, lying in XZ or YZ.

    `upright` swaps the plane, so consecutive links sit at right angles — which
    is what stops a chain reading as a ladder.
    """
    z0, z1 = z_top - LINK_H, z_top
    if upright:  # loop in the YZ plane
        add_box(-WIRE, WIRE, -LINK_HW, -LINK_HW + 2 * WIRE, z0, z1)  # side
        add_box(-WIRE, WIRE, LINK_HW - 2 * WIRE, LINK_HW, z0, z1)    # side
        add_box(-WIRE, WIRE, -LINK_HW, LINK_HW, z1 - 2 * WIRE, z1)   # top bar
        add_box(-WIRE, WIRE, -LINK_HW, LINK_HW, z0, z0 + 2 * WIRE)   # bottom bar
    else:        # loop in the XZ plane
        add_box(-LINK_HW, -LINK_HW + 2 * WIRE, -WIRE, WIRE, z0, z1)
        add_box(LINK_HW - 2 * WIRE, LINK_HW, -WIRE, WIRE, z0, z1)
        add_box(-LINK_HW, LINK_HW, -WIRE, WIRE, z1 - 2 * WIRE, z1)
        add_box(-LINK_HW, LINK_HW, -WIRE, WIRE, z0, z0 + 2 * WIRE)


# The fixing plate, flush under the ceiling.
add_box(-PLATE_HW, PLATE_HW, -PLATE_HW, PLATE_HW, CEIL - PLATE_T, CEIL)

# The links. They overlap by one wire thickness so the chain reads as joined.
z = CEIL - PLATE_T
for i in range(LINKS):
    add_link(z, upright=(i % 2 == 1))
    z -= LINK_H - 2 * WIRE

# A hook at the bottom: a C of short bars swept through 220 degrees, which is
# enough of a curve to read as a hook without a real sweep primitive.
import math  # noqa: E402  (local to the hook, kept beside its use)
cz = z - HOOK_R
for i in range(HOOK_SEG):
    a = math.radians(-20.0 + 220.0 * i / (HOOK_SEG - 1))
    px = HOOK_R * math.sin(a)
    pz = cz + HOOK_R * math.cos(a)
    add_box(px - WIRE, px + WIRE, -WIRE, WIRE, pz - WIRE, pz + WIRE)

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
uv = bm.loops.layers.uv.verify()
for face in bm.faces:
    nx, ny, nz = (abs(c) for c in face.normal)
    for loop in face.loops:
        co = loop.vert.co
        if nz >= nx and nz >= ny:   p = (co.x, co.y)
        elif nx >= ny:              p = (co.y, co.z)
        else:                       p = (co.x, co.z)
        loop[uv].uv = (p[0] / TILE, p[1] / TILE)

mesh = bpy.data.meshes.new("hanging_chain")
bm.to_mesh(mesh)
bm.free()

obj = bpy.data.objects.new("hanging_chain", mesh)
bpy.context.scene.collection.objects.link(obj)

zs = [v.co.z for v in mesh.vertices]
print(f"BuildHangingChain: {len(mesh.vertices)} verts, "
      f"z {min(zs):+.3f}..{max(zs):+.3f} (units)")
assert max(zs) <= CEIL + 1e-6, "the chain pokes through the ceiling"
# It must hang in the room, not end up at head height where the party walks.
assert min(zs) > 0.40, "the chain hangs into head height"

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
print(f"BuildHangingChain: wrote {OUT}")
