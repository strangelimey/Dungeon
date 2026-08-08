# ============================================================================
# tools/BuildPortcullis.py — authors the iron lattice that fills a `rise` door,
# and writes a UNIT-SPACE .glb.
#
#   blender --background --factory-startup --python tools\BuildPortcullis.py -- <out.glb>
#
#   AssetBaker import-model <out.glb> <assets> portcullis_leaf --raw \
#       --texture-set sconce
#
#   live preview: python tools\bsend.py -f tools\BuildPortcullis.py
#
# Replaces the portcullis DOOR's use of `door_panel` — ModelBaker's plain slab,
# which was a stand-in for every leaf before any of them was authored. A
# portcullis is the one door that is NOT a panel: it is a grid you can see,
# shoot and be spoken to through, and a solid rectangle wearing a metal texture
# says none of that.
#
# `portcullis_leaf`, NOT `portcullis`, and the name is load-bearing: ModelBaker
# already builds a cosmetic `portcullis` DECORATION (a grate filling a whole
# cell face, decorations.cat, currently placed nowhere), exactly as it builds a
# cosmetic `door` beside the functional `door_panel`. The two are different
# sizes because they fill different things — a decoration fills a cell face at
# 2.1 x 2.32 m, a leaf fills its frame's opening at 1.7 x 2.1 m — so one mesh
# cannot serve both, and importing over that name silently replaced it.
#
# EVERYTHING IS IN UNITS: 1.0 = one dungeon square (game::kUnit, 2.5 m), Z up.
# Blender X is the engine's X, Blender Z is the engine's Y (up), Blender Y is
# the door's thickness. Same frame as BuildDoorFrame.py and BuildDoorLeaf.py,
# and for the same reason: a leaf authored Z-up lands the right way round with
# no quarter-turn about X.
#
# ---------------------------------------------------------------------------
# ONE MODEL, not a leaf plus a trim
# ---------------------------------------------------------------------------
# The wooden door is two models because it is two MATERIALS — the import path
# binds one texture set per model, so its iron straps cannot live inside a
# wood-textured mesh. A portcullis is iron all through, so that reason is
# absent and splitting it would only buy an extra draw call. The `trim` it used
# to carry (door_band, the rectangular band) is retired with this, and so is
# the last caller of ModelBaker's AddBandRect.
#
# ---------------------------------------------------------------------------
# WHAT IT HAS TO SURVIVE
# ---------------------------------------------------------------------------
# 1. THE OPENING IS A CONTRACT WITH THE FRAME (tools/BuildDoorFrame.py, built
#    with --head for this motion): OPEN 0.34 x DOOR_H 0.84 units. The lattice
#    fills it exactly; the jambs lap 0.8 cm over it, which swallows the outer
#    bars' edges and is why they may sit hard against x = +/- OPEN.
# 2. IT MUST CLEAR ITS OWN MORTICE. The frame's head slot is SLOT_T 0.032 units
#    half-high, so nothing here may be thicker than that or the door jams on
#    the way up. Asserted.
# 3. RAISED, IT MUST BE GONE. `rise` translates the leaf up by the type's
#    `travel`, and what hides it is the CEILING BLOCK over a walkable cell —
#    there is no scenery above the head slot to hide it in. So travel has to
#    carry the SPIKE TIPS (z = 0) past the ceiling at z = 1.0, which is exactly
#    why doors.cat says travel = 1.0 for this type. Asserted against TRAVEL
#    below, which must be kept in step with the catalog.
# 4. IT MUST READ AS OPEN. A bar's gap is the whole point of the object, so the
#    gaps are sized in real terms — 18 cm, a hand and not a shoulder — rather
#    than by picking a bar count that looked right.
#
# ---------------------------------------------------------------------------
# THE TEXTURE, and the feature-size rule
# ---------------------------------------------------------------------------
# `sconce`, the game's uniform worn-metal set, for the reason doors.cat gives
# at the portcullis entry: a lattice is a run of narrow bars, so its set must
# have no feature wider than a bar or each bar picks up a different piece of
# the scan and the grid reads as assembled from offcuts. That rule is why this
# is not `rusted_iron`, whose sheet panels and seams are far wider than a bar.
# ============================================================================
import sys

import bmesh
import bpy
from mathutils import Vector

# --- the opening, which the frame asserts too -------------------------------
OPEN = 0.34         # half-width
DOOR_H = 0.84       # height
TOP = 1.0           # floor to ceiling — what a raised leaf has to clear
TRAVEL = 1.0        # doors.cat [portcullis] `travel`; keep the two in step
SLOT_T = 0.032      # the frame's head mortice, half-height

