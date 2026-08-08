# ============================================================================
# tools/BuildDoorFrame.py — authors the stone surround a door leaf hangs in,
# and writes a UNIT-SPACE .glb.
#
#   blender --background --factory-startup --python tools\BuildDoorFrame.py -- <out.glb>
#   blender --background --factory-startup --python tools\BuildDoorFrame.py -- <out.glb> --rough
#
#   AssetBaker import-model <out.glb> <assets> door_frame --raw --texture-set wall_stone
#
#   live preview (frame + a stand-in leaf): python tools\bsend.py -f tools\BuildDoorFrame.py
#
# Replaces ModelBaker's BuildDoorFrame(), which was three boxes: two posts and a
# lintel. It is BuildWallArch.py's construction with a square head — the passage
# arch already solved this exact problem, and the reasons hold here unchanged:
# the opening is CONSTRUCTED from panels rather than booleaned (predictable
# topology, a reveal that is a clean strip), and every stone is its own island
# with a real mortar gap (so a whole-mesh bevel cannot weld two stones together,
# and weathering is a per-island transform).
#
# EVERYTHING IS IN UNITS: 1.0 = one dungeon square (game::kUnit, 2.5 m), Z up.
# The exporter's +Y-Up conversion turns Blender Z into the engine's Y and
# Blender -Y into the engine's +Z, so the frame's thickness runs along Blender
# Y — which is the door's travel axis. No quarter-turn about X here: that is
# for a WALL prop authored "z out of the wall" (BuildLever), and a door
# authored Z-up already lands the right way round.
#
# THE OPENING IS A CONTRACT WITH THE LEAF, not a free choice. The leaves are
# built in ModelBaker in metres (kLeafHalfW 0.85, kLeafH 2.1) and divided by
# kUnit, so they arrive at exactly OPEN 0.34 x DOOR_H 0.84. The asserts at the
# bottom check it, because a frame that disagrees with its leaf shows as a
# glowing sliver of daylight down one jamb and nothing else.
#
# The jambs LAP over the opening by INSET, which is deliberate and load-bearing
# in two ways: it hides the slab's cut edge from anyone looking along the
# passage, and it puts the leaf in a REBATE — the stone stands in front of the
# leaf's edge, so a shut door has a stop rather than a seam, and a sliding one
# has somewhere to disappear to. The stones are proud of the slab on both faces
# and the leaf is thin and centred, so it passes cleanly behind them.
# ============================================================================
import math
import random
import sys

import bmesh
import bpy
from mathutils import Matrix, Vector, noise

# --- the square -------------------------------------------------------------
HALF = 0.5          # cell half-width; the slab spans the whole square
TOP = 1.0           # floor to ceiling
SLAB_T = 0.075      # slab half-thickness

# --- the opening, which the leaf must fill exactly --------------------------
OPEN = 0.34         # half-width  (ModelBaker kLeafHalfW 0.85 m / kUnit)
DOOR_H = 0.84       # height      (ModelBaker kLeafH    2.1 m  / kUnit)

# --- the stonework ----------------------------------------------------------
INSET = 0.008       # how far the stones lap over the opening — see the header
BAND = 0.075        # jamb depth, inner face to outer
STONE_T = 0.095     # stone half-thickness — proud of the slab on both faces
MORTAR = 0.005      # gap between neighbouring stones
COURSE_H = 0.115    # target jamb course height; the count is fitted to the head

LINTEL_H = 0.130    # a monolithic head stone, bearing on both jambs
LINTEL_OVER = 0.030 # how far it oversails the jambs each side
LINTEL_PROUD = 0.012

# --- weathering -------------------------------------------------------------
# Default is DRESSED: a door surround is worked stone, not rubble, so it gets
# only enough tilt and scale jitter to stop the courses reading as machined.
# --rough adds the arch's full treatment on top.
SEED = 20260807
BEVEL = 0.004       # arris break, both variants
ROT_DEG, SCALE_JIT = 1.4, 0.018
ROUGH_ROT_DEG, ROUGH_SCALE_JIT = 4.0, 0.05
SUBDIV = 2
NOISE_AMP, NOISE_FREQ = 0.006, 22.0
COARSE_AMP, COARSE_FREQ = 0.008, 7.0

