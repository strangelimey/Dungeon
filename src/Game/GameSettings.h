// ============================================================================
// Game/GameSettings.h — user options, persisted to settings.ini next to the
// exe (quality=0..3, language=<code>, volume=0..1, barscale=0.5..1.5,
// baropacity=0..1, theme_<name>=r,g,b,a, key_<action>=vkey).
//
// This struct is the master copy of everything user-tunable: GameUI copies
// the theme into every UIContext (ApplyTheme), the Party receives moveKeys via
// SetKeys, and DungeonWorld reads the quality tier when loading meshes and
// textures. The kThemeFields / kKeyFields tables drive both the ini round-trip
// here and the Settings-page controls in GameUI, so adding a field is a
// one-line change. (The resource-bar colours are NOT user settings any more:
// the fills are procedural, each kind with its own colour. An old ini's
// bar_<name>= lines are ignored, and gone at the next save.)
// ============================================================================
#pragma once

#include "Game/Party.h"           // MoveKeys
#include "Game/PartyHudTypes.h"   // HudPanelLook (not the whole HUD)
#include "Graphics/DisplayEnum.h" // gfx::FullscreenMode
#include "UI/Controls.h"          // ui::Widget complete for UIContext's unique_ptr
#include "UI/UIContext.h"         // ui::Theme

#include <array>
#include <iterator> // std::size (kLookEaseOptions helper)
#include <string>

