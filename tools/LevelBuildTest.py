# tools/LevelBuildTest.py - the level-building thread's checks (docs/level-building.md).
#
# Run:  python tools\LevelBuildTest.py [phase ...]   (needs a debug build;
#       no phases = all seven)
#
# The eval runner's own verdict only says every line matched a command, so it
# reads PASS on a run that stranded every level it made. This reads what the
# run PRODUCED - the checker's answer and the level files - and judges that.
#
#   1. CREATE AND REROLL - in a scratch copy of the demo world, make three new
#      crypt floors (generated, generated, empty) and reroll one twice, the
#      second time into a map too small for its stairs. Demand: every floor in
#      the crypt, the checker clean, and every stair on disk PAIRED - its
#      destination holds a stair leading straight back to the same square.
#   2. THE SHAPE IS COUNTS (P2) - generate with the path and branch knobs set,
#      then MEASURE each level from its .map, not from anything the generator
#      says: rooms are the floor that lies in 2x2 blocks (corridors are one
#      square wide), branches come from the dead ends. The measurement must
#      match the knobs AND the generator's own report, so a report that lies
#      fails as loudly as a shape that is wrong. The control pair differs only
#      in `branches` (0 vs 4); the shortfall case asks for far more than a
#      16x16 map holds and must stop short and say so.
#   3. COMPLEXITY IS FOUR KNOBS (P3) - one base level with every complexity
#      knob at zero, and four variants each raising ONE of them: its own
#      measured count must rise from zero and no other may move. Loops are the
#      link graph's cycles, dead ends the corridors touching one room, winding
#      the links that are not a single row or column, irregular the rooms that
#      do not fill their bounding box. And a level with loops AND locks: every
#      door, shut on its own, must still strand floor - the proof no loop was
#      built round a lock.
#   4. DIFFICULTY IS STRENGTH (P4) - each level's monsters are joined to the
#      game's own `threat` table (printed, never copied). Control pairs one knob
#      apart: difficulty 0.2 -> 0.8 gives stronger and about as many (easy is
#      never empty); density 0.2 -> 0.8 gives many more and no stronger; ramp
#      0 -> 1 makes the far half out-threaten the near half by more; boss off ->
#      on puts the pool's strongest kind, absent otherwise at that difficulty, in
#      exactly one dead-end room. On every level: nothing in the start room or
#      within three steps of the arrival square. And the table itself: exactly
#      the ranged kinds (by the catalog FILE's archetype) are scored with a shot,
#      and a caster's shot is its spell - 3x+ its melee.
#   5. THE RECIPE (P4b) - room sizes 3..3 vs 8..10 on one seed, every room
#      measured inside its range; theme ooze vs undead, every monster carrying
#      that tag in monsters.cat and the level recording its theme; palette
#      copied from crypt2 (made DISTINCT first, since every demo level shares
#      one) and not from the default; presets loaded, built with, saved (no
#      seed in the recipe), listed and deleted, read back from genpresets.cat.
#   6. PLAY IT (P5) - generate, then play: the party must stand on the level's
#      start square AS THE FILE HAS IT, on that level (mapinfo's size and
#      walkable count against the file's grid), still in play (an in-game
#      command answers), and an unknown level is refused without moving it. A
#      second run - its script written here, since the square to wander to comes
#      from the first - plays the level the party is ALREADY on, from somewhere
#      else on it, and must bring it back to the start.
#   7. EVERY WAY IN SURVIVES A REROLL - the scratch world's opening moves to a
#      square of crypt1 no stair uses, and a doorway is added landing on one of
#      crypt2's. Each level is rerolled three times into a small map; the checker
#      (its arrivalblocked rule) must stay clean after every one, the doorway's
#      square must be floor joined to the level, and crypt1, with no floor above,
#      must START on the opening. MUTATION: with the arrivals ignored, four
#      checks fail - but only once the doorway has no stair under it, since the
#      demo's own doorways land on exit stairs a reroll already kept.
#
# Checked by mutation (2026-09-24): with the reroll's stairs dropped, the
# checker reported 5 errors (stairblocked, stairunpaired, three levellost) and
# phase 1 fails. With the link sought AFTER both layouts existed (the first
# version), all three new floors came back unreachable.
import io
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, r"build\debug\bin\Dungeon.exe")
LOG = os.path.join(ROOT, r"build\debug\bin\dungeon.log")
SCRIPTS = os.path.join(ROOT, r"tools\EvalScripts")
DEMO = os.path.join(ROOT, r"assets\projects\dungeon-demo")

failures = 0


def check(ok, label, detail=""):
    global failures
    print(f"  {'[ok  ]' if ok else '[FAIL]'} {label}")
    if not ok:
        failures += 1
        if detail:
            print(f"         {detail}")


def run(script, project):
    args = [EXE, "-headless", "-project", project, "-eval", os.path.join(SCRIPTS, script)]
    try:
        code = subprocess.run(args, cwd=ROOT, capture_output=True, timeout=600).returncode
    except subprocess.TimeoutExpired:
        # A debug assert parks the game on a CRT dialog until the timeout (the
        # process is killed on the way out). Report it as a failed run, with
        # the FATAL line from the log, rather than dying with a traceback.
        code = -1
    log = io.open(LOG, encoding="utf-8", errors="replace").read()
    for line in log.splitlines():
        if "FATAL" in line:
            print(f"         game FATAL: {line.split('FATAL', 1)[1][:160]}")
    return code, [l.split("console: ", 1)[1] for l in log.splitlines()
                  if "console: " in l]


def scratch(name):
    path = os.path.join(ROOT, "assets", "projects", name)
    shutil.rmtree(path, ignore_errors=True)
    shutil.copytree(DEMO, path)
    return path


def stairs_of(levels_dir, stem):
    out = []
    p = os.path.join(levels_dir, stem + ".map")
    for line in io.open(p, encoding="utf-8").read().splitlines():
        m = re.match(r"stairs (\S+) (\d+) (\d+) \S+ dest=(\S+) destx=(\d+) destz=(\d+)", line)
        if m:
            out.append((m.group(1), int(m.group(2)), int(m.group(3)), m.group(4),
                        int(m.group(5)), int(m.group(6))))
    return out


