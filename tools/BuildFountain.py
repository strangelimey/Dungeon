# ============================================================================
# tools/BuildFountain.py — authors the two fountains and writes UNIT-SPACE .glb.
#
#   blender --background --factory-startup --python tools\BuildFountain.py -- <out.glb>
#   blender --background --factory-startup --python tools\BuildFountain.py -- <out.glb> --wall
#
# Then:
#   AssetBaker import-model <out.glb> <assets> fountain_round --raw --texture-set marble_pillar
#   AssetBaker import-model <out.glb> <assets> fountain_wall  --raw --texture-set marble_pillar
#
# EVERYTHING IS IN UNITS: 1.0 = one dungeon square (game::kUnit, 2.5 m), Z up.
# The exporter's +Y-Up conversion turns Blender Z into the engine's Y and
# Blender -Y into the engine's +Z, which is why the wall fountain backs onto
# y = 0 and reaches into -Y: that is the room side once imported.
#
# This generalises BuildPlinth.py's lofter. That one walked a table of
# (half-width, height) stations emitting four corners per ring — a square loft.
# Here a ring takes any number of segments and any angular sweep, so the same
# profile table gives a square plinth, a turned column, a full basin, or the
# half basin a wall fountain needs. Profiles are still the whole vocabulary:
# equal radii between neighbouring stations make a cylinder, differing radii
# make a chamfer or a flare.
#
# A profile is a closed section through the solid — up the outside, over the
# rim and back down the inside — so revolving it produces a watertight basin
# rather than a surface needing caps. Stations at radius 0 sit on the axis and
# emit a triangle fan instead of quads.
# ============================================================================
import math
import sys

import bmesh
import bpy

SEGMENTS = 40      # facets around a full revolve
TILE = 0.24        # texture repeat, matching the engine's TileUvs (0.6 m)
BEVEL = 0.004      # arris break, so torchlight catches the edges

# --- free-standing fountain: a round basin with a central spout ------------
# (radius, height), a closed section: base out to the foot, up the outside,
# over the rim, down the inside, and back to the axis along the bowl floor.
BASIN = [
    (0.000, 0.000),  # base, on the axis
    (0.340, 0.000),  # foot — 1.7 m across, leaving margin inside the square
    (0.340, 0.045),
    (0.310, 0.070),  # chamfer in off the foot
    (0.300, 0.180),  # bowl wall
    (0.325, 0.205),  # flare out to the rim
    (0.325, 0.245),  # rim, top outer — 0.61 m, a sitting height
    (0.280, 0.245),  # rim, top inner
    (0.265, 0.215),
    (0.260, 0.090),  # inner wall
    (0.000, 0.070),  # bowl floor, back to the axis
]
SPOUT = [
    (0.000, 0.070),  # standing on the bowl floor
    (0.080, 0.070),
    (0.070, 0.100),
    (0.050, 0.280),  # shaft
    (0.110, 0.310),  # upper bowl, underside
    (0.130, 0.350),
    (0.130, 0.380),  # upper rim — 0.95 m overall
    (0.100, 0.380),
    (0.085, 0.350),
    (0.000, 0.335),
]

# --- wall fountain ----------------------------------------------------------
WALL_BASIN = [
    (0.000, 0.240),
    (0.210, 0.240),
    (0.225, 0.275),
    (0.225, 0.360),  # rim outer
    (0.190, 0.360),  # rim inner
    (0.180, 0.290),
    (0.000, 0.275),  # basin floor
]
BACK_X, BACK_T = 0.260, 0.035    # back slab: half-width, thickness off the wall
BACK_Z0, BACK_Z1 = 0.240, 0.740  # spans the basin up past eye level (0.62)
SPOUT_Z = 0.545                  # where the water would issue

args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = args[0] if args else "fountain.glb"
WALL = "--wall" in args

bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()
bm = bmesh.new()


