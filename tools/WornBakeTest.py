# tools/WornBakeTest.py - the worn-block bake has ONE authority (code-review
# batch 90: C406, C408, C409).
#
# Run:  python tools\WornBakeTest.py [--selftest] [--config release|debug]
#       (release by default, like Bc7Test: the baker is a tool, and the debug
#       one is slow for no gain here)
#
# A surface texture set's worn meshes (worn_<set>_<tier>*.gltf) are shaped by
# ONE record, Assets/WornSets.h - kind, relief, seed - which `AssetBaker models`
# and `AssetBaker wornblock` (what the editor runs on an import or a type save)
# both read. They used to be two tables that disagreed, so an editor save of
# ANY surface field quietly re-baked a shipped set at another depth with other
# noise. Every bake here goes to a scratch folder; the tree is never written.
#
# "Committed" means what GIT holds - the index, which is HEAD's unless a re-bake
# has been staged - never assets\models on disk. The working tree is the one
# thing a regression would rewrite (`AssetBaker models` writes it), so a judge
# reading it would compare a bad bake with itself; and an editor import or type
# save writes worn files there too, which are work in progress, not the pool.
#
#   RECORD    every committed worn set has a record or an import (a set an
#             imports.cat names as a surface gets a DERIVED record - WornSets.h),
#             and every record its committed files.
#   REBAKE    `models` re-bakes each set's worn files byte-for-byte as committed.
#   AGREE     `wornblock <kind> <set>` with no relief or wear writes, for every
#             shipped set, exactly the bytes `models` wrote (C406).
#   FLAT      `wornblock --wear 0` is the bare quad - 4 vertices, 2 triangles, no
#             displacement - for a wall, a floor, a ceiling and a procedural wall
#             (C409: floors and ceilings used to keep their full noisy grid).
#   SCALE     relief x wear scales EVERY wear term: wear 0.5 halves a scanned
#             wall's displacement (its bowed-masonry noise included), a floor's,
#             a ceiling's, a procedural wall's and a procedural floor's and
#             ceiling's; twice the set's relief doubles it (C409: only the
#             height-map term used to see either knob). No shipped floor or
#             ceiling lacks a height map, so those two are fixtures with a flat
#             one, as the importer packs for a scan that shipped none.
#   KIND      a shipped set asked for as another kind is refused and writes
#             nothing; an unknown kind is refused too (C406).
#   ASPECT    a 2:1 set whose only height map is flat keeps its aspect - its worn
#             UVs span half a repeat across - where the resolution fallback used
#             to drop it to 1 (C408).
#
# --selftest gives every check group its own fault and demands that EXACTLY the
# expected checks fail and the rest still pass.
import base64
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from concurrent.futures import ThreadPoolExecutor

import harness_game

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")
TOOL = "wornbaketest"

results = []  # (label, ok)


def check(ok, label, detail=""):
	results.append((label, bool(ok)))
	print(f"  [{'ok  ' if ok else 'FAIL'}] {label}" + (f"  ({detail})" if detail else ""))
	return ok


def run(baker, *args):
	r = subprocess.run([baker, *args], capture_output=True, text=True, errors="replace")
	return r.returncode, r.stdout + r.stderr


def worn_files(folder, name):
	"""Every worn file of one set: worn_<set>_<tier>.gltf and the wall panels
	worn_<set>_<tier>_<phase><sides>.gltf - but not another set's whose name
	merely starts with this one's (wall_stone vs wall_stone_28)."""
	out = []
	if not os.path.isdir(folder):
		return out
	prefix = f"worn_{name}_"
	for f in os.listdir(folder):
		if not f.startswith(prefix) or not f.endswith(".gltf"):
			continue
		rest = f[len(prefix):-len(".gltf")]
		tier, _, panel = rest.partition("_")
		if tier in ("low", "med", "high") and (not panel or panel[0].isdigit()):
			out.append(f)
	return sorted(out)


def worn_sets(files):
	"""The set names behind a list of worn file names (every set has its three
	worn_<set>_<tier>.gltf; the wall panels beside them add no set)."""
	sets = set()
	for f in files:
		if f.startswith("worn_") and f.endswith(".gltf"):
			stem = f[len("worn_"):-len(".gltf")]
			for tier in ("_low", "_med", "_high"):
				if stem.endswith(tier):
					sets.add(stem[:-len(tier)])
	return sets


