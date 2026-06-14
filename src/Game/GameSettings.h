// ============================================================================
// Game/GameSettings.h — user options, persisted to settings.ini next to the
// exe (quality=0..3, language=<code>, volume=0..1, barscale=0.5..1.5,
// baropacity=0..1, theme_<name>=r,g,b,a, bar_<name>=r,g,b,a,
// key_<action>=vkey).
//
// This struct is the master copy of everything user-tunable: GameUI copies
// the theme into every UIContext (ApplyTheme), the HUD widgets point at
// barColors, the Party receives moveKeys via SetKeys, and DungeonWorld reads
// the quality tier when loading meshes and textures. The kThemeFields /
// kBarFields / kKeyFields tables drive both the ini round-trip here and the
// Settings-page controls in GameUI, so adding a field is a one-line change.
// ============================================================================
#pragma once

#include "Game/Party.h"    // MoveKeys
#include "Game/PartyHud.h" // ResourceBarColors
#include "UI/UIContext.h"  // ui::Theme

#include <string>

namespace dungeon::game {

// Quality tiers, selected on the Settings → Video tab and hot-swapped
// without restart. Meshes: low/med/high worn-block tessellation (Ultra
// reuses high). Textures: 1K (Low/Medium), 2K (High), 4K (Ultra — fetchable
// content, see tools/FetchTextures.ps1; falls back per-material to 2K with
// a warning when the 4K sets are absent).
enum class Quality { Low, Medium, High, Ultra };

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

// Same idea for the HUD resource-bar fills (PartyHud's ResourceBarColors;
// ini keys bar_<key>=r,g,b,a).
struct BarField {
	const char* key;
	const char* labelKey;
	Vec4 ResourceBarColors::*field;
};
inline constexpr BarField kBarFields[] = {
	{"health", "bar.health", &ResourceBarColors::health},
	{"stamina", "bar.stamina", &ResourceBarColors::stamina},
	{"mana", "bar.mana", &ResourceBarColors::mana},
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

struct GameSettings {
	Quality quality = Quality::Medium;
	std::string language = "en";  // assets/lang/<code>.lang stem
	float volume = 1.0f;          // master volume, pushed into the AudioEngine
	float partyBarScale = 1.0f;   // HUD party bar: 0.5–1.5 about its top center
	float partyBarOpacity = 1.0f; // HUD party bar: slot background alpha
	ui::Theme theme;              // the 8 user-editable control colors
	ResourceBarColors barColors;  // health/stamina/mana fills
	MoveKeys moveKeys;            // movement key bindings (vkeys)
	bool mapPaletteCollapsed = false; // map editor: left brush dock collapsed
	bool mapLegendCollapsed = false;  // map editor: right key dock collapsed

	// settings.ini round-trip (the exe's directory). Load keeps the defaults
	// for anything missing or malformed; a first run with no file is fine.
	void Load();
	void Save() const;

	// Quality-derived asset suffixes.
	const char* MeshSuffix() const;    // "low" / "med" / "high" (worn blocks)
	const char* TextureSuffix() const; // "1k" / "2k" / "4k" (texture sets)
	const char* QualityLabel() const;  // "Low" / "Medium" / "High" / "Ultra"

	// The log's movement help line ("W/S move, A/D strafe, Q/E turn."),
	// built from the live bindings.
	std::string MoveKeysHelp() const;
};

} // namespace dungeon::game
