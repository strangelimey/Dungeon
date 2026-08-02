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
// `em` — relative to the widget's OWN font — collapses onto rem while every
// widget draws with its context's font. Em() exists so the seam has a name: if
// per-widget font sizes ever arrive, Em is the one that changes and every call
// site already says which it meant.
//
// THE ONLY RAW PIXELS LEFT are hairlines: the 1px borders (DrawBorder,
// Separator) and the 2px text caret. A hairline expressed as a fraction blurs
// across two rows of pixels or vanishes entirely, so it stays a hairline.
// ============================================================================
#pragma once

namespace dungeon::ui {

class UIContext;

// `n` rem in pixels — n x the context's root font size.
float Rem(const UIContext& ctx, float n = 1.0f);

// `n` em in pixels. Identical to Rem today; see the header note.
float Em(const UIContext& ctx, float n = 1.0f);

} // namespace dungeon::ui
