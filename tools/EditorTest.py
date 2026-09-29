# tools/EditorTest.py - the editor-updates branch's checks, checked
# (docs/editor-updates-plan.md).
#
# Run:  python tools\EditorTest.py      (needs a debug build)
#
# The eval harness REPORTS and never judges (docs/eval-harness.md); this is the
# judge. Each phase runs a script from tools\EvalScripts headless and reads
# dungeon.log. Surfaces are read by GEOMETRY - `geomhash` fingerprints the
# level's walls/floors/ceilings - so "put back" means the hash matches the one
# taken before, not that a message said so.
#
#   1. A PAINT DRAG IS ONE UNDO STEP, even when its first square already had
#      the texture (the step used to be decided on the press and dropped, so
#      Ctrl+Z skipped the drag and undid the edit before it).
import io
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, r"build\debug\bin\Dungeon.exe")
LOG = os.path.join(ROOT, r"build\debug\bin\dungeon.log")
SCRIPTS = os.path.join(ROOT, r"tools\EvalScripts")

failures = 0


def run(script, project="dungeon-demo"):
    # -project keeps the run off whatever world the developer last switched to.
    args = [EXE, "-headless", "-project", project, "-eval", os.path.join(SCRIPTS, script)]
    subprocess.run(args, cwd=ROOT, capture_output=True, timeout=600)
    return io.open(LOG, encoding="utf-8", errors="replace").read()


def check(ok, label, detail=""):
    global failures
    print(f"  {'[ok  ]' if ok else '[FAIL]'} {label}")
    if not ok:
        failures += 1
        if detail:
            print(f"         {detail}")


GEOM = re.compile(r"console: geomhash (\S+) walls=(\w+) floors=(\w+) ceilings=(\w+)")


def hashes(log):
    """Every geomhash line, in order, as (level, walls, floors, ceilings)."""
    return [m.groups() for m in GEOM.finditer(log)]


def passed(log):
    return "eval BATCH RESULT=PASS" in log


# --- phase 1: a drag is one undo step ----------------------------------------
print("1 - a paint drag is one undo step")
log = run("strokeundo.eval")
check(passed(log), "the script ran clean")
h = hashes(log)
if len(h) != 5:
    check(False, "five geometry readings", f"got {len(h)}")
else:
    h0, h1, h2, u1, u2 = h
    # Non-vacuous: each edit must actually MOVE the geometry, or "undo put it
    # back" would be satisfied by edits that did nothing.
    check(h1 != h0, "the place changed the floor")
    check(h2 != h1, "the drag changed the floor again")
    check(u1 == h1, "undo takes off the drag ALONE", f"{u1} vs {h1}")
    check(u2 == h0, "a second undo takes off the place", f"{u2} vs {h0}")

print()
print("PASS" if failures == 0 else f"FAIL - {failures} check(s) failed")
sys.exit(0 if failures == 0 else 1)
