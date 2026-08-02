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

**P1 — tree inspector. DONE.** Dev console `uitree` (on / off / no-arg toggle)
outlines every widget in every context, tinted by depth, and highlights the
chain under the cursor with a breadcrumb naming each link and its pixel rect.
`uitree dump [hud|menu|settings|pause|saves|sheet|confirm]` prints the tree
indented, each line carrying the widget's pixel rect and its bounds fractions.
It hooks into `UIContext::Render`, so every context — the HUD, the pages, each
dialog — is covered with no per-caller wiring, and only contexts that actually
rendered draw an overlay. Names come from `typeid` with the namespace stripped,
or from `Widget::debugName` when set.

The hover pick descends into the SMALLEST child containing the point, which is
deliberately NOT the input rule (reverse add order) — see the first finding
below.

**P2 — `TabControl` onto the shared mechanism. DONE.** Scroll, clip and cull
came out into `ui::ScrollArea`, a container that scrolls its children when they
overflow it: `ContentRect()` is its view box shifted by the scroll, so the
offset applies to descendants for free; `ChildActive` culls what has scrolled
out; `ChildClip` clips the rest. A tab is now just a label plus a `ScrollArea`
child filling the page, and `TabControl` is down to the strip, the frame, and
showing the active page — its own scroll state, per-tab child lists, manual
child walk and manual `DrawOverlay` forwarding are all gone. Authoring is
unchanged (`AddTab` / `AddChild(tab, ...)`), and so is the layout: the page's
padding and gutter are the same numbers in their new home.

Clipping needed one addition to the base: `virtual const gfx::Rect* ChildClip()`.
The draw walk intersects a clip with whatever is already in force and restores
the outer one afterwards, so a scroll area inside a scrolled page cannot widen
its parent's clip — which the old single `SetScissor(nullptr)` would have done.

**P3 — Party Bar. DONE.** `PartyBar` owns the slots and splits itself into
`kSlots` columns; each `CharacterPanel` owns `PortraitBox`, an `EffectsArea`
(the first `Repeater` — one `EffectIcon` per live effect) and `StatsArea`.
`ApplyPartyBarScale` went from re-deriving every slot rect plus patching
`bounds.y` on a saved-Y side list to setting two rects: the bar's, and the y of
a `BelowBar` container everything else now hangs from. `m_belowBarWidgets` is
gone. Each part owns its own hover and click, so `CharacterPanel::Update`'s
`m_hotEffect` / `BarsRect` ordering is gone too, and the effect tooltip moved to
`DrawOverlaySelf` — it now floats over the neighbouring slot instead of being
paintable-over.

Two things this phase established for the ones after it:

- **Computed child bounds.** A panel's parts are aspect- and font-locked: the
  portrait is a square sized by the slot's HEIGHT, so its width fraction depends
  on the slot's aspect (which the scale slider changes), and the effect strip
  and bar band are sized from the font's line advance. `LayoutSelf` works those
  fractions out per frame from the live pixel rect. Still parent-relative — the
  child multiplies out against its parent exactly as before — the parent just
  derives the fractions rather than having them authored.
- **`UpdateBeforeChildren`.** The slot highlights as one piece, but its children
  cover most of it and claim the mouse first, so asking `IsMouseConsumed()`
  after they run reads "not hovered". The new hook gives a container its first
  look at the mouse, while consumption still reflects only what lies OUTSIDE its
  subtree.

`BelowBar` is a placeholder shape: it spans the whole window rather than the
area below the bar, so its children keep the window fractions they were authored
with and the offset is all it contributes. P4/P6 give it a real rect as those
widgets become containers.

**P4 — Control Bar. DONE.** `ControlBar` (Game/ControlBar.h) → `MovementPad`
(6 buttons) / `HandsArea` → `HandPair` → `HandSlot` / `MagicArea` → heading +
`SpellbookPanel`. Four levels, every bound a fraction of its own parent, and the
whole panel now moves by setting one rect. `BuildHud`'s share is a deps struct
and one `Add` — the `innerX` / `moveTop` / `handsTop` / `magicTop` chain is gone,
along with `setW` / `handW` / `setH` / `bookY` / `bookH`.

The layout constants moved into `ControlBar.cpp` in the window fractions they
were authored in, with each child dividing them through by its own parent's
span. Keeping the source numbers (and the divisions) visible is what makes the
"same pixels, new structure" claim checkable — and it held: every pixel rect in
`uitree dump hud` is unchanged from P3.

STILL SELF-DRAWN: `SpellbookPanel`'s interior — the member selector row, symbol
grid, sequence row and Cast/Clear — is one widget laying its own parts out
against its pixel rect. Splitting it into the sketch's "character selection /
symbol selection / spell details" children is real work (interlocking sequence
and disabled-symbol state) and hasn't been done.

**P5 — Character Sheet.** Header (portrait, name), tab selector, body.
`CharacterSheetLayout.h`'s constants get pushed down into whichever sub-area owns
them, so doll cells are fractions of the doll area rather than of the sheet. The
sheet's list tabs and `SlotList` each re-implement thumb maths that `ScrollArea`
(P2) now owns — fold them onto it here.

**P6 — the remainder.** Left status/options column, message log, inventory
overlay, then the dialogs. Two items the inspector has already named:
`MessageLog` must take its real bounds (below), and `ContextMenu` /
`InventoryWindow` dump as `0x0` because they position themselves in absolute
pixels — legitimate for a screen-anchored popup, but it should be stated in
their headers rather than inferred from a zero rect.

## What the inspector found

- **`MessageLog` claims the whole window.** Its bounds are `{0,0,1,1}` and it
  sizes itself down inside Draw, so on a pure reverse-add-order hit test it is
  the topmost thing under every pixel on screen: hovering a party slot reported
  the log. That is why the inspector picks the smallest match instead. The
  widget still only CONSUMES the mouse over its real area, so input is correct
  today — but a widget whose bounds overstate what it draws is a layout bug
  waiting to happen, and P6 fixes it at the source.
- **The HUD is 30-odd siblings all at depth 1.** The dump shows one flat row of
  Panels, Labels, Buttons, HandSlots and the SpellbookPanel directly under root
  — the flatness this thread exists to remove, now visible rather than inferred.

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
