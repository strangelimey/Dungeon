# ============================================================================
# tools/BuildStatue.py — authors the standing figure statues, UNIT-SPACE .glb.
#
#   blender --background --factory-startup --python tools\BuildStatue.py -- <out.glb> [figure]
#   AssetBaker import-model <out.glb> <assets> statue_<figure> --raw --texture-set <stone>
#
# Replaces ModelBaker's BuildStatue(), which was six axis-aligned boxes and a
# revolved head. Michael: "it looks very blocky. Can we make it look like a
# human?" It cannot be fixed by adding boxes — a human reads by its SILHOUETTE
# and boxes have the wrong one at every height.
#
# ONE FIGURE, NOT FOUR — and the reason is the useful part of this file.
#
# The plan was four: this hooded sentinel, an armoured knight, a classical
# draped figure and a bare-headed mourner, sharing a plinth and a lofter. The
# sentinel works. The knight was built and REJECTED on sight — Michael: "that
# looks like a nutcracker wearing an acorn as a hat" — and the other two are not
# being attempted procedurally.
#
# WHY THE ROBE WORKS AND THE ARMOUR DOES NOT, since the two look like the same
# job. Drapery IS a lofted surface: a smooth column with a periodic ripple, and
# both of those are things a profile lofter expresses natively and exactly.
# Plate armour is a set of hard-edged shells — pauldron, breastplate, fauld,
# greave — whose entire appeal is the crispness of the joints between them, and
# a stack of lofted sections renders that as a stack of pots. Bare anatomy fails
# the same way: procedural musculature goes wrong fast, and a blank modelled
# face is worse than no face.
#
# IT IS THE SAME WALL AS THE HANDS, ONE LEVEL UP. Down there, every feature
# small enough to be a hand's feature was too small to resolve. Here, every
# feature that would make armour read as armour is a hard edge a lofter cannot
# put where it needs to be. The answer both times is not more stations.
#
# THE DECISION (2026-08-07): the remaining figures wait on a BOUGHT OR SCULPTED
# base mesh, imported through the FetchModels path and dressed with this file's
# plinth and plaque. That is what tools\FetchModels.ps1 exists for. The knight
# attempt is preserved in the branch's stash rather than in this file, because a
# rejected shape is worth remembering as a sentence, not as dead code.
#
# AUTHORED IN METRES, SCALED TO UNITS AT THE END. Anatomy is the one thing in
# this codebase easier to reason about in real dimensions — a 1.85 m figure, a
# 0.45 m shoulder span — so the whole build runs in metres and the last step
# divides by kUnit. Every OTHER Build*.py emits units directly; this is the
# exception and the scale assert at the bottom is what keeps it honest.
#
# Z is up here (Blender); the engine's Y. 1 unit = one square = 2.5 m.
# ============================================================================
import math
import sys

import bmesh
import bpy

KUNIT = 2.5          # metres per dungeon square — Game/DungeonMap.h kUnit

# SEGMENTS MUST BE SAMPLED AGAINST THE FOLDS, NOT AGAINST THE SILHOUETTE. At 28
# segments and 9 folds this was 3.1 samples per fold, and a cosine sampled three
# times a period is not a curve, it is a triangle wave — which is exactly what
# Michael saw: "it looks like it's made of long triangles". A smooth body would
# have been fine at 28; the ripple is what sets the floor, and 7 samples a fold
# is where it stops reading as faceting.
SEG = 64             # segments around the body
HEAD_SEG = 32        # the head carries no folds, so it needs far fewer
FOLDS = 9            # drapery folds around the robe
FOLD_DEPTH = 0.055   # how far a fold stands proud, in metres, at the hem

# Faces meeting at less than this are shaded smooth; sharper ones are split so
# they stay crisp. See the shading block near the end.
SMOOTH_ANGLE = math.radians(38.0)

# TWO WAYS TO RUN THIS, and the difference is only whether it exports.
#
#   headless:  blender --background ... --python BuildStatue.py -- <out.glb> [figure]
#   live:      python tools\bsend.py -f tools\BuildStatue.py
#
# The live path builds into the running bridge Blender and writes NOTHING, so
# the shape can be turned around in the viewport before it costs an import and
# a game relaunch. Michael, after a bad one reached the game: "let's start using
# Blender, not headless, so I can see them in Blender before we put them in the
# game." Judging shape through a 90-second round trip was never the right loop.
#
# The bridge shares one namespace across snippets, so a preceding
#   python tools\bsend.py -c "STATUE_FIGURE = 'knight'"
# picks the figure where the headless path uses argv.
args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
EXPORT = bool(args)
OUT = args[0] if args else "statue.glb"
FIGURE = args[1] if len(args) > 1 else globals().get("STATUE_FIGURE", "sentinel")

# Leave whatever mode the last look ended in. Under the bridge this script runs
# against a Blender someone has been INSPECTING — very often still in Edit Mode
# from a select-linked — and select_all/delete both refuse to poll there.
if bpy.context.mode != "OBJECT":
    bpy.ops.object.mode_set(mode="OBJECT")
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()

bm = bmesh.new()


