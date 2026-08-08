# ============================================================================
# tools/BuildStoneSlab.py — authors the sliding stone door's leaf, and writes a
# UNIT-SPACE .glb.
#
#   blender --background --factory-startup --python tools\BuildStoneSlab.py -- <out.glb>
#
#   AssetBaker import-model <out.glb> <assets> stone_slab --raw \
#       --texture-set wall_stone_granite
#
#   live preview: python tools\bsend.py -f tools\BuildStoneSlab.py
#
# Replaces the stone door's use of `door_panel` — ModelBaker's plain slab, the
# stand-in every leaf wore before any was authored. It was the last type using
# it, so door_panel is now a generic placeholder for a NEW door type rather than
# any shipped one's leaf.
#
# EVERYTHING IS IN UNITS: 1.0 = one dungeon square (game::kUnit, 2.5 m), Z up.
# Same frame as BuildDoorFrame.py and BuildDoorLeaf.py — Blender Y is the door's
# thickness, and a leaf authored Z-up needs no quarter-turn about X.
#
# ---------------------------------------------------------------------------
# WHY IT IS MASONRY AND NOT A MONOLITH
# ---------------------------------------------------------------------------
# doors.cat already said what this door is for: "the vault frame matches its
# slab, so the door reads as cut from the surround". Its texture is
# `wall_stone_granite`, which is a scan of COURSED ASHLAR — a wall of blocks. A
# monolithic slab wearing it would be one stone with joints painted on, which is
# the same failure the wooden leaf had before its boards were real: the mesh and
# the scan describing different objects.
#
# So the slab IS a piece of wall — five courses of dressed blocks in running
# bond, jointed for real — and what says "door" is the bronze it carries
# (door_bosses: corner studs and a ring, still ModelBaker, since bronze cannot
# live in a granite-textured mesh). That is a better dungeon object than a
# rectangle anyway: a section of wall that slides.
#
# ---------------------------------------------------------------------------
# THE COURSES ARE MEASURED OFF THE SCAN, joint by joint
# ---------------------------------------------------------------------------
# Same discipline as the wooden leaf, and the scan made it easy: this one is a
# textbook running bond — six courses per tile at a 170.5 px pitch on 1024
# (autocorrelation 0.89), each course five blocks at a 0.2-tile pitch, alternate
# courses offset half a block. So the mesh takes the scan's grid rather than
# inventing one:
#
#   * FIVE courses fill the door's height, which is what sets the v scale: the
#     door is 0.84 units and a course is 1/6 of a tile, so the tile is stretched
#     0.8% to make five of them land exactly on the top and bottom edges. A
#     leftover sliver course at the head is the tell that nobody did this sum.
#   * THREE AND A HALF blocks fill its width, which is what sets the u scale and
#     is also just what a real wall does at a reveal — the half block falls at
#     one end on even courses and the other end on odd ones, which is the bond.
#   * Each course gets its own u offset from PHASE_U, because the scan's courses
#     are not offset by exactly half a block (they run 0.081..0.199 where a
#     clean bond would be 0.1/0.2). One global phase left the odd courses about
#     1.5 cm out — enough for a painted joint to sit beside a modelled one and
#     read as a double line.
#
# ---------------------------------------------------------------------------
# WHAT IT HAS TO SURVIVE
# ---------------------------------------------------------------------------
# 1. THE OPENING IS A CONTRACT WITH THE FRAME (tools/BuildDoorFrame.py, built
#    --right for `slide`): OPEN 0.34 x DOOR_H 0.84 units.
# 2. IT MUST CLEAR ITS OWN MORTICE, and here that is tight, because the leaf is
#    not the thickest thing on it — the RING is. The slab is 11 cm of stone and
#    the ring stands off whatever it hangs against by its own thickness, so the
#    two together only fit the 16 cm slot because the ring hangs in a POCKET.
#    That is the pocket's real job; looking like a carved grip is a bonus.
# 3. THE POCKET IS A CONTRACT WITH THE BRONZE. ModelBaker's BuildDoorBosses
#    hangs the ring off POCKET floor depth and sets its studs on the slab's
#    face, so both numbers are mirrored there and named.
# ============================================================================
import sys

import bmesh
import bpy
from mathutils import Vector, noise

# --- the opening, which the frame asserts too -------------------------------
OPEN = 0.34         # half-width
DOOR_H = 0.84       # height
SLOT_T = 0.032      # the frame's jamb mortice, half-height

