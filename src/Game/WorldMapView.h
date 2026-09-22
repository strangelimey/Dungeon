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
#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h" // ui::Theme

#include <functional>
#include <string>
#include <vector>

namespace dungeon::game {

class WorldMapView {
public:
	// Two modes of one view, the shape MapView already arrived at for the
	// dungeon map (Michael, 2026-09-09: the world screen is BOTH). FOG is the
	// difference, exactly as it is below ground — Play draws undiscovered
	// ground as unknown and hides the locations the party has not found;
	// Editor draws the whole world, because you cannot place what you cannot
	// see.
	//
	// Unlike MapView there is no "the world keeps simulating behind it"
	// problem to solve: the world map is an app state that simulates nothing,
	// so this is a mode of that state rather than an overlay over a game.
	enum class Mode { Play, Editor };

	// The Editor band's tools. A deliberately short list — the world screen has
	// four verbs where the level editor has ten — but the SAME one-list idiom
	// MapView uses: geometry, hover, click dispatch and drawing all walk
	// ToolbarButtons(), so adding a tool (W5's dungeon picker) is one line and
	// cannot land in three of the four places.
	enum class Tool { None, Settings, Save, Undo, Redo };

	WorldMapView(gfx::GraphicsDevice& device, ui::FontLibrary& fonts);

	Mode CurrentMode() const { return m_mode; }
	void SetMode(Mode m) { m_mode = m; }
	bool Editing() const { return m_mode == Mode::Editor; }

	// The terrain the paint brush will lay down, by id. Empty = nothing armed,
	// and a click paints nothing — the same "nothing armed until you pick a
	// row" rule the dungeon palette has.
	void ArmTerrain(std::string id) { m_armed = std::move(id); }
	const std::string& ArmedTerrain() const { return m_armed; }

	// Fired when the editor paints a cell, so the owner can bracket it as ONE
	// undo step and mark the world dirty. The view never mutates the world
	// itself: it decides WHAT and WHERE, and the owner decides whether that is
	// allowed and what it costs.
	std::function<void(int x, int z, const std::string& terrainId)> onPaint;
	// A cell was right-clicked in Editor mode: the owner opens whatever
	// inspects that square (a location, or the cell itself).
	std::function<void(int x, int z)> onInspect;
	// A toolbar tool was clicked. ONE callback rather than four, because the
	// view has no opinion about any of them — it knows a disc was pressed and
	// which one, and the owner knows what that means.
	std::function<void(Tool)> onTool;
	// Whether undo/redo have anything to take back. Asked every frame the band
	// draws, so a dimmed arrow is the history's live state and not a flag this
	// view has to be told to update.
	std::function<bool(bool redo)> canUndo;

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
	// The map area: the panel minus the toolbar band on top (Editor mode only)
	// and the caption band along the bottom.
	gfx::Rect GridArea(const gfx::Rect& panel) const;
	// The toolbar band. Zero-height in Play mode, which is what keeps the grid
	// in the same place whether or not the band is there to push it down.
	gfx::Rect ToolbarRect(const gfx::Rect& panel) const;
	bool CellAt(float px, float py, const WorldMap& world, const gfx::Rect& panel,
				int& outX, int& outZ) const;

	struct ToolButton {
		Tool id = Tool::None;
		gfx::Rect rect{};
		std::string label; // the tooltip, and the face when the art is missing
		const gfx::Texture* icon = nullptr;
		bool enabled = true;
	};
	std::vector<ToolButton> ToolbarButtons(const gfx::Rect& panel) const;

	ui::FontLibrary& m_fonts;
	const ui::Font* m_font = nullptr;
	// Borrowed from the shared cache (AssetUtil's ToolbarIcon) — the level
	// editor's band draws from the same textures.
	const gfx::Texture *m_icoSettings = nullptr, *m_icoSave = nullptr,
					   *m_icoUndo = nullptr, *m_icoRedo = nullptr;
	Tool m_hoverTool = Tool::None; // tracked by Update in WINDOW pixels; the
								   // render re-derives its own geometry and
								   // matches by IDENTITY, never by coordinate

	Mode m_mode = Mode::Play;
	std::string m_armed; // terrain id the brush lays down; empty = none
	bool m_painting = false; // a drag in progress: one undo step, many cells
	bool m_rightDown = false; // right press seen; a stationary release inspects
	Vec2 m_rightFrom{0.0f, 0.0f};
	float m_zoom = 1.0f;
	Vec2 m_pan{0.0f, 0.0f};
	bool m_dragging = false;
	Vec2 m_dragFrom{0.0f, 0.0f};
	int m_hoverX = -1, m_hoverZ = -1;
};

} // namespace dungeon::game
