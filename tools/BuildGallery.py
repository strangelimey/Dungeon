#!/usr/bin/env python
# ---------------------------------------------------------------------------
# BuildGallery.py — regenerate the texture-gallery levels.
#
# A gallery level is a lattice of small rooms joined by corridors, where EVERY
# room wears a different wall/floor/ceiling triple. It exists to be walked: the
# point is to see each scanned set at full size, lit, next to its neighbours,
# rather than as a thumbnail in the asset picker.
#
# The room set is DERIVED FROM THE CATALOGS, so importing new surfaces and
# re-running this is how the gallery grows — there is no hand-maintained list
# to fall out of step. Walls drive the room count (they are the most numerous);
# floors and ceilings cycle underneath.
#
#   python tools\BuildGallery.py                 # writes into assets\projects
#   python tools\BuildGallery.py --project <dir> --levels a b
#
# The emitted .map is exactly the format the in-game editor writes, so the
# gallery can be edited afterwards like any other level.
# ---------------------------------------------------------------------------

import argparse
import os
import re
import sys

# --- geometry ---------------------------------------------------------------
# Rooms sit on a fixed pitch with a 5x5 interior. The 3-cell gap between two
# interiors is what makes per-room walls possible AT ALL: a wall variant lives
# on the SOLID cell and paints all four of its faces, so two rooms sharing one
# block would have to share its texture. At this pitch each room owns its own
# ring and the middle column/row between them is neutral corridor stone.
ROOM = 5   # interior extent
PITCH = 8  # room origin to room origin
COLS = 7   # rooms per row
ROWS = 2   # rows of rooms per level

# The corridors' own triple. Held constant everywhere so the rooms read as the
# variable and the connective tissue does not: walking a corridor should feel
# like leaving one exhibit and arriving at the next.
CORRIDOR_WALL = "wall_stone_plain"
CORRIDOR_FLOOR = "floor_slate"
CORRIDOR_CEILING = "ceiling_rough"

# Brighter and clearer than a play level: dust and haze exist to hide distance,
# which is the opposite of what a gallery wants. Tunable live in the editor's
# Level dialog if it reads flat.
ATMOSPHERE = "atmosphere dust=0.035 haze=0.6 ambient=1.35"


def read_ids(path):
	"""Catalog block headers, in file order — that order is the palette order."""
	with open(path, encoding="utf-8") as f:
		return re.findall(r"^\[([A-Za-z0-9_-]+)\]", f.read(), re.M)