# --- the slab ---------------------------------------------------------------
T = 0.022           # half-thickness (11 cm of stone) — see constraint 2 for why
                    # it is not thicker, which for a vault door it would like to be
N_COURSE = 5
BLOCKS = 3.5        # blocks across the width; the half is the bond
JOINT_W = 0.005     # mortar joint at the face (1.25 cm)
JOINT_D = 0.004     # how deep it cuts (1 cm)
JITTER = 0.0011     # per-block face offset — dressed stone, not machined

# --- the ring's pocket ------------------------------------------------------
# Mirrored in ModelBaker as kSlabPocket / kSlabHalfT. The ring reaches
# RING_REACH past the surface it hangs against, which is what the budget below
# is spent on.
POCKET_D = 0.010    # depth from the face (2.5 cm), so the floor is at T - this
POCKET_X = 0.080    # half-width  (40 cm across)
POCKET_Z0, POCKET_Z1 = 0.250, 0.430
RING_REACH = 0.0176 # what the bronze adds past the pocket floor (44 mm / kUnit)

BEVEL = 0.0015      # arris break — dressed stone keeps a crisp but broken edge

# --- the scan ---------------------------------------------------------------
# Measured off assets/textures/wall_stone_granite_1k.png. SEAM_V is the six
# course joints, normalised; PHASE_U is where each course's vertical joints
# start, per course, since they are not a clean alternation.
SEAM_V = (0.0000, 0.1680, 0.3340, 0.5020, 0.6689, 0.8340)
PHASE_U = (0.0957, 0.1895, 0.0811, 0.1758, 0.0986)
BLOCK_U = 0.2       # the scan's block pitch, in tiles

args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
EXPORT = bool(args)
OUT = args[0] if args else "stone_slab.glb"

noise.seed_set(20260809)

if bpy.context.mode != "OBJECT":
    bpy.ops.object.mode_set(mode="OBJECT")
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

BLOCK_W = 2.0 * OPEN / BLOCKS
TILE_U = BLOCK_W / BLOCK_U          # so a modelled block is a scan block wide
COURSE_H = DOOR_H / N_COURSE

# ============================================================================
# the grid — the same SOLIDITY GRID the frame and the wooden leaf are built from
# ============================================================================
# Blocks, joints and the ring's pocket are all "which cells hold stone". The
# joints are the reason it has to be a grid rather than a pile of separate
# stones with gaps between them, which is how BuildDoorFrame lays ITS courses: a
# frame's stones are set into a wall and something solid is always behind them,
# where a door has nothing behind it at all. Gaps here would be daylight. So the
# blocks share a solid CORE and only the outer skin is cut — a joint, not a slit,
# exactly as the wooden leaf's boards are parted.

# Vertical joints fall every HALF block, and each belongs to one course parity:
# even courses joint at the half offsets, odd courses at the whole ones. That is
# the whole of the running bond.
JOINT_X = [(-OPEN + k * 0.5 * BLOCK_W, k % 2) for k in range(1, int(2 * BLOCKS))]

XS, PARITY = [-OPEN], []            # PARITY is per COLUMN: None, or the course
for at, par in JOINT_X:             # parity this column is a joint for
    XS += [at - JOINT_W * 0.5, at + JOINT_W * 0.5]
    PARITY += [None, par]
XS.append(OPEN)
PARITY.append(None)

# The pocket's side walls have to be grid planes.
for at in (-POCKET_X, POCKET_X):
    i = next(i for i in range(len(XS) - 1) if XS[i] < at < XS[i + 1])
    XS.insert(i + 1, at)
    PARITY.insert(i + 1, PARITY[i])

# Course bands, with a joint band between each pair. No joint at the head or the
# sill: the frame's jambs and the floor take those edges.
ZS, COURSE = [0.0], []              # COURSE is per BAND: its course index, or
for c in range(1, N_COURSE):        # -1 for a joint band
    at = c * COURSE_H
    ZS += [at - JOINT_W * 0.5, at + JOINT_W * 0.5]
    COURSE += [c - 1, -1]
ZS.append(DOOR_H)
COURSE.append(N_COURSE - 1)

