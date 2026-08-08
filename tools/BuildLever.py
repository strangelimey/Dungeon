# ============================================================================
# tools/BuildLever.py — authors the wall lever (the `button` prop), in UNITS.
#
#   blender --background --factory-startup --python tools\BuildLever.py -- <out.glb> <plate|handle>
#   AssetBaker import-model <out.glb> <assets> lever_<part> --raw --texture-set rusted_iron
#
#   live preview (BOTH parts assembled):  python tools\bsend.py -f tools\BuildLever.py
#
# Replaces ModelBaker's BuildLever(), which was four axis-aligned boxes.
#
# IT IS TWO MESHES, AND THAT IS THE POINT. The render tilts the button prop
# around X by `activated` (DungeonWorld_Render), and the old model was a single
# mesh — so the BACK PLATE tilted too, rocking in and out of the wall. It only
# got away with it by being thin. A lever's plate is bolted to the stone and
# never moves; only the handle swings. So the plate and the handle are separate
# models, drawn with different matrices, exactly the way a door already draws
# its static `frame` and its sliding `panel`.
#
# THE PIVOT IS THE ORIGIN, which the old mesh also got wrong. XMMatrixRotationX
# rotates about the X axis THROUGH THE ORIGIN, and the old pivot boss sat at
# z = 0.055 — so the handle orbited a point 5.5 cm behind itself instead of
# turning on its own pin. Here the pin IS (0, 0, 0): the hub's back half is
# inside the wall, which the wall hides, and the handle turns truly.
#
# AUTHORED +Z INTO THE ROOM, origin on the wall face at hand height — the sconce
# convention. DungeonWorld::MountOnWall supplies the position and yaw.
#
# Authored in METRES and divided by kUnit at the end (the BuildStatue pattern):
# ironmongery is easier to reason about at real size. Z is up here (Blender).
# ============================================================================
import math
import sys

import bmesh
import bpy

KUNIT = 2.5
SMOOTH_ANGLE = math.radians(38.0)

# --- the plate, bolted to the wall ------------------------------------------
PLATE_W, PLATE_H = 0.072, 0.120     # half-extents of the back plate
PLATE_T = 0.014                     # how far it stands off the stone
INNER_W, INNER_H, INNER_T = 0.056, 0.100, 0.022
CHEEK_IN, CHEEK_OUT = 0.030, 0.046  # the bracket that carries the pin
CHEEK_H, CHEEK_Z = 0.030, 0.046

# --- the handle, turning on the pin -----------------------------------------
HUB_R, HUB_HALF = 0.026, 0.030      # the hub between the cheeks
SHAFT_Z0, SHAFT_Z1 = 0.020, 0.196
SHAFT_R0, SHAFT_R1 = 0.020, 0.014
GRIP_Z, GRIP_R = 0.212, 0.030

args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
EXPORT = bool(args)
OUT = args[0] if args else "lever.glb"
# Headless builds ONE part, because each is its own .gltf. The live preview
# builds both, because what needs judging is the assembled lever.
PART = args[1] if len(args) > 1 else globals().get("LEVER_PART", None)

if bpy.context.mode != "OBJECT":
    bpy.ops.object.mode_set(mode="OBJECT")
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

bm = bmesh.new()
HARD = set()   # flat-shaded: plate and bracket are cut ironwork, not turned


def stack(rings, close_bottom=False, close_top=False, flip=False):
    """Winding derived for rings advancing along +axis with the section ordered
    counter-clockwise about it — the BuildStatue derivation, and checked by the
    outward assert at the end."""
    for i in range(len(rings) - 1):
        r0, r1 = rings[i], rings[i + 1]
        n = len(r0)
        for j in range(n):
            k = (j + 1) % n
            quad = (r0[j], r1[j], r1[k], r0[k]) if flip else (r0[j], r0[k], r1[k], r1[j])
            bm.faces.new(quad)
    if close_bottom:
        fan(rings[0], up=flip)
    if close_top:
        fan(rings[-1], up=not flip)


