# ============================================================================
# tools/RoughenArch.py — weathers the stones of an existing arch into a rough,
# dry-laid variant, and writes a new UNIT-SPACE .glb.
#
#   blender --background --factory-startup --python tools\RoughenArch.py -- <in.glb> <out.glb>
#
# Then:
#   AssetBaker import-model <out.glb> <assets> wall_arch_rough --raw --texture-set stacked_stone
#
# WHY DERIVE RATHER THAN REBUILD: the source arch carries decisions that were
# made by eye and are expensive to rediscover — the voussoir taper that closes
# the joints, the flattened springer bottoms, the 0.97 inset that hides the
# slab's cut edge inside the stonework. Re-modelling from a profile would throw
# all of that away. This only perturbs what is already correct.
#
# The stones are found as CONNECTED COMPONENTS: the arch was joined into one
# mesh, but each stone is still its own island, and the wall slab is the only
# island spanning the full cell. So the slab is identified by its width and
# left completely alone — it has to keep meeting the neighbouring walls.
#
# Roughness comes from three passes, in increasing subtlety:
#   1. per-stone JITTER  — small rotation/scale about each stone's own centre,
#      so the courses stop looking machined. This is most of the effect.
#   2. surface NOISE     — smooth (not per-vertex random) displacement, so faces
#      read as undulating rather than spiky.
#   3. a heavier BEVEL   — chipped arrises that catch torchlight.
#
# INNER-FACE BUDGET: the source sits 0.97 of the opening radius, leaving roughly
# 0.008 of clearance before a stone would poke through the slab's cut edge into
# the passage. Jitter and noise both eat into that, so the defaults below stay
# well inside it. Raise them and the slab edge starts showing again.
# ============================================================================
import random
import sys

import bmesh
import bpy
from mathutils import Matrix, Vector, noise

SEED = 20260722
ROT_DEG = 2.0      # per-stone tilt, degrees on each axis
SCALE_JIT = 0.025  # per-stone size variation
NOISE_AMP = 0.0035 # surface displacement, units (~9 mm)
NOISE_FREQ = 26.0  # higher = finer, lumpier detail
SUBDIV = 1         # edge cuts before displacing; each step ~4x the faces
TILE = 0.24        # texture repeat, matching the engine's TileUvs
# No second bevel: the source arch was already bevelled before export, so
# beveling again only shaves slivers off the existing chamfers while multiplying
# the vertex count. Roughness here comes from jitter and noise instead.

SLAB_MIN_WIDTH = 0.8  # an island wider than this is the wall, not a stone

args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
SRC, OUT = args[0], args[1]

random.seed(SEED)
noise.seed_set(SEED)

# --- load the source arch ---------------------------------------------------
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()
bpy.ops.import_scene.gltf(filepath=SRC)

meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
if not meshes:
    raise SystemExit(f"RoughenArch: no mesh in {SRC}")
for obj in meshes:
    obj.select_set(True)
bpy.context.view_layer.objects.active = meshes[0]
if len(meshes) > 1:
    bpy.ops.object.join()
obj = bpy.context.view_layer.objects.active
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

bm = bmesh.new()
bm.from_mesh(obj.data)

# WELD FIRST. glTF stores attributes per-corner, so importing splits a vertex
# everywhere normals or UVs differ — which means every hard-edged face arrives
# as its own disconnected island. Without this the island search below finds
# hundreds of "stones" that are really single faces, and jittering them blows
# the model apart.
before = len(bm.verts)
bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-5)
print(f"RoughenArch: welded {before} -> {len(bm.verts)} verts")
bm.verts.ensure_lookup_table()

# --- split into islands -----------------------------------------------------
# Re-derived rather than cached, because subdividing invalidates held vertex
# references (a stale BMVert raises ReferenceError the moment you touch it).
def stone_islands(mesh):
    mesh.verts.ensure_lookup_table()
    seen, islands = set(), []
    for seed_vert in mesh.verts:
        if seed_vert.index in seen:
            continue
        stack, group = [seed_vert], []
        seen.add(seed_vert.index)
        while stack:
            v = stack.pop()
            group.append(v)
            for edge in v.link_edges:
                other = edge.other_vert(v)
                if other.index not in seen:
                    seen.add(other.index)
                    stack.append(other)
        islands.append(group)
    # The wall panel is the only island spanning the cell; leave it untouched so
    # it keeps meeting the neighbouring walls.
    stones = [g for g in islands
              if max(v.co.x for v in g) - min(v.co.x for v in g) < SLAB_MIN_WIDTH]
    return islands, stones


islands, stones = stone_islands(bm)
print(f"RoughenArch: {len(islands)} islands — "
      f"{len(islands) - len(stones)} slab, {len(stones)} stones")

# --- 1. per-stone jitter ----------------------------------------------------
for group in stones:
    centre = sum((v.co for v in group), Vector()) / len(group)
    rot = (Matrix.Rotation(random.uniform(-ROT_DEG, ROT_DEG) * 0.0174533, 3, "X")
           @ Matrix.Rotation(random.uniform(-ROT_DEG, ROT_DEG) * 0.0174533, 3, "Y")
           @ Matrix.Rotation(random.uniform(-ROT_DEG, ROT_DEG) * 0.0174533, 3, "Z"))
    scale = 1.0 + random.uniform(-SCALE_JIT, SCALE_JIT)
    for v in group:
        v.co = centre + (rot @ (v.co - centre)) * scale

# --- 2. surface noise -------------------------------------------------------
# Subdivide first: a bare box has 8 corners, so displacing it just skews the
# box. Cutting each edge gives enough vertices for the noise to read as
# weathering across a face.
stone_edges = {e for group in stones for v in group for e in v.link_edges}
bmesh.ops.subdivide_edges(
    bm, edges=list(stone_edges), cuts=SUBDIV, use_grid_fill=True)

# Re-find the islands on the subdivided mesh — the pre-subdivision references
# are dead, and the island membership is unchanged by cutting edges.
_, stones = stone_islands(bm)
bm.normal_update()
for group in stones:
    for v in group:
        n = v.normal
        if n.length_squared < 1e-9:
            continue
        v.co += n.normalized() * (noise.noise(v.co * NOISE_FREQ) * NOISE_AMP)

bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])

# --- world-aligned tiling UVs (the bevel and subdivision invalidated them) ---
uv = bm.loops.layers.uv.verify()
for face in bm.faces:
    nx, ny, nz = (abs(c) for c in face.normal)
    for loop in face.loops:
        co = loop.vert.co
        if nz >= nx and nz >= ny: p = (co.x, co.y)
        elif nx >= ny:            p = (co.y, co.z)
        else:                     p = (co.x, co.z)
        loop[uv].uv = (p[0] / TILE, p[1] / TILE)

bm.to_mesh(obj.data)
bm.free()

co = [v.co for v in obj.data.vertices]
print(f"RoughenArch: {len(co)} verts, "
      f"x {min(c.x for c in co):.3f}..{max(c.x for c in co):.3f}  "
      f"y {min(c.y for c in co):.3f}..{max(c.y for c in co):.3f}  "
      f"z {min(c.z for c in co):.3f}..{max(c.z for c in co):.3f}")

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
print(f"RoughenArch: wrote {OUT}")
