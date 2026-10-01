# UI updates (second thread) - plan

Branch `ui-updates`, worktree C:\Dev\Dungeon-ui-updates. Notes, organized themes
and Michael's answers: docs/ui-updates-2-notes.md. Ten phases, ordered so each
one stands on the last: text first (every later look depends on it), then the
skin and colour work, then the HUD rework, then the two gameplay features.

Every phase ends with: build both configs, `uioverlap` over the screens it
touched, the AllocTest modes named, and the game HANDED TO MICHAEL fresh for a
look (visual judgement is his - see hands-on-visual-testing). One commit per
phase.

## Phase 1 - Text that reads on stone (answer 2: clearer face AND outline, everywhere on stone)

Today glyphs are plain coverage in an atlas (Font.cpp:178), one quad per glyph
cut exactly to the bitmap (Font.cpp:235), through sprite.hlsl's
`sample * color`. Nothing outlines; the one shadow is hand-rolled in
CharacterSheet_Stats.cpp:119.

OUTLINE, done in the shader rather than by drawing 8 offset copies (9x the
quads, and a ragged edge at small sizes):
- Font: raise `kGlyphPad` from 1 to the outline radius + 1, and at Commit write
  a DILATED copy of each glyph's coverage into the atlas's spare RGB (today
  white). Alpha stays the fill; RGB becomes the outline mask.
- SpriteBatch: a third mode, `Mode::Text` (the `Mode::Bar` pattern - its own
  PSO, a switch flushes), whose shader puts the fill colour over the outline
  colour: `rgb = lerp(outline, fill, a)`, `alpha = max(a, dilated) * color.a`.
  Glyph quads grow by the radius so the outline is not clipped.
- WHO GETS IT: a SpriteBatch state, `SetTextOutline(color | none)`, set by each
  UIContext's draw pass when it is skinned and cleared after. Every one of the
  ~110 `Font::Draw` sites inside a skinned context gets the outline with no
  per-site edit; flat mode (uiskin=0), the editor dialogs and the dev console
  stay as they are. The Stats hand-rolled shadow is deleted.
- Radius in px from the font height (about 1px at 17px, 2px at 28px+).

CLEARER FACE: the Body role is Spectral (thin serifs). Trial the installed faces
on stone with the console (`font body <name>`): Bitter, Alegreya, Marcellus,
Gentium Book Plus - Michael picks in-game. If he wants a HEAVIER weight than
the Regular we have, fetch the OFL Bold/SemiBold of the chosen family (free, but
a download - ask first, and record it in docs/costs.md). Note the Body role also
serves the editor dialogs; if the new face reads worse there, add a role for
stone text rather than splitting by site.

Checks: uioverlap sweep (InGameTest) - a heavier face is wider, so this phase
is the one most likely to make a row overrun; AllocTest default + -Sheet.

## Phase 2 - Skin the drop-down, checkbox, text field and slider (answer A)

All four draw flat from Theme (`control` brown fill, `panelBorder` tan border,
`accent` yellow; Controls.cpp:322, 399, 573, 1195). The target is the SELECTED
SETTINGS TAB: `Face::ButtonDown` (stone x0.95, the sunken frame with an 18%
centre veil) plus a `controlActive` wash (Controls.cpp:279-292).

- One helper, `ui::DrawFieldFace(ctx, batch, rect, state)` beside DrawSlotFace:
  ButtonDown + an extra darkening veil (he asked for DARKER than the tab), a
  faint lift on hover, a soft accent EDGE GLOW (not a yellow border) on
  focus/open. Flat mode keeps today's draw.
- Drop-down: the face is a field face; expander unchanged; text `theme.text`
  (outlined by Phase 1).
- Text field: field face; focus = the edge glow; caret stays accent.
- Checkbox: the box is a small field face; the tick an accent mark with a glow;
  the row's hover/highlight wash stays.
- Slider: the track is a thin field-face groove, the fill an accent strip, the
  thumb a raised `Face::Button` so it reads as the thing you grab.
- Settings pages, pause, and every skinned dialog pick this up at once.

Checks: uioverlap (the inset frame eats a few px of each control - check text
still fits), AllocTest default.

## Phase 3 - Member colours: brighter, plus a glow (answer 3)

The defaults are dark on purpose (GameSettings.h:75-81: rust / moss / gold /
indigo) and sink into stone.
- New brighter, more saturated defaults, chosen ON STONE in-game with Michael
  (the Settings > UI Party Colors pickers already edit them live).
- MIGRATION: settings.ini stores `member_<n>=`, so an existing ini keeps the old
  dark values forever. On load, a value equal to the OLD default is replaced by
  the new default; a colour the player changed is left alone.
- A GLOW helper (`DrawMemberGlow`: the member colour through
  assets/ui/glow_radial.png or a soft 9-slice halo) used wherever a member
  colour marks something: the Magic panel member buttons
  (SpellbookPanel.cpp:100-133), the portrait identity border
  (PartyHudDraw.cpp:141), and Phase 4's hand border.
