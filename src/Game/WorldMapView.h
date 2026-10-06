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

#include <algorithm>
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
	//
	// THE MODE BELONGS TO THE TRAVEL SCREEN. The same view is also the player
	// map's world page inside a dungeon (SetOverlay), and that page is never
	// the editor, whatever the mode says: it keeps the fog, shows no toolbar
	// and neither paints nor inspects. Only the WorldMap state routes the
	// world dialogs and closes a paint stroke, so an editing overlay opened
	// dialogs nothing could close and painted outside the undo history
	// (code-review C77, C79). Editing() is the one test of that.
	enum class Mode { Play, Editor };

	// The Editor band's tools. A deliberately short list — the world screen has
	// four verbs where the level editor has ten — but the SAME one-list idiom
	// MapView uses: geometry, hover, click dispatch and drawing all walk
	// ToolbarButtons(), so adding a tool (W5's dungeon picker) is one line and
	// cannot land in three of the four places. `Worlds` is the one tool about
	// something ABOVE this world — the other worlds beside it (W8).
	enum class Tool { None, Worlds, NewWorld, Settings, Save, Undo, Redo };

	WorldMapView(gfx::GraphicsDevice& device, ui::FontLibrary& fonts);

	Mode CurrentMode() const { return m_mode; }
	void SetMode(Mode m) { m_mode = m; }
	// Editor mode AND the travel screen: the overlay page is always play (see
	// Mode). Everything the editor adds - the toolbar, the fog lifted, paint,
	// the right-click inspect - asks this, never the mode alone.
	bool Editing() const { return m_mode == Mode::Editor && !m_overlay; }

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

	// --- shown as the PLAYER'S map, rather than as the travel screen (W6) ---
	// The same view, drawn into the map overlay's panel while the party is
	// inside a dungeon. It adds a button back to the dungeon map and a close
	// box - which is why it is a flag and not a mode: OUTSIDE there is no
	// dungeon map to go back to, so the travel screen must not offer one - and
	// it takes the editor away (Editing() is false here in either mode).
	void SetOverlay(bool on) { m_overlay = on; }
	bool IsOverlay() const { return m_overlay; }

	// WHAT THE LAST Render DREW, recorded by Render itself rather than worked
	// out again for a report: whether it drew as the overlay page, and whether
	// it lifted the fog (drew every cell, discovered or not). The `worldview`
	// command reads it, so a page that drew the whole world reads as "fog off"
	// whatever Editing() says it should have drawn (code-review C79).
	struct Drawn {
		bool overlay = false;
		bool fogLifted = false;
	};
	const Drawn& LastDrawn() const { return m_drawn; }
	// That button. Null (or not an overlay) hides it.
	std::function<void()> onShowDungeon;
	// The overlay's close box, top-right - on MapView's close box pixels, so
	// it too stays put across the page toggle. Null (or not an overlay) hides it.
	std::function<void()> onClose;
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

	// --- for the `worldview` command ----------------------------------------
	// How many toolbar discs the band holds in `panel` - ToolbarButtons' own
	// count, so a report cannot disagree with what Render draws.
	size_t ToolCount(const gfx::Rect& panel) const { return ToolbarButtons(panel).size(); }
	// The window point at the centre of cell (x, z) in `panel`, the inverse of
	// the pick Update makes - so a scripted click lands where a mouse would.
	// False when the cell is off the world.
	bool CellPoint(const WorldMap& world, const gfx::Rect& panel, int x, int z,
				   Vec2& out);

	void Render(gfx::SpriteBatch& batch, const ui::Theme& theme,
				const WorldMap& world, const WorldState& state,
				const gfx::Rect& panel);

private:
	struct Transform {
		float cell = 1.0f;
		float ox = 0.0f, oy = 0.0f;
	};
	Transform ComputeTransform(const WorldMap& world, const gfx::Rect& panel) const;
	// The map area: the panel minus the toolbar band on top (while Editing)
	// and the caption band along the bottom.
	gfx::Rect GridArea(const gfx::Rect& panel) const;
	// The toolbar band. Zero-height unless Editing, which is what keeps the grid
	// in the same place whether or not the band is there to push it down.
	gfx::Rect ToolbarRect(const gfx::Rect& panel) const;
	// The font, sized off the panel - Update, Render and CellPoint all lay out
	// against it, so all three fit it the same way first.
	void FitFont(const gfx::Rect& panel) {
		SetFontHeight(std::clamp(panel.h * 0.030f, 11.0f, 30.0f));
	}
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
	// The overlay's way back to the dungeon map, top-left of the grid - the
	// SAME corner MapView puts its way here, so the pair reads as one control
	// that stays put rather than two buttons that swap places.
	gfx::Rect DungeonButton(const gfx::Rect& panel) const;
	bool ShowDungeonButton() const { return m_overlay && onShowDungeon != nullptr; }
	gfx::Rect CloseButton(const gfx::Rect& panel) const; // mirrors DungeonButton
	bool ShowCloseButton() const { return m_overlay && onClose != nullptr; }

	ui::FontLibrary& m_fonts;
	const ui::Font* m_font = nullptr;
	// Borrowed from the shared cache (AssetUtil's ToolbarIcon) — the level
	// editor's band draws from the same textures.
	const gfx::Texture *m_icoSettings = nullptr, *m_icoSave = nullptr,
					   *m_icoUndo = nullptr, *m_icoRedo = nullptr,
					   *m_icoWorlds = nullptr, *m_icoNewWorld = nullptr,
					   *m_icoBoxDungeon = nullptr, // the player's way back
					   *m_icoClose = nullptr;      // the shared dialog close box
	Tool m_hoverTool = Tool::None; // tracked by Update in WINDOW pixels; the
								   // render re-derives its own geometry and
								   // matches by IDENTITY, never by coordinate
	bool m_overlay = false;     // drawn as the player's map, not the travel screen
	Drawn m_drawn;              // what the last Render drew (LastDrawn)
	bool m_hoverDungeon = false; // that button's hover, tracked the same way
	bool m_hoverClose = false;   // and the close box's

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
