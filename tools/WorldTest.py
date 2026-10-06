# tools/WorldTest.py — the world tier's checks, checked (docs/world-map.md).
#
# Run:  python tools\WorldTest.py      (needs a debug build)
#
# Twenty-three phases, all built on one principle: a check that never fires
# reports "clean" just as loudly as one that works, so every expectation here is
# paired with something that makes it fail.
#
#   1. VALIDATION - break the world data eleven ways, one at a time, and demand
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
#      the editor's single undo history; and the player's map page in a
#      dungeon, which stays play in Editor mode (clicked, with the travel
#      screen as the control).
#  13. PROPERTIES, AREAS AND DOORWAYS — the world's own settings, the area
#      ordering rule made visible, locations validated as the loader would,
#      and each settings tab's status row its own.
#   4. TRAVEL — a journey costs the time its terrain says and the supplies that
#      span buys, refuses an impassable square instead of clamping, and reveals
#      what walking past a place should reveal. The times are read off
#      terrain.cat, never off the run.
#  14. A WORLD OF ITS OWN — made, opened by name with -project, and clean.
#  15. THE WORLDS DIALOG — lists, creates, refuses each mistake in its own
#      words, and ARMS an Open rather than relaunching on one click.
#  16. DELETING A WORLD — only by typing its name exactly; never the running
#      world or the fallback; and a harness run ignores the developer's world.
#  17. DELETING A DUNGEON — its levels go with it, but only once every rule is
#      met: each obstacle is put in place and must be named, then lifted and
#      the delete must be allowed again. Then the typed confirmation, and the
#      one failure no message shows: savemap writing a deleted level back.
#  18. RENAMING — a dungeon and a level renamed reach EVERYTHING that names
#      them (doorways, the opening, the harness level, the dungeon's list,
#      other levels' stairs), on disk at once; and an exit stair's LOCATION
#      is not mistaken for the level it happens to be spelled like.
#  19. A WORLD WHEN A GAME STARTS — none on the title screen; switched in the
#      process, A -> B -> A, and the GPU's descriptor count comes back to
#      where the first visit left it (docs/world-on-demand.md).
#  20. THE DEAD STAY DEAD — a monster killed in a dungeon is still dead after
#      leaving and coming back: straight in, via an ambush on the road, and via
#      a save made on the world map (which must also load ONTO the world map).
#  21. THE CHARACTER SHEET - not a pause, and paging keeps its close box.
#  22. SAVES AND A NEW GAME (code-review batch 52) - after a world switch the
#      lists show the world in hand's saves and the editor's sweep finds them
#      (C207); a hand-edited save naming member -1 or pack -1 is refused and
#      the game goes on (C349); a save held open is not deleted, and says so
#      with its error code (C367); and Start New Game from crypt2 opens with
#      the same lines as on the level in hand (C364).
#  23. TERRAIN KINDS AND LANDING CELLS (code-review batch 89) - two "+ New"
#      terrains get glyphs of their own, a glyph the world could not be read
#      with is refused WITHOUT being written, a delete of a painted kind is
#      refused, a changed glyph reaches world.map at once and survives an undo,
#      a glyph change or a delete writes world.map although no square in
#      MEMORY is the kind (the file is what the next launch reads), a rename
#      reaches the world's copy of the kinds, half a landing cell or a
#      negative one is refused from the console and the dialog (C345, C343) -
#      and then a SECOND launch opens what the first wrote, which is where
#      every one of these used to abort.
#
# NOTHING HERE TOUCHES THE REAL WORLD (code-review C431). Every phase runs in
# wt_demo, a scratch copy of dungeon-demo made at the start and deleted at the
# end; the scripts that name the world they are in are given the scratch name.
# The one exception is phase 16's control, which must open dungeon-demo itself
# and is read-only there. The saves are THIS worktree's (harness_game.save_name)
# in the folder everyone shares, and only those are deleted. The run ends by
# checking the real worlds and the library are byte for byte as it found them
# (before it cleared up after a killed run) and that git status names none of
# its scratch worlds. A failure that stops the run early is still a FAIL (exit
# 1), never the "nothing ran" of exit 2 (tools/harness_game.py).
import io
import os
import re
import shutil
import stat
import sys

import harness_game

# The checkout this script lives in (tools\..), NOT a fixed path: a hardcoded
# worktree meant a run from any other checkout drove THAT tree's exe and
# mutated THAT tree's project files, underneath whoever was working there.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REAL_PROJ = os.path.join(ROOT, r"assets\projects\dungeon-demo")
SCRATCH = "wt_demo"
PROJ = os.path.join(harness_game.projects_dir(ROOT), SCRATCH)
WORLD = os.path.join(PROJ, r"world\world.map")
DUNGEONS = os.path.join(PROJ, r"catalog\dungeons.cat")
TERRAIN = os.path.join(PROJ, r"catalog\terrain.cat")
MANIFEST = os.path.join(PROJ, "project.ini")
EXE = os.path.join(ROOT, r"build\debug\bin\Dungeon.exe")
LOG = os.path.join(ROOT, r"build\debug\bin\dungeon.log")
SCRIPTS = os.path.join(ROOT, r"tools\EvalScripts")
COPIES = os.path.join(ROOT, r"build\harness-scripts\worldtest")
# Phase 22's second world, named for THIS worktree like its saves: a -project
# run lists the saves that name the world in hand, and the folder is shared, so
# under a fixed name a second run's save there (another worktree's at the same
# moment, or a killed run's) was counted in this one's "1 save(s)".
SAVES_WORLD = "wt_saves_" + harness_game.worktree_tag(ROOT)
# Every world a phase makes, and the scratch one: cleared at the start (what a
# killed run left) and at the end.
WORLDS = (SCRATCH, "wt_scratch", "wt_dlg", "wt_del", "wt_del2", "wt_dng", "wt_ren", "wt_swap",
          SAVES_WORLD)

# Never a stale exe, and never beside this worktree's own game, which shares
# the log every phase reads (tools/harness_game.py).
harness_game.refuse_if_stale(EXE)
harness_game.refuse_if_running(EXE)

# The scripts' save slots, renamed to this worktree's (worldtrip -> worldtrip_
# <worktree>_<hash>): the saves folder is shared with every other session and
# with Michael's own play, and this suite downgrades one and deletes them all.
SAVES = {n: harness_game.save_name(ROOT, n)
         for n in ("worldtrip", "encountertrip", "questtrip", "worldpersist",
                   "worldsaves_a", "worldsaves_b", "worldsaves_bad")}
SAVE = harness_game.save_path(SAVES["worldtrip"])

failures = 0


def read(p):
    return io.open(p, encoding="utf-8", newline="").read()


def write(p, s):
    io.open(p, "w", encoding="utf-8", newline="").write(s)


def run(script, project=SCRATCH, words=None, headless=True):
    # `project` is the -project flag: which WORLD to open, for one run, leaving
    # settings.ini alone. It is how a test scenario gets a world of its own, and
    # by default it is the scratch copy; None opens what a bare harness run
    # opens (dungeon-demo), which only phase 16's control wants. `words` renames
    # whole words in the script's commands (a world it names by id).
    # headless=False draws (a window shows for the run): only for a check that
    # reads what was DRAWN, which a headless run never does (phase 12's fog).
    # A run that died before its verdict counts as a failure on its own, not
    # as a log to be read as if it were whole.
    global failures
    extra = ["-project", project] if project else []
    path = harness_game.eval_script(os.path.join(SCRIPTS, script), COPIES, SAVES, words)
    code, log = harness_game.run_eval(EXE, ROOT, LOG, [path], extra, headless=headless)
    if harness_game.report_unfinished(code, log, script):
        failures += 1
    # And a run that finished FAIL counts too: the checks below read only what
    # they look for, so a declined setup line (a refused move, a missing id)
    # used to pass straight through them (code-review C442).
    elif harness_game.report_failed_script(log, script):
        failures += 1
    return log


def check(ok, label, detail=""):
    global failures
    print(f"  {'[ok  ]' if ok else '[FAIL]'} {label}")
    if not ok:
        failures += 1
        if detail:
            print(f"         {detail}")


