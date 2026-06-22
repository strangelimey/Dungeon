# Items

Requirements (collected — planning to follow).

## Requirements

- Items have categories, such as weapon, clothing, armor, food, etc.
- Items have weight. When carried by a party member, that member's carry load increases.
- Most items have commands — accessible when the item is in a character's hand and the player right-clicks on that hand icon on the control bar. For instance, runes currently have the "memorize" command.

## Where the code is today (baseline)

- **Catalog**: `assets/projects/dungeon-demo/catalog/items.cat` — entries have
  `name`, `category`, `symbol`. Only `category = rune` does anything.
- **`ItemKind`** (`DungeonWorld.h`, ~line 411): rune-centric (`isRune`,
  `runeSymbol`, glow, shared tablet mesh). Built lazily in
  `DungeonWorld::ItemKindFor` (`DungeonWorld_Load.cpp:308`).
- **`Inventory`** (`Inventory.h`): paper-doll equipment slots (incl. both hands)
  + a backpack; items are catalog-id strings. **No weight / carry-load anywhere.**
- **Hand commands**: `GameUI::OnHandRightClick` (`GameUI.cpp:141`) is hardcoded —
  offers "Memorize" only if `RuneSymbolFromItemId` matches. Not data-driven.
- **Icons**: `ItemIconBank` (`PartyHud.h:63`) maps catalog id → texture; filled by
  `Game::LoadRuneIcons` (`Game.cpp:797`) for runes only. Floor items all reuse the
  rune tablet mesh.
- **Character stats**: `strength` exists (`Character.h:35`); no derived load yet.

## Plan — first slice (decided: placeholder visuals, all four mechanics)

### Phase 1 — Generalize the item data model
- Add fields to `items.cat`: `weight` (float, kg) and `command` (space/comma
  list of command ids, e.g. `eat`); keep `category` free-form
  (`rune|weapon|armor|clothing|food|misc`).
- Extend `ItemKind`: add `category` (string or small enum), `float weight`,
  `std::vector<std::string> commands`. Keep `isRune`/`runeSymbol` as the
  rune specialization. `ItemKindFor` parses the new fields; runes implicitly
  get the `memorize` command so existing behaviour is unchanged.

### Phase 2 — New items + placeholder visuals
- Add a few catalog entries across categories, e.g. `dagger` (weapon),
  `leather_armor` (armor), `tunic` (clothing), `apple`/`bread` (food, `command = eat`).
- Floor mesh: reuse the shared tablet mesh, drawn with a neutral stone set + a
  per-category tint (no new asset pipeline).
- Icons: generate one solid category-tinted placeholder texture per category at
  startup and register each new item id → its category icon in `ItemIconBank`
  (code-only; no PNG authoring). New en.lang `item.*` display strings.

### Phase 3 — Weight → carry load
- Derived total: sum of weights over a character's equipment + backpack. Weight
  lives in `ItemKind` (world), `Inventory` is pure data → add a weight-lookup
  seam (`std::function<float(const std::string&)>`) wired from Game into GameUI,
  mirroring the existing hooks.
- Max load derived from `strength` (e.g. `strength * k`). Show "Load: X / Y" on
  the character sheet; tint when over. (Over-load *penalty* — slowed movement —
  is noted as a later follow-up, not this slice.)

### Phase 4 — Data-driven hand commands
- Replace the hardcoded rune check in `OnHandRightClick` with a loop over
  `ItemKind::commands`, mapping each command id → {loc key, handler} via a small
  registry. First handlers: `memorize` (existing) and `eat` (food: restore some
  stamina/health, consume the item). GameUI reaches the command list via a seam
  like the weight one.

### Phase 5 — Save round-trip + testing
- Carry load is derived (no save); commands are data (no save). Items already
  save by id (equip/backpack lines), and eat just clears a slot → covered.
  Verify nothing new is dynamic per the save-state checklist
  (`CaptureState`/`ApplyState` + `SaveData`).
- Test via `docs/drive.ps1`: place the new items in `level1.ent`, launch, pick
  up / drop, read carry load on the sheet, eat food and confirm the slot clears
  and stamina rises. Screenshots into `docs/`.

### Out of scope (note for later)
- Real authored/imported models for items (kept placeholder for now).
- Over-encumbrance penalty (movement slow); equip/wield commands; stacking.

## Status

- **Phase 1 — DONE.** `ItemKind` carries `category`/`weight`/`commands`;
  `ItemKindFor` parses them (runes implicitly gain `memorize`). Compiles.
- **Phase 2 — DONE (code), visuals need polish.** New catalog items
  (dagger/leather_armor/tunic/apple/bread) with `weight` + `eat` commands; loc
  strings; non-rune floor render = tablet mesh, category-tinted; generated
  solid-tint placeholder icons per item. Placed in level1.ent for testing.
  Verified: builds, loads without crash, scene renders, items present (map
  overlay shows green Item markers).
  - **Polish gap**: non-rune items are a *flat floor slab* viewed near edge-on
    and cast no light pool (unlike runes), so they read faintly even with the
    emissive bumped to 0.55. A chunkier/standing placeholder mesh or a small
    cast light would fix it — deferred.
  - **Verified manually** (user, 2026-06-22): pickup + tinted hand icon work.
- **Phase 3 — DONE.** `Character::MaxCarryLoad()` (strength × 5, STUB); an
  `ItemWeightBank` (catalog id → kg) Game fills from the catalog and passes to
  the sheet via `SetItemWeights`; `CharacterSheet::CarryLoad()` sums equipment +
  backpack; a "Load: X / Y kg" line on the inventory page (turns red over
  capacity). Verified on screen: fresh party shows "Load: 0.0 / 80 kg" (str 16).
  The load updates as items enter inventory (sum is trivial; user can confirm
  via the manual pickup path). No over-encumbrance penalty yet (display only).
  - NOTE on driving: UI clicks DO work via PostMessage (mouse pos comes from
    WM_MOUSEMOVE, Window.cpp) — the sheet opened by clicking a portrait. The
    floor-item pick just needs accurate pixel coords on the 1600x900 client;
    small dim tablets are easy to miss.

## Gotcha: worktree texture provisioning (cost us a long detour)

This worktree was created without the gitignored texture sets. The whole scene
rendered as the **magenta/black missing-texture placeholder** (AssetUtil.cpp).
Causes + fix:
- The initial `robocopy ... /XO` (exclude-older) silently copied **zero** `.dds`.
- Copying only `*.dds` was **not enough** — the scene stayed magenta until the
  source **PNGs** were also present (123 png + 123 dds, matching a populated
  worktree). Provision the **full** `assets/textures` tree (PNG+DDS) into BOTH
  the worktree source and `build/<cfg>/bin/assets`, via `FetchTextures.ps1` or a
  plain `robocopy <populated>/assets/textures <dst> /E` (no `/XO`).
