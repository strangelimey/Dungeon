# ConvertMesh.py — Blender headless converter for fab.com mesh imports.
#
# The engine's runtime loader (assets::LoadModel) reads only glTF/GLB + OBJ, but
# fab.com listings most often ship FBX (and sometimes USD), and a single listing
# is frequently a PACK of many meshes baked into one file. AssetBaker's
# import-model merges every mesh into one, so a 18-weapon pack would collapse to a
# blob. This script closes both gaps before import-model ever runs:
#   * reads fbx / usd(z) / glb / gltf / obj via Blender's importers
#   * re-emits .glb (which cgltf reads), Y-up like the engine convention
#   * --split writes one .glb per top-level object (each weapon/prop separate)
#   * --keep-rig preserves the armature + skin weights for monsters (the rigged
#     path bypasses import-model, which strips joints, so this output drops
#     straight into assets/models)
#   * --height M normalizes scale/ground/center IN Blender (used by the rigged
#     path; the static-prop path leaves geometry raw and lets import-model
#     normalize)
#
# Usage (invoked by tools/FetchModels.ps1, but runnable by hand):
#   blender --background --factory-startup --python tools/ConvertMesh.py -- \
#       <in-file> <out-dir> [--split] [--keep-rig] [--height M]
#
# Exit code is 0 on success, non-zero on failure (FetchModels keys off it).

import bpy
import sys
import os
import re


def parse_args():
    argv = sys.argv
    if "--" not in argv:
        raise SystemExit("ConvertMesh: expected '-- <in-file> <out-dir> ...'")
    argv = argv[argv.index("--") + 1:]
    if len(argv) < 2:
        raise SystemExit(
            "ConvertMesh: usage <in-file> <out-dir> [--split] [--keep-rig] "
            "[--height M]")
    opts = {
        "infile": os.path.abspath(argv[0]),
        "outdir": os.path.abspath(argv[1]),
        "split": "--split" in argv,
        "keep_rig": "--keep-rig" in argv,
        "height": 0.0,
    }
    if "--height" in argv:
        opts["height"] = float(argv[argv.index("--height") + 1])
    return opts


def fresh_scene():
    # Start from an empty scene so every object afterwards is one we imported
    # (no default cube/camera/light to leak into the export or the split).
    bpy.ops.wm.read_factory_settings(use_empty=True)


def try_ops(candidates, filepath):
    # Operator names drift across Blender versions (the OBJ/USD importers moved to
    # the wm.* namespace; FBX may follow). Try each known spelling in turn.
    last = None
    for getter in candidates:
        try:
            op = getter()
        except AttributeError:
            continue
        try:
            op(filepath=filepath)
            return True
        except Exception as e:  # noqa: BLE001 - report and try the next spelling
            last = e
    if last:
        raise last
    raise SystemExit("ConvertMesh: no importer available for this format")


def import_file(path):
    ext = os.path.splitext(path)[1].lower()
    if ext == ".fbx":
        try_ops([lambda: bpy.ops.import_scene.fbx,
                 lambda: bpy.ops.wm.fbx_import], path)
    elif ext in (".gltf", ".glb"):
        try_ops([lambda: bpy.ops.import_scene.gltf], path)
    elif ext == ".obj":
        try_ops([lambda: bpy.ops.wm.obj_import,
                 lambda: bpy.ops.import_scene.obj], path)
    elif ext in (".usd", ".usdz", ".usda", ".usdc"):
        try_ops([lambda: bpy.ops.wm.usd_import], path)
    else:
        raise SystemExit(f"ConvertMesh: unsupported source extension '{ext}'")


def mesh_objects():
    return [o for o in bpy.context.scene.objects if o.type == "MESH"]


def normalize(height, keep_rig):
    # Scale so the tallest extent (or, if --height given, the Y extent) maps to a
    # cell-friendly size, ground to y=0 and center XZ — the same convention
    # import-model applies, done here for the rig path that skips import-model.
    # Object-level transform on the armature (rig path) or on the mesh roots
    # (static), so skin weights are untouched; the glTF exporter bakes it.
    objs = bpy.context.scene.objects
    if not objs:
        return
    # Combined world-space bounds over all mesh corners.
    import mathutils  # noqa: PLC0415 - Blender-only module
    lo = mathutils.Vector((1e18, 1e18, 1e18))
    hi = mathutils.Vector((-1e18, -1e18, -1e18))
    for o in mesh_objects():
        for corner in o.bound_box:
            p = o.matrix_world @ mathutils.Vector(corner)
            lo = mathutils.Vector((min(lo.x, p.x), min(lo.y, p.y), min(lo.z, p.z)))
            hi = mathutils.Vector((max(hi.x, p.x), max(hi.y, p.y), max(hi.z, p.z)))
    ext = hi - lo
    # Blender is Z-up; "height" is the Z extent here (glTF export converts to Y).
    max_ext = max(ext.x, ext.y, ext.z, 1e-4)
    scale = (height / max(ext.z, 1e-4)) if height > 0.0 else (2.0 / max_ext)
    cx = (lo.x + hi.x) * 0.5
    cy = (lo.y + hi.y) * 0.5

    roots = ([o for o in objs if o.type == "ARMATURE"] if keep_rig else []) or \
        [o for o in objs if o.parent is None]
    for o in roots:
        o.scale = (o.scale[0] * scale, o.scale[1] * scale, o.scale[2] * scale)
        o.location = ((o.location.x - cx) * scale, (o.location.y - cy) * scale,
                      (o.location.z - lo.z) * scale)


def export_glb(path, keep_rig, use_selection):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=path,
        export_format="GLB",
        use_selection=use_selection,
        export_skins=keep_rig,
        export_yup=True,
        export_apply=not keep_rig,  # apply modifiers for static; keep for rigged
    )
    print(f"ConvertMesh: wrote {path}")


def safe_name(name):
    s = re.sub(r"[^A-Za-z0-9_-]+", "_", name).strip("_").lower()
    return s or "mesh"


def main():
    opts = parse_args()
    fresh_scene()
    import_file(opts["infile"])

    if opts["height"] > 0.0:
        normalize(opts["height"], opts["keep_rig"])

    if opts["split"]:
        meshes = mesh_objects()
        if not meshes:
            raise SystemExit("ConvertMesh: --split but no mesh objects imported")
        for o in meshes:
            bpy.ops.object.select_all(action="DESELECT")
            o.select_set(True)
            bpy.context.view_layer.objects.active = o
            out = os.path.join(opts["outdir"], safe_name(o.name) + ".glb")
            export_glb(out, opts["keep_rig"], use_selection=True)
    else:
        stem = os.path.splitext(os.path.basename(opts["infile"]))[0]
        out = os.path.join(opts["outdir"], safe_name(stem) + ".glb")
        export_glb(out, opts["keep_rig"], use_selection=False)


if __name__ == "__main__":
    main()
