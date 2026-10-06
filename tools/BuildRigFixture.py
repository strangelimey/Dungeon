# BuildRigFixture.py - a tiny rigged FBX whose import gives TWO actions sharing
# one take name: the fixture tools\ConvertMeshTest.ps1 converts.
#
# Run:  blender --background --factory-startup --python-exit-code 1 \
#           --python tools\BuildRigFixture.py -- <out.fbx>
#
# WHY. ConvertMesh --keep-rig de-duplicates an FBX's takes: the importer makes
# one action per take on EVERY animated object ("Armature|Walk" on the rig and
# "Body|Walk" on the mesh), and the converter keeps the SKELETAL one - the one
# whose curves drive pose bones (its is_skeletal). That test only runs when two
# actions share a base name, and the packs converted so far never had one, so
# it read `action.fcurves` - gone in Blender 5.x - for as long as it existed
# without anyone seeing the AttributeError (code-review C434). This builds the
# shape that reaches it: a two-bone rig with a skinned column, the rig keyed on
# a pose bone and the column keyed on its own location, exported as one take.
#
# The fixture CHECKS ITSELF: it re-imports what it wrote, through the importer
# ConvertMesh uses, and refuses (exit 1) unless the import holds two actions
# with one base name - one skeletal, one not - so a judge built on it can never
# pass for want of reaching the code it is about.
import os
import sys

import bpy


def all_fcurves(action):
	# The flat list on an older Blender, else every channelbag of a slotted
	# action (ConvertMesh's and ImportAnimLibrary's iterator).
	legacy = getattr(action, "fcurves", None)
	if legacy is not None:
		yield from legacy
		return
	for layer in action.layers:
		for strip in layer.strips:
			for cbag in strip.channelbags:
				yield from cbag.fcurves


def build():
	bpy.ops.wm.read_factory_settings(use_empty=True)
	scene = bpy.context.scene
	scene.frame_start, scene.frame_end = 1, 20

	# The rig: Root (z 0..0.5) and Tip (z 0.5..1) above it.
	bpy.ops.object.armature_add(location=(0.0, 0.0, 0.0))
	arm = bpy.context.object
	arm.name = "Armature"
	bpy.ops.object.mode_set(mode="EDIT")
	bones = arm.data.edit_bones
	root = bones[0]
	root.name = "Root"
	root.head, root.tail = (0.0, 0.0, 0.0), (0.0, 0.0, 0.5)
	tip = bones.new("Tip")
	tip.head, tip.tail = (0.0, 0.0, 0.5), (0.0, 0.0, 1.0)
	tip.parent = root
	bpy.ops.object.mode_set(mode="OBJECT")

	# The body: a column a square tall, its lower half on Root, upper on Tip.
	bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, 0.5))
	body = bpy.context.object
	body.name = "Body"
	body.scale = (0.2, 0.2, 1.0)
	bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
	low = body.vertex_groups.new(name="Root")
	high = body.vertex_groups.new(name="Tip")
	for v in body.data.vertices:
		(high if v.co.z > 0.5 else low).add([v.index], 1.0, "REPLACE")
	mod = body.modifiers.new("Armature", "ARMATURE")
	mod.object = arm
	body.parent = arm

	# A stowaway beside it: an UNSKINNED mesh with a take of its own (the
	# reference planes vendors leave in a pack). The skinned body's own object
	# keys do not survive the FBX round trip - the exporter files it under the
	# rig's skin - so this is what the importer names alike: "Marker|Scene"
	# beside "Armature|Scene". ConvertMesh drops the mesh (bake_rig) but its
	# action stays in bpy.data.actions, where the de-duplication meets it.
	bpy.ops.mesh.primitive_plane_add(size=0.2, location=(0.5, 0.0, 0.0))
	marker = bpy.context.object
	marker.name = "Marker"

	# The take: Tip bends on the rig; the marker slides (object location) - the
	# non-skeletal action that shares the take's name.
	pose_tip = arm.pose.bones["Tip"]
	pose_tip.rotation_mode = "XYZ"
	for frame, angle in ((1, 0.0), (20, 0.6)):
		pose_tip.rotation_euler = (angle, 0.0, 0.0)
		pose_tip.keyframe_insert("rotation_euler", frame=frame)
	for frame, x in ((1, 0.5), (20, 0.7)):
		marker.location = (x, 0.0, 0.0)
		marker.keyframe_insert("location", frame=frame)
	scene.frame_set(1)
	return arm, body


def export(path):
	os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
	bpy.ops.object.select_all(action="SELECT")
	bpy.ops.export_scene.fbx(filepath=path, use_selection=False, add_leaf_bones=False,
							 bake_anim=True, bake_anim_use_all_actions=False,
							 bake_anim_use_nla_strips=False, bake_anim_force_startend_keying=True)
	print(f"BuildRigFixture: wrote {path}")


def verify(path):
	"""Re-import as ConvertMesh does and demand two actions with one base name,
	one of them skeletal and one not."""
	bpy.ops.wm.read_factory_settings(use_empty=True)
	try:
		bpy.ops.import_scene.fbx(filepath=path)
	except AttributeError:
		bpy.ops.wm.fbx_import(filepath=path)
	by_base = {}
	for action in bpy.data.actions:
		base = action.name.rsplit("|", 1)[-1].strip() or action.name
		skeletal = any(fc.data_path.startswith("pose.bones") for fc in all_fcurves(action))
		by_base.setdefault(base, []).append((action.name, skeletal))
	print(f"BuildRigFixture: the import holds {sorted((a, s) for v in by_base.values() for a, s in v)}")
	for base, acts in by_base.items():
		kinds = {s for _, s in acts}
		if len(acts) >= 2 and kinds == {True, False}:
			print(f"BuildRigFixture: take '{base}' - {len(acts)} actions, one skeletal: the fixture reaches is_skeletal")
			return True
	print("BuildRigFixture: REFUSED - no take with a skeletal and a non-skeletal action; "
		  "ConvertMesh's de-duplication would not run on this file")
	return False


def main():
	argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
	if len(argv) != 1:
		raise SystemExit("BuildRigFixture: usage -- <out.fbx>")
	out = os.path.abspath(argv[0])
	build()
	export(out)
	if not verify(out):
		sys.exit(1)


if __name__ == "__main__":
	main()
