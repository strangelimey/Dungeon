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
        "max_tex": 0,
        "dump_images": "",
    }
    if "--height" in argv:
        opts["height"] = float(argv[argv.index("--height") + 1])
    if "--fit" in argv:
        opts["fit"] = float(argv[argv.index("--fit") + 1])
    else:
        opts["fit"] = 0.0
    if "--max-tex" in argv:
        opts["max_tex"] = int(argv[argv.index("--max-tex") + 1])
    if "--dump-images" in argv:
        opts["dump_images"] = os.path.abspath(argv[argv.index("--dump-images") + 1])
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


def scene_bounds():
    import mathutils  # noqa: PLC0415 - Blender-only module
    lo = mathutils.Vector((1e18, 1e18, 1e18))
    hi = mathutils.Vector((-1e18, -1e18, -1e18))
    for o in mesh_objects():
        for corner in o.bound_box:
            p = o.matrix_world @ mathutils.Vector(corner)
            lo = mathutils.Vector((min(lo.x, p.x), min(lo.y, p.y), min(lo.z, p.z)))
            hi = mathutils.Vector((max(hi.x, p.x), max(hi.y, p.y), max(hi.z, p.z)))
    return lo, hi


def all_fcurves(action):
    # Blender 5.1 slotted actions vs the legacy flat list (ImportAnimLibrary's
    # iterator, shared convention).
    legacy = getattr(action, "fcurves", None)
    if legacy is not None:
        yield from legacy
        return
    for layer in action.layers:
        for strip in layer.strips:
            for cbag in strip.channelbags:
                yield from cbag.fcurves


def bake_rig(height, fit):
    # The ENGINE drops non-joint node transforms when a rig animates (its
    # Animator walks the joint hierarchy only — the ImportAnimLibrary pipeline
    # documents the same invariant), so scale/ground must be BAKED into the
    # armature data, never left on the armature object. transform_apply covers
    # the rest pose; pose-bone .location keys are in bone-parent space and
    # DON'T scale with it, so every action's location keys are multiplied by
    # the same factor (ImportAnimLibrary's trick, verbatim).
    arm = next((o for o in bpy.context.scene.objects if o.type == "ARMATURE"), None)
    if arm is None:
        return normalize(height, True, fit)  # no rig after all
    if any(abs(r) > 1e-3 for r in arm.rotation_euler):
        print("ConvertMesh: WARNING armature carries a rotation — baking it can "
              "skew root-motion location keys; inspect the result")

    # Drop UNSKINNED meshes (vendor FBXs smuggle reference spheres/planes): the
    # engine renders meshes[0] of a monster model, so a stowaway can shadow the
    # actual body — and it poisons the bounds math below.
    for o in list(mesh_objects()):
        if not any(mod.type == "ARMATURE" for mod in o.modifiers):
            print(f"ConvertMesh: dropping unskinned mesh '{o.name}'")
            bpy.data.objects.remove(o, do_unlink=True)

    lo, hi = scene_bounds()
    ext = hi - lo
    max_ext = max(ext.x, ext.y, ext.z, 1e-4)
    if fit > 0.0:
        s = fit / max_ext
    else:
        s = (height / max(ext.z, 1e-4)) if height > 0.0 else (2.0 / max_ext)
    print(f"ConvertMesh: extents {ext.x:.2f} x {ext.y:.2f} x {ext.z:.2f} m, "
          f"scale {s:.3f}")

    # ImportAnimLibrary's proven sequence, mesh and armature handled SEPARATELY
    # (a multi-select apply only bakes each object's LOCAL transform — a mesh
    # parented under the armature loses its inherited scale and blows up):
    # 1. unparent the meshes keeping world placement (the armature MODIFIER,
    #    not the parenting, drives the skin — deform survives),
    # 2. apply each mesh's full world transform into its vertices,
    # 3. scale the armature object by the wanted factor and apply — bone rests
    #    absorb (old object scale x s); pose .location keys don't scale with
    #    transform_apply, so multiply them by the same total factor,
    # 4. scale the mesh DATA by s to match (its step-2 bake left it at raw
    #    world size), then ground/center both together.
    def apply_all(obj, location=True, rotation=True, scale=True):
        bpy.ops.object.select_all(action="DESELECT")
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.transform_apply(location=location, rotation=rotation,
                                       scale=scale)

    meshes = mesh_objects()
    for o in meshes:
        if o.parent is not None:
            bpy.ops.object.select_all(action="DESELECT")
            o.select_set(True)
            bpy.context.view_layer.objects.active = o
            bpy.ops.object.parent_clear(type="CLEAR_KEEP_TRANSFORM")
        apply_all(o)

    total = arm.scale.x * s  # the full factor the armature DATA absorbs
    arm.scale = (arm.scale.x * s, arm.scale.y * s, arm.scale.z * s)
    apply_all(arm)
    for action in bpy.data.actions:
        for fc in all_fcurves(action):
            if fc.data_path.endswith(".location"):
                for kp in fc.keyframe_points:
                    kp.co.y *= total
                    kp.handle_left.y *= total
                    kp.handle_right.y *= total

    for o in meshes:
        o.scale = (s, s, s)
        apply_all(o)

    # Ground + center on the SCALED bounds; shift meshes AND armature equally
    # so the rest alignment (mesh data vs bone rests) is preserved.
    lo, hi = scene_bounds()
    cx = (lo.x + hi.x) * 0.5
    cy = (lo.y + hi.y) * 0.5
    for o in meshes + [arm]:
        o.location.x -= cx
        o.location.y -= cy
        o.location.z -= lo.z
        apply_all(o, rotation=False, scale=False)
    lo, hi = scene_bounds()
    print(f"ConvertMesh: baked rig transform (factor {total:.5f}); final bounds "
          f"({hi.x - lo.x:.2f} x {hi.y - lo.y:.2f} x {hi.z - lo.z:.2f} m, "
          f"floor at {lo.z:.2f})")


