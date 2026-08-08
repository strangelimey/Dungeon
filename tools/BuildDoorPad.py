# ============================================================================
# tools/BuildDoorPad.py — authors Dungeon Master's jamb pad, UNIT-SPACE .glb.
#
#   blender --background --factory-startup --python tools\BuildDoorPad.py -- <out.glb> <pad|mount>
#   AssetBaker import-model <out.glb> <assets> door_pad       --raw --texture-set wood_planks_old8
#   AssetBaker import-model <out.glb> <assets> door_pad_mount --raw --texture-set sconce
#
#   live preview (BOTH parts assembled): python tools\bsend.py -f tools\BuildDoorPad.py
#
# Replaces ModelBaker's BuildDoorPad/BuildDoorPadMount, which were two boxes.
#
# IT IS TWO MESHES, AND THAT IS THE POINT — twice over. The render sinks the pad
# along the wall normal and leaves the mount alone, because a button whose frame
# goes in with it reads as the whole fitting being shoved into the stone rather
# than as something being pressed. And the two are different MATERIALS: the pad
# is the worn wooden square a hand actually hits, the mount the iron surround
# bolted to the jamb, and the import path binds one texture set per model.
#
# THAT SPLIT DECIDES WHERE THE STUDS GO. A studded wooden pad is the obvious
# picture, but a stud is iron and would have to live in the wood-textured mesh —
# the same trap the wooden door's straps were pulled out of. So the bolts belong
# to the MOUNT, where they are also more honest: they are what fixes the fitting
# to the stone, and they are the part that does not move.
#
# THE TRAVEL IS BOUNDED BY THE GEOMETRY. DungeonWorld.h kPadPress is 1.75 cm, so
# the pad's face has to stand further than that above the surround or a press
# swallows it. It stands 2.4 cm, which leaves a 6.5 mm lip at full travel —
# asserted below, because the first version pressed 3.5 cm (deeper than the whole
# fitting) and the button vanished into the jamb instead of being pushed.
#
# AUTHORED +Z OUT OF THE WALL, origin on the wall face at hand height — the
# sconce convention, and BuildLever's frame exactly, down to the quarter turn
# about X at the end that turns it into the engine's axes. Metres here and
# divided by kUnit at the end (the BuildLever/BuildStatue pattern): ironmongery
# is easier to reason about at real size.
# ============================================================================
import math
import sys

import bmesh
import bpy

KUNIT = 2.5
PRESS = 0.0175          # DungeonWorld.h kPadPress, in metres

# --- the mount: an iron surround bolted to the jamb -------------------------
PLATE_HALF = 0.115      # the back plate, flat against the stone
PLATE_T = 0.012
RIM_IN = 0.086          # the opening the pad sits in
RIM_Z = 0.050           # how far the rim stands off the stone
BOLT_AT, BOLT_R = 0.100, 0.014

# --- the pad: the wooden square a hand hits ---------------------------------
# 4 mm narrower than the rim's opening all round, which is clearance for the
# press and also the shadow line that says the thing is loose in its frame.
PAD_HALF = 0.082
PAD_Z0 = 0.008          # its back sits behind the rim's front, so a pressed pad
                        # is still captured rather than floating in a hole
PAD_SHOULDER = 0.058    # where the face starts to chamfer
PAD_Z1 = 0.074          # the face — 2.4 cm proud of the rim
PAD_FACE_HALF = 0.066   # the chamfer's top, so the pad is not a flat-topped box

SMOOTH_ANGLE = math.radians(38.0)

args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
EXPORT = bool(args)
OUT = args[0] if args else "door_pad.glb"
# Headless builds ONE part, because each is its own .gltf. The live preview
# builds both, because what needs judging is the assembled fitting.
PART = args[1] if len(args) > 1 else globals().get("PAD_PART", None)
assert PART in (None, "pad", "mount"), f"unknown part {PART}"

if bpy.context.mode != "OBJECT":
    bpy.ops.object.mode_set(mode="OBJECT")
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

bm = bmesh.new()
HARD = set()            # flat-shaded: everything cut, nothing turned


