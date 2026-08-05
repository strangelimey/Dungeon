# ============================================================================
# tools/BuildStairsSpiral.py — authors BOTH halves of the spiral staircase and
# writes UNIT-SPACE .glb files.
#
#   blender --background --factory-startup --python tools\BuildStairsSpiral.py -- <outdir>
#
# Emits stairs_spiral.glb (the UP half) and stairs_spiral_down.glb (the DOWN
# half). Then, for each:
#   AssetBaker import-model <outdir>\<name>.glb <assets> <name> --raw --texture-set wall_stone
# --raw MATTERS: without it import-model auto-fits to ~2 m and the exact storey
# height below is lost.
#
# WHY A SPIRAL EXISTS AT ALL. A straight flight climbing kWallH inside ONE cell
# is forced to 45 degrees — the run is the cell and there is nothing more to
# give. Winding the flight measures the run along the ARC instead, which buys a
# ~31 degree pitch in the same square. That is the entire justification; if the
# tread count or radius changes, check the pitch the script prints.
#
# BOTH HALVES FROM ONE GENERATOR. The engine draws a cross-level stair twice —
# a descending shaft on the upper level, a rising flight on the lower one — so
# the two meshes are the same helix seen from either side of a floor. `sign`
# flips it: +1 climbs to the ceiling and opens a hole in it, -1 descends from
# the floor and opens a hole in that.
#
# HEADROOM is the thing to preserve when editing. The collar's inner radius is
# OUTSIDE the tread radius, so no tread ever has ceiling above it — the climb is
# open to the well the whole way. A straight flight cannot do this, which is
# why its ceiling opening has to be positioned by hand.
#
# UNITS throughout: 1.0 = one dungeon square (2.5 m), Blender Z up.
# ============================================================================
import math
import sys

import bmesh
import bpy

CELL_HALF = 0.5      # the cell it must fit inside
HEIGHT = 1.0         # kWallH — one storey
TREADS = 12          # one full turn per storey
R_OUT = 0.46         # outer tread radius (1.15 m), clear of the walls
R_NEWEL = 0.06       # central post
# Treads reach INSIDE the post rather than stopping on its surface. At exactly
# R_NEWEL the tread's inner arc and the post's skin are coincident, and the two
# z-fight — which renders as vertical stripes banding up the column, not as the
# gaps in the stone it looks like. Biting 3 cm in buries the face instead.
R_TREAD_IN = R_NEWEL - 0.012
R_WELL = 0.47        # shaft radius — OUTSIDE R_OUT, see HEADROOM above
SLAB = 0.016         # collar thickness
SHAFT = 0.52         # stairwell beyond the storey (up half only)
RING = 48            # arc subdivisions around the collar / shaft
ARC = 4              # arc subdivisions per tread
TILE = 0.24          # texture repeat, matching the engine's TileUvs

OUT_DIR = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else "."

D_ANG = 2.0 * math.pi / TREADS
D_H = HEIGHT / TREADS
# Tread thickness is exactly one rise, so each wedge's underside meets the top
# of the step below and the flight is a SOLID helix of stone. Thinner slabs
# leave daylight between the treads and read as a modern metal spiral.
TREAD_T = D_H


def pt(r, a, z):
    return (r * math.cos(a), r * math.sin(a), z)


def square_r(a):
    """Centre-to-boundary distance of the SQUARE cell at angle `a` — the collar
    has to reach the corners, which a circle never does."""
    return CELL_HALF / max(abs(math.cos(a)), abs(math.sin(a)))


