# Party creation - plan

From `party-creation-notes.md` (Michael's brain dump and his answers, 2026-10-02).
Goal: after choosing a world on Start New Game, the player builds a party of 1-4
members - name, portrait, race, colour, a few stat points, two boosted starting
skills and two low-quality starting items each - or fills the page with today's
four through Default party, and starts. No classes: a character is what they do.

## The rules, as decided

- RACES (human / elf / dwarf / orc) are DATA, a new `races.cat` per world: stat
  modifiers (from 10), an extra free-point count, a pace nudge, nature resists,
  the base resources a member of that race starts with, the portrait tag it
  filters the picker by, and a lang key. Michael's approved first table:
  human (+2 free points), elf (DEX +2 INT +1 / VIT -2 STR -1, faster), dwarf
  (VIT +2 STR +1 / DEX -1 INT -1, resists poison, slower), orc (STR +3 VIT +1 /
  INT -2 WIL -2, resists bash). Every number tunable.
- STATS = race base + 5 FREE POINTS (plus the race's extra), spent one at a time,
  never below a floor (say 3). Moving points can be undone; nothing else costs.
- SKILLS: pick 2 of the trainable skills; each starts at LEVEL 2 (xp 4). Applied
  before the maxima / pace re-derive, since some skills feed pools and pace.
- ITEMS: pick 2 from a world's STARTING LIST (low-quality picks from existing
  catalogs: dagger, club, padded jack, tunic, rock, a flask, food, waterskin...).
  The list is data (the project manifest), so the later sword / potion / wand /
  ring branch only appends to it. A weapon goes in a hand, armour on the body, the
  rest in the pack.
- COLOUR: picked per member, SAVED WITH THE CHARACTER. The Settings palette stays,
  but only recolours the current party; it no longer overwrites the roster on
  every new game and load. Its ini values become the colours offered to new
  members.
- DEFAULT PARTY fills the page with today's four exactly as CreateDefaultParty
  makes them (the eval suites measure them, so they do not change). They are
  PREMADE: name, portrait and colour are editable; changing a premade member's
  race turns them into an ordinary created member (race base + free points, no
  kit). Their races are human (their portraits are).
- The HARNESS (`newgame`, `reset`) keeps skipping the page and keeps the default
  four.

## Where things stand (survey)

- Saves carry no name, race, colour, pace or roster size; LoadGame rebuilds the
  default four and lays the save over them. The roster is never resized.
- The HUD and sheet are resize-safe (by index, RebuildForRoster for a new size),
  but a party of 1 or 3 has never run: the formation is by slot (0-1 front, 2-3
  rear), and a few places assume 4 (threat std::array<float,4>, the quadrant
  lanes, SpellbookPanel's loop, AllocTest's member loop).
- The portrait catalog loads with the game (a load task), after the title screen;
  the item catalogs come with the world.
- No `classKey` exists in code; four `class.*` lang keys are unused.

## Phase 1 - the data: races, a created member, and saving it

- `races.cat` in the project (and the world template, so a blank world has the
  four): one block per race with the fields above. Loaded with the project like
  any catalog; type-editable in the editor later for free (a schema table).
- A pure `Game/PartyBuild.h`: the member a choice describes - `MemberSpec` (name,
  race id, portrait id, colour, points spent per stat, 2 skill ids, 2 item ids,
  premade index or none) -> a Character, through ONE function used by the page,
  the dev command and the tests. In RollTest, like the other pure rules.
- `Character::raceId`; `natureResists` filled from the race (its intended job).
- SAVE: a roster-size line and per-member `name`, `race`, `color`, `pace` lines.
  LoadGame resizes the roster to the save (and asks for the deferred
  RebuildForRoster); a save without them loads as today. No version bump.
- `ResetRoster` takes the PARTY TO START WITH (the created one, else the default
  four) instead of always CreateDefaultParty.
- Colour: ApplyMemberColors stops overwriting on new game / load.
- Dev: `party new <spec>` builds a party from a one-line spec (the harness's way
  in, and the page's twin), `party` prints it. The `class.*` keys go.
- Checked by: an eval suite - a 2-member created party survives save -> reset ->
  load with its names, races, colours, stats, skills and items; a RollTest case
  for the spec -> character rules (points, floors, race bonuses, skill levels).

DONE (2026-10-02). What it came to:
- The dev commands are `newparty default | <member> | <member> ...` (key=value
  words) and `roster` - `party` already existed (the pools, read by scripts),
  so it was left alone. The save's size line is `roster <n>`: `party` is the
  party's position line.
- Poison deals EARTH damage (effects.cat), so the dwarf resists `earth 0.25`.
- Race bases are first cuts: human 20/23/13, elf 18/22/16, dwarf 23/24/10,
  orc 24/25/8 (health/stamina/mana); paces 1.0 / 1.1 / 0.9 / 1.0.
- `DungeonWorld::TrainableSkills` lifted out of SeedPartySkills (now
  de-duplicated) is what a starting-skill pick is checked against.
- A premade member given no name / colour keeps its own (`MemberSpec::colorSet`).
- Checked: partycreation.eval PASS (Aria the elf 9,14,8,13,11 and Old Tom the
  dwarf 13,9,15,10,9, dagger in hand + apple in pack / jack worn + club in hand,
  identical after save -> reset -> load); RollTest 343 checks PASS (22 new);
  CheckAll quick tier PASS; all ten eval suites PASS (the default four
  unchanged).

## Phase 2 - parties of one to four, in play

- Walk every 4-assumption: threat per member (sized to the roster), the quadrant
  lanes and formation for 1 and 3 (a lone member stands front-left; three = two
  front, one rear-left), the rear-rank reach rule, the spellbook loop, the
  member-to-member fumble neighbour, the HUD's 2+1 hand layout.
- Checked by: eval runs with a party of 1, 2 and 3 (`party new`) through the
  existing combat / resources suites' shape; AllocTest default and `-Sheet` with a
  3-member party; InGameTest's HUD sweep with 1 and 3.

## Phase 3 - the creation page

- `MenuPage::PartyCreation` (GameUI, its own TU), reached from the world pick
  (and straight from Start when there is one world). The world is switched in
  FIRST (the deferred SwitchWorld), so its catalogs - items, races - are there;
  the portrait catalog moves into the boot load so the picker has it on the title
  screen.
- Layout (a PageCard in the stone house style): a row of up to four member
  slots (portrait + name, an Add slot while fewer than four, a remove mark), and
  the selected member's editor - name (ui::TextField), portrait (opens the
  picker filtered to the race), race (drop-down; shows its bonuses), colour
  (ui::ColorPicker), stats (five rows with - / + and the points left), skills
  (pick 2), items (pick 2). Footer: Default party, Back, Start (refuses with a
  reason until every member has a name and a portrait).
- Start: the built specs go to Game, StartNewGame builds the roster from them,
  RebuildForRoster runs deferred, the game loads.
- Lang keys x5 for everything on it.
- Checked by: `uioverlap` over the page (empty, one member, four, the picker open
  over it); InGameTest gains `sweep_partycreation`; a scripted build through the
  page's own dev twin (`party page ...`) reaches Playing with the party it built.

## Phase 4 - colour in Settings, docs, handover

- Settings -> UI's member colours edit the CURRENT party (live, saved with the
  game) and their ini values are the new-member defaults.
- CLAUDE.md: races, the creation flow, the save lines, the 1-4 rules; the notes
  closed out.
- Launch a fresh game and hand it to Michael: build a party, play, save, load.

## Not in this branch

- New items (sword, healing potion, wand, ring) and an item quality field - their
  own branch, which appends to the starting list.
- Voices (with the sound work).
- More races (undead, demon) - a races.cat block each when wanted.