for at in (POCKET_Z0, POCKET_Z1):
    k = next(k for k in range(len(ZS) - 1) if ZS[k] < at < ZS[k + 1])
    ZS.insert(k + 1, at)
    COURSE.insert(k + 1, COURSE[k])

# Five layers through the thickness: the joint's depth on each face, then the
# pocket's, then the core the whole slab shares.
YS = (-T, -T + JOINT_D, -T + POCKET_D, T - POCKET_D, T - JOINT_D, T)

NX, NY, NZ = len(XS) - 1, len(YS) - 1, len(ZS) - 1
POCKET_I = [i for i in range(NX)
            if XS[i] >= -POCKET_X - 1e-9 and XS[i + 1] <= POCKET_X + 1e-9]
POCKET_K = [k for k in range(NZ)
            if ZS[k] >= POCKET_Z0 - 1e-9 and ZS[k + 1] <= POCKET_Z1 + 1e-9]


def course_of(k):
    """The course a band belongs to; a joint band takes the one below it."""
    return COURSE[k] if COURSE[k] >= 0 else COURSE[k - 1]


def solid(i, j, k):
    """Is there stone in grid cell (i, j, k)? Outside the grid there is not."""
    if not (0 <= i < NX and 0 <= j < NY and 0 <= k < NZ):
        return False
    # The pocket is the deepest cut, so it wins wherever it overlaps a joint.
    if i in POCKET_I and k in POCKET_K:
        return j == 2
    if COURSE[k] < 0:                       # a bed joint, running right across
        return 1 <= j <= 3
    if PARITY[i] is not None and COURSE[k] % 2 == PARITY[i]:
        return 1 <= j <= 3                  # a perpend, but only on its own courses
    return True


bm = bmesh.new()
AXES = (XS, YS, ZS)


def face(axis, at, u0, u1, v0, v1, positive):
    """An axis-aligned quad, wound so its normal points along +/- `axis`."""
    if axis == 0:
        pts = [(at, u0, v0), (at, u1, v0), (at, u1, v1), (at, u0, v1)]
    elif axis == 1:
        pts = [(u0, at, v0), (u0, at, v1), (u1, at, v1), (u1, at, v0)]
    else:
        pts = [(u0, v0, at), (u1, v0, at), (u1, v1, at), (u0, v1, at)]
    bm.faces.new([bm.verts.new(p) for p in (pts if positive else pts[::-1])])


for i in range(NX):
    for j in range(NY):
        for k in range(NZ):
            if not solid(i, j, k):
                continue
            cell = (i, j, k)
            for axis in range(3):
                for step in (-1, 1):
                    nb = list(cell)
                    nb[axis] += step
                    if solid(*nb):
                        continue
                    at = AXES[axis][cell[axis] + (1 if step > 0 else 0)]
                    if axis == 0:   a, b = 1, 2
                    elif axis == 1: a, b = 0, 2   # keep u = x, v = z: the winding
                    else:           a, b = 0, 1   # in face() assumes it
                    face(axis, at, AXES[a][cell[a]], AXES[a][cell[a] + 1],
                         AXES[b][cell[b]], AXES[b][cell[b] + 1], step > 0)

bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-6)
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])


# ============================================================================
# dressed, not machined: one face offset per block
# ============================================================================
# Dressed ashlar is cut by hand, so no two blocks sit at quite the same plane.
# One offset per block rather than a noise field over the whole slab, because
# what the eye reads on a wall is the STEP at a joint, not roughness within a
# stone — and a step is what a per-block offset makes. The pocket floor is left
# alone so the ring keeps its clearance.
def block_of(x, c):
    """Which block of course `c` holds this x — the count of that course's own
    joints below it."""
    return sum(1 for at, par in JOINT_X if par == c % 2 and x > at + 1e-9)


def course_at(z):
    return min(N_COURSE - 1, max(0, int(z / COURSE_H + 1e-9)))


for v in bm.verts:
    y = v.co.y
    if abs(y) <= T - POCKET_D + 1e-9:
        continue
    c = course_at(v.co.z)
    key = float(c) * 7.3 + float(block_of(v.co.x, c)) * 2.9
    v.co.y = y + (1.0 if y > 0.0 else -1.0) * JITTER * noise.noise(
        Vector((key, 0.0, 0.0)))

bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
bmesh.ops.bevel(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:],
                offset=BEVEL, offset_type="OFFSET", segments=1,
                profile=0.5, affect="EDGES", clamp_overlap=True)
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])