def normalize(height, keep_rig, fit=0.0):
    # Scale so the tallest extent (or, if --height given, the Y extent) maps to a
    # cell-friendly size, ground to y=0 and center XZ — the same convention
    # import-model applies, done here for the rig path that skips import-model.
    # `fit` instead scales the LONGEST extent to that size — the right normalizer
    # for crawlers (a spider is judged by leg span across the cell, not standing
    # height; scaling its tiny Z to a "height" blows the footprint out).
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
    if fit > 0.0:
        scale = fit / max_ext
    else:
        scale = (height / max(ext.z, 1e-4)) if height > 0.0 else (2.0 / max_ext)
    print(f"ConvertMesh: extents {ext.x:.2f} x {ext.y:.2f} x {ext.z:.2f} m, "
          f"scale {scale:.3f}")
    cx = (lo.x + hi.x) * 0.5
    cy = (lo.y + hi.y) * 0.5

    roots = ([o for o in objs if o.type == "ARMATURE"] if keep_rig else []) or \
        [o for o in objs if o.parent is None]
    for o in roots:
        o.scale = (o.scale[0] * scale, o.scale[1] * scale, o.scale[2] * scale)
        o.location = ((o.location.x - cx) * scale, (o.location.y - cy) * scale,
                      (o.location.z - lo.z) * scale)


