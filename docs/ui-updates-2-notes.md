# UI updates (second thread) - notes

Branch `ui-updates` (2026-10-01), worktree C:\Dev\Dungeon-ui-updates. The first
thread of this name is docs/ui-updates-notes.md / -plan.md (merged b46f2c0).

## Raw notes (Michael's brain dump, 2026-10-01)

1. Resource bar FRAMES: "The iron borders for the bars is too dark. Do we have a
   lighter one?"
   - Answered while dumping: the kit has no lighter IRON frame - all 21 Life bars
     are the same near-black iron as the one in use (#1). The kit's other frame
     styles are SILVER (the 20 Mana bars - pale steel/pewter) and GOLD (the XP
     bars, very ornate). Silver ones closest to #1's rounded tube + scrollwork:
     #13, #16, #7, #6. Contact sheets were sent (scratchpad sheet_mana.png /
     sheet_life.png / sheet_xp.png, made by bar_sheet.py).
   - Two routes noted: re-cut from a silver bar (CutBarFrame.py finds the tube by
     its RED fluid, so it needs adapting for blue), or lift #1's iron at cut time
     (cheaper, but risks grey/flat rather than lighter metal).
   - No choice made yet.

2. DROP-DOWN control has NO SKIN. "We need to make it look similar to the
   selected tab in the settings dialog. A darker, in-set look instead of the
   yellow border and brown interior." "We'll need to make sure the text stands
   out on the background."

3. CHECKBOX: "Same goes for the checkbox control." (no skin - give it the same
   darker, inset look as the selected settings tab, readable text)

4. TEXT FIELDS: "And the text fields too." (same treatment)

5. SLIDERS: "And the sliders." (same treatment)

6. MINIMIZED PANELS -> a "CLOSED WINDOWS" BUTTON SET. "When a panel is minimized
   (movement, magic, etc.), instead of collapsing into a rectangle, it should be
   added to a new 'closed windows' set of buttons. This set lives just above
   where the current movement panel sits. So, when the movement panel's minimize
   button is pressed, the whole panel hides and a 'movement panel' button appears
   in the set of buttons. Pressing it will bring the panel back and remove itself
   from the button list."
   - Minimize = the WHOLE panel hides (no collapsed strip left behind).
   - The button set sits just ABOVE the Movement panel's current spot.
   - One button per minimized panel; pressing it restores the panel and the
     button leaves the set.

7. TEXT ON STONE IS HARD TO READ. "I think all text that is drawn on a button or
   panel that has a stone background is in danger of being washed out and hard
   to read. Can we make a clearer font? Or an outlined one?"
   - Scope as he put it: ALL text over a stone-skinned button or panel.
   - Two ideas offered: a clearer font, or an outlined one. Open question.
   - Same concern as #2's "make sure the text stands out on the background".

8. MAGIC PANEL MEMBER BUTTONS - colours hard to see. "In the magic panel, the
   player selection buttons have the color of the character they represent. Some
   of these are hard to see now, too. For instance, Tilo's purple is very hard to
   see and Maren's yellow, too."
   - The spellbook selector row's member-coloured buttons.
   - Named: Tilo (purple) very hard to see; Maren (yellow) too. "now" = since the
     stone chrome landed.

9. HAND CONTROLS - drop the colour bars, use a BORDER. "I don't like the
   character color bars inside each hand-control. We need to indicate character
   by surrounding the two hands and the effort meter in a border of that
   character's color instead."
   - Remove the identity stripe drawn inside each hand box.
   - Instead: ONE border in the member's colour around the whole group - both
     hands AND the effort (over-exertion) meter.

10. EFFORT BAR - liven it up. "The effort bar is flat and 2D. Can we liven it up
    like the health/stamina/mana meters? Don't want it too busy, so keep the
    animation low key."
    - Model: the resource bars (framed, procedural, animated, emissive).
    - Constraint: LOW-KEY animation, not busy (echoes the resource bars' own
      "too busy - it draws the eye" round).

11. MAGIC WINDOW RUNES - glow + pulse. "The runes in the magic window are flat
    and 2D. I want the rune symbol to glow and pulse in the school's color. For
    non-schooled, just white for now."
    - The rune SYMBOL itself glows and pulses.
    - Colour = the rune's school colour; a rune with no school = white ("for now").

12. PARTY LEADER (new concept). "We need to introduce the concept of 'party
    leader'. The leader is the one who performs actions with the mouse. For
    instance, if a rock is held and then thrown by clicking in the view window,
    the leader's throw skills are used and they accrue any experience. A leader
    is selected by clicking the name in the party bar and is indicated by
    highlighting the name."
    - The leader performs the party's MOUSE actions.
    - Example: a held rock thrown by clicking in the 3D view -> the LEADER's
      throw skill is used and the leader earns the XP.
    - Select: click the member's NAME in the party bar.
    - Shown by: the leader's name is highlighted.
    - "Current leader is saved at the party level" - the leader is PARTY state
      and persists (rides the save).

## Organized (2026-10-01)

