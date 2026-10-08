// ============================================================================
// Game/ScrollPoke.h - a check of the ONE scrollbar (code-review C127,
// UI/ScrollBar.h), on a scratch tree in a context of its own, set in the HUD's
// font and never drawn: what it tests is the bar's input, not the picture.
//
// `scrollpoke` (tools\HealthTest.ps1's `uiscroll` case). A DropDown of 120
// rows - longer than any window, so its open list scrolls - and a ScrollArea
// five views tall, each driven a frame at a time with synthetic input:
//   the list   opened by a press on its face, at its top; one wheel notch down
//              moves it exactly one row, and the row under the pointer lights;
//              a notch with the pointer off the list moves nothing and keeps it
//              open; a press on the thumb starts a drag that picks no row and
//              closes nothing; dragged past the track's foot it reaches the
//              end; carried off the bar onto the rows and half the track's
//              travel, it sits half way and NO row lights under the pointer;
//              released, the drag ends and nothing is picked; the wheel far up
//              stops at the top; and once dragged to the end again, a press on
//              the last row picks the LAST item and closes the list.
//   the page   one notch moves it one step; the thumb dragged past the foot
//              reaches the end; released, the drag ends.
// The two used to carry a scrollbar each, line for line; they share one now.
// A bar that stopped following the drag or stepping the wheel fails here on
// either; one the list stopped yielding the pointer to fails on the lit row of
// the drag carried over the rows. That is the only step that can see the hold:
// the rows stop short of the gutter, so a press on the thumb itself reaches no
// row and lands inside the list whether the bar holds the pointer or not.
// ============================================================================
#pragma once

#include <string>

namespace dungeon::ui {
class FontLibrary;
}

namespace dungeon::game {

// Builds the tree, drives it and returns one line: "scrollpoke: PASS - ..." or
// "scrollpoke: FAIL - ..." naming every step that did not hold.
std::string RunScrollCheck(ui::FontLibrary& fonts, float designHeight, float width,
						   float height);

} // namespace dungeon::game