# --- what git holds -------------------------------------------------------------
def git(*args, stdin=None):
	r = subprocess.run(["git", "-C", ROOT, *args], input=stdin, capture_output=True)
	return r.returncode, r.stdout, r.stderr.decode("utf-8", "replace")


def export_committed(dest):
	"""Writes git's copy (the index) of every worn file into dest/assets/models,
	as a checkout would - so the judge compares with what is committed (or a
	staged re-bake), never with a working tree a bake may have rewritten.
	Returns (folder, file names)."""
	code, out, err = git("ls-files", "-z", "--", "assets/models/worn_*.gltf")
	paths = [p for p in out.decode("utf-8").split("\0") if p]
	if code != 0 or not paths:
		raise SystemExit(f"git ls-files found no worn files ({err.strip()})")
	prefix = dest.replace("\\", "/").rstrip("/") + "/"
	code, _, err = git("checkout-index", "-z", "--stdin", f"--prefix={prefix}",
					   stdin="\0".join(paths).encode("utf-8"))
	if code != 0:
		raise SystemExit(f"git checkout-index failed: {err.strip()}")
	return os.path.join(dest, "assets", "models"), [os.path.basename(p) for p in paths]


def import_sets(text):
	"""The worn sets an imports.cat names: each `kind = texture` entry with a
	`surface`, by its BASE name - the manifest keys the pool asset
	(mywall_2k) while the worn bake takes the base (ReplayImports does the
	same strip). Read as Game/Serialize.cpp reads the block format: ';' starts
	a comment only at the head of a line, a header's id runs to its ']'."""
	sets, entry = set(), None

	def close(e):
		if e and e.get("kind", "texture") == "texture" and e.get("surface"):
			base = e["id"]
			for res in ("_1k", "_2k", "_4k"):
				if base.endswith(res):
					base = base[:-len(res)]
			sets.add(base)

	for raw in text.splitlines():
		line = raw.strip()
		if not line or line.startswith(";"):
			continue
		if line.startswith("["):
			if "]" in line:
				close(entry)
				entry = {"id": line[1:line.index("]")].strip()}
		elif entry is not None and "=" in line:
			key, _, value = line.partition("=")
			entry[key.strip()] = value.strip()
	close(entry)
	return sets


def committed_imports():
	"""Every surface set a COMMITTED imports.cat names, in any project or
	template - read from git too, since a manifest that never reached git
	rebuilds nothing on a fresh clone."""
	code, out, _ = git("ls-files", "-z", "--", "assets/*imports.cat")
	sets = set()
	for path in (p for p in out.decode("utf-8").split("\0") if p):
		code, blob, _ = git("show", f":{path}")
		if code == 0:
			sets |= import_sets(blob.decode("utf-8", "replace"))
	return sets


def read(path):
	with open(path, "rb") as fh:
		return fh.read()


# --- glTF ---------------------------------------------------------------------
_COMPONENTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}
_FORMATS = {5126: "f", 5125: "I", 5123: "H"}


def gltf_mesh(path):
	"""(positions, uvs, index count) of the single primitive the baker writes."""
	doc = json.loads(read(path))
	uri = doc["buffers"][0]["uri"]
	blob = base64.b64decode(uri.split(",", 1)[1])

	def accessor(i):
		acc = doc["accessors"][i]
		view = doc["bufferViews"][acc["bufferView"]]
		n = _COMPONENTS[acc["type"]]
		fmt = _FORMATS[acc["componentType"]]
		start = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
		size = struct.calcsize(fmt)
		vals = struct.unpack_from(f"<{acc['count'] * n}{fmt}", blob, start)
		return [vals[k:k + n] for k in range(0, len(vals), n)], acc["count"], size

	prim = doc["meshes"][0]["primitives"][0]
	pos, _, _ = accessor(prim["attributes"]["POSITION"])
	uv, _, _ = accessor(prim["attributes"]["TEXCOORD_0"])
	_, icount, _ = accessor(prim["indices"])
	return pos, uv, icount


# The displacement axis per kind: a wall panel sinks along z, a floor or a
# ceiling moves along y (BuildWorn*Block).
AXIS = {"wall": 2, "floor": 1, "ceiling": 1}


