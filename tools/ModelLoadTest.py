# tools/ModelLoadTest.py - the model loaders read what a file says (code-review
# batch 58: C394, C360, C396).
#
# Run:  python tools\ModelLoadTest.py [--config debug|release]
#       (debug by default, like the game harnesses: it runs the game too)
#
# Every fixture is written by this script; nothing it judges is a shipped file.
#
#   OBJ       an OBJ whose faces are `v//n` (normals, no UVs - a common exporter
#             option) plus one face with no normal at all, through `AssetBaker
#             import-model --raw` into a scratch assets folder. Every normal the
#             written .gltf carries is unit length, the v//n faces keep the
#             file's normals - one of them TILTED off its face's flat normal, so
#             a parse that drops the //n and flat-shades every face cannot pass -
#             and the bare face gets its flat one (+Y; no other direction), and
#             the import SAYS one face had none.
#             Before: "%d/%d/%d" read "5//3" as just the 5, the `%d//%d`
#             fallback never ran, and every normal came in zero.
#   SIDECARS  a glTF whose images are [a data: URI, a.png 8x8, b.png 16x16],
#             used by materials in another order (m0 -> image 2, m1 -> image 1,
#             m2 -> image 0), through `AssetBaker model-images` in a scratch
#             folder holding two sidecars named the OLD way and a decoy. The
#             sidecars come out named by the FILE's index - .img1.dds 8x8,
#             .img2.dds 16x16, no .img0 (a data: URI is not loaded) - the old
#             ones are deleted and the decoy kept, and a second run finds both
#             current. Before: named by first-use order, so m0's image took
#             index 0 and every later one shifted onto another's chain.
#   PREVIEW   both fixtures copied into assets\models (under modelloadtest_*,
#             removed in a finally, and before the run if a killed one left
#             them) and shown by the game's `preview` in a WINDOWED run, frames
#             drawn after each. The material-less glTF loads with ONE default
#             material and draws; the sidecar model binds each material's base
#             colour to the sidecar of ITS image (by size: image2 16x16, image1
#             8x8, both baked, m2 none). Before: a glTF with no material left
#             materials empty, and every materials[0] read off it aborted.
#
# Exit: 0 PASS, 1 FAIL, 2 nothing ran (no build), 3/4 the shared refusals
# (harness_game). Last line: `modelloadtest RESULT=... checks=N failures=M
# self_test=0`.
import base64
import glob
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile

import harness_game
from WornBakeTest import write_png

sys.stdout.reconfigure(errors="backslashreplace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODELS = os.path.join(ROOT, "assets", "models")
TOOL = "modelloadtest"
PREFIX = "modelloadtest_"
NOMAT = PREFIX + "nomat"
SIDECAR = PREFIX + "sidecar"

results = []  # (label, ok)


def check(ok, label, detail=""):
	results.append((label, bool(ok)))
	print(f"  [{'ok  ' if ok else 'FAIL'}] {label}" + (f"  ({detail})" if detail else ""))
	return ok


def run(exe, *args):
	r = subprocess.run([exe, *args], capture_output=True, text=True, encoding="utf-8",
					   errors="replace")
	return r.returncode, r.stdout + r.stderr


def said(out, *words):
	return any(all(w in line for w in words) for line in out.splitlines())


# --- fixtures ---------------------------------------------------------------------
def triangle_gltf(extra, primitive_material=None):
	"""One triangle facing +Z, its buffer embedded; `extra` adds top-level keys."""
	pos = struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0)
	nrm = struct.pack("<9f", 0, 0, 1, 0, 0, 1, 0, 0, 1)
	blob = pos + nrm
	prim = {"attributes": {"POSITION": 0, "NORMAL": 1}}
	if primitive_material is not None:
		prim["material"] = primitive_material
	doc = {
		"asset": {"version": "2.0"},
		"scene": 0,
		"scenes": [{"nodes": [0]}],
		"nodes": [{"mesh": 0}],
		"meshes": [{"primitives": [prim]}],
		"buffers": [{"byteLength": len(blob),
					 "uri": "data:application/octet-stream;base64," +
							base64.b64encode(blob).decode("ascii")}],
		"bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
						{"buffer": 0, "byteOffset": 36, "byteLength": 36}],
		"accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
					   "min": [0, 0, 0], "max": [1, 1, 0]},
					  {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"}],
	}
	doc.update(extra)
	return doc


def write_json(path, doc):
	with open(path, "w", encoding="utf-8", newline="\n") as fh:
		json.dump(doc, fh, indent=1)


