// ============================================================================
// Game/GameUI.cpp — see GameUI.h.
// ============================================================================
#include "Game/GameUI.h"

#include "Core/Loc.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/HudTray.h"
#include "Game/MemberCards.h"
#include "Game/MenuPanel.h"
#include "Game/PartyHudDraw.h" // the resource bars' heartbeat (TickResourceBars)
#include "Game/Project.h"
#include "Game/SaveGame.h"
#include "Game/Spell/Spell.h"
#include "Graphics/DisplayEnum.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <iterator>

namespace dungeon::game {

namespace {
// A world's title as the player reads it (its manifest `name`), from its
// folder name; the folder itself when the manifest names nothing.
std::string WorldTitle(const std::string& folder) {
	const std::string title =
		Project::ReadName(Project::FolderFor(paths::Asset("projects"), folder));
	return title.empty() ? folder : title;
}

// The movement stones' etched symbols (assets/ui/etch_move_<name>.png), in the
// MovementPad's order: turn left, forward, turn right / strafe left, back,
// strafe right.
constexpr const char* kMoveEtches[] = {"turn_left",   "forward", "turn_right",
									   "strafe_left", "back",	 "strafe_right"};
// The sheet's tab stones (assets/ui/etch_tab_<name>.png), in its Mode order.
constexpr const char* kTabEtches[] = {"inventory", "stats", "skills", "spells", "effects"};

// Font pixel heights at the 900px-tall design window (the layouts in
// BuildMenu/BuildHud are authored against the same design size). UpdateFonts
// rescales them against the live window height so text tracks the UI.
constexpr float kFontDesignWindowH = 900.0f;
constexpr float kHudFontH = 17.0f;
constexpr float kMenuFontH = 28.0f;
constexpr float kSheetFontH = 22.0f;
constexpr float kTitleFontH = 64.0f;
// Re-bakes wait for the window height to hold still this long, so an
// interactive resize drag doesn't drain the GPU on every size change.
constexpr float kFontSettleDelay = 0.25f;

// The chrome every menu-style page draws above its own widgets: the big title,
// then the subtitle a line under it. Both the landing page and the pause
// overlay draw them, so the positions live HERE rather than being repeated at
// each draw site — which is how the saves page came to start its content at
// 0.16/0.24, straight on top of the subtitle it could not see.
//
// kMenuContentY is the first row of page-owned space: below the subtitle, plus
// air. A page that authors its widgets from here cannot collide with the
// chrome, whatever the chrome later does.
constexpr float kMenuTitleY = 0.16f;
constexpr float kMenuSubtitleY = kMenuTitleY + 74.0f / kFontDesignWindowH;
constexpr float kMenuContentY =
	kMenuSubtitleY + (kMenuFontH + 20.0f) / kFontDesignWindowH;
// The bottom of the save / load / world pages' stone card. Everything on the
// card is stacked inside it, so nothing can be pushed off its foot.
constexpr float kSavesCardBottom = 0.95f;
// One slot on those pages, in rem, gap included (SlotList::rowHeight): its
// stone is the height of a menu entry's (MenuPanel::kEntryRem), and the save
// name's field and the Save / Back stones match it.
constexpr float kSlotRowRem = 2.1f;

// Widget bounds are normalized fractions [0..1] of their container
// (see Widget.h). Layouts are authored directly in those fractions — never
// design pixels, never a post-hoc Norm() conversion.

// --- the HUD's DEFAULT layout, as window fractions ---------------------------
// Where a floating HUD panel sits until the player moves it (BuildHud's
// defaultPos / size functions). File scope so the numbers live in one place.
constexpr float kBarTop = 0.018f;
// Party bar height at scale 1. Raised from 0.107 for the framed resource bars
// (Michael, 2026-09-30: "make the party bar tubes taller") - at 0.107 a tube
// was ~10 px and its iron frame shrank to a dark rim with no ornament visible.
constexpr float kBarH0 = 0.140f;
constexpr float kBarGap = 0.018f; // party bar -> the panels under it
constexpr float kFooter = 0.071f; // the message-log footer along the bottom
// The right-hand dock column at scale 1 (~250/1600), and its gap from the
// window's right edge.
constexpr float kControlW = 0.156f;
constexpr float kControlMargin = 0.01f;

// --- settings page rows ------------------------------------------------------
// In REM (UI/Units.h) — the settings context's own type size, which already
// tracks the window (UpdateFonts). These replace the page fractions the old
// Flow placed rows at, and they were the same numbers written twice: labelH
// 0.057 of a 0.55-tall page is 28px at the design height, which is exactly one
// line of the menu font. Saying it once, in the unit that means it, is the
// whole point.
constexpr float kSetLabel = 1.05f;  // a heading / field name
constexpr float kSetCtrl = 1.45f;   // a dropdown, checkbox or key bind
constexpr float kSetSlider = 1.85f; // label over track
constexpr float kSetPicker = 2.6f;  // a colour swatch with its label
constexpr float kSetGroup = 0.85f;  // the gap BETWEEN sections
constexpr float kSetRule = 0.15f;   // the hairline separator's own row

// A settings tab's rows. Content-sized (UI/Layout.h), so a tab is as long as
// its settings and the page scrolls — which the Video tab has always needed.
ui::Stack* SettingsTab(ui::TabControl& tabs, size_t tab) {
	ui::Stack* rows = tabs.AddChild<ui::Stack>(tab, gfx::Rect{0, 0, 1, 1});
	rows->debugName = "rows";
	rows->fitContent = true;
	rows->padRem = 1.0f;
	// The gap a label and the control under it want. A SECTION break asks for
	// more by adding a Space — the old Flow expressed both as per-row collapsing
	// margins, which meant every row carried two numbers describing its
	// neighbours rather than itself.
	rows->gapRem = 0.42f;
	return rows;
}

} // namespace

GameUI::GameUI(Window& window, gfx::GraphicsDevice& device,
			   gfx::SpriteBatch& spriteBatch, audio::AudioEngine& audio,
			   const SoundBank& sounds, GameSettings& settings,
			   std::vector<Character>& characters, ui::FontLibrary& fonts)
	: m_window(window), m_device(device), m_fonts(fonts),
	  m_spriteBatch(spriteBatch), m_audio(audio), m_sounds(sounds),
	  m_settings(settings), m_characters(characters),
	  m_hudUi(fonts, ui::FontRole::Body, kHudFontH),
	  m_menuUi(fonts, ui::FontRole::Body, kMenuFontH),
	  m_settingsUi(fonts, ui::FontRole::Body, kMenuFontH),
	  m_pauseUi(fonts, ui::FontRole::Body, kMenuFontH),
	  m_savesUi(fonts, ui::FontRole::Body, kMenuFontH),
	  m_sheetUi(fonts, ui::FontRole::Body, kSheetFontH),
	  m_confirmUi(fonts, ui::FontRole::Body, kMenuFontH),
	  m_titleFont(&fonts.Get(ui::FontRole::Display, kTitleFontH)) {}

void GameUI::BuildStaticUi() {
	ApplyTheme();
	// BEFORE the pages: BuildCharacterSheet hands the close box to a Button,
	// which copies the POINTER at Add time. This used to be loaded over in
	// LoadTitleArt — a boot LOAD TASK, i.e. long after this runs — so the sheet
	// captured a null and drew the text "x" fallback for the life of the
	// process while every editor dialog (which loads in its own constructor)
	// showed the real icon. Cheap to do here: CloseIcon loads once and hands
	// back the same texture to everyone.
	m_closeIcon = CloseIcon(m_device);
	// Same reason, one step further: the drop-down's expander box is read from
	// a shared registry by the CONTROL, so it has to be installed before any
	// page — or any editor dialog — draws one.
	LoadSharedControlIcons(m_device);
	// And the sheet's tab stones (tools/BuildEtchGlyphs.py), for the same
	// reason: BuildCharacterSheet hands their pointers to the tabs.
	for (size_t i = 0; i < std::size(kTabEtches); ++i) {
		const std::string stem = std::string("ui\\etch_tab_") + kTabEtches[i];
		m_tabEtch[i] = TryLoadTextureFile(m_device, paths::Asset(stem));
		m_tabEtchLit[i] = TryLoadTextureFile(m_device, paths::Asset(stem + "_lit"));
	}
	BuildMenu();
	BuildPauseMenu();
	BuildCharacterSheet();
	// The item details dialog, built whole now so a right-click only fills it.
	m_itemDetails = std::make_unique<ItemDetailsDialog>(m_device, m_fonts);
	ApplyTheme(); // again, now the dialog exists to receive it (the skin
				  // arrives with LoadTitleArt, whose ApplySkin reaches it too)
}

void GameUI::LoadTitleArt() {
	m_titleBackground = LoadTextureFile(m_device, paths::Asset("ui\\title_bg"));
	// Small UI glyph; optional (the SlotList falls back to a text "X").
	m_deleteIcon = TryLoadTextureFile(m_device, paths::Asset("ui\\delete"));

	// UI skin: the bevel overlays every stone face is drawn with
	// (assets/ui/frame_*.png + sheen_panel.png, made by tools/BuildUiFrames.py,
	// committed source like the other UI images) and the picked stone (LoadStone).
	// All optional — a missing frame leaves that chrome flat, and the flat look
	// survives whole as the debug mode / uiskin=0.
	m_framePanelTex = TryLoadTextureFile(m_device, paths::Asset("ui\\frame_panel"));
	m_frameButtonTex = TryLoadTextureFile(m_device, paths::Asset("ui\\frame_button"));
	m_frameButtonDownTex = TryLoadTextureFile(m_device, paths::Asset("ui\\frame_button_down"));
	m_frameSlotTex = TryLoadTextureFile(m_device, paths::Asset("ui\\frame_slot"));
	m_sheenTex = TryLoadTextureFile(m_device, paths::Asset("ui\\sheen_panel"));
	// The cut-stone block's chamfer, up and pressed (same script).
	m_frameBlockTex = TryLoadTextureFile(m_device, paths::Asset("ui\\frame_block"));
	m_frameBlockDownTex = TryLoadTextureFile(m_device, paths::Asset("ui\\frame_block_down"));
	// The resource bars' iron frame (tools/CutBarFrame.py). Optional too:
	// without it the bars draw flat.
	m_barFrameTex = TryLoadTextureFile(m_device, paths::Asset("ui\\bar_frame"));
	m_barStyle.frame = m_barFrameTex.get();
	// The spellbook's Cast / Clear face glyphs (tools/BuildToolIcons.py); without
	// them the stone buttons carry their words.
	m_castGlyphTex = TryLoadTextureFile(m_device, paths::Asset("ui\\glyph_cast"));
	m_clearGlyphTex = TryLoadTextureFile(m_device, paths::Asset("ui\\glyph_clear"));
	// The tray's button faces, one per panel that minimizes (same script);
	// without one, that button carries the panel's name.
	for (size_t i = 0; i < kHudSheet; ++i)
		if (const char* glyph = kHudPanelFields[i].glyph)
			m_panelGlyphs[i] = TryLoadTextureFile(
				m_device, paths::Asset(std::string("ui\\glyph_") + glyph));
	// The movement pad's chevrons (single = step, double = turn), rotated in
	// quarter turns per direction by ui::Button::iconTurns.
	m_chevronTex = TryLoadTextureFile(m_device, paths::Asset("ui\\icon_chevron"));
	m_chevron2Tex = TryLoadTextureFile(m_device, paths::Asset("ui\\icon_chevron2"));
	// ...and their CUT-STONE symbols (tools/BuildEtchGlyphs.py), one per
	// direction plus a lit twin, in the pad's order. The chevrons above stay
	// the fallback for the flat debug look.
	for (size_t i = 0; i < std::size(kMoveEtches); ++i) {
		const std::string stem = std::string("ui\\etch_move_") + kMoveEtches[i];
		m_moveEtch[i] = TryLoadTextureFile(m_device, paths::Asset(stem));
		m_moveEtchLit[i] = TryLoadTextureFile(m_device, paths::Asset(stem + "_lit"));
	}
	// The soft glow behind a SET hand box (tools/BuildGlow.py). Optional: without
	// it a set hand shows the flat tint alone.
	m_glowTex = TryLoadTextureFile(m_device, paths::Asset("ui\\glow_radial"));
	// (The shared close box is loaded in BuildStaticUi, which needs it before
	// this load task runs — see the note there.)
	// The overlays are authored at 2x (64 texels, a 16-texel corner) and drawn
	// at half scale times the UI's window scale (UpdateSkinScale). They carry
	// only light, uniform along each edge, so they stretch. The last number is
	// the VISIBLE frame - rim + bevel (+ the panel's groove) - which is where a
	// face's content starts (ui::FaceInset); it must match BuildUiFrames.py.
	m_skin.panel = {m_framePanelTex.get(), 16.0f, 0.5f, /*stretch*/ true, 12.5f};
	m_skin.button = {m_frameButtonTex.get(), 16.0f, 0.5f, true, 9.0f};
	m_skin.buttonDown = {m_frameButtonDownTex.get(), 16.0f, 0.5f, true, 8.0f};
	m_skin.slot = {m_frameSlotTex.get(), 16.0f, 0.5f, true, 8.0f};
	m_skin.sheen = {m_sheenTex.get(), 0.0f, 1.0f, true};
	// The block's visible edge is its joint + chamfer (outline 2 + band 11).
	m_skin.block = {m_frameBlockTex.get(), 16.0f, 0.5f, true, 13.0f};
	m_skin.blockDown = {m_frameBlockDownTex.get(), 16.0f, 0.5f, true, 10.0f};
	ApplyStone(); // the pinned material, or the place's (GameUI_Stone.cpp)
	UpdateSkinScale();
	ApplySkin();
}

// The frames and the stone grain track the window like the fonts do, so a
// bevel is the same share of a button at any resolution. Assignments only -
// runs every frame from UpdateFonts.
void GameUI::UpdateSkinScale() {
	const float s = 0.5f * m_fontScale;
	m_skin.panel.scale = m_skin.button.scale = m_skin.buttonDown.scale = m_skin.slot.scale = s;
	m_skin.block.scale = m_skin.blockDown.scale = s;
	m_skin.stoneTile = 1024.0f * m_fontScale;
}

void GameUI::ApplyTheme() {
	for (ui::UIContext* ctx :
		 {&m_hudUi, &m_menuUi, &m_settingsUi, &m_pauseUi, &m_savesUi, &m_sheetUi,
		  &m_confirmUi})
		ctx->SetTheme(m_settings.theme);
	if (m_itemDetails) m_itemDetails->UI().SetTheme(m_settings.theme);
}

void GameUI::ApplySkin() {
	const ui::Skin* skin = m_settings.uiSkin ? &m_skin : nullptr;
	m_barStyle.framed = m_settings.uiSkin; // the flat debug look takes the bars too
	for (ui::UIContext* ctx :
		 {&m_hudUi, &m_menuUi, &m_settingsUi, &m_pauseUi, &m_savesUi, &m_sheetUi,
		  &m_confirmUi})
		ctx->SetSkin(skin);
	if (m_itemDetails) m_itemDetails->UI().SetSkin(skin);
}

void GameUI::Click(float volume) { m_audio.Play(m_sounds.click, volume); }

// --- held-item placement -----------------------------------------------------
// The HandSlot/CharacterPanel already consumed the mouse, so the world won't
// also treat these clicks as a drop.

// Left-click a portrait: while CARRYING an item, quick-stow it into that
// member's selected pack (right-click instead opens the backpack to place it
// precisely); empty-handed, it opens the member's sheet.
void GameUI::OnPortraitClick(size_t i) {
	if (i >= m_characters.size()) return;
	if (Holding()) {
		Character& c = m_characters[i];
		const Inventory& inv = c.inventory;
		const std::string& packId = inv.packs[static_cast<size_t>(inv.selectedPack)].typeId;
		// Honour the selected pack's content restriction (same as the sheet drop).
		if (m_itemCategories && !m_itemCategories->PackAcceptsItem(packId, **m_held)) {
			m_audio.Play(m_sounds.bump, 0.5f);
			AddLogLine(loc::FormatLine("log.pack_rejects", loc::ViewKey("item.", **m_held),
									   loc::ViewKey("item.", packId)));
		} else if (const loc::Line name = loc::ViewKey("item.", **m_held);
				   c.inventory.Stow(*m_held)) { // the name is taken first: Stow empties the cursor
			AddLogLine(loc::FormatLine("log.stow", c.name, name), c.portraitColor);
			Click();
		} else {
			AddLogLine(loc::View("log.pack_full")); // full — keep carrying it
		}
		return;
	}
	onOpenSheet(i); // synchronous (Game sets state + ShowSheet)
	m_sheet->SetMode(CharacterSheet::Mode::Inventory);
}

// Right-click a portrait ALWAYS opens that member's backpack (sheet), whether or
// not an item is carried — so a held item can be placed into a specific slot.
void GameUI::OnPortraitRightClick(size_t i) {
	if (i >= m_characters.size()) return;
	onOpenSheet(i);
	m_sheet->SetMode(CharacterSheet::Mode::Inventory);
}

// A click on the stat bars opens the sheet on the Stats tab.
void GameUI::OnPortraitBars(size_t i) {
	if (i >= m_characters.size()) return;
	onOpenSheet(i);
	m_sheet->SetMode(CharacterSheet::Mode::Stats);
}

// A click on one of the panel's status-effect icons (the name-band row): the
// sheet's Effects tab is the icon's long form, so the icon IS its door.
void GameUI::OnPortraitEffects(size_t i) {
	if (i >= m_characters.size()) return;
	onOpenSheet(i);
	m_sheet->SetMode(CharacterSheet::Mode::Effects);
}

// ============================================================================
// Landing page — title plus a MenuList; entries highlight on mouse hover or
// keyboard selection. All entries are wired: Continue loads the newest save,
// Load opens the saves browser, Start New Game and Settings work as labeled.
// ============================================================================
void GameUI::BuildMenu() {
	BuildMenuList();
	SeedVideoStaging(); // fresh edit: stage = applied settings
	BuildSettings();
}

// Just the landing list. Split out of BuildMenu because it is REBUILT whenever
// the saves on disk change (see RefreshMenuEntriesIfDirty) and the settings
// page must not be rebuilt with it — m_settingsUi is a different context, and
// BuildSettings would double up its widgets.
void GameUI::BuildMenuList() {
	m_menuUi.Clear(); // the list is this context's only content
	// Continue and Load only appear when at least one save exists, so the list
	// is sized to whatever entries are present (one quarter each with all four,
	// half each with just Start + Settings). Bounds are window fractions.
	const bool hasSaves = !ListSaves().empty();
	m_menuHasSaves = hasSaves;
	// +1 for Exit, which is always present: it is the ONLY pointer-driven way out
	// of the title screen now that Esc no longer quits (see Game.cpp's Menu case).
	const int itemCount = (hasSaves ? 5 : 3) + 1; // Editor sits under Start
	// The same stone card as the pause menu (Michael chose both), untitled:
	// the game's own title stays over the art, and the card starts just under
	// its subtitle.
	auto* panel = m_menuUi.Add<MenuPanel>(std::string(), static_cast<size_t>(itemCount),
										  kMenuContentY + 0.02f);
	ui::MenuList* menu = panel->List();

	// Order: Continue / Load (only when a save exists), then Start New Game just
	// above Settings. Continue loads the most recent save outright (no browser).
	if (hasSaves) {
		menu->AddItem(loc::Tr("menu.continue"), [this] {
			const std::vector<SaveSlot> slots = ListSaves();
			if (slots.empty()) return; // raced with a deletion
			Click(0.6f);
			m_menuPage = MenuPage::Main;
			if (onEditorOnArrival) onEditorOnArrival(false);
			onLoadSave(slots.front().path); // ListSaves is newest-first
		});
		menu->AddItem(loc::Tr("menu.load"), [this] {
			Click();
			if (onEditorOnArrival) onEditorOnArrival(false);
			OpenSavesPage(SavesMode::Load);
		});
	}
	menu->AddItem(loc::Tr("menu.start"), [this] { BeginNewGame(false); });
	// A new game that opens straight into the editor, paused (Michael,
	// 2026-09-25): the way in for building rather than playing.
	menu->AddItem(loc::Tr("menu.editor"), [this] { BeginNewGame(true); });
	menu->AddItem(loc::Tr("menu.settings"), [this] {
		Click();
		m_menuPage = MenuPage::Settings;
	});
	// Exit LAST, the way the pause menu ends with it. Deliberately the only click
	// that quits from here, since Esc no longer does.
	menu->AddItem(loc::Tr("menu.exit"), [this] {
		Click();
		onQuit();
	});
}

// The shared settings page (landing + pause route to the same m_settingsUi).
// Split out of BuildMenu so a Video-tab adapter/monitor change can rebuild just
// this page (different dropdown structure) without touching the menu list.
void GameUI::BuildSettings() {
	// Settings page: tabs over a shared page + Back beneath.
	// All bounds are [0..1] of parent (TabControl of the window; children of
	// the tab page). Fonts still track window height via UpdateFonts.
	constexpr float kTabsX = 0.10f, kTabsY = 0.29f, kTabsW = 0.80f, kTabsH = 0.55f;
	constexpr float kStrip = 60.0f / 552.0f;
	auto* tabs = m_settingsUi.Add<ui::TabControl>(gfx::Rect{kTabsX, kTabsY, kTabsW, kTabsH}, kStrip);
	m_settingsTabs = tabs; // kept so a Video repopulate restores the active tab
	const size_t tabGame = tabs->AddTab(loc::Tr("settings.tab.game"));
	const size_t tabControls = tabs->AddTab(loc::Tr("settings.tab.controls"));
	const size_t tabVideo = tabs->AddTab(loc::Tr("settings.tab.video"));
	const size_t tabAudio = tabs->AddTab(loc::Tr("settings.tab.audio"));
	const size_t tabUi = tabs->AddTab(loc::Tr("settings.tab.ui"));
	// One content-sized stack per tab; every row below says how tall it is and
	// nothing says where it goes.
	ui::Stack* gf = SettingsTab(*tabs, tabGame);
	ui::Stack* cf = SettingsTab(*tabs, tabControls);
	ui::Stack* vf = SettingsTab(*tabs, tabVideo);
	ui::Stack* af = SettingsTab(*tabs, tabAudio);
	ui::Stack* uf = SettingsTab(*tabs, tabUi);
	BuildStoneTab(*tabs); // the sixth tab, GameUI_Stone.cpp

	// Game: language. The language list is whatever assets/lang holds;
	// selecting one defers to Game (settings save + string reload +
	// RebuildForLanguage at the top of the next frame — rebuilding here
	// would destroy this dropdown mid-callback).
	gf->Row<ui::Label>(ui::Len::Fixed(kSetLabel), loc::Tr("settings.language"))->dim =
		true;
	m_languages = loc::ScanLanguages(paths::Asset("lang"));
	std::vector<std::string> languageNames;
	int languageIndex = 0;
	for (size_t i = 0; i < m_languages.size(); ++i) {
		languageNames.push_back(m_languages[i].name);
		if (m_languages[i].code == m_settings.language)
			languageIndex = static_cast<int>(i);
	}
	gf->Row<ui::DropDown>(
		ui::Len::Fixed(kSetCtrl), std::move(languageNames),
		languageIndex, [this](int index) {
			Click();
			if (index >= 0 && index < static_cast<int>(m_languages.size()) &&
				m_languages[static_cast<size_t>(index)].code != m_settings.language)
				onLanguageSelected(m_languages[static_cast<size_t>(index)].code);
		});

	// Controls: movement key bindings (kKeyFields). Click a key box, press
	// the new key; binding a key another action already uses hands that
	// action the old key (swap) so the set stays conflict-free. Each rebind
	// goes straight into the Party (onKeysChanged) and persists.
	cf->Row<ui::Label>(ui::Len::Fixed(kSetLabel), loc::Tr("settings.movement_keys"));
	m_keyBinds.clear();
	for (size_t i = 0; i < std::size(kKeyFields); ++i) {
		const KeyField& field = kKeyFields[i];
		auto* bind = cf->Row<ui::KeyBind>(
			ui::Len::Fixed(kSetCtrl),
			loc::Tr(field.labelKey), m_settings.moveKeys.*(field.field),
			[this, member = field.field](int vkey) {
				Click();
				MoveKeys& keys = m_settings.moveKeys;
				const int old = keys.*member;
				for (size_t j = 0; j < std::size(kKeyFields); ++j) {
					int MoveKeys::*other = kKeyFields[j].field;
					if (other != member && keys.*other == vkey) {
						keys.*other = old;
						m_keyBinds[j]->SetKey(old);
					}
				}
				keys.*member = vkey;
				onKeysChanged();
				m_settings.Save();
			});
		bind->capturePrompt = loc::Tr("settings.press_a_key");
		m_keyBinds.push_back(bind);
	}

	// Controls → Mouse Look: right-mouse free-look feel. Sliders apply live while
	// dragging (onLookChanged pushes the values into the Party) and persist on
	// release; the curve dropdowns apply + persist on selection. The page scrolls
	// once these run past its height.
	cf->Space(ui::Len::Fixed(kSetGroup));
	cf->Row<ui::Separator>(ui::Len::Fixed(kSetRule));
	cf->Space(ui::Len::Fixed(kSetGroup));
	cf->Row<ui::Label>(ui::Len::Fixed(kSetLabel), loc::Tr("settings.mouselook"));
	auto lookSlider = [&](const char* key, float lo, float hi, float* field) {
		auto* s = cf->Row<ui::Slider>(
			ui::Len::Fixed(kSetSlider), loc::Tr(key), lo, hi,
			*field, [this, field](float v) {
				*field = v;
				if (onLookChanged) onLookChanged();
			});
		s->onRelease = [this] { m_settings.Save(); };
	};
	auto easeNames = [] {
		std::vector<std::string> names;
		for (const EaseOption& o : kLookEaseOptions) names.push_back(loc::Tr(o.labelKey));
		return names;
	};
	auto easeDrop = [&](const char* labelKey, Easing* field) {
		cf->Space(ui::Len::Fixed(kSetGroup));
		cf->Row<ui::Label>(ui::Len::Fixed(kSetLabel), loc::Tr(labelKey));
		cf->Row<ui::DropDown>(
			ui::Len::Fixed(kSetCtrl), easeNames(),
			LookEaseIndex(*field), [this, field](int index) {
				Click();
				if (index < 0 || index >= static_cast<int>(std::size(kLookEaseOptions)))
					return;
				*field = kLookEaseOptions[static_cast<size_t>(index)].value;
				if (onLookChanged) onLookChanged();
				m_settings.Save();
			});
	};
	lookSlider("settings.look_sensitivity", 0.25f, 3.0f, &m_settings.look.sensitivity);
	lookSlider("settings.look_hold", 0.0f, 2.0f, &m_settings.look.returnHold);
	lookSlider("settings.look_return", 0.2f, 5.0f, &m_settings.look.returnTime);
	easeDrop("settings.look_curve", &m_settings.look.snapEasing);
	lookSlider("settings.look_move", 0.05f, 1.5f, &m_settings.look.moveTime);
	easeDrop("settings.look_move_curve", &m_settings.look.moveEasing);

	// Controls → Hands: hand-slot behaviour. A checkbox — whether picking an
	// entry from a hand's right-click use menu also performs it (off = the menu
	// only sets the hand's left-click default) — and the Magic quick-cast
	// count (how many recently-cast spells the menu lists). Both persist
	// immediately.
	cf->Space(ui::Len::Fixed(kSetGroup));
	cf->Row<ui::Separator>(ui::Len::Fixed(kSetRule));
	cf->Space(ui::Len::Fixed(kSetGroup));
	cf->Row<ui::Label>(ui::Len::Fixed(kSetLabel), loc::Tr("settings.hands"));
	cf->Row<ui::Checkbox>(
		ui::Len::Fixed(kSetCtrl),
		loc::Tr("settings.usemenu_execute"), m_settings.useMenuExecutes,
		[this](bool on) {
			Click();
			m_settings.useMenuExecutes = on;
			m_settings.Save();
		});
	cf->Space(ui::Len::Fixed(kSetGroup));
	cf->Row<ui::Label>(ui::Len::Fixed(kSetLabel), loc::Tr("settings.spell_mru"));
	{
		std::vector<std::string> counts;
		for (int n = 1; n <= 10; ++n) counts.push_back(std::to_string(n));
		cf->Row<ui::DropDown>(
			ui::Len::Fixed(kSetCtrl),
			std::move(counts), m_settings.spellMruCount - 1, [this](int index) {
				Click();
				m_settings.spellMruCount = index + 1;
				m_settings.Save();
			});
	}

	// Video: the page overflows its height, so its stack is content-sized and
	// the TabControl scrolls. Each setting is a dim heading over its control.
	auto videoLabel = [&](const char* key) {
		vf->Space(ui::Len::Fixed(kSetGroup));
		vf->Row<ui::Label>(ui::Len::Fixed(kSetLabel), loc::Tr(key))->dim = true;
	};
	const gfx::AdapterInfo* selAdapter =
		(!m_adapters.empty() && m_selAdapter < static_cast<int>(m_adapters.size()))
			? &m_adapters[static_cast<size_t>(m_selAdapter)]
			: nullptr;
	const gfx::OutputInfo* selOutput =
		(selAdapter && m_selOutput < static_cast<int>(selAdapter->outputs.size()))
			? &selAdapter->outputs[static_cast<size_t>(m_selOutput)]
			: nullptr;

	// Adapter (GPU): a dropdown when several exist, otherwise just its name.
	videoLabel("settings.adapter");
	if (m_adapters.size() > 1) {
		std::vector<std::string> names;
		for (const gfx::AdapterInfo& a : m_adapters) names.push_back(a.name);
		vf->Row<ui::DropDown>(
			ui::Len::Fixed(kSetCtrl), std::move(names),
			m_selAdapter, [this](int index) {
				Click();
				if (index == m_selAdapter) return;
				m_selAdapter = index; // monitor/resolution lists depend on it
				m_selOutput = 0;
				m_selRes = 0;
				m_videoRebuildPending = true;
			});
	} else {
		vf->Row<ui::Label>(ui::Len::Fixed(kSetCtrl),
						   selAdapter ? selAdapter->name : std::string("—"));
	}
	// Monitor (output) of the selected adapter.
	videoLabel("settings.monitor");
	if (selAdapter && selAdapter->outputs.size() > 1) {
		std::vector<std::string> names;
		for (const gfx::OutputInfo& o : selAdapter->outputs) names.push_back(o.name);
		vf->Row<ui::DropDown>(
			ui::Len::Fixed(kSetCtrl), std::move(names),
			m_selOutput, [this](int index) {
				Click();
				if (index == m_selOutput) return;
				m_selOutput = index; // resolution list depends on the monitor
				m_selRes = 0;
				m_videoRebuildPending = true;
			});
	} else {
		vf->Row<ui::Label>(ui::Len::Fixed(kSetCtrl),
						   selOutput ? selOutput->name : std::string("—"));
	}
	// Resolution supported by the adapter/monitor combination.
	videoLabel("settings.resolution");
	{
		std::vector<std::string> resOptions;
		if (selOutput)
			for (const gfx::DisplayMode& m : selOutput->modes)
				resOptions.push_back(std::format("{} x {}", m.width, m.height));
		if (resOptions.empty()) resOptions.push_back("—");
		vf->Row<ui::DropDown>(
			ui::Len::Fixed(kSetCtrl), std::move(resOptions),
			m_selRes, [this](int index) {
				Click();
				m_selRes = index;
			});
	}
	// Display mode: Windowed / Borderless / Exclusive full-screen.
	videoLabel("settings.display_mode");
	vf->Row<ui::DropDown>(
		ui::Len::Fixed(kSetCtrl),
		std::vector<std::string>{loc::Tr("mode.windowed"), loc::Tr("mode.borderless"),
								 loc::Tr("mode.exclusive")},
		static_cast<int>(m_selMode), [this](int index) {
			Click();
			m_selMode = static_cast<gfx::FullscreenMode>(index);
		});
	// Apply the staged display selection (the only Video control that isn't
	// live). Narrower than a full row, like a button: a horizontal stack with
	// the leftover space beside it, rather than a width poked into the rect the
	// layout just handed back.
	vf->Space(ui::Len::Fixed(kSetGroup));
	{
		ui::Stack* row = vf->Row<ui::Stack>(ui::Len::Fixed(kSetCtrl), true);
		row->Row<ui::Button>(ui::Len::Fill(0.35f), loc::Tr("settings.apply"), [this] {
			Click();
			OnVideoApply();
		});
		row->Space(ui::Len::Fill(0.65f));
	}

	// Divider between the display section above and the rendering section below.
	vf->Space(ui::Len::Fixed(kSetGroup));
	vf->Row<ui::Separator>(ui::Len::Fixed(kSetRule));

	// Video: quality tier (hot-swaps meshes/textures in place).
	videoLabel("settings.quality");
	vf->Row<ui::DropDown>(
		ui::Len::Fixed(kSetCtrl),
		std::vector<std::string>{
			loc::Tr("settings.quality.low"), loc::Tr("settings.quality.medium"),
			loc::Tr("settings.quality.high"), loc::Tr("settings.quality.ultra")},
		static_cast<int>(m_settings.quality), [this](int index) {
			Click();
			onQualitySelected(index);
		});
	// Video: max dynamic lights. Quality resets this to its tier value (Low=16,
	// up to Ultra=64; SyncMaxLights re-points the dropdown afterward); picking a
	// value here overrides it until the next quality change.
	videoLabel("settings.maxlights");
	std::vector<std::string> lightOptions;
	for (int budget : kLightBudgets) lightOptions.push_back(std::to_string(budget));
	m_maxLightsDrop = vf->Row<ui::DropDown>(
		ui::Len::Fixed(kSetCtrl), std::move(lightOptions),
		GameSettings::LightBudgetIndex(m_settings.maxPointLights), [this](int index) {
			Click();
			m_settings.maxPointLights = kLightBudgets[index];
			m_settings.Save();
		});
	// Video: frame-rate cap. Each option presents every Nth monitor vblank, so
	// the rate is a tear-free divisor of the refresh (full = VSync, then half /
	// third / quarter). Capping below refresh cuts GPU load. Labels show the
	// resulting FPS from the live refresh rate; live (a present-interval change).
	videoLabel("settings.framelimit");
	const int refreshHz = m_device.RefreshHz();
	std::vector<std::string> fpsOptions;
	for (u32 interval : kPresentIntervals)
		fpsOptions.push_back(
			interval == 1
				? loc::Format("settings.framelimit.vsync", refreshHz)
				: loc::Format("settings.framelimit.fps",
							  (refreshHz + static_cast<int>(interval) / 2) /
								  static_cast<int>(interval)));
	vf->Row<ui::DropDown>(
		ui::Len::Fixed(kSetCtrl), std::move(fpsOptions),
		GameSettings::PresentIntervalIndex(m_settings.presentInterval),
		[this](int index) {
			Click();
			onFrameLimitSelected(index);
		});

	// Audio: master volume (the slider's label + track live inside its bounds).
	// Live while dragging; persisted once on release.
	auto* volume = af->Row<ui::Slider>(
		ui::Len::Fixed(kSetSlider),
		loc::Tr("settings.volume"), 0.0f, 1.0f, m_settings.volume, [this](float v) {
			m_settings.volume = v;
			m_audio.SetMasterVolume(v);
		});
	volume->onRelease = [this] { m_settings.Save(); };

	// UI → Textured UI: flips every context between the skinned chrome and the
	// flat theme-fill look (kept as a debug mode — containment/extents read at
	// a glance). Live — widgets re-check the skin pointer each draw.
	uf->Row<ui::Checkbox>(
		ui::Len::Fixed(kSetCtrl),
		loc::Tr("settings.uiskin"), m_settings.uiSkin, [this](bool on) {
			Click();
			m_settings.uiSkin = on;
			ApplySkin();
			m_settings.Save();
		});

	// (The Stone choice is its own tab now - GameUI_Stone.cpp.)

	// UI → Head bob: the walking camera's footfall dip/sway. Off for motion-
	// sensitive players; pushed to the Party via onHeadBobChanged.
	uf->Row<ui::Checkbox>(
		ui::Len::Fixed(kSetCtrl),
		loc::Tr("settings.headbob"), m_settings.headBob, [this](bool on) {
			Click();
			m_settings.headBob = on;
			if (onHeadBobChanged) onHeadBobChanged();
			m_settings.Save();
		});

	// UI → HUD panels: every floating panel's scale and background opacity
	// (kHudPanelFields), above them the layout lock and the reset. The panels
	// read their HudPanelLook live, so a slider needs no apply step; scale is
	// the SAME number the panel's corner grip edits (OnHudPanelMoved moves the
	// slider back to it). Apply while dragging, persist on release. Safe before
	// the HUD exists - nothing here touches a widget of it but the party slots,
	// whose list is empty until the first game load.
	uf->Space(ui::Len::Fixed(kSetGroup));
	uf->Row<ui::Label>(ui::Len::Fixed(kSetLabel), loc::Tr("settings.hud_panels"));
	// The layout (P4): Standard, or Minimal - the party bar and the hands
	// folded into one card per member. A switch rebuilds the HUD at once (it
	// lives in another context, so this callback cannot pull its own widget
	// out from under itself).
	uf->Row<ui::Label>(ui::Len::Fixed(kSetLabel), loc::Tr("settings.hud_layout"))->dim = true;
	uf->Row<ui::DropDown>(
		ui::Len::Fixed(kSetCtrl),
		std::vector<std::string>{loc::Tr("settings.layout_standard"),
								 loc::Tr("settings.layout_minimal")},
		m_settings.hudLayout, [this](int index) {
			Click();
			SetHudLayout(index);
		});
	uf->Row<ui::Checkbox>(
		ui::Len::Fixed(kSetCtrl),
		loc::Tr("settings.hud_lock"), m_settings.hudLocked, [this](bool on) {
			Click();
			m_settings.hudLocked = on;
			m_settings.Save();
		});
	auto* resetRow = uf->Row<ui::Stack>(ui::Len::Fixed(kSetCtrl), true);
	resetRow->Row<ui::Button>(ui::Len::Fill(), loc::Tr("settings.hud_reset"), [this] {
		Click();
		ResetHudLayout();
	});
	resetRow->Space(ui::Len::Fill());
	resetRow->Space(ui::Len::Fill());
	for (size_t i = 0; i < std::size(kHudPanelFields); ++i) {
		const HudPanelField& field = kHudPanelFields[i];
		HudPanelLook& look = m_settings.*(field.look);
		uf->Space(ui::Len::Fixed(kSetGroup));
		uf->Row<ui::Label>(ui::Len::Fixed(kSetLabel), loc::Tr(field.labelKey));
		auto* scale = uf->Row<ui::Slider>(
			ui::Len::Fixed(kSetSlider),
			loc::Tr("settings.bar_scale"), 0.5f, 1.5f, look.scale,
			[&look](float v) { look.scale = v; });
		scale->onRelease = [this] { m_settings.Save(); };
		m_hudScaleSliders[i] = scale;
		const bool partyBar = i == kHudParty;
		auto* opacity = uf->Row<ui::Slider>(
			ui::Len::Fixed(kSetSlider),
			loc::Tr("settings.bar_opacity"), 0.0f, 1.0f, look.opacity,
			[this, &look, partyBar](float v) {
				look.opacity = v;
				// The party slots keep their own copy (they fade only the slot
				// fills, and are built per member); every other panel reads it.
				if (partyBar)
					for (CharacterPanel* panel : m_partyPanels) panel->backgroundOpacity = v;
			});
		opacity->onRelease = [this] { m_settings.Save(); };
	}

	// UI → Theme Colors (kThemeFields): color pickers, three per row. Edits
	// recolor every context live (ApplyTheme) and persist once when a picker's
	// popup closes. (The resource bars had a grid here too; their fills are
	// procedural now, each with its own fixed colour - ResourceBarStyle.)
	//
	// A grid is rows of three: each row is a horizontal stack, so the columns
	// line up by construction. It used to be a block of hand-computed cells —
	// a column width, a row pitch, and an index-to-(row, column) formula — laid
	// over a single reserved rectangle whose height was a fourth calculation
	// that had to agree with the other three.
	auto colorGrid = [&](ui::Stack& page, size_t count, auto&& makePicker) {
		ui::Stack* row = nullptr;
		for (size_t i = 0; i < count; ++i) {
			if (i % 3 == 0) {
				row = page.Row<ui::Stack>(ui::Len::Fixed(kSetPicker), true);
				row->gapRem = 0.7f;
			}
			makePicker(*row, i);
		}
		// Pad the last row so a partial one keeps its cells the same width.
		if (row)
			for (size_t i = count % 3; i > 0 && i < 3; ++i) row->Space(ui::Len::Fill());
	};
	auto section = [&](ui::Stack& page, const char* labelKey) {
		page.Space(ui::Len::Fixed(kSetGroup));
		page.Row<ui::Separator>(ui::Len::Fixed(kSetRule));
		page.Space(ui::Len::Fixed(kSetGroup));
		page.Row<ui::Label>(ui::Len::Fixed(kSetLabel), loc::Tr(labelKey));
	};

	section(*uf, "settings.theme_colors");
	colorGrid(*uf, std::size(kThemeFields), [&](ui::Stack& row, size_t i) {
		const ThemeField& field = kThemeFields[i];
		auto* picker = row.Row<ui::ColorPicker>(
			ui::Len::Fill(), loc::Tr(field.labelKey), m_settings.theme.*(field.field),
			[this, member = field.field](const Vec4& color) {
				m_settings.theme.*member = color;
				ApplyTheme();
			});
		picker->onClose = [this] { m_settings.Save(); };
	});

	// UI → Party Colors: one picker per roster slot — the member's identity
	// color (portrait border, hand stripe, log tint). Edits land in the
	// settings (the master, member_<n>= in the ini) AND on the live roster,
	// so the HUD recolors immediately; persists when the popup closes.
	section(*uf, "settings.party_colors");
	colorGrid(*uf, kMemberColorCount, [&](ui::Stack& row, size_t i) {
		// Label with the member's name when the roster has the slot (proper
		// nouns, not localized); a slot number otherwise.
		const std::string label =
			i < m_characters.size() ? m_characters[i].name
									: loc::Format("settings.member_n", i + 1);
		auto* picker = row.Row<ui::ColorPicker>(
			ui::Len::Fill(), label, m_settings.memberColors[i],
			[this, i](const Vec4& color) {
				m_settings.memberColors[i] = color;
				if (i < m_characters.size())
					m_characters[i].portraitColor = color;
			});
		picker->onClose = [this] { m_settings.Save(); };
	});

	m_settingsUi.Add<ui::Button>(gfx::Rect{(1.0f - 0.14f) * 0.5f, kTabsY + kTabsH + 0.03f, 0.14f, 0.05f},
		loc::Tr("menu.back"), [this] {
			Click();
			m_menuPage = MenuPage::Main;
		});
}

// In-game pause menu (Esc while playing): same look as the landing list,
// drawn over the frozen scene under a dark wash (RenderPauseOverlay).
// Settings routes to the same shared page as the landing menu; Save/Load
// wait on the save system.
// Rebuilt on the same signal as the landing list: an in-game Save has to make
// Load appear, and deleting the last save has to take it away again.
void GameUI::BuildPauseMenu() {
	m_pauseUi.Clear(); // the list is this context's only content
	// Load only appears when at least one save exists; the list is sized to the
	// entries actually present (six with Load, five without).
	const bool hasSaves = !ListSaves().empty();
	m_menuHasSaves = hasSaves;
	const int itemCount = hasSaves ? 6 : 5;
	// A stone card, centred, with the title carved on it and the entries as
	// cut stones (Game/MenuPanel.h; more-ui-updates Phase 4). RenderPauseOverlay
	// no longer draws the title above it on this page.
	auto* panel = m_pauseUi.Add<MenuPanel>(loc::Tr("pause.title"),
										   static_cast<size_t>(itemCount), -1.0f);
	ui::MenuList* menu = panel->List();
	menu->AddItem(loc::Tr("menu.save"), [this] {
		Click();
		OpenSavesPage(SavesMode::Save);
	});
	if (hasSaves) {
		menu->AddItem(loc::Tr("menu.load"), [this] {
			Click();
			OpenSavesPage(SavesMode::Load);
		});
	}
	menu->AddItem(loc::Tr("menu.settings"), [this] {
		Click();
		m_menuPage = MenuPage::Settings;
	});
	// Out of THIS game and back to the title (Michael, 2026-09-24) — just above
	// Exit, the other way out. The game stays loaded, as after a party wipe, so
	// Continue / Load / Start New Game from there cost no reload. ASKED FIRST:
	// whatever has not been saved goes with it, and nothing brings it back.
	menu->AddItem(loc::Tr("menu.return_main"), [this] {
		Click();
		AskYesNo(loc::Tr("menu.return_main.ask"), loc::Tr("menu.return_main.lost"),
				 [this] { onReturnToMain(); });
	});
	menu->AddItem(loc::Tr("menu.exit"), [this] {
		Click();
		onQuit();
	});
	menu->AddItem(loc::Tr("menu.back"), [this] {
		Click();
		onResume();
	});
}

// Save-slot browser, shared by the landing/pause Load entries and the pause
// Save entry. Unlike the static pages this is rebuilt from disk every time it
// opens (saves come and go) — and again after a deletion (deferred, see
// m_savesDirty). Both modes show the slots in a scrolling SlotList with a
// per-row Delete: Load activates a row to load it; Save fills the name field
// from a row to overwrite it, above the name field + Save button.
void GameUI::OpenSavesPage(SavesMode mode) {
	m_savesMode = mode;
	m_overwriteArmed = false;
	m_saveField = nullptr;
	m_saveButton = nullptr;
	m_savesUi.Clear();

	const std::vector<SaveSlot> slots = ListSaves();

	// The page is a stone card with its title carved on it and its rows in one
	// Stack (more-ui-updates: the same treatment as the pause and title menus).
	// The slots are cut stones that press and act on release; Save and Back are
	// carved stones. Nothing below writes a coordinate.
	ui::Stack* col = SavesCard(mode == SavesMode::Save ? "saves.title_save" : "saves.title_load");

	// Builds the slots into the column's filling row. The list sits ABOVE the
	// Back row in add order, so Back is updated first: the list's delete confirm
	// takes the pointer through ClaimPopup, not by being added last.
	auto buildList = [&] {
		auto* list = col->Row<ui::SlotList>(ui::Len::Fill());
		list->rowHeight = kSlotRowRem;
		list->deleteIcon = m_deleteIcon.get();
		list->confirmPrompt = loc::Tr("saves.delete_prompt");
		list->deleteLabel = loc::Tr("saves.delete");
		list->cancelLabel = loc::Tr("saves.cancel");
		for (const SaveSlot& slot : slots) {
			ui::SlotList::Row row;
			row.primary = slot.name;
			// WHICH WORLD, beside when: the list holds every world's saves, and
			// loading one from another world switches to it.
			row.secondary = WorldTitle(slot.world) + "  ·  " + slot.timestamp;
			if (mode == SavesMode::Save)
				row.onActivate = [this, name = slot.name] {
					if (m_saveField) {
						m_saveField->text = name;
						m_saveField->SetFocused(true);
					}
					DisarmOverwrite();
				};
			else
				row.onActivate = [this, path = slot.path] {
					Click();
					m_menuPage = MenuPage::Main;
					onLoadSave(path);
				};
			row.onDelete = [this, path = slot.path] {
				Click(0.4f);
				std::error_code ec;
				std::filesystem::remove(path, ec);
				MarkSavesChanged(); // caught up next frame, never from here
			};
			list->AddRow(std::move(row));
		}
	};

	if (mode == SavesMode::Save) {
		m_saveField = col->Row<ui::TextField>(ui::Len::Fixed(kSlotRowRem - 0.2f),
			loc::Format("saves.default_name", slots.size() + 1));
		m_saveField->placeholder = loc::Tr("saves.name_placeholder");
		m_saveField->onChange = [this] { DisarmOverwrite(); };
		m_saveField->onSubmit = [this] { CommitSave(); };
		m_saveField->SetFocused(true);

		m_saveButton = col->Row<ui::Button>(ui::Len::Fixed(kSlotRowRem - 0.2f), loc::Tr("menu.save"),
			[this] { CommitSave(); });
		m_saveButton->carved = true;

		if (slots.empty()) {
			col->Space(ui::Len::Fill());
		} else {
			col->Row<ui::Label>(ui::Len::Fixed(1.25f), loc::Tr("saves.overwrite_label"))->dim = true;
			buildList();
		}
	} else if (slots.empty()) {
		col->Row<ui::Label>(ui::Len::Fixed(1.25f), loc::Tr("saves.none"))->dim = true;
		col->Space(ui::Len::Fill());
	} else {
		buildList();
	}
	SavesBackRow(*col);

	m_menuPage = MenuPage::Saves;
}

// The card the save, load and world pages stand on, and the column inside it:
// centred under the big title. The card carries the page's own title, so these
// pages draw no subtitle and the card starts where it would have been.
ui::Stack* GameUI::SavesCard(const char* titleKey) {
	constexpr float kCardW = 0.50f;
	constexpr float kCardTop = kMenuSubtitleY;
	constexpr float kCardBottom = kSavesCardBottom;
	auto* card = m_savesUi.Add<PageCard>(
		gfx::Rect{(1.0f - kCardW) * 0.5f, kCardTop, kCardW, kCardBottom - kCardTop},
		loc::Tr(titleKey));
	auto* col = card->Add<ui::Stack>(gfx::Rect{0, 0, 1, 1});
	col->gapRem = 0.4f;
	return col;
}

// A carved Back stone, centred, as the page's last row.
void GameUI::SavesBackRow(ui::Stack& col) {
	auto* row = col.Row<ui::Stack>(ui::Len::Fixed(kSlotRowRem - 0.2f), true);
	row->Space(ui::Len::Fill());
	row->Row<ui::Button>(ui::Len::Fixed(8.0f), loc::Tr("menu.back"), [this] {
		Click();
		m_menuPage = MenuPage::Main;
	})->carved = true;
	row->Space(ui::Len::Fill());
}

void GameUI::BeginNewGame(bool editor) {
	// WHICH WORLD first, when there is a choice to make. Asked at the click,
	// not at build: a world made in the editor since the list was built must
	// be offered.
	const std::vector<WorldChoice> worlds =
		onListWorlds ? onListWorlds() : std::vector<WorldChoice>{};
	if (worlds.size() > 1) {
		Click();
		m_worldsForEditor = editor;
		OpenWorldsPage();
		return;
	}
	Click(0.6f);
	if (onEditorOnArrival) onEditorOnArrival(editor);
	// One world: straight into THAT one - which is not necessarily the default
	// a harness start would open.
	if (worlds.size() == 1 && onStartNewGameIn) onStartNewGameIn(worlds.front().folder);
	else onStartNewGame();
}

// The new-game world list: one row per world, its title and (dim) its folder,
// the Load page's own list control so the two pages read as one family. No
// delete here — that is the editor's Worlds dialog, behind a typed name.
void GameUI::OpenWorldsPage() {
	m_savesUi.Clear();
	m_saveField = nullptr; // the Save page's pointers die with this context too
	m_saveButton = nullptr;
	const std::vector<WorldChoice> worlds =
		onListWorlds ? onListWorlds() : std::vector<WorldChoice>{};

	ui::Stack* col = SavesCard("worlds.title");
	auto* list = col->Row<ui::SlotList>(ui::Len::Fill());
	list->rowHeight = kSlotRowRem;
	for (const WorldChoice& w : worlds) {
		ui::SlotList::Row row;
		row.primary = w.display.empty() ? w.folder : w.display;
		// The folder, dim at the right — but only when it says something the
		// title does not (a world made in the editor is titled by its folder).
		row.secondary = row.primary == w.folder ? std::string() : w.folder;
		row.onActivate = [this, folder = w.folder] {
			Click(0.6f);
			m_menuPage = MenuPage::Main;
			if (onEditorOnArrival) onEditorOnArrival(m_worldsForEditor);
			onStartNewGameIn(folder);
		};
		list->AddRow(std::move(row)); // onDelete left null: no delete icon
	}
	SavesBackRow(*col);
	m_menuPage = MenuPage::Worlds;
}

// Save page: write the named slot, arming a one-shot overwrite confirm first
// if a save of that name already exists (the button label flips; a second
// click — or editing the name — clears it). Empty names fall back to a
// default so the file is never just ".dsav".
void GameUI::CommitSave() {
	if (!m_saveField) return;
	std::string name = m_saveField->text;
	if (name.empty()) name = loc::Tr("saves.untitled");

	if (std::filesystem::exists(SaveSlotPath(name)) && !m_overwriteArmed) {
		m_overwriteArmed = true;
		if (m_saveButton) m_saveButton->text = loc::Tr("saves.overwrite_confirm");
		return;
	}
	Click(0.6f);
	m_menuPage = MenuPage::Main;
	MarkSavesChanged(); // the first save has to make Load appear
	onSaveSlot(name);
}

void GameUI::DisarmOverwrite() {
	if (!m_overwriteArmed) return;
	m_overwriteArmed = false;
	if (m_saveButton) m_saveButton->text = loc::Tr("menu.save");
}

// Character details page (clicking a party-bar portrait): the sheet widget
// draws the page itself; prev/next buttons cycle the roster and Back (or
// Esc) resumes play. Like the pause menu it overlays the frozen scene.
void GameUI::BuildCharacterSheet() {
	// Parent-relative layout (fractions of the window) — no design-pixel Norm.
	// Centered panel, slightly above geometric center so the footer buttons fit.
	// 30% wider than it was, so the backpack tab has room for three real
	// columns: paper doll, defense numbers, backpack (CharacterSheetLayout.h
	// kWiden, which keeps the square cells square through the change).
	constexpr float kSheetW = 0.65f;
	// The tabs plus the status bar beneath them (the sheet owns the split, so it
	// can keep its tabs at the height they were authored at).
	constexpr float kSheetH = CharacterSheet::kBodyH + CharacterSheet::kStatusH;
	constexpr float kSheetX = (1.0f - kSheetW) * 0.5f;
	constexpr float kSheetY = (1.0f - kSheetH) * 0.5f - 0.03f;
	// The member arrows and "All" sit in a row under the card, inside the window.
	constexpr float kBtnGap = 0.02f, kBtnH = 0.045f, kBtnW = 0.05f;
	constexpr float kWindowH = kSheetH + kBtnGap + kBtnH;

	// A FLOATING WINDOW (ui-panels P3b): the card and its button row are one
	// panel the player moves (by the title band right of the portrait, the gap
	// above the buttons, or the move grip) and scales (the corner grip). Its
	// scale is the SHEET CONTEXT'S root font size (UpdateFonts), so rem itself
	// moves and every tab's rem-sized detail follows - not a fontScale on top.
	// Capped at 1.3: past it the window would outgrow a 16:9 screen.
	auto* layer = m_sheetUi.Add<ui::FloatingLayer>();
	layer->bounds = {0, 0, 1, 1};
	layer->onResetAll = [this] { ResetHudLayout(); };
	layer->resetTip = loc::Tr("hud.reset_layout");
	// The sheet floats over the HUD, so it lines up with the HUD's panels too.
	// Asked per drag: BuildHud replaces the HUD's layer.
	layer->snapPeer = [this] { return static_cast<const ui::FloatingLayer*>(m_hudLayer); };
	auto* window = layer->Add<ui::FloatingPanel>();
	window->debugName = "SheetPanel";
	window->posX = &m_settings.hudSheet.x;
	window->posY = &m_settings.hudSheet.y;
	window->scale = &m_settings.hudSheet.scale;
	window->minScale = 0.6f;
	window->maxScale = 1.3f;
	window->scalesText = false;
	window->locked = &m_settings.hudLocked;
	window->onChanged = [this] { OnHudPanelMoved(); };
	window->size = [](ui::UIContext& ctx, float s) {
		return Vec2{kSheetW * s * ctx.Width(), kWindowH * s * ctx.Height()};
	};
	window->defaultPos = [](ui::UIContext& ctx) {
		return Vec2{kSheetX * ctx.Width(), kSheetY * ctx.Height()};
	};
	m_hudPanels[kHudSheet] = window;

	// Added FIRST so the buttons below it update on top (and consume their clicks
	// before the sheet's slot hit-testing).
	m_sheet = window->Add<CharacterSheet>(gfx::Rect{0, 0, 1, kSheetH / kWindowH},
										  &m_characters,
											&m_barStyle, m_itemIcons,
											m_itemWeights, m_slotIcons,
											m_itemCategories, m_held);
	{
		std::array<const gfx::Texture*, 5> etch{}, lit{};
		for (size_t i = 0; i < etch.size(); ++i) {
			etch[i] = m_tabEtch[i].get();
			lit[i] = m_tabEtchLit[i].get();
		}
		m_sheet->SetModeEtches(etch, lit);
	}
	// A pack refused the held item: a soft thud + a "won't fit" log line. Item
	// names follow the item.<id> loc convention (same as ItemKind::nameKey).
	m_sheet->onRejectDrop = [this](const std::string& item, const std::string& pack) {
		m_audio.Play(m_sounds.bump, 0.5f);
		AddLogLine(loc::FormatLine("log.pack_rejects", loc::ViewKey("item.", item),
								   loc::ViewKey("item.", pack)));
	};
	// A hand doll cell refused a non-holdable item: same thud, the shared
	// "can't be held" line (also used by the control-bar hand slots).
	m_sheet->onRejectHold = [this](const std::string& item) {
		m_audio.Play(m_sounds.bump, 0.5f);
		AddLogLine(loc::FormatLine("log.cant_hold", loc::ViewKey("item.", item)));
	};
	m_sheet->opacity = &m_settings.hudSheet.opacity;
	m_sheet->defenseFor = [this](const Character& c) {
		return defenseFor ? defenseFor(c) : DefenseReadout{};
	};
	m_sheet->defenseWith = [this](const Character& c, const std::string& id) {
		return defenseWith ? defenseWith(c, id) : DefenseReadout{};
	};
	// The item mouse buttons on the sheet: right = details, middle = the use
	// menu (a rune memorizes from the pack too, not just a hand).
	m_sheet->onItemDetails = [this](ItemPlace place) { OpenItemDetails(m_sheetIndex, place); };
	m_sheet->onItemUse = [this](ItemPlace place) {
		if (m_sheetMenu) OpenItemUseMenu(m_sheetIndex, place, *m_sheetMenu);
	};
	// The Spells tab resolves learned-spell ids through the same registry the
	// spellbook uses (deferred so spellDefs is wired by cast time).
	m_sheet->spells = [this] {
		return spellDefs ? spellDefs()
						 : std::span<const std::unique_ptr<Spell>>{};
	};

	// The button row, in fractions of the window panel (authored as fractions
	// of the game window, divided through the panel's own extents).
	constexpr float btnY = (kSheetH + kBtnGap) / kWindowH, btnH = kBtnH / kWindowH;
	constexpr float btnW = kBtnW / kSheetW;
	// Previous / next member: the square arrow boxes (tools/BuildToolIcons.py),
	// loaded HERE rather than in a load task for the close box's reason above;
	// the "<" / ">" text shows only if the art is missing.
	window->Add<ui::Button>(gfx::Rect{0.0f, btnY, btnW, btnH}, "<",
							[this] {
								const size_t count = m_characters.size();
								onOpenSheet((m_sheetIndex + count - 1) % count);
							})->icon = ToolbarIcon(m_device, "box_left");
	window->Add<ui::Button>(gfx::Rect{1.0f - btnW, btnY, btnW, btnH}, ">", [this] {
		onOpenSheet((m_sheetIndex + 1) % m_characters.size());
	})->icon = ToolbarIcon(m_device, "box_right");
	// "All" → the party window, every member on this tab (Game/PartyWindow.h).
	m_sheetAll = window->Add<ui::Button>(gfx::Rect{0.06f / kSheetW, btnY, 0.08f / kSheetW, btnH},
							loc::Tr("ui.inv_all"), [this] {
								Click();
								if (onShowPartyInventory) onShowPartyInventory();
							});
	// Close (= resume) is the shared corner box at the sheet panel's top-right,
	// matching every other dialog — no footer Back button. In a slot INSIDE the
	// sheet, the way the editor dialogs reserve one: floated over the panel as a
	// sibling it sat on top of it, which is a widget claiming an area it does
	// not own however deliberate the corner looks.
	constexpr float kCloseW = 0.0525f, kCloseH = 0.0717f; // ~42x40px of the sheet
	auto* closeSlot = m_sheet->Add<ui::Box>(
		gfx::Rect{1.0f - kCloseW - 0.016f, 0.013f, kCloseW, kCloseH});
	closeSlot->debugName = "close";
	ui::AddCloseButton(*closeSlot, m_closeIcon, [this] {
		Click();
		onResume();
	});
	// The sheet's own context menu (the item use menus), added LAST so it
	// updates first and its popup draws over everything.
	m_sheetMenu = m_sheetUi.Add<ui::ContextMenu>();
	m_sheetMenu->onPick = [this](int id) { OnUseMenuPick(id); };
}

// Rebuilds every page in the active language (loc:: was just reloaded). The
// builders re-Add into cleared contexts, so all the raw widget pointers
// (m_sheet, m_keyBinds, m_log, ...) are re-pointed here. Rebuilding the HUD
// clears the message log; the movement help line is restored so the log
// isn't empty mid-game. Deferred to the top of a frame by Game — never run
// this from inside a widget callback.
void GameUI::RebuildForLanguage() {
	m_menuUi.Clear();
	m_settingsUi.Clear();
	m_stonePicker = nullptr; // died with the page; BuildStoneTab sets it again
	m_pauseUi.Clear();
	m_savesUi.Clear();
	m_sheetUi.Clear();
	BuildMenu();
	BuildPauseMenu();
	BuildCharacterSheet();
	if (m_itemDetails) { // its row labels are localized; what it showed is stale
		m_itemDetails->Close();
		m_itemDetails->Build();
	}
	// The saves page is built on demand; repopulate it in the new language if
	// it happens to be open (OpenSavesPage leaves m_menuPage on Saves).
	if (m_menuPage == MenuPage::Saves) OpenSavesPage(m_savesMode);
	if (m_menuPage == MenuPage::Worlds) OpenWorldsPage();
	if (!m_characters.empty()) {
		m_sheetIndex = std::min(m_sheetIndex, m_characters.size() - 1);
		m_sheet->SetCharacter(m_sheetIndex);
	}
	if (m_log) {
		m_hudUi.Clear();
		BuildHud();
		AddLogLine(m_settings.MoveKeysHelp());
		ResetHudStatus();
	}
}

// See the header: per-member HUD widgets are baked one-per-slot by BuildHud,
// so a roster whose size changed needs the bar/hand grid re-laid-out (the
// widgets themselves survive a resize safely — they resolve by index).
void GameUI::RebuildForRoster() {
	if (!m_characters.empty())
		m_sheetIndex = std::min(m_sheetIndex, m_characters.size() - 1);
	if (m_sheet) m_sheet->SetCharacter(m_sheetIndex);
	if (m_log) {
		m_hudUi.Clear();
		BuildHud();
		AddLogLine(m_settings.MoveKeysHelp());
		ResetHudStatus();
	}
}

void GameUI::SyncMaxLights() {
	if (m_maxLightsDrop)
		m_maxLightsDrop->SetSelected(
			GameSettings::LightBudgetIndex(m_settings.maxPointLights));
}

// ============================================================================
// Video tab: adapter / monitor / resolution / display-mode selection.
// ============================================================================

// Stage = the live settings, resolved against the enumerated hardware. Called
// when the page is built fresh (open or language rebuild) — NOT on the deferred
// repopulate, which must preserve the user's in-progress choice.
void GameUI::SeedVideoStaging() {
	if (m_adapters.empty()) m_adapters = gfx::EnumerateAdapters();

	// Adapter: the saved LUID, or (for "auto" = 0) the running device's adapter.
	const u64 want =
		m_settings.adapterLuid != 0 ? m_settings.adapterLuid : m_device.AdapterLuid();
	m_selAdapter = 0;
	for (size_t i = 0; i < m_adapters.size(); ++i)
		if (m_adapters[i].luid == want) {
			m_selAdapter = static_cast<int>(i);
			break;
		}

	const gfx::AdapterInfo* a =
		m_adapters.empty() ? nullptr : &m_adapters[static_cast<size_t>(m_selAdapter)];

	// Monitor.
	m_selOutput = 0;
	if (a && m_settings.displayOutput >= 0 &&
		m_settings.displayOutput < static_cast<int>(a->outputs.size()))
		m_selOutput = m_settings.displayOutput;

	// Resolution: match the saved size in the selected output's mode list.
	m_selRes = 0;
	if (a && m_selOutput < static_cast<int>(a->outputs.size())) {
		const auto& modes = a->outputs[static_cast<size_t>(m_selOutput)].modes;
		for (size_t i = 0; i < modes.size(); ++i)
			if (static_cast<int>(modes[i].width) == m_settings.displayWidth &&
				static_cast<int>(modes[i].height) == m_settings.displayHeight) {
				m_selRes = static_cast<int>(i);
				break;
			}
	}

	m_selMode = m_settings.fullscreen;
}

void GameUI::OnVideoApply() {
	if (m_adapters.empty() || m_selAdapter >= static_cast<int>(m_adapters.size()))
		return;
	const gfx::AdapterInfo& a = m_adapters[static_cast<size_t>(m_selAdapter)];

	// Resolve the staged resolution to a concrete width/height.
	u32 cw = 0, ch = 0;
	if (m_selOutput < static_cast<int>(a.outputs.size())) {
		const auto& modes = a.outputs[static_cast<size_t>(m_selOutput)].modes;
		if (m_selRes >= 0 && m_selRes < static_cast<int>(modes.size())) {
			cw = modes[static_cast<size_t>(m_selRes)].width;
			ch = modes[static_cast<size_t>(m_selRes)].height;
		}
	}

	if (a.luid != m_device.AdapterLuid()) {
		// A GPU change can't be done in place; confirm, then persist + relaunch.
		OpenConfirm(loc::Tr("confirm.restart.title"), loc::Tr("confirm.restart.body"),
					[this, luid = a.luid, out = m_selOutput, cw, ch, mode = m_selMode] {
						m_settings.adapterLuid = luid;
						m_settings.displayOutput = out;
						m_settings.displayWidth = static_cast<int>(cw);
						m_settings.displayHeight = static_cast<int>(ch);
						m_settings.fullscreen = mode;
						onAdapterRestart();
					});
		return;
	}

	// Same GPU: monitor / resolution / mode apply in place.
	m_settings.adapterLuid = a.luid;
	m_settings.displayOutput = m_selOutput;
	m_settings.displayWidth = static_cast<int>(cw);
	m_settings.displayHeight = static_cast<int>(ch);
	m_settings.fullscreen = m_selMode;
	onVideoApply();
}

void GameUI::OpenConfirm(const std::string& title, const std::string& body,
						 std::function<void()> onYes, std::function<void()> onNo,
						 const char* yesKey, const char* noKey) {
	m_confirmYes = std::move(onYes);
	m_confirmNo = std::move(onNo);
	m_confirmAnswer = 0;
	m_confirmUi.Clear();
	// Centered panel as window fractions.
	constexpr float kPW = 0.34f, kPH = 0.24f;
	constexpr float kPX = (1.0f - kPW) * 0.5f, kPY = (1.0f - kPH) * 0.5f;
	constexpr float kIn = 0.03f; // inset as fraction of window (~panel pad)
	m_confirmUi.Add<ui::Panel>(gfx::Rect{kPX, kPY, kPW, kPH});
	m_confirmUi.Add<ui::Label>(gfx::Rect{kPX + kIn, kPY + 0.03f, kPW - 2 * kIn, 0.04f}, title);
	m_confirmUi.Add<ui::Label>(gfx::Rect{kPX + kIn, kPY + 0.09f, kPW - 2 * kIn, 0.035f}, body)
		->dim = true;

	constexpr float kBW = 0.125f, kBH = 0.05f;
	const float by = kPY + kPH - kBH - 0.025f;
	// The buttons RECORD the answer; ResolveConfirm runs it after the update.
	m_confirmUi.Add<ui::Button>(gfx::Rect{kPX + kIn, by, kBW, kBH}, loc::Tr(yesKey),
								[this] { m_confirmAnswer = 1; });
	m_confirmUi.Add<ui::Button>(gfx::Rect{kPX + kPW - kBW - kIn, by, kBW, kBH},
								loc::Tr(noKey), [this] { m_confirmAnswer = 2; });

	m_confirmUi.SetTheme(m_settings.theme);
	m_confirmActive = true;
}

void GameUI::ResolveConfirm() {
	if (!m_confirmAnswer) return;
	const bool yes = m_confirmAnswer == 1;
	m_confirmAnswer = 0;
	m_confirmActive = false;
	// Moved out before it runs: the answer may itself ask the next question.
	std::function<void()> answer = std::move(yes ? m_confirmYes : m_confirmNo);
	m_confirmYes = {};
	m_confirmNo = {};
	Click();
	if (answer) answer();
}

void GameUI::AskYesNo(const std::string& title, const std::string& body,
					  std::function<void()> onYes, std::function<void()> onNo) {
	OpenConfirm(title, body, std::move(onYes), std::move(onNo), "world.ask.yes",
				"world.ask.no");
}

void GameUI::UpdatePrompt(const Input& input) {
	if (!m_confirmActive) return;
	// The keyboard answers too: Enter is how the world map already says "go
	// in", so it is the natural yes, and Esc is every screen's "back out".
	if (input.WasKeyPressed(VK_RETURN) || input.WasKeyPressed('Y')) m_confirmAnswer = 1;
	else if (input.WasKeyPressed(VK_ESCAPE) || input.WasKeyPressed('N')) m_confirmAnswer = 2;
	else m_confirmUi.Update(input, WindowW(), WindowH());
	ResolveConfirm();
}

void GameUI::ApplyPendingVideoRebuild() {
	if (!m_videoRebuildPending) return;
	m_videoRebuildPending = false;
	const int active = m_settingsTabs ? m_settingsTabs->ActiveTab() : 0;
	m_settingsUi.Clear();
	m_stonePicker = nullptr; // died with the page; BuildStoneTab sets it again
	BuildSettings(); // preserves the staged m_sel* (no SeedVideoStaging)
	if (m_settingsTabs) m_settingsTabs->SetActiveTab(active);
}

void GameUI::ShowSheet(size_t index) {
	m_sheetIndex = index;
	m_sheet->SetCharacter(index);
	// A use menu left open names a slot of whoever it was opened on; paging to
	// another member (or reopening the sheet) must not keep it.
	if (m_sheetMenu) m_sheetMenu->Close();
}

void GameUI::RefreshSheet() {
	m_sheet->SetCharacter(m_sheetIndex);
	// The party window shows the same things, a card per member.
	if (m_inventory && m_inventory->IsOpen()) m_inventory->Open(m_inventory->CurrentMode());
}

bool GameUI::OpenSpellbook(size_t i) { return m_spellbook && m_spellbook->Open(i); }

void GameUI::CloseSpellbook() {
	if (m_spellbook) m_spellbook->Close();
}

// --- dev: the widget trees by name (the console's `uitree dump`) -------------

ui::UIContext* GameUI::UiTree(std::string_view name) {
	if (name == "hud") return &m_hudUi;
	if (name == "menu") return &m_menuUi;
	if (name == "settings") return &m_settingsUi;
	if (name == "pause") return &m_pauseUi;
	if (name == "saves") return &m_savesUi;
	if (name == "sheet") return &m_sheetUi;
	if (name == "confirm") return &m_confirmUi;
	return nullptr;
}

std::string GameUI::UiTreeNames() {
	return "hud menu settings pause saves sheet confirm";
}

// ============================================================================
// HUD — authored in design pixels from the initial window size, stored as
// window fractions (Norm), so it scales with the screen. Widgets the game
// updates later are kept as raw pointers (m_log, m_compass, m_position); the
// UIContext owns all widgets.
// ============================================================================
void GameUI::BuildHud() {
	// CLEARED FIRST: a game load builds the HUD, and a second world's load
	// builds it again (docs/world-on-demand.md) — it used to append a second
	// copy on top. Every cached HUD pointer is reassigned below.
	m_hudUi.Clear();
	// Every HUD panel pointer dies with the clear - but not the sheet's, which
	// lives in the sheet's own context (BuildCharacterSheet owns that entry).
	for (size_t i = 0; i < m_hudPanels.size(); ++i)
		if (i != kHudSheet) m_hudPanels[i] = nullptr;

	// THE FLOATING PANELS (ui-panels P3a, UI/FloatingPanel.h). Every piece of
	// HUD chrome but the message log is a panel the player moves and resizes:
	// its spot and scale are GameSettings' (kHudPanelFields), a never-moved
	// panel sits at the DEFAULT spot below - the layout the HUD always had - and
	// its size at a scale is its content's. The numbers are window fractions
	// (kBarTop and friends, file scope), turned into pixels each layout.
	m_hudLayer = m_hudUi.Add<ui::FloatingLayer>();
	m_hudLayer->bounds = {0, 0, 1, 1};
	// Ctrl over any panel offers a button that puts EVERY panel home.
	m_hudLayer->onResetAll = [this] { ResetHudLayout(); };
	m_hudLayer->resetTip = loc::Tr("hud.reset_layout");
	// ... and, on a panel that minimizes, one that puts it in the tray (Phase
	// 8). A flag flips and the layout follows: nothing is rebuilt.
	m_hudLayer->onHideChanged = [this] { OnHudPanelHidden(false); };
	m_hudLayer->hideTip = loc::Tr("hud.minimize");
	auto makePanel = [this](size_t field, const char* name) {
		HudPanelLook& look = m_settings.*(kHudPanelFields[field].look);
		auto* panel = m_hudLayer->Add<ui::FloatingPanel>();
		panel->debugName = name;
		panel->posX = &look.x;
		panel->posY = &look.y;
		panel->scale = &look.scale;
		panel->locked = &m_settings.hudLocked;
		panel->onChanged = [this] { OnHudPanelMoved(); };
		if (kHudPanelFields[field].glyph) panel->hidden = &look.hidden;
		m_hudPanels[field] = panel;
		return panel;
	};
	// THE LAYOUT (P4): Standard = the party bar across the top and the Hands
	// dock in the right column; Minimal = neither, one card per member instead
	// (Game/MemberCards.h), the cards block where the Hands dock was.
	const bool minimal = m_settings.hudLayout == 1;

	// The defaults hang off the party bar's DEFAULT height at its scale - what
	// the old below-bar container did when the bar grew - so an unmoved left
	// column and dock column still start just under it. No bar, no gap for one.
	auto belowTop = [this, minimal](ui::UIContext& ctx) {
		if (minimal) return kBarTop * ctx.Height();
		return (kBarTop + kBarH0 * m_settings.hudParty.scale + kBarGap) * ctx.Height();
	};

	// THE PARTY LEADER (Phase 9): every member's name reads who leads and picks
	// on a click - in the bar and on a card alike, since both are this panel.
	m_leaderLink.leader = [this] { return partyLeader ? partyLeader() : 0; };
	m_leaderLink.pick = [this](size_t i) {
		Click();
		if (onPickLeader) onPickLeader(i);
	};
	m_leaderLink.leaderTip = loc::Tr("hud.leader_tip");
	m_leaderLink.pickTip = loc::Tr("hud.make_leader");

	// One member's party-bar slot (portrait, name, effects, bars), the same
	// widget in the bar and on a card.
	auto addMemberPanel = [this](ui::Widget& parent, size_t i) {
		CharacterPanel* panel = parent.Add<CharacterPanel>(
			gfx::Rect{}, &m_characters, i, &m_barStyle,
			m_hitSplats, m_itemIcons, [this, i] { OnPortraitClick(i); },
			[this, i] { OnPortraitRightClick(i); },
			[this, i] { OnPortraitBars(i); },
			[this, i] { OnPortraitEffects(i); });
		panel->SetLeaderLink(&m_leaderLink);
		return panel;
	};

	// The party bar. Its width only shrinks below scale 1 and is pinned at the
	// window's span above it, so past 1 the bar only grows taller - the
	// slider's old behaviour, kept: four portraits cannot get wider than the
	// screen.
	m_partyBar = nullptr;
	m_partyPanels.clear();
	if (!minimal) {
		ui::FloatingPanel* party = makePanel(kHudParty, "PartyPanel");
		m_partyBar = party->Add<PartyBar>(gfx::Rect{0, 0, 1, 1});
		party->size = [this](ui::UIContext& ctx, float s) {
			const float ws = std::min(s, 1.0f);
			constexpr float kMargin = 0.01f;
			constexpr float kGap0 = 0.006f;
			const float gap = kGap0 * ws;
			const float usable = 1.0f - 2 * kMargin - 3 * gap;
			const float barW = (usable * ws) + 3 * gap;
			m_partyBar->gap = gap / barW; // the same pixel gap, as a bar fraction
			return Vec2{barW * ctx.Width(), kBarH0 * s * ctx.Height()};
		};
		party->defaultPos = [party](ui::UIContext& ctx) {
			const Vec2 size = party->size(ctx, party->Scale());
			return Vec2{(ctx.Width() - size.x) * 0.5f, kBarTop * ctx.Height()};
		};
		for (size_t i = 0; i < m_characters.size() && i < PartyBar::kSlots; ++i) {
			CharacterPanel* panel = addMemberPanel(*m_partyBar, i);
			panel->backgroundOpacity = m_settings.hudParty.opacity;
			m_partyPanels.push_back(panel);
		}
	}

	// Left column: the status plate (compass + position) over the options plate
	// (torchlight + Rest/Help). Two padded panels, each laying its own rows out
	// as fractions of itself.
	constexpr float kLeftX = 0.01f, kLeftW = 0.15f;
	constexpr float kStatusH = 0.071f, kOptionsH = 0.16f;
	constexpr float kOptionsGap = 0.013f;

	ui::FloatingPanel* statusPanel = makePanel(kHudStatus, "StatusPanel");
	statusPanel->size = [](ui::UIContext& ctx, float s) {
		return Vec2{kLeftW * s * ctx.Width(), kStatusH * s * ctx.Height()};
	};
	statusPanel->defaultPos = [belowTop](ui::UIContext& ctx) {
		return Vec2{kLeftX * ctx.Width(), belowTop(ctx)};
	};
	auto* status = statusPanel->Add<ui::Panel>(gfx::Rect{0, 0, 1, 1});
	status->debugName = "StatusPlate";
	status->opacity = &m_settings.hudStatus.opacity;
	status->padX = 0.0467f; // 0.007 of the window, as a fraction of the plate
	status->padY = 0.1549f; // 0.011 likewise
	m_compass = status->Add<ui::Label>(gfx::Rect{0, 0, 1, 0.449f}, "");
	m_position = status->Add<ui::Label>(gfx::Rect{0, 0.551f, 1, 0.449f}, "");
	// SetHudStatus rewrites both every time the party turns or steps, and does it
	// by assigning into this storage. A line is clipped to loc::Line's capacity, so
	// taking that much here means the assignment can never need more.
	m_compass->text.reserve(loc::Line::kCapacity);
	m_position->text.reserve(loc::Line::kCapacity);
	m_position->dim = true;

	ui::FloatingPanel* optionsPanel = makePanel(kHudOptions, "OptionsPanel");
	optionsPanel->size = [](ui::UIContext& ctx, float s) {
		return Vec2{kLeftW * s * ctx.Width(), kOptionsH * s * ctx.Height()};
	};
	optionsPanel->defaultPos = [this, belowTop](ui::UIContext& ctx) {
		return Vec2{kLeftX * ctx.Width(),
					belowTop(ctx) + (kStatusH * m_settings.hudStatus.scale + kOptionsGap) *
										ctx.Height()};
	};
	auto* options = optionsPanel->Add<ui::Panel>(gfx::Rect{0, 0, 1, 1});
	options->debugName = "OptionsPlate";
	options->opacity = &m_settings.hudOptions.opacity;
	options->padX = 0.0600f; // 0.009 of the window
	options->padY = 0.0688f; // 0.011 of the window
	options->Add<ui::Label>(gfx::Rect{0, 0, 1, 0.1594f}, loc::Tr("hud.options"));
	auto* torchLabel = options->Add<ui::Label>(gfx::Rect{0, 0.2391f, 1, 0.1594f},
											   loc::Tr("hud.torchlight"));
	torchLabel->dim = true;
	options->Add<ui::DropDown>(gfx::Rect{0, 0.4348f, 1, 0.2101f},
		std::vector<std::string>{loc::Tr("torch.warm"), loc::Tr("torch.cold"),
								 loc::Tr("torch.eerie")},
		m_torchPalette, [this](int index) {
			Click();
			m_torchPalette = index;
			onTorchPalette(index);
		});
	constexpr float kHalfBtn = 0.4773f; // 0.063 of the window, of the inner width
	// REST — the toggle for the rest STATE (docs/health-and-healing.md). This
	// button was "Wait", which logged a line and did nothing else; rest is what
	// waiting was always a placeholder for, so it takes the slot rather than
	// crowding a second button in beside a dead one.
	//
	// The LABEL SHOWS THE ACTION, not the state — "Rest" while awake, "Wake"
	// while resting — the same convention the editor's play-pause button uses,
	// because a button labelled with the state it is already in reads as broken.
	// It is re-labelled every frame from the world, so the rest ending BY ITSELF
	// (fully recovered, attacked, out of food) puts the label back with no
	// callback and no chance of the two disagreeing.
	m_restButton =
		options->Add<ui::Button>(gfx::Rect{0, 0.7609f, kHalfBtn, 0.2246f},
			loc::Tr("hud.rest"), [this] {
				Click();
				if (onToggleRest) onToggleRest();
			});
	// Flush with the plate's inner right edge. It was authored at x 0.5379,
	// which plus the button's own 0.4773 comes to 1.0152 — three pixels out of
	// the plate, which is what `uioverlap` reported.
	options->Add<ui::Button>(gfx::Rect{1.0f - kHalfBtn, 0.7609f, kHalfBtn, 0.2246f},
		loc::Tr("hud.help"), [this] {
			Click();
			m_log->AddLine(m_settings.MoveKeysHelp());
			m_log->AddLine(loc::View("log.scroll_hint"));
		});

	// Movement, hands and magic: three docks, each its own floating panel
	// (Game/ControlBar.h). Their default column runs from under the party bar to
	// the top of the log footer, flush right.
	ControlBarDeps deps;
	deps.roster = &m_characters;
	deps.icons = m_itemIcons;
	deps.chevron = m_chevronTex.get();
	deps.chevron2 = m_chevron2Tex.get();
	for (size_t i = 0; i < m_moveEtch.size(); ++i) {
		deps.moveEtch[i] = m_moveEtch[i].get();
		deps.moveEtchLit[i] = m_moveEtchLit[i].get();
	}
	deps.lastMove = [this](MoveAction& action) -> unsigned {
		return moveCounter ? moveCounter(action) : 0u;
	};
	deps.boxMinus = ToolbarIcon(m_device, "box_minus");
	deps.minimizeTip = loc::Tr("hud.minimize");
	deps.onMove = [this](MoveAction action) { onMoveAction(action); };
	deps.onHandLeft = [this](size_t i, size_t hand) { OnHandLeftClick(i, hand); };
	deps.onHandRight = [this](size_t i, size_t hand) { OnHandRightClick(i, hand); };
	deps.onHandMiddle = [this](size_t i, size_t hand) { OnHandMiddleClick(i, hand); };
	deps.onHandHold = [this](size_t i, size_t hand) { OnHandHold(i, hand); };
	deps.handSetUse = [this](size_t i, size_t hand) { return HandSetUseFor(i, hand); };
	deps.useIcons = m_useIcons; // Game's stable bank, set before any HUD build
	deps.glow = m_glowTex.get();
	deps.onGuardChange = [this](size_t i, float share) {
		if (onGuardChange) onGuardChange(i, share);
	};
	deps.exertMax = [this] { return exertMax ? exertMax() : 1.0f; };
	deps.moveLabel = loc::Tr("hud.movement");
	deps.magicLabel = loc::Tr("hud.magic");
	// A dock's header button minimizes it into the tray, saved at once.
	deps.onHideChanged = [this] { OnHudPanelHidden(false); };
	deps.moveLook = &m_settings.hudMove;
	deps.handsLook = &m_settings.hudHands;
	deps.magicLook = &m_settings.hudMagic;
	deps.columnW = kControlW;
	deps.columnMargin = kControlMargin;
	// The docks start under the tray's strip, kept whether or not the tray is
	// showing, so minimizing a panel never moves the others' defaults.
	deps.columnTop = [this](ui::UIContext& ctx) { return DockColumnTop(ctx); };
	deps.columnBottom = [](ui::UIContext& ctx) { return (1.0f - kFooter) * ctx.Height(); };
	deps.locked = &m_settings.hudLocked;
	deps.onPlacementChanged = [this] { OnHudPanelMoved(); };
	if (minimal) {
		// The cards take the column under Movement, so Magic's default moves to
		// the LEFT column, under the options plate (at the plates' scale 1, so
		// resizing a plate never shifts or resizes it), running down to the log.
		deps.withHands = false;
		auto magicTop = [belowTop](ui::UIContext& ctx) {
			return belowTop(ctx) +
				   (kStatusH + kOptionsGap + kOptionsH + kOptionsGap) * ctx.Height();
		};
		deps.magicDefaultPos = [magicTop](ui::UIContext& ctx) {
			return Vec2{kLeftX * ctx.Width(), magicTop(ctx)};
		};
		deps.magicHeight1 = [magicTop](ui::UIContext& ctx) {
			return (1.0f - kFooter) * ctx.Height() - magicTop(ctx);
		};
	}
	const HudDocks docks = BuildHudDocks(*m_hudLayer, deps);
	m_hudPanels[kHudMove] = docks.move;
	m_hudPanels[kHudHands] = docks.hands;
	m_hudPanels[kHudMagic] = docks.magic;

	// THE PARTY CARDS (Minimal, Game/MemberCards.h): one floating block, two
	// cards across in formation order, where the Hands dock sits in Standard -
	// under Movement, flush right. Each card is the bar's member panel over that
	// member's hand pair; the card draws the one face behind both.
	if (minimal) {
		constexpr float kCardsW = 0.24f; // the block at scale 1, of the window width
		const size_t members = std::min<size_t>(m_characters.size(), PartyBar::kSlots);
		ui::FloatingPanel* cards = makePanel(kHudCards, "CardsPanel");
		cards->size = [cards, members](ui::UIContext& ctx, float s) {
			const float w = kCardsW * s * ctx.Width();
			return Vec2{w, CardGrid::Height(w, cards->EmAt(ctx, s), members)};
		};
		const std::function<float(ui::UIContext&)> slotTop = docks.handsTop;
		cards->defaultPos = [cards, slotTop](ui::UIContext& ctx) {
			const float w = kCardsW * cards->Scale() * ctx.Width();
			return Vec2{ctx.Width() * (1.0f - kControlMargin) - w, slotTop(ctx)};
		};
		auto* grid = cards->Add<CardGrid>();
		grid->bounds = {0, 0, 1, 1};
		for (size_t i = 0; i < members; ++i) {
			auto* card = grid->Add<MemberCard>(&m_characters, i, &m_settings.hudCards.opacity);
			CharacterPanel* panel = addMemberPanel(*card, i);
			panel->backgroundOpacity = 0.0f; // the card's face shows through
			card->SetPieces(panel, card->Add<HandPair>(gfx::Rect{}, i, deps));
		}
	}

	// THE CLOSED-PANELS TRAY (Phase 8, Game/HudTray.h): a button for each panel
	// of THIS layout that minimizes, shown while that panel is minimized. Its
	// default spot heads the right-hand column, its right edge on the column's
	// (the party bar's and the docks' - Michael), growing leftward as buttons
	// arrive, snug under the party bar (TrayTop). The docks start under the
	// strip it needs (DockColumnTop).
	{
		ui::FloatingPanel* trayPanel = makePanel(kHudTray, "TrayPanel");
		auto* tray = trayPanel->Add<HudTray>(&m_settings.hudTray.opacity);
		tray->bounds = {0, 0, 1, 1};
		for (size_t i = 0; i < kHudSheet; ++i) {
			const HudPanelField& field = kHudPanelFields[i];
			if (!field.glyph || !m_hudPanels[i]) continue;
			tray->AddPanel(m_hudPanels[i], &(m_settings.*(field.look)).hidden,
						   m_panelGlyphs[i].get(), loc::Tr(field.labelKey),
						   [this] { OnHudPanelHidden(true); });
		}
		trayPanel->size = [trayPanel, tray](ui::UIContext& ctx, float s) {
			return HudTray::Size(tray->ShownCount(), trayPanel->EmAt(ctx, s));
		};
		trayPanel->shownWhen = [tray] { return tray->ShownCount() > 0; };
		trayPanel->defaultPos = [this, trayPanel](ui::UIContext& ctx) {
			const float w = trayPanel->size(ctx, trayPanel->Scale()).x;
			return Vec2{ctx.Width() * (1.0f - kControlMargin) - w, TrayTop(ctx)};
		};
	}

	// The party window (more-ui-updates Phase 5, Game/PartyWindow.h): a
	// floating WINDOW (P3b) - the last panel on the layer, so it draws over the
	// others, and shown only while open. Centred until moved; the world stays
	// clickable around it. Sized in its own em, so its shape holds at any scale.
	ui::FloatingPanel* inventoryPanel = makePanel(kHudInventory, "InventoryPanel");
	// Its size follows the TAB (Phase 6): the Inventory cards carry the sheet's
	// own squares, so the window grows on that tab and shrinks back after.
	inventoryPanel->size = [this, inventoryPanel](ui::UIContext& ctx, float s) {
		const float em = inventoryPanel->EmAt(ctx, s);
		return m_inventory ? m_inventory->PanelSize(ctx, s, em, m_inventory->CurrentMode())
						   : PartyWindow::SizeForEm(em);
	};
	// Centred at its OTHER-tabs size, so its top-left - and with it the row of
	// tab stones - stays put while the size changes under a tab switch.
	inventoryPanel->defaultPos = [inventoryPanel](ui::UIContext& ctx) {
		const Vec2 size = PartyWindow::SizeForEm(inventoryPanel->EmAt(ctx, inventoryPanel->Scale()));
		return Vec2{(ctx.Width() - size.x) * 0.5f, (ctx.Height() - size.y) * 0.5f};
	};
	inventoryPanel->shownWhen = [this] { return m_inventory && m_inventory->IsOpen(); };
	m_inventory = inventoryPanel->Add<PartyWindow>(
		inventoryPanel, &m_characters, &m_barStyle, m_itemIcons, m_itemWeights, m_slotIcons,
		m_itemCategories, m_held, m_closeIcon, [this] {
			Click();
			CloseInventory();
		});
	m_inventory->bounds = {0, 0, 1, 1};
	m_inventory->squareDesign = [this] { return m_sheetUi.DesignHeight(); };
	m_inventory->opacity = &m_settings.hudInventory.opacity;
	{
		std::array<const gfx::Texture*, 5> etch{}, lit{};
		for (size_t i = 0; i < etch.size(); ++i) {
			etch[i] = m_tabEtch[i].get();
			lit[i] = m_tabEtchLit[i].get();
		}
		m_inventory->SetModeEtches(etch, lit);
	}
	// Each card is wired as the sheet is, but for ITS member: the item mouse
	// buttons (right = details, middle = the use menu), the refusals, the
	// defense readouts and the spell registry.
	for (size_t i = 0; i < PartyWindow::kMaxCards; ++i) {
		CharacterSheet* card = m_inventory->Card(i);
		card->onItemDetails = [this, i](ItemPlace place) { OpenItemDetails(i, place); };
		card->onItemUse = [this, i](ItemPlace place) {
			if (m_handMenu) OpenItemUseMenu(i, place, *m_handMenu);
		};
		card->onRejectDrop = [this](const std::string& item, const std::string& pack) {
			m_audio.Play(m_sounds.bump, 0.5f);
			AddLogLine(loc::FormatLine("log.pack_rejects", loc::ViewKey("item.", item),
									   loc::ViewKey("item.", pack)));
		};
		card->onRejectHold = [this](const std::string& item) {
			m_audio.Play(m_sounds.bump, 0.5f);
			AddLogLine(loc::FormatLine("log.cant_hold", loc::ViewKey("item.", item)));
		};
		card->defenseFor = [this](const Character& c) {
			return defenseFor ? defenseFor(c) : DefenseReadout{};
		};
		card->defenseWith = [this](const Character& c, const std::string& id) {
			return defenseWith ? defenseWith(c, id) : DefenseReadout{};
		};
		card->spells = [this] {
			return spellDefs ? spellDefs() : std::span<const std::unique_ptr<Spell>>{};
		};
	}

	m_spellbook = docks.spellbook;
	m_spellbook->onClick = [this] { Click(); };
	m_spellbook->SetActionIcons(m_castGlyphTex.get(), m_clearGlyphTex.get());
	m_spellbook->spells = [this] {
		return spellDefs ? spellDefs()
						 : std::span<const std::unique_ptr<Spell>>{};
	};
	m_spellbook->onCast = [this](size_t member, std::span<const SpellSymbol> seq) {
		Click();
		if (onCastSequence) onCastSequence(member, kBookHands, seq);
	};

	// Message log: screen-anchored, and it writes its own bounds each frame
	// (footer or restore button — see MessageLog.h).
	m_log = m_hudUi.Add<MessageLog>();
	m_log->restoreLabel = loc::Tr("hud.log_show");

	// LAST, so the menu updates first and draws over everything - the
	// inventory window included, whose use menu opens on top of it.
	m_handMenu = m_hudUi.Add<ui::ContextMenu>();
	m_handMenu->onPick = [this](int id) { OnUseMenuPick(id); };
	// Room for any item id, so opening the menu never grows it (see the member).
	m_handMenuItem.reserve(64);
}

void GameUI::OpenInventory(CharacterSheet::Mode mode) {
	if (m_inventory) m_inventory->Open(mode);
}
void GameUI::CloseInventory() { if (m_inventory) m_inventory->Close(); }
bool GameUI::InventoryOpen() const { return m_inventory && m_inventory->IsOpen(); }
CharacterSheet::Mode GameUI::InventoryMode() const {
	return m_inventory ? m_inventory->CurrentMode() : CharacterSheet::Mode::Inventory;
}
std::string_view GameUI::InventoryStatusName() const {
	return m_inventory ? m_inventory->StatusName() : std::string_view{};
}
std::string_view GameUI::InventoryStatusText() const {
	return m_inventory ? m_inventory->StatusText() : std::string_view{};
}
unsigned GameUI::InventoryOpens() const { return m_inventory ? m_inventory->Opens() : 0; }
gfx::Rect GameUI::InventoryStoneRect(size_t i) const {
	return m_inventory ? m_inventory->StoneRect(i) : gfx::Rect{};
}
bool GameUI::InventorySlotRect(size_t member, int slot, gfx::Rect& out) const {
	const CharacterSheet* card = m_inventory ? m_inventory->Card(member) : nullptr;
	if (!card || !card->visible) return false;
	out = card->PackSlotRect(slot);
	return true;
}

// A panel was dragged or resized: save, and mark the Settings sliders behind
// the scale the corner grip left (a slider and the grip edit one number). NOT
// synced here: this runs in a frame the allocation guard arms, and a slider
// rebuilds its readout text - AllocTest -Panels caught exactly that. The page
// is only ever seen from the menu or the pause screen, whose updates catch up
// (SyncHudPanelSlidersIfStale), so the sliders are right before they show.
void GameUI::OnHudPanelMoved() {
	m_settings.Save();
	m_hudSlidersStale = true;
}

// The tray's DEFAULT top: snug under the party bar at the bar's scale (Michael:
// "tighter to the party bar"), or the bar's own top in the Minimal layout,
// which has none. Asked every layout; the tray panel is built after the docks,
// so it is looked up, not captured.
float GameUI::TrayTop(ui::UIContext& ctx) const {
	if (m_settings.hudLayout == 1) return kBarTop * ctx.Height();
	const ui::FloatingPanel* tray = m_hudPanels[kHudTray];
	const float gap = tray ? tray->EmAt(ctx, 1.0f) * 0.3f : 0.0f;
	return (kBarTop + kBarH0 * m_settings.hudParty.scale) * ctx.Height() + gap;
}

// Where the right-hand column's docks start by default: under the tray's
// strip - one row at its scale and a dock gap - kept whether or not it shows.
float GameUI::DockColumnTop(ui::UIContext& ctx) const {
	const ui::FloatingPanel* tray = m_hudPanels[kHudTray];
	if (!tray) return TrayTop(ctx);
	return TrayTop(ctx) + HudTray::Size(1, tray->EmAt(ctx, tray->Scale())).y +
		   tray->EmAt(ctx, 1.0f) * 0.5f;
}

// A panel went into the tray, or came back out of it: a click, and the flag
// saved. Inside an armed frame like a drag's end; Save excuses itself.
void GameUI::OnHudPanelHidden(bool restored) {
	++(restored ? m_panelRestores : m_panelMinimizes);
	Click();
	m_settings.Save();
}

void GameUI::SyncHudPanelSlidersIfStale() {
	if (!m_hudSlidersStale) return;
	m_hudSlidersStale = false;
	SyncHudPanelSliders();
}

void GameUI::SyncHudPanelSliders() {
	for (size_t i = 0; i < m_hudScaleSliders.size(); ++i)
		if (m_hudScaleSliders[i])
			m_hudScaleSliders[i]->SetValue((m_settings.*(kHudPanelFields[i].look)).scale);
}

// Standard (0) or Minimal (1) - Settings -> UI "Layout", dev `hudpanel layout`.
// Saved, and the HUD rebuilt in the new shape if a game has built one (before
// that, the first BuildHud reads the setting). The rebuild is RebuildForRoster's:
// it restores the movement help line, but the message log starts afresh.
void GameUI::SetHudLayout(int layout) {
	layout = std::clamp(layout, 0, 1);
	if (layout == m_settings.hudLayout) return;
	m_settings.hudLayout = layout;
	m_settings.Save();
	RebuildForRoster();
}

// Settings -> UI "Reset HUD layout", and the reset button on a Ctrl-hovered
// panel: every panel back to its default spot and size, and out of the tray
// (Michael: "restore all"). Opacity is a look, not a layout, so it stays. The
// panel's button presses it inside an armed frame, so the sliders are only
// marked stale (OnHudPanelMoved) - the Settings page catches them up before it
// shows.
void GameUI::ResetHudLayout() {
	for (const HudPanelField& field : kHudPanelFields) {
		HudPanelLook& look = m_settings.*(field.look);
		look.x = look.y = -1.0f;
		look.scale = 1.0f;
		look.hidden = false;
	}
	OnHudPanelMoved();
}

// What the pointer shape should be this frame: a grip's arrow while one of
// panels [first, last) is hovered or dragging, else the plain arrow. A range,
// because only the context that just updated has a fresh answer - the HUD's
// panels while playing, the sheet's window while the sheet is up.
Window::Cursor GameUI::PanelCursor(size_t first, size_t last) const {
	for (size_t i = first; i < last && i < m_hudPanels.size(); ++i) {
		const ui::FloatingPanel* panel = m_hudPanels[i];
		if (!panel || !panel->visible) continue;
		if (panel->CursorWanted() == 1) return Window::Cursor::SizeAll;
		if (panel->CursorWanted() == 2) return Window::Cursor::SizeNWSE;
	}
	return Window::Cursor::Arrow;
}

// ============================================================================
// Per-frame updates
// ============================================================================

void GameUI::TickResourceBars(float dt, bool noticed) {
	// Wrapped once an hour so the shader's float clock never loses precision;
	// the fills jump once at the wrap, which nobody watching a bar will catch.
	m_barStyle.clock = std::fmod(m_barStyle.clock + dt, 3600.0f);
	m_spriteBatch.SetTime(m_barStyle.clock);
	for (size_t i = 0; i < ResourceBarStyle::kMaxMembers; ++i) {
		const Character* c = RosterMember(&m_characters, i);
		const float target = !c ? 0.0f
							 : m_barStyle.pinnedBpm >= 0.0f ? m_barStyle.pinnedBpm
															: HeartRateTarget(*c, noticed);
		TickBarPulse(m_barStyle.pulse[i], target, dt);
	}
}

// Keeps fonts in step with the window height so text scales with the
// normalized UI. Re-bakes are debounced until the height has settled for
// kFontSettleDelay (each one drains the GPU), then run between frames (never
// while a command list records); until then text simply renders at the old
// size inside the already-scaled widgets.
void GameUI::UpdateFonts(float dt) {
	const float windowH = static_cast<float>(m_window.Height());
	if (windowH != m_fontWindowH) {
		m_fontWindowH = windowH;
		m_fontSettle = 0.0f;
	} else if (m_fontSettle < kFontSettleDelay &&
			   (m_fontSettle += dt) >= kFontSettleDelay) {
		m_fontScale = windowH / kFontDesignWindowH;
	}
	UpdateSkinScale();

	// Re-resolve every context every frame. This is a map lookup per context,
	// not a re-bake: the library returns the SAME Font while the role's face and
	// the settled scale are unchanged. Doing it unconditionally means a face
	// swapped live (the audition) is picked up next frame with no invalidation
	// dance, and — unlike the seven hand-written SetHeight lines this replaces —
	// a context cannot be left out. m_savesUi was exactly that bug: constructed
	// and themed, but absent from the old list, so the saves page never tracked
	// the window height and never committed new glyphs.
	m_hudUi.UseFont(ui::FontRole::Body, kHudFontH * m_fontScale);
	m_menuUi.UseFont(ui::FontRole::Body, kMenuFontH * m_fontScale);
	m_settingsUi.UseFont(ui::FontRole::Body, kMenuFontH * m_fontScale);
	m_pauseUi.UseFont(ui::FontRole::Body, kMenuFontH * m_fontScale);
	m_savesUi.UseFont(ui::FontRole::Body, kMenuFontH * m_fontScale);
	// The sheet is a floating window whose scale IS its context's root font
	// size (BuildCharacterSheet): the panel's clamped scale, so a slider set
	// past the sheet's cap scales the text no further than the window.
	const float sheetScale = m_hudPanels[kHudSheet] ? m_hudPanels[kHudSheet]->Scale() : 1.0f;
	m_sheetUi.UseFont(ui::FontRole::Body, kSheetFontH * m_fontScale * sheetScale);
	m_confirmUi.UseFont(ui::FontRole::Body, kMenuFontH * m_fontScale);
	m_titleFont = &m_fonts.Get(ui::FontRole::Display, kTitleFontH * m_fontScale);

	// Flush any glyphs cached during last frame's draw/measure to the GPU. Runs
	// every frame (cheap no-op when nothing new was seen), before any widget
	// draws this frame — the safe between-frames point the atlas upload needs.
	// One call now covers every font in the game, dialogs and console included.
	m_fonts.CommitAll();
}

// A save was written or deleted. Two things go stale, and each catches up at
// its own moment, so the flags are set together and cleared apart.
void GameUI::MarkSavesChanged() {
	m_savesDirty = true;        // the open browser, if one is open
	m_menuEntriesDirty = true;  // Continue / Load, which need a save to exist
}

// A deletion last frame asks for a fresh page; rebuild here, before any widget
// updates, so the list isn't cleared from inside its own callback.
void GameUI::RefreshSavesIfDirty() {
	if (m_savesDirty && m_menuPage == MenuPage::Saves) {
		m_savesDirty = false;
		OpenSavesPage(m_savesMode);
	}
}

// Continue / Load (landing) and Load (pause) are hidden when no save exists,
// and both lists were built once at startup — so deleting the last save left a
// Continue that loaded nothing, and writing the FIRST save never grew a Load
// entry until the next launch. Re-filter whenever the saves have changed.
//
// Deferred to the same top-of-frame point as RefreshSavesIfDirty: rebuilding a
// list destroys the widgets, so it must never run from inside one of their
// callbacks. Gated on the list page actually being the one showing, which also
// means a browser the player is still looking at is never yanked away — the
// flag simply waits until they come back.
void GameUI::RefreshMenuEntriesIfDirty() {
	if (!m_menuEntriesDirty || m_menuPage != MenuPage::Main) return;
	m_menuEntriesDirty = false;
	const bool hasSaves = !ListSaves().empty();
	if (hasSaves == m_menuHasSaves) return; // the entries would come out the same
	BuildMenuList();
	BuildPauseMenu();
}

void GameUI::UpdateMenu(const Input& input) {
	RefreshSavesIfDirty();
	RefreshMenuEntriesIfDirty();
	SyncHudPanelSlidersIfStale();
	if (m_confirmActive) { // modal: freeze the page beneath it
		m_confirmUi.Update(input, WindowW(), WindowH());
		ResolveConfirm();
		return;
	}
	MenuContext().Update(input, WindowW(), WindowH());
}

void GameUI::UpdatePause(const Input& input) {
	RefreshSavesIfDirty();
	RefreshMenuEntriesIfDirty();
	SyncHudPanelSlidersIfStale();
	if (m_confirmActive) {
		// The keyboard answers here too (Enter/Y, N) — Esc has already been
		// taken by Game as a No, through CloseSettingsPage.
		UpdatePrompt(input);
		return;
	}
	PauseContext().Update(input, WindowW(), WindowH());
}

void GameUI::UpdateSheet(const Input& input, float dt) {
	m_hudMouseX = input.MouseX(); // for the held-item cursor over the sheet
	m_hudMouseY = input.MouseY();
	// The item details dialog is modal for the mouse: while it is up it gets the
	// update and the sheet under it holds still (the world does not - Game runs
	// it either way).
	if (ItemDetailsOpen()) {
		m_itemDetails->Update(input, WindowW(), WindowH(), dt);
		return;
	}
	m_sheetUi.Update(input, WindowW(), WindowH());
	m_hudCursor = PanelCursor(kHudSheet, kHudSheet + 1);

	// THE KEYBOARD PAGES THE SHEET (play-test #4 and #5, Michael 2026-09-28).
	// The party does not move while the sheet is open, so the strafe keys are
	// free: strafe-left / strafe-right step to the previous / next member, the
	// same as the < and > buttons (the BOUND keys, so a rebind carries over), and
	// Tab / Shift+Tab step through the tabs. Not while the item menu is open: it
	// names a slot of the member it was opened on.
	if (m_characters.empty() || (m_sheetMenu && m_sheetMenu->IsOpen())) return;
	const MoveKeys& keys = m_settings.moveKeys;
	const size_t count = m_characters.size();
	if (input.WasKeyPressed(keys.strafeLeft) && onOpenSheet)
		onOpenSheet((m_sheetIndex + count - 1) % count);
	else if (input.WasKeyPressed(keys.strafeRight) && onOpenSheet)
		onOpenSheet((m_sheetIndex + 1) % count);
	if (input.WasKeyPressed(VK_TAB) && m_sheet)
		m_sheet->StepMode(input.IsKeyDown(VK_SHIFT) ? -1 : +1);
}

void GameUI::UpdateHud(const Input& input, float dt) {
	m_hudMouseX = input.MouseX(); // stashed for the held-item cursor in RenderHud
	m_hudMouseY = input.MouseY();
	if (ItemDetailsOpen()) {
		// Modal for the mouse, as over the sheet; the log still ticks its fades.
		m_itemDetails->Update(input, WindowW(), WindowH(), dt);
		if (m_log) m_log->Tick(dt);
		return;
	}
	m_hudUi.Update(input, WindowW(), WindowH());
	m_hudCursor = PanelCursor(0, kHudSheet); // the sheet's window is its own context
	// The log reads this frame's hover/scroll (set during Update above) to
	// advance its fades and expand/collapse animation.
	if (m_log) m_log->Tick(dt);
}

// The two labels are rewritten by a STEP or a TURN, which is exactly the kind
// of thing the memory rule stopped exempting (docs/message-allocation.md): a
// std::string built by loc::Format and handed to a label allocated whenever the
// text outgrew the one already there — "Position: 13, 19" is one character past
// the small-string buffer, so walking east out of a single-digit column paid for
// a heap block. Format into a loc::Line and ASSIGN, which reuses the label's own
// storage; the reserve below is what guarantees there is enough of it, once, at
// a point where allocating is free.
void GameUI::SetHudStatus(int facing, int gridX, int gridZ) {
	if (facing != m_lastFacing) {
		m_lastFacing = facing;
		m_compass->text.assign(
			loc::FormatLine("hud.facing", loc::View(Party::FacingName(facing))).View());
	}
	if (gridX != m_lastGridX || gridZ != m_lastGridZ) {
		m_lastGridX = gridX;
		m_lastGridZ = gridZ;
		m_position->text.assign(loc::FormatLine("hud.position", gridX, gridZ).View());
	}
}

void GameUI::SetHudStatus(const Party& party) {
	SetHudStatus(party.Facing(), party.GridX(), party.GridZ());
}

// The rest button's face, pushed from the world every frame rather than set by
// the callback that toggles it — because rest ends BY ITSELF as often as by a
// click (fully recovered, attacked, out of food), and a label maintained at the
// click site would be wrong every one of those times.
void GameUI::SetResting(bool resting) {
	if (!m_restButton || resting == m_restLabelState) return;
	m_restLabelState = resting;
	m_restButton->text = loc::Tr(resting ? "hud.wake" : "hud.rest");
}

void GameUI::ResetHudStatus() { m_lastFacing = m_lastGridX = m_lastGridZ = -1; }

// Backs out of any open sub-page (Settings or Saves) to the main list,
// returning true; false means the list itself is showing and the caller owns
// the Esc (quit / resume).
bool GameUI::CloseSettingsPage() {
	if (m_confirmActive) { // Esc cancels the restart confirm first
		m_confirmAnswer = 2; // a No — its callback runs, if it has one
		ResolveConfirm();
		return true;
	}
	if (m_menuPage == MenuPage::Main) return false;
	m_menuPage = MenuPage::Main;
	return true;
}

void GameUI::ResetToMainPage() { m_menuPage = MenuPage::Main; }

// Pause list is built once at construction (before any save exists); rebuild it
// on demand so the Load entry appears/disappears as saves are written/deleted.
void GameUI::RebuildPauseMenu() {
	m_pauseUi.Clear();
	BuildPauseMenu();
}

bool GameUI::KeyCaptureActive() const {
	for (const ui::KeyBind* bind : m_keyBinds)
		if (bind->IsCapturing()) return true;
	return false;
}

// The HUD log widget exists only once BuildHud has run (the last game-load
// task), but world feedback can be raised before then — a dev-console command
// against the loading world, say. A null m_log drops the line instead of
// crashing on it.
void GameUI::AddLogLine(std::string_view line) {
	if (m_log) m_log->AddLine(line);
}

void GameUI::AddLogLine(std::string_view line, const Vec4& memberColor) {
	if (!m_log) return;
	// Identity colors are bright now (ui-updates), so the old "double it" lift
	// would bleach most of them to white. A quarter of the way toward white is
	// enough for ink on the dark footer and keeps the hue - which is the point.
	const auto lift = [](float c) { return c + (1.0f - c) * 0.25f; };
	m_log->AddLine(line,
				   Vec4{lift(memberColor.x), lift(memberColor.y),
						lift(memberColor.z), 1.0f});
}

void GameUI::ClearLog() {
	if (m_log) m_log->Clear();
}

// ============================================================================
// Rendering — all 2D, inside the caller's SpriteBatch Begin/End.
// ============================================================================

// Progress bar + current step name, shared by both loading screens.
void GameUI::DrawLoadProgress(const LoadQueue& queue, float barY) {
	const float w = DeviceW();
	const float h = DeviceH();
	const ui::Theme& theme = m_menuUi.GetTheme();

	const gfx::Rect bar{w * 0.3f, barY, w * 0.4f, h * (14.0f / kFontDesignWindowH)};
	// Skinned: the track is a groove sunk into the stone (the item-socket face),
	// the progress filling its well; flat mode keeps the bordered fill.
	const ui::Skin* skin = m_menuUi.GetSkin();
	if (skin && skin->slot.texture) {
		const gfx::Rect track =
			ui::DrawSlotFace(m_menuUi, m_spriteBatch, bar, {0.0f, 0.0f, 0.0f, 1.0f});
		m_spriteBatch.DrawRect({track.x, track.y, track.w * queue.Progress(), track.h},
							   theme.accent);
	} else {
		m_spriteBatch.DrawRect(bar, theme.control);
		m_spriteBatch.DrawRect({bar.x, bar.y, bar.w * queue.Progress(), bar.h},
							   theme.accent);
		ui::DrawBorder(m_spriteBatch, bar, theme.panelBorder);
	}

	const std::string_view step = queue.CurrentLabel();
	ui::Font& font = m_menuUi.GetFont();
	const float stepW = font.MeasureWidth(step);
	font.Draw(m_spriteBatch, step, (w - stepW) * 0.5f, bar.y + bar.h * 2.0f,
			  theme.textDim);
}

// Title face, horizontally centered at y in the accent color — every title
// screen draws "DUNGEON" this way.
// Takes a VIEW, so a caller can hand it loc::View and pay nothing. Font's own
// Draw/MeasureWidth have always taken string_view; only this signature stood
// between them and the table's own storage.
void GameUI::DrawCenteredTitle(std::string_view text, float y) {
	const float titleW = m_titleFont->MeasureWidth(text);
	m_titleFont->Draw(m_spriteBatch, text, (DeviceW() - titleW) * 0.5f, y,
					 m_menuUi.GetTheme().accent);
}

void GameUI::RenderLoadingScreen(const LoadQueue& queue) {
	const float h = DeviceH();
	DrawCenteredTitle(loc::View("title"), h * 0.32f);
	DrawLoadProgress(queue, h * 0.52f);
}

// Shown between "Start New Game" and Playing: the title art again, washed
// darker than the menu so the bar and step names read clearly.
void GameUI::RenderGameLoadingScreen(const LoadQueue& queue) {
	const float w = DeviceW();
	const float h = DeviceH();
	const ui::Theme& theme = m_menuUi.GetTheme();

	m_spriteBatch.DrawSprite({0, 0, w, h}, {0, 0, 1, 1}, *m_titleBackground,
							 {1, 1, 1, 1});
	m_spriteBatch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.55f});

	DrawCenteredTitle(loc::View("title"), h * kMenuTitleY);

	const std::string_view subtitle = loc::View("loading.descending");
	ui::Font& font = m_menuUi.GetFont();
	const float subW = font.MeasureWidth(subtitle);
	font.Draw(m_spriteBatch, subtitle, (w - subW) * 0.5f,
			  h * kMenuSubtitleY, theme.textDim);

	DrawLoadProgress(queue, h * 0.56f);
}

