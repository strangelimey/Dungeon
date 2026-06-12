// ============================================================================
// Game/GameSettings.h — user options, persisted to settings.ini next to the
// exe (quality=0..3, volume=0..1, barscale=0.5..1.5, baropacity=0..1,
// theme_<name>=r,g,b,a, bar_<name>=r,g,b,a, key_<action>=vkey).
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
// ini round-trip (theme_<key>=r,g,b,a) and the color-picker grid.
struct ThemeField {
	const char* key;
	const char* label;
	Vec4 ui::Theme::*field;
};
inline constexpr ThemeField kThemeFields[] = {
	{"panel", "Panel", &ui::Theme::panel},
	{"panelborder", "Border", &ui::Theme::panelBorder},
	{"control", "Control", &ui::Theme::control},
	{"controlhot", "Hot", &ui::Theme::controlHot},
	{"controlactive", "Active", &ui::Theme::controlActive},
	{"text", "Text", &ui::Theme::text},
	{"textdim", "Dim text", &ui::Theme::textDim},
	{"accent", "Accent", &ui::Theme::accent},
};

// Same idea for the HUD resource-bar fills (PartyHud's ResourceBarColors;
// ini keys bar_<key>=r,g,b,a).
struct BarField {
	const char* key;
	const char* label;
	Vec4 ResourceBarColors::*field;
};
inline constexpr BarField kBarFields[] = {
	{"health", "Health", &ResourceBarColors::health},
	{"stamina", "Stamina", &ResourceBarColors::stamina},
	{"mana", "Mana", &ResourceBarColors::mana},
};

// And for the movement keys (MoveKeys; ini keys key_<action>=vkey). Order is
// the Settings → Game tab's row order and must match GameUI's key-bind rows.
struct KeyField {
	const char* key;
	const char* label;
	int MoveKeys::*field;
};
inline constexpr KeyField kKeyFields[] = {
	{"forward", "Move forward", &MoveKeys::forward},
	{"back", "Move back", &MoveKeys::back},
	{"strafeleft", "Step left", &MoveKeys::strafeLeft},
	{"straferight", "Step right", &MoveKeys::strafeRight},
	{"turnleft", "Turn left", &MoveKeys::turnLeft},
	{"turnright", "Turn right", &MoveKeys::turnRight},
};

struct GameSettings {
	Quality quality = Quality::Medium;
	float volume = 1.0f;          // master volume, pushed into the AudioEngine
	float partyBarScale = 1.0f;   // HUD party bar: 0.5–1.5 about its top center
	float partyBarOpacity = 1.0f; // HUD party bar: slot background alpha
	ui::Theme theme;              // the 8 user-editable control colors
	ResourceBarColors barColors;  // health/stamina/mana fills
	MoveKeys moveKeys;            // movement key bindings (vkeys)

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
