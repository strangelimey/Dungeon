# Portraits - notes

Michael's brain dump for the portrait work, taken down as he gives it. Organised
and turned into questions afterwards, then into `portraits-plan.md`.

## Already decided (2026-10-01, before the branch)

- Art: Magory "Fantasy Character Portraits Pack" (itch.io, $7.99, AI-assisted;
  docs/costs.md). Archived in `OneDrive\DungeonAssets\ui\magory-fantasy-portraits\`.
  The plain set is `256square/256x256/`: 2458 portraits (2104 `portrait###` +
  bonus `b###` / `d###`).
- Every portrait is tagged in `tags.tsv` beside the zips: race (human / elf /
  dwarf / orc / undead / demon / other), sex (male / female / unclear), age
  (young / adult / old / unclear), look (warrior / rogue / mage / cleric /
  noble / commoner, `+` joins two), framing (face / bust / figure), and which
  fields were a guess.
- Look is weak as a class filter: most portraits are face crops with no gear in
  frame, so 75% are "commoner".
- Full-body figures (framing = figure, 72 of them) are DROPPED from the shipped set.
- The selection screen gets a list of categories to filter by.
- Today the four party portraits are committed PNGs, `assets/ui/portrait_<name>.png`,
  loaded by Game::LoadPortraits - not baked.

## Brain dump

1. The selection screen's real home is PARTY CREATION - which does not exist yet.
   Party creation is a later job.
2. For now: a button on the CHARACTER SHEET that changes that member's portrait
   (opens the selection screen).
3. The filters are RACE, SEX and AGE (no look / class filter).
4. The screen shows a GRID OF THUMBNAILS; clicking one picks it.

(End of brain dump.)

## Organised

**What is being built now:** a PORTRAIT PICKER - a window of thumbnails with
race / sex / age filters - opened from a button on the character sheet, which
sets that member's portrait. Party creation, its eventual home, is a later job;
the picker must be a standalone piece it can reuse.

**Things the brain dump implies, decided without asking (each a sensible default):**
- A click picks and closes, no separate confirm (item 4 says "click to pick").
- Each filter has an "Any" choice, the default. A portrait whose tag is `unclear`
  shows only under Any for that field; an `uncertain` guess is filtered by the
  guess.
- The chosen portrait is SAVE STATE (a member's portrait id in the save file -
  a new save line, so the save version bumps). Changing it in one game does not
  change a new game.
- The shipped set is the pack minus the 72 figures: about 2386 images. At 250 MB
  as PNG it does NOT go in git - gitignored and provisioned from the OneDrive
  archive like the scanned textures, ideally as BC7 .dds.
- The SRV heap holds 1024 textures, so the picker cannot load all of them at
  once. It loads only the thumbnails on screen and evicts the rest - the editor's
  AssetPicker tile pattern.
- The tags become a catalog (`portraits.cat` or similar) the picker reads, so
  tags are data and editable.

**Open questions (asked one at a time):**
- Q1. The starter party's DEFAULT portraits. ANSWER: Claude proposes, Michael
  approves. Approved 2026-10-01 - Brand `portrait018` (helmeted knight), Sera
  `portrait398` (green hood), Maren `portrait1419` (white hood, silver circlet),
  Tilo `b045` (old, grey-bearded, red hood). The old baked busts retire.

**Later (2026-10-02):** a second pack joined, Corax Digital Art's 500 human
heroes (bought and tagged in a forked session; 493 shipped). With Magory's 2386
that is 2879 portraits. Everything above was built as planned
(docs/portraits-plan.md, all four phases); party creation remains the picker's
eventual home.
