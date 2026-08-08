# ============================================================================
# tools/BuildDoorLeaf.py — authors the LEFT HALF of a split wooden door's leaf,
# and writes a UNIT-SPACE .glb.
#
#   blender --background --factory-startup --python tools\BuildDoorLeaf.py -- <out.glb>
#
#   AssetBaker import-model <out.glb> <assets> door_panel_half --raw \
#       --texture-set wood_planks_old8
#
#   live preview: python tools\bsend.py -f tools\BuildDoorLeaf.py
#
# Replaces ModelBaker's BuildDoorPanelHalf(), which was four boxes making a flat
# slab with a pocket in it. The asset keeps its name (`door_panel_half`) so the
# catalog and the trim that meets it need no edit; the builder is retired there
# the same way BuildDoorFrame's was, since `AssetBaker models` would otherwise
# overwrite the imported mesh with boxes again.
#
# EVERYTHING IS IN UNITS: 1.0 = one dungeon square (game::kUnit, 2.5 m), Z up.
# Blender X is the engine's X, Blender Z is the engine's Y (up), and Blender Y
# is the door's THICKNESS. No quarter-turn about X: that is for a wall prop
# authored "z out of the wall" (BuildLever), and a leaf authored Z-up like its
# frame already lands the right way round.
#
# ---------------------------------------------------------------------------
# WHAT THIS MESH HAS TO SURVIVE, and every one of these shaped it
# ---------------------------------------------------------------------------
# 1. IT IS THE LEFT HALF ONLY. DungeonWorld_Render draws it twice, the second
#    copy turned a HALF TURN about Y — a proper rotation, so the winding
#    survives and an authored (back-culled) mesh does not turn inside out the
#    way a scale(-1, 1, 1) mirror would.
# 2. THEREFORE SYMMETRIC IN SECTION. A half turn about the engine's Y is
#    (x, y) -> (-x, -y) in Blender, so the turned copy shows what was the BACK
#    face. A leaf with a distinct front and back reads wrong on one half. Every
#    displacement below is applied as sign(y) * f(x, z) for exactly this reason,
#    and the assert at the bottom CHECKS the vertex set is its own mirror
#    rather than trusting that the code kept the discipline.
# 3. ANYTHING IT CARRIES REACHES x = 0 EXACTLY OR STOPS WELL SHORT. The old
#    braces stopped at 96% of the half width; on a split door the two insets
#    met, and the 3.4 cm notch where they did read as a downward arrow stamped
#    on the door. So the meeting edge is SQUARE — the chamfer that separates
#    every other pair of boards is suppressed there.
# 4. THE OPENING IS A CONTRACT WITH THE FRAME (tools/BuildDoorFrame.py):
#    OPEN 0.34 x DOOR_H 0.84 units, asserted at both ends. The frame's jambs
#    lap 2 cm over it as a rebate, so the leaf's outer edge is never seen.
# 5. IT MUST STAY THINNER THAN ITS IRONWORK. The trim (door_band_half, still
#    ModelBaker) stands 0.026 units proud; the frame's mortice is 0.032 half-
#    high. Boards that bow past either would poke through their own straps or
#    jam in the slot, so the bow, the jitter and the noise are all budgeted
#    against BAND_T and asserted.
#
# ---------------------------------------------------------------------------
# THE BOARDS, and why they are MEASURED off the texture rather than chosen
# ---------------------------------------------------------------------------
# A plank door is boards, and the scan already paints boards — so if the mesh
# picks its own pitch the two patterns beat against each other and the door
# reads as boarding printed on a slab. The fix is not to guess the scan's pitch
# but to READ IT: `wood_planks_old8` is ten boards across a square scan, and
# they are not evenly spaced (the widest is 13% wider than the narrowest), so
# SEAMS_U below is the measured seam table, normalised.
#
# Each mesh board is then given ONE scan board — its own u span — which is the
# unroll idiom the arch soffit uses. Three things fall out of it: the painted
# seam lands IN the modelled groove instead of a centimetre beside it (a near
# miss reads as a double line, which is the same failure as the 3.4 cm notch);
# the boards can differ in width without stretching visibly, since each is
# normalised to its own board; and BOARDS picks which scan board each one gets,
# so neighbours differ in colour and grain the way sawn boards do.
#
# This is the same sum kDoorTile did for the old flat leaf and got wrong in one
# place — it assumed 16 boards where the scan has 10, so the boards it laid
# were 27 cm, not the 17 cm the comment claimed. Measuring beats counting.
# ============================================================================
import sys