TILE = 0.24         # texture repeat, matching the arch and the engine's TileUvs

args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
EXPORT = bool(args)
OUT = args[0] if args else "door_frame.glb"
ROUGH = "--rough" in args

STONE_IN = OPEN - INSET
STONE_OUT = STONE_IN + BAND
LINTEL_HALF = STONE_OUT + LINTEL_OVER

random.seed(SEED)
noise.seed_set(SEED)

if bpy.context.mode != "OBJECT":
    bpy.ops.object.mode_set(mode="OBJECT")
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

bm = bmesh.new()


def quad(a, b, c, d):
    bm.faces.new([bm.verts.new(p) for p in (a, b, c, d)])


# ============================================================================
# the wall slab — panels around the void, never a boolean
# ============================================================================
# EMITTED ON A SHARED GRID, and that is not tidiness. The first version ran the
# side panels floor-to-ceiling as one quad each and started the head panel at
# DOOR_H, so the head's corner landed in the MIDDLE of the side panel's edge: a
# T-junction. remove_doubles welds coincident VERTICES, not a vertex into an
# edge, so those seams never closed and the shell came out with 20 boundary
# edges. Cutting every face at the same x and z breaks makes each shared edge
# meet a partner exactly.
XS = (-HALF, -OPEN, OPEN, HALF)
ZS = (0.0, DOOR_H, TOP)

for sy in (-SLAB_T, SLAB_T):
    front = sy > 0
    for i in range(3):
        for j in range(2):
            if i == 1 and j == 0:
                continue  # the opening
            x0, x1, z0, z1 = XS[i], XS[i + 1], ZS[j], ZS[j + 1]
            a, b = (x0, sy, z0), (x1, sy, z0)
            c, d = (x1, sy, z1), (x0, sy, z1)
            quad(a, b, c, d) if front else quad(b, a, d, c)

# The reveal: the surface swept through the opening. Three flat strips, so
# unlike the arch's soffit this needs no unrolling — a dominant-axis projection
# is valid on it, because the normal never rotates within a face.
quad((-OPEN, -SLAB_T, 0.0), (-OPEN, -SLAB_T, DOOR_H),
     (-OPEN, SLAB_T, DOOR_H), (-OPEN, SLAB_T, 0.0))
quad((OPEN, SLAB_T, 0.0), (OPEN, SLAB_T, DOOR_H),
     (OPEN, -SLAB_T, DOOR_H), (OPEN, -SLAB_T, 0.0))
quad((-OPEN, -SLAB_T, DOOR_H), (OPEN, -SLAB_T, DOOR_H),
     (OPEN, SLAB_T, DOOR_H), (-OPEN, SLAB_T, DOOR_H))

# Outer edges, so the slab closes where it meets the neighbouring walls — cut
# on the same grid as the faces they join, for the T-junction reason above.
for z0, z1 in zip(ZS, ZS[1:]):
    quad((-HALF, -SLAB_T, z0), (-HALF, -SLAB_T, z1), (-HALF, SLAB_T, z1), (-HALF, SLAB_T, z0))
    quad((HALF, SLAB_T, z0), (HALF, SLAB_T, z1), (HALF, -SLAB_T, z1), (HALF, -SLAB_T, z0))
for x0, x1 in zip(XS, XS[1:]):
    quad((x0, -SLAB_T, TOP), (x0, SLAB_T, TOP), (x1, SLAB_T, TOP), (x1, -SLAB_T, TOP))
for x0, x1 in ((-HALF, -OPEN), (OPEN, HALF)):
    quad((x0, SLAB_T, 0.0), (x1, SLAB_T, 0.0), (x1, -SLAB_T, 0.0), (x0, -SLAB_T, 0.0))


# ============================================================================
# the stones — each its own island, so weathering is a per-island transform
# ============================================================================
def add_stone(corners_xz, half_t):
    """A solid from four (x, z) corners extruded through +/-half_t on Y."""
    lo = [bm.verts.new((x, -half_t, z)) for x, z in corners_xz]
    hi = [bm.verts.new((x, half_t, z)) for x, z in corners_xz]
    for i in range(4):
        j = (i + 1) % 4
        bm.faces.new((lo[i], lo[j], hi[j], hi[i]))
    bm.faces.new(lo[::-1])
    bm.faces.new(hi)