namespace dungeon::game {

// Quality tiers, selected on the Settings → Video tab and hot-swapped
// without restart. Meshes: low/med/high worn-block tessellation (Ultra
// reuses high). Textures: 1K (Low/Medium), 2K (High), 4K (Ultra — fetchable
// content, see tools/FetchTextures.ps1; falls back per-material to 2K with
// a warning when the 4K sets are absent).
enum class Quality { Low, Medium, High, Ultra };

// Per-quality point-light budget (indexed by Quality: Low/Medium/High/Ultra).
// The Ultra value is the renderer's hard ceiling (gfx::kMaxPointLights / the
// shader's MAX_POINT_LIGHTS array size — kept in sync by a static_assert in
// the .cpp). These are also the options shown by the Video tab's Max Lights
// dropdown; picking a quality resets the budget to its tier value, after which
// the dropdown can override it.
inline constexpr int kLightBudgets[] = {16, 32, 48, 64};

// Present sync intervals offered by the Video tab's Frame Rate dropdown. Each
// presents every Nth monitor vblank, so the cap is always a divisor of the
// refresh rate and stays tear-free: 1 = full refresh, 2 = half, etc. (on a
// 144 Hz panel: 144 / 72 / 48 / 36 FPS). Capping cuts GPU load, heat, and fan
// noise. settings key presentinterval=.
inline constexpr u32 kPresentIntervals[] = {1, 2, 3, 4};

// The user-editable theme colors (Settings → UI tab). One table drives the
// ini round-trip (theme_<key>=r,g,b,a) and the color-picker grid. labelKey
// is a loc:: key (assets/lang) — GameUI translates it when building the row.
struct ThemeField {
	const char* key;
	const char* labelKey;
	Vec4 ui::Theme::*field;
};
inline constexpr ThemeField kThemeFields[] = {
	{"panel", "theme.panel", &ui::Theme::panel},
	{"panelborder", "theme.border", &ui::Theme::panelBorder},
	{"control", "theme.control", &ui::Theme::control},
	{"controlhot", "theme.hot", &ui::Theme::controlHot},
	{"controlactive", "theme.active", &ui::Theme::controlActive},
	{"text", "theme.text", &ui::Theme::text},
	{"textdim", "theme.textdim", &ui::Theme::textDim},
	{"accent", "theme.accent", &ui::Theme::accent},
};

// Party identity colors, one per roster slot (portrait border, hand-slot
// stripe, the log tint on lines about that member). The settings copy is the
// MASTER — Game::ApplyMemberColors pushes it onto the roster at creation/
// reset and the Settings → UI picker edits it live; CreateDefaultParty's
// authored palette only mirrors these defaults. Ini keys member_<n>=r,g,b,a.
// BRIGHT since ui-updates (2026-10-01): the old rust / moss / gold / indigo were
// authored dark for flat fills and sank into the stone chrome (Tilo's indigo and
// Maren's gold worst). The old values are retired in GameSettings.cpp, so an
// existing ini moves to these unless the player picked a colour.
inline constexpr size_t kMemberColorCount = 4;
inline constexpr Vec4 kDefaultMemberColors[kMemberColorCount] = {
	{0.92f, 0.36f, 0.20f, 1.0f}, // slot 0 - ember (Brand)
	{0.36f, 0.82f, 0.34f, 1.0f}, // slot 1 - leaf (Sera)
	{1.00f, 0.82f, 0.26f, 1.0f}, // slot 2 - gold (Maren)
	{0.66f, 0.48f, 1.00f, 1.0f}, // slot 3 - violet (Tilo)
};

// And for the movement keys (MoveKeys; ini keys key_<action>=vkey). Order is
// the Settings → Game tab's row order and must match GameUI's key-bind rows.
struct KeyField {
	const char* key;
	const char* labelKey;
	int MoveKeys::*field;
};
inline constexpr KeyField kKeyFields[] = {
	{"forward", "settings.key.forward", &MoveKeys::forward},
	{"back", "settings.key.back", &MoveKeys::back},
	{"strafeleft", "settings.key.strafeleft", &MoveKeys::strafeLeft},
	{"straferight", "settings.key.straferight", &MoveKeys::strafeRight},
	{"turnleft", "settings.key.turnleft", &MoveKeys::turnLeft},
	{"turnright", "settings.key.turnright", &MoveKeys::turnRight},
};

// The easing curves offered by the mouse-look "return curve" dropdowns (Settings
// → Controls). A curated subset of Easing (Core/Easing.h) — labelKey is a loc::
// key. The selected INDEX into this table is what round-trips to settings.ini, so
// appending is safe; reordering remaps existing saves.
struct EaseOption {
	const char* labelKey;
	Easing value;
};
inline constexpr EaseOption kLookEaseOptions[] = {
	{"ease.linear", Easing::Linear},
	{"ease.easein", Easing::EaseIn},
	{"ease.easeinout", Easing::EaseInOut},
	{"ease.easeincubic", Easing::EaseInCubic},
	{"ease.easeinquart", Easing::EaseInQuart},
	{"ease.easeout", Easing::EaseOut},
};

// Index of an easing within kLookEaseOptions (0 if not offered) — maps a stored
// LookSettings curve back onto the dropdown selection.
inline int LookEaseIndex(Easing e) {
	for (int i = 0; i < static_cast<int>(std::size(kLookEaseOptions)); ++i)
		if (kLookEaseOptions[i].value == e) return i;
	return 0;
}

struct GameSettings {
	Quality quality = Quality::Medium;
	int maxPointLights = kLightBudgets[1]; // active light budget (defaults to the
										   // Medium tier; ini key maxlights=)
	u32 presentInterval = 1;      // 1 = VSync to refresh, 2/3/4 = divide it
								  // (tear-free; ini presentinterval=)
	std::string language = "en";  // assets/lang/<code>.lang stem
	float volume = 1.0f;          // master volume, pushed into the AudioEngine
	// Textured UI chrome (UI/Skin.h). Off = the flat theme-fill look, kept
	// deliberately as a DEBUG MODE (widget containment/extents read at a
	// glance). Settings → UI checkbox; ini uiskin=.
	bool uiSkin = true;
	// The stone that chrome is cut from: an assets/ui/stones/<name>.png stem
	// (tools/BuildUiStones.py). Settings → UI dropdown; ini ui_stone=.
	std::string uiStone = "granite_grey";
	// Walking head bob (Party::EyePosition's footfall dip + sway). Off for
	// motion-sensitive players — the eye glides dead level. Settings → UI
	// checkbox; ini headbob=.
	bool headBob = true;
	ui::Theme theme;              // the 8 user-editable control colors
	// Per-slot party identity colors (see kDefaultMemberColors above).
	std::array<Vec4, kMemberColorCount> memberColors{
		kDefaultMemberColors[0], kDefaultMemberColors[1],
		kDefaultMemberColors[2], kDefaultMemberColors[3]};
	MoveKeys moveKeys;            // movement key bindings (vkeys)
	LookSettings look;           // right-mouse free-look feel (Controls tab)
	// Picking an entry from a hand's right-click use menu also PERFORMS it (on
	// top of recording it as the hand's left-click default). Off = the menu
	// only sets the default. Controls tab checkbox; ini usemenu_execute=.
	bool useMenuExecutes = true;
	// How many most-recently-cast spells the hand menu's Magic quick-cast
	// list shows (per member, below the Spellbook entry). Controls → Hands
	// dropdown, 1..10; ini spell_mru=.
	int spellMruCount = 5;
	// THE WORLD THE GAME OPENS (W7): a project folder name under
	// assets/projects. Persisted rather than passed, because switching one
	// RELAUNCHES — the same bargain the adapter change makes, and for the
	// same reason: everything downstream of the choice is built at startup.
	// `-project <name>` on the command line overrides it for one run, which
	// is how a test scenario gets its own world without touching the ini.
	std::string projectName = "dungeon-demo";

