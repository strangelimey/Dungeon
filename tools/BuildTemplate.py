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
# their CRLF endings and header comments come across as they are. So are the
# manifest fields a new world inherits (SOURCE_FIELDS): they are READ from
# dungeon-demo's project.ini, never retyped here - a hand-kept start_items once
# lost fire_flask (code-review C439).
#
# --out <dir> writes the template somewhere else (a judge's scratch folder)
# instead of replacing assets\templates\default.
import argparse
import io
import os
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, r"assets\projects\dungeon-demo")
OUT = os.path.join(ROOT, r"assets\templates\default")

# The source manifest's fields a new world keeps, in this order: the default
# fixture ids and the starting-item picks. Everything else in it (the level
# list, the start, the eval level) names this world's own places.
SOURCE_FIELDS = ("default_sconce", "default_brazier", "start_items")

# Catalogs whose ENTRIES are places or provenance: only their header comment
# (everything before the first [id]) comes across, so the file still explains
# itself to whoever opens it.
HEADER_ONLY = {"dungeons.cat", "quests.cat", "flags.cat", "imports.cat"}
# Fields that point at places, stripped from every entry of these catalogs.
STRIP = {"items.cat": ("quest", "reveals", "flag"), "weapons.cat": ("quest", "reveals", "flag"),
         "armor.cat": ("quest", "reveals", "flag")}

MANIFEST_HEAD = (
    "; The TEMPLATE a blank new world starts from (docs/editor-updates-plan.md P4).\r\n"
    "; Built by tools/BuildTemplate.py - edit the script, not this file.\r\n"
    ";\r\n"
    "; It lives outside assets/projects/, so no world list offers it. A world made\r\n"
    "; from it is given its own name, one starter room and an overworld.\r\n"
    "\r\n"
    "name = New World\r\n"
)
# A comment written above a copied field.
FIELD_NOTES = {
    "start_items": "; What a new party member may pick as their two starting items (party creation).\r\n",
}


def field_name(line):
    s = line.strip()
    if not s or s.startswith(";") or s.startswith("["):
        return None
    return s.split("=", 1)[0].strip()


def source_fields():
    """SOURCE_FIELDS as dungeon-demo's project.ini has them: {key: value}. A
    field the source does not set is left out, so the game's default applies to
    a new world exactly as it does to the source (Project.cpp)."""
    found = {}
    path = os.path.join(SOURCE, "project.ini")
    for line in io.open(path, encoding="utf-8").read().splitlines():
        key = field_name(line)
        if key in SOURCE_FIELDS and key not in found:
            found[key] = line.split("=", 1)[1].strip()
    return found


def manifest():
    fields = source_fields()
    text = MANIFEST_HEAD
    for key in SOURCE_FIELDS:
        if key not in fields:
            continue
        if key in FIELD_NOTES:
            text += "\r\n" + FIELD_NOTES[key]
        text += "%s = %s\r\n" % (key, fields[key])
    return text


def main():
    parser = argparse.ArgumentParser(description="Builds the new-world template from dungeon-demo.")
    parser.add_argument("--out", default=OUT, help="where to write it (default: assets\\templates\\default)")
    out = os.path.abspath(parser.parse_args().out)
    # The folder is REPLACED, so it must be a template (or empty), never a world.
    projects = os.path.normcase(os.path.join(ROOT, "assets", "projects")) + os.sep
    if os.path.normcase(out + os.sep).startswith(projects):
        raise SystemExit("BuildTemplate: refusing to write over a world: " + out)
    if os.path.isdir(out) and os.listdir(out) and not os.path.isfile(os.path.join(out, "project.ini")):
        raise SystemExit("BuildTemplate: %s is not empty and holds no project.ini - not replacing it" % out)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "catalog"))
    io.open(os.path.join(out, "project.ini"), "w", encoding="utf-8", newline="").write(manifest())
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
        io.open(os.path.join(out, "catalog", name), "w", encoding="utf-8", newline="").write(
            "".join(lines))
        print("  " + name)
    print("wrote", out)


if __name__ == "__main__":
    main()
