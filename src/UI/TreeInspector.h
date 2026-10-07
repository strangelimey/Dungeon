// ============================================================================
// UI/TreeInspector.h — the debug view of the control tree (dev console
// `uitree`). See docs/ui-hierarchy.md; this is P1, and it is what the later
// conversions get verified with instead of pixel-hunting screenshots.
//
// When enabled, every UIContext::Render outlines its whole tree (one 1px box
// per widget, tinted by depth) and, for the widget under the cursor, highlights
// the full ancestor chain and lists it beside the pointer — so containment and
// extents read at a glance and a child that has escaped its parent is obvious.
// Drawing hooks into UIContext::Render itself, so every context is covered —
// the HUD, the pages, and each dialog — with no per-caller wiring, and only the
// contexts that actually rendered this frame draw an overlay.
//
// It is dev-facing: text stays English, no Loc.
// ============================================================================
#pragma once

#include "Core/Types.h"
#include "Graphics/SpriteBatch.h"

#include <functional>
#include <string>
#include <string_view>

namespace dungeon::ui {

class UIContext;
class Widget;

namespace inspect {

// Toggle for the whole facility (a static, like the dev console's own state —
// the console command flips it and every context obeys next frame).
bool Enabled();
void SetEnabled(bool on);
// How many hovered-chain breadcrumbs Draw has built in a frame the allocation
// guard ARMED (alloc::FrameArmed), since launch. The overlay is a reporter and
// excuses itself (code-review C223); this is AllocTest -UiTree's proof that the
// breadcrumb - the part that formats - was built inside its window.
u64 ArmedChainDraws();

// Draws the outlines + hovered chain for one context's tree. UIContext::Render
// calls this last, after the overlay pass, so it sits above everything.
void Draw(UIContext& ctx, gfx::SpriteBatch& batch);

// Writes the tree as indented text (one line per widget: name, then its pixel
// rect and its bounds fractions) — the `uitree dump` command's body. The rects
// are from the last Layout, so a context that has not rendered reads as zeros.
void Dump(const UIContext& ctx,
		  const std::function<void(const std::string&)>& out);

// A widget's display name: `debugName` when set, else the class name with its
// namespace stripped.
std::string Name(const Widget& widget);

// --- the overlap audit (dev console `uioverlap`) -----------------------------
// The rule it checks: a widget's area is ITS OWN — no sibling may paint into
// it. UI/Layout.h's Stack is how a layout keeps that true by construction; this
// is how the parts that don't go through a Stack get told when they break it.
//
// Armed for ONE frame, it walks every context that renders — the HUD, the
// pages, and whichever dialog happens to be open, with no per-caller wiring —
// and reports two things, using INK rects (Widget::InkRect, so a label wider
// than its row counts): SIBLINGS whose areas intersect, and any child that
// ESCAPES its parent's ContentRect. The second matters as much as the first: a
// row that runs off the end of its container lands on something with a
// different parent, which no sibling check would ever compare.
//
// And a third, which neither of those can see: a widget that kept to its area
// only by TRIMMING its text (Widget::TextOverrun - a drop-down's face cut with
// ".."). It collides with nothing, yet the layout did not give it room for
// what it shows.
//
// Widgets marked `overlapOk` are spared the first two - they are MEANT to lie
// over their siblings, or anywhere on screen - but still asked about trims: an
// open context menu is overlapOk and reports its rows (code-review C382). Empty
// INK rects are skipped outright, which is what a closed popup has. Ink, not
// layout: a label in a row squeezed to zero height still paints its line, and
// must still be compared.
//
// Arm it, and the next frame's contexts report through `out`.
void ArmOverlapAudit(std::function<void(const std::string&)> out);
// UIContext::Render calls this after laying its tree out. No-op unless armed.
void RunOverlapAudit(UIContext& ctx);
// Closes the armed window and prints the verdict. Called once per frame from
// the game's render path — no UIContext can know it was the frame's last.
void EndOverlapAuditFrame();

// HAND-DRAWN CHROME'S TRIMS (code-review C224). A view drawn outside every
// UIContext (the world and level maps' toolbars and buttons) has no widget for
// the walk above to ask, so a face it had to cut reports itself here, from its
// draw: `where` names the face, `shown` and `mark` are the prefix and the trim
// mark it PAINTED (ui::FittedFace's text and mark, as DrawFittedButtonFace
// returns them - nothing is added or recomputed here, so a face that drew no
// mark, or split a character, is quoted exactly as drawn), `rect` its face and
// `cut` how far the whole label ran past its room, in px. A finding of the
// armed window like a widget's trim. No-op unless armed, so a draw may call it
// every frame.
void NoteChromeTrim(std::string_view where, std::string_view shown, std::string_view mark,
					const gfx::Rect& rect, float cut);

} // namespace inspect
} // namespace dungeon::ui