def fan(ring, up):
    c = bm.verts.new((sum(v.co.x for v in ring) / len(ring),
                      sum(v.co.y for v in ring) / len(ring),
                      sum(v.co.z for v in ring) / len(ring)))
    for j in range(len(ring)):
        k = (j + 1) % len(ring)
        bm.faces.new((c, ring[j], ring[k]) if up else (c, ring[k], ring[j]))


def box(x0, x1, y0, y1, z0, z1):
    """Always HARD. A box has no curved surface, so every face is flat.

    BOUNDS ARE SORTED, and that is not tidiness. The face list below is written
    for x0 < x1, so a caller passing them the other way round gets the whole box
    wound INSIDE OUT — which is exactly what happened to the mirrored bracket
    cheek: `box(side * CHEEK_IN, side * CHEEK_OUT, ...)` is ordered for
    side = +1 and reversed for side = -1. Mirroring by negating a coordinate is
    the natural way to write a symmetric prop, so the helper has to survive it.
    """
    x0, x1 = min(x0, x1), max(x0, x1)
    y0, y1 = min(y0, y1), max(y0, y1)
    z0, z1 = min(z0, z1), max(z0, z1)
    v = [bm.verts.new(p) for p in (
        (x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
        (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1))]
    for f in ((0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
              (2, 3, 7, 6), (1, 2, 6, 5), (3, 0, 4, 7)):
        HARD.add(bm.faces.new([v[i] for i in f]))


def tube_z(stations, seg=16):
    """A body of revolution about the Z axis. stations: (z, radius)."""
    rings = [[bm.verts.new((r * math.cos(2 * math.pi * j / seg),
                            r * math.sin(2 * math.pi * j / seg), z))
              for j in range(seg)] for (z, r) in stations]
    stack(rings, close_bottom=True, close_top=True)


def tube_x(stations, cy, cz, seg=16):
    """A body of revolution about the X axis — the hub turns on this axis, and
    the generic Z helper cannot express it. stations: (x, radius)."""
    rings = [[bm.verts.new((x, cy + r * math.cos(2 * math.pi * j / seg),
                            cz + r * math.sin(2 * math.pi * j / seg)))
              for j in range(seg)] for (x, r) in stations]
    # Advancing along +X with the section counter-clockwise about it reverses
    # the handedness relative to the Z case, so the winding flips.
    stack(rings, close_bottom=True, close_top=True, flip=True)


want_plate = PART in (None, "plate")
want_handle = PART in (None, "handle")

if want_plate:
    # A stepped escutcheon: a broad back plate against the stone and a raised
    # inner plate on it, which is what gives the silhouette a shadow line
    # instead of reading as one flat slab.
    box(-PLATE_W, PLATE_W, -PLATE_H, PLATE_H, 0.0, PLATE_T)
    box(-INNER_W, INNER_W, -INNER_H, INNER_H, PLATE_T, INNER_T)
    # Two cheeks carrying the pin. They flank the hub in X, which is the axis
    # the handle turns on — so they read as the thing holding it up.
    for side in (-1.0, 1.0):
        box(side * CHEEK_IN, side * CHEEK_OUT, -CHEEK_H, CHEEK_H,
            INNER_T, CHEEK_Z)

if want_handle:
    # The hub, on the pin, centred on the ORIGIN — see the header. Its back half
    # is inside the wall and the wall hides it; that is the price of the pivot
    # being where the rotation actually happens.
    tube_x([(-HUB_HALF, 0.018), (-HUB_HALF + 0.006, HUB_R),
            (HUB_HALF - 0.006, HUB_R), (HUB_HALF, 0.018)], cy=0.0, cz=0.0)
    # The shaft, tapering out into the room.
    tube_z([(SHAFT_Z0, SHAFT_R0), (SHAFT_Z1, SHAFT_R1)], seg=14)
    # A ball grip, slightly ovoid so it does not read as a bead on a stick.
    tube_z([(GRIP_Z - GRIP_R * 0.95, 0.013),
            (GRIP_Z - GRIP_R * 0.55, GRIP_R * 0.80),
            (GRIP_Z, GRIP_R),
            (GRIP_Z + GRIP_R * 0.55, GRIP_R * 0.78),
            (GRIP_Z + GRIP_R * 0.95, 0.011)], seg=16)

# --- Blender axes to engine axes --------------------------------------------
# THE CONVERSION THAT BIT. glTF export turns Blender +Z into the engine's UP and
# Blender +Y into the engine's -Z. This file authors in the frame that is
# natural for a WALL prop — z out of the wall, y up the wall — because that is
# how the mounting contract reads. Without this rotation "out of the wall"
# exports as "up", and the lever renders as a shelf sticking out of the stone
# with the handle standing on it, which is exactly how the first build looked.
#
# (x, y, z) -> (x, -z, y) is a quarter turn about X: a PROPER rotation, so the
# winding survives it and the outward assert below still means something. The
# pin stays on X, which is the axis the render tilts about.
#
# BuildStatue.py does not need this only because it authors z as up already.
for v in bm.verts:
    x, y, z = v.co.x, v.co.y, v.co.z
    v.co.x, v.co.y, v.co.z = x, -z, y

# --- shading ----------------------------------------------------------------
for f in bm.faces:
    f.smooth = f not in HARD
sharp = [e for e in bm.edges
         if len(e.link_faces) == 2
         and (e.calc_face_angle(0.0) > SMOOTH_ANGLE
              or any(f in HARD for f in e.link_faces))]
bmesh.ops.split_edges(bm, edges=sharp)
bm.normal_update()

mesh = bpy.data.meshes.new("lever")
bm.to_mesh(mesh)
bm.free()
for v in mesh.vertices:
    v.co /= KUNIT

obj = bpy.data.objects.new("lever", mesh)
bpy.context.scene.collection.objects.link(obj)

# World-aligned tiling UVs; iron is isotropic, so only the rate matters.
bm2 = bmesh.new()
bm2.from_mesh(mesh)
uv = bm2.loops.layers.uv.verify()
TILE = 0.10
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
print(f"BuildLever[{PART or 'both'}]: {len(mesh.vertices)} verts, "
      f"{len(mesh.polygons)} faces, out {min(out):+.4f}..{max(out):+.4f}, "
      f"up {min(up):+.4f}..{max(up):+.4f} units")

# THE MOUNTING CONTRACT, checked rather than trusted. Nothing may sit behind the
# wall face except the hub's buried half, and the prop must stay inside a
# quarter square so a lever never pokes through the wall it hangs on. These
# assert on the ENGINE axes deliberately: an axis mix-up is precisely the bug
# they exist to catch, so checking the authoring frame would have missed it.
assert min(out) > -HUB_R / KUNIT - 1e-6, "geometry behind the wall face"
assert max(out) < 0.25, "the lever juts more than a quarter square into the room"
assert max(abs(min(up)), max(up)) < 0.25, "the plate is taller than half a square"

# WINDING, checked rather than trusted — an inverted island reads as a shading
# fault, not as a normals bug, and a mirrored box() call inverted a bracket
# cheek exactly that way. Everything here is convex about the prop's own axis,
# which after the rotation above is Y, so a majority of faces must point away
# from the line x = z = 0.
outward = 0
for poly in mesh.polygons:
    c = poly.center
    rl = math.hypot(c.x, c.z)
    if rl < 1e-4 or abs(poly.normal.y) > 0.85:
        continue
    if (poly.normal.x * c.x + poly.normal.z * c.z) / rl > 0.0:
        outward += 1
    else:
        outward -= 1
assert outward > 0, f"an island is wound inside out (vote {outward})"

if EXPORT:
    bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
    print(f"BuildLever[{PART}]: wrote {OUT}")
else:
    print("BuildLever: built in the bridge, not exported")
