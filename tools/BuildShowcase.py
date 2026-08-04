#!/usr/bin/env python3
# ============================================================================
# tools/BuildShowcase.py — generates the `showcase` level: a hand-composed
# level whose only job is to PHOTOGRAPH well.
#
# The gallery levels (BuildGallery.py) exist to audit surfaces: every room
# wears a different wall and the lighting is deliberately flat and even so a
# material can be judged. That is the opposite of what a screenshot wants.
# This level lights from few sources, leaves the corners dark, and puts a
# colonnade between the camera and the fire so the columns throw real shadows
# down the aisle.
#
# Composition notes, since they are not obvious from the numbers below:
#   * The entry corridor is ONE cell wide and seven long, so the first thing
#     the camera sees is a lit throat opening into a much larger space. Grid
#     crawlers photograph best when the sightline is long.
#   * The colonnade runs x=10 and x=19, leaving a wide central aisle. Braziers
#     sit BESIDE the columns, not in the aisle, so the light rakes across the
#     column shafts instead of flattening them.
#   * The apse is marble and lit from its own two sconces, so it reads as a
#     brighter room seen through a darker one — depth cue, and it gives the
#     Bone Knight something to be silhouetted against.
#   * Ambient is low. The whole point is that the torches do the work; see
#     docs/wall-details.md on why a grazing light is what makes relief read.
#
# Re-run after editing, then `goto showcase` in the dev console.
# ============================================================================
import os

W, H = 28, 24

# --- surface palettes -------------------------------------------------------
# Index 0 is the default for any cell without a `variant` record, so the hall's
# triple goes first and only the other rooms need records written.
WALLS = ["wall_temple_ancient", "wall_marble", "wall_brick_old", "wall_cobble_mossy"]
FLOORS = ["floor_temple", "floor_ancient_stone", "floor_slabs", "floor_cobble_mossy"]
CEILINGS = ["ceiling_rock_layered", "ceiling_stone", "ceiling_cracked", "ceiling_rock_porous"]

HALL, SHRINE, CORRIDOR, CRYPT = 0, 1, 2, 3

# --- rooms (x0, x1, z0, z1 inclusive) --------------------------------------
# The hall is EIGHT cells wide, not fourteen. The ceiling is one cell high and
# cannot be raised, so a wide room reads as squat and its side walls fall out of
# torch range entirely — the first cut was 14 wide and photographed like a car
# park. At eight, the sconces reach both walls and the space reads as a hall.
ROOMS = {
	HALL:     (10, 17, 3, 14),
	SHRINE:   (11, 15, 1, 2),
	CRYPT:    (21, 25, 6, 11),
}
# Corridors are carved separately so they can be one cell wide.
CORRIDORS = [
	(13, 13, 15, 21),  # south entry throat
	(18, 20, 8, 8),    # hall -> crypt passage
]

# x=13 lines the entry throat up with the aisle and the apse statue, so the
# whole level has one long sightline to walk into.
START = (13, 21)

FIXTURES = [
	# Braziers sit in the SIDE aisles, outboard of the colonnade, so the columns
	# stand between the fire and the camera and throw their shadows up the nave.
	("brazier", 10, 8, None), ("brazier", 17, 8, None),
	# Sconces every three cells down both side walls. The first cut lit the hall
	# with ambient instead and it went flat — the room has to be lit by LIGHTS or
	# there is nothing for the shadows to be cast by.
	("sconce", 10, 4, "west"), ("sconce", 10, 7, "west"),
	("sconce", 10, 10, "west"), ("sconce", 10, 13, "west"),
	("sconce", 17, 4, "east"), ("sconce", 17, 7, "east"),
	("sconce", 17, 10, "east"), ("sconce", 17, 13, "east"),
	("sconce", 11, 3, "north"), ("sconce", 16, 3, "north"),
	("sconce", 11, 14, "south"), ("sconce", 16, 14, "south"),
	# The apse: its own pair, so it reads brighter than the hall in front of it.
	("sconce", 11, 1, "west"), ("sconce", 15, 1, "east"),
	# The entry throat: alternating sides, so the walk in is not evenly lit.
	("sconce", 13, 16, "west"), ("sconce", 13, 19, "east"),
	# The passage and the crypt.
	("sconce", 19, 8, "north"),
	("brazier", 23, 8, None),
	("sconce", 21, 7, "west"), ("sconce", 25, 10, "east"),
]

DECORATIONS = [
	# Colonnade — solid, so they occlude both the light and the party.
	("column", 11, 5, None), ("column", 11, 8, None), ("column", 11, 11, None),
	("column", 16, 5, None), ("column", 16, 8, None), ("column", 16, 11, None),
	# The apse.
	("statue", 13, 1, "south"),
	# Hall dressing, kept out of the nave.
	("banner", 10, 6, "west"), ("banner", 17, 6, "east"),
	("barrel", 10, 12, None), ("crate", 17, 12, None),
	# Crypt clutter.
	("ancient_pot", 22, 10, None), ("boulder", 24, 6, None),
	("mossy_rock", 21, 9, None), ("chest", 25, 6, None),
]