class Level:
	def __init__(self, stem, walls, floors, ceilings):
		self.stem = stem
		self.w = PITCH * COLS - 1
		self.h = PITCH * ROWS - 1
		self.grid = [["#"] * self.w for _ in range(self.h)]
		self.rooms = []       # (col, row, ox, oz, wall, floor, ceiling)
		self.variants = []    # (kind, x, z, index)
		self.fixtures = []    # text lines
		self.stairs = []      # text lines

		# The palette IS the variant index space, so it is built once and every
		# lookup goes through it. The corridor triple is appended if the level's
		# own share of the catalog does not already carry it.
		self.wall_pal = list(walls)
		self.floor_pal = list(floors)
		self.ceil_pal = list(ceilings)
		for pal, id_ in ((self.wall_pal, CORRIDOR_WALL),
						 (self.floor_pal, CORRIDOR_FLOOR),
						 (self.ceil_pal, CORRIDOR_CEILING)):
			if id_ not in pal:
				pal.append(id_)

		# Rooms cycle through everything EXCEPT the corridor materials, so a
		# room never accidentally impersonates a corridor. Walls are one-per-room
		# (they are what the gallery is counting); floors and ceilings repeat.
		room_floors = [f for f in floors if f != CORRIDOR_FLOOR] or floors
		room_ceils = [c for c in ceilings if c != CORRIDOR_CEILING] or ceilings
		n = 0
		for r in range(ROWS):
			for c in range(COLS):
				if n >= len(walls):
					break
				ox, oz = 1 + PITCH * c, 1 + PITCH * r
				self.rooms.append((c, r, ox, oz, walls[n],
								   room_floors[n % len(room_floors)],
								   room_ceils[n % len(room_ceils)]))
				n += 1

	# --- carving ------------------------------------------------------------
	def carve(self):
		for (_c, _r, ox, oz, _w, _f, _cl) in self.rooms:
			for z in range(oz, oz + ROOM):
				for x in range(ox, ox + ROOM):
					self.grid[z][x] = "."
		# East-west corridors join every neighbouring pair; north-south ones
		# join the rows on alternating columns. Fully connecting both axes would
		# make a lattice with no sense of route — this keeps it walkable while
		# leaving a few rooms as pockets you double back through.
		for (c, r, ox, oz, *_rest) in self.rooms:
			if c + 1 < COLS and self.room_at(c + 1, r):
				for x in range(ox + ROOM, ox + ROOM + 3):
					self.grid[oz + 2][x] = "."
			if r + 1 < ROWS and c % 2 == 0 and self.room_at(c, r + 1):
				for z in range(oz + ROOM, oz + ROOM + 3):
					self.grid[z][ox + 2] = "."

	def room_at(self, col, row):
		for room in self.rooms:
			if room[0] == col and room[1] == row:
				return room
		return None

	# --- surfaces -----------------------------------------------------------
	def paint(self):
		wi = {n: i for i, n in enumerate(self.wall_pal)}
		fi = {n: i for i, n in enumerate(self.floor_pal)}
		ci = {n: i for i, n in enumerate(self.ceil_pal)}
		painted = set()
		for (_c, _r, ox, oz, wall, floor, ceil) in self.rooms:
			for z in range(oz - 1, oz + ROOM + 1):
				for x in range(ox - 1, ox + ROOM + 1):
					if not (0 <= x < self.w and 0 <= z < self.h):
						continue
					inside = ox <= x < ox + ROOM and oz <= z < oz + ROOM
					if inside:
						self.variants.append(("floor", x, z, fi[floor]))
						self.variants.append(("ceiling", x, z, ci[ceil]))
					elif self.grid[z][x] == "#":
						# Ring block. A cell the corridor punched through is
						# floor now and must NOT take a wall record — the loader
						# drops variants that land on the wrong cell type, and a
						# dropped record is a silent lie in the file.
						self.variants.append(("wall", x, z, wi[wall]))
					painted.add((x, z))
		# Everything the rooms did not claim is corridor: the connecting cells
		# and the neutral stone between the rings.
		for z in range(self.h):
			for x in range(self.w):
				if (x, z) in painted:
					continue
				if self.grid[z][x] == "#":
					self.variants.append(("wall", x, z, wi[CORRIDOR_WALL]))
				else:
					self.variants.append(("floor", x, z, fi[CORRIDOR_FLOOR]))
					self.variants.append(("ceiling", x, z, ci[CORRIDOR_CEILING]))

	# --- lighting -----------------------------------------------------------
	def light(self):
		# A sconce on each of the four walls plus a brazier in a corner. Lighting
		# a gallery room from one side is what a play level does — here it would
		# flatten exactly the relief the scanned height maps were bought for, so
		# every wall gets its own grazing light and the corner fire fills in.
		# All five sit on cells no corridor passes through, and each sconce names
		# a wall that is still solid after carving (the east and west ones sit
		# off-centre because the corridor punches through the middle row).
		for (_c, _r, ox, oz, *_rest) in self.rooms:
			self.fixtures.append(f"fixture sconce {ox + 1} {oz} north")
			self.fixtures.append(f"fixture sconce {ox + 3} {oz + ROOM - 1} south")
			self.fixtures.append(f"fixture sconce {ox} {oz + 3} west")
			self.fixtures.append(f"fixture sconce {ox + ROOM - 1} {oz + 1} east")
			self.fixtures.append(f"fixture brazier {ox + ROOM - 1} {oz}")
		# One sconce at the midpoint of every corridor, so the runs between
		# exhibits are lit rather than black.
		for (c, r, ox, oz, *_rest) in self.rooms:
			if c + 1 < COLS and self.room_at(c + 1, r):
				self.fixtures.append(f"fixture sconce {ox + ROOM + 1} {oz + 2} north")
			if r + 1 < ROWS and c % 2 == 0 and self.room_at(c, r + 1):
				self.fixtures.append(f"fixture sconce {ox + 2} {oz + ROOM + 1} east")

	# --- writing ------------------------------------------------------------
	def legend(self):
		out = ["; Room legend — walk order is left to right, top row first.",
			   ";",
			   ";   room   cell     wall / floor / ceiling"]
		for i, (c, r, ox, oz, wall, floor, ceil) in enumerate(self.rooms):
			cell = f"{ox + 2},{oz + 2}"
			out.append(f";   {i + 1:>2} ({c},{r}) {cell:>7}  "
					   f"{wall} / {floor} / {ceil}")
		out.append(f";   corridors        {CORRIDOR_WALL} / {CORRIDOR_FLOOR}"
				   f" / {CORRIDOR_CEILING}")
		return out

	def text(self):
		lines = [f"; {self.stem} — texture gallery, generated by "
				 "tools\\BuildGallery.py.",
				 "; Rooms are derived from the catalogs: re-run after importing "
				 "new surfaces.",
				 ""]
		lines += self.legend()
		lines += ["",
				  "palette wall " + " ".join(self.wall_pal),
				  "palette floor " + " ".join(self.floor_pal),
				  "palette ceiling " + " ".join(self.ceil_pal),
				  ATMOSPHERE,
				  ";"]
		lines += ["".join(row) for row in self.grid]
		lines += [";"]
		lines += self.fixtures
		lines += self.stairs
		lines += [f"variant {k} {x} {z} {i}" for (k, x, z, i) in self.variants]
		return "\n".join(lines) + "\n"


