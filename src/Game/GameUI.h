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
#include "Game/MessageLog.h"
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
	// Re-point the Video tab's Max Lights dropdown at the current setting after
	// a quality change reset the budget (Game calls this from SetQuality).
	void SyncMaxLights();
	// Rebuilds the settings page if a Video-tab adapter/monitor change staged one
	// last frame (rebuilding from inside the dropdown callback would destroy it).
	// Game calls this at the top of Update, like the deferred language switch.
	void ApplyPendingVideoRebuild();

	// --- per-frame updates (which page runs is the app state's call) ------------
	// Keeps fonts in step with the window height so text scales with the
	// normalized UI; re-bakes are debounced until a resize settles.
	void UpdateFonts(float dt);
	void UpdateMenu(const Input& input);  // landing list or settings page
	void UpdatePause(const Input& input); // pause list or settings page
	void UpdateSheet(const Input& input);
	// dt advances the message footer's fades / expand animation (real frame
	// time, not world time).
	void UpdateHud(const Input& input, float dt);
	// Reformats the HUD compass/position labels when the values change
	// (per-frame string formatting is needless heap churn).
	void SetHudStatus(int facing, int gridX, int gridZ);
	void ResetHudStatus(); // forces the next SetHudStatus to reformat

	// Esc handling support: leaves the settings page if it is open (returns
	// true); false means the caller owns the Esc (quit / resume).
	bool CloseSettingsPage();
	void ResetToMainPage();
	// Rebuilds the pause list from current state so the Load entry tracks
	// whether a save exists (saves come and go during play). Call before
	// opening the pause menu.
	void RebuildPauseMenu();
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
	void RenderConfirmOverlay();        // dark wash + the Yes/No restart modal
	void RenderHud();

	// --- callbacks into the app state machine -------------------------------------
	std::function<void()> onStartNewGame;       // landing "Start New Game"
	std::function<void()> onQuit;               // pause "Exit"
	std::function<void()> onResume;             // pause/sheet "Back"
	// A save slot was chosen to load (landing Continue/Load, pause Load). The
	// receiver loads it — from the landing page that may first stage the
	// dungeon load. Argument is the full .dsav path.
	std::function<void(const std::string&)> onLoadSave;
	// The player named and confirmed a save (pause Save). Argument is the
	// display name; the receiver writes it and resumes play.
	std::function<void(const std::string&)> onSaveSlot;
	std::function<void(size_t)> onOpenSheet;    // portrait click, prev/next
	std::function<void(int)> onQualitySelected; // Video tab quality dropdown
	std::function<void(int)> onFrameLimitSelected; // Video tab frame-rate dropdown
	std::function<void(int)> onTorchPalette;    // HUD torchlight dropdown
	std::function<void(MoveAction)> onMoveAction; // HUD movement buttons
	std::function<void()> onKeysChanged;        // a movement key was rebound
	// Game tab language dropdown. The receiver must NOT rebuild the UI from
	// inside the callback (see RebuildForLanguage) — record and defer.
	std::function<void(const std::string&)> onLanguageSelected;
	// Video tab Apply with only monitor/resolution/mode changed: apply in place.
	std::function<void()> onVideoApply;
	// Video tab Apply with the adapter changed (confirmed): persist + relaunch.
	std::function<void()> onAdapterRestart;