def write_sidecar_fixture(folder):
	"""SIDECAR.gltf + its two PNGs in `folder`."""
	write_png(os.path.join(folder, SIDECAR + "_a.png"), 8, 8, (200, 40, 40, 255))
	write_png(os.path.join(folder, SIDECAR + "_b.png"), 16, 16, (40, 40, 200, 255))
	tiny = os.path.join(folder, "tiny.png")
	write_png(tiny, 4, 4, (40, 200, 40, 255))
	with open(tiny, "rb") as fh:
		data_uri = "data:image/png;base64," + base64.b64encode(fh.read()).decode("ascii")
	os.remove(tiny)
	doc = triangle_gltf({
		"images": [{"uri": data_uri}, {"uri": SIDECAR + "_a.png"}, {"uri": SIDECAR + "_b.png"}],
		"textures": [{"source": 1}, {"source": 2}, {"source": 0}],
		"materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 1}}},
					  {"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}},
					  {"pbrMetallicRoughness": {"baseColorTexture": {"index": 2}}}],
	}, primitive_material=0)
	write_json(os.path.join(folder, SIDECAR + ".gltf"), doc)


# The first face's file normal is TILTED (0.6 0 -0.8) where its own flat normal
# is (0 0 -1): only a parse that reads the //n can produce it, so a regressed
# parse - every face falling to the flat fallback - cannot pass as the file's.
TILTED = (0.6, 0.0, -0.8)
OBJ_TEXT = """# modelloadtest: a unit cube, five faces v//n, the top with no normal
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
v 0 0 1
v 1 0 1
v 1 1 1
v 0 1 1
vn 0.6 0 -0.8
vn 0 0 1
vn -1 0 0
vn 1 0 0
vn 0 -1 0
f 1//1 4//1 3//1 2//1
f 5//2 6//2 7//2 8//2
f 1//3 5//3 8//3 4//3
f 2//4 3//4 7//4 6//4
f 1//5 2//5 6//5 5//5
f 4 8 7 3
"""


def gltf_floats(doc, accessor):
	"""An accessor's float VEC3s, from a buffer embedded as a data: URI."""
	acc = doc["accessors"][accessor]
	view = doc["bufferViews"][acc["bufferView"]]
	uri = doc["buffers"][view["buffer"]]["uri"]
	blob = base64.b64decode(uri.split(",", 1)[1])
	start = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
	stride = view.get("byteStride", 12)
	return [struct.unpack_from("<3f", blob, start + i * stride) for i in range(acc["count"])]


def dds_size(path):
	with open(path, "rb") as fh:
		head = fh.read(20)
	height, width = struct.unpack_from("<II", head, 12)
	return width, height


# --- the checks -----------------------------------------------------------------
def obj_checks(baker, scratch):
	src = os.path.join(scratch, "objsrc")
	os.makedirs(src)
	obj = os.path.join(src, "cube.obj")
	with open(obj, "w", encoding="ascii", newline="\n") as fh:
		fh.write(OBJ_TEXT)
	assets = os.path.join(scratch, "objassets")
	code, out = run(baker, "import-model", obj, assets, PREFIX + "obj", "--raw",
					"--texture-set", "none")
	out_file = os.path.join(assets, "models", PREFIX + "obj.gltf")
	if not check(code == 0 and os.path.isfile(out_file), "OBJ import-model writes the cube",
				 f"exit {code}"):
		print(out[-1500:])
		return
	check(said(out, "1 faces had no normals"), "OBJ the import says the one face with no normal")
	doc = json.load(open(out_file, encoding="utf-8"))
	prim = doc["meshes"][0]["primitives"][0]
	normals = gltf_floats(doc, prim["attributes"]["NORMAL"])
	lengths = [math.sqrt(x * x + y * y + z * z) for x, y, z in normals]
	short = [l for l in lengths if abs(l - 1.0) > 1e-3]
	check(normals and not short, "OBJ every normal is unit length",
		  f"{len(normals)} normals" + (f", {len(short)} not unit (shortest {min(lengths):.3f})"
									   if short else ""))
	# Every direction the cube carries, to 1e-3: the file's five (the tilted one
	# included, and NOT the first face's flat (0 0 -1)) and the bare face's +Y.
	want = {TILTED, (0, 0, 1), (-1, 0, 0), (1, 0, 0), (0, -1, 0), (0, 1, 0)}
	seen = {tuple(round(c, 3) + 0.0 for c in n) for n in normals}
	want = {tuple(float(c) for c in d) for d in want}
	check(seen == want, "OBJ the v//n faces keep the file's normals (the tilted one exactly), "
		  "the bare face gets +Y", f"directions {sorted(seen)}")


