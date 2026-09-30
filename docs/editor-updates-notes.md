# editor-updates - brain dump (raw, verbatim in substance)

- Just tried editing the crypt levels and it was awkward and clunky.
- Hard to set all the walls in a corridor to the same (texture).
- Hard to set the ceiling and floor tiles to something consistent, too.
- Need a way to say "set all the floor on this level / in this area". Same for wall and ceiling.
- Knows there are keys for fill, rect and dropper but can't remember what they are.
  -> Add an editor toolbar with those on it.
  -> Also put the new buttons for setting ceiling/wall/floor to a set, etc. on it.
- Need a way to create a NEW WORLD. Add a button. Clicking it opens a dialog where the user can:
  - start blank,
  - copy the current world / level / *.cat files, or
  - set them up with a "new world" wizard.
- A level should be AUTO-VALIDATED ON EDIT. Examples: stairs that don't connect, locked doors with no key/button/etc.
  - Anything that fails validation is highlighted with a RED BOX on the map.
  - Hovering over the box shows a TOOLTIP explaining why it fails.
- Pick a COMBINATION of floor/wall/ceiling, give it a NAME (e.g. "marble hall"), and save it in the palette.

(end of dump 1 - "that's it for now. let's organize")

---

# Organized

## A. Painting surfaces consistently (lines 1-3)
- Making a corridor's walls, floors and ceilings match is awkward.
- Wants "set all floor/wall/ceiling" scoped to the LEVEL or to an AREA.
- Exists: Shift+click rect, Ctrl+click flood, Alt+click eyedropper.
- Likely root of the pain: flood matches the same RESOLVED variant, so on a
  default hash-varied corridor it stops at every cell that rolled differently.
  It recolours a patch; it can't unify a mixed run.

## B. Named surface combinations (line 7)
- Floor + wall + ceiling saved together under a name ("marble hall") in the palette.
- Pairs with A: "set this area to marble hall" is one action, not three.

## C. Discoverable editor tools (line 4)
- A toolbar with rect / fill / dropper, so nobody has to remember the modifier keys.
- The A/B "set to ..." actions go on it too.
- Exists: a top toolbar band (level dropdown, +level, Level, Balance, Check,
  undo/redo, pause, save, to source).

## D. Live validation on the map (line 6)
- Revalidate after every edit.
- Red box on each failing thing; hover tooltip says why.
- Examples: stairs that don't connect, a locked door with no key or button.
- Exists: Validate.h (project-wide lock/key reachability, stair pairing,
  world checks) reports each Issue with level + x/z; ValidateDialog lists them
  on demand from the Check button. So the new work is running it on edit and
  drawing it on the map, not the checks themselves.

## E. Create a new world (line 5)
- A button opening a dialog with three choices: blank / copy the current
  world, levels and .cat files / a "new world" wizard.
- Exists: WorldsDialog (globe disc on the WORLD editor's toolbar) creates a
  world that copies CATALOGS but not places (W7, his choice then), plus one
  starter room and an eval_level so it loads and passes the checker.

## Open questions / gaps (not answered in the dump)
1. A: what is an "area"? A dragged rectangle, the connected room/corridor
   (by walkability, ignoring texture), or something named and persistent?
   ANSWER: the connected room or corridor.
   FOLLOW-UP: rooms and corridors connect, so what bounds it?
   ANSWER: stop at doorways and narrow openings (a room and the corridor
   leaving it are separate areas).
   Note for the plan: every cell of a 1-wide corridor is itself "narrow", so
   the rule is really narrow-vs-open. The fill spreads through cells of the
   SAME kind as the clicked one and stops where the kind changes, so a
   corridor fills as one run and a room stops at its doorways.
2. A/B: "set all floor" - does it pin every cell to one texture, or can a
   target be a MIX (keeping the hash variation, drawn from a chosen few)?
   ANSWER: a mix that keeps the random variation.
   Design fork for the plan: (a) at paint time, roll each cell's pick from
   the mix and write ordinary per-cell variant records - no format change,
   but the "mix" is forgotten once painted; or (b) store the mix itself and
   let cells resolve through it, like today's default hash - editing the
   mix later re-rolls every cell that uses it.
   ANSWER: (b) store the mix.
3. B: where does a combination live - project-wide (a catalog, reusable on
   any level) or on one level? Today's per-level `palette` is append-only and
   index-based, so a combo naming a type the level lacks would have to add it.
   ANSWER: world-wide (project-wide), usable on any level.
4. C: new modes replacing the modifiers (click a Fill tool, then click), or
   buttons that just arm the modifier? Is it the same top band or a separate
   tool strip beside the palette?
   ANSWER: tools you pick (modal, modifiers stay as shortcuts), in a
   SEPARATE strip by the palette.
5. D: where does a finding with no cell go (x < 0: a level- or world-wide
   issue)? Does a cross-level fault (a stair whose pair is missing) box on
   both ends? Errors only, or warnings too (in another colour)?
   ANSWERS: cell-less findings -> a count badge on the toolbar's Check
   button that opens the existing list. Broken stair pair -> box on BOTH
   ends. Warnings too, in amber (errors red).
6. D: the checker walks the WHOLE project. Is that cheap enough per paint
   stroke, or should it run on stroke end / debounced?
   ANSWER: when the stroke ends.
7. E: "copy" now means places as well (levels, dungeons), which reverses the
   W7 content-not-places rule. Does that still apply to the new world?
   "Current world/level" - is copying one level alone an option?
8. E: what does the wizard ask? (Name, theme/tags, starting dungeon via the
   generator, size, difficulty...?) Nothing in the dump yet.
9. E: "blank" - truly empty catalogs, or engine defaults? A world still needs
   one loadable room (the W7 lesson).
10. E: should the button sit on the level editor toolbar too, or only on the
   world editor, where the globe is now?

   ANSWERS (E): 7 - yes, copy brings places too (levels, dungeons,
   overworld), AND copying a single level into a new world is an option.
   8 - wizard asks: name, theme/tags, starting dungeon via the generator,
   size, difficulty. 9 - blank = engine DEFAULT catalogs + the one starter
   room. 10 - button on BOTH toolbars (level editor and world editor).
   GAP for the plan: there is no "engine defaults" catalog set today - the
   W7 create copies the running world's catalogs. Defaults need a home
   (a shipped template project, or dungeon-demo's catalogs as the template).