def grid_of(levels_dir, stem):
    """The .map's grid rows (every other line is a record or a comment)."""
    p = os.path.join(levels_dir, stem + ".map")
    return [l for l in io.open(p, encoding="utf-8").read().splitlines()
            if l and l[0] in "#.PDTF" and not l.startswith(";")]


def measure(rows):
    """Rooms, branches and tree-ness of a generated grid, from the grid alone.

    A ROOM square is floor lying in some 2x2 block of floor: rooms are at least
    3x3, and the generator keeps corridors one square wide and a square clear of
    everything but their two ends, so no corridor square is ever in such a block.
    Rooms are the connected groups of room squares; corridors the rest. The
    room graph is a tree, and each branch ends in exactly one dead end, as does
    the main path (the exit is never a branch's anchor) - so branches are the
    dead ends, less the exit's, less the start room's when it has only the path.
    """
    floor = {(x, z) for z, row in enumerate(rows) for x, ch in enumerate(row) if ch != "#"}
    start = next(((x, z) for z, row in enumerate(rows) for x, ch in enumerate(row)
                  if ch == "P"), None)

    def in_block(x, z):
        return any(all((x + dx + i, z + dz + j) in floor for i in (0, 1) for j in (0, 1))
                   for dx in (-1, 0) for dz in (-1, 0))

    room_sq = {c for c in floor if in_block(*c)}

    def components(cells):
        label, n = {}, 0
        for c in cells:
            if c in label:
                continue
            stack = [c]
            label[c] = n
            while stack:
                x, z = stack.pop()
                for nb in ((x + 1, z), (x - 1, z), (x, z + 1), (x, z - 1)):
                    if nb in cells and nb not in label:
                        label[nb] = n
                        stack.append(nb)
            n += 1
        return label, n

    room_of, rooms = components(room_sq)
    corr_of, corridors = components(floor - room_sq)
    touches = [set() for _ in range(corridors)]
    cells = [[] for _ in range(corridors)]
    for (x, z), k in corr_of.items():
        cells[k].append((x, z))
        for nb in ((x + 1, z), (x - 1, z), (x, z + 1), (x, z - 1)):
            if nb in room_of:
                touches[k].add(room_of[nb])
    # A corridor touching TWO rooms is a link; ONE, a dead end (P3). Anything
    # else - none, or three - is not something the generator makes.
    links = [k for k in range(corridors) if len(touches[k]) == 2]
    stubs = [k for k in range(corridors) if len(touches[k]) == 1]
    stray = corridors - len(links) - len(stubs)
    degree = [0] * rooms
    for k in links:
        for r in touches[k]:
            degree[r] += 1
    # The link graph is connected, so its CYCLES are links beyond a spanning
    # tree's rooms - 1: exactly the loops.
    loops = len(links) - (rooms - 1)
    leaves = sum(1 for d in degree if d == 1)
    start_room = room_of.get(start)
    # Branches from the dead-end rooms only mean anything on a tree; with loops
    # a branch's end can be joined back, so it is not measured there.
    branches = None
    if loops == 0:
        branches = 0
        if rooms > 1 and start_room is not None:
            branches = leaves - 1 - (1 if degree[start_room] == 1 else 0)
    # A link that is not a single row or column JOGS (winding).
    wound = sum(1 for k in links if len({x for x, _ in cells[k]}) > 1 and
                len({z for _, z in cells[k]}) > 1)
    # A room whose floor does not fill its bounding box is irregular: an L, a
    # cross, or a hall with pillars (holes).
    irregular = 0
    by_room = {}
    for c, r in room_of.items():
        by_room.setdefault(r, []).append(c)
    for sq in by_room.values():
        xs = [x for x, _ in sq]
        zs = [z for _, z in sq]
        if len(sq) != (max(xs) - min(xs) + 1) * (max(zs) - min(zs) + 1):
            irregular += 1
    adj = [set() for _ in range(rooms)]
    for k in links:
        a, b = sorted(touches[k])
        adj[a].add(b)
        adj[b].add(a)
    return {"rooms": rooms, "branches": branches, "tree": loops == 0 and stray == 0,
            "loops": loops, "deadends": len(stubs), "wound": wound,
            "irregular": irregular, "stray": stray, "floor": floor, "start": start,
            "room_of": room_of, "adj": adj, "start_room": start_room}


def doors_still_shut_something(levels_dir, stem, floor, start):
    """Every door, shut on its own, must strand floor the start cannot reach -
    the proof that no loop was built round it. Returns the doors that do not."""
    ent = os.path.join(levels_dir, stem + ".ent")
    doors = []
    for line in io.open(ent, encoding="utf-8").read().splitlines():
        m = re.match(r"door \S+ (\d+) (\d+)", line)
        if m:
            doors.append((int(m.group(1)), int(m.group(2))))
    bad = []
    for d in doors:
        seen, stack = {start}, [start]
        while stack:
            x, z = stack.pop()
            for nb in ((x + 1, z), (x - 1, z), (x, z + 1), (x, z - 1)):
                if nb in floor and nb != d and nb not in seen:
                    seen.add(nb)
                    stack.append(nb)
        if len(floor) - len(seen) - 1 <= 0:  # nothing but the door itself unreached
            bad.append(d)
    return doors, bad


REPORT = re.compile(
    r"generate: built path (?P<pg>\d+)/(?P<pw>\d+) rooms, branches (?P<bg>\d+)/(?P<bw>\d+) "
    r"\(rooms: (?P<br>[\d -]+)\), loops (?P<lg>\d+)/(?P<lw>\d+), "
    r"dead ends (?P<dg>\d+)/(?P<dw>\d+), winding (?P<wg>\d+)/(?P<wc>\d+) corridors, "
    r"irregular (?P<ir>\d+) rooms, locks (?P<kg>\d+)/(?P<kw>\d+)")