# ============================================================================
# UVs — the scan's own grid, course by course
# ============================================================================
# u is linear in x at the scan's block pitch, offset per course so each modelled
# perpend lands on a painted one. v is linear in z at the scan's course pitch.
# Both are piecewise by COURSE rather than global, which is the same unroll the
# wooden leaf does per board and the arch does along its soffit.
uv = bm.loops.layers.uv.verify()
for f in bm.faces:
    c = course_at(sum(l.vert.co.z for l in f.loops) / len(f.loops))
    # The offset that puts this course's first modelled joint on its first
    # painted one. Even courses joint half a block in, odd ones a whole block.
    first = -OPEN + (0.5 if c % 2 == 1 else 1.0) * BLOCK_W
    u0 = PHASE_U[c] - first / TILE_U
    v0, v1 = SEAM_V[c], SEAM_V[c + 1]
    for loop in f.loops:
        co = loop.vert.co
        t = (co.z - c * COURSE_H) / COURSE_H
        loop[uv].uv = (u0 + co.x / TILE_U, v0 + t * (v1 - v0))

mesh = bpy.data.meshes.new("stone_slab")
bm.to_mesh(mesh)
bm.free()
obj = bpy.data.objects.new("stone_slab", mesh)
bpy.context.scene.collection.objects.link(obj)

co = [v.co for v in mesh.vertices]
print(f"BuildStoneSlab: {N_COURSE} courses of {COURSE_H * 250:.0f} cm, blocks "
      f"{BLOCK_W * 250:.0f} cm wide, {len(co)} verts, "
      f"x {min(c.x for c in co):+.4f}..{max(c.x for c in co):+.4f}  "
      f"y {min(c.y for c in co):+.4f}..{max(c.y for c in co):+.4f}  "
      f"z {min(c.z for c in co):+.4f}..{max(c.z for c in co):+.4f}")

# ============================================================================
# the contract, checked rather than trusted
# ============================================================================
# THE OPENING (constraint 1).
assert abs(min(c.x for c in co) + OPEN) < 1e-4, "the slab misses the left jamb"
assert abs(max(c.x for c in co) - OPEN) < 1e-4, "the slab misses the right jamb"
assert abs(max(c.z for c in co) - DOOR_H) < 1e-4, "the slab is not the door's height"
assert abs(min(c.z for c in co)) < 1e-4, "the slab does not reach the floor"

# THE MORTICE (constraint 2), which the STONE alone would pass easily — so it is
# checked with the bronze's reach added, because that is the pair that has to go
# into the slot together. Take the pocket away and this fails, which is the
# point: the pocket is structural, not decoration.
thick = max(abs(c.y) for c in co)
assert thick < SLOT_T - 0.002, (
    f"the slab is {thick:.4f} thick, past the mortice's {SLOT_T:.4f}")
assert (T - POCKET_D) + RING_REACH < SLOT_T, (
    f"the ring reaches {(T - POCKET_D) + RING_REACH:.4f}, past the mortice's "
    f"{SLOT_T:.4f} — deepen POCKET_D and tell ModelBaker")

# THE POCKET IS A CONTRACT WITH THE BRONZE (constraint 3). It has to hold the
# ring, so it has to be bigger than one — and the ring is authored in metres in
# another file, so the number that fails here is the one to go and change.
assert POCKET_X > 0.062 and POCKET_Z1 - POCKET_Z0 > 0.15, (
    "the pocket is too small for BuildDoorBosses' ring")

# THE BOND. Five courses and three and a half blocks are what make the scan's
# grid land on the mesh's; a change to either that forgets the other puts a
# sliver course at the head or a sliver block at a jamb.
assert abs(N_COURSE * COURSE_H - DOOR_H) < 1e-9, "the courses do not fill the height"
assert abs(BLOCKS % 1.0 - 0.5) < 1e-9, "a running bond needs a HALF block spare"

# CLOSED. Every edge wants exactly two faces.
check = bmesh.new()
check.from_mesh(mesh)
n_boundary = len([e for e in check.edges if len(e.link_faces) != 2])
check.free()
assert n_boundary == 0, f"{n_boundary} boundary edges — the shell is not closed"

if EXPORT:
    bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
    print(f"BuildStoneSlab: wrote {OUT}")
else:
    print("BuildStoneSlab: built in the bridge, not exported")
