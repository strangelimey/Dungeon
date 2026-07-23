# ============================================================================
# tools/BlenderStarter.py — writes a starter .blend set up for authoring
# dungeon assets in UNIT space (1.0 = one dungeon square). See
# docs/authoring-scale.md for the conventions this file encodes.
#
#   blender --background --factory-startup --python tools\BlenderStarter.py -- <out.blend>
#
# What it builds, all in a "REFERENCE" collection that is excluded from
# rendering and export (wireframe, unselectable):
#   * CELL        — the dungeon square: 1 x 1 x 1, floor at Z = 0
#   * WALL_FACE   — the back plane a WALL-mounted prop sits against (Y = 0);
#                   such a prop reaches toward -Y, which the glTF exporter
#                   turns into the engine's +Z (the room side)
#   * EYE_LEVEL   — the party's eye height, 0.62
#   * DOORWAY     — a typical clear opening, 0.68 wide x 0.84 tall
#
# Axis note (the one that bites): the exporter's +Y-Up conversion maps
# Blender -Y to the engine's +Z. So the FRONT of anything you build faces -Y
# in Blender, and Blender +Z is up in both.
# ============================================================================
import sys

import bpy

OUT = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else "starter.blend"

# --- empty the factory scene -------------------------------------------------
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete()
for block in (bpy.data.meshes, bpy.data.materials):
    for item in list(block):
        if item.users == 0:
            block.remove(item)

scene = bpy.context.scene

# --- units: 1 Blender metre == 1 dungeon square ------------------------------
# Unit scale stays 1.0 — the glTF exporter writes Blender units straight
# through, so 1.0 in equals 1.0 out. Do not "helpfully" change this.
scene.unit_settings.system = "METRIC"
scene.unit_settings.scale_length = 1.0
scene.unit_settings.length_unit = "METERS"

# --- the reference collection ------------------------------------------------
ref = bpy.data.collections.new("REFERENCE")
scene.collection.children.link(ref)


def add(name, verts, edges):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, edges, [])
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    obj.display_type = "WIRE"
    obj.hide_render = True
    obj.hide_select = True  # click through it while you model
    ref.objects.link(obj)
    return obj


def box(name, x0, x1, y0, y1, z0, z1):
    v = [(x, y, z) for z in (z0, z1) for y in (y0, y1) for x in (x0, x1)]
    # 0..3 bottom (y0x0, y0x1, y1x0, y1x1), 4..7 top
    e = [(0, 1), (1, 3), (3, 2), (2, 0),
         (4, 5), (5, 7), (7, 6), (6, 4),
         (0, 4), (1, 5), (2, 6), (3, 7)]
    return add(name, v, e)


def rect_xz(name, x0, x1, z0, z1, y=0.0):
    v = [(x0, y, z0), (x1, y, z0), (x1, y, z1), (x0, y, z1)]
    return add(name, v, [(0, 1), (1, 2), (2, 3), (3, 0)])


# The square itself: floor at Z = 0, ceiling at Z = 1, walls at X/Y = +/-0.5.
box("CELL", -0.5, 0.5, -0.5, 0.5, 0.0, 1.0)

# A wall-mounted prop's back plane. Build against this and reach toward -Y.
rect_xz("WALL_FACE", -0.5, 0.5, 0.0, 1.0, y=0.0)

# Eye level (0.62) — hang wall props relative to this, not to the floor.
add("EYE_LEVEL",
    [(-0.5, 0.0, 0.62), (0.5, 0.0, 0.62)], [(0, 1)])

# A typical clear doorway opening, for proportioning an arch against.
rect_xz("DOORWAY", -0.34, 0.34, 0.0, 0.84, y=0.0)

# --- a collection for YOUR work, active so new objects land in it -------------
work = bpy.data.collections.new("MODEL")
scene.collection.children.link(work)
layer = bpy.context.view_layer.layer_collection
bpy.context.view_layer.active_layer_collection = layer.children[work.name]

# Keep REFERENCE out of the view layer's selection/render path but visible.
for child in layer.children:
    if child.name == "REFERENCE":
        child.holdout = False

# --- viewport grid: one square per grid cell, tenths as subdivisions ----------
# Best-effort: --background may not carry a 3D view, and the setting is
# per-area, so a missing screen is not an error.
for screen in bpy.data.screens:
    for area in screen.areas:
        if area.type != "VIEW_3D":
            continue
        for space in area.spaces:
            if space.type == "VIEW_3D":
                space.overlay.grid_scale = 1.0
                space.overlay.grid_subdivisions = 10
                space.clip_start = 0.01  # so you can zoom into small detail
                space.clip_end = 100.0

bpy.ops.wm.save_as_mainfile(filepath=OUT)
print(f"BlenderStarter: wrote {OUT}")
