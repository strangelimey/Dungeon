# ============================================================================
# tools/MeshTest.py - the script-built meshes are CLOSED, face OUT, and run
# their u ONE WAY (code-review C435, C436, C404).
#
#   powershell -File tools\MeshTest.ps1 [-SelfTest]     (finds Blender, runs this)
#   blender --background --factory-startup --python-exit-code 1 ^
#       --python tools\MeshTest.py -- [--selftest] [file ...]
#
# It runs INSIDE Blender because the outward test casts rays, and mathutils'
# BVHTree is the ray caster to hand; but it reads no .blend and imports nothing
# through Blender. It parses the SHIPPED files itself - assets\models as the
# game loads them, after the exporter and AssetBaker import-model have had their
# turn - so what it judges is what draws, not the script's idea of it. Given
# file paths it judges those instead (an old copy out of git, a scratch bake).
#
# Every file's positions are WELDED (exactly equal floats are one point: the
# exporter splits a vertex wherever its normal or UV differs, and a split one
# keeps its position bit for bit), and its triangles fall into ISLANDS - the
# sets joined by shared edges. A stone, a cork, a box is an island.
#
#   closed   every welded edge has exactly two triangles. Zero is a closed
#            shell; a T-junction or a missing panel shows as open edges. The
#            hand-listed arch slab had 298 (C435), and the round fountain 160 -
#            its spout welded to its basin through one axis vertex.
#   outward  every triangle of a CLOSED island faces out of it - CLAUDE.md's
#            winding test, made general: from just off the triangle's face, a
#            ray along its winding normal crosses the island's own surface an
#            EVEN number of times when it leaves the solid, an odd number when
#            it was pointed in. Three slightly different rays vote, so one that
#            grazes an edge cannot decide it. Only the island's own triangles
#            count: the arch's stones stand INTO its slab, and a stone face
#            buried in the slab still faces out of its stone.
#   normals  every corner's shading normal agrees with its triangle's winding:
#            an inward normal lights a face as though it were turned away, which
#            reads as a dark slot and survives every A/B but a measurement
#            (CLAUDE.md, the Blender normals trap).
#   uspan    no face SPANS the u range BACKWARDS (C436). Take two triangles
#            sharing an edge that is continuous in UV (both ends carry the same
#            UVs on both sides) where the surface is smooth (their normals within
#            60 degrees). On the surface their third corners lie on opposite
#            sides of the edge; if in UV they lie on the SAME side the map FOLDS
#            there, and the two faces' u-spans must then be alike - within
#            SPAN_RATIO (10x). An atan2 seam made exactly the unlike case: the
#            column straddling it had one edge at the top of the u range and the
#            other at the bottom, so it spanned the circumference backwards
#            against its neighbour's one segment - 31x on a 32-segment potion,
#            ~500x on the round fountain. A fold between faces of LIKE span is a
#            mirror, not a reversal, and is left alone: a glass cavity floor that
#            dips to its wall (v is height there, and height turns back), the
#            corner where a wall fountain's cap turns into its basin. So are
#            sharp creases (a blade's two faces mirror one projection) and
#            slivers (a bevel's near-zero triangles have no meaningful normal).
#            Each file also reports its widest face's u-span as a share of its
#            u range: a column spanning the circumference is nearly all of it.
#
# --selftest plants three faults in every file, one at a time, and demands that
# EXACTLY the checks each one is for fail:
#   flip   one triangle of a closed island wound the other way   -> outward, normals
#   hole   one triangle of a closed island removed               -> closed, and
#          outward too where that was the file's only closed island, since a
#          hole in the only shell leaves nothing to ray-test
#   fold   one face's third corner flung far along its neighbour's side of an
#          edge uspan judged, so it folds there spanning 50x as far  -> uspan
#
# Exit 0 PASS (or under --selftest every planted fault caught, nothing else); 1
# FAIL; 2 nothing judged. Last line (tools\Verdict.ps1 reads it):
#   meshtest RESULT=PASS|FAIL checks=N failures=M self_test=0|1 [caught=0|1]
# ============================================================================
import base64
import json
import math
import os
import struct
import sys

from mathutils import Matrix, Quaternion, Vector
from mathutils.bvhtree import BVHTree

TOOL = "meshtest"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODELS = os.path.join(ROOT, "assets", "models")

