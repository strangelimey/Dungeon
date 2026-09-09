// ============================================================================
// Game/WorldMapView.h — the overworld, drawn (docs/world-map.md).
//
// NOT an overlay. MapView is a view OF where the party is; this is WHERE THE
// PARTY IS — an app state with no level loaded and no scene behind it, so the
// renderer skips the 3D passes entirely while it is up (the same treatment the
// full-screen editor map already gets).
//
// It is a separate class rather than a third MapView mode on purpose: MapView
// is built around a DungeonMap and its docks, palette, brushes and level
// browsing, none of which mean anything here. What the two share is the LOOK —
// the stylized ink of MapColors.h and the same fit-to-view pan/zoom feel — and
// that is a convention worth copying rather than a base class worth extracting
// from two objects that have almost no state in common.
//
// It DRAWS and it PICKS, and it does not travel: a keypress becomes a direction,
// and Game turns that into a journey (Game_World.cpp). Keeping the rules out of
// the renderer is what lets the harness travel with no window open.
//
// FOG: undiscovered ground is drawn as unknown rather than omitted, so the shape
// of what you have not seen is still legible — an unexplored map should look
// like a map with fog on it, not like a smaller map.
// ============================================================================
#pragma once

#include "Game/GameSettings.h"
#include "Game/WorldMap.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h" // ui::Theme

#include <string>

namespace dungeon::game {

class WorldMapView {
public:
	WorldMapView(ui::FontLibrary& fonts) : m_fonts(fonts) {}

	// Re-bakes at the window's font height, like every other screen.
	void SetFontHeight(float pixelHeight) {
		m_font = &m_fonts.Get(ui::FontRole::Body, pixelHeight);
	}

	// Resets the view to fit the whole world, so opening it is predictable
	// rather than wherever it was last panned.
	void Reset() {
		m_zoom = 1.0f;
		m_pan = {0.0f, 0.0f};
	}

	// Mouse only: pan by dragging, zoom on the wheel (cursor-anchored, as the
	// dungeon map does). Keyboard is deliberately untouched — travel keys are
	// Game's, so the view can never swallow a step.
	void Update(const Input& input, const WorldMap& world, const gfx::Rect& panel);

	// `hoverX/Z` is the cell under the cursor, or -1: the caption reads it out,
	// which is how you learn what a square costs before walking into it.
	int HoverX() const { return m_hoverX; }
	int HoverZ() const { return m_hoverZ; }

	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme,
				const WorldMap& world, const WorldState& state,
				const gfx::Rect& panel);

private:
	struct Transform {
		float cell = 1.0f;
		float ox = 0.0f, oy = 0.0f;
	};
	Transform ComputeTransform(const WorldMap& world, const gfx::Rect& panel) const;
	// The map area: the panel minus the caption band along the bottom.
	gfx::Rect GridArea(const gfx::Rect& panel) const;
	bool CellAt(float px, float py, const WorldMap& world, const gfx::Rect& panel,
				int& outX, int& outZ) const;

	ui::FontLibrary& m_fonts;
	const ui::Font* m_font = nullptr;

	float m_zoom = 1.0f;
	Vec2 m_pan{0.0f, 0.0f};
	bool m_dragging = false;
	Vec2 m_dragFrom{0.0f, 0.0f};
	int m_hoverX = -1, m_hoverZ = -1;
};

} // namespace dungeon::game