	bool mapPaletteCollapsed = false;  // map editor: left brush dock collapsed
	bool mapLegendCollapsed = false;   // map editor: right key dock collapsed
	bool mapShowCatalog = false;       // map editor: surfaces show the whole catalog
	int mapTool = 0;                   // map editor: the tool strip's picked tool (MapEditor::Tool)
	// map editor: the palette's category bar - which grouping (MapEditor::
	// Grouping: 0 stage, 1 kind) and the group picked in each.
	// map editor: the docks' dragged widths, as a share of the editor panel's
	// width (0 = the built-in default; MapView clamps whatever is stored).
	float mapPaletteWidth = 0.0f;
	float mapLegendWidth = 0.0f;
	// map editor: the right dock's two sections, and the overview's scope
	// (MapView::OverviewScope: 0 world, 1 dungeon, 2 level).
	bool mapOverviewCollapsed = false;
	bool mapKeyCollapsed = false;
	int mapOverviewScope = 2;
	int mapPaletteGrouping = 0;
	int mapPaletteStage = 1;           // Build: where a new level's work starts
	int mapPaletteKind = 0;
	// The FLOATING HUD panels (UI/FloatingPanel.h): each one's saved spot, scale,
	// background opacity and whether it is minimized into the tray (PartyHudTypes.h
	// HudPanelLook). kHudPanelFields below lists them and drives the ini
	// round-trip (hud_<id>_pos / _scale / _opacity / _hidden), the Settings -> UI
	// rows and Reset. The party bar's scale and opacity were barscale= /
	// baropacity= before it floated, and the two docks' hud_move_collapsed= /
	// hud_magic_collapsed= became their _hidden; those still load.
	HudPanelLook hudParty, hudStatus, hudOptions, hudMove, hudHands, hudMagic;
	// The two floating WINDOWS (P3b): the party inventory and the sheet.
	HudPanelLook hudInventory, hudSheet;
	// The closed-panels TRAY (ui-updates Phase 8, Game/HudTray.h): a button per
	// minimized panel. Never hidden itself - it shows while it has a button.
	HudPanelLook hudTray;
	// THE HUD LAYOUT (P4): 0 = Standard (party bar + Hands dock), 1 = Minimal
	// (one card per member: portrait, bars and hands together - Game/
	// MemberCards.h). Settings -> UI "Layout"; ini hud_layout=. The cards
	// block is a floating panel of its own.
	int hudLayout = 0;
	HudPanelLook hudCards;
	// Settings -> UI "Lock HUD layout": no grips, no drags. ini hud_locked=.
	bool hudLocked = false;
	// The level generator's last-USED knobs, encoded by Game/GenerateKnobs.h
	// ("path:6 branches:3 ..."). Held as the opaque line so settings knows
	// nothing of the generator; empty = its defaults. ini gen_knobs=.
	std::string generatorKnobs;

