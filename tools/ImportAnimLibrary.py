# ImportAnimLibrary.py — bake a STATE-ORGANIZED animation library onto a creature
# mesh and emit the matching monsters.cat rows.
#
# Each creature type is a mesh + a set of animations (where available). This tool
# ingests a library laid out as ONE FOLDER PER CreatureState, each holding one or
# more Mixamo .fbx clips:
#
#     <library_root>/
#         Idle/      Breathing Idle.fbx  Orc Idle.fbx  ...
#         InCombat/  Fight Idle.fbx      Ready Idle.fbx
#         Attack/    Zombie Attack.fbx   ...
#         Walk/  Run/  Flee/  Defend/  Hit/  Die/  Spawn/   (any may be empty)
#
# The FOLDER names the state (matched case-insensitively to a CreatureState token,
# see STATE_TOKENS below = src/Animation/CreatureState.h); the FILE inside is one
# animation. Each clip is exported as a glTF animation named <state>__<sanitised
# filename> — the state is encoded in the clip NAME so the model self-describes its
# grouping (the editor's monster-config dialog shows a state's animations by
# filtering on this prefix). The tool prints + writes the catalog rows that group
# them:
#
#     states = idle incombat attack ...
#     anim_idle = idle__breathing_idle idle__orc_idle ...
#     anim_incombat = incombat__fight_idle incombat__ready_idle
#     anim_attack = attack__zombie_attack ...
#
# so the data-driven animation table picks a random variation per state.
#
# It generalises the per-mesh rig_and_export.py: every Mixamo clip shares one
# standard skeleton, so any number bind to a single rigid bind — drop more .fbx in
# a state folder and re-run, no re-binding.
#
# The output is authored in UNIT SPACE like every other model on disk (1.0 = one
# dungeon square; docs/authoring-scale.md puts a humanoid at ~0.68 units), so
# --height is in UNITS — the same convention as import-model's --height and
# FetchModels' $modelSets Height. It used to be "match this reference model's
# height" instead, which is how five bought skeletons ended up in metre space:
# the reference was measured over EVERY mesh in the scene, and Blender's glTF
# importer leaves a stray 2-unit Icosphere behind, so the "reference height" was
# a constant 2.000 whatever model was named. An explicit number can't drift.
#
# Two modes:
#   --plan : walk the library, print the clip plan + catalog rows, and exit. Pure
#            python (NO Blender) — a dry run / CI check of the folder layout.
#   (bake) : run under Blender to rig-bind the mesh and export the glTF library.
#
# Usage (bake, under Blender):
#   blender --background --factory-startup --python tools/ImportAnimLibrary.py -- \
#       <mesh.fbx> <library_root> <out.gltf> --height <units> \
#       [--catalog-out <file>] [--mesh-yaw DEG] [--keep-fingers]
# Usage (plan, plain python):
#   python tools/ImportAnimLibrary.py --plan <library_root> [--catalog-out <file>]
#
# Exit code is 0 on success, non-zero on failure (a driver script keys off it).

import sys
import os
import glob
import re
import tempfile

# The canonical state tokens — MUST stay in sync with dungeon::anim::CreatureState
# (src/Animation/CreatureState.h). A library subfolder maps to a state by name
# (case-insensitive); this list is also the ORDER states are written to the catalog.
STATE_TOKENS = [
    "spawn", "idle", "incombat", "walk", "run", "flee", "attack", "defend", "hit", "die",
]


def sanitize(name):
    """A filename stem -> a clip id: lowercase, runs of non-alnum collapse to one
    '_', trimmed. 'Orc Idle (1).fbx' -> 'orc_idle_1'."""
    stem = os.path.splitext(os.path.basename(name))[0].lower().strip()
    return re.sub(r"[^a-z0-9]+", "_", stem).strip("_")


def build_plan(library_root):
    """Walk the library. Returns (clip_files, rows, unknown):
      clip_files = [(clip_name, abspath), ...]   in catalog/state order
      rows       = {token: [clip_name, ...], ...}
      unknown    = [folder_name, ...]            folders not matching a state
    Clip names are globally de-duplicated (a repeat gets a _2/_3 suffix)."""
    if not os.path.isdir(library_root):
        raise SystemExit(f"library root not found: {library_root}")
    # Map each subfolder to a state token (lowercased name); collect unknowns.
    by_token = {}
    unknown = []
    for entry in sorted(os.listdir(library_root)):
        path = os.path.join(library_root, entry)
        if not os.path.isdir(path):
            continue
        token = entry.lower()
        if token in STATE_TOKENS:
            by_token[token] = path
        else:
            unknown.append(entry)

    clip_files = []
    rows = {}
    used = set()
    for token in STATE_TOKENS:  # deterministic order
        folder = by_token.get(token)
        if not folder:
            continue
        names = []
        for fbx in sorted(glob.glob(os.path.join(folder, "*.fbx"))):
            # Clip id = <state>__<sanitised filename>, so the state is encoded in
            # the clip NAME (the editor's monster-config dialog filters candidates
            # by this prefix). sanitize() collapses non-alnum to a single '_', so
            # the double underscore only ever appears as the state/clip separator.
            clip = f"{token}__{sanitize(fbx) or 'clip'}"
            base = clip
            n = 2
            while clip in used:  # global de-dup
                clip = f"{base}_{n}"
                n += 1
            used.add(clip)
            clip_files.append((clip, os.path.abspath(fbx)))
            names.append(clip)
        if names:
            rows[token] = names
    return clip_files, rows, unknown


def catalog_text(rows):
    """The monsters.cat animation rows for a [creature] block."""
    states = [t for t in STATE_TOKENS if rows.get(t)]
    lines = ["; --- animation rows generated by ImportAnimLibrary ---",
             "states = " + " ".join(states)]
    for t in STATE_TOKENS:
        if rows.get(t):
            lines.append(f"anim_{t} = " + " ".join(rows[t]))
    return "\n".join(lines) + "\n"


