// ============================================================================
// UI/Units.h — typographic units: the CSS rem/em model.
//
// Widget.h covers LAYOUT: a child's bounds are [0..1] of its parent, resolved
// from a window-sized root down. This is the other half — the DETAIL inside a
// control. Text padding, a row's height, a scrollbar's width, a thumb's
// minimum grab size: these belong to the TEXT they sit beside, not to whatever
// rect happens to contain them. A checkbox's label gap should not stretch
// because the row it was dropped into is wide.
//
//   1rem = the context's ROOT font size (Font::Height).
//
// Every UIContext is its own document with its own root — the HUD's 17px, the
// menus' 28px, the sheet's 22px — and all of them already track the window
// height (GameUI::UpdateFonts), so anything expressed in rem scales with the UI
// for free, at every resolution, without a second scaling rule.
//
// Useful conversions: one line of text is 1.25rem (Font::LineAdvance), so a row
// holding a single line with comfortable air above and below is ~1.75rem.
//
// `em` — relative to the widget's OWN font — is now REAL: a widget can set
// Widget::fontRole and draw in a different face from the document root, and its
// em follows that face while rem does not. Because em is a property of a
// particular widget it lives ONLY on Widget (Widget::Em); there is deliberately
// no free Em(ctx), which could only have meant the root while reading as though
// it meant the widget.
//
// The division is load-bearing. Layout geometry — a row's height, a page's
// padding, the gap between controls — stays in REM, so re-facing one label
// cannot re-space the controls around it. Detail belonging to a widget's own
// text is in EM, so it tracks the face it sits beside.
//
// THE ONLY RAW PIXELS LEFT are hairlines: the 1px borders (DrawBorder,
// Separator) and the 2px text caret. A hairline expressed as a fraction blurs
// across two rows of pixels or vanishes entirely, so it stays a hairline.
// ============================================================================
#pragma once

namespace dungeon::ui {

class UIContext;

// `n` rem in pixels — n x the context's root font size.
//
// There is no free Em(): em belongs to a widget, so it is Widget::Em(n).
float Rem(const UIContext& ctx, float n = 1.0f);

} // namespace dungeon::ui