	// --- display (Settings → Video) ---------------------------------------------
	// The chosen GPU (packed LUID, 0 = auto), the monitor (output index on that
	// adapter), the resolution (0 = the window default / the monitor's native for
	// borderless), and the presentation mode. Read at boot by Main to construct
	// the device/window; an adapter change is applied by relaunching the exe.
	u64 adapterLuid = 0;
	int displayOutput = 0;
	int displayWidth = 0, displayHeight = 0;
	gfx::FullscreenMode fullscreen = gfx::FullscreenMode::Windowed;

	// settings.ini round-trip (the exe's directory). Load keeps the defaults
	// for anything missing or malformed; a first run with no file is fine.
	void Load();
	void Save() const;

	// Quality-derived asset suffixes.
	const char* MeshSuffix() const;    // "low" / "med" / "high" (worn blocks)
	const char* TextureSuffix() const; // "1k" / "2k" / "4k" (texture sets)
	const char* QualityLabel() const;  // "Low" / "Medium" / "High" / "Ultra"

	// The light budget a quality tier resets to (kLightBudgets[quality]), and
	// the index of a budget value within kLightBudgets (snapped to the nearest
	// tier for the Max Lights dropdown). Both used by the Video tab.
	static int QualityLightBudget(Quality q) {
		return kLightBudgets[static_cast<int>(q)];
	}
	static int LightBudgetIndex(int value);

	// Index of a present interval within kPresentIntervals (exact match, else 0 =
	// full refresh) — drives the Video tab's Frame Rate dropdown selection.
	static int PresentIntervalIndex(u32 interval);

	// The log's movement help line ("W/S move, A/D strafe, Q/E turn."),
	// built from the live bindings.
	std::string MoveKeysHelp() const;
};

// The floating HUD panels, in the order Settings -> UI lists them: the id is
// the ini stem (hud_<id>_pos ...) and the `hudpanel` dev command's name, the
// label key heads its Settings rows (and names its tray button). `glyph` is
// the tray button's face (assets/ui/glyph_<glyph>.png, tools/BuildToolIcons.py)
// and says the panel MINIMIZES at all: null = it does not (the two windows
// close instead, and the tray only empties).
struct HudPanelField {
	const char* id;
	const char* labelKey;
	HudPanelLook GameSettings::*look;
	const char* glyph = nullptr;
};
inline constexpr HudPanelField kHudPanelFields[] = {
	{"party", "settings.party_bar", &GameSettings::hudParty, "panel_party"},
	{"status", "settings.status_panel", &GameSettings::hudStatus, "panel_status"},
	{"options", "settings.options_panel", &GameSettings::hudOptions, "panel_options"},
	{"move", "settings.move_panel", &GameSettings::hudMove, "panel_move"},
	{"hands", "settings.hands_panel", &GameSettings::hudHands, "panel_hands"},
	{"magic", "settings.magic_panel", &GameSettings::hudMagic, "panel_magic"},
	{"cards", "settings.cards_panel", &GameSettings::hudCards, "panel_cards"},
	{"inventory", "settings.inventory_panel", &GameSettings::hudInventory},
	{"tray", "settings.tray_panel", &GameSettings::hudTray},
	{"sheet", "settings.sheet_panel", &GameSettings::hudSheet},
};
// Their indices, for code that needs one panel by name. The sheet stays LAST:
// it alone lives in another UI context, and [0, kHudSheet) is "the HUD's".
enum HudPanelIndex : size_t {
	kHudParty, kHudStatus, kHudOptions, kHudMove, kHudHands, kHudMagic,
	kHudCards, kHudInventory, kHudTray, kHudSheet
};
static_assert(std::size(kHudPanelFields) == kHudSheet + 1);

} // namespace dungeon::game