def parse_args(argv):
    """Parse args. Under Blender the real args follow '--'; under plain python
    (--plan dry run) they are argv[1:] (argv[0] is the script path)."""
    argv = argv[argv.index("--") + 1:] if "--" in argv else argv[1:]
    plan = "--plan" in argv
    argv = [a for a in argv if a != "--plan"]
    opts = {"plan": plan, "mesh": None, "library": None, "out": None, "height": 0.0,
            "catalog_out": None, "mesh_yaw": 90.0, "keep_fingers": False,
            "textures": "", "tex_dir": "", "arm_drop": 0.0, "skinned": False,
            "rigid_islands": False, "original": "", "islands": "",
            "mirror": False, "rig_from": "", "flip_green": False}
    usage = ("usage: <mesh.fbx|.obj> <library_root> <out.gltf> --height <units> "
             "[--catalog-out f] [--mesh-yaw deg] [--keep-fingers] "
             "[--textures '<mat>=<albedo>[:<normal>[:<orm>]];...' --tex-dir <dir>] "
             "[--flip-green] [--islands '<minvert>=<bone>;...'] [--mirror] "
             "[--rig-from <sibling_rigged.fbx>]  |  "
             "--plan <library_root> [--catalog-out f]")

    def value(i, flag):  # the arg after a value-flag, or a usage error if omitted
        if i + 1 >= len(argv):
            raise SystemExit(f"{flag} needs a value\n{usage}")
        return argv[i + 1]

    pos = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--catalog-out":
            opts["catalog_out"] = value(i, a); i += 2
        elif a == "--height":
            opts["height"] = float(value(i, a)); i += 2
        elif a == "--mesh-yaw":
            opts["mesh_yaw"] = float(value(i, a)); i += 2
        elif a == "--keep-fingers":
            opts["keep_fingers"] = True; i += 1
        elif a == "--textures":
            opts["textures"] = value(i, a); i += 2
        elif a == "--tex-dir":
            opts["tex_dir"] = value(i, a); i += 2
        elif a == "--skinned":
            opts["skinned"] = True; i += 1
        elif a == "--rigid-islands":
            opts["rigid_islands"] = True; i += 1
        elif a == "--original":
            opts["original"] = value(i, a); i += 2
        elif a == "--islands":
            opts["islands"] = value(i, a); i += 2
        elif a == "--mirror":
            opts["mirror"] = True; i += 1
        elif a == "--flip-green":
            opts["flip_green"] = True; i += 1
        elif a == "--rig-from":
            opts["rig_from"] = value(i, a); i += 2
        elif a == "--arm-drop":
            opts["arm_drop"] = float(value(i, a)); i += 2
        elif a.startswith("--"):  # an unknown flag, not a silently-swallowed positional
            raise SystemExit(f"unknown option '{a}'\n{usage}")
        else:
            pos.append(a); i += 1
    if plan:
        if not pos:
            raise SystemExit("usage: --plan <library_root> [--catalog-out f]")
        opts["library"] = pos[0]
    else:
        if len(pos) < 3:
            raise SystemExit(usage)
        opts["mesh"], opts["library"], opts["out"] = pos[:3]
        # No default: a silently-assumed height is what put five skeletons in
        # metre space. Say the number, in units.
        if opts["height"] <= 0.0:
            raise SystemExit("--height <units> is required (1.0 = one dungeon "
                             "square; a humanoid is ~0.68 — see "
                             "docs/authoring-scale.md)\n" + usage)
    return opts


def emit_catalog(rows, catalog_out):
    text = catalog_text(rows)
    print("\n" + text)
    if catalog_out:
        with open(catalog_out, "w", encoding="utf-8") as f:
            f.write(text)
        print(f"[catalog] wrote {catalog_out}")


# ===========================================================================
# Plan mode (no Blender): print the clip plan + catalog rows and exit.
# ===========================================================================
def run_plan(opts):
    clip_files, rows, unknown = build_plan(opts["library"])
    print(f"[plan] library: {opts['library']}")
    for t in STATE_TOKENS:
        if rows.get(t):
            print(f"  {t:9} ({len(rows[t])}): {', '.join(rows[t])}")
        else:
            print(f"  {t:9} (0): -")
    if unknown:
        print(f"[warn] folders not matching a CreatureState (ignored): {', '.join(unknown)}")
    if not clip_files:
        raise SystemExit("[plan] no .fbx clips found in any state folder")
    emit_catalog(rows, opts["catalog_out"])