void GameUI::RenderMenuOverlay() {
	const float w = DeviceW();
	const float h = DeviceH();
	const ui::Theme& theme = m_menuUi.GetTheme();

	// Baked title art, stretched to the window, with a light darkening wash
	// so the menu text stays readable over the bright portal.
	m_spriteBatch.DrawSprite({0, 0, w, h}, {0, 0, 1, 1}, *m_titleBackground,
							 {1, 1, 1, 1});
	m_spriteBatch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.30f});

	// Title + subtitle.
	DrawCenteredTitle(loc::View("title"), h * kMenuTitleY);

	// The saves and worlds pages carry their title on their stone card
	// (SavesCard), which stands where the subtitle would.
	if (m_menuPage != MenuPage::Saves && m_menuPage != MenuPage::Worlds) {
		const char* subKey = m_menuPage == MenuPage::Settings ? "menu.subtitle_settings" : "menu.subtitle";
		const std::string_view subtitle = loc::View(subKey);
		ui::Font& font = m_menuUi.GetFont();
		const float subW = font.MeasureWidth(subtitle);
		font.Draw(m_spriteBatch, subtitle, (w - subW) * 0.5f,
				  h * kMenuSubtitleY, theme.textDim);
	}

	MenuContext().Render(m_spriteBatch, w, h);
	RenderConfirmOverlay();
}

