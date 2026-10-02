# ui-bars-updates - plan

Built from docs/ui-bars-updates-notes.md (Michael's dump of 2026-10-02 and his
answers A1-C3). Three phases, in order, each ending with him judging it in the
running game. Every harness and screenshot follows the PID / PrintWindow rule
(CLAUDE.md, docs/drive.ps1).

## Phase 1 - Skill bars (notes A)

The Skills tab's flat bars become framed, animated PROGRESS bars, coloured by
one of two schemes on trial.

1. **The fill** - `gfx::BarKind::Progress` (7) and `ProgressFill` in bar.hlsl
   (already drafted): the caller's colour as a glow, brighter toward the
   leading edge, sparse sparks drifting forward; not dimmed as it empties
   (empty = just levelled). Calm, like Effort - the sheet is read, not glanced at.
2. **The frame** - `FitFramedTube` + `DrawProgressBar` (PartyHudDraw, drafted);
   the Stats tab already moved onto FitFramedTube (same arithmetic as before -
   check its bars land on the same pixels).
3. **Open the rows up (A1)** - `MeasureSkillRow` grows the row pitch so the
   whole frame fits round a glass about 0.8 of the text height (frame = glass
   x ~1.9, so a pitch of ~1.6 text heights). One constant for the glass share,
   used by Measure and Draw alike. `DrawSkillRow` centres the bar on the text
   line and draws it through DrawProgressBar; the flat look (uiskin=0) keeps
   DrawStatBar in the same colour.
4. **The two schemes** - `ResourceBarStyle::SkillColors { Grade, Class }`, read
   at draw time so a flip needs no rebake (drafted: `SkillBarColor` in
   CharacterSheet_Lists.cpp). GRADE: grey 0.30 -> green by the bar's fraction.
   CLASS: magic by school, weapons steel, defence bronze, reserves their pool's
   colour (A2: the earth/conditioning and water/attunement clashes stay for
   the trial). The status line keeps its current colouring.
5. **Dev** - `hudbars skills grade|class` flips it; `hudbars status` prints the
   mode. The party window's cards draw the same rows, so they change too.
6. **Check** - both configs build; `uioverlap` on the sheet's Skills tab and a
   card; `AllocTest.ps1 -Sheet` and `-All` PASS (fills are sprites, nothing is
   allocated per frame); `/check-ingame` sweep. Then hand him the game with the
   Skills tab open to flip between the schemes.
7. **After his pick (A3)** - delete the losing scheme, the enum and the console
   switch; CLAUDE.md's "the skill XP bars stay flat on purpose" is rewritten.

## Phase 2 - Rune names on hover (notes B)

1. **The text** - a new `rune.tip = {0} - {1}` key x5 languages, filled with
   `rune.<id>` (Futhark name) and `symbol.<id>` (its meaning): "Kenaz - Fire".
   Formatted into a fixed buffer (loc::FormatLine), so a hover allocates nothing.
2. **One drawer** - `DrawRuneTip(ctx, batch, anchor, symbol)` in PartyHudDraw,
   placed by `ui::PlaceTooltip` like every other tip, in the tooltip face the
   hand box's tip already uses.
3. **Every site (B2)** - each records the hovered rune's symbol + rect in Update
   and draws the tip in its overlay pass:
   - Known Spells rows (CharacterSheet_Lists DrawSpellRow - the recipe glyphs);
   - the Magic window's rune grid and sequence (SpellbookPanel already tracks
     `m_hotSymbol` / `m_hotSeq`);
   - a set hand's recipe (HandSlot) - over a rune, the rune's tip REPLACES the
     hand's "what a click does" tip; elsewhere on the box the hand tip stays.
4. **Status line (B3)** - on the sheet, while over a recipe rune, the status
   line shows the rune's name and meaning instead of the spell's.
5. **Dev / check** - `sheet status` and the spellbook's status report the
   hovered rune; `AllocTest.ps1 -Sheet` and `-Cast` hover a rune inside the
   guarded window and PASS; `uioverlap` covers the tip.

## Phase 3 - Rune tablets in item sockets (notes C) - a trial

The world already draws a rune item as `rune_tablet.gltf` wearing the rune's own
texture set (`rune_<symbol>`), pulsing in its school's colour on the floor. The
sockets draw the flat-tile PNG's glyph instead (DrawRuneGlow).

1. **Bake a tablet icon** - give each rune ItemKind an `iconTarget` and bake it
   at load with the shared studio rig: the tablet mesh + its material from
   `FillItemPreview` (already used by the details dialog), posed FACE-ON (the
   flat pose that dialog uses). Needs a `BakeIcon` variant taking that submesh
   span instead of a MultiMaterialModel. Static: baked once, like any model icon.
2. **The glow (C3: both)** - drawn LIVE over the baked tablet by DrawItemIcon:
   the soft halo (rune_glow_<id>, tinted, the Magic window's slow pulse) behind
   the tablet, and the glyph mask (rune_glyph_<id>, tinted) laid over the
   CARVING so the cut rune itself glows. The carving's rect inside the icon is
   measured once and held as constants; a dev overlay (`runeicons carve`)
   outlines it so the fit can be checked by eye.
3. **Where (C1/C2)** - every ITEM socket (hands, doll, backpack, party
   inventory, cursor) through DrawItemIcon. The Magic window, a set hand's
   recipe and Known Spells keep the bare glyph (they draw through DrawRuneGlow
   directly, not DrawItemIcon - unchanged).
4. **Trial switch** - `runeicons tablet|glyph` flips DrawItemIcon between the
   new tablet and today's glyph, live, for side-by-sides. After his pick the
   loser and the switch go (the A3 rule).
5. **Risks** - legibility at hand-box size (~40 px): the trial is how we find
   out, and the glyph overlay is what keeps the rune readable; the carving
   offset if the texture's carving is not centred; nine more icon targets
   (SRV gauge +9, negligible).
6. **Check** - both builds; `AllocTest.ps1 -Items` (a rune moved pack -> floor
   -> pack) and `-Throw`; `/check-ingame` sweep; PrintWindow screenshots of a
   hand, the backpack and the cursor in both modes for him to judge.

## Not in this branch

- Retuning the CLASS palette's clashes (only if CLASS wins).
- Any change to the floor tablet's look or the Magic window's runes.
