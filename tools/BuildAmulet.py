# ============================================================================
# tools/BuildAmulet.py - authors the MOONSTONE AMULET (lighting-updates Phase
# 5, the first worn item that gives light) and writes it as a .glb.
#
#   blender --background --factory-startup --python tools\BuildAmulet.py -- assets\models\moonstone_amulet.glb
#
# Like tools/BuildRock.py: an item's model is read as <model>.glb, so this
# writes one straight into assets/models, and the .glb is committed by name
# (.gitignore makes the exception) because a fresh clone must have it.
#
# EVERYTHING IS IN UNITS: 1.0 = one dungeon square (game::kUnit, 2.5 m), Z up
# here (the glTF exporter turns it Y up). Bigger than life on purpose, as the
# rock is - an item must read lying on the floor two squares off.
#
# THE SHAPE, three parts, each its own material:
#   - a silver BEZEL: an oval ring of round wire round the stone;
#   - the MOONSTONE: a domed cabochon, front and back, set in the bezel, its
#     glTF EMISSIVE a cool blue-white - the light it gives in the game is its
#     armor.cat `light` (the `moonstone` profile), this only makes it look lit;
#   - a CHAIN, drawn as a thin cord loop above it through a small bail.
# All of it lies in the XZ plane (the pendant hangs along -Z), so the floor
# lays it on its back (FloorItemWorld turns the thinnest axis up).
#
# WINDING IS THE CONTRACT (CLAUDE.md): every solid here is a bmesh primitive
# (torus-like sweeps built ring by ring, a UV sphere), emitted outward, and no
# recalc is called.
# ============================================================================
import math
import sys

import bmesh
import bpy
from mathutils import Vector

STONE_W = 0.016     # the stone's half-width (x), units
STONE_H = 0.021     # its half-height (z)
STONE_DOME = 0.008  # how far it domes out front and back (y)
WIRE = 0.0028       # the bezel wire's radius
CORD = 0.0011       # the chain cord's radius
LOOP_R = 0.026      # the chain loop's radius
BAIL_R = 0.0045     # the bail's radius (the small ring the chain runs through)
SEG = 40            # segments round each sweep
RING = 10           # segments round each wire's section

args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = args[0] if args else "moonstone_amulet.glb"

if bpy.context.mode != "OBJECT":
    bpy.ops.object.mode_set(mode="OBJECT")
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()


def sweep(bm, path, radius, closed=True):
    """A round wire of `radius` along `path` (a list of points in the XZ
    plane), each section a circle in the plane across the path. Quads wound
    counter-clockwise seen from outside."""
    n = len(path)
    rings = []
    for i, p in enumerate(path):
        a = path[(i - 1) % n] if closed or i > 0 else path[i]
        b = path[(i + 1) % n] if closed or i < n - 1 else path[i]
        t = (b - a).normalized()
        side = Vector((0.0, 1.0, 0.0))        # out of the pendant's plane
        norm = t.cross(side).normalized()     # in the plane, across the path
        ring = []
        for k in range(RING):
            th = 2.0 * math.pi * k / RING
            ring.append(bm.verts.new(p + (norm * math.cos(th) + side * math.sin(th)) * radius))
        rings.append(ring)
    count = n if closed else n - 1
    for i in range(count):
        r0, r1 = rings[i], rings[(i + 1) % n]
        for k in range(RING):
            k1 = (k + 1) % RING
            bm.faces.new((r0[k], r1[k], r1[k1], r0[k1]))


def ellipse(cx, cz, rx, rz, n):
    return [Vector((cx + rx * math.cos(2 * math.pi * i / n), 0.0, cz + rz * math.sin(2 * math.pi * i / n)))
            for i in range(n)]


def make_object(name, bm, material):
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    for poly in mesh.polygons:
        poly.use_smooth = True
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    mesh.materials.append(material)
    return obj


def material(name, color, metallic, roughness, emission=None):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (*color, 1.0)
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Roughness"].default_value = roughness
    if emission:
        bsdf.inputs["Emission Color"].default_value = (*emission, 1.0)
        bsdf.inputs["Emission Strength"].default_value = 1.0
    return mat


silver = material("silver", (0.78, 0.78, 0.8), 1.0, 0.32)
stone = material("moonstone", (0.55, 0.68, 0.95), 0.0, 0.12, emission=(0.14, 0.24, 0.52))

# The pendant's centre; the chain loop sits above it, the bail between.
stone_z = STONE_H + WIRE
bail_z = stone_z + STONE_H + WIRE + BAIL_R * 0.6
loop_z = bail_z + LOOP_R

# --- the bezel and the bail (silver) -------------------------------------------
bm = bmesh.new()
sweep(bm, ellipse(0.0, stone_z, STONE_W + WIRE * 0.5, STONE_H + WIRE * 0.5, SEG), WIRE)
sweep(bm, ellipse(0.0, bail_z, BAIL_R, BAIL_R, 16), WIRE * 0.6)
bezel = make_object("bezel", bm, silver)

# --- the stone: a sphere squashed into a cabochon, domed both sides --------------
bm = bmesh.new()
bmesh.ops.create_uvsphere(bm, u_segments=32, v_segments=16, radius=1.0)
for v in bm.verts:
    v.co = Vector((v.co.x * STONE_W, v.co.y * STONE_DOME, v.co.z * STONE_H + stone_z))
gem = make_object("stone", bm, stone)

# --- the chain: a cord loop above, through the bail (silver) -----------------------
bm = bmesh.new()
sweep(bm, ellipse(0.0, loop_z, LOOP_R * 0.85, LOOP_R, SEG), CORD)
chain = make_object("chain", bm, silver)

for o in (bezel, gem, chain):
    o.select_set(True)
bpy.context.view_layer.objects.active = bezel
bpy.ops.object.join()

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB", use_selection=True)
print("wrote", OUT, "verts", len(bpy.context.active_object.data.vertices))
