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
#   2. CHECKING DOES NOT CHANGE WHAT A SAVE WRITES: after a validate and two
#      `typerefs`, savemap writes the active level alone (the checker and the
#      type-usage count both used to stash every level, and a stashed level is
#      one savemap rewrites).
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
#   6. LIVE VALIDATION'S BOXES: two items walled into a pocket are one finding
#      but two boxes; a stair whose partner was deleted is boxed on its own
#      level AND at the far end on the other; a finding with no square (a
#      dungeon with no levels) is counted on the Check badge instead.
#   7. PAINTING WITH A THEME: an area fill with "marble hall" makes the
#      room's floors and ceilings and the walls around it REFERENCE it, every
#      one resolving to its one member per surface (the floor one the palette
#      had to enrol); the eyedropper picks the theme up; it survives a save and
#      a reload with the geometry unchanged; and "+ New..." seeds a theme
#      from the selected square's look.
#   8. EDITING A THEME reaches every square painted with it: a new floor
#      shows at once on the level in hand and on a level NOT loaded, and the
#      save that follows writes that level but not one that does not use it;
#      a rename reaches the squares, a delete is refused while any use it, and
#      renaming a member floor type keeps it resolving.
#   9. MAKING A WORLD three ways: blank from the template, this world whole
#      (its UNSAVED edit written into the copy, and this world's own file left
#      alone), and one level (its stairs replaced by one exit). Each refusal
#      says its own reason; a half-built `.building-*` folder is never listed;
#      and each new world opens by name and passes the checker as it stands.
#  10. THE NEW WORLD DIALOG: its "Copy one level" makes a one-level world of
#      the level picked; a name in use is refused in its own words; and a world
#      made over the Worlds dialog lands in that list ARMED.
#  11. THE WIZARD: the template's content and one generated floor. The same
#      knobs and seed make an identical floor, another seed a different one;
#      the tag holds the monsters to it; the size is the map's; there
#      is a way out; and each world opens by name and passes the checker.
#  12. THE PALETTE'S CATEGORY BAR (tool-refinement Phase 1): both groupings
#      are the designed tables, each group lists exactly its sections, effects
#      are in none; the world sections list their entries; a filter from inside
#      one group finds matches in the others and drops sections with none; a
#      filter is capped in characters, never cut inside one (C383); and the
#      grouping survives a restart through settings.ini.
#  13. MONSTER POWER (Phase 2): unset, a kind's power is its derived threat;
#      the bands are the fifths of the project's range (recomputed here, not
#      read back) and the palette's rows wear them; an override moves that
#      kind, re-cuts every band, and moves the generator's pick - a control
#      level has the swarm, the same level after the override has none, and a
#      boss is exactly one swarm; removing the override restores everything.
#  14. THE DOCKS AND THE OVERVIEW (Phase 3): a dock takes the width it is
#      dragged to and everything beside it follows (strip, grid, palette
#      body); both clamps hold; the widths survive a restart; the overview's
#      level / dungeon / world counts equal the PROJECT FILES' (read here, not
#      from the game); and a monster placed on the viewed level counts at once
#      and uncounts on undo.
#  15. FLAGS (Phase 4), on a fixture written into eval_arena: a door waiting on
#      a flag stays sealed and a lever waiting on it stays put until a lever
#      sets it; an item's hook sets one; the save carries them; a stair waiting
#      on one bars the way until it is on, then goes down; the checker names a
#      wait nothing satisfies, a flag flags.cat lacks, a dungeon's flag used in
#      another and a flag nothing touches - and nothing about the sound ones;
#      the inspectors' setters rewrite the records; the overview counts flags
#      per scope; and the palette's Quest items & flags section lists this
#      dungeon's and the world's (not another dungeon's), says where a quest
#      item lies, arms it, goes there and opens a flag's editor.
#  16. STYLES (Phase 5): the world's and the library's are listed apart; the
#      armed style ranks the Monsters section by its list and disarms again;
#      adding a library style copies it, its themes and the one floor the
#      world lacked - and nothing the world had - and a second add is a no-op;
#      a world style saves to the library with what it names; the checker
#      names a monster a style lists and the world lacks; and renaming a theme
#      or a flag rewrites the styles, levers, stairs and items that name it.
#  17. THE SHAPE BRUSHES (Phase 6), on eval_arena turned to rock round one
#      room: a corridor opens the rock it crosses, reaches its far end, and
#      wears the style's corridor theme on its floor and walls - and undo puts
#      the geometry back exactly; a room opens its rectangle in the room theme;
#      a stamp raises its pillars, never on the party; a region generates
#      inside its box joined to the room it touches; no style = plain carving;
#      a region under 6x6 makes nothing.
#  18. THE WORKFLOW, WIRED THROUGH (Phase 7): a blank world made in a library
#      style receives it (and its themes), its dungeon names it and its first
#      room wears it; a wizard world in a style lays both themes and draws the
#      style's monsters; both pass the checker. Then the walk, inside the new
#      world: [+] opens on the dungeon's style; Create and Empty each land in
#      Build with the style armed, the level tagged and themed with no Level
#      settings visit; a room, a corridor and a stamp build; Populate fills the
#      hand-built floor from the style's list; and the overview's "what next"
#      reads build -> populate -> check/ready, counting what the files hold.
#
# NOTHING HERE EDITS THE REAL WORLD (code-review C431). Each phase starts on a
# FRESH SCRATCH COPY of dungeon-demo, et_demo, and every run opens it with
# -project; the phases used to edit dungeon-demo itself and put it back with a
# delete then a copy, so a run killed mid-phase left it changed - and the next
# run began by deleting the backup that was its only clean copy. Now a killed
# run leaves a scratch folder, which the next run clears. The style library has
# one fixed home (assets/library), so phase 16 still changes it - behind a
# backup that only the restore that used it deletes (harness_game.back_up). The
# save phase 15 makes is this worktree's (harness_game.save_name). The run ends
# by checking the real worlds and the library are byte for byte as it found
# them - BEFORE it cleared up after a killed run, so the clean-up is judged too
# - and that git status shows nothing new and nothing of this judge's worlds.
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECTS = os.path.join(ROOT, r"assets\projects")
SCRATCH = "et_demo"
PROJ = os.path.join(PROJECTS, SCRATCH)
LIBRARY = os.path.join(ROOT, r"assets\library")
LIBBAK = os.path.join(ROOT, r"build\editortest-library")
EXE = os.path.join(ROOT, r"build\debug\bin\Dungeon.exe")
LOG = os.path.join(ROOT, r"build\debug\bin\dungeon.log")
SCRIPTS = os.path.join(ROOT, r"tools\EvalScripts")
COPIES = os.path.join(ROOT, r"build\harness-scripts\editortest")
# Every world a phase makes (and the half-built one phase 9 plants), with the
# scratch copy: cleared at the start (what a killed run left) and at the end,
# each with the .building-<name> a create killed half-way leaves.
WORLDS = (SCRATCH, "nw_blank", "nw_copy", "nw_level", "nw_bad", ".building-nw_ghost",
          "nwd_level", "nwd_blank", "wz_a", "wz_b", "wz_c", "wz_undead", "wz_dlg",
          "p7_world", "p7_wiz")

# Never a stale exe, and never beside this worktree's own game, which shares
# the log every phase reads (tools/harness_game.py).
import harness_game
harness_game.refuse_if_stale(EXE)
harness_game.refuse_if_running(EXE)

# flags.eval's save slot, renamed to this worktree's: the saves folder is
# shared with every other session and with Michael's own play.
SAVES = {"flagtest": harness_game.save_name(ROOT, "flagtest")}


def cleanup():
    """Every scratch world and save this judge makes, and the library put back
    if a backup of it is standing. Safe to run twice."""
    harness_game.recover(LIBRARY, LIBBAK)
    for w in WORLDS:
        harness_game.remove_world(ROOT, w)
    harness_game.remove_saves(SAVES.values())


# The guard is taken BEFORE clearing up what a killed run left, so a clean-up
# that does damage fails the run instead of becoming its baseline: the library
# must end as a standing backup holds it, and nothing of this judge's worlds may
# be left in git status, a killed run's included.
real = harness_game.RealTree(ROOT, own=WORLDS, backups={LIBRARY: LIBBAK})
cleanup()

# Muted for the whole run (tools/harness_audio.py), and cleaned up however it
# ends. The phases are flat, not one try block, so both ride atexit - which also
# runs after sys.exit, an uncaught exception and Ctrl+C (not after a kill: the
# next run's cleanup() above is for that).
import atexit
import harness_audio
atexit.register(harness_audio.restore, harness_audio.mute(os.path.dirname(EXE)))
atexit.register(cleanup)

failures = 0


def fresh():
    """Each phase starts on a fresh copy of dungeon-demo."""
    harness_game.scratch_world(ROOT, SCRATCH)