# ===========================================================================
# Bake mode (Blender): rig-bind the mesh + export the glTF library. Generalises
# rig_and_export.py — the clip list now comes from build_plan (state folders).
# ===========================================================================
def run_bake(opts):
    import bpy
    import math
    from mathutils import Vector  # noqa: F401 (kept for parity / future use)
    from collections import Counter
    import bmesh

    clip_files, rows, unknown = build_plan(opts["library"])
    if not clip_files:
        raise SystemExit("no .fbx clips found in any state folder")
    print(f"[clips] {len(clip_files)} clips: {[n for n, _ in clip_files]}")
    if unknown:
        print(f"[warn] folders not matching a CreatureState (ignored): {', '.join(unknown)}")

    mesh_fbx, out_path = opts["mesh"], opts["out"]
    mesh_yaw = math.radians(opts["mesh_yaw"])
    fingers_excluded = not opts["keep_fingers"]

    def clear_scene():
        # read_factory_settings is not enough on its own to trust: an importer
        # can leave objects of its own behind (Blender's glTF importer parks a
        # stray 2-unit 'Icosphere' beside every rig it reads), and anything left
        # in the scene poisons the next whole-scene measurement. Purge by hand.
        bpy.ops.wm.read_factory_settings(use_empty=True)
        for o in list(bpy.data.objects):
            bpy.data.objects.remove(o, do_unlink=True)

    clear_scene()

    # NOTE a --skinned mesh must be an FBX. A Mixamo rig re-exported as .glb
    # imports with different bone orientations and scale (glTF stores no bone
    # roll or length, so Blender synthesises them), which leaves the rig at a
    # different scale from its own mesh and gives the re-rest a basis that
    # doesn't match the clip library's. Download the rigged FBX from Mixamo.
    def import_fbx(path):
        before = set(bpy.data.objects)
        bpy.ops.import_scene.fbx(filepath=path)
        return [o for o in bpy.data.objects if o not in before]

    # The height the finished creature stands, in UNITS (1.0 = one dungeon
    # square). Everything below fits and grounds to this one number.
    ref_h = opts["height"]
    print(f"[fit] target height={ref_h:.3f} units")

    # ---- import every clip; keep the first armature as THE rig ----------------
    rig = None
    target_h = None  # the Mixamo MESH visual height (scale anchor)
    for clip_name, path in clip_files:
        new_objs = import_fbx(path)
        arm = next(o for o in new_objs if o.type == "ARMATURE")
        act = arm.animation_data.action
        act.name = clip_name
        act.use_fake_user = True
        if rig is None:
            rig = arm
            rig.name = "rig"
            ybot = [o for o in new_objs if o.type == "MESH"]
            zs = [(o.matrix_world @ v.co).z for o in ybot for v in o.data.vertices]
            target_h = max(zs) - min(zs)
        for o in new_objs:  # drop each import's meshes; drop extra armatures
            if o.type == "MESH":
                bpy.data.objects.remove(o, do_unlink=True)
            elif o.type == "ARMATURE" and o is not rig:
                bpy.data.objects.remove(o, do_unlink=True)
    print(f"[rig] kept '{rig.name}', {len(bpy.data.actions)} actions")

    # Bake the rig to the reference scale up front (Mixamo armatures import tiny;
    # leaving an object scale on the root makes the engine drop it -> invisible).
    factor = (ref_h / target_h) * rig.scale.x
    bpy.ops.object.select_all(action="DESELECT")
    rig.select_set(True)
    bpy.context.view_layer.objects.active = rig
    rig.scale = (factor, factor, factor)
    bpy.ops.object.transform_apply(scale=True)

    def all_fcurves(act):
        legacy = getattr(act, "fcurves", None)
        if legacy is not None:
            yield from legacy
            return
        for layer in act.layers:  # Blender 5.1 slotted actions
            for strip in layer.strips:
                for cbag in strip.channelbags:
                    yield from cbag.fcurves

    # Apply-scale doesn't touch animation translation — scale every .location key.
    for act in bpy.data.actions:
        for fc in all_fcurves(act):
            if fc.data_path.endswith(".location"):
                for kp in fc.keyframe_points:
                    kp.co.y *= factor
                    kp.handle_left.y *= factor
                    kp.handle_right.y *= factor
    bpy.context.view_layer.update()
    print(f"[rig] baked scale {factor:.4f}")

    # ---- A-pose rest adjustment (--arm-drop DEG) --------------------------------
    # A mesh authored with arms DOWN (bought kits) binds terribly to the Mixamo
    # T-pose rest: hand/forearm bones sit out at shoulder height, so a shield
    # hanging at the hip rigid-binds to the pelvis and stays behind when the arm
    # swings. Rotating the upper-arm bones down by the mesh's hang angle and
    # APPLYING that as the new rest fixes the island->bone assignment; the
    # Mixamo clips key every bone with absolute parent-relative rotations, so
    # they play unchanged against the new rest.
    if opts["arm_drop"] > 0.0:
        from mathutils import Matrix
        drop = math.radians(opts["arm_drop"])
        bpy.ops.object.select_all(action="DESELECT")
        rig.select_set(True)
        bpy.context.view_layer.objects.active = rig
        bpy.ops.object.mode_set(mode="POSE")
        dropped = 0
        for pb in rig.pose.bones:
            n = pb.name
            side = 1.0 if "LeftArm" in n else (-1.0 if "RightArm" in n else 0.0)
            if side == 0.0 or "ForeArm" in n:
                continue  # only the UPPER arm bones pivot; forearms follow
            # Rotate the pose bone about the armature's forward (+Y) axis at
            # the bone head — swings the arm down in the frontal plane.
            pivot = pb.matrix.to_translation()
            rot = (Matrix.Translation(pivot) @
                   Matrix.Rotation(side * -drop, 4, "Y") @
                   Matrix.Translation(-pivot))
            pb.matrix = rot @ pb.matrix
            bpy.context.view_layer.update()
            dropped += 1
        bpy.ops.pose.armature_apply(selected=False)
        bpy.ops.object.mode_set(mode="OBJECT")
        print(f"[rest] dropped {dropped} upper-arm bone(s) by {opts['arm_drop']}deg")

    def bone_world(b, head=True):
        return rig.matrix_world @ (b.head_local if head else b.tail_local)

    def rig_height():
        # The ARMATURE is the yardstick for "how big is this creature", never
        # the mesh: a mesh's bounds include whatever it carries (the spearman's
        # raised spear tops his skull by a fifth of his height), and the kit
        # variants share one rig, so the armature reads the same for all four.
        zs = [bone_world(b, h).z for b in rig.data.bones for h in (True, False)]
        return max(zs) - min(zs)

    def apply_all(obj, location=True, rotation=True, scale=True):
        bpy.ops.object.select_all(action="DESELECT")
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.transform_apply(location=location, rotation=rotation,
                                       scale=scale)

    if opts["skinned"] or opts["rig_from"]:
        # ---- pre-skinned mesh (Mixamo auto-rigged download) ---------------------
        # The mesh arrives already WEIGHTED on the standard Mixamo skeleton, so
        # the download's own armature becomes THE rig — the clip actions target
        # it by bone name — and the rigid bind below is skipped entirely. The
        # clip-import rig is dropped (its actions persist via use_fake_user) —
        # but its per-bone REST ORIENTATIONS are stashed first: they are the
        # basis every clip's pose rotations are authored against, and the
        # re-rest pass at the end of this branch aligns the download rig to
        # them (Mixamo fits its skeleton to the UPLOADED pose, so A-pose art
        # arrives with arm bones resting along the hanging arms — clips played
        # against that rest fold the arms across the body).
        std_rest = {b.name: b.matrix_local.to_3x3().normalized()
                    for b in rig.data.bones}
        bpy.data.objects.remove(rig, do_unlink=True)

        def import_join_apply(path, what):
            # Import a model, join its mesh parts into one, bake transforms
            # (the ConvertMesh-proven ceremony: unparent keeping world
            # placement, then apply each object alone).
            objs = import_fbx(path)
            arm = next((o for o in objs if o.type == "ARMATURE"), None)
            parts = [o for o in objs if o.type == "MESH"]
            if not parts:
                raise SystemExit(f"no mesh objects in {what}: {path}")
            if len(parts) > 1:
                bpy.ops.object.select_all(action="DESELECT")
                for o in parts:
                    o.select_set(True)
                bpy.context.view_layer.objects.active = parts[0]
                bpy.ops.object.join()
                print(f"[mesh] {what}: joined {len(parts)} objects")
            m = parts[0]
            if m.parent is not None:
                bpy.ops.object.select_all(action="DESELECT")
                m.select_set(True)
                bpy.context.view_layer.objects.active = m
                bpy.ops.object.parent_clear(type="CLEAR_KEEP_TRANSFORM")
            apply_all(m)
            if arm is not None:
                apply_all(arm)
            return m, arm

        def fit_ground(objs, mref):
            # Scale a mesh (+ companions) to the ref height and ground it.
            # Returns the applied (scale, z-shift) so a sibling authored in
            # the SAME native frame can be mapped identically.
            zs = [(mref.matrix_world @ v.co).z for v in mref.data.vertices]
            s = ref_h / (max(zs) - min(zs))
            for o in objs:
                o.scale = (s, s, s)
                apply_all(o)
            zs = [(mref.matrix_world @ v.co).z for v in mref.data.vertices]
            for o in objs:
                o.location.z -= min(zs)
                apply_all(o, rotation=False, scale=False)
            return s, -min(zs)

        def apply_fit(objs, s, dz):
            # Apply a fit computed elsewhere — the variant must ride its
            # SIBLING'S transform, not its own extent fit: a tall carried
            # spear tops the skull, and extent-fitting it shrinks the body
            # away from the borrowed rig.
            for o in objs:
                o.scale = (s, s, s)
                apply_all(o)
                o.location.z += dz
                apply_all(o, rotation=False, scale=False)

        if opts["rig_from"]:
            # ---- Mixamo-less variant (--rig-from <sibling download.fbx>) ----
            # When the auto-rigger refuses a variant, borrow a SIBLING
            # download's fitted armature: kit variants share one body, so the
            # sibling's rig matches this variant's pose exactly, and its
            # Mixamo WEIGHTS transfer by nearest-vertex — the shared body
            # (including the soft spine/clavicle blends) comes across
            # verbatim, gear inherits junk the rigid-island snap overrides
            # anyway. mesh_fbx here is the variant's UNRIGGED upload FBX.
            src_mesh, rig = import_join_apply(opts["rig_from"], "rig-from")
            if rig is None:
                raise SystemExit(f"--rig-from has no armature: {opts['rig_from']}")
            rig.name = "rig"
            s_src, dz_src = fit_ground((src_mesh, rig), src_mesh)
            mesh, _ = import_join_apply(mesh_fbx, "variant")
            apply_fit((mesh,), s_src, dz_src)
            zs = [(mesh.matrix_world @ v.co).z for v in mesh.data.vertices]
            print(f"[rig-from] variant z-range {min(zs):+.2f}..{max(zs):+.2f} "
                  f"under the sibling's fit (feet ~0; gear may overhang)")
            # Nearest-vertex weight transfer, then drop the source mesh.
            # data_transfer runs ACTIVE -> selected: the source is active.
            bpy.ops.object.select_all(action="DESELECT")
            mesh.select_set(True)
            src_mesh.select_set(True)
            bpy.context.view_layer.objects.active = src_mesh
            bpy.ops.object.data_transfer(data_type="VGROUP_WEIGHTS",
                                         vert_mapping="NEAREST",
                                         layers_select_src="ALL",
                                         layers_select_dst="NAME",
                                         use_create=True)
            n_groups = len(mesh.vertex_groups)
            if n_groups == 0:
                raise SystemExit("--rig-from: weight transfer produced no "
                                 "vertex groups")
            # Drop the source mesh AND its datablocks, then give the variant's
            # materials their kit names back: the source imported first, so
            # the variant's same-named materials arrived as 'X.001' — which
            # the --textures wiring can't match (white, untextured bones).
            src_data = src_mesh.data
            bpy.data.objects.remove(src_mesh, do_unlink=True)
            bpy.data.meshes.remove(src_data)
            for m in [m for m in bpy.data.materials if m.users == 0]:
                bpy.data.materials.remove(m)
            for slot in mesh.material_slots:
                m = slot.material
                if m is None or "." not in m.name:
                    continue
                base = m.name.rsplit(".", 1)[0]
                if base and bpy.data.materials.get(base) is None:
                    print(f"[rig-from] material '{m.name}' -> '{base}'")
                    m.name = base
            mod = mesh.modifiers.new("Armature", "ARMATURE")
            mod.object = rig
            mesh.parent = rig
            print(f"[rig-from] weights transferred ({n_groups} groups) from "
                  f"{os.path.basename(opts['rig_from'])}")
        else:
            new_objs = import_fbx(mesh_fbx)
            rig = next((o for o in new_objs if o.type == "ARMATURE"), None)
            if rig is None:
                raise SystemExit(f"--skinned but no armature in {mesh_fbx}")
            rig.name = "rig"
            parts = [o for o in new_objs if o.type == "MESH"]
            if not parts:
                raise SystemExit(f"no mesh objects in {mesh_fbx}")
            if len(parts) > 1:
                bpy.ops.object.select_all(action="DESELECT")
                for o in parts:
                    o.select_set(True)
                bpy.context.view_layer.objects.active = parts[0]
                bpy.ops.object.join()
                print(f"[mesh] joined {len(parts)} objects")
            mesh = parts[0]
            if mesh.parent is not None:
                bpy.ops.object.select_all(action="DESELECT")
                mesh.select_set(True)
                bpy.context.view_layer.objects.active = mesh
                bpy.ops.object.parent_clear(type="CLEAR_KEEP_TRANSFORM")
            apply_all(mesh)
            apply_all(rig)
            fit_ground((mesh, rig), mesh)
            mesh.parent = rig  # tidy hierarchy for the exporter
        print(f"[fit] skinned mesh dims {mesh.dimensions.x:.2f} x "
              f"{mesh.dimensions.y:.2f} x {mesh.dimensions.z:.2f}")
        rig_h_fit = rig_height()

        # --mirror: flip the model LEFT<->RIGHT (art authored with the weapon
        # in the off hand — the library's attack clips lead with the RIGHT).
        # Mesh mirrors across X (winding then re-flipped so faces point out),
        # the rig mirrors with it, and Left/Right bone + vertex-group names
        # swap so the standard clips drive the swapped sides. Downstream code
        # (island assignment, --islands keys, re-rest) runs on the mirrored
        # model: island keys stay valid (vertex ORDER is untouched) but an
        # --islands BONE side must be authored post-mirror.
        if opts["mirror"]:
            # Unparent for the flips: applying the rig's -1 scale otherwise
            # COMPENSATES the child mesh (folds a canceling -1 into its object
            # transform), silently un-mirroring it in world space. Both sit at
            # identity here, so plain re-parenting is lossless.
            mesh.parent = None
            mesh.scale = (-1.0, 1.0, 1.0)
            apply_all(mesh)  # negative-scale apply reverses winding ITSELF —
            # faces stay outward; do NOT flip normals on top (inside-out).

            # Mirror the rig by negative-scale apply — one atomic operation.
            # (Hand-flipping edit_bones is a trap: welded head/tail edits
            # propagate between parent and child MID-ITERATION, half the rig
            # double-flips, and the result depends on bone order.)
            rig.scale = (-1.0, 1.0, 1.0)
            apply_all(rig)
            mesh.parent = rig

            def flipped(n):
                if "Left" in n:
                    return n.replace("Left", "Right")
                if "Right" in n:
                    return n.replace("Right", "Left")
                return None

            # Rename the bones only (two-phase to dodge collisions): Blender's
            # automatic group-follows-bone rename carries the mesh's vertex
            # groups along with each rename, keeping weights and bones in
            # lockstep.
            bsided = [b for b in rig.data.bones if flipped(b.name)]
            for b in bsided:
                b.name = "TMP__" + b.name
            for b in bsided:
                b.name = flipped(b.name.removeprefix("TMP__"))
            print(f"[mirror] mirrored across X; {len(bsided)} bones "
                  f"side-swapped (weight groups follow)")

        # --rigid-islands: a skeleton-and-plate kit has NO organic surface —
        # every island (skull, pauldron, greave, blade) is a hard piece, and
        # Mixamo's heat weights make them all flop/tear. Snap each island
        # rigid. An organic mesh should NOT set this — its soft weights stay.
        if not opts["rigid_islands"]:
            print("[rigid] soft Mixamo weights kept (--rigid-islands not set)")
        else:
            bm = bmesh.new()
            bm.from_mesh(mesh.data)
            bm.verts.ensure_lookup_table()
            seen = [False] * len(bm.verts)
            islands = []
            for v in bm.verts:
                if seen[v.index]:
                    continue
                comp = []
                stack = [v]
                while stack:
                    cur = stack.pop()
                    if seen[cur.index]:
                        continue
                    seen[cur.index] = True
                    comp.append(cur.index)
                    for e in cur.link_edges:
                        ov = e.other_vert(cur)
                        if not seen[ov.index]:
                            stack.append(ov)
                islands.append(comp)
            bm.free()

            # Assign by NEAREST BONE SEGMENT, not by weight dominance:
            # Mixamo's heat weights routinely give a sword piece to the chest
            # or an arm plate to the barely-moving clavicle, and snapping to
            # those breaks the model apart (disconnected blades, arms pinned
            # to the torso). Mesh and rig are ALIGNED here (same T-pose,
            # unlike the raw-kit bind that had to guess), so geometry is
            # unambiguous. Fingers fold into the hand (a gripped weapon must
            # not stretch to a fingertip); Shoulder/clavicle bones are
            # excluded as targets.
            SKIP_SEG = ("Thumb", "Index", "Middle", "Ring", "Pinky", "Shoulder")
            segs = []
            for b in rig.data.bones:
                if any(t in b.name for t in SKIP_SEG):
                    continue
                segs.append((b.name, rig.matrix_world @ b.head_local,
                             rig.matrix_world @ b.tail_local))

            def dist_pt_seg(p, a, b):
                ab = b - a
                denom = ab.dot(ab)
                t = max(0.0, min(1.0, (p - a).dot(ab) / denom)) \
                    if denom > 1e-9 else 0.0
                return (p - (a + ab * t)).length

            mw = mesh.matrix_world
            co = [mw @ v.co for v in mesh.data.vertices]

            # --- rest-position REPAIR (--original <upload mesh>) --------------
            # Mixamo re-poses the A-pose art into T-pose USING ITS WEIGHTS, so
            # any island it weighted badly arrives with its rest geometry
            # displaced (arm plates half-carried toward the chest). Rigid
            # weights alone would freeze that displacement. Repair: per bone,
            # solve the rigid A->T transform (Kabsch) from the verts Mixamo
            # weighted CONFIDENTLY (>= 0.9 — the well-bound cores), then
            # recompute every rigid island's rest from the ORIGINAL art
            # through its assigned bone's transform. Same vertex order in
            # upload and download (Mixamo preserves geometry).
            bone_xform = {}
            orig_co = None
            if opts["original"]:
                import numpy as np
                before = set(bpy.data.objects)
                bpy.ops.import_scene.fbx(filepath=opts["original"])
                oobjs = [o for o in bpy.data.objects if o not in before]
                omeshes = [o for o in oobjs if o.type == "MESH"]
                over = sum(len(o.data.vertices) for o in omeshes)
                if over != len(mesh.data.vertices):
                    print(f"[repair] WARNING vertex count mismatch "
                          f"(original {over} vs download "
                          f"{len(mesh.data.vertices)}) - repair skipped")
                else:
                    oco = []
                    for o in omeshes:  # single mesh expected; keep file order
                        m = o.matrix_world
                        oco.extend((m @ v.co) for v in o.data.vertices)
                    # Pre-scale the original to the download's baked size; a
                    # --mirror run mirrors the original with the download so
                    # the per-bone A->T fits stay pure rotations.
                    oz = [p.z for p in oco]
                    dz = [p.z for p in co]
                    s0 = (max(dz) - min(dz)) / max(max(oz) - min(oz), 1e-6)
                    sx = -s0 if opts["mirror"] else s0
                    orig_co = np.array([[p.x * sx, p.y * s0, p.z * s0]
                                        for p in oco])
                    down_co = np.array([[p.x, p.y, p.z] for p in co])
                    gname = {g.index: g.name for g in mesh.vertex_groups}
                    conf = {}
                    for i, v in enumerate(mesh.data.vertices):
                        for ge in v.groups:
                            if ge.weight >= 0.9:
                                conf.setdefault(gname[ge.group], []).append(i)
                                break
                    for bname, idx in conf.items():
                        if len(idx) < 8:
                            continue
                        A = orig_co[idx]
                        B = down_co[idx]
                        ca, cb = A.mean(0), B.mean(0)
                        H = (A - ca).T @ (B - cb)
                        U, S, Vt = np.linalg.svd(H)
                        d = np.sign(np.linalg.det(Vt.T @ U.T))
                        D = np.diag([1.0, 1.0, d])
                        R = Vt.T @ D @ U.T
                        bone_xform[bname] = (R, cb - R @ ca)
                    print(f"[repair] solved A->T transforms for "
                          f"{len(bone_xform)} bone(s)")
                for o in oobjs:
                    bpy.data.objects.remove(o, do_unlink=True)

            def repair_bone_for(name):
                # The assigned bone's transform, else the nearest ancestor's.
                b = rig.data.bones.get(name)
                while b is not None:
                    if b.name in bone_xform:
                        return bone_xform[b.name]
                    b = b.parent
                return None

            # With the original art available, ASSIGN in A-space too: map each
            # bone segment back through its inverse A->T transform and measure
            # against the clean original positions — a plate that Mixamo
            # dragged to the chest still assigns to the ARM it really wraps.
            segs_assign = segs
            co_assign = co
            if orig_co is not None and bone_xform:
                import mathutils
                segs_assign = []
                for name, h, t_ in segs:
                    xf = repair_bone_for(name)
                    if xf is None:
                        continue
                    R, t = xf
                    hn = R.T @ (np.array([h.x, h.y, h.z]) - t)
                    tn = R.T @ (np.array([t_.x, t_.y, t_.z]) - t)
                    segs_assign.append((name, mathutils.Vector(hn),
                                        mathutils.Vector(tn)))
                co_assign = [mathutils.Vector(p) for p in orig_co]

            # --islands "<minvert>=<bone>;...": per-island ASSIGNMENT OVERRIDES
            # for the pieces nearest-segment gets wrong (a strapped shield's
            # face/straps voting Hand/Arm, wrist cuffs coin-flipping across the
            # joint). An island is keyed by its SMALLEST vertex index — stable
            # for a given source FBX (vertex order is preserved) and printed in
            # the per-island log below, so a bad piece's key can be read
            # straight off a previous run. Bone names may omit the mixamorig:
            # prefix. The special value `soft` keeps Mixamo's BLENDED weights
            # for that island instead of any rigid snap — for a piece that must
            # BEND across joints (the vertebral column spanning pelvis to neck:
            # snapped rigid it follows one bone and floats free of both ends
            # whenever a hit react folds the torso).
            SOFT = object()
            overrides = {}
            for tok in filter(None, opts["islands"].split(";")):
                k, _, bn = tok.partition("=")
                if not bn:
                    raise SystemExit(f"--islands entry '{tok}' is not key=bone")
                if bn.lower() == "soft":
                    overrides[int(k)] = SOFT
                    continue
                if not bn.startswith("mixamorig:"):
                    bn = "mixamorig:" + bn
                if rig.data.bones.get(bn) is None:
                    raise SystemExit(f"--islands: unknown bone '{bn}'")
                overrides[int(k)] = bn

            vgs = {g.name: g for g in mesh.vertex_groups}
            for b in rig.data.bones:  # groups the download didn't weight
                if b.name not in vgs:
                    vgs[b.name] = mesh.vertex_groups.new(name=b.name)
            rigid_n = repaired_n = 0
            inv = mw.inverted()
            for comp in islands:
                nb = [min(segs_assign,
                          key=lambda s: dist_pt_seg(co_assign[i], s[1], s[2]))[0]
                      for i in comp]
                dom, dom_n = Counter(nb).most_common(1)[0]
                ov = overrides.pop(min(comp), None)
                if ov is SOFT:
                    print(f"[rigid] island {min(comp)} ({len(comp)} verts) "
                          f"SOFT — Mixamo's blended weights kept")
                    continue
                if ov is not None:
                    print(f"[rigid] island {min(comp)} ({len(comp)} verts) "
                          f"OVERRIDE {dom} -> {ov}")
                    dom = ov
                elif len(comp) > 300:
                    print(f"[rigid] island {min(comp)} ({len(comp)} verts) -> "
                          f"{dom} ({dom_n / len(comp):.0%})")
                for vg in mesh.vertex_groups:
                    if vg.name != dom:
                        vg.remove(comp)
                vgs[dom].add(comp, 1.0, "REPLACE")
                rigid_n += 1
                if orig_co is not None:
                    xf = repair_bone_for(dom)
                    if xf is not None:
                        import mathutils
                        R, t = xf
                        for i in comp:
                            p = R @ orig_co[i] + t
                            mesh.data.vertices[i].co = inv @ mathutils.Vector(
                                (p[0], p[1], p[2]))
                        repaired_n += 1
            mesh.data.update()
            print(f"[rigid] snapped {rigid_n} island(s) to nearest bone "
                  f"segments; rest positions repaired for {repaired_n}")
            if overrides:  # keys that matched no island = stale/typo'd
                raise SystemExit(f"--islands keys matched no island: "
                                 f"{sorted(overrides)}")

        # ---- RE-REST to the clip library's skeleton --------------------------
        # Blender pose rotations are RELATIVE TO REST, and the library clips
        # were authored on the standard Mixamo skeleton (T-pose rest). The
        # download's rest is fitted to the uploaded art instead, so wherever
        # the two rests disagree (an A-pose's arm chain, most of all) every
        # clip inherits the difference and the limbs fold across the body.
        # Align the basis once at import: pose each bone to the standard rest
        # ORIENTATION (translations follow the parents, keeping the art's
        # proportions), bake that pose into the mesh (armature modifier
        # apply), then apply it as the new rest. A download that already
        # matches the standard rest poses an identity delta, so this runs for
        # every skinned import.
        from mathutils import Matrix
        bpy.ops.object.select_all(action="DESELECT")
        rig.select_set(True)
        bpy.context.view_layer.objects.active = rig
        bpy.ops.object.mode_set(mode="POSE")

        def walk(pb):  # parents before children — pb.matrix reads the parent
            yield pb
            for c in pb.children:
                yield from walk(c)

        reposed = 0
        for root_pb in [p for p in rig.pose.bones if p.parent is None]:
            for pb in walk(root_pb):
                m = std_rest.get(pb.name)
                if m is None:
                    continue  # a bone the library skeleton lacks keeps its rest
                loc = pb.matrix.to_translation()
                pb.matrix = Matrix.Translation(loc) @ m.to_4x4()
                bpy.context.view_layer.update()
                reposed += 1
        bpy.ops.object.mode_set(mode="OBJECT")

        # Bake the posed shape into the mesh, then the pose into the rest.
        arm_mod = next((md for md in mesh.modifiers if md.type == "ARMATURE"),
                       None)
        if arm_mod is None:
            raise SystemExit("--skinned mesh has no armature modifier to bake")
        mod_name = arm_mod.name
        bpy.ops.object.select_all(action="DESELECT")
        mesh.select_set(True)
        bpy.context.view_layer.objects.active = mesh
        bpy.ops.object.modifier_apply(modifier=mod_name)
        bpy.ops.object.select_all(action="DESELECT")
        rig.select_set(True)
        bpy.context.view_layer.objects.active = rig
        bpy.ops.object.mode_set(mode="POSE")
        bpy.ops.pose.armature_apply(selected=False)
        bpy.ops.object.mode_set(mode="OBJECT")
        arm_mod = mesh.modifiers.new("Armature", "ARMATURE")
        arm_mod.object = rig
        print(f"[rest] re-rested {reposed} bone(s) onto the clip library's "
              f"skeleton basis")

        # Re-GROUND. fit_ground put the feet on z=0, but that was before the
        # rest repair and the re-rest, which reshape the body and take the feet
        # with them (a re-rested A-pose stands up straighter and lifts off the
        # floor). Models are authored feet-at-y=0 and the engine draws a monster
        # from there, so whatever the last pass leaves is what the player sees
        # floating. Ground AFTER the reshaping, not before it.
        # (Unparent for the shift, as the mirror pass does: both objects sit at
        # identity here, so the re-parent is lossless. Applying the translation
        # into the data matters — the engine walks the joint hierarchy only and
        # would drop a translation left on the rig OBJECT.)
        zs = [(mesh.matrix_world @ v.co).z for v in mesh.data.vertices]
        dz = -min(zs)
        if abs(dz) > 1e-5:
            mesh.parent = None
            for o in (mesh, rig):
                o.location.z += dz
                apply_all(o, rotation=False, scale=False)
            mesh.parent = rig
            print(f"[fit] re-grounded {dz:+.3f} units after re-resting")
    else:
        # ---- import our mesh, co-face the armature, fit to it -------------------
        # OBJ sources (bought kits) import via the wm importer and often split
        # into one object per group (bones/armor/weapons) — JOIN them into one
        # mesh; the material slots survive the join, so a multi-material kit
        # keeps its parts.
        if mesh_fbx.lower().endswith(".obj"):
            before = set(bpy.data.objects)
            try:
                bpy.ops.wm.obj_import(filepath=mesh_fbx)
            except AttributeError:
                bpy.ops.import_scene.obj(filepath=mesh_fbx)
            new_objs = [o for o in bpy.data.objects if o not in before]
        else:
            new_objs = import_fbx(mesh_fbx)
        parts = [o for o in new_objs if o.type == "MESH"]
        if not parts:
            raise SystemExit(f"no mesh objects in {mesh_fbx}")
        bpy.ops.object.select_all(action="DESELECT")
        for o in parts:
            o.select_set(True)
        bpy.context.view_layer.objects.active = parts[0]
        if len(parts) > 1:
            bpy.ops.object.join()
            print(f"[mesh] joined {len(parts)} objects")
        mesh = parts[0]
        bpy.ops.object.select_all(action="DESELECT")
        mesh.select_set(True)
        bpy.context.view_layer.objects.active = mesh
        # The importers park their axis conversion (Y-up -> Z-up) on the OBJECT
        # rotation; bake it FIRST — assigning rotation_euler below would
        # silently discard it and the mesh would bind lying down.
        bpy.ops.object.transform_apply(rotation=True)
        mesh.rotation_euler = (0, 0, mesh_yaw)  # co-face the +Y Mixamo armature
        bpy.ops.object.transform_apply(rotation=True)

        zs = [(mesh.matrix_world @ v.co).z for v in mesh.data.vertices]
        s = ref_h / (max(zs) - min(zs))
        mesh.scale = (s, s, s)
        bpy.ops.object.transform_apply(scale=True)
        zs = [(mesh.matrix_world @ v.co).z for v in mesh.data.vertices]
        mesh.location.z -= min(zs)  # feet to z=0
        bpy.ops.object.transform_apply(location=True)
        print(f"[fit] mesh dims {mesh.dimensions.x:.2f} x {mesh.dimensions.y:.2f} "
              f"x {mesh.dimensions.z:.2f}")
        rig_h_fit = rig_height()

    # Materials: with a --textures map, KEEP the material slots (the engine's
    # multi-material monster path renders one primitive per material) and wire
    # each slot's albedo/normal images explicitly — bought-kit MTLs reference
    # the author's machine and omit normal maps entirely. Without a map, the
    # legacy single-material merge (texture= set bound by the engine) applies.
    tex_map = {}
    for pair in (p for p in opts["textures"].split(";") if p.strip()):
        mat_name, _, files = pair.partition("=")
        tex_map[mat_name.strip()] = files.strip()

    flip_dir = tempfile.mkdtemp(prefix="animlib_norm_") if opts["flip_green"] else ""

    def load_map(fname, non_color=False, flip_green=False):
        img = bpy.data.images.load(os.path.join(opts["tex_dir"], fname))
        if non_color:  # read/write the stored bytes, not a colour-managed view
            img.colorspace_settings.name = "Non-Color"
        if not flip_green:
            return img
        import numpy as np
        px = np.empty(len(img.pixels), dtype=np.float32)
        img.pixels.foreach_get(px)
        px[1::4] = 1.0 - px[1::4]
        img.pixels.foreach_set(px)
        # Save the flipped pixels out and re-load THAT file. The glTF exporter
        # embeds an image's source file whenever it can, so an in-memory edit
        # would be dropped without a word and ship the unflipped map.
        out = os.path.join(flip_dir, fname)
        img.filepath_raw = out
        img.file_format = "PNG"
        img.save()
        bpy.data.images.remove(img)
        img = bpy.data.images.load(out)
        img.colorspace_settings.name = "Non-Color"
        return img

    if tex_map:
        for slot in mesh.material_slots:
            mat = slot.material
            if mat is None or mat.name not in tex_map:
                key = mat.name if mat else "<empty>"
                print(f"[tex] WARNING no texture mapping for material '{key}'")
                continue
            spec = tex_map[mat.name].split(":")
            mat.use_nodes = True
            nt = mat.node_tree
            bsdf = next((n for n in nt.nodes if n.type == "BSDF_PRINCIPLED"), None)
            if bsdf is None:
                bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
                out = next((n for n in nt.nodes if n.type == "OUTPUT_MATERIAL"),
                           None) or nt.nodes.new("ShaderNodeOutputMaterial")
                nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
            if spec[0]:
                img = load_map(spec[0])
                node = nt.nodes.new("ShaderNodeTexImage")
                node.image = img
                nt.links.new(node.outputs["Color"], bsdf.inputs["Base Color"])
            if len(spec) > 1 and spec[1]:
                # An embedded map never passes through ImportTextures, so it
                # never gets that importer's green flip. DON'T assume it needs
                # one: reasoning from "glTF is green-up by spec" is a trap,
                # because glTF also puts v=0 at the TOP of the image while
                # OpenGL puts it at the bottom, and the two flips cancel. Every
                # bought normal map measured so far — the Skeleton Army kit's
                # four included — already matched the engine.
                # MEASURE instead. A normal map is a height GRADIENT, so it must
                # be curl-free: compare median|dp/dy - dq/dx| against the same
                # with green negated (p=-nx/nz, q=-ny/nz). The smaller residual
                # is the real orientation, and assets/textures/*_n.png are 200-odd
                # known-good samples to calibrate against. Flip only on a clear
                # margin — the engine's own maps score ~3.4x, and anything near
                # 1x is noise.
                img = load_map(spec[1], non_color=True, flip_green=opts["flip_green"])
                node = nt.nodes.new("ShaderNodeTexImage")
                node.image = img
                nm = nt.nodes.new("ShaderNodeNormalMap")
                nt.links.new(node.outputs["Color"], nm.inputs["Color"])
                nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
            orm = len(spec) > 2 and spec[2]
            if orm:
                # glTF ORM: R=occlusion, G=roughness, B=metallic — the layout the
                # engine's MaterialParams::metalRough slot samples, so exporting
                # this image as the metallicRoughness texture hands the shader its
                # occlusion for free. Drive Roughness from G and leave Metallic on
                # its 0 default: B is 1.0 across both of this pack's maps, and a
                # metallic FACTOR of 1 would render the bones as chrome.
                img = load_map(spec[2], non_color=True)
                node = nt.nodes.new("ShaderNodeTexImage")
                node.image = img
                sep = nt.nodes.new("ShaderNodeSeparateColor")
                nt.links.new(node.outputs["Color"], sep.inputs["Color"])
                nt.links.new(sep.outputs["Green"], bsdf.inputs["Roughness"])
            else:
                bsdf.inputs["Roughness"].default_value = 0.75
            bsdf.inputs["Metallic"].default_value = 0.0
            print(f"[tex] '{mat.name}' <- {tex_map[mat.name]}"
                  f"{' (green-flipped)' if opts['flip_green'] else ''}")
    else:
        # merge to ONE material -> single glTF primitive (single-set monsters)
        for p in mesh.data.polygons:
            p.material_index = 0
        while len(mesh.data.materials) > 1:
            mesh.data.materials.pop(index=1)

    if not opts["skinned"] and not opts["rig_from"]:
        # Bone segments (optionally excluding finger bones so a coarse hand
        # binds rigidly to the Hand bone instead of stretching to a fingertip).
        FINGERS = ("Thumb", "Index", "Middle", "Ring", "Pinky")
        segs = []
        for b in rig.data.bones:
            if fingers_excluded and any(f in b.name for f in FINGERS):
                continue
            segs.append((b.name, bone_world(b, True), bone_world(b, False)))

        def dist_pt_seg(p, a, b):
            ab = b - a
            denom = ab.dot(ab)
            t = max(0.0, min(1.0, (p - a).dot(ab) / denom)) if denom > 1e-9 else 0.0
            return (p - (a + ab * t)).length

        # ---- hybrid rigid bind: compact island -> one bone; else per-vertex ----
        bm = bmesh.new()
        bm.from_mesh(mesh.data)
        bm.verts.ensure_lookup_table()
        seen = [False] * len(bm.verts)
        islands = []
        for v in bm.verts:
            if seen[v.index]:
                continue
            comp = []
            stack = [v]
            while stack:
                cur = stack.pop()
                if seen[cur.index]:
                    continue
                seen[cur.index] = True
                comp.append(cur.index)
                for e in cur.link_edges:
                    ov = e.other_vert(cur)
                    if not seen[ov.index]:
                        stack.append(ov)
            islands.append(comp)
        bm.free()

        vgs = {b.name: mesh.vertex_groups.new(name=b.name) for b in rig.data.bones}
        mw = mesh.matrix_world
        co = [mw @ v.co for v in mesh.data.vertices]
        rigid_n = span_n = 0
        for comp in islands:
            nb = [min(segs, key=lambda s: dist_pt_seg(co[i], s[1], s[2]))[0]
                  for i in comp]
            dom, dom_n = Counter(nb).most_common(1)[0]
            if len(comp) > 300:  # the big pieces (shield, blade, torso shells)
                print(f"[bind] island({len(comp)} verts) -> {dom} "
                      f"({dom_n / len(comp):.0%})")  # eyeball the assignment
            # A spanning island falls back to a PER-VERTEX snap, which tears it
            # apart: each vertex chases whatever bone is nearest, so a ribcage
            # that reaches from pelvis to neck comes apart rib by rib and a
            # scapula splits across the joint it lies over. --rigid-islands says
            # the model has no soft tissue — every island is a hard piece — so
            # keep it whole on its dominant bone however poorly it scored. It is
            # the same call the Skeleton Army kit makes, and a stiff torso beats
            # a shredded one.
            if opts["rigid_islands"] or dom_n / len(comp) >= 0.70:
                vgs[dom].add(comp, 1.0, "REPLACE")
                rigid_n += 1
            else:                          # spanning island -> per-vertex
                for i, bone in zip(comp, nb):
                    vgs[bone].add([i], 1.0, "REPLACE")
                span_n += 1
        print(f"[bind] {len(islands)} islands: {rigid_n} rigid, {span_n} per-vertex")

        mesh.parent = rig
        mesh.matrix_parent_inverse = rig.matrix_world.inverted()
        mod = mesh.modifiers.new("Armature", "ARMATURE")
        mod.object = rig
    # (--skinned / --rig-from: the weights + armature modifier already drive
    # the mesh — re-binding here would CLOBBER the rigid-island overrides and
    # the soft islands.)

    # ---- verify the size survived the reshaping passes -------------------------
    # The fit lands mid-pipeline and everything after it reshapes the model —
    # mirroring, the Kabsch rest repair, re-resting onto the clip skeleton — any
    # of which can quietly rescale it. A wrong scale is invisible in the file and
    # only shows up in-game as a monster standing through the ceiling, so measure
    # again and refuse to write rather than emit a model that needs a modelscale
    # to correct it.
    drift = rig_height() / rig_h_fit
    if abs(drift - 1.0) > 0.02:
        raise SystemExit(f"[fit] size drifted {drift:.3f}x between the fit and "
                         f"export (rig {rig_h_fit:.3f} -> {rig_height():.3f} "
                         f"units) — refusing to write {out_path}")
    zs = [(mesh.matrix_world @ v.co).z for v in mesh.data.vertices]
    print(f"[fit] final: rig {rig_height():.3f}, mesh {max(zs) - min(zs):.3f} "
          f"units standing on {min(zs):+.3f} (target {ref_h:.3f})")

    # ---- push actions onto NLA tracks so glTF exports all as clips ------------
    if rig.animation_data is None:
        rig.animation_data_create()
    rig.animation_data.action = None
    for clip_name, _ in clip_files:
        act = bpy.data.actions[clip_name]
        track = rig.animation_data.nla_tracks.new()
        track.name = clip_name
        track.strips.new(clip_name, int(act.frame_range[0]), act)

    # ---- export glTF ----------------------------------------------------------
    bpy.context.view_layer.update()
    bpy.ops.object.select_all(action="DESELECT")
    mesh.select_set(True)
    rig.select_set(True)
    bpy.context.view_layer.objects.active = rig
    # A textured multi-material bake EMBEDS its images and ships as one GLB
    # (written to the .gltf output name — the engine's monster loader reads by
    # that name and cgltf sniffs the binary; same convention as the bought
    # rigged crawlers). The legacy single-set path keeps its image-less export.
    if tex_map:
        fmt = "GLB"
        img_fmt = "AUTO"
    else:
        fmt = "GLTF_SEPARATE" if out_path.lower().endswith(".gltf") else "GLB"
        img_fmt = "NONE"
    export_path = out_path
    if fmt == "GLB" and out_path.lower().endswith(".gltf"):
        export_path = out_path[:-5] + ".glb"  # exporter names it; rename below
    bpy.ops.export_scene.gltf(
        filepath=export_path,
        export_format=fmt,
        use_selection=True,
        export_animations=True,
        export_animation_mode="NLA_TRACKS",
        export_yup=True,
        export_image_format=img_fmt,
    )
    if export_path != out_path:
        os.replace(export_path, out_path)
    print(f"[done] wrote {out_path} ({fmt}, {len(clip_files)} clips)")
    emit_catalog(rows, opts["catalog_out"])


def main():
    opts = parse_args(sys.argv)
    if opts["plan"]:
        if not opts["library"]:
            raise SystemExit("usage: --plan <library_root> [--catalog-out f]")
        run_plan(opts)
    else:
        run_bake(opts)


if __name__ == "__main__":
    main()
