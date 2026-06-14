// ============================================================================
// Game/MapView.h — the in-game map overlay, and the seed of the future
// dungeon editor.
//
// A stylized top-down view of the level, drawn into a large panel (~80% of the
// screen) OVER the running game. Like the dev console it is a toggle, not an
// app state: while it is open the world keeps simulating and the party still
// walks (the overlay only claims the mouse, for panning/zooming/editing).
//
// Walls and floors render as filled cells, fixtures and entities as glyph
// icons, and the party as a facing triangle. Only fog-of-war-revealed cells
// draw (DungeonWorld::IsSeen) — the seen set is dynamic, save-side state, never
// baked into the .map file.
//
// The same view is the substrate for editing: a paint tool turns a clicked
// cell wall/floor through DungeonWorld::EditCell, picked via CellAt. The
// minimap (Tool::None) and the editor are the one renderer plus one pick math.
//
// Coordinate note: the map is +X east / +Z south with row index growing
// southward (DungeonMap.h), and screen Y grows downward too, so cell→screen is
// a direct mapping with north up — no flips.
// ============================================================================
#pragma once

#include "Game/DungeonWorld.h"
#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h" // ui::Theme

namespace dungeon::game {

class MapView {
public:
	// Editing tools. None = view mode (drag pans, wheel zooms, party still
	// walks). The paint tools write cells through DungeonWorld::EditCell.
	enum class Tool { None, PaintFloor, PaintWall };

	MapView(gfx::GraphicsDevice& device, DungeonWorld& world);

	bool IsOpen() const { return m_open; }
	// Opening resets the view to fit-the-whole-map, so the overlay is
	// predictable each time rather than wherever it was last panned.
	void Open() {
		m_open = true;
		m_zoom = 1.0f;
		m_pan = {0.0f, 0.0f};
	}
	void Close() { m_open = false; }
	void Toggle() {
		if (m_open) Close();
		else Open();
	}

	Tool CurrentTool() const { return m_tool; }
	void SetTool(Tool tool) { m_tool = tool; }

	// Re-bakes the icon font when the window height changes (the overlay text
	// scales with the screen like the rest of the UI).
	void SetFontHeight(float pixelHeight) { m_font.SetHeight(pixelHeight); }

	// Mouse-only interaction within `panel` (pan/zoom, or paint when a tool is
	// armed). `panel` is in the same pixel space as Input's mouse coords
	// (window pixels). Keyboard is deliberately untouched so movement keys keep
	// reaching the party. Returns true if the mouse was over the panel (so the
	// caller can keep it from also driving the HUD).
	bool Update(const Input& input, const gfx::Rect& panel);

	// Draws the framed panel and the grid inside the caller's SpriteBatch
	// Begin/End. `panel` is in the draw pass's pixel space (device pixels); the
	// view transform is resolution-independent (pan is a fraction of the panel,
	// zoom is unitless), so Update and Render agree even when window and device
	// sizes differ.
	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme,
				const gfx::Rect& panel);

private:
	// Resolved view transform for a given panel: pixels-per-cell and the grid's
	// top-left origin (in the panel's pixel space).
	struct Transform {
		float cell = 1.0f;
		float ox = 0.0f, oy = 0.0f;
	};
	Transform ComputeTransform(const gfx::Rect& panel) const;
	// Pixel → cell. Returns false when the point is outside the grid bounds.
	bool CellAt(float px, float py, const gfx::Rect& panel, int& outX,
				int& outZ) const;

	// The editor tool palette. Tool(i) is the tool the i-th button selects;
	// ButtonRect resolves a button's pixel rect from the panel (resolution
	// independent — sized from the panel, so Update and Render agree).
	static constexpr int kToolCount = 3;
	static Tool ToolForButton(int index);
	static const char* ToolLabelKey(Tool tool);
	gfx::Rect ButtonRect(const gfx::Rect& panel, int index) const;

	gfx::GraphicsDevice& m_device;
	DungeonWorld& m_world;
	ui::Font m_font; // glyph icons + labels (own atlas, like the dev console)

	bool m_open = false;
	Tool m_tool = Tool::None;

	// View state. m_pan is a fraction of the panel size so it is independent of
	// the pass's pixel resolution; m_zoom multiplies the fit-to-panel cell size.
	float m_zoom = 1.0f;
	Vec2 m_pan{0.0f, 0.0f};
	bool m_panning = false;
	Vec2 m_lastMouse{0.0f, 0.0f};
};

} // namespace dungeon::game