class Stop(Exception):
    """A failure (already counted by check()) that leaves the later phases
    nothing to stand on. The run still clears up, checks the real tree and
    prints its FAIL verdict."""


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
     "level=crypt1 entryx=7 entryz=8", "",
     "map.check.locationnolevel"),
    ("a doorway naming a level of another dungeon", WORLD,
     "level=crypt1 entryx=7 entryz=8", "level=nowhere entryx=7 entryz=8",
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
    # A glyph that is not one character loads as '?' (GlyphOf) and Load lets it
    # through while nothing is painted with it - the one terrain fault that can
    # reach the checker from a file (code-review C345). Painted, or a second
    # one, and the next launch aborts.
    ("a terrain glyph the world cannot be read with", TERRAIN,
     "[road]", "[bog]\ndisplay = Bog\nglyph = MM\n\n[road]",
     "map.check.terrainglyph"),
]

# The guard is taken BEFORE clearing up what a killed run left (its worlds and
# its saves, all this worktree's own), so the clean-up is judged too: nothing
# of this judge's worlds may be left in git status, a killed run's included.
real = harness_game.RealTree(ROOT, own=WORLDS)
for w in WORLDS:
    harness_game.remove_world(ROOT, w)
harness_game.remove_saves(SAVES.values())
harness_game.scratch_world(ROOT, SCRATCH)
# The scratch world's own originals: phases 1, 10, 11 and 23 change these files
# and put them back, because later phases run in the same scratch world.
originals = {p: read(p) for p in (WORLD, DUNGEONS, TERRAIN, MANIFEST)}
# settings.ini is the DEVELOPER'S, not the project's: phase 16 points it at a
# scratch world on purpose, and it must come back exactly as it was - or go
# again, if the run found none.
SETTINGS = os.path.join(ROOT, r"build\debug\bin\settings.ini")
settings_before = read(SETTINGS) if os.path.isfile(SETTINGS) else None
# Muted for the whole run (tools/harness_audio.py) - AFTER the snapshot above,
# so the restore of settings_before below carries the real volume back too.
import harness_audio
audio = harness_audio.mute(os.path.dirname(SETTINGS))
try:
    print("1 - the world checks fire when the world is broken")
    log = run("worldcheck.eval")
    baseline = log[log.rfind("> validate"):]
    check("clean - no faults found" in baseline,
          "baseline is clean, so a finding below is the mutation's", baseline[:600])
    if "clean - no faults found" not in baseline:
        raise Stop("the baseline is not clean - every mutation's finding would be ambiguous")

    # The new-game world state, kept as phase 2's control.
    newgame = party_lines(log)
    check(bool(newgame), "the `world` command printed the new game's party line")
    if not newgame:
        raise Stop("no new-game party line - phase 2 has no control")
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
            # WHATEVER VERSION THE FILE CARRIES goes one below the floor. This
            # used to replace the literal "version=1", which silently did
            # nothing the day the format became v2 - and the check below it
            # still passed, because OTHER old saves in the folder logged the
            # same refusal. So the version is read off the file, the result is
            # asserted to have changed, and the refusal must name THIS save.
            text = read(SAVE)
            m = re.search(r"save version=(\d+)", text)
            floor_minus_one = (int(m.group(1)) - 1) if m else 0
            downgraded = re.sub(r"save version=\d+",
                                f"save version={floor_minus_one}", text, count=1)
            check(m is not None and downgraded != text,
                  f"the save was downgraded (version {m.group(1) if m else '?'} "
                  f"-> {floor_minus_one})")
            write(SAVE, downgraded)
            log = run("worldload.eval")
            # The refusal line alone cannot say the LOAD read this file: the boot's
            # save listing reads every header and logs it too. The load's own
            # failure names its path, so a probe that loaded some other slot (a
            # missing one, as it did until batch 52 renamed the probe's) fails.
            check(f"{os.path.basename(SAVE)} is version {floor_minus_one}, older than the "
                  f"minimum" in log,
                  "the log says THIS save was refused, and what the floor is")
            check(f"LoadGame: could not read {SAVE}" in log,
                  "and the load of THIS save did not happen")
            check("eval RESULT=PASS" in log, "while the game itself kept running")
        finally:
            shutil.move(SAVE + ".bak", SAVE)

        # A SAVE KNOWS ITS WORLD (docs/world-on-demand.md): Continue and Load
        # choose the world from the file before reading anything else in it,
        # so one that names none is refused - the control being the untouched
        # save's own line, read first.
        text = read(SAVE)
        check(f"save world={SCRATCH}" in text,
              "a save names the world it belongs to")
        shutil.copy(SAVE, SAVE + ".bak")
        try:
            write(SAVE, re.sub(r"save world=[^\r\n]*\r?\n", "", text, count=1))
            log = run("worldload.eval")
            check(f"{os.path.basename(SAVE)} names no world - refusing it" in log and
                  f"LoadGame: could not read {SAVE}" in log,
                  "one that names no world is refused, not loaded into this one")
        finally:
            shutil.move(SAVE + ".bak", SAVE)
    # Each save goes as soon as its phase is done with it: while it is on disk
    # it is in everyone's Load list (the folder is shared).
    harness_game.remove_saves([SAVES["worldtrip"]])

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
    # THE CONTROL NEEDS A START ELSEWHERE. crypt2's start ('P') stands on 10,6,
    # the very square the back way lands on, so "landed on the entry, not the
    # start" could not be told apart - the old check ended in `or True`
    # (code-review C432). For this run the scratch crypt2 starts on the first
    # floor square that is not 10,6 and holds no stair; a back way that ignored
    # its entry would land THERE, and mapinfo must report it as the start.
    crypt2 = os.path.join(PROJ, r"levels\crypt2.map")
    crypt2_before = read(crypt2)
    eol = "\r\n" if "\r\n" in crypt2_before else "\n"
    lines = crypt2_before.split(eol)
    rows = [i for i, l in enumerate(lines) if l[:1] in ("#", ".", "P")]
    stairs = {(int(m.group(1)), int(m.group(2))) for m in
              re.finditer(r"^stairs \S+ (\d+) (\d+)", crypt2_before, re.M)}
    elsewhere = next(((x, z) for z, i in enumerate(rows) for x, ch in enumerate(lines[i])
                      if ch == "." and (x, z) != (10, 6) and (x, z) not in stairs), None)
    for i in rows:
        lines[i] = lines[i].replace("P", ".")
    if elsewhere:
        r = rows[elsewhere[1]]
        lines[r] = lines[r][:elsewhere[0]] + "P" + lines[r][elsewhere[0] + 1:]
    write(crypt2, eol.join(lines))
    try:
        log = run("worldback.eval")
    finally:
        write(crypt2, crypt2_before)

    # The back way opens the SAME dungeon on a DIFFERENT level: crypt1 is
    # 14x10 and crypt2 12x8, so the map line alone says which one opened.
    check("12x8 map" in log,
          "the back way opens a different floor of the same dungeon")
    # ...and at ITS cell, not the level's start (moved off it above), which
    # is the control: landing there would mean the location's entry was
    # ignored and the front door's rule applied.
    # FACING NORTH: 10,6 holds crypt2's exit stair, and arriving on a stair
    # faces the way it does (the way you step off it, 2026-09-25).
    check("10,6 facing north" in log,
          "landing on the location's own cell, not the level's start (10,6 vs P), "
          "facing off the exit stair there")
    started = re.search(r"12x8 map, start (\d+),(\d+)", log)
    check(elsewhere is not None and started is not None and
          (int(started.group(1)), int(started.group(2))) == elsewhere,
          "and the level really does start elsewhere (the control): mapinfo names "
          f"the moved start {elsewhere}",
          started.group(0) if started else "(no 12x8 mapinfo line)")

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
    # This worktree's slot, cleared at the start: so a file here is this run's.
    trip = harness_game.save_path(SAVES["encountertrip"])
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
    harness_game.remove_saves([SAVES["questtrip"]])
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
                       "entryx=7 entryz=8"):
            check(record in after, f"kept: {record}")
    finally:
        write(WORLD, world_before)
    # --- phase 11: the world tier is editable content -----------------------
    print("\n11 - dungeons, terrain and quests are types you can author")
    try:
        log = run("worldtypes.eval")
    finally:
        # A "+ New" terrain is SAVED at once now (code-review C345: the world
        # can come to name its glyph), and the save writes the dungeon made
        # just before it too - later phases expect the scratch world's own.
        for p in (DUNGEONS, TERRAIN):
            write(p, originals[p])

    # THE SWEEP SEES THE WORLD. The crypt is referenced by its two doorways
    # and by NOTHING in any level - so the "0 level record(s)" half is the
    # control: a sweep that only walked levels would have called it safe to
    # delete, which is the exact hole W2 had to close. THREE since W11: the
    # game's opening (start_dungeon = crypt) is a reference too, and the sweep
    # that renames the doorways renames it.
    check("crypt': 0 level record(s), 3 other reference(s)" in log,
          "a dungeon's two doorways and the opening are found, and none is in "
          "any level")
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
    # WINDOWED: `worldview`'s fog is what WorldMapView::Render DREW on the last
    # frame (code-review C79), and a headless run draws nothing - it would read
    # "fog not drawn" and prove nothing about the page.
    log = run("worldedit.eval", headless=False)
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

    # THE PLAYER'S MAP IS NEVER THE EDITOR (code-review C77, C79). With Editor
    # mode on and a terrain armed, the M map's world page in a dungeon used to
    # show the toolbar, lift the fog, paint a stroke nothing closed and open
    # world dialogs nothing routed. The lines are read in order: the map page's
    # first, then the travel screen's - the CONTROL, the same mode and the same
    # clicks doing all four, so the page's "nothing" is the page's doing and
    # not a click that never landed. `worldview` READS the view's overlay flag
    # as the frame derived it (it refuses, and prints no view line, when the
    # flag disagrees with the screen that is up), and its fog is Render's.
    views = [l.split("console: ", 1)[1].strip() for l in log.splitlines()
             if "console: world view:" in l or "console: worldview click" in l]
    check(len(views) == 6, f"the script reported the view six times (got {len(views)})",
          " | ".join(views))
    if len(views) == 6:
        check(views[0] == "world view: map page, mode editor, editing no, toolbar 0, fog on",
              "in a dungeon the map's world page stays play in Editor mode: no toolbar, "
              "the fog on", views[0])
        check(views[1] == "worldview click 7,6 left: road -> road, stroke none, dialogs none",
              "and a left click there paints nothing and opens no stroke", views[1])
        check(views[2].startswith("worldview click 10,10 right:") and
              views[2].endswith("dialogs none"),
              "and a right-click on a doorway there opens no dialog", views[2])
        check(views[3] == "world view: travel screen, mode editor, editing yes, toolbar 6, "
              "fog off",
              "the control: the travel screen in the same mode is the editor", views[3])
        check(views[4] == "worldview click 7,6 left: road -> moor, stroke open, dialogs none",
              "...where the same left click paints, its stroke left for the state to close",
              views[4])
        check(views[5].startswith("worldview click 10,10 right:") and
              "stroke none" in views[5] and views[5].endswith("dialogs settings"),
              "...the state closed that stroke, and the same right-click opens the doorway's "
              "settings", views[5])
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

    # EACH TAB'S STATUS ROW IS ITS OWN (code-review C102). `worldsettings
    # status` reads each row's LABEL, so a note written into the other tab's
    # row shows here. Opened on a doorway: its row is empty, not the World
    # tab's start caption. A refused start: the World row says why, the
    # Doorways row stays empty. A refused move: the Doorways row says why, and
    # the World row keeps its own. Then a doorway added, refused a move and
    # DELETED: with nothing selected the Doorways row stands under "+ Add",
    # and it must not keep the refusal about the doorway that is gone. Read by
    # structure (which row changed), not by the English sentences.
    notes = [re.search(r"tab (\S+) selected '([^']*)' - world note '(.*)' - "
                       r"doorways note '(.*)'$", l)
             for l in log.splitlines() if "console: world settings open: tab" in l]
    notes = [m.groups() for m in notes if m]
    check(len(notes) == 5, f"the dialog's status was read five times (got {len(notes)})")
    if len(notes) == 5:
        (tab0, sel0, w0, d0), (_, _, w1, d1), (_, _, w2, d2), \
            (_, sel3, w3, d3), (_, sel4, w4, d4) = notes
        check(tab0 == "doorways" and sel0 == "crypt_gate" and w0 not in ("", "-")
              and d0 == "",
              "opened on a doorway: the Doorways row is empty, the World row has its "
              "start caption", f"{notes[0]}")
        check("water" in w1 and w1 != w0 and d1 == "",
              "a refused start speaks in the World row, and the Doorways row stays empty",
              f"{notes[1]}")
        check(d2 not in ("", "-") and d2 != w1 and w2 == w1,
              "a refused move speaks in the Doorways row, and the World row keeps its own",
              f"{notes[2]}")
        check(re.search(r"console: world settings add: selected 'door\d+'", log) is not None
              and sel3.startswith("door") and d3 not in ("", "-") and w3 == w1,
              "an added doorway is selected, and a refused move speaks in its row",
              f"{notes[3]}")
        check("console: world settings delete: selected ''" in log and sel4 == ""
              and d4 == "" and w4 == w1,
              "deleting it takes its refusal with it: the row under \"+ Add\" is empty, "
              "and the World row keeps its own", f"{notes[4]}")

    # --- W5: a level belongs to a dungeon -----------------------------------
    # The toolbar's picker is two-tier now; this is the same grouping read
    # through the same Project helpers, which is the half a screenshot cannot
    # assert. THE ORPHAN ROW IS THE CONTROL: it reads 0 both times, so the
    # grouping accounts for every stem the manifest holds — a helper that
    # simply failed to match would pile them all up there instead.
    groups = [l.split("console:")[-1].strip()
              for l in log.splitlines() if "(no dungeon)" in l]
    check(groups[:2] == ["(no dungeon) (0)", "(no dungeon) (0)"],
          "every level belongs to a dungeon, before and after",
          " | ".join(groups[:2]))
    check("crypt      (2) crypt1 crypt2" in log and
          "crypt      (3) crypt1 crypt2 crypt3" in log,
          "and a new level lands INSIDE the dungeon, named after it")
    check("created crypt3 in crypt" in log, "which is what creating one says")

    # THE WRITERS' FIDELITY. `levels new` saves the whole project, and that
    # used to rewrite four files and delete every comment in project.ini.
    # Every file the game checked must round-trip with none absent, and it
    # must check AT LEAST the 29 it did when this was written (project.ini +
    # 28 catalogs to shapes.cat) - a floor, not an exact count: the exact 29
    # failed this check the day lights.cat and trails.cat joined the project,
    # though nothing had gone wrong. A drop below it is a writer gone missing.
    m = re.search(r"catround (\d+) of (\d+) file\(s\) round-trip, (\d+) absent", log)
    ok_files, seen_files, absent = (int(g) for g in m.groups()) if m else (0, 0, -1)
    check(m is not None and ok_files == seen_files and absent == 0 and seen_files >= 29,
          "and saving the project leaves every file it did not change alone",
          m.group(0) if m else "no catround line")

    # --- W6: the player's map has two pages ---------------------------------
    # The drawing is not checkable from here; the STATE MACHINE is, and it is
    # the half that breaks silently. The whole sequence is read in order, so a
    # command that had stopped doing anything would show up as a run of
    # identical lines rather than as a pass.
    pages = [l.split("console:")[-1].strip()
             for l in log.splitlines() if "map page:" in l]
    check(pages == ["map page: dungeon (toggle offered, map closed)",
                    "map page: dungeon (toggle offered, map closed)",
                    "map page: dungeon (toggle offered, map open)",
                    "map page: world (toggle offered, map open)",
                    "map page: dungeon (toggle offered, map open)",
                    "map page: dungeon (toggle offered, map closed)",
                    "map page: dungeon (toggle offered, map closed)"],
          "the map's page follows the overlay and cannot outlive it",
          " | ".join(pages))
    # --- phase 14: a world of its own ---------------------------------------
    print("\n14 - a world can be made, opened by name, and checked")
    log = run("worldnew.eval")
    check("created world 'wt_scratch'" in log, "a new world is created")
    # THE CONTROL: it is listed beside the one that made it, and the one that
    # made it is still the one that is OPEN. Creating a world must not move you
    # into it — that is a separate, deliberate switch (`worlds load`).
    check(f"{SCRATCH}  (open)" in log and "wt_scratch" in log,
          "...listed beside the world that made it, which is still the open one")

    # Opened BY NAME on the command line, which is the scenario interface.
    log = run("worldscratch.eval", project="wt_scratch")
    check("wt_scratch  (open)" in log,
          "-project opens it, without touching settings.ini")
    check("keep       (1) room1" in log,
          "and it has its starter room, in its starter dungeon")
    # CONTENT CAME ACROSS AND PLACES DID NOT, and this is the check that says
    # the difference was made cleanly: the copied items carry quest and reveals
    # hooks naming a quest and a location the new world never had, and the very
    # first run of this reported three errors for exactly that.
    check("validate: clean" in log,
          "a world made this way passes the checker as it stands")

    # --- phase 15: the worlds dialog ----------------------------------------
    print("\n15 - the worlds dialog lists, creates and arms")
    # The script clicks the RUNNING world's row by name: the scratch world's.
    log = run("worldsdialog.eval", words={"dungeon-demo": SCRATCH})
    dlg = [l.split("console: ", 1)[1] for l in log.splitlines()
           if "console: worlds dialog" in l or "console: the worlds dialog" in l]
    check(any("needs the world map" in l for l in dlg),
          "it refuses to open inside a level, where nothing would route input to it")
    # "worlds dialog open: [a b c] armed 'x' - note" -> (state, {worlds}, x, note).
    # The LIST is parsed rather than matched whole: phase 14's scratch world is
    # still on disk here (cleanup runs at the end), and a check that spelled
    # the list out would be testing the order phases run in.
    def parse(l):
        if not l.startswith("worlds dialog "):
            return None
        state = l.split(":", 1)[0].split()[-1]
        worlds = set(l.split("[", 1)[1].split("]", 1)[0].split())
        armed = l.split("armed '", 1)[1].split("'", 1)[0]
        return state, worlds, armed, l.split(" - ", 1)[1]
    rows = [r for r in map(parse, dlg) if r]
    check(bool(rows) and rows[0][0] == "open" and SCRATCH in rows[0][1]
          and "dungeon-demo" in rows[0][1]
          and "wt_dlg" not in rows[0][1] and rows[0][2] == "",
          "on the world screen it opens, listing the worlds on disk, nothing armed")
    # EACH REFUSAL NAMES ITS OWN RULE (W4's lesson): a check for the duplicate
    # rule would pass with that rule deleted if both refusals said one thing.
    check(any(a == "" and n.startswith("Type a name first") for _, _, a, n in rows),
          "a name that filters to nothing is refused, and says so")
    check(any("wt_dlg" in w and a == "wt_dlg" and n.startswith("Created 'wt_dlg'")
              for _, w, a, n in rows),
          "a create lists the new world and ARMS it - one click from going there")
    check(any("already exists" in l for l in dlg),
          "a second create of that name is refused with its own sentence")
    check(os.path.isfile(os.path.join(ROOT, r"assets\projects\wt_dlg\project.ini")),
          "...and the world is on disk, which is what the list was re-read from")
    # THE CONTROL for the arm: reopening starts with NOTHING armed, so the
    # armed row after the next click is that click's doing, not a leftover.
    reopen = [i for i, (s, w, a, _) in enumerate(rows)
              if s == "open" and "wt_dlg" in w and a == ""]
    armed = [i for i, (_, _, a, n) in enumerate(rows)
             if a == "wt_dlg" and "Click Switch to start a new game in 'wt_dlg'" in n]
    check(bool(reopen) and bool(armed) and armed[0] > reopen[-1],
          "reopened it arms nothing; one click on Open arms that row and says "
          "what the second will do", " | ".join(dlg))

    # --- phase 16: deleting a world ----------------------------------------
    print("\n16 - a world is deleted only by typing its name")
    # THE CONTROL FIRST, for the harness rule this phase needed: point the
    # developer's settings at a scratch world, and a harness run must STILL
    # open dungeon-demo. Before the rule, a world Michael switched into became
    # every suite's ground.
    # From the file as it is NOW, not settings_before, or this unmutes the run.
    # ALWAYS set up: with no settings.ini before the run this used to skip the
    # control and still print its check (code-review C432); the finally block
    # removes a file the run made.
    now = read(SETTINGS) if os.path.isfile(SETTINGS) else ""
    lines = [l for l in now.splitlines() if not l.startswith("project=")]
    write(SETTINGS, "\n".join(lines + ["project=wt_scratch"]) + "\n")
    check("project=wt_scratch" in read(SETTINGS),
          "the control: settings.ini names another world (wt_scratch)")
    # NO -project: this run must land on the real dungeon-demo by the harness
    # rule alone. It only reads it (RealTree checks that at the end).
    log = run("worlddelete.eval", project=None)
    con = [l.split("console: ", 1)[1] for l in log.splitlines() if "console: " in l]
    check("  dungeon-demo  (open)" in con,
          "a harness run opens dungeon-demo even when settings.ini names another world")
    check(any("deleting ''" in l and "open another world" in l for l in con),
          "the RUNNING world is refused before any confirmation opens")
    check(any("deleting 'wt_del'" in l and "case-sensitive" in l for l in con),
          "a row's Delete opens the confirmation, saying the name is case-sensitive")
    # The listings are the evidence: each wrong name is followed by a `worlds`
    # listing that must still contain the world.
    listings, cur = [], None
    for l in con:
        if l == "> worlds":
            cur = []
            listings.append(cur)
        elif cur is not None and l.startswith("  "):
            cur.append(l.split()[0])
        elif l.startswith(">"):
            cur = None
    mism = [l for l in con if "not the name" in l]
    check(len(mism) == 2 and len(listings) >= 2 and "wt_del" in listings[0]
          and "wt_del" in listings[1],
          "wrong case and a prefix are both refused - the world is still on disk",
          f"refusals {len(mism)}, listings {listings}")
    check(any("Deleted 'wt_del'" in l for l in con) and len(listings) >= 3
          and "wt_del" not in listings[2]
          and not os.path.isdir(os.path.join(ROOT, "assets", "projects", "wt_del")),
          "the exact name deletes it: gone from the list AND from disk")
    check("to delete, type the name twice: worlds delete wt_del2 wt_del2 "
          "(case-sensitive)" in con and "deleted world 'wt_del2'" in con,
          "the console's form wants the name twice, exactly")
    check("'wt_del2' is not there any more." in con,
          "and deleting it again says it is gone, rather than succeeding")

    # Inside another world, the two refusals about WHICH world separate:
    # dungeon-demo is not running there, so refusing it is the fallback rule.
    log = run("worldfallback.eval", project="wt_dlg")
    con = [l.split("console: ", 1)[1] for l in log.splitlines() if "console: " in l]
    check(any("'dungeon-demo' is where a launch lands" in l for l in con),
          "dungeon-demo is refused as the fallback, even when it is not running")
    check(any("You are in 'wt_dlg'" in l for l in con),
          "and the world you are in is refused as the running one")
    check(os.path.isfile(os.path.join(REAL_PROJ, "project.ini")),
          "...and dungeon-demo is, of course, still there")

    # --- phase 17: deleting a dungeon ----------------------------------------
    print("\n17 - a dungeon is deleted with its levels, and only when nothing leads in")
    DNG = os.path.join(ROOT, r"assets\projects\wt_dng")
    shutil.rmtree(DNG, ignore_errors=True)  # a previous run's, if it died
    log = run("dungeonrefuse.eval")
    con = [l.split("console: ", 1)[1] for l in log.splitlines() if "console: " in l]
    # THE REAL CONTENT, asked and never touched: each demo dungeon meets a
    # different rule first, and the crypt's doorways only once the opening that
    # stood in front of them has moved.
    check("delete 'crypt': refused - The game begins in crypt1 - change the "
          "opening in World settings first." in con,
          "the crypt is refused as the game's opening")
    check("delete 'eval': refused - The party is in eval_arena - go to another "
          "dungeon first." in con,
          "the proving ground is refused while the party stands in it")
    check(any(l.startswith("delete 'crypt': refused - 2 doorway(s)") and
              "crypt_gate" in l and "crypt_back" in l for l in con),
          "with the opening moved, the crypt's two doorways are named")
    # BY NAME AND BY COUNT, and the two must agree — read as a block, since an
    # earlier phase leaves the crypt a third level until the cleanup.
    head = next((i for i, l in enumerate(con) if "(crypt) goes, and its" in l), -1)
    named = []
    for l in con[head + 1:] if head >= 0 else []:
        if not l.startswith("  - "):
            break
        named.append(l[4:])
    stated = con[head].split("its ", 1)[1].split(" ")[0] if head >= 0 else ""
    check(head >= 0 and stated == str(len(named)) and
          {"crypt1", "crypt2"} <= set(named),
          "the account names the levels by name AND by count, and they agree",
          f"stated {stated}, named {named}")
    check("delete 'nosuch': refused - There is no dungeon named 'nosuch'." in con,
          "a dungeon that does not exist says so")
    check(os.path.isfile(os.path.join(PROJ, r"levels\crypt1.map")),
          "...and asking deleted nothing")

    log = run("dungeondelete.eval", project="wt_dng")
    con = [l.split("console: ", 1)[1] for l in log.splitlines() if "console: " in l]
    # Every `dungeons what dungeon1` answer, in order: allowed, then each
    # obstacle and its control. A rule reported out of place would shift this
    # sequence, so it is compared whole.
    whats = [l.split(" - ", 1)[1].split(" ")[0] if "refused" in l else "allowed"
             for l in con if l.startswith("delete 'dungeon1':")]
    want = ["allowed", "A", "allowed", "1", "allowed", "dungeon11", "allowed",
            "The", "allowed"]
    check(whats == want,
          "each rule refuses with its own sentence and lifts cleanly: "
          "stair, doorway, harness level, opening", f"{whats}")
    check(any("A stair in room1 at 7,7 leads into dungeon11" in l for l in con),
          "the stair refusal names the stair's level, cell and destination")
    check(any("refused - The party is in room1" in l for l in con),
          "the starter dungeon is refused while the party stands in it")
    mism = [l for l in con if "confirming - That is not the name" in l]
    listings = [i for i, l in enumerate(con) if l == "> dungeons"]
    def listed_after(i):
        out = []
        for l in con[i + 1:]:
            if l.startswith(">"):
                break
            if l.startswith("  "):
                out.append(l.split()[0])
        return out
    after = [listed_after(i) for i in listings]
    check(len(mism) == 2 and len(after) >= 5 and "dungeon1" in after[2] and
          "dungeon1" in after[3],
          "wrong case and a prefix are refused - the dungeon is still there",
          f"refusals {len(mism)}, listings {after}")
    check("dungeons dialog: closed '' - " in con and "dungeon1" not in after[4],
          "the exact id deletes it and closes the dialog")
    saved = [l for l in con if l.startswith("saved levels:")]
    # THE ONE FAILURE NO MESSAGE SHOWS. dungeon11 was pulled into a stash on
    # purpose before it was deleted; a stash left behind is written straight
    # back by savemap. Mutation-tested: without the stash erase this reads
    # "room1, dungeon11".
    check(saved == ["saved levels: room1"],
          "savemap after the delete does not write the deleted level back",
          f"{saved}")
    check("usage: dungeons delete <id> <id again> (exact, case-sensitive)" in con
          and "deleted 'dungeon1'" in con,
          "the console's form wants the id twice, exactly")
    levels_dir = os.listdir(os.path.join(DNG, "levels"))
    dcat = read(os.path.join(DNG, r"catalog\dungeons.cat"))
    manifest = read(os.path.join(DNG, "project.ini"))
    check(sorted(levels_dir) == ["room1.ent", "room1.map"] and
          "[dungeon1]" not in dcat and "dungeon11" not in manifest,
          "on disk: the files, the catalog entry and the manifest entry are all gone",
          f"{levels_dir}")

    # THE RULE NO COMMAND CAN SET UP: a level two dungeons claim. Written by
    # hand — the checker calls it an error, which is why nothing authors it.
    for ext in (".map", ".ent"):
        shutil.copyfile(os.path.join(DNG, "levels", "room1" + ext),
                        os.path.join(DNG, "levels", "room2" + ext))
    write(os.path.join(DNG, r"catalog\dungeons.cat"),
          dcat + "\n[twin_a]\nlevels = room2\n\n[twin_b]\nlevels = room2\n")
    write(os.path.join(DNG, "project.ini"),
          manifest.replace("levels = room1", "levels = room1 room2"))
    log = run("dungeonshared.eval", project="wt_dng")
    con = [l.split("console: ", 1)[1] for l in log.splitlines() if "console: " in l]
    check("delete 'twin_a': refused - room2 also belongs to dungeon 'twin_b' - "
          "deleting would take it from that one too." in con,
          "a level another dungeon also claims is refused, naming the other",
          " | ".join(l for l in con if "twin" in l))

    # --- phase 18: renaming a dungeon and a level ------------------------------
    print("\n18 - a rename reaches everything that names the dungeon or the level")
    REN = os.path.join(ROOT, r"assets\projects\wt_ren")
    shutil.rmtree(REN, ignore_errors=True)
    run("renameworld.eval")
    log = run("renamesetup.eval", project="wt_ren")
    con = [l.split("console: ", 1)[1] for l in log.splitlines() if "console: " in l]
    check("saved levels: room1, keep1" in con,
          "the scenario is built: two levels, a stair between them, three doorways",
          " | ".join(con[-4:]))
    # AN EXIT NO COMMAND CAN AUTHOR, written by hand: on keep1, leading out to
    # the world LOCATION named room1 — spelled exactly like the level about to
    # be renamed, which is the whole point of it.
    k1 = os.path.join(REN, r"levels\keep1.map")
    write(k1, read(k1) + "stairs stairs_exit 9 9 south dest=room1 destx=0 destz=0\r\n")

    log = run("rename.eval", project="wt_ren")
    con = [l.split("console: ", 1)[1] for l in log.splitlines() if "console: " in l]
    def block(cmd, n):
        """The lines the n-th `cmd` printed."""
        idx = [i for i, l in enumerate(con) if l == "> " + cmd]
        if len(idx) <= n:
            return []
        out = []
        for l in con[idx[n] + 1:]:
            if l.startswith(">"):
                break
            out.append(l.strip())
        return out
    clean = [l for l in con if l == "validate: clean - no faults found"]
    check(len(clean) == 3,
          "the checker is clean before, after the dungeon rename, and after the "
          "level rename", " | ".join(l for l in con if l.startswith("validate")))
    locs = [l.split() for l in block("worldloc", 0)]
    check(len(locs) == 3 and all(l[4] == "castle" for l in locs),
          "the dungeon rename reaches EVERY doorway - including the one named "
          "the short way, with no dungeon= of its own", f"{locs}")
    check("opening: castle / room1 at 8,8" in block("worldprops", 0),
          "...and the game's opening")
    check("castle 'The Keep': hall keep1" in block("dungeons", 1),
          "the level rename keeps the level in its dungeon, in its place")
    locs = {l.split()[1]: l.split()[4:6] for l in block("worldloc", 1)}
    check(locs.get("keep_gate") == ["castle", "hall"] and
          locs.get("keep") == ["castle", "hall"] and
          locs.get("room1") == ["castle", "keep1"],
          "doorways naming the level follow it; the one naming another level "
          "does not", f"{locs}")
    check("opening: castle / hall at 8,8" in block("worldprops", 1) and
          "harness level: hall" in block("worldprops", 1),
          "the opening and the harness level follow it")
    refusals = [l.split(" - ", 1)[1] if " - " in l else l
                for l in con if l.startswith("rename level")]
    check(len(refusals) == 3 and "not a level name" in refusals[0] and
          "There is no level named 'nosuch'" in refusals[1] and
          "already exists" in refusals[2],
          "each refusal says its own rule: not a stem, no such level, taken",
          f"{refusals}")
    # ON DISK, with no savemap in the script: a rename moves files at once, so
    # whatever points at them has to be written at once too.
    dcat = read(os.path.join(REN, r"catalog\dungeons.cat"))
    man = read(os.path.join(REN, "project.ini"))
    world = read(os.path.join(REN, r"world\world.map"))
    k1 = read(k1)
    check("[castle]" in dcat and "[keep]" not in dcat and
          "levels = hall keep1" in dcat,
          "on disk: the dungeon's entry renamed, its level list following")
    check("start_dungeon = castle" in man and "start_level = hall" in man and
          "eval_level = hall" in man,
          "on disk: the manifest's opening and harness level")
    check("location dungeon keep 3 3 dungeon=castle level=hall" in world,
          "on disk: the short-way doorway now names its dungeon outright")
    # Any facing: a placed stair faces its first open side (DungeonMap::
    # OpenFacing), and which side that is has nothing to do with the rename.
    check(re.search(r"stairs stairs_up 7 7 (north|east|south|west) dest=hall\b", k1),
          "on disk: another level's stair repointed WITHOUT a savemap")
    check("stairs stairs_exit 9 9 south dest=room1" in k1,
          "an exit stair's location is left alone, though it is spelled like "
          "the old level")
    check(os.path.isfile(os.path.join(REN, r"levels\hall.map")) and
          not os.path.isfile(os.path.join(REN, r"levels\room1.map")),
          "and the files themselves moved")

    # --- phase 19: a world is loaded when a game starts ----------------------
    print("\n19 - no world until a game starts, and a switch that leaks nothing")
    shutil.rmtree(os.path.join(ROOT, r"assets\projects\wt_swap"), ignore_errors=True)
    # A is the scratch world: the script's `worlds load dungeon-demo` loads it.
    log = run("worldswap.eval", words={"dungeon-demo": SCRATCH})
    status = [l.split("console: ", 1)[1] for l in log.splitlines()
              if "console: world " in l]
    def srv(line):
        return int(line.split(" srv ", 1)[1].split(" ", 1)[0]) if " srv " in line else -1
    check(len(status) == 5, f"the script reported five times (got {len(status)})",
          " | ".join(status))
    if len(status) == 5:
        check(status[0].startswith("world none  game not loaded"),
              "the title screen has NO world - the world loads when a game starts",
              status[0])
        check(status[1].startswith(f"world {SCRATCH}  game loaded") and
              status[2].startswith("world wt_swap  game loaded") and
              status[3].startswith(f"world {SCRATCH}  game loaded"),
              "A -> B -> A switches in the process, no relaunch", " | ".join(status[1:4]))
        # THE LEAK CHECK. Descriptor slots are the one GPU resource with a
        # visible gauge; a world's textures are most of them. Coming back to A
        # must land exactly where the first visit did - and B, a different
        # world, must differ, or the count is not measuring the world at all.
        check(srv(status[3]) == srv(status[1]) and srv(status[2]) != srv(status[1]),
              f"coming back to A frees what B took: srv {srv(status[1])} -> "
              f"{srv(status[2])} -> {srv(status[3])}")
        check(srv(status[0]) < srv(status[1]),
              f"and the title screen holds less than a world ({srv(status[0])})")
    check(f"already in '{SCRATCH}'" in log and len(status) == 5 and
          srv(status[4]) == srv(status[3]),
          "loading the world already open reloads nothing")
    check("World unloaded" in log and "eval RESULT=PASS script=worldswap.eval" in log,
          "and the run ended in play, having unloaded along the way")

    # --- phase 20: a dungeon keeps what happened in it -----------------------
    print("\n20 - the dead stay dead across the world map")
    log = run("worldpersist.eval")
    # Each section is the text between its echo marker and the next one, so a
    # "dead" line can only be credited to the section that printed it.
    sections = {}
    for part in log.split("console: --- persist: ")[1:]:
        name, _, body = part.partition(" ---")
        sections[name] = body
    # crypt1's one skeleton stands at 10,4 (moved there in Michael's editor
    # pass, dccbd3f); worldpersist.eval aims its blasts at the same square.
    def skeleton(name):
        m = re.search(r"skeleton @ 10,4  hp ([\d.]+)", sections.get(name, ""))
        return float(m.group(1)) if m else None
    # THE CONTROL: alive before the kill, dead after it — otherwise "dead on
    # the way back" would be satisfied by a skeleton that never lived.
    check((skeleton("before the kill") or 0) > 0 and skeleton("killed") == 0,
          "the skeleton was alive, then killed",
          f"before {skeleton('before the kill')}, after {skeleton('killed')}")
    for name, label in (("back through the gate", "straight back through the gate"),
                        ("after an encounter", "with an ambush on the road in between"),
                        ("loaded over the same level", "through a world-map save, loaded over its level"),
                        ("loaded over another level", "and loaded over a different level")):
        check(skeleton(name) == 0, f"still dead {label}", f"hp {skeleton(name)}")
    for name in ("loaded over the same level", "loaded over another level"):
        check("state worldmap" in sections.get(name, ""),
              f"a save made on the world map loads ONTO the world map ({name})")
    # THE OTHER LEVEL REALLY IS ANOTHER ONE (code-review C300). The party is in
    # crypt1 when the script resets, and `reset` used to re-read the level it
    # found - so the second load ran over crypt1 again and took the same-level
    # branch twice. The reset goes back to the harness level now; its
    # `transients` readout names where it landed.
    landed = re.search(r"transients on (\S+)", sections.get("reset onto another level", ""))
    check(landed is not None and landed.group(1) != "crypt1",
          "the reset before the second load left crypt1 for another level",
          f"reset onto {landed.group(1) if landed else None}")
    check("one-pipeline violation" not in log,
          "and no monster's health moved outside the pipeline on the way")
    check("eval RESULT=PASS script=worldpersist.eval" in log, "the script ran clean")
    harness_game.remove_saves([SAVES["worldpersist"]])

    # --- phase 21: the character sheet ----------------------------------------
    print("\n21 - the character sheet: not a pause, and paging keeps its close box")
    log = run("worldsheet.eval")
    states = re.findall(r"console: state (\S+)", log)
    hp = [float(h) for h in re.findall(r"console:\s+\[0\] \S+\s+hp ([\d.]+)/", log)]
    # NOT A PAUSE: poisoned Brand loses health over frames with his sheet open.
    # The twenty filler `state` lines ran INSIDE the sheet, which they say. The
    # size of the drop rides the headless frame time; that it moves at all is
    # the claim - a frozen world prints the same figure twice (MUTATION-checked).
    check(states[:20] == ["sheet"] * 20 and len(hp) >= 2 and hp[1] <= hp[0] - 0.2,
          "the world goes on under an open sheet: a poisoned member keeps losing "
          "health while it is up", f"hp {hp[:2]}, states {states[:20]}")
    # Paging re-entered OpenCharacterSheet, which recorded the SHEET as the state
    # to resume to, so the close box resumed into the sheet (MUTATION-checked:
    # without the guard both lines read "state sheet").
    check(states[20:22] == ["playing", "worldmap"],
          "after paging through members, closing returns to the level, and on the "
          "world map to the world map", f"states {states[20:]}")
    check("eval RESULT=PASS script=worldsheet.eval" in log, "the script ran clean")

    # --- phase 22: saves and a new game (code-review batch 52) ---------------
    print("\n22 - saves follow the world in hand; a bad one is refused; a held one says "
          "so; a new game opens alike everywhere")

    def sections_of(text, marker):
        """The text between each `echo --- <marker>: <name> ---` and the next."""
        out = {}
        for part in text.split(f"console: --- {marker}: ")[1:]:
            name, _, body = part.partition(" ---")
            out[name] = body
        return out

    # C207: the -project run switches world, saves there, and every list follows.
    save_a, save_b = SAVES["worldsaves_a"], SAVES["worldsaves_b"]
    log = run("worldsaves.eval", words={"wt_saves": SAVES_WORLD})
    sec = sections_of(log, "saves")
    listed = sec.get("listed after the switch", "")
    names = re.findall(r"console:\s+(\S+) \[", listed)
    # THE CONTROL is A, saved before the switch: listed, the rule was gone; B
    # missing, the list was the launch world's (the bug).
    check(save_b in names and save_a not in names,
          "after a switch, `load` lists the world in hand's save and not the launch "
          "world's", f"listed: {names}")
    check(f"1 save(s) listed for '{SAVES_WORLD}'" in listed,
          "...and the title lists from the same world", listed.strip()[:300])
    typ = sec.get("a type only a save names", "")
    check("monsters 'blob': 0 level record(s), 0 other reference(s)" in typ,
          "the blob type is named by nothing in the new world but the save",
          typ.strip()[:300])
    check(f"Save file(s) still reference type 'blob': {save_b}" in typ,
          "deleting it warns that the world in hand's save still names it (the sweep "
          "found the save)", typ.strip()[:400])
    check("eval RESULT=PASS script=worldsaves.eval" in log, "the script ran clean")

    # C349: a copy of the good save with ONE hand-edited line, member -1 then
    # pack -1. The good save loading first is the control.
    good = harness_game.save_path(save_a)
    bad = harness_game.save_path(SAVES["worldsaves_bad"])
    if not os.path.isfile(good):
        check(False, "worldsaves.eval left a save to break", good)
    else:
        text = read(good)
        anchor = re.search(r"^char 0 [^\r\n]*\r?\n", text, re.M)
        check(anchor is not None, "the save has a member line to set the bad one beside")
        for what, line, says in (("member -1", "char -1 10 10 10 10 10 10 0", "names member -1"),
                                 ("pack -1", "packc 0 -1 -", "names pack -1")):
            if anchor is None:
                break
            write(bad, text[:anchor.end()] + line + "\n" + text[anchor.end():])
            log = run("worldbadsave.eval")
            check(f"console: loaded: {save_a}" in log,
                  f"{what}: the good save loads (the control)")
            check(f"'{line}' {says}" in log and f"LoadGame: could not read {bad}" in log,
                  f"{what}: the save is refused, naming the line, and not loaded")
            check("console: state playing" in log and
                  "eval RESULT=PASS script=worldbadsave.eval" in log,
                  f"{what}: and the game goes on (no abort, the probe refused as written)")
        harness_game.remove_saves([SAVES["worldsaves_bad"]])

    # C367: A held open by this process for the length of the run, B read-only.
    held = harness_game.save_path(save_a)
    ro = harness_game.save_path(save_b)
    if not (os.path.isfile(held) and os.path.isfile(ro)):
        check(False, "worldsaves.eval left both saves for the delete check", f"{held} / {ro}")
    else:
        os.chmod(ro, stat.S_IREAD)
        try:
            with open(held, "rb"):
                log = run("savedelete.eval", words={"worldsaves_a": save_a,
                                                    "worldsaves_b": save_b})
        finally:
            for p in (held, ro):
                if os.path.exists(p):
                    os.chmod(p, stat.S_IREAD | stat.S_IWRITE)
        sec = sections_of(log, "delete")
        # The control: a delete works at all - and past the read-only attribute,
        # which the library's remove ignores, so read-only is not a failure mode.
        check(f"console: deleted: {save_b}" in sec.get("a read-only save", "") and
              not os.path.exists(ro),
              "a read-only save is deleted (the control that a delete works)")
        heldsec = sec.get("a save held open", "")
        code = re.search(r"deletesave: " + re.escape(f"Could not delete {save_a} (error ")
                         + r"(\d+)\)\.", heldsec)
        check(code is not None,
              "a save held open is refused with the page's line: its name and the error code",
              heldsec.strip()[:400])
        check(re.search(r"Could not delete save \S*" + re.escape(os.path.basename(held)) +
                        r": .+ \(error \d+\)", heldsec) is not None,
              "...and the log says why, in the system's words")
        check(os.path.isfile(held), "...and the save is still there")
        check("eval RESULT=PASS script=savedelete.eval" in log, "the script ran clean")

    # C364: Start New Game opens alike on the level in hand and from another.
    log = run("newgameintro.eval")
    sec = sections_of(log, "intro")
    opening = ("You descend into the dungeon", "Something shuffles in the dark")

    def opens(name):
        return all(o in sec.get(name, "") for o in opening)
    check(opens("in hand"), "a new game on the level in hand opens with the opening (the control)")
    check(not opens("after a goto"),
          "a plain level change starts the log afresh, so the next lines are the new game's")
    check(opens("from crypt2"),
          "Start New Game from crypt2, which loads its level first, opens with the same lines",
          sec.get("from crypt2", "").strip()[:400])
    check("eval RESULT=PASS script=newgameintro.eval" in log, "the script ran clean")

    # --- phase 23: terrain kinds and landing cells (code-review batch 89) ----
    print("\n23 - terrain kinds and landing cells the world opens again with")
    # From the scratch world's own files: earlier phases leave their edits in
    # it, and this phase reads the terrain and doorways it starts from.
    for p in (WORLD, TERRAIN):
        write(p, originals[p])

    def terrain_blocks(con):
        """{id: (glyph, cells)} for each `world` readout, in order."""
        out, cur = [], None
        for l in con:
            if l == "> world":
                cur = {}
                out.append(cur)
            elif l.startswith(">"):
                cur = None
            elif cur is not None:
                m = re.match(r"\s*terrain (\S+)\s+'(.)'\s+(\d+) cells", l)
                if m:
                    cur[m.group(1)] = (m.group(2), int(m.group(3)))
        return out

    def notes_of(con):
        return [m.group(1) for m in (re.search(r"doorways note '(.*)'$", l) for l in con
                                     if l.startswith("world settings open: tab")) if m]

    def wrote_world(log, cmd, last=False):
        """Did script line `cmd` write world.map before the next line ran? The
        first such line, or the last with `last` (the script deletes terrain1
        twice)."""
        at = (log.rfind if last else log.find)("console: > " + cmd)
        if at < 0:
            return False
        nxt = log.find("console: >", at + 1)
        return any("Wrote " in l and l.rstrip().endswith("world.map")
                   for l in log[at:nxt if nxt >= 0 else len(log)].splitlines())

    try:
        log = run("worldterrain.eval")
        con = [l.split("console: ", 1)[1] for l in log.splitlines() if "console: " in l]

        # C343: HALF AN ENTRY and a negative one are refused, each saying which
        # (the expect-refuse lines already demand the refusal; this, the rule).
        check(any(l.startswith("refused: half an entry - gate2") for l in con),
              "`worldloc set gate2 entryx 5` on a doorway with no landing cell is refused "
              "as half an entry")
        check(sum(l == "refused: an entry cell cannot be negative" for l in con) == 2,
              "and a negative Z (or either half) is refused as negative")
        check("gate2.entry = 4,5" in con and "gate2.entry = 6,5" in con,
              "a whole entry is taken, and then one half moves on its own (the control)")
        notes = notes_of(con)
        check(len(notes) == 2 and notes[0] not in ("", "-") and notes[1] == "",
              "the dialog's landing fields refuse a negative in their status row, and take "
              "a whole cell", f"notes {notes}")

        # C345: two "+ New" terrains, a glyph each - none another kind's, no '?'.
        blocks = terrain_blocks(con)
        check(len(blocks) == 9, f"the script read the world nine times (got {len(blocks)})")
        g1 = g2 = ""
        if blocks:
            made = blocks[0]
            g1 = made.get("terrain1", ("", 0))[0]
            g2 = made.get("terrain2", ("", 0))[0]
            others = {g for k, (g, _) in made.items() if k not in ("terrain1", "terrain2")}
            check(g1 and g2 and g1 != g2 and "?" not in (g1, g2) and not ({g1, g2} & others),
                  "two new terrains get glyphs of their own, neither '?' nor another kind's",
                  f"terrain1 '{g1}', terrain2 '{g2}', taken {sorted(others)}")
        refusals = [l for l in con if l.startswith("typeset terrain 'terrain2': glyph refused")]
        for want, what in (("already the glyph of Road", "road's glyph"),
                           ("'b' is lowercase", "a lowercase glyph"),
                           ("('CC' is not)", "two characters"),
                           ("('' is not)", "no glyph at all")):
            check(any(want in l for l in refusals), f"a save naming {what} is refused, saying so",
                  " | ".join(refusals))
        # ...AND NOTHING WAS WRITTEN: a refusal that wrote first would leave the
        # empty or two-character glyph, which the world reads as '?' - so the
        # kind still wears the glyph it was made with (the refusal lines above
        # match the message alone, which a write-then-refuse prints too).
        check(len(blocks) >= 2 and blocks[1].get("terrain2", ("", 0))[0] == g2 != "",
              "and none of the four refused saves wrote the glyph: terrain2 keeps its own",
              f"made with '{g2}', then {blocks[1:2]}")
        check(len(blocks) >= 2 and blocks[1].get("terrain2", ("", 0))[1] == 1,
              "the new kind paints at once (the world was handed it)")
        check("typeset delete terrain 'terrain2': refused - Still painted on 1 world "
              "square(s) - paint them over first." in con and
              any(l.startswith("typeset delete terrain 'road': refused - Still painted on")
                  for l in con),
              "a delete of a painted kind is refused, naming its squares - the new one and road")
        check("terrain 'terrain2': 0 level record(s), 1 other reference(s)" in con,
              "typerefs counts a world square as a reference")
        # A CHANGED GLYPH IS WRITTEN AT ONCE: world.map, saved with the old one a
        # moment before, would otherwise spell 6,6 with a glyph terrain.cat no
        # longer names - the next launch's abort.
        check(wrote_world(log, "typeset terrain terrain2 glyph Q"),
              "a changed glyph writes world.map in the same breath as terrain.cat")
        check(len(blocks) >= 3 and blocks[2].get("terrain2") == ("Q", 1),
              "and the square reads the new glyph", f"{blocks[2:3]}")
        # The undo KEEPS TODAY'S KINDS: the snapshot still held the old glyph.
        check(len(blocks) >= 5 and blocks[3].get("terrain2") == ("Q", 0) and
              blocks[4].get("terrain2") == ("Q", 1),
              "an undo takes back the paint and keeps the new glyph; the redo puts it back",
              f"{[b.get('terrain2') for b in blocks[3:5]]}")
        check("typeset delete terrain 'terrain1': done" in con and len(blocks) >= 6 and
              "terrain1" not in blocks[5] and blocks[5].get("terrain2") == ("Q", 1),
              "an unused kind deletes, and the kind after it keeps its square",
              f"{blocks[5:6]}")

        # A RENAME REACHES THE WORLD'S COPY (SweepCatalogRefs -> RenameTerrain):
        # without it the brush refuses the new id, and the sync by id after the
        # glyph change finds a painted kind gone, keeps the old glyph and writes
        # nothing - the file left naming a glyph terrain.cat does not, which the
        # reopen below aborts on. (terrain1 and terrain3 were made just before.)
        check("typeset rename terrain 'terrain2': done" in con,
              "the painted terrain2 is renamed marsh")
        b6 = blocks[6] if len(blocks) >= 7 else {}
        check("terrain2" not in b6 and b6.get("marsh") == ("R", 2),
              "the world knows it by the new name: it paints, and both squares take its "
              "new glyph", f"{b6}")
        check(wrote_world(log, "typeset terrain marsh glyph R"),
              "and that glyph reaches world.map at once")

        # WORLD.MAP ON DISK, NOT THE LOADED WORLD. terrain1 (a free id again) and
        # terrain3 were painted and SAVED, then painted over in memory only, so
        # the loaded world's text did not move when terrain3's glyph changed or
        # terrain1 went - but the file still spelled a square with each old glyph,
        # and terrain.cat no longer named either: the reopen aborted on the first.
        # Nothing writes the world after this in the script, so the reopen sees
        # what these two left.
        b7 = blocks[7] if len(blocks) >= 8 else {}
        check(b7.get("terrain1", ("", 0))[1] == 1 and b7.get("terrain3", ("", 0))[1] == 1,
              "two more new kinds, a square each, saved (the premise)", f"{b7}")
        check(wrote_world(log, "typeset terrain terrain3 glyph S"),
              "a glyph change writes world.map although no square in memory is the kind")
        check(wrote_world(log, "typeset delete terrain terrain1", last=True),
              "and so does deleting a kind no square in memory is, but one on disk was")
        b8 = blocks[8] if len(blocks) >= 9 else {}
        check("terrain1" not in b8 and b8.get("terrain3") == ("S", 0) and
              b8.get("marsh") == ("R", 2),
              "the deleted kind gone, the other under its new glyph and on no square",
              f"{b8}")
        check(not any("map.check.terrain" in l for l in con),
              "and the checker finds nothing wrong with the terrain")

        # THE SECOND LAUNCH. Every one of these faults used to pass in the session
        # that made it and abort WorldMap::Load in the next; finishing is the check.
        log = run("worldreopen.eval")
        con = [l.split("console: ", 1)[1] for l in log.splitlines() if "console: " in l]
        check("eval RESULT=PASS script=worldreopen.eval" in log,
              "the world opens again in a new launch")
        blocks = terrain_blocks(con)
        check(len(blocks) == 1 and blocks[0].get("marsh") == ("R", 2) and
              blocks[0].get("terrain3") == ("S", 0) and
              not ({"terrain1", "terrain2"} & set(blocks[0])),
              "with the renamed kind's squares under its new glyph, the other new kind "
              "under its own, and the deleted kind gone", f"{blocks}")
        locs = {l.split()[1]: l.split()[-1] for l in con
                if re.match(r"\s+dungeon\s+\S+\s+\d+,\d+\s+->", l)}
        check(locs.get("gate2") == "6,5" and locs.get("crypt_gate") == "7,7",
              "and the landing cells as set: the console's and the dialog's", f"{locs}")
        check(not any("map.check.terrain" in l for l in con),
              "and the checker finds nothing wrong with the terrain it read back")
        world = read(WORLD)
        cat = read(TERRAIN)
        check("location dungeon gate2 12 6 dungeon=crypt level=crypt1 entryx=6 entryz=5" in world
              and re.search(r"\[marsh\][^\[]*\nglyph = R\r?\n", cat) and
              re.search(r"\[terrain3\][^\[]*\nglyph = S\r?\n", cat) and
              "[terrain1]" not in cat and "[terrain2]" not in cat,
              "on disk: the whole entry, the new kinds with their glyphs, the deleted one gone")
    finally:
        for p in (WORLD, TERRAIN):
            write(p, originals[p])

except Stop as why:
    # Already counted as a failure where it was found: a game RAN, so this is a
    # FAIL like any other (exit 1), never the "nothing ran" of exit 2.
    print(f"\nstopped early: {why}")
finally:
    harness_audio.restore(audio)
    if settings_before is not None:
        write(SETTINGS, settings_before)
    elif os.path.isfile(SETTINGS):
        os.remove(SETTINGS)  # the run made it (the mute, phase 16's control)
    # THE SAVES THIS SUITE MAKES ARE NOT ITS OWN BUSINESS ALONE. A save on disk
    # puts Continue and Load on the landing page, and the folder is shared, so
    # they all go - and only they: each is this worktree's slot. The worlds go
    # too, the scratch copy with them (and phase 13's crypt3 inside it, which
    # once made the NEXT run's "created crypt3" land on crypt4).
    for w in WORLDS:
        harness_game.remove_world(ROOT, w)
    harness_game.remove_saves(SAVES.values())

print("\nthe real tree")
real.check(check)
print(f"\nworldtest RESULT={'FAIL' if failures else 'PASS'} failures={failures}")
sys.exit(1 if failures else 0)
