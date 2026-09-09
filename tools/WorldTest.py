# tools/WorldTest.py — the world tier's checks, checked (docs/world-map.md).
#
# Run:  python tools\WorldTest.py      (needs a debug build)
#
# Six phases, all built on one principle: a check that never fires reports
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
#   5. ENTERING AND LEAVING — the round trip through a location, and the
#      obligation that `reset` still means a LEVEL now that a new game opens on
#      the world map.
#   6. TWO DOORS — a dungeon with a front gate and a back way, which land in
#      different parts of it and surface in different parts of the world.
#   4. TRAVEL — a journey costs the time its terrain says and the supplies that
#      span buys, refuses an impassable square instead of clamping, and reveals
#      what walking past a place should reveal. The times are read off
#      terrain.cat, never off the run.
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

    # --- phase 4: travel is a journey ---------------------------------------
    print("\n4 - travel costs what the terrain says")
    log = run("worldtravel.eval")
    travelled = [l.strip() for l in log.splitlines() if "console: travelled" in l]
    party = party_lines(log)
    supplies = [l for l in log.splitlines() if "console:   [0] Brand" in l]

    # 3 road squares at 0.50h, then grass 1.00h + moor 1.40h. Read off
    # terrain.cat, NOT off the run: a test that checks a number against itself
    # would pass whatever the travel cost happened to become.
    check(any("to 9,6 - 1.50h" in l for l in travelled),
          "3 road squares cost 3 x 0.50h", " / ".join(travelled[:2]))
    check(any("to 9,4 - 3.90h" in l for l in travelled),
          "then grass 1.00h + moor 1.40h = 3.90h total", " / ".join(travelled[:3]))

    # The impassable case: SOME movement, then a stop, and a report saying so.
    check(any("1 of 4" in l and "(blocked)" in l for l in travelled),
          "walking into water stops short and says it was blocked",
          " / ".join(travelled))

    # Supplies fall BECAUSE of the journey - the before/after pair is the
    # check, and the before line is the control.
    check(len(supplies) >= 2,
          f"supplies reported before and after (got {len(supplies)})")
    if len(supplies) >= 2:
        check("100.0/100" in supplies[0], "the party set out fully supplied",
              supplies[0].strip())
        check("100.0/100" not in supplies[-1],
              "and 3.90h of travel cost it food and water", supplies[-1].strip())

    # Discovery is the STEP's doing: the line before it must say discovered 0,
    # or a location found earlier would make this pass for the wrong reason.
    disc = [l for l in party if "discovered" in l]
    check(len(disc) >= 2 and "discovered 0" in disc[-2] and
          "discovered 1" in disc[-1],
          "stepping within sight of a location discovers it",
          " / ".join(x.strip() for x in disc[-2:]))
    # --- phase 5: in and out of a dungeon ------------------------------------
    print("\n5 - entering and leaving a dungeon")
    log = run("worldenter.eval")
    states = [l.split("console: state ")[-1].strip()
              for l in log.splitlines() if "console: state " in l]
    maps = [l for l in log.splitlines() if "map, start" in l]

    # THE OBLIGATION: `reset` still leaves the party in a LEVEL, even though a
    # new game now opens on the world map. Both halves are checked - the state
    # AND that a real map is loaded under it, because "playing" alone would be
    # satisfied by a party standing in a field.
    check(states and states[0] == "playing",
          "reset still leaves the party in a level, not on the world map",
          f"states: {states}")
    check(any("28x24 map" in m for m in maps),
          "and a real level is loaded under it", " / ".join(maps[:1]))

    check("state worldmap" in log, "leaving a dungeon reaches the world map",
          f"states: {states}")
    # Discovery is a GATE, not decoration: the refusal before the discover is
    # the evidence, and without it the success after would prove nothing.
    check("could not enter waystation" in log,
          "an undiscovered location refuses to be entered")
    check("entering waystation" in log, "and a discovered one lets you in")
    check(states and states[-1] == "playing",
          "which lands the party back in a level", f"states: {states}")
    check("inside waystation" in log,
          "and the world remembers which location it went in by")

    print("\nNOT covered here: the EXIT STAIR itself. It fires on a party STEP,")
    print("and the console can only teleport (`tp` sets the cell without")
    print("stepping), so walking onto it is checked by driving the real game.")
    # --- phase 6: a dungeon with two ways in --------------------------------
    print("\n6 - two doors into one dungeon")
    log = run("worldback.eval")

    # The back way opens the SAME dungeon on a DIFFERENT level. showcase is
    # 28x24 and level2 is 55x15, so the map line alone says which one opened.
    check("55x15 map" in log,
          "the back way opens a different level of the same dungeon")
    # ...and at ITS cell, not the level's start. level2 starts at 3,3, which
    # is the control: landing there would mean the location's entry was
    # ignored and the front door's rule applied.
    check("9,4 facing south" in log,
          "landing on the location's own cell, not the level's start (3,3)")
    check("start 3,3" in log,
          "and the level really does start elsewhere (the control)")

    # Out by the FRONT after coming in the BACK: an exit knows its own door.
    # 10,10 is the front location, 5,13 the back one.
    check("back on the world at 10,10" in log,
          "leaving by the front surfaces at the front, not where you came in")
    # Coming out of a door finds it - which is how a back way is discovered
    # from the inside. One discovered going in, two coming out.
    check("discovered 2" in log,
          "and coming out of a door discovers it")
finally:
    for p, s in originals.items():
        write(p, s)

print(f"\nworldtest RESULT={'FAIL' if failures else 'PASS'} failures={failures}")
sys.exit(1 if failures else 0)