def displacement(path, kind):
	pos, _, _ = gltf_mesh(path)
	return [p[AXIS[kind]] for p in pos]


# --- a PNG for the aspect fixture -------------------------------------------
def write_png(path, w, h, rgba):
	raw = b"".join(b"\x00" + bytes(rgba) * w for _ in range(h))

	def chunk(tag, data):
		c = struct.pack(">I", len(data)) + tag + data
		return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

	png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
		   + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
	with open(path, "wb") as fh:
		fh.write(png)


# --- the checks -----------------------------------------------------------------
def main():
	selftest = "--selftest" in sys.argv
	config = "release"
	if "--config" in sys.argv:
		config = sys.argv[sys.argv.index("--config") + 1]
	baker = os.path.join(ROOT, "build", config, "bin", "AssetBaker.exe")
	if not os.path.isfile(baker):
		print(f"no {config} AssetBaker at {baker} - run .\\build.cmd {config}")
		return harness_game.EXIT_USAGE
	harness_game.refuse_if_stale(baker)

	scratch = tempfile.mkdtemp(prefix="wornbaketest-")
	try:
		return judge(baker, scratch, selftest)
	finally:
		shutil.rmtree(scratch, ignore_errors=True)


def judge(baker, scratch, selftest):
	expected = set()  # the labels the self-test's faults must fail

	code, out = run(baker, "wornsets")
	records = []
	for line in out.splitlines():
		parts = line.split()
		if len(parts) == 4 and parts[1] in AXIS:
			records.append((parts[0], parts[1], float(parts[2])))
	if not check(code == 0 and records, "the baker lists the shipped worn sets",
				 f"{len(records)} records"):
		return 1
	kinds = {name: kind for name, kind, _ in records}

	print("RECORD  (what git holds, not the working tree)")
	held, tracked = export_committed(os.path.join(scratch, "git"))
	listed = set(kinds)
	if selftest:
		# FAULT: a record missing for a set git has files for.
		listed.discard("wall_moss")
		expected.add("every committed worn set has a record or an import")
	committed = worn_sets(tracked)
	imported = committed_imports()
	# The manifest reader on a sample of its own: a texture import with a
	# surface names its BASE set; a model, or a texture with no surface, none.
	sample = ("; a header\n[mywall_2k]\nkind = texture\nsurface = wall\n"
			  "[mytile_4k]\nsurface = floor\n[mymesh]\nkind = model\nsurface = wall\n"
			  "[mytex_2k]\nkind = texture\n")
	got = import_sets(sample)
	check(got == {"mywall", "mytile"}, "an imports.cat surface entry names its base set",
		  ", ".join(sorted(got)) or "none")
	unrecorded = committed - listed - imported
	check(not unrecorded, "every committed worn set has a record or an import",
		  ", ".join(sorted(unrecorded))
		  or f"{len(committed)} sets, {len(committed - listed)} imported")
	check(listed <= committed, "every record has committed worn files",
		  ", ".join(sorted(listed - committed)) or f"{len(listed)} records")

	print("REBAKE  (`models` against the committed files)")
	a = os.path.join(scratch, "models")
	code, out = run(baker, "models", ASSETS, "--out", a)
	check(code == 0, "`AssetBaker models --out` runs", f"exit {code}")
	reference = {}
	if selftest:
		# FAULT: one committed file differs from what a re-bake writes.
		name = "worn_marble_white_low.gltf"
		blob = bytearray(read(os.path.join(held, name)))
		blob[len(blob) // 2] ^= 0x01
		reference[name] = bytes(blob)
		expected.add("REBAKE marble_white")
	for name, _, _ in records:
		files = worn_files(a, name)
		bad = [f for f in files if not os.path.isfile(os.path.join(held, f))
			   or read(os.path.join(a, f)) != reference.get(f, read(os.path.join(held, f)))]
		bad += [f for f in worn_files(held, name) if f not in files]
		check(files and not bad, f"REBAKE {name}",
			  f"{len(files)} files" + (f"; differ: {', '.join(bad)}" if bad else ""))

	print("AGREE  (`wornblock` with no relief or wear against `models`)")
	# FAULT: three sets baked with an override a type might carry.
	faults = {"wall_brick": ["--wear", "0.95"], "floor_slabs": ["--relief", "0.051"],
			  "ceiling_rough": ["--wear", "0.999"]} if selftest else {}
	expected |= {f"AGREE {n}" for n in faults}
	b = os.path.join(scratch, "wornblock")

	def bake_one(rec):
		name, kind, _ = rec
		return rec, run(baker, "wornblock", kind, name, ASSETS, "--out", b, *faults.get(name, []))

	with ThreadPoolExecutor(max_workers=4) as pool:
		for (name, kind, _), (code, out) in pool.map(bake_one, records):
			files = worn_files(b, name)
			want = worn_files(a, name)
			bad = [f for f in want if f not in files or read(os.path.join(b, f)) != read(os.path.join(a, f))]
			check(code == 0 and files == want and not bad, f"AGREE {name}",
				  f"exit {code}, {len(files)} files" + (f"; differ: {', '.join(bad)}" if bad else ""))

	print("FLAT  (`wornblock --wear 0`)")
	flat_sets = [("wall_brick", "wall"), ("floor_slabs", "floor"), ("ceiling_rough", "ceiling"),
				 ("marble_white", "wall")]
	wear0 = "0.001" if selftest else "0"  # FAULT: nearly flat is not flat
	f = os.path.join(scratch, "flat")
	for name, kind in flat_sets:
		label = f"FLAT {kind} {name}: the bare quad at every tier"
		if selftest:
			expected.add(label)
		code, out = run(baker, "wornblock", kind, name, ASSETS, "--wear", wear0, "--out", f)
		files = worn_files(f, name)
		why = []
		for tier in ("low", "med", "high"):
			path = os.path.join(f, f"worn_{name}_{tier}.gltf")
			if not os.path.isfile(path):
				why.append(f"{tier} missing")
				continue
			pos, _, icount = gltf_mesh(path)
			off = max(abs(p[AXIS[kind]]) for p in pos)
			if len(pos) != 4 or icount != 6 or off > 0.0:
				why.append(f"{tier}: {len(pos)} verts, {icount // 3} tris, off {off:.4f}")
		if len(files) != 3:
			why.append(f"{len(files)} files (a flat wall bakes no panel siblings)")
		check(code == 0 and not why, label, "; ".join(why) or "4 verts, 2 tris, 0 off")

	print("SCALE  (relief x wear against the set's own bake)")
	# Every shipped floor and ceiling has a height map, so the procedural floor
	# and ceiling fields (FloorWearHeight / CeilingWearDepth) are reached only
	# through fixtures: a set whose map is flat - constant alpha, what the
	# importer packs for a scan with no displacement - bakes procedural wear, at
	# its kind's derived record. Its no-override bake is the reference.
	proc = os.path.join(scratch, "proc")
	procref = os.path.join(scratch, "procref")
	os.makedirs(os.path.join(proc, "textures"))
	for name, kind in (("wornbaketest_procfloor", "floor"), ("wornbaketest_procceil", "ceiling")):
		write_png(os.path.join(proc, "textures", f"{name}_1k_n.png"), 32, 32, (128, 128, 255, 255))
		code, out = run(baker, "wornblock", kind, name, proc, "--out", procref)
		check(code == 0 and "no packed height map" in out and
			  os.path.isfile(os.path.join(procref, f"worn_{name}_med.gltf")),
			  f"SCALE fixture {name} bakes procedural {kind} wear", f"exit {code}")
	# (set, kind, flags, factor, assets, reference bake): a halved wear and a
	# doubled relief, on a scanned wall (height map + bow + ground wear), a
	# floor, a ceiling, a procedural wall (no height map at all - the knobs used
	# to do nothing) and the procedural floor and ceiling fixtures.
	relief = {name: r for name, _, r in records}
	cases = [("wall_brick", "wall", ["--wear", "0.5"], 0.5, ASSETS, a),
			 ("wall_brick", "wall", ["--relief", f"{relief['wall_brick'] * 2:.4f}"], 2.0, ASSETS, a),
			 ("floor_slabs", "floor", ["--wear", "0.5"], 0.5, ASSETS, a),
			 ("ceiling_rough", "ceiling", ["--wear", "0.5"], 0.5, ASSETS, a),
			 ("marble_white", "wall", ["--wear", "0.5"], 0.5, ASSETS, a),
			 ("marble_white", "wall", ["--relief", f"{relief['marble_white'] * 2:.4f}"], 2.0, ASSETS, a),
			 ("wornbaketest_procfloor", "floor", ["--wear", "0.5"], 0.5, proc, procref),
			 ("wornbaketest_procceil", "ceiling", ["--wear", "0.5"], 0.5, proc, procref)]
	for i, (name, kind, flags, factor, root, refdir) in enumerate(cases):
		label = f"SCALE {name} {' '.join(flags)}: every vertex x{factor:g}"
		if selftest:
			# FAULT: the knob moved by a fifth more than the factor expected.
			expected.add(label)
			flags = [flags[0], f"{float(flags[1]) * 1.2:.4f}"]
		s = os.path.join(scratch, f"scale{i}")
		code, out = run(baker, "wornblock", kind, name, root, *flags, "--out", s)
		ref = displacement(os.path.join(refdir, f"worn_{name}_med.gltf"), kind)
		path = os.path.join(s, f"worn_{name}_med.gltf")
		got = displacement(path, kind) if os.path.isfile(path) else []
		worst = max((abs(g - factor * r) for g, r in zip(got, ref)), default=1.0)
		depth = max((abs(r) for r in ref), default=0.0)
		check(code == 0 and len(got) == len(ref) and depth > 1e-3 and worst <= 1e-6 + 1e-5 * depth,
			  label, f"deepest {depth:.4f} units, worst miss {worst:.2e}")

	print("KIND")
	k = os.path.join(scratch, "kind")
	asked = "wall" if selftest else "floor"  # FAULT: the set's own kind
	label = "KIND a shipped wall set asked for as a floor is refused, writing nothing"
	if selftest:
		expected.add(label)
	code, out = run(baker, "wornblock", asked, "wall_brick", ASSETS, "--out", k)
	check(code != 0 and not worn_files(k, "wall_brick"), label,
		  f"exit {code}, {len(worn_files(k, 'wall_brick'))} files")
	bogus = "wall" if selftest else "walls"  # FAULT: a real kind
	label = "KIND an unknown kind is refused, never baked as a wall"
	if selftest:
		expected.add(label)
	code, out = run(baker, "wornblock", bogus, "wall_moss", ASSETS, "--out", k)
	check(code != 0 and not worn_files(k, "wall_moss"), label, f"exit {code}")

	print("ASPECT  (a 2:1 set with only a flat 1k height map)")
	fixture = os.path.join(scratch, "fixture")
	os.makedirs(os.path.join(fixture, "textures"))
	w, h = (32, 32) if selftest else (64, 32)  # FAULT: a square scan
	label = "ASPECT the worn UVs span half a repeat across (aspect 2 kept)"
	if selftest:
		expected.add(label)
	write_png(os.path.join(fixture, "textures", "wornbaketest_wide_1k_n.png"), w, h, (128, 128, 255, 255))
	code, out = run(baker, "wornblock", "wall", "wornbaketest_wide", fixture)
	path = os.path.join(fixture, "models", "worn_wornbaketest_wide_med.gltf")
	span = 0.0
	if os.path.isfile(path):
		_, uv, _ = gltf_mesh(path)
		span = max(t[0] for t in uv) - min(t[0] for t in uv)
	check(code == 0 and abs(span - 0.5) < 1e-6, label, f"exit {code}, u spans {span:.4f}")

	failed = {lbl for lbl, ok in results if not ok}
	if selftest:
		wrong = sorted((expected - failed) | (failed - expected))
		for lbl in wrong:
			print(f"  selftest: '{lbl}' {'passed with its fault' if lbl in expected else 'failed with no fault'}")
		caught = not wrong
		print(f"{TOOL} RESULT={'FAIL' if failed else 'PASS'} checks={len(results)} "
			  f"failures={len(failed)} self_test=1 caught={int(caught)}")
		return 0 if caught else 1
	print(f"{TOOL} RESULT={'FAIL' if failed else 'PASS'} checks={len(results)} "
		  f"failures={len(failed)} self_test=0")
	return 1 if failed else 0


if __name__ == "__main__":
	sys.exit(main())
