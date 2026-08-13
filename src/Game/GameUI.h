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
#include "UI/Layout.h" // ui::Stack — the settings page's rows
#include "UI/Skin.h"
#include "UI/UIContext.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dungeon::game {

class GameUI {
public:
	GameUI(Window& window, gfx::GraphicsDevice& device,
		   gfx::SpriteBatch& spriteBatch, audio::AudioEngine& audio,
		   const SoundBank& sounds, GameSettings& settings,
		   std::vector<Character>& characters, ui::FontLibrary& fonts);

	// --- building ---------------------------------------------------------------
	void BuildStaticUi(); // theme + landing menu, pause menu, character sheet
	void BuildHud();      // party bar, log, status/options panels (load task)
	void LoadTitleArt();  // landing-page background (boot load task)
	// Rebuilds every page in the (just reloaded) active language; the HUD too
	// when it exists, which clears the message log. Never call from inside a
	// widget callback — the widget would die under its own Update. Game defers
	// the language change to the top of the next frame instead.
	void RebuildForLanguage();
	// Call after the roster CHANGES SIZE (party creation builds 1..4 members):
	// re-clamps the sheet member and rebuilds the per-member HUD widgets
	// (party panels, hand slots, name labels) for the new count. The widgets'
	// per-frame (roster, index) resolution already keeps a stale slot inert,
	// so this is layout, not safety. Same deferral rule as RebuildForLanguage:
	// never call from inside a widget callback.
	void RebuildForRoster();
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
	// Convenience: pull facing/grid straight from the party (the usual caller).
	void SetHudStatus(const Party& party);
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

	// Points the party bar at the hit-feedback splat icons (owned by Game,
	// loaded from assets). The struct address must stay stable; the panels read
	// it live, so it can be set before the textures finish loading.
	void SetHitSplats(const HitSplatIcons* splats) { m_hitSplats = splats; }

	// Item icons (catalog id → texture), owned by Game; used to draw the held
	// cursor + (later) hand/inventory slots. Stable address; set once.
	void SetItemIcons(const ItemIconBank* icons) { m_itemIcons = icons; }
	// Item carry weights (catalog id → kg), owned by Game; the sheet sums them
	// into a member's carry load. Stable address; set once.
	void SetItemWeights(const ItemWeightBank* weights) { m_itemWeights = weights; }
	// Equipment-slot outline silhouettes (slot type → texture), owned by Game; the
	// sheet draws them behind empty doll slots. Stable address; set once.
	void SetSlotIcons(const ItemIconBank* icons) { m_slotIcons = icons; }
	// Item categories (catalog id → category), owned by Game; the sheet uses it to
	// tell whether a held item is a pack (container). Stable address; set once.
	void SetItemCategories(const ItemCategoryBank* cats) { m_itemCategories = cats; }
	// The cursor-carried item (Game's m_heldItem). RenderHud paints its icon at
	// the mouse, and the held-aware portrait/hand handlers place INTO and pick
	// OUT OF it, so the pointer is mutable. Address stable; value read/written live.
	void SetHeldItem(std::optional<std::string>* held) { m_held = held; }
	// True if a HUD widget consumed the mouse this frame (so the world should not
	// also treat the click as a pick/drop). Valid after UpdateHud.
	bool HudMouseConsumed() const { return m_hudUi.IsMouseConsumed(); }

	// Party inventory window (right-click a portrait). Non-modal; Game drives
	// open/close (and routes Esc to close it before the pause menu). The world
	// view's right button is mouse-look, so it no longer opens this.
	void OpenInventory();
	void CloseInventory();
	bool InventoryOpen() const;

	// --- character sheet ---------------------------------------------------------
	void ShowSheet(size_t index); // re-points the sheet at the member
	void RefreshSheet();          // re-caches after the roster resets in place

