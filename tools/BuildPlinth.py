# ============================================================================
# tools/BuildPlinth.py — authors the `plinth` prop and writes a UNIT-SPACE .glb.
#
#   blender --background --factory-startup --python tools\BuildPlinth.py -- <out.glb>
#
# Then bring it into the game the usual way (docs/authoring-scale.md):
#   AssetBaker import-model <out.glb> <assets> plinth --raw --texture-set marble_pillar
#
# EVERYTHING HERE IS IN UNITS: 1.0 = one dungeon square (game::kUnit, currently
# 2.5 m), Z up — the glTF exporter's +Y-Up conversion turns Blender Z into the
# engine's Y and Blender -Y into the engine's +Z. A prop stands on the floor at
# z = 0 and is centred on the cell in X/Y.
#
# The shape is a square LOFT: a list of (half-width, height) stations, lofted
# into side quads and capped. That is the whole vocabulary needed for classical
# architecture — plinths, column bases, cornices, fountain rims — so the next
# prop is a new PROFILE rather than new code.
# ============================================================================
import sys

import bmesh
import bpy

# --- the profile ------------------------------------------------------------
# (half-width, z), bottom to top. Equal-width neighbours make a straight block;
# differing widths make a chamfer, which is what reads as a moulding.
BASE_HW, DIE_HW = 0.18, 0.14   # 0.90 m and 0.70 m across
PROFILE = [
    (BASE_HW, 0.000),  # base block, bottom
    (BASE_HW, 0.060),  # base block, top
    (0.155,   0.085),  # lower chamfer, drawing in to the die
    (DIE_HW,  0.100),
    (DIE_HW,  0.340),  # the die (plain shaft) — 0.24 tall
    (0.155,   0.365),  # upper chamfer, flaring back out
    (BASE_HW, 0.390),
    (BASE_HW, 0.420),  # cap slab, top — 1.05 m overall
]

BEVEL = 0.004    # edge break, so torchlight catches the arrises
SEGMENTS = 2
TILE = 0.24      # texture repeat, matching the engine's TileUvs (0.6 m/tile)

OUT = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else "plinth.glb"

# --- empty the factory scene ------------------------------------------------
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

# --- loft the profile into a closed solid -----------------------------------
bm = bmesh.new()

rings = []
for half, z in PROFILE:
    rings.append([
        bm.verts.new((-half, -half, z)),
        bm.verts.new((half, -half, z)),
        bm.verts.new((half, half, z)),
        bm.verts.new((-half, half, z)),
    ])

for lower, upper in zip(rings, rings[1:]):
    for i in range(4):
        j = (i + 1) % 4
        bm.faces.new((lower[i], lower[j], upper[j], upper[i]))

bm.faces.new(rings[0][::-1])  # floor (reversed so it points down)
bm.faces.new(rings[-1])       # top

bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])

# --- break the edges --------------------------------------------------------
# Done in bmesh rather than as a modifier: no operator context to fight in
# --background, and the result is baked before the UVs are laid down.
if BEVEL > 0.0:
    bmesh.ops.bevel(
        bm,
        geom=bm.verts[:] + bm.edges[:] + bm.faces[:],
        offset=BEVEL,
        offset_type="OFFSET",
        segments=SEGMENTS,
        profile=0.5,
        affect="EDGES",
        clamp_overlap=True,
    )
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])

# --- world-aligned tiling UVs ----------------------------------------------
# Each face projects onto the plane of its dominant normal axis, so the texel
# density stays even across differently-sized faces instead of one stretched
# tile per face. Same idea as ModelBaker's TileUvs and the Cube Projection used
# on the hand-built arch.
uv = bm.loops.layers.uv.verify()
for face in bm.faces:
    nx, ny, nz = (abs(c) for c in face.normal)
    for loop in face.loops:
        co = loop.vert.co
        if nz >= nx and nz >= ny:   p = (co.x, co.y)  # up/down faces
        elif nx >= ny:              p = (co.y, co.z)  # x-facing
        else:                       p = (co.x, co.z)  # y-facing
        loop[uv].uv = (p[0] / TILE, p[1] / TILE)

mesh = bpy.data.meshes.new("plinth")
bm.to_mesh(mesh)
bm.free()

obj = bpy.data.objects.new("plinth", mesh)
bpy.context.scene.collection.objects.link(obj)

lo = min(v.co.z for v in mesh.vertices)
hi = max(v.co.z for v in mesh.vertices)
wide = max(abs(v.co.x) for v in mesh.vertices)
print(f"BuildPlinth: {len(mesh.vertices)} verts, "
      f"z {lo:.3f}..{hi:.3f}, half-width {wide:.3f} (units)")

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
print(f"BuildPlinth: wrote {OUT}")
