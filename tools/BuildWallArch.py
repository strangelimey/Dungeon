# ============================================================================
# tools/BuildWallArch.py — authors a stone passage arch and writes a UNIT-SPACE
# .glb. One script, two looks:
#
#   blender --background --factory-startup --python tools\BuildWallArch.py -- <out.glb>
#   blender --background --factory-startup --python tools\BuildWallArch.py -- <out.glb> --rough
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
# The slab's opening is CONSTRUCTED, not booleaned: the wall face is emitted as
# panels around the void and a fan above the arc. That keeps the topology
# predictable and the reveal a clean strip to unwrap.
# ============================================================================
import math
import random
import sys

import bmesh
import bpy
from mathutils import Matrix, Vector, noise

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
reveal_faces = set()   # unwrapped separately — see the UV section
stone_groups = []      # one vertex list per stone, for the weathering pass


def quad(a, b, c, d, reveal=False):
    verts = [bm.verts.new(p) for p in (a, b, c, d)]
    face = bm.faces.new(verts)
    if reveal:
        reveal_faces.add(face)
    return face


def arc_point(theta, radius):
    """A point on the opening, theta = 0 at the crown, +/-pi/2 at the springline."""
    return (radius * math.sin(theta), SPRING + radius * math.cos(theta))


# ============================================================================
# the wall slab
# ============================================================================
thetas = [(-math.pi / 2) + math.pi * i / ARC_SEGS for i in range(ARC_SEGS + 1)]
outline = [arc_point(t, R) for t in thetas]

for sy in (-SLAB_T, SLAB_T):
    n = 1 if sy > 0 else -1
    # full-height panels either side of the opening
    for x0, x1 in ((-HALF, -R), (R, HALF)):
        a, b = (x0, sy, 0.0), (x1, sy, 0.0)
        c, d = (x1, sy, TOP), (x0, sy, TOP)
        quad(a, b, c, d) if n > 0 else quad(b, a, d, c)
    # the spandrel: a fan from the arc up to the ceiling
    for (x0, z0), (x1, z1) in zip(outline, outline[1:]):
        a, b = (x0, sy, z0), (x1, sy, z1)
        c, d = (x1, sy, TOP), (x0, sy, TOP)
        quad(a, b, c, d) if n > 0 else quad(b, a, d, c)

# the reveal: the swept surface through the opening
for z0, z1 in ((0.0, SPRING),):
    quad((-R, -SLAB_T, z0), (-R, -SLAB_T, z1), (-R, SLAB_T, z1), (-R, SLAB_T, z0),
         reveal=True)
    quad((R, SLAB_T, z0), (R, SLAB_T, z1), (R, -SLAB_T, z1), (R, -SLAB_T, z0),
         reveal=True)
for (x0, z0), (x1, z1) in zip(outline, outline[1:]):
    quad((x0, -SLAB_T, z0), (x1, -SLAB_T, z1), (x1, SLAB_T, z1), (x0, SLAB_T, z0),
         reveal=True)

# outer edges, so the slab is closed where it meets the neighbouring walls
quad((-HALF, -SLAB_T, 0.0), (-HALF, -SLAB_T, TOP), (-HALF, SLAB_T, TOP), (-HALF, SLAB_T, 0.0))
quad((HALF, SLAB_T, 0.0), (HALF, SLAB_T, TOP), (HALF, -SLAB_T, TOP), (HALF, -SLAB_T, 0.0))
quad((-HALF, -SLAB_T, TOP), (-HALF, SLAB_T, TOP), (HALF, SLAB_T, TOP), (HALF, -SLAB_T, TOP))
for x0, x1 in ((-HALF, -R), (R, HALF)):
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
if ROUGH:
    # Subdivide first: a bare box has 8 corners, so noise on it only skews the
    # box rather than weathering its faces.
    _, slab, stones = islands_of(bm)
    bmesh.ops.subdivide_edges(
        bm, edges=list({e for g in stones for v in g for e in v.link_edges}),
        cuts=SUBDIV, use_grid_fill=True)
    _, slab, stones = islands_of(bm)

    for group in stones:
        centre = sum((v.co for v in group), Vector()) / len(group)
        rot = (Matrix.Rotation(math.radians(random.uniform(-ROT_DEG, ROT_DEG)), 3, "X")
               @ Matrix.Rotation(math.radians(random.uniform(-ROT_DEG, ROT_DEG)), 3, "Y")
               @ Matrix.Rotation(math.radians(random.uniform(-ROT_DEG, ROT_DEG)), 3, "Z"))
        scale = 1.0 + random.uniform(-SCALE_JIT, SCALE_JIT)
        for v in group:
            v.co = centre + (rot @ (v.co - centre)) * scale

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
# UVs — the reveal is UNROLLED, everything else projects
# ============================================================================
# The reveal set built during construction is long dead (welding, subdividing
# and beveling all rebuild faces), so re-find it the same way FixArchSoffitUv
# does: the slab's faces that look along the wall and are not on a cell edge.
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

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
print(f"BuildWallArch: wrote {OUT}")