# Jamb courses, fitted to the head so the top one beds the lintel. The two
# sides are stepped half a course out of phase, which is what stops a door
# surround reading as two identical ladders.
n_course = max(2, round(DOOR_H / COURSE_H))
course = DOOR_H / n_course
for side, phase in ((-1.0, 0.0), (1.0, 0.5)):
    z = 0.0
    first = True
    while z < DOOR_H - 1e-4:
        h = course * (phase if first and phase > 0.0 else 1.0)
        first = False
        z1 = min(z + h - MORTAR, DOOR_H)
        if z1 - z > MORTAR:
            add_stone([(side * STONE_IN, z), (side * STONE_OUT, z),
                       (side * STONE_OUT, z1), (side * STONE_IN, z1)], STONE_T)
        z += h

# The lintel: one stone across the head, oversailing both jambs and standing a
# little prouder than they do, so the head reads as carrying the wall above it.
# Its underside laps the opening by INSET like the jambs — it does NOT sit on
# the soffit, which would put two coplanar faces in the same place.
add_stone([(-LINTEL_HALF, DOOR_H - INSET), (LINTEL_HALF, DOOR_H - INSET),
           (LINTEL_HALF, DOOR_H - INSET + LINTEL_H),
           (-LINTEL_HALF, DOOR_H - INSET + LINTEL_H)], STONE_T + LINTEL_PROUD)

# Weld. Every quad above was emitted with fresh vertices, so the slab is a soup
# of disconnected faces until now — and recalc_face_normals needs CONNECTIVITY
# to mean anything, so this has to come first. Stones survive as separate
# islands because their mortar gaps mean nothing of one is ever coincident with
# another.
bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-5)
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])


def islands_of(mesh):
    """Connected components, re-derived on demand — welding and subdividing
    both invalidate held references (a stale one raises ReferenceError)."""
    mesh.verts.ensure_lookup_table()
    seen, groups = set(), []
    for seed_vert in mesh.verts:
        if seed_vert.index in seen:
            continue
        stack, group = [seed_vert], []
        seen.add(seed_vert.index)
        while stack:
            v = stack.pop()
            group.append(v)
            for e in v.link_edges:
                o = e.other_vert(v)
                if o.index not in seen:
                    seen.add(o.index)
                    stack.append(o)
        groups.append(group)
    wide = max(groups, key=lambda g: max(v.co.x for v in g) - min(v.co.x for v in g))
    return groups, wide, [g for g in groups if g is not wide]


# ============================================================================
# weathering
# ============================================================================
_, slab, stones = islands_of(bm)
if ROUGH:
    # Subdivide first: a bare box has 8 corners, so noise on it only skews the
    # box rather than weathering its faces.
    bmesh.ops.subdivide_edges(
        bm, edges=list({e for g in stones for v in g for e in v.link_edges}),
        cuts=SUBDIV, use_grid_fill=True)
    _, slab, stones = islands_of(bm)

rot_deg = ROUGH_ROT_DEG if ROUGH else ROT_DEG
scale_jit = ROUGH_SCALE_JIT if ROUGH else SCALE_JIT
for group in stones:
    centre = sum((v.co for v in group), Vector()) / len(group)
    rot = (Matrix.Rotation(math.radians(random.uniform(-rot_deg, rot_deg)), 3, "X")
           @ Matrix.Rotation(math.radians(random.uniform(-rot_deg, rot_deg)), 3, "Y")
           @ Matrix.Rotation(math.radians(random.uniform(-rot_deg, rot_deg)), 3, "Z"))
    scale = 1.0 + random.uniform(-scale_jit, scale_jit)
    for v in group:
        v.co = centre + (rot @ (v.co - centre)) * scale

if ROUGH:
    bm.normal_update()
    for group in stones:
        for v in group:
            n = v.normal
            if n.length_squared < 1e-9:
                continue
            d = (noise.noise(v.co * NOISE_FREQ) * NOISE_AMP
                 + noise.noise(v.co * COARSE_FREQ) * COARSE_AMP)
            v.co += n.normalized() * d

bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])