def drop():
    """...and its scratch world goes with it."""
    harness_game.remove_world(ROOT, SCRATCH)


def run(script, project=SCRATCH):
    # -project opens the scratch world, never the real one (nor whatever world
    # the developer last switched to). A run that died before its verdict
    # counts as a failure on its own, not as a log to be read as if it were
    # whole.
    global failures
    path = harness_game.eval_script(os.path.join(SCRIPTS, script), COPIES, SAVES)
    code, log = harness_game.run_eval(EXE, ROOT, LOG, [path], ["-project", project])
    if harness_game.report_unfinished(code, log, script):
        failures += 1
    return log


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


def console_sections(log):
    """Every console line (not only `editor` ones), split on '--- name ---'."""
    out, name = {}, None
    for line in log.splitlines():
        if "console: " not in line:
            continue
        text = line.split("console: ", 1)[1]
        m = re.match(r"--- (.*) ---$", text)
        if m:
            name = m.group(1)
            out[name] = []
        elif name is not None:
            out[name].append(text)
    return out


# The editor's own state (palette grouping, dock widths) lives in settings.ini
# beside the exe; the phases that change it put the developer's copy back.
SETTINGS = os.path.join(os.path.dirname(EXE), "settings.ini")


# --- phase 1: a drag is one undo step ----------------------------------------
print("1 - a paint drag is one undo step")
fresh()
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
fresh()
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
    drop()

# --- phase 3: the edit counter ------------------------------------------------
print("3 - the edit counter moves on a change and only on a change")
fresh()
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
fresh()
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
fresh()
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

# --- phase 6: live validation's boxes -----------------------------------------
print("6 - live validation boxes what it finds, where it is")


def sections(log):
    """The console output split on the script's '--- name ---' echoes."""
    out, name = {}, None
    for line in log.splitlines():
        m = re.search(r"console: --- (.*) ---$", line)
        if m:
            name = m.group(1)
            out[name] = []
        elif name is not None and "console: editor " in line:
            out[name].append(line.split("console: ", 1)[1])
    return out


fresh()
try:
    # The two breakages the eval script's PART B and the badge rely on.
    crypt2 = os.path.join(PROJ, r"levels\crypt2.map")
    text = io.open(crypt2, encoding="utf-8", newline="").read()
    lines = [l for l in text.splitlines(True) if not l.startswith("stairs stairs_up 1 1")]
    check(len(lines) == len(text.splitlines(True)) - 1, "removed crypt2's stair back up")
    io.open(crypt2, "w", encoding="utf-8", newline="").write("".join(lines))
    dungeons = os.path.join(PROJ, r"catalog\dungeons.cat")
    d = io.open(dungeons, encoding="utf-8", newline="").read()
    io.open(dungeons, "w", encoding="utf-8", newline="").write(
        d + "\r\n[hollow]\r\nlevels =\r\n")

    log = run("liveissues.eval")
    check(passed(log), "the script ran clean")
    s = sections(log)
    base = s.get("A: baseline", [])
    check(not any(l.startswith("editor box eval_arena") for l in base),
          "eval_arena starts with nothing boxed", str(base))
    # It is two world findings, not one (no levels, and so no way in either);
    # without it the project's badge reads 0, so any count here is its.
    badge = [int(m.group(1)) for l in base for m in [re.search(r"badge (\d+)", l)] if m]
    check(bool(badge) and badge[0] >= 1,
          "the dungeon with no levels is counted on the Check badge", str(base))
    walled = s.get("A: two items walled in", [])
    check("editor box eval_arena 3,3 warning map.check.itemslost" in walled,
          "the first walled-in item is boxed amber", str(walled))
    check("editor box eval_arena 4,3 warning map.check.itemslost (from eval_arena 3,3)" in walled,
          "so is the second, from the same one finding", str(walled))
    own = s.get("B: the stair's own end", [])
    check("editor box crypt1 1,1 error map.check.stairunpaired" in own,
          "the unpaired stair is boxed red where it stands", str(own))
    far = s.get("B: the far end", [])
    check("editor box crypt2 1,1 error map.check.stairunpaired (from crypt1 1,1)" in far,
          "and at its far end, on the level it leads to", str(far))
finally:
    drop()

# --- phase 7: painting with a theme -------------------------------------------
print("7 - a theme paints a whole look, by reference")
CELL = re.compile(r"console: editor cell (\S+) (\d+),(\d+) (\w+) wall=(\S+)/(\S+) "
                  r"floor=(\S+)/(\S+) ceiling=(\S+)/(\S+)")
fresh()
try:
    # floor_rubble is not in eval_arena's palette: painting must enrol it.
    io.open(os.path.join(PROJ, r"catalog\themes.cat"), "w", encoding="utf-8", newline="").write(
        "[marble_hall]\r\ndisplay = Marble Hall\r\nfloor = floor_rubble\r\n"
        "wall = wall_marble\r\nceiling = ceiling_stone\r\n")
    log = run("themes.eval")
    check(passed(log), "the script ran clean")
    m = re.search(r"editor: Filled the room or corridor \((\d+) cells\)", log)
    check(m is not None and int(m.group(1)) == 212,
          "the area fill took the room (154) and its walls (58)", m.group(0) if m else "none")
    cells = [c.groups() for c in CELL.finditer(log)]
    before = cells[:6]
    opens = [c for c in before if c[3] == "open"]
    solids = [c for c in before if c[3] == "solid"]
    check(len(opens) == 4 and len(solids) == 2, "four room squares and two walls read back",
          str(len(before)))
    check(all(c[6] == "theme:marble_hall" and c[7] == "floor_rubble" and
              c[8] == "theme:marble_hall" and c[9] == "ceiling_stone" for c in opens),
          "every room square references it and shows its members", str(opens))
    check(len(opens) > 0 and all(c[7] == "floor_rubble" for c in opens),
          "including the floor the palette had to enrol", str([c[7] for c in opens]))
    check(all(c[4] == "theme:marble_hall" and c[5] == "wall_marble" for c in solids),
          "the walls around the room reference it too", str(solids))
    check("console: editor pick: themes marble_hall" in log,
          "the eyedropper picks up the theme, not its member")
    h = hashes(log)
    check(len(h) == 3 and h[1] != h[0] and h[2] == h[1],
          "painting changed the level, and a save and reload kept it exactly",
          str([x[2] for x in h]))
    after = cells[6:8]
    check(len(after) == 2 and after[0][6] == "theme:marble_hall" and
          after[1][4] == "theme:marble_hall", "the references came back from the file", str(after))
    # "+ New..." seeds a theme from the SELECTED square: 1,5's floor and
    # ceiling and its west wall, one member each - so what it paints must show
    # exactly those.
    seed = cells[8:]
    if len(seed) != 3:
        check(False, "three squares read for the new theme", str(len(seed)))
    else:
        src, room, wall = seed
        check(room[6] == "theme:theme1" and room[7] == src[7] and room[9] == src[9],
              "a new theme takes the selected square's floor and ceiling",
              f"{src[7]}/{src[9]} -> {room[7]}/{room[9]}")
        check(wall[4] == "theme:theme1" and wall[5] == "wall_marble",
              "and the wall beside it", str(wall))
finally:
    drop()

# --- phase 8: editing a theme -------------------------------------------------
print("8 - editing a theme repaints every square that uses it")
fresh()
try:
    io.open(os.path.join(PROJ, r"catalog\themes.cat"), "w", encoding="utf-8", newline="").write(
        "[marble_hall]\r\ndisplay = Marble Hall\r\nfloor = floor_rubble\r\n"
        "wall = wall_marble\r\nceiling = ceiling_stone\r\n")
    log = run("themeedit.eval")
    check(passed(log), "the script ran clean")
    s = {}
    name = None
    for line in log.splitlines():
        m = re.search(r"console: --- (\d+):", line)
        if m:
            name = m.group(1)
            s[name] = []
        elif name and "console: " in line:
            s[name].append(line.split("console: ", 1)[1])
    two, three, four = s.get("2", []), s.get("3", []), s.get("4", [])
    floors2 = [l for l in two if l.startswith("editor cell")]
    check(len(floors2) == 3 and all("floor=theme:marble_hall/floor_temple" in l for l in floors2),
          "the new floor shows on the level in hand AND on crypt2, not loaded",
          str(floors2))
    saved = [l for l in two if l.startswith("saved levels:")]
    check(saved and "crypt2" in saved[0] and "crypt1" not in saved[0],
          "the save wrote crypt2, which uses it, and not crypt1, which does not",
          str(saved))
    check(any("floor=theme:grand_hall/" in l for l in three), "a rename reaches the squares",
          str(three))
    check(any(l.startswith("typeset delete themes 'grand_hall': refused") for l in three),
          "a delete is refused while squares use it", str(three))
    check(any("floor=theme:grand_hall/floor_temple_b" in l for l in four),
          "a member floor type renamed keeps the theme resolving", str(four))