# The re-run assets of code-review batch 95 and the scripts it touched. A row is
# (file under assets/models, closed shell required, what makes it).
ASSETS = [
	("wall_arch_rustic.gltf", True, "BuildWallArch.py + import-model --raw"),
	("wall_arch_rough.gltf", True, "BuildWallArch.py --rough + import-model --raw"),
	("fountain_round.gltf", True, "BuildFountain.py + import-model --raw"),
	("fountain_wall.gltf", True, "BuildFountain.py --wall + import-model --raw"),
	("potion_vial.glb", True, "BuildPotion.py"),
	("potion_bottle.glb", True, "BuildPotion.py"),
	("potion_flask.glb", True, "BuildPotion.py"),
	("rock.glb", True, "BuildRock.py"),
	("door_frame.gltf", True, "BuildDoorFrame.py + import-model --raw"),
	("door_frame_slide.gltf", True, "BuildDoorFrame.py --right + import-model --raw"),
	("door_frame_split.gltf", True, "BuildDoorFrame.py --left --right + import-model --raw"),
	("door_frame_rise.gltf", True, "BuildDoorFrame.py --head + import-model --raw"),
]
# NOT statue_sentinel.gltf (BuildStatue.py), though batch 95 touched its script:
# measured, its hood's stitch faces - the rims either side of the face opening,
# the hem and the peak, 108 triangles - are wound INTO the cloth while the two
# surfaces they join face out. Real, older than this check, and fixing it changes
# how the statue looks, so it is reported rather than fixed here;
# `MeshTest.ps1 -Files assets\models\statue_sentinel.gltf -Detail` shows it. (Its
# `closed` fails there too, by design: the robe and head are open lofts that
# meet, so a row for it would ask closed = False.)
CLOSED = {name: closed for name, closed, _ in ASSETS}

SMOOTH_COS = 0.5        # uspan judges an edge whose faces turn by less than 60 degrees
SPAN_RATIO = 10.0       # ... and fails a fold whose two faces' u-spans differ more than this
SLIVER = 1e-4           # ... and skips a triangle whose area is below this x its longest edge squared
VOTES = 3               # rays per triangle in the outward test
NO_FOLD, MIRROR, REVERSED = 0, 1, 2  # what Mesh.fold finds at a judged edge


