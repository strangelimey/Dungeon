# tools/BuildTemplate.py - builds assets/templates/default, the TEMPLATE a
# blank new world starts from (docs/editor-updates-plan.md, P4).
#
# Run:  python tools\BuildTemplate.py
#
# WHY A TEMPLATE. A new world used to copy whatever world happened to be
# RUNNING, so "blank" meant "this world's content, minus its places" and its
# contents drifted with whatever you had open. The template is a fixed
# starting kit instead, and it lives OUTSIDE assets/projects/ so no world list
# offers it and nothing can switch into it or delete it by accident.
#
# WHAT IS IN IT. Every catalog the game needs to run and an author needs to
# build with - surfaces, props, monsters, items, spells, effects, the combat
# numbers, stairs, doors - taken from dungeon-demo, the content this game
# ships. What is NOT in it is anything that makes a particular GAME: no
# levels, no dungeons, no quests, no flags, no overworld, no opening, and no
# imports.cat provenance (it describes files on one machine). Items lose
# `quest`, `reveals` and `flag`, which name a quest stage, a world location and
# a flag (flags.cat, since tool-refinement Phase 4) that do not exist in a new
# world - W7's lesson, found by the checker on its first run: content comes
# across, what content POINTS AT does not.
#
# The template is DEFINED BY THIS SCRIPT (the Build*.py rule): to change what a
# new world starts with, change dungeon-demo's catalogs or the rules below and
# re-run. Files are copied byte for byte apart from the lines removed, so
# their CRLF endings and header comments come across as they are.
import io
import os
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, r"assets\projects\dungeon-demo")
OUT = os.path.join(ROOT, r"assets\templates\default")

# Catalogs whose ENTRIES are places or provenance: only their header comment
# (everything before the first [id]) comes across, so the file still explains
# itself to whoever opens it.
HEADER_ONLY = {"dungeons.cat", "quests.cat", "flags.cat", "imports.cat"}
# Fields that point at places, stripped from every entry of these catalogs.
STRIP = {"items.cat": ("quest", "reveals", "flag"), "weapons.cat": ("quest", "reveals", "flag"),
         "armor.cat": ("quest", "reveals", "flag")}

MANIFEST = (
    "; The TEMPLATE a blank new world starts from (docs/editor-updates-plan.md P4).\r\n"
    "; Built by tools/BuildTemplate.py - edit the script, not this file.\r\n"
    ";\r\n"
    "; It lives outside assets/projects/, so no world list offers it. A world made\r\n"
    "; from it is given its own name, one starter room and an overworld.\r\n"
    "\r\n"
    "name = New World\r\n"
    "default_sconce = sconce\r\n"
    "default_brazier = brazier\r\n"
    "\r\n"
    "; What a new party member may pick as their two starting items (party creation).\r\n"
    "start_items = dagger, club, padded_jack, tunic, rock, apple, bread, waterskin\r\n"
)


def field_name(line):
    s = line.strip()
    if not s or s.startswith(";") or s.startswith("["):
        return None
    return s.split("=", 1)[0].strip()


def main():
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(os.path.join(OUT, "catalog"))
    io.open(os.path.join(OUT, "project.ini"), "w", encoding="utf-8", newline="").write(MANIFEST)
    src = os.path.join(SOURCE, "catalog")
    for name in sorted(os.listdir(src)):
        if not name.endswith(".cat"):
            continue
        lines = io.open(os.path.join(src, name), encoding="utf-8", newline="").read().splitlines(True)
        if name in HEADER_ONLY:
            head = []
            for line in lines:
                if line.lstrip().startswith("["):
                    break
                head.append(line)
            lines = head
        elif name in STRIP:
            lines = [l for l in lines if field_name(l) not in STRIP[name]]
        io.open(os.path.join(OUT, "catalog", name), "w", encoding="utf-8", newline="").write(
            "".join(lines))
        print("  " + name)
    print("wrote", os.path.relpath(OUT, ROOT))


if __name__ == "__main__":
    main()
