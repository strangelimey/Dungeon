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

#include "Graphics/SpriteBatch.h"

#include <functional>
#include <string>

namespace dungeon::ui {

class UIContext;
class Widget;

namespace inspect {

// Toggle for the whole facility (a static, like the dev console's own state —
// the console command flips it and every context obeys next frame).
bool Enabled();
void SetEnabled(bool on);

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
// Widgets marked `overlapOk` are skipped; so are empty rects, which is what a
// screen-anchored popup has.
//
// Arm it, and the next frame's contexts report through `out`.
void ArmOverlapAudit(std::function<void(const std::string&)> out);
// UIContext::Render calls this after laying its tree out. No-op unless armed.
void RunOverlapAudit(UIContext& ctx);
// Closes the armed window and prints the verdict. Called once per frame from
// the game's render path — no UIContext can know it was the frame's last.
void EndOverlapAuditFrame();

} // namespace inspect
} // namespace dungeon::ui