# --- construction helpers ---------------------------------------------------
# WINDING IS STATED, NOT RECALCULATED (CLAUDE.md's trap: recalc_face_normals
# needs connectivity and this file is a pile of disjoint islands).
#
# DERIVED HERE, IN THIS FRAME, and that qualifier is the lesson. The first cut
# reused the winding from BuildHangingChain's sweep, which is correct there —
# but that helper builds its sections on a (B, N) frame carried along a tangent,
# while these are horizontal rings in Blender's Z-up, and the two disagree by a
# sign. Michael's verdict on the result: "it looks inside out". It was.
#
# For section P(t) = (r cos t, r sin t, z) stacked upward, dtheta =
# (-r sin t, r cos t, 0) and up = (0, 0, 1), so cross(up, dtheta) = -r * (cos t,
# sin t, 0) — INWARD. The outward quad is therefore the other diagonal order:
# (i,j) -> (i,k) -> (i+1,k) -> (i+1,j), whose normal is cross(dtheta, up).
# A measured majority check at the bottom of the file keeps this honest.
def stack(rings, close_bottom=False, close_top=False):
    for i in range(len(rings) - 1):
        r0, r1 = rings[i], rings[i + 1]
        n = len(r0)
        for j in range(n):
            k = (j + 1) % n
            # A ring collapsed to a point (a pole) degenerates to a triangle.
            if r0[j] is r0[k]:
                bm.faces.new((r0[j], r1[k], r1[j]))
            elif r1[j] is r1[k]:
                bm.faces.new((r0[j], r0[k], r1[j]))
            else:
                bm.faces.new((r0[j], r0[k], r1[k], r1[j]))
    if close_bottom:
        fan(rings[0], up=False)
    if close_top:
        fan(rings[-1], up=True)


def fan(ring, up):
    """A cap. For a ring ordered counter-clockwise seen from above, (c, Pj, Pk)
    faces +Z — so the TOP takes that order and the bottom takes the reverse."""
    c = bm.verts.new((sum(v.co.x for v in ring) / len(ring),
                      sum(v.co.y for v in ring) / len(ring),
                      sum(v.co.z for v in ring) / len(ring)))
    for j in range(len(ring)):
        k = (j + 1) % len(ring)
        bm.faces.new((c, ring[j], ring[k]) if up else (c, ring[k], ring[j]))


def ring_at(z, radius, depth_scale=1.0, seg=SEG, fold=0.0, cx=0.0, cy=0.0, t0=0.0,
            front_flat=0.0):
    """One horizontal section.

    `depth_scale` squashes the section front-to-back — a body is much wider
    than it is deep, and a circular section is most of what made the old statue
    read as a lathe-turned post rather than a person.

    `fold` ripples the radius with cos(FOLDS * theta). THIS IS THE DETAIL THAT
    MAKES CLOTH: without it a lofted robe is a smooth vase, and no amount of
    proportion fixes that. It fades to zero at the shoulders, where cloth is
    pulled taut over the body.

    `t0` rotates the section. It exists for the PLINTH: a 4-segment loft puts
    its vertices at 0/90/180/270 degrees, so the square lands as a diamond with
    a corner pointing where the figure faces — the figure ends up facing its own
    base's diagonal. Turning the base by 45 degrees squares the two up AND puts
    the plinth's flat faces parallel to the cell walls. It has to be the base
    that turns, not the figure: movement, line of sight and projectiles are all
    4-cardinal, so a figure facing a diagonal would face nothing the game has.

    `front_flat` presses the FRONT of the section back and damps its pleats,
    leaving the sides and back alone. It exists because a robe that is elliptical
    all the way round swallows anything the figure holds in front of it: the
    sentinel's blade sits at y = 0.150 with its back face at 0.112, while the
    hem reached 0.186 and further at a fold peak, so the lower blade was inside
    the cloth. Scaling the whole section down instead would have thinned the
    figure from every angle to fix a problem that only exists at the front.
    """
    verts = []
    for j in range(seg):
        t = t0 + 2.0 * math.pi * j / seg
        # 1 at the sides and back, falling to (1 - front_flat) dead ahead.
        front = 1.0 - front_flat * max(math.sin(t), 0.0)
        r = radius + fold * front * math.cos(FOLDS * t)
        verts.append(bm.verts.new((cx + r * math.cos(t),
                                   cy + r * depth_scale * front * math.sin(t), z)))
    return verts


def loft(stations, depth_scale=1.0, seg=SEG, close_bottom=True, close_top=True,
         t0=0.0, front_flat=0.0):
    """stations: (z, radius, fold_amount, cx, cy)."""
    rings = [ring_at(z, r, depth_scale, seg, f, cx, cy, t0, front_flat)
             for (z, r, f, cx, cy) in stations]
    stack(rings, close_bottom, close_top)
    return rings


def tube(path, radii, seg=10, flat=1.0):
    """A circular-section tube along an arbitrary 3D path.

    Unlike BuildHangingChain's sweep this does NOT need a planar path, and the
    reason is worth stating: the section is a CIRCLE, so a twist about the
    tangent produces the identical surface. Any continuously varying frame will
    do, and Gram-Schmidt against a fixed reference is continuous as long as the
    tangent never lines up with it — arms and blades here run mostly along Z, so
    the reference is X.

    `flat` squashes the section along the SECOND basis axis, turning the circle
    into a lens. It exists for the sword: an arm is a pole and wants a round
    section, but a blade is wide and thin, and one helper drawing both is what
    made the sword read as "a round pole rather than a flat blade". Note this is
    also the one case where the twist argument above stops holding — a squashed
    section is NOT rotationally symmetric, so it depends on the frame being what
    the comment says. For a vertical path it works out as u = +X and v = -Y, so
    the width lands across the figure and the thickness front-to-back.
    """
    ref = (1.0, 0.0, 0.0)
    rings = []
    for i, p in enumerate(path):
        a = path[max(i - 1, 0)]
        b = path[min(i + 1, len(path) - 1)]
        t = [b[k] - a[k] for k in range(3)]
        tl = math.sqrt(sum(c * c for c in t)) or 1.0
        t = [c / tl for c in t]
        d = sum(ref[k] * t[k] for k in range(3))
        u = [ref[k] - d * t[k] for k in range(3)]
        ul = math.sqrt(sum(c * c for c in u)) or 1.0
        u = [c / ul for c in u]
        v = [t[1] * u[2] - t[2] * u[1], t[2] * u[0] - t[0] * u[2],
             t[0] * u[1] - t[1] * u[0]]
        ring = []
        for j in range(seg):
            th = 2.0 * math.pi * j / seg
            c, s = math.cos(th) * radii[i], math.sin(th) * radii[i] * flat
            ring.append(bm.verts.new((p[0] + u[0] * c + v[0] * s,
                                      p[1] + u[1] * c + v[1] * s,
                                      p[2] + u[2] * c + v[2] * s)))
        rings.append(ring)
    stack(rings, close_bottom=True, close_top=True)
    return rings


