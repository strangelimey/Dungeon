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
- Output: `build\<config>\bin\Dungeon.exe`. Assets copy next to the exe on
  link (post-build); after changing assets without code, sync manually:
  `robocopy assets build\debug\bin\assets /MIR` (MIR also removes stale).
- `gen-vs.cmd` → `build\vs\Dungeon.slnx` (VS 2026 emits .slnx, not .sln).
- Debug builds open a console for logs; DN_ASSERT failures abort() — in
  debug that means a CRT dialog and the process LOOKS alive but is stuck.

## Architecture (docs/ARCHITECTURE.md has the full version)

Nine strictly layered static libs, one-way deps:
Core → Platform/Assets → Animation/Graphics → UI/Audio → Game → Main(exe).
Key conventions (memorize, they bite):
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
  section). One convention carries an invariant no compiler checks: cached
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
  ExecuteImmediate; Texture::RenderTarget calls WaitIdle first).
- Lifetime conventions: ~Game calls AudioEngine::StopAll() because sound
  playback is ZERO-COPY from SoundBank memory and the engine outlives
  Game; preview-mesh resets (dev console `preview`, AssetDialog) WaitIdle
  first since up to kFrameCount-1 in-flight frames still reference the
  buffers; C-API boundaries (cgltf, FILE*, shell COM) are RAII-wrapped —
  keep new ones that way.
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
  BoltSpell/WardSpell forms; behaviour = the Cast() override, reaching the
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
  opposite-quadrant body is flown past. Adding a weapon: items.cat
  damage/speed/stats/reach + `command` (its attack list) + item.<id> lang
  keys ×5; a new attack VERB is a Balance-ctor row + attacks.cat entry +
  GameUI kMeleeUses + use.<verb> keys ×5.
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
monsters/fires/pillar) cull by bounding sphere too. Shadow cubes are CACHED
per slot (ShadowSlotCache): a cube re-renders only when its light changed/
moved (>2cm), a flicker tick is due (fire cubes throttle to half rate via
PointLight::flickerShadow), geometry changed (map Revision), or an animating
caster (monster/pillar) is in range — otherwise the cube stays in its SRV
state and is reused (the per-slot RT/SRV barrier guard makes the skip safe).
DrawMesh skips redundant PSO swaps and, in the shadow pass, the texture-table
binds; skinning palettes upload once per frame (cached by the animator's
buffer, reused across all ~25 submissions).

## Asset pipeline (everything loads from assets/, nothing generated at runtime)

- `AssetBaker <assets>` — regenerates all procedural assets (block models
  incl. worn tiers, monsters, sconce/brazier, pillar, sounds, title art,
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
  at %ProgramFiles%\Blender Foundation\Blender 5.1, or -Blender <path>).
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
  FetchAnimLibrary.ps1 `$animSets` table drives it (Name/Mesh/Library/Ref/MeshYaw,
  archive-relative); raw clips live in OneDrive\DungeonAssets\anim\<library>\.
  Paste the emitted rows into the creature's monsters.cat [id] — or just check
  the boxes in the editor's monster config dialog (it auto-discovers the model's
  clips). Humanoid Mixamo defaults (mesh +90 yaw to co-face the armature, finger
  bones excluded); non-humanoid rigs may need --mesh-yaw/--keep-fingers tuning.
- `AssetBaker mips <assets>` — rebakes derived .dds (BC7 mode-6 encoder in
  tools/AssetBaker/Bc7Encoder.cpp; use the RELEASE baker, encode is slow).
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
  code-bound prop/creature sets (sconce/brazier/pillar/skeleton/mummy/blob,
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
    metal=0).
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
  grid happens to fit that texture's large blocks anyway.

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
art title_bg, MenuList: Continue/Load/Start New Game/Settings — Continue/Load
appear only when a save exists; all entries work) → Playing ⇄ Paused (Esc in-game freezes
the world and shows Save/Load/Settings/Exit/Back over the scene; Esc backs
out / resumes). Esc on the landing page quits; in-game quit is the pause
menu's Exit (Game::QuitRequested polled by the main loop). During the three
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
reN restarts) with clickable halt|run, << / >> (halve/double rate), kill, and
boot (reboot a dead/quarantined slot). Layout lives in one place — Render records
the button rects, next frame's Update hit-tests clicks. Commands: throttle
<scale> (manual governor), governor auto [targetFps] | off (ADAPTIVE — eases all
background cadences when the frame's over budget, asymmetric easing so it
recovers; opt-in, keys off whole-frame time so it's a coarse heuristic, can be
GPU-bound), threadprio/threadaffinity <id> ..., threadspawn/threadwedge (stress
workers — the latter ignores its token, to exercise the hard Kill), threadreap.

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
start cell's accent outline, door bars, stair/pit triangles, and the party as
a rotated triangle — facing*90° CW from north-up; screen Y is down so it
matches the compass; SpriteBatch::DrawTriangle). Editor-only green facing
arrows skip types with catalog `facing_arrow = 0` (monsters: `faces = false`);
the instance inspector's "Map arrow" checkbox beside its Facing dropdown edits
that per type. Visibility goes through
MapView::CellVisible (always true in Editor, else IsSeen). The transform is
resolution-independent (pan = fraction of the grid area, zoom = unitless,
fit-whole-map at zoom 1) and resolves against GridArea, so Update (window-
pixel panel, matches mouse coords) and Render (device-pixel panel) agree;
zoom is cursor-anchored. CellAt is the inverse pick. The left-dock palette is a
catalog-driven collapsible accordion (MapEditor::PaletteCat + the kCategoryInfo
table): Structure (Wall/Floor), Walls/Floors/Ceilings
(per-cell surface VARIANT paint via DungeonMap variant grids — the BLOCK owns
its texture: wall variants live on the SOLID cell, one texture for all four
faces of that block both sides included, floor/ceiling variants on the floor
cell; the brush paints exactly the square clicked, and EditVariant no-ops the
wrong cell type. Stale variant records on the wrong cell type — pre-2026-07-13
files kept wall variants on bordering floor cells — are DROPPED at load), and the entity
categories Decorations/Fixtures/Monsters/Buttons/Doors/Stairs/Items (live
placement). Entries carrying a `category` field group under collapsible
SUB-accordions ("+ Weapon (4)"); every catalog is authored with them.
Items in each category come from the active project's catalogs; a "+ New..." row
opens the asset-creation dialog (see below). Mouse model: LEFT paints/places
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
undo/redo (dimmed when their stack is empty) / Save (savemap) / To source
(synctosource) — built by MapView::ToolbarButtons (ONE item list that
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
  fixtures/monsters/doors/stairs/items), block format: `[id]` headers + `key =
  value` fields naming pool assets (model/texture) + params (solid/authored/
  height_scale/mount). Levels reference catalog ids.
