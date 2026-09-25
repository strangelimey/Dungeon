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

    print(f"\n{'PASS' if failures == 0 else f'FAIL ({failures})'}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
