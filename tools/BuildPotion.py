# ============================================================================
# tools/BuildPotion.py - authors the three POTION CONTAINERS (transparency
# Phase 2) and writes each as a .glb with its texture embedded.
#
#   blender --background --factory-startup --python-exit-code 1 --python tools\BuildPotion.py -- assets\models
#
# writes potion_vial.glb, potion_bottle.glb and potion_flask.glb into that
# folder. An item's model is read as <model>.glb with its images inside
# (DungeonWorld::ItemKindFor -> ModelMulti), so there is no import-model step;
# the files are this script's output - rerun to change them - and are committed
# by name (.gitignore makes the exception), as rock.glb is.
#
# THE STRENGTH LADDER IS THE SIZE (Michael): a minor potion comes in the VIAL,
# a standard one in the small BOTTLE, a greater one in the round FLASK. The
# fire and poison bombs use the same three.
#
# EVERYTHING IS IN UNITS: 1.0 = one dungeon square (game::kUnit, 2.5 m), Z up
# here (the glTF exporter turns it Y up). The tables below are in CENTIMETRES
# for readability and converted once (CM).
#
# TWO PARTS, TWO MATERIALS, because the engine marks see-through PER PART:
#   GLASS - alphaMode BLEND, which the loader reads as MaterialParams::
#           transparent. Base colour RGB is the TINT (near white = clear) and
#           its alpha the DENSITY (scene.hlsl GlassOutput). No texture.
#   CORK  - opaque, a small speckled texture computed here.
# The exporter's own alpha handling has moved between Blender versions, so the
# glass material's alphaMode and colour are WRITTEN INTO THE .glb AFTERWARDS
# (PatchGlass) rather than trusted to it - and then read back and checked.
#
# THE GLASS IS A REVOLVED CLOSED SECTION (BuildFountain.py's idea): up the
# outside from the base, over the lip, and back down the inside to the floor of
# the cavity. The inside is the outside OFFSET inward by the wall thickness
# along each station's normal, so a profile is authored once. Both walls exist
# on purpose: Phase 3 builds the liquid from the bottle's shape, and the inner
# wall is where that liquid meets the glass.
#
# WINDING IS THE CONTRACT (CLAUDE.md, the Blender normals trap): no
# recalc_face_normals. Every face is checked against the normal it MUST have -
# the right-hand normal of the section's direction of travel, which points out
# of the glass on the outside, up over the lip and into the cavity on the
# inside - and flipped where it disagrees. That is a per-face rule computed
# from the profile, not a guess from connectivity.
# ============================================================================
import json
import math
import os
import struct
import sys

import bmesh
import bpy
from mathutils import Vector, noise

# The tables are a real vial, bottle and flask; the game draws them at GAME_SIZE
# times that, because at true size (8-18 cm on a 2.5 m square) a clear bottle
# all but vanished on the floor. The rock set the precedent: "big enough to
# read on the floor" beats the tape measure.
GAME_SIZE = 1.5  # Michael picked 1.5 over 2 (2026-10-03)
CM = 0.004 * GAME_SIZE  # one (table) centimetre in units (1 unit = 250 cm)
SEGMENTS = 32         # facets around the revolve
WALL = 0.12           # glass thickness, cm
SHARP_DEG = 40.0      # an edge sharper than this keeps a hard crease
TEX = 128             # cork texture side, px
SEED = 20261002

# Glass: a faint cool tint. scene.hlsl reads RGB as the tint and alpha as the
# density; a clear pane is ~0.1. FROSTED (Michael chose it from four side-by-
# sides, 2026-10-03): perfectly clear glass all but vanished on a dark floor,
# so the density is raised and the surface roughened, which spreads the torch's
# highlight across the glass instead of a pinpoint.
GLASS_TINT = (0.88, 0.95, 1.0)
GLASS_DENSITY = 0.25
GLASS_ROUGHNESS = 0.35

