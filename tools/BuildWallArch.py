# ============================================================================
# tools/BuildWallArch.py — authors a stone passage arch and writes a UNIT-SPACE
# .glb. One script, two looks:
#
#   blender --background --factory-startup --python-exit-code 1 --python tools\BuildWallArch.py -- <out.glb>
#   blender --background --factory-startup --python-exit-code 1 --python tools\BuildWallArch.py -- <out.glb> --rough
#
# --python-exit-code 1 is what makes the checks at the bottom fail the RUN:
# without it a failed assert still exits 0, the export is skipped, and the next
# step imports whatever out.glb an earlier run left.
#
# Then:
#   AssetBaker import-model <out.glb> <assets> wall_arch_rustic --raw --texture-set stacked_stone
#
# EVERYTHING IS IN UNITS: 1.0 = one dungeon square (game::kUnit, 2.5 m), Z up.
# The exporter's +Y-Up conversion turns Blender Z into the engine's Y and
# Blender -Y into the engine's +Z, so the wall's thickness runs along Blender Y.
#
# WHY BUILT RATHER THAN DERIVED: an earlier version roughened an imported,
# hand-modelled arch. That meant reconstructing which vertices belonged to
# which stone from connectivity — and glTF's per-corner attributes plus stones
# modelled face-to-face made that unreliable (the jamb stones fused into one
# island and could not be tilted individually; three attempts to rip them apart
# shattered the mesh). Here every stone is placed as its own island with a real
# mortar gap, so it is known exactly which vertices are which stone and
# roughening is a per-island transform that cannot go wrong.
#
# The slab's opening is CONSTRUCTED, not booleaned: the slab is a SOLIDITY GRID
# (BuildDoorFrame.py's construction, on a lattice whose springline row follows
# the arc) and its boundary is emitted mechanically. That keeps the topology
# predictable, the reveal a clean strip to unwrap - and the shell CLOSED, which
# the hand-listed panels it replaced never were (see the slab section).
# ============================================================================
import math
import os
import random
import sys

import bmesh
import bpy
from mathutils import Matrix, Vector, noise

if "__file__" in globals():  # headless; under the bridge, tools/ is already on the path
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blendlib  # noqa: E402 - the shared closed-shell check (code-review C435)

# --- the arch ---------------------------------------------------------------
HALF = 0.5          # cell half-width; the slab spans the whole square
TOP = 1.0           # floor to ceiling
SLAB_T = 0.08       # slab half-thickness
R = 0.26            # opening radius, and the springline half-width
SPRING = 0.55       # height the curve springs from (crown lands at 0.81)
ARC_SEGS = 48       # smoothness of the slab's soffit

# --- the stonework ----------------------------------------------------------
INSET = 0.012       # stones sit this far inside the opening, hiding the slab's
                    # cut edge from anyone looking along the passage
BAND = 0.080        # ring depth, inner face to outer
STONE_T = 0.092     # stone half-thickness — proud of the slab on both faces
N_VOUSSOIR = 9      # odd, so one lands square on the crown as the keystone
MORTAR = 0.005      # gap between neighbouring stones
JAMB_H = 0.110      # target course height; the count is fitted to the springline

KEY_OUT = 0.055     # extra radius on the keystone
KEY_PROUD = 0.018   # extra half-thickness on the keystone
KEY_WIDEN = 1.30    # extra angular width on the keystone

# --- weathering (--rough only) ----------------------------------------------
SEED = 20260722
BEVEL = 0.004       # arris break, on both variants
ARRIS_DEG = 45.0    # a stone edge folding more than this is an ARRIS (a box
                    # edge, ~90); the --rough grid's folds stay well under it
ROT_DEG = 4.0       # per-stone tilt
SCALE_JIT = 0.05    # per-stone size
SUBDIV = 2          # cuts before displacing
NOISE_AMP, NOISE_FREQ = 0.006, 22.0     # fine surface texture
COARSE_AMP, COARSE_FREQ = 0.008, 7.0    # per-stone-scale variation

TILE = 0.24         # texture repeat, matching the engine's TileUvs

args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = args[0] if args else "wall_arch.glb"
ROUGH = "--rough" in args