import bmesh
import bpy
from mathutils import Vector, noise

# --- the opening, which the frame asserts too -------------------------------
OPEN = 0.34         # half-width of the opening; this leaf spans x in [-OPEN, 0]
DOOR_H = 0.84       # height

# --- the leaf ---------------------------------------------------------------
T = 0.020           # half-thickness (ModelBaker kLeafHalfT 0.05 m / kUnit)
BAND_T = 0.026      # the trim's proud half-thickness — the ceiling for our bow

N_BOARD = 5         # boards across the half leaf: 5 x ~16 cm, 10 on the door
GROOVE_W = 0.004    # gap between two boards at the face (1 cm)
GROOVE_D = 0.003    # how deep that gap cuts (7.5 mm) — a groove, not a slit:
                    # the boards stay solid through their middle, so no light
                    # passes and the door still reads as one leaf edge-on

# --- the flush pull, cut through BOTH leaf and band -------------------------
# Shared with ModelBaker's kPull* constants, converted to units. The pocket was
# first cut into the band alone, which made it 1.5 cm deep and invisible: the
# meeting stile is the nearest surface to a torch held at the eye, so it is the
# BRIGHTEST thing on the door and a shallow step throws no shadow against a
# hotspot. Cutting the leaf as well takes it to 4.7 cm, which goes dark.
PULL_Z = 0.420      # hand height (kPullY 1.05 m)
PULL_HZ = 0.030     # half-height (kPullHalfH 0.075 m)
PULL_X = -0.036     # inner edge of the pocket; it runs out to x = 0
WEB = 0.0072        # leaf left behind the pocket, half-thickness

# --- the life in it ---------------------------------------------------------
# Sawn boards cup, sit a little proud of each other, and weather. All three are
# tiny — the whole budget is 4 mm on a 5 cm leaf — but they are what stops a
# torch sweeping across the door lighting it as one flat rectangle.
BOW = 0.0022        # crown at a board's centre line
JITTER = 0.0012     # per-board thickness, so the run is not machined flush
# The warp runs along a board's LENGTH and is constant across it, which is both
# what a board does and all the mesh can carry: there are three stations across
# a board and eight up it, so a field varying faster than that would alias into
# per-station noise — irregularity by accident rather than by design.
WARP_AMP, WARP_FREQ = 0.0008, 2.5
BEVEL = 0.0015      # arris break: rounds the groove lips into a soft V

# --- the scan ---------------------------------------------------------------
# Measured off assets/textures/wood_planks_old8_1k.png: column-mean minima,
# confirmed by autocorrelation (pitch 101.5 px on 1024 = 10.1 boards). Eleven
# entries — the ten boards' edges, first and last being the tiling seam.
SEAMS_U = (0.0000, 0.0986, 0.1992, 0.2949, 0.3936, 0.5020,
           0.6045, 0.7031, 0.7998, 0.9014, 1.0000)
# Which scan board each mesh board wears, outside in. Shuffled rather than
# sequential so no two neighbours share a grain — and so the CENTRE, where the
# turned copy puts board 4 against itself, is the one the iron meeting stile
# covers anyway.
BOARDS = (2, 7, 0, 5, 3)

args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
EXPORT = bool(args)
OUT = args[0] if args else "door_panel_half.glb"

noise.seed_set(20260808)

if bpy.context.mode != "OBJECT":
    bpy.ops.object.mode_set(mode="OBJECT")
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

# ============================================================================
# the grid — the same SOLIDITY GRID the frame is built from
# ============================================================================
# Boards, grooves and the pull pocket are all "which cells of a coarse grid
# hold wood", and every face between a solid cell and a hollow one is emitted
# mechanically. The frame's note explains why at length; the short version is
# that a grid boundary cannot have a T-junction (every face is one cell face)
# and cannot leave a hole (a face is emitted exactly when solidity changes),
# which is what makes the closed-shell assert at the bottom a real check on the
# construction rather than a restatement of it.
#
# Hand-listing was tried in ModelBaker and worked there because the leaf was
# four boxes. Four boards each cut by a groove, one of them also cut by a
# pocket, is where that stops paying.
BOARD_W = (OPEN - (N_BOARD - 1) * GROOVE_W) / N_BOARD

XS, KIND, OWNER = [-OPEN], [], []   # KIND/OWNER are per COLUMN, so len = len(XS)-1
_x = -OPEN
for b in range(N_BOARD):
    for s in range(1, 4):           # two interior stations, so a board can bow
        XS.append(_x + BOARD_W * s / 3.0)
        KIND.append("board")
        OWNER.append(b)
    _x += BOARD_W
    if b < N_BOARD - 1:
        _x += GROOVE_W
        XS.append(_x)
        KIND.append("groove")
        OWNER.append(b)             # a groove belongs to the board on its left

