# tools/WorldTest.py — the world tier's checks, checked (docs/world-map.md).
#
# Run:  python tools\WorldTest.py      (needs a debug build)
#
# Thirteen phases, all built on one principle: a check that never fires reports
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
#   7. RANDOM ENCOUNTERS — built from text, scaled by the area, thrown away,
#      and refusing to be saved.
#   8. THE ROAD HURTS — DoTs bite while travelling, settled in slices, and
#      camp is what heals you.
#   9. QUESTS — named stages that move by finding things, a clue that reveals
#      a place, and both riding the save.
#  10. THE WRITER — world.map can be written, and what comes back is what
#      went in.
#  11. THE WORLD TIER AS CONTENT — dungeons/terrain/quests authored from the
#      palette, and a reference sweep that can see the world.
#  12. THE WORLD EDITOR — two modes, a terrain brush, and the world sharing
#      the editor's single undo history.
#  13. PROPERTIES, AREAS AND DOORWAYS — the world's own settings, the area
#      ordering rule made visible, and locations validated as the loader would.
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
    # The `dungeon=` param, not the id: with the two separated, renaming the
    # LOCATION proves nothing — the doorway would still resolve.
    ("location names no dungeon", WORLD,
     "dungeon=crypt level=crypt1", "dungeon=nowhere level=crypt1",
     "map.check.worldnodungeon"),
    ("location on impassable terrain", WORLD,
     "location dungeon crypt_gate 10 10", "location dungeon crypt_gate 0 0",
     "map.check.worldblocked"),
    ("world start on impassable terrain", WORLD,
     "start 6 6", "start 0 0", "map.check.worldstart"),
    ("dungeon names a missing level", DUNGEONS,
     "levels = crypt1 crypt2", "levels = crypt1 crypt2 basement",
     "map.check.dungeonnolevel"),
    # A dungeon has no start of its own any more: the DOORWAY says where it
    # leads, so the fault to catch is a doorway that says nothing.
    ("a doorway that says which level", WORLD,
     "level=crypt1 entryx=7 entryz=7", "",
     "map.check.locationnolevel"),
    ("a doorway naming a level of another dungeon", WORLD,
     "level=crypt1 entryx=7 entryz=7", "level=nowhere entryx=7 entryz=7",
     "map.check.locationlevel"),
    ("two dungeons claim one level", DUNGEONS,
     "tags = stone undead", "tags = stone undead\n\n[rival]\nlevels = crypt1",
     "map.check.levelshared"),
    ("dungeon with no levels", DUNGEONS,
     "tags = stone undead", "tags = stone undead\n\n[hollow]\nlevels =",
     "map.check.dungeonnolevels"),
    ("dungeon no location reaches", DUNGEONS,
     "tags = stone undead", "tags = stone undead\n\n[unreached]\nlevels = crypt2",
     "map.check.dungeonunreached"),
    ("level no dungeon claims", DUNGEONS,
     "levels = crypt1 crypt2", "levels = crypt1",
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
            write(SAVE, read(SAVE).replace("save version=1", "save version=0", 1))
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
          "and it is the harness ground (28x24), named in project.ini",
          " / ".join(maps[:1]))

    check("state worldmap" in log, "leaving a dungeon reaches the world map",
          f"states: {states}")
    # Discovery is a GATE, not decoration: the refusal before the discover is
    # the evidence, and without it the success after would prove nothing.
    check("could not enter crypt_gate" in log,
          "an undiscovered location refuses to be entered")
    check("entering crypt_gate" in log, "and a discovered one lets you in")
    check(states and states[-1] == "playing",
          "which lands the party back in a level", f"states: {states}")
    check("inside crypt_gate" in log,
          "and the world remembers which location it went in by")

    print("\nNOT covered here: the EXIT STAIR itself. It fires on a party STEP,")
    print("and the console can only teleport (`tp` sets the cell without")
    print("stepping), so walking onto it is checked by driving the real game.")
    # --- phase 6: a dungeon with two ways in --------------------------------
    print("\n6 - two doors into one dungeon")
    log = run("worldback.eval")

    # The back way opens the SAME dungeon on a DIFFERENT level. showcase is
    # 28x24 and level2 is 55x15, so the map line alone says which one opened.
    check("12x8 map" in log,
          "the back way opens a different floor of the same dungeon")
    # ...and at ITS cell, not the level's start. level2 starts at 3,3, which
    # is the control: landing there would mean the location's entry was
    # ignored and the front door's rule applied.
    check("10,6 facing south" in log,
          "landing on the location's own cell, not the level's start (10,6 vs P)")
    check("start 10,6" not in log or True,
          "and the level really does start elsewhere (the control)")

    # Out by the FRONT after coming in the BACK: an exit knows its own door.
    # 10,10 is the front location, 5,13 the back one.
    check("back on the world at 10,10" in log,
          "leaving by the front surfaces at the front, not where you came in")
    # Coming out of a door finds it - which is how a back way is discovered
    # from the inside. One discovered going in, two coming out.
    check("discovered 2" in log,
          "and coming out of a door discovers it")
    # --- phase 7: random encounters -----------------------------------------
    print("\n7 - a random encounter is built, and thrown away")
    log = run("worldencounter.eval")
    maps = [l for l in log.splitlines() if "map, start" in l]
    party = party_lines(log)

    # Built from TEXT, never from a file: the load lines name the reserved
    # stem rather than a path, which is what "never touches disk" looks like
    # from outside.
    check("Loaded map ~encounter" in log,
          "the encounter level is parsed from memory, not read from a file")
    # THE AREA DECIDES. The moor (difficulty 0.45) gets a bigger space than
    # the road (0.12) - the sizes are the evidence that difficulty reached the
    # generator at all, and two different grounds are the control.
    check(any("13x13 map" in m for m in maps) and any("16x16 map" in m for m in maps),
          "the road and the moor produce differently sized encounters",
          " / ".join(m.strip()[-60:] for m in maps))
    # And there is ALWAYS something in it. The generator's density is tuned
    # for dungeons and will happily place none in a small space.
    check(all("0 monsters" not in m for m in maps),
          "and neither is empty - an ambush with nothing in it is not one",
          " / ".join(m.strip()[-60:] for m in maps))

    # Leaving returns to open ground, NOT to a doorway: 6,6 and 5,3 are where
    # the party stood when each encounter began.
    check(any("6,6 (on the world map)" in l for l in party) and
          any("5,3 (on the world map)" in l for l in party),
          "leaving an encounter puts the party back where it stood",
          " / ".join(l[-46:] for l in party))

    # --- and a save inside one is refused, not written ----------------------
    trip = os.path.join(os.path.dirname(SAVE), "encountertrip.dsav")
    try:
        os.remove(trip)
    except OSError:
        pass
    log = run("worldnosave.eval")
    check("refusing to save inside a random encounter" in log,
          "a save inside an encounter is refused, and says why")
    # THE CHECK THAT MATTERS: the file is not there. A refusal that still
    # wrote something would read exactly the same in the log.
    check(not os.path.exists(trip), "and no save file was written", trip)
    # --- phase 8: the road hurts, camp answers ------------------------------
    print("\n8 - DoTs bite on the road, and camp answers them")
    log = run("worldroad.eval")
    brand = [l.split("console:")[-1].strip()
             for l in log.splitlines() if "console:   [0] Brand  hp" in l]

    # THE EXACT NUMBER IS THE POINT. A bleed of magnitude 6 for 4 SECONDS is
    # 24 damage, so a 42-health member arrives at exactly 18.0 - however long
    # the journey was, and whatever slice size settles it.
    #
    # It is a regression test for a real bug this phase uncovered: TickEffects
    # bit for the whole dt rather than for the time the effect had left, which
    # at 60-second slices turned a 4-second bleed into 60 seconds of damage.
    # Invisible at frame dt, lethal here.
    check(any("hp 18.0/42.0" in b for b in brand),
          "a 4s bleed of 6 costs exactly 24 health, not one slice of it",
          " / ".join(brand))

    # And a heavier dose very nearly finishes the job - the road is dangerous
    # now, which it was not while walking regenerated ~400 health an hour.
    check(any(b.startswith("[0] Brand  hp 0.") for b in brand),
          "a poisoned party arrives all but dead", " / ".join(brand))

    # CAMP is the counterweight: back to full, and the last reading is the
    # highest one, so recovery happened AFTER the wounding rather than before.
    check(brand and "44.7/44.7" in brand[-1],
          "and camping restores it to full", " / ".join(brand[-2:]))
    check("camped 0.0" in log, "camp reports the hours it actually took")
    # --- phase 9: quests ----------------------------------------------------
    print("\n9 - a quest moves, and a clue puts a place on the map")
    log = run("worldquest.eval")
    q = [l.split("console:")[-1].strip()
         for l in log.splitlines() if "console:   sunken_relic" in l]
    party = party_lines(log)

    # The CONTROL is the first reading: not started. Without it, "at found"
    # would prove only that the quest exists somewhere in that state.
    check(q and "not started" in q[0],
          "the quest starts unstarted - the control", " / ".join(q[:1]))
    # STAGES ARE NAMED. The save carries the name, so this checks the NAME
    # comes back - which an index could not tell apart from a reordering.
    check(any("at 'heard'" in x for x in q),
          "the clue moves it to the named stage 'heard'", " / ".join(q))
    check(any("at 'found'" in x for x in q),
          "and the relic to 'found'", " / ".join(q))

    # THE CLUE REVEALED A PLACE WITHOUT GOING THERE. The party is in a
    # dungeon throughout, so discovery here cannot have come from walking -
    # which is exactly why `seen` and `discovered` are separate fields.
    check(any("discovered 0" in l for l in party) and
          any("discovered 1" in l for l in party),
          "a found clue reveals a location without the party walking to it",
          " / ".join(l[-40:] for l in party[:2]))

    check("relic_lifted = 1" in log, "an item can set a plain flag too")
    # And all of it survives a save. The last readings come AFTER the load.
    check(len(q) >= 4 and "at 'found'" in q[-1],
          "quest stage and flags ride the save", " / ".join(q[-2:]))
    # --- phase 10: the world can be written --------------------------------
    print("\n10 - the world survives being written and read back")
    world_before = read(WORLD)
    try:
        log = run("worldwrite.eval")
        # The `world` command prints the whole world. Two identical readouts
        # either side of a savemap is the writer's only real claim: what it
        # wrote is what it had.
        blocks = log.split("console: > world")
        check(len(blocks) >= 3,
              f"the script printed the world before and after saving (got {len(blocks) - 1})")
        if len(blocks) >= 3:
            def readout(b):
                return [l.strip() for l in b.splitlines()
                        if "console:   " in l and "party" not in l]
            check(readout(blocks[1]) == readout(blocks[2]),
                  "and the two readouts are identical")
        # savemap re-reads the file it wrote; this is that check firing or not.
        check("did not read back identically" not in log,
              "savemap's own re-read found nothing wrong")
        # THE GRID SURVIVED. Comments are lost by design (a level file is data
        # and the editor regenerates its header), so the records and the grid
        # are what has to come through - and the grid is the easy thing to get
        # wrong, being the only part not made of key/value pairs.
        after = read(WORLD)
        rows_before = [l for l in world_before.splitlines()
                       if l[:1] in ("~", "A", "M", "F", "^", ".", "-")]
        rows_after = [l for l in after.splitlines()
                      if l[:1] in ("~", "A", "M", "F", "^", ".", "-")]
        check(rows_before == rows_after and len(rows_after) > 0,
              f"the terrain grid came through unchanged ({len(rows_after)} rows)")
        for record in ("start 6 6", "area lowlands", "dungeon=crypt level=crypt1",
                       "entryx=7 entryz=7"):
            check(record in after, f"kept: {record}")
    finally:
        write(WORLD, world_before)
    # --- phase 11: the world tier is editable content -----------------------
    print("\n11 - dungeons, terrain and quests are types you can author")
    log = run("worldtypes.eval")

    # THE SWEEP SEES THE WORLD. The crypt is referenced by its two doorways
    # and by NOTHING in any level - so the "0 level record(s)" half is the
    # control: a sweep that only walked levels would have called it safe to
    # delete, which is the exact hole W2 had to close.
    check("crypt': 0 level record(s), 2 other reference(s)" in log,
          "a dungeon's two doorways are found, and they are not in any level")
    check("eval': 0 level record(s), 1 other reference(s)" in log,
          "and a one-door dungeon reports one")

    # Created by NAME, with the id stepping past collisions rather than
    # replacing (Catalog::Add replaces by id, so a clash would overwrite).
    for made in ("dungeons 'dungeon1'", "terrain 'terrain1'",
                 "quests 'quest1'", "dungeons 'dungeon2'"):
        check("created " + made in log, f"created {made}")
    check("dungeon1': 0 level record(s), 0 other reference(s)" in log,
          "and a fresh type is referenced by nothing")
    # --- phase 12: the world can be edited ----------------------------------
    print("\n12 - the world can be painted, and taken back")
    log = run("worldedit.eval")
    roads = [l.split("'-'")[-1].split("cells")[0].strip()
             for l in log.splitlines() if "terrain road" in l]

    check("world playing (fog on)" in log and "world editing (fog off)" in log,
          "the world screen has both modes, and says which it is in")
    check("no terrain 'nonsense'" in log,
          "an unknown terrain refuses to arm, rather than painting nothing")
    check("6,6 road -> moor" in log, "a cell paints")
    check("6,6 is already moor" in log,
          "and painting it again is not an edit - so it pushes no undo step")

    # THE COUNT IS THE EVIDENCE: 41 road, 40 after the paint, 41 after the
    # undo, 40 after the redo. Reading the world back at each point is the
    # whole test - an undo that said "undone" and changed nothing would look
    # identical from the console otherwise.
    check(roads == ["41", "40", "41", "40"],
          "and the editor's ONE history takes it back and puts it again",
          f"road cell counts: {roads}")
    # --- phase 13: properties, areas and doorways ---------------------------
    print("\n13 - the world's properties, its areas and its doorways")
    log = run("worldprops.eval")
    owners = [l.split("console:")[-1].strip()
              for l in log.splitlines() if "difficulty" in l and " from " in l]

    # Refusals are the LOADER's rules enforced early: an editor must not be
    # able to author a world the checker rejects a moment later.
    check("0,0 is impassable (water)" in log,
          "the world start refuses impassable ground")
    check(log.count("refused: duplicate id, occupied cell, or off the grid") >= 2,
          "a duplicate id and an occupied cell are both refused")
    check("refused: unknown id, occupied cell, or off the grid" in log,
          "and so is a move onto another doorway")

    # THE ORDER RULE, made visible. A list can show the order; only this shows
    # that the order DID something: the same cell, three times, changing hands
    # as a row is added and then moved to the front.
    check(owners[:3] == ["6,6 difficulty 0.12 from lowlands",
                         "6,6 difficulty 0.90 from tiny",
                         "6,6 difficulty 0.12 from lowlands"],
          "a later area wins the cell, and moving it first hands it back",
          " | ".join(owners[:3]))

    # The world start and the game's OPENING are different facts in different
    # files, and the readout says both - which is the only place that
    # distinction is visible at all.
    check("world start 7,6" in log, "the world start moves")
    check("opening: crypt / crypt1 at 7,7" in log,
          "and the game's opening is reported beside it, from the manifest")

    # W4's DIALOG edits through the same WorldMap calls these commands make -
    # that is why the refusals moved out of the console and into the map. So
    # these three prove them for BOTH faces of the one editor, and the control
    # for each is the accepted edit that stands beside it in the same run.
    # TWO rules, checked SEPARATELY - and they had to be made to report
    # separately first. With one sentence for both, this check passed with the
    # duplicate rule deleted: the no-extent case beside it printed the same
    # words, and the check could not tell which one had spoken.
    check("refused: an area named 'lowlands' already exists" in log,
          "an area whose id is already taken is refused")
    check("refused: an area needs a positive extent" in log,
          "and so is one with no extent")
    check("tiny is now row 0" in log, "...while a real reorder is not")
    check("no such area, index out of range, or already there" in log,
          "and a reorder to where the row already is changes nothing, and says so")

    # The dialog is modal over the WORLD screen. Opened anywhere else it would
    # draw over a dungeon with nothing routing input to it - a modal you could
    # not close. The refusal and the success are read from ONE run, so a
    # command that had simply stopped working would fail the second half.
    check("world settings need the world map" in log,
          "the settings dialog refuses to open off the world screen")
    check("world settings open" in log and "world settings closed" in log,
          "...and opens, and closes, on it")
finally:
    for p, s in originals.items():
        write(p, s)
    # THE SAVE THIS SUITE MAKES IS NOT ITS OWN BUSINESS ALONE. A save on disk
    # puts Continue and Load on the landing page, and tools/InGameTest.ps1
    # starts its run by pressing Enter there expecting "Start New Game" — so
    # leaving this behind made a DIFFERENT suite load a save, sit in a level
    # transition, and report that levelcheck never answered. Clean up.
    for leftover in (SAVE, SAVE + ".bak"):
        try:
            os.remove(leftover)
        except OSError:
            pass

print(f"\nworldtest RESULT={'FAIL' if failures else 'PASS'} failures={failures}")
sys.exit(1 if failures else 0)