STONE_IN = R - INSET
STONE_OUT = STONE_IN + BAND

random.seed(SEED)
noise.seed_set(SEED)

bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

bm = bmesh.new()
stone_groups = []      # one vertex list per stone, built in add_stone's order
# Which stone a vertex belongs to: add_stone's BUILD index + 1, the slab 0 (the
# layer's default). It is how the weathering pass hands each stone its draws in
# build order and how the bevel tells stones from slab, and it rides every op
# below (welding keeps a vertex's value; a vertex the subdivide or the bevel
# makes is interpolated from one stone's, so it keeps that stone's). Removed
# before export.
STONE = bm.verts.layers.int.new("stone")


def arc_point(theta, radius):
    """A point on the opening, theta = 0 at the crown, +/-pi/2 at the springline."""
    return (radius * math.sin(theta), SPRING + radius * math.cos(theta))


# ============================================================================
# the wall slab - a SOLIDITY GRID whose boundary is emitted mechanically
# ============================================================================
# The slab is described as which cells of a lattice hold stone, and every face
# between a solid cell and a hollow one (or the outside) is emitted. Nothing is
# hand-listed. It is BuildDoorFrame.py's construction (read its slab section
# for the long version), on a lattice that BENDS: the column lines stand at the
# side walls and at every arc sample, and the middle row line runs along the
# springline beside the opening and along the ARC over it. So a cell over the
# opening is a column of wall standing on one chord of the soffit - the same
# quads the old spandrel fan drew - and a solid cell's faces are always cut
# where its neighbours' are.
#
# WHY (code-review C435). The first slab was hand-listed: full-height side
# panels at x = +-R while the spandrel fan started at the springline, and ONE
# top quad across the whole width while the faces under it split at +-R and at
# every arc segment. Each of those is a T-junction - a corner landing mid-way
# along another face's edge - and remove_doubles welds vertex to vertex, never
# a vertex into an edge, so they never closed: ~300 open edges in each arch,
# read by recalc_face_normals as an open shell and bevelled into a notch above
# each jamb. A lattice cannot have a T-junction (every face edge is one lattice
# edge, its two ends shared with the face beside it) and cannot leave a hole
# (a face is emitted exactly where solidity changes). The closed-shell assert
# at the bottom checks the result rather than this reasoning.
thetas = [(-math.pi / 2) + math.pi * i / ARC_SEGS for i in range(ARC_SEGS + 1)]

# Column lines: the two outer walls, then every arc sample (the first and last
# of which ARE the springline corners at x = -R and +R).
LINES = [(-HALF, SPRING)] + [arc_point(t, R) for t in thetas] + [(HALF, SPRING)]
NC = len(LINES) - 1      # columns; 0 and NC - 1 stand beside the opening
NR = 2                   # rows: 0 floor..springline/arc, 1 springline/arc..ceiling


def lattice(c, k):
    """Lattice point (column line c, row line k) in the (x, z) plane."""
    x, mid = LINES[c]
    return (x, (0.0, mid, TOP)[k])


def solid(c, k):
    """Is there stone in cell (c, k)? Outside the lattice there is not."""
    if not (0 <= c < NC and 0 <= k < NR):
        return False
    return k == 1 or c in (0, NC - 1)  # under the arc is the passage itself


def slab_face(pts, want):
    """One face from (x, y, z) points, wound so its normal points along `want`."""
    a, b, c = (Vector(p) for p in pts[:3])
    if (b - a).cross(c - a).dot(Vector(want)) < 0.0:
        pts = pts[::-1]
    bm.faces.new([bm.verts.new(p) for p in pts])


