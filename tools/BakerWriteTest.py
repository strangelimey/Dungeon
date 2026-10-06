# tools/BakerWriteTest.py - the asset baker's writes fail loudly (code-review
# batch 93: C416), and an import writes the green its normal map was asked for
# (batch 88: C393).
#
# Run:  python tools\BakerWriteTest.py [--selftest] [--config release|debug]
#       (release by default, like WornBakeTest: the baker is a tool)
#
# Every file the baker writes goes through assets::WriteBinaryFile now (a PNG
# through assets::WritePngFile), which checks the open, the write and the
# closing flush and says WHY one failed. Before: the glTF writer printed "Wrote"
# whatever its fwrite did (a truncated .gltf aborts a level load later), the
# sound baker ignored dr_wav's writes and the frame count it returns, the PNG
# writers ignored stb's fwrite and fclose, joint and clip names went into the
# JSON raw, and an import dropped a found map's load error in silence. Every
# bake here goes to a scratch folder; the tree is never written.
#
#   MODELS   `models --out` over a READ-ONLY wall_block.gltf exits non-zero,
#            names the file and the reason, leaves it as it was, still writes
#            every other model (the same bytes as a clean bake) and closes with
#            its failure line; and every model a clean bake writes is strict
#            JSON.
#   SOUNDS   `sounds` over a read-only footstep.wav: the same; and every other
#            WAV is the committed one byte for byte (they are built in memory
#            now, so the bytes must not have moved) with a header whose sizes
#            and frame count are the data it carries.
#   IMPORT   a set whose roughness map is found but will not load says so - the
#            file and the loader's reason - and still imports; over a read-only
#            <name>_n.png the import exits non-zero naming it, and leaves it.
#   FLIP     (code-review C393) a normal map's green is flipped by the GL token
#            at the END of its name, past a resolution tag ("nor_gl_2k"), and
#            not by a "gl" inside a word ("jungle_normal"); --no-flip-green
#            keeps a GL-named map's green, --flip-green flips an unmarked one,
#            and the two together are refused. Read off the packed _n.png.
#   NAMES    `rig-names` writes joints and clips named with quotes,
#            backslashes, control characters and UTF-8: the file is strict JSON
#            and every name reads back exactly.
#
# --selftest gives every check group its own fault and demands that EXACTLY the
# expected checks fail and the rest still pass.
import json
import os
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import zlib

import harness_game
from WornBakeTest import write_png

# A detail line may quote a name outside cp1252 (NAMES' runic letter) or a
# baker line carrying U+FFFD: through CheckAll's pipe stdout is the ANSI code
# page, so a \u escape - not a UnicodeEncodeError in place of the verdict (the
# EditorTest rule).
sys.stdout.reconfigure(errors="backslashreplace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")
TOOL = "bakerwritetest"

# What a read-only file is refused with: the C runtime's words for EACCES.
DENIED = "Permission denied"
# What sits in a read-only target before the bake: not anything a bake writes,
# so a target that WAS rewritten cannot pass for one left alone.
SENTINEL = b"bakerwritetest: this file is read-only and must not be rewritten\n"

results = []  # (label, ok)


def check(ok, label, detail=""):
	results.append((label, bool(ok)))
	print(f"  [{'ok  ' if ok else 'FAIL'}] {label}" + (f"  ({detail})" if detail else ""))
	return ok


def run(baker, *args):
	r = subprocess.run([baker, *args], capture_output=True, text=True, encoding="utf-8",
					   errors="replace")
	return r.returncode, r.stdout + r.stderr


def read(path):
	with open(path, "rb") as fh:
		return fh.read()


def plant_read_only(path, read_only=True):
	"""The sentinel at `path`, read-only (or, as a self-test fault, not)."""
	os.makedirs(os.path.dirname(path), exist_ok=True)
	with open(path, "wb") as fh:
		fh.write(SENTINEL)
	if read_only:
		os.chmod(path, stat.S_IREAD)