A. SKIN THE REMAINING CONTROLS (#2, #3, #4, #5)
   Drop-down, checkbox, text field, slider: today flat (yellow border, brown
   interior). Give them the SELECTED SETTINGS TAB's look - darker, inset - with
   text that stands out on it. One look, four controls.

B. LEGIBILITY ON STONE (#7, #8, and #2's text clause)
   - All text on a stone button/panel washes out. Ideas: a clearer font, or an
     outlined one.
   - Member colours wash out too: Tilo's purple and Maren's yellow on the Magic
     panel's member buttons.
   - Same root: stone is mid-toned and busy, so anything of similar value sinks.
     #9's member-colour borders will meet the same problem.

C. MEMBER IDENTITY ON THE HAND CONTROLS (#9)
   Remove the colour stripe inside each hand box; one member-colour BORDER
   around that member's two hands + effort meter.

D. LIVELIER HUD ELEMENTS (#10, #11) - the resource bars' language
   - Effort bar: framed/animated/emissive like the resource bars, LOW-KEY motion.
   - Magic window runes: the symbol glows and pulses in its school's colour;
     unschooled runes white (for now).

E. RESOURCE BAR FRAME (#1)
   Lighter than the iron. Options on the table: re-cut from a silver (Mana) bar
   (#13 / #16 / #7 / #6 closest to the current shape), or lift the iron at cut
   time. Undecided.

F. CLOSED-WINDOWS TRAY (#6)
   Minimize hides the WHOLE panel and puts a button for it in a tray just above
   the Movement panel; pressing the button restores the panel and removes the
   button. Today only Movement and Magic have a minimize (hudMoveCollapsed /
   hudMagicCollapsed, persisted), and they collapse to a strip.

G. PARTY LEADER (#12) - new game concept, not UI chrome
   The leader performs the party's mouse actions (their skills, their XP);
   chosen by clicking a name in the party bar; the name is highlighted; saved
   as party state.

## Gaps and questions found while organizing

1. (E) Which frame: a silver bar (which number), or lighten the iron?
2. (B) Outline, a different face, or both? And does "stone" mean the HUD only, or
   also the menus / settings / sheet (all stone-skinned)?
3. (B, C) Member colours: change the PALETTE itself (brighter, more saturated),
   or keep the colours and draw them so they read (a dark keyline, a glow)? The
   border in (C) needs the same answer.
4. (F) Which panels minimize into the tray - only Movement and Magic (which have
   the button today), or every floating HUD panel (Status, Options, Hands, Cards,
   the party bar)? The inventory and sheet are windows with a close box - out?
5. (F) "Just above where the Movement panel sits": does the tray FOLLOW Movement
   when it is moved, sit at Movement's DEFAULT spot, or is the tray its own
   floating panel (Ctrl-drag like the rest)? And where does it go when Movement
   itself is minimized?
6. (F) A tray button's face: the panel's name as text, or an icon?
7. (G) THROWING DOES NOT EXIST. A left-click in the view with an item held DROPS
   it on the floor (DropItemAt); there is no throw and no throwing skill. Is a
   throw (and its skill) part of this branch, or is the leader built now with
   the throw following later? If a throw: how does a click choose throw vs drop?
8. (G) Which other mouse actions belong to the leader? Picking an item up (does
   it go to the leader?), opening doors, pulling levers, the portrait quick-stow.
9. (G) Default leader (slot 0, Brand?) and what happens when the leader is
   downed or dies - does leadership pass automatically?
10. (G) The Minimal layout has no party bar. Click the name on the member CARD
    there (the cards carry the same CharacterPanel)?

## Answers (2026-10-01, asked one at a time)

1. Bar frame: re-cut from SILVER Mana bar #13.
2. Stone text: BOTH - a clearer face AND an outline.
   Scope: EVERYTHING on stone (HUD, menus, settings, pause, sheet).
3. Member colours: pick BRIGHTER colours AND add a GLOW (he picked "palette" +
   "glow", not the keyline). Applies to the Magic member buttons and the new
   hand-control border.
4. Tray panels: EVERY HUD panel (party bar, Status, Options, Movement, Hands,
   Magic, Cards) gets a minimize and goes to the tray. Inventory and sheet keep
   their close box.
5. Tray spot: its OWN FLOATING PANEL - defaults just above Movement's default
   spot, Ctrl-drag to move, saved; independent of Movement.
6. Tray button: an ICON (house-style disc per panel, name in a tooltip).
7. Throwing: ADD IT IN THIS BRANCH (a throw + a throwing skill).
   Throw vs drop: BY SCREEN HEIGHT (Grimrock) - click the floor low in the view
   = drop there; click higher = throw.
8. Leader's actions: throwing, PICK-UP, DOORS AND LEVERS. The portrait
   quick-stow stays as now - the portrait dropped on decides who gets the item
   (confirmed in a follow-up).
9. Leader default/loss: slot 0 (Brand) leads a new game; leadership PASSES to
   the next standing member automatically when the leader is downed or dies.
10. Minimal layout: click the NAME ON THE CARD; it highlights the same way.
    - (Note for organize: the name sits on the party bar, which the Minimal
      layout does not have - the cards carry the same CharacterPanel.)
