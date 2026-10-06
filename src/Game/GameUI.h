// ============================================================================
// Game/GameUI.h — every 2D surface the game shows.
//
// Owns the seven UIContexts (the HUD with its floating panels and the party
// window, landing menu, shared settings page, pause menu, the saves context -
// save / load pages, the world list and the party creation page - the
// character sheet, and the Yes/No confirm), the two dialogs that bring their
// own (ItemDetailsDialog, PortraitPicker), the title font and landing art, and
// all the widgets in them. Builds the static pages up front (BuildStaticUi) and the HUD as a load
// task (BuildHud, once the roster's portraits exist); renders the loading
// screens, menu/pause/sheet overlays, and the in-game HUD.
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
#include "Game/ItemDetailsDialog.h"
#include "Game/PortraitPicker.h"
#include "Game/LoadQueue.h"
#include "Game/MessageLog.h"
#include "Game/Party.h"
#include "Game/PartyCreationPage.h"
#include "Game/PartyHud.h"
#include "Game/SoundBank.h"
#include "Graphics/SpriteBatch.h"
#include "Graphics/Texture.h"
#include "Platform/Window.h"
#include "UI/Controls.h"
#include "UI/FloatingPanel.h"
#include "UI/Layout.h" // ui::Stack — the settings page's rows
#include "UI/Skin.h"
#include "UI/UIContext.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace dungeon::game {

