# Dungeon — project context for Claude

Old-school grid dungeon crawler (Grimrock / Dungeon Master style), C++23 +
DirectX 12, owned by Michael (GitHub: strangelimey/Dungeon, private repo).
Built collaboratively with Claude across sessions; this file is the handoff.

## Build & run

- `build.cmd [debug|release]` — VS 2026 Community's bundled CMake + Ninja
  (plain `cmake` is NOT on PATH; the script sets up vcvars64). The
  "'vswhere.exe' is not recognized" warning it prints is harmless.
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
  heap (docs/ARCHITECTURE.md "Memory strategy").
- Constants that must match HLSL: kMaxPointLights=16, kMaxSkinJoints=128,
  root signature layout in Renderer.h header comment.
- The Game lib is split by category: Game.cpp is just the app state machine
  + wiring; GameSettings (ini round-trip, quality tier, the kThemeFields/
  kBarFields/kKeyFields tables), SoundBank, LoadQueue (staged loading),
  DungeonWorld (world state, simulation, both render passes), GameUI (all
  five UIContexts: menus, settings page, HUD, sheet, overlays), AssetUtil
  (load-or-die helpers). World→log feedback flows through
  DungeonWorld::onMessage; UI→state-machine actions through GameUI's on*
  callbacks, both wired in the Game constructor.
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

Forward pass: Blinn-Phong with per-material specular (MaterialParams),
normal + steep-parallax mapping (derivative cotangent frame, height in
normal-map alpha), per-cell volumetric dust (turbidity grid texture t2,
raymarched extinction + in-scattering), point-light cube shadows with
distance-graded slots (4 slots, 512/256/256/128, slot 0 = nearest light,
PCF; carried torch always wins slot 0; dust march samples the same cubes →
god rays), fire light positions wander so shadows flicker. Shaders compile
at launch with an on-disk cache (shadercache/, hash-invalidated) — edit
.hlsl and relaunch, no rebuild.

## Asset pipeline (everything loads from assets/, nothing generated at runtime)

- `AssetBaker <assets>` — regenerates all procedural assets (block models
  incl. worn tiers, monsters, sconce/brazier, pillar, sounds, title art,
  party portraits) and ends with a mip bake.
- `AssetBaker import <folder> <assets> <name> [--flip-green]` — packs a
  downloaded PBR set (auto-detects maps by filename; flips GL normals;
  AO→albedo; height→normal alpha; bakes BC7 DDS).
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
  with their textures). `tools\FetchTextures.ps1` imports ONLY the
  materials the maps' `textures` records reference (override: -Materials
  list, -All for everything — slow, hundreds of BC7 bakes; -Resolutions
  1k,2k,4k). A full pre-history-rewrite git bundle also lives there.
- Maps are two files per level, split static vs dynamic for the future
  save system (saves will only ever store the dynamic side):
  - assets/maps/level1.map — STATIC layer (DungeonMap): ASCII grid, ';'
    comments, glyphs '#' rock '.' floor 'D' dusty 'T' sconce 'F' brazier
    (blocks movement) 'P' start. Lines starting lowercase are records
    (grid glyphs are never lowercase): `textures <wall|floor|ceiling>
    <set> ...` declares the level's surface palette — MANDATORY, the game
    loads only those sets + their worn meshes, order = variant index —
    plus decoration records.
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
ui::TabControl
(pages scroll: children authored past the page bottom — bounds fraction > 1 —
trigger a per-tab scrollbar, wheel or thumb drag, page-scissored):
quality dropdown on Video (Low/Medium/High/Ultra: mesh tier low/med/high/high
+ textures 1k/1k/2k/4k), master-volume slider on Audio, party-bar sliders on
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
dungeon::KeyName (Platform/Input) renders vkey names.
Game tab hosts the Language dropdown (see the Core/Loc bullet above).
All persist to settings.ini next to exe (quality=0..3, language=<code>,
volume=0..1, barscale, baropacity, theme_<name>= and bar_<name>=r,g,b,a,
key_<action>=vkey; sliders save on release, pickers when their popup closes,
key binds and language immediately). Quality hot-swaps in place (WaitIdle +
rebuild); Ultra falls back per-material to 2k with a warning if 4k not
installed.

## Game state machine

Loading (staged tasks, one per frame, progress screen) → Menu (baked title
art title_bg, MenuList: Continue/Start New Game/Load/Settings — only
Start New Game and Settings work) → Playing ⇄ Paused (Esc in-game freezes
the world and shows Save/Load/Settings/Exit/Back over the scene; Esc backs
out / resumes). Esc on the landing page quits; in-game quit is the pause
menu's Exit (Game::QuitRequested polled by the main loop). Monsters
block movement (no combat yet); fires are sconces at 'T' (wall-mounted,
light at flame) and braziers at 'F', each with FireEffect particles
(flame/spark/smoke via gfx::ParticleBatch premultiplied billboards) and
fire-driven turbidity rings around them.