# Outer profiles, (radius, height) in cm, from the base on the axis up to the
# top of the lip. The last station is the lip's outer top edge; the section
# then crosses the lip and comes back down the offset inside.
VIAL = [
	(0.00, 0.00),
	(0.45, 0.08),
	(0.72, 0.35),   # a rounded foot, still able to stand
	(0.80, 0.80),
	(0.80, 6.40),   # the tube
	(0.70, 6.75),   # a slight waist under the lip
	(0.70, 7.15),
	(0.86, 7.30),   # the lip, rolled out
	(0.86, 7.60),
]
BOTTLE = [
	(0.00, 0.00),
	(2.55, 0.00),   # a flat base
	(2.90, 0.30),
	(3.00, 1.00),
	(3.00, 5.80),   # the body
	(2.80, 7.30),
	(2.20, 8.40),   # round shoulders
	(1.30, 9.20),
	(1.00, 9.80),
	(1.00, 11.00),  # the neck
	(1.18, 11.20),
	(1.18, 11.70),  # the lip
]


def flask_profile():
	"""A round-bellied flask on a small flat base, with a long neck."""
	radius, base_r, neck_r = 6.0, 2.6, 1.15
	centre = math.sqrt(radius * radius - base_r * base_r)  # the base lies on the circle
	pts = [(0.0, 0.0), (base_r, 0.0)]
	a0 = math.asin(-centre / radius)                       # the base's angle on the circle
	a1 = math.acos(neck_r / radius)                        # where the belly meets the neck
	steps = 14
	for i in range(1, steps + 1):
		a = a0 + (a1 - a0) * i / steps
		pts.append((radius * math.cos(a), centre + radius * math.sin(a)))
	top = pts[-1][1]
	pts += [
		(neck_r, top + 5.0),   # the neck
		(1.35, top + 5.25),
		(1.35, top + 5.75),    # the lip
	]
	return pts


CONTAINERS = {
	"potion_vial": VIAL,
	"potion_bottle": BOTTLE,
	"potion_flask": flask_profile(),
}

args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT_DIR = args[0] if args else "."
# Look overrides, for trying a variant without editing the constants:
#   --tint r,g,b   --density d   --roughness r
for i, a in enumerate(args):
	if a == "--tint":
		GLASS_TINT = tuple(float(c) for c in args[i + 1].split(","))
	elif a == "--density":
		GLASS_DENSITY = float(args[i + 1])
	elif a == "--roughness":
		GLASS_ROUGHNESS = float(args[i + 1])
noise.seed_set(SEED)


# --- the section ---------------------------------------------------------------
def station_normals(profile):
	"""Outward 2D normal at each station: the average of its segments' right-hand
	normals ((dz, -dr) for travel (dr, dz)), which on an outer profile running up
	from the axis points away from the glass."""
	n = []
	for i in range(len(profile)):
		acc = [0.0, 0.0]
		for j, k in ((i - 1, i), (i, i + 1)):
			if j < 0 or k >= len(profile):
				continue
			dr = profile[k][0] - profile[j][0]
			dz = profile[k][1] - profile[j][1]
			length = math.hypot(dr, dz)
			acc[0] += dz / length
			acc[1] += -dr / length
		length = math.hypot(acc[0], acc[1])
		n.append((acc[0] / length, acc[1] / length))
	return n


def closed_section(outer):
	"""Outer profile + the inside offset by WALL, as one closed (r, z) section:
	up the outside, across the lip, down the inside to the cavity floor."""
	normals = station_normals(outer)
	inner = []
	for (r, z), (nr, nz) in zip(outer, normals):
		inner.append((max(r - nr * WALL, 0.0), z - nz * WALL))
	inner[0] = (0.0, inner[0][1])     # the cavity floor meets the axis
	# The top station of the inside sits level with the lip's top.
	inner[-1] = (inner[-1][0], outer[-1][1])
	return outer + inner[::-1]


# --- the revolve -----------------------------------------------------------------
def revolve(bm, section, expected):
	"""Revolve a closed (r, z) section in cm about Z, emitting faces wound so each
	one's normal agrees with `expected(i)` - the outward 2D normal of segment i."""
	rings = []
	for r, z in section:
		if r <= 1e-6:
			rings.append([bm.verts.new((0.0, 0.0, z * CM))])
			continue
		rings.append([bm.verts.new((r * CM * math.sin(t), -r * CM * math.cos(t), z * CM))
					  for t in (2.0 * math.pi * s / SEGMENTS for s in range(SEGMENTS))])
	flipped = 0
	for i, (lower, upper) in enumerate(zip(rings, rings[1:])):
		er, ez = expected(i)
		for s in range(SEGMENTS):
			s2 = (s + 1) % SEGMENTS
			a = lower[s if len(lower) > 1 else 0]
			b = lower[s2 if len(lower) > 1 else 0]
			c = upper[s2 if len(upper) > 1 else 0]
			d = upper[s if len(upper) > 1 else 0]
			verts = [v for k, v in enumerate((a, b, c, d)) if v not in (a, b, c, d)[:k]]
			if len(verts) < 3:
				continue
			face = bm.faces.new(verts)
			face.normal_update()
			# The expected normal in 3D at this face's middle angle.
			t = 2.0 * math.pi * (s + 0.5) / SEGMENTS
			want = Vector((er * math.sin(t), -er * math.cos(t), ez))
			if face.normal.dot(want) < 0.0:
				face.normal_flip()
				flipped += 1
	return flipped