finally:
    drop()

# --- phase 9: making a world ----------------------------------------------------
print("9 - a new world three ways: blank, this world whole, one level")
MADE = ("nw_blank", "nw_copy", "nw_level", "nw_bad")
LEFTOVER = os.path.join(PROJECTS, ".building-nw_ghost")
fresh()
arena = os.path.join(PROJ, r"levels\eval_arena.map")
arena_before = io.open(arena, "rb").read()
try:
    # An interrupted create leaves one of these; no world list may offer it.
    os.makedirs(LEFTOVER, exist_ok=True)
    io.open(os.path.join(LEFTOVER, "project.ini"), "w").write("name = ghost\n")
    log = run("newworlds.eval")
    check(passed(log), "the script ran clean")
    for w in ("nw_blank", "nw_copy", "nw_level"):
        check(f"console: created world '{w}'" in log, f"{w} was made")
    refusals = log.split("--- refusals ---", 1)[-1]
    check("could not create: A world named 'nw_blank' already exists." in refusals,
          "a name in use is refused, in its own words")
    check("could not create: Type a name first" in refusals,
          "a name that filters to nothing is refused, in its own words")
    check("could not create: 'nowhere' is not a level of this world." in refusals,
          "a level the world does not have is refused, in its own words")
    check("nw_ghost" not in refusals and ".building" not in refusals,
          "a half-built .building folder is not listed as a world")
    check(not os.path.isdir(os.path.join(PROJECTS, "nw_bad")),
          "a refused create leaves nothing behind")

    def read(world, rel):
        p = os.path.join(PROJECTS, world, rel)
        return io.open(p, encoding="utf-8").read() if os.path.isfile(p) else ""

    # THE COPY CARRIES THE UNSAVED EDIT, AND THIS WORLD KEEPS ITS FILE. The 3x3
    # at 3..5 was painted and never saved: the copy's eval_arena must hold it
    # (variant records on those squares) and the world it was copied from (the
    # scratch copy of dungeon-demo) must not have moved.
    copied = read("nw_copy", r"levels\eval_arena.map")
    painted = sum(1 for x in range(3, 6) for z in range(3, 6)
                  if re.search(rf"^variant floor {x} {z} \d+", copied, re.M))
    check(painted == 9, "the copy carries the unsaved edit (9 painted squares)", str(painted))
    check(io.open(arena, "rb").read() == arena_before,
          "and the world it was copied from was NOT saved behind your back")
    one = read("nw_level", r"levels\crypt1.map")
    stairs = re.findall(r"^stairs (\S+) .*dest=(\S+)", one, re.M)
    check(stairs == [("stairs_exit", "keep_gate")],
          "the one-level world keeps no stair but an exit to its doorway", str(stairs))
    blank = read("nw_blank", r"levels\room1.map")
    check(re.search(r"^stairs stairs_exit 8 7 south dest=keep_gate", blank, re.M) is not None,
          "the blank world's first room has a way out")
    check("[marble_hall]" in read("nw_blank", r"catalog\themes.cat") and
          "[crypt]" not in read("nw_blank", r"catalog\dungeons.cat"),
          "and the template's content, without dungeon-demo's places")
    # Each opens BY NAME (-project) and is clean as it stands.
    for w in ("nw_blank", "nw_copy", "nw_level"):
        wlog = run("worldcheck.eval", project=w)
        check("validate: clean" in wlog, f"{w} opens and passes the checker",
              next((l for l in wlog.splitlines() if "validate" in l), "no validate line"))
finally:
    for w in MADE + (os.path.basename(LEFTOVER),):
        harness_game.remove_world(ROOT, w)
    drop()

# --- phase 10: the New world dialog ---------------------------------------------
print("10 - the New world dialog makes a world, and hands it to the Worlds list")
fresh()
try:
    log = run("newworlddialog.eval")
    check(passed(log), "the script ran clean")
    lines = [l.split("console: ", 1)[1] for l in log.splitlines()
             if "console: new world dialog" in l or "console: worlds dialog" in l]
    check(any(l.startswith("new world dialog open: source level made 'nwd_level'") for l in lines),
          "Copy one level makes the world, and says so", " | ".join(lines))
    check(any("already exists" in l for l in lines if l.startswith("new world dialog")),
          "a name in use is refused, in its own words")
    manifest = os.path.join(PROJECTS, "nwd_level", "project.ini")
    text = io.open(manifest, encoding="utf-8").read() if os.path.isfile(manifest) else ""
    check(re.search(r"^levels = crypt2\s*$", text, re.M) is not None,
          "and it holds the level picked, alone", text[:200])
    after = [l for l in lines if l.startswith("worlds dialog open")]
    check(bool(after) and "nwd_blank" in after[-1] and "armed 'nwd_blank'" in after[-1],
          "a world made over the Worlds dialog is listed there, armed",
          after[-1] if after else "no worlds dialog line")
finally:
    for w in ("nwd_level", "nwd_blank"):
        harness_game.remove_world(ROOT, w)
    drop()

# --- phase 11: the wizard -------------------------------------------------------
print("11 - the wizard generates a first floor, tagged and reproducible")
WIZ = ("wz_a", "wz_b", "wz_c", "wz_undead", "wz_dlg")


def tagged(tag):
    """Template monsters carrying `tag` (monsters.cat `tags`)."""
    text = io.open(os.path.join(ROOT, r"assets\templates\default\catalog\monsters.cat"),
                   encoding="utf-8").read()
    out, cur = set(), None
    for line in text.splitlines():
        m = re.match(r"\[(\S+)\]", line)
        if m:
            cur = m.group(1)
        elif cur and re.match(r"tags\s*=", line) and tag in line.split("=", 1)[1].split():
            out.add(cur)
    return out


fresh()
try:
    log = run("wizard.eval")
    check(passed(log), "the script ran clean")
    for w in WIZ[:-1]:
        check(f"console: created world '{w}'" in log, f"{w} was made")
    check("source wizard made 'wz_dlg'" in log, "wz_dlg was made, through the dialog")
    themes = next((l.split("wizard tags: ", 1)[1] for l in log.splitlines()
                   if "wizard tags: " in l), "")
    check("vermin" in themes.split() and "undead" in themes.split(),
          "the tag choices are the template's tags", themes)

    def floor(w, ext):
        p = os.path.join(PROJECTS, w, "levels", "floor1." + ext)
        return io.open(p, encoding="utf-8").read() if os.path.isfile(p) else ""

    a_map, a_ent = floor("wz_a", "map"), floor("wz_a", "ent")
    check(a_map != "" and a_map == floor("wz_b", "map") and a_ent == floor("wz_b", "ent"),
          "the same knobs and seed make the SAME floor")
    check(floor("wz_c", "map") not in ("", a_map), "another seed makes a different one")
    check(floor("wz_dlg", "map") == a_map and floor("wz_dlg", "ent") == a_ent,
          "the dialog's wizard rows make the same floor as the console, from the same knobs")
    grid = [l for l in a_map.splitlines() if l and l[0] in "#.P"]
    check(bool(grid) and len(grid) == 24 and all(len(l) == 24 for l in grid),
          "the size asked for is the map's (24 x 24)", f"{len(grid)} rows")
    for w, tag in (("wz_a", "vermin"), ("wz_undead", "undead")):
        monsters = set(re.findall(r"^monster (\S+)", floor(w, "ent"), re.M))
        check(bool(monsters) and monsters <= tagged(tag),
              f"{w}'s monsters all carry '{tag}'", str(sorted(monsters)))
        check(re.search(r"^stairs stairs_exit \d+ \d+ \w+ dest=keep_gate", floor(w, "map"), re.M)
              is not None, f"{w}'s floor has a way out")
    for w in WIZ:
        wlog = run("worldcheck.eval", project=w)
        check("validate: clean" in wlog, f"{w} opens and passes the checker",
              next((l for l in wlog.splitlines() if "validate" in l), "no validate line"))
finally:
    for w in WIZ:
        harness_game.remove_world(ROOT, w)
    drop()

# --- phase 12: the palette's category bar -------------------------------------
print("12 - the category bar shows one group, both ways, and the filter sees past it")
# The groupings AS DESIGNED (docs/tool-refinement-plan.md Phase 1). Stated here
# rather than read back from the game, so a section moved to the wrong group is
# a failure and not a new truth.
STAGE = {"world": ["styles", "dungeons", "quests", "flags", "terrain"],
         "build": ["shapes", "themes", "walls", "floors", "ceilings", "wallfeatures",
                   "surfacefeatures", "doors", "stairs"],
         # Lights (lighting-updates Phase 2) sit beside the fixtures that give them.
         "furnishings": ["decorations", "fixtures", "lights", "trails", "buttons"],
         "populate": ["monsters", "items", "weapons", "armor"]}