# The pocket's side wall has to be a grid plane. It lands inside the innermost
# board, between two of its bow stations.
_i = next(i for i in range(len(XS) - 1) if XS[i] < PULL_X < XS[i + 1])
XS.insert(_i + 1, PULL_X)
KIND.insert(_i + 1, KIND[_i])
OWNER.insert(_i + 1, OWNER[_i])

# Five layers through the thickness: the groove's depth on each face, then the
# web's depth on each face, then the core.
YS = (-T, -T + GROOVE_D, -WEB, WEB, T - GROOVE_D, T)

# Height stations: the pocket's band exactly, and enough between for the noise
# to have somewhere to act.
ZS = sorted({0.0, DOOR_H, PULL_Z - PULL_HZ, PULL_Z + PULL_HZ}
            | {DOOR_H * i / 6.0 for i in range(1, 6)})

NX, NY, NZ = len(XS) - 1, len(YS) - 1, len(ZS) - 1
POCKET_K = [i for i in range(NZ)
            if ZS[i] >= PULL_Z - PULL_HZ - 1e-9 and ZS[i + 1] <= PULL_Z + PULL_HZ + 1e-9]
POCKET_I = [i for i in range(NX) if XS[i] >= PULL_X - 1e-9]


def solid(i, j, k):
    """Is there wood in grid cell (i, j, k)? Outside the grid there is not."""
    if not (0 <= i < NX and 0 <= j < NY and 0 <= k < NZ):
        return False
    if KIND[i] == "groove":
        return 1 <= j <= 3            # a groove cuts GROOVE_D into both faces
    if i in POCKET_I and k in POCKET_K:
        return j == 2                 # the pull: only the web is left
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

# Weld. Every quad above was emitted with fresh vertices, so the leaf is a soup
# of disconnected faces until now — and recalc_face_normals needs CONNECTIVITY
# to mean anything, so this has to come first. See the normals trap.
bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-6)
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])

# ============================================================================
# the life: crown, jitter, weather — all as sign(y) * f(x, z)
# ============================================================================
# Displacing along the thickness and taking the sign from the face is what
# keeps the section symmetric (constraint 2). It also keeps the groove's DEPTH
# constant: a groove's lip and its floor are both outside the web, so they move
# together and the groove travels with the board it is cut beside.
#
# The web is left alone (|y| <= WEB), so the pull's recess keeps its depth.


def board_at(x):
    """Which board owns this x. Grooves belong to the board on their left, so a
    vertex shared between a board and its groove moves with that board."""
    for i in range(NX):
        if x <= XS[i + 1] + 1e-9:
            return OWNER[i]
    return N_BOARD - 1


def board_span(b):
    x0 = -OPEN + b * (BOARD_W + GROOVE_W)
    return x0, x0 + BOARD_W


for v in bm.verts:
    y = v.co.y
    if abs(y) <= WEB + 1e-9:
        continue
    b = board_at(v.co.x)
    x0, x1 = board_span(b)
    u = 2.0 * (v.co.x - 0.5 * (x0 + x1)) / BOARD_W        # -1..1 across the board
    crown = BOW * max(0.0, 1.0 - u * u)
    # One jitter value per board, sampled from the noise field at its centre so
    # the script stays free of a random seed's ordering.
    jit = JITTER * noise.noise(Vector((float(b) * 3.7, 0.0, 0.0)))
    warp = WARP_AMP * noise.noise(Vector((float(b) * 5.1, v.co.z * WARP_FREQ, 0.0)))
    v.co.y = y + (1.0 if y > 0.0 else -1.0) * (crown + jit + warp)

bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])

# Break the arrises so torchlight catches them, and so the square grooves read
# as a soft V rather than a routed slot. Safe on the whole mesh: it is one
# connected shell, so there are no islands a bevel could weld together.
bmesh.ops.bevel(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:],
                offset=BEVEL, offset_type="OFFSET", segments=1,
                profile=0.5, affect="EDGES", clamp_overlap=True)
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])

