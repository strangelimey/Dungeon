# ============================================================================
# tools/FixArchSoffitUv.py — re-UVs an arch so the REVEAL (the swept surface
# through the opening: curved soffit plus the straight jambs below the
# springline) stops seaming, and writes a new .glb.
#
#   blender --background --factory-startup --python tools\FixArchSoffitUv.py -- <in.glb> <out.glb>
#
# THE BUG IT FIXES: every other surface in this project is UV'd by projecting
# each face onto the plane of its DOMINANT NORMAL AXIS (ModelBaker's TileUvs,
# Blender's Cube Projection, the other scripts here). That is sound on box-ish
# geometry, but the reveal is a swept band whose normal rotates through 90
# degrees from springline to crown — so consecutive faces flip projection plane
# partway round, and the texture visibly seams and stretches at the flip.
#
# THE FIX: the reveal is a DEVELOPABLE surface, so it wants unrolling, not
# projecting —
#   u = arc length along the opening's outline, measured from the crown
#   v = depth through the wall (Blender Y)
# which is continuous the whole way round and keeps real-world texel density.
# Arc length continues past the springline down the straight jambs, so the
# curve and the straights share one unbroken mapping.
#
# The arch's centre and radius are FITTED from the mesh rather than passed in,
# because the three arches differ (the smooth one's opening is a slight ellipse,
# the rustic pair a true semicircle at a different springline). A least-squares
# circle through the reveal points above the springline is close enough on all
# three: an imperfect fit still yields a CONTINUOUS mapping, which is the whole
# point — the seam is what reads as wrong, not a few percent of stretch.
#
# Only the SLAB's reveal is remapped. The voussoir stones each present a single
# flat chord to the opening, and a flat face projects cleanly; the joints
# between stones are real joints and should look like them.
# ============================================================================
import sys

import bmesh
import bpy
import numpy as np

TILE = 0.24          # texture repeat, matching the engine's TileUvs (0.6 m)
SLAB_MIN_WIDTH = 0.8 # an island wider than this is the wall panel
FLAT_N_Y = 0.3       # |normal.y| below this = a face looking along the wall
EDGE_TOL = 0.01      # margin used to exclude the slab's outer boundary faces

args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
SRC, OUT = args[0], args[1]

# --- load -------------------------------------------------------------------
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()
bpy.ops.import_scene.gltf(filepath=SRC)

meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
if not meshes:
    raise SystemExit(f"FixArchSoffitUv: no mesh in {SRC}")
for o in meshes:
    o.select_set(True)
bpy.context.view_layer.objects.active = meshes[0]
if len(meshes) > 1:
    bpy.ops.object.join()
obj = bpy.context.view_layer.objects.active
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

bm = bmesh.new()
bm.from_mesh(obj.data)
# Weld: glTF stores attributes per corner, so an imported mesh has no shared
# vertices and every hard-edged face is its own island (see RoughenArch.py).
bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-5)
bm.verts.ensure_lookup_table()
bm.normal_update()

# --- find the slab ----------------------------------------------------------
seen, islands = set(), []
for seed in bm.verts:
    if seed.index in seen:
        continue
    stack, group = [seed], []
    seen.add(seed.index)
    while stack:
        v = stack.pop()
        group.append(v)
        for e in v.link_edges:
            o = e.other_vert(v)
            if o.index not in seen:
                seen.add(o.index)
                stack.append(o)
    islands.append(group)

slab = max(islands, key=lambda g: max(v.co.x for v in g) - min(v.co.x for v in g))
if max(v.co.x for v in slab) - min(v.co.x for v in slab) < SLAB_MIN_WIDTH:
    raise SystemExit("FixArchSoffitUv: no full-width slab island found")
slab_faces = {f for v in slab for f in v.link_faces}

x_lo, x_hi = min(v.co.x for v in slab), max(v.co.x for v in slab)
z_lo, z_hi = min(v.co.z for v in slab), max(v.co.z for v in slab)

# --- isolate the reveal -----------------------------------------------------
# Of the slab's faces looking along the wall (|n.y| small), the outer boundary
# ones sit on the cell edges; whatever is left bounds the opening.
def on_outer_boundary(face):
    c = face.calc_center_median()
    return (c.x <= x_lo + EDGE_TOL or c.x >= x_hi - EDGE_TOL
            or c.z <= z_lo + EDGE_TOL or c.z >= z_hi - EDGE_TOL)

reveal = [f for f in slab_faces
          if abs(f.normal.y) < FLAT_N_Y and not on_outer_boundary(f)]
if not reveal:
    raise SystemExit("FixArchSoffitUv: found no reveal faces")

# --- fit the arch circle ----------------------------------------------------
# Kasa fit on the reveal's upper half: x^2+z^2 = a*x + b*z + c, whence
# centre = (a/2, b/2) and R = sqrt(c + (a/2)^2 + (b/2)^2).
pts = np.array([[v.co.x, v.co.z] for f in reveal for v in f.verts])
mid_z = 0.5 * (pts[:, 1].min() + pts[:, 1].max())
upper = pts[pts[:, 1] > mid_z]
A = np.column_stack([upper[:, 0], upper[:, 1], np.ones(len(upper))])
b_vec = upper[:, 0] ** 2 + upper[:, 1] ** 2
a, b, c = np.linalg.lstsq(A, b_vec, rcond=None)[0]
cx, cz = a / 2.0, b / 2.0
R = float(np.sqrt(max(c + cx * cx + cz * cz, 1e-9)))
print(f"FixArchSoffitUv: {len(reveal)} reveal faces, "
      f"fitted centre ({cx:.3f}, {cz:.3f}) radius {R:.3f}")

def arc_length(x, z):
    """Signed distance along the opening outline, zero at the crown."""
    dx, dz = x - cx, z - cz
    if dz >= 0.0:
        return R * np.arctan2(dx, dz)              # around the curve
    side = 1.0 if dx >= 0.0 else -1.0              # on down the straight jamb
    return side * (R * (np.pi / 2.0) - dz)

# --- lay down the UVs -------------------------------------------------------
uv = bm.loops.layers.uv.verify()
reveal_set = set(reveal)
for face in bm.faces:
    if face in reveal_set:
        for loop in face.loops:
            co = loop.vert.co
            loop[uv].uv = (arc_length(co.x, co.z) / TILE, co.y / TILE)
        continue
    # everything else keeps the dominant-axis tiling projection
    nx, ny, nz = (abs(v) for v in face.normal)
    for loop in face.loops:
        co = loop.vert.co
        if nz >= nx and nz >= ny: p = (co.x, co.y)
        elif nx >= ny:            p = (co.y, co.z)
        else:                     p = (co.x, co.z)
        loop[uv].uv = (p[0] / TILE, p[1] / TILE)

bm.to_mesh(obj.data)
bm.free()

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
print(f"FixArchSoffitUv: wrote {OUT}")