- `levels/<stem>.map` + `.ent` — the level layers. The .map's surface palette is
  a `palette <wall|floor|ceiling> <id>...` record (catalog ids), and it also
  carries `stairs <type> <x> <z> [facing] dest= destx= destz= [destfacing=]`,
  `variant <wall|floor|ceiling> <x> <z> <index>`, and `atmosphere [dust=…]
  [haze=…] [ambient=…]` (per-level mood knobs, authored by the editor's Level
  settings dialog) records. (The old `assets/maps/level1.*` with `textures`
  records is dead — superseded by the project copies.)

Serialize.* is the block (de)serialization primitive (free Find/Get/GetFloat/
GetBool/Set over a Field vector; Block + CatalogEntry both delegate). Catalog.*
adds CatalogGet/CatalogBool (null-safe). Project.* loads/saves the manifest +
catalogs and maps a key→Catalog (CatalogForKey). DungeonWorld resolves catalog ids
to model+texture at load (ModelAndTexture helper). Game owns the active Project,
passes it to DungeonWorld; MapView reads it for the palette. NOTE: editor/asset/
level WRITES go to the asset copy next to the exe (paths::Asset), not the git
source — `synctosource` (or paths::RepoAssetsDir, compiled via DN_REPO_ASSETS)
pushes them back. Full per-phase history + gotchas live in the editor-overhaul
memory.

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
  load-or-dies on derived/imported assets that are gitignored. There are THREE,
  not one (a missing-texture magenta-placeholder fallback exists, but there is NO
  fallback for models — a missing `.glb` aborts hard at level load, e.g.
  `AssetUtil.cpp model.has_value()` "failed to parse glTF: ...viking_dagger.glb").
  Copy all three from a populated sibling worktree (e.g. `C:\Dev\Dungeon`), then
  MIRROR into `build\<cfg>\bin\assets` too (the post-build asset copy only runs on
  a link):
  - `assets\textures` — whole dir, BOTH `.dds` (BC7) and source `.png` (dds-only
    still renders magenta; ~273 dds + ~261 png).
  - `assets\models\*.glb` — the 5 imported authored meshes (viking_dagger,
    french_dagger, khukri, snake_dagger, leather_armor) — PLUS the bought rigged
    monsters, gitignored BY NAME despite the `.gltf` extension (centipede.gltf,
    giant_spider.gltf — embedded-texture GLBs inside; see .gitignore). Committed
    `.gltf` models come with the checkout; only those files need copying.
  - then `robocopy <new>\assets <new>\build\<cfg>\bin\assets /MIR`.
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
  medieval iron, brazier=bronze, pillar=peacock-ore, skeleton=carved limestone
  (bone), mummy=stained burlap, blob=alien-slime. ModelBaker gives the box-
  built props world-aligned tiling UVs (TileUvs); the glTF baseColor stays as
  the flat fallback if a set is missing. Bought authored decoration meshes
  (boulder/mossy_rock/pot) ride the import-model path like ancient_pot.
- BC7 encoder is mode-6 only (slight banding possible on smooth gradients).
- UI widget bounds are normalized (0..1 of container, see UI/Widget.h) and
  resolve against the live window each frame, so the HUD scales on resize.
  Fonts track the window height too (Font::SetHeight re-bakes the atlas,
  driven from the top of Game::Update).
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