for c in range(NC):
    for k in range(NR):
        if not solid(c, k):
            continue
        corners = [lattice(c, k), lattice(c + 1, k), lattice(c + 1, k + 1), lattice(c, k + 1)]
        # The wall faces, front and back: the slab is one cell thick in y.
        for sy in (-SLAB_T, SLAB_T):
            slab_face([(x, sy, z) for x, z in corners], (0.0, sy, 0.0))
        # Each side whose neighbour is hollow: the side's lattice edge swept
        # through the thickness, facing out of this cell.
        cx = sum(x for x, _ in corners) / 4.0
        cz = sum(z for _, z in corners) / 4.0
        for (p, q), nb in (((corners[0], corners[1]), (c, k - 1)),     # below
                           ((corners[3], corners[2]), (c, k + 1)),     # above
                           ((corners[0], corners[3]), (c - 1, k)),     # left
                           ((corners[1], corners[2]), (c + 1, k))):    # right
            if solid(*nb):
                continue
            mx, mz = (p[0] + q[0]) / 2.0, (p[1] + q[1]) / 2.0
            slab_face([(p[0], -SLAB_T, p[1]), (q[0], -SLAB_T, q[1]),
                       (q[0], SLAB_T, q[1]), (p[0], SLAB_T, p[1])],
                      (mx - cx, 0.0, mz - cz))


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
    for v in lo + hi:
        v[STONE] = len(stone_groups) + 1
    stone_groups.append(lo + hi)


# voussoirs, sprung symmetrically about the crown
span = math.pi / N_VOUSSOIR
gap = MORTAR / max(STONE_IN, 1e-6)          # mortar expressed as an angle
for k in range(N_VOUSSOIR):
    mid = -math.pi / 2 + span * (k + 0.5)
    keystone = (k == N_VOUSSOIR // 2)
    half_span = span / 2 - gap / 2
    if keystone:
        half_span *= KEY_WIDEN
    t0, t1 = mid - half_span, mid + half_span
    r_out = STONE_OUT + (KEY_OUT if keystone else 0.0)
    add_stone(
        [arc_point(t0, STONE_IN), arc_point(t1, STONE_IN),
         arc_point(t1, r_out), arc_point(t0, r_out)],
        STONE_T + (KEY_PROUD if keystone else 0.0),
    )

# jamb courses, fitted to the springline so the top one meets the springer
n_jamb = max(1, round(SPRING / JAMB_H))
course = SPRING / n_jamb
for side in (-1.0, 1.0):
    for i in range(n_jamb):
        z0 = i * course
        z1 = z0 + course - MORTAR
        x_in, x_out = side * STONE_IN, side * STONE_OUT
        add_stone([(x_in, z0), (x_out, z0), (x_out, z1), (x_in, z1)], STONE_T)

# Weld. Every quad above was emitted with fresh vertices, so the slab is a soup
# of disconnected faces until now. Stones survive as separate islands because
# they are built with real mortar gaps and sit INSET from the slab's opening —
# nothing of one stone is ever coincident with another, or with the wall.
bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-5)
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])


def islands_of(mesh):
    """Connected components, re-derived on demand — welding and subdividing
    both invalidate held references (a stale one raises ReferenceError)."""
    mesh.verts.ensure_lookup_table()
    mesh.verts.index_update()   # `seen` keys on .index, which an op leaves dirty
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


def stones_in_build_order(mesh):
    """Every stone's island, in add_stone's order - checked whole: the slab
    holds no stone, each island exactly one, and every stone is there."""
    _, slab, stones = islands_of(mesh)
    assert all(v[STONE] == 0 for v in slab), "BuildWallArch: a stone fused to the slab"
    by_stone = {}
    for group in stones:
        tags = {v[STONE] for v in group}
        assert len(tags) == 1 and 0 not in tags, f"BuildWallArch: an island holds stones {sorted(tags)}"
        by_stone[tags.pop()] = group
    assert sorted(by_stone) == list(range(1, len(stone_groups) + 1)), \
        f"BuildWallArch: {len(by_stone)} stone islands for {len(stone_groups)} stones"
    return [by_stone[i] for i in sorted(by_stone)]


# Every stone is its own island before anything reshapes one.
stones_in_build_order(bm)


