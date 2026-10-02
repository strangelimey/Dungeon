# Portraits - plan

From `portraits-notes.md` (Michael's brain dump, 2026-10-01, and the decisions
under "Organised"). Goal: the party's portraits come from the Magory pack, and a
button on the character sheet opens a PORTRAIT PICKER - a filterable grid of
thumbnails - that sets that member's portrait. The picker is a standalone piece
so party creation (later) can open it too.

## Where things stand (survey, 2026-10-01)

- `Character::portrait` is a bare `const gfx::Texture*`; nothing stores WHICH
  portrait. `Game::LoadPortraits` derives `portrait_<name>` and loads it from
  `assets/ui/`, so the portrait is tied to the member's name.
- Every draw goes through `DrawPortrait` (PartyHudDraw.cpp), from the party bar's
  `PortraitBox`, the member cards (same `CharacterPanel`) and the sheet's
  `SheetPortrait`. None of them needs to change - they draw whatever texture the
  member holds.
- Save format is v2. A per-member line (`supply <i> ...`) is a writer line plus
  a reader `else if`; an absent line keeps the default, so a new line needs NO
  version bump.
- The editor's `AssetPicker` already solves the hard part - a thumbnail grid that
  loads only visible tiles (2 a frame, from Update), drops big mips
  (`LoadTextureThumb`), evicts least-recently-seen past a cap, and drains the GPU
  before freeing - but the class is bound to the asset pool. Its pieces are
  reusable; the class is not.
- `ItemDetailsDialog` is the model for a modal over the sheet: its own
  UIContext, built once, gets input first in `GameUI::UpdateSheet`, closed first
  by `GameUI::DismissPopup` on Esc, drawn after the sheet.
- A sheet over a level is an ARMED frame for the allocation guard.

## Decisions (from the notes)

- Filters: race / sex / age, each with "Any" (the default). An `unclear` tag only
  shows under Any. No look filter.
- A click on a thumbnail picks it and closes; the close box or Esc cancels.
- The portrait is SAVE STATE: `portrait <i> <id>`. A new game uses the defaults.
- Defaults: Brand `portrait018`, Sera `portrait398`, Maren `portrait1419`,
  Tilo `b045`. The baked busts retire.
