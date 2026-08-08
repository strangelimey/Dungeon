# ============================================================================
# tools/BuildHangingChain.py — authors every chain in the game, UNIT-SPACE .glb.
#
#   blender --background --factory-startup --python tools\BuildHangingChain.py -- <out.glb> [part]
#
#   ceiling  (default)  AssetBaker import-model ... hanging_chain      --raw --texture-set rusted_iron
#   chain               AssetBaker import-model ... door_chain         --raw --texture-set sconce
#   socket              AssetBaker import-model ... door_chain_socket  --raw --texture-set sconce
#
#   live preview: python tools\bsend.py -c "CHAIN_PART='chain'" -f tools\BuildHangingChain.py
#
# `ceiling` is a chain hanging from the ceiling: a round eye plate, interlocking
# links, and a hook at the bottom. A DECORATION authored high — see
# BuildCeilingBeam's header for why that needs no engine support.
#
# `chain` and `socket` are Grimrock's DOOR OPENER, replacing ModelBaker's box
# placeholders. They live here rather than in a script of their own because the
# only thing a door chain is, is a chain: the same stadium links interlocking the
# same way, and the sweep helper and the whole normals argument below apply
# unchanged. What differs is where it hangs from and how big the stock is.
#
# THE TWO PARTS ARE TWO MODELS BECAUSE ONE OF THEM MOVES. The render translates
# the chain down by kChainDrop when the door is worked and leaves the socket
# alone, which is what makes the chain read as being DRAWN OUT of its anchor
# instead of sliding bodily down the jamb. So the socket is a closed solid: the
# links inside it are hidden by depth, and the drop has to stay inside the length
# it conceals or the chain's top end walks out of the bottom of its own anchor.
#
# A DOOR OPENER IS A WALL PROP, and needs no rotation to become one. The render
# wants engine +Z out of the wall and +Y up; the exporter turns Blender +Z into
# engine +Y and Blender -Y into engine +Z, so authoring "up the wall" as Blender
# Z and "out of the wall" as Blender -Y — which is what a chain hanging down a
# wall already wants — lands correctly. BuildLever needs its quarter turn about X
# only because it authors z OUT of the wall for its own readability.
#
# LINKS ARE SWEPT TUBES, NOT BOXES. The first version built each link from four
# thin boxes on the argument that a real link costs ten times as much for
# something a couple of centimetres across in a dark room. Michael's verdict on
# seeing it: "they look too square and artificial." The argument was sound and
# the conclusion was wrong — a chain is one of the few props a player looks
# directly UP at from a metre away, and the silhouette is the whole read. At
# ~1800 verts the honest version is still a rounding error beside the 6268-vert
# pillar, so the saving was never worth what it cost.
#
# A LINK IS A STADIUM, NOT A TORUS, which is the other thing the box version got
# wrong in spirit. Real chain links are oblong — two straight flanks joined by
# semicircular ends — and that shape is what lets consecutive links INTERLOCK
# through each other rather than merely touch. Alternating the plane 90 degrees
# is the tell that stops a chain reading as a ladder; interlocking is the tell
# that stops it reading as a stack of rings.
#
# THE SWEEP FRAME IS CONSTANT, WHICH IS WHY THIS NEEDS NO PARALLEL TRANSPORT.
# Every path here is PLANAR, so the plane's normal serves as one section axis at
# every station and the frame cannot twist. A non-planar path would need real
# frame propagation; do not copy this helper to one without adding it.
#
# EVERYTHING IS IN UNITS: 1.0 = one dungeon square (2.5 m), Z up — Blender Z
# becomes the engine's Y, so z = 1.0 is the ceiling.
# ============================================================================
import math
import sys

import bmesh
import bpy

CEIL = 1.0          # kWallH — the ceiling plane

PLATE_R = 0.048     # the round fixing plate at the top
PLATE_T = 0.010
EYE_R = 0.026       # the suspension eye the first link hangs through
EYE_WIRE = 0.006

LINKS = 8            # 9 put the hook's point at z = 0.399, a millimetre under
                     # the head-height assert — the eye and hook cost length the
                     # box version's flat plate did not
WIRE = 0.005        # the metal's own radius — 2.5 cm thick stock
LINK_HALF_W = 0.018 # half the link's short axis (9 cm wide)
LINK_HALF_L = 0.038 # half its long axis (19 cm long)

