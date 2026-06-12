// ============================================================================
// Game/GameUI.h — every 2D surface the game shows.
//
// Owns the five UIContexts (HUD, landing menu, shared settings page, pause
// menu, character sheet), the title font and landing art, and all the
// widgets in them. Builds the static pages up front (BuildStaticUi) and the
// HUD as a load task (BuildHud, once the roster's portraits exist); renders
// the loading screens, menu/pause/sheet overlays, and the in-game HUD.
//
// GameUI edits GameSettings directly (it hosts the Settings page) and saves
// it on the same triggers as before (sliders on release, pickers when their
// popup closes, key binds immediately). Anything beyond UI + settings goes
// out through the on* callbacks — the app state machine stays in Game.
// ============================================================================
#pragma once

#include "Audio/AudioEngine.h"
#include "Core/Loc.h"
#include "Game/Character.h"
#include "Game/GameSettings.h"
#include "Game/LoadQueue.h"
#include "Game/Party.h"
#include "Game/PartyHud.h"
#include "Game/SoundBank.h"
#include "Graphics/SpriteBatch.h"
#include "Graphics/Texture.h"
#include "Platform/Window.h"
#include "UI/Controls.h"
#include "UI/UIContext.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dungeon::game {

class GameUI {
public:
	GameUI(Window& window, gfx::GraphicsDevice& device,
		   gfx::SpriteBatch& spriteBatch, audio::AudioEngine& audio,
		   const SoundBank& sounds, GameSettings& settings,
		   std::vector<Character>& characters);

	// --- building ---------------------------------------------------------------
	void BuildStaticUi(); // theme + landing menu, pause menu, character sheet
	void BuildHud();      // party bar, log, status/options panels (load task)
	void LoadTitleArt();  // landing-page background (boot load task)
	// Rebuilds every page in the (just reloaded) active language; the HUD too
	// when it exists, which clears the message log. Never call from inside a
	// widget callback — the widget would die under its own Update. Game defers
	// the language change to the top of the next frame instead.
	void RebuildForLanguage();

	// --- per-frame updates (which page runs is the app state's call) ------------
	// Keeps fonts in step with the window height so text scales with the
	// normalized UI; re-bakes are debounced until a resize settles.
	void UpdateFonts(float dt);
	void UpdateMenu(const Input& input);  // landing list or settings page
	void UpdatePause(const Input& input); // pause list or settings page
	void UpdateSheet(const Input& input);
	void UpdateHud(const Input& input);
	// Reformats the HUD compass/position labels when the values change
	// (per-frame string formatting is needless heap churn).
	void SetHudStatus(int facing, int gridX, int gridZ);
	void ResetHudStatus(); // forces the next SetHudStatus to reformat

	// Esc handling support: leaves the settings page if it is open (returns
	// true); false means the caller owns the Esc (quit / resume).
	bool CloseSettingsPage();
	void ResetToMainPage();
	// True while a Settings key-bind box is armed ("press a key...") — Esc
	// then cancels the capture instead of leaving the settings page.
	bool KeyCaptureActive() const;

	// --- character sheet ---------------------------------------------------------
	void ShowSheet(size_t index); // re-points the sheet at the member
	void RefreshSheet();          // re-caches after the roster resets in place

	// --- message log ---------------------------------------------------------------
	void AddLogLine(const std::string& line);
	void ClearLog();

	// --- rendering (inside the caller's SpriteBatch Begin/End) -------------------
	void RenderLoadingScreen(const LoadQueue& queue);     // boot: black + title
	void RenderGameLoadingScreen(const LoadQueue& queue); // title art + progress
	void RenderMenuOverlay();
	void RenderPauseOverlay(); // dark wash + pause menu over the frozen scene
	void RenderCharacterSheetOverlay(); // dark wash + the details page
	void RenderHud();

