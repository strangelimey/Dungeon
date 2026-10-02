# Party creation - notes

Michael's brain dump for party creation, taken down as he gives it. Organised
and turned into questions afterwards, then into `party-creation-plan.md`.

## What exists already (2026-10-02, from main a6de980)

- The party is fixed: `CreateDefaultParty` (src/Game/Character.cpp) makes
  Brand (fighter), Sera (rogue), Maren (cleric), Tilo (mage) with authored
  stats, resource bases, move speeds, starting kit and portraits.
- The roster is already SIZE-SAFE: the HUD and sheet address members by
  (roster, index) and re-resolve every frame, and the party bar always reserves
  four slots, so a party of 1..4 lays out. A size change goes through
  GameUI::RebuildForRoster. Nothing can build a party of other than four yet.
- The portrait picker (Game/PortraitPicker) was built standalone for this:
  Open(title, currentId, onPick), race / sex / age filters over 2879 tagged
  portraits.
- Stats: STR / DEX / VIT / WIL / INT; classes exist only as `class.*` lang keys
  (fighter / rogue / cleric / mage); skills train by use.
- New game: the title menu's "Start New Game" -> a world list -> StartNewGame,
  which resets the roster to the default party.

## Brain dump

1. WHEN: party creation opens AFTER picking a world, BEFORE the game starts.
2. A "Default party" button creates the party we have now (Brand, Sera, Maren,
   Tilo as CreateDefaultParty makes them).
3. Each member gets a NAME, a PORTRAIT and a RACE.
4. There is NO CLASS. A character is whatever they do - they get better at what
   they do (the skills-train-by-use model). (Today's `class.*` keys and the
   classKey are labels with nothing behind them.)
5. The party has 1 to 4 members.
6. LATER (with the sound work, not this branch): a character could have a
   VOICE - girl, boy, high pitched, low pitched, etc.
7. Each member SELECTS 2 PIECES OF STARTING EQUIPMENT, of LOW QUALITY: sword,
   dagger, potion, wand, ring, etc.
8. Each member can PICK SOME STARTING SKILLS (how many: not decided) that start
   BOOSTED above 0 instead of from nothing.
9. RACE MATTERS: it gives certain physical / mental BONUSES and WEAKNESSES.
10. Each member's CHARACTER COLOUR can be picked - the colour used for the
    portrait bar, the hand icons and their log text. (Today it comes from a
    settings palette, GameSettings::memberColors via Game::ApplyMemberColors.)

(End of brain dump.)

## What the code says (survey, 2026-10-02)

- There is no `classKey` field in code at all - only four unused `class.*` lang
  keys. "No class" costs nothing to honour; the keys can go.
- A save does NOT carry a member's name, move speed, colour or race, nor the
  roster size: LoadGame resets to CreateDefaultParty and lays the save over it.
  A created party needs new save lines.
- The member colour is a SETTING (Settings -> UI picker, ini `member_<n>`) that
  overwrites the roster on every new game and load.
- Of the brain dump's example items only the DAGGER exists (weapons.cat:
  dagger, khukri, the enchanted blades, club; armor; flasks, food, runes,
  bags). No sword, potion, wand or ring; no item quality field. The ring and
  amulet wear slots exist but nothing fills them. No healing source exists.
- No race system: portraits.cat has race tags (human / elf / dwarf / orc /
  undead / demon / other) and Character::natureResists is an unused resist
  table "for the race layer".
- Skills seeded at 0 for everyone: the four schools, constitution /
  conditioning / attunement, unarmed / throwing / avoid, light / medium /
  heavy armour, blade / blunt. Level = floor(sqrt(xp)). A seeded skill must be
  followed by the re-derive (pools and pace depend on some skills).
- The formation is by roster slot: 0-1 front, 2-3 rear (rear needs reach to
  melee). A party of one or three is untested anywhere.
- Text entry exists for a menu page (the Save page's ui::TextField).
- The harness (`newgame`, `reset`) starts games through callbacks below the
  menu, so a creation page added at the menu level leaves it on the default
  party.

## Organised

**What is being built:** a PARTY CREATION page between choosing a world and the
game starting: 1-4 members, each with a name, portrait, race (with bonuses and
weaknesses), colour, two low-quality starting items and some boosted starting
skills; and a Default party button for today's four.

**Decided without asking (sensible defaults):**
- The page appears after a world is chosen on Start New Game (and straight after
  Start when there is only one world). The harness's `newgame` / `reset` skip it
  and keep the default party.
- The portrait button opens the existing picker, pre-filtered to the member's
  race.
- A created party round-trips: new per-member save lines (name, race, colour,
  pace) and the roster size; an old save without them loads as today.
- The default four get races that match their portraits.

**Open questions (asked one at a time):**
- Q1. Stats: chosen by the player, or set by race? ANSWER: RACE + A FEW POINTS -
  the race sets the base (from 10, its bonuses and weaknesses), then the player
  spreads a small pool on top.
- Q2. The playable races, and what each gives. ANSWER: HUMAN, ELF, DWARF, ORC
  (the classic four; ~2500 portraits between them). Claude's table APPROVED,
  every number in a `races.cat` to tune later. Stats from 10; STR/DEX/VIT
  physical, WIL/INT mental; then 5 free points:
  - Human: no changes; +2 extra free points (versatile).
  - Elf: DEX +2, INT +1 / VIT -2, STR -1; a little faster on foot.
  - Dwarf: VIT +2, STR +1 / DEX -1, INT -1; resists poison; a little slower.
  - Orc: STR +3, VIT +1 / INT -2, WIL -2; resists bash.
- Q3. Starting skills: how many, and how big a boost. ANSWER: 2 SKILLS, each
  boosted TO LEVEL 2 (xp 4); they still grow by use after.
- Q4. Starting items: author the missing ones (sword, potion, wand, ring) in this
  branch, or offer what exists now? ANSWER: EXISTING ITEMS NOW - low-quality
  picks from what the catalogs already hold (dagger, club, padded jack, a flask,
  a rock, food...). Sword / potion / wand / ring are their own later branch and
  join the list when built (so the list is DATA, not code).
- Q5. Colour: does it move from Settings onto the character? ANSWER: BOTH - the
  colour is saved ON THE CHARACTER (picked at creation, kept by the save), and
  the Settings palette can still RECOLOUR THE CURRENT PARTY. It stops
  overwriting the roster on every new game / load.
- Q6. Default party: start at once, or fill the page so it can be edited?
  ANSWER: FILL THE PAGE with today's four (names, faces, stats, kit), editable,
  then Start.
