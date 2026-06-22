# Animation

Design notes for the dungeon's animation work. This is the living reference for
the `animation` branch — keep it in sync with the `animation` entry in Claude's
project memory (the two carry the same design, status, and remaining work; update
both together).

The sections describe the **target** design; "Current implementation status"
records what the **code** actually does today and where it diverges.

## The standard (and what we already have)

Skeletal animation here follows the universal real-time approach, which is also
exactly what glTF encodes: a **skinned mesh + linear-blend skinning** driven by a
**joint matrix palette**.

- Skeleton = joint tree; each joint has a parent, a local rest TRS, and an
  **inverse bind matrix** (model space → joint local space).
- Each vertex carries up to **4 joint indices + 4 weights** (the universal cap).
- A clip = per-(joint, T/R/S) **channels**, each a sampler of `times[]`+`values[]`
  interpolated between keys (rotation slerp/nlerp, T/S lerp).
- Per frame: sample clip → local TRS → globals (parent-first) →
  `palette[j] = inverseBind[j] × global[j]` → upload to `b2`; the VS blends
  `Σ weight[i]·palette[joint[i]]·vertex`.

**This pipeline is already fully implemented in the engine** — the infra is not
the work:

- Data model: `assets::Vertex` (joints[4]/weights[4]), `JointData`,
  `SkeletonData` (topo-sorted), `AnimationChannelData`/`AnimationClipData`
  (`src/Assets/Model.h`).
- glTF loader reads skins, inverse-binds, channels/samplers (`src/Assets/Model.cpp`).
- `anim::Animator` runs the exact pipeline above into `b2` of `scene.hlsl`
  (`src/Animation/Animator.*`).
- Every monster already owns an `Animator` playing `"idle"`
  (`DungeonWorld_Load.cpp` MakeMonster); the serpent pillar plays `"sway"`.
  `kMaxSkinJoints=128`; skinning palettes upload once per frame.
- AssetBaker bakes procedural rigs (skeleton/mummy share a 7-joint humanoid;
  `tools/AssetBaker/ModelBaker.cpp` + `GltfWriter.cpp`).

So "adding skeletal animation" is **clips + a state machine with blending**, not a
new pipeline.

## Motivation

Monsters chase and melee the party (async AI + grid-step glide), but the act of
attacking has no visual — a hit just drains health. The rigs are
skinning-ready but only ever play `"idle"`. This thread brings them to life.

## Goals

(Ordered.)

1. **Clip cross-fade in the Animator** — the one real infra gap: today `Play()` is
   a hard single-clip cut. Add a short cross-fade between outgoing/incoming pose
   (blend two sampled poses by a weight that ramps over a transition window).
   Foundation for everything below. Optional later: additive layers (e.g. a
   hit-recoil over a walk).
2. **Clip state machine** — drive which clip plays from monster state
   (idle ↔ walk ↔ attack ↔ die), advanced by the host per frame at the monster's
   own cadence (like AI thinking-vs-acting). Walk plays over the grid-step glide
   tween; attack syncs to the melee cadence + the landed-hit moment (pairs with
   the deferred hit-feedback splat); die plays once on slay.
3. **Procedural clips** — author attack / walk / hit / death onto the existing
   7-joint rigs in `ModelBaker`/`GltfWriter`. Proves the state machine without
   external assets.
4. **(Later) authored models** — buy/import rigged+animated glTF via
   `AssetBaker import-model` (already ingests animation channels) to replace the
   procedural rigs. Deferred; goals 1–3 don't depend on it.

## Decisions

- **Clip source: procedural now, authored later.** Build out the state machine on
  the procedural rigs (goal 3); swap in bought rigged models down the line (goal 4).
- **Blending: cross-fade, not hard cuts.** Extend the Animator (goal 1) rather than
  shipping popping transitions.

## Notes / gotchas

- New per-monster animation state that's reload-visible (e.g. which clip + phase a
  monster is mid-death on) must round-trip through `CaptureState`/`ApplyState` +
  `SaveData` (save/load convention). Pure cosmetic idle phase need not.