KIND = {"surfaces": ["themes", "walls", "floors", "ceilings", "wallfeatures",
                     "surfacefeatures"],
        "structure": ["shapes", "doors", "stairs"],
        "furnishings": ["decorations", "fixtures", "lights", "trails", "buttons"],
        "creatures": ["monsters"],
        "items": ["items", "weapons", "armor"],
        "world": ["styles", "dungeons", "quests", "flags", "terrain"]}
SHOWS = re.compile(r"editor palette: (\w+) (\w+) filter='([^']*)' shows:(.*)")


def shown(line):
    """(grouping, group, filter, [(section, rows)]) from one palette line."""
    m = SHOWS.search(line)
    if not m:
        return None
    secs = [(s, int(n)) for s, n in re.findall(r"(\w+)\((\d+)\)", m.group(4))]
    return m.group(1), m.group(2), m.group(3), secs


# The bar's state lives in settings.ini beside the exe (SETTINGS, above), not
# in the world, so this phase puts the developer's copy back afterwards.
fresh()
saved_settings = io.open(SETTINGS, "rb").read() if os.path.isfile(SETTINGS) else None
try:
    log = run("palette.eval")
    check(passed(log), "the script ran clean")
    sec = sections(log)
    tables = {}
    for line in sec.get("tables", []):
        m = re.match(r"editor palette group (\w+) (\w+):(.*)", line)
        if m:
            tables.setdefault(m.group(1), {})[m.group(2)] = m.group(3).split()
    check(tables.get("stage") == STAGE, "the stage groups are World / Build / Furnishings / Populate as designed",
          str(tables.get("stage")))
    check(tables.get("kind") == KIND, "the kind groups are as designed", str(tables.get("kind")))
    for mode, groups in (("stage", STAGE), ("kind", KIND)):
        listed = [c for cats in groups.values() for c in cats]
        check(len(listed) == len(set(listed)) and "effects" not in listed,
              f"by {mode}, every section is in one group and effects in none")
    for mode, groups in (("stage", STAGE), ("kind", KIND)):
        lines = [shown(l) for l in sec.get(mode, []) if shown(l)]
        got = {g: [s for s, _ in secs] for _, g, _, secs in lines}
        check(got == groups, f"each {mode} group lists exactly its own sections", str(got))
    world = next((secs for _, g, _, secs in
                  (shown(l) for l in sec.get("stage", []) if shown(l)) if g == "world"), [])
    counts = dict(world)
    # Quest items & flags: the world's one flag and the two quest items (both
    # world-scoped; the harness views eval_arena, whose dungeon has none).
    check(counts.get("dungeons", 0) == 2 and counts.get("quests", 0) == 1
          and counts.get("terrain", 0) == 7 and counts.get("flags", 0) == 3
          and counts.get("styles", 0) == 4,
          "the world sections list their entries (4 styles, 2 dungeons, 1 quest, 3 quest rows, "
          "7 terrains)", str(world))
    flt = [shown(l) for l in sec.get("filter", []) if shown(l)]
    if len(flt) != 3:
        check(False, "three readings in the filter section", str(flt))
    else:
        (_, _, _, before), (_, g, f, during), (_, _, f2, after) = flt
        names = [s for s, _ in during]
        check(before == [s for s in before if s[0] == "monsters"] and len(before) == 1,
              "Creatures alone lists monsters", str(before))
        check(f == "marble" and "themes" in names and "decorations" in names,
              "a filter from inside Creatures finds the marble theme and props", str(during))
        check("monsters" not in names and all(n > 0 for _, n in during),
              "while filtering, a section with no match drops out", str(during))
        check(f2 == "" and [s for s, _ in after] == ["monsters"],
              "clearing the filter hands the list back to the bar", str(after))
    check(any("no group 'build' when grouped by kind" in l for l in sec.get("refuse", [])),
          "a group the grouping lacks is refused by name")
    # C383: "a" and 12 Cyrillic letters is 13 characters but 25 bytes. A cap in
    # bytes keeps half the last letter (a lone lead byte, read back as U+FFFD).
    wide = "a" + "".join(chr(0x430 + i) for i in range(12))
    uf = [shown(l) for l in sec.get("utf8", []) if shown(l)]
    check(len(uf) == 2 and uf[0][2] == wide and uf[1][2] == "",
          "a 13-character filter of 25 bytes is kept whole, not cut at a byte",
          ascii([u[2] for u in uf]))
    ini = io.open(SETTINGS, encoding="utf-8").read() if os.path.isfile(SETTINGS) else ""
    check("map_palette_group=1" in ini and "map_palette_kind=4" in ini,
          "the grouping and its group are saved to settings.ini")
    log2 = run("palette-persist.eval")
    back = [shown(l) for l in sections(log2).get("reopened", []) if shown(l)]
    check(len(back) == 1 and back[0][:2] == ("kind", "items"),
          "a fresh start opens the palette where it was left", str(back))
finally:
    drop()
    if saved_settings is None:
        if os.path.isfile(SETTINGS):
            os.remove(SETTINGS)
    else:
        io.open(SETTINGS, "wb").write(saved_settings)

# --- phase 13: monster power ---------------------------------------------------
print("13 - a monster's power is its threat until overridden, and everything ranks by it")


THREAT = re.compile(r"threat (\S+) ([\d.]+) offence=.* power=([\d.]+)(\(set\))? band=(\d)")


def table(lines):
    """{id: (threat, power, overridden, band)} from `threat` lines."""
    out = {}
    for l in lines:
        m = THREAT.match(l)
        if m:
            out[m.group(1)] = (float(m.group(2)), float(m.group(3)), bool(m.group(4)),
                               int(m.group(5)))
    return out


def expected_band(p, lo, hi):
    # Game/Power.h, written out again HERE so the check is not the code judging
    # itself: which fifth of the range, the top edge in band 5, no width = 3.
    if hi - lo <= 1e-9:
        return 3
    t = min(1.0, max(0.0, (p - lo) / (hi - lo)))
    return min(5, 1 + int(t * 5))


def bands_agree(t):
    powers = [v[1] for v in t.values()]
    lo, hi = min(powers), max(powers)
    return {k: v[3] for k, v in t.items()} == {k: expected_band(v[1], lo, hi) for k, v in t.items()}


def palette_bands(lines):
    return {m.group(1): int(m.group(2)) for m in
            (re.match(r"editor palette item monsters (\S+) band=(\d)", l) for l in lines) if m}


def swarms(stem):
    p = os.path.join(PROJ, "levels", stem + ".ent")
    text = io.open(p, encoding="utf-8").read() if os.path.isfile(p) else ""
    return len(re.findall(r"^monster skel_swarm ", text, re.M)), bool(text)


