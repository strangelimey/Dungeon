# tools/TemplateTest.py - the new-world template is what tools/BuildTemplate.py
# makes of dungeon-demo (code-review C439).
#
# Run:  python tools\TemplateTest.py [--selftest]
#
# assets\templates\default is DEFINED BY A SCRIPT (the Build*.py rule): a blank
# new world starts from it, and the script makes it from dungeon-demo. Its
# manifest used to carry a hand-typed start_items, which fell behind the source's
# (fire_flask was never in it), so a party made in a new world could not pick
# what the shipped world offers. The script now READS those fields; this judge
# runs it into a scratch folder (the tree is never written) and holds the result
# to three things:
#
#   FIELDS     each field the template inherits (BuildTemplate's SOURCE_FIELDS:
#              default_sconce, default_brazier, start_items) is dungeon-demo's,
#              start_items compared item by item.
#   ITEMS      every start_items pick is an item of the template's own catalogs
#              (items / weapons / armor), or a new world's party page offers an
#              id nothing defines.
#   TREE       the scratch build is byte for byte the template ON DISK, the
#              assets\templates\default a new world copies: the manifest, the
#              catalogs, and the set of files. A drift here means dungeon-demo
#              moved (its working tree, which BuildTemplate reads) and
#              BuildTemplate.py was not re-run - and re-running it is the whole
#              remedy. The git INDEX is deliberately not the reference: the
#              build reads the working tree, and an editor save lands there, so
#              a check against the index would go on failing after the re-run
#              until the rebuilt template was staged (a new catalog file, until
#              it was added). Committing the two together is what `git status`
#              is for.
#
# --selftest plants a fault in the scratch build for each group - start_items
# loses fire_flask (the C439 shape), the [dagger] weapon a pick names is renamed
# - and demands that EXACTLY the checks resting on them fail.
#
# Exit: 0 PASS (or, under --selftest, every planted fault caught and nothing
# else); 1 FAIL; 2 nothing judged (the script would not run).
import io
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "assets", "projects", "dungeon-demo")
TEMPLATE = os.path.join(ROOT, "assets", "templates", "default")
TOOL = "templatetest"
ITEM_CATALOGS = ("items.cat", "weapons.cat", "armor.cat")

results = []  # (label, ok)


def check(ok, label, detail=""):
	results.append((label, bool(ok)))
	print(f"  [{'ok  ' if ok else 'FAIL'}] {label}" + (f"  ({detail})" if detail else ""))
	return ok


def fields(path):
	"""The `key = value` lines of a block-format file, first value of each key."""
	out = {}
	for line in io.open(path, encoding="utf-8").read().splitlines():
		s = line.strip()
		if not s or s.startswith(";") or s.startswith("[") or "=" not in s:
			continue
		key, value = s.split("=", 1)
		out.setdefault(key.strip(), value.strip())
	return out


def id_list(value):
	return [t.strip() for t in value.replace(",", " ").split() if t.strip()]


def catalog_ids(path):
	ids = set()
	for line in io.open(path, encoding="utf-8").read().splitlines():
		s = line.strip()
		if s.startswith("[") and s.endswith("]"):
			ids.add(s[1:-1].strip())
	return ids


def tree_files(folder):
	"""{relative path: bytes} of every file under `folder` - the scratch build,
	or the template on disk (checked out through .gitattributes' `eol=crlf`,
	which is what the script writes)."""
	out = {}
	for d, _, names in os.walk(folder):
		for n in names:
			full = os.path.join(d, n)
			out[os.path.relpath(full, folder)] = open(full, "rb").read()
	return out


def plant(scratch):
	"""The self-test's faults, written into the scratch build."""
	ini = os.path.join(scratch, "project.ini")
	text = io.open(ini, encoding="utf-8", newline="").read()
	lines = text.splitlines(True)
	for i, line in enumerate(lines):
		if line.strip().startswith("start_items"):
			key, value = line.split("=", 1)
			kept = [t for t in id_list(value) if t != "fire_flask"]
			lines[i] = "%s= %s\r\n" % (key, ", ".join(kept))
	io.open(ini, "w", encoding="utf-8", newline="").write("".join(lines))
	weapons = os.path.join(scratch, "catalog", "weapons.cat")
	text = io.open(weapons, encoding="utf-8", newline="").read()
	if "[dagger]" not in text:
		raise RuntimeError("the self-test's plant needs a [dagger] in weapons.cat")
	io.open(weapons, "w", encoding="utf-8", newline="").write(text.replace("[dagger]", "[templatetest_dagger]", 1))


def judge(scratch, selftest):
	expected = set()
	if selftest:
		plant(scratch)
		expected |= {"FIELDS start_items", "ITEMS", "TREE project.ini", "TREE catalogs"}

	source = fields(os.path.join(SOURCE, "project.ini"))
	built = fields(os.path.join(scratch, "project.ini"))
	for key in ("default_sconce", "default_brazier", "start_items"):
		want, got = source.get(key), built.get(key)
		if key == "start_items":
			want_l, got_l = id_list(want or ""), id_list(got or "")
			missing = [t for t in want_l if t not in got_l]
			extra = [t for t in got_l if t not in want_l]
			check(want_l == got_l, "FIELDS " + key,
				  "%d picks" % len(got_l) if want_l == got_l else
				  "missing %s, extra %s" % (missing or "-", extra or "-"))
		else:
			check(want == got, "FIELDS " + key, "%s / %s" % (want, got))

	known = set()
	for name in ITEM_CATALOGS:
		path = os.path.join(scratch, "catalog", name)
		if os.path.isfile(path):
			known |= catalog_ids(path)
	picks = id_list(built.get("start_items", ""))
	unknown = [p for p in picks if p not in known]
	check(picks and not unknown, "ITEMS",
		  "%d picks, all defined" % len(picks) if picks and not unknown else
		  ("no start_items" if not picks else "not an item: " + ", ".join(unknown)))

	disk = tree_files(TEMPLATE)
	made = tree_files(scratch)
	rerun = " - re-run tools\\BuildTemplate.py"
	check(set(disk) == set(made), "TREE file set",
		  "%d files" % len(made) if set(disk) == set(made) else
		  "only built: %s; only on disk: %s%s" % (sorted(set(made) - set(disk)) or "-",
												  sorted(set(disk) - set(made)) or "-", rerun))
	check(made.get("project.ini") == disk.get("project.ini"), "TREE project.ini",
		  "" if made.get("project.ini") == disk.get("project.ini") else "differs" + rerun)
	cats = sorted(k for k in set(disk) | set(made) if k != "project.ini")
	drift = [k for k in cats if made.get(k) != disk.get(k)]
	check(not drift, "TREE catalogs",
		  "%d files identical" % len(cats) if not drift else "differ: " + ", ".join(drift) + rerun)

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


def main():
	selftest = "--selftest" in sys.argv
	scratch_root = tempfile.mkdtemp(prefix="templatetest-")
	try:
		scratch = os.path.join(scratch_root, "template")
		r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "BuildTemplate.py"), "--out", scratch],
						   capture_output=True, text=True, errors="replace")
		if r.returncode != 0 or not os.path.isfile(os.path.join(scratch, "project.ini")):
			print(r.stdout + r.stderr)
			print(f"{TOOL} RESULT=FAIL checks=0 failures=0 self_test={int(selftest)} - BuildTemplate.py did not run")
			return 2
		print("built the template into " + scratch)
		return judge(scratch, selftest)
	finally:
		shutil.rmtree(scratch_root, ignore_errors=True)


if __name__ == "__main__":
	sys.exit(main())
