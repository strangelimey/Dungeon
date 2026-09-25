# tools/LevelBuildTest.py — the level-building thread's checks (docs/level-building.md).
#
# Run:  python tools\LevelBuildTest.py      (needs a debug build)
#
# The eval runner's own verdict only says every line matched a command, so it
# reads PASS on a run that stranded every level it made. This reads what the
# run PRODUCED — the checker's answer and the level files — and judges that.
#
#   1. CREATE AND REROLL — in a scratch copy of the demo world, make three new
#      crypt floors (generated, generated, empty) and reroll one twice, the
#      second time into a map too small for its stairs. Demand: every floor in
#      the crypt, the checker clean, and every stair on disk PAIRED — its
#      destination holds a stair leading straight back to the same square.
#   2. THE SHAPE IS COUNTS (P2) — generate with the path and branch knobs set,
#      then MEASURE each level from its .map, not from anything the generator
#      says: rooms are the floor that lies in 2x2 blocks (corridors are one
#      square wide), branches come from the dead ends. The measurement must
#      match the knobs AND the generator's own report, so a report that lies
#      fails as loudly as a shape that is wrong. The control pair differs only
#      in `branches` (0 vs 4); the shortfall case asks for far more than a
#      16x16 map holds and must stop short and say so.
#   3. COMPLEXITY IS FOUR KNOBS (P3) — one base level with every complexity
#      knob at zero, and four variants each raising ONE of them: its own
#      measured count must rise from zero and no other may move. Loops are the
#      link graph's cycles, dead ends the corridors touching one room, winding
#      the links that are not a single row or column, irregular the rooms that
#      do not fill their bounding box. And a level with loops AND locks: every
#      door, shut on its own, must still strand floor — the proof no loop was
#      built round a lock.
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
    r = subprocess.run(args, cwd=ROOT, capture_output=True, timeout=600)
    log = io.open(LOG, encoding="utf-8", errors="replace").read()
    return r.returncode, [l.split("console: ", 1)[1] for l in log.splitlines()
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
    the main path (the exit is never a branch's anchor) — so branches are the
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
    # else — none, or three — is not something the generator makes.
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
    return {"rooms": rooms, "branches": branches, "tree": loops == 0 and stray == 0,
            "loops": loops, "deadends": len(stubs), "wound": wound,
            "irregular": irregular, "stray": stray, "floor": floor, "start": start}


def doors_still_shut_something(levels_dir, stem, floor, start):
    """Every door, shut on its own, must strand floor the start cannot reach —
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
    return runs


def show(m):
    """A measurement without the raw floor set, for a failure's detail line."""
    return {k: v for k, v in m.items() if k not in ("floor", "start")}


def main():
    if not os.path.isfile(EXE):
        print(f"no debug build at {EXE}")
        return 2

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
        # canvas, joined up by hand), and the checker must SAY so — anything
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
                # A world exit names a LOCATION (crypt_gate), not a floor —
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
        # them — in whichever direction its stairs lie outside 12x12.
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

    print(f"\n{'PASS' if failures == 0 else f'FAIL ({failures})'}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
