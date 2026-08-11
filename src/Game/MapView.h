// ============================================================================
// Game/MapView.h — the in-game map overlay, and the in-game dungeon editor.
//
// A stylized top-down view of the level drawn OVER the running game. Like the
// dev console it is a toggle, not an app state: while it is open the world
// keeps simulating and the party still walks (the overlay only claims the
// mouse, for panning/zooming/editing). Two modes:
//   Player (M key)       — an 80%-centered overlay; fog of war (only revealed
//                          cells and their contents draw, DungeonWorld::IsSeen).
//                          Carries a right-docked symbol key (a trimmed subset
//                          of the editor's), collapsible like the editor docks.
//   Editor (`editor` cmd)— full-screen and drawn alone; the whole map and
//                          every creature/item draw regardless of fog, with a
//                          brush palette docked left and a full symbol key
//                          docked right. Every dock collapses to a single
//                          flip-arrow button (state persisted in GameSettings).
//
// MapView is the shared VIEWPORT — pan/zoom, the cell/marker/party render, fog,
// and the right symbol key (both modes). The editor's brush palette and brush-
// apply logic live in a separate collaborator, MapEditor (NOT a subclass — that
// would fight the in-place Player<->Editor mode flip): set it once via
// SetEditor and MapView drives it while in Editor mode. The left dock's frame,
// collapse button and "Brushes" header are MapView's (they affect layout); its
// body is filled by MapEditor::RenderBody.
//
// Coordinate note: the map is +X east / +Z south with row index growing
// southward (DungeonMap.h), and screen Y grows downward too, so cell->screen is
// a direct mapping with north up — no flips.
// ============================================================================
#pragma once

