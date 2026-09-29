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
#   2. CHECKING DOES NOT CHANGE WHAT A SAVE WRITES: after a validate, savemap
#      writes the active level alone (the checker used to stash every level,
#      and a stashed level is one savemap rewrites).
#   3. THE EDIT COUNTER live validation keys on moves on a change and STAYS PUT
#      on a no-op (a repaint of the same texture), and moves on undo.
#   4. BATCHED FILLS LEAVE NO CHUNK STALE: after a rectangle recolour, a
#      rectangle that raises walls across chunk edges, and a flood, the chunks
#      actually uploaded match a fresh bake (`geomlayout`); undo restores the
#      surfaces exactly.
#   5. THE AREA FILL paints the room or corridor, stopping where narrow meets
#      open: on a carved layout with known answers (a 154-square room, a
#      13-square corridor with a bend and a doorway, a 60-square room, the
#      corridor's 25 wall blocks, 227 floor squares in all) every count must
#      come out exact and every fill must leave the chunks current.
#
# Every project file a phase writes is restored byte for byte afterwards.
import io
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJ = os.path.join(ROOT, r"assets\projects\dungeon-demo")
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

# --- phase 2: a check leaves the save alone -----------------------------------
print("2 - checking does not change what a save writes")
backup = os.path.join(ROOT, r"build\editortest-backup")
shutil.rmtree(backup, ignore_errors=True)
shutil.copytree(PROJ, backup)
try:
    log = run("validatesave.eval")
    check(passed(log), "the script ran clean")
    ran = "console: > validate" in log and "console: > savemap" in log
    check(ran, "both the check and the save ran")
    m = re.search(r"console: saved levels: (.*)", log)
    saved = [s.strip() for s in m.group(1).split(",")] if m else []
    # The level the harness opens in (project.ini eval_level) is the active one.
    check(saved == ["eval_arena"], "savemap wrote the active level alone",
          f"saved: {saved}")
finally:
    shutil.rmtree(PROJ)
    shutil.copytree(backup, PROJ)
    shutil.rmtree(backup, ignore_errors=True)

# --- phase 3: the edit counter ------------------------------------------------
print("3 - the edit counter moves on a change and only on a change")
log = run("editrev.eval")
check(passed(log), "the script ran clean")
revs = [int(r) for r in re.findall(r"console: editor rev (\d+)", log)]
if len(revs) != 4:
    check(False, "four counter readings", f"got {revs}")
else:
    r0, r1, r2, r3 = revs
    check(r1 > r0, "a paint moves it", f"{r0} -> {r1}")
    check(r2 == r1, "repainting the same texture does not", f"{r1} -> {r2}")
    check(r3 > r2, "an undo moves it", f"{r2} -> {r3}")

# --- phase 4: batched fills leave no chunk stale ------------------------------
print("4 - a batched fill rebuilds every chunk it touched")
log = run("chunkbatch.eval")
check(passed(log), "the script ran clean")
h = hashes(log)
layouts = re.findall(r"console: geomlayout \S+ fresh=\w+ live=\w+ (\w+)", log)
if len(h) != 5 or len(layouts) != 5:
    check(False, "five readings", f"got {len(h)} hashes, {len(layouts)} layouts")
else:
    # Non-vacuous: each fill must have changed the geometry, or "no chunk is
    # stale" would hold for fills that touched nothing.
    for i, label in ((1, "the recolour"), (2, "the wall raise"), (3, "the flood")):
        check(h[i] != h[i - 1], f"{label} changed the level")
        check(layouts[i] == "match", f"after {label} the uploaded chunks match a fresh bake",
              layouts[i])
    check(h[4] == h[0], "undoing all of it restores the surfaces exactly")
    fills = re.findall(r"console: editor fill: (\w+) \S+ in ([\d.]+) ms", log)
    print("         fill times (ms): " + ", ".join(f"{k} {t}" for k, t in fills))

# --- phase 5: the area fill ---------------------------------------------------
print("5 - the area fill paints the room or corridor, and stops there")
log = run("areafill.eval")
check(passed(log), "the script ran clean")
areas = [int(n) for n in re.findall(r"editor: Filled the room or corridor \((\d+) cells\)", log)]
# The corridor is the one that matters most: 13 only if the fill turned the
# bend AND stopped at both of its ends (room A directly, room B at a doorway).
expected = [("room A", 154), ("the bent corridor", 13), ("room B", 60),
            ("the corridor's wall blocks", 25)]
if len(areas) != len(expected):
    check(False, "four area fills", f"got {areas}")
else:
    for (label, want), got in zip(expected, areas):
        check(got == want, f"{label}: {want} squares", f"got {got}")
check("editor: Click inside a room or corridor to fill it" in log,
      "a solid square has no area, and says so")
m = re.search(r"editor: Filled the whole level \((\d+) cells\)", log)
check(m is not None and int(m.group(1)) == 227, "fill level: all 227 floor squares",
      m.group(1) if m else "no report")
layouts = re.findall(r"console: geomlayout \S+ fresh=\w+ live=\w+ (\w+)", log)
check(len(layouts) == 6 and all(l == "match" for l in layouts),
      "every fill left the uploaded chunks current", str(layouts))
h = hashes(log)
check(len(h) == 6 and all(h[i] != h[i - 1] for i in range(1, 6)),
      "every fill changed the level")

print()
print("PASS" if failures == 0 else f"FAIL - {failures} check(s) failed")
sys.exit(0 if failures == 0 else 1)