- Figures (framing = figure, 72) are not shipped: 2386 portraits.
- A SECOND SOURCE joined 2026-10-02: Corax Digital Art's 500 Human Hero
  portraits ($10, `OneDrive\DungeonAssets\ui\corax-human-heroes\`, its own
  `tags.tsv` with the same columns). Ids `corax001`..`corax500`, so they cannot
  collide with Magory's. 512x512 (twice Magory's), kept at native size - the
  draw scales and the picker's thumbnails drop mips anyway. Their LICENSE needs
  a credit ("Corax Digital Art" + https://linktr.ee/coraxdigitalart, in
  docs/costs.md Attributions) and forbids redistributing the files, which the
  gitignore already honours.
- The images stay out of git (250 MB) and are provisioned from the OneDrive
  archive; the TAGS are a committed catalog.

## Phase 1 - the portrait set

- `tools/BuildPortraitCatalog.py`: reads EACH SOURCE'S `tags.tsv` from the
  archive (a small table of sources: folder, zip, member pattern -> id), drops
  figures, writes `assets/portraits/portraits.cat` - one `[id]` block per
  portrait with `race` / `sex` / `age` / `look` / `uncertain` (look is kept as
  data though nothing filters on it yet) and `source` (magory / corax, so a
  credits screen can ask what ships). The script is the record of how the
  catalog was made; a re-tag is a re-run. File header comment explains the
  fields, as every .cat does.
- `tools/FetchPortraits.ps1`: extracts from each archived zip (Magory's
  `256square/256x256/`; Corax's `coraxdigitalart-realistic-human-heroes (N).png`
  renamed to `coraxNNN`) into `assets/portraits/` for every id the catalog lists
  (so the catalog decides what ships, and figures never land), then bakes BC7
  `.dds` beside them.
  Same shape as FetchTextures: finds the archive, refuses loudly, reports a count.
  Check whether `AssetBaker mips` can be pointed at a folder other than
  `assets/textures`; if not, add that.
- `.gitignore`: `assets/portraits/*.png` and `*.dds`, EXCEPT the four defaults,
  which are committed so a fresh clone's party has faces before anyone runs the
  fetch.
- Worktree provisioning (CLAUDE.md) gains `assets\portraits` as a third copy.

Checked by: the fetch's own count against the catalog's; `portraits.cat` loads
with 2386 entries.

DONE (2026-10-02). What it came to:
- 2879 portraits: Magory 2386 (72 figures out) + Corax 493 (1 figure and 6
  byte-identical duplicates out - the builder's DUPLICATES table).
- `AssetBaker mips` already took any folder (`BakeAllMips(dir)`); it gained
  `skipCurrent` and a `portrait-mips` mode (`mips` covers portraits too), so a
  re-run costs only what changed - 0 bakes, a few seconds.
- NEW, found on the first run: 91 Magory images are not square multiples of 4
  (253x256, 256x250, two 182x256 tall crops), which BC7 refused, leaving them on
  the PNG fallback and drawing stretched in a square slot. The fetch now squares
  them: crop to the short side, centred across, TOP-anchored (a face sits high),
  then scale. Its last check demands a .dds per image.
- NEW, also found: zip extraction keeps the entry's old timestamp, so a
  re-extracted image looked older than its .dds and the skip-current bake
  skipped it. The fetch stamps every file it writes.
- A full fetch is about two minutes. The in-game catalog load is checked in
  phase 2, the first code that reads it.

## Phase 2 - portraits by id

- `Character::portraitId` (a string; the texture pointer stays the draw-time
  handle). `CreateDefaultParty` sets the four defaults.
- `Game::LoadPortraits` loads `assets/portraits/<portraitId>` instead of
  `ui/portrait_<name>`. A missing image still falls back to the tinted initial,
  with the existing warning.
- `Game::SetPortrait(member, id)`: swaps one member's texture - WaitIdle before
  the old texture dies (the SRV recycling rule), then load the new one.
- Save: `portrait <i> <id>` written per member, read into `CharState`, applied in
  LoadGame (and reloaded only if it differs). `ResetRoster` / new game put the
  defaults back.
- Retire the busts: delete `assets/ui/portrait_*.png`, the AssetBaker
  `portraits` mode and PortraitBaker.cpp, and fix the stale CLAUDE.md bullet.
- Dev: `portrait [member] [id]` - bare prints each member's id; with an id sets
  it (for scripts and the harness).

Checked by: a save -> load round trip keeps a changed portrait; a new game resets
it; AllocTest (default) still PASSES.

DONE (2026-10-02). What it came to:
- `Game::SyncPortraits` is the one loader (it replaced the plan's SetPortrait-
  does-its-own-load): a slot reloads only when its id differs from what it
  holds, so ResetRoster, LoadGame and SetPortrait all just call it.
- The PortraitBaker was already gone; retiring the busts was the four PNGs and
  the stale docs.
- `tools/EvalScripts/portraits.eval` (headless) PASSES: defaults loaded, Sera ->
  corax001, a reset restores her default, the save brings corax001 back. An
  unknown id is refused (checked by hand - a refusal fails an eval run).
- AllocTest default and -Sheet PASS. NEW: AllocTest started its game with Enter
  on the landing page, which is Continue whenever a save exists - so it measured
  whichever save was newest, and timed out on one whose level was already held.
  It now starts with the console's `newgame`.
- Cost to know: the portraits load task is 85 ms / 298k allocs in DEBUG, nearly
  all of it parsing the 2879-entry catalog (a Field vector of strings per line).
  Fine for a load task; if it ever matters, the catalog is the place to look.

## Phase 3 - the picker

- `ThumbCache` (Game lib): the AssetPicker's thumbnail cache lifted out - load
  N a frame from Update, mark seen in Draw, LRU-evict past a cap, drain before
  freeing. AssetPicker moves onto it, so there is one copy of those rules.
- `PortraitPicker` (Game lib, its own UIContext, `BuildDialogChrome`): title,
  close box, a filter row of three DropDowns (Race / Sex / Age), a scrolling
  grid of square tiles, and a count ("312 portraits"). The tiles' contents come
  from the filtered list (a Repeater-style index per tile, never a pointer). The
  member's current portrait is outlined. Built ONCE; the filtered list is a
  reserved index vector, so filtering allocates nothing.
- API: `Open(currentId, onPick)` - the caller decides what a pick does, which is
  what lets party creation reuse it.
- Allocation guard: an open picker DISARMS the frame (beside the map view in
  `SteadyStateFrame`'s quiet test). Streaming textures as you scroll is loading,
  not a steady state. Closed, the sheet is guarded exactly as before.
- Lang keys x5: title, filter labels, Any, each race / sex / age value, the count.

Checked by: `uioverlap` with the picker open (and a long filter result); the SRV
gauge stays well under 1024 after scrolling the whole set; a scroll through every
portrait leaves live SRVs back where they started once closed.

DONE (2026-10-02). What it came to:
- THE GRID IS ONE WIDGET (`PortraitGrid`, PortraitPicker.cpp), not the
  AssetPicker's widget-per-tile: it writes its own height into `bounds` during
  layout (the fitContent idea) so the ScrollArea scrolls the right distance, and
  draws and hit-tests only the rows in the area's ViewRect. 2879 tiles cost what
  a screenful does.
- `ThumbCache<Payload>` (Game/ThumbCache.h, header-only) holds the rules; the
  AssetPicker moved onto it (its model-bake mesh rides in the payload). New in
  the move: eviction drains to a LOW-WATER mark (3/4 of the cap) so a long scroll
  drains the GPU now and then, not every frame once over the cap; and Clear
  drains first (the AssetPicker's Open used to clear without).
- `LoadTextureThumb` gained `srgb` (default true): the portraits load LINEAR, so a
  thumbnail must too or a face is darker in the grid than once picked.
- Every Open starts on Any/Any/Any and scrolls the current portrait into the
  middle of the view; Close frees every thumbnail.
- Allocation: an open picker is not a quiet frame (SteadyStateFrame), and
  `Game::OpenPortraitPicker` calls OverlayOpenedThisFrame for the click that
  opens it.
- Dev: `portrait picker [member|off|status]`, `portrait picker filter <race|any>
  <sex|any> <age|any>`, `portrait picker scroll <0..1>`; and `assetpicker
  textures|models|off|status`, because nothing could open the AssetPicker without
  clicking a type-editor field, and it is the cache's other client.
- Measured, picker open over the sheet: `uioverlap` clean; a scroll through all
  2879 peaks at 568 of 1024 SRVs (the cache cycles 240 -> 180) and closing drops
  back to 341 (339 before the first open: the +2 is a one-time lazy load, the
  same after a second open). A click picks and closes; Esc closes only the
  picker. AllocTest default and -Sheet PASS; release builds; portraits.eval PASS.
- Not built: a hover readout of a portrait's tags.

## Phase 4 - the sheet button, and handing it over

- A small button on the sheet beside the portrait ("Change portrait", lang x5),
  routed `GameUI::onChangePortrait(member)` -> Game opens the picker for that
  member with `SetPortrait` as the pick.
- Input and Esc follow ItemDetailsDialog: picker first in `UpdateSheet`,
  `DismissPopup` closes it before the sheet, mouse consumed while open.
- Dev: `portrait picker [member|off|status]` so a script can open it.
- InGameTest gains a `sweep_portraits` (uioverlap over the sheet with the picker
  open); AllocTest `-Sheet` still PASSES with the picker closed.
- Docs: CLAUDE.md section (where portraits live, the fetch, the picker, the
  guard ruling); `portraits-notes.md` closed out.
- Launch a fresh game and hand it to Michael to judge the four defaults in the
  party bar and the sheet, and the picker itself.

BUILT (2026-10-02), awaiting Michael's look:
- "Change portrait" is a worded sheet button under the name, its foot level
  with the portrait's (CharacterSheetLayout.h kPortraitBtn*; sheets only - a
  party-window card has no portrait). CharacterSheet::onChangePortrait ->
  GameUI::onChangePortrait -> Game::OpenPortraitPicker, the one opener (it
  excuses the frame; the guard's verdict is taken at EndFrame, so disarming after
  the picker allocated does cover the click).
- Game::OpenPortraitPicker logs `portrait picker: open for <name>`, and
  InGameTest's `sweep_portraits` FAILS without it - a clean audit of the sheet
  beneath would otherwise pass for the picker's (the stair-inspector rule).
- Checked: a scripted click on the button opened the picker for Brand and a
  click on a tile set portrait009, shown at once on the sheet; `uioverlap` over
  the sheet with the button is clean; InGameTest PASS with sweep_portraits.

## Not in this branch

- Party creation (the picker's eventual home).
- A look / class filter, or re-tagging the "commoner" majority.
- Portraits for monsters or NPCs.