	// --- callbacks into the app state machine -------------------------------------
	std::function<void()> onStartNewGame;       // landing "Start New Game"
	std::function<void()> onQuit;               // pause "Exit"
	std::function<void()> onResume;             // pause/sheet "Back"
	std::function<void(size_t)> onOpenSheet;    // portrait click, prev/next
	std::function<void(int)> onQualitySelected; // Video tab dropdown
	std::function<void(int)> onTorchPalette;    // HUD torchlight dropdown
	std::function<void(MoveAction)> onMoveAction; // HUD movement buttons
	std::function<void()> onKeysChanged;        // a movement key was rebound
	// Game tab language dropdown. The receiver must NOT rebuild the UI from
	// inside the callback (see RebuildForLanguage) — record and defer.
	std::function<void(const std::string&)> onLanguageSelected;

private:
	enum class MenuPage { Main, Settings };

	void BuildMenu(); // landing list + the shared settings page
	void BuildPauseMenu();
	void BuildCharacterSheet();
	// Pushes the settings theme into every UIContext (each owns a copy).
	void ApplyTheme();
	// Re-derives the party-bar slot rects from the settings scale and shifts
	// the widgets beneath the bar to match; no-op until BuildHud has run.
	void ApplyPartyBarScale();
	void DrawLoadProgress(const LoadQueue& queue, float barY); // shared bar
	void Click(float volume = 0.5f); // UI click feedback

	Window& m_window;
	gfx::GraphicsDevice& m_device;
	gfx::SpriteBatch& m_spriteBatch;
	audio::AudioEngine& m_audio;
	const SoundBank& m_sounds;
	GameSettings& m_settings;
	std::vector<Character>& m_characters;

	ui::UIContext m_hudUi;      // in-game HUD (17px font)
	ui::UIContext m_menuUi;     // landing page (28px font)
	ui::UIContext m_settingsUi; // settings page (28px font, shared by pause)
	ui::UIContext m_pauseUi;    // pause menu (28px font)
	ui::UIContext m_sheetUi;    // character sheet (22px font)
	ui::Font m_titleFont;       // big face for "DUNGEON" titles
	std::unique_ptr<gfx::Texture> m_titleBackground; // landing-page art

	MenuPage m_menuPage = MenuPage::Main;

	// Widgets the game updates later; the UIContexts own them.
	ui::TextOutput* m_log = nullptr;
	ui::Label* m_compass = nullptr;
	ui::Label* m_position = nullptr;
	CharacterSheet* m_sheet = nullptr;
	size_t m_sheetIndex = 0; // member shown by the character sheet

	// The Game tab's key-bind rows, parallel to kKeyFields — kept so a
	// rebind can swap a duplicate key out of its old row.
	std::vector<ui::KeyBind*> m_keyBinds;

	// Installed languages (assets/lang scan), in the Game tab dropdown's
	// order; maps the selection index back to a language code.
	std::vector<loc::LanguageInfo> m_languages;

	// Last torchlight dropdown selection, so a HUD rebuild (language change)
	// recreates the dropdown showing the palette that is actually active.
	int m_torchPalette = 0;

	// Party-bar slots and the widgets authored beneath the bar with their
	// design-pixel Y at scale 1; ApplyPartyBarScale resizes the slots and
	// shifts the rest so they track the bar's bottom edge.
	std::vector<CharacterPanel*> m_partyPanels; // owned by m_hudUi
	std::vector<std::pair<ui::Widget*, float>> m_belowBarWidgets;
	float m_hudDesignW = 0.0f, m_hudDesignH = 0.0f; // window size at BuildHud

	// Font re-bake debounce: last seen window height and how long it has
	// held (fonts re-bake once it settles — see UpdateFonts).
	float m_fontWindowH = 0.0f;
	float m_fontSettle = 0.0f;

	// Last values shown in the HUD labels (reformat only on change).
	int m_lastFacing = -1;
	int m_lastGridX = -1;
	int m_lastGridZ = -1;
};

} // namespace dungeon::game
