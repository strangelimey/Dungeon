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
# Two modes:
#   --plan : walk the library, print the clip plan + catalog rows, and exit. Pure
#            python (NO Blender) — a dry run / CI check of the folder layout.
#   (bake) : run under Blender to rig-bind the mesh and export the glTF library.
#
# Usage (bake, under Blender):
#   blender --background --factory-startup --python tools/ImportAnimLibrary.py -- \
#       <mesh.fbx> <library_root> <out.gltf> <ref_skeleton.gltf> \
#       [--catalog-out <file>] [--mesh-yaw DEG] [--keep-fingers]
# Usage (plan, plain python):
#   python tools/ImportAnimLibrary.py --plan <library_root> [--catalog-out <file>]
#
# Exit code is 0 on success, non-zero on failure (a driver script keys off it).

import sys
import os
import glob
import re

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
    opts = {"plan": plan, "mesh": None, "library": None, "out": None, "ref": None,
            "catalog_out": None, "mesh_yaw": 90.0, "keep_fingers": False,
            "textures": "", "tex_dir": "", "arm_drop": 0.0}
    usage = ("usage: <mesh.fbx|.obj> <library_root> <out.gltf> <ref_skeleton.gltf> "
             "[--catalog-out f] [--mesh-yaw deg] [--keep-fingers] "
             "[--textures '<mat>=<albedo>[:<normal>];...' --tex-dir <dir>]  |  "
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
        elif a == "--mesh-yaw":
            opts["mesh_yaw"] = float(value(i, a)); i += 2
        elif a == "--keep-fingers":
            opts["keep_fingers"] = True; i += 1
        elif a == "--textures":
            opts["textures"] = value(i, a); i += 2
        elif a == "--tex-dir":
            opts["tex_dir"] = value(i, a); i += 2
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
        if len(pos) < 4:
            raise SystemExit("usage: <mesh.fbx> <library_root> <out.gltf> "
                             "<ref_skeleton.gltf> [--catalog-out f] [--mesh-yaw deg] "
                             "[--keep-fingers]")
        opts["mesh"], opts["library"], opts["out"], opts["ref"] = pos[:4]
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

    mesh_fbx, out_path, ref_gltf = opts["mesh"], opts["out"], opts["ref"]
    mesh_yaw = math.radians(opts["mesh_yaw"])
    fingers_excluded = not opts["keep_fingers"]

    bpy.ops.wm.read_factory_settings(use_empty=True)

    def import_fbx(path):
        before = set(bpy.data.objects)
        bpy.ops.import_scene.fbx(filepath=path)
        return [o for o in bpy.data.objects if o not in before]

    # Measure the reference model (existing <type>.gltf) to match its in-engine
    # height + ground placement exactly, then clear the scene.
    bpy.ops.import_scene.gltf(filepath=ref_gltf)
    _rp = [o.matrix_world @ v.co for o in bpy.data.objects if o.type == "MESH"
           for v in o.data.vertices]
    ref_h = max(p.z for p in _rp) - min(p.z for p in _rp)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    print(f"[ref] height={ref_h:.3f}")

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

    # ---- import our mesh, co-face the armature, fit to it ----------------------
    # OBJ sources (bought kits) import via the wm importer and often split into
    # one object per group (bones/armor/weapons) — JOIN them into one mesh; the
    # material slots survive the join, so a multi-material kit keeps its parts.
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
    # rotation; bake it FIRST — assigning rotation_euler below would silently
    # discard it and the mesh would bind lying down (exploded animations).
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
    print(f"[fit] mesh dims {mesh.dimensions.x:.2f} x {mesh.dimensions.y:.2f} x "
          f"{mesh.dimensions.z:.2f}")

    # Materials: with a --textures map, KEEP the material slots (the engine's
    # multi-material monster path renders one primitive per material) and wire
    # each slot's albedo/normal images explicitly — bought-kit MTLs reference
    # the author's machine and omit normal maps entirely. Without a map, the
    # legacy single-material merge (texture= set bound by the engine) applies.
    tex_map = {}
    for pair in (p for p in opts["textures"].split(";") if p.strip()):
        mat_name, _, files = pair.partition("=")
        tex_map[mat_name.strip()] = files.strip()
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
                img = bpy.data.images.load(os.path.join(opts["tex_dir"], spec[0]))
                node = nt.nodes.new("ShaderNodeTexImage")
                node.image = img
                nt.links.new(node.outputs["Color"], bsdf.inputs["Base Color"])
            if len(spec) > 1 and spec[1]:
                img = bpy.data.images.load(os.path.join(opts["tex_dir"], spec[1]))
                img.colorspace_settings.name = "Non-Color"
                node = nt.nodes.new("ShaderNodeTexImage")
                node.image = img
                nm = nt.nodes.new("ShaderNodeNormalMap")
                nt.links.new(node.outputs["Color"], nm.inputs["Color"])
                nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
            bsdf.inputs["Roughness"].default_value = 0.75
            bsdf.inputs["Metallic"].default_value = 0.0
            print(f"[tex] '{mat.name}' <- {tex_map[mat.name]}")
    else:
        # merge to ONE material -> single glTF primitive (single-set monsters)
        for p in mesh.data.polygons:
            p.material_index = 0
        while len(mesh.data.materials) > 1:
            mesh.data.materials.pop(index=1)

    # Bone segments (optionally excluding finger bones so a coarse hand binds
    # rigidly to the Hand bone instead of stretching to a fingertip).
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

    # ---- hybrid rigid bind: compact island -> one bone; spanning -> per-vertex -
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
        nb = [min(segs, key=lambda s: dist_pt_seg(co[i], s[1], s[2]))[0] for i in comp]
        dom, dom_n = Counter(nb).most_common(1)[0]
        if len(comp) > 300:  # the big pieces (shield, blade, torso shells) —
            print(f"[bind] island({len(comp)} verts) -> {dom} "
                  f"({dom_n / len(comp):.0%})")  # eyeball the assignment
        if dom_n / len(comp) >= 0.70:  # compact shell -> one rigid bone
            vgs[dom].add(comp, 1.0, "REPLACE")
            rigid_n += 1
        else:                          # spanning island -> articulate per-vertex
            for i, bone in zip(comp, nb):
                vgs[bone].add([i], 1.0, "REPLACE")
            span_n += 1
    print(f"[bind] {len(islands)} islands: {rigid_n} rigid, {span_n} per-vertex")

    mesh.parent = rig
    mesh.matrix_parent_inverse = rig.matrix_world.inverted()
    mod = mesh.modifiers.new("Armature", "ARMATURE")
    mod.object = rig

    # ---- push actions onto NLA tracks so glTF exports all as clips ------------
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
