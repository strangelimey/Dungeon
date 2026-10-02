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

DONE (2026-10-02). The survey's "places that assume four" were already
guarded: the formation is by slot (a lone member stands front-left, a third
rear-left), the per-file blocking rule treats a missing member as a fallen one
(so the member behind a hole is reachable), the lane hit, the wild fumble and
the threat loops all stop at the roster's end, and the HUD, sheet, party window
and Minimal cards lay out whatever count they find. So the phase was proving it,
which found three real things:
- `newparty` was refused on the TITLE SCREEN: the world is built only when a
  game starts, and the command was gated off without one. It now opens the
  default world first, as Start New Game does, and the portrait catalog loads
  with the BOOT load (`Game::LoadPortraitCatalog`) so a face can be checked
  before any game - both of which phase 3's page needs anyway.
- The Magic dock's member row drew all four buttons; a short party showed dead
  ones. A slot past the roster's end now has no button (the columns stay put).
- The four-slot tables (a monster's threat, the save's threat line, the party
  bar's slots) are static_asserted against `party::kMaxMembers`.
Checked: the new `parties` eval suite (smallparty.eval: one, three and two
members, melee from ahead and behind, an archer, a throw, casts from either
rank, the rear-rank reach rule, a rest) - from behind, the three-member party is
hurt only on Sera and Maren, never on Brand, exactly as the blocking rule says;
all eleven suites PASS. AllocTest `-Party <spec>` (new; refuses a PASS if the
party was not built): default, -Sheet, -All, -Minimal and -Cast with three
members, default and -Minimal with one, all PASS. InGameTest sweeps
`sweep_party3` (+sheet, inventory, minimal) and `sweep_party1` (+minimal), and
demands both parties were really built: PASS, all clean.

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

DONE (2026-10-02). What it came to:
- `Game/PartyCreationPage.*` is the page (MenuPage::Party, built into the menu's
  page context like the world list; `GameUI_Party.cpp` opens, rebuilds, leaves and
  starts it). It edits `party::MemberSpec`s and shows a PREVIEW of each built by
  the game's own BuildMember (+ the world's pool rules), so the numbers on the page
  are the game's. Number edits (points, picks, a typed name) leave the tree
  standing and Tick rewrites the live text; structural ones (select, add, remove,
  a race that makes a premade member anew) rebuild a frame later.
- Layout, settled by `uioverlap`: the member strip; the selected member's RACE
  LINE across the card (a dwarf's runs ~700 px, too long for a column); three
  columns - name / race / face / colour / pools, the stats with their stones,
  two skill and two item drop-downs; the refusal; Default party / Back / Start.
  The page draws no big title above its card: it needs the height (with the
  title, the columns were starved to under 6 rem and their rows squeezed).
- Premade members (the Default party) show their authored stats with no stones
  and no picks; a race change makes one an ordinary made member, keeping the name,
  face and colour. The race line is not shown for them either (a premade human
  gets no "+2 free points").
- The world is loaded first (`Game::OpenPartyCreation`; another world switches a
  frame later and opens the page when it lands). Back returns to the world list
  it came from, or the title. The Editor entry, `newgame` and `reset` skip it.
- Esc: the face picker first, then an open list (a DropDown now closes on Esc, as
  the colour picker did; `UIContext::PopupOpen` says one took it), then the page.
- The dev twin is `partypage` (`party` was taken): open / add / default / back /
  start / picker / select / remove / set k=v / spend / skill / item, every verb one
  of the page's own edit methods. `party::ApplySpecField` (pure, RollTest) is the
  key=value parser `newparty` and `partypage set` share.
- Checked: tools/EvalScripts/partypage.eval (run on its own - it starts on the
  title) PASS, the started roster exactly what the page showed; uioverlap clean
  over a member of every race, four members, the default four and the picker over
  the page; InGameTest sweeps `sweep_partycreation` / `sweep_partydefault` /
  `sweep_partypicker` on the title (demanding the page and the picker really
  opened) PASS; RollTest 354 PASS; by mouse - title -> page -> Default party ->
  Start -> playing, a typed name, the stones, a face from the grid, a skill from a
  list, Esc twice, and the world list -> Test-World -> Back -> Dungeon Demo.
- Main was merged in here (c56d095): breakables, and InGameTest / ProfileTest
  starting by `newgame` - which this page needed, since Start New Game no longer
  starts.

## Phase 4 - colour in Settings, docs, handover

- Settings -> UI's member colours edit the CURRENT party (live, saved with the
  game) and their ini values are the new-member defaults.
- CLAUDE.md: races, the creation flow, the save lines, the 1-4 rules; the notes
  closed out.
- Launch a fresh game and hand it to Michael: build a party, play, save, load.

DONE (2026-10-02). Settings -> UI -> Party Colors follows the party: with a game
in play (paused) row n names member n and shows THEIR colour, and an edit
recolours them at once (saved with the game, as every member's colour is) and
sets slot n's default too; on the title, or for a slot a short party leaves
empty, the row is "Member n" and edits only the default a new member in that
slot starts with (the page's Add uses it; the default four wear them).
`GameUI::SyncMemberColorPickers` re-labels the rows when the page opens or is
rebuilt; `partyInPlay` (wired by Game: loaded and not on the title) decides.
Checked by hand: a created elf's row showed her own blue, not the slot's
orange; raising its red gave fda0ff on her (from her blue) and member_1 in the
ini; the title showed Member 1-4 at their defaults; uioverlap clean over the
UI tab with a 15-character name in row 1.

THE BRANCH IS DONE. What is left is the play-test.

## Not in this branch

- New items (sword, healing potion, wand, ring) and an item quality field - their
  own branch, which appends to the starting list.
- Voices (with the sound work).
- More races (undead, demon) - a races.cat block each when wanted.