# --- the lattice ------------------------------------------------------------
# A bar is square in section, because a portcullis is barred stock and not
# strap: it has to read as heavy from the side as from the front, and the
# quarter-turn views are the ones a party gets while walking past a gatehouse.
N_BAR = 8
BAR_H = 0.011       # bar half-section (5.5 cm square)
SPIKE_Z = 0.075     # where the shaft ends and the point begins (19 cm of spike)

# Three cross-rails, the top one a BEAM: a portcullis hangs from its head, so
# the member that carries the whole grid should look like it could.
RAIL_HY = 0.017     # rail half-thickness — thicker than a bar, so the bars pass
                    # INSIDE it and no two faces are ever coplanar
RAIL_HZ = 0.016     # a rail's half-height (8 cm)
BEAM_HZ = 0.026     # the head beam's (13 cm)
RAIL_Z = (0.16, 0.47)               # the two rails
BEAM_Z = DOOR_H - BEAM_HZ           # the beam, its top flush with the opening

RIVET_R = 0.007     # a rivet head at every crossing, both faces
TILE = 0.14         # texture repeat (0.35 m, the metal tile the trim used)

args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
EXPORT = bool(args)
OUT = args[0] if args else "portcullis_leaf.glb"

if bpy.context.mode != "OBJECT":
    bpy.ops.object.mode_set(mode="OBJECT")
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

bm = bmesh.new()

# Bar centres, the outermost pair hard against the opening's edges so the jambs
# lap over them. Everything between is evenly pitched.
PITCH = (2.0 * OPEN - 2.0 * BAR_H) / (N_BAR - 1)
BAR_X = [-OPEN + BAR_H + i * PITCH for i in range(N_BAR)]
GAP = PITCH - 2.0 * BAR_H


def quad(*pts):
    bm.faces.new([bm.verts.new(p) for p in pts])


def box(cx, cz, hx, hy, hz):
    """An axis-aligned box. Winding is left to recalc_face_normals below, which
    is safe here because every solid is closed and — after the weld — connected;
    that connectivity is the whole precondition (see the normals trap)."""
    x0, x1 = cx - hx, cx + hx
    y0, y1 = -hy, hy
    z0, z1 = cz - hz, cz + hz
    quad((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1))
    quad((x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0))
    quad((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0))
    quad((x1, y0, z0), (x1, y0, z1), (x1, y1, z1), (x1, y1, z0))
    quad((x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1))
    quad((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1))


# --- the bars ---------------------------------------------------------------
# Each bar is ONE closed solid: a shaft capped at the head, and a four-sided
# point below the shoulder. Built as one rather than as a shaft plus a spike
# because two solids meeting face to face weld into an edge with four faces,
# which is not a shell — the same trap that made the wooden leaf's pocket a
# grid rather than a stack of boxes.
for cx in BAR_X:
    x0, x1 = cx - BAR_H, cx + BAR_H
    y0, y1 = -BAR_H, BAR_H
    ring = ((x0, y0), (x1, y0), (x1, y1), (x0, y1))
    quad(*[(x, y, DOOR_H) for x, y in ring])                       # head cap
    for i in range(4):
        ax, ay = ring[i]
        bx, by = ring[(i + 1) % 4]
        quad((ax, ay, SPIKE_Z), (bx, by, SPIKE_Z),
             (bx, by, DOOR_H), (ax, ay, DOOR_H))                   # shaft
        bm.faces.new([bm.verts.new(p) for p in
                      ((ax, ay, SPIKE_Z), (bx, by, SPIKE_Z), (cx, 0.0, 0.0))])

# --- the rails and the head beam --------------------------------------------
# They run the full width and are thicker than a bar, so a bar crossing one is
# swallowed by it. That is deliberate: two solids that INTERSECT bury each
# other's faces, where two that meet flush put two coplanar faces in the same
# place and z-fight.
for cz, hz in ((RAIL_Z[0], RAIL_HZ), (RAIL_Z[1], RAIL_HZ), (BEAM_Z, BEAM_HZ)):
    box(0.0, cz, OPEN, RAIL_HY, hz)

