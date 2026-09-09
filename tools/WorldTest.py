# tools/WorldTest.py — the world tier's checks, checked (docs/world-map.md).
#
# Run:  python tools\WorldTest.py      (needs a debug build)
#
# Mutation check for the world-tier validation.
#
# A clean baseline proves nothing on its own: a check that never fires reports
# "clean" just as loudly as one that works. So break the world data one way at
# a time and demand the matching check fires. The harness restores every file
# it touches, and asserts the baseline is clean first so a mutation's finding
# cannot be something that was already there.
import io, os, shutil, subprocess, sys

ROOT = r"C:\Dev\Dungeon-world-map"
PROJ = os.path.join(ROOT, r"assets\projects\dungeon-demo")
WORLD = os.path.join(PROJ, r"world\world.map")
DUNGEONS = os.path.join(PROJ, r"catalog\dungeons.cat")
EXE = os.path.join(ROOT, r"build\debug\bin\Dungeon.exe")
LOG = os.path.join(ROOT, r"build\debug\bin\dungeon.log")
SCRIPT = os.path.join(ROOT, r"tools\EvalScripts\worldcheck.eval")


def read(p):
    return io.open(p, encoding="utf-8", newline="").read()


def write(p, s):
    io.open(p, "w", encoding="utf-8", newline="").write(s)


def run():
    subprocess.run([EXE, "-headless", "-eval", SCRIPT], cwd=ROOT,
                   capture_output=True, timeout=600)
    return read(LOG)


# (name, file, find, replace, expected loc key)
CASES = [
    ("location names no dungeon", WORLD,
     "location dungeon waystation 10 10", "location dungeon nowhere 10 10",
     "map.check.worldnodungeon"),
    ("location on impassable terrain", WORLD,
     "location dungeon waystation 10 10", "location dungeon waystation 0 0",
     "map.check.worldblocked"),
    ("world start on impassable terrain", WORLD,
     "start 6 6", "start 0 0", "map.check.worldstart"),
    ("dungeon names a missing level", DUNGEONS,
     "levels = showcase start level2", "levels = showcase start level2 basement",
     "map.check.dungeonnolevel"),
    ("dungeon entry is not its own level", DUNGEONS,
     "entry = showcase", "entry = level9", "map.check.dungeonentry"),
    ("two dungeons claim one level", DUNGEONS,
     "tags = stone", "tags = stone\n\n[rival]\nlevels = showcase\nentry = showcase",
     "map.check.levelshared"),
    ("dungeon with no levels", DUNGEONS,
     "tags = stone", "tags = stone\n\n[hollow]\nlevels =\nentry =",
     "map.check.dungeonnolevels"),
    ("dungeon no location reaches", DUNGEONS,
     "tags = stone", "tags = stone\n\n[unreached]\nlevels = level2\nentry = level2",
     "map.check.dungeonunreached"),
    ("level no dungeon claims", DUNGEONS,
     "levels = showcase start level2", "levels = showcase start",
     "map.check.levelorphan"),
]

originals = {p: read(p) for p in (WORLD, DUNGEONS)}
try:
    log = run()
    tail = log[log.rfind("> validate"):]
    if "clean - no faults found" not in tail:
        print("BASELINE NOT CLEAN — a mutation's finding would be ambiguous:")
        print(tail)
        sys.exit(2)
    print("baseline           clean")

    failures = 0
    for name, path, find, repl, key in CASES:
        s = originals[path]
        assert s.count(find) == 1, f"{name}: anchor not unique in {path}"
        write(path, s.replace(find, repl, 1))
        try:
            log = run()
            tail = log[log.rfind("> validate"):]
            ok = key in tail
        finally:
            write(path, originals[path])
        print(f"{'PASS' if ok else 'FAIL'}  {name:<34} -> {key}")
        if not ok:
            failures += 1
            print("      " + " / ".join(l.strip() for l in tail.splitlines()[:6]))
    print(f"\n{len(CASES) - failures}/{len(CASES)} checks fired")
    sys.exit(1 if failures else 0)
finally:
    for p, s in originals.items():
        write(p, s)
