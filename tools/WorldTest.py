# tools/WorldTest.py — the world tier's checks, checked (docs/world-map.md).
#
# Run:  python tools\WorldTest.py      (needs a debug build)
#
# Three phases, all built on one principle: a check that never fires reports
# "clean" just as loudly as one that works, so every expectation here is paired
# with something that makes it fail.
#
#   1. VALIDATION — break the world data nine ways, one at a time, and demand
#      the matching check fire. The baseline is asserted clean first, so a
#      finding cannot be something that was already there.
#   2. THE SAVE ROUND TRIP — move all three parts of the global tier OFF their
#      new-game values before saving, because a round trip that starts from the
#      defaults would be reproduced exactly by a save that wrote nothing. The
#      new-game values are read first and used as the control.
#   3. THE VERSION FLOOR — downgrade a save on disk and demand the load is
#      REFUSED rather than half-understood.
#
# Every file this touches is restored, including the save it downgrades.
import io
import os
import shutil
import subprocess
import sys

ROOT = r"C:\Dev\Dungeon-world-map"
PROJ = os.path.join(ROOT, r"assets\projects\dungeon-demo")
WORLD = os.path.join(PROJ, r"world\world.map")
DUNGEONS = os.path.join(PROJ, r"catalog\dungeons.cat")
EXE = os.path.join(ROOT, r"build\debug\bin\Dungeon.exe")
LOG = os.path.join(ROOT, r"build\debug\bin\dungeon.log")
SCRIPTS = os.path.join(ROOT, r"tools\EvalScripts")
SAVE = os.path.join(os.environ["USERPROFILE"],
                    r"OneDrive\Documents\DungeonSaves\worldtrip.dsav")

failures = 0


def read(p):
    return io.open(p, encoding="utf-8", newline="").read()


def write(p, s):
    io.open(p, "w", encoding="utf-8", newline="").write(s)


def run(script):
    subprocess.run([EXE, "-headless", "-eval", os.path.join(SCRIPTS, script)],
                   cwd=ROOT, capture_output=True, timeout=600)
    return io.open(LOG, encoding="utf-8", errors="replace").read()


def check(ok, label, detail=""):
    global failures
    print(f"  {'[ok  ]' if ok else '[FAIL]'} {label}")
    if not ok:
        failures += 1
        if detail:
            print(f"         {detail}")


def party_lines(log):
    """Every 'party  x,z ...' line the `world` command printed, in order."""
    return [l.strip() for l in log.splitlines() if "console:   party" in l]


# --- phase 1: the validation checks fire ------------------------------------
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
    print("1 - the world checks fire when the world is broken")
    log = run("worldcheck.eval")
    baseline = log[log.rfind("> validate"):]
    if "clean - no faults found" not in baseline:
        print("BASELINE NOT CLEAN - a mutation's finding would be ambiguous:")
        print(baseline[:600])
        sys.exit(2)
    check(True, "baseline is clean, so a finding below is the mutation's")

    # The new-game world state, kept as phase 2's control.
    newgame = party_lines(log)
    if not newgame:
        print("the `world` command printed no party line - cannot continue")
        sys.exit(2)
    control = newgame[-1]

    for name, path, find, repl, key in CASES:
        s = originals[path]
        assert s.count(find) == 1, f"{name}: anchor not unique in {path}"
        write(path, s.replace(find, repl, 1))
        try:
            log = run("worldcheck.eval")
            tail = log[log.rfind("> validate"):]
        finally:
            write(path, originals[path])
        check(key in tail, f"{name} -> {key}",
              " / ".join(l.strip() for l in tail.splitlines()[:4]))

    # --- phase 2: the global tier survives a save/load round trip -----------
    print("\n2 - the global tier survives a save and load")
    log = run("worldsave.eval")
    lines = party_lines(log)
    check(len(lines) >= 2, f"the script reported before and after (got {len(lines)})")
    if len(lines) >= 2:
        before, after = lines[0], lines[-1]
        # The control: the values being round-tripped are NOT the ones a new
        # game starts with, so a save that wrote nothing could not reproduce them.
        check(before != control,
              "the state was moved off its new-game values before saving",
              f"new game: {control}")
        check(after == before, "and came back identical after the load",
              f"before: {before}\n         after:  {after}")
        for want in ("3,12", "seen 2", "discovered 1"):
            check(want in after, f"round-tripped {want}", after)

    # --- phase 3: an older save is refused, not half-read -------------------
    print("\n3 - a save older than the floor is refused")
    if not os.path.exists(SAVE):
        check(False, "phase 2 left a save to downgrade", SAVE)
    else:
        shutil.copy(SAVE, SAVE + ".bak")
        try:
            write(SAVE, read(SAVE).replace("save version=26", "save version=25", 1))
            log = run("worldload.eval")
            check("older than the minimum" in log,
                  "the log says which version was refused and what the floor is")
            check("LoadGame: could not read" in log, "and the load did not happen")
            check("eval RESULT=PASS" in log, "while the game itself kept running")
        finally:
            shutil.move(SAVE + ".bak", SAVE)
finally:
    for p, s in originals.items():
        write(p, s)

print(f"\nworldtest RESULT={'FAIL' if failures else 'PASS'} failures={failures}")
sys.exit(1 if failures else 0)
