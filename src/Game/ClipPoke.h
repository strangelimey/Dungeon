// ============================================================================
// Game/ClipPoke.h - two checks of the UI walk's CLIP (code-review C208), each a
// small scratch tree in a context of its own, set in the HUD's font. Neither is
// ever drawn to the screen: what they test is the walk, not the picture.
//
// THE THROW (`crashpoke uiclip`; tools\HealthTest.ps1's `uiclip` case). A
// clipping scroll area whose child throws mid-walk, and a button outside the
// area. The throw leaves through the console command for the main loop's catch,
// as `crashpoke throw` does; the tree then STAYS UP, walked every frame with the
// REAL input (Game::Update), so the harness can click the button and read in
// dungeon.log whether the click landed. Before C208 the area's clip outlived the
// throw, and every later walk - in every context - was cut to it: the button
// was skipped and the click was lost. The throw CHECKS its own premise first: a
// clip in force, the button wholly outside it. Its message names both rects only
// when that holds, and the harness expects that form - a scroll area that
// stopped overflowing clips nothing, and the click would land under the old walk
// too.
//
// THE NESTING (`clippoke`; HealthTest's `uinest` case). A scroll area inside a
// scrolled tab page, with a sibling on that page added before it and one added
// after it. Both must take a click and draw under the PAGE's clip. Before C208
// the inner clip never popped, and the update walk runs in reverse add order:
// the sibling added BEFORE the inner area was skipped by the update, and the one
// added AFTER it was drawn under the inner area's clip - scissored away.
// ============================================================================
#pragma once

#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/UIContext.h"

#include <string>

namespace dungeon::ui {
class Button;
class FontLibrary;
class ScrollArea;
} // namespace dungeon::ui

namespace dungeon::game {

class ClipThrowPoke {
public:
	// `designHeight` is the context's authored font size (the HUD's, so the
	// library hands back a face it already holds).
	ClipThrowPoke(ui::FontLibrary& fonts, float designHeight);

	// Lays the tree out at the window's size; the button's rect and the area's
	// then hold, in window pixels.
	void Layout(float width, float height);
	gfx::Rect ButtonRect() const;
	gfx::Rect AreaRect() const;

	// The next walk throws, from inside the scroll area's clip. Once.
	void Arm();
	void Update(const Input& input, float width, float height);

	int Clicks() const { return m_clicks; }

private:
	ui::UIContext m_ui;
	ui::ScrollArea* m_area = nullptr;
	ui::Button* m_button = nullptr;
	bool* m_armed = nullptr; // the throwing child's flag
	int m_clicks = 0;
};

// Builds the nesting tree, clicks each sibling and draws it through `batch`
// (whose recorded draws go nowhere between frames - it is the scissor the walk
// sets that counts). Returns one line: "clippoke: PASS - ..." or
// "clippoke: FAIL - ..." naming what did not hold.
std::string RunNestedClipCheck(ui::FontLibrary& fonts, float designHeight,
							   gfx::SpriteBatch& batch, float width, float height);

} // namespace dungeon::game