def box(x0, x1, y0, y1, z0, z1):
    """BOUNDS ARE SORTED, and that is not tidiness — the face list below is
    written for x0 < x1, so a caller passing them the other way round gets the
    box wound inside out. Mirroring by negating a coordinate is the natural way
    to write a symmetric prop, so the helper has to survive it. (BuildLever
    learned this on a bracket cheek.)"""
    x0, x1 = min(x0, x1), max(x0, x1)
    y0, y1 = min(y0, y1), max(y0, y1)
    z0, z1 = min(z0, z1), max(z0, z1)
    v = [bm.verts.new(p) for p in (
        (x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
        (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1))]
    for f in ((0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
              (2, 3, 7, 6), (1, 2, 6, 5), (3, 0, 4, 7)):
        HARD.add(bm.faces.new([v[i] for i in f]))


def rect_loft(stations):
    """A closed solid from rectangles stacked along +z: [(z, half_x, half_y)].

    ONE SOLID RATHER THAN A STACK OF BOXES, which is what makes the chamfer
    safe: two boxes meeting face to face put two coplanar faces in the same
    place and z-fight, and pushing them into each other to avoid that leaves a
    step where a chamfer should be continuous."""
    rings = [[bm.verts.new(p) for p in ((-hx, -hy, z), (hx, -hy, z),
                                        (hx, hy, z), (-hx, hy, z))]
             for (z, hx, hy) in stations]
    for i in range(len(rings) - 1):
        r0, r1 = rings[i], rings[i + 1]
        for j in range(4):
            k = (j + 1) % 4
            HARD.add(bm.faces.new((r0[j], r0[k], r1[k], r1[j])))
    HARD.add(bm.faces.new(rings[0][::-1]))
    HARD.add(bm.faces.new(rings[-1]))


def dome(cx, cy, cz, r, squash):
    """A bolt head: a sphere squashed along z so it swells out of the surface
    instead of resting on it as a ball."""
    verts = bmesh.ops.create_uvsphere(bm, u_segments=10, v_segments=6,
                                      radius=r)["verts"]
    for v in verts:
        v.co = (cx + v.co.x, cy + v.co.y, cz + v.co.z * squash)


want_pad = PART in (None, "pad")
want_mount = PART in (None, "mount")

if want_mount:
    # The back plate, then a rim standing off it with the pad's opening through
    # the middle. The rim is four bars rather than a frame with a hole cut in
    # it: the top and bottom run the full width and the sides span only BETWEEN
    # them, so no two of them share a face.
    box(-PLATE_HALF, PLATE_HALF, -PLATE_HALF, PLATE_HALF, 0.0, PLATE_T)
    box(-PLATE_HALF, PLATE_HALF, RIM_IN, PLATE_HALF, PLATE_T, RIM_Z)
    box(-PLATE_HALF, PLATE_HALF, -RIM_IN, -PLATE_HALF, PLATE_T, RIM_Z)
    box(RIM_IN, PLATE_HALF, -RIM_IN, RIM_IN, PLATE_T, RIM_Z)
    box(-RIM_IN, -PLATE_HALF, -RIM_IN, RIM_IN, PLATE_T, RIM_Z)
    for sx in (-1.0, 1.0):
        for sy in (-1.0, 1.0):
            dome(sx * BOLT_AT, sy * BOLT_AT, RIM_Z, BOLT_R, 0.5)

if want_pad:
    rect_loft([(PAD_Z0, PAD_HALF, PAD_HALF),
               (PAD_SHOULDER, PAD_HALF, PAD_HALF),
               (PAD_Z1, PAD_FACE_HALF, PAD_FACE_HALF)])

# --- Blender axes to engine axes --------------------------------------------
# (x, y, z) -> (x, -z, y) is a quarter turn about X: a PROPER rotation, so the
# winding survives it. Without it "out of the wall" exports as "up" and the
# fitting renders as a shelf sticking out of the stone — which is exactly how
# BuildLever's first build looked.
for v in bm.verts:
    x, y, z = v.co.x, v.co.y, v.co.z
    v.co.x, v.co.y, v.co.z = x, -z, y

for f in bm.faces:
    f.smooth = f not in HARD
sharp = [e for e in bm.edges
         if len(e.link_faces) == 2
         and (e.calc_face_angle(0.0) > SMOOTH_ANGLE
              or any(f in HARD for f in e.link_faces))]
bmesh.ops.split_edges(bm, edges=sharp)
bm.normal_update()

NAME = f"door_pad{'' if PART != 'mount' else '_mount'}"
mesh = bpy.data.meshes.new(NAME)
bm.to_mesh(mesh)
bm.free()
for v in mesh.vertices:
    v.co /= KUNIT

obj = bpy.data.objects.new(NAME, mesh)
bpy.context.scene.collection.objects.link(obj)

# World-aligned tiling UVs. The rates differ by material and both are read off
# the SCAN, not chosen for the prop: `sconce` is isotropic worn metal so only
# the rate matters, but `wood_planks_old8` is ten boards across its tile, so a
# 16 cm pad wants a tile that puts ONE board across it — anything finer and the
# pad is striped like a tiny fence, which is the mistake the door leaf made.
TILE = (0.12 if PART == "mount" else 0.64)
bm2 = bmesh.new()
bm2.from_mesh(mesh)
uv = bm2.loops.layers.uv.verify()
for face in bm2.faces:
    nx, ny, nz = (abs(c) for c in face.normal)
    for loop in face.loops:
        co = loop.vert.co
        if nz >= nx and nz >= ny:   p = (co.x, co.y)
        elif nx >= ny:              p = (co.y, co.z)
        else:                       p = (co.x, co.z)
        loop[uv].uv = (p[0] / TILE, p[1] / TILE)
bm2.to_mesh(mesh)
bm2.free()

# Everything below is in ENGINE axes: -Y is out of the wall, +Z is up it.
out = [-v.co.y for v in mesh.vertices]
up = [v.co.z for v in mesh.vertices]
across = [v.co.x for v in mesh.vertices]
print(f"BuildDoorPad[{PART or 'both'}]: {len(mesh.vertices)} verts, "
      f"{len(mesh.polygons)} faces, out {min(out):+.4f}..{max(out):+.4f}, "
      f"up {min(up):+.4f}..{max(up):+.4f} units")

# THE MOUNTING CONTRACT, checked rather than trusted, and on the ENGINE axes
# deliberately: an axis mix-up is precisely the bug these exist to catch, so
# checking the authoring frame would have missed it.
assert min(out) > -1e-6, "geometry behind the wall face"
assert max(out) < 0.25, "the fitting juts more than a quarter square into the room"
assert max(abs(min(up)), max(up)) < 0.25, "the fitting is taller than half a square"
assert max(abs(min(across)), max(across)) < 0.25, "the fitting is wider than half a square"

# THE PRESS, which is the whole reason this is two models. At full travel the
# pad must still stand proud of the rim, or the press reads as the button
# falling into a hole rather than being pushed.
if PART != "mount":
    lip = (PAD_Z1 - PRESS) - RIM_Z
    assert lip > 0.004, (
        f"a {PRESS * 100:.1f} cm press leaves a {lip * 100:.1f} cm lip — the pad "
        f"sinks into its own surround")

if EXPORT:
    bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
    print(f"BuildDoorPad: wrote {OUT}")
else:
    print("BuildDoorPad: built in the bridge, not exported")
