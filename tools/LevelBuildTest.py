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
    for (x, z), k in corr_of.items():
        for nb in ((x + 1, z), (x - 1, z), (x, z + 1), (x, z - 1)):
            if nb in room_of:
                touches[k].add(room_of[nb])
    degree = [0] * rooms
    for t in touches:
        for r in t:
            degree[r] += 1
    tree = all(len(t) == 2 for t in touches) and corridors == rooms - 1
    leaves = sum(1 for d in degree if d == 1)
    start_room = room_of.get(start)
    branches = 0
    if rooms > 1 and start_room is not None:
        branches = leaves - 1 - (1 if degree[start_room] == 1 else 0)
    return {"rooms": rooms, "branches": branches, "tree": tree}


REPORT = re.compile(r"generate: built path (\d+)/(\d+) rooms, branches (\d+)/(\d+) "
                    r"\(rooms: ([\d -]+)\), locks (\d+)/(\d+)")


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
        check("validate: clean - no faults found" in con,
              "the checker finds nothing wrong with the whole world",
              next((l for l in con if l.startswith("validate:")), "(no validate line)"))
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
        want = {frozenset(("crypt2", "crypt3")), frozenset(("crypt3", "crypt4")),
                frozenset(("crypt4", "crypt5"))}
        check(want <= linked, "each new floor is stairs-linked to the one above it",
              f"linked: {sorted(tuple(sorted(p)) for p in linked)}")
        # The reroll into 12x12 had to GROW to hold crypt3's stairs, not drop them.
        grid = [l for l in io.open(os.path.join(levels, "crypt3.map"), encoding="utf-8")
                .read().splitlines() if l and l[0] in "#.P"]
        width = max(len(l) for l in grid) if grid else 0
        downs = [s for s in stairs_of(levels, "crypt3") if s[3] == "crypt4"]
        check(downs and width > downs[0][1] and width > 12,
              "a reroll into a map too small for its stairs grows to hold them",
              f"width {width}, stairs {downs}")
    finally:
        shutil.rmtree(proj, ignore_errors=True)

    print("\n2 - the main path and its branches are counts, measured from the files")
    proj = scratch("lb_shape")
    try:
        code, con = run("levelshape.eval", "lb_shape")
        check(code == 0, "the script ran to the end", f"exit {code}")
        # Pair each `wrote <stem>` with the `built ...` line that follows it.
        runs = []
        for i, l in enumerate(con):
            m = re.match(r"generate: wrote (\S+) ", l)
            if m and i + 1 < len(con) and (r := REPORT.match(con[i + 1])):
                g = r.groups()
                runs.append({"stem": m.group(1), "path": (int(g[0]), int(g[1])),
                             "branches": (int(g[2]), int(g[3])),
                             "branch_rooms": [int(n) for n in g[4].split() if n != "-"],
                             "locks": (int(g[5]), int(g[6]))})
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
                  f"measured {m}")
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

    print(f"\n{'PASS' if failures == 0 else f'FAIL ({failures})'}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