HOOK_R = 0.032      # the hook's bend radius
HOOK_SHANK = 0.022

PATH_ARC = 8        # path stations per semicircular end
PATH_STRAIGHT = 2   # per straight flank
SECTION = 7         # segments around the wire

TILE = 0.16         # texture repeat — iron at this scale

# --- the door opener ---------------------------------------------------------
# Heights are measured from the opener's ORIGIN, which the render puts at hand
# height (DungeonWorld.h kOpenerY, derived from the eye and not from the wall).
# A chain authored downward from its anchor would need the anchor's height as a
# second number to keep in step with the frame; measuring up from the grip means
# the one height that matters is the one the render already owns.
CHAIN_OUT = -0.0152     # Blender -Y is out of the wall: 3.8 cm of standoff
RING_CROWN = 0.030      # where the grip ring's top sits, just above the origin
RING_R, RING_WIRE = 0.026, 0.005
# Smaller stock than the ceiling chain's: that one is looked up at from a metre
# away, this one is read across a room beside a 2.1 m door, and 19 cm links on it
# looked like ship's cable.
D_LINK_HALF_L, D_LINK_HALF_W, D_WIRE = 0.022, 0.011, 0.0035
# High on the jamb, so the visible run is long. LENGTH COMES FROM RAISING THE
# ANCHOR, not from dropping the ring: the ring is where the hand goes and that
# height is fixed, so hanging it lower to get more chain puts the grip around
# the party's knees. The top lands at 2.43 m of a 2.5 m room.
SOCKET_Z0, SOCKET_Z1 = 0.340, 0.412
SOCKET_R, SOCKET_MOUTH = 0.020, 0.026
CHAIN_DROP = 0.045      # DungeonWorld.h kChainDrop — asserted against below

args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = args[0] if args else "hanging_chain.glb"
PART = (args[1] if len(args) > 1 else globals().get("CHAIN_PART", "ceiling"))
assert PART in ("ceiling", "chain", "socket"), f"unknown part {PART}"

# --- empty the factory scene ------------------------------------------------
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

bm = bmesh.new()


def sweep(path, normal, radii, closed):
    """Sweep a circular section along `path`, returning the ring vertex lists.

    `normal` is the path's plane normal and doubles as one section axis, so the
    frame is constant and cannot twist (see the header). `radii` is per-station,
    which is what lets the hook taper to a point.

    WINDING IS STATED, NOT RECALCULATED. bmesh's recalc_face_normals needs
    connectivity to decide anything, and gets it wrong on the disjoint islands
    this file is full of (CLAUDE.md's trap). Derived once instead: with the
    section basis (B, N) where B = T x N, advancing the section angle moves
    toward +N and advancing the station moves along +T, so the quad
    (i,j) -> (i+1,j) -> (i+1,j+1) -> (i,j+1) has normal T x N = B — outward.
    Taking the stations in the other order would turn the whole tube inside out.
    """
    nx, ny, nz = normal
    rings = []
    for i, (px, py, pz) in enumerate(path):
        # Tangent by central difference, wrapping when the path is a loop.
        if closed:
            a, b = path[i - 1], path[(i + 1) % len(path)]
        else:
            a, b = path[max(i - 1, 0)], path[min(i + 1, len(path) - 1)]
        tx, ty, tz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
        tl = math.sqrt(tx * tx + ty * ty + tz * tz) or 1.0
        tx, ty, tz = tx / tl, ty / tl, tz / tl
        # B = T x N.
        bx = ty * nz - tz * ny
        by = tz * nx - tx * nz
        bz = tx * ny - ty * nx
        bl = math.sqrt(bx * bx + by * by + bz * bz) or 1.0
        bx, by, bz = bx / bl, by / bl, bz / bl

        r = radii[i]
        ring = []
        for j in range(SECTION):
            phi = 2.0 * math.pi * j / SECTION
            c, s = math.cos(phi) * r, math.sin(phi) * r
            ring.append(bm.verts.new((px + bx * c + nx * s,
                                      py + by * c + ny * s,
                                      pz + bz * c + nz * s)))
        rings.append(ring)

    last = len(rings) if closed else len(rings) - 1
    for i in range(last):
        r0, r1 = rings[i], rings[(i + 1) % len(rings)]
        for j in range(SECTION):
            k = (j + 1) % SECTION
            bm.faces.new((r0[j], r1[j], r1[k], r0[k]))
    return rings