fresh()
try:
    log = run("power.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    before, after, cleared = (table(sec.get(s, [])) for s in ("before", "after", "cleared"))
    check(len(before) >= 10 and all(v[0] == v[1] and not v[2] for v in before.values()),
          "unset, every kind's power IS its derived threat", str(len(before)))
    check(bands_agree(before) and len({v[3] for v in before.values()}) >= 3,
          "the bands are the fifths of the project's power range (3+ bands in use)",
          str({k: v[3] for k, v in before.items()}))
    check(palette_bands(sec.get("before", [])) == {k: v[3] for k, v in before.items()},
          "the palette's rows wear the same bands")
    sw = after.get("skel_swarm")
    check(sw is not None and sw[1] == 40.0 and sw[2] and sw[0] == before["skel_swarm"][0],
          "an override sets the power and leaves the derived threat as it was", str(sw))
    check(sw is not None and sw[3] == 5 and bands_agree(after)
          and after["skel_lurker"][3] < before["skel_lurker"][3],
          "the bands re-cut against the new top (the old strongest drops a band or more)",
          str({k: v[3] for k, v in after.items()}))
    check(palette_bands(sec.get("after", [])) == {k: v[3] for k, v in after.items()},
          "the palette's pips follow the override")
    stems = [re.match(r"generate: wrote (\S+) ", l).group(1)
             for s in ("control", "picks") for l in sec.get(s, []) if l.startswith("generate: wrote")]
    if len(stems) != 3:
        check(False, "three generated levels", str(stems))
    else:
        (c_n, c_ok), (p_n, p_ok), (b_n, b_ok) = (swarms(s) for s in stems)
        check(c_ok and c_n > 0, "before the override the swarm is among the weak, and turns up",
              f"{c_n} swarms")
        check(p_ok and p_n == 0, "overridden to the top, the same low-difficulty level has none",
              f"{p_n} swarms")
        check(b_ok and b_n == 1, "with a boss, the boss is the overridden swarm (exactly one)",
              f"{b_n} swarms")
    cw = cleared.get("skel_swarm")
    check(cw is not None and not cw[2] and cw[1] == cw[0] and cleared == before,
          "removing the override puts every power and band back", str(cw))
finally:
    drop()

# --- phase 14: the docks and the overview --------------------------------------
print("14 - the docks resize and remember, and the overview counts what the files hold")
DOCK = re.compile(r"editor dock panel=(\d+) left=(\d+) right=(\d+) grid=(-?\d+),(\d+),(\d+) "
                  r"strip=(-?\d+) palette=(-?\d+),(\d+)")


def docks(lines):
    out = []
    for l in lines:
        m = DOCK.match(l)
        if m:
            v = [int(x) for x in m.groups()]
            out.append(dict(zip(("panel", "left", "right", "gx", "gw", "gright", "strip", "px", "pw"), v)))
    return out


def overview(lines, scope):
    return {m.group(1): m.group(2) for m in
            (re.match(rf"editor overview {scope} (\S+) (.*)", l) for l in lines) if m}


def files_census():
    """Each level's counts, read from the PROJECT FILES - not from the game."""
    ini = io.open(os.path.join(PROJ, "project.ini"), encoding="utf-8").read()
    levels = re.search(r"^levels\s*=\s*(.*)$", ini, re.M).group(1).split()
    quest, cur = set(), None
    for cat in ("items", "weapons", "armor"):
        for l in io.open(os.path.join(PROJ, "catalog", cat + ".cat"), encoding="utf-8"):
            h = re.match(r"\[(\S+)\]", l)
            if h:
                cur = h.group(1)
            elif cur and re.match(r"(quest|flag|reveals)\s*=", l):
                quest.add(cur)
    scenery, cur = set(), None
    for l in io.open(os.path.join(PROJ, r"catalog\stairs.cat"), encoding="utf-8"):
        h = re.match(r"\[(\S+)\]", l)
        if h:
            cur = h.group(1)
        elif cur and re.match(r"traverse\s*=\s*0", l):
            scenery.add(cur)
    out = {}
    for s in levels:
        ent = io.open(os.path.join(PROJ, "levels", s + ".ent"), encoding="utf-8").read()
        mp = io.open(os.path.join(PROJ, "levels", s + ".map"), encoding="utf-8").read()
        items = re.findall(r"^item (\S+)", ent, re.M)
        out[s] = {"monsters": len(re.findall(r"^monster ", ent, re.M)), "items": len(items),
                  "quest": sum(1 for i in items if i in quest),
                  "doors": len(re.findall(r"^door ", ent, re.M)),
                  "stairs": sum(1 for t in re.findall(r"^stairs (\S+)", mp, re.M) if t not in scenery)}
    return out


def dungeon_levels():
    out, cur = {}, None
    for l in io.open(os.path.join(PROJ, r"catalog\dungeons.cat"), encoding="utf-8"):
        h = re.match(r"\[(\S+)\]", l)
        if h:
            cur = h.group(1)
        m = re.match(r"levels\s*=\s*(.*)", l)
        if cur and m:
            out[cur] = m.group(1).split()
    return out


saved_settings = io.open(SETTINGS, "rb").read() if os.path.isfile(SETTINGS) else None
fresh()
try:
    log = run("docks.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    w = docks(sec.get("widths", []))
    if len(w) != 5:
        check(False, "five layout readings", str(len(w)))
    else:
        d0, d1, d2, d3, d4 = w
        grow = 400 - d0["left"]
        check(d1["left"] == 400 and grow != 0, "the palette dock takes the width it is dragged to",
              f"{d0['left']} -> {d1['left']}")
        check(d1["strip"] - d0["strip"] == grow and d1["gx"] - d0["gx"] == grow
              and d1["pw"] - d0["pw"] == grow,
              "the tool strip, the grid and the palette's body all move with its edge",
              f"strip {d0['strip']}->{d1['strip']} grid {d0['gx']}->{d1['gx']} palette {d0['pw']}->{d1['pw']}")
        check(d2["right"] == 350 and d2["gright"] == d2["panel"] - 350,
              "the key dock likewise, and the grid ends at its edge",
              f"right {d2['right']}, grid ends {d2['gright']} of {d2['panel']}")
        check(d3["left"] == 120, "too narrow clamps to the floor (120)", str(d3["left"]))
        check(d4["left"] == round(d4["panel"] * 0.30),
              "too wide clamps to 30% of the panel, so the grid stays the larger part",
              f"{d4['left']} of {d4['panel']}")
    truth = files_census()
    viewed = "eval_arena"  # project.ini's eval_level, where the harness opens
    dl = dungeon_levels()
    home = next((d for d, lv in dl.items() if viewed in lv), None)
    ov = {s: overview(sec.get("overview", []), s) for s in ("level", "dungeon", "world")}
    lvl, dun, wld = ov["level"], ov["dungeon"], ov["world"]

    def total(key, stems):
        return sum(truth[s][key] for s in stems)

    def bands_sum(o):
        return sum(int(x) for x in o.get("bands", "").split(",") if x)

    for scope, o, stems in (("level", lvl, [viewed]), ("dungeon", dun, dl.get(home, [])),
                            ("world", wld, list(truth))):
        want = {k: str(total(k, stems)) for k in ("monsters", "items", "quest", "doors", "stairs")}
        got = {k: o.get(k) for k in want}
        check(got == want and bands_sum(o) == total("monsters", stems),
              f"the {scope} counts are the files' (and every monster has a band)",
              f"got {got}, files {want}, bands {o.get('bands')}")
    check(wld.get("levels") == str(len(truth)) and dun.get("levels") == str(len(dl.get(home, []))),
          "the level counts are the manifest's and the dungeon's",
          f"world {wld.get('levels')}, dungeon {dun.get('levels')}")
    links = {k[len("dungeon:"):]: v for k, v in wld.items() if k.startswith("dungeon:")}
    check(set(links) == set(dl) and all(v.startswith(str(len(dl[d]))) for d, v in links.items()),
          "the world lists every dungeon with its level count", str(links))
    placed = overview(sec.get("placed", []), "level")
    placed_w = overview(sec.get("placed", []), "world")
    undone = overview(sec.get("undone", []), "level")
    check(placed.get("monsters") == str(int(lvl.get("monsters", -1)) + 1)
          and placed_w.get("monsters") == str(int(wld.get("monsters", -1)) + 1)
          and bands_sum(placed) == bands_sum(lvl) + 1,
          "a monster placed on the viewed level counts at once, there and in the world",
          f"{lvl.get('monsters')} -> {placed.get('monsters')}")
    check(undone == lvl, "undo takes it off the count again", str(undone))
    back = docks(console_sections(run("docks-persist.eval")).get("reopened", []))
    check(len(back) == 1 and back[0]["left"] == 300 and back[0]["right"] == 350,
          "a fresh start opens both docks at the widths they were left",
          str(back[0] if back else "no reading"))
finally:
    drop()
    if saved_settings is None:
        if os.path.isfile(SETTINGS):
            os.remove(SETTINGS)
    else:
        io.open(SETTINGS, "wb").write(saved_settings)

# --- phase 15: flags --------------------------------------------------------------
print("15 - flags: what waits on them, what sets them, the checker and the palette")

ARENA = os.path.join(PROJ, r"levels\eval_arena")
FIXTURE_FLAGS = """
[arena_gate]
display = Arena gate
dungeon = eval

[beacon_lit]
display = Beacon lit

[crypt_seal]
display = Crypt seal
dungeon = crypt

[orphan_flag]
display = Orphan
"""
FIXTURE_ENTS = [
    "door wooden_door 12 3 north flag=arena_gate",
    "button lever 5 4 north sets=arena_gate",
    "button lever 8 4 north flag=arena_gate",
    "button lever 10 4 north toggles=crypt_seal",
    "button lever 14 4 north flag=ghost_flag",
]
FIXTURE_STAIR = "stairs stairs_down 20 10 south dest=crypt2 destx=1 destz=1 flag=beacon_lit"


def write_fixture():
    def edit(path, fn):
        raw = io.open(path, "rb").read().decode("utf-8")
        eol = "\r\n" if "\r\n" in raw else "\n"
        io.open(path, "wb").write(eol.join(fn(raw.split(eol))).encode("utf-8"))

    def walled(lines):
        grid = [i for i, l in enumerate(lines) if l.startswith("#")]
        row = grid[3]  # z = 3: a wall across, its doorway at x = 12
        lines[row] = "#" * 12 + "." + "#" * (len(lines[row]) - 13)
        lines.insert(grid[0], FIXTURE_STAIR)
        return lines

    edit(ARENA + ".map", walled)
    edit(ARENA + ".ent", lambda lines: [l for l in lines if l] + FIXTURE_ENTS + [""])
    edit(os.path.join(PROJ, r"catalog\flags.cat"),
         lambda lines: lines + FIXTURE_FLAGS.strip("\n").split("\n") + [""])


FLAGLINE = re.compile(r"flag (\S+) (on|off) (\S+)")


def flag_states(lines):
    return {m.group(1): (m.group(2), m.group(3)) for m in (FLAGLINE.match(l) for l in lines) if m}


def palette_rows(lines):
    out = []
    for l in lines:
        m = re.match(r"editor palette item flags (\S+) band=\d group='([^']*)' ref=(\S+) goto=(\S+) "
                     r"label='([^']*)'", l)
        if m:
            out.append(m.groups())
    return out


fresh()
try:
    write_fixture()
    log = run("flags.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    start = flag_states(sec.get("start", []))
    check(start == {"arena_gate": ("off", "dungeon:eval"), "beacon_lit": ("off", "world"),
                    "crypt_seal": ("off", "dungeon:crypt"), "orphan_flag": ("off", "world"),
                    "relic_lifted": ("off", "world")},
          "every flag starts off, each with its scope", str(start))
    local = [l for l in sec.get("start", []) if l.startswith("flag ")][len(start):]
    check([l.split()[1] for l in local] == ["arena_gate"],
          "`flags dungeon` lists the party's dungeon's own flags alone", str(local))
    door = sec.get("door", [])
    want = ["door 12,3 -> shut", "button 8,4 -> off", "button 5,4 -> on",
            "flag arena_gate on dungeon:eval", "door 12,3 -> open", "button 8,4 -> on"]
    check([l for l in door if l.startswith(("door ", "button ", "flag "))] == want,
          "sealed and stuck until the lever sets the flag, then both give", str(door))
    item = flag_states(sec.get("item", []))
    lines = [l for l in sec.get("item", []) if l.startswith("flag ")]
    check(len(lines) == 2 and lines[0].startswith("flag relic_lifted off")
          and lines[1].startswith("flag relic_lifted on"),
          "lifting the relic turns its flag on", str(lines))
    saved = [l for l in sec.get("save", []) if l.startswith("flag ")]
    check(saved[-2:] == ["flag arena_gate on dungeon:eval", "flag relic_lifted on world"],
          "a load puts back the flags the save held", str(saved))
    issues = [l.strip() for l in sec.get("validate", [])]

    def found(sev, where, key, arg):
        return any(re.match(rf"{sev}\s+{re.escape(where)}\s*{key} {arg}$", l) for l in issues)

    check(found("ERR", "eval_arena @20,10", "map.check.flagwaits", "beacon_lit")
          and found("ERR", "eval_arena @14,4", "map.check.flagwaits", "ghost_flag"),
          "a stair and a lever waiting on flags nothing sets are errors where they stand")
    check(found("warn", "eval_arena @14,4", "map.check.flagunknown", "ghost_flag"),
          "a flag flags.cat lacks is named")
    check(found("warn", "eval_arena @10,4", "map.check.flagscope", "crypt_seal"),
          "the crypt's flag toggled from the Proving Ground is named")
    check(found("warn", "", "map.check.flagunused", "orphan_flag"),
          "a flag nothing sets or reads is named")
    check(not any(re.search(r"flag\w+ (arena_gate|relic_lifted)$", l) for l in issues),
          "the sound flags raise nothing (the gate's lever, the relic in the crypt)",
          str([l for l in issues if "flag" in l]))
    wire = [l for l in sec.get("wire", []) if l.startswith("flagwire ")]
    check(wire == ["flagwire door 12,3 flag=beacon_lit", "flagwire door 12,3 flag=arena_gate",
                   "flagwire lever 8,4 flag= clears=arena_gate",
                   "flagwire lever 8,4 flag=arena_gate op="],
          "the inspectors' setters rewire a door and a lever", str(wire))
    pal = sec.get("palette", [])
    wld, dun = overview(pal, "world"), overview(pal, "dungeon")
    check(wld.get("flags") == "3 (1 on)" and dun.get("flags") == "1 (1 on)",
          "the overview counts the world's flags and the dungeon's own, and how many are on",
          f"world {wld.get('flags')}, dungeon {dun.get('flags')}")
    # Listed twice: before `newtype flags` (N rows) and after it (N + 1).
    item_lines = [l for l in pal if l.startswith("editor palette item flags")]
    first = palette_rows(item_lines[:(len(item_lines) - 1) // 2])
    rows = {r[0]: r for r in palette_rows(item_lines)}
    groups = {r[0]: r[1] for r in first}
    check(groups.get("arena_gate", "").endswith("(this dungeon)")
          and all(groups.get(f) == "World" for f in ("beacon_lit", "orphan_flag", "relic_lifted"))
          and "crypt_seal" not in groups,
          "the section lists this dungeon's flags and the world's, not the crypt's", str(groups))
    relic = rows.get("sunken_relic")
    check(relic is not None and relic[2] == "items" and relic[3] == "crypt2@5,4"
          and "crypt2 5,4" in relic[4] and relic[1] == "World",
          "a quest item lists with where it lies, and a link there", str(relic))
    check("flag1" in rows and rows["flag1"][1].endswith("(this dungeon)") and "flag1" not in groups,
          "a new flag joins the viewed dungeon's own")
    used = [l for l in pal if l.startswith("editor palette used")]
    exp = ["armed=items:sunken_relic", "armed=-:", "typeeditor=arena_gate", "view=crypt2 sel=5,4"]
    check(len(used) == 4 and all(e in u for e, u in zip(exp, used)),
          "a quest item row arms its brush and again puts it down; a flag row opens its "
          "editor; the link goes to where the item lies", "\n         ".join(used))
    stair = sec.get("stair", [])
    maps = [l for l in stair if " map, start " in l]
    check(any(l.startswith("20,10 ") for l in stair) and len(maps) == 2
          and maps[0].startswith("28x24") and not maps[1].startswith("28x24"),
          "a stair waiting on a flag bars the way until it is on, then goes down",
          str([l for l in stair if " map, " in l or l.startswith("20,")]))
finally:
    drop()
    harness_game.remove_saves(SAVES.values())  # its save, gone as soon as it is done with

# --- phase 16: styles and the library ---------------------------------------------
print("16 - styles: the library, adding and saving, the monster lens, the rename sweeps")

STYLELINE = re.compile(r"style (\S+) (world|library)( current)? room=(\S+) corridor=(\S+) "
                       r"width=(\S+) monsters=(.*)")


def style_lines(lines):
    return {m.group(1): m.groups()[1:] for m in (STYLELINE.match(l) for l in lines) if m}


def palette_lens(lines, section):
    return {m.group(1): m.group(2) for m in
            (re.match(rf"editor palette item {section} (\S+) .* lens=(on|off)$", l) for l in lines) if m}


def palette_listings(lines, section):
    """Each consecutive run of `editor palette item <section>` lines, as one listing."""
    runs, cur = [], []
    for l in lines:
        if l.startswith(f"editor palette item {section} "):
            cur.append(l)
        elif cur:
            runs.append(cur)
            cur = []
    if cur:
        runs.append(cur)
    return runs


def drop_block(path, block_id):
    raw = io.open(path, "rb").read().decode("utf-8")
    eol = "\r\n" if "\r\n" in raw else "\n"
    out, skip = [], False
    for l in raw.split(eol):
        if l.strip().startswith("["):
            skip = l.strip() == f"[{block_id}]"
        if not skip:
            out.append(l)
    io.open(path, "wb").write(eol.join(out).encode("utf-8"))


def read(rel):
    return io.open(os.path.join(PROJ, rel), encoding="utf-8").read()


fresh()
# THE LIBRARY IS REAL: it has one home, so this phase changes it (an add reads
# it, a save writes it) behind a backup. back_up() never deletes a backup it
# finds - that one is a killed run's and the clean copy - and restore() writes
# the library back in place and only then discards it (renamed out of its name
# first, so a kill during the delete never leaves a partial "clean copy").
harness_game.back_up(LIBRARY, LIBBAK)
try:
    drop_block(os.path.join(PROJ, r"catalog\floors.cat"), "ground_soil_rocky")
    walls_before = io.open(os.path.join(PROJ, r"catalog\walls.cat"), "rb").read()

    def arena(ext, fn):
        p = ARENA + ext
        raw = io.open(p, "rb").read().decode("utf-8")
        eol = "\r\n" if "\r\n" in raw else "\n"
        io.open(p, "wb").write(eol.join(fn(raw.split(eol))).encode("utf-8"))

    arena(".ent", lambda ls: [l for l in ls if l] +
          ["button lever 5 1 north flag=relic_lifted sets=relic_lifted", ""])
    arena(".map", lambda ls: ls[:next(i for i, l in enumerate(ls) if l.startswith("#"))] +
          ["stairs stairs_down 20 10 south dest=crypt2 destx=1 destz=1 flag=relic_lifted"] +
          ls[next(i for i, l in enumerate(ls) if l.startswith("#")):])

    log = run("styles.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    listed = style_lines(sec.get("list", []))
    check({k: v[0] for k, v in listed.items()} ==
          {"small_crypt": "world", "dirt_tunnels": "library", "guard_barracks": "library",
           "marble_halls": "library"},
          "the world's style and the library's three are listed", str({k: v[0] for k, v in listed.items()}))
    groups = {m.group(1): (m.group(2), m.group(3)) for m in
              (re.match(r"editor palette item styles (\S+) .*group='([^']*)' ref=(\S+)", l)
               for l in sec.get("list", [])) if m}
    check(groups.get("small_crypt") == ("This world", "-")
          and all(groups.get(s, ("", ""))[1] == "library"
                  for s in ("dirt_tunnels", "guard_barracks", "marble_halls")),
          "the palette lists This world, then the library's", str(groups))
    lens = sec.get("lens", [])
    runs = [palette_lens(r, "monsters") for r in palette_listings(lens, "monsters")]
    crypt = {"skeleton", "skel_archer", "skel_coward", "mummy"}
    if len(runs) != 3:
        check(False, "three Monsters listings", str(len(runs)))
    else:
        check(all(v == "on" for v in runs[0].values()) and len(runs[0]) >= 10,
              "with no style armed every monster is on the lens")
        check({k for k, v in runs[1].items() if v == "on"} == crypt,
              "Small Crypt armed: its four monsters lead, the rest fall below the divider",
              str({k for k, v in runs[1].items() if v == "on"}))
        check(runs[2] == runs[0], "disarmed, the lens is the level's again")
    rows = [l for l in lens if l.startswith("style current")]
    check(rows[-2:] == ["style current small_crypt", "style current -"],
          "clicking the world style's row arms it, and again puts it down", str(rows))
    add = [l for l in sec.get("add", []) if l.startswith("style add")]
    check(len(add) == 2 and add[0] == "style add dirt_tunnels: added copied=floors:ground_soil_rocky,"
          "themes:dirt_cave,themes:dirt_tunnel,styles:dirt_tunnels missing=-",
          "adding Dirt Tunnels copies its two themes and the one floor the world lacked",
          add[0] if add else "(none)")
    check(len(add) == 2 and add[1].startswith("style add dirt_tunnels: already copied=-"),
          "a second add is a no-op", add[1] if len(add) > 1 else "(none)")
    check(style_lines(sec.get("add", [])).get("dirt_tunnels", ("",))[0] == "world",
          "and it lists as the world's own now")
    check("[ground_soil_rocky]" in read(r"catalog\floors.cat")
          and io.open(os.path.join(PROJ, r"catalog\walls.cat"), "rb").read() == walls_before,
          "the floor reached floors.cat, and walls.cat (which had wall_rock) is untouched")
    saved = [l for l in sec.get("save", []) if l.startswith("style save")]
    lib = io.open(os.path.join(LIBRARY, "styles.cat"), encoding="utf-8").read()
    check(saved == ["style save style1: saved copied=walls:wall_stone_30,floors:floor_ancient_stone,"
                    "ceilings:ceiling_stone,themes:crypt_chamber,styles:style1"]
          and "[style1]" in lib and "monsters = skeleton 2, ghoul" in lib,
          "a world style saves to the library with its theme and the theme's surfaces",
          str(saved))
    issues = [l.strip() for l in sec.get("save", [])]
    check(any(re.match(r"warn\s+map\.check\.stylenomonster style1$", l) for l in issues),
          "the checker names the monster the style lists and the world lacks")
    renamed = style_lines(sec.get("rename", []))
    check(renamed.get("small_crypt", ("",) * 3)[2] == "crypt_room"
          and renamed.get("style1", ("",) * 3)[2] == "crypt_room",
          "renaming a theme rewrites the styles that name it",
          str({k: v[2] for k, v in renamed.items()}))
    ent, mp = read(r"levels\eval_arena.ent"), read(r"levels\eval_arena.map")
    items = read(r"catalog\items.cat")
    check("flag=relic_taken sets=relic_taken" in ent and "flag=relic_taken" in mp
          and "flag = relic_taken=1" in items and "[relic_taken]" in read(r"catalog\flags.cat"),
          "renaming a flag rewrites the lever, the stair and the item that name it")
finally:
    drop()
    harness_game.restore(LIBBAK, LIBRARY)

# --- phase 17: the shape brushes ---------------------------------------------------
print("17 - shape brushes: corridor, room, stamp and region, in the current style")

SHAPELINE = re.compile(r"editor shape (\w+): squares=(\d+) solid=(\d+) opened=(\d+) "
                       r"raised=(\d+) painted=(\d+)")
WALKABLE = re.compile(r"(\d+)x(\d+) map, start (\d+),(\d+), (\d+) walkable")


def shape_lines(lines):
    return [tuple(int(x) if x.isdigit() else x for x in m.groups())
            for m in (SHAPELINE.match(l) for l in lines) if m]


def walkables(lines):
    return [int(m.group(5)) for m in (WALKABLE.match(l) for l in lines) if m]


def read_level(stem):
    """(grid rows, {(surface, x, z): theme}) of a saved level."""
    text = io.open(os.path.join(PROJ, "levels", stem + ".map"), encoding="utf-8").read()
    rows = [l for l in text.splitlines() if l and l[0] in "#.PDTF"]
    themes = {}
    for m in re.finditer(r"^theme (\w+) (\d+) (\d+) (\S+)", text, re.M):
        themes[(m.group(1), int(m.group(2)), int(m.group(3)))] = m.group(4)
    return rows, themes


fresh()
try:
    # The fixture: rock everywhere but a 5x5 room round the start (14,12).
    def rocky(lines):
        grid = [i for i, l in enumerate(lines) if l and l[0] in "#.P"]
        for i, z in zip(grid, range(len(grid))):
            row = ["#"] * len(lines[i])
            if 10 <= z <= 14:
                for x in range(12, 17):
                    row[x] = "."
            if z == 12:
                row[14] = "P"
            lines[i] = "".join(row)
        return lines

    for ext, fn in ((".map", rocky), (".ent", lambda ls: ["; eval_arena - emptied by EditorTest 17", ""])):
        p = ARENA + ext
        raw = io.open(p, "rb").read().decode("utf-8")
        eol = "\r\n" if "\r\n" in raw else "\n"
        io.open(p, "wb").write(eol.join(fn(raw.split(eol))).encode("utf-8"))

    log = run("shapes.eval")
    check(passed(log), "the script ran clean")
    sec = console_sections(log)
    base = walkables(sec.get("base", []))
    cor = walkables(sec.get("corridor", []))
    shapes = shape_lines([l.split("console: ", 1)[1] for l in log.splitlines()
                          if "console: editor shape" in l])
    kinds = [s[0] for s in shapes]
    check(base == [25], "the fixture is the 5x5 room and rock", str(base))
    c = shapes[0] if shapes else None
    check(c is not None and c[0] == "corridor" and len(cor) == 3 and cor[0] == 25 + c[3]
          and c[3] > 0 and c[5] > c[3],
          "a corridor opens the rock it crosses and paints it and its walls",
          f"{c}, walkable {cor}")
    hashes_seen = [h for h in hashes(log)]
    check(len(cor) == 3 and cor[1] == 25 and cor[2] == cor[0]
          and len(hashes_seen) >= 2 and hashes_seen[0] == hashes_seen[1],
          "undo puts the rock back exactly (geometry hash), redo lays it again",
          f"walkable {cor}")
    rows, themes = read_level("eval_arena")
    open_ = {(x, z) for z, r in enumerate(rows) for x, ch in enumerate(r) if ch != "#"}

    def reach(start):
        seen, todo = {start}, [start]
        while todo:
            x, z = todo.pop()
            for n in ((x + 1, z), (x - 1, z), (x, z + 1), (x, z - 1)):
                if n in open_ and n not in seen:
                    seen.add(n)
                    todo.append(n)
        return seen

    from_start = reach((14, 12))
    check((3, 3) in from_start, "the corridor joins the room to its far end")
    check(themes.get(("floor", 3, 3)) == "crypt_passage"
          and any(themes.get(("wall", x, z)) == "crypt_passage"
                  for x, z in ((2, 3), (3, 2), (2, 2), (4, 2))),
          "its floor and the walls along it wear the style's corridor theme",
          str({k: v for k, v in themes.items() if k[1] <= 4 and k[2] <= 4}))
    r = shapes[1] if len(shapes) > 1 else None
    room = {(x, z) for x in range(18, 25) for z in range(3, 9)}
    check(r is not None and r[0] == "room" and r[1] == 42 and room <= open_
          and themes.get(("floor", 18, 3)) == "crypt_chamber",
          "a room opens its rectangle in the style's room theme", str(r))
    s = shapes[2] if len(shapes) > 2 else None
    check(s is not None and s[0] == "stamp" and s[2] == 6 and s[4] == 2
          and (14, 10) not in open_ and (14, 13) not in open_ and (14, 12) in open_,
          "a stamp over the start room raises its two pillars there - never on the party",
          str(s))
    g = shapes[3] if len(shapes) > 3 else None
    region = {(x, z) for x in range(12, 23) for z in range(15, 23)} & open_
    check(g is not None and g[0] == "region" and g[3] > 20 and region and region <= from_start,
          "a region generates rooms in its box, joined to the room touching it",
          f"{g}, {len(region)} open, {len(region - from_start)} unreached")
    pl = shapes[4] if len(shapes) > 4 else None
    check(pl is not None and pl[0] == "room" and pl[3] == 9 and pl[5] == 0
          and ("floor", 25, 13) not in themes,
          "with no style a room is plain carving: opened, nothing painted", str(pl))
    sm = shapes[5] if len(shapes) > 5 else None
    check(sm is not None and sm[0] == "region" and sm[1] == 0 and sm[3] == 0,
          "a region under 6x6 generates nothing", str(sm))
finally:
    drop()

# --- phase 18: the workflow, wired through ------------------------------------------
print("18 - the four stages as one path: a styled world, add, build, populate, overview")
P7 = ("p7_world", "p7_wiz")
DIRT = {"centipede", "giant_spider", "blob"}
MARBLE = {"skel_mage", "mummy", "skel_warrior", "skel_berserker"}


def world_file(world, rel):
    p = os.path.join(PROJECTS, world, rel)
    return io.open(p, encoding="utf-8").read() if os.path.isfile(p) else ""


def theme_ids(text):
    return {m.group(1) for m in re.finditer(r"^theme \w+ \d+ \d+ (\S+)", text, re.M)}


def monsters_in(text):
    return [m.group(1) for m in re.finditer(r"^monster (\S+)", text, re.M)]


# The dialog's Create and Populate PERSIST their knobs (settings.ini gen_knobs),
# and other suites' scripts inherit the knobs they leave unset - so the style's
# recipe this phase uses must not outlive it.
settings_before = io.open(SETTINGS, "rb").read() if os.path.isfile(SETTINGS) else None
fresh()
try:
    for w in P7:
        harness_game.remove_world(ROOT, w)
    log = run("workflow_worlds.eval")
    check(passed(log), "the worlds script ran clean")
    # A BLANK world in a library style: the style and what it names arrive, the
    # starter dungeon names it, and the first room wears it - tags included.
    check("[dirt_tunnels]" in world_file("p7_world", r"catalog\styles.cat")
          and "[dirt_cave]" in world_file("p7_world", r"catalog\themes.cat")
          and "[dirt_tunnel]" in world_file("p7_world", r"catalog\themes.cat"),
          "a blank world made in Dirt Tunnels receives the style and its two themes")
    check(re.search(r"^style = dirt_tunnels\s*$", world_file("p7_world", r"catalog\dungeons.cat"), re.M)
          is not None, "its starter dungeon names the style as its default")
    room1 = world_file("p7_world", r"levels\room1.map")
    check(re.search(r"^tags cave vermin ooze\s*$", room1, re.M) is not None
          and re.search(r"^theme floor 8 8 dirt_cave\s*$", room1, re.M) is not None
          and re.search(r"^theme wall 6 6 dirt_cave\s*$", room1, re.M) is not None
          and re.search(r"^palette floor .*ground_soil_rocky", room1, re.M) is not None,
          "the first room wears the room theme, floor and walls, with the style's tags")
    # The WIZARD in a style: its recipe, tags, monsters and both themes.
    wiz_map, wiz_ent = world_file("p7_wiz", r"levels\floor1.map"), world_file("p7_wiz", r"levels\floor1.ent")
    wiz_monsters = set(monsters_in(wiz_ent))
    check(theme_ids(wiz_map) == {"marble_hall", "marble_gallery"}
          and re.search(r"^tags undead stone\s*$", wiz_map, re.M) is not None,
          "a wizard floor in Marble Halls lays its room and corridor themes, with its tags",
          str(theme_ids(wiz_map)))
    check(bool(wiz_monsters) and wiz_monsters <= MARBLE,
          "...and draws its monsters from the style's list", str(sorted(wiz_monsters)))
    for w in P7:
        wlog = run("worldcheck.eval", project=w)
        check("validate: clean" in wlog, f"{w} opens and passes the checker",
              next((l for l in wlog.splitlines() if "validate" in l), "no validate line"))

    # THE WALK, inside p7_world.
    log = run("workflow_walk.eval", project="p7_world")
    check(passed(log), "the walk ran clean")
    sec = console_sections(log)
    add = sec.get("add", [])
    opened = next((l for l in add if l.startswith("generate dialog: create")), "")
    check("style=dirt_tunnels" in opened and "winding:0.85" in opened,
          "[+] opens on the dungeon's style, its shape knobs loaded", opened[:120])
    check("generate dialog: made keep1" in add, "Create makes the floor", " | ".join(add[:4]))
    check(any(l.startswith("editor palette: stage build ") for l in add)
          and any(re.match(r"style dirt_tunnels world current ", l) for l in add),
          "...and lands in the Build stage with the style armed (it was on Creatures before)")
    keep1_map, keep1_ent = world_file("p7_world", r"levels\keep1.map"), world_file("p7_world", r"levels\keep1.ent")
    check(theme_ids(keep1_map) == {"dirt_cave", "dirt_tunnel"}
          and re.search(r"^tags cave vermin ooze\s*$", keep1_map, re.M) is not None,
          "the generated floor wears both themes and carries the style's tags - no Level settings visit",
          str(theme_ids(keep1_map)))
    check(set(monsters_in(keep1_ent)) <= DIRT and monsters_in(keep1_ent),
          "...and its monsters are the style's", str(sorted(set(monsters_in(keep1_ent)))))
    empty = sec.get("empty", [])
    check("generate dialog: made keep2" in empty
          and any(l.startswith("editor palette: stage build ") for l in empty),
          "Empty makes the box in the style, landing in Build")

    def ov(lines, key):
        return next((l.split(f"editor overview level {key} ", 1)[1] for l in lines
                     if l.startswith(f"editor overview level {key} ")), None)

    def followed(lines):
        return next((l.split(" -> ", 1)[1].split(" ", 1)[0] for l in lines
                     if l.startswith("editor overview follow next")), None)

    check(ov(empty, "squares") == "9" and followed(empty) == "stage:build",
          "an empty floor's next step is Build (and its link opens that stage)",
          f"squares {ov(empty, 'squares')}, next -> {followed(empty)}")
    build = sec.get("build", [])
    shapes = shape_lines(build)
    check([s[0] for s in shapes] == ["room", "corridor", "stamp"] and all(s[3] > 0 and s[5] > 0 for s in shapes),
          "room, corridor and stamp each open rock and paint it", str(shapes))
    check(int(ov(build, "squares") or 0) > 9 and followed(build) == "populate",
          "built, the next step is Populate", f"squares {ov(build, 'squares')}, next -> {followed(build)}")
    pop = sec.get("populate", [])
    keep2_ent = world_file("p7_world", r"levels\keep2.ent")
    placed = monsters_in(keep2_ent)
    runs = [tuple(int(x) for x in m.groups()) for m in
            re.finditer(r"populate keep2: (\d+) monsters, (\d+) loot, (\d+) replaced", log)]
    check(len(runs) == 2, "the dialog's Populate and the console's both populate the viewed floor",
          str(runs))
    check(len(runs) == 2 and runs[0][0] > 0 and runs[1][2] == runs[0][0] + runs[0][1]
          and len(placed) == runs[1][0],
          "populating again replaces what the first populate placed: the file holds the second's alone",
          f"{runs}, file {len(placed)}")
    check(bool(placed) and set(placed) <= DIRT,
          "the hand-built floor is populated from the style's list", str(placed))
    check(ov(pop, "monsters") == str(len(placed)),
          "the overview counts what the file holds", f"{ov(pop, 'monsters')} vs {len(placed)}")
    check(followed(pop) in ("-", "check"),
          "populated, the next step is the check or play", str(followed(pop)))
    keep2_map = world_file("p7_world", r"levels\keep2.map")
    check(re.search(r"^tags cave vermin ooze\s*$", keep2_map, re.M) is not None
          and "dirt_cave" in theme_ids(keep2_map),
          "the empty floor carries the style's tags and room theme too")
finally:
    for w in P7:
        harness_game.remove_world(ROOT, w)
    drop()
    if settings_before is not None:
        io.open(SETTINGS, "wb").write(settings_before)
    elif os.path.isfile(SETTINGS):
        os.remove(SETTINGS)

# --- the real tree --------------------------------------------------------------
print("the real tree: dungeon-demo and the library as the run found them")
cleanup()
real.check(check)

print()
print("PASS" if failures == 0 else f"FAIL - {failures} check(s) failed")
sys.exit(0 if failures == 0 else 1)