#include "Game/DungeonWorld.h"
#include "Game/GameSettings.h" // collapse-state persistence
#include "Game/Placement.h"    // Placement (the hover ghost)
#include "Graphics/GraphicsDevice.h"
#include "Graphics/SpriteBatch.h"
#include "Platform/Input.h"
#include "UI/Font.h"
#include "UI/UIContext.h" // ui::Theme

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dungeon::game {

class MapEditor; // the editor's brush palette + tools (Editor mode collaborator)

class MapView {
public:
	// Two modes (see the file banner). Player: fog of war (only revealed cells
	// and their contents draw), no editing — the in-game map. Editor: the whole
	// map and every creature/item draw regardless of fog, plus the tool palette
	// and cell painting — the dungeon-building tool, reached via the dev
	// console's `editor` command.
	enum class Mode { Player, Editor };

	MapView(gfx::GraphicsDevice& device, DungeonWorld& world,
			GameSettings& settings, ui::FontLibrary& fonts);

	// Wires the Editor-mode collaborator (Game owns both; see file banner). Until
	// set, Editor mode shows an empty left dock.
	void SetEditor(MapEditor* editor) { m_editor = editor; }

	// Fired by the Editor-mode header buttons top-right of the grid: Save writes
	// every edited level (the savemap console command), To source additionally
	// copies the project into the repo tree (synctosource). The owner (Game)
	// does the work and messages the outcome.
	std::function<void(bool toSource)> onSave;
	// The Balance toolbar button: opens the combat-tuning dialog
	// (BalanceDialog — the balance.cat/attacks.cat front-end).
	std::function<void()> onBalance;
	// The Level toolbar button: opens the per-level settings dialog
	// (LevelSettingsDialog — the .map `atmosphere` record's front-end) for
	// the level the viewport is SHOWING (ViewedLevel/ViewedMap).
	std::function<void()> onLevelSettings;
	// The toolbar's [+] button: the owner (Game) creates a fresh level on disk
	// (minimal .map/.ent + a manifest append) and returns its stem so the view
	// can jump straight to it — or "" if creation failed.
	std::function<std::string()> onNewLevel;

	bool IsOpen() const { return m_open; }
	Mode CurrentMode() const { return m_mode; }

	// Opens the overlay in `mode`, resetting the view to fit-the-whole-map (and
	// back to the ACTIVE level) so it is predictable each time rather than
	// wherever it was last panned/browsed.
	void Open(Mode mode = Mode::Player) {
		m_open = true;
		m_mode = mode;
		m_zoom = 1.0f;
		m_pan = {0.0f, 0.0f};
		m_browse.reset();
		m_levelsOpen = false;
		m_editorPaused = false; // closing/reopening always resumes the world
	}
	void Close() {
		m_open = false;
		m_editorPaused = false; // "editor closed => game always un-paused"
	}
	// The M-key player-map toggle: open in Player mode, or close.
	void Toggle() {
		if (m_open) Close();
		else Open(Mode::Player);
	}
	// Flips an already-open map's mode without disturbing the view (the dev
	// console's `editor` / `editor off`).
	void SetMode(Mode mode) {
		m_mode = mode;
		m_levelsOpen = false; // the level dropdown is Editor toolbar chrome
		m_editorPaused = false; // leaving Editor mode resumes the world
	}

	// The editor's pause/play toolbar button: while true, Game freezes the
	// world simulation (monsters, party, particles) so the level can be
	// edited against a still scene. Editor-mode only, and always cleared when
	// the overlay closes or flips to Player mode (Open/Close/SetMode) — so a
	// closed editor never leaves the game paused.
	bool EditorPaused() const {
		return m_mode == Mode::Editor && m_editorPaused;
	}

	// A level was renamed (the Level dialog's inline edit): if the viewport is
	// browsing it, rebuild the snapshot under the new stem so ViewedLevel()
	// and the dropdown label stay truthful.
	void OnLevelRenamed(const std::string& oldStem, const std::string& newStem) {
		if (m_browse && m_browse->stem == oldStem)
			m_browse = m_world.BrowseLevel(newStem);
	}

	// Re-bakes the icon font when the window height changes (the overlay text
	// scales with the screen like the rest of the UI).
	void SetFontHeight(float pixelHeight) {
		m_font = &m_fonts.Get(ui::FontRole::Body, pixelHeight);
	}

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

	// --- shared with MapEditor (the left dock lives partly in each class) ------
	// The shared icon/label font (one atlas, sized to the panel each frame).
	const ui::Font& Font() const { return *m_font; }
	// The level the viewport is SHOWING (the [^]/[v] arrows browse the project's
	// level order). MapEditor routes the brush by these: the active level edits
	// live state, any other level edits its in-memory stash (DungeonWorld's
	// remote seam).
	bool Browsing() const { return m_browse != nullptr; }
	const std::string& ViewedLevel() const;
	const DungeonMap& ViewedMap() const;
	// The left dock's scrollable body rectangle (below the collapse button +
	// "Brushes" header), where MapEditor lays out and draws the accordion.
	gfx::Rect PaletteBody(const gfx::Rect& panel) const;
	// Inner padding for dock chrome, derived from the panel so Update (window
	// pixels) and Render (device pixels) agree.
	static float DockPad(const gfx::Rect& panel);

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
	// Pixel → cell PLUS the fractional position within it (0..1 each axis, from
	// the cell's north-west corner). The sub-cell part is what lets a click name
	// an EDGE rather than just a square — see FaceAt.
	bool CellAtF(float px, float py, const gfx::Rect& panel, int& outX, int& outZ,
				 float& outFx, float& outFz) const;
	// Pixel → the wall FACE being pointed at: the nearest edge (of the four,
	// splitting the square diagonally) that actually separates a walkable cell
	// from solid rock. Either side works — point at the floor square near a wall
	// or at the wall block near that floor and you name the SAME face, because an
	// edge is shared by exactly one of each. Nearer-but-invalid edges are skipped
	// so a sloppy click still lands on something sensible. Invalid result = the
	// pointer isn't over any floor/rock boundary.
	WallFace FaceAt(float px, float py, const gfx::Rect& panel) const;
	// Whether a cell (and its contents) should draw: always in Editor mode,
	// only once revealed in Player mode. The future map-fragment / reveal-spell
	// mechanics feed the same fog set (DungeonWorld::MarkSeen), so they need no
	// change here; monster-detection effects would be a separate entity-only
	// override layered on top.
	bool CellVisible(int x, int z) const;

	// The grid-drawing area within the panel: the whole panel in Player mode,
	// the panel minus BOTH docks in Editor mode. The transform and CellAt work
	// in this rect so the map never draws under a dock.
	gfx::Rect GridArea(const gfx::Rect& panel) const;

	// Docks (resolution independent — all sized from the panel, so Update and
	// Render agree). Each collapses to a thin strip showing only its flip-arrow
	// button. The left brush palette is Editor-only; the right symbol key shows
	// in both modes (full set in Editor, trimmed in Player). Collapse flags live
	// in GameSettings — the right key's flag is per mode, via Legend*().
	gfx::Rect LeftDockRect(const gfx::Rect& panel) const;   // brush palette
	gfx::Rect RightDockRect(const gfx::Rect& panel) const;  // symbol key
	gfx::Rect LeftCollapseButton(const gfx::Rect& panel) const;
	gfx::Rect RightCollapseButton(const gfx::Rect& panel) const;
	bool LegendCollapsed() const; // the right key dock's collapse flag for the mode
	void ToggleLegend();          // flips that flag and persists

	// --- level browsing (both modes) -----------------------------------------
	// The viewport can SHOW a level other than the active one: the [^]/[v]
	// header arrows step the viewed level through the project's level order (an
	// edge level hides its dead-direction arrow — nothing above the top level,
	// nothing below the bottom). A browsed level draws a read-only snapshot of
	// its static layer + .ent records (and, in Player mode, its stashed fog);
	// the selection, party marker, and live entity markers stay active-level.
	// In EDITOR mode the brush works on a browsed level too — MapEditor routes
	// those edits to DungeonWorld's remote seam (the level's stash), and Update
	// rebuilds the snapshot after a paint so the edit shows immediately.
	// m_browse null = viewing the active level (live state).
	// Stem `step` levels away from the VIEWED one in the project's order
	// ("" = none that way); +1 = below (next stem), -1 = above.
	std::string LevelNeighbor(int step) const;
	void StepViewLevel(int step); // rebuilds m_browse (or resets, back on active)
	gfx::Rect LevelUpButton(const gfx::Rect& panel) const;
	gfx::Rect LevelDownButton(const gfx::Rect& panel) const;

	void DoUndoRedo(bool redo);
	// A trigger only LATCHES here; Render draws the buttons disabled, and the
	// NEXT Update executes. The restore itself is fast now (the surface rebake
	// is deferred to editor close — DungeonWorld::FlushGeometry), but the
	// respawn/WaitIdle can still hitch briefly, and the latch keeps the click
	// visibly taken either way. 0 = idle, -1 = undo pending, +1 = redo
	// pending. Triggers are swallowed while set.
	int m_pendingHistory = 0;

	gfx::GraphicsDevice& m_device;
	DungeonWorld& m_world;
	GameSettings& m_settings; // owns the persisted dock-collapse flags
	MapEditor* m_editor = nullptr; // Editor-mode brush palette + tools (not owned)
	ui::FontLibrary& m_fonts;
	// Glyph icons + labels, borrowed from the library (Body) and re-pointed
	// whenever the panel resizes -- no atlas of its own any more.
	const ui::Font* m_font = nullptr;

	// Toolbar icon discs (Wenrexa house style, baked by the icon factory —
	// see the editor-polish thread notes). A missing file leaves the pointer
	// null and that button falls back to its text face.
	std::unique_ptr<gfx::Texture> m_icoLevel, m_icoBalance, m_icoUndo,
		m_icoRedo, m_icoSave, m_icoSource, m_icoNew, m_icoPlay, m_icoPause;
	bool m_editorPaused = false; // pause/play toolbar toggle (see EditorPaused)

	bool m_open = false;
	Mode m_mode = Mode::Player;

	// View state. m_pan is a fraction of the panel size so it is independent of
	// the pass's pixel resolution; m_zoom multiplies the fit-to-panel cell size.
	float m_zoom = 1.0f;
	Vec2 m_pan{0.0f, 0.0f};
	bool m_panning = false;
	Vec2 m_lastMouse{0.0f, 0.0f};
	Vec2 m_panStart{0.0f, 0.0f}; // pan-press position: release near it = a CLICK
	// Grid cell currently under the mouse (-1 = none). Tracked in Update so Render
	// can highlight hovered contents (e.g. the faint item icon goes opaque). Cell
	// indices are resolution-independent, so this is valid across the window-pixel
	// (Update) / device-pixel (Render) split.
	int m_hoverX = -1, m_hoverZ = -1;
	// The wall face under the mouse, tracked the same way but only while a
	// wall-mounted brush is armed (niche, sconce, any `mount = wall` kind). Render
	// draws it as a highlight bar on the target edge, so the face a click will
	// take is visible BEFORE committing — the gesture explains itself.
	WallFace m_hoverFace;
	// Where the armed PLACEMENT brush would land, resolved every Update from the
	// hovered cell + face through MapEditor::ResolveBrush — the same call the
	// click makes, which is the whole point (Placement.h). Render draws it as a
	// ghost, including its REFUSALS: a preview that only vanishes teaches nothing,
	// so an invalid pose still draws, in the refusal colour.
	Placement m_hoverPlace;
	// Which chrome button the mouse is over, tracked the same way (Update
	// hit-tests in window pixels; Render styles the matching device-pixel rect
	// by IDENTITY, since the two spaces disagree numerically). Drives the shared
	// ui::DrawButtonFace hover styling on the hand-drawn buttons.
	enum class HoverBtn {
		None, LevelUp, LevelDown, Undo, Redo, Save, SaveSource, Balance,
		LevelSettings, NewLevel, LevelPick, PlayPause, CollapseL, CollapseR
	};
	HoverBtn m_hoverBtn = HoverBtn::None;

	// The editor's TOOLBAR — a full-width band fixed across the top of the
	// panel (Editor mode only; the docks and grid sit below it). The level
	// dropdown + [+] new-level button anchor its left end; ToolbarButtons
	// builds the icon tools right-to-left from one fixed item order, and
	// geometry, hover, click dispatch and render all read the same list —
	// adding a tool is one `add` line. Buttons draw as house-style icon
	// discs (assets/ui/icon_tb_*.png, hover brightens like ui::Button's
	// icon path; label = the hover tooltip, and the face-text fallback if
	// an icon file is missing); a disabled one (empty undo/redo stack, or
	// a latched history trigger) draws dimmed and swallows its click.
	struct ToolButton {
		HoverBtn id;
		gfx::Rect rect;
		std::string label; // tooltip (icon buttons) or face text (fallback)
		const gfx::Texture* icon;
		bool visible;
		bool enabled;
	};
	std::vector<ToolButton> ToolbarButtons(const gfx::Rect& panel) const;
	// The band itself: full panel width in Editor mode, zero-height otherwise
	// (Player mode keeps the floating browse arrows instead).
	gfx::Rect ToolbarRect(const gfx::Rect& panel) const;
	// The level DROPDOWN (left end of the band): the closed box shows the
	// viewed level's stem; open, it lists every level in the project's order.
	// Hand-rolled like the rest of MapView's chrome (the toolbar is not a
	// UIContext widget tree).
	gfx::Rect LevelPickRect(const gfx::Rect& panel) const;
	gfx::Rect LevelItemRect(int index, const gfx::Rect& panel) const;
	gfx::Rect NewLevelButton(const gfx::Rect& panel) const; // [+], right of it
	// Jump the viewport to a level by stem (the dropdown's pick; the arrows'
	// StepViewLevel folds into this).
	void SetViewLevel(const std::string& stem);
	bool m_levelsOpen = false; // dropdown popup showing
	int m_levelsHover = -1;    // hovered popup row (Update-tracked, like m_hoverBtn)

	// Read-only snapshot of the browsed level (see the level-browsing section
	// above); null = the viewport shows the active level's live state.
	std::unique_ptr<DungeonWorld::LevelBrowse> m_browse;
};

} // namespace dungeon::game