- The log line tint (GameUI.cpp:2088) re-checked against the new values.

## Phase 4 - Hand controls: a member border instead of the stripe (answer C)

- Delete the identity stripe (HandSlot.cpp:115-117).
- `HandPair` (ControlBar.h:130, the widget holding both hands + the effort
  meter) draws ONE member-colour border with the Phase 3 glow around all three.
  Its layout grows a border inset so the border sits inside the pair's own
  area (the "chrome overlapped the container" rule from the bar frames).
- Same widget in the Standard Hands dock and the Minimal cards, so both change.

Checks: uioverlap hud (Standard and Minimal), AllocTest -Panels -Minimal.

## Phase 5 - The effort bar, alive but quiet (answer D)

The effort meter is `GuardSlider` (GuardSlider.cpp:192-236), all flat rects:
a green-to-yellow honest fill and a red over-exertion overlay.
- New `gfx::BarKind::Effort` in bar.hlsl: a glass tube look (inner shadow,
  highlight) with a SLOW, low-amplitude shimmer in the honest fill and a slow
  ember breathe in the over-exerted part. Under the resource bars' `kSubdue`, and
  slower still - he asked for low key.
- The slider's interaction and colours' meaning are untouched; only DrawSelf.
- Real time (the bars' `TickResourceBars` clock), so rest's 60x never speeds it.
- Frame: try it unframed first; the meter is 0.6em tall and a 3-slice frame may
  swamp it. If it looks naked, a thin variant of the Phase 7 silver frame.

## Phase 6 - Runes glow and pulse in their school's colour (answer D)

Runes draw through `DrawRuneFace` (PartyHudDraw.cpp:164-189) from
assets/ui/rune_icon_<id>.png. Scope: the Magic window (SpellbookPanel's rune grid
and its sequence row).
- A soft GLOW behind the symbol, pulsing slowly (each rune a little out of phase
  so the grid does not throb in unison), coloured by school through
  `ElementColor` (Spells.cpp:79) for Fire / Earth / Air / Water, WHITE for the
  form runes (ElementColor gives them gold - the Magic window overrides that).
- The glow image is made by a committed script (`tools/BuildRuneGlow.py`: blur
  each rune icon's symbol mask into rune_glow_<id>.png), the house rule that the
  script is the asset. The symbol itself is drawn brightened over its glow.
- Earth's brown and Air's white are checked on stone; Earth may need its own
  UI shade.

Checks: AllocTest -Cast (the book is open in it).

## Phase 7 - The silver bar frame (answer 1: Mana bar #13)

- tools/CutBarFrame.py finds the tube by RED fluid (`red_mask`, :68). Add
  `--fluid red|blue` (a blue mask), point it at "Mana Status Bars (13).png",
  and loosen the column test for wisps that do not fill the glass evenly (fill
  the hole between each column's top and bottom run). The key ramp was tuned for
  near-black iron; the silver may need its own.
- Re-paste the seven printed constants into PartyHudDraw.cpp:26-34. The frame's
  REACH changes, so StatsArea's tube sizing moves with it - re-check the party
  bar and the sheet's five bars.
- The script stays committed with both sources documented.

Checks: uioverlap hud + `sheet`, screenshots of the bars at two window heights.

## Phase 8 - The closed-windows tray (answers 4-6)

Today only Movement and Magic minimize (`hudMoveCollapsed`/`hudMagicCollapsed`),
collapsing to their header strip; the Hands dock and the plates cannot.
- HIDDEN becomes per-panel state: `HudPanelLook::hidden` (ini
  `hud_<panel>_hidden`; the two old `_collapsed` keys load into it). A panel is
  shown when its own `shownWhen` says so AND it is not hidden.
- EVERY HUD panel gets a minimize: the docks keep their header button (now
  meaning HIDE, not collapse); the headerless ones (party bar, status, options,
  hands, cards) get the same box-minus button in their top-right corner.
- THE TRAY is a new floating panel, `tray` in kHudPanelFields (before `sheet`,
  which must stay last). Default spot just above Movement's default; Ctrl-drag,
  snapping, scale + opacity, Reset and Lock like every other panel. It shows only
  while something is hidden, one ICON button per hidden panel in a row, tooltip =
  the panel's name. Clicking restores the panel and the button leaves.
- Icons: seven new glyphs in tools/BuildToolIcons.py (party, status, options,
  movement, hands, magic, cards), house-style discs.
- Only panels of the CURRENT layout appear (Minimal has cards, no party bar or
  hands).
- Allocation: hiding/restoring is a click in an armed frame - it saves settings
  (already excused) and must not rebuild the HUD.

Checks: AllocTest -Panels extended (hide Movement, restore it from the tray);
uioverlap hud with the tray up; InGameTest sweeps.

## Phase 9 - The party leader (answers 8-10, notes 12)

- STATE: `leader` (roster index) is party state in DungeonWorld beside the look
  offset, through CaptureState/ApplyState, and a `leader <i>` save line beside
  `held`. Absent = 0, so no save-version bump (unknown lines are ignored and the
  default is right). New game = slot 0 (Brand).
- PASSING: when the leader is not standing (`!IsAlive()`: down or dead), the
  next standing member in roster order takes over, checked where a member falls
  and on load. It does not return when they get up (answer 9).
- PICKING: a clickable NAME in CharacterPanel's name row (a small widget sized to
  the measured name; it shares the row with the right-aligned effect icons).
  Clicking makes that member leader; a downed member cannot be picked. The
  Minimal cards carry the same panel, so the card name works for free.
- SHOWN: the leader's name in the accent colour with a soft underglow; the
  others in plain text.
- WHAT THE LEADER DOES: throwing (Phase 10), picking up, doors and levers. Today
  none of those involve a member at all - a pick goes to the cursor and a door
  checks the whole party's keys. So for pick-up / doors / levers this phase makes
  the LEADER the actor (log lines name them, and nobody acts while the whole
  party is down) and leaves the hook for later skill or strength checks. Keys
  still count from anyone's pack. Portrait quick-stow is unchanged.
- Dev: `leader [member]`. Save round-trip checked by the eval harness (set,
  save, load, read back).

## Phase 10 - Throwing (answer 7)

- THROW OR DROP: with an item held, a click whose ray hits the FLOOR of a square
  in reach (the party's or the one ahead) DROPS there, as today; any other click
  - above the floor's horizon, on a wall, beyond reach - THROWS. That is
  Grimrock's screen-height rule, derived from the view rather than a magic
  fraction, and it keeps AllocTest -Items' click (0.835 H, the square ahead) a
  drop. Today a miss falls at your feet; it becomes a throw.
- THE FLIGHT: ProjectileSpec gains `const ItemKind* item` (a pointer into the
  stable item kinds - no string, nothing allocated). It launches down the
  LEADER's quadrant lane (the CastSpell origin offset) along the facing; the
  item's own model draws tumbling in flight (the floor-item draw), not a
  billboard.
- THE HIT: `fx::DamageEvent::Bolt` through `fx::Deal`, like a bolt. A weapon
  throws with its damage and damage type; anything else with a weight-based bash.
  Accuracy is DEX plus the throwing skill (the house rule). New balance.cat knobs
  (`throw_base`, `throw_weight`, `throw_speed`, `throw_range`, `throw_interval`,
  and the stamina cost of a throw like a swing's) so it tunes in the Balance
  dialog.
- THE LANDING: the item is never lost. On a hit it falls in the monster's cell;
  on a wall it falls in the last open cell (the projectile has already stepped
  into the wall cell, so back off one); at the end of its range, where it is.
  Through the existing `DropItemInCell` / `PlaceDrop` path. Projectiles are
  transient and CLEARED on save, level change, arena reset and inspector Remove -
  a thrown item in flight is LANDED first at each of those, or it would vanish.
- THE SKILL: `throwing`, seeded in SeedPartySkills (the steady-state rule), XP
  to the leader on a landed hit like a melee blow, stats creep by the house rule;
  `skill.throwing` + `.hint` lang keys x5; docs/skills.md. The sheet's Skills
  tab lists it on its own.
- Dev: `throw [member]` (throw the held item, or a given item, straight ahead)
  and a `throws=` tally. Checks: a new AllocTest -Throw (pick, throw, it lands,
  pick it up again), the eval harness for hit/miss/landing/XP, RollTest for any
  pure formula, PipelineTest (the new damage source must go through fx::Deal).

## Answers to the plan's questions (2026-10-01)

1. Font weight: try the installed Regulars first; download a heavier OFL weight
   only if none reads well (record it in docs/costs.md).
2. Reset layout RESTORES minimized panels too - the tray empties.
3. The corner minimize on headerless panels shows ONLY WHILE CTRL IS HELD, with
   the other arrange controls.
4. Throwables: ANY held item can be thrown, AND a throwable rock item is added
   (script-built model, items.cat entry, lang keys x5 incl. .desc).
5. Leader on pick-up / doors / levers: the NAMED ACTOR plus a hook for later
   checks - no new check this branch.

## Questions this plan raised

1. Phase 1: OK to download a Bold/SemiBold weight (free OFL) if the Regulars
   are not clear enough?
2. Phase 8: should Reset layout also RESTORE minimized panels, or only move
   panels home?
3. Phase 8: the corner minimize on the headerless panels - always visible, or
   only while Ctrl is held (with the other arrange controls)?
4. Phase 10: there is no rock or stone item. Throw what exists (daggers, runes,
   keys...), or add a throwable rock item (it needs a small model)? And should
   KEYS and quest items be throwable at all?
5. Phase 9: pick-up, doors and levers involve no skill today, so "the leader
   does them" means the leader is the named actor and the hook for later checks.
   Is that what you meant, or did you want something concrete now (e.g. strength
   to force a stuck door)?