def normalize_object(obj, size):
    # Scale ONE object so its longest world-space extent == size, ground it
    # (min Z -> 0) and center it on X/Y. Used per-piece when splitting a pack so
    # each weapon is sized on its OWN bbox, not the whole pack's combined bounds
    # (the pack lays its weapons side by side, so a combined scale mis-sizes them).
    import mathutils  # noqa: PLC0415 - Blender-only module
    lo = mathutils.Vector((1e18, 1e18, 1e18))
    hi = mathutils.Vector((-1e18, -1e18, -1e18))
    for corner in obj.bound_box:
        p = obj.matrix_world @ mathutils.Vector(corner)
        lo = mathutils.Vector((min(lo.x, p.x), min(lo.y, p.y), min(lo.z, p.z)))
        hi = mathutils.Vector((max(hi.x, p.x), max(hi.y, p.y), max(hi.z, p.z)))
    ext = hi - lo
    s = size / max(ext.x, ext.y, ext.z, 1e-4)
    cx, cy = (lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5
    obj.scale = (obj.scale[0] * s, obj.scale[1] * s, obj.scale[2] * s)
    obj.location = ((obj.location.x - cx) * s, (obj.location.y - cy) * s,
                    (obj.location.z - lo.z) * s)


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


def downscale_images(maxpx):
    # Shrink every embedded image so the exported GLB stays reasonable — a bought
    # multi-material pack can ship ~20 4K maps (70+ MB). Cap the longest side at
    # maxpx, preserving aspect; only touches images larger than the cap.
    for img in list(bpy.data.images):
        # NB: don't gate on img.has_data — for freshly imported packed images it
        # is lazily False until the pixels are touched, which would skip every
        # image. Reading img.size loads the dimensions without that pitfall.
        w, h = img.size[0], img.size[1]
        longest = max(w, h)
        if w == 0 or longest <= maxpx:
            continue
        s = maxpx / float(longest)
        img.scale(max(1, int(w * s)), max(1, int(h * s)))
        # The glTF exporter re-emits an image's ORIGINAL packed bytes when it can,
        # ignoring scale(). Re-pack from the scaled buffer (as PNG) so the export
        # actually carries the smaller image. file_format must be set for pack().
        img.file_format = "PNG"
        img.pack()
        print(f"ConvertMesh: scaled {img.name} {w}x{h} -> {img.size[0]}x{img.size[1]}")


def safe_name(name):
    s = re.sub(r"[^A-Za-z0-9_-]+", "_", name).strip("_").lower()
    return s or "mesh"


def dump_images(outdir):
    # Save every image the import brought in (fab FBX monsters EMBED their PBR
    # maps) as a loose PNG named by the image's own name — the source names
    # (DiffuseMap/NormalMap/MetallicMap/RoughnessMap/HeightMap) are exactly what
    # AssetBaker's DiscoverMaps classifies by, so the dump feeds `import` as if
    # the listing had shipped loose maps.
    os.makedirs(outdir, exist_ok=True)
    count = 0
    for img in list(bpy.data.images):
        if img.size[0] == 0:
            continue
        path = os.path.join(outdir, safe_name(os.path.splitext(img.name)[0]) + ".png")
        img.file_format = "PNG"
        try:
            img.save(filepath=path)
        except TypeError:  # older Blender: save() writes filepath_raw
            img.filepath_raw = path
            img.save()
        print(f"ConvertMesh: dumped {path} ({img.size[0]}x{img.size[1]})")
        count += 1
    if count == 0:
        print("ConvertMesh: WARNING --dump-images found no images")


def main():
    opts = parse_args()
    fresh_scene()
    import_file(opts["infile"])

    # Combined-scene normalize only when NOT splitting; split pieces normalize
    # individually below so each is sized on its own bounds. Rigged models BAKE
    # the transform into the data (the engine drops armature-node transforms
    # when animating); static models keep the cheap object-level normalize.
    do_norm = opts["height"] > 0.0 or opts["fit"] > 0.0
    if do_norm and not opts["split"]:
        if opts["keep_rig"]:
            bake_rig(opts["height"], opts["fit"])
        else:
            normalize(opts["height"], False, opts["fit"])

    if opts["max_tex"] > 0:
        downscale_images(opts["max_tex"])

    if opts["dump_images"]:
        dump_images(opts["dump_images"])

    # Rig sanity: the engine's skinning palette caps at 128 joints
    # (gfx::kMaxSkinJoints) — flag an over-budget armature at convert time,
    # not as a mangled pose in-game.
    if opts["keep_rig"]:
        for o in bpy.context.scene.objects:
            if o.type == "ARMATURE":
                bones = len(o.data.bones)
                tag = " (OVER the engine's 128-joint cap!)" if bones > 128 else ""
                print(f"ConvertMesh: armature '{o.name}' has {bones} bones{tag}")
        # FBX imports create one action per take on the ARMATURE (named
        # "Armature|Atk") and usually a bare-named duplicate on the mesh object.
        # Per take, keep the SKELETAL action (fcurves drive pose bones), drop the
        # rest, and strip the pipe prefix — so the glTF clip names match the take
        # names exactly (monsters.cat anim rows + the editor clip list key off
        # them) with no junk duplicates alongside.
        def is_skeletal(action):
            return any(fc.data_path.startswith("pose.bones")
                       for fc in action.fcurves)

        by_base = {}
        for action in list(bpy.data.actions):
            base = action.name.rsplit("|", 1)[-1].strip() or action.name
            kept = by_base.get(base)
            if kept is None:
                by_base[base] = action
            elif is_skeletal(action) and not is_skeletal(kept):
                by_base[base] = action
                bpy.data.actions.remove(kept)
            else:
                bpy.data.actions.remove(action)
        for base, action in by_base.items():
            if action.name != base:
                action.name = base
        print(f"ConvertMesh: kept {len(by_base)} clip(s): "
              f"{', '.join(sorted(by_base))}")

    if opts["split"]:
        meshes = mesh_objects()
        if not meshes:
            raise SystemExit("ConvertMesh: --split but no mesh objects imported")
        for o in meshes:
            if do_norm:
                normalize_object(o, opts["height"])
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