def writable(root):
	"""Clears the read-only bit under root, so the scratch folder can go."""
	for folder, _, files in os.walk(root):
		for f in files:
			try:
				os.chmod(os.path.join(folder, f), stat.S_IREAD | stat.S_IWRITE)
			except OSError:
				pass


def said(out, *words):
	"""Whether one line of the output carries every word."""
	return any(all(w in line for w in words) for line in out.splitlines())


def strict_json(text):
	"""JSON as the spec has it: no NaN / Infinity, no raw control characters."""
	def refuse(token):
		raise ValueError(f"not JSON: {token}")
	return json.loads(text, parse_constant=refuse)


def wav_holds_its_frames(blob):
	"""(ok, why): the RIFF size is the file's, the data chunk's size is the bytes
	it carries, and that is a whole number of frames, at least one."""
	if len(blob) < 12 or blob[:4] != b"RIFF" or blob[8:12] != b"WAVE":
		return False, "not a RIFF WAVE"
	if struct.unpack_from("<I", blob, 4)[0] != len(blob) - 8:
		return False, f"RIFF size {struct.unpack_from('<I', blob, 4)[0]}, file {len(blob) - 8}"
	pos, align, data = 12, 0, None
	while pos + 8 <= len(blob):
		tag, size = blob[pos:pos + 4], struct.unpack_from("<I", blob, pos + 4)[0]
		if tag == b"fmt ":
			align = struct.unpack_from("<H", blob, pos + 8 + 12)[0]
		elif tag == b"data":
			data = (pos + 8, size)
			break
		pos += 8 + size + (size & 1)
	if not align or data is None:
		return False, "no fmt or data chunk"
	start, size = data
	if start + size > len(blob):
		return False, f"data chunk says {size} bytes, {len(blob) - start} there"
	if size == 0 or size % align:
		return False, f"{size} data bytes is not a whole number of {align}-byte frames"
	return True, f"{size // align} frames"


# FLIP's normal maps: a green no flip can leave where it was (255 - 100 = 155).
GREEN_IN = 100