# ============================================================================
# UVs — one scan board per mesh board (see the header)
# ============================================================================
# u comes from x through the owning board's measured span, so the painted seam
# falls in the modelled groove. v runs the scan's full height over the door's,
# so a board's grain runs its whole length and nothing repeats vertically.
#
# The end caps (normal along z) are the exception: they are a constant z, so
# taking v from z would collapse them to a line. They sit in the frame's rebate
# and are never seen, so they take v from the thickness instead.
uv = bm.loops.layers.uv.verify()
for f in bm.faces:
    cap = abs(f.normal.z) >= max(abs(f.normal.x), abs(f.normal.y))
    for loop in f.loops:
        co = loop.vert.co
        b = board_at(co.x)
        x0, x1 = board_span(b)
        u0, u1 = SEAMS_U[BOARDS[b]], SEAMS_U[BOARDS[b] + 1]
        u = u0 + (co.x - x0) / (x1 - x0) * (u1 - u0)
        v = (0.5 + co.y / DOOR_H) if cap else (co.z / DOOR_H)
        loop[uv].uv = (u, v)

mesh = bpy.data.meshes.new("door_panel_half")
bm.to_mesh(mesh)
bm.free()
obj = bpy.data.objects.new("door_panel_half", mesh)
bpy.context.scene.collection.objects.link(obj)

co = [v.co for v in mesh.vertices]
print(f"BuildDoorLeaf: {N_BOARD} boards of {BOARD_W * 2.5 * 100:.1f} cm, "
      f"{len(co)} verts, x {min(c.x for c in co):+.4f}..{max(c.x for c in co):+.4f}  "
      f"y {min(c.y for c in co):+.4f}..{max(c.y for c in co):+.4f}  "
      f"z {min(c.z for c in co):+.4f}..{max(c.z for c in co):+.4f}")

# ============================================================================
# the contract, checked rather than trusted
# ============================================================================
# THE OPENING (constraint 4). A leaf narrower than the frame shows as a lit
# sliver down one jamb, which reads as a lighting fault rather than a dimension
# error and is very hard to chase.
assert abs(min(c.x for c in co) + OPEN) < 1e-4, "the leaf does not fill the opening"
assert abs(max(c.z for c in co) - DOOR_H) < 1e-4, "the leaf is not the door's height"
assert abs(min(c.z for c in co)) < 1e-4, "the leaf does not reach the floor"

# THE MEETING EDGE (constraint 3). The bevel shrinks the face at x = 0 but does
# not move it, so this is exact — and it is the one that bit before.
assert abs(max(c.x for c in co)) < 1e-4, "the meeting edge misses the centre line"

# THE THICKNESS BUDGET (constraint 5). Room left for the trim to stand proud
# and for the frame's mortice to swallow the pair.
thick = max(abs(c.y) for c in co)
assert thick < BAND_T - 0.001, (
    f"the boards bow to {thick:.4f}, past the trim's {BAND_T:.4f}")

# SYMMETRIC IN SECTION (constraint 2). Every vertex must have a mirror in y, or
# the turned copy shows a different door. Checked rather than reasoned about,
# because every displacement above had to remember to do it.
#
# By DISTANCE, not by an exact key: the bevel places its vertices through a
# solver, so a mirror pair agrees to about 1e-7 rather than to the bit. A key
# match on rounded coordinates called 32 of those a break — the mesh was
# symmetric and the test was not. 1e-4 units is a quarter of a millimetre;
# what this is guarding against is a face on one side only, which is
# millimetres at least.
CELL = 1e-3
grid = {}
for c in co:
    grid.setdefault((int(c.x // CELL), int(c.y // CELL), int(c.z // CELL)), []).append(c)
worst = 0.0
for c in co:
    m = Vector((c.x, -c.y, c.z))
    bx, by, bz = int(m.x // CELL), int(m.y // CELL), int(m.z // CELL)
    near = [q for dx in (-1, 0, 1) for dy in (-1, 0, 1) for dz in (-1, 0, 1)
            for q in grid.get((bx + dx, by + dy, bz + dz), ())]
    worst = max(worst, min(((q - m).length for q in near), default=1e9))
assert worst < 1e-4, f"a vertex is {worst:.5f} from any mirror — the section is not symmetric"

# CLOSED. Every edge wants exactly two faces: an unclosed shell means a face is
# missing or something did not weld, and it is the precondition for
# recalc_face_normals meaning anything at all.
check = bmesh.new()
check.from_mesh(mesh)
n_boundary = len([e for e in check.edges if len(e.link_faces) != 2])
check.free()
assert n_boundary == 0, f"{n_boundary} boundary edges — the shell is not closed"

if EXPORT:
    bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
    print(f"BuildDoorLeaf: wrote {OUT}")
else:
    print("BuildDoorLeaf: built in the bridge, not exported")
