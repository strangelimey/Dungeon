# ============================================================================
# tools/BuildRock.py - authors the throwable ROCK item (ui-updates Phase 10)
# and writes it as a .glb with its texture embedded.
#
#   blender --background --factory-startup --python-exit-code 1 --python tools\BuildRock.py -- assets\models\rock.glb
#
# An item's model is read as <model>.glb with its images inside (DungeonWorld::
# ItemKindFor -> ModelMulti), so this writes one straight into assets/models;
# no import-model step. The .glb is the script's output - rerun to change it -
# and is committed by name (.gitignore makes the exception) because items load
# only .glb and a fresh clone must have it.
#
# EVERYTHING IS IN UNITS: 1.0 = one dungeon square (game::kUnit, 2.5 m), Z up
# here (the glTF exporter turns it Y up). The rock is ROCK_SIZE across its
# longest side - a fist and a half, big enough to read on the floor.
#
# THE SHAPE: an icosphere pushed out by two octaves of noise, squashed into a
# flattish lump, with its underside cut flat so it sits rather than balances.
# Flat-shaded facets kept on the noise's sharper ridges, smooth elsewhere.
#
# THE SURFACE: a 256 px texture computed here - grey-brown value noise with a
# few darker veins and paler grit - so the script needs nothing but itself.
# ============================================================================
import math
import sys

import bmesh
import bpy
from mathutils import Vector, noise

ROCK_SIZE = 0.085        # units across its longest side (~21 cm)
SQUASH = (1.0, 0.82, 0.62)  # x, y, z proportions before the noise
LUMP = 0.22              # how far the noise pushes the surface (fraction of radius)
FLAT_BOTTOM = -0.42      # where the underside is cut (fraction of radius)
TEX = 256                # texture side, px
SEED = 20261001

args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = args[0] if args else "rock.glb"

noise.seed_set(SEED)

if bpy.context.mode != "OBJECT":
    bpy.ops.object.mode_set(mode="OBJECT")
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

# --- the shape ---------------------------------------------------------------
bm = bmesh.new()
bmesh.ops.create_icosphere(bm, subdivisions=4, radius=1.0)
for v in bm.verts:
    d = v.co.normalized()
    n = (noise.noise(d * 1.7) * 0.7 + noise.noise(d * 4.1 + Vector((3.1, 7.7, 1.3))) * 0.3)
    r = 1.0 + LUMP * n
    p = Vector((d.x * SQUASH[0], d.y * SQUASH[1], d.z * SQUASH[2])) * r
    p.z = max(p.z, FLAT_BOTTOM * SQUASH[2] * 1.6)
    v.co = p

# Scale to ROCK_SIZE across the longest side, and sit it on z = 0.
xs = [v.co.x for v in bm.verts]
ys = [v.co.y for v in bm.verts]
zs = [v.co.z for v in bm.verts]
longest = max(max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs))
s = ROCK_SIZE / longest
zmin = min(zs)
for v in bm.verts:
    v.co = Vector((v.co.x * s, v.co.y * s, (v.co.z - zmin) * s))

# Smooth where the surface is gentle, faceted where the noise folds sharply.
bm.normal_update()
for f in bm.faces:
    f.smooth = True
for e in bm.edges:
    if len(e.link_faces) == 2:
        a, b = e.link_faces
        if a.normal.angle(b.normal) > math.radians(28):
            e.smooth = False

mesh = bpy.data.meshes.new("rock")
bm.to_mesh(mesh)
bm.free()
obj = bpy.data.objects.new("rock", mesh)
bpy.context.collection.objects.link(obj)
bpy.context.view_layer.objects.active = obj
obj.select_set(True)

# UVs: spherical about the centre - a rock has no seams worth hiding.
#
# BUT NO FACE MAY SPAN ONE (code-review C436). atan2 jumps from +pi to -pi on
# the rock's -X side, and a face straddling that line used to take u from each
# corner's own angle - one edge near u = 2, the other near 0 - so it ran the
# texture backwards round the whole rock. The icosphere has no segment index to
# take u from (BuildPillar.py's rule), so the face's CENTRE names the branch:
# every corner's angle is unwrapped to within half a turn of it, and a corner on
# the axis (a pole, which has no angle) takes the face's. The seam stays where
# it was, as a jump BETWEEN faces that the tileable texture cannot show.
uv = mesh.uv_layers.new(name="UVMap")
centre = Vector((0.0, 0.0, ROCK_SIZE * 0.3))
turn = 2.0 * math.pi
for poly in mesh.polygons:
    c = poly.center - centre
    branch = math.atan2(c.y, c.x)
    for li in poly.loop_indices:
        p = mesh.vertices[mesh.loops[li].vertex_index].co - centre
        a = branch
        if math.hypot(p.x, p.y) > 1e-9:
            a = math.atan2(p.y, p.x)
            a += turn * round((branch - a) / turn)
        u = 0.5 + a / turn
        w = 0.5 + math.asin(max(-1.0, min(1.0, p.z / max(p.length, 1e-6)))) / math.pi
        uv.data[li].uv = (u * 2.0, w)

# --- the surface ---------------------------------------------------------------
def value(x, y, scale, off):
    # Tileable in u: sample a cylinder so the seam at u = 0/1 matches.
    a = x * 2.0 * math.pi
    return noise.noise(Vector((math.cos(a) * scale, math.sin(a) * scale, y * scale * 2.0 + off)))


img = bpy.data.images.new("rock_albedo", TEX, TEX, alpha=False)
pixels = [0.0] * (TEX * TEX * 4)
for j in range(TEX):
    y = j / TEX
    for i in range(TEX):
        x = i / TEX
        base = 0.5 + 0.30 * value(x, y, 2.0, 0.0) + 0.15 * value(x, y, 6.0, 11.0) \
            + 0.08 * value(x, y, 18.0, 23.0)
        vein = abs(value(x, y, 3.5, 41.0))
        dark = 0.55 if vein < 0.05 else 1.0          # thin dark veins
        grit = 1.08 if value(x, y, 40.0, 57.0) > 0.45 else 1.0  # pale flecks
        g = max(0.0, min(1.0, base)) * dark * grit
        k = (j * TEX + i) * 4
        # Grey with a little warmth, as dungeon stone is under torchlight.
        pixels[k + 0] = 0.20 + 0.34 * g
        pixels[k + 1] = 0.19 + 0.31 * g
        pixels[k + 2] = 0.17 + 0.27 * g
        pixels[k + 3] = 1.0
img.pixels = pixels
img.pack()

mat = bpy.data.materials.new("rock")
mat.use_nodes = True
nodes = mat.node_tree.nodes
bsdf = nodes.get("Principled BSDF")
tex = nodes.new("ShaderNodeTexImage")
tex.image = img
mat.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
bsdf.inputs["Roughness"].default_value = 0.92
bsdf.inputs["Metallic"].default_value = 0.0
mesh.materials.append(mat)

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB", use_selection=True)
print("wrote", OUT, "verts", len(mesh.vertices))