def cap(ring, outward):
    """Close an open tube end with a fan. `outward` is the direction the cap
    faces, used only to pick the winding."""
    centre = bm.verts.new((sum(v.co.x for v in ring) / len(ring),
                           sum(v.co.y for v in ring) / len(ring),
                           sum(v.co.z for v in ring) / len(ring)))
    for j in range(len(ring)):
        k = (j + 1) % len(ring)
        tri = (centre, ring[j], ring[k]) if outward > 0 else (centre, ring[k], ring[j])
        bm.faces.new(tri)


def stadium(half_w, half_l, upright, centre_z):
    """A chain link's centreline: two straight flanks and two semicircular ends.

    `upright` swaps the plane between XZ and YZ so consecutive links sit at right
    angles — the thing that stops a chain reading as a ladder.
    """
    zc = half_l - half_w
    pts = []
    # Right flank, upward.
    for i in range(PATH_STRAIGHT):
        t = i / PATH_STRAIGHT
        pts.append((half_w, -zc + 2.0 * zc * t))
    # Top end.
    for i in range(PATH_ARC):
        a = math.pi * 0.5 * (1.0 - 2.0 * i / PATH_ARC)
        pts.append((half_w * math.sin(a), zc + half_w * math.cos(a)))
    # Left flank, downward.
    for i in range(PATH_STRAIGHT):
        t = i / PATH_STRAIGHT
        pts.append((-half_w, zc - 2.0 * zc * t))
    # Bottom end.
    for i in range(PATH_ARC):
        a = math.pi * 0.5 * (1.0 - 2.0 * i / PATH_ARC)
        pts.append((-half_w * math.sin(a), -zc - half_w * math.cos(a)))

    if upright:
        path = [(0.0, u, centre_z + v) for (u, v) in pts]
        normal = (1.0, 0.0, 0.0)
    else:
        path = [(u, 0.0, centre_z + v) for (u, v) in pts]
        normal = (0.0, 1.0, 0.0)
    return path, normal


if PART == "ceiling":
    # --- the fixing plate and its eye ---------------------------------------
    # A short cylinder flush under the ceiling. Round, because a boxed plate was
    # part of what made the old chain read as carpentry rather than ironwork.
    plate_top, plate_bot = CEIL, CEIL - PLATE_T
    plate_rings = []
    for z in (plate_top, plate_bot):
        plate_rings.append([bm.verts.new((PLATE_R * math.cos(2.0 * math.pi * j / 16),
                                          PLATE_R * math.sin(2.0 * math.pi * j / 16), z))
                            for j in range(16)])
    for j in range(16):
        k = (j + 1) % 16
        bm.faces.new((plate_rings[0][j], plate_rings[0][k],
                      plate_rings[1][k], plate_rings[1][j]))
    cap(plate_rings[1], -1)  # underside; the top is against the ceiling

    # The eye: a ring in the XZ plane, hanging below the plate, that the first
    # link passes through. It is what makes the chain look FIXED, not glued.
    eye_centre = plate_bot - EYE_R
    eye_path = [(EYE_R * math.sin(2.0 * math.pi * i / 20), 0.0,
                 eye_centre + EYE_R * math.cos(2.0 * math.pi * i / 20))
                for i in range(20)]
    sweep(eye_path, (0.0, 1.0, 0.0), [EYE_WIRE] * len(eye_path), closed=True)

    # --- the links ----------------------------------------------------------
    # Advance is short of a full link so consecutive ends OVERLAP and interlock,
    # each passing through the one above. Two wire diameters is the clearance a
    # real link needs to swing.
    advance = 2.0 * (LINK_HALF_L - 2.0 * WIRE)
    # The first link hangs THROUGH the eye, so its centre is placed from the
    # eye's BOTTOM (eye_centre - EYE_R), not from the eye's centre — dropped by
    # its own half-length and lifted by the two wire diameters that have to
    # overlap for the eye's stock to sit inside the link's hole. Getting this
    # from the eye's centre instead pushed the link's top 7 mm through the
    # ceiling, which the assert at the bottom caught.
    z = (eye_centre - EYE_R) - LINK_HALF_L + 2.0 * (WIRE + EYE_WIRE)
    for i in range(LINKS):
        path, normal = stadium(LINK_HALF_W, LINK_HALF_L, upright=(i % 2 == 1),
                               centre_z=z)
        sweep(path, normal, [WIRE] * len(path), closed=True)
        z -= advance

    # --- the hook -----------------------------------------------------------
    # Swept like everything else, tapering toward the point. Sweeping 200
    # degrees rather than 180 is what makes it a hook that would HOLD something
    # instead of an open C.
    hook_top = z + LINK_HALF_L
    shank_z = hook_top - HOOK_SHANK
    hook_path = [(0.0, 0.0, hook_top - HOOK_SHANK * i / 3.0) for i in range(3)]
    HOOK_SEG = 18
    for i in range(HOOK_SEG + 1):
        a = math.pi + (math.pi + 0.35) * i / HOOK_SEG
        hook_path.append((HOOK_R + HOOK_R * math.cos(a), 0.0,
                          shank_z + HOOK_R * math.sin(a)))
    radii = ([WIRE] * 3
             + [WIRE * (1.0 - 0.62 * i / HOOK_SEG) for i in range(HOOK_SEG + 1)])
    hook_rings = sweep(hook_path, (0.0, 1.0, 0.0), radii, closed=False)
    cap(hook_rings[0], 1)
    cap(hook_rings[-1], -1)

