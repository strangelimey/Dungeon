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

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
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

	MapView(gfx::GraphicsDevice& device, GameSettings& settings,
			ui::FontLibrary& fonts);

	// The world it draws — REBOUND, not fixed (docs/world-on-demand.md): a
	// world is built when a game starts and destroyed when another is chosen,
	// so the view is handed each new one and told when it is gone (null). No
	// view drawing or picking happens without one; the owner never opens the
	// map with no world loaded.
	void SetWorld(DungeonWorld* world) {
		m_world = world;
		m_browse.reset(); // a snapshot of the old world's level
	}

	// Wires the Editor-mode collaborator (Game owns both; see file banner). Until
	// set, Editor mode shows an empty left dock.
	void SetEditor(MapEditor* editor); // also hands it the palette's box icons

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
	// The toolbar's Check button: run the playability check and show its report.
	std::function<void()> onValidate;
	// The toolbar's Generate button: open the generator knobs for this level.
	std::function<void()> onGenerate;
	// The toolbar's [+] button: the owner (Game) opens the generator dialog in
	// CREATE mode, and jumps the view (SetViewLevel) onto whatever it makes -
	// the creation is the dialog's decision now, so there is no stem to return
	// here.
	//
	// IT LANDS IN THE DUNGEON BEING VIEWED (W5): the argument is that dungeon's
	// id, so the new level joins its `levels` list instead of arriving loose in
	// the manifest. Empty when the viewed level belongs to no dungeon — an
	// orphan begets an orphan, which is honest rather than guessing a home.
	std::function<void(const std::string& dungeonId)> onNewLevel;
	// The toolbar's New world disc, right of [+]: the owner opens the
	// NewWorldDialog (P4) - the world screen's toolbar has the same disc.
	std::function<void()> onNewWorld;

	// --- the player map's WORLD page (W6) ------------------------------
	// The M-map can show the overworld instead of this level. MapView does
	// not draw it — WorldMapView does, into the same panel — so all this
	// side needs is the way ACROSS. (That split is the one MapView's own
	// header already argues for: this class is built around a DungeonMap,
	// and bending it to a grid of terrain kinds would cost more than the
	// drawing code it saved.)
	std::function<void()> onShowWorld;
	// Whether the project HAS an overworld. False hides the toggle rather
	// than dimming it: a game that is all dungeon should not advertise a
	// map it does not have.
	bool hasWorld = false;
	// The player map's close box, top-right (Esc still closes it too). The
	// owner closes the overlay; null hides the box.
	std::function<void()> onClose;

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
		m_statusLen = 0;        // an old message does not greet a fresh open
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

	// LIVE VALIDATION (docs/editor-updates-plan.md, P2): the findings to box on
	// the grid - Game's cache (Game::RefreshLiveIssues), borrowed. Null when the
	// editor is not up, which draws nothing.
	void SetIssues(const std::vector<validate::Issue>* issues) { m_issues = issues; }

	// The editor's pause/play toolbar button: while true, Game freezes the
	// world simulation (monsters, party, particles) so the level can be
	// edited against a still scene. Editor-mode only, and always cleared when
	// the overlay closes or flips to Player mode (Open/Close/SetMode) — so a
	// closed editor never leaves the game paused.
	bool EditorPaused() const {
		return m_mode == Mode::Editor && m_editorPaused;
	}
	// Press the pause button from outside (the landing page's Editor entry
	// opens the editor paused). Editor mode only, like the button.
	void SetEditorPaused(bool on) { m_editorPaused = on && m_mode == Mode::Editor; }

	// The editor's MESSAGE LINE. Every editor report (a placement, a refusal,
	// an erase) goes out through DungeonWorld::onMessage to the HUD's message
	// log - which the full-screen editor does not draw, so a refused brush used
	// to fail with no word at all (play-test #1: "nothing happens"). The owner
	// forwards each line here while the editor is up, and Render shows the
	// latest one over the bottom of the grid for a few seconds. A fixed buffer,
	// so a message costs no allocation; an over-long line is cut.
	void ShowStatus(std::string_view line);
	// What the message line holds (empty once it has faded) - for the harness.
	std::string_view Status() const;

	// Jump the viewport to a level by stem (the dropdown's pick; the arrows'
	// StepViewLevel folds into this). Public because the check report navigates
	// by it: a finding names a level, and clicking it has to take you there.
	void SetViewLevel(const std::string& stem);

	// A level was renamed (the Level dialog's inline edit): if the viewport is
	// browsing it, rebuild the snapshot under the new stem so ViewedLevel()
	// and the dropdown label stay truthful.
	void OnLevelRenamed(const std::string& oldStem, const std::string& newStem) {
		if (m_browse && m_browse->stem == oldStem)
			m_browse = m_world->BrowseLevel(newStem);
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
	// The dungeon whose `levels` list claims the viewed level, or "" when
	// nothing does (W5). What the [+] button hands the owner, and what the
	// picker opens expanded.
	std::string ViewedDungeon() const;
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

	// THE EDGE HANDLES (play-test #3: "drag the map's edges"). In Editor mode the
	// map is fitted with a margin of EdgeBand() all round, and that margin is
	// where the handles are - OUTSIDE every cell, so grabbing an edge can never be
	// mistaken for painting or moving the square beside it. EdgeAt names the edge
	// under the pointer: 0 none, 1 left, 2 right, 3 top, 4 bottom.
	float EdgeBand(const gfx::Rect& panel) const;
	int EdgeAt(float mx, float my, const gfx::Rect& panel) const;

public:
	// The drawn map's own rectangle (all of its cells) in `panel`'s pixel space,
	// and the handle band around it - for the harness (`editor view`), which has
	// to aim at an edge the way a hand does.
	gfx::Rect MapRect(const gfx::Rect& panel) const;
	float HandleBand(const gfx::Rect& panel) const { return EdgeBand(panel); }

private:
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
	// The TOOL STRIP (Editor only, MapView_Tools.cpp): a column of tool discs
	// between the palette dock and the grid - paint, rectangle, flood, area,
	// eyedropper, then Fill level. Its own column rather than rows in the dock
	// so it stays put when the palette collapses. GridArea gives it up.
	gfx::Rect ToolStripRect(const gfx::Rect& panel) const;
	// The STATUS BAR (Editor only): a full-width band fixed across the panel
	// bottom, the toolbar's mirror - the docks and the grid end above it. It
	// reads out the hovered square's coordinates.
	gfx::Rect StatusBarRect(const gfx::Rect& panel) const;
	// (AppendStripButtons is declared beside ToolbarButtons, after ToolButton.)
	// The strip's frame, the picked tool's ring and the Rectangle tool's
	// in-progress box, drawn before the buttons (Render calls it).
	void RenderToolStrip(gfx::SpriteBatch& batch, const ui::Theme& theme,
						 const gfx::Rect& panel) const;
	// A left press / hold / release on the grid, routed through the picked tool
	// (Shift/Ctrl/Alt borrow Rectangle/Flood/Eyedropper). Returns true when the
	// frame's input was consumed; `painted` says a browsed snapshot is stale.
	bool UpdateBrush(const Input& input, const gfx::Rect& panel, float mx, float my,
					 bool overGrid, bool& painted);

	// --- live validation (MapView_Issues.cpp) ---------------------------------
	// The findings on square (x,z) of the VIEWED level - where a finding stands,
	// or one of the other squares it names (Issue::also: a stair's far end, a
	// lost item beyond the first). Appends to `out`.
	void IssuesAt(int x, int z, std::vector<const validate::Issue*>& out) const;
	// A red (error) or amber (warning) box on every square of the viewed level a
	// finding names; red wins where both land. Drawn under the hover and
	// selection rings, inside the grid's scissor.
	void RenderIssueBoxes(gfx::SpriteBatch& batch, const gfx::Rect& panel) const;
	// The hovered boxed square's findings, word-wrapped, placed like the hand
	// slot tooltips (below the square, above when that would run off). Only on
	// a frame Update ran - a modal dialog stops Update, and the hovered square
	// it last saw is stale under the dialog.
	void RenderIssueTooltip(gfx::SpriteBatch& batch, const ui::Theme& theme,
							const gfx::Rect& panel);
	// The Check disc's badge: how many findings have NO square (a level or the
	// world as a whole), red when any is an error. Clicking Check lists them.
	void RenderCheckBadge(gfx::SpriteBatch& batch, const gfx::Rect& disc) const;
	const std::vector<validate::Issue>* m_issues = nullptr; // Game's cache, borrowed
	bool m_updatedSinceRender = false; // see RenderIssueTooltip
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
	DungeonWorld* m_world = nullptr; // see SetWorld
	GameSettings& m_settings; // owns the persisted dock-collapse flags
	MapEditor* m_editor = nullptr; // Editor-mode brush palette + tools (not owned)
	ui::FontLibrary& m_fonts;
	// Glyph icons + labels, borrowed from the library (Body) and re-pointed
	// whenever the panel resizes -- no atlas of its own any more.
	const ui::Font* m_font = nullptr;

	// Toolbar icon discs (Wenrexa house style, baked by the icon factory —
	// see the editor-polish thread notes). A missing file leaves the pointer
	// null and that button falls back to its text face. BORROWED from the
	// shared cache (AssetUtil's ToolbarIcon), which the world screen's toolbar
	// draws from too — one texture, one SRV slot, whichever asks first.
	const gfx::Texture *m_icoCheck = nullptr, *m_icoGen = nullptr;
	const gfx::Texture *m_icoLevel = nullptr, *m_icoBalance = nullptr,
					   *m_icoUndo = nullptr, *m_icoRedo = nullptr,
					   *m_icoSave = nullptr, *m_icoSource = nullptr,
					   *m_icoNew = nullptr, *m_icoPlay = nullptr,
					   *m_icoPause = nullptr, *m_icoNewWorld = nullptr;
	// The tool strip's discs, by MapEditor::Tool, then Fill level (icon_tb_tool_*).
	std::array<const gfx::Texture*, 5> m_icoTools{};
	const gfx::Texture* m_icoFillLevel = nullptr;
	// The docks' collapse buttons: square boxes, "<<" and ">>" (icon_tb_dock_*).
	const gfx::Texture *m_icoDockL = nullptr, *m_icoDockR = nullptr;
	// The palette's boxes, passed on to MapEditor (see MapEditor::SetIcons).
	const gfx::Texture *m_icoClose = nullptr, *m_icoBoxPlus = nullptr,
					   *m_icoBoxMinus = nullptr;
	// The Player map's level browse arrows (icon_tb_box_up / _down).
	const gfx::Texture *m_icoBoxUp = nullptr, *m_icoBoxDown = nullptr;
	const gfx::Texture* m_icoBoxWorld = nullptr; // the way to the world map
	// The Rectangle tool's drag: the press square and the square under the
	// pointer now. Painted on the release (UpdateBrush), previewed until then.
	bool m_rectDrag = false;
	int m_rectX0 = 0, m_rectZ0 = 0, m_rectX1 = 0, m_rectZ1 = 0;
	bool m_editorPaused = false; // pause/play toolbar toggle (see EditorPaused)
	// The edge drag (EdgeAt): the edge under the pointer, the one being dragged,
	// where the drag started (along its axis), and how many cells it has moved -
	// negative is left/up, so dragging the left edge left grows the level.
	int m_edgeHover = 0;
	int m_edgeDrag = 0;
	float m_edgeStart = 0.0f;
	int m_edgeDelta = 0;
	// The message line (ShowStatus): the text, its length, and when it arrived.
	static constexpr float kStatusSeconds = 5.0f; // shown this long, the last 1 s fading
	std::array<char, 256> m_status{};
	size_t m_statusLen = 0;
	std::chrono::steady_clock::time_point m_statusAt{};
	float StatusAge() const;

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
		LevelSettings, Check, Generate, NewLevel, LevelPick, PlayPause, CollapseL,
		CollapseR, ShowWorld, NewWorld, Close,
		// The tool strip, in MapEditor::Tool order, then its one action.
		ToolPaint, ToolRect, ToolFlood, ToolArea, ToolPick, FillLevel
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
		bool selected = false; // the strip's picked tool (drawn ringed)
		bool strip = false;    // a strip button: its tooltip opens BESIDE it
	};
	std::vector<ToolButton> ToolbarButtons(const gfx::Rect& panel) const;
	// Appends the TOOL STRIP's buttons to that list, so hover, click and render
	// walk them with everything else (ToolbarButtons calls it).
	void AppendStripButtons(std::vector<ToolButton>& btns, const gfx::Rect& panel) const;
	// The band itself: full panel width in Editor mode, zero-height otherwise
	// (Player mode keeps the floating browse arrows instead).
	gfx::Rect ToolbarRect(const gfx::Rect& panel) const;
	// The level DROPDOWN (left end of the band): the closed box shows the
	// viewed level's stem; open, it lists the project's levels GROUPED BY THE
	// DUNGEON that claims them (W5). Hand-rolled like the rest of MapView's
	// chrome (the toolbar is not a UIContext widget tree).
	gfx::Rect LevelPickRect(const gfx::Rect& panel) const;
	gfx::Rect LevelItemRect(int index, const gfx::Rect& panel) const;
	gfx::Rect NewLevelButton(const gfx::Rect& panel) const; // [+], right of it
	// The player map's way to the WORLD page, top-right of the grid — the
	// same corner WorldMapView puts the way back, so the pair reads as one
	// control that stays put rather than two that swap places.
	gfx::Rect WorldButton(const gfx::Rect& panel) const;
	bool ShowWorldButton() const {
		return m_mode == Mode::Player && hasWorld && onShowWorld != nullptr;
	}
	// The close box: the World button's square, mirrored into the top-right
	// corner (WorldMapView's is on the same pixels).
	gfx::Rect CloseButton(const gfx::Rect& panel) const;
	bool ShowCloseButton() const { return m_mode == Mode::Player && onClose != nullptr; }

	// ONE ROW LIST that hover, click and render all walk — the toolbar's own
	// idiom, applied to the popup. Before W5 the popup was a flat vector of
	// stems and each of the three walked it separately; a two-tier list with
	// three copies of the tier logic would be three chances to disagree about
	// which row is which.
	struct LevelRow {
		bool header = false;  // a dungeon (click expands), else a level (opens)
		std::string id;       // dungeon id, or level stem; "" = the orphan group
		std::string label;    // what is drawn
		bool expanded = false; // headers only
	};
	std::vector<LevelRow> LevelRows() const;
	// Opens the popup, expanding ONLY the group holding the viewed level — so
	// it opens on where you are rather than wherever it was left, the same
	// bargain Open() makes about pan and zoom.
	void OpenLevelList();

	bool m_levelsOpen = false; // dropdown popup showing
	int m_levelsHover = -1;    // hovered popup row (Update-tracked, like m_hoverBtn)
	// Which groups are expanded, by dungeon id ("" = the orphans). Reseeded
	// every time the popup opens, so it never accumulates.
	std::vector<std::string> m_groupsOpen;

	// Read-only snapshot of the browsed level (see the level-browsing section
	// above); null = the viewport shows the active level's live state.
	std::unique_ptr<DungeonWorld::LevelBrowse> m_browse;
};

} // namespace dungeon::game