# --- the rivets -------------------------------------------------------------
# One at every crossing, on both faces, as a squashed sphere: a full one reads
# as a ball resting on the iron where a rivet head is a swelling of it. Only on
# the two RAILS — the head beam swallows the bars whole, so there is nothing
# there for a rivet to be holding.
for cz in RAIL_Z:
    for cx in BAR_X:
        for sy in (-1.0, 1.0):
            rivet = bmesh.ops.create_uvsphere(
                bm, u_segments=8, v_segments=5, radius=RIVET_R)["verts"]
            for v in rivet:
                v.co = Vector((cx + v.co.x, sy * RAIL_HY + v.co.y * 0.45,
                               cz + v.co.z))

bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-6)
bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])

# ============================================================================
# UVs — dominant-axis projection, which IS valid here
# ============================================================================
# The rule that bites elsewhere (a swept or revolved surface turns its normal
# 90 degrees mid-surface, the dominant axis flips, and the texture seams) needs
# a curved surface to bite on. Everything here is box-ish: bars, rails and a
# beam are flat-sided, and the only tilted faces are the spikes' four facets,
# each of which is planar and keeps one dominant axis over its whole area.
uv = bm.loops.layers.uv.verify()
for f in bm.faces:
    nx, ny, nz = (abs(c) for c in f.normal)
    for loop in f.loops:
        co = loop.vert.co
        if nz >= nx and nz >= ny: p = (co.x, co.y)
        elif nx >= ny:            p = (co.y, co.z)
        else:                     p = (co.x, co.z)
        loop[uv].uv = (p[0] / TILE, p[1] / TILE)

mesh = bpy.data.meshes.new("portcullis_leaf")
bm.to_mesh(mesh)
bm.free()
obj = bpy.data.objects.new("portcullis_leaf", mesh)
bpy.context.scene.collection.objects.link(obj)

co = [v.co for v in mesh.vertices]
print(f"BuildPortcullis: {N_BAR} bars of {BAR_H * 2 * 250:.1f} cm at "
      f"{GAP * 250:.1f} cm gaps, {len(co)} verts, "
      f"x {min(c.x for c in co):+.4f}..{max(c.x for c in co):+.4f}  "
      f"y {min(c.y for c in co):+.4f}..{max(c.y for c in co):+.4f}  "
      f"z {min(c.z for c in co):+.4f}..{max(c.z for c in co):+.4f}")

# ============================================================================
# the contract, checked rather than trusted
# ============================================================================
# THE OPENING (constraint 1). A lattice narrower than its frame shows as a lit
# sliver down one jamb, which reads as a lighting fault rather than a dimension
# error and is very hard to chase.
assert abs(min(c.x for c in co) + OPEN) < 1e-4, "the lattice misses the left jamb"
assert abs(max(c.x for c in co) - OPEN) < 1e-4, "the lattice misses the right jamb"
assert abs(max(c.z for c in co) - DOOR_H) < 1e-4, "the lattice is not the door's height"
assert abs(min(c.z for c in co)) < 1e-4, "the spikes do not reach the floor"

# THE MORTICE (constraint 2). Thicker than its slot and the door jams on the
# way up — and it would jam INVISIBLY, since the leaf is inside the stone by
# then and all that shows is a portcullis that stopped.
thick = max(abs(c.y) for c in co)
assert thick < SLOT_T - 0.002, (
    f"the lattice is {thick:.4f} thick, past the mortice's {SLOT_T:.4f}")

# RAISED, IT IS GONE (constraint 3). The spikes are the lowest thing on it, so
# they are what has to clear the ceiling.
assert min(c.z for c in co) + TRAVEL >= TOP - 1e-6, (
    f"travel {TRAVEL} leaves the spikes {TOP - TRAVEL:.3f} below the ceiling")

# IT READS AS OPEN (constraint 4). Not a modelling check but a design one: the
# number of bars is a free choice and the gap is not, so the gap is what is
# stated and the count follows.
assert 0.06 < GAP < 0.09, f"a {GAP * 250:.0f} cm gap is not a portcullis"

# CLOSED. Every edge wants exactly two faces. Bars, rails and rivets INTERSECT
# — which is fine, and invisible, since neither is opened by the other — but
# each solid must be a shell in its own right, or recalc_face_normals had
# nothing to work from and the winding above is unchecked.
check = bmesh.new()
check.from_mesh(mesh)
n_boundary = len([e for e in check.edges if len(e.link_faces) != 2])
check.free()
assert n_boundary == 0, f"{n_boundary} boundary edges — a solid is not closed"

if EXPORT:
    bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
    print(f"BuildPortcullis: wrote {OUT}")
else:
    print("BuildPortcullis: built in the bridge, not exported")