def build(sign, name):
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()
    bm = bmesh.new()
    uv = bm.loops.layers.uv.verify()

    # WINDING IS THE CONTRACT HERE, not recalc_face_normals. Every quad is built
    # from fresh verts, so the mesh has no shared edges and no connectivity —
    # and recalc_face_normals needs connectivity to tell inside from outside. Run
    # on disconnected faces it flips them arbitrarily, which lit a quarter of the
    # newel as if it faced away and read as dark slots cut down the column.
    #
    # So each face states its own orientation: quads are wound counter-clockwise
    # seen from OUTSIDE, and `flip` reverses that for the far side of a solid.
    def quad(pts, uvs, smooth=False, flip=False):
        if flip:
            pts, uvs = pts[::-1], uvs[::-1]
        f = bm.faces.new([bm.verts.new(p) for p in pts])
        f.smooth = smooth
        for loop, t in zip(f.loops, uvs):
            loop[uv].uv = t

    def planar(pts):  # flat, Z-facing: planar projection is correct
        return [(p[0] / TILE, p[1] / TILE) for p in pts]

    # --- the helix -----------------------------------------------------------
    # Up: tread i's top at +(i+1)*D_H, climbing to the ceiling.
    # Down: tread i's top at -(i+1)*D_H, the first step already below the floor.
    for i in range(TREADS):
        a0 = i * D_ANG
        top = sign * (i + 1) * D_H
        bot = top - TREAD_T if sign > 0 else top + TREAD_T
        lo, hi = min(top, bot), max(top, bot)
        for s in range(ARC):
            b0 = a0 + D_ANG * s / ARC
            b1 = a0 + D_ANG * (s + 1) / ARC
            # This winding gives +Z, so the underside flips.
            for z, down in ((hi, False), (lo, True)):
                p = [pt(R_TREAD_IN, b0, z), pt(R_OUT, b0, z),
                     pt(R_OUT, b1, z), pt(R_TREAD_IN, b1, z)]
                quad(p, planar(p), flip=down)
            # Outer and inner arcs: UNROLLED (u = arc length, v = height). A
            # swept surface flips its dominant axis mid-curve, so planar
            # projection would seam — the rule from CLAUDE.md. This winding
            # points radially OUT, so the inner arc (facing the newel) flips.
            for r, inward in ((R_OUT, False), (R_TREAD_IN, True)):
                p = [pt(r, b0, lo), pt(r, b1, lo), pt(r, b1, hi), pt(r, b0, hi)]
                u0, u1 = r * b0 / TILE, r * b1 / TILE
                quad(p, [(u0, lo / TILE), (u1, lo / TILE),
                         (u1, hi / TILE), (u0, hi / TILE)],
                     smooth=True, flip=inward)
        # Radial ends = the visible riser faces. This winding faces the -angle
        # direction, which is outward for the start end and inward for the far.
        for a, far_end in ((a0, False), (a0 + D_ANG, True)):
            p = [pt(R_TREAD_IN, a, lo), pt(R_OUT, a, lo),
                 pt(R_OUT, a, hi), pt(R_TREAD_IN, a, hi)]
            quad(p, [(R_TREAD_IN / TILE, lo / TILE), (R_OUT / TILE, lo / TILE),
                     (R_OUT / TILE, hi / TILE), (R_TREAD_IN / TILE, hi / TILE)],
                 flip=far_end)

    # --- newel post, spanning the storey -------------------------------------
    n0, n1 = (0.0, HEIGHT) if sign > 0 else (-HEIGHT, 0.0)
    for s in range(RING):
        b0 = 2 * math.pi * s / RING
        b1 = 2 * math.pi * (s + 1) / RING
        quad([pt(R_NEWEL, b0, n0), pt(R_NEWEL, b1, n0),
              pt(R_NEWEL, b1, n1), pt(R_NEWEL, b0, n1)],
             [(R_NEWEL * b0 / TILE, n0 / TILE), (R_NEWEL * b1 / TILE, n0 / TILE),
              (R_NEWEL * b1 / TILE, n1 / TILE), (R_NEWEL * b0 / TILE, n1 / TILE)],
             smooth=True)
        # CAP BOTH ENDS. Left open, the post is a hollow tube you can see down —
        # it reads as the column being broken rather than as a missing face.
        for z, down in ((n1, False), (n0, True)):
            p = [pt(0.0001, b0, z), pt(R_NEWEL, b0, z),
                 pt(R_NEWEL, b1, z), pt(0.0001, b1, z)]
            quad(p, planar(p), flip=down)

    # --- collar + shaft, built as ONE CLOSED SHELL ---------------------------
    # The cell's whole floor/ceiling block is skipped (stairs.cat `hole`), so
    # the collar closes everything the shaft does not open, corners included —
    # a square slab with a round hole, and a tube rising out of that hole,
    # capped at the far end.
    #
    # It has to be CLOSED (slab faces + square outer rim + tube + cap) because
    # recalc_face_normals can only orient a manifold shell. Leave an edge open
    # and it flips faces inward, which lights them as if they faced away: dark
    # bands that read as slots cut in the stone rather than as a normals bug.
    c0 = HEIGHT if sign > 0 else -SLAB
    c1 = c0 + SLAB
    far = HEIGHT + SHAFT if sign > 0 else -HEIGHT  # the capped end of the tube
    t_lo, t_hi = min(c0, far), max(c1, far)
    for s in range(RING):
        b0 = 2 * math.pi * s / RING
        b1 = 2 * math.pi * (s + 1) / RING
        q0, q1 = square_r(b0), square_r(b1)
        for z, down in ((c1, False), (c0, True)):  # slab top and bottom
            p = [pt(R_WELL, b0, z), pt(q0, b0, z), pt(q1, b1, z), pt(R_WELL, b1, z)]
            quad(p, planar(p), flip=down)
        # square outer rim of the slab (faces out of the cell, into the rock)
        p = [pt(q0, b0, c0), pt(q1, b1, c0), pt(q1, b1, c1), pt(q0, b0, c1)]
        quad(p, [(q0 * b0 / TILE, c0 / TILE), (q1 * b1 / TILE, c0 / TILE),
                 (q1 * b1 / TILE, c1 / TILE), (q0 * b0 / TILE, c1 / TILE)])
        # The shaft tube, run right THROUGH the slab so the two read as one
        # solid. It is seen from the AXIS side, so it faces inward: flip.
        quad([pt(R_WELL, b0, t_lo), pt(R_WELL, b1, t_lo),
              pt(R_WELL, b1, t_hi), pt(R_WELL, b0, t_hi)],
             [(R_WELL * b0 / TILE, t_lo / TILE), (R_WELL * b1 / TILE, t_lo / TILE),
              (R_WELL * b1 / TILE, t_hi / TILE), (R_WELL * b0 / TILE, t_hi / TILE)],
             smooth=True, flip=True)
        # Cap the well (up) / lay the landing (down) so you cannot see out. The
        # cap is seen from BELOW and the landing from ABOVE, hence the sign.
        # NO FURTHER TREADS EITHER WAY — the flight lands flush with the storey
        # it reaches and the next turn belongs to the cell on the next level.
        p = [pt(0.0001, b0, far), pt(R_WELL, b0, far),
             pt(R_WELL, b1, far), pt(0.0001, b1, far)]
        quad(p, planar(p), flip=(sign > 0))

    # NO recalc_face_normals and NO remove_doubles — see the note on `quad`.
    # Winding above is the single source of truth for orientation.
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)

    zs = [v.co.z for v in mesh.vertices]
    xs = [abs(v.co.x) for v in mesh.vertices] + [abs(v.co.y) for v in mesh.vertices]
    print(f"{name}: {len(mesh.vertices)} verts, {len(mesh.polygons)} faces, "
          f"z {min(zs):+.3f}..{max(zs):+.3f}, half-extent {max(xs):.3f} u")
    return obj


mid_r = 0.5 * (R_NEWEL + R_OUT)
print(f"BuildStairsSpiral: rise {D_H * 2.5:.3f} m, arc run at mid-radius "
      f"{mid_r * D_ANG * 2.5:.3f} m -> pitch "
      f"{math.degrees(math.atan2(D_H, mid_r * D_ANG)):.1f} deg "
      f"(a straight flight in one cell is 45)")

for sign, name in ((1, "stairs_spiral"), (-1, "stairs_spiral_down")):
    build(sign, name)
    out = f"{OUT_DIR}\\{name}.glb"
    bpy.ops.export_scene.gltf(filepath=out, export_format="GLB")
    print(f"BuildStairsSpiral: wrote {out}")