elif PART == "chain":
    # --- the grip ring ------------------------------------------------------
    # What the hand actually takes. A ring rather than a hook because a hook has
    # a direction and a ring does not: this is pulled straight down, and a shape
    # that says "hang something on me" says the wrong thing.
    ring_centre = RING_CROWN - RING_R
    ring_path = [(RING_R * math.sin(2.0 * math.pi * i / 20), CHAIN_OUT,
                  ring_centre + RING_R * math.cos(2.0 * math.pi * i / 20))
                 for i in range(20)]
    sweep(ring_path, (0.0, 1.0, 0.0), [RING_WIRE] * len(ring_path), closed=True)

    # --- the links, climbing into the socket --------------------------------
    # The first link hangs THROUGH the ring's crown, by the same overlap sum the
    # ceiling chain does at its eye: a link's half-length up from the crown, less
    # the two wire diameters that must interpenetrate for the ring's stock to sit
    # inside the link's hole. Butting them instead leaves the ring hanging in air.
    advance = 2.0 * (D_LINK_HALF_L - 2.0 * D_WIRE)
    z = RING_CROWN + D_LINK_HALF_L - 2.0 * (D_WIRE + RING_WIRE)
    # Up to the TOP of the socket, not to where the chain stops being visible.
    # That buried length is what the pull draws out.
    i = 0
    while z - D_LINK_HALF_L < SOCKET_Z1:
        path, normal = stadium(D_LINK_HALF_W, D_LINK_HALF_L, upright=(i % 2 == 1),
                               centre_z=z)
        # stadium() builds about the origin; the whole chain stands off the wall.
        path = [(px, py + CHAIN_OUT, pz) for (px, py, pz) in path]
        sweep(path, normal, [D_WIRE] * len(path), closed=True)
        z += advance
        i += 1
    chain_top = z - advance + D_LINK_HALF_L

elif PART == "socket":
    # --- the anchor the chain runs out of -----------------------------------
    # A CLOSED SOLID, not a sleeve. An open tube would hide the links from the
    # side and show them from below, and this sits at 2.25 m in a 2.5 m room —
    # looking up at it is the normal view. Capped at both ends, the depth buffer
    # does the hiding and no hole has to be modelled; the chain simply
    # interpenetrates the underside, which is what a chain leaving a socket
    # looks like.
    sleeve_path = [(0.0, CHAIN_OUT, SOCKET_Z0),
                   (0.0, CHAIN_OUT, SOCKET_Z0 + 0.014),
                   (0.0, CHAIN_OUT, SOCKET_Z1 - 0.010),
                   (0.0, CHAIN_OUT, SOCKET_Z1)]
    sleeve_radii = [SOCKET_MOUTH, SOCKET_R, SOCKET_R, SOCKET_MOUTH * 0.92]
    sleeve = sweep(sleeve_path, (1.0, 0.0, 0.0), sleeve_radii, closed=False)
    cap(sleeve[0], -1)
    cap(sleeve[-1], 1)
    # A collar around the mouth, so the chain looks THREADED through a fitting
    # rather than sprouting from a lump. Swept in the horizontal plane, which the
    # constant-frame sweep handles because a circle is planar.
    collar = [(SOCKET_MOUTH * math.cos(2.0 * math.pi * i / 18),
               CHAIN_OUT + SOCKET_MOUTH * math.sin(2.0 * math.pi * i / 18),
               SOCKET_Z0 + 0.006) for i in range(18)]
    sweep(collar, (0.0, 0.0, 1.0), [0.006] * len(collar), closed=True)