# --- monsters (.ent) --------------------------------------------------------
# The four REAL skeleton models, not the procedural `skeleton` placeholder.
# asleep=1 keeps each one standing where it was placed instead of charging the
# camera; it gates the AI decision only, so they still play their idle clip.
MONSTERS = [
	("skel_warrior", 12, 9, "south"),
	("skel_berserker", 15, 9, "south"),
	("skel_spearman", 14, 6, "south"),
	("skel_bare", 12, 12, "south"),
	("skel_warrior", 22, 7, "south"),    # crypt
	("skel_bare", 24, 10, "west"),
]


def build_grid():
	grid = [["#"] * W for _ in range(H)]
	owner = {}

	def carve(x0, x1, z0, z1, room):
		for z in range(z0, z1 + 1):
			for x in range(x0, x1 + 1):
				grid[z][x] = "."
				owner[(x, z)] = room

	for room, (x0, x1, z0, z1) in ROOMS.items():
		carve(x0, x1, z0, z1, room)
	for (x0, x1, z0, z1) in CORRIDORS:
		carve(x0, x1, z0, z1, CORRIDOR)

	grid[START[1]][START[0]] = "P"
	return grid, owner


def wall_owners(grid, owner):
	"""Which room each SOLID cell should be textured for.

	A wall variant lives on the solid cell and paints all four of its faces, so
	a block between two rooms can only wear one of them. Rooms are applied in
	order and the later one wins, which is why the apse is last: its marble
	ring frames the opening the hall looks through.
	"""
	walls = {}
	for room in (HALL, CORRIDOR, CRYPT, SHRINE):
		for (x, z), r in owner.items():
			if r != room:
				continue
			for dz in (-1, 0, 1):
				for dx in (-1, 0, 1):
					nx, nz = x + dx, z + dz
					if 0 <= nx < W and 0 <= nz < H and grid[nz][nx] == "#":
						walls[(nx, nz)] = room
	return walls


def main():
	grid, owner = build_grid()
	walls = wall_owners(grid, owner)

	out = []
	out.append("; showcase — a level composed to be PHOTOGRAPHED, not audited.")
	out.append("; Generated by tools\\BuildShowcase.py — edit there and re-run.")
	out.append(";")
	out.append("; Hall lit by four braziers flanking a colonnade; the apse carries its own")
	out.append("; pair so it reads brighter through the darker hall. Ambient is deliberately")
	out.append("; low — the torches are meant to do the work.")
	out.append("")
	out.append("palette wall " + " ".join(WALLS))
	out.append("palette floor " + " ".join(FLOORS))
	out.append("palette ceiling " + " ".join(CEILINGS))
	out.append("atmosphere dust=0.075 haze=0.80 ambient=1.55")
	out.append(";")
	for row in grid:
		out.append("".join(row))

	out.append(";")
	for (kind, x, z, facing) in FIXTURES:
		out.append(f"fixture {kind} {x} {z}" + (f" {facing}" if facing else ""))
	out.append(";")
	for (kind, x, z, facing) in DECORATIONS:
		out.append(f"decoration {kind} {x} {z}" + (f" {facing}" if facing else ""))

	out.append(";")
	for (x, z), room in sorted(owner.items(), key=lambda kv: (kv[0][1], kv[0][0])):
		if room != HALL:
			out.append(f"variant floor {x} {z} {room}")
			out.append(f"variant ceiling {x} {z} {room}")
	for (x, z), room in sorted(walls.items(), key=lambda kv: (kv[0][1], kv[0][0])):
		if room != HALL:
			out.append(f"variant wall {x} {z} {room}")

	here = os.path.dirname(os.path.abspath(__file__))
	levels = os.path.join(here, "..", "assets", "projects", "dungeon-demo", "levels")
	with open(os.path.join(levels, "showcase.map"), "w", encoding="utf-8", newline="\n") as f:
		f.write("\n".join(out) + "\n")

	ent = ["; showcase — dynamic layer.",
		   "; The five bought skeleton models (skel_bare / berserker / spearman / warrior",
		   "; / human), NOT the procedural `skeleton` placeholder. asleep=1 holds each one",
		   "; on its mark for the camera; it gates the AI decision only, so the idle clip",
		   "; still plays.",
		   ""]
	for (kind, x, z, facing) in MONSTERS:
		ent.append(f"monster {kind} {x} {z} {facing} asleep=1")
	with open(os.path.join(levels, "showcase.ent"), "w", encoding="utf-8", newline="\n") as f:
		f.write("\n".join(ent) + "\n")

	floors = sum(r.count(".") + r.count("P") for r in ("".join(g) for g in grid))
	print(f"showcase.map  {W}x{H}, {floors} floor cells, "
		  f"{len(FIXTURES)} fixtures, {len(DECORATIONS)} decorations")
	print(f"showcase.ent  {len(MONSTERS)} monsters")


if __name__ == "__main__":
	main()