class StonePicker; // Game/StonePicker.h - the Material tab's grid
class PageCard;    // Game/MenuPanel.h - the menu pages' stone card
struct SaveSlot;   // Game/SaveGame.h - one listed save

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
	// Advances the resource bars by `dt` REAL seconds - the fills' animation
	// clock (handed to the SpriteBatch) and each member's heartbeat, whose rate
	// follows their health and whether the party is `noticed`. Every frame, in
	// every state, so the bars never stutter on a state change.
	void TickResourceBars(float dt, bool noticed);
	// The UI material the PLACE asks for (Game::RefreshPlaceStone; empty = it
	// asks for none). Applied at once while the Material setting follows the
	// place; remembered either way, so switching back to follow lands on it.
	void SetPlaceStone(std::string name);
	// What the place currently asks for (empty = nothing).
	const std::string& PlaceStone() const { return m_placeStone; }
	// The material showing now (a stem), whoever chose it.
	const std::string& ShownStone() const { return m_shownStone; }
	// A material's thumbnail (assets/ui/stones/thumbs), null for an unknown
	// stem - for the editor dialogs that pick one. Owned here; scans on first ask.
	const gfx::Texture* StoneThumb(std::string_view name);
	// Every material's stem in the Material tab's order - grouped by kind,
	// lightest first within one - so an editor list reads the same way.
	std::vector<std::string> StoneOrder();
	// The category buttons for a material list (ui::DropDown::filterLabels):
	// all, light, dark, then each kind - and which of them a material passes,
	// as that control's bit mask (bit f = shown under button f).
	std::vector<std::string> StoneFilterLabels() const;
	// Each of those buttons' colour chip (ui::DropDown::filterColors).
	std::vector<Vec4> StoneFilterColors() const;
	unsigned StoneFilterBits(std::string_view name);
	// A PREVIEW wins over everything while it is set (the Level dialog's
	// material row: picked = shown at once); EndStonePreview hands the chrome
	// back to whatever the setting and the place decide.
	void PreviewStone(std::string name);
	void EndStonePreview();
	// `uimaterial sweep` (code-review C204): solves every material's inks as a
	// switch to it would (nothing is loaded or shown) and logs one line each -
	// the carved gold's contrast and the weakest etched symbol's, as drawn and in
	// the authored gold. Returns how many materials it logged. Its header also
	// counts the etches measured, plain and lit, so one that failed to load
	// cannot hide behind the others.
	int SweepMaterials();
	// `uimaterial drawn`: logs what DrawCutStone last painted an etch with,
	// plain and lit (ui::LastEtchDrawn, reset whenever the material changes),
	// beside the shown material's solved inks and the authored ones - the
	// sweep reads the inks the draw is meant to use, this what it did use.
	// Returns the console's summary line.
	std::string ReportEtchDrawn() const;
	// Sets the player's Material setting to follow the place (and saves it).
	// The Level dialog's Save calls this (Michael: authoring a level's material
	// while pinned to another showed nothing in play). False = already did.
	bool FollowPlaceStone();
	// The skin the game chrome draws with - for a sample of it inside the
	// editor's own (unskinned) dialogs.
	const ui::Skin& GameSkin() const { return m_skin; }
	void UpdateMenu(const Input& input, float dt); // landing list or a sub-page
	void UpdatePause(const Input& input); // pause list or settings page
	void UpdateSheet(const Input& input, float dt);
	// dt advances the message footer's fades / expand animation (real frame
	// time, not world time).
	void UpdateHud(const Input& input, float dt);
	// Reformats the HUD compass/position labels when the values change
	// (per-frame string formatting is needless heap churn).
	void SetHudStatus(int facing, int gridX, int gridZ);
	// Convenience: pull facing/grid straight from the party (the usual caller).
	void SetHudStatus(const Party& party);
	void ResetHudStatus(); // forces the next SetHudStatus to reformat
	// Re-label the Rest button from the world's live state. Pushed per frame
	// because rest ends by itself as often as by a click.
	void SetResting(bool resting);

	// Esc handling support: leaves the settings page if it is open (returns
	// true); false means the caller owns the Esc (quit / resume).
	bool CloseSettingsPage();
	void ResetToMainPage();
	// Rebuilds the pause list from current state so the Load entry tracks
	// whether a save exists (saves come and go during play). Call before
	// opening the pause menu.
	void RebuildPauseMenu();
	// Back on the title: its main page, with its list REBUILT from the saves on
	// disk (Game::ReturnToTitle). The backstop for Continue / Load, whatever the
	// dirty flags missed on the way (code-review C366). Never from inside the
	// title list's own walk - every caller is a world tick or a pause-menu answer.
	void ShowTitle();
	// Flags everything that depends on WHICH saves exist. Call from any path
	// that writes or removes one (Game::SaveGame does, for every way in).
	void MarkSavesChanged();
	// A save row's Delete, and the console's `deletesave`: removes the file and
	// re-lists the saves next frame; when the file stays, the Save / Load page
	// says so above its list, with the system's error code (SaveNotice), and
	// the reason is logged (code-review C367). True when the save is gone.
	bool DeleteSaveSlot(const std::string& path, const std::string& name);
	// What the Save / Load page says above its list ("" = nothing).
	const std::string& SaveNotice() const { return m_saveNotice; }
	// The title's or the pause menu's entries as they are built, joined with
	// ", " - the console's `title status` (an InGameTest row reads it).
	std::string MenuEntries(bool pause) const;
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
	// Hand-use pictures (verb → texture, ui/use_<verb>.png), owned by Game; an
	// empty HUD hand set to that verb shows it. Stable address; set once, before
	// any HUD is built.
	void SetUseIcons(const ItemIconBank* icons) { m_useIcons = icons; }
	// Item categories (catalog id → category), owned by Game; the sheet uses it to
	// tell whether a held item is a pack (container). Stable address; set once.
	void SetItemCategories(const ItemCategoryBank* cats) { m_itemCategories = cats; }
	// The cursor-carried item (Game's m_heldItem). RenderHud paints its icon at
	// the mouse, and the held-aware portrait/hand handlers place INTO and pick
	// OUT OF it, so the pointer is mutable. Address stable; value read/written live.
	void SetHeldItem(HeldItem* held) { m_held = held; }
	// True if a HUD widget consumed the mouse this frame (so the world should not
	// also treat the click as a pick/drop). Valid after UpdateHud.
	// The item details dialog holds the pointer while it is up, so it counts.
	bool HudMouseConsumed() const {
		return m_hudUi.IsMouseConsumed() || ItemDetailsOpen() || PortraitPickerOpen();
	}

	// --- the floating HUD panels (ui-panels P3a) -------------------------------
	// Every panel back to its default spot and size (Settings -> UI "Reset HUD
	// layout", dev `hudpanel reset`); opacity stays.
	void ResetHudLayout();
	// The HUD layout: 0 Standard, 1 Minimal (party cards). Rebuilds the HUD.
	void SetHudLayout(int layout);
	// The pointer shape the grips of panels [first, last) want this frame.
	Window::Cursor PanelCursor(size_t first, size_t last) const;
	// What this frame's HUD / sheet update asked the pointer to be, handed back
	// once and reset to the arrow - so a state that ran neither (paused, a menu)
	// asks for nothing. Game sets the window's cursor in ONE place each frame
	// (beside the editor's dock-edge arrow) and passes this in.
	Window::Cursor TakeHudCursor() {
		const Window::Cursor wanted = m_hudCursor;
		m_hudCursor = Window::Cursor::Arrow;
		return wanted;
	}
	// A panel by kHudPanelFields index, for the `hudpanel` dev command (null
	// before the first game load builds the HUD).
	const ui::FloatingPanel* HudPanel(size_t index) const {
		return index < m_hudPanels.size() ? m_hudPanels[index] : nullptr;
	}
	// How many times a panel was minimized by a click, and restored from the
	// tray, since launch - AllocTest -Panels' evidence its clicks landed.
	unsigned PanelMinimizes() const { return m_panelMinimizes; }
	unsigned PanelRestores() const { return m_panelRestores; }

	// --- the item details dialog (docs/ui-updates-plan.md P3) ---------------------
	// Over the HUD or the sheet, wherever the right-click landed. While it is up
	// UpdateSheet / UpdateHud update IT instead of their own pages (it is modal
	// for the mouse); Game routes Esc to CloseItemDetails first, and renders its
	// 3D preview (DetailsDialog()->PreviewSubs) into the pane.
	// `typeId` weighing `weightKg`: the one opener every right-click goes through
	// (a floor item has no inventory place, so it comes straight here).
	void ShowItemDetails(const std::string& typeId, float weightKg);
	bool ItemDetailsOpen() const { return m_itemDetails && m_itemDetails->IsOpen(); }
	void CloseItemDetails() {
		if (m_itemDetails) m_itemDetails->Close();
	}
	// Esc's first job over the HUD or the sheet: close whatever popup is up (the
	// details dialog, else an open use menu). True if it closed something - then
	// the Esc is spent. Without it an Esc over an open use menu closed the SHEET
	// and left the menu open, waiting to eat the next click when it reopened.
	bool DismissPopup();
	void RenderItemDetails();
	// (Not `ItemDetails()`: inside the class that name would hide the struct.)
	ItemDetailsDialog* DetailsDialog() { return m_itemDetails.get(); }
	// The harness's way in (`itemdetails pack` / `memorize`): the dialog on
	// member `i`'s pack slot, exactly as a right-click there opens it, and its
	// Memorize button pressed. False when the button is not up.
	void OpenPackItemDetails(size_t i, int slot) {
		OpenItemDetails(i, {ItemPlace::Kind::Pack, slot});
	}
	bool PressDetailsMemorize() {
		if (!ItemDetailsOpen() || !m_itemDetails->MemorizeShown()) return false;
		MemorizeFromDetails();
		return true;
	}

	// --- the portrait picker (docs/portraits-plan.md, phase 3) -----------------
	// Handled like the details dialog: modal for the mouse, updated instead of
	// the page under it, closed first by DismissPopup. Opened for a roster
	// member, whose pick goes to onSetPortrait. The OPENER must also excuse the
	// frame (Game::OpenPortraitPicker does), since opening allocates.
	void OpenPortraitPicker(size_t member);
	bool PortraitPickerOpen() const { return m_portraitPicker && m_portraitPicker->IsOpen(); }
	void ClosePortraitPicker() {
		if (m_portraitPicker) m_portraitPicker->Close();
	}
	void RenderPortraitPicker();
	PortraitPicker* Portraits() { return m_portraitPicker.get(); }
	// A pick: (member, portraits.cat id). Game::SetPortrait.
	std::function<bool(size_t, const std::string&)> onSetPortrait;
	// The sheet's "Change portrait" button, for the shown member. Game opens the
	// picker through Game::OpenPortraitPicker, which also excuses the frame.
	std::function<void(size_t)> onChangePortrait;

	// The party window (the sheet's "All"; Game/PartyWindow.h): every member's
	// card on one tab. Non-modal; Game drives open/close (and routes Esc to
	// close it before the pause menu). Opens on `mode`, the tab the sheet was
	// showing.
	void OpenInventory(CharacterSheet::Mode mode = CharacterSheet::Mode::Inventory);
	void CloseInventory();
	bool InventoryOpen() const;
	// What it shows, for the `inventory status` readout: its tab, its status
	// line, and where member `member`'s pack slot `slot` is (false = no card).
	CharacterSheet::Mode InventoryMode() const;
	std::string_view InventoryStatusName() const;
	std::string_view InventoryStatusText() const;
	bool InventorySlotRect(size_t member, int slot, gfx::Rect& out) const;
	// And for AllocTest -All: how often it has opened, where its tab stone `i`
	// is, and where the sheet's "All" button is (empty rects when not laid out).
	unsigned InventoryOpens() const;
	gfx::Rect InventoryStoneRect(size_t i) const;
	gfx::Rect SheetAllRect() const { return m_sheetAll ? m_sheetAll->Pixel() : gfx::Rect{}; }
	// And for AllocTest -Rest: where the log's Rest button is (empty before the
	// HUD is built), so a run can start a rest the way a player does.
	gfx::Rect RestButtonRect();

	// --- character sheet ---------------------------------------------------------
	void ShowSheet(size_t index); // re-points the sheet at the member
	void RefreshSheet();          // re-caches after the roster resets in place
	// What the sheet shows (the `sheet status` readout).
	size_t SheetIndex() const { return m_sheetIndex; }
	CharacterSheet::Mode SheetMode() const {
		return m_sheet ? m_sheet->CurrentMode() : CharacterSheet::Mode::Inventory;
	}
	// The resource bars' live style, for the `hudbars` dev command.
	ResourceBarStyle& BarStyle() { return m_barStyle; }
	// The sheet's status bar this frame (empty = nothing hovered).
	std::string_view SheetStatusName() const {
		return m_sheet ? m_sheet->StatusName() : std::string_view{};
	}
	std::string_view SheetStatusText() const {
		return m_sheet ? m_sheet->StatusText() : std::string_view{};
	}
	unsigned SheetPackEquips() const { return m_sheet ? m_sheet->PackEquips() : 0u; }

	// --- spellbook (the Magic area) ------------------------------------------------
	// Opens member `i`'s book exactly as its selector button does, or refuses
	// (false) where that button is disabled - absent, down, or no symbols. The
	// dev `book` command's path, so a harness can hold a book open.
	bool OpenSpellbook(size_t i);
	void CloseSpellbook();
	SpellbookPanel* Spellbook() { return m_spellbook; } // the dev `book` command

	// --- the hand menu, for a check of it (the dev `handmenu` command) -------------
	// Opens member `i`'s hand `hand` use menu as a right-click on its HUD hand box
	// does, at that box's centre. False when no shown box is that hand's, or the
	// hand has nothing to offer (no menu opens).
	bool OpenHandMenuAtBox(size_t i, size_t hand);
	ui::ContextMenu* HandMenu() { return m_handMenu; }

	// --- message log ---------------------------------------------------------------
	// Borrows the line: it is copied once, into the log's own ring slot, so
	// printing a message allocates nothing (docs/message-allocation.md).
	void AddLogLine(std::string_view line);
	// A line ABOUT a party member, tinted with their identity color (the
	// portrait/hand-stripe color, brightened to read as text ink on the dark
	// footer). Casting, learning, eating, being struck — anything personal.
	void AddLogLine(std::string_view line, const Vec4& memberColor);
	void ClearLog();
	// The newest `n` lines of the log, oldest first (empty before the HUD is
	// built) - the dev `messages` readout; it allocates.
	std::vector<std::string> RecentLogLines(size_t n) const;

	// --- rendering (inside the caller's SpriteBatch Begin/End) -------------------
	void RenderLoadingScreen(const LoadQueue& queue);     // boot: title (+ art once loaded)
	void RenderGameLoadingScreen(const LoadQueue& queue); // title art + progress
	void RenderMenuOverlay();
	void RenderPauseOverlay(); // dark wash + pause menu over what it paused (scene or world map)
	void RenderCharacterSheetOverlay(); // dark wash + the details page
	void RenderConfirmOverlay();        // dark wash + the Yes/No restart modal

	// A Yes/No QUESTION over whatever is on screen — walking onto a doorway on
	// the world map, onto a dungeon's exit (Michael, 2026-09-24). The same modal
	// the adapter restart uses. While PromptActive() the owner freezes what is
	// beneath it and routes input to UpdatePrompt; Enter/Y answer yes, Esc/N no,
	// and either button. The answer's callback runs AFTER the modal's update,
	// never inside it, so a Yes that starts a level load cannot pull the tree
	// out from under the button that fired it.
	void AskYesNo(const std::string& title, const std::string& body,
				  std::function<void()> onYes, std::function<void()> onNo = {});
	bool PromptActive() const { return m_confirmActive; }
	void UpdatePrompt(const Input& input);
	void RenderHud();

	// --- callbacks into the app state machine -------------------------------------
	std::function<void()> onStartNewGame;       // a new game in the RUNNING world
	// "Start New Game" asks WHICH WORLD first when there is more than one to
	// choose from (Michael, 2026-09-24): onListWorlds names them, and a pick
	// goes to onStartNewGameIn - which starts at once in the running world and
	// switches to any other in the process (Game::SwitchWorld), starting there.
	// One world, or none listed, skips the page entirely.
	struct WorldChoice {
		std::string folder;  // what settings.ini stores and -project names
		std::string display; // the manifest's `name`, what the player reads
	};
	std::function<std::vector<WorldChoice>()> onListWorlds;
	std::function<void(const std::string& folder)> onStartNewGameIn;
	// PARTY CREATION (docs/party-creation-plan.md phase 3): Start New Game,
	// once the world is chosen, asks for the party page instead of starting -
	// the receiver opens the world and calls OpenPartyPage with what it offers
	// (empty folder = the resident world, else the default). The page's Start
	// hands its specs to onStartParty, which builds the party and starts the
	// game; false + why when it cannot (the page shows why itself first). The
	// Editor entry and the harness's `newgame` / `reset` skip the page.
	std::function<void(const std::string& folder)> onOpenPartyCreation;
	std::function<bool(const std::vector<party::MemberSpec>&, std::string& why)> onStartParty;
	// Whether a game is in play (not the title screen): the Settings colour rows
	// then edit the party's members, not only the new-member defaults.
	std::function<bool()> partyInPlay;

	// The page itself (GameUI_Party.cpp). OpenPartyPage shows it next frame,
	// fresh (one new member); Back leaves for the world list it came from, or the
	// title. Start goes through StartPartyPage, which the dev command calls too.
	void OpenPartyPage(PartyCreationData data);
	bool PartyPageOpen() const { return m_menuPage == MenuPage::Party; }
	// Open, or asked for and building next frame: its edits apply either way.
	bool PartyPageActive() const { return PartyPageOpen() || m_partyBuildPending; }
	PartyCreationPage* PartyPage() { return m_partyPage.get(); }
	void LeavePartyPage();                  // Back / Esc (deferred a frame)
	bool StartPartyPage(std::string& why);  // Start: false + why if refused
	// The face picker over the page, for one of its members.
	void OpenPartyPortraitPicker(size_t member, const std::string& raceTag);
	// The landing page's "Editor" entry starts a new game exactly as Start New
	// Game does (the same world question), then opens the editor, PAUSED, the
	// moment the game arrives. Every landing entry says which it is, so the
	// request cannot outlive the click that made it: Editor arms it, Start /
	// Continue / Load disarm it.
	std::function<void(bool)> onEditorOnArrival;
	std::function<void()> onQuit;              // landing + pause "Exit" (the ONLY
												// click that quits — Esc does not)
	std::function<void()> onResume;             // pause/sheet "Back"
	std::function<void()> onReturnToMain;       // pause "Return to Main Menu"
	// A save slot was chosen to load (landing Continue/Load, pause Load). The
	// receiver loads it — from the landing page that may first stage the
	// dungeon load. Argument is the full .dsav path.
	std::function<void(const std::string&)> onLoadSave;
	// The player named and confirmed a save (pause Save). Argument is the
	// display name; the receiver writes it and resumes play.
	std::function<void(const std::string&)> onSaveSlot;
	// Which world's saves the menus list (a world folder; "" = every world's) -
	// Game::SaveListWorld, asked on every build so it follows a world switch.
	std::function<std::string()> saveWorld;
	std::function<void(size_t)> onOpenSheet;    // portrait click, prev/next
	std::function<void()> onShowPartyInventory; // sheet "All" -> combined backpacks
	std::function<void(int)> onQualitySelected; // Video tab quality dropdown
	std::function<void(int)> onFrameLimitSelected; // Video tab frame-rate dropdown
	std::function<void(MoveAction)> onMoveAction; // HUD movement buttons
	// The party's Act count and last action (Party::ActCount), for the pad to
	// press the stone a KEY move used. 0 with no world. Must not allocate.
	std::function<unsigned(MoveAction& last)> moveCounter;
	// The offense/defense stance slider under a member's hands: (member,
	// share). The widget reports where it was dragged; Game owns the roster
	// and does the writing.
	std::function<void(size_t, float)> onGuardChange;
	// The party leader (Phase 9): who leads (the world's roster index), and a
	// click on a member's name in the party bar or on their card.
	std::function<int()> partyLeader;
	std::function<void(size_t)> onPickLeader;
	// The live Balance::exertMax, so the slider can show over-exertion as a
	// percentage of the way to it (wired to the world's balance by Game).
	std::function<float()> exertMax;
	// The character sheet's defense breakdown, sourced from the world by the
	// owner — the sheet cannot resolve worn items or balance knobs itself.
	std::function<DefenseReadout(const Character&)> defenseFor;
	std::function<DefenseReadout(const Character&, const std::string&)> defenseWith;
	// HUD hand-slot click (member, hand 0=L/1=R, melee verb — the executed
	// command id, e.g. "stab" = the ATTACK, Balance::FindAttack).
	std::function<void(size_t, size_t, std::string_view)> onHandAttack;
	// A hand's `throw` use (member, the item id): true = it was thrown and the
	// hand empties; false = not now (down, or still recovering), it stays.
	std::function<bool(size_t, const std::string&, float charge)> onHandThrow;
	// What the hand menu may do to an item's flame (wired to DungeonWorld::
	// TorchActFor): 0 nothing, 1 Put out (a lit torch), 2 Light (a magical
	// torch, which no spell lights). onTorchAct does it to member's hand
	// (`light` false = put out); the world says what happened.
	std::function<int(const std::string&)> torchActFor;
	std::function<void(size_t member, size_t hand, bool light)> onTorchAct;
	// The hand right-click menu's command list for an item id (ItemKind::commands),
	// wired by Game to the world's item kinds — keeps the command source single.
	// By REFERENCE: a copy per hand click was a steady-state allocation. The
	// wiring lambda must spell out its `-> const std::vector<std::string>&`
	// return type, or it deduces a value and the reference dangles.
	std::function<const std::vector<std::string>&(const std::string&)> itemCommands;
	// The project's whole spell registry (wired to DungeonWorld::SpellDefs);
	// the hand-slot Magic submenu filters it by the member's known symbols.
	std::function<std::span<const std::unique_ptr<Spell>>()> spellDefs;
	// Member `i` casts the spell with this catalog id (a "cast:<id>" hand
	// default) from hand `hand` (0 = L, 1 = R) — wired to DungeonWorld::
	// CastSpellById (vocab/mana gates; the firing hand's MRU is credited).
	std::function<void(size_t, std::string_view, size_t)> onCastSpell;
	// Member `i` casts a symbol sequence BUILT in the spellbook panel (the
	// Magic area's member selector picks whose book) — wired to DungeonWorld::
	// CastSpell (exact-recipe match; a miss fizzles). The hand argument is
	// kBookHands: a book cast credits both hands' quick-cast MRU. True = the
	// caster lacked the mana, so the book keeps the spell built.
	std::function<bool(size_t, size_t, std::span<const SpellSymbol>)>
		onCastSequence;
	// Member `i` eats or drinks the item with this catalog id — wired to
	// DungeonWorld::ConsumeItem, which owns the catalogs and the two meters.
	// Returns what it actually RESTORED, so the caller can refuse the action
	// (and keep the item) when it would do nothing.
	std::function<resource::Refill(size_t, const std::string&)> onConsume;
	// What consuming this item LEAVES in its place (items.cat `drink_as`: a
	// waterskin steps down a fill level); empty = it is used up.
	std::function<std::string_view(const std::string&)> consumeLeaves;
	// The item details dialog's two questions of the world (wired to
	// DungeonWorld::ItemDetailsFor / ItemPreviewForType): what to say about an
	// item type (false = no such type), and its 3D preview into a buffer
	// (returns the submesh count). Neither may allocate.
	std::function<bool(const std::string&, ItemDetails&)> itemDetails;
	std::function<size_t(const std::string&, std::span<gfx::PreviewSubmesh>, Vec3&, Vec3&,
						 Mat4&)>
		itemPreview;
	// The Options panel's Rest button — wired to DungeonWorld::SetResting.
	std::function<void()> onToggleRest;
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
	// Worlds: the new-game world list. It borrows m_savesUi (both are one-list
	// pages rebuilt on open, and only one sub-page is ever showing).
	// Party: the party creation page, which borrows m_savesUi too.
	enum class MenuPage { Main, Settings, Saves, Worlds, Party };
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
	// The button labels default to the adapter restart's own ("Restart" /
	// "Cancel"); a question passes plain Yes / No.
	void OpenConfirm(const std::string& title, const std::string& body,
					 std::function<void()> onYes, std::function<void()> onNo = {},
					 const char* yesKey = "confirm.yes", const char* noKey = "confirm.no");
	// Runs the recorded answer, once, outside any widget walk.
	void ResolveConfirm();
	// Rebuilds the (dynamic) save-slot browser from the files on disk and
	// switches to the Saves page in the given mode. Shared by the landing/pause
	// Load entries and the pause Save entry; widgets live in m_savesUi.
	// `keepNotice` is the rebuild a delete asks for, which keeps what the page
	// says about it; opened afresh, the page starts with nothing to say.
	void OpenSavesPage(SavesMode mode, bool keepNotice = false);
	// The new-game world list (MenuPage::Worlds), built into m_savesUi.
	void OpenWorldsPage();
	// The stone card those three pages stand on (its title from `titleKey`) and
	// the column of rows inside it; the carved Back stone that ends the column.
	ui::Stack* SavesCard(const char* titleKey);
	PageCard* AddPageCard(float width, const char* titleKey, float top = -1.0f);
	void SavesBackRow(ui::Stack& col);
	// Start New Game and Editor share one flow; `editor` is which was clicked.
	void BeginNewGame(bool editor);
	// Save page helpers: commit the named save (arming an overwrite confirm
	// first if the name collides), and clear that armed confirm.
	void CommitSave();
	void DisarmOverwrite();
	// Rebuilds the Saves page if a deletion flagged it dirty (deferred so the
	// SlotList isn't cleared from inside its own row callback).
	void RefreshSavesIfDirty();
	// Re-filters the landing and pause lists (Continue / Load appear only with
	// a save) once the saves have changed. Deferred like RefreshSavesIfDirty.
	void RefreshMenuEntriesIfDirty();
	// The saves the menus offer: ListSaves of the world `saveWorld` names.
	std::vector<SaveSlot> SaveList() const;
	// Pushes the settings theme into every UIContext (each owns a copy).
	void ApplyTheme();
	// Pushes the skin (or null, per settings.uiSkin) into every UIContext.
	// Live — widgets re-check the pointer each draw, no rebuild needed.
	void ApplySkin();
	// Loads assets/ui/stones/<name>.png as the skin's stone (UI/Skin.h).
	void LoadStone(const std::string& name);
	// The Settings -> Material tab (GameUI_Stone.cpp): a filter row over the
	// StonePicker grid. Built with the rest of the settings page.
	void BuildStoneTab(ui::TabControl& tabs);
	// Loads whichever material should show now - the pinned one, or the
	// place's while the setting follows it - if it is not the one showing.
	void ApplyStone();
	// Reads the curated stones (assets/ui/stones: the tiles, stones.cat, the
	// thumbnails) into m_stones - once; a page rebuild reuses them.
	void ScanStones();
	// Sets the skin's legibility knobs (luma, calm, stoneMean, the text ring,
	// the solved inks) for the material on show, and logs how its inks read -
	// GameUI_Stone.cpp.
	void ApplyLegibility();
	// Scales the skin's frames and stone grain with the window, like the fonts.
	void UpdateSkinScale();
	// A floating HUD panel was dragged or resized (save + slider sync), and the
	// sync on its own (the scale sliders follow a corner drag).
	void OnHudPanelMoved();
	// A panel was minimized into the tray, or restored from it (click + save).
	void OnHudPanelHidden(bool restored);
	// The tray's default top, and where the right-hand docks start under its
	// strip, in pixels.
	float TrayTop(ui::UIContext& ctx) const;
	float DockColumnTop(ui::UIContext& ctx) const;
	void SyncHudPanelSliders();
	void SyncHudPanelSlidersIfStale();
	void DrawLoadProgress(const LoadQueue& queue, float barY); // shared bar
	// Title face centered at y (accent color) on centreX, a fraction of the
	// window width. Shared by every title screen.
	void DrawCenteredTitle(std::string_view text, float y, float centreX = 0.5f);
	void DrawTitleBackground(float wash); // title art, cover-fitted, then a wash
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
	// remembered per-item-type pick, else the item's first defaultable command,
	// which is performed WITHOUT being recorded), and with nothing to do at all
	// (bare hand, rune, key) it opens the use menu instead. Picking an item OUT
	// of a hand is a press-and-hold (OnHandHold), so a click stays a swing.
	void OnHandLeftClick(size_t i, size_t hand);
	// A left press HELD on a HUD hand box (HandSlot::kHoldSeconds): the hand's
	// item comes up onto the cursor, or swaps with the cursor's (Michael,
	// ui-updates). An empty hand under an empty cursor does nothing.
	void OnHandHold(size_t i, size_t hand);
	// A right- or middle-click on member `i`'s HUD hand `hand`: its USE menu (see
	// OpenHandUseMenu), where the hand's default is set. A left-click on a hand
	// with NO default yet opens the same menu, so the first click picks what
	// future clicks will do. (Right = details was tried on the hand boxes and
	// taken back - they are controls; the item map applies to the sheet.)
	void OnHandRightClick(size_t i, size_t hand);
	void OnHandMiddleClick(size_t i, size_t hand);
	// The item id at `place` in member `i`'s inventory, or null when the place
	// is empty or out of range.
	const std::string* ItemAt(size_t i, ItemPlace place) const;
	// The slot at `place`, for the handlers that consume an item (null for a
	// bag - a Pack is not an ItemSlot - and for anything out of range).
	ItemSlot* SlotAt(size_t i, ItemPlace place);
	// What the item at `place` weighs, a bag with its contents.
	float ItemWeightAt(size_t i, ItemPlace place) const;
	// The item mouse buttons' two actions, from wherever the click landed (the
	// sheet, the HUD, the party inventory): RIGHT = the details dialog, MIDDLE =
	// the use menu in `menu` - the hand menu for a hand, otherwise the uses that
	// work off the hand (memorize, eat, drink), or a "finds no use" log line
	// when there are none.
	void OpenItemDetails(size_t i, ItemPlace place);
	void OpenItemUseMenu(size_t i, ItemPlace place, ui::ContextMenu& menu);
	// Both context menus' onPick: dispatches on what the open menu was for.
	void OnUseMenuPick(int id);
	// The hand's USE menu: the item's data-driven command entries, and — when
	// the hand has no defaultable item command (bare hand, rune, key) — the
	// grouped default pickers: Combat > Punch/Kick and Magic > the member's
	// known spells (each submenu chains through the same ContextMenu).
	// Selecting an entry records it as the member's default for that item type
	// ("unarmed" for a bare hand) and, per GameSettings::useMenuExecutes,
	// performs it. A last Clear row (only while the hand is SET) forgets the
	// pick, so the hand is unset again. `menu` is the context the click landed
	// in (the HUD's, or the sheet's for a doll hand cell).
	void OpenHandUseMenu(size_t i, size_t hand, ui::ContextMenu& menu);
	// The hand menu's onPick: decodes a row id (the kUse* ranges in GameUI.cpp)
	// against what the menu was opened for (m_handMenuMember/Hand/Item).
	void OnHandMenuPick(int id);
	// The item's hand commands, or an empty list for a bare hand / no wiring.
	const std::vector<std::string>& CommandsFor(const std::string& itemId) const;
	// A use-menu entry was picked: record it as the default (menu-only commands
	// like memorize are never recorded) and execute per the Controls setting.
	void SelectUse(size_t i, size_t hand, std::string_view itemId,
				   std::string_view cmd);
	// Performs one use command on member `i`'s hand `hand` (the dispatch behind
	// both the left-click default and the menu): eat/memorize map to their
	// handlers, the melee verbs to onHandAttack. Unknown/empty ids no-op.
	void ExecuteUse(size_t i, size_t hand, std::string_view cmd);
	// The command a left-click on `itemId` ("" = bare hand) in hand `hand`
	// executes for this member: the hand's SET use (SetUseFor) when it has one,
	// else the item's first defaultable (non-menu-only) command - performed, but
	// never recorded, so the hand stays unset - else "": nothing to do, so the
	// left-click opens the use menu to pick one. A VIEW of the stored pick or
	// the catalog command (a returned string was a steady-state allocation per
	// swing): use it before anything records a new default.
	std::string_view DefaultUseFor(const Character& c, size_t hand,
								   const std::string& itemId) const;
	// The use the player explicitly SET for `itemId` in this hand from its menu,
	// while it is still valid, else "" (never picked, cleared, or stale). SET is
	// what the HUD shows and what Clear forgets; the item's own first command is
	// a default but NOT a set use.
	std::string_view SetUseFor(const Character& c, size_t hand,
							   const std::string& itemId) const;
	// What member `i`'s hand box shows (ControlBarDeps::handSetUse): whether the
	// hand is SET, and the spell when that use is a cast. Every frame, so it
	// builds nothing.
	HandSetUse HandSetUseFor(size_t i, size_t hand) const;
	// Whether a remembered default is still usable: an item command the item
	// still offers, one of the bare-hand combat verbs, or a "cast:<id>" whose
	// spell exists and whose symbols the member all knows.
	bool UseValidFor(const Character& c, const std::vector<std::string>& cmds,
					 std::string_view cmd) const;
	// Commits the rune in member `i`'s hand to memory: the symbol is learned and
	// the tablet consumed.
	void MemorizeFromHand(size_t i, size_t hand);
	// The shared memorize: learns `slot`'s rune symbol and consumes the tablet
	// — a rune memorizes from WHEREVER it sits (hand or backpack).
	void MemorizeSlot(size_t i, ItemSlot& slot);
	// THE ONE TEST for offering Memorize (Michael, spell-updates): the item is a
	// rune and member `i` - the one holding it - does not know it yet. Every
	// place the option appears asks it: the hand menu, the pack / doll menu, and
	// the details dialog's button. MemorizeSlot asks it too, so a known rune is
	// never spent for nothing.
	bool CanMemorize(size_t i, std::string_view itemId) const;
	// The details dialog's Memorize: the slot it was opened on, re-checked.
	void MemorizeFromDetails();
	// Eats (or drinks) the item in member `i`'s hand / in `slot`, wherever it
	// sits: the world restores what it restores and the item is consumed, or it
	// is refused with a line when it would restore nothing.
	void EatFromHand(size_t i, size_t hand);
	void EatSlot(size_t i, ItemSlot& slot);
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
		case MenuPage::Worlds:   return m_savesUi; // the list page it borrows
		case MenuPage::Party:    return m_savesUi; // likewise
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
	// Textured-chrome skin (UI/Skin.h): the bevel overlays, the polish, the
	// picked stone + the Skin handed to every context by ApplySkin (null when
	// settings.uiSkin is off — the flat debug look). Textures are optional;
	// missing frames stay flat, a missing stone draws without grain.
	std::unique_ptr<gfx::Texture> m_framePanelTex;
	std::unique_ptr<gfx::Texture> m_frameButtonTex;
	std::unique_ptr<gfx::Texture> m_frameButtonDownTex;
	std::unique_ptr<gfx::Texture> m_frameSlotTex;
	std::unique_ptr<gfx::Texture> m_sheenTex;
	std::unique_ptr<gfx::Texture> m_stoneTex;
	ui::Skin m_skin;
	// The resource bars' look (PartyHudTypes.h): the iron frame, the fills'
	// clock and every member's heartbeat. The party bar and the sheet point at
	// it; TickResourceBars keeps it moving, ApplySkin follows uiskin.
	std::unique_ptr<gfx::Texture> m_barFrameTex;
	ResourceBarStyle m_barStyle;
	// The spellbook's Cast / Clear face glyphs (drawn on stone buttons).
	std::unique_ptr<gfx::Texture> m_castGlyphTex;
	std::unique_ptr<gfx::Texture> m_clearGlyphTex;
	// The closed-panels tray's button faces, by kHudPanelFields index (only the
	// panels that minimize have one).
	std::array<std::unique_ptr<gfx::Texture>, kHudSheet> m_panelGlyphs;
	// The movement pad's chevron icon faces (single = step, double = turn).
	std::unique_ptr<gfx::Texture> m_chevronTex;
	std::unique_ptr<gfx::Texture> m_chevron2Tex;
	// ...and the cut-stone pad's etched symbols, in the pad's order (plain and
	// gold-lit), plus the block chamfer they sit on.
	std::array<std::unique_ptr<gfx::Texture>, 6> m_moveEtch;
	std::array<std::unique_ptr<gfx::Texture>, 6> m_moveEtchLit;
	// The sheet's tab stones' symbols, in its Mode order (loaded in
	// BuildStaticUi, before the sheet that points at them is built).
	std::array<std::unique_ptr<gfx::Texture>, 5> m_tabEtch;
	std::array<std::unique_ptr<gfx::Texture>, 5> m_tabEtchLit;
	// What every etch's gold floor shows (ui::MeasureEtchFloor, taken as each
	// loads), plain and lit: the pad's six, then the tabs' five. The material
	// report reads them (GameUI_Stone.cpp).
	std::array<ui::EtchFloor, 6 + 5> m_etchFloor{};
	std::array<ui::EtchFloor, 6 + 5> m_etchFloorLit{};
	std::unique_ptr<gfx::Texture> m_frameBlockTex;
	std::unique_ptr<gfx::Texture> m_frameBlockDownTex;
	std::unique_ptr<gfx::Texture> m_glowTex; // a set hand box's centre glow
	const gfx::Texture* m_closeIcon = nullptr; // shared, owned by AssetUtil

	MenuPage m_menuPage = MenuPage::Main;
	// Whether the world list was opened by Editor rather than Start New Game:
	// the pick, not the page, is where the new game begins.
	bool m_worldsForEditor = false;
	// The party creation page (GameUI_Party.cpp): built next frame after
	// OpenPartyPage or an edit that needs a new tree, and left next frame on
	// Back (both from inside its own widgets' callbacks).
	std::unique_ptr<PartyCreationPage> m_partyPage;
	bool m_partyBuildPending = false;
	bool m_partyLeavePending = false;
	bool m_partyFromWorlds = false; // Back returns to the world list
	void BuildPartyPage();
	void RefreshPartyPageIfDirty();
	// Settings -> UI -> Party Colors (GameUI_Party.cpp): the four pickers, kept
	// in step with whoever holds each slot.
	std::array<ui::ColorPicker*, kMemberColorCount> m_memberColorPickers{};
	bool MemberColorInPlay(size_t slot) const; // a member of a game in play holds it
	void SyncMemberColorPickers();
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
	// Whether EACH list was built WITH the save-only entries, so a rebuild is
	// skipped when deleting one of several saves changes nothing. One flag per
	// list (code-review C366): they were one, and the pause list rebuilt on
	// every Esc set it for both - so after a first save the title kept its
	// save-less list, no Continue, no Load, until the next launch.
	bool m_titleHasSaves = false;
	bool m_pauseHasSaves = false;
	// The two lists as built (in m_menuUi / m_pauseUi, valid until the next
	// build), for MenuEntries.
	ui::MenuList* m_titleList = nullptr;
	ui::MenuList* m_pauseList = nullptr;
	// What the Save / Load page says above its list: a delete that failed.
	// Kept across the rebuild the delete itself asks for, cleared when the page
	// is opened afresh.
	std::string m_saveNotice;

	// Widgets the game updates later; the UIContexts own them.
	MessageLog* m_log = nullptr;
	ui::Label* m_compass = nullptr;
	ui::Label* m_position = nullptr;
	// The log's corner row after its Log button (MessageLog::cornerButtons), by
	// index: Rest, whose label tracks the world (SetResting), then Help.
	static constexpr size_t kCornerRest = 0, kCornerHelp = 1, kCornerCount = 2;
	CharacterSheet* m_sheet = nullptr;
	size_t m_sheetIndex = 0; // member shown by the character sheet

	// The Game tab's key-bind rows, parallel to kKeyFields — kept so a
	// rebind can swap a duplicate key out of its old row.
	std::vector<ui::KeyBind*> m_keyBinds;

	// The Video tab's Max Lights dropdown — kept so SyncMaxLights can re-point
	// it when a quality change resets the light budget.
	ui::DropDown* m_maxLightsDrop = nullptr;

	// HUD use menu (a hand box's uses, a party-inventory item's), opened by the
	// middle button. Reused: GameUI fills it for whatever was clicked.
	ui::ContextMenu* m_handMenu = nullptr;
	// What the open use menu is FOR, read back by OnUseMenuPick: a menu row
	// carries only an int id (ui::ContextMenu is allocation-free). A HAND menu
	// (member + hand) or a SLOT menu (member + place); either way the item id
	// it was opened on, assign()ed so it keeps its capacity across opens.
	enum class UseMenuFor { Hand, Slot };
	UseMenuFor m_useMenuFor = UseMenuFor::Hand;
	size_t m_handMenuMember = 0;
	size_t m_handMenuHand = 0;
	ItemPlace m_useMenuPlace;
	std::string m_handMenuItem;
	// What the details dialog was opened on, for its Memorize button: whose it
	// is, where it sits, and what it was (re-checked when the button is pressed).
	size_t m_detailsMember = 0;
	ItemPlace m_detailsPlace;
	std::string m_detailsItem;
	// The SHEET's own context menu (the sheet hides the HUD, so m_handMenu
	// can't serve it) - the same menus, in the sheet's context.
	ui::ContextMenu* m_sheetMenu = nullptr;
	// The item details dialog: built once in BuildStaticUi (a right-click in a
	// guarded frame must not build a widget tree), rebuilt on a language switch.
	std::unique_ptr<ItemDetailsDialog> m_itemDetails;
	// The portrait picker: built once, likewise; filled by Game::LoadPortraits.
	std::unique_ptr<PortraitPicker> m_portraitPicker;
	// The party window (owned by m_hudUi), opened by the sheet's "All".
	PartyWindow* m_inventory = nullptr;
	ui::Button* m_sheetAll = nullptr; // that button (owned by m_sheetUi)
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
	bool m_confirmActive = false;       // the Yes/No modal is up
	std::function<void()> m_confirmYes, m_confirmNo; // its two answers
	int m_confirmAnswer = 0;            // 1 yes / 2 no, set by a button, run by ResolveConfirm

	// Installed languages (assets/lang scan), in the Game tab dropdown's
	// order; maps the selection index back to a language code.
	std::vector<loc::LanguageInfo> m_languages;
	// The stones the Settings -> Stone tab offers (ScanStones): each tile's
	// stem, its toned luminance from stones.cat (the light / dark filter) and
	// its thumbnail. Loaded once and kept across page rebuilds, so a language
	// switch does not reload fourteen textures; the picker points into it.
	struct StoneInfo {
		std::string name;
		float luminance = 0.0f;
		std::string family; // stones.cat `family` (stone / wood / forest / ...)
		std::unique_ptr<gfx::Texture> thumb;
		// stones.cat `mean` / `detail`: the tile's mean colour and how busy it
		// is at glyph scale - what ApplyStone calms it by (Skin::calm).
		Vec4 mean{0.2f, 0.2f, 0.2f, 1.0f};
		float detail = 0.0f;
	};
	std::vector<StoneInfo> m_stones;
	bool m_stonesScanned = false;
	// The place's material (SetPlaceStone) and the one actually loaded into
	// the skin (ApplyStone compares the two names, so a level change that keeps
	// the material reloads nothing).
	std::string m_placeStone;
	std::string m_shownStone;
	std::string m_previewStone; // PreviewStone; empty = none
	// The Material tab's grid, while the settings page stands - so a place
	// change can update its "follow" tile. Dies with the page (UIContext rule).
	StonePicker* m_stonePicker = nullptr;

	// The floating HUD (UI/FloatingPanel.h): the layer every movable panel sits
	// on, and the panels by kHudPanelFields index (null until BuildHud). The
	// party bar owns the slots. All owned by m_hudUi.
	ui::FloatingLayer* m_hudLayer = nullptr;
	std::array<ui::FloatingPanel*, std::size(kHudPanelFields)> m_hudPanels{};
	PartyBar* m_partyBar = nullptr;
	std::vector<CharacterPanel*> m_partyPanels; // owned by m_partyBar
	// Settings -> UI's per-panel scale sliders (kHudPanelFields order), kept so a
	// corner drag can move them (SyncHudPanelSliders). Owned by m_settingsUi.
	std::array<ui::Slider*, std::size(kHudPanelFields)> m_hudScaleSliders{};
	bool m_hudSlidersStale = false; // a drag moved a scale; sync before showing
	unsigned m_panelMinimizes = 0, m_panelRestores = 0; // see PanelMinimizes
	// What every member panel's name reads and calls (BuildHud fills it).
	LeaderLink m_leaderLink;
	// The pointer shape the HUD asked for last frame (a grip's arrow), applied
	// at the top of the next (UpdateFonts) so every other state resets it.
	Window::Cursor m_hudCursor = Window::Cursor::Arrow;
	const HitSplatIcons* m_hitSplats = nullptr; // hit-feedback icons (Game-owned)
	const ItemIconBank* m_itemIcons = nullptr;  // item icons (Game-owned)
	const ItemWeightBank* m_itemWeights = nullptr; // item carry weights (Game-owned)
	const ItemIconBank* m_slotIcons = nullptr;  // equipment-slot outlines (Game-owned)
	const ItemIconBank* m_useIcons = nullptr;   // hand-use pictures (Game-owned)
	const ItemCategoryBank* m_itemCategories = nullptr; // item categories (Game-owned)
	// Cursor-carried item (Game owns the storage; placement handlers mutate it)
	// + the last HUD mouse position (stashed in UpdateHud so RenderHud can draw
	// the held icon, which has no Input).
	HeldItem* m_held = nullptr;
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