def main():
	here = os.path.dirname(os.path.abspath(__file__))
	ap = argparse.ArgumentParser()
	ap.add_argument("--project",
					default=os.path.join(here, "..", "assets", "projects",
										 "dungeon-demo"))
	ap.add_argument("--levels", nargs=2, default=["start", "level2"],
					help="level stems to write (first is where the party starts)")
	args = ap.parse_args()

	cat = os.path.join(args.project, "catalog")
	walls = read_ids(os.path.join(cat, "walls.cat"))
	floors = read_ids(os.path.join(cat, "floors.cat"))
	ceilings = read_ids(os.path.join(cat, "ceilings.cat"))

	per = PITCH and COLS * ROWS  # rooms a level can hold
	if len(walls) > per * 2:
		print(f"warning: {len(walls)} walls but only {per * 2} rooms across two "
			  f"levels — the tail will not be shown", file=sys.stderr)

	# Split the walls between the levels; each level's floors are its own share
	# so the two do not load the same sets twice. Ceilings are few enough that
	# both levels carry all of them.
	half = min(per, (len(walls) + 1) // 2)
	fsplit = (len(floors) + 1) // 2
	levels = [
		Level(args.levels[0], walls[:half], floors[:fsplit], ceilings),
		Level(args.levels[1], walls[half:half + per], floors[fsplit:], ceilings),
	]
	for lv in levels:
		lv.carve()
		lv.paint()
		lv.light()

	# The stair pair, authored on both sides by hand (the editor would do this
	# for us, but a generated file has no editor). Down-stair in the last room
	# of level one, up-stair in the first room of level two; each destination is
	# the cell BESIDE the far stair, never the stair cell itself.
	a, b = levels
	ax, az = a.rooms[-1][2], a.rooms[-1][3]
	bx, bz = b.rooms[0][2], b.rooms[0][3]
	a.stairs.append(f"stairs stairs_down {ax} {az} south dest={b.stem} "
					f"destx={bx + 1} destz={bz} destfacing=south")
	b.stairs.append(f"stairs stairs_up {bx} {bz} south dest={a.stem} "
					f"destx={ax + 1} destz={az} destfacing=south")

	# EVERY level needs a start cell, not just the one the party begins on:
	# DungeonMap asserts on a map without a 'P' (DungeonMap.cpp), so a level
	# reached only by stairs still aborts the process the moment anything loads
	# it directly — the dev `goto` command, for one. Centre of the first room.
	for lv in levels:
		lv.grid[lv.rooms[0][3] + 2][lv.rooms[0][2] + 2] = "P"

	for lv in levels:
		mp = os.path.join(args.project, "levels", lv.stem + ".map")
		with open(mp, "w", encoding="utf-8", newline="\n") as f:
			f.write(lv.text())
		ent = os.path.join(args.project, "levels", lv.stem + ".ent")
		with open(ent, "w", encoding="utf-8", newline="\n") as f:
			f.write(f"; {lv.stem} — texture gallery (dynamic layer).\n"
					"; Deliberately empty: nothing should be fighting you while "
					"you look at walls.\n")
		print(f"{lv.stem}: {len(lv.rooms)} rooms, {lv.w}x{lv.h}, "
			  f"{len(lv.wall_pal)}w/{len(lv.floor_pal)}f/{len(lv.ceil_pal)}c, "
			  f"{len(lv.fixtures)} fixtures, {len(lv.variants)} variants")


if __name__ == "__main__":
	main()
