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
#      one group finds matches in the others and drops sections with none; and
#      the grouping survives a restart through settings.ini.
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

# Muted for the whole run (tools/harness_audio.py). The phases are flat, not
# one try block, so the restore rides atexit - which also runs after sys.exit,
# an uncaught exception and Ctrl+C.
import atexit
import harness_audio
atexit.register(harness_audio.restore, harness_audio.mute(os.path.dirname(EXE)))

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


backup = os.path.join(ROOT, r"build\editortest-backup")
shutil.rmtree(backup, ignore_errors=True)
shutil.copytree(PROJ, backup)
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
    shutil.rmtree(PROJ)
    shutil.copytree(backup, PROJ)
    shutil.rmtree(backup, ignore_errors=True)

# --- phase 7: painting with a theme -------------------------------------------
print("7 - a theme paints a whole look, by reference")
CELL = re.compile(r"console: editor cell (\S+) (\d+),(\d+) (\w+) wall=(\S+)/(\S+) "
                  r"floor=(\S+)/(\S+) ceiling=(\S+)/(\S+)")
backup = os.path.join(ROOT, r"build\editortest-backup")
shutil.rmtree(backup, ignore_errors=True)
shutil.copytree(PROJ, backup)
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
    shutil.rmtree(PROJ)
    shutil.copytree(backup, PROJ)
    shutil.rmtree(backup, ignore_errors=True)

# --- phase 8: editing a theme -------------------------------------------------
print("8 - editing a theme repaints every square that uses it")
backup = os.path.join(ROOT, r"build\editortest-backup")
shutil.rmtree(backup, ignore_errors=True)
shutil.copytree(PROJ, backup)
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
    shutil.rmtree(PROJ)
    shutil.copytree(backup, PROJ)
    shutil.rmtree(backup, ignore_errors=True)

# --- phase 9: making a world ----------------------------------------------------
print("9 - a new world three ways: blank, this world whole, one level")
PROJECTS = os.path.join(ROOT, r"assets\projects")
MADE = ("nw_blank", "nw_copy", "nw_level", "nw_bad")
LEFTOVER = os.path.join(PROJECTS, ".building-nw_ghost")
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
    # (variant records on those squares) and dungeon-demo's must not have moved.
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
    for w in MADE:
        shutil.rmtree(os.path.join(PROJECTS, w), ignore_errors=True)
    shutil.rmtree(LEFTOVER, ignore_errors=True)

# --- phase 10: the New world dialog ---------------------------------------------
print("10 - the New world dialog makes a world, and hands it to the Worlds list")
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
        shutil.rmtree(os.path.join(PROJECTS, w), ignore_errors=True)

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
        shutil.rmtree(os.path.join(PROJECTS, w), ignore_errors=True)

# --- phase 12: the palette's category bar -------------------------------------
print("12 - the category bar shows one group, both ways, and the filter sees past it")
# The groupings AS DESIGNED (docs/tool-refinement-plan.md Phase 1). Stated here
# rather than read back from the game, so a section moved to the wrong group is
# a failure and not a new truth.
STAGE = {"world": ["dungeons", "quests", "terrain"],
         "build": ["themes", "walls", "floors", "ceilings", "wallfeatures",
                   "surfacefeatures", "doors", "stairs"],
         "populate": ["monsters", "items", "weapons", "armor", "decorations",
                      "fixtures", "buttons"]}
KIND = {"surfaces": ["themes", "walls", "floors", "ceilings", "wallfeatures",
                     "surfacefeatures"],
        "structure": ["doors", "stairs", "buttons", "fixtures"],
        "props": ["decorations"],
        "creatures": ["monsters"],
        "items": ["items", "weapons", "armor"],
        "world": ["dungeons", "quests", "terrain"]}
SHOWS = re.compile(r"editor palette: (\w+) (\w+) filter='([^']*)' shows:(.*)")


def shown(line):
    """(grouping, group, filter, [(section, rows)]) from one palette line."""
    m = SHOWS.search(line)
    if not m:
        return None
    secs = [(s, int(n)) for s, n in re.findall(r"(\w+)\((\d+)\)", m.group(4))]
    return m.group(1), m.group(2), m.group(3), secs


# The bar's state lives in settings.ini beside the exe, so this phase puts the
# developer's copy back afterwards like every project file.
SETTINGS = os.path.join(os.path.dirname(EXE), "settings.ini")
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
    check(tables.get("stage") == STAGE, "the stage groups are World / Build / Populate as designed",
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
    check(counts.get("dungeons", 0) == 2 and counts.get("quests", 0) == 1
          and counts.get("terrain", 0) == 7,
          "the world sections list their entries (2 dungeons, 1 quest, 7 terrains)", str(world))
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
    ini = io.open(SETTINGS, encoding="utf-8").read() if os.path.isfile(SETTINGS) else ""
    check("map_palette_group=1" in ini and "map_palette_kind=4" in ini,
          "the grouping and its group are saved to settings.ini")
    log2 = run("palette-persist.eval")
    back = [shown(l) for l in sections(log2).get("reopened", []) if shown(l)]
    check(len(back) == 1 and back[0][:2] == ("kind", "items"),
          "a fresh start opens the palette where it was left", str(back))
finally:
    if saved_settings is None:
        if os.path.isfile(SETTINGS):
            os.remove(SETTINGS)
    else:
        io.open(SETTINGS, "wb").write(saved_settings)

print()
print("PASS" if failures == 0 else f"FAIL - {failures} check(s) failed")
sys.exit(0 if failures == 0 else 1)
