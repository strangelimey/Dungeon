// ============================================================================
// UI/Widget.h — base class for all controls. See UIContext.h for the
// update/draw/overlay flow and input-consumption rules.
// ============================================================================
#pragma once

#include "Graphics/SpriteBatch.h"

namespace dungeon::ui {

class UIContext;

// Base class for all controls. `bounds` is normalized: x/y/w/h are fractions
// (0..1) of the containing widget or window, so the UI scales with the screen.
// The container resolves bounds to pixels via Layout() each frame before
// Update/Draw; controls hit-test and draw from Pixel().
class Widget {
public:
	virtual ~Widget() = default;

	virtual void Update(UIContext& ctx) = 0;
	virtual void Draw(UIContext& ctx, gfx::SpriteBatch& batch) = 0;
	// Second draw pass for popups (e.g. an open drop-down list) so they
	// render above every normally drawn widget.
	virtual void DrawOverlay(UIContext&, gfx::SpriteBatch&) {}

	// Resolves the normalized bounds against the container's pixel rect.
	// UIContext calls this for top-level widgets (container = window);
	// container widgets (TabControl) call it for their children.
	void Layout(const gfx::Rect& container) {
		m_pixel = {container.x + bounds.x * container.w,
				   container.y + bounds.y * container.h, bounds.w * container.w,
				   bounds.h * container.h};
	}

	gfx::Rect bounds; // fractions of the container (0..1)
	bool visible = true;

protected:
	// The pixel rect resolved by the most recent Layout().
	const gfx::Rect& Pixel() const { return m_pixel; }

private:
	gfx::Rect m_pixel{};
};

} // namespace dungeon::ui