def sidecar_checks(baker, scratch):
	assets = os.path.join(scratch, "sideassets")
	models = os.path.join(assets, "models")
	os.makedirs(models)
	write_sidecar_fixture(models)
	model = os.path.join(models, SIDECAR + ".gltf")
	legacy = [model + ".0.dds", model + ".1.dds"]
	decoy = os.path.join(models, "readme.0.dds")
	for p in legacy + [decoy]:
		with open(p, "wb") as fh:
			fh.write(b"modelloadtest: an old-style sidecar\n")
	code, out = run(baker, "model-images", assets)
	if not check(code == 0, "SIDECARS model-images bakes the fixture", f"exit {code}"):
		print(out[-1500:])
		return
	img = {i: model + f".img{i}.dds" for i in range(3)}
	sizes = {i: dds_size(p) for i, p in img.items() if os.path.isfile(p)}
	check(sizes.get(1) == (8, 8) and sizes.get(2) == (16, 16) and 0 not in sizes,
		  "SIDECARS named by the file's image index (img1 8x8, img2 16x16, no img0)",
		  f"found {sizes}")
	check(not any(os.path.exists(p) for p in legacy) and os.path.isfile(decoy)
		  and said(out, "2 old-style sidecars removed"),
		  "SIDECARS the old-style sidecars are removed, the decoy kept",
		  f"left {[os.path.basename(p) for p in legacy if os.path.exists(p)]}")
	code, again = run(baker, "model-images", assets)
	check(code == 0 and said(again, "0 images baked", "2 already current"),
		  "SIDECARS a second run finds both current by the same names",
		  next((l for l in again.splitlines() if "Model image bake" in l), "no summary line"))
	return models


def preview_checks(exe, scratch, sidecar_dir):
	nomat = triangle_gltf({})
	write_json(os.path.join(MODELS, NOMAT + ".gltf"), nomat)
	# The sidecar fixture as baked: copy2 keeps each file's time, so the sidecars
	# stay newer than the model and the game takes them as current.
	for p in sorted(glob.glob(os.path.join(sidecar_dir, SIDECAR + "*"))):
		shutil.copy2(p, os.path.join(MODELS, os.path.basename(p)))
	script = os.path.join(scratch, "modelloadtest.eval")
	with open(script, "w", encoding="utf-8", newline="\n") as fh:
		fh.write("\n".join([
			"; modelloadtest: preview a material-less glTF and the sidecar fixture",
			"logecho on", "reset",
			f"preview {NOMAT}", "state", "state", "state",
			f"preview {SIDECAR}", "state", "state", "state",
			"preview off", "state", ""]))
	log = os.path.join(os.path.dirname(exe), "dungeon.log")
	code, text = harness_game.run_eval(exe, ROOT, log, [script], timeout=300, headless=False)
	if harness_game.report_unfinished(code, text, "PREVIEW the game run"):
		check(False, "PREVIEW the game run finished")
		return
	failed = harness_game.report_failed_script(text, "modelloadtest.eval")
	check(not failed, "PREVIEW the script ran clean")
	check(said(text, f"preview: {NOMAT} (meshes=1 materials=1)") and
		  said(text, "preview: material 0 base=-"),
		  "PREVIEW a glTF with no material loads with one default and draws")
	want = ["preview: material 0 base=image2 16x16 baked",
			"preview: material 1 base=image1 8x8 baked",
			"preview: material 2 base=-"]
	got = [l.split("console: ", 1)[-1] for l in text.splitlines()
		   if "preview: material" in l and SIDECAR not in l]
	check(said(text, f"preview: {SIDECAR} (meshes=1 materials=3)") and
		  all(said(text, w) for w in want),
		  "PREVIEW each material binds the sidecar of its own image",
		  "; ".join(g.strip() for g in got[-3:]))
	errs = [l for l in text.splitlines() if "d3d12 error" in l.lower()]
	check(not errs, "PREVIEW no D3D12 error while drawing", errs[0][:120] if errs else "")


def clear_fixtures():
	for p in glob.glob(os.path.join(MODELS, PREFIX + "*")):
		try:
			os.remove(p)
		except OSError:
			pass


def main():
	config = "debug"
	if "--config" in sys.argv:
		config = sys.argv[sys.argv.index("--config") + 1]
	bindir = os.path.join(ROOT, "build", config, "bin")
	baker = os.path.join(bindir, "AssetBaker.exe")
	exe = os.path.join(bindir, "Dungeon.exe")
	for f in (baker, exe):
		if not os.path.isfile(f):
			print(f"no {config} build at {f} - run .\\build.cmd {config}")
			return harness_game.EXIT_USAGE
	harness_game.refuse_if_stale(baker)
	harness_game.refuse_if_stale(exe)
	harness_game.refuse_if_running(exe)

	clear_fixtures()  # a killed run's
	scratch = tempfile.mkdtemp(prefix="modelloadtest-")
	try:
		obj_checks(baker, scratch)
		sidecar_dir = sidecar_checks(baker, scratch)
		if sidecar_dir:
			preview_checks(exe, scratch, sidecar_dir)
		else:
			check(False, "PREVIEW skipped: no baked sidecar fixture")
	finally:
		clear_fixtures()
		shutil.rmtree(scratch, ignore_errors=True)

	failed = [lbl for lbl, ok in results if not ok]
	print(f"{TOOL} RESULT={'FAIL' if failed else 'PASS'} checks={len(results)} "
		  f"failures={len(failed)} self_test=0")
	return 1 if failed else 0


if __name__ == "__main__":
	sys.exit(main())