def read_png(path):
	"""The (r, g, b, a) pixels of an 8-bit RGB or RGBA PNG, non-interlaced, every
	row filter undone - the importer's PNGs are stb's, which picks a filter a row."""
	data = read(path)
	pos, idat, width, height, kind = 8, b"", 0, 0, 0
	while pos < len(data):
		size = struct.unpack_from(">I", data, pos)[0]
		tag, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + size]
		if tag == b"IHDR":
			width, height, depth, kind, _, _, laced = struct.unpack(">IIBBBBB", body)
			if depth != 8 or kind not in (2, 6) or laced:
				raise ValueError(f"{path}: not an 8-bit RGB(A) non-interlaced PNG")
		elif tag == b"IDAT":
			idat += body
		pos += 12 + size
	step = 4 if kind == 6 else 3
	raw = zlib.decompress(idat)
	stride = width * step
	rows, prev = [], bytearray(stride)
	for y in range(height):
		f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
		for i in range(stride):
			a = line[i - step] if i >= step else 0
			b, c = prev[i], prev[i - step] if i >= step else 0
			if f == 1:
				line[i] = (line[i] + a) & 0xFF
			elif f == 2:
				line[i] = (line[i] + b) & 0xFF
			elif f == 3:
				line[i] = (line[i] + (a + b) // 2) & 0xFF
			elif f == 4:
				p = a + b - c
				pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
				line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
		rows.append(line)
		prev = line
	return [tuple(r[x * step:x * step + step]) + ((255,) if step == 3 else ())
			for r in rows for x in range(width)]


def git_blob(path):
	r = subprocess.run(["git", "-C", ROOT, "show", f":{path}"], capture_output=True)
	return r.stdout if r.returncode == 0 else None


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

	scratch = tempfile.mkdtemp(prefix="bakerwritetest-")
	try:
		return judge(baker, scratch, selftest)
	finally:
		writable(scratch)
		shutil.rmtree(scratch, ignore_errors=True)


def judge(baker, scratch, selftest):
	expected = set()  # the labels the self-test's faults must fail

	def fault(*labels):
		if selftest:
			expected.update(labels)
		return selftest

	print("MODELS  (`models --out` over a read-only wall_block.gltf)")
	clean = os.path.join(scratch, "models_clean")
	code, out = run(baker, "models", ASSETS, "--out", clean)
	check(code == 0, "MODELS a clean bake runs", f"exit {code}")
	baked = sorted(f for f in os.listdir(clean) if f.endswith(".gltf")) if os.path.isdir(clean) else []
	labels = ("MODELS a read-only target fails the bake (exit non-zero)",
			  "MODELS the failure names the file and says why",
			  "MODELS the read-only file is left as it was",
			  "MODELS the bake closes with its failure line")
	ro = os.path.join(scratch, "models_ro")
	target = os.path.join(ro, "wall_block.gltf")
	plant_read_only(target, read_only=not fault(*labels))  # FAULT: a writable target
	code, out = run(baker, "models", ASSETS, "--out", ro)
	check(code != 0, labels[0], f"exit {code}")
	check(said(out, "wall_block.gltf", DENIED), labels[1],
		  next((l.strip() for l in out.splitlines() if "wall_block.gltf" in l), "no line names it"))
	check(read(target) == SENTINEL, labels[2], f"{len(read(target))} bytes")
	check(said(out, "Model bake FAILED"), labels[3])
	others = [f for f in baked if f != "wall_block.gltf"]
	differ = [f for f in others if not os.path.isfile(os.path.join(ro, f))
			  or read(os.path.join(ro, f)) != read(os.path.join(clean, f))]
	check(len(others) > 40 and not differ, "MODELS every other model is still written, as a clean "
		  "bake writes it", f"{len(others)} models" + (f"; differ: {', '.join(differ[:6])}" if differ else ""))
	label = "MODELS every model a clean bake writes is strict JSON"
	bad = []
	for f in baked:
		text = read(os.path.join(clean, f)).decode("utf-8", "replace")
		if f == "skeleton.gltf" and fault(label):
			# FAULT: a joint name written raw, the way names used to go in.
			text = text.replace('"name":"root"', '"name":"ro"ot"', 1)
		try:
			strict_json(text)
		except ValueError as e:
			bad.append(f"{f}: {e}")
	check(baked and not bad, label, "; ".join(bad[:3]) or f"{len(baked)} files")

	print("SOUNDS  (`sounds` over a read-only footstep.wav)")
	labels = ("SOUNDS a read-only target fails the bake (exit non-zero)",
			  "SOUNDS the failure names the file and says why",
			  "SOUNDS the read-only file is left as it was",
			  "SOUNDS the bake closes with its failure line")
	root = os.path.join(scratch, "sounds_root")
	sounds = os.path.join(root, "sounds")
	target = os.path.join(sounds, "footstep.wav")
	plant_read_only(target, read_only=not fault(*labels))  # FAULT: a writable target
	code, out = run(baker, "sounds", root)
	check(code != 0, labels[0], f"exit {code}")
	check(said(out, "footstep.wav", DENIED), labels[1],
		  next((l.strip() for l in out.splitlines() if "footstep.wav" in l), "no line names it"))
	check(read(target) == SENTINEL, labels[2], f"{len(read(target))} bytes")
	check(said(out, "Sound bake FAILED"), labels[3])
	r = subprocess.run(["git", "-C", ROOT, "ls-files", "-z", "--", "assets/sounds/*.wav"],
					   capture_output=True)
	committed = [p for p in r.stdout.decode("utf-8").split("\0") if p and not p.endswith("/footstep.wav")]
	label = "SOUNDS every other WAV is the committed one, byte for byte"
	reference = {p: git_blob(p) for p in committed}
	if fault(label) and committed:
		# FAULT: one committed WAV differs from what the bake writes.
		blob = bytearray(reference[committed[0]])
		blob[len(blob) // 2] ^= 0x01
		reference[committed[0]] = bytes(blob)
	differ = []
	for p in committed:
		mine = os.path.join(sounds, *p.split("/")[2:])
		if not os.path.isfile(mine) or read(mine) != reference[p]:
			differ.append(p.split("/", 2)[2])
	check(len(committed) >= 8 and not differ, label,
		  f"{len(committed)} WAVs" + (f"; differ: {', '.join(differ)}" if differ else ""))
	label = "SOUNDS every WAV's header holds the frames its data chunk carries"
	bad, frames = [], 0
	for p in committed:
		mine = os.path.join(sounds, *p.split("/")[2:])
		if not os.path.isfile(mine):
			bad.append(f"{p.split('/', 2)[2]}: not written")
			continue
		blob = read(mine)
		if p == committed[0] and fault(label):
			blob = blob[:-2]  # FAULT: a WAV two bytes short of what its header says
		ok, why = wav_holds_its_frames(blob)
		if not ok:
			bad.append(f"{p.split('/', 2)[2]}: {why}")
		else:
			frames += int(why.split()[0])
	check(committed and not bad, label, "; ".join(bad) or f"{frames} frames in all")

	print("IMPORT  (a found map that will not load; a read-only target)")
	source = os.path.join(scratch, "import_src")
	os.makedirs(source)
	write_png(os.path.join(source, "stone_albedo.png"), 8, 8, (120, 110, 100, 255))
	write_png(os.path.join(source, "stone_normal.png"), 8, 8, (128, 128, 255, 255))
	rough = os.path.join(source, "stone_roughness.png")
	label = "IMPORT a found map that will not load is said: the file and the loader's reason"
	if fault(label):
		write_png(rough, 8, 8, (200, 200, 200, 255))  # FAULT: a roughness map that loads
	else:
		with open(rough, "wb") as fh:
			fh.write(b"this is not an image, whatever its name says\n")
	dest = os.path.join(scratch, "import_ok")
	code, out = run(baker, "import", source, dest, "bwt_set")
	line = next((l.strip() for l in out.splitlines() if "stone_roughness.png" in l), "")
	check("not loaded" in line and "failed to load image" in line, label, line or "no line names it")
	pngs = [f for f in ("bwt_set.png", "bwt_set_n.png", "bwt_set_mr.png")
			if os.path.isfile(os.path.join(dest, "textures", f))]
	check(code == 0 and len(pngs) == 3, "IMPORT the set still imports", f"exit {code}, {len(pngs)} PNGs")
	labels = ("IMPORT a read-only <name>_n.png fails the import (exit non-zero)",
			  "IMPORT the failure names the file and says why",
			  "IMPORT the read-only file is left as it was")
	dest = os.path.join(scratch, "import_ro")
	target = os.path.join(dest, "textures", "bwt_set_n.png")
	plant_read_only(target, read_only=not fault(*labels))  # FAULT: a writable target
	code, out = run(baker, "import", source, dest, "bwt_set")
	check(code != 0, labels[0], f"exit {code}")
	check(said(out, "bwt_set_n.png", DENIED), labels[1],
		  next((l.strip() for l in out.splitlines() if "bwt_set_n.png" in l), "no line names it"))
	check(read(target) == SENTINEL, labels[2], f"{len(read(target))} bytes")

	print("FLIP  (a normal map's green: by the GL token at the END of its name, or as asked)")
	# Each case imports one albedo and one normal map whose green is GREEN_IN; the
	# packed <name>_n.png comes out with that green, or its flip (code-review C393:
	# any "gl" in the name flipped, and the editor could only ever ask for the
	# flip ON). (label, normal map's name, flags, its name under the fault, flags
	# under the fault, flipped?)
	cases = (
		("FLIP a 'gl' inside a word is not the GL token: jungle_normal keeps its green",
		 "jungle_normal.png", [], "jungle_normal_gl.png", [], False),
		("FLIP Poly Haven's nor_gl_2k is the GL token past its resolution tag: flipped",
		 "rock_nor_gl_2k.png", [], "rock_nor_dx_2k.png", [], True),
		("FLIP --no-flip-green keeps a GL-named map's green",
		 "rock_nor_gl_2k.png", ["--no-flip-green"], "rock_nor_gl_2k.png", [], False),
		("FLIP --flip-green flips a map its name does not mark",
		 "jungle_normal.png", ["--flip-green"], "jungle_normal.png", ["--no-flip-green"], True),
	)
	for n, (label, name, flags, fault_name, fault_flags, flipped) in enumerate(cases):
		if fault(label):
			name, flags = fault_name, fault_flags  # FAULT: the opposite answer is right
		source = os.path.join(scratch, f"flip_src{n}")
		os.makedirs(source)
		write_png(os.path.join(source, "slab_albedo.png"), 8, 8, (120, 110, 100, 255))
		write_png(os.path.join(source, name), 8, 8, (128, GREEN_IN, 255, 255))
		dest = os.path.join(scratch, f"flip_out{n}")
		code, out = run(baker, "import", source, dest, "bwt_flip", *flags)
		packed = os.path.join(dest, "textures", "bwt_flip_n.png")
		greens = sorted({px[1] for px in read_png(packed)}) if os.path.isfile(packed) else []
		want = 255 - GREEN_IN if flipped else GREEN_IN
		line = next((l.strip() for l in out.splitlines() if "Normal:" in l), "no Normal: line")
		check(code == 0 and greens == [want], label, f"exit {code}, green {greens} (want {want}); {line}")
	label = "FLIP --flip-green and --no-flip-green together are refused, and nothing is imported"
	source = os.path.join(scratch, "flip_src0")
	dest = os.path.join(scratch, "flip_both")
	flags = ["--flip-green"] if fault(label) else ["--flip-green", "--no-flip-green"]  # FAULT: one flag
	code, out = run(baker, "import", source, dest, "bwt_flip", *flags)
	check(code != 0 and not os.path.exists(os.path.join(dest, "textures", "bwt_flip_n.png")),
		  label, f"exit {code}")

	print("NAMES  (`rig-names`: joints and clips named to be escaped)")
	names = ['quote"d', "back\\slash", "tab\there", "bell\x07and\x1funit", "Ægir ᚠ",
			 "mixamorig:Hips", '{"json": [1]}', "slash/and \\u0041 kept"]
	listing = os.path.join(scratch, "names.txt")
	with open(listing, "wb") as fh:
		fh.write("\n".join(names).encode("utf-8"))
	rig = os.path.join(scratch, "rig", "named.gltf")
	code, out = run(baker, "rig-names", rig, listing)
	check(code == 0 and os.path.isfile(rig), "NAMES `rig-names` writes the rig", f"exit {code}")
	labels = ("NAMES the rig is strict JSON", "NAMES every joint and clip name reads back exactly")
	text = read(rig).decode("utf-8", "replace") if os.path.isfile(rig) else ""
	# Each check reads its own copy so each takes its own fault: the read-back's
	# leaves the rig valid JSON, so its detail - the names as read, quoted in
	# ASCII (!a: every code point visible, whatever the console) - is printed,
	# the path a mis-written name takes in a real run.
	strict = text
	if fault(labels[0]):
		strict = strict.replace('\\"', '"', 1)  # FAULT: one quote written raw
	try:
		strict_json(strict)
		check(True, labels[0], f"{len(text)} chars")
	except ValueError as e:
		check(False, labels[0], str(e))
	readback = text
	if fault(labels[1]):
		readback = readback.replace('\\t', ' ', 1)  # FAULT: an escaped tab read back as a space
	doc = None
	try:
		doc = strict_json(readback)
	except ValueError:
		pass
	joints = [n.get("name") for n in doc["nodes"][1:]] if doc else []
	clips = [a.get("name") for a in doc.get("animations", [])] if doc else []
	check(joints == names and clips == names, labels[1],
		  f"{len(names)} names" if joints == names and clips == names
		  else f"joints {joints!a}, clips {clips!a}")

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
