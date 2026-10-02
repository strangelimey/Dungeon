# ============================================================================
# tools/BuildWallTorchBracket.py - the bare bracket a wall torch leaves when its
# torch is taken (spell-updates; fixtures.cat [sconce] `empty_model`).
#
# The game's wall_torch.gltf is the vendor's Holder + Torch MERGED into one mesh
# and normalized by import-model (scale, ground, wall-back, lift), so the bracket
# cannot be cut out of it. It comes from the source FBX instead, where Holder is
# its own object - but it must land EXACTLY where it sits in wall_torch.gltf, or
# the empty sconce jumps when its torch is taken. So this FITS the transform:
# the FBX's Holder + Torch, together, are mapped onto wall_torch.gltf by the
# uniform scale and translation that align their bounds (import-model applies
# nothing else for this entry), the fit is CHECKED vertex by vertex, and the
# same transform is applied to the Holder alone.
#
# Run (Blender 5.x, headless):
#   blender --background --factory-startup --python tools/BuildWallTorchBracket.py -- \
#       <assets>/models/wall_torch.gltf <S_Torch_01a.fbx> <out.glb>
# then:  AssetBaker import-model <out.glb> <assets> wall_torch_bracket --raw
#                                --texture-set wall_torch
# The FBX lives in the archive: OneDrive\DungeonAssets\fab\props\
# medieval-stylized-torch\extracted\source\S_Torch_01a.fbx.
# ============================================================================
import bpy
import sys
from mathutils import Matrix, Vector
from mathutils.kdtree import KDTree

argv = sys.argv[sys.argv.index("--") + 1:]
TARGET, FBX, OUT = argv[0], argv[1], argv[2]
TOLERANCE = 0.002  # units: how far a fitted vertex may sit from the target's


def purge():
	bpy.ops.wm.read_factory_settings(use_empty=True)


def world_verts(objs):
	out = []
	for o in objs:
		out.extend(o.matrix_world @ v.co for v in o.data.vertices)
	return out


def bounds(points):
	lo = Vector((min(p.x for p in points), min(p.y for p in points), min(p.z for p in points)))
	hi = Vector((max(p.x for p in points), max(p.y for p in points), max(p.z for p in points)))
	return lo, hi


purge()
# The target, as the game has it. (The glTF importer leaves a stray helper
# object beside what it reads: only meshes count.)
bpy.ops.import_scene.gltf(filepath=TARGET)
target = [o for o in bpy.context.scene.objects if o.type == "MESH"]
tv = world_verts(target)
for o in list(bpy.context.scene.objects):
	bpy.data.objects.remove(o, do_unlink=True)

bpy.ops.import_scene.fbx(filepath=FBX)
src = {o.name: o for o in bpy.context.scene.objects if o.type == "MESH"}
for o in list(bpy.context.scene.objects):
	if o.name not in ("Holder", "Torch"):
		bpy.data.objects.remove(o, do_unlink=True)
holder, torch = src["Holder"], src["Torch"]
sv = world_verts([holder, torch])

tlo, thi = bounds(tv)
slo, shi = bounds(sv)
text, sext = thi - tlo, shi - slo
scales = [text[i] / sext[i] for i in range(3)]
s = sum(scales) / 3.0
if max(scales) - min(scales) > 0.01 * s:
	raise SystemExit(f"BuildWallTorchBracket: bounds do not scale uniformly {scales} - "
					 "the import is not a scale + offset of the source any more")
offset = tlo - slo * s
fit = Matrix.Translation(offset) @ Matrix.Diagonal((s, s, s, 1.0))

# The check: every fitted source vertex must have a target vertex where it
# lands (the merge kept every vertex of both pieces).
tree = KDTree(len(tv))
for i, p in enumerate(tv):
	tree.insert(p, i)
tree.balance()
worst = max(tree.find(fit @ p)[2] for p in sv)
print(f"BuildWallTorchBracket: scale {s:.5f}, offset {tuple(round(c, 4) for c in offset)}, "
	  f"worst vertex miss {worst:.5f}")
if worst > TOLERANCE:
	raise SystemExit(f"BuildWallTorchBracket: fit misses by {worst:.4f} > {TOLERANCE} - refusing")

# The bracket alone, carried by the same transform, applied into its data.
bpy.data.objects.remove(torch, do_unlink=True)
holder.matrix_world = fit @ holder.matrix_world
bpy.context.view_layer.objects.active = holder
holder.select_set(True)
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB", use_selection=True,
						  export_yup=True)
print(f"BuildWallTorchBracket: wrote {OUT}")