	// --- message log ---------------------------------------------------------------
	void AddLogLine(const std::string& line);
	// A line ABOUT a party member, tinted with their identity color (the
	// portrait/hand-stripe color, brightened to read as text ink on the dark
	// footer). Casting, learning, eating, being struck — anything personal.
	void AddLogLine(const std::string& line, const Vec4& memberColor);
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
	std::function<void()> onQuit;               // landing + pause "Exit" (the ONLY
												// click that quits — Esc does not)
	std::function<void()> onResume;             // pause/sheet "Back"
	// A save slot was chosen to load (landing Continue/Load, pause Load). The
	// receiver loads it — from the landing page that may first stage the
	// dungeon load. Argument is the full .dsav path.
	std::function<void(const std::string&)> onLoadSave;
	// The player named and confirmed a save (pause Save). Argument is the
	// display name; the receiver writes it and resumes play.
	std::function<void(const std::string&)> onSaveSlot;
	std::function<void(size_t)> onOpenSheet;    // portrait click, prev/next
	std::function<void()> onShowPartyInventory; // sheet "All" -> combined backpacks
	std::function<void(int)> onQualitySelected; // Video tab quality dropdown
	std::function<void(int)> onFrameLimitSelected; // Video tab frame-rate dropdown
	std::function<void(int)> onTorchPalette;    // HUD torchlight dropdown
	std::function<void(MoveAction)> onMoveAction; // HUD movement buttons
	// The offense/defense stance slider under a member's hands: (member,
	// share). The widget reports where it was dragged; Game owns the roster
	// and does the writing.
	std::function<void(size_t, float)> onGuardChange;
	// The character sheet's defense breakdown, sourced from the world by the
	// owner — the sheet cannot resolve worn items or balance knobs itself.
	std::function<DefenseReadout(const Character&)> defenseFor;
	std::function<DefenseReadout(const Character&, const std::string&)> defenseWith;
	// HUD hand-slot click (member, hand 0=L/1=R, melee verb — the executed
	// command id, e.g. "stab" = the ATTACK, Balance::FindAttack).
	std::function<void(size_t, size_t, const std::string&)> onHandAttack;
	// The hand right-click menu's command list for an item id (ItemKind::commands),
	// wired by Game to the world's item kinds — keeps the command source single.
	std::function<std::vector<std::string>(const std::string&)> itemCommands;
	// The project's whole spell registry (wired to DungeonWorld::SpellDefs);
	// the hand-slot Magic submenu filters it by the member's known symbols.
	std::function<std::span<const std::unique_ptr<Spell>>()> spellDefs;
	// Member `i` casts the spell with this catalog id (a "cast:<id>" hand
	// default) from hand `hand` (0 = L, 1 = R) — wired to DungeonWorld::
	// CastSpellById (vocab/mana gates; the firing hand's MRU is credited).
	std::function<void(size_t, const std::string&, size_t)> onCastSpell;
	// Member `i` casts a symbol sequence BUILT in the spellbook panel (the
	// Magic area's member selector picks whose book) — wired to DungeonWorld::
	// CastSpell (exact-recipe match; a miss fizzles). The hand argument is
	// kBookHands: a book cast credits both hands' quick-cast MRU.
	std::function<void(size_t, size_t, const std::vector<SpellSymbol>&)>
		onCastSequence;
	std::function<void()> onKeysChanged;        // a movement key was rebound
	std::function<void()> onLookChanged;        // a mouse-look knob changed (push to Party)
	std::function<void()> onHeadBobChanged;     // the head-bob checkbox (push to Party)
	// Game tab language dropdown. The receiver must NOT rebuild the UI from
	// inside the callback (see RebuildForLanguage) — record and defer.
	std::function<void(const std::string&)> onLanguageSelected;
	// Video tab Apply with only monitor/resolution/mode changed: apply in place.
	std::function<void()> onVideoApply;
	// Video tab Apply with the adapter changed (confirmed): persist + relaunch.
	std::function<void()> onAdapterRestart;

	// One of this UI's widget trees, by name, for the dev console's `uitree
	// dump` (dev-facing, so the names stay English). Null for an unknown name;
	// UiTreeNames lists what is accepted.
	ui::UIContext* UiTree(std::string_view name);
	static std::string UiTreeNames();

private:
	enum class MenuPage { Main, Settings, Saves };
	// The Saves sub-page serves two jobs: Load (a list of slots to load) and
	// Save (a name field + existing slots to overwrite). m_savesMode picks.
	enum class SavesMode { Load, Save };

	void BuildMenu();     // landing list (then BuildSettings for the shared page)
	void BuildMenuList(); // just the landing list — rebuilt when saves change
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
	// Flags everything that depends on WHICH saves exist. Call from any path
	// that writes or removes one.
	void MarkSavesChanged();
	// Re-filters the landing and pause lists (Continue / Load appear only with
	// a save) once the saves have changed. Deferred like RefreshSavesIfDirty.
	void RefreshMenuEntriesIfDirty();
	// Pushes the settings theme into every UIContext (each owns a copy).
	void ApplyTheme();
	// Pushes the skin (or null, per settings.uiSkin) into every UIContext.
	// Live — widgets re-check the pointer each draw, no rebuild needed.
	void ApplySkin();
	// Re-derives the party-bar slot rects from the settings scale and shifts
	// the widgets beneath the bar to match; no-op until BuildHud has run.
	void ApplyPartyBarScale();
	void DrawLoadProgress(const LoadQueue& queue, float barY); // shared bar
	// Title face centered horizontally at y (accent color); returns y so a
	// subtitle can be placed relative to it. Shared by every title screen.
	void DrawCenteredTitle(const std::string& text, float y);
	void Click(float volume = 0.5f); // UI click feedback
	void DrawHeldCursor();           // the cursor-carried item icon (HUD + sheet)