private:
	enum class MenuPage { Main, Settings, Saves };
	// The Saves sub-page serves two jobs: Load (a list of slots to load) and
	// Save (a name field + existing slots to overwrite). m_savesMode picks.
	enum class SavesMode { Load, Save };

	void BuildMenu();     // landing list (then BuildSettings for the shared page)
	void BuildSettings(); // the tabbed settings page (Game/Controls/Video/Audio/UI)
	void BuildPauseMenu();
	void BuildCharacterSheet();
	// Video tab: seed the staged adapter/monitor/resolution/mode selection from
	// the live settings + enumerated hardware (call when opening/rebuilding the
	// page for a fresh edit, not on the deferred repopulate).
	void SeedVideoStaging();
	// Commit the staged Video selection: in-place for monitor/res/mode, or open
	// the restart-confirm dialog when the adapter changed.
	void OnVideoApply();
	// Builds the centered Yes/No modal (m_confirmUi) and arms it.
	void OpenConfirm(const std::string& title, const std::string& body,
					 std::function<void()> onYes);
	// Rebuilds the (dynamic) save-slot browser from the files on disk and
	// switches to the Saves page in the given mode. Shared by the landing/pause
	// Load entries and the pause Save entry; widgets live in m_savesUi.
	void OpenSavesPage(SavesMode mode);
	// Save page helpers: commit the named save (arming an overwrite confirm
	// first if the name collides), and clear that armed confirm.
	void CommitSave();
	void DisarmOverwrite();
	// Rebuilds the Saves page if a deletion flagged it dirty (deferred so the
	// SlotList isn't cleared from inside its own row callback).
	void RefreshSavesIfDirty();
	// Pushes the settings theme into every UIContext (each owns a copy).
	void ApplyTheme();
	// Re-derives the party-bar slot rects from the settings scale and shifts
	// the widgets beneath the bar to match; no-op until BuildHud has run.
	void ApplyPartyBarScale();
	void DrawLoadProgress(const LoadQueue& queue, float barY); // shared bar
	// Title face centered horizontally at y (accent color); returns y so a
	// subtitle can be placed relative to it. Shared by every title screen.
	void DrawCenteredTitle(const std::string& text, float y);
	void Click(float volume = 0.5f); // UI click feedback

	// Live window/device dimensions as floats (the UI authors in floats and
	// the window and back buffer track the same size).
	float WindowW() const { return static_cast<float>(m_window.Width()); }
	float WindowH() const { return static_cast<float>(m_window.Height()); }
	float DeviceW() const { return static_cast<float>(m_device.Width()); }
	float DeviceH() const { return static_cast<float>(m_device.Height()); }
	// The menu/pause flows share the Settings and Saves sub-pages; the active
	// context depends on which sub-page (if any) is open over the list.
	ui::UIContext& MenuContext() {
		switch (m_menuPage) {
		case MenuPage::Settings: return m_settingsUi;
		case MenuPage::Saves:    return m_savesUi;
		default:                 return m_menuUi;
		}
	}
	ui::UIContext& PauseContext() {
		switch (m_menuPage) {
		case MenuPage::Settings: return m_settingsUi;
		case MenuPage::Saves:    return m_savesUi;
		default:                 return m_pauseUi;
		}
	}

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
	ui::UIContext m_savesUi;    // save-slot browser (28px font, shared by both)
	ui::UIContext m_sheetUi;    // character sheet (22px font)
	ui::UIContext m_confirmUi;  // modal Yes/No (adapter-change restart confirm)
	ui::Font m_titleFont;       // big face for "DUNGEON" titles
	std::unique_ptr<gfx::Texture> m_titleBackground; // landing-page art
	std::unique_ptr<gfx::Texture> m_deleteIcon;      // red X for the save browser

	MenuPage m_menuPage = MenuPage::Main;
	SavesMode m_savesMode = SavesMode::Load;
	// Save page widgets (live in m_savesUi, valid only while it is built): the
	// name field and the Save button, plus whether a second click is needed to
	// confirm overwriting an existing slot of the same name.
	ui::TextField* m_saveField = nullptr;
	ui::Button* m_saveButton = nullptr;
	bool m_overwriteArmed = false;
	// A slot was deleted this frame; the Saves page is rebuilt from disk at the
	// top of the next Update (rebuilding inside the row callback would destroy
	// the list mid-iteration).
	bool m_savesDirty = false;

	// Widgets the game updates later; the UIContexts own them.
	MessageLog* m_log = nullptr;
	ui::Label* m_compass = nullptr;
	ui::Label* m_position = nullptr;
	CharacterSheet* m_sheet = nullptr;
	size_t m_sheetIndex = 0; // member shown by the character sheet

	// The Game tab's key-bind rows, parallel to kKeyFields — kept so a
	// rebind can swap a duplicate key out of its old row.
	std::vector<ui::KeyBind*> m_keyBinds;

	// The Video tab's Max Lights dropdown — kept so SyncMaxLights can re-point
	// it when a quality change resets the light budget.
	ui::DropDown* m_maxLightsDrop = nullptr;

	// Video tab: the enumerated hardware (cached for the dropdowns + Apply), the
	// settings TabControl (kept so a repopulate can restore the active tab), and
	// the STAGED selection — held separately from m_settings so the Apply button
	// commits it (and survives the deferred adapter/monitor repopulate).
	std::vector<gfx::AdapterInfo> m_adapters;
	ui::TabControl* m_settingsTabs = nullptr;
	int m_selAdapter = 0;
	int m_selOutput = 0;
	int m_selRes = 0;
	gfx::FullscreenMode m_selMode = gfx::FullscreenMode::Windowed;
	bool m_videoRebuildPending = false; // adapter/monitor changed; rebuild next frame
	bool m_confirmActive = false;       // the restart-confirm modal is up

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
