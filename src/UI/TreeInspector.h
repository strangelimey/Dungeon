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

} // namespace inspect
} // namespace dungeon::ui