	// --- held-item placement (the cursor carries one tablet at a time) ----------
	// A left-click landed on member `i`'s portrait: when holding a tablet, drop
	// it into that member's first free backpack slot (else open the sheet).
	void OnPortraitClick(size_t i);
	// A right-click on member `i`'s portrait: opens the inventory focused on that
	// member (so a carried tablet can be dropped into their backpack).
	void OnPortraitRightClick(size_t i);
	// A click (either button) on member `i`'s stat bars: opens their sheet on the
	// Stats tab.
	void OnPortraitBars(size_t i);
	// A left-click on one of member `i`'s status-effect icons (the panel's
	// name-band row): opens their sheet on the Effects tab.
	void OnPortraitEffects(size_t i);
	// A left-click landed on member `i`'s hand `hand`. Carrying a holdable item
	// on the cursor places it there (swapping any occupant onto the cursor; a
	// non-holdable item is refused with a log line). Empty-cursor, the control-
	// bar hand is an ACTION button: it executes the hand's default use (the
	// remembered per-item-type pick, else the item's first defaultable command;
	// an empty hand throws the unarmed punch). Picking an item OUT of a hand is
	// the character sheet's job (its hand cells keep pick/swap semantics).
	void OnHandLeftClick(size_t i, size_t hand);
	// A right-click on member `i`'s hand `hand`: opens the USE menu (see
	// OpenHandUseMenu). A left-click on a hand with NO default yet opens the
	// same menu, so the first click picks what future clicks will do.
	void OnHandRightClick(size_t i, size_t hand);
	// The hand's USE menu: the item's data-driven command entries, and — when
	// the hand has no defaultable item command (bare hand, rune, key) — the
	// grouped default pickers: Combat > Punch/Kick and Magic > the member's
	// known spells (each submenu chains through the same ContextMenu).
	// Selecting an entry records it as the member's default for that item type
	// ("unarmed" for a bare hand) and, per GameSettings::useMenuExecutes,
	// performs it.
	void OpenHandUseMenu(size_t i, size_t hand);
	// A use-menu entry was picked: record it as the default (menu-only commands
	// like memorize are never recorded) and execute per the Controls setting.
	void SelectUse(size_t i, size_t hand, const std::string& itemId,
				   const std::string& cmd);
	// Performs one use command on member `i`'s hand `hand` (the dispatch behind
	// both the left-click default and the menu): eat/memorize map to their
	// handlers, the melee verbs to onHandAttack. Unknown/empty ids no-op.
	void ExecuteUse(size_t i, size_t hand, const std::string& cmd);
	// The command a left-click on `itemId` ("" = bare hand) in hand `hand`
	// executes for this member: THAT hand's remembered useDefaults pick while
	// it is still valid, else the item's first defaultable (non-menu-only)
	// command, else "" — no default, so the left-click opens the use menu to
	// pick one.
	std::string DefaultUseFor(const Character& c, size_t hand,
							  const std::string& itemId) const;
	// Whether a remembered default is still usable: an item command the item
	// still offers, one of the bare-hand combat verbs, or a "cast:<id>" whose
	// spell exists and whose symbols the member all knows.
	bool UseValidFor(const Character& c, const std::vector<std::string>& cmds,
					 const std::string& cmd) const;
	// Commits the rune in member `i`'s hand to memory: the symbol is learned and
	// the tablet consumed.
	void MemorizeFromHand(size_t i, size_t hand);
	// The shared memorize: learns `slot`'s rune symbol and consumes the tablet
	// — a rune memorizes from WHEREVER it sits (hand or backpack).
	void MemorizeSlot(size_t i, ItemSlot& slot);
	// Right-clicked backpack slot `slot` on the open sheet: pop the item's use
	// menu in the SHEET's context (a rune offers Memorize).
	void OpenPackUseMenu(int slot);
	// Eats the food in member `i`'s hand: restores some stamina, consumes it.
	void EatFromHand(size_t i, size_t hand);
	bool Holding() const { return m_held && m_held->has_value(); }

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
	ui::FontLibrary& m_fonts; // owned by Game; outlives every context here
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
	// Big face for the "DUNGEON" titles, which GameUI draws itself (outside the
	// widget tree). Borrowed from the library at the Display role, re-resolved
	// every UpdateFonts. Nothing else holds it: the sheet and the party panel
	// used to be handed this pointer and now resolve their own heading font
	// through UIContext::FontAt.
	const ui::Font* m_titleFont = nullptr;
	std::unique_ptr<gfx::Texture> m_titleBackground; // landing-page art
	std::unique_ptr<gfx::Texture> m_deleteIcon;      // red X for the save browser
	// Textured-chrome skin (UI/Skin.h): the part textures + the Skin handed to
	// every context by ApplySkin (null when settings.uiSkin is off — the flat
	// debug look). Textures are optional; missing parts stay flat.
	std::unique_ptr<gfx::Texture> m_skinPanelTex;
	std::unique_ptr<gfx::Texture> m_skinButtonTex;
	std::unique_ptr<gfx::Texture> m_skinSlotTex;
	ui::Skin m_skin;
	// The spellbook's Cast/Clear round icon faces (optional).
	std::unique_ptr<gfx::Texture> m_castIconTex;
	std::unique_ptr<gfx::Texture> m_clearIconTex;
	// The movement pad's chevron icon faces (single = step, double = turn).
	std::unique_ptr<gfx::Texture> m_chevronTex;
	std::unique_ptr<gfx::Texture> m_chevron2Tex;
	const gfx::Texture* m_closeIcon = nullptr; // shared, owned by AssetUtil

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
	// Ditto for the landing/pause lists, which hide Continue and Load when no
	// save exists. Cleared separately from m_savesDirty because the two catch
	// up at different moments: the browser while it is open, these once the
	// player is back on the list page.
	bool m_menuEntriesDirty = false;
	// Whether those lists were built WITH the save-only entries, so a rebuild
	// is skipped when deleting one of several saves changes nothing.
	bool m_menuHasSaves = false;

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