The HUD's top bar shows the 4-member party (Character.h roster, widgets in
PartyHud.h: portrait, name, health/stamina/mana bars); clicking a portrait
freezes the world (AppState::CharacterSheet, like Paused) and opens the
character details page (prev/next cycle members, Esc/Back resumes). Widgets
hold pointers into Game::m_characters, so the roster is filled once and
StartNewGame resets members in place. Left column under the bar: the
facing/position panel, then the Options panel (torchlight dropdown,
Wait/Help). Right edge: a Dungeon Master-style control panel — six movement
arrow buttons (turn/forward over strafe/back; GameUI::onMoveAction →
Party::Act(MoveAction), the same discrete actions the bound keys map to in
HandleInput), a left+right HandSlot (PartyHud.h) pair per member (empty
boxes with the character's identity stripe; clicking logs "hands are empty"
until items exist), and a reserved Magic area below.

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
  title, no tools. The `M` key is hardcoded (kKeyFields is MoveKeys-only; a
  bindable map key needs a separate UI-keybinds table).
- Editor mode (dev console: `editor` opens/flips into it, `editor off` returns
  to Player without disturbing the view; reachable in all builds): the
  dungeon-builder. FULL-SCREEN and drawn alone — Game skips the shadow/scene
  passes and the HUD while it is up (editorMap flag in Render), so nothing
  renders behind it. The WHOLE map and EVERY creature/item draw regardless of
  fog. A brush palette docks down the LEFT side (MapView::DockRect); the map
  grid lives in the remaining GridArea (panel minus dock) so it never draws
  under the dock. SetMode flips an open map's mode in place; Open(mode) resets
  the view to fit-whole-map.

MapView (Game lib) is the one renderer + one pick math behind both modes.
Cells render as filled blocks (walls = bright stone ink, floors recede),
fixtures/entities as colored square markers (torch/brazier from the map;
monster/item/button from DungeonEntities — repoint at live runtime state once
monsters move and projectiles exist; Editor is meant to show all of them, in
flight or not), the start cell as an accent outline, and the party as a
rotated triangle (facing*90° CW from north-up; screen Y is down so it matches
the compass — uses SpriteBatch::DrawTriangle). Visibility goes through
MapView::CellVisible (always true in Editor, else IsSeen). The transform is
resolution-independent (pan = fraction of the grid area, zoom = unitless,
fit-whole-map at zoom 1) and resolves against GridArea, so Update (window-
pixel panel, matches mouse coords) and Render (device-pixel panel) agree;
zoom is cursor-anchored. CellAt is the inverse pick. The left-dock brush
palette (MapView::Brush — Floor/Wall now; doors/creatures/items are the next
entries, extend the enum + kBrushCount) proves the edit seam: a brush is
always armed in Editor, left paints, right-drag pans. A paint click →
DungeonWorld::EditCell → DungeonMap::SetCell (bumps Revision()) → MarkSeen →
RebuildGeometry (WaitIdle + re-bake, same as the quality hot-swap) → the wall
appears in-world immediately. Edits are in-memory only (no .map/.ent
serialization yet). All overlay text goes through Loc (map.* keys).

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
editor icons) — the axis-aligned DrawRect/DrawSprite couldn't express them.

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
- NEVER rewrite UTF-8 files via PowerShell Get-Content/Set-Content — it
  mojibakes em-dashes (happened twice). Use the Write/Edit tools.
- User prefs: concise replies, no emojis; permission prompts disabled.

## Known gaps / natural next steps

- Continue/Load (landing) and Save/Load (pause menu) entries are inert
  (save system not started).
- No combat: monsters are static blockers that announce + face the party.
  Character stats (Character.h) are static placeholder data — nothing
  drains health/stamina/mana yet. Portraits are simple baked busts
  (AssetBaker portraits); the tinted-initial fallback still draws if the
  textures are missing.
- Monster models are box-rigs; authored glTF would drop in via LoadModel
  (JOINTS_0 remap already handled).
- BC7 encoder is mode-6 only (slight banding possible on smooth gradients).
- UI widget bounds are normalized (0..1 of container, see UI/Widget.h) and
  resolve against the live window each frame, so the HUD scales on resize.
  Fonts track the window height too (Font::SetHeight re-bakes the atlas,
  driven from the top of Game::Update).
- The clean (non-worn) block set is baked but unused — intended for newer
  dungeon areas, needs per-region block-set selection in DungeonMeshBuilder.
- Roughness maps from imports are ignored (specular is per-material).