# Faces that must stay FLAT-shaded whatever the angle threshold decides. The
# threshold is a good default for a body — cloth and skin curve, so anything
# that turns hard is meant to be an edge — but it is the wrong instrument for
# ARCHITECTURE, where a face is flat because it was cut flat, not because of how
# far it turns from its neighbour. A plinth that shades smooth stops reading as
# dressed stone however square its silhouette is (Michael: "keep the plinth very
# square. It is now a bit rounded").
HARD = set()


def add_face(vs):
    """Make a face, dropping repeated vertices and skipping what degenerates.

    Needed because the hood's angular gap CLOSES at the top: a ring whose gap is
    zero has its two ends welded to one vertex, so the quads there collapse to
    triangles or to nothing. Handling it here keeps the gap a continuous
    parameter instead of forcing the shell to be built in two pieces."""
    out = []
    for v in vs:
        if v not in out:
            out.append(v)
    if len(out) >= 3:
        try:
            bm.faces.new(out)
        except ValueError:
            pass  # the face already exists


def arc_ring(z, radius, depth_scale, a0, a1, seg, cy=0.0):
    """An OPEN arc of a section — a ring with a slice missing.

    A span of exactly 2*pi WELDS the ends to one vertex rather than leaving two
    coincident ones, so a closed ring stitches to its open neighbours cleanly."""
    full = abs((a1 - a0) - 2.0 * math.pi) < 1e-9
    verts = []
    for j in range(seg if full else seg + 1):
        t = a0 + (a1 - a0) * j / seg
        verts.append(bm.verts.new((radius * math.cos(t),
                                   cy + radius * depth_scale * math.sin(t), z)))
    if full:
        verts.append(verts[0])
    return verts


def stack_open(rings, flip=False):
    """Stack arc rings without wrapping j. `flip` reverses the winding, which is
    how the same stations serve as a shell's inner surface."""
    for i in range(len(rings) - 1):
        r0, r1 = rings[i], rings[i + 1]
        for j in range(len(r0) - 1):
            k = j + 1
            add_face((r0[j], r1[j], r1[k], r0[k]) if flip
                     else (r0[j], r0[k], r1[k], r1[j]))