// The adapter-change restart confirm: a dark wash + the centered Yes/No modal,
// drawn on top of whichever settings page raised it (menu or pause).
void GameUI::RenderConfirmOverlay() {
	if (!m_confirmActive) return;
	const float w = DeviceW();
	const float h = DeviceH();
	m_spriteBatch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.55f});
	m_confirmUi.Render(m_spriteBatch, w, h);
}

// Esc pause: the frozen scene stays up behind a dark wash, with a menu list
// like the landing page. The settings page is the same one the landing menu
// uses (m_menuPage routes both).
void GameUI::RenderPauseOverlay() {
	const float w = DeviceW();
	const float h = DeviceH();
	const ui::Theme& theme = m_pauseUi.GetTheme();

	m_spriteBatch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.55f});

	// The main page's title is carved on its stone card (MenuPanel); the
	// settings and saves pages keep the floating one above them.
	if (m_menuPage != MenuPage::Main) DrawCenteredTitle(loc::View("pause.title"), h * kMenuTitleY);

	// Only Settings has a subtitle now: the saves page's title is on its card.
	if (m_menuPage == MenuPage::Settings) {
		const std::string_view subtitle = loc::View("menu.subtitle_settings");
		ui::Font& font = m_pauseUi.GetFont();
		const float subW = font.MeasureWidth(subtitle);
		font.Draw(m_spriteBatch, subtitle, (w - subW) * 0.5f,
				  h * kMenuSubtitleY, theme.textDim);
	}

	PauseContext().Render(m_spriteBatch, w, h);
	RenderConfirmOverlay();
}