	// HUD right-click context menu (hand-slot item actions, e.g. Memorize).
	// Reused: GameUI opens it with the actions for whatever was right-clicked.
	ui::ContextMenu* m_handMenu = nullptr;
	// The SHEET's own context menu (backpack-slot actions — the sheet freezes
	// the HUD, so m_handMenu can't serve it).
	ui::ContextMenu* m_sheetMenu = nullptr;
	// Party inventory window (owned by m_hudUi); opened on right-click-while-holding.
	InventoryWindow* m_inventory = nullptr;
	// The Magic-area spellbook (owned by m_hudUi): opened from a hand's use
	// menu (Magic » Spellbook), where a member builds a symbol sequence.
	SpellbookPanel* m_spellbook = nullptr;

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

	// The party bar (owns the slots) and the container holding everything under
	// it; ApplyPartyBarScale resizes the one and slides the other, and the
	// trees carry their contents. Both are owned by m_hudUi.
	PartyBar* m_partyBar = nullptr;
	ui::Widget* m_belowBar = nullptr;
	std::vector<CharacterPanel*> m_partyPanels; // owned by m_partyBar
	const HitSplatIcons* m_hitSplats = nullptr; // hit-feedback icons (Game-owned)
	const ItemIconBank* m_itemIcons = nullptr;  // item icons (Game-owned)
	const ItemWeightBank* m_itemWeights = nullptr; // item carry weights (Game-owned)
	const ItemIconBank* m_slotIcons = nullptr;  // equipment-slot outlines (Game-owned)
	const ItemCategoryBank* m_itemCategories = nullptr; // item categories (Game-owned)
	// Cursor-carried item (Game owns the storage; placement handlers mutate it)
	// + the last HUD mouse position (stashed in UpdateHud so RenderHud can draw
	// the held icon, which has no Input).
	std::optional<std::string>* m_held = nullptr;
	float m_hudMouseX = 0.0f, m_hudMouseY = 0.0f;

	// Font re-bake debounce: last seen window height and how long it has
	// held (fonts re-bake once it settles — see UpdateFonts).
	float m_fontWindowH = 0.0f;
	float m_fontSettle = 0.0f;
	// The settled window/design height ratio. Held rather than recomputed per
	// frame precisely BECAUSE it must not move every frame: UpdateFonts asks the
	// library for a font at this scale every frame, and a size that changed
	// continuously would mint a new atlas each time (UI/FontLibrary.h).
	float m_fontScale = 1.0f;

	// Last values shown in the HUD labels (reformat only on change).
	int m_lastFacing = -1;
	int m_lastGridX = -1;
	int m_lastGridZ = -1;
};

} // namespace dungeon::game
