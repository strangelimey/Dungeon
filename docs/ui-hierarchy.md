# UI control hierarchy

The plan for turning the UI from a flat list of window-relative widgets into a
strict parent/child tree, where every control's `[0..1]` bounds resolve against
its parent's pixel rect, recursively from the window down.

## Where we start from

The primitive is already right. `ui::Widget` ([src/UI/Widget.h](../src/UI/Widget.h))
carries normalized `bounds` and a `Layout(container)` that resolves them against a
parent pixel rect. What is missing is *children*: nothing owns a subtree, so the
only widget that ever passes a non-window container is `TabControl`, and every
other parent/child relationship is computed by hand at authoring time.

That hand computation lives in two places.

**Authoring math in `GameUI::BuildHud`.** Every HUD widget is authored as a
fraction of the WINDOW, with the parent chain multiplied out by hand:

```
kPanelX   = 1 - kPanelW - 0.01
innerX    = kPanelX + kPad
moveTop   = kBelowBar0 + 0.013
handsTop  = moveTop + 2 * (moveW + moveGap) + 0.016
magicTop  = handsTop + setH * handRows
```

Move the control panel and all of it must be re-derived.

**Rect math inside monolithic widgets.** `CharacterPanel::PortraitRect` /
`BarsRect` / `EffectIconRect`, `SpellbookPanel`'s five `*Rect` helpers, and the
~40 sheet-relative constants in `CharacterSheetLayout.h`. `BarsRect` carries the
comment "kept in sync with Draw's bar layout" — a hit test duplicating a draw
layout is the symptom.

The clearest evidence of the missing parent is `GameUI::ApplyPartyBarScale`: it
keeps a side vector `m_belowBarWidgets` of every widget's scale-1 Y and patches
`bounds.y` on each one when the bar grows. With a real parent that is one
container's `bounds` changing.

`TabControl` already demonstrates the whole target pattern — per-tab child list,
`child.Layout(content)` against the page rect minus strip minus scroll, reverse
order for input, forwarded `DrawOverlay`. The work is to generalize it.

Scale: 21 `Widget` subclasses, 15 `UIContext` instances.

## The target tree

```
window (UIContext root)
  Party Bar
    Character slot x4
      Portrait
      Effects area
        Effect icon x n          (repeater)
      Stats area
        Health / Stamina / Mana bar
  Control Bar
    Movement pad
      Direction button x6
    Hands area
      Hand pair x4
        Hand slot x2
          Hand contents
    Magic area
      Character selection
      Symbol selection
      Spell details
  Character Sheet
    Portrait
    Stats
    Tab selector
    Contents (backpack, spells, effects, skills, ...)
```

## Mechanism (decided)

**Ownership.** `Widget` gains `std::vector<std::unique_ptr<Widget>> m_children`
and `Add<T>(args...)` with the same contract as `UIContext::Add`. `UIContext`
grows a hidden root widget sized to the window and forwards `Add` to it, so
today's flat lists become "children of root" and keep working untouched.

**`ContentRect()`.** `virtual gfx::Rect ContentRect() const { return Pixel(); }`
— the rect children resolve against. Padding, `TabControl`'s page inset, and a
scroll offset each become one override rather than bespoke math.

**Layout.** `Layout(container, ctx)` becomes non-virtual: resolve self, then
recurse into children over `ContentRect()`. It takes the `UIContext` so a
container can measure text while laying out (`TabControl::LayoutStrip` needs the
font today and cannot reach it from `Layout`).

**Split virtuals.** `Update` / `Draw` / `DrawOverlay` become non-virtual on
`Widget` and own the recursion; subclasses override `UpdateSelf` / `DrawSelf` /
`DrawOverlaySelf` and only ever handle themselves. A container cannot forget to
visit its children, and the order is fixed in one place: draw parent-then-children
in add order, update children in reverse before the parent's own hit test, so the
child that owns the pixel always claims the mouse first. Cost is a mechanical
rename across the 21 subclasses.

**Repeater.** Variable-count content (effect icons, rune grid, list rows) is a
`Repeater` container: a factory that makes one child, a count read each frame, and
an indexer that assigns child bounds. The pool GROWS to the high-water mark and
never shrinks — surplus children are simply hidden — so no child is ever destroyed
mid-frame and nothing dangles. Each repeated child owns its own hover and click.
Children hold an INDEX and re-resolve against the model every frame (the existing
`RosterMember` pattern), never a cached pointer into a model container.

## Phases

**P0 — mechanism, UI lib only.** Everything under "Mechanism" above. No
behaviour change; existing flat authoring keeps working, so nothing has to
convert at once.

**P1 — tree inspector.** A dev-console `uitree`: outline every widget's pixel
rect with its class name, highlight the chain under the cursor, dump the tree to
the log. This is how each phase below gets verified without pixel-hunting
screenshots.

**P2 — `TabControl` onto the shared mechanism.** Each tab becomes a page
container child; scroll/clip/cull move into a reusable `ScrollArea` that the
sheet's list tabs and `SlotList` can share instead of each re-implementing thumb
math.

**P3 — Party Bar.** A `PartyBar` container whose children are the four
`CharacterPanel`s at `{i*step, 0, w, 1}`; each panel gets Portrait / EffectsArea
(a repeater) / StatsArea children, and StatsArea gets three resource bars.
`ApplyPartyBarScale` collapses to setting one `bounds`; `m_belowBarWidgets` and
its saved-Y bookkeeping go away. Child-first input claim deletes the
`m_hotEffect` vs `BarsRect` ordering in `CharacterPanel::Update`.

**P4 — Control Bar.** `ControlBar` → MovementPad (6 buttons as fractions of the
pad) / HandsArea → HandPair → HandSlot → contents / MagicArea → the spellbook's
selector, symbol grid and details as children. The `innerX` / `moveTop` /
`handsTop` / `magicTop` arithmetic in `BuildHud` goes away.

**P5 — Character Sheet.** Header (portrait, name), tab selector, body.
`CharacterSheetLayout.h`'s constants get pushed down into whichever sub-area owns
them, so doll cells are fractions of the doll area rather than of the sheet.

**P6 — the remainder.** Left status/options column, message log, inventory
overlay, then the dialogs.

## Rules the tree carries

- A child's bounds are fractions of its parent's `ContentRect()`, never of the
  window.
- Screen-anchored things stay absolute and overlay-drawn, and say so in their
  header: `ContextMenu`'s absolute pixel position, and the `DropDown` /
  `ColorPicker` popups that clamp themselves to the window.
- The existing "cached widget pointers die on `Clear()`" rule extends to any
  subtree rebuild. Repeater children are never cached by anyone.
- A widget draws only itself. Anything that draws its children's content is a
  container that hasn't been split yet.

## Out of scope

`MapView` / `MapEditor` and the dev console are not widgets at all — they draw
straight to `SpriteBatch` with hand-rolled hit tests (`HoverBtn` identity across
the window-px / device-px split). Converting them is a separate thread.