# --- UVs --------------------------------------------------------------------
# Dominant-axis projection. Valid here in a way it would NOT be on the tubes'
# swept surface if the texture had direction — but rusted_iron is isotropic
# grime with no grain to point the wrong way, so the only thing that matters is
# an even texel rate, which this gives. (Contrast BuildCeilingBeam, where the
# planks made orientation everything.)
uv = bm.loops.layers.uv.verify()
for face in bm.faces:
    nx, ny, nz = (abs(c) for c in face.normal)
    for loop in face.loops:
        co = loop.vert.co
        if nz >= nx and nz >= ny:   p = (co.x, co.y)
        elif nx >= ny:              p = (co.y, co.z)
        else:                       p = (co.x, co.z)
        loop[uv].uv = (p[0] / TILE, p[1] / TILE)

bm.normal_update()

NAME = {"ceiling": "hanging_chain", "chain": "door_chain",
        "socket": "door_chain_socket"}[PART]
mesh = bpy.data.meshes.new(NAME)
bm.to_mesh(mesh)
bm.free()

obj = bpy.data.objects.new(NAME, mesh)
bpy.context.scene.collection.objects.link(obj)

zs = [v.co.z for v in mesh.vertices]
ys = [-v.co.y for v in mesh.vertices]      # ENGINE axis: out of the wall
print(f"BuildHangingChain[{PART}]: {len(mesh.vertices)} verts, "
      f"{len(mesh.polygons)} faces, z {min(zs):+.3f}..{max(zs):+.3f}, "
      f"out {min(ys):+.3f}..{max(ys):+.3f} (units)")

if PART == "ceiling":
    assert max(zs) <= CEIL + 1e-6, "the chain pokes through the ceiling"
    # It must hang in the room, not end up at head height where the party walks.
    assert min(zs) > 0.40, "the chain hangs into head height"
else:
    # A WALL PROP'S MOUNTING CONTRACT, on the engine axes deliberately: an axis
    # mix-up is exactly the bug these exist to catch, so checking the authoring
    # frame would miss it. Nothing may sit behind the wall face except what the
    # stone is meant to hide, and nothing may jut a quarter square into the room.
    assert max(ys) < 0.25, "the opener juts a quarter square into the room"
    assert max(abs(min(zs)), max(zs)) < 0.5, "the opener is taller than half a square"
    if PART == "chain":
        # THE PULL'S CONTRACT WITH THE SOCKET. The chain slides down by
        # kChainDrop and the socket does not, so the top link has to still be
        # inside the socket at full travel — otherwise the end of the chain
        # walks out of the bottom of its own anchor, which is the one way this
        # arrangement can look broken.
        assert chain_top - CHAIN_DROP > SOCKET_Z0, (
            f"a {CHAIN_DROP} drop pulls the chain's top to "
            f"{chain_top - CHAIN_DROP:.3f}, out of a socket that starts at "
            f"{SOCKET_Z0}")

# Winding, CHECKED rather than trusted — the one thing a swept tube gets silently
# wrong, and an inside-out chain reads as a shading fault, not a normals bug.
# Every face on a tube must point away from the chain's own vertical axis; flat
# caps are the exception and are skipped. The axis is not the origin for a wall
# prop: the whole thing stands off the stone, so the test measures from where the
# chain actually hangs.
AXIS_Y = 0.0 if PART == "ceiling" else CHAIN_OUT
outward = 0
for poly in mesh.polygons:
    c = poly.center
    if abs(poly.normal.z) > 0.9:
        continue
    radial = (c.x, c.y - AXIS_Y)
    rl = math.hypot(*radial)
    if rl < 1e-5:
        continue
    if (poly.normal.x * radial[0] + poly.normal.y * radial[1]) / rl > 0.0:
        outward += 1
    else:
        outward -= 1
assert outward > 0, "the tubes are wound inside out"

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
print(f"BuildHangingChain: wrote {OUT}")