# ============================================================================
# weathering
# ============================================================================
if ROUGH:
    # Subdivide first: a bare box has 8 corners, so noise on it only skews the
    # box rather than weathering its faces. The edges go in the mesh's order: a
    # set of BMEdges iterates in ADDRESS order.
    bmesh.ops.subdivide_edges(
        bm, edges=[e for e in bm.edges if e.verts[0][STONE]],
        cuts=SUBDIV, use_grid_fill=True)

    # Each stone's tilt and size are drawn in BUILD order (code-review batch
    # 95). They used to go to the islands in the order islands_of found them,
    # which walks the vertex pool - and remove_doubles frees slab slots that the
    # subdivide's new stone vertices refill, so WHICH stone got which draw
    # followed the slab's topology. Rebuilding the slab re-weathered every
    # stone; now only a change to the stones, the seed or the ranges can.
    for group in stones_in_build_order(bm):
        centre = sum((v.co for v in group), Vector()) / len(group)
        rot = (Matrix.Rotation(math.radians(random.uniform(-ROT_DEG, ROT_DEG)), 3, "X")
               @ Matrix.Rotation(math.radians(random.uniform(-ROT_DEG, ROT_DEG)), 3, "Y")
               @ Matrix.Rotation(math.radians(random.uniform(-ROT_DEG, ROT_DEG)), 3, "Z"))
        scale = 1.0 + random.uniform(-SCALE_JIT, SCALE_JIT)
        for v in group:
            v.co = centre + (rot @ (v.co - centre)) * scale

    bm.normal_update()
    for group in stones_in_build_order(bm):
        for v in group:
            n = v.normal
            if n.length_squared < 1e-9:
                continue
            d = (noise.noise(v.co * NOISE_FREQ) * NOISE_AMP
                 + noise.noise(v.co * COARSE_FREQ) * COARSE_AMP)
            v.co += n.normalized() * d
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])

# Break the arrises so torchlight catches them - the SLAB and the STONES in two
# calls. clamp_overlap clamps a whole CALL to the offset its tightest corner
# allows, not each edge to its own, and the slab has slivers: the column beside
# each springline is R(1 - cos(pi/ARC_SEGS)) = 0.00056 wide, because the soffit
# is near vertical there. Bevelled together, that sliver set every STONE's
# arris too - BEVEL came out 0.00056 in the arches as first shipped (one side of
# the sliver was an open edge then) and 0.00028 once the slab closed, so a slab
# rebuild moved every stone (code-review batch 95). Apart, the stones take
# BEVEL as authored and the slab what its own slivers allow (0.00028: in effect
# the slab is not bevelled, and never was). The mortar gaps still mean no bevel
# can weld two stones together. The slab goes first, so its new vertices read
# 0 whether the bevel copies the layer or leaves its default.
#
# A stone's call takes only its ARRISES. On a --rough stone the subdivision's
# grid lines are edges too, and BEVEL along every one of them softens the faces
# into pillows rather than breaking the edges - harmless while the slab's clamp
# held every offset to a hair, plain once the stones got their own.
def bevel_part(of_stones):
    def mine(v):
        return bool(v[STONE]) == of_stones
    if of_stones:
        geom = [e for e in bm.edges if mine(e.verts[0]) and e.is_manifold
                and e.calc_face_angle(0.0) > math.radians(ARRIS_DEG)]
        # Exactly the boxes' edges, 12 a stone, each cut SUBDIV + 1 times on a
        # --rough one: the angle test must catch every arris and no grid line.
        want = len(stone_groups) * 12 * ((SUBDIV + 1) if ROUGH else 1)
        assert len(geom) == want, f"BuildWallArch: {len(geom)} arrises found, {want} wanted"
        print(f"BuildWallArch: {len(geom)} stone arrises bevelled")
    else:
        geom = ([v for v in bm.verts if mine(v)] + [e for e in bm.edges if mine(e.verts[0])]
                + [f for f in bm.faces if mine(f.verts[0])])
    bmesh.ops.bevel(bm, geom=geom, offset=BEVEL, offset_type="OFFSET", segments=2,
                    profile=0.5, affect="EDGES", clamp_overlap=True)


bm.normal_update()   # the arris test reads the face normals
bevel_part(of_stones=False)
bevel_part(of_stones=True)
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])

