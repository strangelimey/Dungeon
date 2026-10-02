# ui-bars-updates - notes

Michael's brain dump for the `ui-bars-updates` branch (2026-10-02), captured as
it came in, then organized. Not yet planned.

## Notes

- The Skills tab of the character sheet shows plain, 2D bars. They need the
  same treatment as the attribute progress bars.
- The bars represent 'progress to gaining next level', right?
  (Checked in code: yes. level = floor(sqrt(xp)) and the bar is
  (xp - L^2) / ((L+1)^2 - L^2) - it empties every time a level is gained.)
- We need to pick colors. Either it goes from dark/grey -> bright/green as it
  approaches 100%, or we pick one for each skill or skill class. Try both and
  see which he likes better.
- On the "Known Spells" sheet, hovering over a rune symbol in the recipe should
  show a tooltip with the name.
- Runes in the backpack and hand are just the symbol, not a stone tablet with
  the rune on it. He likes how clear it is now, but try it as a rendered tablet
  with a glow effect.

(End of dump, 2026-10-02: "That's it for now. Let's organize.")

Done on main beside the dump: the harness rule - every harness drives its game
window by PID and screenshots with PrintWindow (main f0c85b8, CLAUDE.md +
docs/drive.ps1).

## Organized

### A. Skill bars on the sheet's Skills tab
- Today: the flat DrawStatBar (theme track, flat fill, 1 px border). CLAUDE.md
  says "the skill XP bars stay flat on purpose" - this reverses that.
- Wanted: the treatment the Stats tab's bars have - the silver frame round an
  animated, emissive procedural fill (bar.hlsl).
- They are PROGRESS bars, not pools: they empty on every level gained.
- Colour is a TRIAL of two schemes, judged side by side:
  1. GRADE - every bar dark grey when empty, brightening to green as it nears
     the next level.
  2. CLASS - a colour per skill or skill family.
- The party window's cards draw the Skills tab through the same code, so they
  change with it.

### B. Rune names on hover (Known Spells tab)
- Each spell row shows its recipe as rune glyphs before the name.
- Hovering one rune glyph shows a tooltip naming it.

### C. Rune tablets in item sockets
- Today a rune item in a socket (backpack, hand; DrawItemIcon routes every
  socket - doll, party inventory, cursor too) draws as the Magic window's
  glyph: the symbol lit over a pulsing halo (DrawRuneGlow). He likes its
  clarity.
- Try instead: a RENDERED stone tablet with the rune on it, with a glow.
- Fact to check in planning: rune items carry no `model` in items.cat, so where
  a tablet render comes from (the floor item's mesh, an icon bake) is open.

## Work started before the dump ended

Started on A before the dump ended; folded into Phase 1 of the plan and
finished there (the list below is how it stood at the time):
- `gfx::BarKind::Progress` (7) and `ProgressFill` in bar.hlsl: the caller's
  colour as a glow, brighter toward the leading edge, sparse sparks drifting
  forward; NOT dimmed as it empties (empty = just levelled).
- `FitFramedTube` + `DrawProgressBar` in PartyHudDraw; the Stats tab now uses
  FitFramedTube (same arithmetic as its old inline code).
- `ResourceBarStyle::SkillColors { Grade, Class }` and, in
  CharacterSheet_Lists.cpp, `SkillBarColor` with first-cut colours: GRADE grey
  0.30 -> green; CLASS = magic by school (ElementColor), weapons (blade, blunt,
  unarmed, throwing, any catalog weapon skill) steel, defence (avoid + the
  three armours) bronze, each reserve its pool's colour (constitution red,
  conditioning green, attunement blue).
- NOT done: DrawSkillRow still draws the flat bar; no `hudbars skills
  grade|class` command yet.

## Open questions (for the planning step)

A1. Bar height. A framed bar must fit its whole frame (glass + silver caps,
    about 1.9x the glass) inside one row, or neighbours' scrollwork collides.
    The rows are one text line apart, so the glass comes out about HALF the text
    height - thinner than today's bar. Accept that, or open the Skills rows up
    so the bar stays as tall as the text?
A2. The CLASS palette: two clashes are built into the first cut - earth magic
    and conditioning are both green, water magic and attunement both blue
    (they sit under different headings). Fine for the trial, or separate them?
A3. The loser of the trial: delete it, or keep both as a Settings -> UI choice?
B1. Tooltip text: the Futhark name alone ("Kenaz"), or name plus meaning
    ("Kenaz - Fire")?
B2. Only the Known Spells list, or everywhere a rune glyph is drawn without
    words (the Magic window's grid and sequence, a set hand's recipe)?
B3. Tooltip only, or the sheet's status bar too (it names the hovered spell
    today)?
C1. Which sockets get tablets: every item socket (hands, doll, backpack, party
    inventory, cursor), or only the hands and backpack he named?
C2. The Magic window, a set hand's recipe and the Known Spells list draw runes
    as glyphs too (they are spell symbols there, not items) - stay glyphs?
C3. The glow: a halo round the tablet, the carved rune on it glowing, or both?

## Answers (Michael, 2026-10-02)

A1. OPEN THE ROWS UP: more space between skill rows so the glass stays about as
    tall as the text (the list gets longer and scrolls sooner).
A2. LEAVE THE CLASHES for the trial; fix them later only if CLASS wins.
A3. DELETE THE LOSER: one scheme, one code path, and the console switch goes too.
    PICKED (2026-10-02): CLASS. Grade "looks like it's sickly" - green, then
    gold/silver/ember/azure/violet run from a dim shade of their own hue (a
    grey-to-colour blend was what read as swamp water) - and class still won.
    Grade and `hudbars skills` deleted.
B1. NAME AND MEANING, e.g. "Kenaz - Fire" (the Futhark name alone does not say
    what the rune does).
B2. EVERYWHERE a rune glyph shows without words: Known Spells, the Magic
    window's grid and sequence, a set hand's recipe.
B3. BOTH: the tooltip, and the sheet's status line switches to the rune's name
    and meaning while the pointer is on it.
C1. EVERY ITEM SOCKET: hands, doll, backpack, party inventory, the cursor.
C2. SPELL GLYPHS STAY SYMBOLS: tablets only where a rune is an item; the Magic
    window, a set hand's recipe and Known Spells keep the glowing glyph.
C3. BOTH: the carved rune glows in its school's colour AND a soft halo round the
    tablet pulses slowly, as the Magic window's runes do.