def parse_runs(con):
    """Each `generate: wrote <stem>` paired with the `built ...` line after it."""
    runs = []
    for i, l in enumerate(con):
        m = re.match(r"generate: wrote (\S+) ", l)
        if m and i + 1 < len(con) and (r := REPORT.match(con[i + 1])):
            g = {k: v for k, v in r.groupdict().items()}
            n = lambda k: int(g[k])
            runs.append({"stem": m.group(1), "path": (n("pg"), n("pw")),
                         "branches": (n("bg"), n("bw")),
                         "branch_rooms": [int(x) for x in g["br"].split() if x != "-"],
                         "loops": (n("lg"), n("lw")), "deadends": (n("dg"), n("dw")),
                         "wound": n("wg"), "corridors": n("wc"), "irregular": n("ir"),
                         "locks": (n("kg"), n("kw"))})
            t = re.search(r"threat ([\d.]+)-([\d.]+), boss (yes|no)", con[i + 1])
            runs[-1]["threat"] = (float(t.group(1)), float(t.group(2))) if t else (0.0, 0.0)
            runs[-1]["boss"] = bool(t) and t.group(3) == "yes"
    return runs


def show(m):
    """A measurement without the raw floor set, for a failure's detail line."""
    return {k: v for k, v in m.items()
            if k not in ("floor", "start", "room_of", "adj", "start_room")}


def monsters_of(levels_dir, stem):
    """(type, x, z) of every monster record in a level's .ent."""
    out = []
    p = os.path.join(levels_dir, stem + ".ent")
    for line in io.open(p, encoding="utf-8").read().splitlines():
        m = re.match(r"monster (\S+) (\d+) (\d+)", line)
        if m:
            out.append((m.group(1), int(m.group(2)), int(m.group(3))))
    return out


def room_depths(m):
    """Tree steps from the start room to every room (BFS over the links)."""
    depth = {m["start_room"]: 0}
    queue = [m["start_room"]]
    while queue:
        r = queue.pop(0)
        for n in m["adj"][r]:
            if n not in depth:
                depth[n] = depth[r] + 1
                queue.append(n)
    return depth


def phase_wanted(n):
    """Phases named on the command line, or all of them."""
    picked = {int(a) for a in sys.argv[1:] if a.isdigit()}
    return not picked or n in picked