def revolve(profile, segments=SEGMENTS, theta0=0.0, theta1=2.0 * math.pi,
            closed=True):
    """Revolve a (radius, height) profile about the Z axis.

    `closed` wraps the last segment back to the first (a full turn). A partial
    sweep leaves the two end sections open, which is what the wall fountain
    wants — they land flat against the wall at y = 0 and are capped there.
    Stations at radius 0 collapse to a single vertex on the axis.
    """
    count = segments if closed else segments + 1
    rings = []
    for r, z in profile:
        if r <= 1e-6:
            rings.append([bm.verts.new((0.0, 0.0, z))])          # on the axis
            continue
        ring = []
        for s in range(count):
            t = theta0 + (theta1 - theta0) * (s / segments)
            ring.append(bm.verts.new((r * math.sin(t), -r * math.cos(t), z)))
        rings.append(ring)

    for lower, upper in zip(rings, rings[1:]):
        for s in range(segments if closed else segments):
            s2 = (s + 1) % len(lower) if len(lower) > 1 else 0
            u2 = (s + 1) % len(upper) if len(upper) > 1 else 0
            a = lower[s if len(lower) > 1 else 0]
            b = lower[s2]
            c = upper[u2]
            d = upper[s if len(upper) > 1 else 0]
            # A station on the axis degenerates the quad into a triangle.
            if a is b:
                bm.faces.new((a, c, d))
            elif c is d:
                bm.faces.new((a, b, c))
            else:
                bm.faces.new((a, b, c, d))
    return rings


def box(x0, x1, y0, y1, z0, z1):
    lo = [bm.verts.new(p) for p in
          ((x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0))]
    hi = [bm.verts.new(p) for p in
          ((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1))]
    for i in range(4):
        bm.faces.new((lo[i], lo[(i + 1) % 4], hi[(i + 1) % 4], hi[i]))
    bm.faces.new(lo[::-1])
    bm.faces.new(hi)


if not WALL:
    revolve(BASIN)
    revolve(SPOUT)
else:
    # Half a basin, swept from one side of the wall round to the other so its
    # open ends land in the plane y = 0 and it bulges into the room (-Y).
    rings = revolve(WALL_BASIN, theta0=-math.pi / 2, theta1=math.pi / 2,
                    segments=SEGMENTS // 2, closed=False)
    # Cap those open ends flat against the wall.
    for idx in (0, -1):
        section = [ring[idx] if len(ring) > 1 else ring[0] for ring in rings]
        section = [v for i, v in enumerate(section)
                   if i == 0 or v is not section[i - 1]]
        if len(section) >= 3:
            face = bm.faces.new(section)
            if idx == 0:
                face.normal_flip()

    box(-BACK_X, BACK_X, -BACK_T, 0.0, BACK_Z0, BACK_Z1)   # back slab
    box(-0.045, 0.045, -0.090, -BACK_T, SPOUT_Z, SPOUT_Z + 0.055)  # spout
    box(-0.075, 0.075, -0.055, -BACK_T, SPOUT_Z + 0.055, SPOUT_Z + 0.080)  # hood

bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-5)
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
bmesh.ops.bevel(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:],
                offset=BEVEL, offset_type="OFFSET", segments=2,
                profile=0.5, affect="EDGES", clamp_overlap=True)
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])

# --- UVs --------------------------------------------------------------------
# A revolved surface has the same failing as the arch reveal: its normal turns
# right through the compass, so a dominant-axis projection flips plane partway
# round and seams. Anything whose normal is mostly horizontal is therefore
# unrolled — u = arc length about the axis, v = height — and only the near
# top/bottom faces keep the flat projection.
uv = bm.loops.layers.uv.verify()
for face in bm.faces:
    if abs(face.normal.z) < 0.7:
        for loop in face.loops:
            co = loop.vert.co
            r = math.hypot(co.x, co.y)
            u = math.atan2(co.x, -co.y) * max(r, 1e-4)
            loop[uv].uv = (u / TILE, co.z / TILE)
    else:
        for loop in face.loops:
            co = loop.vert.co
            loop[uv].uv = (co.x / TILE, co.y / TILE)

mesh = bpy.data.meshes.new("fountain")
bm.to_mesh(mesh)
bm.free()
bpy.context.scene.collection.objects.link(bpy.data.objects.new("fountain", mesh))

co = [v.co for v in mesh.vertices]
print(f"BuildFountain: {'wall' if WALL else 'round'}, {len(co)} verts, "
      f"x {min(c.x for c in co):.3f}..{max(c.x for c in co):.3f}  "
      f"y {min(c.y for c in co):.3f}..{max(c.y for c in co):.3f}  "
      f"z {min(c.z for c in co):.3f}..{max(c.z for c in co):.3f}")

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
print(f"BuildFountain: wrote {OUT}")
