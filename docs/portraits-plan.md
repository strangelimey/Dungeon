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
- The images stay out of git (250 MB) and are provisioned from the OneDrive
  archive; the TAGS are a committed catalog.

## Phase 1 - the portrait set

- `tools/BuildPortraitCatalog.py`: reads `tags.tsv` from the archive, drops
  figures, writes `assets/portraits/portraits.cat` - one `[id]` block per
  portrait with `race` / `sex` / `age` / `look` / `uncertain` (look is kept as
  data though nothing filters on it yet). The script is the record of how the
  catalog was made; a re-tag is a re-run. File header comment explains the
  fields, as every .cat does.
- `tools/FetchPortraits.ps1`: extracts `256square/256x256/` from the archived
  zip into `assets/portraits/` for every id the catalog lists (so the catalog
  decides what ships, and figures never land), then bakes BC7 `.dds` beside them.
  Same shape as FetchTextures: finds the archive, refuses loudly, reports a count.
  Check whether `AssetBaker mips` can be pointed at a folder other than
  `assets/textures`; if not, add that.
- `.gitignore`: `assets/portraits/*.png` and `*.dds`, EXCEPT the four defaults,
  which are committed so a fresh clone's party has faces before anyone runs the
  fetch.
- Worktree provisioning (CLAUDE.md) gains `assets\portraits` as a third copy.

Checked by: the fetch's own count against the catalog's; `portraits.cat` loads
with 2386 entries.

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

## Not in this branch

- Party creation (the picker's eventual home).
- A look / class filter, or re-tagging the "commoner" majority.
- Portraits for monsters or NPCs.
