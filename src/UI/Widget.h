// ============================================================================
// UI/Widget.h — base class for all controls. See UIContext.h for the
// update/draw/overlay flow and input-consumption rules.
// ============================================================================
#pragma once

#include "Graphics/SpriteBatch.h"

namespace dungeon::ui {

class UIContext;

// Base class for all controls. Coordinates are absolute pixels.
class Widget {
public:
	virtual ~Widget() = default;

	virtual void Update(UIContext& ctx) = 0;
	virtual void Draw(UIContext& ctx, gfx::SpriteBatch& batch) = 0;
	// Second draw pass for popups (e.g. an open drop-down list) so they
	// render above every normally drawn widget.
	virtual void DrawOverlay(UIContext&, gfx::SpriteBatch&) {}

	gfx::Rect bounds;
	bool visible = true;
};

} // namespace dungeon::ui