def main():
    if not os.path.isfile(EXE):
        print(f"no debug build at {EXE}")
        return 2

    if phase_wanted(1):
        print("1 - new floors join their dungeon, stairs to the floor above, and survive a reroll")
        proj = scratch("lb_create")
        try:
            code, con = run("levelcreate.eval", "lb_create")
            check(code == 0, "the script ran to the end", f"exit {code}")
            # The LAST listing: the script prints one before creating anything too.
            crypt = next((l for l in reversed(con) if l.strip().startswith("crypt ")), "")
            check("crypt1 crypt2 crypt3 crypt4 crypt5" in crypt,
                  "all three new floors are in the crypt, in order", crypt.strip())
            # EXACTLY ONE finding: the empty crypt5 has no stair (it is the blank
            # canvas, joined up by hand), and the checker must SAY so - anything
            # else it finds is a real fault.
            at = next((i for i, l in enumerate(con) if l.startswith("validate:")), -1)
            found = []
            for l in con[at + 1:] if at >= 0 else []:
                if not l.startswith("  "):
                    break
                found.append(" ".join(l.split()[:3]))
            check(at >= 0 and found == ["ERR crypt5 map.check.levellost"],
                  "the checker's only finding is the empty, unlinked crypt5 - and it finds it",
                  f"{con[at] if at >= 0 else '(no validate line)'}: {found}")
            check(all("check says 0 error(s)" in l for l in con if "check says" in l)
                  and any("check says" in l for l in con),
                  "every generate and reroll checked clean at the moment it ran")

            levels = os.path.join(proj, "levels")
            unpaired = []
            linked = set()
            floors = ("crypt1", "crypt2", "crypt3", "crypt4", "crypt5")
            for stem in floors:
                for (typ, x, z, dest, dx, dz) in stairs_of(levels, stem):
                    # A world exit names a LOCATION (crypt_gate), not a floor -
                    # spelled like one, which is why this asks for the set.
                    if dest not in floors:
                        continue
                    back = [s for s in stairs_of(levels, dest)
                            if s[3] == stem and (s[1], s[2]) == (dx, dz) and (s[4], s[5]) == (x, z)]
                    if back:
                        linked.add(frozenset((stem, dest)))
                    else:
                        unpaired.append(f"{stem} {typ} @{x},{z} -> {dest}")
            check(not unpaired, "every stair on disk is paired with one leading straight back",
                  "; ".join(unpaired))
            want = {frozenset(("crypt2", "crypt3")), frozenset(("crypt3", "crypt4"))}
            check(want <= linked, "each GENERATED floor is stairs-linked to the one above it",
                  f"linked: {sorted(tuple(sorted(p)) for p in linked)}")
            # The EMPTY level is the old blank canvas: the fixed 16x16 box with its
            # room at 7..9 and no stair. WorldTest's rename scenario builds on that,
            # and moving the room once put a hand-written stair in rock (an abort).
            c5 = grid_of(levels, "crypt5")
            check(not stairs_of(levels, "crypt5") and len(c5) == 16 and
                  c5[8][7:10] == ".P." and c5[7][7:10] == "...",
                  "the EMPTY level is the old blank canvas: fixed box, room at 7..9, no stair",
                  f"stairs {stairs_of(levels, 'crypt5')}, rows {c5[7:10] if len(c5) > 9 else c5}")
            # The reroll into 12x12 had to GROW to hold crypt3's stairs, not drop
            # them - in whichever direction its stairs lie outside 12x12.
            grid = grid_of(levels, "crypt3")
            width = max(len(l) for l in grid) if grid else 0
            height = len(grid)
            held = all(x < width - 1 and z < height - 1 and grid[z][x] != "#"
                       for (_, x, z, *_rest) in stairs_of(levels, "crypt3"))
            outside = any(x >= 11 or z >= 11 for (_, x, z, *_rest) in stairs_of(levels, "crypt3"))
            check(held and outside and (width > 12 or height > 12),
                  "a reroll into a map too small for its stairs grows to hold them, on floor",
                  f"{width}x{height}, stairs {stairs_of(levels, 'crypt3')}")
        finally:
            shutil.rmtree(proj, ignore_errors=True)

    if phase_wanted(2):
        print("\n2 - the main path and its branches are counts, measured from the files")
        proj = scratch("lb_shape")
        try:
            code, con = run("levelshape.eval", "lb_shape")
            check(code == 0, "the script ran to the end", f"exit {code}")
            runs = parse_runs(con)
            check(len(runs) == 5, "all five generates reported what they built",
                  f"{len(runs)} reports")
            levels = os.path.join(proj, "levels")
            measured = {r["stem"]: measure(grid_of(levels, r["stem"])) for r in runs}
            for r in runs:
                m = measured[r["stem"]]
                want_rooms = r["path"][0] + sum(r["branch_rooms"])
                check(m["tree"] and m["rooms"] == want_rooms and
                      m["branches"] == r["branches"][0],
                      f"{r['stem']}: the file holds what the report says it built "
                      f"({want_rooms} rooms, {r['branches'][0]} branches, a tree)",
                      f"measured {show(m)}")
            if len(runs) == 5:
                a, b, fixed, short, locked = runs
                check(measured[a["stem"]]["branches"] == 0 and
                      measured[b["stem"]]["branches"] == 4,
                      "the control pair: branches 0 then 4 on one seed measures 0 then 4",
                      f"{measured[a['stem']]['branches']} / {measured[b['stem']]['branches']}")
                check(a["path"] == (6, 6) and b["path"] == (6, 6),
                      "...and both built the whole six-room path", f"{a['path']} {b['path']}")
                check(fixed["branches"] == (2, 2) and fixed["branch_rooms"] == [3, 3]
                      and fixed["path"] == (10, 10),
                      "fixed-length branches: two of exactly three rooms, off a 10-room path",
                      f"{fixed}")
                check(short["path"][0] < short["path"][1] and short["path"][0] >= 1,
                      "the shortfall stops short and SAYS so (path got < wanted)",
                      f"path {short['path']}, branches {short['branches']}")
                check(locked["locks"] == (1, 1), "a lock asked for is a lock built",
                      f"{locked['locks']}")
            check("validate: clean - no faults found" in con,
                  "the checker finds nothing wrong: every key before its door, every floor reached",
                  next((l for l in con if l.startswith("validate:")), "(no validate line)"))
        finally:
            shutil.rmtree(proj, ignore_errors=True)

    if phase_wanted(3):
        print("\n3 - complexity is four separate knobs, measured from the files")
        proj = scratch("lb_complex")
        try:
            code, con = run("levelcomplex.eval", "lb_complex")
            check(code == 0, "the script ran to the end", f"exit {code}")
            runs = parse_runs(con)
            check(len(runs) == 6, "all six generates reported what they built",
                  f"{len(runs)} reports")
            levels = os.path.join(proj, "levels")
            measured = {r["stem"]: measure(grid_of(levels, r["stem"])) for r in runs}
            # EVERY level: the file agrees with the report on every count.
            for r in runs:
                m = measured[r["stem"]]
                want_rooms = r["path"][0] + sum(r["branch_rooms"])
                check(m["stray"] == 0 and m["rooms"] == want_rooms and
                      m["loops"] == r["loops"][0] and m["deadends"] == r["deadends"][0] and
                      m["wound"] == r["wound"] and m["irregular"] == r["irregular"],
                      f"{r['stem']}: the file holds what the report says "
                      f"({want_rooms} rooms, {r['loops'][0]} loops, {r['deadends'][0]} dead "
                      f"ends, {r['wound']} wound, {r['irregular']} irregular)",
                      f"measured {show(m)}")
            if len(runs) == 6:
                base, loops, wind, odd, dead, mixed = runs
                mb = measured[base["stem"]]
                check(mb["loops"] == 0 and mb["deadends"] == 0 and mb["wound"] == 0 and
                      mb["irregular"] == 0, "the base, every complexity knob at zero, is plain",
                      f"{show(mb)}")
                # Each variant moves ITS number from zero, and the others stay put.
                for r, key, label in ((loops, "loops", "loops"), (wind, "wound", "winding"),
                                      (odd, "irregular", "irregular rooms"),
                                      (dead, "deadends", "dead ends")):
                    m = measured[r["stem"]]
                    others = [k for k in ("loops", "wound", "irregular", "deadends") if k != key]
                    check(m[key] > 0 and all(m[k] == 0 for k in others),
                          f"{label} alone: its own count rises from zero and no other does",
                          f"{show(m)}")
                check(loops["loops"] == (3, 3) and dead["deadends"] == (4, 4),
                      "the counted knobs build what they ask: 3/3 loops, 4/4 dead ends",
                      f"loops {loops['loops']}, dead ends {dead['deadends']}")
                # THE LOCK PROOF: with loops on, every door must still strand floor.
                m = measured[mixed["stem"]]
                doors, bad = doors_still_shut_something(levels, mixed["stem"], m["floor"],
                                                       m["start"])
                check(mixed["loops"][0] > 0 and len(doors) == mixed["locks"][0] > 0 and not bad,
                      f"with {mixed['loops'][0]} loops, each of the {len(doors)} doors still "
                      f"shuts floor off - no loop was built round a lock",
                      f"doors {doors}, bypassed {bad}, loops {mixed['loops']}")
            check("validate: clean - no faults found" in con,
                  "the checker finds nothing wrong: every key before its door, every floor reached",
                  next((l for l in con if l.startswith("validate:")), "(no validate line)"))
        finally:
            shutil.rmtree(proj, ignore_errors=True)

    if phase_wanted(4):
        print("\n4 - difficulty means strength, ramps toward the exit, and never greets the party")
        proj = scratch("lb_threat")
        try:
            code, con = run("levelthreat.eval", "lb_threat")
            check(code == 0, "the script ran to the end", f"exit {code}")
            # The game's own ranking, printed by `threat` - never a copy of it.
            table = {}
            for l in con:
                t = re.match(r"threat (\S+) ([\d.]+) offence=", l)
                if t:
                    table[t.group(1)] = float(t.group(2))
            check(len(table) >= 5, "the threat table was printed", f"{len(table)} kinds")
            runs = parse_runs(con)
            check(len(runs) == 38, "all 38 generates reported what they built", f"{len(runs)}")
            levels = os.path.join(proj, "levels")

            # RANGED KINDS ARE SCORED BY WHAT THEY THROW. Each `threat` line carries
            # its melee and shot per second; the archetype comes from the catalog
            # FILE, so the join does not trust the game's own reading of it.
            parts = {}
            for l in con:
                t = re.match(r"threat (\S+) [\d.]+ offence=([\d.]+) melee=([\d.]+) shot=([\d.]+)", l)
                if t:
                    parts[t.group(1)] = tuple(float(t.group(i)) for i in (2, 3, 4))
            arch, spell = {}, {}
            cur = None
            for l in io.open(os.path.join(proj, r"catalog\monsters.cat"), encoding="utf-8").read().splitlines():
                h = re.match(r"\[(\S+)\]", l)
                if h:
                    cur = h.group(1)
                    arch[cur] = "brute"
                kv = re.match(r"(\w+)\s*=\s*(\S+)", l)
                if cur and kv and kv.group(1) == "archetype":
                    arch[cur] = kv.group(2)
                if cur and kv and kv.group(1) == "spell":
                    spell[cur] = kv.group(2)
            ranged = {k for k, a in arch.items() if a in ("skirmisher", "caster")}
            wrong = [k for k, (_, _, shot) in parts.items() if (shot > 0) != (k in ranged)]
            check(parts and ranged and not wrong,
                  f"exactly the ranged kinds ({', '.join(sorted(ranged))}) are scored with a shot",
                  f"wrong: {wrong}, parts {parts}")
            casters = [k for k in ranged if arch[k] == "caster" and k in spell and k in parts]
            # The mage's staff is feeble and its flame bolt is not. Scoring the
            # spell as a plain bolt (MUTATION) puts its shot at 1.6x its melee;
            # the real bolt is 4.4x.
            weak = [(k, parts[k]) for k in casters if parts[k][2] < 3.0 * parts[k][1]]
            check(casters and not weak,
                  "a caster's shot is its SPELL: 3x+ its melee (the flame bolt, not the staff)",
                  f"casters {casters}, too weak {weak}")
            # And the shot is what it is ranked by, the edge on top.
            unused = [(k, parts[k]) for k in casters if parts[k][0] <= parts[k][2]]
            check(casters and not unused,
                  "a caster's offence is its shot with the ranged edge on top",
                  f"{unused}")

            if len(runs) == 38 and table:
                easies, hards = runs[0:5], runs[5:10]
                flats, rampeds = runs[10:18], runs[18:26]
                noboss, boss = runs[26], runs[27]
                sparse, dense = runs[28:33], runs[33:38]
                info = {}
                for r in runs:
                    m = measure(grid_of(levels, r["stem"]))
                    mons = monsters_of(levels, r["stem"])
                    depth = room_depths(m)
                    deepest = max(depth.values()) if depth else 0
                    rows = []
                    for (t, x, z) in mons:
                        room = m["room_of"].get((x, z))
                        rows.append((t, x, z, room, depth.get(room), table.get(t)))
                    info[r["stem"]] = (m, rows, deepest)
                def mean(xs):
                    return sum(xs) / len(xs) if xs else 0.0
                # SAFE START, on every level - and NOT VACUOUSLY: a level at low
                # difficulty can hold no monsters at all, so the count checked across
                # all of them has to be worth something too.
                checked = 0
                for r in runs:
                    m, rows, _ = info[r["stem"]]
                    sx, sz = m["start"]
                    checked += len(rows)
                    near = [(t, x, z) for (t, x, z, room, _, _) in rows
                            if room == m["start_room"] or abs(x - sx) + abs(z - sz) <= 3]
                    check(not near,
                          f"{r['stem']}: no monster in the start room or within 3 steps of it "
                          f"({len(rows)} checked)",
                          f"too near: {near}")
                check(checked >= 20, "...across enough monsters to mean something",
                      f"{checked} checked")
                # Every placed kind is in the table (the join is sound).
                unknown = {t for st in info for (t, *_rest) in info[st][1] if t not in table}
                check(not unknown, "every placed monster's kind has a threat in the table",
                      f"{unknown}")
                # DIFFICULTY IS STRENGTH ONLY: stronger by a MARGIN over five seeds
                # a side, and about as many (within a quarter). Not EXACTLY as many:
                # each new floor is built round the stair square of the one above,
                # so the two sides of a pair do not share a layout (29 vs 30 when
                # measured). Difficulty used to set the number too - ~4x across
                # this pair - and a 0.2 level came out with no monsters at all.
                e = [x for r in easies for x in info[r["stem"]][1]]
                h = [x for r in hards for x in info[r["stem"]][1]]
                check(abs(len(h) - len(e)) <= 0.25 * max(len(h), len(e), 1) and
                      mean([x[5] for x in h]) - mean([x[5] for x in e]) >= 3.0,
                      "difficulty 0.2 -> 0.8 (five seeds a side): 3+ threat stronger, and "
                      f"about as many: {len(e)} at {mean([x[5] for x in e]):.2f} -> "
                      f"{len(h)} at {mean([x[5] for x in h]):.2f}")
                empty = [r["stem"] for r in easies if not info[r["stem"]][1]]
                check(not empty, "easy is not empty: every difficulty 0.2 level has monsters",
                      f"empty: {empty}")
                # DENSITY IS NUMBER ONLY: many more at 0.8 than at 0.2 (the knob is
                # linear, so ~4x), and no stronger - the same rank band.
                s = [x for r in sparse for x in info[r["stem"]][1]]
                d = [x for r in dense for x in info[r["stem"]][1]]
                shift = mean([x[5] for x in d]) - mean([x[5] for x in s])
                check(len(d) >= 2.5 * max(1, len(s)) and abs(shift) <= 1.5,
                      "density 0.2 -> 0.8 (five seeds a side): 2.5x+ as many, and no "
                      f"stronger (within 1.5): {len(s)} at {mean([x[5] for x in s]):.2f} -> "
                      f"{len(d)} at {mean([x[5] for x in d]):.2f}")
                # RAMP: the deep half is stronger than the shallow half, and more so
                # than without the ramp.
                def gap(group):
                    """Deep-half minus near-half mean threat, pooled over the seeds."""
                    shallow, deep = [], []
                    for r in group:
                        _, rows, deepest = info[r["stem"]]
                        shallow += [x[5] for x in rows if x[4] is not None and x[4] * 2 <= deepest]
                        deep += [x[5] for x in rows if x[4] is not None and x[4] * 2 > deepest]
                    return mean(deep) - mean(shallow), len(shallow), len(deep)
                g0, g1 = gap(flats), gap(rampeds)
                # A MARGIN, not merely "more", and MANY monsters: with the pick
                # ignoring the ramp (MUTATION) noise gave -0.51 -> +0.23 and passed a
                # bare comparison, and a fully RANDOM pick cleared 2.5 on three seeds
                # (+2.59). The real ramp moves it ~+5.7; eight seeds a side puts the
                # noise well under the bar.
                check(g1[0] - g0[0] >= 2.5 and min(g1[1], g1[2]) >= 20,
                      "ramp 0 -> 1: the far half's monsters out-threaten the near half's "
                      f"by 2.5+ more than without it (eight seeds pooled, 20+ a half): "
                      f"{g0[0]:+.2f} -> {g1[0]:+.2f}",
                      f"gap {g0[0]:+.2f} (n {g0[1]}/{g0[2]}) -> {g1[0]:+.2f} (n {g1[1]}/{g1[2]})")
                # BOSS: the strongest kind of the pool - absent at this difficulty
                # without it, present with it, in a dead-end room.
                top = max((t for st in info for (t, *_r) in info[st][1]), key=lambda t: table[t],
                          default=None)
                top = max(table, key=table.get) if top is None else top
                pool_top = max((t for t in table if t in {x[0] for st in info for x in info[st][1]}),
                               key=lambda t: table[t])
                nb = [x for x in info[noboss["stem"]][1] if x[0] == pool_top]
                m_b, rows_b, _ = info[boss["stem"]]
                bb = [x for x in rows_b if x[0] == pool_top]
                leaf = bb and len(m_b["adj"][bb[0][3]]) == 1 if bb and bb[0][3] is not None else False
                check(not nb and len(bb) == 1 and leaf and boss["boss"] and not noboss["boss"],
                      f"boss off -> on at difficulty 0.2: the pool's strongest ({pool_top}) goes "
                      f"from absent to exactly one, in a dead-end room, and the report says so",
                      f"off {len(nb)}, on {len(bb)} {bb}, leaf {leaf}, "
                      f"report {noboss['boss']}/{boss['boss']}")
                check(boss["threat"][1] == table.get(pool_top) or
                      abs(boss["threat"][1] - table.get(pool_top, 0)) < 0.01,
                      "the report's top threat is the boss's", f"{boss['threat']} vs {table.get(pool_top)}")
            check("validate: clean - no faults found" in con,
                  "the checker finds nothing wrong",
                  next((l for l in con if l.startswith("validate:")), "(no validate line)"))
        finally:
            shutil.rmtree(proj, ignore_errors=True)

    if phase_wanted(5):
        print("\n5 - room sizes, theme, palette and presets")
        proj = scratch("lb_recipe")
        try:
            # crypt2 gets a palette OF ITS OWN (the same surface types, reversed),
            # or copying it would be indistinguishable from the default.
            c2 = os.path.join(proj, r"levels\crypt2.map")
            text = io.open(c2, encoding="utf-8", newline="").read()
            def reverse_palette(line):
                parts = line.split(" ")
                return " ".join(parts[:2] + parts[:1:-1])
            text = "\n".join(reverse_palette(l) if l.startswith("palette ") else l
                             for l in text.split("\n"))
            io.open(c2, "w", encoding="utf-8", newline="").write(text)

            code, con = run("levelrecipe.eval", "lb_recipe")
            check(code == 0, "the script ran to the end", f"exit {code}")
            runs = parse_runs(con)
            check(len(runs) == 8, "all eight generates reported what they built", f"{len(runs)}")
            levels = os.path.join(proj, "levels")
            if len(runs) == 8:
                small, big, ooze, undead, pal, default, before, recipe = runs
                # ROOM SIZES, from the file: every room's box inside its range.
                def sizes(stem):
                    m = measure(grid_of(levels, stem))
                    boxes = {}
                    for (x, z), r in m["room_of"].items():
                        b = boxes.setdefault(r, [x, z, x, z])
                        b[0], b[1] = min(b[0], x), min(b[1], z)
                        b[2], b[3] = max(b[2], x), max(b[3], z)
                    return [(b[2] - b[0] + 1, b[3] - b[1] + 1) for b in boxes.values()]
                s3, s8 = sizes(small["stem"]), sizes(big["stem"])
                check(s3 and all(w == 3 and h == 3 for w, h in s3) and
                      s8 and all(8 <= w <= 10 and 8 <= h <= 10 for w, h in s8),
                      "room sizes 3..3 vs 8..10 on one seed: every room measured inside its range",
                      f"small {sorted(set(s3))}, big {sorted(set(s8))}")
                # THEME, joined to the scratch world's own catalog tags.
                tags = {}
                cat = io.open(os.path.join(proj, r"catalog\monsters.cat"), encoding="utf-8").read()
                for block in re.split(r"\n(?=\[)", cat):
                    m = re.match(r"\[(\w+)\]", block)
                    t = re.search(r"^tags\s*=\s*(.*)$", block, re.M)
                    if m:
                        tags[m.group(1)] = set(t.group(1).split()) if t else set()
                def themed(stem, tag):
                    mons = monsters_of(levels, stem)
                    bad = [t for (t, _x, _z) in mons if tag not in tags.get(t, set())]
                    rec = any(l.strip() == f"theme {tag}" for l in
                              io.open(os.path.join(levels, stem + ".map"), encoding="utf-8"))
                    return mons, bad, rec
                mo, bo, ro = themed(ooze["stem"], "ooze")
                mu, bu, ru = themed(undead["stem"], "undead")
                check(mo and mu and not bo and not bu and ro and ru and
                      {t for t, *_ in mo}.isdisjoint({t for t, *_ in mu}),
                      "theme ooze vs undead on one seed: every monster carries its level's tag, "
                      "the two share no kind, and each level records its theme",
                      f"ooze {len(mo)} (off-theme {bo}), undead {len(mu)} (off-theme {bu}), "
                      f"records {ro}/{ru}")
                # PALETTE: crypt2's (now distinct) vs the default (the active level's).
                def palette(stem):
                    return [l.strip() for l in io.open(os.path.join(levels, stem + ".map"),
                                                       encoding="utf-8") if l.startswith("palette ")]
                want = palette("crypt2")
                arena = palette("eval_arena")
                check(want != arena and palette(pal["stem"]) == want and
                      palette(default["stem"]) == arena,
                      "palette chosen from crypt2 copies crypt2's, and the default still copies "
                      "the active level's (which differs)",
                      f"chosen {palette(pal['stem'])[:1]}, default {palette(default['stem'])[:1]}")
                # PRESETS: the loaded recipe built the level, the seed was kept.
                lab = next((l for l in con if l.startswith("preset labyrinth ")), "")
                recipe_line = next((l for l in con if l.startswith(f"generate: wrote {recipe['stem']} ")), "")
                lab_knobs = lab.split(" ", 2)[2] if lab.count(" ") >= 2 else ""
                check(lab_knobs and all(k in recipe_line for k in lab_knobs.split()) and
                      "seed:54" in recipe_line and "seed" not in lab_knobs,
                      "a loaded preset builds the level with its recipe, and keeps the seed "
                      "you were on (a preset holds none)",
                      f"preset '{lab_knobs[:60]}...', built '{recipe_line[:90]}...'")
                saved = [l for l in con if l.startswith("preset my_recipe ")]
                check(len(saved) == 1 and "seed" not in saved[0] and
                      saved[0].split(" ", 2)[2] == lab_knobs,
                      "saving stores the current knobs WITHOUT the seed, and it is listed",
                      f"{saved}")
                presets = io.open(os.path.join(proj, r"catalog\genpresets.cat"), encoding="utf-8").read()
                lists = [i for i, l in enumerate(con) if l == "> generate preset list"]
                after = con[lists[-1] + 1:] if lists else []
                check("[my_recipe]" not in presets and "[labyrinth]" in presets and
                      not any(l.startswith("preset my_recipe") for l in after),
                      "deleting removes it from the list and from genpresets.cat, and only it",
                      f"file has my_recipe: {'[my_recipe]' in presets}")
            check(any(l.startswith("catround 25 of 25") for l in con),
                  "every catalog file round-trips, genpresets.cat included (25 of 25)",
                  next((l for l in con if l.startswith("catround")), "(no catround line)"))
            check("validate: clean - no faults found" in con,
                  "the checker finds nothing wrong",
                  next((l for l in con if l.startswith("validate:")), "(no validate line)"))
        finally:
            shutil.rmtree(proj, ignore_errors=True)

    if phase_wanted(6):
        print("\n6 - generate, then play it from its start")
        proj = scratch("lb_play")
        try:
            # A FLOATING SCONCE, planted: a 'T' in open floor with no wall on any
            # side. crypt1 shipped one until 2026-09-25 (then moved to the north
            # wall), and it is what exposed the writer bug checked below - so the
            # scratch copy gets one back, or that check would pass vacuously.
            c1p = os.path.join(proj, r"levels\crypt1.map")
            rows = io.open(c1p, encoding="utf-8", newline="").read().split("\n")
            grid_at = [i for i, l in enumerate(rows) if l[:1] in "#.PT"]
            r4 = grid_at[4]
            rows[r4] = rows[r4][:4] + "T" + rows[r4][5:]
            io.open(c1p, "w", encoding="utf-8", newline="").write("\n".join(rows))
            code, con = run("levelplay.eval", "lb_play")
            check(code == 0, "the script ran to the end", f"exit {code}")
            runs = parse_runs(con)
            stem = runs[0]["stem"] if runs else ""
            levels = os.path.join(proj, "levels")
            grid = grid_of(levels, stem) if stem else []
            start = next(((x, z) for z, row in enumerate(grid) for x, ch in enumerate(row)
                          if ch == "P"), None)
            walkable = sum(1 for row in grid for ch in row if ch != "#")
            poses = [l for l in con if re.match(r"-?\d+,-?\d+ facing ", l)]
            info = next((l for l in con if re.match(r"\d+x\d+ map, start ", l)), "")
            check(any(l == f"generate: playing {stem}" for l in con),
                  "the play command says it is playing the level just made", stem)
            check(start is not None and poses and poses[0].startswith(f"{start[0]},{start[1]} "),
                  "the party stands on the level's start square, as the FILE has it",
                  f"file start {start}, pos {poses[:1]}")
            check(grid and info.startswith(f"{max(len(r) for r in grid)}x{len(grid)} map, "
                                          f"start {start[0]},{start[1]}, {walkable} walkable"),
                  "...on THAT level: mapinfo's size, start and walkable count match the file",
                  f"mapinfo '{info}', file {max(len(r) for r in grid) if grid else 0}x{len(grid)} "
                  f"{walkable} walkable")
            check(any(l.startswith("validate:") for l in con) and
                  not any("only works in-game" in l for l in con),
                  "and it is IN PLAY: an in-game command answers rather than refusing")
            check("generate: cannot play nosuchlevel" in con and len(poses) >= 2 and
                  poses[1] == poses[0],
                  "an unknown level is refused, and the party does not move", f"{poses[:2]}")

            # THE WRITER'S ROUND TRIP (found by this phase, 2026-09-25): crypt1 has a
            # sconce glyph in open floor, which loads with a default facing - but
            # `savemap` wrote it back as `... north`, a record the loader ASSERTS
            # faces a wall, so the next load of the world died. Run 2 below reloads
            # it; this reads what the writer produced.
            c1 = [l.strip() for l in io.open(os.path.join(levels, "crypt1.map"), encoding="utf-8")
                  if l.startswith("fixture sconce 4 4")]
            check(c1 == ["fixture sconce 4 4"],
                  "a sconce with no wall to face is written back WITHOUT a facing "
                  "(so the world still loads after a savemap)", f"{c1}")

            # THE SAME-LEVEL PATH: stand somewhere else on it, then play it.
            away = next(((x, z) for z, row in enumerate(grid) for x, ch in enumerate(row)
                         if ch == "." and start and abs(x - start[0]) + abs(z - start[1]) > 4),
                        None)
            second = os.path.join(proj, "levelplay2.eval")
            if away:
                io.open(second, "w", encoding="utf-8", newline="\n").write(
                    "logecho on\nreset\n"
                    f"goto {stem}\npos\ntp {away[0]} {away[1]}\npos\n"
                    f"generate play {stem}\npos\n")
            code2, con2 = run(second, "lb_play") if away else (1, [])
            p2 = [l for l in con2 if re.match(r"-?\d+,-?\d+ facing ", l)]
            check(away is not None and len(p2) == 3 and
                  p2[1].startswith(f"{away[0]},{away[1]} ") and
                  p2[2].startswith(f"{start[0]},{start[1]} "),
                  "playing the level the party is already on brings it back to the start",
                  f"away {away}, positions {p2}")
        finally:
            shutil.rmtree(proj, ignore_errors=True)

    if phase_wanted(7):
        print("\n7 - a reroll keeps every way in: world-map doorways and the game's opening")
        proj = scratch("lb_arrive")
        try:
            # The opening moves OFF the exit stair it shares in the demo (7,7) onto
            # a square nothing else holds open, or keeping stairs would keep it too.
            ini = os.path.join(proj, "project.ini")
            text = io.open(ini, encoding="utf-8", newline="").read()
            text = text.replace("start_x = 7", "start_x = 12").replace("start_z = 7", "start_z = 1")
            io.open(ini, "w", encoding="utf-8", newline="").write(text)
            # And a doorway with NO stair under it. The demo's crypt_back lands on
            # crypt2's exit stair, which a reroll already kept as a stair - so
            # against it alone, a reroll that ignored doorways passed this phase
            # (MUTATION, 2026-09-25). crypt_side lands on 5,3, which nothing holds.
            wm = os.path.join(proj, r"world\world.map")
            text = io.open(wm, encoding="utf-8", newline="").read()
            eol = "\r\n" if "\r\n" in text else "\n"
            anchor = "location dungeon crypt_back"
            at = text.index(anchor)
            text = (text[:at] + "location dungeon crypt_side 8 13 dungeon=crypt level=crypt2 "
                    "entryx=5 entryz=3" + eol + text[at:])
            io.open(wm, "w", encoding="utf-8", newline="").write(text)

            code, con = run("levelarrive.eval", "lb_arrive")
            check(code == 0, "the script ran to the end", f"exit {code}")
            checks = [l for l in con if "check says" in l]
            check(len(checks) == 6 and all("check says 0 error(s)" in l for l in checks),
                  "all six rerolls checked clean at the moment they ran",
                  "; ".join(checks) or "(no check lines)")
            verdicts = [i for i, l in enumerate(con) if l.startswith("validate:")]
            blocked = [l.strip() for l in con if "arrivalblocked" in l]
            check(len(verdicts) == 6 and not blocked and
                  all(con[i].startswith("validate: clean") for i in verdicts),
                  "no reroll left a doorway opening onto rock, nor any other fault",
                  f"{len(verdicts)} verdicts, blocked {blocked}, "
                  f"{[con[i] for i in verdicts if not con[i].startswith('validate: clean')]}")

            levels = os.path.join(proj, "levels")

            def reached(grid, start):
                """Floor squares joined to `start` (4-connected, '#' is rock)."""
                seen, todo = set(), [start]
                while todo:
                    x, z = todo.pop()
                    if (x, z) in seen or not (0 <= z < len(grid) and 0 <= x < len(grid[z])):
                        continue
                    if grid[z][x] == "#":
                        continue
                    seen.add((x, z))
                    todo += [(x + 1, z), (x - 1, z), (x, z + 1), (x, z - 1)]
                return seen

            def start_of(grid):
                return next(((x, z) for z, row in enumerate(grid) for x, ch in enumerate(row)
                             if ch == "P"), None)

            g2 = grid_of(levels, "crypt2")
            s2 = start_of(g2)
            joined = reached(g2, s2) if s2 else set()
            check((5, 3) in joined,
                  "crypt2: the doorway crypt_side's square 5,3 (no stair under it) is floor, "
                  "joined to the level", f"start {s2}, rows {g2}")
            check((10, 6) in joined,
                  "crypt2: crypt_back's square 10,6 (its exit stair) is still joined on",
                  f"start {s2}, rows {g2}")
            g1 = grid_of(levels, "crypt1")
            s1 = start_of(g1)
            check(s1 == (12, 1),
                  "crypt1: with no floor above, the game's OPENING is the entry - the level "
                  "starts on it", f"start {s1}")
            check(s1 is not None and (7, 7) in reached(g1, s1),
                  "crypt1: the exit stair (and crypt_gate's landing) at 7,7 is still joined on",
                  f"rows {g1}")
        finally:
            shutil.rmtree(proj, ignore_errors=True)

    print(f"\n{'PASS' if failures == 0 else f'FAIL ({failures})'}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