def segment_normals(section):
	out = []
	for (r0, z0), (r1, z1) in zip(section, section[1:]):
		dr, dz = r1 - r0, z1 - z0
		length = math.hypot(dr, dz)
		out.append((dz / length, -dr / length))
	return out


def build_part(name, section, material):
	bm = bmesh.new()
	seg = segment_normals(section)
	revolve(bm, section, lambda i: seg[i])
	# Smooth everywhere, creased where the profile turns sharply (the lip's
	# edges, the base) so the highlight does not smear round a corner.
	for f in bm.faces:
		f.smooth = True
	for e in bm.edges:
		if len(e.link_faces) == 2:
			fa, fb = e.link_faces
			if fa.normal.angle(fb.normal) > math.radians(SHARP_DEG):
				e.smooth = False
	# UVs: unrolled - u round the axis, v up it - in units of the part's height,
	# so the cork's speckle is not stretched.
	#
	# U COMES FROM THE FACE'S OWN SEGMENT (BuildPillar.py's rule, code-review
	# C436). Wrapping each corner's angle into [0, 1) put the last segment's two
	# edges at u = 31/32 and u = 0, so that column ran BACKWARDS across the whole
	# texture. A face's centre names its segment, every corner's angle is
	# unwrapped onto that segment's branch, and a corner on the axis (no angle
	# of its own) takes the segment's. The last column now runs on to u = 1,
	# which the cork's texture - tileable round the cork - meets seamlessly.
	uv = bm.loops.layers.uv.verify()
	height = max(z for _, z in section) * CM
	turn = 2.0 * math.pi
	for f in bm.faces:
		centre = f.calc_center_median()
		segment = math.atan2(centre.x, -centre.y) % turn
		for loop in f.loops:
			co = loop.vert.co
			a = segment
			if math.hypot(co.x, co.y) > 1e-9:
				a = math.atan2(co.x, -co.y)
				a += turn * round((segment - a) / turn)
			loop[uv].uv = (a / turn, co.z / max(height, 1e-6))
	mesh = bpy.data.meshes.new(name)
	bm.to_mesh(mesh)
	bm.free()
	mesh.materials.append(material)
	obj = bpy.data.objects.new(name, mesh)
	bpy.context.collection.objects.link(obj)
	return obj


# --- materials -------------------------------------------------------------------
def glass_material():
	mat = bpy.data.materials.new("glass")
	mat.use_nodes = True
	bsdf = mat.node_tree.nodes.get("Principled BSDF")
	bsdf.inputs["Base Color"].default_value = (*GLASS_TINT, 1.0)
	bsdf.inputs["Alpha"].default_value = GLASS_DENSITY
	bsdf.inputs["Roughness"].default_value = GLASS_ROUGHNESS
	bsdf.inputs["Metallic"].default_value = 0.0
	return mat


def cork_material():
	img = bpy.data.images.new("cork_albedo", TEX, TEX, alpha=False)
	px = [0.0] * (TEX * TEX * 4)
	for j in range(TEX):
		for i in range(TEX):
			a = 2.0 * math.pi * i / TEX   # tileable round the cork
			p = Vector((math.cos(a) * 3.0, math.sin(a) * 3.0, j / TEX * 3.0))
			g = 0.5 + 0.35 * noise.noise(p * 2.0) + 0.15 * noise.noise(p * 7.0 + Vector((5, 1, 9)))
			pit = 0.7 if noise.noise(p * 11.0 + Vector((2, 8, 4))) > 0.42 else 1.0
			g = max(0.0, min(1.0, g)) * pit
			k = (j * TEX + i) * 4
			px[k + 0] = 0.36 + 0.22 * g
			px[k + 1] = 0.24 + 0.15 * g
			px[k + 2] = 0.13 + 0.08 * g
			px[k + 3] = 1.0
	img.pixels = px
	img.pack()
	mat = bpy.data.materials.new("cork")
	mat.use_nodes = True
	nodes = mat.node_tree.nodes
	bsdf = nodes.get("Principled BSDF")
	tex = nodes.new("ShaderNodeTexImage")
	tex.image = img
	mat.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
	bsdf.inputs["Roughness"].default_value = 0.9
	bsdf.inputs["Metallic"].default_value = 0.0
	return mat


