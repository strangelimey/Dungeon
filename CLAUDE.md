# Dungeon — project context for Claude

Old-school grid dungeon crawler (Grimrock / Dungeon Master style), C++23 +
DirectX 12, owned by Michael (GitHub: strangelimey/Dungeon, private repo).
Built collaboratively with Claude across sessions; this file is the handoff.

## Build & run

- `build.cmd [debug|release]` — VS 2026 Community's bundled CMake + Ninja
  (plain `cmake` is NOT on PATH; the script sets up vcvars64). Both build.cmd
  and gen-vs.cmd prepend the VS Installer dir to PATH so vcvars' VsDevCmd.bat
  finds vswhere.exe by PATH (it otherwise runs a bare `vswhere.exe` that fails
  under NoDefaultCurrentDirectoryInExePath=1, printing a harmless but noisy
  "'vswhere.exe' is not recognized" warning).
- Output: `build\<config>\bin\Dungeon.exe`. There is NO asset copy: every
  config runs straight out of the repo's `assets\` (baked in as
  `DN_ASSETS_DIR`, resolved by `paths::AssetsDir`), so debug/release/vs/any
  future profile build share ONE tree and changing an asset needs only a
  relaunch — no rebuild, no robocopy. Two consequences to hold: an editor
  save or import lands in the GIT TREE immediately (it shows in `git status`
  instead of needing "To source"), and a rebuild can no longer clobber
  unsynced editor work the way the old post-build copy did. Only PACKAGING
  copies assets beside the exe — `paths::AssetsDir` falls back to that copy
  when the baked-in source path isn't present, which is what makes a shipped
  build work. Genuinely per-config files stay exe-side: `dungeon.log`,
  `settings.ini`, `shadercache\`.
- `gen-vs.cmd` → `build\vs\Dungeon.slnx` (VS 2026 emits .slnx, not .sln).
- Debug builds open a console for logs; DN_ASSERT failures abort() — in
  debug that means a CRT dialog and the process LOOKS alive but is stuck. The
  REPORT lands before the abort now (it is routed through crash::ReportFatal —
  see Diagnostics), so the evidence is on disk even when the dialog is the only
  thing you can see.
- The full log also writes to `dungeon.log` NEXT TO THE EXE (truncated per
  run, flushed per line so the tail survives a crash/abort) — read that
  instead of scraping the console window. The file is named after the RUNNING
  EXE (Core/Log.cpp via paths::ExecutableName), so the tools that also link
  Core get `assetbaker.log`, `bc7test.log`, `threadstress.log`. That matters
  because they all share `build\<cfg>\bin`: with the old hardcoded name an
  asset import silently truncated the GAME's log and wrote its own output
  over it, which destroyed the evidence mid-debug once.
- A CRASH also drops a MINIDUMP beside the exe — `dungeon-<fault|fatal|
  terminate>-<pid>-<n>.dmp`, up to 3 a run, ~34 MB each (ignored by the blanket
  `build/` rule AND by an explicit `*.dmp`, since one accidental commit of one
  costs a history rewrite). Open it in VS for the faulting register state and
  every thread's stack; the log already carries the symbolized faulting stack,
  so reach for the dump only when that is not enough.

## Architecture (docs/ARCHITECTURE.md has the full version)

Nine strictly layered static libs, one-way deps:
Core → Platform/Assets → Animation/Graphics → UI/Audio → Game → Main(exe).
Key conventions (memorize, they bite):
- SCALE: every model on disk is authored in UNITS, 1.0 = one dungeon SQUARE,
  and the square is a CUBE. `kUnit` (Game/DungeonMap.h, 2.5 m) is the single
  authority; kCellSize and kWallHeight both derive from it, and `UnitScale()`
  multiplies it in at the handful of mesh-to-world seams (DungeonMeshBuilder
  StampCell; decoration/stair/fixture transforms in DungeonWorld_Load +
  _Editing; doors/buttons/monsters/items in _Render). Change kUnit, rebuild,
  and the whole world rescales with NO rebake — that invariant is the point,
  so a NEW draw site must go through UnitScale() or its content comes out
  2.5x small. AssetBaker authors the block family directly in units
  (kCellHalf=0.5, kWallH=1.0, U(metres)/M(units) convert) and pushes its
  metre-proportioned props/creatures through ScaleMeshToUnits /
  ScaleModelToUnits at one boundary (FinishProp + the few self-assembling
  builders). import-model's --height/--lift and FetchModels' $modelSets
  Height/Fit/Lift are UNITS too. Per-kind `scale` (monsters: `modelscale`)
  trims a prop on top of its authored size. Full authoring guide (Blender
  setup, reference dimensions, export/import): docs/authoring-scale.md.
- DirectXMath ROW-vector convention: v' = v*M, translation in row 4
  (_41.._43); matrices uploaded raw; HLSL always uses mul(matrix, vector).
  glTF column-major memcpy is CORRECT under this pairing (same bytes).
- Left-handed, +Y up, camera forward = (sin yaw, 0, cos yaw). Facing index
  +1 is on-screen LEFT (see Party.cpp comment — controls were once reversed
  because of this).
- All indentation is TABS (see .editorconfig). Comments use file banners +
  section dividers; keep that style.
- Per-frame GPU transients come from UploadAllocator arenas (one per frame
  in flight, kFrameCount=3); steady-state frames allocate nothing on the
  heap (docs/ARCHITECTURE.md "Memory strategy"). A full allocation audit
  (2026-07-03) verified the rule and closed its last violations (formation
  scratch, flat AI-snapshot grids, shared icon light rig — see the AI
  section). The rule is now CHECKED, not just held (docs/ARCHITECTURE.md
  "Checking the rule"): Core/AllocTrack replaces the global ::operator new
  family and counts per THREAD (lock-free, constant-initialized slot; Debug,
  or -DDN_TRACK_ALLOCS=ON in Release). Main brackets each frame,
  Game::SteadyStateFrame arms it (Playing, no console/overlay/load/deferred
  rebuild, 120-frame warm-up), and a violating frame's call stacks are
  symbolized into dungeon.log once per unique site. Dev: `alloctest [secs]`
  (one machine-readable verdict line), `allocguard [status|strict on|off|
  reset]`, `allocpoke` (violate on purpose); `tools\AllocTest.ps1` is the
  re-runnable regression run (`-SelfTest` inverts the verdict, so the harness
  must catch a real violation to pass). TWO POLICIES to know before "fixing" a
  report: EVENT frames (a bump message → loc::Tr + MessageLog::AddLine) DO
  allocate and are reported but not asserted on — allocation proportional to
  events isn't what the rule forbids, and they are deliberately NOT wrapped in
  alloc::Excused because that would also hide something logging every frame;
  and anything reporting from inside a guarded frame must excuse ITSELF
  (log::Write formats a string). Staged loading is measured too — LoadQueue
  times/counts every task and dumps a table when the last lands (`loadstats`
  reprints; each Add takes an English dev name beside its localized label),
  and LoadGltf reports allocs/MB per model. TRAP when reading those numbers:
  DEBUG allocation counts are NOT release ones — MSVC iterator debugging makes
  vector's move ctor allocate a proxy (so it isn't noexcept, so push_back
  growth COPIES elements; each copied anim channel re-allocates both its
  buffers). Same load: 223k allocs debug vs 43k release. Reserving the clip
  vectors fixed the copies (debug → 129k); the residue is the per-move proxy
  and is intrinsic to the debug CRT. 80% of the load is four rigged skeletons
  (40 clips × 99 channels × 2 buffers = the floor for that layout).
  One convention carries an invariant no compiler checks: cached
  UIContext widget pointers die on Clear(), so any callback that triggers
  a page rebuild must DEFER it a frame (the m_pendingLanguage /
  m_videoRebuildPending pattern). The party roster is resize-safe: the
  HUD/sheet widgets address Game::m_characters by (vector, index) and
  RE-RESOLVE the member every Update/Draw (PartyHud.h RosterMember), so a
  party of 1..4 members — or a future resize — can't dangle them; a roster
  SIZE change still needs GameUI::RebuildForRoster (deferred, never from a
  widget callback) to re-lay-out the per-member widgets.
- Shader-visible SRV heap slots (kSrvHeapCapacity=1024) RECYCLE through a
  free list: gfx::Texture returns its slot on destruction
  (GraphicsDevice::FreeSrv), so the texture-churn paths (font atlas
  rebakes, level transitions, quality swaps, turbidity rebuilds) reuse
  slots instead of leaking the heap. RULE for any new AllocateSrv caller:
  a recycled slot's old descriptor can still be referenced by in-flight
  frames — drain the GPU before overwriting it (Texture::Upload drains via
  ExecuteImmediate; Texture::RenderTarget calls WaitIdle first). It is still
  a hard CEILING whose arrival is an abort, so occupancy is VISIBLE now:
  SrvLive()/SrvHighWater() draw an `SRV 275 / 1024 (peak 275)` gauge in the
  console perf panel, 75%/90% crossings log a warning, and the exhaustion
  assert quotes the peak (reads as "something leaks", not "the limit is
  1024"). Measured: showcase = 275 live, and two quality swaps (every
  texture reloaded twice) leave live AND peak at 275. GROWING the heap is
  deliberately NOT built — it needs index-only SrvHandles first, since the
  absolute CPU/GPU pointers handed out today would dangle across a
  reallocation, and 27% occupancy says that work hasn't earned itself.
- Lifetime conventions: ~Game calls AudioEngine::StopAll() because sound
  playback is ZERO-COPY from SoundBank memory and the engine outlives
  Game; preview-mesh resets (dev console `preview`, AssetDialog) WaitIdle
  first since up to kFrameCount-1 in-flight frames still reference the
  buffers; C-API boundaries (cgltf, FILE*, shell COM) are RAII-wrapped —
  keep new ones that way. The MAIN THREAD MUST STAY STA-CAPABLE: never
  CoInitializeEx it into the MTA (AudioEngine's ctor used to, for XAudio2,
  which needs no COM since 2.8). A thread's apartment is fixed once joined,
  so an MTA main thread makes every shell dialog return RPC_E_CHANGED_MODE
  and DEADLOCK — the editor's "Browse Folder..." wedged the process with no
  window ever shown. Platform/FileDialog now refuses (logged) rather than
  hanging if it ever happens again; running the picker on a private STA
  thread does NOT help, since Show() messages the owner window.
- Constants that must match HLSL: kMaxPointLights=64 (the point-light array
  CEILING = the Ultra tier; the per-frame count is a runtime budget,
  GameSettings::maxPointLights, Low=16..Ultra=64 — see the quality system),
  kMaxSkinJoints=128, root signature layout in Renderer.h header comment.
- The Game lib is split by category: Game.cpp is just the app state machine
  + wiring; GameSettings (ini round-trip, quality tier, the kThemeFields/
  kBarFields/kKeyFields tables), SoundBank, LoadQueue (staged loading),
  DungeonWorld (world state, simulation, both render passes), GameUI (all
  five UIContexts: menus, settings page, HUD, sheet, overlays), AssetUtil
  (load-or-die helpers). World→log feedback flows through
  DungeonWorld::onMessage; UI→state-machine actions through GameUI's on*
  callbacks, both wired in the Game constructor.
- MAGIC (full model: docs/magic system.md + spells.md + skills.md): every
  spell is a CLASS in src/Game/Spell/ (one file pair per spell; Spell base →
  BoltSpell/WardSpell/SightSpell forms — the shared tier-2 form runes Project/
  Protect/Sight; behaviour = the Cast() override, reaching the
  world only through host-wired CastServices) — spells.cat is NUMERIC
  OVERRIDES only, the class recipe is identity. MagicSystem runs the common
  gates (vocab, mana, skill/fumble roll, power ×(1+0.10×school level) ×
  (1 + spell_stat × the school's stat)); skills train BY USE (per-school +
  per-weapon-class, level = sqrt(xp); the CREEP TARGET is the source's
  associated stats now — docs/combat.md part 2 superseded docs/skills.md's
  creep table). Status effects live in ONE Character::effects list (wards
  stack across schools, same-school recast replaces; poison/bleed are the
  first non-ward kinds — see COMBAT); the party bar draws them in the name
  band, the sheet's Effects tab (hourglass) is the long form. Defaults +
  spell MRU are per member AND per hand; the SPELLBOOK is the Magic area's
  member-colored selector row (button disabled = absent/down/no symbols; no
  menu entry — book casts pass kBookHands and credit BOTH hands' MRU).
  Save v14/15/16 lines cover effects/skills/per-hand. Adding a spell: file
  pair + AllSpells.cpp + CMakeLists (hand-listed) + spell.<id> lang keys ×5
  (+ .desc for ward-like effects).
- COMBAT (full model: docs/combat.md — "The attack formula"; built by the
  combat-depth thread): every constant is a KNOB in the project's
  balance.cat ([formula] block → the Balance struct in Game/Balance.h;
  kBalanceFields drives load/save AND the dialog rows) and attacks.cat
  (per-attack numbers; IDENTITY — attack id + damage type — is the typed
  C++ table in Balance's ctor, the spells.cat pattern). Both edit LIVE in
  the editor map's Balance header-button dialog (Formula/Attacks tabs, "?"
  explains the columns; Save writes the catalogs). Seven damage types
  (slash/pierce/bash + the four elements): spells type by school
  (BoltSpell::MakeBolt), monster melee by `dmgtype`. Attack side: damage =
  (weapon damage, or the unarmed knobs, + stat_damage × avg of items.cat
  `stats`) × attack numbers × (1 + skill_damage × level); ACCURACY IS
  ALWAYS DEX. Defender side: evasion, then (rolled − soak) × (1 −
  resist[type]) floored — resists SUM nature (monsters.cat `resists`;
  Character::natureResists is the future race layer) + worn equipment
  (`resists`/`armor`) + Stone Skin as physical, clamped ±resist_clamp
  (a nature cell of 1.0 = immunity). Resources DERIVE: max = base + k ×
  statAvg (Character::RecomputeMaxima; authored bases ride save v17,
  pre-v17 back-solves). STAMINA is the exertion meter (swings spend
  (stamina_swing + stamina_weight×kg) × the attack's stamina column, steps
  spend stamina_step, every spend feeds VIT's creep — part 3's conditioning
  loop; an empty bar latches EXHAUSTED penalties with hysteresis). 0 HP =
  UNCONSCIOUS (self-stabilizes at stabilize_health after stabilize_time
  safe seconds — any monster in aggro resets the clock); DEAD only by
  OVERKILL (a blow on a downed member, or ≥ overkill×max; v18 "dead" line;
  poison/bleed DoTs tick the downed — that kills). REACH: party rear rank
  (roster slots 2-3; quadrants read Brand front-L, Sera front-R, Maren
  rear-L, Tilo rear-R) attacks only with polearms (items.cat `reach =
  polearm`) / ranged / spells; a monster with `reach = 2` melees from its
  queue post down a shared row/column. Projectiles fly QUADRANT LANES both
  ways: casts spawn a quarter-cell down the caster's lane and hits test
  lateral distance vs sub-cell position (kLaneHalfWidth = 0.35 cell) — an
  opposite-quadrant body is flown past. Adding a weapon: weapons.cat
  damage/speed/skill/stats/reach + `command` (its attack list) + item.<id> lang
  keys ×5 (armor -> armor.cat with armor/resists; runes/keys/food/etc ->
  items.cat — see the editor palette section for the three-catalog item split);
  a new attack VERB is a Balance-ctor row + attacks.cat entry +
  GameUI kMeleeUses + use.<verb> keys ×5. ENCHANTED weapons: weapons.cat
  `element = fire` + `element_bonus` — a landed blow adds elemental damage
  through the target's resist for that element (no soak, no separate to-hit
  roll), and the element becomes the FLAVOUR its on-hit effects arrive with.
  Dev: `equip <item> [member] [hand]`.
- EFFECTS (full model: docs/effects.md — the system every source of damage
  goes through; built in six phases 2026-07-24): ONE pipeline for everything
  that happens to a combatant. A source builds an `fx::DamageEvent` and calls
  `fx::Deal`, which walks DEFLECT → strike → mitigate → ABSORB → apply, then
  the caller narrates and calls `fx::React` (stage 6 is split out so a
  reaction's line reads AFTER the blow it answers — the same reason
  `WoundMember` returns a `Fall` the caller says, and a monster's slain LINE
  stays at its call site while the death PATH lives in the adapter). A
  REPRISAL is itself a `Deal`, so the react hook takes an `fx::ReactCtx`
  (strike knobs + RNG, built by DungeonWorld::Reaction) and a fire shield's
  burn is deflected/absorbed/DRUNK like any other damage; cascade is
  prevented by `Deal` never calling `React`, not by skipping the pipeline.
  `fx::ITarget` is all the module knows of a combatant; DungeonWorld
  implements it twice (`PartyTarget`/`MonsterTarget`) and those adapters are
  the ONLY place the two sides differ. An effect is a CLASS in src/Game/
  Effect/ (one file pair, hand-listed in AllEffects.cpp — the spells pattern);
  effects.cat holds numbers/look only (name, icon, school, plume, damage_type,
  stacking, apply_party/apply_monster) and an entry naming no class is a
  warning. A combatant carries `std::vector<fx::Inst>` — Character and Monster
  ALIKE, so a monster can be poisoned or warded. Each ward is its own kind
  overriding the stage it acts at (windward=deflect, stoneskin=mitigate,
  waterveil=absorb, fireshield=react); wards stacking across schools falls out
  of that. `fx::Apply` owns the stacking rule. EVERYTHING IS RESISTED — the
  event PRESETS name a kind of damage and set the maths, so no caller sets
  flags by hand: Blow/Bolt (rolled+soaked+resisted), Impact (a COLLISION — a
  wall, a door, a PIT LANDING: bash damage armour blunts and Stone Skin
  turns; unrolled but soaked+resisted — the world's two blows, OnBumpImpact
  and OnFallImpact, share DungeonWorld::CollideParty and the balance.cat
  `bump_damage`/`fall_damage` knobs), Burst (magic riding something else —
  an enchanted blade's element, a ward's reprisal: resisted, NOT soaked,
  "plate turns a blade not a flame"), Tick (a DoT's bite: resisted, not
  soaked). RESIST PAST 1 IS ABSORPTION: an authored NATURE cell (monsters.cat
  `resists`) of 1.0 = true immunity (zero, not the wound_floor — ResolveAttack
  only floors a blow that got through) and past 1.0 the target DRINKS that
  element and is healed by it (`fire 1.5` = half again as healing). Both escape
  the ±resist_clamp, which only caps STACKED mitigation. ITarget::Absorb is the
  mirror of Wound: capped at max, provokes a monster but earns no threat, can
  wake the unconscious but never the dead; a blow that does nothing says
  "unharmed" rather than "for 0 damage", and a feeding TICK says nothing.
  DoTs store RAW magnitude and
  are resisted AS THEY BITE (a ward raised mid-burn helps at once), each as
  its own authored damage type — bleeding tints fire red but wounds as pierce,
  and a burn takes the element that lit it. Content names effects BY ID:
  `on_hit = burn 3 6 0.5, bleed 2 10` on a weapon or monster (`poison`/
  `bleed`/`element_dot` still load as aliases). Presentation is DERIVED — a
  burning body's plume + light come from "any effect with `plume = 1`", so
  they restore for free. Save v22 round-trips both sides. Adding an effect:
  a class + AllEffects.cpp + CMakeLists + an effects.cat block + effect.<id>
  lang keys ×5. Dev: `effect <id> [member|ahead] [magnitude] [seconds]`.
- ALL user-facing text goes through Core/Loc (loc::Tr(key) /
  loc::Format(key, args...) for {} placeholders), loaded from
  assets/lang/<code>.lang (UTF-8 key=value, ';' comments; en.lang is the
  reference — add new strings there). Missing keys render as the key
  itself (visible, never fatal); a missing language file falls back to
  en.lang. Dev-facing text (log::, DN_ASSERT, asset names, ini keys) stays
  English. Dynamic ids map to keys by convention: monster.<ent type>,
  class.* (Character.classKey), facing.* (Party::FacingName returns the
  key). Settings → Game has a Language dropdown (loc::ScanLanguages; each
  file self-names via lang.name); switching saves language=<code>, reloads
  strings, and rebuilds every page next frame (GameUI::RebuildForLanguage —
  deferred via Game::m_pendingLanguage because the rebuild destroys the
  dropdown; an in-game switch clears the HUD message log). ui::Font bakes
  Latin-1 (32..255) and Draw/MeasureWidth decode UTF-8, so Western European
  scripts work out of the box; other scripts need a wider bake range.

## Renderer features (assets/shaders/scene.hlsl)

Forward pass: metallic-roughness PBR (Cook-Torrance GGX in scene.hlsl's
BRDF()), driven per-draw by MaterialParams (metallic/roughness factors +
optional ORM map at t11: R=occlusion, G=roughness, B=metallic, glTF order;
factors scale the map). Albedo textures are sampled sRGB (Texture's srgb
flag → *_SRGB DXGI format); normal/height/ORM stay linear. Two scene PSOs:
m_pso (CULL_NONE, default for hand-built procedural geometry) and m_psoCull
(CULL_BACK, for authored/imported meshes — MaterialParams::doubleSided=false;
DrawMesh swaps PSO per draw, never during the shadow pass). Plus
normal + steep-parallax mapping (derivative cotangent frame, height in
normal-map alpha), per-cell volumetric dust (turbidity grid texture t2,
raymarched extinction + in-scattering), point-light cube shadows with
distance-graded slots (8 slots, 512/256×3/128×4, cubes at t3..t10; slot 0 =
nearest light, PCF; carried torch always wins slot 0; dust march samples the
same cubes → god rays; kShadowSlots sizes the C++ side, scene.hlsl mirrors
the registers BY HAND), fire light positions wander so shadows flicker.
KNOW THIS about "missing" shadows: in a fire-dense room the dust in-scatter
(every fire feeds a turbidity ring) washes surface shadows to ~invisible —
that's a turbidity/ambient tuning matter, not a shadow bug (A/B: console
`shadows off` barely changes such a room; `dust off` transforms it). Shaders compile
at launch with an on-disk cache (shadercache/, hash-invalidated) — edit
.hlsl and relaunch, no rebuild.

Per-frame efficiency (DungeonWorld + Renderer): surface geometry is split
into spatial chunks (DungeonMeshBuilder GeometryChunk, kChunkCells=4, each
with an AABB + texture variant), so the main pass frustum-culls off-screen
chunks (DungeonWorld::ViewCull, Gribb-Hartmann from Camera::ViewProj) and
each shadow cube sphere-culls out-of-range chunks; discrete meshes (props/
monsters/fires) cull by bounding sphere too. Shadow cubes are CACHED
per slot (ShadowSlotCache): a cube re-renders only when its light changed/
moved (>2cm), a flicker tick is due (fire cubes throttle to half rate via
PointLight::flickerShadow), geometry changed (map Revision), or an animating
caster (a monster) is in range — otherwise the cube stays in its SRV
state and is reused (the per-slot RT/SRV barrier guard makes the skip safe).
DrawMesh skips redundant PSO swaps and, in the shadow pass, the texture-table
binds; skinning palettes upload once per frame (cached by the animator's
buffer, reused across all ~25 submissions).

## Asset pipeline (everything loads from assets/, nothing generated at runtime)

- `AssetBaker <assets>` — regenerates all procedural assets (block models
  incl. worn tiers, monsters, sconce/brazier, sounds, title art,
  party portraits) and ends with a mip bake.
- `AssetBaker import <folder> <assets> <name> [--flip-green]` — packs a
  downloaded PBR set into three files: <name>.png (albedo), <name>_n.png
  (normal, height in alpha), <name>_mr.png (ORM: R=occlusion, G=roughness,
  B=metallic). Auto-detects maps by filename; flips GL normals; bakes all
  three to BC7 DDS. (AO is no longer multiplied into albedo — it rides the
  ORM map.)
- `AssetBaker import-model <model-file|folder> <assets> <name> [--height M]
  [--yaw deg] [--up y|z]` — imports an authored/bought model (.gltf/.glb/.obj):
  merges all meshes into one (WriteGltf is single-mesh), normalizes scale
  (--height, or auto-fit largest extent to ~2 m), orientation (--up z does
  Z-up→Y-up; --yaw), grounds (min y=0) and centers XZ, then writes
  assets/models/<name>.gltf and imports the folder's PBR maps as the texture
  set <name>. The game binds prop textures by name, so the decoration loader
  (DungeonWorld::LoadDecorations) auto-uses the <name> set for an imported
  type and renders it back-face culled (authored=true). `--texture-set <name>`
  skips the per-call PBR import and points the model at an already-imported set
  instead, so every item split out of one multi-mesh pack shares a single set.
- `tools\Build*.py` — SCRIPT-AUTHORED props, the default way to make new
  architecture (docs/authoring-scale.md; Michael does not hand-model). Each is
  run headless — `blender --background --factory-startup --python
  tools\BuildX.py -- <out.glb>` — then `import-model --raw`, a catalog entry,
  and place. The asset is DEFINED BY THE SCRIPT, so it is diffable and a
  revision is a constant change plus a re-run; the .glb is a build artifact,
  not a source. Two patterns, both emitting UNIT space:
  * CONSTRUCTED — BuildWallArch.py assembles a slab (opening built from panels
    + a fan, NOT booleaned, so the topology stays predictable) plus individually
    placed stones. Stones carry real MORTAR GAPS and are inset inside the
    opening, so islands never fuse, a whole-mesh bevel is safe, and the slab's
    cut edge is hidden — all by construction rather than later correction.
    `--rough` weathers each stone (tilt/scale jitter + two noise octaves).
  * SHAPE-PER-STATION — BuildPillar.py extends the loft: each station names a
    SHAPE (octagon or circle) as well as a radius, every ring is built at the
    SAME segment count, and only the RADIUS varies with angle
    (`radius_at`: a polygon peaks at its corners and falls to the apothem at
    each edge midpoint). Same topology, different silhouette — so an octagonal
    plinth lofts straight into a round shaft with no stitching. SEGMENTS must
    be a MULTIPLE of the polygon's side count or its corners land between
    samples and it reads as a lumpy circle (the script refuses otherwise).
    Shading is the other half of "round": 24 facets FLAT-shaded still read as a
    polygon, so the shaft band is marked `face.smooth` and everything else left
    flat — AFTER the bevel, so the bevel's own faces get classified, and side
    faces only, since catching a cap or a moulding step smears the arris it is
    meant to define. Cost to know: 24 segments = 2064 verts in Blender but
    6268 after the glTF per-corner split.
  * PROFILE-LOFTED — BuildPlinth.py and BuildFountain.py walk a table of
    (radius, height) stations. The lofter takes any segment count and any
    angular sweep, so 4 = a square plinth, 40 = a basin, a 180-degree sweep =
    a wall fountain. A profile is a CLOSED section (up the outside, over the
    rim, back down the inside) so a revolve is watertight; radius 0 fans.
  UV RULE THAT BITES: dominant-axis projection (TileUvs, Cube Projection) is
  only valid on BOX-ISH geometry. On a swept or revolved surface the normal
  rotates 90 degrees and the dominant axis FLIPS mid-surface, which seams —
  arch reveals and basins are UNROLLED instead (u = arc length, v = depth or
  height). tools\FixArchSoffitUv.py retrofits that onto the hand-built arch.
  Three Blender traps, each of which cost a run: glTF stores attributes PER
  CORNER so an imported mesh has NO shared verts (weld before anything
  connectivity-based); subdividing invalidates held BMVert refs (re-derive,
  don't carry); and — the same principle, the expensive way —
  `recalc_face_normals` NEEDS CONNECTIVITY. A generator that builds each face
  from fresh `bm.verts.new()` calls produces a soup of disconnected quads with
  no shared edges, and recalc then orients them ARBITRARILY: BuildStairsSpiral
  came out with 24 of 96 newel faces inverted. `remove_doubles` is not the fix
  either — welding the treads' abutting edges fuses them into 4-face
  NON-manifold junctions, which recalc also cannot orient. So for a
  script-authored solid, WINDING IS THE CONTRACT: emit every quad
  counter-clockwise seen from outside (a `flip` flag for the far side of each
  solid) and call neither op. THE SYMPTOM IS WHY THIS IS WORTH KNOWING: an
  inward normal lights as though the face were turned away, so it reads as dark
  slots cut in the stone and is INVISIBLE to `shadows off`, to a parallax
  change, and to any texture swap. Three A/Bs came back negative before the
  normals were measured directly. When a visual defect survives an A/B, stop
  hypothesising and measure the mesh attribute — for each face at a known
  radius, `dot(normal, radial)` should be positive on an outward surface.
- `tools\blender-bridge.cmd` (→ `.ps1`) + `tools\blender_bridge.py` +
  `tools\bsend.py` — the INTERACTIVE counterpart to the headless Build*.py flow:
  launch Blender with the bridge and Claude executes Python inside the LIVE
  session (`python tools\bsend.py -c "..."`) while Michael watches the viewport
  and says "wider" / "more weathered". He is the judge, Claude is the
  translator; he does not learn the menus. THE ONE RULE THE DESIGN TURNS ON:
  `bpy` is NOT thread-safe, so the socket thread only ENQUEUES and a
  `bpy.app.timers` callback (main thread) is the sole executor — a handler
  touching the scene directly crashes Blender far from the cause. A shared
  namespace persists across calls, so a model is built up over many small
  steps like a REPL. Every executed snippet is appended to `tools\.bridge-log.py`
  (gitignored), which is what keeps the SCRIPT-IS-THE-ASSET rule intact: the
  transcript distils into a committed `tools\Build*.py`. Use `--no-log` for
  INSPECTION (measuring, listing) so the log stays a buildable recipe. Errors
  come back as a traceback with exit 1. Binds 127.0.0.1 only and executes what
  it is sent — local dev tool, never exposed. Blender is DISCOVERED (newest
  install, sorted by [version]), never pinned.
- `tools\FetchModels.ps1` — the mesh analog of FetchTextures.ps1 for fab.com
  (or any authored-model) sources. SELECTION RULE: a fab listing's "Included
  formats" must include glb/obj/fbx; Unreal-Engine-ONLY listings are .uasset
  packs the engine can't read (don't buy them). Raw downloads live OUTSIDE the
  res tree in OneDrive\DungeonAssets\fab\<category>\<pack>\ (source mesh + PBR
  maps). An editable `$modelSets` table (like FetchTextures' $propSets) drives
  the import; each entry -> one model. The script chains: source mesh --(Blender
  `tools\ConvertMesh.py`, only for fbx/usd or a multi-mesh pack)--> .glb -->
  `AssetBaker import` (the shared PBR set, base name + _2k) --> `import-model
  --texture-set` (the normalized model). glb/obj sources skip Blender. `Split`
  packs split per top-level object (Object= picks the piece); `Rig` monsters
  convert with --keep-rig + --height straight into assets/models (bypassing
  import-model's joint-strip). Then wire a catalog [id] (decorations/monsters/
  items.cat) and place it in a level. ConvertMesh.py needs Blender (auto-found
  the NEWEST %ProgramFiles%\Blender Foundation\Blender <ver>, or -Blender
  <path>; the version is discovered, never hardcoded — a pinned list silently
  skips every import the day Blender self-updates).
- `tools\ImportAnimLibrary.py` + `tools\FetchAnimLibrary.ps1` — the ANIMATION
  side of the monster pipeline: bake a creature's STATE-ORGANIZED clip library
  onto its mesh. The library is one folder PER CreatureState (Idle/ InCombat/
  Attack/ Walk/ Run/ Flee/ Defend/ Hit/ Die/ Spawn/, any may be empty), each
  holding one or more Mixamo .fbx; the FOLDER names the state (matched to the
  src/Animation/CreatureState.h token, case-insensitive), the FILE is one
  animation. ImportAnimLibrary.py (a generalised rig_and_export.py) walks the
  folders, names each clip `<state>__<sanitised filename>` (the state is encoded
  in the clip name, so the model self-describes its grouping — the editor's
  monster-config dialog shows a state's animations by filtering this prefix;
  globally de-duped), rigid-binds the mesh to the shared Mixamo armature (every
  Mixamo clip uses one
  skeleton, so any number bind once — add .fbx and re-run, no re-bind), exports
  one assets/models/<name>.gltf, and EMITS the matching monsters.cat rows
  (`states = ...` + `anim_<state> = <clips>`) to <name>.anim.cat. `--plan`
  (plain python, no Blender) prints the clip plan + rows for a dry run. The
  FetchAnimLibrary.ps1 `$animSets` table drives it (Name/Mesh/Library/Height/
  MeshYaw, archive-relative); raw clips live in
  OneDrive\DungeonAssets\anim\<library>\. `Height` (→ `--height`) is the finished
  creature's height in UNITS like every other import knob, and the bake FITS AND
  GROUNDS to it: the fit rides the ARMATURE, not the mesh bounds, so a raised
  spear tops the skull without shrinking the skeleton under it, and the size +
  feet are re-checked AFTER the reshaping passes (mirror / rest repair /
  re-rest), which refuses to write rather than emit a model needing a
  `modelscale` to correct it. Height used to be "match this reference model",
  measured over every mesh in the scene — and Blender's glTF importer leaves a
  stray 2-unit Icosphere beside each rig it reads, so the reference was a
  constant 2.000 and five bought skeletons baked in metre space, 4.8 m tall in a
  2.5 m room. Any whole-scene measurement in a Blender tool needs the scene
  purged BY HAND first (read_factory_settings clears the startup file, not an
  importer's leftovers).
  Paste the emitted rows into the creature's monsters.cat [id] — or just check
  the boxes in the editor's monster config dialog (it auto-discovers the model's
  clips). Humanoid Mixamo defaults (mesh +90 yaw to co-face the armature, finger
  bones excluded); non-humanoid rigs may need --mesh-yaw/--keep-fingers tuning.
- `AssetBaker mips <assets>` — rebakes derived .dds (BC7 encoder in
  tools/AssetBaker/Bc7Encoder.cpp; use the RELEASE baker). The encoder trials
  FOUR modes per 4x4 block and keeps the lowest error: mode 6 (one RGBA line, 16
  index steps — photographic albedo), modes 1 and 3 (two subsets with a colour
  line EACH, so a block straddling brick and mortar stops smearing one line
  through the middle — mode 1 spends its bits on index steps, mode 3 on endpoint
  precision, so neither dominates; RGB-only, opaque blocks only), and mode 5 (one
  channel gets its OWN endpoints and index set, plus a ROTATION naming which
  channel that is). Every mode's error is the same quantity — squared difference
  over 16 px x 4 channels — which is what makes "keep the lowest" meaningful
  across them. Mode 5's rotation is the one to understand: it was nearly left out
  on the argument that this project's odd channel out is the height in alpha,
  which mode 5 already decouples. Wrong — in a normal map the awkward channel is
  usually BLUE (z is derived from x and y and behaves nothing like them), and the
  rotations took one scanned normal map from 35.8 to 39.9 dB, its mode-5 share
  going from 2% of blocks to 93%.
  The knobs live in Bc7Options (Bc7Encoder.h), each with its measured
  justification in the comment; every non-obvious default was SET by
  `Bc7Test --audit`, not guessed. TWO of those measurements are worth carrying
  forward: the partition shortlist is ranked by within-subset SCATTER, not
  bounding-box extent (the old score was blind to subset population, and fixing
  it was worth more than doubling the shortlist); and shapeTrials went 8 -> 16 ->
  8 as modes were added, because a block the shortlist mis-partitions usually has
  another MODE that suits it. Search breadth and mode coverage buy overlapping
  things — re-measure both whenever a mode lands.
  CHECKED, not assumed — `tools\Bc7Test.ps1` (docs/bc7.md): the encoder records
  the error it believes each block carries, and the harness decodes the packed
  bytes with an INDEPENDENT decoder and demands exact agreement. That estimate
  is what picks the mode, so if it lies, mode selection is a coin toss and every
  quality claim is void. `-SelfTest` corrupts the bytes and requires a FAIL.
  TRAP when reading its numbers: aggregate PSNR by the MEAN of per-image PSNR,
  never by pooling squared error — pooling is dominated by whichever tile
  compresses worst (the noise tile sits ~1000x higher in MSE than a smooth one),
  and it hid a knob worth 1.35 dB on brick behind an average of +0.01 dB.
- `AssetBaker models <assets>` — rebakes only the .gltf models (fast). Worn
  blocks sample the installed texture height maps, so rerun after
  FetchTextures.ps1 or a texture import.
- `AssetBaker portraits <assets>` — rebakes only the party portraits
  (portrait_<name>.png, 256², PortraitBaker.cpp: SDF-mask busts, one
  headpiece per class) and their mip chains. Names must match the roster
  in src/Game/Character.cpp.
- Textures: PNG = source, .dds = derived BC7 mip chains (gitignored).
  Scanned sets are NOT in git: raw downloads live in
  OneDrive\DungeonAssets\<1k|2k|4k>\<category>\<material>\ — the res folder
  is the material's NATIVE resolution, categories mirror the FreePBR pack
  (walls, floors, rocks, metals, ...) plus ceilings. Contents: 7 Poly Haven
  CC0 sets (all three res) + the FreePBR Premium pack (~620 sets, almost
  all 2k native; the models/ and bonus/ categories carry .obj prop meshes
  with their textures). `tools\FetchTextures.ps1` imports the materials the
  maps' `textures` records reference PLUS a fixed `$propSets` table — the
  code-bound prop/creature sets (sconce/brazier/skeleton/mummy/blob,
  renamed from their archive folders, 2k-native) — since those load by code
  convention, not a map record (override: -Materials list skips props, -All
  for everything — slow, hundreds of BC7 bakes; -Resolutions 1k,2k,4k). A
  full pre-history-rewrite git bundle also lives there.
  - Mixed source formats: Poly Haven / FreePBR ship loose PNG/JPG maps the
    importer reads directly. textures.com PBR sets instead ship TIFF (8/16-bit),
    which stb_image (the C++ importer) can't read. FetchTextures handles this
    transparently: a material folder containing any .tif/.tiff is staged to PNG
    first (Convert-TiffMaps, WIC/PresentationCore, bit depth preserved so a
    16-bit height map round-trips as a 16-bit PNG for stbi_load_16) into %TEMP%\
    DungeonTexImport\, leaving the OneDrive archive pristine, and imported with
    --flip-green (textures.com normals are OpenGL but their filenames lack the
    'gl' token the importer auto-detects; Poly Haven '_nor_gl' still auto-flips).
    textures.com download notes: pick the flat PBR maps (Albedo/Normal/Height/
    Roughness/AO), NOT the .sbsar Substance file (procedural, unreadable) and
    NOT the "Regular Photos" (diffuse-only JPG, no relief). Map filenames match
    DiscoverMaps substrings as-is; stone has no metallic map (importer defaults
    metal=0). SKIP the Mask maps a scan sometimes offers: DiscoverMaps reads
    "mask" as an OPACITY map and would pack it into the albedo's alpha.
  - `tools\SortTextureDownloads.ps1` files a textures.com batch from Downloads
    into the archive, renaming TCom_<theirName>_<res>_<map>.tif to the game's
    <res>\<category>\<name>\<name>_<map>.tif. Its `$sets` table IS the decision
    record — one row per bought set, category + the name the catalog will use —
    because the name is permanent: the worn mesh bakes as worn_<name>_<tier>
    (so a set is one surface kind for life) and LoadPbrSet resolves a catalog's
    `texture` to <name>_<res>. A SET NAME MAY NOT CONTAIN A MAP-TYPE TOKEN
    (rough/albedo/normal/height/metal/_ao/occ/mask/...): name and kind meet in
    one filename, <name>_<map>.png, and DiscoverMaps tests those substrings over
    the whole stem IN ORDER — roughness before albedo — so `wall_brick_rough`
    had its _albedo.png claimed as the ROUGHNESS map and died with "No albedo
    map found". The script now refuses such names up front, because the failure
    reads like a bad download and only surfaces after the slow bake.
    Unknown sets are reported, never guessed;
    -WhatIf / -Copy / -Force, and it refuses to overwrite. Pass its printed
    FetchTextures line through `powershell -Command`, NOT -File: -File binds
    `a,b,c` to [string[]] as ONE element, so every name matches nothing and the
    import dies with "Nothing imported".
  - A SCAN NEED NOT BE SQUARE, and ten of the installed sets are 2:1 (a
    4096x2048 tile holds two squares of stone across and one down). The worn
    bake CORRECTS for that automatically — `TextureHeight::Aspect` reads the
    image and every U in ModelBaker is divided by it, so one repeat spans that
    many squares of world width instead of being squashed into one. Nothing is
    authored and nothing can drift from its own texture. It went unnoticed for a
    whole texture batch because the defect reads as "these stones are a bit
    narrow", not as an error: `kUvScale` is "one tile per cell" in BOTH axes.
    The correction MUST stay paired between the mesh UVs and the wear field —
    they share that mapping precisely so the displacement lands on the painted
    stones, and correcting one alone slides them apart. Re-bake with `AssetBaker
    models`; the square sets come out byte-identical, which is the check that a
    change here is a no-op at aspect 1. The shared WALL FEATURES (`wall_niche`,
    `wall_niche_arch`, `wall_window`, `wall_window_rect`) take the SAME
    correction by a different route, and the split is principled: one feature
    mesh serves all 54 surfaces, so it cannot carry an aspect in its baked UVs —
    but it is stamped into `wallB[wallVariant]`, the wall block's own variant
    bucket, so the builder knows its surface at that moment. `Surface::uAspect`
    (read off the loaded albedo) rides a span into BuildDungeonGeometry /
    BuildDungeonRegion and `AppendTransformed` scales `uv.x` for the FEATURE
    ONLY. A worn block's aspect must be fixed at BAKE because its wear field
    samples the height map through those UVs; a feature has no displacement, so
    its aspect is free to be applied at STAMP — and that is also the only place
    a shared mesh can learn which surface it landed on. Nothing to rebake, and
    a set imported later is right for free. NOTE when testing this: a feature
    replaces the WHOLE wall panel for its edge, so a mismatch squishes the
    entire face, not just the recess — and no level currently places one.
- Maps are two files per level, split static vs dynamic for the future
  save system (saves will only ever store the dynamic side):
  - assets/maps/level1.map — STATIC layer (DungeonMap): ASCII grid, ';'
    comments, glyphs '#' rock '.' floor 'D' dusty 'T' sconce 'F' brazier
    (blocks movement) 'P' start. Lines starting lowercase are records
    (grid glyphs are never lowercase): `textures <wall|floor|ceiling>
    <set> ...` declares the level's surface palette — MANDATORY, the game
    loads only those sets + their worn meshes, order = variant index —
    plus `decoration <type> <x> <z> [facing]` and `fixture <id> <x> <z>
    [facing]` records — the kind token is a fixtures.cat id, EVERY entry is
    placeable (per-record FIXTURE KINDS: DungeonWorld::FixtureKind caches
    id→mesh/tex/flame like DecorationKind; the parser routes wall-vs-floor
    via the FixtureTypes info DungeonWorld passes at every DungeonMap
    construction, since the map has no catalog access; fixtures.cat
    `flame = 0` = a flameless kind, placed lit=0 — brazier_empty). The
    'T'/'F' glyphs are one-per-cell shorthand for an auto-faced default
    sconce/brazier; the fixture record places kinds explicitly so several
    can share a cell (e.g. two sconces on different walls — sconce facing
    names the solid wall it mounts on). Sconces resolve their mount wall at
    load (DungeonMap::WallSconce). A decoration record can
    also take `wall=<dir>` to hang flat on that wall instead of standing at the
    cell centre, so a sconce + a banner + other wall props can share one square.
    The wall mount (offset to the wall face, +Z turned to face the room) is one
    helper, DungeonWorld::MountOnWall, shared by sconces and wall decorations;
    the map overlay edge-draws both. Wall-mounted decorations default non-solid
    (they're on the wall, floor stays clear). The `banner` model is authored
    wall-backed for this; other wall-mounted props should be too.
  - assets/maps/level1.ent — DYNAMIC layer (DungeonEntities): monsters,
    items, buttons; one record per line, `<kind> <type> <x> <z> [facing]
    [key=value ...]` (Entity.h). Monster type → model: <type>.gltf.
    Records validate against the map at load (bounds, walkability,
    buttons face a wall). Edit + relaunch, no rebuild.
- Worn block meshes are baked PER SURFACE TEXTURE at 3 tiers
  (worn_<texture>_<low|med|high>.gltf), displaced by that texture's scanned
  height map (normal-map alpha) so geometric relief matches the painted
  bricks/slabs; DungeonMeshBuilder stamps the mesh matching each cell's
  texture variant. wall_stone's Poly Haven displacement export is flat
  (detected at import), so it uses procedural wear — its 0.5x0.31 block
  grid happens to fit that texture's large blocks anyway. TWO KNOBS, not
  one: `relief` is the displacement AMPLITUDE in metres (how far the stones
  stand proud) and `wear` is a 0..1 SCALE over it, both catalog fields on
  the surface schema and both `rebakes` (`AssetBaker wornblock ... --relief
  --wear`; an absent relief keeps the baker's per-kind default — wall 0.055
  / floor 0.045 / ceiling 0.08 — so untouched types bake as before). Before
  relief existed the amplitude was a baker constant and `wear` could only
  take relief AWAY, so "wear = 1" looked like a no-op. Don't confuse either
  with `height_scale`, which is the SHADER's parallax depth: fake, per-draw,
  no rebake, and by construction invisible head-on (the offset scales with
  the view's tangential component) — real silhouette relief only comes from
  the mesh, and deep relief on the `med` tier can facet.
- WALLS ARE PLAIN. The old `columns` knob baked edge pillars / border strips
  into every wall block (default ON, so 26 of 28 wall types carried them); it
  was RETIRED 2026-08-05 in favour of COMPOSITION — a pillar is a decoration you
  place, so one model serves all 54 surface types instead of being baked into
  each and multiplied by 3 tiers. The general rule behind it: bake detail into
  the mesh only when it is structurally bound and must align exactly (a door
  frame, a vault that IS the ceiling); compose anything that varies
  independently of its host. Surface-level detail is the expensive kind — it
  multiplies by texture set and needs a rebake — while decorations are N+M.
  Removal was safe because a worn block's surface spans the FULL cell and its
  displacement is pinned to zero at every edge (PinRamp in TextureWallWear), so
  blocks already tile watertight on their own; the same held for both niches and
  both window bores, whose frames also reach the cell edges. `AddWallPillars`
  SURVIVES for the clean (baked-but-unused) block set ALONE, whose recessed
  panel stops at kPanelX so its backing strip is the only thing covering the
  wall plane out to the edge and its outer cap the only thing closing the
  convex-corner notch — don't delete it without widening that panel first.

## Quality system

Settings page (landing page) is tabbed Game/Controls/Video/Audio/UI via
ui::TabControl. The whole page is authored in design px against a 900px-tall
window and SCALED by the live window height (GameUI::BuildSettings uiScale =
h/kFontDesignWindowH) because the page fonts scale that same way (UpdateFonts);
a fixed-pixel page would let the font outgrow its row at taller resolutions and
collide — so any new settings geometry must scale by uiScale too (the `page`
rect passed to children stays unscaled design units; only the TabControl's pixel
size scales, carrying the children with it). The confirm modal scales likewise.
Each tab stacks its rows with a Flow helper (GameUI.cpp, anon namespace) — a
vertical layout with CSS-style COLLAPSING margins: the gap between two items is
max(upper.marginBottom, lower.marginTop), not the sum, so equal margins on
neighbours overlap into one (constants mTight label→control, mRow list rows,
mGroup between settings/sections). ui::Slider is self-contained (label on the
top line, track in the band beneath, all inside its bounds) so it lays out by
its box like every other control. Sections are divided by ui::Separator (a 1px
horizontal rule, like HTML <hr>, placed through the Flow with mGroup both sides).
(pages scroll: children authored past the page bottom — bounds fraction > 1 —
trigger a per-tab scrollbar, wheel or thumb drag, page-scissored; the strip
sizes each tab to its label and grows + recenters the control to fit
[TabControl::LayoutStrip], and the content area is inset from the frame
[TabControl::ContentRect]):
quality dropdown on Video (Low/Medium/High/Ultra: mesh tier low/med/high/high
+ textures 1k/1k/2k/4k + point-light budget 16/32/48/64) plus a Max Lights
dropdown on Video (GameSettings::kLightBudgets; picking a quality resets the
budget to its tier value via Game::SetQuality → GameUI::SyncMaxLights, then
the dropdown can override it; DungeonWorld::UpdateLights keeps the nearest-to-
eye lights up to the budget) plus a Frame Rate dropdown (GameSettings::
kPresentIntervals → GraphicsDevice::SetPresentInterval: present sync interval
1..4 = full-refresh VSync down to refresh/4, a tear-free divisor cap that cuts
GPU load; options labelled with the live rate from GraphicsDevice::RefreshHz;
ini presentinterval=). Above quality/lights the Video tab has the
DISPLAY block: adapter (GPU), monitor (DXGI output), resolution, and display
mode (Windowed/Borderless/Exclusive fullscreen). The list comes from
gfx::EnumerateAdapters (Graphics/DisplayEnum.*, a device-independent DXGI walk,
also read by Main at boot); adapter/monitor render as a plain Label when only
one exists, else a DropDown. The selection is STAGED (GameUI::m_selAdapter/
Output/Res/Mode, separate from GameSettings) and committed by an Apply button —
only Video uses Apply, every other control is live. A monitor/resolution/mode
change applies in place (Game::ApplyDisplaySettings → Window::SetWindowed /
SetBorderless or GraphicsDevice::SetFullscreen, all of which resize the
swapchain through the usual onResize path). An adapter change can't be done in
place (the device is bound to its GPU), so it pops a Yes/No confirm modal
(GameUI::m_confirmUi, drawn over the page; Esc = No) and on confirm persists +
relaunches the exe (Game::RestartApp via platform::Process); the new process
binds the chosen adapter by LUID (GraphicsDevice ctor's preferredAdapterLuid).
Changing the adapter/monitor dropdown also repopulates the dependent lists by
rebuilding the settings page next frame (GameUI::m_videoRebuildPending →
ApplyPendingVideoRebuild, deferred like the language switch since the rebuild
destroys the live dropdown; BuildSettings is split out of BuildMenu for this).
master-volume slider on Audio, party-bar sliders on
UI (scale 0.5–1.5 resizes the bar about its top center and shifts the panels
beneath it — GameUI::ApplyPartyBarScale; width is pinned at the window span,
so above 1 the bar only grows taller; background opacity 0–1 fades the slot
fills only) plus color-picker grids for Theme Colors (the 8 ui::Theme
colors — GameSettings owns the master theme, GameUI::ApplyTheme pushes it
into all five UIContexts live) and Resource Bars (health/stamina/mana fills,
ResourceBarColors in PartyHud.h — the HUD widgets point at
GameSettings::barColors). The ColorPicker control's swatch opens an R/G/B/A
slider popup; kThemeFields/kBarFields in GameSettings.h drive both grids and
the ini round-trip. Controls tab: movement key bindings via ui::KeyBind rows
(click the key box, press the new key; Esc/click cancels —
GameUI::KeyCaptureActive suppresses the page's own Esc while armed; binding a
key another action holds swaps the two). kKeyFields drives the rows and the
ini round-trip; MoveKeys (Party.h) is pushed into the Party via SetKeys, and
dungeon::KeyName (Platform/Input) renders vkey names. The Controls tab also has
a Mouse Look section: sliders for look sensitivity, the return Delay (hold) and
Time (duration) of the hands-off camera return, and the move-straighten
duration, plus dropdowns for the two return easing curves (kLookEaseOptions, a
curated Easing subset). LookSettings (Party.h) is the master copy — round-tripped
to settings.ini (look_* keys; the curves store the dropdown index) and pushed
into the Party via SetLook (GameUI::onLookChanged); sensitivity is read live by
the Game's drag handler. (See the free-look paragraph under Game state machine.)
Game tab hosts the Language dropdown (see the Core/Loc bullet above).
All persist to settings.ini next to exe (quality=0..3, maxlights=16/32/48/64,
presentinterval=1..4, language=<code>, volume=0..1, barscale, baropacity, theme_<name>= and
bar_<name>=r,g,b,a, key_<action>=vkey, look_sensitivity/look_hold/look_return/
look_move=<float> and look_curve/look_move_curve=<easing index>,
adapter=<packed LUID, 0=auto>,
output=<index>, reswidth=/resheight=<0=window default>, fullscreen=0/1/2;
sliders save on release, pickers when their popup closes, key binds and language
immediately, display fields on Apply). Main reads the display fields BEFORE the
window/device exist (its own GameSettings::Load, same file Game re-loads).
Quality hot-swaps in place (WaitIdle + rebuild); Ultra falls back per-material
to 2k with a warning if 4k not installed.

## Game state machine

Loading (staged tasks, one per frame, progress screen) → Menu (baked title
art title_bg, MenuList: Continue/Load/Start New Game/Settings/Exit — Continue/Load
appear only when a save exists; all entries work) → Playing ⇄ Paused (Esc in-game freezes
the world and shows Save/Load/Settings/Exit/Back over the scene; Esc backs
out / resumes). QUITTING IS ALWAYS DELIBERATE (Michael, 2026-08-11): an Exit
entry (landing or pause) or the console's `quit`/`exit`. **Esc does NOT quit** —
on the landing list it does nothing, and it only backs out of the settings page.
It used to quit there, which read as a CRASH: a party wipe drops you on the
title screen, and a reflexive Esc at a screen that appeared by itself killed the
process with no confirmation and no log line. Esc during the two LOADING states
still quits (an abort hatch for a long load, and no Exit button is up).
Everything routes through Game::QuitRequested, polled by the main loop. During the three
loading states the world is only PARTIALLY built (the HUD log, meshes,
monsters arrive task by task), so dev-console COMMANDS are gated off
(DevConsole::SetCommandsEnabled — Enter prints a notice; a `cast` mid-load
once crashed on the null HUD log) while the queue keeps pumping even with
the console open; GameUI::AddLogLine/ClearLog are null-safe pre-BuildHud for
the same reason. Monsters
chase + melee the party, driven OFF the main thread (see "Threading & async
monster AI" below); fires are sconces at 'T' (wall-mounted,
light at flame) and braziers at 'F', each with FireEffect particles
(flame/spark/smoke via gfx::ParticleBatch premultiplied billboards) and
fire-driven turbidity rings around them.

The HUD's top bar shows the party — 1..4 members; planned party creation
lets the player build fewer than 4, and the bar always reserves four slots
so a short roster keeps its slot size (Character.h roster, widgets in
PartyHud.h: portrait, name, health/stamina/mana bars); clicking a portrait
freezes the world (AppState::CharacterSheet, like Paused) and opens the
character details page (prev/next cycle members modulo the live roster
size, Esc/Back resumes). The per-member widgets (CharacterPanel, HandSlot,
CharacterSheet) hold NO Character* across frames: they address
Game::m_characters by (roster, index) and re-resolve through PartyHud.h's
RosterMember at the top of every Update/Draw — an index past the roster's
end just goes inert (no draw, no mouse) — so a roster of any size, or a
resize, can't dangle them. StartNewGame/LoadGame still reset members in
place (keeping each slot's loaded portrait); a roster SIZE change must
call GameUI::RebuildForRoster (deferred like RebuildForLanguage, never
from a widget callback) to re-lay-out the per-member widgets — BuildHud
lays out whatever count it finds (hand pairs fill 2 wide, 2+1 for three). Left column under the bar: the
facing/position panel, then the Options panel (torchlight dropdown,
Wait/Help). Right edge: a Dungeon Master-style control panel — six movement
arrow buttons (turn/forward over strafe/back; GameUI::onMoveAction →
Party::Act(MoveAction), the same discrete actions the bound keys map to in
HandleInput), a left+right HandSlot (PartyHud.h) pair per member (empty
boxes with the character's identity stripe; clicking logs "hands are empty"
until items exist), and a reserved Magic area below.

In the 3D view the mouse does two things (Game::Update, gated by
GameUI::HudMouseConsumed so HUD widgets win the click): LEFT-click picks a floor
tablet up onto the cursor (DungeonWorld::TryPickItem) or drops the held one
(DropItemAt); holding the RIGHT button and dragging is MOUSE LOOK. The drag adds
a yaw/pitch offset on top of the grid facing (Party::AddLook → m_lookYaw/Pitch;
DungeonWorld::UpdateCamera feeds the camera Party::EyeYaw()/EyePitch(), while
Yaw()/Facing() stay the grid pose for the HUD/compass). Once the yaw passes 45°
(kLookSnap) the ordinal facing snaps one quarter and the inverse folds back into
the offset, so the view glides on continuously while the grid facing turns under
it — look (and then walk) around corners, reach awkward floor items. Releasing
PARKS the view; after a hold it eases back to orthogonal with a slow-build/fast-
finish curve (a window to grab an item, then a settle), while a movement/turn
triggers a much faster straighten that OVERTAKES an in-flight hands-off return.
The return runs through the shared Core/Easing.h EaseLerp (same machinery as the
walk/turn tweens). The free-look offset is part of the save (DungeonWorld::
CaptureState/ApplyState + the SaveData look line), so a reload restores the exact
camera angle. Every duration, the hold, and both curves are user-tunable on the
Settings → Controls "Mouse Look" section (LookSettings, pushed in via SetLook).

## Threading & async monster AI

Monster AI runs OFF the main thread. Core/ThreadManager (namespace
dungeon::threads) is the engine-wide worker-thread registry — the one home for
"lots of stuff on lots of threads"; everything threaded becomes a CLIENT of it.
A worker runs a JobFn once per tick in a loop the Manager owns; the Manager
handles cadence (Options::hz), cooperative cancellation (std::jthread +
stop_token — the cadence sleep is a condition_variable_any that wakes on stop),
per-tick crash capture (a throwing job records the error and keeps running, no
std::terminate), and OS thread naming (SetThreadDescription → workers show by
name in the debugger/profilers). Full control surface, addressed by STABLE
WorkerId (monotonic, NOT the array index — so Reap can drop dead slots while
survivors keep their ids): Pause/Resume, SetRate (live cadence), SetPriority/
SetAffinity, RequestStop (cooperative) vs Kill (HARD — request stop, 250ms
grace, then TerminateThread + detach + State::Quarantined; force-termination can
leak the CRT heap lock → process-fatal, genuine last resort), Restart (reboot a
slot, force-terminating a wedged one so it never hangs), SetGlobalThrottle (a
governor scaling EVERY worker's cadence; wakeNow=false for per-frame use so it
doesn't wake everyone each frame), Reap (drop Dead/Quarantined slots). A built-in
supervisor thread auto-reboots an autoRestart worker that stalls past 5× its
watchdog. Watchdog "stall" is a DERIVED view in Inspect (a tick still Running
past Options::watchdogMs), not a stored state. Lock order: m_mx (registry) →
per-worker sleepMx; lifecycle ops serialize on a per-worker controlMx; Inspect/
SnapshotAll read atomics so they never block a worker. The Manager is owned by
Game (declared BEFORE m_world so it outlives every client) and is inspected/
controlled live from the dev console.

The AI itself (Game/MonsterAI.h, namespace dungeon::ai) is walled off like
MagicSystem — it knows nothing about DungeonWorld/Party/map, reaching the world
only through ai::IWorldView. THINKING is split from ACTING: Brain::Think (cheap,
IQ-gated) sets a monster's standing orders (ai::Intent: idle, or engage toward a
cell) plus a full chase PATH (Brain::FindPath, 4-connected BFS); the host
EXECUTES those orders EVERY frame at the monster's own move/attack cadence — so a
dim monster still moves and swings at full speed, only its CHANGE OF MIND lags.
ai::AsyncDirector spawns one worker per IQ bucket (4) on the Manager. Each frame
the main thread publishes an immutable ai::Snapshot (party cell, a revision-
cached walkability grid, live monster positions + per-monster id/iq/aggro — from
a POOL of reused buffers so steady-state frames allocate nothing per the memory
strategy; the blocked/occupancy sets are FLAT mapW*mapH grids, not node-based
containers, so clear-and-refill really is allocation-free — anything that
hand-builds a Snapshot, e.g. tools/ThreadStress, must size those grids) and the
workers post ai::Plan batches (intent + path; batches and their path vectors are
pooled per bucket with the same use_count()==1 reuse as the snapshots) the main
thread consumes and executes (popping path cells, re-validating each against LIVE
occupancy). Plans are keyed by a STABLE per-monster runtimeId (DungeonWorld
assigns from m_nextMonsterId, never reused) — NOT an array index — so a plan
whose monster died / changed bucket / was erased simply finds no match
(MonsterByRuntimeId) and is dropped, never misapplied to a neighbour that shifted
into its slot. A monster's iq (monsters.cat field; Scheduler::BucketForIq) picks
its bucket; bucket intervals are PRIME milliseconds (251/499/997/1999 ms ≈
4/2/1/0.5 Hz; Scheduler::BucketInterval) — coprime, so the buckets almost never
fire together (cicada pattern) instead of resonating like power-of-two harmonics.

Dev console (`~`) THREADS panel (top, under the perf gauges): a live row per
worker (name / state[colored] / iterations / last+avg ms / hz / pN priority /
reN restarts / `!N ~M` health — see Diagnostics) with clickable halt|run, << / >>
(halve/double rate), kill, and
boot (reboot a dead/quarantined slot). Layout lives in one place — Render records
the button rects, next frame's Update hit-tests clicks. Commands: throttle
<scale> (manual governor), governor auto [targetFps] | off (ADAPTIVE — eases all
background cadences when the frame's over budget, asymmetric easing so it
recovers; opt-in, keys off whole-frame time so it's a coarse heuristic, can be
GPU-bound), threadprio/threadaffinity <id> ..., threadspawn/threadwedge (stress
workers — the latter ignores its token, to exercise the hard Kill), threadreap,
and the diagnostics side: health / health probe / crashpoke.

## Diagnostics — exceptions, faults, stalls (docs/diagnostics.md is the model)

The game used to die with NO useful information: `wWinMain` had no try/catch, no
fault filter, no dump writer, and ThreadManager's worker catch kept only a bare
`lastError` string that the next failure overwrote and nothing ever logged. A
worker that threw a thousand times looked exactly like one that threw once — the
catch FALLS THROUGH, so a thrown tick still stamps its timings and still counts
as an iteration, making it statistically indistinguishable from a healthy one.
Four layers replace that, and the rule behind all of them is that a crash, a
hang and a reboot must each leave EVIDENCE.

- THE RECORD (Core/Diagnostics) — six kinds (Exception/Fault/Stall/Restart/
  Killed/Fatal), a 16-event ring per thread across 32 slots, each event carrying
  wall time, TSC (so it sits on the profiler's timeline), worker id, iteration,
  a 192-char message and a 32-frame stack. ALWAYS COMPILED IN, unlike
  Core/Profile — the crash worth reporting happens in a plain debug or release
  run, so a record gated behind a profiling preset would be absent exactly when
  wanted. THE REGISTRY OWNS THE STORAGE and a thread holds only a slot index
  (the same choice, for the same reason, as Core/AllocTrack): `Kill`
  force-terminates with TerminateThread, which runs no destructors and frees the
  thread's TLS, so a table of pointers INTO TLS would dangle exactly when the
  evidence is wanted. Writes are LOCK-FREE — a mutex here would be taken on the
  failure path, including by Kill moments before it ends a thread that might
  hold it. A writer claims a slot with one fetch_add and publishes with a
  release store to that slot's SEQUENCE NUMBER, which is the ABSOLUTE CLAIM
  INDEX (slot i holds event n only if seq == n+1, re-checked after copying);
  absolute indices are also what makes it ABA-immune, since a full lap lands on
  n+17. A REBOOT DOES NOT CLEAR THE RECORD — Profile resets a rebooted worker's
  slot, this deliberately does the opposite, because "it threw twice, stalled
  and was restarted" IS the history being asked for.
- THE LOG THROTTLE is TWO layers, because they catch different failures.
  Identical consecutive events collapse to powers of ten; DISTINCT ones are
  rate-limited per THREAD (8 lines/sec, with a line saying how many were
  swallowed). The second layer exists because the collapse is blind to a message
  carrying a tick number — measured, 16k such events wrote a 1.2 MB log, and
  2.7 KB after. The RECORD still takes every event; only the log is throttled.
  Anything added here must key on the thread, not the message: the message is
  exactly the part a failing worker varies.
- THE CAPTURE SITES — ThreadManager's worker catch (records kind/worker/tick/
  message); the supervisor, which records the STALL and the REBOOT as two
  separate facts, once per stall EPISODE (it polls at 100 ms; a minute-long
  wedge would otherwise write 600 identical events and flush the ring) and
  detects INDEPENDENTLY of whether it reboots, so a worker with no autoRestart
  is covered; `Kill` (against the VICTIM's timeline, with no stack — the only
  stack there belongs to the killer); and the main thread, which has a slot, a
  frame try/catch and a DIE-AFTER-10-CONSECUTIVE policy. Sharp edge commented at
  the site: a throw between BeginFrame and EndFrame leaves the command list
  open, so the following frame is unlikely to be sound — the counter bounds it.
  Core/CrashHandler adds what no catch can see: SetUnhandledExceptionFilter for
  SEH faults (access violation, divide-by-zero, stack overflow — MOST of what
  actually kills a game), set_terminate, DN_ASSERT routed through ReportFatal,
  and minidumps capped at 3 a run. Everything there assumes a damaged process:
  no heap, no locks, paths snapshotted into fixed buffers at Install, a
  re-entrancy guard, and the record written BEFORE the dump and the dump before
  the log — decreasing order of how likely each is to survive.
- THE STACKS (Core/StackTrace, lifted out of AllocTrack's private symbolizer;
  AllocTrack keeps its own SeenSet so crash sites and allocation sites cannot
  mask each other). THE HARD PART: at a `catch` site the stack has ALREADY
  UNWOUND, so capturing there names the handler and never the thrower. A
  VECTORED EXCEPTION HANDLER installed first in the chain records every C++
  throw (0xE06D7363) on the throwing thread before unwinding, and catch sites
  read it back through ThrowFrames (caveat: it is the LAST throw, so a throw
  during unwinding can leave an outer catch holding the inner one's stack).
  A FAULT is the mirror image — nothing unwinds, but the frames are in the
  CONTEXT_RECORD, so WalkContext runs StackWalk64 over a COPY of it (the walk
  mutates what it walks and the dump writer needs the original). WalkThread —
  the live probe — suspends another thread and walks it with
  RtlLookupFunctionEntry + RtlVirtualUnwind, NOT StackWalk64, because that takes
  DbgHelp's lock and the thread you just froze might be holding it: the probe
  would deadlock the process it was meant to diagnose. Nothing between Suspend
  and Resume touches DbgHelp, allocates or logs; symbolizing happens after. A
  frame with no unwind entry STOPS the walk rather than guessing a return
  address — invented frames look real. DbgHelp is single-threaded by contract,
  so every Sym* call serializes on one mutex, which is also why symbolizing is
  not part of recording. IsPlumbingFrame is ONE rule for every readout (a stack
  that reads differently in two places cannot be compared) and is deliberately
  NOT applied to the probe, where the OS frame IS the diagnosis
  (NtWaitForSingleObject names a lock). A stack is logged ONCE per distinct
  site, or a repeating failure would undo the rate limit.
- THE READOUTS — the console's HEALTH section: one strip per thread that has
  failed, on the profile graphs' x-axis (240 samples x 50 ms = 12 s), marks
  coloured by kind, oldest at the left, CLICK A MARK for the event and its
  stack (it snaps to the nearest mark within 4 cells — a cell is about a pixel).
  A cell keeps the MOST SEVERE kind in its window, not the last. Deliberately
  its own strip rather than rows on the profile graphs: those are per NODE while
  health is per THREAD, and the profiler is compiled out of plain builds while
  this is not. The section only exists once something has gone wrong. Plus the
  THREADS panel's `!N ~M` column (exceptions/stalls — without it a worker that
  threw 18 times reads `sleeping · it 18 · 2.00hz`, every column normal), and
  `health` / `health <thread>` / `health probe <id|name>`.
- CHECKED, NOT ASSUMED. `DiagTest.exe` (tools/DiagTest) exercises the ring
  directly — 35 checks, including the one that matters: four writers hammering
  one slot while a reader walks it, every event self-describing so a torn read
  cannot pass (measured 16k writes, 39k live reads, 0 torn). `tools\HealthTest.
  ps1` breaks the REAL game seven ways and reads dungeon.log and nothing else —
  if the answer is not in the file you open after a crash, it does not count.
  `-SelfTest` skips every injection and requires FAIL, and all 7 cases plus
  every expectation do fail, which is the evidence none is vacuously satisfied.
  Dev: `crashpoke <throw|worker|fault|assert>`, `threadwedge`, `threadspawn
  <ms>`. NOT covered: the Killed kind (a hard kill is a panel button, not a
  command) — the harness says so on every run rather than leaving it to be
  discovered.

## Map overlay / editor (MapView)

A stylized top-down map with two modes (MapView::Mode). Like the dev console
it is NOT an AppState — Game owns m_mapView and, while it is open, keeps
calling m_world.Update, so the world simulates and the party still walks on
the keyboard; the overlay only claims the MOUSE (pan/zoom/edit). The panel
rect comes from Game::MapPanel (mode-aware).
- Player mode (`M` toggles open/closed; Esc also closes — both handled before
  the Esc→Paused branch in Game::Update): the in-game map. An 80%-centered
  panel drawn over the HUD behind a dim wash (so the scene shows around it).
  Fog of war — only revealed cells and their contents draw — plus a centered
  title and a right-docked symbol KEY (a trimmed subset: party/start/torch/
  brazier/monster/item/button, dropping the obvious wall/floor rows). The key
  collapses like the editor docks (own persisted flag map_player_key_collapsed).
  No brush dock / editing. The `M` key is hardcoded (kKeyFields is MoveKeys-
  only; a bindable map key needs a separate UI-keybinds table).
- Editor mode (dev console: `editor` opens/flips into it, `editor off` returns
  to Player without disturbing the view; reachable in all builds): the
  dungeon-builder. FULL-SCREEN and drawn alone — Game skips the shadow/scene
  passes and the HUD while it is up (editorMap flag in Render), so nothing
  renders behind it. The WHOLE map and EVERY creature/item draw regardless of
  fog. Two docks: a brush palette LEFT (MapView::LeftDockRect) and a symbol
  KEY/legend RIGHT (RightDockRect); the map grid lives in GridArea (panel minus
  BOTH docks) so it never draws under a dock. Each dock collapses to a thin
  strip showing only its flip-arrow button (left `<<`/`>>`, right `>>`/`<<`);
  the two collapsed flags persist in settings.ini (map_palette_collapsed,
  map_legend_collapsed) — MapView holds a GameSettings& and Save()s on toggle.
  SetMode flips an open map's mode in place; Open(mode) resets the view to
  fit-whole-map.

MapView (Game lib) is the one renderer + one pick math behind both modes; the
Editor-only brush palette + brush-apply logic live in a separate collaborator,
MapEditor (NOT a subclass — that would fight the in-place Player⇄Editor mode
flip). Game owns both and wires the view to the editor (MapView::SetEditor); the
view draws the left-dock frame/collapse/header and hit-tests the grid, then
drives MapEditor for the palette body (RenderBody/OnClick/OnWheel) and the brush
(Paint→ApplyBrush). The shared map ink palette is MapColors.h.
Cells render as filled blocks (walls = bright stone ink, floors recede),
fixtures/entities/monsters/items as BAKED MODEL ICONS — each kind renders its
own 3D model once into a small RT (UpdateMapIcons: monster kinds get a
head-shot framing the model's top quarter, decorations/fixtures bake whole,
floor items reuse their HUD item icons at the cell corner; colored square +
type initial is the not-yet-baked fallback), semantic glyphs stay glyphs (the
start cell's accent outline, door bars, stair/pit triangles, the party as
a rotated triangle — facing*90° CW from north-up; screen Y is down so it
matches the compass; SpriteBatch::DrawTriangle — and IN-FLIGHT PROJECTILES
as small arrowheads at their SUB-CELL world positions, pointing along
travel, colored by side (blue = party shot, amber = monster shot). The
projectile is TRANSIENT combat content — ProjectileSystem gives each item a
stable runtime id (DungeonWorld::LiveProjectiles / ProjectilesAt /
ProjectileById / RemoveProjectile pass through); right-clicking one opens the
ProjectileInspector (a standalone read-only modal — side / damage type+amount
/ accuracy / speed / range-left, with a Remove that dismisses the in-flight
item). Freeze one with the editor's pause button to catch a fast shot.
AnyInspectableAt counts projectiles so InspectAt fires onInspect on their
cell.). Editor-only green facing
arrows skip types with catalog `facing_arrow = 0` (monsters: `faces = false`);
the instance inspector's "Map arrow" checkbox beside its Facing dropdown edits
that per type. Visibility goes through
MapView::CellVisible (always true in Editor, else IsSeen). The transform is
resolution-independent (pan = fraction of the grid area, zoom = unitless,
fit-whole-map at zoom 1) and resolves against GridArea, so Update (window-
pixel panel, matches mouse coords) and Render (device-pixel panel) agree;
zoom is cursor-anchored. CellAt is the inverse pick. The left-dock palette has a
fixed CONTROLS ROW at the top of its body (above the scrolled accordion): a
FILTER text box + [x] clear + [-] collapse-all. Clicking the box focuses it
(typed chars land there and MapEditor::KeyboardCaptured gates the game's
party keys / M / Esc so an 'm' doesn't toggle the map; Esc/Enter or a grid
paint release it); a set filter lists matching items FLAT under each category
header regardless of accordion/group state and drops empty categories, [x]
clears it, [-] collapses every accordion + sub-group. Below it, the palette is a
catalog-driven collapsible accordion (MapEditor::PaletteCat + the kCategoryInfo
table): Walls/Floors/Ceilings — THE structural brushes (the old Structure
Wall/Floor rows folded in): per-cell surface VARIANT paint via DungeonMap
variant grids that also CONVERTS the cell type on click/rect — a wall
texture on a floor square raises the wall, a floor/ceiling texture carves
rock walkable — while FLOOD stays a recolor (its region keys on the
resolved variant, so a wrong-type start is a no-op, not a room-to-solid
foot-gun). The BLOCK owns its texture: wall variants live on the SOLID
cell, one texture for all four faces of that block both sides included,
floor/ceiling variants on the floor cell; middle-click's variant-reset
rung restores the default hash-varied mix (no override pinned). Stale
variant records on the wrong cell type — pre-2026-07-13
files kept wall variants on bordering floor cells — are DROPPED at load), and the entity
categories Decorations/Fixtures/Monsters/Buttons/Doors/Stairs/Items/Weapons/Armor
(live placement). Entries carrying a `category` field group under collapsible
SUB-accordions ("+ Weapon (4)"); every catalog is authored with them. ITEMS
are three catalogs — items.cat (runes/keys/food/containers/ingredients),
weapons.cat, armor.cat — split ONLY so weapon (damage/speed/skill/stats/reach/
command) and armor (armor/resists) settings don't clutter every other item's
type editor. All three are EntityKind::Item at runtime and place/carry/equip
identically; Project::FindItem / HasItem / AllItems resolve an item id across the
three so nothing downstream cares which file it is in (a weapon/armor rename
sweeps the same `item` .ent records). A new weapon/armor gets `name = item.<id>`
+ a `category` default like any item.
Items in each category come from the active project's catalogs; a "+ New..." row
opens the asset-creation dialog — which makes a type THREE ways (AssetDialog::
Source): Import new (browse + AssetBaker, the original), Use installed (bind an
asset already in the pool — no bake, so a second wall type off an existing set
is seconds, not a re-import) and Duplicate (copy another entry of this category,
including fields no schema row covers). The id is validated as you type
([A-Za-z0-9_-], records are whitespace-tokenised) and CHECKED FOR COLLISION —
Catalog::Add replaces by id, so an unchecked name silently overwrote a type
every level used. The new entry's SHAPE comes from the category's schema
defaults (Game::CreateCatalogEntry), so a new stair gets its up/pair/hole rows
and a new item its weight/holdable, where the old writer stamped
authored=1/solid=1 on everything; a surface type also joins the viewed level's
palette on creation (Phase 1's seam), since otherwise it would be unreachable.
An import REPORTS what it will do first: assets::DiscoverPbrMaps (moved out of
AssetBaker into Assets so the dialog and the baker cannot disagree) lists the
maps recognised in the folder, warns when no height map means flat parallax, and
pre-ticks the --flip-green override from the normal map's filename. The preview
pane shows the picked mesh, or wall_block.gltf wearing the picked texture set —
including one still loose in a download folder, since the maps are loaded from
their source files. A failed bake now lands in the dialog with the exit code
instead of only in the log. An import is RECORDED in the project's provenance
manifest (`catalog/imports.cat`: pool asset name → kind / source path /
flip_green / the surface kind its worn meshes were baked as), because the baked
pool is gitignored — without it a created type reaches git as a catalog entry
whose asset a fresh clone cannot rebuild. `tools\ReplayImports.ps1` replays the
missing ones (re-rooting a source path from another machine onto this one's
OneDrive archive), and `synctosource` now also copies the manifest's asset FILES
from the exe-side pool into the source tree, so a `build/` wipe doesn't take
them. NOTE the naming rule an editor import must follow: a PBR set installs under
its RESOLUTION-tagged name (`<set>_2k` — LoadPbrSet's universal fallback) while
the catalog's `texture` field names the BASE; the worn-block bake takes the base
and finds the height map at any installed resolution. EXCEPT the three surface categories,
whose rows come from the VIEWED LEVEL's `palette` record, not the catalog — a
catalog type must JOIN that palette before the brush can reach it, which is what
their extra "+ Catalog..." row does (a chooser of the types this level doesn't
list yet → DungeonWorld::AddPaletteEntry, or ...Remote for a browsed level's
stash; the row hides once the level uses them all). The append is UNDOABLE (the
palette rides the map through the undo snapshot; RestoreEditorState flags
m_surfacesDirty so FlushGeometry reloads the sets, not just the chunks) and
reloads that surface's textures + worn meshes live (ReloadDungeonBlocks), gated
by SurfaceAssetsAvailable — the worn-mesh load is a LoadModelOrDie that would
ABORT on an unbaked type. APPEND-ONLY, and that rule is load-bearing: `variant`
records store the palette INDEX, so inserting or removing mid-list would
silently repaint every cell above it (removal needs an index remap — not built).
For the same reason a paint validates the armed index against the viewed
palette's CURRENT size (MapEditor::PaintCell): browsing a level with a shorter
palette, or undoing an add, outlives the index the brush was armed with.
A pool asset field (`texture` / `model`) is NOT a dropdown: those rows are
buttons that open the ASSET PICKER (Game/AssetPicker.*, modal above whatever
opened it — the type editor and the create dialog's "Use installed"), a
searchable thumbnail GRID of the installed sets/models with resolution badges,
a details pane (maps present, height-map real-vs-flat, size on disk, the
imports.cat source) and the shared 3D preview. Texture tiles cost nothing to
show: the installed .dds mip chain is loaded with its big levels DROPPED (the
first level ≤128px down), ~16 KB a tile. Model tiles are RENDERED via
DungeonWorld::BakeIconFor (the map-icon rig) — two rules there, each learned the
hard way: create the render target in UPDATE, never mid-recording (gfx::Texture::
RenderTarget drains the GPU), and REBIND THE BACK BUFFER after baking or the 2D
pass draws itself into the last icon at its 256px viewport. Tiles evict
least-recently-seen past a cap (drain before freeing — the SRV rule).
RIGHT-CLICKING a palette row opens the per-TYPE catalog editor
(TypeEditorDialog) for EVERY category — one dialog, because it renders its form
from a SCHEMA: Game/CatalogSchema.h is a FieldSpec table per catalog (key, kind,
section, range/step, options, one-line help), so exposing a field is one table
row and a new category is one table (the kBalanceFields idiom). Sections become
tabs, kinds become widgets (Bool→checkbox, Float→snapped slider, Enum/
TextureSet/Model/CatalogRef→dropdown filled by Game through optionsFor —
AssetUtil::InstalledTextureSets/InstalledModels scan the pool), and "?" explains
the active tab's fields. NO live apply (a type is referenced by every placement
and, for surfaces, by baked geometry): Save writes the .cat and, when a touched
field is `rebakes` (a surface's texture/relief/wear), re-runs the wornblock bake
behind the busy overlay. A surface's PER-DRAW knobs are the exception —
`height_scale`, `metallic` and `roughness` are per-variant values the draw reads
(Surface::heightScale / ::factors, filled by ResolveSurfacePalettes →
ApplySurfaceFactors), so saving them pushes at the live scene through
DungeonWorld::RefreshSurfaceMaterials: no reload, no rebuild. The factors follow
the PROP rule — absent = -1 = the set's ORM map stays authoritative, a value
REPLACES the draw's factor (which the shader multiplies over the map). Wart: an
absent factor draws as 0.00 on its slider, indistinguishable from an explicit 0
(only TOUCHED fields are written, so the behaviour is right — the display just
doesn't say "map-driven"). Only fields the user TOUCHED are written and an empty
value REMOVES the field, so rows the schema doesn't cover survive — including the
ones MonsterConfigDialog owns and rewrites (states/anim_*/archetype/threat_*),
which is why the monster schema omits them and offers an "Animation..." button
through to that dialog instead. WallStyleDialog is GONE — relief/wear are just
schema rows now (`columns` is gone too, see the worn-block bullet). RENAME + DELETE live here too: the id in the title is a rename
affordance (click it, edit, Enter — the LevelSettingsDialog stem pattern), and
Delete arms on the first click. Both go through a REFERENCE SWEEP
(DungeonWorld::SweepTypeRefs) that walks EVERY level — the live one, the edit
stashes, and any not yet in memory, parsed on demand — plus the cross-catalog
fields (stairs `pair`, doors `key`) and the project's default fixture ids
(Game::SweepCatalogRefs, a closed list). A rename rewrites all of them, renames
the entry IN PLACE (Catalog::Rename — a remove + re-add would move it to the end
of the file, dragging its lead comments, i.e. the file header, with it),
re-spawns the live objects from the retyped records (RespawnFromRecords) and
CLEARS the undo history (every held snapshot names the old id). A delete REFUSES
while anything still references the type and says which levels — a record naming
a missing type is not a soft failure at load. `savemap` persists the touched
levels. Catalog comments survive a write (serialize::Block::lead →
CatalogEntry::lead): the .cat headers document each category's fields, and an
editor write used to delete them. Mouse model: LEFT paints/places
the armed brush (nothing armed until a palette row is picked), a stationary
RIGHT-CLICK inspects the cell (select + contents + the object's edit dialog
immediately; ≤3px press-release = click) while a right-DRAG pans, and
MIDDLE-CLICK erases (the ladder: stair pair → entity → fixture → variant
reset; one undo step each). MODIFIER GESTURES on a left press (paint brushes
only — Structure + surface variants; placement falls back to a plain click):
Shift+click fills the RECTANGLE from the last painted cell (every paint
leaves the anchor, so click-then-shift-click is the Photoshop line idiom),
Ctrl+click FLOOD-fills the contiguous region (same cell type + same RESOLVED
variant — override-or-hash, matching the 3D scene), Alt+click is the
EYEDROPPER (arms the clicked square's wall/floor texture; ceilings pick while
the Ceilings brush is armed). Rect/flood are ONE undo step and message
map.fill.done. The editor's TOOLBAR is a full-width band fixed across the
panel top (docks + grid inset below it): the level DROPDOWN (all levels in
project order; hand-rolled popup — picking one browses it; Player mode keeps
the [^]/[v] arrows instead) and the [+] NEW LEVEL button pin its left end
(Game::CreateNewLevel writes a minimal 16x16 .map/.ent — 3x3 'P' room,
palettes copied from the ACTIVE level — appends the manifest, and the view
jumps onto the new canvas); the tools sit right as house-style ICON DISCS
(assets/ui/icon_tb_*.png: Wenrexa discs + composited glyphs; hover brightens
+ shows a tooltip under the band; a missing icon falls back to the text
face) — Level (per-level settings dialog) / Balance (combat tuning) /
undo/redo (dimmed when their stack is empty) / PLAY-PAUSE (the editor is a
LIVE view — the world keeps simulating while it is open; this freezes it so
you edit against a still scene: MapView::EditorPaused → Game SKIPS the whole
m_world.Update, since monster actions fire off cooldowns not dt and a full-
screen editor renders no scene needing a camera/light refresh. The button
shows the ACTION — pause glyph while running, play glyph + "resume" tooltip
while frozen — and the flag ALWAYS clears when the overlay closes or flips to
Player mode, so a closed editor is never left paused) / Save (savemap) / To
source (synctosource) — built by MapView::ToolbarButtons (ONE item list that
geometry, hover, click dispatch and render all walk; adding a tool is one
`add` line; hover on hand-drawn chrome is tracked by HoverBtn identity
across the window-px/device-px split). The Level button opens
LevelSettingsDialog for the VIEWED level (active or browsed): the three
lighting mood knobs — dust density / haze ambient / ambient scale —
live-previewed while that level is active, committed to the level's
map/stash on Save, and persisted as the .map `atmosphere` record
(`atmosphere dust=… haze=… ambient=…`, only set values written;
DungeonWorld::EffectiveAtmosphere resolves unset ones to the
gfx::Atmosphere defaults, applied in BuildTurbidityMap on every load/
swap/fixture-rebuild — the dev console dust/haze/ambient knobs override live
but are reset by that application). The dialog's title stem is a RENAME
affordance: click it, edit inline ([A-Za-z0-9_-]), Enter commits —
Game::RenameLevel (manifest + browse fix-up) + DungeonWorld::RenameLevel
(file moves, stash rekeys, stair dest= sweep via lazy EnsureMapStash, undo
history drop). Old SAVE FILES keep the old stem and won't load past a
rename (dev-cycle cost).
A structural paint → DungeonWorld::EditCell → DungeonMap::SetCell (bumps
Revision()) → RebuildChunksAround(x,z), which rebuilds ONLY the touched chunk +
its orthogonal-neighbour chunks (≤5), not the whole map — so paints are near-
instant (the old whole-map RebuildGeometry is gone; BuildDungeonMeshes is the full
bake for load/quality-swap). Placement appends to the live world lists (and
DungeonMap for fixtures), drawn next frame. Markers draw from the LIVE world
(MonsterMarkers/DecorationMarkers), so placed/erased entities show immediately.
Both modes can BROWSE other levels: [^]/[v] arrows top-left of the grid step the
viewed level through the project's level order (an edge level hides its dead-
direction arrow), with the stem labelled beside them (accent color = not the
party's level). A browsed level draws a read-only snapshot
(DungeonWorld::BrowseLevel: static map with the edit stash winning over the
file, .ent records ditto, and in Player mode the stashed fog — a never-visited
level shows nothing); the selection/party/live markers are active-level only,
and the view snaps back to live if the party arrives on the browsed level. In
EDITOR mode the brush EDITS the browsed level too (the editor edits ANY level):
MapEditor routes those edits to DungeonWorld's remote seam (EditCellRemote /
EditVariantRemote / Add{Decoration,Monster,Fixture}Remote / EraseRemote /
AddStairAt), which mutates the level's in-memory stashes — m_levelMaps (static;
also stashed on every level swap so unsaved edits survive, live decoration
placements synced back into records first) and m_levelEnts (.ent records,
created on demand; record ids stay stable across removals so the per-id
dynamic diffs in m_levelStates remain valid) — and MapView rebuilds the browse
snapshot after each paint. Entering a level consumes its stashes; the Select
tool's inspectors still need the level active (no live instances remotely).
DOORS are functional (doors.cat, EntityKind::Door, .ent record `door <type>
<x> <z> <facing> [name=] [open=1]`): a door fills a DOORWAY cell (solid walls
flanking exactly one axis — the brush auto-detects the orientation, no facing
UI) and blocks the party (isOccupied), monsters (AI blocked set + slot checks)
and projectiles until opened. The panel (door_panel.gltf — door.gltf is the
COSMETIC decoration, don't collide) slides sideways into the wall (openT anim);
open it by clicking from the cell in front (Game's world-click falls through
TryPickItem to ToggleDoorAhead) or via a button whose target= names the door's
name= (ToggleButtonAt → ToggleDoorsNamed — the button wiring's first consumer).
Doors are RECORD-BACKED: placement authors the .ent record AND spawns the live
instance (one truth for writer/stash/remote), open-state diffs ride the save
like button toggles (kind Door reuses EntityState.activated; "door <id> <open>"
save lines). doors.cat `hidden = 1` marks internal entries (the shared
[door_frame]) the palette skips. Stairs AND PITS are one cross-level op for
any viewed level: each half lands
on the live map when its side is the active level (prop too), else in that
level's stash. stairs.cat drives everything per type: `up` (destination
direction + map-icon arrow), `pair` (the type auto-authored on the destination
— pit pairs with pit_ceiling), `hole` = floor|ceiling|none (which cell block
the mesh builder skips so the type's shaft mesh shows: CellHolesFn, fed by
DungeonWorld::FloorHoleAt/CeilingHoleAt), `traverse` = 0 (stepping on the tile
does NOT transition — a pit's ceiling hole is scenery), `fall` = 1 (the
transition is a PLUNGE: the step glide onto the pit finishes, then the camera
drops through the shaft on an accelerating curve — DungeonWorld::m_pendingFall
sequences it in Update, PartyEye applies the drop to camera + carried torch —
then the swap fires with the party's facing preserved; movement is swallowed
meanwhile, and the world.pitfall message plays). Meshes:
stairs.gltf (rising flight), stairs_down.gltf (below-grade stairwell shaft),
pit.gltf (sheer drop, a storey deep), pit_ceiling.gltf (the rising shaft above
a ceiling hole on the level below) — all AssetBaker Build*. Live stair/pit
placement/erase rebuilds the touched chunks so holes open/close immediately. Edits persist via the dev console `savemap` =
DungeonWorld::SaveAllLevels (the active level from live state + every stashed
level from its records; an untouched .ent is not rewritten), and
`synctosource` copies the project to the git source tree. All overlay text
goes through Loc (map.* keys).

The editor edits a PROJECT (see "Project & catalogs" below), not hardcoded
content — adding a category/type is data, not code.

Fog of war (Player mode) is on day one: DungeonWorld::m_seen is a per-cell
bitset (dynamic/save-side state, NEVER baked into DungeonMap), revealed via
MarkSeen (a cell + its 8 neighbors) on every Party::onStep and on edits,
seeded at the start cell. The planned reveal items (map fragments, reveal
spells) just feed the same set — MarkSeen over a region — so they need no
MapView change; a detect-monsters effect would instead be an entity-only
override layered on CellVisible. A future save serializes m_seen alongside
the .ent layer.

SpriteBatch gained DrawTriangle (the markers) and DrawSpriteRotated/
DrawRectRotated (rotate the 4 corner verts; for future textured/rotated
editor icons) — the axis-aligned DrawRect/DrawSprite couldn't express them, plus
a DrawSprite overload taking a raw GPU SRV handle (for the asset preview RT).

## Project & catalogs (the data model)

Content is data, not hardcoded. A PROJECT is a folder under
`assets/projects/<name>/` (default `dungeon-demo`) holding DEFINITIONS + LEVELS,
separate from the shared baked asset POOL (`assets/textures`, `assets/models`,
worn_*, lang, shaders — what AssetBaker emits):
- `project.ini` — manifest (name, level list, default fixture ids), block format.
- `catalog/*.cat` — one Catalog per category (walls/floors/ceilings/decorations/
  fixtures/monsters/doors/stairs/items/weapons/armor/effects), block format:
  `[id]` headers + `key = value` fields naming pool assets (model/texture) +
  params (solid/authored/height_scale/mount). Levels reference catalog ids.
  NOT every catalog is placeable: `effects` is authored + tuned only, so its
  palette category opens the type editor on a row click and offers no
  "+ New..." (an effect needs a class) — MapEditor's `placeable` flag.
- `levels/<stem>.map` + `.ent` — the level layers. The .map's surface palette is
  a `palette <wall|floor|ceiling> <id>...` record (catalog ids), and it also
  carries `stairs <type> <x> <z> [facing] dest= destx= destz= [destfacing=]`,
  `variant <wall|floor|ceiling> <x> <z> <index>`, and `atmosphere [dust=…]
  [haze=…] [ambient=…]` (per-level mood knobs, authored by the editor's Level
  settings dialog) records. (The old `assets/maps/level1.*` with `textures`
  records is dead — superseded by the project copies.)
- FEATURES are the one kind of content that is not a prop: a mesh stamped IN
  PLACE OF a surface block, into that block's own variant bucket, so it wears
  the cell's texture and IS the surface rather than sitting on it. Two flavours,
  the same idea turned 90 degrees: `niche <type> <x> <z> [facing]` replaces a
  wall panel (wallfeatures.cat), and `floorfeature <type> <x> <z>` replaces a
  cell's FLOOR block (floorfeatures.cat, `[recess]`). REACH FOR A FEATURE, NOT A
  PROP, whenever the thing is a hole: a floor grate modelled as a prop can only
  ever be a box parked on the floor, because the floor is a displaced grid and
  nothing below y=0 is visible — Michael rejected exactly that on sight, and the
  recess is the honest fix. A feature mesh MUST match the block it replaces: full
  cell extent, surface at y = 0, and the block's own UVs (floor: u = x + 0.5,
  v = z + 0.5). What makes a flat tile meet its displaced neighbours seamlessly
  is that the worn blocks pin their displacement to zero at the cell edges
  (PinRamp), the same property that lets them tile at all;
  `tools/BuildFloorRecess.py` asserts both and is the reference. A feature is ONE
  mesh shared by all 54 surfaces, so like the wall features it takes the 2:1
  aspect correction at STAMP time (`floorUAspect`), never baked. And because it
  rides the variant bucket it can only wear the cell's texture — so anything
  needing its OWN material composes on top as a decoration, which is why an iron
  grate is two records: `floorfeature recess` plus `decoration floor_grate`.

Serialize.* is the block (de)serialization primitive (free Find/Get/GetFloat/
GetBool/Set over a Field vector; Block + CatalogEntry both delegate). Catalog.*
adds CatalogGet/CatalogBool (null-safe). Project.* loads/saves the manifest +
catalogs and maps a key→Catalog (CatalogForKey). DungeonWorld resolves catalog ids
to model+texture at load (ModelAndTexture helper). Game owns the active Project,
passes it to DungeonWorld; MapView reads it for the palette. NOTE: editor/asset/
level WRITES go through paths::Asset, which in a dev build IS the git source
tree — a `savemap` or an editor import dirties the working copy immediately, so
`git status` (and `git checkout` to discard) is the review surface. The "To
source" button and `synctosource` survive only for a PACKAGED build, where
assets are the copy beside the exe; in a dev build they detect
paths::AssetsDir() == paths::RepoAssetsDir() and report "nothing to copy".
Full per-phase history + gotchas live in the editor-overhaul memory.

## Workflow conventions used so far

- Verify changes by launching the exe and driving it with PostMessage
  keystrokes + CopyFromScreen screenshots into docs/ (dot-source
  docs/drive.ps1: Key/Click/Shot helpers, client coords; ALWAYS send the
  keyup or the next keydown of that key won't register as pressed).
  Menu nav: Down/Enter; allow ~10s+ load on
  High/Ultra cold cache before sending keys.
- Commit per feature with detailed messages; push to origin/main. Long
  commit messages via a temp file + `git commit -F` (PowerShell mangles
  embedded quotes).
- ONE branch PER WORKING DIRECTORY. A git branch only isolates committed
  history, NOT the files on disk — the working tree is a single shared
  checkout with one HEAD. So parallel sessions/branches must each use their
  OWN directory: `git worktree add ../Dungeon-<branch> <branch>` (separate
  folder, same repo) or a separate clone. Do NOT run two branches' work out
  of `C:\Dev\Dungeon` at once — switching HEAD switches it for everyone and their
  uncommitted edits intermingle (this bit us: rune work and a save-improvements
  session collided in the same tree).
- IMMEDIATELY AFTER `git worktree add`, PROVISION THE GITIGNORED ASSETS before
  launching — a fresh worktree only checks out TRACKED files, and the game
  load-or-dies on derived/imported assets that are gitignored. There are TWO —
  and only two, confirmed with `git status --ignored --porcelain assets` (a
  missing-texture magenta-placeholder fallback exists, but there is NO fallback
  for models — a missing `.glb` aborts hard at level load, e.g.
  `AssetUtil.cpp model.has_value()` "failed to parse glTF: ...viking_dagger.glb").
  Copy both from a populated sibling worktree (e.g. `C:\Dev\Dungeon`). There is
  NO third step: the game reads `<worktree>\assets` directly, so nothing is
  mirrored into `build\<cfg>\bin` and a worktree costs one copy, not one per
  config:
  - `assets\textures` — whole dir, BOTH `.dds` (BC7) and source `.png` (dds-only
    still renders magenta; ~273 dds + ~261 png).
  - `assets\models` gitignored files — the imported authored meshes (`.glb`) AND
    the bought rigged monsters gitignored BY NAME despite the `.gltf` extension
    (embedded-texture GLBs inside), each often with an `.anim.cat` sidecar.
    DON'T trust a hardcoded list — the set GROWS over time (the daggers +
    centipede/giant_spider were once "the whole list", then a skeleton-warrior
    kit — skel_bare/berserker/spearman/warrior + their .anim.cat — was added and
    an old-list provision still aborted on `skel_warrior.gltf`, 2026-07-21).
    DISCOVER the real set from a populated sibling and copy exactly those:
    `git -C <populated> status --ignored --porcelain assets/models | grep '^!!'`
    (or just robocopy the whole `assets\models` dir — the committed `.gltf` that
    come with the checkout copy identically, so it's safe and future-proof).
  Use BACKSLASH paths (robocopy rejects forward slashes → copies nothing) and
  VERIFY with a file count afterward — robocopy returns exit 0 when it copied
  NOTHING (exit 1 = files copied), so a "successful" run can leave you empty. The
  regenerable alternative is `tools\FetchTextures.ps1` + `FetchModels.ps1` in the
  background (needs `build\<cfg>\bin\AssetBaker.exe` first, so build once). Symptom
  decoder: magenta scene = missing textures; hard abort on a `.glb` = missing
  models.
- AFTER a branch is merged to main, TIDY UP its worktree so the drive doesn't
  fill up. Once the merge is on main and pushed, remove the now-dead working
  copy: `git worktree remove ../Dungeon-<branch>` (add `--force` if it still
  holds leftover gitignored files like the baked textures), then `git branch -d
  <branch>` to drop the merged branch and `git worktree prune` to clear stale
  metadata. First confirm the branch really is merged (`git branch --merged
  main`) with no uncommitted/unpushed work — the worktree's gitignored assets are
  regenerable (FetchTextures.ps1 / FetchModels.ps1) but un-merged commits are not.
- NEVER rewrite UTF-8 files via PowerShell Get-Content/Set-Content — it
  mojibakes em-dashes (happened twice). Use the Write/Edit tools.
- User prefs: concise replies, no emojis; permission prompts disabled.

## Known gaps / natural next steps

- Combat is built out (see the COMBAT bullet: the attack formula, damage
  types/resists, stamina exertion, death/revive, DoTs, reach, quadrant
  lanes) but UNTUNED — every number is a first cut awaiting a balance pass
  (the editor's Balance dialog / balance.cat + attacks.cat). No polearm or
  ranged weapon is authored yet (the reach/lane plumbing is ready); no
  healing source exists beyond unconscious self-stabilize (potions and a
  heal spell are queued in the magic backlog). Portraits are simple baked
  busts (AssetBaker portraits); the tinted-initial fallback still draws if
  the textures are missing.
- Monster models are still simple procedural rigs (tapered-tube limbs + a
  skull for the humanoids, a lumpy sphere for the blob); a bought/authored
  rigged glTF would drop in via LoadModel (JOINTS_0 remap already handled).
  Everything is PBR-textured: each generated prop binds a scanned set by name
  (DungeonWorld::LoadPropTextures, shared with decorations) — sconce=worn-
  medieval iron, brazier=bronze, skeleton=carved limestone
  (bone), mummy=stained burlap, blob=alien-slime. ModelBaker gives the box-
  built props world-aligned tiling UVs (TileUvs); the glTF baseColor stays as
  the flat fallback if a set is missing. Bought authored decoration meshes
  (boulder/mossy_rock/pot) ride the import-model path like ancient_pot.
- BC7 encoder implements 4 of the 8 modes (6, 1, 3, 5 — see the `AssetBaker mips`
  bullet and docs/bc7.md). The unimplemented ones are quality left on the table,
  not a correctness gap: a mode is only ever chosen when it MEASURES better, so
  the missing ones cost dB, never pixels. MODE 7 (the strongest candidate — the
  only mode with two subsets AND alpha) was measured and DECLINED: `Bc7Test
  --headroom` runs its real search and it wins 2.3% of blocks for +0.09 dB. It
  buys its second subset by being the coarsest two-subset mode there is (5 colour
  bits + a p-bit across all four channels, four index positions), and mode 5's
  decoupled channel beats that on the very blocks it targets. Modes 0/2/4 are
  narrower still. THE LESSON, which generalises past BC7: the CEILING said +3.05
  dB and the mode delivered +0.08 — a 40x overstatement, because a bound counts
  ADDRESSABLE error while a real mode also has to pay for the structure in
  precision. Bounds rule things OUT well and rule things IN badly, so run the
  solver before building anything (`EstimateMode7Error` is the pattern: search
  only, no packer, no decoder, no GPU check).
- The UI is a strict CONTROL TREE (docs/ui-hierarchy.md): every widget owns its
  children, and a child's normalized bounds (0..1) resolve against its PARENT's
  ContentRect(), recursively from a window-sized root down — so moving or
  resizing a parent carries every descendant and no authoring site multiplies a
  parent chain out by hand. The walk lives ONCE in Widget (Layout/Update/Draw/
  DrawOverlay are non-virtual); a subclass overrides UpdateSelf/DrawSelf/
  DrawOverlaySelf/LayoutSelf and handles only itself, so a container cannot
  forget its children. Order is fixed: layout self then children; UPDATE
  children in reverse add order BEFORE self (the child owning a pixel claims the
  mouse first — UpdateBeforeChildren is the hook for a parent that needs first
  look, e.g. a slot that highlights as one piece, or a modal); DRAW self then
  children. Containers express themselves through hooks rather than driving
  children: ContentRect (padding, a tab page, a scroll offset), ChildActive
  (culling), ChildClip (clipping, intersected and restored so clips nest).
  ui::ScrollArea owns ALL scroll/thumb/clip behaviour — nothing else may
  re-implement it — and ui::Repeater builds children from a per-frame count with
  a grow-only pool (repeated children hold an INDEX and re-resolve, never a
  pointer into the model). CAVEAT that bit once: a ScrollArea measures overflow
  from its OWN children's bounds, so rows behind a Repeater are invisible to it
  — size the repeater to the stacked height. Bounds may be COMPUTED in
  LayoutSelf rather than authored when a child is aspect- or font-locked (a
  square sized by the parent's height; a row the height of a line advance) —
  still parent-relative, just derived. Screen-anchored popups (ContextMenu,
  InventoryWindow) keep zero bounds and draw in the overlay pass on purpose.
  UNITS are typographic, the CSS model (UI/Units.h): bounds are [0..1] of the
  parent, but the DETAIL inside a control — padding, row heights, a scrollbar's
  width, a thumb's minimum — is in REM, where 1rem = that context's root font
  size (Font::Height; the HUD's 17px, menus' 28px, sheet's 22px, all already
  tracking window height). `Widget::Rem(n)` resolves it from a value captured at
  Layout so even a const rect helper can ask. Detail belongs to the TEXT beside
  it, not to whatever rect contains it — a fraction-of-parent label gap stretches
  when the row is wide. THE ONLY RAW PIXELS ALLOWED are hairlines: the 1px
  borders and the 2px caret (a fractional hairline blurs or vanishes).
  A WIDGET'S AREA IS ITS OWN, and that is a CHECKED rule, not a held one. ROWS
  GO IN A ui::Stack (UI/Layout.h): a site says how much room a row NEEDS
  (Len::Fixed(n) rem / Len::Fill(w)) and never where it goes, so two rows cannot
  overlap — positions are computed in LayoutSelf, the moment the font and
  therefore rem are known, which is what build-time fractions could never see. A
  Stack shrinks its fixed rows rather than overrunning; `fitContent` inverts it
  for the inside of a scrolling page, measuring the rows and writing the extent
  back into `bounds` (what ScrollArea reads) — so any scroll you set must be
  applied AFTER the layout, or it clamps to zero against a height the area does
  not yet know. Editor dialogs get the whole card from game::BuildDialogChrome
  (Game/DialogLayout.h); tab-page rows from game::TabStack. IF YOU ARE WRITING A
  Y COORDINATE OR STEPPING A CURSOR, the layout is about to drift.
  Dev console `uitree` outlines the whole tree by depth and names the chain
  under the cursor; `uitree dump <hud|menu|settings|pause|saves|sheet|confirm>`
  prints it with pixel rects. `uioverlap [label]` AUDITS the rule: it arms a
  two-frame pass over every context that renders — no per-caller wiring, so
  whichever dialog is open is covered — and reports both SIBLINGS whose ink
  intersects and any child that ESCAPES its parent's ContentRect, to the console
  and (labelled) to dungeon.log. Widget::InkRect is what a widget PAINTS as
  against Pixel(), what the layout gave it: Label and Checkbox measure their
  text, so a label wider than its row counts. `overlapOk` opts out the
  deliberately layered; a parent that CLIPS is exempt from the escape check,
  since a scroll area's children are meant to run past it. RUN IT AFTER TOUCHING
  ANY SCREEN — a full sweep (2026-08-08) found four defects nobody had reported,
  three of them placeholder bounds earlier phases had promised to fix. The rule
  it enforces has a second half in INPUT: the pointer is claimed by whoever is
  UNDER it and the wheel by whoever can ACT on it (ConsumeMouse / ConsumeWheel
  are separate; a modal takes both), and input is CLIPPED like drawing, so a
  scrolled-out row is not hot. A widget never claims a pixel it does not paint.
  Fonts track the window height too (Font::SetHeight
  re-bakes the atlas, driven from the top of Game::Update).
  TYPE is addressed by ROLE, never by path (docs/fonts.md; assets/fonts/fonts.cat
  maps Body/Display/Script/Mono → file + an optical `scale`, live-switchable with
  the console `font <role> <name|index|next|prev|off>` / `font scale` / `font
  save`). UI\FontLibrary shares face bytes per path and hands out ONE Font per
  (face, ROUNDED pixel height) — a safety property, not tidiness: Font re-rasters
  every glyph in SetHeight and Commit calls WaitIdle, so two owners sharing a Font
  at different sizes would re-bake each other every frame. FACE AND SIZE ARE TWO
  INHERITED FIELDS on Widget, both optional and both flowing down the subtree like
  CSS: `fontRole` picks the face, `fontScale` the size, resolved in Layout BEFORE
  LayoutSelf. `fontScale` moves `em` but NOT `rem` (rem stays the CONTEXT root),
  so enlarging one label cannot re-space its neighbours. A DrawSelf uses
  `TextFont()`, never `ctx.GetFont()` — the context font is the document root and
  ignores roles BY DESIGN (that is what makes it a stable 1rem), so a widget
  reaching for it silently opts out of inheritance; `ctx.GetFont()` is right only
  OUTSIDE the tree, and a raw draw takes `UIContext::FontAt(role, px)`. Resolve
  sizes off `UIContext::DesignHeight()`, NOT GetFont().Height(): the library
  applies the role's optical scale inside Get, so multiplying an already-scaled
  height applies it twice (they agree for a Body root and diverge for any other).
  Editor dialogs read at `ui::kDialogTitleScale` / `kDialogTextScale` (widgets via
  fontScale, raw draws via the DialogTitleFont / DialogTextFont helpers); numeric
  readouts take NEITHER — sized to their digits, document size, Mono. And MEASURE
  IN THE FACE AND SIZE YOU DRAW IN, or a row will not fit its own contents.
  That rule has a STANDING CONSEQUENCE for the editor dialogs, which author their
  regions as window FRACTIONS: every one of the eight was authored when titles
  drew at 1x, and when the fonts thread took them to kDialogTitleScale nothing
  re-derived a single band — so all eight drew their title down through the row,
  tab strip or preview header beneath it. `ui::kDialogTitleBandH` (0.075 of the
  window height) is that gap DERIVED ONCE — the contexts all size their font
  clamp(h*0.020, 12, 24) and a title's line advance is that x2.9 x1.25 = 0.0725h,
  the clamp only making it easier above h=1200 — and it is the ADVANCE, not the
  ink, that has to clear or the next row sits on the descenders. Place whatever
  follows a title at `kTitle.y + kDialogTitleBandH`; take the title's rect from
  `ui::DialogTitleBand(panel, left, top)`, which also stops it short of the CLOSE
  BOX (the same top-right corner the title line runs toward — "the full inner
  width" silently means "under the close button"); and draw through
  `ui::FitDialogTitle`, which shrinks for height then WIDTH and only ellipsises
  once it has run out of shrink. Shrinking before cutting matters because two of
  those titles carry the object's id AND are the click-to-rename affordance — and
  there the hit-target rect and the draw must ask ONE function for the fitted
  font (TypeEditorDialog::TitleFont / LevelSettingsDialog::TitleFont), since the
  size now depends on the text. The same audit found ProjectileInspector's ROW
  PITCH short for the same reason; a row's advance is 0.020 x 2.0 x 1.25 = 0.050h.
- The clean (non-worn) block set is baked but unused — intended for newer
  dungeon areas, needs per-region block-set selection in DungeonMeshBuilder.
- Texture sets are now installed at 1k/2k/4k with ORM maps, so Low/Medium and
  Ultra use their native resolution (no 2k fallback). The .dds are gitignored,
  so a fresh clone still runs FetchTextures.ps1 to regenerate them.
- Editor (data-driven, see "Project & catalogs" + the MapView section) is built
  out: catalog palette, structural/variant paint, decoration/monster/fixture
  placement, asset-creation dialog with 3D preview + AssetBaker bake, multi-level
  stairs/pits with auto-authored pairs, functional doors, level browsing +
  remote editing of any level, per-level saves, .map/.ent writers, chunk-local
  edit rebuilds (details in the MapView / Project sections above). Right-clicking a door opens the
  DoorInspector (open/closed toggles the live panel + the record's open= param;
  a "Requires key" dropdown lists items.cat entries with category=key — none
  exist yet, so it offers only None — and authors key=, which LOCKS the door
  against the party's click until key items + an inventory check land; wired
  buttons ignore locks; a Name field (ui::TextField, input filtered to
  [A-Za-z0-9_-] — records are whitespace-tokenised, a space would corrupt the
  .ent line) authors name=, the id a button's target= toggles). BUTTONS are
  real props with a brush: a Buttons palette category (buttons.cat, [lever])
  places a record-backed wall lever auto-mounted on the cell's first solid
  wall; it renders at hand height (lever.gltf — origin at the PIVOT, so the
  render's X-tilt flips the handle by `activated`); the party presses it by
  clicking while standing on its cell facing its wall (PressButtonFacing —
  the world-click chain is pick-item → door-ahead → button-facing), toggling
  the doors its target= names; right-click opens the ButtonInspector
  (Target dropdown = the level's door names via DungeonWorld::DoorNames +
  None; a stale wired name stays selectable). The `press <x> <z>` dev
  command still force-toggles one. The former next-steps list is DONE: item
  placement brush (record-backed AddItem/AddItemRemote, one item per quarter
  slot, erase rung included); KEY items (items.cat category=key — iron/brass
  authored — unlock a door whose key= names them via Inventory::Has across
  the roster; not consumed, re-locks when shut, buttons bypass; `give <item>
  [member]` dev command stows one for testing); REAL material sliders (the
  asset dialog persists only the metallic/roughness/height_scale/color
  values the user MOVED as catalog fields — hand-authorable on any model
  entry — replacing the draw factors the shader multiplies over the ORM
  map; multi-material kinds bake them per submesh); Save / To source header
  buttons (Game::SyncProjectToSource shared with the console command);
  snapshot-based UNDO/REDO (one step = copies of every level's
  editor-visible state incl. the SnapshotActive dynamic diffs; restore =
  the level-re-entry flow with the SURFACE REBAKE DEFERRED to editor close
  behind a one-frame "Rebuilding geometry..." notice — GeometryDirty/
  FlushGeometry; </> buttons + Ctrl+Z/Y; drag stroke = one step; history
  clears on level transitions). Instance dialogs: footer = Save (+ Delete
  on the item/decoration dialog — targeted RemoveItemById/
  RemoveDecorationByIndex, undo-bracketed), closing = top-right "x" or Esc.
  DIALOG CLOSE CONVENTION (all of them, editor AND main game): the close
  affordance is the shared box icon (assets/ui/icon_close.png) in the panel's
  TOP-RIGHT CORNER, never a footer "Close"/"Cancel"/"Back" button —
  ui::AddCloseButton(ctx, panelRect, icon, onClose) places it identically
  everywhere (it's a ui::Button with the icon; text "x" is the missing-asset
  fallback). ONE texture serves them all: AssetUtil's CloseIcon(device) loads
  it on the first ask and each dialog borrows a `const gfx::Texture*
  m_closeIcon`; ~Game calls ReleaseSharedIcons() while the device is still
  alive, since a gfx::Texture returns its SRV slot on destruction. Each dialog
  USED to own a unique_ptr and load its own — 15 loads (9 dialog classes + the
  6 InstanceInspector subclasses) and 15 SRV slots for one icon; sharing them
  measured 275 -> 261 live. TRAP that cost the character sheet its icon for
  months: AddCloseButton copies the POINTER, so whoever builds the widget must
  have loaded the icon ALREADY — the sheet is built from Game's ctor
  (BuildStaticUi) while GameUI only loaded it in the LoadTitleArt load task, so
  it captured a null and drew the "x" fallback forever. Load in BuildStaticUi,
  not in a load task. Footer keeps only ACTION buttons
  (Save/Delete/Remove/Animation/?), right-aligned to the panel's inner edge so
  nothing overruns it. Covered: the InstanceInspector base (all 6 per-instance
  inspectors), TypeEditorDialog, AssetDialog, BalanceDialog, MonsterConfigDialog,
  LevelSettingsDialog, ProjectileInspector, InspectPicker, and the character
  sheet (GameUI). NOT touched: Yes/No confirm modals (their explicit choice
  buttons aren't a "Close") and the full-screen menu/settings/save PAGES (Back
  is page navigation, not a dialog dismiss).
