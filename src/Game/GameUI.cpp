// ============================================================================
// Game/GameUI.cpp — see GameUI.h.
// ============================================================================
#include "Game/GameUI.h"

#include "Core/Loc.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/SaveGame.h"
#include "Graphics/DisplayEnum.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <iterator>

namespace dungeon::game {

namespace {

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

// Widget bounds are normalized fractions of their container (see Widget.h).
// Layouts here are still authored in design pixels for readability and
// converted with this helper; they scale with the live window size.
gfx::Rect Norm(const gfx::Rect& designPx, const gfx::Rect& container) {
	return {(designPx.x - container.x) / container.w,
			(designPx.y - container.y) / container.h, designPx.w / container.w,
			designPx.h / container.h};
}

// Vertical stack with CSS-style collapsing margins: the gap between two items is
// max(upper.marginBottom, lower.marginTop) — never the sum — so equal margins on
// neighbours overlap into one. Place() returns the next item's design-px rect (a
// fixed column x/width); Norm turns it into page fractions. The first item gets
// no top margin (it sits at startY, like a block whose top margin collapses
// through its parent's padding). Used to lay out the settings tabs.
class Flow {
public:
	Flow(float x, float width, float startY) : m_x(x), m_w(width), m_y(startY) {}
	gfx::Rect Place(float height, float marginTop, float marginBottom) {
		if (m_started) m_y += std::max(m_prevBottom, marginTop);
		m_started = true;
		const gfx::Rect r{m_x, m_y, m_w, height};
		m_y += height;
		m_prevBottom = marginBottom;
		return r;
	}
	float Cursor() const { return m_y; } // y just below the last item

private:
	float m_x, m_w, m_y;
	float m_prevBottom = 0.0f;
	bool m_started = false;
};

} // namespace

GameUI::GameUI(Window& window, gfx::GraphicsDevice& device,
			   gfx::SpriteBatch& spriteBatch, audio::AudioEngine& audio,
			   const SoundBank& sounds, GameSettings& settings,
			   std::vector<Character>& characters)
	: m_window(window), m_device(device), m_spriteBatch(spriteBatch),
	  m_audio(audio), m_sounds(sounds), m_settings(settings),
	  m_characters(characters), m_hudUi(device, "", kHudFontH),
	  m_menuUi(device, "", kMenuFontH), m_settingsUi(device, "", kMenuFontH),
	  m_pauseUi(device, "", kMenuFontH), m_savesUi(device, "", kMenuFontH),
	  m_sheetUi(device, "", kSheetFontH), m_confirmUi(device, "", kMenuFontH),
	  m_titleFont(device, "", kTitleFontH) {}

void GameUI::BuildStaticUi() {
	ApplyTheme();
	BuildMenu();
	BuildPauseMenu();
	BuildCharacterSheet();
}

void GameUI::LoadTitleArt() {
	m_titleBackground = LoadTextureFile(m_device, paths::Asset("ui\\title_bg"));
	// Small UI glyph; optional (the SlotList falls back to a text "X").
	m_deleteIcon = TryLoadTextureFile(m_device, paths::Asset("ui\\delete"));
}

void GameUI::ApplyTheme() {
	for (ui::UIContext* ctx :
		 {&m_hudUi, &m_menuUi, &m_settingsUi, &m_pauseUi, &m_savesUi, &m_sheetUi,
		  &m_confirmUi})
		ctx->SetTheme(m_settings.theme);
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
			AddLogLine(loc::Format("log.pack_rejects", loc::Tr("item." + **m_held),
								   loc::Tr("item." + packId)));
		} else if (c.inventory.Stow(**m_held)) {
			AddLogLine(loc::Format("log.stow", c.name,
								   loc::Tr(std::format("item.{}", **m_held))));
			m_held->reset();
			Click();
		} else {
			AddLogLine(loc::Tr("log.pack_full")); // full — keep carrying it
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

void GameUI::OnHandLeftClick(size_t i, size_t hand) {
	if (i >= m_characters.size() || hand > 1) return;
	ItemSlot& slot = m_characters[i].inventory.Hand(static_cast<int>(hand));
	if (Holding()) {
		// Place the carried tablet in this hand, swapping any occupant onto the
		// cursor (so a click never silently destroys an item).
		std::string incoming = **m_held;
		if (slot.Empty()) m_held->reset();
		else *m_held = slot.typeId;
		slot.typeId = std::move(incoming);
		Click();
		return;
	}
	if (!slot.Empty()) { // empty-handed: pick this hand's item up onto the cursor
		*m_held = slot.typeId;
		slot.Clear();
		Click();
		return;
	}
	// Empty hand, empty cursor: nothing happens (for now). The hand's "activate"
	// gesture (unarmed attack, onHandAttack) is being moved off the left click.
}

void GameUI::OnHandRightClick(size_t i, size_t hand) {
	if (i >= m_characters.size() || hand > 1) return;
	const ItemSlot& slot = m_characters[i].inventory.Hand(static_cast<int>(hand));
	if (slot.Empty() || !m_handMenu) return;
	// Build the action menu from the item's data-driven command list (ItemKind::
	// commands, supplied by Game). Each command id maps to a label + handler here;
	// an unknown id is skipped (no entry) so adding a command is data + one case.
	const std::vector<std::string> cmds =
		itemCommands ? itemCommands(slot.typeId) : std::vector<std::string>{};
	std::vector<ui::ContextMenu::Entry> entries;
	for (const std::string& cmd : cmds) {
		if (cmd == "memorize")
			entries.push_back({loc::Tr("ui.memorize"),
							   [this, i, hand] { MemorizeFromHand(i, hand); }});
		else if (cmd == "eat")
			entries.push_back({loc::Tr("ui.eat"),
							   [this, i, hand] { EatFromHand(i, hand); }});
	}
	if (entries.empty()) return; // nothing actionable — don't pop an empty menu
	m_handMenu->Open(m_hudMouseX, m_hudMouseY, std::move(entries));
}

void GameUI::MemorizeFromHand(size_t i, size_t hand) {
	if (i >= m_characters.size() || hand > 1) return;
	ItemSlot& slot = m_characters[i].inventory.Hand(static_cast<int>(hand));
	SpellSymbol sym;
	if (!RuneSymbolFromItemId(slot.typeId, sym)) return;
	m_characters[i].Learn(sym);
	slot.Clear(); // the tablet is consumed
	Click();
	AddLogLine(loc::Format("log.memorize", m_characters[i].name,
						   loc::Tr(SymbolKey(sym))));
	RefreshSheet(); // the sheet's known symbols may be on screen later
}

void GameUI::EatFromHand(size_t i, size_t hand) {
	if (i >= m_characters.size() || hand > 1) return;
	Character& c = m_characters[i];
	ItemSlot& slot = c.inventory.Hand(static_cast<int>(hand));
	if (slot.Empty()) return;
	// Food restores a fraction of max stamina (scale-independent). A per-food
	// nutrition value is a future catalog field; flat for this first slice.
	constexpr float kRestoreFrac = 0.25f;
	c.stamina = std::min(c.maxStamina, c.stamina + kRestoreFrac * c.maxStamina);
	// Localized food name by the item.<id> convention (matches ItemKind::nameKey).
	const std::string foodName = loc::Tr(std::format("item.{}", slot.typeId));
	slot.Clear(); // the food is consumed
	Click();
	AddLogLine(loc::Format("log.eat", c.name, foodName));
	RefreshSheet(); // stamina bar / carry load on the sheet may be on screen
}

// ============================================================================
// Landing page — title plus a MenuList; entries highlight on mouse hover or
// keyboard selection. All entries are wired: Continue loads the newest save,
// Load opens the saves browser, Start New Game and Settings work as labeled.
// ============================================================================
void GameUI::BuildMenu() {
	const float w = static_cast<float>(m_window.Width());
	const float h = static_cast<float>(m_window.Height());
	const gfx::Rect window{0, 0, w, h};

	// Continue and Load only appear when at least one save exists, so the list
	// is sized to whatever entries are present (one quarter each with all four,
	// half each with just Start + Settings).
	const bool hasSaves = !ListSaves().empty();
	const int itemCount = hasSaves ? 4 : 2;
	const float menuW = 420.0f;
	const float itemH = 58.0f;
	auto* menu = m_menuUi.Add<ui::MenuList>(
		Norm({(w - menuW) * 0.5f, h * 0.42f, menuW, itemH * itemCount}, window),
		1.0f / static_cast<float>(itemCount));

	// Order: Continue / Load (only when a save exists), then Start New Game just
	// above Settings. Continue loads the most recent save outright (no browser).
	if (hasSaves) {
		menu->AddItem(loc::Tr("menu.continue"), [this] {
			const std::vector<SaveSlot> slots = ListSaves();
			if (slots.empty()) return; // raced with a deletion
			Click(0.6f);
			m_menuPage = MenuPage::Main;
			onLoadSave(slots.front().path); // ListSaves is newest-first
		});
		menu->AddItem(loc::Tr("menu.load"), [this] {
			Click();
			OpenSavesPage(SavesMode::Load);
		});
	}
	menu->AddItem(loc::Tr("menu.start"), [this] {
		Click(0.6f);
		onStartNewGame();
	});
	menu->AddItem(loc::Tr("menu.settings"), [this] {
		Click();
		m_menuPage = MenuPage::Settings;
	});

	SeedVideoStaging(); // fresh edit: stage = applied settings
	BuildSettings();
}

// The shared settings page (landing + pause route to the same m_settingsUi).
// Split out of BuildMenu so a Video-tab adapter/monitor change can rebuild just
// this page (different dropdown structure) without touching the menu list.
void GameUI::BuildSettings() {
	const float w = static_cast<float>(m_window.Width());
	const float h = static_cast<float>(m_window.Height());
	const gfx::Rect window{0, 0, w, h};

	// Settings page: Game / Video / Audio tabs over a shared page area, with
	// a Back button beneath. Tab children are fractions of the PAGE (the
	// area below the strip); each row leaves room for the settings font.
	//
	// The page is authored in design pixels against a 900px-tall window and
	// SCALED by the live window height (uiScale). The page fonts scale the SAME
	// way (UpdateFonts: kMenuFontH * windowH / kFontDesignWindowH), so rows and
	// text stay in step at any resolution — without this a taller window grows
	// the font past its fixed-height row and the labels collide. `page` stays in
	// unscaled design units (children keep their design-px layout); only the
	// TabControl's pixel size scales, which scales every child with it.
	const float uiScale = h / kFontDesignWindowH;
	const float designW = 760.0f;
	// Taller header strip so longer tab labels have room; designH grows with it so
	// the content area below keeps its height (designH - stripH = 492 design px).
	const float stripH = 60.0f;
	const float designH = 552.0f;
	const float tabsWpx = designW * uiScale;
	const float tabsHpx = designH * uiScale;
	const float tabsX = (w - tabsWpx) * 0.5f;
	const float tabsY = h * 0.29f;
	auto* tabs = m_settingsUi.Add<ui::TabControl>(
		Norm({tabsX, tabsY, tabsWpx, tabsHpx}, window), stripH / designH);
	m_settingsTabs = tabs; // kept so a Video repopulate restores the active tab
	const size_t tabGame = tabs->AddTab(loc::Tr("settings.tab.game"));
	const size_t tabControls = tabs->AddTab(loc::Tr("settings.tab.controls"));
	const size_t tabVideo = tabs->AddTab(loc::Tr("settings.tab.video"));
	const size_t tabAudio = tabs->AddTab(loc::Tr("settings.tab.audio"));
	const size_t tabUi = tabs->AddTab(loc::Tr("settings.tab.ui"));
	const gfx::Rect page{0, 0, designW, designH - stripH}; // child design space
	const float pad = 24.0f;
	const float rowW = page.w - 2 * pad;
	// A "setting" is a label stacked over its input control. Tabs lay out with a
	// Flow (collapsing-margin vertical stack, see the helper up top): mTight binds
	// a control to the label above it, mGroup separates settings/sections, mRow
	// paces the key-bind list. Margins collapse, so a control's mGroup bottom and
	// the next label's mGroup top overlap into one mGroup gap (not the sum). All
	// design px; Norm turns the Flow rects into 0..1 page fractions.
	const float labelH = 28.0f;  // a label row
	const float ctrlH = 40.0f;   // dropdown / button height
	const float sliderH = 50.0f; // self-contained slider (label line + track band)
	const float mTight = 12.0f;  // label -> its own control
	const float mRow = 14.0f;    // between list rows (key binds)
	const float mGroup = 24.0f;  // between settings / sections

	// Game: language. The language list is whatever assets/lang holds;
	// selecting one defers to Game (settings save + string reload +
	// RebuildForLanguage at the top of the next frame — rebuilding here
	// would destroy this dropdown mid-callback).
	Flow gf{pad, rowW, pad};
	tabs->AddChild<ui::Label>(tabGame, Norm(gf.Place(labelH, mGroup, mTight), page),
							  loc::Tr("settings.language"))
		->dim = true;
	m_languages = loc::ScanLanguages(paths::Asset("lang"));
	std::vector<std::string> languageNames;
	int languageIndex = 0;
	for (size_t i = 0; i < m_languages.size(); ++i) {
		languageNames.push_back(m_languages[i].name);
		if (m_languages[i].code == m_settings.language)
			languageIndex = static_cast<int>(i);
	}
	tabs->AddChild<ui::DropDown>(
		tabGame, Norm(gf.Place(ctrlH, mTight, mGroup), page), std::move(languageNames),
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
	Flow cf{pad, rowW, pad};
	tabs->AddChild<ui::Label>(tabControls, Norm(cf.Place(labelH, mGroup, mRow), page),
							  loc::Tr("settings.movement_keys"));
	m_keyBinds.clear();
	for (size_t i = 0; i < std::size(kKeyFields); ++i) {
		const KeyField& field = kKeyFields[i];
		auto* bind = tabs->AddChild<ui::KeyBind>(
			tabControls, Norm(cf.Place(36, mRow, mRow), page),
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
	tabs->AddChild<ui::Separator>(tabControls, Norm(cf.Place(1.0f, mGroup, mGroup), page));
	tabs->AddChild<ui::Label>(tabControls, Norm(cf.Place(labelH, mGroup, mTight), page),
							  loc::Tr("settings.mouselook"));
	auto lookSlider = [&](const char* key, float lo, float hi, float* field, float mTop,
						  float mBot) {
		auto* s = tabs->AddChild<ui::Slider>(
			tabControls, Norm(cf.Place(sliderH, mTop, mBot), page), loc::Tr(key), lo, hi,
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
		tabs->AddChild<ui::Label>(tabControls, Norm(cf.Place(labelH, mGroup, mTight), page),
								  loc::Tr(labelKey));
		tabs->AddChild<ui::DropDown>(
			tabControls, Norm(cf.Place(ctrlH, mTight, mGroup), page), easeNames(),
			LookEaseIndex(*field), [this, field](int index) {
				Click();
				if (index < 0 || index >= static_cast<int>(std::size(kLookEaseOptions)))
					return;
				*field = kLookEaseOptions[static_cast<size_t>(index)].value;
				if (onLookChanged) onLookChanged();
				m_settings.Save();
			});
	};
	lookSlider("settings.look_sensitivity", 0.25f, 3.0f, &m_settings.look.sensitivity,
			   mTight, mGroup);
	lookSlider("settings.look_hold", 0.0f, 2.0f, &m_settings.look.returnHold, mGroup, mGroup);
	lookSlider("settings.look_return", 0.2f, 5.0f, &m_settings.look.returnTime, mGroup,
			   mGroup);
	easeDrop("settings.look_curve", &m_settings.look.snapEasing);
	lookSlider("settings.look_move", 0.05f, 1.5f, &m_settings.look.moveTime, mGroup, mGroup);
	easeDrop("settings.look_move_curve", &m_settings.look.moveEasing);

	// Video: the page overflows its height, so the TabControl scrolls. A Flow
	// (collapsing-margin vertical stack) places each label-over-control setting.
	Flow vf{pad, rowW, pad};
	auto videoLabel = [&](const char* key) {
		tabs->AddChild<ui::Label>(tabVideo, Norm(vf.Place(labelH, mGroup, mTight), page),
								  loc::Tr(key))
			->dim = true;
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
		tabs->AddChild<ui::DropDown>(
			tabVideo, Norm(vf.Place(ctrlH, mTight, mGroup), page), std::move(names),
			m_selAdapter, [this](int index) {
				Click();
				if (index == m_selAdapter) return;
				m_selAdapter = index; // monitor/resolution lists depend on it
				m_selOutput = 0;
				m_selRes = 0;
				m_videoRebuildPending = true;
			});
	} else {
		tabs->AddChild<ui::Label>(tabVideo, Norm(vf.Place(ctrlH, mTight, mGroup), page),
								  selAdapter ? selAdapter->name : std::string("—"));
	}
	// Monitor (output) of the selected adapter.
	videoLabel("settings.monitor");
	if (selAdapter && selAdapter->outputs.size() > 1) {
		std::vector<std::string> names;
		for (const gfx::OutputInfo& o : selAdapter->outputs) names.push_back(o.name);
		tabs->AddChild<ui::DropDown>(
			tabVideo, Norm(vf.Place(ctrlH, mTight, mGroup), page), std::move(names),
			m_selOutput, [this](int index) {
				Click();
				if (index == m_selOutput) return;
				m_selOutput = index; // resolution list depends on the monitor
				m_selRes = 0;
				m_videoRebuildPending = true;
			});
	} else {
		tabs->AddChild<ui::Label>(tabVideo, Norm(vf.Place(ctrlH, mTight, mGroup), page),
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
		tabs->AddChild<ui::DropDown>(
			tabVideo, Norm(vf.Place(ctrlH, mTight, mGroup), page), std::move(resOptions),
			m_selRes, [this](int index) {
				Click();
				m_selRes = index;
			});
	}
	// Display mode: Windowed / Borderless / Exclusive full-screen.
	videoLabel("settings.display_mode");
	tabs->AddChild<ui::DropDown>(
		tabVideo, Norm(vf.Place(ctrlH, mTight, mGroup), page),
		std::vector<std::string>{loc::Tr("mode.windowed"), loc::Tr("mode.borderless"),
								 loc::Tr("mode.exclusive")},
		static_cast<int>(m_selMode), [this](int index) {
			Click();
			m_selMode = static_cast<gfx::FullscreenMode>(index);
		});
	// Apply the staged display selection (the only Video control that isn't live).
	gfx::Rect applyRect = vf.Place(ctrlH, mTight, mGroup);
	applyRect.w = 220.0f; // narrower than a full row, like a button
	tabs->AddChild<ui::Button>(tabVideo, Norm(applyRect, page),
							   loc::Tr("settings.apply"), [this] {
								   Click();
								   OnVideoApply();
							   });

	// Divider between the display section above and the rendering section below.
	tabs->AddChild<ui::Separator>(tabVideo,
								  Norm(vf.Place(1.0f, mGroup, mGroup), page));

	// Video: quality tier (hot-swaps meshes/textures in place).
	videoLabel("settings.quality");
	tabs->AddChild<ui::DropDown>(
		tabVideo, Norm(vf.Place(ctrlH, mTight, mGroup), page),
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
	m_maxLightsDrop = tabs->AddChild<ui::DropDown>(
		tabVideo, Norm(vf.Place(ctrlH, mTight, mGroup), page), std::move(lightOptions),
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
	tabs->AddChild<ui::DropDown>(
		tabVideo, Norm(vf.Place(ctrlH, mTight, mGroup), page), std::move(fpsOptions),
		GameSettings::PresentIntervalIndex(m_settings.presentInterval),
		[this](int index) {
			Click();
			onFrameLimitSelected(index);
		});

	// Audio: master volume (the slider's label + track live inside its bounds).
	// Live while dragging; persisted once on release.
	Flow af{pad, rowW, pad};
	auto* volume = tabs->AddChild<ui::Slider>(
		tabAudio, Norm(af.Place(sliderH, mGroup, mGroup), page),
		loc::Tr("settings.volume"), 0.0f, 1.0f, m_settings.volume, [this](float v) {
			m_settings.volume = v;
			m_audio.SetMasterVolume(v);
		});
	volume->onRelease = [this] { m_settings.Save(); };

	// UI → Party Bar: scale resizes the bar live (about its top center) and
	// opacity fades the slot backgrounds. Both apply while dragging and
	// persist on release; safe before the HUD exists (the panel list is empty
	// until the first game load).
	Flow uf{pad, rowW, pad};
	tabs->AddChild<ui::Label>(tabUi, Norm(uf.Place(labelH, mGroup, mTight), page),
							  loc::Tr("settings.party_bar"));
	auto* barScale = tabs->AddChild<ui::Slider>(
		tabUi, Norm(uf.Place(sliderH, mTight, mGroup), page),
		loc::Tr("settings.bar_scale"), 0.5f, 1.5f, m_settings.partyBarScale,
		[this](float v) {
			m_settings.partyBarScale = v;
			ApplyPartyBarScale();
		});
	barScale->onRelease = [this] { m_settings.Save(); };
	auto* barOpacity = tabs->AddChild<ui::Slider>(
		tabUi, Norm(uf.Place(sliderH, mGroup, mGroup), page),
		loc::Tr("settings.bar_opacity"), 0.0f, 1.0f, m_settings.partyBarOpacity,
		[this](float v) {
			m_settings.partyBarOpacity = v;
			for (CharacterPanel* panel : m_partyPanels)
				panel->backgroundOpacity = v;
		});
	barOpacity->onRelease = [this] { m_settings.Save(); };

	// UI → Theme Colors (kThemeFields) and Resource Bars (kBarFields): color
	// pickers, three per row. Theme edits recolor every context live
	// (ApplyTheme); bar edits show on the HUD widgets' next draw (they point
	// at the settings' barColors). Both persist once when a picker's popup
	// closes. Each grid is one Flow block (its rows are placed inside it).
	const float colW = (rowW - 2 * 16.0f) / 3.0f;
	const float pickRowH = 44.0f; // per grid row (3 pickers across)
	const float pickH = 36.0f;    // a picker swatch row
	auto gridHeight = [&](size_t count) {
		const size_t rows = (count + 2) / 3;
		return rows == 0 ? 0.0f : static_cast<float>(rows - 1) * pickRowH + pickH;
	};
	auto pickerCell = [&](size_t index, float blockTop) {
		return Norm({pad + (colW + 16.0f) * static_cast<float>(index % 3),
					 blockTop + pickRowH * static_cast<float>(index / 3), colW, pickH},
					page);
	};
	tabs->AddChild<ui::Separator>(tabUi, Norm(uf.Place(1.0f, mGroup, mGroup), page));
	tabs->AddChild<ui::Label>(tabUi, Norm(uf.Place(labelH, mGroup, mTight), page),
							  loc::Tr("settings.theme_colors"));
	const float themeTop =
		uf.Place(gridHeight(std::size(kThemeFields)), mTight, mGroup).y;
	size_t themeIndex = 0;
	for (const ThemeField& field : kThemeFields) {
		auto* picker = tabs->AddChild<ui::ColorPicker>(
			tabUi, pickerCell(themeIndex++, themeTop), loc::Tr(field.labelKey),
			m_settings.theme.*(field.field),
			[this, member = field.field](const Vec4& color) {
				m_settings.theme.*member = color;
				ApplyTheme();
			});
		picker->onClose = [this] { m_settings.Save(); };
	}
	tabs->AddChild<ui::Separator>(tabUi, Norm(uf.Place(1.0f, mGroup, mGroup), page));
	tabs->AddChild<ui::Label>(tabUi, Norm(uf.Place(labelH, mGroup, mTight), page),
							  loc::Tr("settings.resource_bars"));
	const float barTop = uf.Place(gridHeight(std::size(kBarFields)), mTight, mGroup).y;
	size_t barIndex = 0;
	for (const BarField& field : kBarFields) {
		auto* picker = tabs->AddChild<ui::ColorPicker>(
			tabUi, pickerCell(barIndex++, barTop), loc::Tr(field.labelKey),
			m_settings.barColors.*(field.field),
			[this, member = field.field](const Vec4& color) {
				m_settings.barColors.*member = color;
			});
		picker->onClose = [this] { m_settings.Save(); };
	}

	const float backW = 220.0f * uiScale;
	m_settingsUi.Add<ui::Button>(
		Norm({(w - backW) * 0.5f, tabsY + tabsHpx + 28.0f * uiScale, backW,
			  44.0f * uiScale},
			 window),
		loc::Tr("menu.back"), [this] {
			Click();
			m_menuPage = MenuPage::Main;
		});
}

// In-game pause menu (Esc while playing): same look as the landing list,
// drawn over the frozen scene under a dark wash (RenderPauseOverlay).
// Settings routes to the same shared page as the landing menu; Save/Load
// wait on the save system.
void GameUI::BuildPauseMenu() {
	const float w = static_cast<float>(m_window.Width());
	const float h = static_cast<float>(m_window.Height());
	const gfx::Rect window{0, 0, w, h};

	// Load only appears when at least one save exists; the list is sized to the
	// entries actually present (five with Load, four without).
	const bool hasSaves = !ListSaves().empty();
	const int itemCount = hasSaves ? 5 : 4;
	const float menuW = 420.0f;
	const float itemH = 58.0f;
	auto* menu = m_pauseUi.Add<ui::MenuList>(
		Norm({(w - menuW) * 0.5f, h * 0.42f, menuW, itemH * itemCount}, window),
		1.0f / static_cast<float>(itemCount));
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
	const float w = static_cast<float>(m_window.Width());
	const float h = static_cast<float>(m_window.Height());
	const gfx::Rect window{0, 0, w, h};

	const std::vector<SaveSlot> slots = ListSaves();

	const float colW = 720.0f;
	const float colX = (w - colW) * 0.5f;

	// Builds the slots into a fixed-height scroll box at [ly, ly+lh]. In Save
	// mode a row fills the name field (overwrite target); in Load mode it loads.
	// Every row's Delete opens a confirm dialog, then removes the file and flags
	// a deferred rebuild. Added LAST by the caller so its modal dialog claims
	// the mouse ahead of the page's other widgets.
	auto buildList = [&](float ly, float lh) {
		auto* list = m_savesUi.Add<ui::SlotList>(Norm({colX, ly, colW, lh}, window));
		list->deleteIcon = m_deleteIcon.get();
		list->confirmPrompt = loc::Tr("saves.delete_prompt");
		list->deleteLabel = loc::Tr("saves.delete");
		list->cancelLabel = loc::Tr("saves.cancel");
		for (const SaveSlot& slot : slots) {
			ui::SlotList::Row row;
			row.primary = slot.name;
			row.secondary = slot.timestamp;
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
				m_savesDirty = true; // rebuilt next frame (UpdateMenu/UpdatePause)
			};
			list->AddRow(std::move(row));
		}
	};

	// Layout pass: place every widget except the slot list, recording the list's
	// box so it can be added last (below).
	float backY = 0.0f;
	bool wantList = false;
	float listY = 0.0f, listH = 0.0f;
	if (mode == SavesMode::Save) {
		float y = h * 0.16f;
		m_saveField = m_savesUi.Add<ui::TextField>(
			Norm({colX, y, colW, 44}, window),
			loc::Format("saves.default_name", slots.size() + 1));
		m_saveField->placeholder = loc::Tr("saves.name_placeholder");
		m_saveField->onChange = [this] { DisarmOverwrite(); };
		m_saveField->onSubmit = [this] { CommitSave(); };
		m_saveField->SetFocused(true);
		y += 60.0f;

		m_saveButton = m_savesUi.Add<ui::Button>(
			Norm({colX, y, colW, 44}, window), loc::Tr("menu.save"),
			[this] { CommitSave(); });
		y += 76.0f;

		if (slots.empty()) {
			backY = y;
		} else {
			m_savesUi.Add<ui::Label>(Norm({colX, y, colW, 28}, window),
									 loc::Tr("saves.overwrite_label"))
				->dim = true;
			listY = y + 36.0f;
			listH = h * 0.40f;
			wantList = true;
			backY = listY + listH + 16.0f;
		}
	} else {
		listY = h * 0.24f;
		if (slots.empty()) {
			m_savesUi.Add<ui::Label>(Norm({colX, listY, colW, 28}, window),
									 loc::Tr("saves.none"))
				->dim = true;
			backY = listY + 56.0f;
		} else {
			listH = h * 0.52f;
			wantList = true;
			backY = listY + listH + 16.0f;
		}
	}

	const float backW = 220.0f;
	m_savesUi.Add<ui::Button>(
		Norm({(w - backW) * 0.5f, backY, backW, 44}, window), loc::Tr("menu.back"),
		[this] {
			Click();
			m_menuPage = MenuPage::Main;
		});

	// The list goes last so its modal confirm dialog claims the mouse before
	// the Back button / name field beneath it (UIContext updates topmost-first).
	if (wantList) buildList(listY, listH);

	m_menuPage = MenuPage::Saves;
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
	const float w = static_cast<float>(m_window.Width());
	const float h = static_cast<float>(m_window.Height());
	const gfx::Rect window{0, 0, w, h};

	const float sheetW = 780.0f;
	const float sheetH = 560.0f;
	const float sx = (w - sheetW) * 0.5f;
	const float sy = (h - sheetH) * 0.5f - 24.0f;
	// Added FIRST so the buttons below it update on top (and consume their clicks
	// before the sheet's slot hit-testing).
	m_sheet = m_sheetUi.Add<CharacterSheet>(Norm({sx, sy, sheetW, sheetH}, window),
											&m_titleFont, &m_settings.barColors,
											m_itemIcons, m_itemWeights, m_slotIcons,
											m_itemCategories, m_held);
	// A pack refused the held item: a soft thud + a "won't fit" log line. Item
	// names follow the item.<id> loc convention (same as ItemKind::nameKey).
	m_sheet->onRejectDrop = [this](const std::string& item, const std::string& pack) {
		m_audio.Play(m_sounds.bump, 0.5f);
		AddLogLine(loc::Format("log.pack_rejects", loc::Tr("item." + item),
							   loc::Tr("item." + pack)));
	};

	const float btnY = sy + sheetH + 16.0f;
	m_sheetUi.Add<ui::Button>(Norm({sx, btnY, 64, 40}, window), "<", [this] {
		const size_t count = m_characters.size();
		onOpenSheet((m_sheetIndex + count - 1) % count);
	});
	m_sheetUi.Add<ui::Button>(Norm({sx + sheetW - 64, btnY, 64, 40}, window), ">",
							  [this] {
								  onOpenSheet((m_sheetIndex + 1) %
											  m_characters.size());
							  });
	// "All" → the combined party-backpacks view (for cross-character swaps).
	m_sheetUi.Add<ui::Button>(Norm({sx + 80, btnY, 100, 40}, window),
							  loc::Tr("ui.inv_all"), [this] {
								  Click();
								  if (onShowPartyInventory) onShowPartyInventory();
							  });
	m_sheetUi.Add<ui::Button>(Norm({(w - 180.0f) * 0.5f, btnY, 180, 40}, window),
							  loc::Tr("menu.back"), [this] {
								  Click();
								  onResume();
							  });
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
	m_pauseUi.Clear();
	m_savesUi.Clear();
	m_sheetUi.Clear();
	BuildMenu();
	BuildPauseMenu();
	BuildCharacterSheet();
	// The saves page is built on demand; repopulate it in the new language if
	// it happens to be open (OpenSavesPage leaves m_menuPage on Saves).
	if (m_menuPage == MenuPage::Saves) OpenSavesPage(m_savesMode);
	if (!m_characters.empty()) m_sheet->SetCharacter(m_characters[m_sheetIndex]);
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
						 std::function<void()> onYes) {
	m_confirmUi.Clear();
	const float w = WindowW();
	const float h = WindowH();
	const gfx::Rect window{0, 0, w, h};

	// Scaled with the window height like the settings page (see BuildSettings).
	const float uiScale = h / kFontDesignWindowH;
	const float panelW = 540.0f * uiScale, panelH = 220.0f * uiScale;
	const float px = (w - panelW) * 0.5f, py = (h - panelH) * 0.5f;
	m_confirmUi.Add<ui::Panel>(Norm({px, py, panelW, panelH}, window));
	m_confirmUi.Add<ui::Label>(
		Norm({px + 24 * uiScale, py + 28 * uiScale, panelW - 48 * uiScale, 32 * uiScale},
			 window),
		title);
	m_confirmUi.Add<ui::Label>(
		Norm({px + 24 * uiScale, py + 84 * uiScale, panelW - 48 * uiScale, 28 * uiScale},
			 window),
		body)
		->dim = true;

	const float bw = 200.0f * uiScale, bh = 44.0f * uiScale,
				by = py + panelH - bh - 24.0f * uiScale;
	m_confirmUi.Add<ui::Button>(Norm({px + 24 * uiScale, by, bw, bh}, window),
								loc::Tr("confirm.yes"),
								[this, onYes = std::move(onYes)] {
									Click();
									m_confirmActive = false;
									onYes();
								});
	m_confirmUi.Add<ui::Button>(
		Norm({px + panelW - bw - 24 * uiScale, by, bw, bh}, window),
		loc::Tr("confirm.no"), [this] {
									Click();
									m_confirmActive = false;
								});

	m_confirmUi.SetTheme(m_settings.theme);
	m_confirmActive = true;
}

void GameUI::ApplyPendingVideoRebuild() {
	if (!m_videoRebuildPending) return;
	m_videoRebuildPending = false;
	const int active = m_settingsTabs ? m_settingsTabs->ActiveTab() : 0;
	m_settingsUi.Clear();
	BuildSettings(); // preserves the staged m_sel* (no SeedVideoStaging)
	if (m_settingsTabs) m_settingsTabs->SetActiveTab(active);
}

void GameUI::ShowSheet(size_t index) {
	m_sheetIndex = index;
	m_sheet->SetCharacter(m_characters[index]);
}

void GameUI::RefreshSheet() { m_sheet->SetCharacter(m_characters[m_sheetIndex]); }

// ============================================================================
// HUD — authored in design pixels from the initial window size, stored as
// window fractions (Norm), so it scales with the screen. Widgets the game
// updates later are kept as raw pointers (m_log, m_compass, m_position); the
// UIContext owns all widgets.
// ============================================================================
void GameUI::BuildHud() {
	const float w = static_cast<float>(m_window.Width());
	const float h = static_cast<float>(m_window.Height());
	const gfx::Rect window{0, 0, w, h};

	// Party bar across the top: one clickable slot per member (portrait,
	// name, health/stamina/mana). Clicking opens the character sheet. The
	// slot rects come from ApplyPartyBarScale (called below) so the Settings →
	// UI scale slider can resize the bar at runtime; the layout always
	// reserves four slots so a short roster keeps its slot size.
	m_hudDesignW = w;
	m_hudDesignH = h;
	m_partyPanels.clear();
	for (size_t i = 0; i < m_characters.size() && i < 4; ++i) {
		auto* panel = m_hudUi.Add<CharacterPanel>(
			gfx::Rect{}, &m_characters[i], &m_titleFont, &m_settings.barColors,
			m_hitSplats, [this, i] { OnPortraitClick(i); },
			[this, i] { OnPortraitRightClick(i); },
			[this, i] { OnPortraitBars(i); });
		panel->backgroundOpacity = m_settings.partyBarOpacity;
		m_partyPanels.push_back(panel);
	}
	const float barH = 96.0f; // design height at scale 1
	const float belowBar = 16.0f + barH + 16.0f; // top edge for the side panels

	// Widgets under the bar register here so ApplyPartyBarScale can shift
	// them when the bar grows or shrinks (design Y recovered from the
	// fraction Norm just stored).
	m_belowBarWidgets.clear();
	auto below = [this, h](ui::Widget* widget) {
		m_belowBarWidgets.push_back({widget, widget->bounds.y * h});
	};

	// The message log is a full-width footer added LAST (see the end of this
	// function) so it sits on top of the control panel when it expands.

	// Status labels, below the party bar on the left. Their text arrives via
	// SetHudStatus before the HUD's first visible frame.
	below(m_hudUi.Add<ui::Panel>(Norm({16, belowBar, 240, 64}, window)));
	m_compass = m_hudUi.Add<ui::Label>(Norm({28, belowBar + 10, 220, 20}, window), "");
	below(m_compass);
	m_position = m_hudUi.Add<ui::Label>(Norm({28, belowBar + 34, 220, 20}, window), "");
	m_position->dim = true;
	below(m_position);

	// Options panel (torchlight + wait/help), on the left under the status
	// panel — the right edge belongs to the control panel.
	const float optTop = belowBar + 76;
	below(m_hudUi.Add<ui::Panel>(Norm({16, optTop, 240, 144}, window)));
	below(m_hudUi.Add<ui::Label>(Norm({30, optTop + 10, 212, 20}, window),
								 loc::Tr("hud.options")));

	auto* torchLabel = m_hudUi.Add<ui::Label>(
		Norm({30, optTop + 40, 212, 20}, window), loc::Tr("hud.torchlight"));
	torchLabel->dim = true;
	below(torchLabel);
	below(m_hudUi.Add<ui::DropDown>(
		Norm({30, optTop + 64, 212, 26}, window),
		std::vector<std::string>{loc::Tr("torch.warm"), loc::Tr("torch.cold"),
								 loc::Tr("torch.eerie")},
		m_torchPalette, [this](int index) {
			Click();
			m_torchPalette = index;
			onTorchPalette(index);
		}));

	below(m_hudUi.Add<ui::Button>(Norm({30, optTop + 104, 101, 28}, window),
								  loc::Tr("hud.wait"), [this] {
									  Click();
									  m_log->AddLine(loc::Tr("log.wait"));
								  }));
	below(m_hudUi.Add<ui::Button>(Norm({141, optTop + 104, 101, 28}, window),
								  loc::Tr("hud.help"), [this] {
									  Click();
									  m_log->AddLine(m_settings.MoveKeysHelp());
									  m_log->AddLine(loc::Tr("log.scroll_hint"));
								  }));

	// Control panel down the right edge, Dungeon Master style: movement
	// arrows on top, then each member's two hands, then the (future) magic
	// area. The arrows feed the same discrete Party actions as the bound
	// keys — the world's own step/turn/bump feedback covers the audio, so
	// they skip the UI click.
	const float panelW = 250.0f;
	const float px = w - panelW - 16.0f;
	const float pad = 14.0f;
	const float innerW = panelW - 2 * pad;
	// The control panel stops short of the bottom so the full-width message
	// footer (added last, flush to the bottom edge) has its collapsed strip.
	const float footerReserve = 64.0f;
	const float panelBottom = h - footerReserve;
	below(m_hudUi.Add<ui::Panel>(
		Norm({px, belowBar, panelW, panelBottom - belowBar}, window)));

	// Movement arrows: turn left / forward / turn right over strafe left /
	// back / strafe right (the classic six), square, three across.
	const struct {
		const char* glyph;
		MoveAction action;
	} moves[] = {
		{"«", MoveAction::TurnLeft}, {"^", MoveAction::Forward},
		{"»", MoveAction::TurnRight}, {"<", MoveAction::StrafeLeft},
		{"v", MoveAction::Back},      {">", MoveAction::StrafeRight},
	};
	const float moveW = (innerW - 2 * 8.0f) / 3.0f;
	for (size_t i = 0; i < std::size(moves); ++i) {
		below(m_hudUi.Add<ui::Button>(
			Norm({px + pad + (moveW + 8.0f) * static_cast<float>(i % 3),
				  belowBar + 12 + (moveW + 8.0f) * static_cast<float>(i / 3),
				  moveW, moveW},
				 window),
			moves[i].glyph,
			[this, action = moves[i].action] { onMoveAction(action); }));
	}

	// Hand pairs, two members side by side per row (so the four-member
	// roster makes a 2x2 grid of sets): the name over a left and a right
	// hand box, square. The slots stay empty until items exist; clicking one
	// logs that.
	const float setW = (innerW - 8.0f) / 2.0f;
	const float handW = (setW - 4.0f) / 2.0f;
	const float setH = 20 + handW + 8;
	const float handsTop = belowBar + 12 + 2 * moveW + 8 + 14;
	for (size_t i = 0; i < m_characters.size() && i < 4; ++i) {
		const float setX = px + pad + (setW + 8.0f) * static_cast<float>(i % 2);
		const float setTop = handsTop + setH * static_cast<float>(i / 2);
		auto* name = m_hudUi.Add<ui::Label>(
			Norm({setX, setTop, setW, 18}, window), m_characters[i].name);
		name->dim = true;
		below(name);
		for (int hand = 0; hand < 2; ++hand) {
			below(m_hudUi.Add<HandSlot>(
				Norm({setX + (handW + 4.0f) * static_cast<float>(hand),
					  setTop + 20, handW, handW},
					 window),
				&m_characters[i], hand, m_itemIcons,
					// Left-click: place a held tablet here / pick this hand's item
					// up / (empty-handed, empty slot) swing this hand. Right-click
					// (a context menu) is wired in P6.
					[this, i, hand] { OnHandLeftClick(i, static_cast<size_t>(hand)); },
					[this, i, hand] { OnHandRightClick(i, static_cast<size_t>(hand)); }));
		}
	}
	const size_t handRows = (std::min<size_t>(m_characters.size(), 4) + 1) / 2;
	const float magicTop = handsTop + setH * static_cast<float>(handRows);

	// Reserved magic area (spellcasting comes later) — it takes whatever the
	// panel has left down to the window bottom.
	below(m_hudUi.Add<ui::Label>(Norm({px + pad, magicTop + 8, innerW, 20}, window),
								 loc::Tr("hud.magic")));
	below(m_hudUi.Add<ui::Panel>(
		Norm({px + pad, magicTop + 32, innerW, panelBottom - pad - (magicTop + 32)},
			 window)));
	auto* magicNone = m_hudUi.Add<ui::Label>(
		Norm({px + pad + 10, magicTop + 44, innerW - 20, 20}, window),
		loc::Tr("hud.magic_none"));
	magicNone->dim = true;
	below(magicNone);

	// Full-width message footer, flush to the bottom edge. Added last so it is
	// the topmost HUD widget — it claims the mouse first and draws over the
	// control panel when the player hovers it open. It sizes itself from the
	// window each frame, so its normalized bounds are nominal (full screen).
	m_log = m_hudUi.Add<MessageLog>();
	m_log->bounds = {0, 0, 1, 1};
	m_log->restoreLabel = loc::Tr("hud.log_show");

	// Right-click context menu + the inventory window, added last so they update
	// first (topmost) and their overlays draw over everything; both closed until
	// opened. The inventory window claims the mouse while open.
	m_handMenu = m_hudUi.Add<ui::ContextMenu>();
	m_inventory = m_hudUi.Add<InventoryWindow>(&m_characters, m_itemIcons, m_held);

	ApplyPartyBarScale();
}

void GameUI::OpenInventory() { if (m_inventory) m_inventory->Open(); }
void GameUI::CloseInventory() { if (m_inventory) m_inventory->Close(); }
bool GameUI::InventoryOpen() const { return m_inventory && m_inventory->IsOpen(); }

// Re-derives the party-bar slot rects from the settings scale — anchored to
// the top center, so scale 1 reproduces the design layout above (16px
// margins, 10px gaps, 96px tall) — and shifts the status/options widgets by
// the bar's growth so they stay 16px clear of its bottom edge. No-op until
// BuildHud has run (the settings sliders exist from boot, the HUD only after
// a game load).
void GameUI::ApplyPartyBarScale() {
	if (m_partyPanels.empty()) return;
	const float w = m_hudDesignW;
	const float h = m_hudDesignH;
	const gfx::Rect window{0, 0, w, h};
	const float s = m_settings.partyBarScale;
	// The bar already spans the full window at scale 1, so width is pinned
	// there: scales above 1 grow the bar vertically only (the portraits and
	// resource bars follow the slot height).
	const float ws = std::min(s, 1.0f);
	const float slotGap = 10.0f * ws;
	const float slotW = (w - 2 * 16.0f - 3 * 10.0f) / 4.0f * ws;
	const float barX = (w - 4 * slotW - 3 * slotGap) * 0.5f;
	for (size_t i = 0; i < m_partyPanels.size(); ++i)
		m_partyPanels[i]->bounds =
			Norm({barX + static_cast<float>(i) * (slotW + slotGap), 16.0f, slotW,
				  96.0f * s},
				 window);

	const float shift = 96.0f * (s - 1.0f);
	for (auto& [widget, designY] : m_belowBarWidgets)
		widget->bounds.y = (designY + shift) / h;
}

// ============================================================================
// Per-frame updates
// ============================================================================

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
		const float fontScale = windowH / kFontDesignWindowH;
		m_hudUi.GetFont().SetHeight(kHudFontH * fontScale);
		m_menuUi.GetFont().SetHeight(kMenuFontH * fontScale);
		m_settingsUi.GetFont().SetHeight(kMenuFontH * fontScale);
		m_pauseUi.GetFont().SetHeight(kMenuFontH * fontScale);
		m_confirmUi.GetFont().SetHeight(kMenuFontH * fontScale);
		m_sheetUi.GetFont().SetHeight(kSheetFontH * fontScale);
		m_titleFont.SetHeight(kTitleFontH * fontScale);
	}

	// Flush any glyphs cached during last frame's draw/measure to the GPU. Runs
	// every frame (cheap no-op when nothing new was seen), before any widget
	// draws this frame — the safe between-frames point the atlas upload needs.
	m_hudUi.GetFont().Commit();
	m_menuUi.GetFont().Commit();
	m_settingsUi.GetFont().Commit();
	m_pauseUi.GetFont().Commit();
	m_confirmUi.GetFont().Commit();
	m_sheetUi.GetFont().Commit();
	m_titleFont.Commit();
}

// A deletion last frame asks for a fresh page; rebuild here, before any widget
// updates, so the list isn't cleared from inside its own callback.
void GameUI::RefreshSavesIfDirty() {
	if (m_savesDirty && m_menuPage == MenuPage::Saves) {
		m_savesDirty = false;
		OpenSavesPage(m_savesMode);
	}
}

void GameUI::UpdateMenu(const Input& input) {
	RefreshSavesIfDirty();
	if (m_confirmActive) { // modal: freeze the page beneath it
		m_confirmUi.Update(input, WindowW(), WindowH());
		return;
	}
	MenuContext().Update(input, WindowW(), WindowH());
}

void GameUI::UpdatePause(const Input& input) {
	RefreshSavesIfDirty();
	if (m_confirmActive) {
		m_confirmUi.Update(input, WindowW(), WindowH());
		return;
	}
	PauseContext().Update(input, WindowW(), WindowH());
}

void GameUI::UpdateSheet(const Input& input) {
	m_sheetUi.Update(input, WindowW(), WindowH());
	m_hudMouseX = input.MouseX(); // for the held-item cursor over the sheet
	m_hudMouseY = input.MouseY();
}

void GameUI::UpdateHud(const Input& input, float dt) {
	m_hudUi.Update(input, WindowW(), WindowH());
	m_hudMouseX = input.MouseX(); // stashed for the held-item cursor in RenderHud
	m_hudMouseY = input.MouseY();
	// The log reads this frame's hover/scroll (set during Update above) to
	// advance its fades and expand/collapse animation.
	if (m_log) m_log->Tick(dt);
}

void GameUI::SetHudStatus(int facing, int gridX, int gridZ) {
	if (facing != m_lastFacing) {
		m_lastFacing = facing;
		m_compass->text =
			loc::Format("hud.facing", loc::Tr(Party::FacingName(facing)));
	}
	if (gridX != m_lastGridX || gridZ != m_lastGridZ) {
		m_lastGridX = gridX;
		m_lastGridZ = gridZ;
		m_position->text = loc::Format("hud.position", gridX, gridZ);
	}
}

void GameUI::SetHudStatus(const Party& party) {
	SetHudStatus(party.Facing(), party.GridX(), party.GridZ());
}

void GameUI::ResetHudStatus() { m_lastFacing = m_lastGridX = m_lastGridZ = -1; }

// Backs out of any open sub-page (Settings or Saves) to the main list,
// returning true; false means the list itself is showing and the caller owns
// the Esc (quit / resume).
bool GameUI::CloseSettingsPage() {
	if (m_confirmActive) { // Esc cancels the restart confirm first
		m_confirmActive = false;
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

void GameUI::AddLogLine(const std::string& line) { m_log->AddLine(line); }

void GameUI::ClearLog() { m_log->Clear(); }

// ============================================================================
// Rendering — all 2D, inside the caller's SpriteBatch Begin/End.
// ============================================================================

// Progress bar + current step name, shared by both loading screens.
void GameUI::DrawLoadProgress(const LoadQueue& queue, float barY) {
	const float w = DeviceW();
	const float h = DeviceH();
	const ui::Theme& theme = m_menuUi.GetTheme();

	const gfx::Rect bar{w * 0.3f, barY, w * 0.4f, h * (14.0f / kFontDesignWindowH)};
	m_spriteBatch.DrawRect(bar, theme.control);
	m_spriteBatch.DrawRect({bar.x, bar.y, bar.w * queue.Progress(), bar.h},
						   theme.accent);
	ui::DrawBorder(m_spriteBatch, bar, theme.panelBorder);

	const std::string_view step = queue.CurrentLabel();
	ui::Font& font = m_menuUi.GetFont();
	const float stepW = font.MeasureWidth(step);
	font.Draw(m_spriteBatch, step, (w - stepW) * 0.5f, bar.y + bar.h * 2.0f,
			  theme.textDim);
}

// Title face, horizontally centered at y in the accent color — every title
// screen draws "DUNGEON" this way.
void GameUI::DrawCenteredTitle(const std::string& text, float y) {
	const float titleW = m_titleFont.MeasureWidth(text);
	m_titleFont.Draw(m_spriteBatch, text, (DeviceW() - titleW) * 0.5f, y,
					 m_menuUi.GetTheme().accent);
}

void GameUI::RenderLoadingScreen(const LoadQueue& queue) {
	const float h = DeviceH();
	DrawCenteredTitle(loc::Tr("title"), h * 0.32f);
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

	DrawCenteredTitle(loc::Tr("title"), h * 0.16f);

	const std::string subtitle = loc::Tr("loading.descending");
	ui::Font& font = m_menuUi.GetFont();
	const float subW = font.MeasureWidth(subtitle);
	font.Draw(m_spriteBatch, subtitle, (w - subW) * 0.5f,
			  h * (0.16f + 74.0f / kFontDesignWindowH), theme.textDim);

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
	DrawCenteredTitle(loc::Tr("title"), h * 0.16f);

	const char* subKey = "menu.subtitle";
	if (m_menuPage == MenuPage::Settings) subKey = "menu.subtitle_settings";
	else if (m_menuPage == MenuPage::Saves)
		subKey = m_savesMode == SavesMode::Save ? "menu.subtitle_save"
												: "menu.subtitle_load";
	const std::string subtitle = loc::Tr(subKey);
	ui::Font& font = m_menuUi.GetFont();
	const float subW = font.MeasureWidth(subtitle);
	font.Draw(m_spriteBatch, subtitle, (w - subW) * 0.5f,
			  h * (0.16f + 74.0f / kFontDesignWindowH), theme.textDim);

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

	DrawCenteredTitle(loc::Tr("pause.title"), h * 0.16f);

	if (m_menuPage != MenuPage::Main) {
		const char* subKey = "menu.subtitle_load";
		if (m_menuPage == MenuPage::Settings) subKey = "menu.subtitle_settings";
		else if (m_savesMode == SavesMode::Save) subKey = "menu.subtitle_save";
		const std::string subtitle = loc::Tr(subKey);
		ui::Font& font = m_pauseUi.GetFont();
		const float subW = font.MeasureWidth(subtitle);
		font.Draw(m_spriteBatch, subtitle, (w - subW) * 0.5f,
				  h * (0.16f + 74.0f / kFontDesignWindowH), theme.textDim);
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
	if (const gfx::Texture* icon = m_itemIcons->For(**m_held)) {
		const float s = DeviceH() * 0.06f;
		const gfx::Rect dst{m_hudMouseX - s * 0.5f, m_hudMouseY - s * 0.5f, s, s};
		m_spriteBatch.DrawSprite(dst, {0, 0, 1, 1}, *icon, {1, 1, 1, 1});
	}
}

void GameUI::RenderHud() {
	m_hudUi.Render(m_spriteBatch, DeviceW(), DeviceH());
	DrawHeldCursor();
}

} // namespace dungeon::game