# --- the .glb fix-up ---------------------------------------------------------------
def patch_glass(path):
	"""Sets the glass material's alphaMode and baseColorFactor in the written .glb
	(the exporter's alpha rules vary by version), then reads it back to check."""
	with open(path, "rb") as f:
		data = bytearray(f.read())
	json_len = struct.unpack_from("<I", data, 12)[0]
	doc = json.loads(data[20:20 + json_len])
	found = False
	for m in doc.get("materials", []):
		if m.get("name") == "glass":
			m["alphaMode"] = "BLEND"
			pbr = m.setdefault("pbrMetallicRoughness", {})
			pbr["baseColorFactor"] = [*GLASS_TINT, GLASS_DENSITY]
			pbr["metallicFactor"] = 0.0
			pbr["roughnessFactor"] = GLASS_ROUGHNESS
			found = True
		else:
			m.pop("alphaMode", None)   # the cork stays opaque whatever was written
	if not found:
		raise SystemExit(f"BuildPotion: no 'glass' material in {path}")
	text = json.dumps(doc, separators=(",", ":")).encode("utf-8")
	text += b" " * ((4 - len(text) % 4) % 4)  # a GLB chunk is 4-byte aligned
	rest = data[20 + json_len:]
	out = bytearray()
	out += struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(text) + len(rest))
	out += struct.pack("<II", len(text), 0x4E4F534A)
	out += text
	out += rest
	with open(path, "wb") as f:
		f.write(out)
	# Read back: the claim is only true if the file says it.
	with open(path, "rb") as f:
		back = f.read()
	n = struct.unpack_from("<I", back, 12)[0]
	mats = {m["name"]: m for m in json.loads(back[20:20 + n])["materials"]}
	assert mats["glass"]["alphaMode"] == "BLEND", path
	assert "alphaMode" not in mats["cork"], path


# --- build ---------------------------------------------------------------------------
os.makedirs(OUT_DIR, exist_ok=True)
for name, outer in CONTAINERS.items():
	if bpy.context.mode != "OBJECT":
		bpy.ops.object.mode_set(mode="OBJECT")
	bpy.ops.object.select_all(action="SELECT")
	bpy.ops.object.delete()
	for block in (bpy.data.meshes, bpy.data.materials, bpy.data.images):
		for item in list(block):
			block.remove(item)

	section = closed_section(outer)
	glass = build_part(name + "_glass", section, glass_material())

	# The cork: a short taper pushed into the neck, standing proud of the lip.
	# Its radius is the NARROWEST inside of the neck over the depth it plugs -
	# the lip flares out, and a cork sized to the flare would poke through the
	# glass below it.
	lip_r, lip_z = outer[-1]
	plug = max(0.6, lip_z * 0.08)          # how deep it goes into the neck
	proud = max(0.45, lip_z * 0.05)        # how far it stands above the lip
	inside = section[len(outer):]
	neck_inner = min(r for r, z in inside if z >= lip_z - plug - 0.3)
	cork_section = [
		(0.0, lip_z - plug),
		(neck_inner * 0.96, lip_z - plug),
		(neck_inner * 1.12, lip_z + proud * 0.85),
		(neck_inner * 1.02, lip_z + proud),
		(0.0, lip_z + proud),
	]
	cork = build_part(name + "_cork", cork_section, cork_material())

	for obj in (glass, cork):
		obj.select_set(True)
	path = os.path.join(OUT_DIR, name + ".glb")
	bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True)
	patch_glass(path)
	verts = len(glass.data.vertices) + len(cork.data.vertices)
	print(f"BuildPotion: wrote {path} - {verts} verts, "
		  f"{(lip_z + proud) * GAME_SIZE:.1f} cm tall in the game "
		  f"({max(r for r, _ in outer) * 2 * GAME_SIZE:.1f} across)")