# --- reading a glTF ---------------------------------------------------------------
COMPONENTS = {5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f"}
WIDTHS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def read_gltf(path):
	"""The file's document and its buffers' bytes: a .glb's BIN chunk, a data
	URI (AssetBaker's .gltf) or a file beside it."""
	with open(path, "rb") as f:
		data = f.read()
	chunk = None
	if data[:4] == b"glTF":
		n = struct.unpack_from("<I", data, 12)[0]
		doc = json.loads(data[20:20 + n])
		at = 20 + n
		if at + 8 <= len(data):
			size = struct.unpack_from("<I", data, at)[0]
			chunk = data[at + 8:at + 8 + size]
	else:
		doc = json.loads(data)
	buffers = []
	for b in doc.get("buffers", []):
		uri = b.get("uri")
		if uri is None:
			buffers.append(chunk)
		elif uri.startswith("data:"):
			buffers.append(base64.b64decode(uri.split(",", 1)[1]))
		else:
			with open(os.path.join(os.path.dirname(path), uri), "rb") as f:
				buffers.append(f.read())
	return doc, buffers


def accessor(doc, buffers, index):
	a = doc["accessors"][index]
	view = doc["bufferViews"][a["bufferView"]]
	data = buffers[view["buffer"]]
	code = COMPONENTS[a["componentType"]]
	width = WIDTHS[a["type"]]
	size = struct.calcsize("<" + code)
	stride = view.get("byteStride") or size * width
	base = view.get("byteOffset", 0) + a.get("byteOffset", 0)
	fmt = "<" + code * width
	if stride == size * width:
		raw = data[base:base + stride * a["count"]]
		return list(struct.iter_unpack(fmt, raw))
	return [struct.unpack_from(fmt, data, base + i * stride) for i in range(a["count"])]


def node_matrix(node):
	if "matrix" in node:
		m = node["matrix"]  # column-major
		return Matrix([m[0:4], m[4:8], m[8:12], m[12:16]]).transposed()
	t = Matrix.Translation(node.get("translation", (0.0, 0.0, 0.0)))
	x, y, z, w = node.get("rotation", (0.0, 0.0, 0.0, 1.0))
	r = Quaternion((w, x, y, z)).to_matrix().to_4x4()
	s = node.get("scale", (1.0, 1.0, 1.0))
	return t @ r @ Matrix.Diagonal((s[0], s[1], s[2], 1.0))


def load_mesh(path):
	"""Every triangle the file draws, in world space, merged:
	(positions, normals, uvs, tris) - normals / uvs hold None for a vertex whose
	primitive carries none."""
	doc, buffers = read_gltf(path)
	pos, nrm, uvs, tris = [], [], [], []

	def visit(index, parent):
		node = doc["nodes"][index]
		world = parent @ node_matrix(node)
		if "mesh" in node:
			normal_m = world.to_3x3().inverted_safe().transposed()
			for prim in doc["meshes"][node["mesh"]]["primitives"]:
				if prim.get("mode", 4) != 4:
					continue
				attrs = prim["attributes"]
				base = len(pos)
				p = accessor(doc, buffers, attrs["POSITION"])
				n = accessor(doc, buffers, attrs["NORMAL"]) if "NORMAL" in attrs else [None] * len(p)
				t = accessor(doc, buffers, attrs["TEXCOORD_0"]) if "TEXCOORD_0" in attrs else [None] * len(p)
				for i in range(len(p)):
					pos.append(world @ Vector(p[i]))
					nrm.append((normal_m @ Vector(n[i])).normalized() if n[i] is not None else None)
					uvs.append(t[i])
				idx = ([i[0] for i in accessor(doc, buffers, prim["indices"])]
					   if "indices" in prim else list(range(len(p))))
				for k in range(0, len(idx) - 2, 3):
					tris.append([base + idx[k], base + idx[k + 1], base + idx[k + 2]])
		for child in node.get("children", []):
			visit(child, world)

	scene = doc["scenes"][doc.get("scene", 0)]
	for root in scene["nodes"]:
		visit(root, Matrix.Identity(4))
	return pos, nrm, uvs, tris


# --- the checks ---------------------------------------------------------------------
class Mesh:
	"""A loaded file prepared for the checks. `tris` and `corner_uv` may be
	edited (the self-test's faults) and `prepare` run again."""

	def __init__(self, pos, nrm, uvs, tris):
		self.pos, self.nrm, self.uvs = pos, nrm, uvs
		self.tris = [list(t) for t in tris]
		self.corner_uv = {}  # (triangle, corner) -> (u, v): a fault's override
		ids = {}
		self.weld = [ids.setdefault(tuple(p), len(ids)) for p in pos]
		self.points = [None] * len(ids)
		for i, w in enumerate(self.weld):
			self.points[w] = pos[i]
		self.prepare()

	def uv(self, t, k):
		over = self.corner_uv.get((t, k))
		return over if over is not None else self.uvs[self.tris[t][k]]

	def prepare(self):
		w = self.weld
		self.normal = []      # per triangle: the winding normal, or None if degenerate
		self.sliver = []      # per triangle: next to no area for its size (uspan skips it)
		self.edges = {}       # (welded a, welded b) -> [(triangle, corner a, corner b)]
		for t, tri in enumerate(self.tris):
			ws = [w[i] for i in tri]
			if len(set(ws)) < 3:
				self.normal.append(None)
				self.sliver.append(True)
				continue
			a, b, c = (self.points[x] for x in ws)
			n = (b - a).cross(c - a)
			self.normal.append(n.normalized() if n.length > 1e-20 else None)
			longest = max((b - a).length, (c - b).length, (a - c).length)
			self.sliver.append(n.length < SLIVER * longest * longest)
			for ka, kb in ((0, 1), (1, 2), (2, 0)):
				key = (ws[ka], ws[kb]) if ws[ka] < ws[kb] else (ws[kb], ws[ka])
				self.edges.setdefault(key, []).append((t, ka, kb))
		# Islands: triangles joined by a shared edge (union-find).
		parent = list(range(len(self.tris)))

		def find(x):
			while parent[x] != x:
				parent[x] = parent[parent[x]]
				x = parent[x]
			return x
		for users in self.edges.values():
			for other in users[1:]:
				ra, rb = find(users[0][0]), find(other[0])
				if ra != rb:
					parent[ra] = rb
		self.islands = {}
		for t in range(len(self.tris)):
			if self.normal[t] is not None:
				self.islands.setdefault(find(t), []).append(t)
		self.island_of = {t: r for r, ts in self.islands.items() for t in ts}
		open_islands = set()
		for users in self.edges.values():
			if len(users) != 2:
				open_islands.update(self.island_of[u[0]] for u in users)
		self.closed_islands = [r for r in self.islands if r not in open_islands]

	def open_edges(self):
		return sum(1 for users in self.edges.values() if len(users) != 2)

	def inward(self):
		"""(triangles wound INTO their closed island, triangles ray-tested)."""
		bad, tested = [], 0
		for root in self.closed_islands:
			ts = self.islands[root]
			local = {}
			for t in ts:
				for i in self.tris[t]:
					local.setdefault(self.weld[i], len(local))
			points = [None] * len(local)
			for wid, li in local.items():
				points[li] = self.points[wid]
			polys = [[local[self.weld[i]] for i in self.tris[t]] for t in ts]
			tree = BVHTree.FromPolygons(points, polys, all_triangles=True)
			lo = Vector((min(p.x for p in points), min(p.y for p in points), min(p.z for p in points)))
			hi = Vector((max(p.x for p in points), max(p.y for p in points), max(p.z for p in points)))
			diag = max((hi - lo).length, 1e-6)
			eps = diag * 1e-6
			for t in ts:
				n = self.normal[t]
				a, b, c = (self.points[self.weld[i]] for i in self.tris[t])
				centre = (a + b + c) / 3.0
				odd = 0
				for d in rays(n):
					if crossings(tree, centre + n * eps, d, eps, diag * 4.0) % 2:
						odd += 1
				tested += 1
				if odd * 2 > VOTES:
					bad.append(t)
		return bad, tested

	def misnormal(self):
		"""(corners whose shading normal opposes their triangle's winding, corners read)."""
		bad, read = 0, 0
		for t, tri in enumerate(self.tris):
			n = self.normal[t]
			if n is None:
				continue
			for i in tri:
				if self.nrm[i] is None:
					continue
				read += 1
				if self.nrm[i].dot(n) < 0.0:
					bad += 1
		return bad, read

	def uspan(self, t):
		us = [self.uv(t, k)[0] for k in range(3)]
		return max(us) - min(us)

	def fold(self, users):
		"""None if uspan does not judge this edge; else NO_FOLD, MIRROR (its two
		faces on the same side of it in UV, spans alike) or REVERSED (the same,
		one face spanning SPAN_RATIO times the other's u - C436's seam)."""
		if len(users) != 2:
			return None
		(t1, a1, b1), (t2, a2, b2) = users
		n1, n2 = self.normal[t1], self.normal[t2]
		if n1 is None or n2 is None or n1.dot(n2) < SMOOTH_COS:
			return None  # a crease, not a surface: see the header
		if self.sliver[t1] or self.sliver[t2]:
			return None  # a near-zero triangle's normal means nothing
		# t2 names the same edge; match its corners to t1's by welded point.
		if self.weld[self.tris[t2][a2]] != self.weld[self.tris[t1][a1]]:
			a2, b2 = b2, a2
		p, q = self.uv(t1, a1), self.uv(t1, b1)
		p2, q2 = self.uv(t2, a2), self.uv(t2, b2)
		r1, r2 = self.uv(t1, 3 - a1 - b1), self.uv(t2, 3 - a2 - b2)
		if None in (p, q, p2, q2, r1, r2):
			return None
		if not same_uv(p, p2) or not same_uv(q, q2):
			return None  # a UV seam: the two sides are separate charts
		ex, ey = q[0] - p[0], q[1] - p[1]
		s1 = ex * (r1[1] - p[1]) - ey * (r1[0] - p[0])
		s2 = ex * (r2[1] - p[1]) - ey * (r2[0] - p[0])
		tol = 1e-7 * math.hypot(ex, ey) * max(math.hypot(r1[0] - p[0], r1[1] - p[1]),
											  math.hypot(r2[0] - p[0], r2[1] - p[1]), 1e-12)
		if abs(s1) <= tol or abs(s2) <= tol:
			return None  # flat in UV on one side (a ring whose v is constant)
		if s1 * s2 < 0.0:
			return NO_FOLD
		w1, w2 = self.uspan(t1), self.uspan(t2)
		return REVERSED if max(w1, w2) > SPAN_RATIO * max(min(w1, w2), 1e-12) else MIRROR

	def folds(self):
		"""(edges where a face spans u backwards, edges judged, mirror folds)."""
		bad, judged, mirrors = [], 0, 0
		for users in self.edges.values():
			f = self.fold(users)
			if f is None:
				continue
			judged += 1
			if f == REVERSED:
				bad.append(users)
			elif f == MIRROR:
				mirrors += 1
		return bad, judged, mirrors

	def span(self):
		"""The widest face's u-span as a share of the file's u range."""
		us = [uv[0] for uv in self.uvs if uv is not None]
		if not us:
			return 0.0
		full = max(us) - min(us)
		widest = 0.0
		for t in range(len(self.tris)):
			if self.normal[t] is None:
				continue
			u = [self.uv(t, k) for k in range(3)]
			if any(x is None for x in u):
				continue
			widest = max(widest, max(x[0] for x in u) - min(x[0] for x in u))
		return widest / full if full > 0.0 else 0.0


def same_uv(a, b):
	return abs(a[0] - b[0]) <= 1e-6 * max(1.0, abs(a[0])) and abs(a[1] - b[1]) <= 1e-6 * max(1.0, abs(a[1]))


def rays(n):
	"""The triangle's winding normal and two directions a little off it."""
	ref = Vector((0.267, 0.534, 0.802))
	if abs(n.dot(ref)) > 0.9:
		ref = Vector((0.802, -0.267, 0.534))
	t1 = n.cross(ref).normalized()
	t2 = n.cross(t1)
	return [n, (n + t1 * 0.031).normalized(), (n - t2 * 0.027).normalized()][:VOTES]


def crossings(tree, origin, d, eps, far):
	"""How many times a ray from `origin` along `d` crosses the tree's surface."""
	n = 0
	for _ in range(100000):
		hit, _, _, dist = tree.ray_cast(origin, d, far)
		if hit is None:
			return n
		n += 1
		origin = hit + d * eps
	return n


# --- one judging pass -----------------------------------------------------------------
def where(mesh, ts):
	"""Where a set of triangles is: their count and bounding box, for --detail."""
	pts = [mesh.points[mesh.weld[i]] for t in ts for i in mesh.tris[t]]
	lo = [min(p[k] for p in pts) for k in range(3)]
	hi = [max(p[k] for p in pts) for k in range(3)]
	return (f"{len(ts)} tris, x {lo[0]:+.3f}..{hi[0]:+.3f} y {lo[1]:+.3f}..{hi[1]:+.3f} "
			f"z {lo[2]:+.3f}..{hi[2]:+.3f}")


def judge(name, mesh, closed_required, say, detail=False):
	"""Runs every check on one mesh; returns {check: ok}. `say` prints a line;
	`detail` also says WHERE a failing check found its faults, island by island."""
	out = {}
	tris = sum(1 for n in mesh.normal if n is not None)
	if closed_required:
		n = mesh.open_edges()
		out["closed"] = n == 0 and tris > 0
		say(out["closed"], f"{name} closed", f"{n} open edges, {tris} triangles, {len(mesh.islands)} islands")
		if detail and n:
			open_ts = sorted({u[0] for users in mesh.edges.values() if len(users) != 2 for u in users})
			print(f"      open edges touch {where(mesh, open_ts)}")
	bad, tested = mesh.inward()
	if detail and bad:
		by_island = {}
		for t in bad:
			by_island.setdefault(mesh.island_of[t], []).append(t)
		for root, ts in by_island.items():
			print(f"      inward: {where(mesh, ts)}  in an island of {where(mesh, mesh.islands[root])}")
	# A file asked to be closed with nothing ray-tested has not been judged.
	out["outward"] = not bad and (tested > 0 or not closed_required)
	say(out["outward"], f"{name} outward",
		f"{len(bad)} of {tested} triangles in {len(mesh.closed_islands)} closed islands wound inward")
	bad_n, read = mesh.misnormal()
	out["normals"] = bad_n == 0 and read > 0
	say(out["normals"], f"{name} normals", f"{bad_n} of {read} corners against their winding")
	folds, judged, mirrors = mesh.folds()
	if detail and folds:
		print(f"      reversed spans touch {where(mesh, sorted({u[0] for users in folds for u in users}))}")
	out["uspan"] = not folds and judged > 0
	say(out["uspan"], f"{name} uspan",
		f"{len(folds)} of {judged} smooth UV-continuous edges span u backwards, {mirrors} mirror; "
		f"widest face spans {mesh.span() * 100.0:.1f}% of the u range")
	return out


# --- the self-test's faults ---------------------------------------------------------------
def first_closed_triangle(mesh):
	for root in mesh.closed_islands:
		return mesh.islands[root][0]
	return None


def plant(mesh, fault):
	"""Plants `fault`; returns False when this file offers no place for it."""
	if fault == "flip":
		t = first_closed_triangle(mesh)
		if t is None:
			return False
		mesh.tris[t][1], mesh.tris[t][2] = mesh.tris[t][2], mesh.tris[t][1]
	elif fault == "hole":
		t = first_closed_triangle(mesh)
		if t is None:
			return False
		del mesh.tris[t]
	elif fault == "fold":
		# The first unfolded edge uspan judges whose second face reaches well
		# along u from it (a column's side edge, say). The first face's third
		# corner is flung 50x as far out along the second face's side of the
		# edge: it folds onto its neighbour spanning u 25x or more as far -
		# the shape of a column that reaches backwards round the circumference.
		target = None
		for users in mesh.edges.values():
			if mesh.fold(users) != NO_FOLD:
				continue
			(t1, a1, b1), (t2, a2, b2) = users
			if mesh.weld[mesh.tris[t2][a2]] != mesh.weld[mesh.tris[t1][a1]]:
				a2, b2 = b2, a2
			p, r2 = mesh.uv(t1, a1), mesh.uv(t2, 3 - a2 - b2)
			reach = abs(r2[0] - p[0])
			if reach > 1e-9 and reach >= 0.5 * mesh.uspan(t2):
				target = (t1, 3 - a1 - b1, p, r2)
				break
		if target is None:
			return False
		t1, k, p, r2 = target
		mesh.corner_uv[(t1, k)] = (p[0] + (r2[0] - p[0]) * 50.0, p[1] + (r2[1] - p[1]) * 50.0)
	mesh.prepare()
	return True


def expected(fault, closed_islands, closed_required):
	"""The checks a fault is for, in a file with `closed_islands` before it."""
	if fault == "flip":
		return {"outward", "normals"}
	if fault == "hole":
		out = {"closed"} if closed_required else set()
		if closed_required and closed_islands == 1:
			out.add("outward")  # the only shell is open now: nothing left to ray-test
		return out
	return {"uspan"}


# --- main ----------------------------------------------------------------------------------
def main():
	argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
	self_test = "--selftest" in argv
	detail = "--detail" in argv
	given = [a for a in argv if not a.startswith("--")]
	files = given or [os.path.join(MODELS, name) for name, _, _ in ASSETS]

	results = []  # (label, ok)

	def say(ok, label, detail=""):
		results.append((label, bool(ok)))
		print(f"  [{'ok  ' if ok else 'FAIL'}] {label}" + (f"  ({detail})" if detail else ""))

	missing = [f for f in files if not os.path.isfile(f)]
	for f in missing:
		say(False, f"{os.path.basename(f)} present", "no such file")
	loaded = []
	for f in files:
		if f in missing:
			continue
		name = os.path.basename(f)
		pos, nrm, uvs, tris = load_mesh(f)
		loaded.append((name, (pos, nrm, uvs, tris), CLOSED.get(name, True)))
	if not loaded:
		print(f"{TOOL} RESULT=FAIL checks=0 failures=0 self_test={int(self_test)}")
		sys.exit(2)

	if not self_test:
		print(f"{TOOL}: {len(loaded)} files")
		for name, data, closed_required in loaded:
			judge(name, Mesh(*data), closed_required, say, detail)
		failures = sum(1 for _, ok in results if not ok)
		result = "FAIL" if failures else "PASS"
		print(f"{TOOL} RESULT={result} checks={len(results)} failures={failures} self_test=0")
		sys.exit(1 if failures else 0)

	# --selftest: each fault in each file on its own, and EXACTLY its checks fail.
	wrong = 0
	for fault in ("flip", "hole", "fold"):
		print(f"{TOOL}: fault '{fault}'")
		for name, data, closed_required in loaded:
			mesh = Mesh(*data)
			want = expected(fault, len(mesh.closed_islands), closed_required)
			if not plant(mesh, fault):
				say(False, f"{name} {fault} planted", "no place for the fault in this file")
				wrong += 1
				continue
			got = judge(name, mesh, closed_required, say)
			failed = {c for c, ok in got.items() if not ok}
			if failed != want:
				wrong += 1
				print(f"  selftest: {name} '{fault}' failed {sorted(failed)}, expected {sorted(want)}")
	failures = sum(1 for _, ok in results if not ok)
	result = "FAIL" if failures else "PASS"
	caught = int(wrong == 0)
	print(f"{TOOL} RESULT={result} checks={len(results)} failures={failures} self_test=1 caught={caught}")
	sys.exit(0 if caught else 1)


if __name__ == "__main__":  # Blender's --python runs a script as __main__
	main()