# Break the arrises so torchlight catches them. Safe on the whole mesh: the
# mortar gaps mean no bevel can weld two stones together.
bmesh.ops.bevel(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:],
                offset=BEVEL, offset_type="OFFSET", segments=2,
                profile=0.5, affect="EDGES", clamp_overlap=True)
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])

# ============================================================================
# UVs — dominant-axis projection, which IS valid here
# ============================================================================
# The arch had to unroll its soffit because a curved reveal turns its normal 90
# degrees mid-surface and the dominant axis flips, seaming the texture. A square
# head has three FLAT reveal strips, so no face has a rotating normal and the
# cheap projection is correct.
uv = bm.loops.layers.uv.verify()
for face in bm.faces:
    nx, ny, nz = (abs(c) for c in face.normal)
    for loop in face.loops:
        co = loop.vert.co
        if nz >= nx and nz >= ny: p = (co.x, co.y)
        elif nx >= ny:            p = (co.y, co.z)
        else:                     p = (co.x, co.z)
        loop[uv].uv = (p[0] / TILE, p[1] / TILE)

mesh = bpy.data.meshes.new("door_frame")
bm.to_mesh(mesh)
bm.free()
obj = bpy.data.objects.new("door_frame", mesh)
bpy.context.scene.collection.objects.link(obj)

co = [v.co for v in mesh.vertices]
print(f"BuildDoorFrame: {'rough' if ROUGH else 'dressed'}, {len(stones)} stones, "
      f"{len(co)} verts, x {min(c.x for c in co):+.3f}..{max(c.x for c in co):+.3f}  "
      f"y {min(c.y for c in co):+.3f}..{max(c.y for c in co):+.3f}  "
      f"z {min(c.z for c in co):+.3f}..{max(c.z for c in co):+.3f}")

# ============================================================================
# the contract, checked rather than trusted
# ============================================================================
# THE OPENING. Nothing may cross into the leaf's face beyond the deliberate
# INSET lap, and nothing may narrow the opening further — a frame that
# disagrees with its leaf shows as a lit sliver down one jamb, which reads as a
# lighting fault rather than a dimension error and is very hard to chase.
# Measured over the opening's HEIGHT BAND. Not at mid-depth, which was the
# first attempt and found nothing: the reveal is a flat strip whose vertices
# all sit on the two slab faces, so there is no geometry at y = 0 to measure.
# Tolerance covers the bevel and the per-stone jitter, both of which move a
# stone's inner arris by a millimetre or two.
SLOP = BEVEL + 0.006
near = [c for c in co if 0.02 < c.z < DOOR_H - 0.02]
assert near, "no geometry beside the opening — the jambs are missing"
inner = min(abs(c.x) for c in near)
assert inner > STONE_IN - SLOP, (
    f"something narrows the opening to {inner:.4f}, past the {STONE_IN:.4f} lap")
assert inner < OPEN, (
    f"the stones stop at {inner:.4f} without lapping the {OPEN:.4f} opening")

# THE SQUARE. A frame is set into a wall, so it may not leave its own cell or
# poke through the ceiling; a prop that does is clipped by the block beside it
# and reads as a modelling error at exactly one viewing angle. The bevel rounds
# the outermost arrises, so it is allowed its own offset here.
assert max(abs(c.x) for c in co) <= HALF + BEVEL + 1e-3, "the frame leaves its square"
assert min(c.z for c in co) >= -BEVEL - 1e-3, "the frame sinks through the floor"
assert max(c.z for c in co) <= TOP + BEVEL + 1e-3, "the frame pokes through the ceiling"

# CLOSED. Every edge wants exactly two faces. This is the real check on the
# construction: an unclosed shell means a panel is missing or a stone did not
# weld, and it is also the precondition for recalc_face_normals meaning
# anything at all (it needs connectivity — see the arch, and the normals trap).
check = bmesh.new()
check.from_mesh(mesh)
boundary = [e for e in check.edges if len(e.link_faces) != 2]
n_boundary = len(boundary)
check.free()
assert n_boundary == 0, f"{n_boundary} boundary edges — the shell is not closed"

if EXPORT:
    bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
    print(f"BuildDoorFrame: wrote {OUT}")
else:
    print("BuildDoorFrame: built in the bridge, not exported")