if not ROUGH:
    # SAY what the stones' arris came out at, and hold it to BEVEL: a clamp is
    # silent, which is how it hid for two re-bakes. Measured on the first jamb
    # stone, whose front face stays a plane: its outline there is BAND wide less
    # the arris each side.
    face = [v.co.x for v in bm.verts
            if v[STONE] == N_VOUSSOIR + 1 and abs(v.co.y + STONE_T) < 1e-6]
    arris = (BAND - (max(face) - min(face))) / 2
    assert abs(arris - BEVEL) < 1e-5, f"BuildWallArch: the stones' arris is {arris:.6f}, not BEVEL {BEVEL}"
    print(f"BuildWallArch: stone arris {arris:.6f} (BEVEL {BEVEL})")

# ============================================================================
# UVs — the reveal is UNROLLED, everything else projects
# ============================================================================
# Welding, subdividing and beveling all rebuild faces, so the reveal is found
# by geometry here, the way FixArchSoffitUv does: the slab's faces that look
# along the wall and are not on a cell edge.
_, slab, _ = islands_of(bm)
reveal_faces = set()
for f in {f for v in slab for f in v.link_faces}:
    c = f.calc_center_median()
    if (abs(f.normal.y) < 0.3 and abs(c.x) < HALF - 0.01
            and 0.01 < c.z < TOP - 0.01):
        reveal_faces.add(f)
# Dominant-axis projection is only valid on box-ish geometry: on the reveal the
# normal rotates 90 degrees between springline and crown, so the dominant axis
# flips mid-surface and the texture seams. The reveal is developable, so it gets
# arc length along the outline against depth through the wall instead.
def arc_length(x, z):
    dx, dz = x, z - SPRING
    if dz >= 0.0:
        return R * math.atan2(dx, dz)
    return math.copysign(R * (math.pi / 2) - dz, dx)


uv = bm.loops.layers.uv.verify()
for face in bm.faces:
    if face in reveal_faces:
        for loop in face.loops:
            co = loop.vert.co
            loop[uv].uv = (arc_length(co.x, co.z) / TILE, co.y / TILE)
        continue
    nx, ny, nz = (abs(c) for c in face.normal)
    for loop in face.loops:
        co = loop.vert.co
        if nz >= nx and nz >= ny: p = (co.x, co.y)
        elif nx >= ny:            p = (co.y, co.z)
        else:                     p = (co.x, co.z)
        loop[uv].uv = (p[0] / TILE, p[1] / TILE)

# The FACE ORDER is the geometry's, not the ops'. The stones' own bevel call, at
# the full BEVEL, writes its new faces in an order that varies run to run - the
# vertices and the faces themselves do not (measured: 420 of 5832 triangles
# moved between two runs, every one still there) - so an unchanged script wrote
# different bytes each time, and the asset IS the script. Sorted by where each
# face is, a re-run is byte-identical again. (One whole-mesh call at the slab's
# clamp happened to order them the same every run.) BMElemSeq.sort wants a
# number from its key, so the rank goes into each face's index and the sort
# follows that.
for rank, face in enumerate(sorted(bm.faces, key=lambda f: (
        tuple(f.calc_center_median()), tuple(c for v in f.verts for c in v.co)))):
    face.index = rank
bm.faces.sort()

bm.verts.layers.int.remove(STONE)   # build bookkeeping, not an attribute to ship
mesh = bpy.data.meshes.new("wall_arch")
bm.to_mesh(mesh)
bm.free()
obj = bpy.data.objects.new("wall_arch", mesh)
bpy.context.scene.collection.objects.link(obj)

co = [v.co for v in mesh.vertices]
print(f"BuildWallArch: {'rough' if ROUGH else 'smooth'}, "
      f"{len(stone_groups)} stones, {len(co)} verts, "
      f"x {min(c.x for c in co):.3f}..{max(c.x for c in co):.3f}  "
      f"y {min(c.y for c in co):.3f}..{max(c.y for c in co):.3f}  "
      f"z {min(c.z for c in co):.3f}..{max(c.z for c in co):.3f}")

# CLOSED. Every edge wants exactly two faces - the slab and every stone. This is
# the check on the construction rather than on the reasoning above it, and the
# precondition for the recalc_face_normals calls meaning anything (the normals
# trap). The hand-listed slab failed it with ~300.
blendlib.assert_closed_shell(mesh, "BuildWallArch")

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
print(f"BuildWallArch: wrote {OUT}")
