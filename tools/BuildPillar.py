# ============================================================================
# tools/BuildPillar.py — authors the `pillar` prop and writes a UNIT-SPACE .glb.
#
#   blender --background --factory-startup --python tools\BuildPillar.py -- <out.glb>
#
# Then bring it into the game the usual way (docs/authoring-scale.md):
#   AssetBaker import-model <out.glb> <assets> pillar --raw --texture-set marble_pillar
#
# --raw MATTERS: without it import-model auto-fits the mesh to ~2 m and the
# exact wall height below is lost, leaving a gap under the ceiling.
#
# EVERYTHING HERE IS IN UNITS: 1.0 = one dungeon square (game::kUnit, currently
# 2.5 m), Z up — the glTF exporter's +Y-Up conversion turns Blender Z into the
# engine's Y and Blender -Y into the engine's +Z. The pillar stands on the floor
# at z = 0 and is exactly HEIGHT = 1.0 tall, so it meets the ceiling with no gap.
#
# THE SHAPE: a profile loft like BuildPlinth.py, but each station also names a
# SHAPE — octagon or circle. Every ring is built at the same segment count and
# only the RADIUS varies with angle, which is what lets an octagonal plinth loft
# straight into a round shaft: same topology, different silhouette, so the
# transition needs no stitching. SEGMENTS must stay a multiple of the octagon's
# 8 sides or its corners land between samples and it reads as a lumpy circle.
#
# Shading is the other half of "round": 24 facets FLAT-shaded still read as a
# polygon. The shaft band is marked smooth and everything else left flat, so the
# shaft looks turned while the plinth and abacus keep the crisp arrises that
# make them look cut.
# ============================================================================
import math
import sys

import bmesh
import bpy

SEGMENTS = 24     # ring resolution; MUST be a multiple of 8 (see header)
TILE = 0.24       # texture repeat, matching the engine's TileUvs (0.6 m/tile)
BEVEL = 0.004     # edge break, so torchlight catches the arrises
BEVEL_SEGMENTS = 2

OCT, CIRC = "oct", "circ"

# (radius, z, shape), bottom to top. Equal radii on neighbours make a straight
# run; differing radii make a chamfer, which is what reads as a moulding.
PROFILE = [
    (0.130, 0.000, OCT),    # plinth, bottom
    (0.130, 0.055, OCT),    # plinth, top
    (0.108, 0.080, OCT),    # chamfer drawing in to the shaft
    (0.098, 0.105, CIRC),   # shaft begins — the round starts here
    (0.086, 0.820, CIRC),   # shaft top, with a slight taper (entasis)
    (0.104, 0.855, OCT),    # capital flaring back out, octagonal again
    (0.128, 0.900, OCT),
    (0.128, 0.950, OCT),
    (0.136, 0.965, OCT),    # abacus oversail
    (0.136, 1.000, OCT),    # meets the ceiling — kWallH exactly
]
SHAFT_Z = (0.105, 0.820)    # the band that gets smooth shading

OUT = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else "pillar.glb"


def radius_at(angle, radius, shape, sides=8):
    """Distance to the boundary at `angle`. A circle is constant; a regular
    polygon peaks at its corners and falls to the apothem at each edge mid."""
    if shape == CIRC:
        return radius
    half = math.pi / sides
    return radius * math.cos(half) / math.cos((angle % (2 * half)) - half)


def build_pillar(profile, segments, tile, bevel, shaft_z, name="pillar"):
    bm = bmesh.new()
    uv = bm.loops.layers.uv.verify()

    rings = []
    for radius, z, shape in profile:
        ring = []
        for i in range(segments):
            a = 2 * math.pi * i / segments
            r = radius_at(a, radius, shape)
            ring.append(bm.verts.new((r * math.cos(a), r * math.sin(a), z)))
        rings.append(ring)

    # UNROLLED side UVs: u = arc length around, v = height. Dominant-axis
    # projection (TileUvs, Cube Projection) is only valid on BOX-ish geometry —
    # on a swept surface the dominant axis flips mid-surface and seams. One
    # reference radius keeps u continuous between rings of differing width
    # instead of stepping at every moulding.
    circumference = 2 * math.pi * max(r for r, _, _ in profile)

    for lower, upper, (_, z0, _), (_, z1, _) in zip(rings, rings[1:],
                                                    profile, profile[1:]):
        for i in range(segments):
            j = (i + 1) % segments
            face = bm.faces.new((lower[i], lower[j], upper[j], upper[i]))
            # u from the UNWRAPPED index (i, i+1), never the wrapped j, so the
            # closing face runs on to the full circumference rather than back
            # to 0 — otherwise the last column of texels reverses.
            u0 = circumference * i / segments / tile
            u1 = circumference * (i + 1) / segments / tile
            for loop, (u, v) in zip(face.loops,
                                    ((u0, z0), (u1, z0), (u1, z1), (u0, z1))):
                loop[uv].uv = (u, v / tile)

    for ring, flip in ((rings[0], True), (rings[-1], False)):
        face = bm.faces.new(ring[::-1] if flip else ring)  # floor points down
        for loop in face.loops:  # caps are flat and Z-facing: planar is correct
            co = loop.vert.co
            loop[uv].uv = (co.x / tile, co.y / tile)

    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])

    # Bevel in bmesh rather than as a modifier: no operator context to fight in
    # --background, and the result is baked before the shading pass below.
    if bevel > 0.0:
        bmesh.ops.bevel(bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:],
                        offset=bevel, offset_type="OFFSET",
                        segments=BEVEL_SEGMENTS, profile=0.5, affect="EDGES",
                        clamp_overlap=True)
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])

    # Smooth the SHAFT only, AFTER the bevel so the bevel's own faces are
    # classified too. Side faces only — a cap or a moulding step caught here
    # would smear the arris it is meant to define.
    z0, z1 = shaft_z
    smoothed = 0
    for face in bm.faces:
        if abs(face.normal.z) < 0.5 and z0 + 1e-4 < face.calc_center_median().z < z1 - 1e-4:
            face.smooth = True
            smoothed += 1

    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)

    lo = min(v.co.z for v in mesh.vertices)
    hi = max(v.co.z for v in mesh.vertices)
    wide = max(math.hypot(v.co.x, v.co.y) for v in mesh.vertices)
    print(f"BuildPillar: {len(mesh.vertices)} verts, {len(mesh.polygons)} faces "
          f"({smoothed} smooth-shaded shaft), z {lo:.3f}..{hi:.3f}, "
          f"max radius {wide:.3f} (units)")
    return obj


if SEGMENTS % 8:
    raise SystemExit(f"SEGMENTS must be a multiple of 8, got {SEGMENTS}")

# --- empty the factory scene ------------------------------------------------
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

build_pillar(PROFILE, SEGMENTS, TILE, BEVEL, SHAFT_Z)

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
print(f"BuildPillar: wrote {OUT}")
