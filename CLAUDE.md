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
  EXE (`log::FilePath()`, via paths::ExecutableName), so the tools that also link
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
- Left-handed, +Y up, camera forward = (sin yaw, 0, cos yaw); the world
  compass is east = +X, north = -Z, and Camera::ViewProj negates clip X so the
  view is not mirrored (it matches the map overlay). Facing index +1 is
  CLOCKWISE = the on-screen RIGHT (TurnRight, StrafeRight, the right-hand
  roster column's lane), +3 the left, and the yaw FALLS as the facing climbs.
  `Game/Facing.h` (facing::Right / Left / StepX / StepZ / Yaw / SlotSide, and
  ForAction - what each MoveAction does, the whole of Party::BeginAction's
  decision; MoveAction is declared there) is the one statement of it -
  Party.cpp, Entity.cpp's DirDX/DirDZ/DirYaw, the cast / throw / held-light
  lanes and PickMeleeVictim's flank files all go through it - and RollTest's
  "Facing" section pins it against the game camera (controls were once
  reversed, and this line once said LEFT).
- All indentation is TABS (see .editorconfig). Comments use file banners +
  section dividers; keep that style.
- Per-frame GPU transients come from UploadAllocator arenas (one per frame
  in flight, kFrameCount=3); steady-state frames allocate nothing on the
  heap (docs/ARCHITECTURE.md "Memory strategy"). An arena is a FIXED size and
  running out asserts - EXCEPT the UI's: SpriteBatch's 8 MB is sized for the
  editor map of the largest generated level (generate::kMaxSide = 128, a quad
  a square at fit zoom; MapView.cpp static_asserts the fit), and a frame past
  it DROPS the batch and counts it (`sprites`, the console's UI gauge beside
  SRV; EditorTest phase 19, code-review C163). A full allocation audit
  (2026-07-03) verified the rule and closed its last violations (formation
  scratch, flat AI-snapshot grids, shared icon light rig — see the AI
  section). The rule is now CHECKED, not just held (docs/ARCHITECTURE.md
  "Checking the rule"): Core/AllocTrack replaces the global ::operator new
  family and counts per THREAD (lock-free, constant-initialized slot; Debug,
  or -DDN_TRACK_ALLOCS=ON in Release). Main brackets each frame,
  Game::SteadyStateFrame arms it (Playing, no console/overlay/load/deferred
  rebuild/running eval script, 120-frame warm-up; a scripted run is a console
  session, so the eval harness is NOT an allocation check - AllocTest is; and a
  frame that ENDS outside Playing/sheet-over-level - Esc to pause, a stair load -
  is a transition, disarmed at the end of Game::Update, `AllocTest.ps1 -Pause`),
  and a violating frame's call stacks are symbolized into dungeon.log once per
  unique site (capture starts in ArmFrame, so the first armed frame after a
  disarm - alloctest's first - is covered; 64 sites at most, then one "set is
  full" line and a count of the CAPTURES turned away - not of sites, since a
  repeating one counts each time). Dev: `alloctest [secs]` (one machine-readable
  verdict line), `allocguard [status|strict on|off|reset]`, `allocpoke
  [secs|once]` (violate on purpose); `tools\AllocTest.ps1` is the re-runnable
  regression run (`-SelfTest` pokes once on the window's first armed frame and
  inverts the verdict, so the harness must catch a real violation AND find its
  call site to pass). THE EVENT-FRAME EXEMPTION IS GONE
  (2026-08-18, docs/message-allocation.md): a bump message used to allocate —
  loc::Tr returned a COPY of text the table already owned, and MessageLog kept
  a std::string per line — and that was written up here as a POLICY, "allocation
  proportional to events isn't what the rule forbids". It was a rationalisation
  of a defect, and its real cost was that a guard which fires during ordinary
  play teaches you to ignore it. Printing a message now allocates nothing
  (loc::View / loc::ViewKey / loc::FormatLine + a fixed MessageLog ring), so the
  rule needs no notion of an event and no exception list: an allocation in a
  settled frame is a bug, full stop. Something firing events every frame is a
  MESSAGE-RATE problem, visible in the log on its own terms. ONE POLICY is left:
  anything reporting from inside a guarded frame must excuse ITSELF - and
  log::Write and its templates do that for every caller (code-review C215), so
  what a reporter still excuses by hand is only what it builds BEFORE the call
  (a formatted argument, a console Print; `AllocTest.ps1 -OnHitTypo` swings a
  typo'd on_hit in the window). Staged loading is measured too - LoadQueue
  times/counts every task and dumps a table when the last lands (`loadstats`
  reprints; each Add takes an English dev name beside its localized label),
  and LoadGltf reports allocs/MB per model. TRAP when reading those numbers:
  DEBUG allocation counts are NOT release ones - MSVC's iterator debugging
  allocates a container PROXY for every std::vector / std::string it
  constructs, the MOVE constructor included. That move is still `noexcept`
  (the proxy is allocated inside a noexcept body), so a growing vector of
  vectors MOVES its elements in debug exactly as in release; the proxies are
  the whole debug-vs-release gap, and intrinsic to the debug CRT. Keep
  reserving the clip vectors (LoadGltf's `model.clips.reserve` and each
  clip's pools): a regrow moves every element, and every move costs debug a
  proxy. A `static_assert(std::is_nothrow_move_constructible_v<
  AnimationClipData>)` beside that reserve makes a member that would turn
  growth into COPIES a compile error. A clip's keys are POOLED
  (AnimationClipData::times / values; a channel is two ranges into them and
  owns no buffer), which took a rigged model's debug load from ~24k
  allocations to under 900 (skel_warrior 24,351 -> 875).
  One convention carries an invariant no compiler checks: cached
  UIContext widget pointers die on Clear(), so any callback that triggers
  a page rebuild must DEFER it a frame (the m_pendingLanguage /
  m_videoRebuildPending pattern). The party roster is resize-safe: the
  HUD/sheet widgets address Game::m_characters by (vector, index) and
  RE-RESOLVE the member every Update/Draw (PartyHudTypes.h RosterMember), so a
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
  buffers; C-API boundaries (cgltf, FILE*, stb / dr_wav buffers, XAudio2,
  shell COM) are RAII-wrapped from the moment they are created (code-review
  C231: an allocation between create and free used to leak them on a throw) -
  keep new ones that way. The MAIN THREAD MUST STAY STA-CAPABLE: never
  CoInitializeEx it into the MTA (AudioEngine's ctor used to, for XAudio2,
  which needs no COM since 2.8). A thread's apartment is fixed once joined,
  so an MTA main thread makes every shell dialog return RPC_E_CHANGED_MODE
  and DEADLOCK — the editor's "Browse Folder..." wedged the process with no
  window ever shown. Platform/FileDialog now refuses (logged) rather than
  hanging if it ever happens again; running the picker on a private STA
  thread does NOT help, since Show() messages the owner window.
- PATHS ARE UTF-8, every one (code-review C384). Core/Paths builds them with
  str::Narrow - never path::string(), which went through CP1252 and THREW on a
  character it could not hold, from crash::Install before any handler was up -
  and the file dialog, the command line and typed text are UTF-8 too. Every
  EXE carries src/Core/Utf8CodePage.manifest (activeCodePage UTF-8; the root
  CMakeLists adds it to each executable target it finds, so a new tool cannot
  miss it), which makes fopen, std::ifstream, std::filesystem, stb, cgltf and
  dr_wav read a char* as UTF-8. So a narrow path needs no conversion; only a
  WIDE Win32 call takes str::Widen. The game warns at boot if its code page is
  not 65001. Checked by `tools\PathsTest.ps1` (CheckAll full tier): the
  manifest read back out of every exe, then the game (headless, an eval) and an
  AssetBaker import and import-model run from `%TEMP%\dn-paths-<pid>-<a name in
  Latin-1, Cyrillic, CJK and U+10348>`; `-SelfTest` strips the code page from
  those copies and every case must fail FOR THAT REASON: the game exits 2 (its
  eval script did not open - so it got past crash::Install, where a
  path::string() in Core/Paths would have thrown and aborted) and the imports
  exit 1. Dev: `readfile <path>`, the answer of assets::ReadBinaryFile (a
  directory is an error, never a buffer).
- Constants that must match HLSL: kMaxPointLights=64 (the point-light array
  CEILING = the Ultra tier; the per-frame count is a runtime budget,
  GameSettings::maxPointLights, Low=16..Ultra=64 — see the quality system),
  kMaxSkinJoints=128, root signature layout in Renderer.h header comment.
- The Game lib is split by category: Game.cpp is the app state machine
  (Update / UpdateStates / Render) plus construction, the world's lifetime
  (LoadWorld / UnloadWorld), the staged-load task lists, new game / save /
  load and arming the allocation guard - NOT "just the state machine"; the
  callback wiring is Game_Wiring.cpp and each other Game_*.cpp is one
  subsystem hanging off it (editor create/bake, eval, generate, party, the
  world tier, the dev-command tables). Game.h's banner lists the AppStates
  and the rules they keep. Beside it: GameSettings (ini round-trip, quality
  tier, the kThemeFields/kKeyFields tables), SoundBank, LoadQueue (staged loading),
  DungeonWorld (world state, simulation, both render passes), GameUI (all
  seven of its UIContexts - HUD, landing menu, settings page, pause menu,
  saves (save/load, the world list, party creation), character sheet, Yes/No
  confirm - plus the item-details and portrait-picker dialogs'), AssetUtil
  (load-or-die helpers). World->log feedback flows through
  DungeonWorld::onMessage, wired by WireWorldCallbacks each time LoadWorld
  builds a world; UI->state-machine actions through GameUI's on* callbacks,
  wired once from the constructor (WireModuleCallbacks).
- MAGIC (full model: docs/magic system.md + spells.md + skills.md): every
  spell is a CLASS in src/Game/Spell/ (one file pair per spell; Spell base →
  BoltSpell/WardSpell/SightSpell/LightSpell forms — the shared tier-2 form runes
  Project/Protect/Sight/Light; behaviour = the Cast() override, reaching the
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
  THREE TIERS (spell-updates, docs/spell-updates-plan.md): a recipe is SCHOOL,
  then an optional FORM, then at most ONE MODIFIER - `Spells.h` TierOf /
  SymbolMayFollow / WellFormedRecipe are the one statement of that grammar
  (no modifier on a bare school rune or on Sight). Runes are SHOWN by their
  Futhark names (`RuneNameKey` -> `rune.<id>`: Kenaz Berkano Ansuz Laguz /
  Tiwaz Algiz Dagaz Sowilo / Ingwaz Hagalaz); the ids stay the meanings. TIER 1 is
  four `HandSpell`s, NOT bolts: Flame lights a held torch / the wall torch /
  a brazier past `brazier_power`; Rock conjures a pebble into a hand or at the
  feet; Gust flares a fire and past `push_power` shoves a monster and REPELS a
  shot (weakened by the power, flung back if the power beats it - its blast and
  on-hit effects keep the share of its strength it keeps, `ProjectilePayload::
  Scale`, and one left with nothing falls with no expiry, so it never bursts:
  code-review C18); Splash fills
  a held skin a step / douses the wall torch / a brazier past its power. Every
  threshold reads CAST POWER. Every hand spell is SEEN on every cast, whatever
  it found to act on: the `handPuff` cast service (DungeonWorld::HandPuff,
  DungeonWorld_Ahead.cpp) draws its element just ahead of the caster's lane -
  a flame puff, a dust cloud dropping grit, a breath of air that drifts away
  down the facing and swirls apart (Puff's `drift` / `swirl`), a splash of
  droplets that fall (ProjectileSystem::Splash) - plus a brief shadowless
  glow from a fixed 4-slot `m_handGlows` (a cast is a guarded frame). Dev:
  `castsvc puff [school]`. TIER 2 Project = single-target bolts (`firebolt`
  `earthbolt` `waterbolt` `airbolt`; were fireburst/slingshot/push). TIER 3 is
  ONE class, `ModifiedSpell`, which AllSpells wraps round every Bolt, Ward and
  Light spell (the lights' pair: see THE LIGHT FORM below): Ingwaz = a volley (each bolt weaker, the caster's own lane, a fixed
  pending-bolt queue via `spawnBoltAfter` - a cast frame must not allocate) or
  the ward on the whole party; Hagalaz = a burst on impact or a burst round the
  caster sparing its square, and no ward. Twenty-four whole spells with their own
  ids and spells.cat entries, so learning / the book / saves needed nothing.
  A modified spell's DEFAULTS are its form's AS TUNED: SpellBook::Build reads
  the forms' entries, then `Spell::DeriveFromForm` (power, 2x mana, on_hit,
  push), then the modified entry - and its bolts carry ITS on_hit and push
  (code-review C19: `[firebolt_volley] on_hit` was read by nothing).
  TRAP: `blast_force` counts SQUARES, not a radius. Monsters cast any spell id;
  the mage ladder is skel_mage / skel_mage_adept / skel_magus (bolt, volley,
  burst). A monster's burst bolt that reaches a member's lane GOES OFF in the
  party's square (ResolveMonsterProjectileHit; code-review C1) unless the Wind
  Ward on the member it would strike turns it - a turned bolt does not land
  (`fx::Deflect`, stage 1 alone) - and `threat` prices it by that blast, on
  every member, unrolled. New spell services reach the world only through
  CastServices (each drivable bare with `castsvc`). Checked by `tools\SpellTest.py` (judges
  spells.eval, CheckAll quick; `--selftest` cuts every cast), `tools\CombatTest.py`
  (the burst on the party, the ward and the gust, the flare's reach, a ward's
  school by hand), `AllocTest.ps1 -Hand` and
  `AllocTest.ps1 -Burst` (the burst, the ward and every repel outcome inside a
  guarded window, fired by `autocast bolt ... [repel <power> <member>]`).
  THE LIGHT FORM (lighting-updates Phase 6): a fourth form rune, SOWILO
  (`SpellSymbol::Light`, APPENDED - bit 9 of knownSymbols, old saves unchanged;
  `SpellIdList` is 64 now - it was exactly full at 32, so a 33rd spell could never
  be learned). `Spell/LightSpell` + Firelight / Tidelight / Skylight /
  Stonelight land ONE effect kind, `light` (Effect/LightEffect, knobs on
  effects.cat [light]), on the CASTER with the school on the instance - schools
  stack, a recast replaces its own - and the world reads it every frame
  (DungeonWorld_SpellLight.cpp): a `LightKind::Spell` light per (member, school)
  from lights.cat `spell_<school>`, sized by the cast power. Each school DOES
  something: fire kindles fires within a step and scorches monsters beside the
  party; water clears a bubble in the haze (a NEGATIVE dust puff - the shader's
  DustDensity clamps at zero), doubles stamina regen and quenches the party;
  air shocks the nearest monster in reach and sight and flickers faster while
  the party is noticed; EARTH is not carried but SET DOWN - a stone in the
  cast's square (`placeLightStone`; a fixed `m_lightStones` of 8 a level, saved
  as `lightstone` lines in its LevelState) that maps every square its light
  reaches in walking steps and shows MONSTER TRACKS there. TRACKS: `m_tracks`,
  one cell per square sized with the fog mask (FitTracksToMap - a NEW site that
  resizes m_seen must call it), written in `StepMonsterTo`, fading over
  balance.cat `track_life`, saved per level as AGES on a `tracks` line; each
  records its MAKER so the party can leave tracks / scent / noise later.
  Ingwaz on a light = one bigger light (`grow` x the power); Hagalaz = a FLARE
  (the `lightFlare` service: a flash, `dazzle` on monsters within 3 steps - a
  dazzled monster skips its turn - and the school's light acting once, on what
  THIS flare dazzled). A light's REACH is one walk, `DungeonWorld::WalkReach`:
  walking steps into OPEN squares (OpenSquare, a blast's test - no rock, no
  shut door), for the flare's dazzle and kindling and a stone's mapping and
  tracks (code-review C17). An effect's expiry line is its KIND's (effects.cat
  `fade_party` / `fade_monster`), never picked by category (C9); `messages`
  prints the HUD log a world line goes to. With
  it: every skeleton resists fire 0.75, and a monsters.cat `flammable` monster
  (the mummy) catches from ANY fire that lands on it - the one seam is
  `MonsterTarget::Wound` (balance.cat `ignite_burn` / `ignite_seconds`). Dev:
  `lightstones [clear]`, `tracks [clear | add <x> <z> <dir>]`; checked by
  `AllocTest.ps1 -Light` (refuses a PASS without the flare's dazzle or the
  stone's track motes). For now each caster STARTS with a Sowilo tablet in the
  pack (CreateDefaultParty); none is placed in a level yet.
- FIRE AND LIGHT (docs/torches-and-fire.md): there is NO light at the eye - a
  LIT TORCH held in a hand (or on the cursor) is the party's light, and an
  ambient-0 level is pitch black. A lit torch burns while HELD (its CHARGE
  counts down `burn_time`, it dims over its last tenth, spent it becomes
  `spent_as`); stowed in a pack it goes out keeping what is left, but ON THE
  FLOOR IT STAYS LIT (lighting-updates Phase 4, Michael: thrown or set down) -
  it burns on there (`TickFloorTorches`; an authored record's torch becomes a
  drop the first time it burns, so the save carries kind and charge), lights
  its square and burns with a small flame from a fixed pool (`m_torchFlames`,
  shared with a torch in FLIGHT, whose flame trails behind it). The hand menu
  offers PUT OUT for a lit torch and LIGHT for a MAGICAL one (its lit kind has a
  `power_level`): Flame passes a magical torch over (`refusesFlame`, a cast
  service) and Light costs balance.cat `torch_light_mana` per level. WORN LIGHT
  (Phase 5): any item on the doll or in a hand whose kind names a `light` and
  does not burn (the moonstone amulet) gives it steadily at its member's side
  (`LightKind::Worn`; `AllocTest -Wear <item>`). A torch is
  also a CLUB (`command = attack`, the `attack` verb: bash, blunt skill, STR);
  a LIT one is `element = fire`, so its fire scales with tier and skill, plus
  an `on_hit` burn. A MAGICAL torch (`torch_magic`) adds `power_level` (burn_time
  x (1 + level), folded in at load) and `flame_color` (its light AND the drawn
  flame: icon, details dialog; ItemKind::flameColor, DrawFlame's tint) - and
  what it SETS ALIGHT burns that colour too: fx::Inst carries an optional
  `tint` (ApplyProcs' last arg; FlameTintOf on a swing, the payload's `tint` on
  a throw), which BurnTintFor / BurnGlow read before the school's, and the
  enteffect / brkeffect lines save as an 8th token. A wall
  BRACKET REMEMBERS its torch: `WallSconce::torch` (unlit id, "" = the fixture's
  own) + `torchCharge`, set by MountTorchAt, read by TakeTorchAt, reset with the
  flips, saved as 6th/7th tokens of the `fire` line. A SMASHED bracket drops
  that torch on its square (DouseFixture -> DropItemInCell) and is left bare. CHARGE IS
  PART OF THE ITEM everywhere it can be: `ItemSlot {typeId, charge}`, the
  cursor's HeldItem, a floor Item, a thrown cargo, and the save (`id#charge`).
  Lighting / dousing / filling RENAMES an item in its slot (`lit_as` /
  `unlit_as` / `fill_as` / `drink_as`). Fires are LIVE, SAVED state: a play
  change is a FLIP on the map's fixture (`flipped`, a sconce's `empty` - its
  torch taken), `SetFireBurning` the one way to change it, `fire` save lines
  the diff. A doused fire SMOKES through the effects system (`on_douse` ->
  `smoke` haze effect -> up to 4 `dustPuffs` in the frame). TWO RULES THAT
  BITE: the STASHED static map keeps only the AUTHORED fires (StashStaticMap
  resets the flips; it once carried a douse into a new game), and a play-time
  fire change must not bump `DungeonMap::Revision()` (`RecomputeTurbidity`) -
  the revision keys the AI walkability grid, which then rebuilt (allocating) on
  every hand spell. Dev: `torch`, `castsvc`, `equip none`.
  WHAT A LIGHT LOOKS LIKE IS DATA (lighting-updates, docs/lighting-updates-
  plan.md): every light is pushed through `DungeonWorld::PushLight` from a
  named PROFILE in the project's `lights.cat` (Game/LightProfile.h, pure, in
  RollTest: colour or `source`, intensity, radius in SQUARES, pulse steady/
  flicker/breathe/strobe/storm + rate/depth, origin `wander`, shadow,
  long_fade). A source NAMES one: fixtures.cat / items.cat `light`, effects.cat
  `light` on a plume. There is no global torch palette any more (the HUD
  Options panel is gone). A placed fire keeps its own reach (Brightness) and
  may carry its own FLAME COLOR (`color=r,g,b` on the .map fixture record, the
  fixture dialog's row): it colours the light AND the flames (FireEffect::
  SetFlameColor). Dev: `lights` / `lights profiles` / `lights reload`.
  THINGS IN FLIGHT (Phase 4, DungeonWorld_Flight.cpp): EVERY launch goes through
  `DungeonWorld::Launch`, which DRESSES the spec - its light profile and its
  TRAIL from, most particular first, the spell's own spells.cat `light`/`trail`,
  the cargo kind's items.cat `light`/`trail`, the school's `bolt_<school>` /
  `trail_<school>`, else `bolt_shot`/`trail_shot` for a monster's plain shot.
  Each flight is ITS OWN light keyed by its projectile id, on from launch to
  landing (a volley's bolts each light their own stretch; a bolt still queued in
  `m_pendingBolts` makes none - it is dressed when queued, since its names are
  borrowed from a spell a reload could replace), and a lit bolt's end leaves a
  0.3 s FLASH. Trails are `trails.cat` (Game/Trail.h, pure, in RollTest: shape
  spark/ember/mote/puff/drip, `rate` per SQUARE flown - shed by distance, thinning
  with distance from the eye and none within half a square of it). They share
  the projectile spark pool, which NEVER GROWS: full, it recycles its oldest
  trail particle, and only with none left does a particle go without, so a hit
  always reads. Dev: `trails [reload]`; checked by AllocTest `-Cast`/`-Impact`/
  `-Throw` and `-Throw -ThrowItem torch_lit`.
- COMBAT (full model: docs/combat.md — "The attack formula"; built by the
  combat-depth thread): every constant is a KNOB in the project's
  balance.cat ([formula] block → the Balance struct in Game/Balance.h, whose
  knobs and DEFAULTS are its base BalanceKnobs in the pure Game/BalanceKnobs.h -
  RollTest, StrikeRules and defense::StanceRules read kBalanceDefaults rather
  than retyping them, code-review C423; kBalanceFields drives load/save AND the
  dialog rows) and attacks.cat
  (per-attack numbers; IDENTITY — attack id + damage type — is the typed
  C++ table in Balance's ctor, the spells.cat pattern). Both edit LIVE in
  the editor map's Balance header-button dialog (Formula/Attacks tabs, "?"
  explains the columns; Save writes the catalogs). DAMAGE TYPES ARE DATA, the
  project's damagetypes.cat (Combat.h's DamageTypeBook; a type is an opaque
  index, kMaxDamageTypes = 16 a ceiling, not a count): eight ship -
  slash/pierce/bash (`physical = 1`), the four elements (`school = ...`) and
  `starve` (an empty meter's bite: no school, not physical, so nothing
  resists it unless it says so). Spells type by school
  (BoltSpell::MakeBolt), monster melee by `dmgtype`. Attack side: damage =
  (weapon damage, or the unarmed knobs, + stat_damage × avg of items.cat
  `stats`) × attack numbers × (1 + skill_damage × level); ACCURACY IS
  ALWAYS DEX. Punch and Kick (Balance's `UnarmedAttacks`, which the hand menu
  reads too) and a hand holding NO weapon (an item with no `damage` of its own:
  a key, a tablet - `DungeonWorld::HandWeapon`, the same `damage > 0` test a
  throw and the details dialog make) swing, train and parry `unarmed`, with no
  hand powers; a weapon naming no `skill` swings as itself and trains `unarmed`
  (`WeaponSkill`; the editor's "+ New..." weapon is one) (code-review C39).
  Defender side: evasion, then max(0, rolled - soak) x (1 -
  resist[type]) (`defense::Mitigate`, ONE rule for both of fx::Deal's branches:
  soak never inverts, only a resist past 1 heals), a blow that got through
  floored at wound_floor - resists SUM nature (monsters.cat `resists`; a
  member's Character::natureResists is its race's races.cat `resists`, set by
  Game::ApplyRaceResists) + worn equipment
  (`resists`/`armor`; WORN = the doll, never the hands, one walk for every
  defensive sum: `DungeonWorld::ForEachWornPiece`) + Stone Skin as physical,
  clamped ±resist_clamp (a nature cell of 1.0 = immunity). A bolt the Wind
  Ward turns was never rolled and trains nothing; a piercing crit trains no
  armour (`defense::SoakMet`). Checked by `tools\CombatTest.py`
  (combat.eval). Resources DERIVE - see RESOURCES below,
  which superseded the old flat `max = base + k × statAvg`. STAMINA is the
  exertion meter (swings spend
  (stamina_swing + stamina_weight×kg) × the attack's stamina column, steps
  spend stamina_step, every spend trains CONDITIONING — the old `vit_exertion`
  VIT creep is GONE, see RESOURCES; an empty bar latches EXHAUSTED penalties
  with hysteresis). 0 HP =
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
  damage/speed/skill/stats/reach + `command` (its attack list) + item.<id> AND
  item.<id>.desc lang keys ×5 (the .desc is the details dialog's paragraph -
  ANY new item needs one; armor -> armor.cat with armor/resists; runes/keys/food/etc ->
  items.cat — see the editor palette section for the three-catalog item split);
  a new attack VERB is a Balance-ctor row + attacks.cat entry +
  GameUI kMeleeUses + use.<verb> keys ×5. ENCHANTED weapons: weapons.cat
  `element = fire` + `element_bonus` — a landed blow adds elemental damage
  through the target's resist for that element (no soak, no separate to-hit
  roll), and the element becomes the FLAVOUR its on-hit effects arrive with.
  NOT after a blow that killed (`ev.slew`), swung or thrown: the burst used to
  wound the corpse and count the kill twice (code-review C5). MonsterTarget::
  Wound refuses the dead, and the blow that KILLS earns no threat and provokes
  nothing - a corpse turns on nobody. A SEVERE FUMBLE's `drop` / `fling` puts
  the held item down AS IT WAS - its kind's id, its charge (a part-burnt torch
  is not relit full), nothing copied in the swing's frame (code-review C10).
  Dev: `equip <item> [member] [hand]`, `fumble severe|plain [member] [hand]`
  (the LOADED DIE, Harness::loadedFumble: the next such swing fumbles without a
  roll - a severe face is ~1 swing in 100), `autoattack hold` (released by
  alloctest's first armed frame); tally `severefumbles= fumbledrops=`. Checked
  by AllocTest -Swing and tools\CombatTest.py's FUMBLE checks.
- RESOURCES (full model: docs/health-and-healing.md; all of it built - the
  pools, food/water, rest, pace and the sheet; the balance pass is what is
  left). Every pool has an APTITUDE
  (a stat — `Character::Aptitude`, the ONE home of health←VIT,
  stamina←(STR+VIT)/2, mana←(INT+WIL)/2) and a PRACTICE (a skill:
  `conditioning`/`attunement`/`constitution`). max = base + k_<r>×aptitude +
  Curve(practice); rate = <r>_regen + <r>_regen_stat×Curve(aptitude) +
  <r>_regen_max×max + Curve(practice). The arithmetic is the PURE TU
  `Game/Resource.h` (Curve.h only, so RollTest links it — the Defense.h
  bargain); `Balance::Resources()` is the adapter, `Character::RecomputeMaxima`
  takes a `resource::PoolRules`. **"Constitution" is a SKILL, not a sixth
  stat** — which is why this needed NO save bump (skillXp rides v15).
  THREE RULES THAT BITE: (1) the practices train and CREEP NOTHING — they all
  go through `GrantResourceXp`, whose whole job is passing an empty stat list,
  because each feeds a pool its aptitude also feeds and the ordinary award
  closes the loop (`vit_exertion` was DELETED, not zeroed); (2) a
  `<r>_skill_cap` of 0 switches that term OFF — `CurveValue` reads a
  non-positive cap as an UNBOUNDED straight line, the armor-floor bug's exact
  shape, so `resource::SkillTerm` owns the guard; (3) health regen now runs
  EVERY FRAME while hurt, so `GrantSkillXp` looks a skill up before inserting
  it (the subscript's `std::string` was a steady-state allocation).
  State gate: the 1.5s `staminaHoldoff` IS the "exerting" signal — stamina 0,
  mana ×`mana_exert`, health 0. Dev: `regen` (all three rates + the ordering,
  read on its `ref` rows NOT the members — the party is unequal on purpose),
  and `char` prints the creep pools so a leak is visible. Checked by
  `RollTest`, `AllocTest.ps1 -Wounded` (a fresh party never runs regen at all)
  and the `resources` eval suite.
  SUPPLIES: `food`/`water` per character (0..`food_max`, save v25 `supply` line;
  a pre-v25 save loads FULL). They drain by TIME — scaled by conditioning, which
  is the design's only downward pressure — and by EXERTION off `SpendStamina`,
  water heavier than food (sweat). Eating is `nutrition`/`hydration` on any
  items.cat entry, and `eat`/`drink` are ONE handler because an apple does both;
  `GameUI::onConsume` → `DungeonWorld::ConsumeItem`, which refuses (keeping the
  item) when it would restore nothing (POTIONS go through the same handler -
  see "Transparency and potions"). AN EMPTY METER KILLS: `starving`/`parched`
  are ordinary DoT effects dealing a `starve` damage type nothing resists, so a
  Tick on a downed member is lethal under the existing overkill rule and NO new
  death path exists. **The effect is the METER'S SHADOW** — `TickSupplies` tops
  its `timeLeft` up while empty and erases it when fed, so there is no
  "permanent effect" concept and exactly one place a member stops starving
  (`ConsumeItem` deliberately does not lift it). Dev: `supplies` (in HOURS LEFT,
  since a meter reading means nothing without its rate), `setsupply`, `consume`,
  and `setpool` (health/stamina/mana set outright; health 0 = unconscious, the
  state AllocTest -Rest needs, and a health write rebases the ledger like `heal`).
  REST is a STATE (the Rest button beside the log's Log button - it was on the
  HUD Options panel, gone in lighting-updates; dev `rest [on|off]`, bare =
  REPORT not toggle; `rest button` = where that button is, for a harness). It
  multiplies TIME at ONE place - `Game::WorldDt`, the frame's `wdt` - so every
  rate, timer and cooldown accelerates together and no second set of resting
  rates can drift. THE WORLD RUNS THAT TIME IN FIXED TICKS (code-review C64): a
  world dt over `DungeonWorld::kMaxWholeStep` (0.1 s - no ordinary frame, which
  Core/Time clamps there, and no `step` tick) runs as `kTick` = 1/60 s ticks,
  the remainder carried, at most `kMaxTicksPerUpdate` (90) a frame with the
  rest DROPPED (Michael: a slow frame rests slightly slower; 90 holds 60x down
  to 40 fps), stopping early when the rest ends, the level changes or the party
  is wiped (`AdvanceSimulation` / `Tick` / `PresentFrame`, which draws once a
  frame). One step of a whole second had every timer act at most once in it: a
  fast AI bucket thought once where awake it thinks four times, a skeleton
  walked one square where it walks two. Flights are ALSO stepped at most half a
  square at a time whatever the dt (`ProjectileSystem::Update`, C48), so a long
  step cannot carry a shot over the party or through a one-thick wall. **It
  forces LOCKSTEP AI while resting** and hands the previous mode
  back: the AI buckets are WALL-CLOCK paced, so a 60x world would give a monster
  1/60th the thinking per simulated second and it would chase stale paths — the
  harness's own "stale orders are worse than frozen" lesson. Ends three ways
  (`RestEndReason()`): `recovered` (nothing left to gain; a DOWNED member is not
  full, so the rest goes on until they come round on the stabilize clock - only
  the DEAD are left out), `attacked` (a blow — a DoT does NOT break it,
  `WoundMember`'s `quiet` flag is exactly that line), `hungry` (an empty meter).
  Transient: not saved. NOTE `step` advances SIM seconds, so it cannot see the
  multiplier at all - rest.eval measures the STATE's rules with it. `frames <n>
  [fps] [whole]` runs PLAY frames through `WorldDt` instead (the dev timescale
  left out), so it sees the multiplier: tools\AITest.py's restpace.eval checks a
  resting chaser walks and thinks a simulated second as an awake one, and with
  `whole` (each frame ONE step, the pre-C64 world - `Harness::wholeSteps`) that
  a 60x shot still lands on the resting party (`shot <x> <z> <dir>`, a plain
  monster shot from no monster) and stops in a one-thick wall (tally `expat=`).
  PACE: conditioning ADDS to a member's authored `moveSpeed` (class identity,
  like baseHealth) through `pace_slope`/`pace_cap`; the party still moves at its
  SLOWEST member's, so training the fastest buys nothing. `DungeonWorld::
  ApplyPartyPace` owns the rule (Game only forwards) because conditioning levels
  inside the combat tick where Game is not in the call chain. SHEET: the Skills
  tab groups under Training / Reserves headings (`SkillRow::header`), and the
  Stats tab draws five bars — the three pools plus food and water, framed like
  the party bar's (see RESOURCE BARS below). Dev: `sheet <member|off>`, which also let the
  sheet JOIN `/check-ingame`'s uioverlap sweep (it was the one screen no audit
  reached; note that sweep sees WIDGETS, and the bars are direct draws).
  TRAP: any dev command that seeds a SKILL must re-derive — `setskill` wrote xp
  and nothing else, which since P1 leaves a conditioned member with a novice's
  bars and the old walking pace. Same lesson `setstat` learned.
  THE HARNESS RECYCLES THE WORLD rather than reloading it: `-eval a b c` runs
  many scripts in ONE process, and `reset` puts the world where `newgame` would
  in ~340 ms against a ~12 s level load (the load is models/textures, cached
  after the first). Ten suites: 155 s → 38 s. `reset` is a directive a SCRIPT
  calls — a specific test opens with it, a PROGRESSION series deliberately does
  not and inherits the last one's party. Its definition is "where a new game
  would leave it" BECAUSE that can be tested: `Eval.ps1 -SelfTest` diffs a
  baseline taken after a real load against one after a reset, and also checks a
  batched suite matches a solo one. TRAPS, all found the hard way: the first
  reset skipped the MAP and the equivalence test passed anyway (nothing it
  printed showed geometry — `mapinfo`'s walkable count is now the only readout
  that can see an arena); `ReadEvalLines` APPENDS, so a batch re-ran every
  earlier script and merely took longer; and `ResetForEval` must go through
  `m_ui.onStartNewGame`, never `StartNewGame()` (P2's cold-boot HUD crash).
  MEASURED by the `expedition` eval suite (fight → retreat → `rest until` →
  repeat): three full recover cycles cost 2.9 food / 5.2 water of 100, so a load
  buys ~55 fights — and the party dies to DIFFICULTY long first. Supplies are
  not the constraint at the shipped numbers; that is the balance pass's starting
  point. TRAP when scripting rest: `rest on` then `step N` does NOT measure a
  rest — the auto-stop is dt-independent so it fires on the next ordinary frame,
  which at timescale 0 falls BETWEEN the two commands; use `rest until`.
  WHAT THE HARNESS COSTS THE SHIPPING CODE (audited 2026-08-15, docs/eval-
  harness.md "What the harness costs"): ALL harness state the world holds is ONE
  member, `DungeonWorld::m_harness` (`struct Harness`: tally / autoAttack +
  autoAttackHeld / loadedFumble / frozen + frozenHeld / pendingSteps / autoCast /
  wholeSteps), touched in a handful of places in the simulation (autoCast is
  `TickAutoCast`, the `autocast` round-robin that puts a LAUNCH inside a guarded
  window - see AllocTest.ps1 -Impact - and, as `autocast bolt`, a shot AT the
  party and a repel of an exact power - -Burst; `freeze hold` keeps monsters from
  even noticing the party until an `alloctest` window opens, so -Melee's first
  notice and first blow land inside it, and `autoattack hold` does the same for
  the party's own swings - -Swing, -OnHitTypo; `fumble` loads the next party
  swing to fumble without a roll, PartyAttack's one branch; wholeSteps is
  `frames ... whole`, a frame's dt taken as ONE step), each reading
  `m_harness.x` so it says what it is; `ResetForEval` is `m_harness = {}`. The
  script runner is its own TU, `Game_Eval.cpp`. Headless is one branch in Main.
  NOT harness machinery despite appearances: lockstep AI (SetResting uses it —
  rest runs the world at 60x and lockstep makes the fast-forward honest), the dev
  console (~130 commands; allocguard/crashpoke/uioverlap predate eval), and the
  damage ledger (a shipping rule check). NONE OF IT IS BEHIND `#ifdef`, and that
  is a decision: the harness's value is that it measures the SHIPPING binary
  (RollTest's rule — the real thing linked in, never a copy), and a fourth build
  configuration would rot the way `build-release` and `build-profile` checks
  exist to prevent.
  HEADLESS: `Dungeon.exe -headless -eval …` (also `Eval.ps1 -Headless`,
  `PipelineTest.ps1 -Headless`) hides the window and skips the whole render half
  of the frame. It is NOT mainly a speed switch — ten suites go 42 s → 37 s,
  because the cost is the asset load plus `step` loops, and a `step` runs many
  simulated seconds inside ONE frame, so there are few frames to save. What it
  buys is a run that steals no focus and survives RDP / a scheduled task, and a
  debug one opens no log console either (Main reads the flag before its
  AllocConsole). "Several at a time" means one PER WORKTREE: a game writes ONE
  dungeon.log beside its exe, truncated on open, so two runs of one build
  interleave each other's verdict source - Eval.ps1, PipelineTest and the Python
  judges refuse to start beside their own worktree's game (tools\HarnessGame.ps1
  / harness_game.py, exit 3), and a run COUNTS only if it finished (exit 0, or
  its `eval BATCH RESULT=` line; a killed run is a failure, not a short log).
  It does NOT remove the graphics device: the swapchain is
  bound to an HWND, so the window still exists and is merely never shown - and
  NEVER means never: a hidden Window (`IsHidden`) is only resized by a mode
  change, and Game skips applying a saved Borderless / Exclusive mode, which
  used to show the window, black out a monitor or switch the display on every
  run (code-review C391; Eval.ps1 -SelfTest watches IsWindowVisible) - and
  prising the device out would mean a null path at every gfx call site for no
  gain (a GPU-less machine is already covered — GraphicsDevice falls back to
  WARP). THE ONE THING THAT MADE IT NON-TRIVIAL: `Game::Render` is pure drawing
  EXCEPT its last line, `++m_framesRendered`, which `RunLoadTasks` gates on ("the
  screen has been presented once") — skip rendering naively and the load queue
  never advances, so the run sits on the loading screen until the 600 s script
  timeout. `Game::EndHeadlessFrame` does that bookkeeping instead; the normal
  path's increment is left where it is. EQUIVALENCE IS CHECKED, not assumed:
  `Eval.ps1 -SelfTest` runs a suite both ways and demands the console output
  match line for line. NOT run headless on purpose: `/check-ingame` (uioverlap
  measures what widgets PAINT) and `/check-alloc` (the guard brackets update AND
  render, so a headless frame is a different frame from the one the rule is
  about). A headless run is also UNATTENDED (`crash::SetUnattended`: a fatal
  error records, dumps and EXITS - no CRT abort box waits); `-unattended` gives
  a run that draws the same, and `harness_game.run_eval` passes it on every run,
  windowed ones included (EditorTest phase 20's swatch check draws). `-eval`
  does not imply it: a developer watching a script may want the box.
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
  of that. `fx::Apply` owns the stacking rule, and the CEILING: a list holds
  `fx::kMaxEffects` (24), reserved when its owner is made, and a full one
  EVICTS rather than grows. The most a member can carry in play (16: four
  wards, three DoTs, two supply, a sight per school, a light per school but
  earth) is counted beside the kind table in AllEffects.cpp from kSchoolCount
  and resource::Supply, under a static_assert - A NEW KIND A MEMBER CAN CARRY
  ADDS ITS TERM THERE, since a kind registered without one leaves it green.
  The backstop needs no term: `EffectBook::Build` bounds EVERY registered kind
  by its stacking (Refresh 1, school kSchoolCount, stack unbounded; 19 today)
  and a class past the ceiling stops the load (DN_ASSERT), an effects.cat
  `stacking` override past it warns. The sheet's Effects rows warm to the
  ceiling. EVERYTHING IS RESISTED - the
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
  THE ONE-PIPELINE RULE IS CHECKED, not held (docs/effects.md "The invariant,
  CHECKED"): `Game/DamageLedger.h` gives every health value the pipeline can
  reach a BASELINE, anything allowed to move it declares itself through a
  `ledger::Explained` scope naming a `Reason`, and checkpoints (one a frame,
  three a world tick) demand the arithmetic come out - a leftover is a write that went around
  `fx::Deal`. SIX sanctioned reasons: `pipeline` (the adapters), `exertion`
  (over-exertion's health half, a DECLARED exception — not resisted, soaked or
  warded, because collapsing under your own effort is not something armour
  turns), `regen`, `growth` (a stat or resource-practice level raises a ceiling
  and carries the current value up — this one fires MID-FIGHT, and the check
  found it), `stabilize`, `drink` (a potion). Wholesale replacement (load / respawn / `heal`)
  REBASES instead of crediting, which is why there is deliberately no `setup`
  reason. IT IS A RUNTIME CHECK BECAUSE A GREP CANNOT WORK: regeneration writes
  health through a lambda taking `float&`, so the identifier never appears on
  the assignment and a source scan reports that file clean. It names the PHASE,
  not the line — the stack is long gone by the checkpoint, and making health a
  guarded type would ripple through save/load, the UI and all of combat. Dev:
  `pipeline`, `pipelineguard [on|off|strict|reset]`, `pipelinepoke`; armed by
  default in debug, off in release. Harness: `tools\PipelineTest.ps1` /
  `/check-pipeline`, in CheckAll's QUICK tier (16 s — the eval `reset` recycles
  the world instead of reloading it). IT DEMANDS MORE THAN PASS: the app must
  still be `playing` and EVERY route must have moved health, because the suite's
  own first run wiped the party on a T-junction blast and then printed a
  confident PASS for four sections that simulated nothing. The ledger itself is
  a PURE TU linked into RollTest (the Defense.h rule), and its checks are
  non-vacuous by MUTATION — one of which passed clean at first because the test
  covered the rule but not the branch.
- ALL user-facing text goes through Core/Loc (loc::Tr(key) /
  loc::Format(key, args...) for {} placeholders), loaded from
  assets/lang/<code>.lang (UTF-8 key=value, ';' comments; en.lang is the
  reference — add new strings there). Missing keys render as the key
  itself (visible, never fatal); a missing language file falls back to
  en.lang. Dev-facing text (log::, DN_ASSERT, asset names, ini keys) stays
  English. Dynamic ids map to keys by convention: monster.<ent type>,
  race.* (races.cat `name`; there are NO classes), facing.* (Party::FacingName returns the
  key). Settings → Game has a Language dropdown (loc::ScanLanguages; each
  file self-names via lang.name); switching saves language=<code>, reloads
  strings, and rebuilds every page next frame (GameUI::RebuildForLanguage —
  deferred via Game::m_pendingLanguage because the rebuild destroys the
  dropdown; an in-game switch clears the HUD message log). ui::Font bakes
  Latin-1 (32..255) and Draw/MeasureWidth decode UTF-8, so Western European
  scripts work out of the box; other scripts need a wider bake range.
  A FACE MUST ALSO HAVE THE SCRIPT: a .lang file names its own face for a role
  that lacks it - `lang.font.<role> = <file under assets/>` + optional
  `.scale` (Game::ApplyLanguageFonts; docs/fonts.md Phase 6). ru.lang draws the
  Script role in Alegreya, since IM Fell English has no Cyrillic, and the
  Display role in Forum (assets/fonts/Forum), since Cinzel has none either.

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
the nearest shadow-casting light, PCF - with one carried light that is it, but
a torch AND a Firelight compete by distance, held by 0.75 m of hysteresis
matched on the light's id; dust march samples the
same cubes → god rays; kShadowSlots sizes the C++ side, scene.hlsl mirrors
the registers BY HAND), fire light positions wander so shadows flicker.
KNOW THIS about "missing" shadows: in a fire-dense room the dust in-scatter
(every fire feeds a turbidity ring) washes surface shadows to ~invisible —
that's a turbidity/ambient tuning matter, not a shadow bug (A/B: console
`shadows off` barely changes such a room; `dust off` transforms it). Shaders compile
at launch with an on-disk cache (shadercache/, hash-invalidated) — edit
.hlsl and relaunch, no rebuild.

THE LIGHT BUDGET (lighting-updates Phase 3, DungeonWorld_LightBudget.cpp +
Graphics/LightTiles.h): every source PUSHES a candidate light (PushLight, with a
stable `id` = kind << 24 | index) into a list capped at 256 (kLightCandidates):
past it PushLight REFUSES the light - in push ORDER, not by distance - and
returns null, as it does for brightness 0, so a caller that adjusts the light it
got back must check (the fire loop did not, code-review C181; `lights` prints
the refusals by source). SelectLights decides what is drawn - drop a
light whose sphere reaches no pixel of the view (the renderer's own LightTiler,
so cull and shader agree), drop one in a square the party cannot walk to (a
BFS from its square; such a light only bled through walls), RANK the rest by
intensity x r^2 / (r^2 + d^2) with a bonus for last frame's keepers, keep the
top Max Lights (a held torch always), and FADE a light crossing that line over
a quarter second (a light merely off screen keeps its fade). The scene shader
then loops only over its TILE'S lights: 32 x 18 NDC tiles, a 64-bit mask each
in the frame constants (`gTileGrid`, `gLightTiles`, LAST in the cbuffer so
shadow.hlsl's shorter copy still lines up; LIGHT_TILE_COUNT mirrors
kLightTileCount BY HAND), and the dust march reuses the pixel's mask because
every sample on its ray projects to that pixel. KNOW THIS before expecting a
big win from tiles: a sphere that contains the eye reaches every pixel, and in
2.5 m squares with 7.5-15 m reaches most nearby lights do - the tiles save on
distant lights, and Max Lights is still the real control (measured in the
plan). The camera updates BEFORE the lights each frame for the cull. Shadow
cubes cache by light id, not list index. Dev: `lights [profiles|reload]`,
`lightstress <n> [near] | fill` (fill: 256 test lights pushed AHEAD of the
fires, so every fire meets a full list), `lighttiles on|off`; checked by RollTest
(a light is never missing from a tile it reaches) and `AllocTest -Lights` (which
also survives `lightstress fill` with a fire refused, and runs floorglow.eval: an
enchanted blade's floor glow is its element's colour, code-review C190).
TRANSPARENCY (glass and the liquid in it) is a QUEUED pass: a draw whose
material is `transparent` is not issued but queued, sorted far to near and
FLUSHED at the end of the pass - see "Transparency and potions" below for the
rules, which bite.

Per-frame efficiency (DungeonWorld + Renderer): surface geometry is split
into spatial chunks (DungeonMeshBuilder GeometryChunk, kChunkCells=4, each
with an AABB + texture variant), so the main pass frustum-culls off-screen
chunks (DungeonWorld::ViewCull, Gribb-Hartmann from Camera::ViewProj) and
each shadow cube sphere-culls out-of-range chunks; discrete meshes (props/
monsters/fires) cull by bounding sphere too. Shadow cubes are CACHED
per slot (Game/ShadowScheduler): a cube re-renders only when a new light holds
the slot, geometry changed (map Revision, or any editor edit), a CASTER in its
reach changed, the light moved (> 2 cm; a wandering one past what its wander
alone could do, PointLight::wander - so a carried Firelight keeps up with a
walk), or a flicker tick is due (a wandering fire's cube, on a 25 Hz WALL-CLOCK
cadence, at most 2 such a frame - `shadowrate`) - otherwise the cube stays in
its SRV state and is reused (the per-slot RT/SRV barrier guard makes the skip
safe). A caster changes two ways (code-review C178): what moves every frame is
the world's verdict (MovingCasterNear: a monster where its body is DRAWN - its
visualPos, not its cell, which jumps a square when a step starts - and a thrown
item in flight, each in the draw's own cull sphere), and
what changes ONCE is NOTED (DungeonWorld::Note*Caster -> a fixed list of
spheres, overflow = every cube): a door leaf each frame it travels, a lever
thrown, a floor item lifted / set down / burnt out, a body gone, a smashed prop,
a wall torch taken. A NEW way for something the shadow pass draws to change
must note it, or its old shadow stays until the light moves. Dev `shadows
status` (each slot's light, re-renders by reason, the pass of its last render
and its light's LAG - how far it stood from its cube's pose while the cube was
reused, in squares) and `shadows door <x> <z>` (the first pass to draw a door's
current pose, stamped by the travel itself, never by the note); checked by
`AllocTest -Lights` (a door opened beside a still party must re-render slot 0
up to the pose it LANDS in and then stop; a Firelight must not re-render
standing still, must re-render walking, and must lag its cube by at most a
tenth of a square; `-ShadowSelfTest` mutates the cache and needs both to fail).
DrawMesh skips redundant PSO swaps and, in the shadow pass, the texture-table
binds; skinning palettes upload once per frame (cached by the animator's
buffer, reused across all ~25 submissions). The cache key is that buffer's
ADDRESS for the whole frame, so a palette handed to DrawMesh must OUTLIVE the
frame - an Animator that lives on, never a temporary: a freed one hands its
address to the next of the same size, which then draws in the first one's
pose (code-review C189 - the map-icon bake's throwaway animators drew eight of
sixteen kinds in another kind's pose; each kind now owns its icon pose).

## Asset pipeline (everything loads from assets/, nothing generated at runtime)

- `AssetBaker <assets>` - the full bake, in this order (tools/AssetBaker/
  Main.cpp): BakeTextures (TextureBaker's procedural stone sets - bare
  `<name>.png` / `_n.png` with no resolution tag, which nothing loads any
  more, since LoadPbrSet asks for `<name>_<res>`), BakeSounds, BakeModels (the
  clean surface blocks and the worn ones at 3 tiers, the wall features,
  sconce/brazier, the procedural props, stairs/pit shafts, door parts and the
  procedural monsters), BakeRunes (the tablet + carved per-element sets), then
  BakeAllMips over assets/textures and BakeModelImageMips (the embedded-image
  sidecars). Title art (assets/ui/title_bg.png) and the party portraits are
  NOT baked: the one is committed art, the others bought (see below).
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
  importer's leftovers). The bake grounds but does NOT centre XZ: the four
  skeleton-kit rigs rest ~(0.34, 0.43) units off their origin. The GAME centres
  every monster model on its rig root's rest XZ at draw time
  (`DungeonWorld::MonsterModelWorld`, `MonsterKind::rigRest`; a load logs any
  rig resting > 0.05 off), and a burn plume rides that root joint's live pose
  (`BurnOrigin`) - so a new draw of a monster model goes through that helper.
  The editor previews take the same point as `gfx::ModelPreview::Render`'s
  `pivot` (`SkeletonData::RootRest` is the one statement of it), and a monster
  preview draws in METRES (`MonsterPreviewData::scale` = kUnit x modelscale,
  capped to fit the pane) - the preview camera frames metres, models are units.
  The kit's clips also carry ROOT MOTION (a walk moves the hips ~0.77 units a
  cycle, a run ~1.3, a death up to 0.6), which slid a walking skeleton ahead of
  its square and snapped it back every loop. The bake keeps it; the PLAYER
  removes it: `Animator::LockRootTravel` (every monster animator and both
  editor previews) subtracts a LOOP's straight-line drift so the cycle closes
  with its sway intact, and scales a ONE-SHOT's travel so the root ends within
  `DungeonWorld::kMonsterRootReach` (0.08) of where it began - a body still
  falls the way it falls, inside its own square. Height is never touched. Every
  monster Animator - a spawn, a kind's map-icon pose, both editor previews, the
  asset picker's tile and preview - comes from `DungeonWorld::MonsterAnimator`,
  which calls it (code-review C395: the rule was each site's to remember, and
  the picker's looping preview had not). Measured by the `rootmotion` eval suite
  (`monsters` prints each one's clip and `root` stray). PLAY'S CONTRACT:
  re-Playing the active clip is a no-op while it loops OR is still fading in (a
  mid-fade re-Play used to restart the fade, so a per-frame Play never finished
  one); an empty name plays nothing - there is no "first clip" default. Checked
  by `AnimTest --contract` (CheckAll `anim`).
  Paste the emitted rows into the creature's monsters.cat [id] — or just check
  the boxes in the editor's monster config dialog (it auto-discovers the model's
  clips). Humanoid Mixamo defaults (mesh +90 yaw to co-face the armature, finger
  bones excluded); non-humanoid rigs may need --mesh-yaw/--keep-fingers tuning.
- `AssetBaker mips <assets>` — rebakes derived .dds (BC7 encoder in
  tools/AssetBaker/Bc7Encoder.cpp; use the RELEASE baker), for the texture sets
  AND for every image EMBEDDED in a model: `<model file>.<index>.dds` beside it
  (skel_warrior.gltf.3.dds; assets::EmbeddedImageSidecar names it, the index is
  ModelData::images order). The game's model loaders load those instead of
  decoding the PNG/JPEG inside a bought .glb (2k PNG decode + CPU mips was ~50 ms
  an image; skel_warrior's six cost ~320 ms of a level change, now ~50). A sidecar
  OLDER than its model is stale: the game WARNS and decodes, so re-run
  `AssetBaker model-images <assets>` (sidecars only, current ones skipped) after
  importing or re-converting a model. Tools load models WITHOUT the option
  (LoadOptions::bakedImages) because they want the real images. The encoder trials
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
  quality claim is void. Quality is gated by `tools\bc7-baseline.txt`, which the
  loader READ AS EMPTY until 2026-10-05 (its '#' header ended a `>>` loop), so
  the gate had never fired; a baseline matching nothing now FAILs, and so does
  ANY gap between the synthetic images and the `syn.*` rows (that corpus is the
  same on every machine, so a gap is a lost row, never the pool), while
  real-texture rows that do not match (the sample follows the installed pool)
  are only listed. `-SelfTest` corrupts a copy of the bytes AND
  raises the baseline 1 dB, and requires exactly the consistency and quality
  checks to fail - the quality one on every matched image.
  TRAP when reading its numbers: aggregate PSNR by the MEAN of per-image PSNR,
  never by pooling squared error — pooling is dominated by whichever tile
  compresses worst (the noise tile sits ~1000x higher in MSE than a smooth one),
  and it hid a knob worth 1.35 dB on brick behind an average of +0.01 dB.
- `AssetBaker models <assets>` — rebakes only the .gltf models (fast). Worn
  blocks sample the installed texture height maps, so rerun after
  FetchTextures.ps1 or a texture import. `--out <dir>` (also on `wornblock`)
  writes the models elsewhere, and `AssetBaker wornsets` lists the worn-set
  records (see the worn-block bullet). FetchTextures, FetchModels and
  ReplayImports call the baker through ONE wrapper, `tools\Pipeline.ps1`
  (`Find-AssetBaker`, `Invoke-Baker` - exit code only, so a stderr warning
  cannot abort a batch under 'Stop').
- PARTY PORTRAITS are BOUGHT, not baked (portraits branch, docs/portraits-
  plan.md; the old SDF-bust PortraitBaker is long gone). Two packs, both
  AI-made, in docs/costs.md: Magory (2386 shipped) and Corax Digital Art (493;
  its license REQUIRES a credit and forbids redistributing the files). They live
  in `assets/portraits/<id>.png` + `.dds`, GITIGNORED except the starter party's
  four, and `tools\FetchPortraits.ps1` extracts them from the zips archived in
  OneDrive\DungeonAssets\ui\<pack>\ and bakes the BC7 chains (`AssetBaker
  portrait-mips`, which skips a current .dds; `mips` covers them too). The
  CATALOG decides what ships: `assets/portraits/portraits.cat` (committed,
  GENERATED by `tools\BuildPortraitCatalog.py` from each pack's `tags.tsv`
  beside its zips - edit a table and re-run) holds every id with source / race /
  sex / age / look, tagged by eye; the fetch extracts exactly those ids. Left out
  there: full-body figures (a face too small for a party slot) and the Corax
  pack's six byte-identical duplicates. The fetch also SQUARES every image (91 of
  Magory's are a few pixels off, two are tall crops): crop to the short side,
  centred across but anchored at the TOP so a face keeps its hair, then scale -
  every slot is square and BC7 needs sides that are a multiple of 4. TRAP it
  hit: zip extraction keeps the entry's OLD timestamp, which made a re-extracted
  image look older than its .dds, so the skip-current bake skipped it - the
  fetch stamps each file it writes. Its last check demands a .dds per image,
  because a missing one still renders (the PNG fallback) and so hides.
  IN THE GAME a member's portrait is an ID (`Character::portraitId`; the default
  party's four are set in CreateDefaultParty) saved as a per-member `portrait
  <i> <id>` line - absent in an older save, which keeps the default, so no
  version bump. `Game::SyncPortraits` is the ONE loader: it reloads only a slot
  whose id differs from what is loaded (draining the GPU first - the SRV rule)
  and re-points `Character::portrait`, so every path that changes an id (new
  game via ResetRoster, LoadGame, `Game::SetPortrait`) just calls it.
  SetPortrait and LoadGame refuse an id portraits.cat does not list. Dev:
  `portrait [member] [id]`; checked by `tools/EvalScripts/portraits.eval`.
  THE PICKER (Game/PortraitPicker.*): a grid of thumbnails filtered by race /
  sex / age, opened by the sheet's "Change portrait" button (under the name),
  STANDALONE (Open(title, currentId, onPick) - the party creation page opens
  it too, filtered to the member's race, on the title screen), owned and
  routed by GameUI exactly like ItemDetailsDialog (built
  once, updated instead of the page under it, DismissPopup closes it first, the
  mouse is its while open). Two rules worth knowing: the GRID IS ONE WIDGET
  (PortraitGrid sizes its bounds to every row so the ScrollArea scrolls right,
  but draws and hit-tests only the rows in view - a widget per tile, the
  AssetPicker's way, does not scale to thousands), and the thumbnails go
  through `Game/ThumbCache.h`, the ONE copy of the load-a-few-a-frame / mark-seen-
  in-draw / LRU-evict-to-a-low-water-mark / drain-before-free rules, which the
  AssetPicker uses too. Thumbnails load LINEAR (`LoadTextureThumb`'s srgb=false)
  to match the party bar. ALLOCATION: an open picker is not a quiet frame
  (SteadyStateFrame), and `Game::OpenPortraitPicker` excuses the opening click's
  frame (OverlayOpenedThisFrame) - open through it, never GameUI directly. Dev:
  `portrait picker [member|off|status]` / `filter <race|any> <sex|any> <age|any>`
  / `scroll <0..1>` (status prints the SRV gauge: a full scroll peaks ~568, and
  closing must return to where it was), and `assetpicker textures|models|off|
  status` for the cache's other client. InGameTest sweeps it (`sweep_portraits`)
  and demands the `portrait picker: open for` log line, or the audit was of the
  sheet beneath.
- Textures: PNG = source, .dds = derived BC7 mip chains (gitignored).
  The game loads the .dds and falls back to the PNG. TRAP, and why a rejected
  .dds now WARNS (TryLoadTextureFile): from 2026-06-11 to 2026-09-28 the DDS
  reader read the pixel-format fields 4 bytes late and rejected EVERY baked
  file, and the silent PNG fallback made the game look fine - so the BC7
  pipeline never reached the screen (uncompressed RGBA8 in VRAM, mips built on
  the CPU, ~80 ms per surface set). Fixing it took the release game load from
  2.2 s to 0.5 s. A fallback that hides its own firing is how this survived.
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
  for everything - slow, hundreds of BC7 bakes; -Resolutions 1k,2k,4k). Both
  it and FetchModels.ps1 take -WhatIf (list what would import, bake nothing),
  and their usage lines are in the `powershell -Command "& { ... }"` form, since
  a comma list through -File imports nothing (see SortTextureDownloads below);
  `tools\UsageLinesTest.ps1` dry-runs every such line and fails one that stops
  selecting the sets or models it names, and fails ANY tools\*.ps1 comment line
  invoking a script through `powershell -File` with a comma list or through
  `-Command` in a shape it cannot run - a line the judge cannot read is a FAIL,
  not a skip (`-SelfTest` runs each line as written as a CONTROL, then plants
  the -File form, a name the archive lacks, and bad shapes). A full
  pre-history-rewrite git bundle also lives there.
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
    image (the first height map that LOADS, flat or not - a missing higher
    resolution once replaced a flat but sized one and dropped the aspect to 1)
    and every U in ModelBaker is divided by it, so one repeat spans that
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
  grid happens to fit that texture's large blocks anyway. ONE RECORD PER
  SET decides how its worn meshes bake - its surface KIND, its RELIEF and its
  noise SEED - in `Assets/WornSets.h` (code-review batch 90): `AssetBaker
  models`, `AssetBaker wornblock` (the editor's import and type save) and the
  type editor's relief row all read it, because the files are named by the
  set alone and shared by every world and type. It used to be two sources -
  the full bake's table and wornblock's per-kind default with a std::hash
  seed - so saving ANY surface field re-baked a shipped set at another depth
  with other noise. A set the table does not list (an editor import) gets a
  DERIVED record (its kind's default relief, an FNV seed of its name), and
  wornblock REFUSES a shipped set asked for as another kind. TWO KNOBS on a
  type, not one: `relief` is the displacement AMPLITUDE in metres (how far the
  stones stand proud) and `wear` is a 0..1 SCALE over it, both catalog fields
  on the surface schema and both `rebakes` (`AssetBaker wornblock ...
  --relief --wear`). They are OVERRIDES on the record: together they scale
  the WHOLE displacement (height-map term, bowed-masonry and unevenness noise,
  ground wear, procedural wear alike) by relief x wear / the record's relief,
  and wear 0 is the bare quad for walls, floors AND ceilings. An absent
  relief is the SET's own (the schema has no default; the dialog shows it as
  "derived"), so a type that sets neither bakes byte-for-byte what `models`
  does - `tools\WornBakeTest.py` (CheckAll `worn`) holds every shipped set to
  that, to the files GIT holds (the index: HEAD's, or a staged re-bake - never
  the working tree, which `models` itself rewrites, so a regressed bake would
  be compared with its own output), to the flat quad, and procedural floors and
  ceilings to the scale (fixtures with a flat map; no shipped one lacks a
  height map). Committed worn files with no record are allowed only for a set
  a committed imports.cat names (its record is derived); an editor import's
  files not yet in git are ignored. Before relief existed
  the amplitude was a baker constant and `wear` could only take relief AWAY,
  so "wear = 1" looked like a no-op. Don't confuse either with
  `height_scale`, which is the SHADER's parallax depth: fake, per-draw, no
  rebake, and by construction invisible head-on (the offset scales with the
  view's tangential component) - real silhouette relief only comes from the
  mesh, and deep relief on the `med` tier can facet.
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

Settings page (the landing menu and the pause menu share it: m_settingsUi,
GameUI::BuildSettings) is SIX tabs - Game / Controls / Video / Audio / UI and
Material (GameUI_Stone.cpp's BuildStoneTab) - in one ui::TabControl placed at
window fractions, with the Back button under it likewise. INSIDE A TAB NOTHING
IS PLACED: each tab's rows go in a content-sized ui::Stack from GameUI.cpp's
SettingsTab() (fitContent, padRem 1.0, gapRem 0.42; BuildStoneTab builds the
Material tab's the same way), and every row says only how TALL it is, in REM,
through the kSet* constants beside it - kSetLabel 1.05 (a heading / field
name), kSetCtrl 1.45 (a dropdown, checkbox, key bind, a button row), kSetSlider
1.85 (label over track), kSetPicker 2.6 (a colour swatch with its label),
kSetGroup 0.85 (the Space between sections) and kSetRule 0.15 (a ui::Separator's
own row). A section break is Space(kSetGroup) + Separator + Space(kSetGroup); a
colour grid is rows of three, each a horizontal Stack. Rem is the settings
context's root font (kMenuFontH 28 px at kFontDesignWindowH 900, rescaled with
the window height by GameUI::UpdateFonts), so the rows grow with their text.
A NEW SETTING IS A Row WITH A kSet* LENGTH, never a coordinate, a pixel or a
fraction of the page (the old Flow helper and its design-px `uiScale` are gone
- docs/ui-hierarchy.md). ui::Slider is self-contained (label on the top line,
track in the band beneath, all inside its bounds). A tab's page is the
TabControl's ScrollArea, so a tab longer than the page scrolls (wheel or thumb,
page-scissored); the strip sizes each tab to its label and grows + recenters
the control to fit [TabControl::LayoutStrip], and the content area is inset from
the frame [TabControl::ContentRect]. The Yes/No confirm modal (GameUI::
OpenConfirm, m_confirmUi) is the exception still hand-placed at window
fractions (code-review C91). On the tabs:
quality dropdown on Video (Low/Medium/High/Ultra: mesh tier low/med/high/high
+ textures 1k/1k/2k/4k + point-light budget 16/32/48/64) plus a Max Lights
dropdown on Video (GameSettings::kLightBudgets; picking a quality resets the
budget to its tier value via Game::SetQuality → GameUI::SyncMaxLights, then
the dropdown can override it; DungeonWorld::SelectLights keeps the lights that
add most to the view up to the budget - see "The light budget") plus a Frame Rate dropdown (GameSettings::
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
master-volume slider on Audio, the MATERIAL tab (the stone picker, see "Stone
chrome" below), and on UI the Textured UI and Head bob checkboxes, the Resource
Bars brightness / saturation sliders, the HUD layout
(Standard / Minimal), Lock / Reset HUD layout and a scale + background-opacity
pair for every floating HUD panel (see "Stone chrome, floating panels,
Minimal layout" below - the party bar's old pair is one of them)
plus a color-picker grid for Theme Colors (the 8 ui::Theme
colors - GameSettings owns the master theme, GameUI::ApplyTheme pushes it
into all seven of GameUI's UIContexts and the two dialogs' own, live) and one
for Party Colors (see PARTY CREATION). The ColorPicker control's swatch opens an R/G/B/A
slider popup; kThemeFields in GameSettings.h drives the grid and the ini
round-trip. (The Resource Bars picker grid is GONE - the fills are procedural,
see RESOURCE BARS; an old ini's bar_<name>= lines are ignored.) Controls tab: movement key bindings via ui::KeyBind rows
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
presentinterval=1..4, language=<code>, volume=0..1, ui_stone=<name>,
hud_<panel>_pos/_scale/_opacity, hud_layout, hud_locked (barscale/baropacity
still load, into the party bar's pair),
theme_<name>=r,g,b,a, key_<action>=vkey, look_sensitivity/look_hold/look_return/
look_move=<float> and look_curve/look_move_curve=<easing index>,
adapter=<packed LUID, 0=auto>,
output=<index>, reswidth=/resheight=<0=window default>, fullscreen=0/1/2;
sliders save on release, pickers when their popup closes, key binds and language
immediately, display fields on Apply). Main reads the display fields BEFORE the
window/device exist (its own GameSettings::Load, same file Game re-loads).
Quality hot-swaps in place (WaitIdle + rebuild); Ultra falls back per-material
to 2k with a warning if 4k not installed.

## Game state machine

Loading (staged tasks, one per frame, progress screen) → Menu (title art
assets/ui/title_bg.png, cover-fitted by GameUI::DrawTitleBackground behind the
menu and both loading screens; the title menu stands in a LEFT column,
kMenuMainCentreX, so the art's centre figure shows; MenuList: Continue/Load/Start New Game/Settings/Exit — Continue/Load
appear only when a save exists; all entries work) → Playing ⇄ Paused (Esc in-game freezes
the world and shows Save/Load/Settings/Exit/Back over the scene; Esc backs
out / resumes). QUITTING IS ALWAYS DELIBERATE (Michael, 2026-08-11): an Exit
entry (landing or pause) or the console's `quit`/`exit`. **ESC NEVER QUITS, IN
ANY STATE** — it only backs out (settings page, pause, sheet, overlays) and does
nothing on the landing list. It used to quit from the landing list AND from both
LOADING states, which read as a CRASH: a party wipe drops you on the title
screen, and a reflexive Esc at a screen that appeared by itself killed the
process with no confirmation and no log line. During a load, where no Exit button
is up, the ways out are the console's `quit` / `exit` (the ONE pair the loading
gate lets through - DevConsole::SubmitLine), Alt+F4 (Window passes VK_F4 on to
DefWindowProc, which makes it WM_CLOSE, in every display mode) and, in Windowed,
the window's own close button. Borderless has no close box and Exclusive covers
the screen, so before code-review C392 a load in either had no way out but Task
Manager. WM_CLOSE sets m_closed independently of all this, so no state can ever
be unquittable. Everything else routes through
Game::QuitRequested, polled by the main loop. During the three
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

PARTY CREATION (party-creation branch, docs/party-creation-plan.md + -notes.md;
built in phases - the page itself is phase 3). NO CLASSES (Michael): a member is
a RACE, the points they spend and the skills they pick, then whatever they do.
- RACES are data: each world's `races.cat` (human / elf / dwarf / orc, also in
  the world template) - stat modifiers from 10, `extra_points`, `pace` (->
  moveSpeed), `base_health/_stamina/_mana`, `resists` (-> Character::
  natureResists; NOT saved, re-applied from `Character::raceId` on load by
  Game::ApplyRaceResists) and the `portrait` tag the picker filters by. Poison
  bites as EARTH, so the dwarf's poison resistance is `earth 0.25`.
- THE ARITHMETIC is the pure `Game/PartyRules.h` (in RollTest): base 10, 5 free
  points + the race's extra, a floor of 3, 2 starting skills boosted to LEVEL 2
  (xp 4), 2 starting items, names 1-16 characters with no underscore. A member
  as chosen is a `party::MemberSpec`; `Game::BuildMember` (Game_Party.cpp) is
  the ONE place it becomes a Character - the page, the dev command and the eval
  all use it. A starting item must be on the world's manifest `start_items` list
  (low-quality existing gear; a later branch adds sword / potion / wand / ring to
  it) and goes where it belongs: a weapon in the first empty hand, armour on its
  `wear` slot, the rest in the pack. A skill must be one `DungeonWorld::
  TrainableSkills` lists (what SeedPartySkills seeds). The DEFAULT FOUR stay
  exactly as authored - every eval suite measures them - as PREMADE members
  (`MemberSpec::premade`): only name / portrait / colour change.
- THE START: `Game::m_startParty` is the party the next new game uses (consumed
  by StartNewGame; empty = the default four; `ResetForEval` clears it, so a suite
  always starts from the four). `ResetRoster(party)` replaces the vector when the
  SIZE differs and calls RebuildForRoster at once (every caller runs outside the
  HUD's widget walk); only the default four take the Settings palette's colours.
- COLOUR is the member's own (saved with them). Settings -> UI -> Party Colors
  FOLLOWS THE PARTY: in a game, row n names member n, shows their colour and
  recolours them live (and sets slot n's default); on the title, or for a slot
  a short party leaves empty, it is "Member n" and edits only the ini's
  `member_<n>=`, the colour a new member in that slot starts with
  (GameUI::SyncMemberColorPickers, re-run whenever the page opens).
- SAVE: `roster <n>` plus per-member `name` (spaces as underscores), `race`,
  `color`, `pace`. A save without them is the default four, so no version bump.
  LoadGame cuts the default four down to `roster` before laying the save on top.
- Dev: `newparty default | <member> [| <member> ...]` (key=value words: name=
  race= portrait= color=rrggbb points=s,d,v,w,i skills=a,b items=a,b premade=n;
  the `|` is its own word) starts a new game with that party; `roster` prints
  what each member was made from. `newparty` works on the TITLE SCREEN too: with
  no world resident it opens the default one first, as Start New Game does (and
  portraits.cat loads with the BOOT load, `Game::LoadPortraitCatalog`, so a face
  can be checked before any game). Checked by `tools/EvalScripts/
  partycreation.eval` (a created 2-member party survives save -> reset -> load)
  and RollTest's "Party creation" section.
- PARTIES OF 1-4 IN PLAY: everything placed by roster SLOT treats a missing
  member as a fallen one - a lone member stands front-left, a third rear-left,
  and the per-file blocking rule opens a file whose near member is absent, so
  the member behind the hole is reachable from that side. The `parties` eval
  suite (smallparty.eval) measures it: from behind, a party of three is hurt on
  Sera and Maren and never on Brand. `AllocTest -Party '<spec>'` runs any mode
  with a created party (refusing a PASS if `newparty` did not build it), and
  InGameTest sweeps a party of three and of one. The four-slot tables (monster
  threat, its save line, the bar's slots) are static_asserted against
  `party::kMaxMembers`; the Magic dock draws no button for an empty slot.
- THE PAGE (Game/PartyCreationPage.*, menu glue GameUI_Party.cpp): Start New
  Game -> (the world list, when there is more than one) -> the party page ->
  Start. `Game::OpenPartyCreation` LOADS THE WORLD FIRST (not its levels), so the
  page offers that world's races / skills / `start_items`; a different world
  switches a frame later and opens the page when it lands. The Editor entry,
  `newgame` and `reset` skip the page and keep the default four. The page edits
  MemberSpecs and shows a PREVIEW of each built by BuildMember plus the world's
  pool rules, so its numbers ARE the game's. Number edits leave the tree standing
  (Tick rewrites the live text each frame); select / add / remove / a race that
  remakes a premade member rebuild a frame later (TakeRebuild, polled at the top
  of UpdateMenu - the cached-pointer rule). It lives in m_savesUi like the world
  list, draws no big title (its card needs the height), and its RACE LINE spans
  the card (a race's line is longer than a column). Default party lays out the
  four as PREMADE members: authored stats, no stones, no picks; another race makes
  one anew. Esc: the face picker, then an open list (a DropDown closes on Esc now;
  `UIContext::PopupOpen` tells the page), then the page (Back: the world list it
  came from, else the title). Dev twin: `partypage [open|add|default|back|start|
  picker|select|remove|set k=v...|spend|skill|item]`, every verb one of the
  page's own edit methods, `set` through the same `party::ApplySpecField` as
  `newparty`. Checked by `tools/EvalScripts/partypage.eval` (ON ITS OWN - it
  starts on the title, where `partypage open` must be) and InGameTest's title
  sweeps `sweep_partycreation` / `sweep_partydefault` / `sweep_partypicker`.

The HUD's top bar shows the party — 1..4 members; party creation
lets the player build fewer than 4, and the bar always reserves four slots
so a short roster keeps its slot size (Character.h roster; the bar is
PartyBar.h, one CharacterPanel.h per slot - portrait, name, effects,
health/stamina/mana bars - and PartyHud.h is only the umbrella header that
includes the party widgets); clicking a portrait
opens the character details page (AppState::CharacterSheet). It is NOT a
pause (Michael, 2026-09-28: only the pause menu and the editor's pause button
stop the game): over a level the world keeps simulating under it, while the
input stays the sheet's, so the party does not walk off under an open page (prev/next cycle members modulo the live roster
size, Esc/Back resumes). The per-member widgets (CharacterPanel, HandSlot,
CharacterSheet) hold NO Character* across frames: they address
Game::m_characters by (roster, index) and re-resolve through PartyHudTypes.h's
RosterMember at the top of every Update/Draw — an index past the roster's
end just goes inert (no draw, no mouse) — so a roster of any size, or a
resize, can't dangle them. StartNewGame/LoadGame still reset members in
place (keeping each slot's loaded portrait); a roster SIZE change must
call GameUI::RebuildForRoster (deferred like RebuildForLanguage, never
from a widget callback) to re-lay-out the per-member widgets — BuildHud
lays out whatever count it finds (hand pairs fill 2 wide, 2+1 for three).
EVERY PIECE OF THE HUD BUT THE MESSAGE LOG IS A FLOATING PANEL (ui-panels, see
"Stone chrome, floating panels, Minimal layout" below; kHudPanelFields: party,
status, move, hands, magic, cards, inventory, tray, sheet), and the layout
described here is only where each one DEFAULTS. Left column under the bar: the
STATUS panel (facing + position, GameUI's m_compass / m_position; the Options
panel that sat under it - torchlight palette, Rest, Help - was REMOVED in
lighting-updates Phase 1: light comes from what is lit, and Rest / Help sit in
the log's corner row beside the Log button, MessageLog::cornerButtons). Right
column: three DOCKS (Game/ControlBar.h - the Dungeon Master control panel was
one framed box; it is three floating panels now, each minimizable to the tray):
MOVEMENT (a MovementPad, 3x2 turn / step buttons: GameUI::onMoveAction ->
Party::Act(MoveAction), the same discrete actions the bound keys map to in
HandleInput), HANDS (a HandsArea, one HandPair per member, each two HandSlots -
HandSlot.h - with the member's identity stripe; a click USES the hand, see
"HAND BOXES" below) and MAGIC (the SpellbookPanel, shown only once a member
knows a symbol). The Minimal layout replaces the bar and the Hands dock with one
card per member (Game/MemberCards.h).

In the 3D view the mouse does three things (Game::Update, gated by
GameUI::HudMouseConsumed so HUD widgets win the click): LEFT-click picks a floor
tablet up onto the cursor (DungeonWorld::TryPickItem) or drops the held one
(DropItemAt); a RIGHT-click that never strays past 3px opens the DETAILS of the
floor item under the press (DungeonWorld::ItemTypeUnder - the same pick without
the lift); holding the RIGHT button and dragging is MOUSE LOOK. The drag adds
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

WHERE A FLOOR ITEM IS has ONE answer, `DungeonWorld::FloorItemPose` (code-review
C180): its spot (its quarter, or a wall niche's pocket), the draw's matrix (the
kind's `floorLay`, worked out once by LayOnFloor - laid flat, or standing
`upright`) and its drawn middle; false when it is lifted or in a SHUT niche. The
draw, a rune's or an enchanted blade's floor glow, a lit torch's light and flame
and the pick all take it, so a shut niche hides its glow too (it used to glow at
the foot of the wall). The pick measures the click ray at that drawn middle
(`ItemKind::floorHeight`, C359 - it used the model's height in model units, laid
flat or not). Every ROUND click target - a niche's pocket, a door's hand-hold, a
wall torch - is one ball test, `Camera::Ray::HitsSphere`, sized in units x kUnit
(C258). Dev: `pickprobe` shoots every target in reach from where it is DRAWN,
through the real tests - an item's drawn box is its model's bounds through the
pose's matrix, never the pick's own inputs (`floorHeight`, the slot's quarter),
since a probe built from those intersects at the height it projected from and
hits for any camera; `niche <x> <z> <dir> [open|shut|put <item>]`. Checked by
AllocTest -Items (tools/EvalScripts/itempose.eval, before its window), which
also wants an open niche's glow over its rune, in the pocket.

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
controlled live from the dev console (`threads` lists the registry as text).
THREE RULES THAT BITE (code-review batch 34): (1) PAUSE ONLY ASKS - it sets a
flag, and a worker mid-tick finishes and publishes that tick first;
`WaitPaused(id, timeout)` is the other half, blocking until the worker HOLDS at
its pause point (a pause GENERATION it acknowledges there, not the Paused state,
which can lag a Resume). (2) RESTART KEEPS A PAUSE - a pause belongs to whoever
asked for it, so the supervisor rebooting a stalled AI worker under lockstep
leaves it paused. (3) A CLIENT WHOSE JOB CAPTURES ITSELF REMOVES ITS WORKERS -
`Stop` leaves a Dead slot still holding the job, which the panel's `boot` would
run against a destroyed client; `Remove(id)` joins, drops the job and takes the
slot out (a Restart already waiting on it gives up). Get hands out SHARED
ownership, so a Worker the supervisor is looking at outlives a Remove or Reap.

The AI itself (Game/MonsterAI.h, namespace dungeon::ai) is walled off like
MagicSystem — it knows nothing about DungeonWorld/Party/map, reaching the world
only through ai::IWorldView. THINKING is split from ACTING: Brain::Think (cheap,
IQ-gated) sets a monster's standing orders - an ai::Intent::Mode of Idle, Engage,
Kite or Flee, picked from its ai::Archetype (Brute / Skirmisher / Caster / Swarm /
Lurker / Sentry, monsters.cat `archetype`), its per-instance leash and its
`fleebelow` - plus, for Engage only, a full chase PATH (Brain::FindPath,
4-connected BFS); the host EXECUTES those orders EVERY frame at the monster's own
move/attack cadence (Kite / Flee, a patrol route and a leash's walk home are
host executors stepping greedily from live positions - docs/ai.md), so a
dim monster still moves and swings at full speed, only its CHANGE OF MIND lags.
ai::AsyncDirector spawns one worker per IQ bucket (4) on the Manager. Each frame
the main thread publishes an immutable ai::Snapshot (party cell, a revision-
cached walkability grid, live monster positions + per-monster id/iq/aggro — from
a POOL of reused buffers so steady-state frames allocate nothing per the memory
strategy; the blocked/occupancy sets are FLAT mapW*mapH grids, not node-based
containers, so clear-and-refill really is allocation-free — anything that
hand-builds a Snapshot, e.g. tools/ThreadStress, must size those grids) and the
workers post ai::Plan batches (intent + path; batches and their path vectors are
pooled per bucket the same way) the main thread consumes and executes (popping
path cells, re-validating each against LIVE occupancy). THE POOLS (code-review
C62/C66/C70): a buffer is reused when it is not the one on show and its
ai::HandBack mark reads idle - a reader TAKES the mark under the hand-off mutex
and GIVES it back with a release, the owner checks it with an acquire; NOT
`use_count()==1`, which the standard gives no ordering. The snapshot and grid
pools are FILLED at level load (DungeonWorld::ReserveAIPools) to kBucketCount + 2
- the snapshot on show, one per worker, the one being built - and never grow in
play. A POOL HAS ONE PRODUCER: each bucket has a worker plan pool (3 batches,
filled in the director's ctor; its plans and paths grow on the worker's own,
unguarded, thread) and an INLINE one (2 batches) for lockstep's main-thread
compute, which REST forces into guarded frames. The two never share a batch -
by construction, not only because entering lockstep now WAITS for every worker
to hold at its pause point (WaitPaused; C63, ThreadStress phase G - a tick still
in flight used to publish after the first inline compute) - so the inline pool
can be SIZED from the main thread: at level
load and on every AddMonster, ReserveAIPools sizes the inline brain's BFS scratch
and gives every monster of a bucket (+2) a plan slot with a whole map's worth of
path. A batch NEVER SHRINKS: it publishes a COUNT and the consumer's Batch is a
span of that many, since destroying a plan frees its path and making one
allocates (in debug even an empty vector, its iterator proxy - the first run of
-RestReach caught exactly that). Anything that grows anyway - a pool, a
snapshot's buffers, the inline brain's scratch or an inline batch's plans - logs
`AI pool grew:` once, wherever it landed, and AllocTest fails any run that logs
it. The BFS open list is a Brain member walked by a head index (it was a
std::queue, a deque per search). `AllocTest -Rest` presses the HUD's Rest button
INSIDE the window, behind a shut door from a monster that has seen the party
(its search fails every frame); `-RestReach` leaves the corridor open and
freezes the monster, so every think finds a path it never walks. Both read
`lockstep stats` (inline ticks / plans / paths since lockstep came on) and
refuse a PASS without the thinking they exist for. Plans are keyed by a STABLE
per-monster runtimeId (DungeonWorld
assigns from m_nextMonsterId, never reused) — NOT an array index — so a plan
whose monster died / changed bucket / was erased simply finds no match
(MonsterByRuntimeId) and is dropped, never misapplied to a neighbour that shifted
into its slot. A NEW GAME OR A LOAD GIVES EVERY MONSTER A NEW ONE
(ResetForNewGame): the workers keep thinking while the world stands frozen on
the title or the pause menu, from the last snapshot, and the first frame back
used to apply those batches and latch `aware` for every monster of the last
fight, sleepers included (code-review C52). Measured where it lives, on the wall
clock: tools/AITest.py's aiasync.eval (no lockstep, no `reset`; `aiwait` blocks
until every bucket has published, `title` takes the pause menu's way out, and
`monsters` ends each line `aware=0|1`). The director's lifetime is the world's:
~AsyncDirector REMOVES its four workers, so a world switch leaves no Dead
`ai.bucketN` slot (C69; the same script reads `threads` after one, and runs with
`-project` so that switch leaves settings.ini's last world alone). A monster's iq (monsters.cat field; Scheduler::BucketForIq) picks
its bucket; bucket intervals are PRIME milliseconds (251/499/997/1999 ms ≈
4/2/1/0.5 Hz; Scheduler::BucketInterval) — coprime, so the buckets almost never
fire together (cicada pattern) instead of resonating like power-of-two harmonics.

WHERE A MONSTER MAY STAND is ONE predicate, `DungeonWorld::MonsterCanStand`
(code-review C58, C74): floor, no brazier, a floor under it (not a pit or a
stairwell), no solid decoration, no shut door. The formation's side list,
`FreeSlotInCell` (every step taken; `FootprintCanStand` for a Huge's 2x2) and
`BuildAISnapshot` (every step planned) all ask it - the snapshot caches the
map's half (`MonsterGroundAt`, keyed by map revision, and also the grid a
monster's sight crosses, so a hole is NOT in it) and asks the predicate itself
at every decoration, door and stair square for the rest. The lists used to
disagree: formation handed out a brazier's, a crate's or a shut door's square as
a side and the monster stood still for good, and nothing knew about a pit. Only
ENGAGING monsters take sides (C57) - a kiter or a fleer took one and never
walked to it, so in a dead end the brute behind it never closed in - but one
STANDING on a side still fills it, and a side goes only to a body
`FreeSlotInCell` has a slot there for (a brute sent to a cornered fleer's square
stalled at the refused last step). Checked by tools\AITest.py (crypt1's brazier
fight, a mage seen kiting while a brute passes it into a dead end, a brute sent
round a dazzled coward on the party's side, a skeleton going round a pit and one
shoved toward it stopping short; the tally's `mswings=` / `mshots=` count
monster swings / a kiter's shots).

Dev console COMMANDS (console-updates branch, docs/console-updates-plan.md): every
command registers a `CmdInfo` - `{.name, .group (CmdGroup enum = the listing
order), .params, .summary}`, designated initializers IN THAT ORDER. `params` is
the synopsis without the name (`<req> [opt] a|b ...`), one FORM per line
('\n'); an EMPTY FIRST form is the command typed bare, for one whose bare form
differs from its verbs. `summary` is one line, no params. Register ASSERTS on a
duplicate name (Execute runs the first match, so a second one is dead - the
per-member `threat` was, until it became `grudges`), an empty or multi-line
summary, and a stray empty form. Both readers use those fields: `help [group|
command|word]` (grouped, three-column; a word that is both a group and a
command gets both) and the TYPE-AHEAD box above the prompt (prefix matches,
then contains-matches dimmed; Up/Down move the selection while it is open,
Tab/Enter take it, Esc shuts it first; a history recall never opens it, or the
second Up would select instead of stepping back; after `name ` it shows that
command's forms). An arity error goes through `devargs::Need(console, args, n)`
/ `DevConsole::RefuseUsage`, which print the REGISTERED params, so `help` and
the error cannot drift. Code: DevConsole_Commands.cpp. A command that DECLINES
what it was asked (no such item, a rule refused, nothing there to act on) calls
`DevConsole::Refuse`, never `Print`, so an eval script counts it; a QUERY's
answer stays a Print, and so does an outcome the world decided (a locked door,
a fizzle). An eval script line written to PROBE a rule is `expect-refuse
<line>`: it must be refused by the command's own rule (the no-game gate does not
count) or the script fails on `unrefused=` (code-review C442; docs/eval-
harness.md "expect-refuse").

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
  exactly the part a failing worker varies. EVERY EVENT IS A LINE OR IN A COUNT,
  and the two counts are kept apart. The collapse's: `collapsed`, the repeats
  since a run's last line - a run of 57 has lines at 1 and 10 and no power of
  ten will ever say the other 47 - written as a closing "N further repeats ...
  (57 in the run; ...)" line when a different event ends the run AND when the
  thread unregisters. The rate limit's: `rateLimited`, only what the BUDGET
  refused (an event's line, or a refused line's collapsed repeats, which go
  with it), written when the next window opens AND at unregister, or a burst
  followed by exit never reached the log. A closing line spends the budget like
  any line, or alternating failures would outrun it. A slot handed to a NEW name
  (ResetEntry) starts with a clean window and no counts; `kLogBurst` /
  `kLogWindowNs` are in Diagnostics.h so DiagTest checks them.
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
  directly - 62 checks, including the one that matters: four writers hammering
  one slot while a reader walks it, every event self-describing so a torn read
  cannot pass (measured 16k writes, 39k live reads, 0 torn). Its LOG checks read
  the real file back through `log::FilePath()` (the path the sink opened, never
  a copy) and an unreadable log is a FAIL, not a skip: the exact budget, one
  swallowed-count line with the right count after a window rolls, the lines a
  thread's exit writes, runs of 57 and 150 closing with their tails (100, the
  one shape needing none, cannot be the only run tried), a run in a spent
  window losing nothing, and a 33rd name taking a dormant slot clean. And the
  stack SeenSet both readouts log through: full at 64, it reports no further
  site, counts each offer it turns away (offers, not distinct sites) and says
  so in one line (code-review C226). `tools\HealthTest.
  ps1` breaks the REAL game nine ways and reads dungeon.log and nothing else -
  if the answer is not in the file you open after a crash, it does not count.
  `-SelfTest` skips every injection and REQUIRES every case to fail on EACH of
  its expectations (no pattern met, no dump) and for no other reason - a harness
  error, a death with nothing injected, or a log without the control line (the
  `logecho off` echo typed just before the injection) fails the self-test. It
  used to pass on any failure, so a game that crashed at boot passed it.
  Two of the nine (`uiclip`, `uinest`; code-review C208) read what a caught
  throw leaves BEHIND - the UI walk's clip, which a throw used to leave in force
  in every context, so a later click outside it was lost. Each checks its own
  premise: `uiclip`'s throw names the clip in force and the button outside it
  only when both hold (a scroll area that stops overflowing clips nothing, and
  the click would land under the old walk too), `uinest` refuses a tree that
  did not nest two clips.
  Dev: `crashpoke <throw|uiclip|worker|fault|assert>`, `clippoke`, `threadwedge`,
  `threadspawn <ms>`. NOT covered: the Killed kind (a hard kill is a panel button, not a
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
  Fog of war - only revealed cells and their contents draw. NO DOCKS AT ALL:
  both docks are Editor-only (Michael, 2026-09-23: the key is for building, not
  for playing), so in Player mode GridArea is the whole panel. The symbol key
  and its own `map_player_key_collapsed` flag went with it (MapView.cpp's
  LegendCollapsed: one flag, mapLegendCollapsed, the editor's). Around the grid:
  a centered title; TOP-LEFT the world-map globe when the world has one
  (ShowWorldButton; MapView::WorldButton - the corner WorldMapView puts its
  way back, so the pair reads as one control that stays put), then the [^]/[v]
  level-browse arrows beside it (LevelUpButton steps right of the globe, or
  takes the corner when there is none) and the viewed level's stem; the shared
  close box top-right, the globe's square mirrored (ShowCloseButton); and a
  footer of the pan/zoom hint and the party's square. No brush dock / editing.
  The `M` key is hardcoded (kKeyFields is MoveKeys-only; a bindable map key
  needs a separate UI-keybinds table).
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
head-shot framing the model's top quarter, posed on the kind's idle's first
frame and framed on that pose - what the asset picker's tile shows, not the
kit's T-pose (MonsterKind::iconPose, code-review C183; dev `mapicons [status|
all|rebake|survey on|off]`, the survey drawing each icon beside the picker's
tile of its model, checked by EditorTest phase 22 on what the bake RECORDS it
drew - MonsterKind::iconDrawn: the framed box, the palette's clip and
fingerprint - against a fresh pose of the idle), decorations/fixtures bake whole,
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
OneDrive archive; a surface's worn bake takes the relief / wear its type's
catalog entry carries, as the editor's save does), and `synctosource` now also copies the manifest's asset FILES
from the exe-side pool into the source tree, so a `build/` wipe doesn't take
them. NOTE the naming rule an editor import must follow: a PBR set installs under
its RESOLUTION-tagged name (`<set>_2k` — LoadPbrSet's universal fallback) while
the catalog's `texture` field names the BASE; the worn-block bake takes the base
and finds the height map at any installed resolution. EXCEPT the three surface categories,
whose rows come from the VIEWED LEVEL's `palette` record, not the catalog — a
catalog type must JOIN that palette before the brush can reach it. The old
"+ Catalog..." row is GONE: a persisted "Catalogue" checkbox (settings.ini
`map_show_catalog`) lists the whole catalog instead, and painting a type the
level lacks enrols it on the spot (DungeonWorld::EnsureSurfaceVariant →
AddPaletteEntry, or ...Remote for a browsed level's stash). The append is UNDOABLE (the
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
A model tile and the preview show the model AS THE WORLD DRAWS IT, through ONE
loader, `DungeonWorld::LoadPoolModelLook`: every primitive with its own glTF
material and embedded maps (the baked .dds sidecars), and the texture set a
catalog binds to it (`AssetPicker::textureFor`, the ModelAndTexture rule) - on a
single-primitive .gltf the set WINS (the world's single-mesh path), elsewhere it
only fills an untextured part. A tile drops that model's meshes and textures
kFrameCount Ticks after its bake (`Thumb::bakedAt`), keeping only the 256 px
image, so browsing never parks a rig's maps on the SRV heap. The preview is
FITTED to the pane (a dagger to a staircase); a flat model tumbles, a standing or
rigged one turns upright (`PreviewStands`). A RIGGED model shows its IDLE, not
its bind pose: the preview plays it looping, a tile bakes its first frame, and
both fit the POSED bounds (`PoolModelLook::FitToPose`, CPU skinning). The idle is
the catalog's (`AssetPicker::idleClipFor`, monsters.cat `anim_idle`), else an
`idle` / `idle__...` clip - never the first clip, which for the kit is a spawn
lying on the floor. A floor-mounted model (a floor feature, the floor block, and a
stairs.cat entry with `hole = floor` - the pit and the stairwells) has two cases.
A SHAFT deeper than a square (the drain and recess, four squares on purpose) is
framed and CUT at half a square (`FrameAboveFloor`, `ClipBelow` in
DungeonWorld_Models.cpp - the picture only, the models untouched) and shown at a
three-quarter view from above (`kFloorFeatureTilt`, ~63 degrees). A WELL within a
square (`PoolModelLook::Well` - the pit, the stairwells) is kept whole and seen
almost straight down (`kFromAboveTilt`, ~86): its walls sit near the cell's edge,
so at any oblique angle a band of wall shows under the tile's near edge - its
outer skin, or with that dropped (`DropWellSkin`) the far wall's inside - and
hiding it takes tan(tilt) >= depth / (0.5 - wall radius). A ceiling hole that is
scenery (stairs.cat `hole = ceiling` + `traverse = 0`, pit_ceiling) is that well
turned over - `Mount::CeilingWell`: its inside lies above its rim (the model's
lowest point), it is seen straight up, and it grades darker going up. A
WALL-SIZED PANEL (a square wide and tall, at most 0.35 deep - the arches, the
archway, the door frames; not a wall feature) gets THE DARK BEYOND: a near-black
backdrop just behind its back face (inset at the sides and top, not the bottom),
so its opening reads as a passage instead of showing the tile's light halo, and
its preview swings across its face (`PoolModelLook::backed`). A BORED wall feature
(a window, its tube more than 0.3 deep) is cut 0.15 behind its face (`ClipBelow`
on z) with the dark beyond at the cut, inset 0.12 all round so the preview's
swing never shows its edge. Parts that only make sense ON something are shown
with it, dimmed (`AssetPicker::contextFor` -> `PoolModelLook::AddContext`): a
door's `trim` on its leaf, an opener on its `mount`. The rune tablet wears the
first rune's set (a rune names no model), a fixture's `empty_model` its
fixture's. A PLATE (flat and broad on both horizontal axes - the grate, the coal
bed) is seen three-quarter from above. `sinks` needs 0.1 below the floor, not a
sliver (the door chain dips 0.027 and was looked at end-on, a dot). A SLENDER
model's TILE (6x taller than wide - only the door chain, 7.7; next is 4.8) frames
its top 40% (`FrameSlenderTop`), capped at `kMaxFrameAspect` (2.2) x its width
and starting at its context's top (the socket, not the loop of chain over it):
mount and first links, readable; the preview still shows it whole. A HUNG model (off the floor, reaching the ceiling at y = 1 -
the hanging chain, 4.8) qualifies from 3.5:1 (`PoolModelLook::Slender`); ratio
alone cannot pick it, since the potion vial is 4.7 and must stay whole. A GLASS
model some item fills (items.cat `liquid_color`; `AssetPicker::liquidFor`, the
first such item) is shown FILLED, glass cleared, as play draws it - through the
same `DungeonWorld::BuildLiquid` / `ClearGlassForLiquid` that `AddLiquid` uses.
A fixture ON a wall (fixtures.cat `mount = wall`: its model and `empty_model`;
`Mount::WallFixture`) tiles from the side (`ViewYaw` -> `kSideYaw`, 1.2 rad), so
what sticks out of the wall - the bracket's ring - shows in profile; its TILE
frames at most `kMaxFrameAspect` x its widest horizontal extent, centred on that
projecting part (`FrameWallFixture`, `projectY`), so the 4.3:1 bracket is ~2x
bigger. An opener's `mount` wears the opener's set. Survey all
89 tiles when changing any of this: open the picker on every eighth model.
A surface or wall feature has no set of its own, so it is shown in the project's
first floor / ceiling / wall type, what it wears in a level. A feature's INSIDE
(every triangle more than 0.05 behind its mounting plane - below y = 0 for a
floor feature, toward -z for a wall one; `PoolModelLook::Mount`, from
`AssetPicker::mountFor`) is split off, back faces culled, and drawn in DEPTH BANDS
from `kInsideShadeTop` at the mouth to `kInsideShade` at the bottom, standing in
for the shadows the icon rig does not have: a niche lit as evenly as its wall, in
the same brick, read as a flat panel, and one flat shade made a stairwell's treads
a single dark sheet - the gradient is what says they go down. The 0.05 clears surface relief (cracked paving
sinks 0.04 and went blotchy at 0.01). A wall feature's preview SWINGS across its
face rather than spinning onto its back. The clean SURFACE BLOCKS (floor_block,
wall_block, ceiling_block - no catalog draws them) are mounted by name in
Game_Wiring's `kSurfaceBlocks` table: each wears its surface's first type and is
seen as a player meets it - a floor from above, a wall face-on, a ceiling (and a
ceiling feature) from below at ~50 degrees (`kFromBelowTilt`; straight up, a
vault's curve flattens). `PoolModelLook::ViewTilt` is the one statement of that.
RIGHT-CLICKING a palette row opens the per-TYPE catalog editor
(TypeEditorDialog) for EVERY category — one dialog, because it renders its form
from a SCHEMA: Game/CatalogSchema.h is a FieldSpec table per catalog (key, kind,
section, range/step, options, one-line help), so exposing a field is one table
row and a new category is one table (the kBalanceFields idiom). Sections become
tabs, kinds become widgets (Bool -> checkbox; Float -> snapped slider; Text ->
text field; Enum / DamageType / CatalogRef -> dropdown, a CatalogRef's ids from
Game through optionsFor; TextureSet / Model -> NOT a dropdown but a button
showing the value that opens the ASSET PICKER through onPickAsset - see above;
CatalogRefPick -> a ticked list with swatches; QuestStages / WeightedRefs ->
rows of their own), and "?" explains the active tab's fields. The kind switch
names EVERY FieldKind with NO default, and the Game lib builds with C4062 as an
ERROR (src/Game/CMakeLists.txt): a new kind fails the build at every switch that
must learn it. Before that, DamageType built no widget at all and two catalog
fields silently could not be authored (code-review C101). `typeset dialog rows
[all]` prints what each schema row built, read off the widget tree, and
EditorTest phase 20 demands a control for every row of every category. A theme
member row's SWATCH is asked of `swatchFor` each time it DRAWS and never kept
(ui::Checkbox::swatch is a function): it is the world's albedo, and a quality
change reloads it under an open dialog, which drew freed textures (C235). NO
live apply (a type is referenced by every placement
and, for surfaces, by baked geometry): Save writes the .cat and, when a touched
field is `rebakes` (a surface's texture/relief/wear), re-runs the wornblock bake
behind the busy overlay. A surface's PER-DRAW knobs are the exception —
`height_scale`, `metallic` and `roughness` are per-variant values the draw reads
(Surface::heightScale / ::factors, filled by ResolveSurfacePalettes →
ApplySurfaceFactors), so saving them pushes at the live scene through
DungeonWorld::RefreshSurfaceMaterials: no reload, no rebuild. The factors follow
the PROP rule — absent = -1 = the set's ORM map stays authoritative, a value
REPLACES the draw's factor (which the shader multiplies over the map). An
OPTIONAL float (no schema default) that is absent shows as a CHECKBOX, "...: from
the texture's map" (or, for a `derivedFor` field such as a monster's `power`,
the derived value), until unticked into a slider, and a set one carries a clear
disc that removes it again - so "map-driven" is never confused with an explicit
0. Only fields the user TOUCHED are written and an empty
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
levels. Each record family is a `DungeonMap::TypeRecords` value, and a catalog a
.map record can name needs one (surface features had none until code-review
C305, so a delete was never refused); a FEATURE type (wall or surface,
`DungeonWorld::StampedIntoSurfaces`) is stamped into the chunks, so its rename
re-files the feature meshes by id (LoadFeatureMeshes) before the re-stamp. A
catalog whose IDS ARE THE CODE'S - effects, spells, attacks, balance
(`Project::IdentityInCode`) - refuses both rename and delete, and its type
editor offers neither: the entry only tunes a class or table row, and a renamed
effect silently lost its tuning (C306). Catalog comments survive a write (serialize::Block::lead →
CatalogEntry::lead): the .cat headers document each category's fields, and an
editor write used to delete them. Three more ways were closed in C323: the
comments after the last entry are the catalog's TRAILER (a header-only file,
the template's flags.cat, is nothing else; a new world drops a source's
dungeons, quests and flags through `Catalog::ClearEntries`, which keeps the
header - assigning a fresh Catalog lost it), deleting the FIRST entry hands its
lead (the file header) to the next entry, and the monster dialog sets its rows
IN PLACE (`Game::ApplyMonsterConfig`). `catround` checks all three on text of
its own (`catround case ...`). Mouse model: LEFT paints/places
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
snapshot after each paint. Entering a level consumes its stashes; the
right-click inspectors (MapEditor::InspectAt) still need the level active (no
live instances remotely - a browsed square only reports its static base).
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
  (`flags.cat` joined them in tool-refinement Phase 4: named on/off facts,
  scoped to a dungeon or the world; `styles.cat` in Phase 5, with a shared
  library in assets/library - see the Tool refinement section.)
  NOT every catalog is placeable: `effects` is authored + tuned only (an
  effect needs a class, so no "+ New..."), and since tool-refinement Phase 1
  it is NOT IN THE PALETTE at all - the Balance dialog's Effects tab lists
  them and each row's disc opens the type editor over that dialog.
- `levels/<stem>.map` + `.ent` — the level layers. The .map's surface palette is
  a `palette <wall|floor|ceiling> <id>...` record (catalog ids), and it also
  carries `stairs <type> <x> <z> [facing] dest= destx= destz=` (a stair's
  FACING is the way you face STEPPING OFF it into its level, and so the way any
  arrival on its square faces - Michael, 2026-09-25: crypt1's stairs up to the
  gate rise south, so they face north. The prop is turned the opposite way. It
  used to mean the way the steps rise, and a file written before carries no
  `stairfacing arrive` line, which is how DungeonMap knows to turn its stairs
  round at load; `destfacing=` is accepted and ignored, since the stair you
  land on already says it),
  `variant <wall|floor|ceiling> <x> <z> <index>`, and `atmosphere [dust=…]
  [haze=…] [ambient=…]` (per-level mood knobs, authored by the editor's Level
  settings dialog) records. (The old `assets/maps/level1.*` with `textures`
  records is dead — superseded by the project copies.)
- FEATURES are the one kind of content that is not a prop: a mesh stamped IN
  PLACE OF a surface block, into that block's own variant bucket, so it wears
  the cell's texture and IS the surface rather than sitting on it. Two
  catalogs, the same idea turned 90 degrees: `niche <type> <x> <z> [facing]`
  replaces a wall panel and `bore <type> <x> <z> <axis>` a whole wall block
  with a see-through window (wallfeatures.cat; a `bore = 1` entry is the
  latter), and `floorfeature <type> <x> <z>` / `ceilingfeature <type> <x>
  <z>` replace a cell's FLOOR or CEILING block (surfacefeatures.cat - ONE
  catalog for both, its `surface = floor|ceiling` field the only difference:
  `recess`, `drain`, `cracked` below, the `vault` above; the RECORD keyword
  names the surface so DungeonMap's parser stays catalog-blind, and an
  untyped record is `recess`). `pit_ceiling` is not one of these - it is a
  stairs.cat type. REACH FOR A FEATURE, NOT A
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
  aspect correction at STAMP time (`floorUAspect` / `ceilingUAspect`), never
  baked. And because it
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
  keystrokes + PrintWindow screenshots into docs/ (dot-source
  `docs/drive.ps1 -GamePid <pid>`: Key/Send/Click/Shot helpers, client coords;
  ALWAYS send the keyup or the next keydown of that key won't register as
  pressed). Menu nav: Down/Enter; allow ~10s+ load on
  High/Ultra cold cache before sending keys.
- SEVERAL SESSIONS RUN AT ONCE (Michael, 2026-10-02), each with its own game, so
  EVERY harness and driver: (1) sends input to a window BY PID - the game it
  launched (keep `Start-Process -PassThru`'s Id), or one picked by THIS
  worktree's exe path - never `Get-Process Dungeon`'s first hit, the foreground
  window or a title search; and (2) captures screenshots with PrintWindow
  (PW_CLIENTONLY | PW_RENDERFULLCONTENT - the second flag is what gets a D3D
  swapchain instead of black) on that HWND, NEVER CopyFromScreen or any desktop
  grab, which photographs whatever window is in front. Kill by PID too. A
  harness that refuses to run beside another game must say why (ProfileTest
  does, on purpose: a second game on the GPU would be measured). drive.ps1
  refuses rather than guess when it cannot tell which game is its own.
  THE ONE COPY of that plumbing is `tools\HarnessGame.ps1`, dot-sourced by
  AllocTest, InGameTest, HealthTest, ProfileTest and TypingTest (and by
  drive.ps1 for its window type): Start-HarnessGame finds the window by PID AND
  window class (`Process.MainWindowHandle` can be a debug build's console
  window) with a 30 s retry; Start-NewGame starts through the console's
  `newgame` - never Enter on the title, which is Continue on the newest shared
  save - waits for `Level ready:`/`New game started` (never `Game loaded:`,
  which lands while commands are still refused) and refuses a run that loaded a
  save; Wait-ConsoleReady counts only NEW log lines, so an echo the title
  screen already wrote cannot satisfy it; Stop-HarnessGame quits, else kills
  that process. A new harness dot-sources it rather than copying a helper.
  EVERY HARNESS REFUSES A STALE EXE (`Assert-ExeCurrent`, Python
  `harness_game.refuse_if_stale`): it asks ninja for a dry run - through a COPY
  of build.ninja, because the CONFIGURE_DEPENDS globs make a plain `ninja -n`
  stop at "Re-running CMake" every time - and exits 4 if anything is pending
  (3 = this worktree's game is already running, 2 = no build or an argument the
  judge does not know; none of the three is a FAIL). `DN_HARNESS_ALLOW_STALE=1`
  runs a stale exe on purpose, with a warning. CheckAll builds what a selection
  runs on, first, whatever the selection (`-Only alloc` used to judge
  yesterday's binary); `CheckAll -Plan` prints the order without running
  anything. `tools\StaleTest.ps1` checks both.
  THE NATIVE JUDGES (RollTest, DiagTest, ThreadStress, Bc7Test) share one tally
  and one LAST LINE, `tools\Common\Verdict.h`: `<tool> RESULT=PASS|FAIL checks=N
  failures=M [fields] self_test=0`, or under `--self-test` `RESULT=FAIL ...
  self_test=1 caught=1` - the checks failed on the injected fault, exactly the
  expected ones, and the exit code follows `caught`. CheckAll and Bc7Test.ps1
  read that line back against the exit code (`tools\Verdict.ps1`) and fail a
  run whose line is missing, counted nothing or disagrees (code-review C425).
  That reader is judged in turn by `tools\VerdictTest.ps1` (CheckAll's quick
  `verdict` row): every refusal, by name, since a reader decayed to trusting
  the exit code would leave every other row green; `-SelfTest` plants exactly
  that decay. A tool's own extra self-test conditions go INTO the summary
  (`verdict::CaughtExactly`'s `otherMisses`, Bc7Test's raised baseline), so the
  `SELF-TEST PASS|FAIL` line never contradicts the `caught=` after it.
  WHAT THE TIERS RUN is `CheckAll.ps1 -List` (quick adds RollTest, `docs` and `verdict`;
  full adds Eval's runner self-test, EditorTest, WorldTest, LevelBuildTest,
  QuitTest and StaleTest), and the /check-* command files carry those lists GENERATED by
  `tools\CheckDocs.ps1` - after changing a row, run it with `-Write`, or the
  quick tier's `docs` check fails.
  A PYTHON JUDGE NEVER EDITS THE REAL WORLD (code-review C431): EditorTest
  (each phase on a fresh `et_demo`), WorldTest (one `wt_demo` for the run) and
  LevelBuildTest (`lb_*`) work on SCRATCH COPIES of dungeon-demo opened with
  `-project` (`harness_game.scratch_world`; `remove_world` refuses a world git
  tracks, and FAILS CLOSED - a git that cannot answer raises rather than reading
  as "untracked" - and also clears the `.building-<name>` a killed create
  leaves). EditorTest used to edit dungeon-demo and restore it by delete-then-
  copy, and a kill between the two once emptied it; a killed run now leaves a
  scratch folder the next run clears. The style library has one fixed home, so
  EditorTest phase 16 still changes it - behind `harness_game.back_up`, which
  RESTORES from a backup it finds (a killed run's, the only clean copy) instead
  of deleting it, and `restore` writes in place before it discards the backup.
  A folder under the backup's name is ALWAYS WHOLE: it arrives by a rename (from
  `.partial`) and leaves by one (to `.discard`, then deleted), because rmtree
  goes a file at a time and a kill during it once could leave a half-deleted
  "clean copy" for the next run to restore from - deleting every real file it
  had lost. A judge's saves are THIS WORKTREE'S (`save_name`:
  `worldtrip_<worktree>_<hash>`, written into a copy of the script by
  `eval_script`) - Documents\DungeonSaves is shared with every session and
  Michael's play - and only those are deleted. Each ends with `harness_game.RealTree.check`, whose
  baseline is taken BEFORE the run clears up after a killed one (taken after,
  a damaging recovery became the baseline and passed): every other real world
  and the library byte for byte as found (a library whose backup stood: as the
  backup holds it), nothing new in `git status` under assets/projects and
  assets/library, and no status line naming the judge's own scratch worlds, a
  killed run's included. LevelBuildTest refuses a phase it does not have
  (`LevelBuildTest 9`, exit 2 - nothing ran) and a run that ran no check; a
  judge that ran a game and stops early (WorldTest's unclean baseline) is a
  FAIL, exit 1.
- TYPED TEXT IS ONE ORDERED STREAM (Platform/Input.h `TypedChars`): printable
  characters plus `Input::kTypedBack` / `kTypedEnter`, pushed by OnKey on the
  press, so a consumer applies Backspace and Enter WHERE THEY FELL (the console,
  ui::TextField and the editor's filter box all walk it; do not go back to
  reading Enter/Backspace as key edges beside it - a heavy frame batching
  `...t<Enter>s` ran `...ts`). The view is fixed at BeginFrame (end of
  PumpMessages); EndFrame clears only what the frame showed, and focus loss
  (ClearAll) never clears it - clearing it there drops exactly one queued
  character, the likeliest cause of `sheet status` arriving as `shee status`
  on 2026-09-30 (another session's harness taking the foreground). Nor does it
  clear the frame's key PRESS edges (code-review batch 8): that lost a harness's
  console toggle posted in the same pump as a focus change, leaving the console
  shut, and every command after it went nowhere. THE TEXT IS WHOLE UTF-8
  CHARACTERS (code-review C383): OnChar joins a surrogate pair and encodes it,
  where it used to keep the UTF-16 unit's low byte (u-umlaut drew '?', Cyrillic
  became control bytes, c-caron U+010D was an Enter). A consumer walks
  `utf8::CharAt` (Core/Utf8.h, pure, in RollTest), deletes with `utf8::PopBack`
  and caps with `utf8::Length` - a TextField's `maxLength` and a member name
  (`party::NameValid`) count CHARACTERS; text set from outside a box (the
  palette filter's `SetFilter`, the party page's `SetName`) is cut with
  `utf8::Prefix`, never `substr`. Checked by `tools\TypingTest.ps1` (FOCUS /
  ORDER / TOGGLE / UNICODE phases; CheckAll full tier; `-SelfTest` types the
  `inputpoke` dev command into every phase and requires EACH phase to fail, not
  just one, with no save touched; the poke drops WHOLE lines, since a fragment
  completed by the type-ahead once ran `save inputpoke` into the shared
  DungeonSaves). The harnesses post through PostMessageW, or a WM_CHAR past
  ASCII is converted from the ANSI code page on the way.
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
  load-or-dies on derived/imported assets that are gitignored. There are TWO
  that are fatal, plus the portraits, confirmed with `git status --ignored
  --porcelain assets` (a
  missing-texture magenta-placeholder fallback exists, but there is NO fallback
  for models — a missing `.glb` aborts hard at level load, e.g.
  `AssetUtil.cpp model.has_value()` "failed to parse glTF: ...viking_dagger.glb").
  Copy both from a populated sibling worktree (e.g. `C:\Dev\Dungeon`). There is
  NO third step: the game reads `<worktree>\assets` directly, so nothing is
  mirrored into `build\<cfg>\bin` and a worktree costs one copy, not one per
  config:
  - `assets\textures` — whole dir, BOTH `.dds` (BC7) and source `.png`. The
    `.dds` is what renders; the `.png` is the source a missing or rejected
    `.dds` falls back to (the old "dds-only renders magenta" note was a symptom
    of the reader bug below, not a rule).
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
    come with the checkout copy identically, so it's safe and future-proof, and
    it also brings the `<model>.<n>.dds` embedded-image sidecars; without them
    the game decodes those images instead, slower but correct).
  - `assets\portraits` - whole dir (thousands of bought `.png` + `.dds`; only
    the catalog and the four starter faces are tracked). NOT fatal when missing:
    a member whose image is absent draws the tinted initial. Copy from a
    populated sibling, or run `tools\FetchPortraits.ps1` (about two minutes).
  Use BACKSLASH paths (robocopy rejects forward slashes → copies nothing) and
  VERIFY with a file count afterward — robocopy returns exit 0 when it copied
  NOTHING (exit 1 = files copied), so a "successful" run can leave you empty. The
  regenerable alternative is `tools\FetchTextures.ps1` + `FetchModels.ps1` (+
  `FetchPortraits.ps1`) in the background (needs `build\<cfg>\bin\AssetBaker.exe` first, so build once). Symptom
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
- NEVER PATCH A FILE THROUGH A BASH HEREDOC (`python - <<'PY'`, `cat <<EOF >
  file`, a heredoc'd sed/perl script). This environment strips ONE level of
  backslash on the way in even from a quoted heredoc, so `"\\n"` lands as a
  real newline and `"\\project.ini"` as `"\project.ini"` - and the script still
  prints "ok". Five real bugs so far, every one silent. Every file change goes
  through the Edit/Write tools. If a mechanical multi-site edit genuinely needs
  a script, WRITE the script to a .py file in the scratchpad with the Write tool
  and run it by path (no heredoc anywhere in the chain), splice by line index
  rather than by escaped literal, and read back the changed region before
  building. ENFORCED: `.claude/hooks/block_heredoc.py` (a PreToolUse hook in
  `.claude/settings.json`) refuses any Bash command carrying a heredoc - an
  operator naming a delimiter plus a later line that is that delimiter alone,
  so a `<<` inside a grep pattern still passes. Commit messages go through a
  Written file and `git commit -F`.
- NO EM-DASHES in anything new - code comments, docs, commit messages, log or
  lang strings. Use a plain ASCII hyphen `-` (or ` - ` as a separator). The
  em-dash is the one non-ASCII character the codebase kept producing, and it is
  what every code-page and encoding trap here mangles. Existing ones can stay;
  just don't add more.
- User prefs: concise replies, no emojis; permission prompts disabled.

## Editor updates (editor-updates branch; docs/editor-updates-plan.md)

Built from Michael's notes after editing the crypt levels ("awkward and
clunky"); the notes, his answers and the plan are in docs/editor-updates-*.md.
The judge for all of it is `tools\EditorTest.py` (11 phases then, 18 now, each
mutation-tested and each on a scratch copy of dungeon-demo - see "A PYTHON JUDGE
NEVER EDITS THE REAL WORLD"; the eval harness only REPORTS). What exists now, and
the rules it rests on:
- ONE SURFACE RESOLVER: `ResolveSurfaceVariant` (DungeonMeshBuilder) answers
  "which texture does surface S of cell (x,z) show" for the mesh builder, the
  map overlay and the editor. Never re-derive it at a call site - that is how
  the scene and the map came to have three copies. `geomhash` (and its
  `geomlayout` line: uploaded chunks vs a fresh bake, "deferred" after an undo)
  checks a change that must not move a vertex.
- STROKES are press-to-release (`MapEditor::BeginStroke/EndStroke`): one undo
  step per drag, and the release is the "edit ended" moment. `DungeonWorld::
  EditRevision()` is the "something changed" signal - bumped by kept undo
  steps, undo/redo, history clears AND the unbracketed edits (inspector apply,
  type writes); a new edit path that takes no undo step must `NoteEdit()`.
- NEVER STASH TO READ. A stashed level is one `savemap` rewrites, so anything
  that only READS other levels (Validate, `typerefs`/delete refusal counts,
  RefreshTheme's "who uses this?") goes through `m_readOnlyLevels`
  (ReadOnlyLevelOf, re-parsed when the file's write time moves). Both Check and
  the type-usage count used to stash every level.
- Multi-cell fills batch chunk rebuilds (`BeginChunkBatch/EndChunkBatch`,
  nesting): ~10x on an 80-square fill.
- TOOL STRIP (MapView_Tools.cpp): Paint / Rectangle (drag) / Flood / Area /
  Eyedropper + Fill level, beside the palette; Shift/Ctrl/Alt borrow Rect/Flood/
  Pick. AREA = `Game/Area.h` (pure): a walkable cell in any 2x2 walkable block
  is OPEN, else NARROW; an area is the 4-connected run of the clicked class
  (so a room stops at its doorways, a corridor turns its bends; a 2-wide
  corridor counts as room). Icons are drawn by `tools/BuildToolIcons.py`.
- LIVE VALIDATION: Game::RefreshLiveIssues re-runs the checker when the edit
  counter moves and no button is held; MapView_Issues.cpp boxes findings
  (red/amber), tooltips them, and badges Check with the cell-less count.
  `validate::Issue::also` lists extra squares to box (a stair's far end, every
  lost item). `editor issues` prints the boxes.
- SURFACE THEMES (were "combinations"; editor-themes branch, docs/editor-themes-
  notes.md): `themes.cat` (world-wide), ONE wall + ONE floor + ONE ceiling id per
  theme (empty = leave that surface be; a legacy list keeps its first word). The
  Themes palette brush RECOLOURS and never changes a cell's type: an open square
  takes floor + ceiling, a wall block the wall - by click, drag, rect, flood,
  area or fill level. A cell REFERENCES the theme: a variant <= -2 is theme slot
  (-2 - v) in the level's list, written as `theme <surface> <x> <z> <id>` BY ID.
  Members resolve to palette INDICES on the map (only palette entries have
  textures), so painting enrols them. Editing one (`FieldKind::CatalogRefPick`:
  a tab per surface, one swatch row ticked) repaints every level using it
  (RefreshTheme). Rename sweeps slots; delete refuses while SQUARES use it; a
  member type's rename is swept into themes.cat and the maps.
  NAMING: "theme" used to ALSO mean a level's content tags (undead, stone). That
  concept is now TAGS everywhere - the .map `tags <tag> ...` record,
  DungeonMap::Tags(), the Level dialog's "Tags" row, the generator's `tag:` knob
  and Tags tab, the wizard's `tag=` and `worlds tags`. ui::Theme (UI colours) is
  a third, unrelated meaning and stays.
- WORLDS: `assets/templates/default` (built by `tools/BuildTemplate.py` from
  dungeon-demo, minus places/provenance/quest hooks; outside projects/, so never
  listed) is what a BLANK world starts from. `Game::CreateWorld(name,
  NewWorldSpec)` (Game_NewWorld.cpp): blank / copy this world (unsaved edits
  written into the COPY via `DungeonWorld::LevelTextFor`, never saved here) /
  copy one level (stairs replaced by one exit) / WIZARD (template content + one
  generated floor; `GenerateWizardLevel`, deterministic from its knobs). Built in
  a hidden `.building-<id>` folder and renamed into place (Project::List skips
  dot-folders). The NewWorldDialog opens from a disc on both toolbars and the
  Worlds dialog's "New world...". A starter or wizard floor has an exit stair.
- Console added: `editor drag|fill|rev|tool|issues|cell|pick`, `geomhash`,
  `typeset [rename|delete]`, `worlds new <n> [blank|copy|level <s>|wizard ...]`,
  `worlds tags`, `worlds newdialog ...`.

## Item mouse buttons, status bar, details dialog (ui-updates branch)

Michael's notes and answers: docs/ui-updates-notes.md; the plan: -plan.md.
- THE ITEM MOUSE MAP, everywhere an item appears (sheet backpack / doll / bag
  row, the party inventory, floor items): LEFT unchanged (pick up / put down /
  swap), RIGHT = the item's DETAILS, MIDDLE = its USE menu (what right-click
  used to open). EXCEPT THE HUD HAND BOXES (Michael, 2026-09-30, after trying
  it): they are controls, so RIGHT still opens the use menu where a hand's
  default is set, as before; middle opens the same menu. A SET hand box shows a
  low flat accent tint plus a soft centre glow (assets/ui/glow_radial.png,
  made by tools/BuildGlow.py); a spell's runes sit in ROWS OF TWO, each the size
  two across leave it (a 3rd/4th rune takes the next row). Off the
  hand the menu offers only memorize/eat/drink (`IsOffHandUse`); nothing to
  offer = no menu and `log.no_use` ("Brand finds no use for that item.").
  Eating from the pack is new (`GameUI::EatSlot`, allocation-free). An item's
  place is an `ItemPlace` (PartyHudTypes.h: Doll/Pack/Bag + index), resolved
  AGAIN at pick time - a slot that changed under an open menu is not acted on.
  The use-menu code lives in GameUI_Items.cpp (split out of GameUI.cpp).
- ESC CLOSES A POPUP FIRST (`GameUI::DismissPopup`: the dialog, else an open use
  menu) before it closes the sheet or pauses. It used to close the SHEET with a
  menu open, and the stale menu then ate the next click on reopen.
- STATUS BAR (CharacterSheet_Status.cpp): one line along the sheet's foot naming
  what the pointer is over, on every tab - an item's name + weight (a bag with
  its contents; the cursor's item over nothing), an attribute / bar / skill with
  its `<key>.hint`, a spell or effect with its description. The panel GREW for
  it: `CharacterSheet::kBodyH / kStatusH` are the split, and every layout
  fraction resolves against `Body()`, which is also the sheet's ContentRect, so
  the tabs kept their exact pixels. A body helper that reaches for `Pixel()` is
  a bug now. `sheet status` prints the bar.
- DETAILS DIALOG (ItemDetailsDialog.*): the item turning slowly in 3D (a turn per
  20 s) beside only the lines it has, plus its `item.<id>.desc`. BUILT ONCE (a
  right-click lands in a guarded frame); Open fills reserved strings and hides
  absent rows (a Stack skips an invisible row). The data comes through
  `DungeonWorld::ItemDetailsFor` into the pure `ItemDetails` struct (views, no
  allocation) and the model through `ItemPreviewForType` into a fixed buffer.
  Game renders it into the editor's `m_modelPreview` AFTER the scene (the editor
  dialogs' preview replaces the scene pass; this one must not) and only while no
  editor preview holds that target. Modal for the mouse, not the keyboard; the
  world keeps running. Its footer has a MEMORIZE button (spell-updates), shown
  only when opened on a member's own rune that member does not know. That rule
  is `GameUI::CanMemorize`, and it is the ONE test for every place Memorize is
  offered - the hand menu and the pack / doll menu skip the row too, and
  `MemorizeSlot` refuses a known rune rather than spend the tablet. Dev:
  `itemdetails <item [kg]|pack <member> <slot>|memorize|off|status>` (status
  prints `memorize=`); judged by SpellTest's MEMORIZE checks.
- CHECKED: `AllocTest.ps1 -Sheet` (hover, all tabs, a right-click open, the
  spin, the menu - inside the window; refuses a PASS with no open counted). It
  found `ModelPreview::Render` building its light rig every frame, which was
  harmless while only the editor drew a preview. `uioverlap` covers the dialog
  (mutation-checked: the old column split's resist line was flagged).
- MOVING AN ITEM ALLOCATES NOTHING (2026-09-30). The cursor's item is a
  `HeldItem` (Game/Inventory.h), NOT a `std::optional<std::string>`: it reads
  like one (has_value / * / reset) but its string lives as long as the cursor,
  empty = nothing held, and every pick, put and swap with a slot is ONE
  `SwapWith` - copying an id constructs a string, which the debug CRT allocates
  for at any length. `Inventory::Stow(HeldItem&)` is the portrait quick-stow;
  `TryPickItem` returns the kind's own id (a pointer). Every item KIND is built
  at load (`DungeonWorld::PreloadItemKinds`) - runes used to be built on their
  first drop, 2 MB in a guarded frame - and a drop reuses a dead runtime drop's
  slot (`PlaceDrop`) inside load-time headroom (`ReserveDropRoom`). A bag's slots
  are a `PackSlots` (fixed capacity, `kMaxPackSlots` = 16, all strings built up
  front), not a vector, so equipping a bigger bag only moves a count; a catalog
  `capacity` past the cap is clamped with a warning. CHECKED: `AllocTest.ps1
  -Items` (pack -> floor -> pack through the inventory window; dev `inventory
  [off|status]`; tally `drops=`/`lifts=`; before its game, itempose.eval - the
  item pose and the click picks; CheckAll's `alloc-items`), mutation-checked
  both ways, and
  `-Packs` (a 4- and an 8-slot bag swapped in the pack row; `sheet status`
  prints the row and an `equips=` count), which FAILed on the vector first.

## RESOURCE BARS (icon-updates branch; docs/icon-updates-notes.md + -plan.md)

The health / stamina / mana bars (party bar AND sheet; the sheet's food/water
too) are an iron FRAME around a PROCEDURAL, ANIMATED, EMISSIVE fill.
- FRAME: `assets/ui/bar_frame.png`, cut from the bought UI kit's "Life Status
  Bars (1)" by `tools/CutBarFrame.py` (committed: the script is the asset). The
  kit is opaque on black and the iron is nearly black, so the key is a TIGHT
  1..3 brightness ramp; the tube is punched per column (snapped to one line, a
  running median otherwise - a ragged hole reads as chewed iron). The script
  PRINTS the tube insets + cap slice points, which PartyHudDraw.cpp holds as
  constants - re-cut, copy the numbers. Drawn 3-SLICE (caps at their aspect, the
  plain rim stretched). The rect a caller passes is the GLASS; `FrameReach` says
  how far the frame sticks out, and the LAYOUT must make room - on the party
  bar the frames stay inside the member's slot (Michael: the chrome overlapped
  the container), which is why StatsArea sizes tubes from the reaches.
- FILL: `SpriteBatch::DrawBarFill` - a second PSO (`assets/shaders/bar.hlsl`,
  premultiplied blend, its own vertex: kind/fraction/beat/seed + tube aspect/px).
  Switching sprite <-> fill FLUSHES, so draw order is still submission order;
  stacks draw every fill, then every frame (`DrawResourceBarFill/Frame`). Health
  = blood ebbing + a heartbeat; stamina = a breathing green glow; mana = blue
  wisps + an occasional lightning strike; food = packed grain (mottle, speckle,
  lighter kernels, nearly still, a crumbly end with no meniscus); water = clear
  TEAL (mana owns blue, a row away), lighter at the surface, soft pools of
  caustic light (thin threads read as mana's lightning), small bubbles, a
  gentle slosh (BarKind Food = 5 / Water = 6).
  Brightness falls with the stat. Tuning lives at the top of bar.hlsl (edit +
  relaunch): `kPace` / `kSubdue` exist because the first cut was "too busy - it
  draws the eye". TRAP that cost a round: a frac(dot) FLOAT HASH disagrees with
  itself across a cell boundary by a rounding ulp, which drew drifting vertical
  seams in every fill - noise lattices hash INTEGERS (`HashLattice`).
- HEARTBEAT: `HeartRateTarget` (PartyHudDraw) - rest 60, NOTICED 120, near
  death (<30% health) slides to 35 and WINS over noticed, down = no beat. The
  phase is integrated on the CPU (`BarPulse`, `TickBarPulse`, eased), never time
  x rate in the shader, so a rate change never jumps the beat. Everything runs
  on REAL time (`GameUI::TickResourceBars`), not the world's (rest is 60x).
  NOTICED = `DungeonWorld::PartyNoticed()`: a live monster that is `aware` AND
  not Idle. NOT the stabilize clock's `danger`, which is distance through walls
  and was true the moment a new game began beside a crypt of sleepers.
- `ResourceBarStyle` (PartyHudTypes.h, owned by GameUI) replaced the user
  `ResourceBarColors` + `kBarFields` + the Settings > UI picker grid. uiskin=0
  keeps the flat `DrawStatBar` look.
- SKILL BARS (ui-bars-updates, docs/ui-bars-updates-plan.md) are PROGRESS bars
  - the way to the next level, empty on every level gained - in the same frame
  with their own fill, `BarKind::Progress` (`DrawProgressBar`): the caller's
  colour as a glow brightening toward the leading edge, NOT dimmed as it empties
  (empty = just levelled). Coloured by skill FAMILY (`SkillBarColor`,
  CharacterSheet_Lists.cpp): magic by school, weapons steel, defence bronze,
  each reserve its pool's colour - Michael picked it over a grade-by-fraction,
  which was deleted. The Skills rows are SkillBand tall so the whole frame fits
  round a glass 0.8 of the text height; any framed bar in a row goes through
  `FitFramedTube` (the Stats tab does). `setskill <m> <skill> 2.5` sets a
  fractional level for showing one part-full.
- BRIGHTNESS / SATURATION are USER SETTINGS, not shader constants (Settings ->
  UI "Resource Bars"; settings.ini bar_brightness= default 0.7, bar_saturation=
  default 1 - Michael picked both from side-by-sides, 1.0 brightness read
  cartoonish). They ride the sprite root constants' SECOND float4
  (`SpriteBatch::SetBarLook`, refreshed each frame in TickResourceBars);
  sprite.hlsl declares only the first, so the root signature carries 8 values.
- Dev: `hudbars [status]` (bpm per member, noticed), `hudbars demo on|off`
  (sweeps every bar), `hudbars rate <bpm|auto>`. Checked: AllocTest (default +
  -Sheet) PASS; `uioverlap hud` clean (it sees widgets; the bars are direct draws).

## Stone chrome, floating panels, Minimal layout (ui-panels branch)

Michael's Grimrock 2 brain dump, organized, answered and planned in
docs/ui-panels-notes.md / -plan.md. What exists, and the rules it rests on:
- STONE IN LAYERS (UI/Skin.h). A skinned face is a seamless STONE tile, tiled
  on a grid anchored to the SCREEN (so a slot reads as cut from its panel's
  slab; the sprite sampler clamps, so it tiles in quads, one per grid cell a
  face touches), under a stone-independent BEVEL overlay that carries only
  light (white top/left, black bottom/right, a dark rim, a dark well for a
  slot), plus a stretched sheen on panels. ONE entry point: `ui::DrawFace(batch,
  rect, skin, Face::Panel|Button|ButtonDown|Slot, tint)` and `ui::FaceInset`
  for where content starts; `ui::DrawSlotFace` is THE item socket (HUD hands,
  sheet doll / pack row / backpack / effect icons, party inventory, empty rune
  cells, the loading bar's track). A NEW socket or button goes through these -
  never a flat kSlotBg rect or a raw DrawNineSlice. The flat look stays whole as
  the debug mode (uiskin=0); editor dialogs never receive the skin.
  Assets are script-made and committed: `tools/BuildUiStones.py` ->
  assets/ui/stones/<name>.png (1024, resized WHOLE - a crop breaks the wrap -
  and toned to a mean luminance PER STONE - ~0.2 the dark set, ~0.3 the light
  one; `--check` reports the seam; adding a stone = a table line + a re-run + a
  `stone.<name>` key x5; it also writes thumbs/<name>.png and stones.cat, each
  stone's luminance) and `tools/BuildUiFrames.py`
  -> assets/ui/frame_*.png + sheen_panel.png (2x, drawn at 0.5 x the window
  scale - GameUI::UpdateSkinScale beside the fonts; the `inset` numbers in
  LoadTitleArt must match the script). Settings -> MATERIAL (its own tab,
  more-ui-updates; GameUI_Stone.cpp + Game/StonePicker.h) shows the folder as
  a grid of thumbnails, filtered by name, kind (stones.cat `family`: stone /
  wood / forest / snow / rock) and shade (`luminance`, light from 0.25), and
  swaps the stone live (`LoadStone` WaitIdles before the old one dies). The
  grid WRAPS, so it is a `Len::Fit` row: a Stack asks the row's
  `Widget::FitExtent(crossPx, remPx)` for its length, for content whose height
  depends on its width. THE PLACE PICKS THE MATERIAL (Michael): ui_stone=follow
  is the default (the grid's first tile) and resolves to the active level's
  `.map` `uistone <name>` record, else its dungeon's dungeons.cat `ui_stone`,
  else granite_grey; any other tile PINS one everywhere. Game::RefreshPlaceStone
  re-resolves when the level or EditRevision moves (a settled frame only
  compares) and GameUI::ApplyStone reloads only when the NAME changes. The
  editor's Level settings dialog authors `uistone` with a LIVE PREVIEW
  (GameUI::PreviewStone wins while the dialog is up; every way out ends it, so
  Esc reverts and Save keeps) and a sample strip drawn in the game skin, since
  the editor's own chrome is flat and would show no change. The folder and ini
  key keep the word "stone" on purpose - renaming them would churn every ini.
  ui::DropDown grew two options there: `icons` (a picture per row; rows grow
  to `iconRowScale`) and CATEGORY BUTTONS (`filterLabels` + per-item
  `itemFilters` bit masks + `filterColors` chips, drawn at `filterScale`,
  pinned above the scrolling rows, the list widening so they fit two lines).
  ItemRect takes a SLOT among the shown rows, not an item index.
  CUT STONE (more-ui-updates P2): a button with `etch` set (and a skin with the
  `block` part) is drawn by `ui::DrawCutStone` - Face::Block / BlockDown
  (frame_block*.png, BuildUiFrames.py: a flat CHAMFER via light_exp /
  shadow_exp) with an etched symbol over it (etch_<name>[_lit].png,
  tools/BuildEtchGlyphs.py: the glyph's DISTANCE-from-edge as a V-groove depth,
  scaled by the stroke's half-width - not the deepest texel, or the gold pools
  in the joints - lit from the top-left, gold on the floor only). THE GOLD IS NOT
  BAKED IN (code-review C204; it used to be, the dark-stone gold, so the etches
  skipped the ink solve and read 1.2:1 on the snows): each PNG is THREE square
  panels side by side - the groove (grey + alpha, light only), a white mask of
  the gold floor, a white mask of its lit slope - and DrawCutStone draws them in
  turn over one square, the masks tinted `ui::EtchInk` (= CarvedGold, CarvedLit
  for a lit tab) and `EtchSheen` (that ink x `kEtchSheen`, which MUST equal the
  script's TONE_MAX). The script SOLVES the panels from the one-image look, so
  any ink draws what the baked etch drew with it (`--check`: exact to 0.84/255
  where no channel clips, and that the files are current). `active` holds it
  down AND swaps in `etchLit`.
  `Button::fireOnPress` acts on the press (the push still plays; `m_fired`
  stops the bottom of the sink acting again); `PressVisual()` plays it with no
  action. The movement pad uses all three and watches `Party::ActCount` so a
  KEY move presses its stone. Checked by `AllocTest.ps1 -Walk` (key turns in
  the window; refuses a PASS under 4 `moves=`, a field of the verdict line).
  CARVED WORDS (P4): `ui::DrawCarvedText` cuts a word into stone (a 1 px dark
  offset up-left, a faint lit one down-right, the fill between - keep both
  offsets ONE pixel and the lit one faint, or it reads as a blurry echo) in
  `ui::CarvedGold` / `CarvedLit` / `CarvedTitle` / `CarvedPlain(skin)`. THEY
  FOLLOW THE MATERIAL (the contrast pass): they are SOLVED against the stone's
  mean colour (`ui::ResolveInks` -> `Skin::inkGold` & co.): an ink is kept where
  it reads at WCAG 4.5:1, else moved toward pale gold or dark bronze, whichever
  gets there with the smaller change (Michael, 2026-10-02: the gold blended into
  the party page's buttons - the old brightness-only rule darkened it on the
  mid-toned materials, the wrong way, and left the snows at 1.2:1). Hover stays
  brighter than the gold; disabled fades toward the stone (`CarvedDisabled`).
  The solve logs `ui material <name>: gold r,g,b reads N:1` on every change - with
  the weakest etched symbol's gold floor beside it, as drawn and as the authored
  gold would draw it (ui::MeasureEtchFloor, taken from each etch PNG as it
  loads) - and `uimaterial <name>` previews one; `uimaterial sweep` logs every
  material's line without showing any (its header counts the etches measured,
  so a missing or refused one cannot hide), and InGameTest judges it (every etch
  measured; on every material the etch reads no worse than the authored gold,
  and better wherever the solve moved the gold). The sweep reads the inks
  through EtchInk and never sees a sprite, so DrawCutStone also RECORDS what it
  last painted, plain and lit (`ui::LastEtchDrawn`, reset on a material change),
  and `uimaterial drawn` prints it beside the solved inks: on snow_packed
  InGameTest photographs the movement pad into build\<cfg>\bin\shots (a look,
  judged only as taken), then opens the sheet (its current tab draws lit) and
  holds each panel's window and colour to them there - where the gold moved -
  and on granite_grey, where the lit slope is not clamped white. The console
  must be SHUT a moment first: an open console owns the frame, the UI is drawn
  but not updated, and no tab is ever current. GameUI::ApplyLegibility also
  rings small text harder there and sets `Skin::calm`, a wash of the stone's own mean colour over
  every face but a slot, from stones.cat `detail` (a band-pass of the baked tile
  at glyph scale; `BuildUiStones.py --index-only` rewrites mean + detail from the
  tiles on disk, no archive needed). Dark calm materials are untouched. Dev
  `uimaterial <name>|off|list|sweep|drawn` previews one without saving. The pause and title menus are
  `Game/MenuPanel` (a card sized in rem from its entries, the pause title carved
  on it) holding a skinned `ui::MenuList`: cut stones that sink on the press and
  ACT ON RELEASE (drag off cancels; Enter presses the selected one), the
  selected lit with a gold hairline. The save, load and world pages are a
  `PageCard` (title carved, children in one Stack - GameUI::SavesCard /
  SavesBackRow) with `Button::carved` stones and SlotList rows as cut stones
  that push the same way; on stone the delete mark is a CARVED cross, red only
  under the pointer (Michael: the red icon was "too loud"). The list's delete
  confirm claims the pointer through ClaimPopup, since the Back stone is added
  after the list. Those pages draw no subtitle: the card carries the title.
- AN OPEN POPUP OWNS THE CLICK (`UIContext::ClaimPopup`): the update walk visits
  children in REVERSE add order, so a control added after a drop-down saw a
  press on its open list first (picking a stone unticked Head bob). An open
  drop-down / colour picker renews the claim each frame and the next walk starts
  with the pointer and wheel claimed; its own open branch never asks
  IsMouseConsumed. A click outside an open list now only closes it.
- FLOATING PANELS (UI/FloatingPanel.h). A FloatingLayer places FloatingPanel
  children from POINTERS to a saved spot (top-left, window fractions; < 0 = the
  panel's `defaultPos`) and scale; `size(ctx, scale)` is the CONTENT'S (a dock's
  height follows from its width), the scale becomes the subtree's fontScale
  (so content measures detail in EM - a new widget in a dock or card must too),
  and `onChanged` fires when a drag ends. HOLD CTRL TO ARRANGE (Michael,
  2026-09-30 - the grips used to show on every hover): with Ctrl held over a
  panel it is outlined in the accent and takes the WHOLE pointer before its
  content (UpdateBeforeChildren) - a drag anywhere moves it, the bottom-right
  wedge scales it, and a RESET button at its top-right puts every panel home
  (`FloatingLayer::onResetAll` -> GameUI::ResetHudLayout, which only marks the
  Settings sliders stale - it runs in an armed frame). A started drag runs to
  the release with or without Ctrl. Without Ctrl a panel is just its content.
  SNAPPING: a moved panel's edges (either edge, onto either edge - lining up and
  butting up) catch any other shown panel's or the window's within half a rem;
  a resize solves the SCALE for its right or bottom edge (a dock's height steps
  with its font, so a solve that cannot land within 1.5 px is no snap). Accent
  hairlines show the caught edge. The sheet's layer snaps to the HUD layer's
  panels too (`FloatingLayer::snapPeer`). The pointer turns to Window::
  SetCursorShape's four-way / diagonal arrows: GameUI records what the panels
  want (PanelCursor) and Game::Update sets the cursor ONCE a frame from it and
  the editor's dock-edge arrow (GameUI::TakeHudCursor) - two writers made the
  editor's arrow flicker back. A panel claims the pointer over its whole rect (a click on
  a dock's padding used to reach the 3D view). `Scale()` clamps to the panel's own
  min/max (the sheet stops at 1.3), whatever the slider's 0.5..1.5 stored.
  THE PANELS are kHudPanelFields (GameSettings.h: party, status, move,
  hands, magic, cards, inventory, tray, sheet - the SHEET LAST, since it alone
  lives in another context and [0, kHudSheet) means "the HUD's"); each a
  HudPanelLook {x, y, scale, opacity, hidden} in settings, a Settings -> UI
  scale + opacity pair, and covered by Lock / Reset. Untouched, every panel sits
  exactly where the old fixed layout put it (the defaults keep its rules: the
  column starts under the party bar's height at its scale, a dock's default top
  follows the SHOWN docks above it and Magic fills what they leave - a minimized dock closes up the column - Magic shows once a member
  knows a symbol). Dev `hudpanel [list] | <id> <x> <y> [scale] | hide|show <id>
  | reset | lock on|off | layout standard|minimal`, `inventory [off]`.
  THE CLOSED-PANELS TRAY (ui-updates Phase 8, Game/HudTray.h): a panel whose
  kHudPanelFields row names a `glyph` MINIMIZES - the docks by their header
  button, every one by a Ctrl button in its top-right corner (reset moved in
  beside it) - and is then not laid out or drawn at all (FloatingPanel::hidden;
  ini hud_<id>_hidden, the old hud_move/magic_collapsed load into it). The tray
  is a floating panel of its own holding a stone button per such panel of the
  current layout (face assets/ui/glyph_panel_<id>.png, BuildToolIcons.py), shown
  while that panel is minimized; it shows only while it has a button. Its
  default heads the right-hand column, right edge on the party bar's and the
  docks', growing leftward; the docks' defaults start under a strip kept for it
  (GameUI::DockColumnTop) whether it shows or not; it sits snug under the bar (TrayTop). A flag flips and the layout follows: nothing rebuilds, so it is
  free in an armed frame (AllocTest -Panels makes the trip). Reset restores all.
  The MAGIC dock's parts keep their tuned size however tall it is stretched
  (SpellbookPanel::RefH caps every vertical fraction at kRefAspect of the
  width); the extra height opens under the rune grid, for more learned runes.
- THE PARTY LEADER (ui-updates Phase 9, DungeonWorld_Leader.cpp): the member
  who does what the mouse does in the world - lift, door hand-hold, lever, and
  (Phase 10) throw. A roster index in DungeonWorld (`leader` save line, absent =
  slot 0; slot 0 leads a new game), picked by a click on a member's NAME
  (CharacterPanel's NameTag child, kNameScale 1.3 of the panel's text - in the
  bar and on a card alike) and shown in the accent over a soft glow. A leader
  who is not standing hands it to the next standing member in roster order,
  checked every frame in Update (one health test covers every way to fall), and
  it does not return. The acts' log lines name the leader; with nobody standing
  a world click does nothing. Checks of the leader's skill hang off
  LeaderMember() later. Dev: `leader [member]`.
- THROWING (ui-updates Phase 10, DungeonWorld_Throw.cpp). THROW OR DROP is
  Grimrock's screen-height rule: with an item on the cursor, a click whose ray
  meets reachable floor a thing can rest on (or an open niche) drops it
  (DropItemAt returns true); any other click - a pit or a stairwell included -
  throws it (ThrowItem; false = the leader is not ready and the
  item stays held). A THROW IS AN ATTACK (Michael): PartyAttackProfile - the
  swing's formula, shared - with the `throwing` skill and the ATTACK the item
  flies as (`throw = <attacks.cat id>`; absent = a weapon's first command,
  else the new `throw` attack, bash), potent with what is worn plus the item's
  own `powers`, dealt as a Blow (crit, fumble band, enchantment burst). SPEED is
  skill against weight (balance.cat throw_speed*). What it leaves is ItemKind::
  throwPayload: its on_hit, its own blast (the spell blast fields + `blast_type`)
  or `throw_spell`'s whole payload; `throw_breaks = 1` shatters it instead of
  landing (the fire flask bursts as firebolt_burst, the poison flask lets go a
  lingering gas). The flight is a projectile carrying the item's kind as CARGO
  (Projectiles.h - opaque to the engine; no billboard, the item draws itself
  tumbling via ForEachCargo). IT IS NEVER LOST: it lands in the struck monster's
  square, before the wall it hit (DungeonWorld::FlightEnd: the last open square,
  where a stopped BOLT's burst and on-hit land too, never inside the stone -
  code-review C43/C44), or where its range ran out, and a level change
  (StashActive) and the inspector's Remove LAND it first (LandCargo). A SAVE
  DOES NOT LAND IT (code-review C47): landing a `throw_breaks` item is setting it
  off, so a save made with a fire flask in the air burst it on the party and
  saved the damage. CaptureState writes each thing in the air as the floor item
  it will be - whole, with its charge, at its landing square (ThrownLanding, the
  rule a landing uses, `shatters` false) - and the flight flies on in the game
  being played (SaveFlyingCargo). TRAINING IS ON CONTACT ONLY (Michael,
  code-review C40): a landed blow, or a bomb bursting on a monster, trains
  `throwing`; a flask that shatters on a wall or at the end of its reach trains
  nothing. WHERE A THING MAY REST is one rule, `DungeonWorld::ItemCanRest`
  (code-review C74): walkable, a floor under it (FloorHoleAt - not a pit or a
  stairwell, where it hung in the air over the shaft) and no shut door. The
  drop, a landing (which backs off along its flight to the first such square,
  never past the party's own - the party can stand on a stairwell) and a
  fumble's fling all ask it; a hole REFUSES a thing rather than sending it to
  the level below. A SHATTERING throw leaves nothing to hang, so it bursts at
  the first OPEN square (walkable, no shut door), a pit's included, not pulled
  back toward the thrower. The rock is script-built (tools/BuildRock.py ->
  assets/models/rock.glb, committed by a .gitignore exception: an item loads
  only .glb). Dev: `throw [item]`, `drop <item> <x> <z>` (DropItemAt aimed at a
  square's centre), `castsvc floor [x z]`, `flooritems [x z|all]` (floor items
  with their charge, and where each throw in the air would come down); tally
  `throws= throwstrikes= throwlandings= landat=` (where the last throw came down
  or burst). Checked by AllocTest -Throw (lift, throw at eval_arena's north wall,
  again, by clicks), tools\AITest.py (a pit refuses a drop and a landing; a
  flask bursts over it; a rock thrown from the stairwell lands on the party's
  square) and tools\CombatTest.py's FLASK / SAVE checks.
- BLASTS ARE SEEN now (they drew nothing): a puff of `blast_color` (else the
  type's element colour) in each square on each tick (ProjectileSystem::Puff,
  LandBlastHit) - a fire front flares, a persistent gas rolls. And `blast_linger`
  is real: a persistent blast, once spread, bites again in every square it filled
  each `blast_rate` (>= 0.3 s) for that long, SILENTLY (ActiveBlast::lingering;
  a kill or a break still speaks). Spell and item read the fields through one
  helper, ReadBlastRules (Spell.h).
- HAND BOXES, after Phase 10's play: a left press HELD 0.4 s on a HUD hand box
  (HandSlot::onHold -> GameUI::OnHandHold) takes its item onto the cursor, or
  swaps it with the cursor's; a held press never also clicks. `throw` is a hand
  USE (ThrowItem takes the thrower: member < 0 = the leader), the click of any
  item listing `command = throw` (the rock, both flasks), and a ROW in every
  held item's hand menu - a row, not a command, so a key or rune in a hand
  still opens its menu on a click and keeps Punch / Kick. use.throw and the
  missing use.drink are in the lang files.
- A RUNE IS A TABLET IN THE PACK, A GLYPH IN THE CONTROLS (Michael,
  2026-10-02): `DrawItemIcon` (PartyHudDraw.h) is the one way an item goes into
  a socket. In the pack, doll, bags, party window and on the cursor a rune is
  its CARVED TABLET - an icon baked from the 3D model (BakeRuneIcon) with the
  school's halo breathing over the groove (RuneFaceUv places it). The HUD hand
  boxes, the doll's hand cells, a set hand's recipe, the Magic window and the
  Known Spells list pass `symbolic` and draw the glyph alone (DrawRuneGlow).
  The groove glows IN 3D too: `MaterialParams::emissiveGroove` decodes the
  carve from the rune set's occlusion (RuneBaker writes 1 - 0.45 x carve), and
  `RuneTabletMaterial` is the one held-tablet material (icon bake + details
  dialog, which breathes it on the same kRuneBreathSeconds), so the two match.
  DrawRuneFace survives only as the glow's fallback.
- THE STARTER KIT (Character.cpp CreateDefaultParty): Brand a dagger in his
  right hand (his bare left is what the harness's `swing 0` uses), Sera one in
  her left and a LIT TORCH in her right (the party's only light - see FIRE AND
  LIGHT); Maren holds fire + project, Tilo earth + protect, school rune left,
  and EACH caster's backpack carries Ingwaz + Hagalaz (the tier-3 modifiers;
  both each, since a tablet is memorized by one member and spent) and, FOR NOW
  (Michael, 2026-10-05), a Sowilo tablet - until a level has one to find. That is the
  PREMADE four; a CREATED member picks two of project.ini `start_items`, which
  carries `torch_lit` so a party of created members is not left in the dark.
- THE MESSAGE LOG opens only from its Log button, which sits at the bottom-left
  in every state (alone once the footer fades, in its corner while it shows,
  pressed while open); hovering does nothing (Michael: it got in the way).
  THE TWO WINDOWS: the character sheet is a panel in m_sheetUi whose scale is
  that CONTEXT'S root font size (UpdateFonts) - rem itself moves - so it sets
  `scalesText = false`. The party inventory is NON-MODAL now - no dim, the world
  clickable around it, closed by its corner box or Esc.
  THE PARTY WINDOW (more-ui-updates P5, Game/PartyWindow.h) replaced the old
  party-backpacks window: the sheet's "All" opens it ON THE SHEET'S TAB, with its
  own row of tab stones and a 2x2 of CARDS. A card IS a CharacterSheet in card
  mode (`card` ctor flag: no portrait, tab stones or status band; Body() is the
  card's tab area stretched back to a whole sheet body, its top above the
  card), so a card and the sheet cannot read differently. To make that work the
  sheet measures in EM, not rem (identical on the sheet, which sets no
  fontScale; a card's is the panel's scale x kCardScale, set by the window in
  LayoutSelf since fontScale is ABSOLUTE, not inherited) - the scroll gutter is
  the one rem left, because ScrollArea reads it in rem. The window is sized in
  card em (SizeForEm), keeps the `inventory` panel slot, and draws the hovered
  card's status line. Its INVENTORY tab (P6) is the exception to "the sheet's
  tab at card size": a card lays its squares out in em from its own corner (no
  doll, the load beside the name, pack row over contents six across) at the
  SHEET'S text size (`squareDesign`, times the window's scale), so a square is
  the sheet's; the window's size follows the tab (PanelSize: the most rows any
  shown bag needs) and its default spot is centred at the other tabs' size, so
  the tab stones do not move. Its four cards are built and warmed with the HUD, so an
  open adds nothing. Dev: `inventory [tab] | off | status | slot <m> <i> |
  stone <tab>` (status gives `tab`, `opens=` and the bar); `sheet status` gives
  the All button's point. Checked: AllocTest `-All` (All, every tab, Esc, the
  portrait, inside the window; mutation-checked), `-Items` now aims by `inventory
  slot` and parks the window clear of its floor point (as `-Panels` does). The
  card's checked drop into a pack found `ItemCategoryBank::CategoryOf` copying a
  string (now a view).
- MINIMAL LAYOUT (Game/MemberCards.h; ini hud_layout=1, Settings -> UI
  "Layout"): no party bar, no Hands dock - one CARD per member, the very
  CharacterPanel and HandPair the Standard layout uses (so every click and hand
  use is identical), on one face, in a 2x2 "cards" block under Movement; Magic's
  default moves to the left column. A switch goes through RebuildForRoster (the
  log restarts with the help line). Shared panels keep ONE saved spot across
  layouts.
- TWO ALLOCATION RULINGS (AllocTest -Panels found both): GameSettings::Save
  EXCUSES itself - it runs on one click or release inside an armed frame (a
  drag's drop, a dock's minimize), formats and writes a file, and cannot be
  allocation-free; and a drag never touches the Settings sliders directly (a
  slider rebuilds its readout text) - it marks them stale and the menu / pause
  updates sync them.
- CHECKED: AllocTest `-Panels` (drags the Movement dock away and back and pulls
  the Hands grip inside the window with the inventory window open; refuses a
  PASS unless all three landed) and `-Minimal` (any mode under the card layout,
  switched FIRST since the rebuild would close -Cast's book; refuses a PASS
  unless the cards were up); InGameTest sweeps `sweep_inventory` and
  `sweep_minimal`. Short rosters are exercised now (see PARTY CREATION):
  `AllocTest -Party <spec>` runs any mode with one, and InGameTest sweeps a
  party of three and of one.

## Tool refinement (tool-refinement branch; docs/tool-refinement-plan.md)

Michael's notes and answers: docs/tool-refinement-notes.md. The goal is the
workflow new world -> add level -> build -> populate, with less repetition.
Judged by `tools\EditorTest.py` (phase 12 onward).
- THE PALETTE'S CATEGORY BAR (MapEditor_Categories.cpp): icon buttons at the
  top of the palette body pick a GROUP; the accordion lists only its sections.
  Two groupings, flipped by the bar's first button: by STAGE (World / Build /
  Furnishings / Populate) and by KIND (Surfaces / Structure / Furnishings / Creatures / Items /
  World). Each is ONE table (kStageGroups / kKindGroups) with static_asserts
  that every listed category is in exactly one group - a category missing from
  a grouping is unreachable except by the filter. The FILTER ignores the bar
  and searches every section. Grouping + group per grouping persist
  (`map_palette_group/_stage/_kind`). Icons: `icon_tb_cat_*` from
  BuildToolIcons.py. The world sections (Dungeons / Quests / Terrain) now list
  their entries; they used to fall through `CategoryItems` to nothing. Dev:
  `editor palette [mode stage|kind | group <name> | filter [text] | groups]`.
- MONSTER POWER (Game/Power.h, pure; DungeonWorld_Census.cpp): a kind's power
  is its derived threat (Game/Threat.h) unless monsters.cat carries `power =
  <n>` (> 0). EVERYTHING that ranks monsters asks `DungeonWorld::MonsterPower`
  - the generator's pools (Game::FillPools -> PowerOf), the palette's band
  pips, the `threat` readout - so one override moves them all. The BAND is
  which fifth of the project's power range a kind falls in (linear, not rank).
  The world caches all kinds per EditRevision; a Balance change moves threat
  with no edit, so the Balance dialog's apply calls InvalidatePowers. The type
  editor shows "Power (derived 3.9)" through its `derivedFor` hook, and
  switching the override on starts it AT the derived value. `threat` lines
  gained `power=` (marked `(set)`) and `band=` at their END - LevelBuildTest
  matches the line's head. Dev: `editor palette items <catalog>`.
- THE DOCKS RESIZE (MapView_Docks.cpp): drag a dock's inner edge (the band
  below its collapse button); the width is saved as a SHARE of the panel
  (`map_palette_width` / `map_legend_width`, 0 = the old default), clamped to
  120 / 150 px and 30% of the panel. Nothing else needed changing - the grid,
  tool strip, palette body, category bar and trimmed names all measure from a
  dock's edge. The pointer turns into the resize arrow over an edge: Window::
  SetCursorShape (WM_SETCURSOR over the client area, a direct SetCursor while a
  drag holds capture), set ONCE per frame in Game::Update from
  MapView::WantsResizeCursor. A dock's hover is only trusted on a frame Update
  ran (m_dockUpdated, the RenderIssueTooltip rule), or a modal dialog would
  leave the arrow stuck.
- THE OVERVIEW (right dock, above the KEY; both are collapsible sections, and
  the dock scrolls): World / Dungeon / Level, summing DungeonWorld::Census -
  one row per project level, walked like Validate (live / stash / read-only,
  NEVER stash to read), cached per edit revision. The active level's MONSTERS
  are its live list, as a save writes them (ActiveEntText) - an editor-placed
  one has no record, and a census of records alone missed it (EditorTest 14
  caught exactly that); so the cache also keys on the live list's size and the
  active level. Lines: counts, a power-band row, the strongest kind, the live
  checker's issues (a link to Check), and links down a tier (the world's
  dungeons, a dungeon's levels). Dev: `editor dock [left|right <px>]`,
  `editor overview [world|dungeon|level]`.
- FLAGS (Phase 4): `flags.cat` - a name and a scope (`dungeon = <id>`, absent =
  the world). The on/off value is save state, in WorldState::flags BY ID (no
  save-format change); `FlagOn` = set to anything but "0". SET by an item's
  `flag` when lifted and a lever's `sets=` / `toggles=` (`clears=` turns one
  off); READ by a door's, a lever's and a stair's `flag=` (each waits until it is
  on: a sealed door refuses the hand BEFORE its key is asked about, a lever will
  not move, a stair says the way is barred) and a world location's `flag=`.
  DungeonWorld borrows the store (SetFlagStore, like SetRoster); a wired button
  still moves a waiting door, as it does a locked one. The three inspectors
  share `FlagDropDown` (InstanceInspector.h). The checker (Validate_Flags.cpp):
  `flagwaits` (an error where the waiter stands - nothing sets it), `flagunknown`,
  `flagscope` (a dungeon's flag used in another), `flagunused`. The palette's
  "Quest items & flags" section (MapEditor_Quests.cpp; rows of two catalogs,
  told apart by PaletteItem::ref) lists this dungeon's and the world's flags and
  quest items - an item's scope is its flag's - with where each item lies and a
  ">" link there; an item row arms its own brush. A quest's stages are rows with
  their log lines (FieldKind::QuestStages); items/weapons/armor have a Quest tab.
  Dev: `flag <id> [on|off]`, `flags [world|dungeon [id]]`, `opendoor <x> <z>`,
  `press <x> <z> party`, `flagwire <x> <z> <door|lever|stair> ...`, `editor
  palette use <id> [link]`. EditorTest 15.
- STYLES (Phase 5): `styles.cat` - `room` / `corridor` themes, `knobs` (the
  generator's settings line), `corridor_width`, `tags`, `monsters` (`<id>
  [weight], ...`; Game/Style.h, pure and in RollTest). THE SHARED LIBRARY is
  `assets/library` (styles.cat + the themes they name + those themes' surface
  types; Game/StyleLibrary.h), outside projects/ like the template. ADDING a
  library style copies it and whatever it points at that the world LACKS -
  never what it has, so an add cannot repaint anything - and reports monsters
  the world lacks instead of copying them; a second add is a no-op. "Save to
  library" (the style editor's footer) goes the other way and replaces the
  library's entries of those ids. The palette's Styles section lists This world
  then the library's: a world row ARMS the current style (MapEditor::
  CurrentStyle, session-only), a library row adds. The armed style RANKS the
  Monsters section (its list, a divider, the rest - the tags lens). Its monster
  rows are FieldKind::WeightedRefs (each named with its power via faceFor).
  Sweeps: theme -> styles, style -> dungeons' `style`, monster -> styles' lists,
  and flags -> items / locations / door, lever and stair records (Phase 4's gap).
  Dev: `styles`, `style use|add|save|row <id>`. EditorTest 16.
- SHAPE BRUSHES (Phase 6): four tools on the strip - Corridor, Room (drag),
  Stamp (click; R turns), Region (drag a box of 6x6 or more for the generator
  to fill) - laid in the CURRENT STYLE (its corridor / room themes, corridor
  width, knobs) or plainly when none is armed. Geometry is `Game/Carve.h`
  (pure, in RollTest); MapEditor_Shapes.cpp commits a carve::Shape as one undo
  step and one chunk batch. A BRUSH NEVER RAISES WALLS round what it carves -
  it opens rock and paints the theme on what opened and the rock round it, so
  a drag over open floor cannot cut a room in two (a stamp's '#' pillars are
  the one raise, never on the party). The region brush calls generate::Run
  as-is, sized to the box, and joins the result to what touches the box; the
  generator was not lifted. Stamps are `shapes.cat` (`rows`, '|'-split); the
  palette's Shapes section leads Build. Every gesture previews exactly what
  its release commits (MapEditor::preview). Dev: `editor shape ...`. EditorTest 17.
- THE WORKFLOW, WIRED THROUGH (Phase 7). A STYLE IS A GENERATOR KNOB now
  (`style`, the Generate dialog's Style tab; picking one loads its knobs, seed
  kept) and supplies tags, look and monster list at once. `Game/StyleLook.h` is
  the ONE place a style becomes level text: palettes = its themes' members where
  it names any (else the donor's - never both, each entry is a texture set to
  load) plus `theme` records BY ID, rooms vs passages decided by the pure
  `carve::Dress` (Area.h's 2x2 rule on a bare grid). Used by a generated level,
  the [+] empty box, a new world's first room and the wizard. NEW WORLD (Blank /
  Wizard) takes a LIBRARY style: added to the world (AddTo), named as the
  starter dungeon's `style`, the first floor built in it. [+] OPENS ON THE
  DUNGEON'S `style` (else the armed one) and both Create and Empty LAND IN BUILD
  (Game::LandInBuild: Stage grouping, the Build group by NAME, the style armed).
  POPULATE ONLY is `generate::Populate` (pure, beside Run in Generate.cpp so they
  share PickNear, the weighted pick): Run's rules on FOUND rooms. Two rules the
  found rooms forced, both learned from the walk: the start's room is skipped
  only WHILE ANOTHER is reachable (a wandering 2-wide corridor is "room" and joins
  what it touches, so the start's room can be the whole floor), and there is
  ALWAYS SOMEONE when density > 0. Game::PopulateViewedLevel replaces what
  populating can make (the POOL'S kinds of monster and loot) and nothing else -
  keys, quest items (never loot) and other monsters stay; one undo step.
  `Params::monsterWeight` is read by Run only when present, so an unstyled
  generate is byte-for-byte what it was. The generator's Tag and Palette-donor
  rows are FOLDED into the style: `hidden` knobs (GenerateKnobs.h), still
  encoded for presets / scripts, given no row, CLEARED when the dialog opens. The
  overview's Level view leads with NEXT (build / populate / fix N / ready, each a
  link into that stage) and a Floor squares line. TRAP for scripts: console
  `generate` starts with NO style (the dialog's rides settings.ini); name one
  with `style:<id>`. Dev: `generate populate [knobs]`, `generate dialog
  create|empty|populate|style <id>`, `editor overview follow <key> [scope]`,
  `worlds new <n> blank|wizard style=<id>`, `worlds newdialog style <id>`.
  EditorTest 18.
- EFFECTS LEFT THE PALETTE for the Balance dialog's Effects tab (a list whose
  rows open the type editor OVER the Balance dialog - which is why the type
  editor's input check now comes before the Balance dialog's in Game::Update,
  and `m_typeOverBalance` rebuilds the tab when it closes).

## Transparency and potions (transparency branch; docs/transparency-plan.md)

Started from bought glass bottles the renderer could not draw. Michael's notes
and answers: docs/transparency-notes.md; each phase's AS BUILT is in the plan.
- THE QUEUE (Renderer): `MaterialParams::transparent` makes DrawMesh QUEUE the
  draw (`QueuedDraw`, a fixed 256 reserved at startup - a full queue drops and
  counts, it never grows) instead of issuing it; the shadow pass skips glass.
  `FlushTransparent` sorts by squared distance (`std::sort` with an index tie
  break - `stable_sort` may allocate) and draws far to near PER OBJECT: every
  far wall (front faces culled), then its liquid both ways, then every near
  wall. A pass that can draw glass MUST flush at its end: RenderScene (before
  the particles), EndItemIconBake, BakeMeshIcon, BakeMonsterIcon, ModelPreview.
  A draw left over is dropped at BeginScene with one warning - that warning
  means a new pass forgot its flush. A skinned draw's palette is uploaded at
  QUEUE time (the animator's buffer may move on). Dev: `glass [status]`.
- GLASS FILTERS, IT DOES NOT PAINT (Michael judged three rounds). `PSGlass` uses
  DUAL-SOURCE blending (`SRC1_COLOR`): result = added + behind x filter, per
  channel. Material RGB is the TINT (white = clear), alpha the DENSITY; filter =
  lerp(1, tint, density); added = body x density^2 x kGlassScatter + arriving x
  fresnel x kRimSheen + specular. OPACITY IS FLAT at every angle: any Fresnel
  rim that raises opacity reads as an OUTLINE whatever fills it (a dark body
  drew black edges, a light estimate "a ghost"). Specular is a second `Shade`
  with a black albedo, so the light loop itself is untouched. A glTF material
  with `alphaMode BLEND` loads as transparent (`MaterialData::blend`); a
  catalog `transparent = 1` (decorations and items) forces it.
- THE LIQUID is GENERATED (`Game/Liquid.h`, pure): the glass mesh's INNER-WALL
  triangles (radial normal pointing in, or a floor), inset into the cavity and
  turned outward. The fill level is a WORLD-space clip plane built from the
  world matrix's Y row; a back face
  seen through the cut is lit with the plane's normal, which is what makes the
  cut read as a surface. Object constants carry `transparent` (0 opaque, 1
  glass, 2 liquid) and `liquidPlane`. items.cat: `liquid_color = r, g, b
  [, density]` (density 0.85 if absent), `liquid_fill` (default 0.6). Filled
  glass is forced CLEAR (density / roughness 0.08) so the colour is the
  liquid's. COLOUR RULE he steered to: a DARK tint at density 0.95 - a light
  tint lets the white behind through and the tonemap lifts it to pastel (the
  health potion read PINK).
- THE BOTTLES are script-built, `tools/BuildPotion.py` -> assets/models/
  potion_vial / potion_bottle / potion_flask.glb (committed by .gitignore
  exceptions): profile-lofted closed sections, the cork sized to the neck, the
  glTF's alpha settings patched after export and verified. 1.5x real size (true
  size vanished on the floor); EMPTY glass is FROSTED (density 0.25, roughness
  0.35). `--tint/--density/--roughness` override. items.cat `upright = 1`
  stands an item on the floor as authored (the flat-item rule laid bottles on
  their side) and frames its icon unstretched.
- DRINKING: items.cat `restore_health / _stamina / _mana` and `cures = poison
  0.5, bleed` (a share scales the effect's magnitude by 1 - share; a whole
  share, or a magnitude near zero, lifts it). One `eat`/`drink` handler,
  `ConsumeItem`; the bottle becomes its `drink_as` empty. A DOWNED member
  cannot drink (Michael: revival is for later spells and items) - `got.downed`,
  `log.consume_downed`. Health from a potion moves under the ledger reason
  `drink` (SIX sanctioned reasons now), and PipelineTest demands that route
  move. The details dialog shows the restores and cures. Potions: health /
  stamina / mana / antidote, each minor (vial) / standard (bottle) / greater
  (flask); they sit in `start_items`, crypt1 / crypt2 and the generator's loot
  (`loot = 0` keeps the empty containers out of it).
- BOMBS: `fire_flask` / `poison_flask` with `_small` and `_large` beside them.
  items.cat `throw_scale` scales whatever the throw delivers - blast damage and
  linger, `blast_force` (rounded, never below 1 square) and on-hit magnitudes -
  applied AFTER a borrowed `throw_spell` payload, which is the case it exists
  for. No shatter effect (Michael: the blast is enough).
- CHECKED: `AllocTest.ps1 -Glass` (a potion in view, queued every armed frame;
  refuses a PASS with fewer glass frames than armed ones; mutation-checked both
  ways), PipelineTest's `drink` route (pipeline.eval section 10), and the eval
  scripts tools/EvalScripts/potions.eval and bombs.eval (a measurement of the
  three sizes, not a pass/fail). NOT covered: a drink from the pack inside an
  armed frame, and a glass bomb in flight (-Throw throws a rock).
- OVERLAP: the `lighting-updates` branch rewrites scene.hlsl's light loop; the
  glass code sits OUTSIDE the loop on purpose (GlassOutput, ShadeSurface), so a
  merge should only need the loop's own conflicts resolved.

## Known gaps / natural next steps

- REMINDER (Michael, 2026-10-03): crypt1.ent carries a TEMPORARY `item
  torch_magic 5 7 north` beside the start, placed only so the magical torch can
  be tried. REMOVE IT once there is somewhere for the player to find one later
  in the game - it is not meant to be starting loot.

- Combat is built out (see the COMBAT bullet: the attack formula, damage
  types/resists, stamina exertion, death/revive, DoTs, reach, quadrant
  lanes) but UNTUNED — every number is a first cut awaiting a balance pass
  (the editor's Balance dialog / balance.cat + attacks.cat). No polearm or
  ranged weapon is authored yet (the reach/lane plumbing is ready). Healing
  is regen, unconscious self-stabilize and the POTIONS (transparency branch);
  nothing yet revives a downed member but time (a heal spell and reviving
  items are queued). Portraits are the bought sets
  (see the asset pipeline); the tinted-initial fallback draws if an image is
  missing.
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
  (culling), ChildClip (clipping, intersected and restored so clips nest - by a
  ui::ScopedClip in both walks, so on a throw too, and every UIContext walk
  begins with no clip: code-review C208, where an inner clip never popped and
  one throw left every context clipped; HealthTest `uiclip` / `uinest`, dev
  `crashpoke uiclip`, `clippoke`).
  ui::ScrollArea owns ALL scroll/thumb/clip behaviour — nothing else may
  re-implement it — and ui::Repeater builds children from a per-frame count with
  a grow-only pool (repeated children hold an INDEX and re-resolve, never a
  pointer into the model). CAVEAT that bit once: a ScrollArea measures overflow
  from its OWN children's bounds, so rows behind a Repeater are invisible to it
  — size the repeater to the stacked height. Bounds may be COMPUTED in
  LayoutSelf rather than authored when a child is aspect- or font-locked (a
  square sized by the parent's height; a row the height of a line advance) —
  still parent-relative, just derived. Screen-anchored popups (ContextMenu)
  keep zero bounds and draw in the overlay pass on purpose (the party
  inventory used to as well; it is a floating window now - ui-panels); an open
  ContextMenu's InkRect is its box, and it is sized and drawn from one set of
  rem insets.
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
  and (labelled, `uioverlap [<label>] --- state <app state>`) to dungeon.log. A
  label only says the command RAN: tools\InGameTest.ps1 also demands each
  screen's own status line before its header (a refused open step audits the
  screen beneath, clean for the wrong reason; a screen row naming no status is
  refused before the game launches) and the state the header names, and
  matches labels exactly. Widget::InkRect is what a widget PAINTS as
  against Pixel(), what the layout gave it: Label, Checkbox and Button measure
  their text, so a label wider than its row counts, a label in a row shorter
  than its font (a starved Stack Fill row resolves to ZERO height) counts at its
  full line, and a Button's centred label wider than the face counts on BOTH
  sides. A DropDown is the other shape: its face TRIMS a selection wider than
  the room left of the expander (ui::FitText / DrawFittedText - "..", whole
  UTF-8 characters, allocation-free; the open list widens to its longest item
  and a trimmed face tooltips itself), so it never paints outside itself and
  its InkRect stays honest - but the trim is still a layout that did not fit,
  so it reports through Widget::TextOverrun and the audit lists it as a third
  finding, "trims its text by Npx" (mutation-checked on ButtonInspector's old
  side-by-side flag row, the case the overlap check could not see). The palette
  and overview docks fit their names through the same helper.
  The audit gates on empty INK, not an empty Pixel(), or that zero-height
  row would be skipped outright - which is how both slipped a sweep once
  (editor-updates 11c2144, NewWorldDialog). `overlapOk` opts the deliberately
  layered out of the overlap and escape checks but NOT the trims check - an open
  ContextMenu (overlapOk, a popup) reports a row whose text runs into its group
  marker or off the window (code-review C382; it drew at raw 10/16 px offsets,
  and below an 18 px rem the marker ran over the label - InGameTest's
  `sweep_handmenu` resizes the window to 720p and opens the bare-hand menu, both
  groups and the Magic submenu, through dev `handmenu`; the HUD sizes an open
  menu only while the console AND the map are shut - `editor off` leaves the
  map open in Player mode, which made the sweep's first run vacuous - and an
  unsized menu is 0x0 and audits clean, so the sweep demands `handmenu status`
  saw its box); a parent that CLIPS is
  exempt from the escape check, since a scroll area's children are meant to
  run past it. RUN IT AFTER TOUCHING
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
  Editor dialogs read at `ui::kDialogTitleScale` (2.9) / `kDialogTextScale` (2.0),
  both through Widget::fontScale (a dialog context's root carries the text scale
  for its rows, the title widgets the title scale); numeric readouts take
  NEITHER - sized to their digits, document size, Mono. And MEASURE IN THE FACE
  AND SIZE YOU DRAW IN, or a row will not fit its own contents. THE CARD IS
  game::BuildDialogChrome (Game/DialogLayout.h), which 16 dialogs build on (the
  editor's, the asset and portrait pickers, the item details): one ui::Stack in
  the panel rect - a title ROW (its height derived from kDialogTitleScale, so the
  title's ADVANCE clears and nothing sits on its descenders) holding the title's
  SLOT and, beside it, the close box's OWN slot, then the body (Fill) and an
  optional footer row. A dialog hands over a panel rect and a title and fills the
  body it gets back with rows sized by game::FormRow(lines) (rem x
  kDialogTextScale, the one place that knows the factor) and footer actions by
  FooterIcon / FooterButton; it writes no coordinate, so nothing can land under
  the title or the close box (the window-fraction bands every dialog used to
  author were exactly how eight titles came to draw through the row beneath
  them). A PLAIN title is a ui::Label at kDialogTitleScale, which draws its
  whole string - a long one ESCAPES its slot and `uioverlap` says so; keep it
  short. A title that carries an object's ID and IS its click-to-rename
  affordance (TypeEditorDialog, LevelSettingsDialog) is a game::EditableTitle in
  that slot (BuildDialogChrome given an empty title): ONE widget for the plain
  prefix and the clickable name, so the hit box is the measured text, and it FITS
  ITSELF - LayoutSelf shrinks it for height, then for WIDTH (one computed size,
  quantized to 2px, never below the document size), and only past that floor
  cuts the NAME's tail with ui::FitText's "..", reporting the cut through
  TextOverrun so `uioverlap` lists it. SHRINK BEFORE CUT because the tail of an id
  is the part that says WHICH type or level is open. Checked by InGameTest's
  `sweep_longtitle` (a 32-character dungeon id in the type editor must audit
  clean). The helpers this paragraph used to teach - kDialogTitleBandH,
  DialogTitleBand, FitDialogTitle, the dialogs' own TitleFont - are GONE;
  ui::DialogTitleFont / DialogTextFont and the panel-rect AddCloseButton /
  CloseButtonRect survive with NO CALLER until code-review C92 deletes them, so
  do not build on them.
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
  TOP-RIGHT CORNER, never a footer "Close"/"Cancel"/"Back" button -
  ui::AddCloseButton(slot, icon, onClose) puts it IN A SLOT the layout reserved
  for it (BuildDialogChrome's title row for every editor dialog; the sheet's and
  the party window's own - GameUI.cpp BuildCharacterSheet, PartyWindow.cpp),
  so it is the same everywhere and nothing can sit under it (it's a ui::Button
  with the icon; text "x" is the missing-asset fallback). ONE texture serves
  them all: AssetUtil's CloseIcon(device) loads
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
  not in a load task. Footer keeps only ACTION buttons (FooterIcon discs:
  Save/Delete/Remove/Animation/?), in the card's footer row so nothing overruns
  it. Covered: every dialog on BuildDialogChrome - the InstanceInspector base
  (all 7 per-instance inspectors; the stair one joined 2026-09-25 -
  right-clicking a stair used to open nothing, because AnyInspectableAt never
  listed stairs), TypeEditorDialog, AssetDialog, BalanceDialog,
  MonsterConfigDialog, LevelSettingsDialog, ProjectileInspector, InspectPicker
  and the rest - plus the character sheet and the party window. NOT touched:
  Yes/No confirm modals (their explicit choice
  buttons aren't a "Close") and the full-screen menu/settings/save PAGES (Back
  is page navigation, not a dialog dismiss).
  ESC BACKS OUT ONE LAYER, in every dialog (code-review C81): an open
  drop-down or colour picker closes itself on Esc, so a dialog that acts on
  Esc asks `UIContext::PopupOpen()` FIRST and leaves the key to the list - it
  used to close the dialog round it, and in the inspectors and Level settings
  revert every live edit too. A NEW dialog's Esc takes the same gate. The
  create dialog (AssetDialog) closes on Esc like the rest (it had none). The
  editor map's ladder: the level drop-down, then a drag, then the armed brush,
  then the map; the world editor's: the terrain brush, then the pause menu.
  CHECKED by EditorTest phase 24 (tools/EvalScripts/dialogesc.eval), whose
  Escs are KEYS: `presskey esc` presses and releases one in the frame after
  the line, so it goes through the frame's own input path rather than a
  handler called by name; a list is opened by `... popup <n>` (UIContext::
  OpenPopupNext - the n-th shown widget with a popup, opened as a press on
  its face does, at that context's next Update).