// Portrait click: the frozen scene under a dark wash, with the sheet page
// (and its prev/next/Back buttons) on top.
void GameUI::RenderCharacterSheetOverlay() {
	const float w = DeviceW();
	const float h = DeviceH();

	m_spriteBatch.DrawRect({0, 0, w, h}, {0, 0, 0, 0.55f});
	m_sheetUi.Render(m_spriteBatch, w, h);
	DrawHeldCursor(); // a carried tablet can be dropped into the sheet's slots
}

// A carried tablet rides the cursor: paint its element icon at the mouse, over
// everything. Shared by the HUD and the (frozen) sheet so dropping works on both.
void GameUI::DrawHeldCursor() {
	if (!m_held || !m_held->has_value() || !m_itemIcons) return;
	const float s = DeviceH() * 0.072f; // ~20% larger than a slot icon reads
	const gfx::Rect dst{m_hudMouseX - s * 0.5f, m_hudMouseY - s * 0.5f, s, s};
	// A rune glows as it does in every socket (DrawItemIcon).
	DrawItemIcon(m_spriteBatch, dst, **m_held, m_itemIcons, 0.0f);
}

void GameUI::RenderHud() {
	m_hudUi.Render(m_spriteBatch, DeviceW(), DeviceH());
	DrawHeldCursor();
}

} // namespace dungeon::game
