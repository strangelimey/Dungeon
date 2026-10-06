# ============================================================================
# tools/blendlib.py - what the Blender build scripts SHARE, and it is CHECKS
# only (code-review C404 / C435).
#
#   if "__file__" in globals():   # headless; under the bridge, blender_bridge.py
#       sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # put tools/ on the path
#   import blendlib
#   blendlib.assert_closed_shell(mesh, "BuildWallArch")
#
# KEEP IT SMALL, AND KEEP GEOMETRY OUT. A script-authored prop is DEFINED BY ITS
# SCRIPT (CLAUDE.md, the asset pipeline): a revision is a constant change plus a
# re-run. A shared helper that BUILDS something would reshape every asset that
# calls it on its next re-run, silently, which strains that rule - so the
# builders (box, face, the voxel loop) stay private to each script, and only
# what MEASURES a finished mesh lives here. A check cannot move a vertex.
#
# The private copies are why this file exists at all: BuildDoorFrame.py
# diagnosed the T-junction slab and closed it with a solidity grid plus this
# assert, while BuildWallArch.py kept its hand-listed slab and shipped ~300 open
# edges in each arch (C435; tools/MeshTest.py counts 298 in the shipped files).
# One copy of the check, called by both - and by BuildFountain, whose weld it
# would have caught.
# ============================================================================
import bmesh


def open_edges(mesh):
	"""How many edges of `mesh` (a bpy Mesh) do not have exactly two faces.

	Zero is a CLOSED SHELL: every panel is there and every stone welded. It is
	also the precondition for recalc_face_normals meaning anything at all - it
	needs connectivity (CLAUDE.md, the Blender normals trap). remove_doubles
	welds coincident VERTICES, never a vertex into an edge, so a T-junction
	(one face's corner landing mid-way along another's edge) shows here as a
	pair of open edges however tightly the mesh was welded."""
	check = bmesh.new()
	check.from_mesh(mesh)
	n = sum(1 for e in check.edges if len(e.link_faces) != 2)
	check.free()
	return n


def assert_closed_shell(mesh, who):
	"""Fail the build unless `mesh` is a closed shell; say what was counted."""
	n = open_edges(mesh)
	assert n == 0, f"{who}: {n} boundary edges - the shell is not closed"
	print(f"{who}: closed shell, 0 boundary edges")
	return n