- Animation lib is at the `Animation/Graphics` layer; palettes already cache +
  upload once per frame — keep that (don't regress the per-frame skinning cache).

## Current implementation status

**Goal 1 (Animator cross-fade) — DONE, compiles clean.** `anim::Animator` now
supports a snapshot cross-fade:
- `Play(name, loop, fade)` — `fade > 0` freezes the current evaluated local pose
  (`m_snap*`) and blends it toward the new clip over `fade` seconds; `fade == 0`
  is the old hard cut. Re-`Play`ing the already-active *looping* clip (not
  mid-fade) is a no-op, so a host can call `Play` every frame for a held state;
  one-shot (`loop == false`) clips always restart. `Fading()` query added.
- Blend is in **local TRS space** before globals/palette (lerp T/S, slerp R),
  smoothstep-eased — never blend matrices/palettes.
- Refactor: old `SamplePose` split into `SampleClip(clip→local TRS arrays)` +
  `BuildPalette(local TRS→globals→palette)`; `Update` samples the active clip,
  blends the snapshot while fading, then builds the palette.
- Existing call sites (`Play("idle")`, `Play("sway")`) unchanged — `fade`
  defaults to 0. Nothing triggers a fade yet; that arrives with goal 2.

**Goal 2 (clip state machine) — DONE, verified in-game.** `DungeonWorld::
DriveMonsterAnim` runs per monster each frame (including downed ones), picks a
clip by priority **die > attack > walk > idle**, and cross-fades (~0.12s) on a
change via the new `Animator::Play(name, loop, fade)`. Per-`Monster` state
(`DungeonWorld.h`): `anim` (current clip), `attackAnim` (swing one-shot, set in
`MonsterAttack`), `deathAnim` (keeps the corpse drawn + animating through the
death clip, then it vanishes — 0 if no clip, so behaviour matches the old instant
removal). `ClipDuration` gates a clip request to what the model actually has, so a
missing clip silently leaves idle playing (no warning spam, no pop). Render +
shadow gates updated to keep a dying monster on-screen while `deathAnim > 0`.

**Goal 3 (procedural clips) — DONE, verified.** `walk`/`attack`/`die` authored in
`tools/AssetBaker/ModelBaker.cpp` for both rigs, so skeleton + mummy + blob all
bake 4 clips each. Humanoid (`BuildHumanoid`): leg/arm/spine articulation (below).
Blob (`BuildBlob`): pure squash-and-stretch since the 2-joint base/top rig never
turns — walk is a bouncing ooze (squashed+low → tall+hopped, loops), attack a
vertical pounce (gather → rear up → slam flat, one-shot), die a deflate to a wide
flat puddle with the top collapsing onto the base (one-shot, holds). Verified
in-game (docs/anim_v_blob_*.png).

**Rig upgrade — DONE, verified.** The humanoid rig went from 7 joints (whole-limb
rigid struts) to **15 joints**: torso (root/spine/head, indices 0/1/2 kept) plus
three-joint limbs — arm = shoulder→elbow→wrist, leg = hip→knee→ankle — with the
hand off the wrist and the foot off the ankle. Each bone is its own tube segment
bound to its joint, with a small ball at every elbow/wrist/knee/ankle to mask the
seam when it bends (skeleton.gltf/mummy.gltf: 523→1239 verts, 7→15 joints). Clips
now use the joints: walk flexes the knees + bends the elbows on the arm swing;
attack cocks the right elbow on the wind-up and snaps it through the chop; die
buckles the knees and lets the arms go limp at shoulder + elbow as it topples.
Verified in-game (docs/anim_v_rig2_*.png): bent-elbow attack swing and a
knee-buckle collapse on death.

Verified by driving the game: scene renders correctly, a mummy chased the party in
(walk), swung (attack — arms raise/drop across frames), and on a kill toppled to
the floor then the corpse was removed (die). Screenshots in `docs/anim_v_*.png`.

NOTE (pre-existing, not this thread): the party's unarmed attack input is mid-
refactor — `onHandAttack`/`PartyAttack` exist but are not wired to any gesture
(the empty-hand left-click was deliberately emptied in `GameUI::OnHandLeftClick`),
so combat is currently undrivable without re-wiring. Verified death by temporarily
re-wiring it (reverted).

Next: goal 4 (authored rigged glTF via `AssetBaker import-model`) — deferred;
optional blob walk/die; tune the procedural clip timings/poses.