def box(x0, x1, y0, y1, z0, z1):
    """An axis-aligned box. Always HARD — a box has no curved surface on it, so
    every one of its faces is flat by construction."""
    v = [bm.verts.new(p) for p in (
        (x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
        (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1))]
    for f in ((0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
              (2, 3, 7, 6), (1, 2, 6, 5), (3, 0, 4, 7)):
        HARD.add(bm.faces.new([v[i] for i in f]))


# --- the plinth -------------------------------------------------------------
# Square, stepped, and lofted at 4 segments — the same profile lofter the round
# work uses, which is the whole point of a lofter that takes a segment count.
PLINTH_TOP = 0.46
# t0 = 45 degrees so the square's FACES point along the axes rather than its
# corners — see ring_at. Rotating a square does NOT change its size, so the
# radii are unchanged by the turn; they are only scaled up from the original
# 0.46 so the hem's fold peaks (0.355 m at their widest) clear the base's flat
# half-width, which is radius * cos(45) rather than the radius itself.
_before = set(bm.faces)
loft([(0.00, 0.52, 0.0, 0, 0), (0.10, 0.52, 0.0, 0, 0),
      (0.12, 0.475, 0.0, 0, 0), (0.36, 0.452, 0.0, 0, 0),
      (0.38, 0.52, 0.0, 0, 0), (PLINTH_TOP, 0.509, 0.0, 0, 0)],
     depth_scale=1.0, seg=4, t0=math.pi * 0.25)
HARD |= set(bm.faces) - _before  # dressed stone, flat on every face

# --- the plaque -------------------------------------------------------------
# A framed panel on the FRONT face of the plinth, centred, with a sunken field
# for an inscription. Built at +Y because the whole figure is authored facing
# +Y and turned at the end — put it at -Y and the 180 would carry it round to
# the back.
#
# The plinth's front face is not vertical: it tapers from y = 0.336 at the
# bottom of the die to 0.320 at the top. So the plaque's BACK is set at 0.310,
# behind the face everywhere in its range, and it is the FRONT that is flat.
# Sitting it on the surface instead would have it half-buried at one end and
# floating at the other.
#
# Five boxes rather than a boolean: four rim pieces around a recessed field.
# The rim stands 37 mm proud of the plaque's back and the field only 28, so the
# 9 mm step is what reads as a border. box() registers everything it makes as
# HARD, so the whole plaque stays crisply flat-shaded like the plinth.
PLAQUE_BACK, PLAQUE_FACE, PLAQUE_FIELD = 0.310, 0.347, 0.338
PL_X, PL_Z0, PL_Z1 = 0.155, 0.165, 0.315
PL_RIM = 0.015
box(-PL_X, PL_X, PLAQUE_BACK, PLAQUE_FACE, PL_Z1 - PL_RIM, PL_Z1)     # rim, top
box(-PL_X, PL_X, PLAQUE_BACK, PLAQUE_FACE, PL_Z0, PL_Z0 + PL_RIM)     # rim, bottom
box(-PL_X, -PL_X + PL_RIM, PLAQUE_BACK, PLAQUE_FACE,
    PL_Z0 + PL_RIM, PL_Z1 - PL_RIM)                                   # rim, left
box(PL_X - PL_RIM, PL_X, PLAQUE_BACK, PLAQUE_FACE,
    PL_Z0 + PL_RIM, PL_Z1 - PL_RIM)                                   # rim, right

# --- the inscription --------------------------------------------------------
# VAGUE RUNES, NOT LETTERING. Real text would need a font converted to a mesh,
# which at 4 mm stroke width costs more geometry than the entire rest of the
# statue for something unreadable from two metres in torchlight. Marks that only
# have to say "something is written here" cost a few hundred faces, and the
# actual words live in the game, surfaced by clicking the plaque.
#
# CARVED WITH A BOOLEAN, AND ISOLATED TO THIS BOX. The field is differenced
# against the rune prisms as its OWN object and only then merged back, because
# the robe and the head are deliberately open-boundary lofts (close_top /
# close_bottom are False so they can meet each other) and an exact boolean over
# non-manifold input is a coin toss. A closed box against a handful of closed
# prisms is not.
RUNE_ROWS, RUNE_COLS = 2, 7
RUNE_DEPTH = 0.004        # how deep the marks cut into the field
RUNE_HALF_W = 0.0022      # half the stroke width


def _rnd(i):
    """A tiny deterministic hash — the same runes bake every time."""
    x = (i * 1103515245 + 12345) & 0x7FFFFFFF
    x ^= x >> 13
    return (x * 1274126177) & 0x7FFFFFFF


# Strokes in a rune's own 0..1 box. A stave plus branches off it, which is what
# makes a mark read as runic rather than as a scratch.
RUNE_STROKES = [
    ((0.5, 0.0), (0.5, 1.0)), ((0.5, 0.58), (1.0, 0.95)),
    ((0.5, 0.58), (0.0, 0.95)), ((0.5, 0.42), (1.0, 0.05)),
    ((0.5, 0.42), (0.0, 0.05)), ((0.0, 0.0), (1.0, 1.0)),
    ((0.0, 1.0), (1.0, 0.0)), ((0.0, 0.5), (1.0, 0.5)),
    ((0.15, 0.0), (0.15, 1.0)), ((0.85, 0.0), (0.85, 1.0)),
]

_cut = bmesh.new()


def _prism(x0, z0, x1, z1, y0, y1):
    """A thin slab along the segment (x0,z0)-(x1,z1), extruded through y."""
    dx, dz = x1 - x0, z1 - z0
    ln = math.hypot(dx, dz) or 1.0
    nx, nz = -dz / ln * RUNE_HALF_W, dx / ln * RUNE_HALF_W
    corners = [(x0 - nx, z0 - nz), (x1 - nx, z1 - nz),
               (x1 + nx, z1 + nz), (x0 + nx, z0 + nz)]
    lo = [_cut.verts.new((cx, y0, cz)) for (cx, cz) in corners]
    hi = [_cut.verts.new((cx, y1, cz)) for (cx, cz) in corners]
    _cut.faces.new(lo[::-1])
    _cut.faces.new(hi)
    for i in range(4):
        j = (i + 1) % 4
        _cut.faces.new((lo[i], lo[j], hi[j], hi[i]))


_fx0, _fx1 = -PL_X + PL_RIM, PL_X - PL_RIM
_fz0, _fz1 = PL_Z0 + PL_RIM, PL_Z1 - PL_RIM
_mx, _mz = 0.012, 0.012
_cw = (_fx1 - _fx0 - 2 * _mx) / RUNE_COLS
_ch = (_fz1 - _fz0 - 2 * _mz) / RUNE_ROWS
for _row in range(RUNE_ROWS):
    for _col in range(RUNE_COLS):
        _seed = _rnd(_row * 31 + _col + 7)
        _ox = _fx0 + _mx + _col * _cw
        _oz = _fz0 + _mz + _row * _ch
        _picks = [0] + [1 + (_rnd(_seed + k) % (len(RUNE_STROKES) - 1))
                        for k in range(1 + _seed % 2)]
        for _s in set(_picks):
            (_u0, _v0), (_u1, _v1) = RUNE_STROKES[_s]
            _prism(_ox + _u0 * _cw * 0.72 + _cw * 0.14,
                   _oz + _v0 * _ch * 0.74 + _ch * 0.13,
                   _ox + _u1 * _cw * 0.72 + _cw * 0.14,
                   _oz + _v1 * _ch * 0.74 + _ch * 0.13,
                   PLAQUE_FIELD - RUNE_DEPTH, PLAQUE_FIELD + 0.012)

# The field itself, as its own mesh, then cut.
_fld = bmesh.new()
_fv = [_fld.verts.new(p) for p in (
    (_fx0, PLAQUE_BACK, _fz0), (_fx1, PLAQUE_BACK, _fz0),
    (_fx1, PLAQUE_FIELD, _fz0), (_fx0, PLAQUE_FIELD, _fz0),
    (_fx0, PLAQUE_BACK, _fz1), (_fx1, PLAQUE_BACK, _fz1),
    (_fx1, PLAQUE_FIELD, _fz1), (_fx0, PLAQUE_FIELD, _fz1))]
for _f in ((0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
           (2, 3, 7, 6), (1, 2, 6, 5), (3, 0, 4, 7)):
    _fld.faces.new([_fv[i] for i in _f])

# A BOOLEAN CARES ABOUT WINDING, and an inverted cutter means "everything
# except this prism" — differencing that deletes the whole field, which is
# exactly what happened first time round (0 faces out). recalc_face_normals is
# safe HERE, unlike the trap CLAUDE.md warns about: each prism and the field are
# closed boxes built from SHARED vertices, so the operator has the connectivity
# it needs to decide. It is the disjoint face soup elsewhere in this file that
# it cannot handle.
bmesh.ops.recalc_face_normals(_cut, faces=_cut.faces[:])
bmesh.ops.recalc_face_normals(_fld, faces=_fld.faces[:])

_fld_mesh = bpy.data.meshes.new("plaque_field")
_fld.to_mesh(_fld_mesh)
_fld.free()
_cut_mesh = bpy.data.meshes.new("plaque_runes")
_cut.to_mesh(_cut_mesh)
_cut.free()
_fld_obj = bpy.data.objects.new("plaque_field", _fld_mesh)
_cut_obj = bpy.data.objects.new("plaque_runes", _cut_mesh)
bpy.context.scene.collection.objects.link(_fld_obj)
bpy.context.scene.collection.objects.link(_cut_obj)
_mod = _fld_obj.modifiers.new("rune_cut", "BOOLEAN")
_mod.operation = "DIFFERENCE"
_mod.object = _cut_obj
_mod.solver = "EXACT"
# THE CUTTER SELF-INTERSECTS BY DESIGN — every rune's branches run into its own
# stave — and the exact solver assumes operands do not unless told. Without this
# it returned an EMPTY mesh rather than an error, which reads as "the boolean
# silently did nothing" and sent me looking at winding and depsgraph staleness
# first. Overlapping cutter volumes are the normal case for carving marks.
_mod.use_self = True
_mod.use_hole_tolerant = True

# EVALUATE THE MODIFIER, DO NOT APPLY IT. bpy.ops.object.modifier_apply is an
# operator with a poll that depends on selection and context, and under the
# bridge it silently did nothing — the boolean contributed zero faces and the
# plaque came out blank. Reading the evaluated depsgraph asks the same solver
# for the same result without needing any of that context to be right.
print(f"  plaque in: field {len(_fld_mesh.polygons)} faces, "
      f"cutter {len(_cut_mesh.polygons)} faces")
# The objects were linked a moment ago; without this the depsgraph the bridge
# hands back can still be the one from before they existed.
bpy.context.view_layer.update()
_dg = bpy.context.evaluated_depsgraph_get()
_eval = _fld_obj.evaluated_get(_dg)
_result = _eval.to_mesh()
_before = set(bm.faces)
bm.from_mesh(_result)
HARD |= set(bm.faces) - _before   # dressed stone, like the rest of the plinth
print(f"  plaque: {len(_result.polygons)} faces after the rune cut")
_eval.to_mesh_clear()

bpy.data.objects.remove(_cut_obj, do_unlink=True)
bpy.data.objects.remove(_fld_obj, do_unlink=True)
bpy.data.meshes.remove(_cut_mesh)
bpy.data.meshes.remove(_fld_mesh)

# --- the figure -------------------------------------------------------------
# Proportions are a real standing adult: 1.85 m to the crown, shoulders at
# 1.52, waist at 1.18. The robe flares from the waist down and is pulled taut
# across the shoulders, which is why the fold amount fades upward.
SHOULDER_Z = PLINTH_TOP + 1.06
CROWN_Z = PLINTH_TOP + 1.39

robe = [
    (PLINTH_TOP,        0.30, FOLD_DEPTH,        0, 0),   # hem, on the plinth
    (PLINTH_TOP + 0.14, 0.29, FOLD_DEPTH,        0, 0),
    (PLINTH_TOP + 0.39, 0.26, FOLD_DEPTH * 0.9,  0, 0),   # knees
    (PLINTH_TOP + 0.59, 0.245, FOLD_DEPTH * 0.7, 0, 0),   # hips
    (PLINTH_TOP + 0.72, 0.205, FOLD_DEPTH * 0.45, 0, 0),  # waist
    (PLINTH_TOP + 0.89, 0.235, FOLD_DEPTH * 0.2, 0, 0),   # chest
    (SHOULDER_Z,        0.255, 0.0,              0, 0),   # shoulders
    (SHOULDER_Z + 0.05, 0.175, 0.0,              0, 0),   # the shoulder slope
    (SHOULDER_Z + 0.10, 0.085, 0.0,              0, 0),   # neck
]
# 0.45 is sized off the worst case, the hem: 0.186 m of front becomes 0.102,
# clearing the blade's back face at 0.112. Everywhere higher has more room than
# that, so one value serves the whole robe.
ROBE_FRONT_FLAT = 0.45
loft(robe, depth_scale=0.62, close_top=False, front_flat=ROBE_FRONT_FLAT)

# The head: an ovoid on the neck, slightly forward of the body's axis the way a
# real head sits. Not a face — see the header.
HEAD_Z = SHOULDER_Z + 0.10
head = [
    (HEAD_Z,        0.075, 0.0, 0, 0.010),
    (HEAD_Z + 0.05, 0.098, 0.0, 0, 0.014),
    (HEAD_Z + 0.13, 0.108, 0.0, 0, 0.014),
    (HEAD_Z + 0.21, 0.098, 0.0, 0, 0.010),
    (HEAD_Z + 0.27, 0.060, 0.0, 0, 0.006),
    (HEAD_Z + 0.29, 0.020, 0.0, 0, 0.004),
]
loft(head, depth_scale=0.86, seg=HEAD_SEG, close_bottom=False)

if FIGURE == "sentinel":
    # THE HOOD, and the thing that makes it a hood rather than a bucket is the
    # OPENING. The first cut swept the cowl through a full 360 degrees, which
    # closes over the face — Michael: "it looks like it has a basket on its
    # head." Quite right: a closed revolve around a head IS a basket.
    #
    # So it is swept through an ARC with a gap at the front, and given real
    # THICKNESS — an outer surface, an inner surface wound the other way, and
    # stitched rims. A single-sided arc would be cheaper and would even look
    # passable head-on, but the cowl would vanish from inside and the figure
    # would be hollow from three-quarters behind.
    HOOD_GAP = math.radians(104.0)   # the face opening at its widest
    HOOD_TH = 0.026                  # cloth thickness
    HOOD_SEG = 22

    # THE GAP IS A PROFILE, NOT A CONSTANT, and that is the difference between a
    # hood and a tube. A constant gap runs the opening the full height of the
    # shell, so there is no BRIM — nothing comes down over the forehead, and the
    # cowl reads as floating around the head rather than worn on it. Michael:
    # "bring the brim down onto the forehead ... tighten the cowl around the
    # head." So the gap closes to zero just above the brow, which both makes a
    # brim and turns the opening into a bounded lens instead of a slot.
    #
    # Radii are now the head's own plus about 12 mm of clearance and the cloth
    # thickness on top, where they used to stand 60-70 mm off it; and the
    # back-shift is 5 mm to 58 mm where it was 15 to 92. A cowl touches.
    #
    # Stations: (z, outer radius, back-shift, gap fraction).
    hood = [
        (SHOULDER_Z - 0.03, 0.250, +0.000, 1.00),  # the mantle, over the shoulders
        (SHOULDER_Z + 0.09, 0.195, -0.008, 1.00),
        (HEAD_Z + 0.02,     0.152, -0.014, 1.00),  # beside the jaw
        (HEAD_Z + 0.13,     0.146, -0.018, 1.00),  # the widest of the face
        (HEAD_Z + 0.21,     0.136, -0.024, 0.55),  # the brow — the gap narrows
        (HEAD_Z + 0.26,     0.112, -0.032, 0.00),  # BRIM: closed over the forehead
        (HEAD_Z + 0.31,     0.072, -0.045, 0.00),
        (HEAD_Z + 0.35,     0.030, -0.058, 0.00),  # the peak, thrown back
    ]

    def hood_arc(r, cy, frac):
        # +Y (t = 90 degrees) is the front the figure faces, so the arc starts
        # just past the front and comes round to just short of it. frac = 0
        # spans the full circle and arc_ring welds the seam.
        half = HOOD_GAP * 0.5 * frac
        return (math.pi * 0.5 + half, math.pi * 0.5 - half + 2.0 * math.pi)

    outer, inner = [], []
    fracs = [frac for (_z, _r, _cy, frac) in hood]
    for (z, r, cy, frac) in hood:
        a0, a1 = hood_arc(r, cy, frac)
        outer.append(arc_ring(z, r, 0.94, a0, a1, HOOD_SEG, cy))
        inner.append(arc_ring(z, max(r - HOOD_TH, 0.010), 0.94, a0, a1, HOOD_SEG, cy))
    stack_open(outer)
    stack_open(inner, flip=True)
    # Stitch the shell: the two rims either side of the face opening, the hem,
    # and the peak.
    #
    # THE RIM ONLY EXISTS WHERE THE GAP DOES. Above the brim both ends of a ring
    # are welded to the same vertex, so stitching a rim there joins the outer
    # surface to the inner one along the front centreline — a fin standing up
    # over the crown, which is why the hood grew a parting and read as hair.
    # The pair that STRADDLES the closure is still stitched: that one is real,
    # and it is what caps the top of the opening.
    for i in range(len(outer) - 1):
        if fracs[i] == 0.0 and fracs[i + 1] == 0.0:
            continue
        add_face((outer[i][0], inner[i][0], inner[i + 1][0], outer[i + 1][0]))
        add_face((outer[i][-1], outer[i + 1][-1], inner[i + 1][-1], inner[i][-1]))
    for j in range(len(outer[0]) - 1):
        add_face((outer[0][j], outer[0][j + 1], inner[0][j + 1], inner[0][j]))
    for j in range(len(outer[-1]) - 1):
        add_face((outer[-1][j], inner[-1][j], inner[-1][j + 1], outer[-1][j + 1]))

# --- the arms ---------------------------------------------------------------
# Swept tubes from the shoulder, down the side and forward to meet at the
# waist. The hands CLASPING is what makes procedural arms look deliberate —
# arms that merely hang read as sausages, because nothing explains their pose.
HAND_Z = PLINTH_TOP + 0.74
HAND_Y = 0.150        # the plane the hands work in, forward of the body

# THE ARM IS A TUBE, AND SO IS THE HAND — deliberately stylised, no anatomy.
#
# Three attempts said this was the right answer. A lofted disc bridging both
# hands read as a ring threaded on the sword; separate fists left a gap at the
# wrist; and a wrist-pinch-then-swell profile, which is anatomically the right
# shape, came out as "melted blobs". The lesson is about SCALE, not craft: a
# hand is 10 cm on a figure seen from two metres in torchlight, and every
# feature small enough to be a hand's feature is too small to resolve — so it
# reads only as lumpiness. A clean tapering tube reads as an arm and reads as
# deliberate, which a half-resolved fist never will.
#
# So: a gentle monotonic taper from shoulder to grip and nothing else. If hands
# ever want real form, the answer is a bought or sculpted mesh, not more
# stations here.
# EVERY COORDINATE HERE IS MONOTONIC, and that is the whole specification. The
# tube's shape is only ever as good as its path, and two faults were hiding in
# the old one:
#
#   * a ZIGZAG. The elbow sat at z = 1.22 and the station after it at 1.285, so
#     the arm descended to the elbow and then climbed again before dropping to
#     the grip. The elbow was also level with the hands, which is why nothing
#     about the pose looked like an arm however the radii were tuned.
#   * a SECOND BEND past the grip, where the path turned from coming inward to
#     running straight down — Michael read it as fingers, because a bend there
#     is where fingers would be.
#
# Now z falls 1.51 -> 1.40 -> 1.30 -> 1.255 -> 1.19 without reversing, y comes
# forward the whole way, and x bulges at the elbow then draws in. One curve, one
# direction, ending ON the grip with nothing after it.
for side in (-1.0, 1.0):
    arm = [
        (side * 0.215, -0.005, SHOULDER_Z - 0.010),  # shoulder
        (side * 0.245, +0.010, SHOULDER_Z - 0.120),  # upper arm
        (side * 0.238, +0.040, SHOULDER_Z - 0.220),  # elbow
        (side * 0.160, +0.105, HAND_Z + 0.055),      # forearm
        (side * 0.048, HAND_Y, HAND_Z - 0.010),      # the hand, on the grip
    ]
    tube(arm, [0.062, 0.058, 0.055, 0.047, 0.042])

# --- the attribute ----------------------------------------------------------
if FIGURE == "sentinel":
    # A sword held point-down through the clasped hands.
    #
    # THE ORDER, top to bottom, is POMMEL, GRIP, CROSSGUARD, BLADE. The first
    # cut had the pommel below the hands and the guard above them — exactly
    # inverted, which is a sword held point-UP with its parts in the wrong
    # places. Michael: "the sword hilt/cross guard is off." The hands grip the
    # GRIP, so the guard has to sit just under them, between fist and blade.
    SWORD_Y = HAND_Y                       # the blade shares the hands' plane
    GUARD_Z = HAND_Z - 0.075               # BELOW the hands
    # The pommel sits higher than a real sword's would. The clasped hands cover
    # most of the grip — correctly, that is what hands do — but they also cover
    # the binding, so the hilt is lengthened to leave about 6 cm of wrap showing
    # above the fists instead of 3.
    POMMEL_Z = HAND_Z + 0.140

    blade = [
        (0.0, SWORD_Y, GUARD_Z),
        (0.0, SWORD_Y, PLINTH_TOP + 0.30),
        (0.0, SWORD_Y, PLINTH_TOP + 0.09),
        (0.0, SWORD_Y, PLINTH_TOP + 0.005),
    ]
    # Six segments and flattened to 28%: the vertices at 0 and 180 degrees
    # become the two EDGES and the rest form a face either side, which is a
    # lenticular blade with a central ridge. 76 mm wide and 18 mm thick at the
    # guard, tapering to the point.
    tube(blade, [0.038, 0.032, 0.022, 0.006], seg=6, flat=0.28)
    # The crossguard: a bar across the blade, slightly thicker at its centre
    # where it meets the hilt.
    box(-0.150, 0.150, SWORD_Y - 0.022, SWORD_Y + 0.022,
        GUARD_Z, GUARD_Z + 0.030)
    box(-0.048, 0.048, SWORD_Y - 0.030, SWORD_Y + 0.030,
        GUARD_Z - 0.014, GUARD_Z + 0.044)
    # THE GRIP, bound in leather strips. The whole statue is one stone material
    # with world-projected UVs, so a binding cannot be a texture — and should
    # not be: a marble figure's grip is CARVED, so the wrap has to be geometry.
    #
    # It is one helical ridge rather than stacked rings. The radius depends on
    # BOTH the angle and the height — cos(theta - 2*pi*WRAPS*f) — which is what
    # makes a spiral instead of a barrel of hoops, and a spiral is what a strip
    # wound round a grip actually leaves. Shallow enough (4 mm on a 24 mm
    # radius) that the smooth-shading threshold keeps it soft, the way carved
    # stone reads rather than real leather.
    GRIP_R = 0.024
    GRIP_WRAPS = 7.0
    GRIP_AMP = 0.004
    GRIP_SEG, GRIP_ROWS = 18, 30
    gz0, gz1 = GUARD_Z + 0.026, POMMEL_Z - 0.020
    grip_rings = []
    for i in range(GRIP_ROWS + 1):
        f = i / GRIP_ROWS
        z = gz0 + (gz1 - gz0) * f
        row = []
        for j in range(GRIP_SEG):
            t = 2.0 * math.pi * j / GRIP_SEG
            r = GRIP_R + GRIP_AMP * math.cos(t - 2.0 * math.pi * GRIP_WRAPS * f)
            row.append(bm.verts.new((r * math.cos(t),
                                     SWORD_Y + r * math.sin(t), z)))
        grip_rings.append(row)
    stack(grip_rings, close_bottom=True, close_top=True)

    # THE POMMEL, asymmetric: heavier at the bottom where it meets the hilt and
    # closing over the top, so it caps the grip rather than being a disc
    # threaded onto it. The grip ends INSIDE it.
    loft([(POMMEL_Z - 0.038, 0.026, 0.0, 0, SWORD_Y),
          (POMMEL_Z - 0.016, 0.040, 0.0, 0, SWORD_Y),
          (POMMEL_Z + 0.006, 0.037, 0.0, 0, SWORD_Y),
          (POMMEL_Z + 0.026, 0.023, 0.0, 0, SWORD_Y),
          (POMMEL_Z + 0.038, 0.007, 0.0, 0, SWORD_Y)],
         depth_scale=1.0, seg=14)

# --- shading ----------------------------------------------------------------
# Everything smooth, then SPLIT the edges that should stay hard. Splitting
# rather than flagging is deliberate: it duplicates the vertices along those
# edges so normals cannot average across them, which is version-independent and
# survives the glTF export — where a "sharp edge" flag is only as good as the
# exporter's handling of it.
#
# The threshold does the sorting by itself. A robe fold or the swell of a head
# turns by a few degrees per segment and stays smooth; a plinth corner, the
# crossguard's arrises and the hood's rim all turn much harder and stay crisp.
# Getting this from geometry beats tagging surfaces by hand, which is a list
# that goes stale the moment a figure is added.
for f in bm.faces:
    f.smooth = f not in HARD
sharp = [e for e in bm.edges
         if len(e.link_faces) == 2
         and (e.calc_face_angle(0.0) > SMOOTH_ANGLE
              or any(f in HARD for f in e.link_faces))]
bmesh.ops.split_edges(bm, edges=sharp)

# --- face the camera --------------------------------------------------------
# The figure is built facing +Y because that reads naturally in the source (the
# hood opens at +Y, the sword sits at +Y), but Blender's Front view looks FROM
# -Y, so an author-facing-+Y figure presents its back every time the file is
# opened. The convention is to face -Y, so turn the finished mesh.
#
# A full 180 about Z — BOTH x and y negated. Negating y alone would look
# identical on a near-symmetric figure and would be a MIRROR: determinant -1,
# so every face's winding inverts and the whole statue turns inside out. A
# proper rotation leaves winding alone, which is why the outward-facing assert
# below still means something after this runs.
for v in bm.verts:
    v.co.x = -v.co.x
    v.co.y = -v.co.y

# --- to units ---------------------------------------------------------------
bm.normal_update()

mesh = bpy.data.meshes.new("statue")
bm.to_mesh(mesh)
bm.free()
for v in mesh.vertices:
    v.co /= KUNIT

obj = bpy.data.objects.new("statue", mesh)
bpy.context.scene.collection.objects.link(obj)

# World-aligned tiling UVs. Stone is isotropic, so unlike the ceiling beam only
# the RATE matters here, not the orientation (see BuildCeilingBeam's header for
# the case where it does).
bm2 = bmesh.new()
bm2.from_mesh(mesh)
uv = bm2.loops.layers.uv.verify()
TILE = 0.22  # in units
for face in bm2.faces:
    nx, ny, nz = (abs(c) for c in face.normal)
    for loop in face.loops:
        co = loop.vert.co
        if nz >= nx and nz >= ny:   p = (co.x, co.y)
        elif nx >= ny:              p = (co.y, co.z)
        else:                       p = (co.x, co.z)
        loop[uv].uv = (p[0] / TILE, p[1] / TILE)
bm2.to_mesh(mesh)
bm2.free()

xs = [v.co.x for v in mesh.vertices]
ys = [v.co.y for v in mesh.vertices]
zs = [v.co.z for v in mesh.vertices]
print(f"BuildStatue[{FIGURE}]: {len(mesh.vertices)} verts, {len(mesh.polygons)} faces, "
      f"x {min(xs):+.3f}..{max(xs):+.3f}, z {min(zs):+.3f}..{max(zs):+.3f} (units)")

# The contract for a floor decoration, checked rather than trusted.
assert abs(min(zs)) < 1e-6, "a statue stands ON the floor; z must start at 0"
assert max(zs) < 1.0, "the statue is taller than the ceiling (kWallH)"
assert max(abs(min(xs)), max(xs)) < 0.5, "the statue overflows its square in x"
assert max(abs(min(ys)), max(ys)) < 0.5, "the statue overflows its square in y"
# The metres-to-units conversion is the one thing here that fails SILENTLY and
# invisibly — a 2.5x error just looks like a different sized statue. Pin it.
assert 0.70 < max(zs) < 0.95, f"figure height {max(zs):.3f} units is not ~1.85 m + plinth"

# WINDING, CHECKED RATHER THAN TRUSTED — this is what the first cut got wrong,
# and an inside-out figure reads as bad lighting rather than as a normals bug,
# so it can survive a look. A MAJORITY vote, not every face: a statue is
# genuinely concave in places (inside the hood, under the arms, the plinth's
# steps), so demanding every face point away from the axis would be wrong.
outward = 0
for poly in mesh.polygons:
    c = poly.center
    rl = math.hypot(c.x, c.y)
    if rl < 1e-4 or abs(poly.normal.z) > 0.85:
        continue  # near the axis, or a horizontal cap that says nothing
    if (poly.normal.x * c.x + poly.normal.y * c.y) / rl > 0.0:
        outward += 1
    else:
        outward -= 1
assert outward > 0, f"the figure is wound inside out (vote {outward})"

if EXPORT:
    bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB")
    print(f"BuildStatue[{FIGURE}]: wrote {OUT}")
else:
    print(f"BuildStatue[{FIGURE}]: built in the bridge, not exported")
