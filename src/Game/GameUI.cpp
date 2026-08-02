// ============================================================================
// Game/GameUI.cpp — see GameUI.h.
// ============================================================================
#include "Game/GameUI.h"

#include "Core/Loc.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/SaveGame.h"
#include "Game/Spell/Spell.h"
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

// Widget bounds are normalized fractions [0..1] of their container
// (see Widget.h). Layouts are authored directly in those fractions — never
// design pixels, never a post-hoc Norm() conversion.

// Hand-use command ids that resolve to a melee swing: the verb IS the attack
// (Balance's closed attack table: damage type + numbers), the strike is the
// one shared PartyAttack path. A new weapon verb is data (items.cat
// `command`) + an AttackSpec row in Balance + a row here + a use.<verb> lang
// key.
constexpr std::string_view kMeleeUses[] = {
	"punch", "kick", "stab",  "slash", "chop",  "bash",
	"swing", "jab",  "thrust", "hack", "melee"};
bool IsMeleeUse(std::string_view cmd) {
	return std::ranges::find(kMeleeUses, cmd) != std::ranges::end(kMeleeUses);
}
// The bare hand's combat verbs — the "Combat" group of the default-picker
// menu, and always-valid defaults regardless of what the hand holds.
constexpr std::string_view kUnarmedUses[] = {"punch", "kick"};
// A spell default is stored as "cast:<spells.cat id>" so it rides the same
// per-item-type default map (and save lines) as the weapon verbs.
constexpr std::string_view kCastPrefix = "cast:";
bool IsCastUse(std::string_view cmd) { return cmd.starts_with(kCastPrefix); }
// Commands that never become a left-click default — one-shot consuming actions
// (memorize destroys the tablet) picked deliberately from the menu each time.
bool IsMenuOnlyUse(std::string_view cmd) { return cmd == "memorize"; }
// True if ExecuteUse can dispatch this id — unknown ids (a catalog typo) get
// no menu entry rather than a dead one.
bool IsExecutableUse(std::string_view cmd) {
	return cmd == "eat" || cmd == "memorize" || IsMeleeUse(cmd) || IsCastUse(cmd);
}
// The useDefaults key a hand's contents map to ("unarmed" for a bare hand —
// item ids are catalog tokens, so the sentinel can never collide).
std::string UseKey(const std::string& itemId) {
	return itemId.empty() ? std::string("unarmed") : itemId;
}

// Vertical stack with CSS-style collapsing margins: the gap between two items is
// max(upper.marginBottom, lower.marginTop) — never the sum. All coordinates are
// fractions [0..1] of the page (tab content). Place() returns bounds ready for
// AddChild (no Norm). First item gets no top margin.
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
	float Cursor() const { return m_y; }

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

	// UI skin parts (assets/ui/skin_*.png, committed source like the other UI
	// images). All optional — a missing part leaves that chrome flat, and the
	// flat look survives whole as the debug mode / uiskin=0.
	m_skinPanelTex = TryLoadTextureFile(m_device, paths::Asset("ui\\skin_panel"));
	m_skinButtonTex = TryLoadTextureFile(m_device, paths::Asset("ui\\skin_button"));
	m_skinSlotTex = TryLoadTextureFile(m_device, paths::Asset("ui\\skin_slot"));
	// The spellbook's Cast/Clear icon faces (optional — text buttons without).
	m_castIconTex = TryLoadTextureFile(m_device, paths::Asset("ui\\icon_cast"));
	m_clearIconTex = TryLoadTextureFile(m_device, paths::Asset("ui\\icon_clear"));
	// The movement pad's chevrons (single = step, double = turn), rotated in
	// quarter turns per direction by ui::Button::iconTurns.
	m_chevronTex = TryLoadTextureFile(m_device, paths::Asset("ui\\icon_chevron"));
	m_chevron2Tex = TryLoadTextureFile(m_device, paths::Asset("ui\\icon_chevron2"));
	// The shared top-right close box (assets/ui/icon_close) — the character
	// sheet's corner close, matching every editor dialog.
	m_closeIcon = TryLoadTextureFile(m_device, paths::Asset("ui\\icon_close"));
	// The panel part is a QUIET bake (near-black + faint noise, one thin brass
	// edge, no black rim — the old stone face read too busy under the kit's
	// wooden buttons); it tiles, so the noise stays dense at any panel size.
	m_skin.panel = {m_skinPanelTex.get(), 8.0f, 1.0f};
	// The button part (Medieval RPG UI kit slot #17: planked face, iron corner
	// plates) is baked 64px with a ~14px frame; scale 0.65 renders it ~9px
	// (full-scale read as heavy on the small movement/hand chrome). Both kit
	// parts are AUTHORED faces with baked lighting, so they stretch rather
	// than tile (tiling their gradients banded visibly).
	m_skin.button = {m_skinButtonTex.get(), 14.0f, 0.65f, /*stretch*/ true};
	// The socket frame (kit slot #12) is baked 96px with a ~20px ring;
	// scale 0.42 renders it ~8px so a hand slot keeps its item visible.
	m_skin.slot = {m_skinSlotTex.get(), 20.0f, 0.42f, /*stretch*/ true};
	ApplySkin();
}

void GameUI::ApplyTheme() {
	for (ui::UIContext* ctx :
		 {&m_hudUi, &m_menuUi, &m_settingsUi, &m_pauseUi, &m_savesUi, &m_sheetUi,
		  &m_confirmUi})
		ctx->SetTheme(m_settings.theme);
}

void GameUI::ApplySkin() {
	const ui::Skin* skin = m_settings.uiSkin ? &m_skin : nullptr;
	for (ui::UIContext* ctx :
		 {&m_hudUi, &m_menuUi, &m_settingsUi, &m_pauseUi, &m_savesUi, &m_sheetUi,
		  &m_confirmUi})
		ctx->SetSkin(skin);
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
								   loc::Tr(std::format("item.{}", **m_held))),
					   c.portraitColor);
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

// A click on one of the panel's status-effect icons (the name-band row): the
// sheet's Effects tab is the icon's long form, so the icon IS its door.
void GameUI::OnPortraitEffects(size_t i) {
	if (i >= m_characters.size()) return;
	onOpenSheet(i);
	m_sheet->SetMode(CharacterSheet::Mode::Effects);
}

void GameUI::OnHandLeftClick(size_t i, size_t hand) {
	if (i >= m_characters.size() || hand > 1) return;
	ItemSlot& slot = m_characters[i].inventory.Hand(static_cast<int>(hand));
	if (Holding()) {
		// Place the carried item in this hand, swapping any occupant onto the
		// cursor (a click never silently destroys an item) — but only holdable
		// items enter a hand; anything else stays on the cursor with a log line.
		if (!m_itemCategories || !m_itemCategories->Holdable(**m_held)) {
			AddLogLine(loc::Format("log.cant_hold",
								   loc::Tr(std::format("item.{}", **m_held))));
			return;
		}
		std::string incoming = **m_held;
		if (slot.Empty()) m_held->reset();
		else *m_held = slot.typeId;
		slot.typeId = std::move(incoming);
		Click();
		return;
	}
	// Empty cursor: the control-bar hand is an ACTION button — it executes the
	// hand's default use. Picking the item UP is the character sheet's job (its
	// hand cells keep the pick/swap semantics), so a swing can't be fumbled into
	// an accidental unequip mid-fight. A hand with NO default yet (bare hand,
	// rune, key — nothing defaultable on the item) opens the use menu instead,
	// so the first click PICKS what future clicks will do.
	const std::string cmd = DefaultUseFor(
		m_characters[i], hand, slot.Empty() ? std::string() : slot.typeId);
	if (cmd.empty()) {
		OpenHandUseMenu(i, hand);
		return;
	}
	ExecuteUse(i, hand, cmd);
}

void GameUI::OnHandRightClick(size_t i, size_t hand) {
	OpenHandUseMenu(i, hand);
}

void GameUI::OpenHandUseMenu(size_t i, size_t hand) {
	if (i >= m_characters.size() || hand > 1 || !m_handMenu) return;
	const Character& c = m_characters[i];
	const ItemSlot& slot = c.inventory.Hand(static_cast<int>(hand));
	const std::string itemId = slot.Empty() ? std::string() : slot.typeId;
	// The item's own command entries (ItemKind::commands, supplied by Game).
	// Labels come from the use.<cmd> lang keys; an id ExecuteUse can't dispatch
	// (catalog typo) gets no entry, so adding a verb is data + one case there.
	const std::vector<std::string> cmds =
		itemId.empty() ? std::vector<std::string>{}
					   : (itemCommands ? itemCommands(itemId)
									   : std::vector<std::string>{});
	std::vector<ui::ContextMenu::Entry> entries;
	for (const std::string& cmd : cmds) {
		if (!IsExecutableUse(cmd)) continue;
		entries.push_back({loc::Tr("use." + cmd), [this, i, hand, itemId, cmd] {
							   SelectUse(i, hand, itemId, cmd);
						   }});
	}
	// An item that offers ANY command of its own — even a menu-only one like
	// a rune's Memorize — shows just those (Michael, 2026-07-07: a rune's
	// menu is Memorize alone). Only a hand with NOTHING to offer (bare hand,
	// key) gets the grouped default pickers as CASCADING groups — the
	// ContextMenu keeps the first tier visible beside an open submenu, so
	// Combat and Magic stay in reach while browsing either: Combat > the
	// unarmed verbs, Magic > this hand's quick-cast spells. The Magic group
	// is the MRU list alone now — the spellbook lives in the Magic area's
	// member selector, not the menu (Michael, 2026-07-10) — and when it is
	// EMPTY there is nothing to group against, so the Combat tier is skipped
	// and the unarmed verbs sit at the top level (one less click).
	if (entries.empty()) {
		// THIS hand's recency list — each hand keeps its own repertoire.
		ui::ContextMenu::Entry magic{loc::Tr("menu.magic"), {}, {}};
		int shown = 0;
		for (const std::string& id : c.spellMru[hand]) {
			if (shown >= m_settings.spellMruCount) break;
			// Skip ids the registry no longer carries (the MRU is state,
			// the spell classes are code — they can drift across edits).
			const Spell* def = nullptr;
			if (spellDefs)
				for (const auto& d : spellDefs())
					if (d->Id() == id) { def = d.get(); break; }
			if (!def) continue;
			std::string cmd = std::string(kCastPrefix) + def->Id();
			magic.children.push_back(
				{loc::Tr(def->NameKey()), [this, i, hand, itemId, cmd] {
					 SelectUse(i, hand, itemId, cmd);
				 }});
			++shown;
		}
		const bool hasMagic = !magic.children.empty();
		ui::ContextMenu::Entry combat{loc::Tr("menu.combat"), {}, {}};
		std::vector<ui::ContextMenu::Entry>& verbs =
			hasMagic ? combat.children : entries;
		for (std::string_view verb : kUnarmedUses) {
			std::string cmd{verb};
			verbs.push_back(
				{loc::Tr("use." + cmd), [this, i, hand, itemId, cmd] {
					 SelectUse(i, hand, itemId, cmd);
				 }});
		}
		if (hasMagic) {
			entries.push_back(std::move(combat));
			entries.push_back(std::move(magic));
		}
	}
	if (entries.empty()) return; // nothing actionable — don't pop an empty menu
	m_handMenu->Open(m_hudMouseX, m_hudMouseY, std::move(entries));
}

void GameUI::SelectUse(size_t i, size_t hand, const std::string& itemId,
					   const std::string& cmd) {
	if (i >= m_characters.size() || hand > 1) return;
	const bool menuOnly = IsMenuOnlyUse(cmd);
	// The pick becomes this member's default for THIS HAND and the item TYPE
	// (so every khukri in that hand chops until they choose otherwise; a
	// bare-hand pick records under the "unarmed" key) — the other hand keeps
	// its own pick, so left can be one spell and right another. Menu-only
	// commands are deliberate one-shots — never recorded.
	if (!menuOnly) m_characters[i].useDefaults[hand][UseKey(itemId)] = cmd;
	// Menu-only commands always perform; a defaultable pick performs per the
	// Controls setting (off = the menu only arms the default).
	if (menuOnly || m_settings.useMenuExecutes) ExecuteUse(i, hand, cmd);
}

void GameUI::ExecuteUse(size_t i, size_t hand, const std::string& cmd) {
	if (i >= m_characters.size() || hand > 1 || cmd.empty()) return;
	if (cmd == "memorize") {
		MemorizeFromHand(i, hand);
	} else if (cmd == "eat") {
		EatFromHand(i, hand);
	} else if (IsCastUse(cmd)) {
		// A "cast:<id>" default: the world's cast façade gates vocabulary and
		// mana and turns the outcome into log + sound; the firing hand's
		// quick-cast MRU is credited.
		if (onCastSpell) onCastSpell(i, cmd.substr(kCastPrefix.size()), hand);
	} else if (IsMeleeUse(cmd)) {
		// Every melee verb lands through the one strike path; the verb IS the
		// attack (damage type + numbers, Balance::FindAttack). Cooldown gating
		// and the alive-check live in DungeonWorld::PartyAttack.
		if (onHandAttack) onHandAttack(i, hand, cmd);
	}
	// Unknown id: a catalog typo — the menu never offered it; a stale saved
	// default falls through DefaultUseFor instead. Nothing to do.
}

std::string GameUI::DefaultUseFor(const Character& c, size_t hand,
								  const std::string& itemId) const {
	if (hand > 1) return {};
	const std::vector<std::string> cmds =
		itemId.empty() ? std::vector<std::string>{}
					   : (itemCommands ? itemCommands(itemId)
									   : std::vector<std::string>{});
	// THIS hand's remembered pick wins while it is still valid — the catalog
	// may have changed since the save was written, and a "cast:" default needs
	// the member to know the spell (a loaded save's defaults must not outrun
	// its vocabulary).
	if (const auto it = c.useDefaults[hand].find(UseKey(itemId));
		it != c.useDefaults[hand].end())
		if (UseValidFor(c, cmds, it->second)) return it->second;
	// Else the item's first defaultable command (a rune's only command is the
	// menu-only memorize, so it yields "" — a left-click can't eat a tablet).
	for (const std::string& cmd : cmds)
		if (!IsMenuOnlyUse(cmd) && IsExecutableUse(cmd)) return cmd;
	return {}; // no default — the left-click opens the use menu to pick one
}

bool GameUI::UseValidFor(const Character& c, const std::vector<std::string>& cmds,
						 const std::string& cmd) const {
	if (IsMenuOnlyUse(cmd) || !IsExecutableUse(cmd)) return false;
	if (IsCastUse(cmd)) {
		const std::string_view id = std::string_view(cmd).substr(kCastPrefix.size());
		if (!spellDefs) return false;
		for (const auto& def : spellDefs())
			if (def->Id() == id) return c.HasLearnedSpell(def->Id());
		return false; // spell gone from the registry
	}
	if (std::ranges::find(cmds, cmd) != cmds.end()) return true;
	// The bare-hand combat verbs are pickable for any hand contents.
	return std::ranges::find(kUnarmedUses, cmd) != std::ranges::end(kUnarmedUses);
}

void GameUI::MemorizeFromHand(size_t i, size_t hand) {
	if (i >= m_characters.size() || hand > 1) return;
	MemorizeSlot(i, m_characters[i].inventory.Hand(static_cast<int>(hand)));
}

void GameUI::MemorizeSlot(size_t i, ItemSlot& slot) {
	if (i >= m_characters.size()) return;
	SpellSymbol sym;
	if (!RuneSymbolFromItemId(slot.typeId, sym)) return;
	m_characters[i].Learn(sym);
	slot.Clear(); // the tablet is consumed
	Click();
	AddLogLine(loc::Format("log.memorize", m_characters[i].name,
						   loc::Tr(SymbolKey(sym))),
			   m_characters[i].portraitColor);
	RefreshSheet(); // the sheet's known symbols may be on screen later
}

void GameUI::OpenPackUseMenu(int slot) {
	// The sheet shows m_sheetIndex's SELECTED pack; `slot` indexes into it. A
	// rune offers Memorize — resolved by index at CLICK time (the sheet is
	// modal, so the pack can't shift under the open menu).
	if (!m_sheetMenu || m_sheetIndex >= m_characters.size() || slot < 0) return;
	const auto& pack = m_characters[m_sheetIndex].inventory.SelectedContents();
	if (slot >= static_cast<int>(pack.size())) return;
	SpellSymbol sym;
	if (!RuneSymbolFromItemId(pack[static_cast<size_t>(slot)].typeId, sym))
		return; // only runes have a pack-side action so far
	std::vector<ui::ContextMenu::Entry> entries;
	entries.push_back({loc::Tr("use.memorize"), [this, slot] {
						   auto& p = m_characters[m_sheetIndex]
										 .inventory.SelectedContents();
						   if (slot < static_cast<int>(p.size()))
							   MemorizeSlot(m_sheetIndex,
											p[static_cast<size_t>(slot)]);
					   }});
	m_sheetMenu->Open(m_hudMouseX, m_hudMouseY, std::move(entries));
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
	AddLogLine(loc::Format("log.eat", c.name, foodName), c.portraitColor);
	RefreshSheet(); // stamina bar / carry load on the sheet may be on screen
}

// ============================================================================
// Landing page — title plus a MenuList; entries highlight on mouse hover or
// keyboard selection. All entries are wired: Continue loads the newest save,
// Load opens the saves browser, Start New Game and Settings work as labeled.
// ============================================================================
void GameUI::BuildMenu() {
	// Continue and Load only appear when at least one save exists, so the list
	// is sized to whatever entries are present (one quarter each with all four,
	// half each with just Start + Settings). Bounds are window fractions.
	const bool hasSaves = !ListSaves().empty();
	const int itemCount = hasSaves ? 4 : 2;
	constexpr float kMenuW = 0.26f;   // ~420/1600
	constexpr float kItemH = 0.064f;  // ~58/900
	const float menuH = kItemH * static_cast<float>(itemCount);
	auto* menu = m_menuUi.Add<ui::MenuList>(gfx::Rect{(1.0f - kMenuW) * 0.5f, 0.42f, kMenuW, menuH},
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
	// Page metrics as fractions of the tab content area.
	const float pad = 0.032f;
	const float rowW = 1.0f - 2.0f * pad;
	const float labelH = 0.057f;
	const float ctrlH = 0.081f;
	const float sliderH = 0.102f;
	const float mTight = 0.024f;
	const float mRow = 0.028f;
	const float mGroup = 0.049f;
	constexpr float kRule = 0.003f; // hairline separator height (page fraction)

	// Game: language. The language list is whatever assets/lang holds;
	// selecting one defers to Game (settings save + string reload +
	// RebuildForLanguage at the top of the next frame — rebuilding here
	// would destroy this dropdown mid-callback).
	Flow gf{pad, rowW, pad};
	tabs->AddChild<ui::Label>(tabGame, gf.Place(labelH, mGroup, mTight),
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
		tabGame, gf.Place(ctrlH, mTight, mGroup), std::move(languageNames),
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
	tabs->AddChild<ui::Label>(tabControls, cf.Place(labelH, mGroup, mRow),
							  loc::Tr("settings.movement_keys"));
	m_keyBinds.clear();
	for (size_t i = 0; i < std::size(kKeyFields); ++i) {
		const KeyField& field = kKeyFields[i];
		auto* bind = tabs->AddChild<ui::KeyBind>(
			tabControls, cf.Place(ctrlH, mRow, mRow),
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
	tabs->AddChild<ui::Separator>(tabControls, cf.Place(kRule, mGroup, mGroup));
	tabs->AddChild<ui::Label>(tabControls, cf.Place(labelH, mGroup, mTight),
							  loc::Tr("settings.mouselook"));
	auto lookSlider = [&](const char* key, float lo, float hi, float* field, float mTop,
						  float mBot) {
		auto* s = tabs->AddChild<ui::Slider>(
			tabControls, cf.Place(sliderH, mTop, mBot), loc::Tr(key), lo, hi,
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
		tabs->AddChild<ui::Label>(tabControls, cf.Place(labelH, mGroup, mTight),
								  loc::Tr(labelKey));
		tabs->AddChild<ui::DropDown>(
			tabControls, cf.Place(ctrlH, mTight, mGroup), easeNames(),
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

	// Controls → Hands: hand-slot behaviour. A checkbox — whether picking an
	// entry from a hand's right-click use menu also performs it (off = the menu
	// only sets the hand's left-click default) — and the Magic quick-cast
	// count (how many recently-cast spells the menu lists). Both persist
	// immediately.
	tabs->AddChild<ui::Separator>(tabControls, cf.Place(kRule, mGroup, mGroup));
	tabs->AddChild<ui::Label>(tabControls, cf.Place(labelH, mGroup, mTight),
							  loc::Tr("settings.hands"));
	tabs->AddChild<ui::Checkbox>(
		tabControls, cf.Place(ctrlH, mTight, mGroup),
		loc::Tr("settings.usemenu_execute"), m_settings.useMenuExecutes,
		[this](bool on) {
			Click();
			m_settings.useMenuExecutes = on;
			m_settings.Save();
		});
	tabs->AddChild<ui::Label>(tabControls, cf.Place(labelH, mGroup, mTight),
							  loc::Tr("settings.spell_mru"));
	{
		std::vector<std::string> counts;
		for (int n = 1; n <= 10; ++n) counts.push_back(std::to_string(n));
		tabs->AddChild<ui::DropDown>(
			tabControls, cf.Place(ctrlH, mTight, mGroup),
			std::move(counts), m_settings.spellMruCount - 1, [this](int index) {
				Click();
				m_settings.spellMruCount = index + 1;
				m_settings.Save();
			});
	}

	// Video: the page overflows its height, so the TabControl scrolls. A Flow
	// (collapsing-margin vertical stack) places each label-over-control setting.
	Flow vf{pad, rowW, pad};
	auto videoLabel = [&](const char* key) {
		tabs->AddChild<ui::Label>(tabVideo, vf.Place(labelH, mGroup, mTight),
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
			tabVideo, vf.Place(ctrlH, mTight, mGroup), std::move(names),
			m_selAdapter, [this](int index) {
				Click();
				if (index == m_selAdapter) return;
				m_selAdapter = index; // monitor/resolution lists depend on it
				m_selOutput = 0;
				m_selRes = 0;
				m_videoRebuildPending = true;
			});
	} else {
		tabs->AddChild<ui::Label>(tabVideo, vf.Place(ctrlH, mTight, mGroup),
								  selAdapter ? selAdapter->name : std::string("—"));
	}
	// Monitor (output) of the selected adapter.
	videoLabel("settings.monitor");
	if (selAdapter && selAdapter->outputs.size() > 1) {
		std::vector<std::string> names;
		for (const gfx::OutputInfo& o : selAdapter->outputs) names.push_back(o.name);
		tabs->AddChild<ui::DropDown>(
			tabVideo, vf.Place(ctrlH, mTight, mGroup), std::move(names),
			m_selOutput, [this](int index) {
				Click();
				if (index == m_selOutput) return;
				m_selOutput = index; // resolution list depends on the monitor
				m_selRes = 0;
				m_videoRebuildPending = true;
			});
	} else {
		tabs->AddChild<ui::Label>(tabVideo, vf.Place(ctrlH, mTight, mGroup),
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
			tabVideo, vf.Place(ctrlH, mTight, mGroup), std::move(resOptions),
			m_selRes, [this](int index) {
				Click();
				m_selRes = index;
			});
	}
	// Display mode: Windowed / Borderless / Exclusive full-screen.
	videoLabel("settings.display_mode");
	tabs->AddChild<ui::DropDown>(
		tabVideo, vf.Place(ctrlH, mTight, mGroup),
		std::vector<std::string>{loc::Tr("mode.windowed"), loc::Tr("mode.borderless"),
								 loc::Tr("mode.exclusive")},
		static_cast<int>(m_selMode), [this](int index) {
			Click();
			m_selMode = static_cast<gfx::FullscreenMode>(index);
		});
	// Apply the staged display selection (the only Video control that isn't live).
	gfx::Rect applyRect = vf.Place(ctrlH, mTight, mGroup);
	applyRect.w = 220.0f; // narrower than a full row, like a button
	tabs->AddChild<ui::Button>(tabVideo, applyRect,
							   loc::Tr("settings.apply"), [this] {
								   Click();
								   OnVideoApply();
							   });

	// Divider between the display section above and the rendering section below.
	tabs->AddChild<ui::Separator>(tabVideo,
								  vf.Place(kRule, mGroup, mGroup));

	// Video: quality tier (hot-swaps meshes/textures in place).
	videoLabel("settings.quality");
	tabs->AddChild<ui::DropDown>(
		tabVideo, vf.Place(ctrlH, mTight, mGroup),
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
		tabVideo, vf.Place(ctrlH, mTight, mGroup), std::move(lightOptions),
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
		tabVideo, vf.Place(ctrlH, mTight, mGroup), std::move(fpsOptions),
		GameSettings::PresentIntervalIndex(m_settings.presentInterval),
		[this](int index) {
			Click();
			onFrameLimitSelected(index);
		});

	// Audio: master volume (the slider's label + track live inside its bounds).
	// Live while dragging; persisted once on release.
	Flow af{pad, rowW, pad};
	auto* volume = tabs->AddChild<ui::Slider>(
		tabAudio, af.Place(sliderH, mGroup, mGroup),
		loc::Tr("settings.volume"), 0.0f, 1.0f, m_settings.volume, [this](float v) {
			m_settings.volume = v;
			m_audio.SetMasterVolume(v);
		});
	volume->onRelease = [this] { m_settings.Save(); };

	// UI → Textured UI: flips every context between the skinned chrome and the
	// flat theme-fill look (kept as a debug mode — containment/extents read at
	// a glance). Live — widgets re-check the skin pointer each draw.
	Flow uf{pad, rowW, pad};
	tabs->AddChild<ui::Checkbox>(
		tabUi, uf.Place(ctrlH, mGroup, mTight),
		loc::Tr("settings.uiskin"), m_settings.uiSkin, [this](bool on) {
			Click();
			m_settings.uiSkin = on;
			ApplySkin();
			m_settings.Save();
		});

	// UI → Head bob: the walking camera's footfall dip/sway. Off for motion-
	// sensitive players; pushed to the Party via onHeadBobChanged.
	tabs->AddChild<ui::Checkbox>(
		tabUi, uf.Place(ctrlH, mTight, mGroup),
		loc::Tr("settings.headbob"), m_settings.headBob, [this](bool on) {
			Click();
			m_settings.headBob = on;
			if (onHeadBobChanged) onHeadBobChanged();
			m_settings.Save();
		});

	// UI → Party Bar: scale resizes the bar live (about its top center) and
	// opacity fades the slot backgrounds. Both apply while dragging and
	// persist on release; safe before the HUD exists (the panel list is empty
	// until the first game load).
	tabs->AddChild<ui::Label>(tabUi, uf.Place(labelH, mGroup, mTight),
							  loc::Tr("settings.party_bar"));
	auto* barScale = tabs->AddChild<ui::Slider>(
		tabUi, uf.Place(sliderH, mTight, mGroup),
		loc::Tr("settings.bar_scale"), 0.5f, 1.5f, m_settings.partyBarScale,
		[this](float v) {
			m_settings.partyBarScale = v;
			ApplyPartyBarScale();
		});
	barScale->onRelease = [this] { m_settings.Save(); };
	auto* barOpacity = tabs->AddChild<ui::Slider>(
		tabUi, uf.Place(sliderH, mGroup, mGroup),
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
	const float colGap = 0.021f;
	const float colW = (rowW - 2.0f * colGap) / 3.0f;
	const float pickRowH = 0.089f; // per grid row (3 pickers across)
	const float pickH = 0.073f;    // a picker swatch row
	auto gridHeight = [&](size_t count) {
		const size_t rows = (count + 2) / 3;
		return rows == 0 ? 0.0f : static_cast<float>(rows - 1) * pickRowH + pickH;
	};
	auto pickerCell = [&](size_t index, float blockTop) {
		return gfx::Rect{pad + (colW + colGap) * static_cast<float>(index % 3),
						 blockTop + pickRowH * static_cast<float>(index / 3), colW,
						 pickH};
	};
	tabs->AddChild<ui::Separator>(tabUi, uf.Place(kRule, mGroup, mGroup));
	tabs->AddChild<ui::Label>(tabUi, uf.Place(labelH, mGroup, mTight),
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
	tabs->AddChild<ui::Separator>(tabUi, uf.Place(kRule, mGroup, mGroup));
	tabs->AddChild<ui::Label>(tabUi, uf.Place(labelH, mGroup, mTight),
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

	// UI → Party Colors: one picker per roster slot — the member's identity
	// color (portrait border, hand stripe, log tint). Edits land in the
	// settings (the master, member_<n>= in the ini) AND on the live roster,
	// so the HUD recolors immediately; persists when the popup closes.
	tabs->AddChild<ui::Separator>(tabUi, uf.Place(kRule, mGroup, mGroup));
	tabs->AddChild<ui::Label>(tabUi, uf.Place(labelH, mGroup, mTight),
							  loc::Tr("settings.party_colors"));
	const float memTop =
		uf.Place(gridHeight(kMemberColorCount), mTight, mGroup).y;
	for (size_t i = 0; i < kMemberColorCount; ++i) {
		// Label with the member's name when the roster has the slot (proper
		// nouns, not localized); a slot number otherwise.
		const std::string label =
			i < m_characters.size() ? m_characters[i].name
									: loc::Format("settings.member_n", i + 1);
		auto* picker = tabs->AddChild<ui::ColorPicker>(
			tabUi, pickerCell(i, memTop), label, m_settings.memberColors[i],
			[this, i](const Vec4& color) {
				m_settings.memberColors[i] = color;
				if (i < m_characters.size())
					m_characters[i].portraitColor = color;
			});
		picker->onClose = [this] { m_settings.Save(); };
	}

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
void GameUI::BuildPauseMenu() {
	// Load only appears when at least one save exists; the list is sized to the
	// entries actually present (five with Load, four without).
	const bool hasSaves = !ListSaves().empty();
	const int itemCount = hasSaves ? 5 : 4;
	constexpr float kMenuW = 0.26f;
	constexpr float kItemH = 0.064f;
	const float menuH = kItemH * static_cast<float>(itemCount);
	auto* menu = m_pauseUi.Add<ui::MenuList>(gfx::Rect{(1.0f - kMenuW) * 0.5f, 0.42f, kMenuW, menuH},
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

	const std::vector<SaveSlot> slots = ListSaves();

	// Column as window fractions (~720/1600 wide, centered).
	constexpr float kColW = 0.45f;
	constexpr float kColX = (1.0f - kColW) * 0.5f;
	constexpr float kRowH = 0.05f;
	constexpr float kLabelH = 0.032f;

	// Builds the slots into a scroll box at [ly, ly+lh] (window fractions).
	// Added LAST by the caller so its modal dialog claims the mouse first.
	auto buildList = [&](float ly, float lh) {
		auto* list = m_savesUi.Add<ui::SlotList>(gfx::Rect{kColX, ly, kColW, lh});
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

	float backY = 0.0f;
	bool wantList = false;
	float listY = 0.0f, listH = 0.0f;
	if (mode == SavesMode::Save) {
		float y = 0.16f;
		m_saveField = m_savesUi.Add<ui::TextField>(gfx::Rect{kColX, y, kColW, kRowH},
			loc::Format("saves.default_name", slots.size() + 1));
		m_saveField->placeholder = loc::Tr("saves.name_placeholder");
		m_saveField->onChange = [this] { DisarmOverwrite(); };
		m_saveField->onSubmit = [this] { CommitSave(); };
		m_saveField->SetFocused(true);
		y += 0.07f;

		m_saveButton = m_savesUi.Add<ui::Button>(gfx::Rect{kColX, y, kColW, kRowH}, loc::Tr("menu.save"),
			[this] { CommitSave(); });
		y += 0.085f;

		if (slots.empty()) {
			backY = y;
		} else {
			m_savesUi.Add<ui::Label>(gfx::Rect{kColX, y, kColW, kLabelH},
									 loc::Tr("saves.overwrite_label"))
				->dim = true;
			listY = y + 0.04f;
			listH = 0.40f;
			wantList = true;
			backY = listY + listH + 0.02f;
		}
	} else {
		listY = 0.24f;
		if (slots.empty()) {
			m_savesUi.Add<ui::Label>(gfx::Rect{kColX, listY, kColW, kLabelH},
									 loc::Tr("saves.none"))
				->dim = true;
			backY = listY + 0.06f;
		} else {
			listH = 0.52f;
			wantList = true;
			backY = listY + listH + 0.02f;
		}
	}

	constexpr float kBackW = 0.14f;
	m_savesUi.Add<ui::Button>(gfx::Rect{(1.0f - kBackW) * 0.5f, backY, kBackW, kRowH}, loc::Tr("menu.back"),
		[this] {
			Click();
			m_menuPage = MenuPage::Main;
		});

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
	// Parent-relative layout (fractions of the window) — no design-pixel Norm.
	// Centered panel, slightly above geometric center so the footer buttons fit.
	constexpr float kSheetW = 0.50f;
	constexpr float kSheetH = 0.62f;
	constexpr float kSheetX = (1.0f - kSheetW) * 0.5f;
	constexpr float kSheetY = (1.0f - kSheetH) * 0.5f - 0.03f;
	const gfx::Rect sheet{kSheetX, kSheetY, kSheetW, kSheetH};

	// Added FIRST so the buttons below it update on top (and consume their clicks
	// before the sheet's slot hit-testing).
	m_sheet = m_sheetUi.Add<CharacterSheet>(sheet, &m_characters, &m_titleFont,
											&m_settings.barColors, m_itemIcons,
											m_itemWeights, m_slotIcons,
											m_itemCategories, m_held);
	// A pack refused the held item: a soft thud + a "won't fit" log line. Item
	// names follow the item.<id> loc convention (same as ItemKind::nameKey).
	m_sheet->onRejectDrop = [this](const std::string& item, const std::string& pack) {
		m_audio.Play(m_sounds.bump, 0.5f);
		AddLogLine(loc::Format("log.pack_rejects", loc::Tr("item." + item),
							   loc::Tr("item." + pack)));
	};
	// A hand doll cell refused a non-holdable item: same thud, the shared
	// "can't be held" line (also used by the control-bar hand slots).
	m_sheet->onRejectHold = [this](const std::string& item) {
		m_audio.Play(m_sounds.bump, 0.5f);
		AddLogLine(loc::Format("log.cant_hold", loc::Tr("item." + item)));
	};
	// Right-clicked backpack slot → its use menu (a rune memorizes from the
	// pack too, not just a hand).
	m_sheet->onSlotMenu = [this](int slot) { OpenPackUseMenu(slot); };
	// The Spells tab resolves learned-spell ids through the same registry the
	// spellbook uses (deferred so spellDefs is wired by cast time).
	m_sheet->spells = [this] {
		return spellDefs ? spellDefs()
						 : std::span<const std::unique_ptr<Spell>>{};
	};

	constexpr float kBtnH = 0.045f;
	constexpr float kBtnW = 0.05f;
	const float btnY = kSheetY + kSheetH + 0.02f;
	m_sheetUi.Add<ui::Button>(gfx::Rect{kSheetX, btnY, kBtnW, kBtnH}, "<",
							  [this] {
								  const size_t count = m_characters.size();
								  onOpenSheet((m_sheetIndex + count - 1) % count);
							  });
	m_sheetUi.Add<ui::Button>(
		gfx::Rect{kSheetX + kSheetW - kBtnW, btnY, kBtnW, kBtnH}, ">", [this] {
			onOpenSheet((m_sheetIndex + 1) % m_characters.size());
		});
	// "All" → the combined party-backpacks view (for cross-character swaps).
	m_sheetUi.Add<ui::Button>(gfx::Rect{kSheetX + 0.06f, btnY, 0.08f, kBtnH},
							  loc::Tr("ui.inv_all"), [this] {
								  Click();
								  if (onShowPartyInventory) onShowPartyInventory();
							  });
	// Close (= resume) is the shared corner box at the sheet panel's top-right,
	// matching every other dialog — no footer Back button.
	ui::AddCloseButton(m_sheetUi, sheet, m_closeIcon.get(), [this] {
		Click();
		onResume();
	});
	// The sheet's own context menu (backpack-slot actions), added LAST so it
	// updates first and its popup draws over everything.
	m_sheetMenu = m_sheetUi.Add<ui::ContextMenu>();
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
						 std::function<void()> onYes) {
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
	m_confirmUi.Add<ui::Button>(gfx::Rect{kPX + kIn, by, kBW, kBH}, loc::Tr("confirm.yes"),
								[this, onYes = std::move(onYes)] {
									Click();
									m_confirmActive = false;
									onYes();
								});
	m_confirmUi.Add<ui::Button>(gfx::Rect{kPX + kPW - kBW - kIn, by, kBW, kBH}, loc::Tr("confirm.no"), [this] {
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
	m_sheet->SetCharacter(index);
}

void GameUI::RefreshSheet() { m_sheet->SetCharacter(m_sheetIndex); }

// ============================================================================
// HUD — authored in design pixels from the initial window size, stored as
// window fractions (Norm), so it scales with the screen. Widgets the game
// updates later are kept as raw pointers (m_log, m_compass, m_position); the
// UIContext owns all widgets.
// ============================================================================
void GameUI::BuildHud() {
	// All HUD bounds are fractions of the window. Party-bar slots start empty
	// and are sized by ApplyPartyBarScale (scale slider); everything below the
	// bar is authored at scale 1 and shifted when the bar grows.
	//
	// Layout fractions (window): bar top margin 0.018, bar height 0.107 at
	// scale 1, gap under bar 0.018 → content starts at kBelowBar0.
	constexpr float kBarTop = 0.018f;
	constexpr float kBarH0 = 0.107f; // scale-1 height
	constexpr float kBarGap = 0.018f;
	constexpr float kBelowBar0 = kBarTop + kBarH0 + kBarGap; // ~0.143

	m_partyPanels.clear();
	for (size_t i = 0; i < m_characters.size() && i < 4; ++i) {
		auto* panel = m_hudUi.Add<CharacterPanel>(
			gfx::Rect{}, &m_characters, i, &m_titleFont, &m_settings.barColors,
			m_hitSplats, m_itemIcons, [this, i] { OnPortraitClick(i); },
			[this, i] { OnPortraitRightClick(i); },
			[this, i] { OnPortraitBars(i); },
			[this, i] { OnPortraitEffects(i); });
		panel->backgroundOpacity = m_settings.partyBarOpacity;
		m_partyPanels.push_back(panel);
	}

	// Widgets under the bar: store their scale-1 fractional Y so
	// ApplyPartyBarScale can shift them with the bar.
	m_belowBarWidgets.clear();
	auto below = [this](ui::Widget* widget) {
		m_belowBarWidgets.push_back({widget, widget->bounds.y});
	};

	// Left status (compass + position).
	constexpr float kLeftX = 0.01f, kLeftW = 0.15f;
	below(m_hudUi.Add<ui::Panel>(gfx::Rect{kLeftX, kBelowBar0, kLeftW, 0.071f}));
	m_compass = m_hudUi.Add<ui::Label>(gfx::Rect{kLeftX + 0.007f, kBelowBar0 + 0.011f, kLeftW - 0.014f, 0.022f}, "");
	below(m_compass);
	m_position = m_hudUi.Add<ui::Label>(gfx::Rect{kLeftX + 0.007f, kBelowBar0 + 0.038f, kLeftW - 0.014f, 0.022f}, "");
	m_position->dim = true;
	below(m_position);

	// Options (torch + wait/help) under status.
	const float optTop = kBelowBar0 + 0.084f;
	below(m_hudUi.Add<ui::Panel>(gfx::Rect{kLeftX, optTop, kLeftW, 0.16f}));
	below(m_hudUi.Add<ui::Label>(gfx::Rect{kLeftX + 0.009f, optTop + 0.011f, kLeftW - 0.018f, 0.022f},
		loc::Tr("hud.options")));
	auto* torchLabel = m_hudUi.Add<ui::Label>(gfx::Rect{kLeftX + 0.009f, optTop + 0.044f, kLeftW - 0.018f, 0.022f},
		loc::Tr("hud.torchlight"));
	torchLabel->dim = true;
	below(torchLabel);
	below(m_hudUi.Add<ui::DropDown>(gfx::Rect{kLeftX + 0.009f, optTop + 0.071f, kLeftW - 0.018f, 0.029f},
		std::vector<std::string>{loc::Tr("torch.warm"), loc::Tr("torch.cold"),
								 loc::Tr("torch.eerie")},
		m_torchPalette, [this](int index) {
			Click();
			m_torchPalette = index;
			onTorchPalette(index);
		}));
	constexpr float kHalfBtn = 0.063f;
	below(m_hudUi.Add<ui::Button>(gfx::Rect{kLeftX + 0.009f, optTop + 0.116f, kHalfBtn, 0.031f}, loc::Tr("hud.wait"),
		[this] {
			Click();
			m_log->AddLine(loc::Tr("log.wait"));
		}));
	below(m_hudUi.Add<ui::Button>(gfx::Rect{kLeftX + 0.009f + kHalfBtn + 0.008f, optTop + 0.116f, kHalfBtn, 0.031f},
		loc::Tr("hud.help"), [this] {
			Click();
			m_log->AddLine(m_settings.MoveKeysHelp());
			m_log->AddLine(loc::Tr("log.scroll_hint"));
		}));

	// Right control panel: movement, hands, magic. Stops above the log footer.
	constexpr float kPanelW = 0.156f; // ~250/1600
	constexpr float kPanelX = 1.0f - kPanelW - 0.01f;
	constexpr float kPad = 0.009f; // pad inside panel as window fraction
	constexpr float kFooter = 0.071f;
	const float panelBottom = 1.0f - kFooter;
	const float panelH = panelBottom - kBelowBar0;
	const float innerX = kPanelX + kPad;
	const float innerW = kPanelW - 2 * kPad;
	below(m_hudUi.Add<ui::Panel>(gfx::Rect{kPanelX, kBelowBar0, kPanelW, panelH}));

	// Movement pad: 3×2 grid of square buttons.
	const struct {
		const char* glyph;
		MoveAction action;
		bool turn;
		int quarters;
	} moves[] = {
		{"«", MoveAction::TurnLeft, true, 2},  {"^", MoveAction::Forward, false, 3},
		{"»", MoveAction::TurnRight, true, 0}, {"<", MoveAction::StrafeLeft, false, 2},
		{"v", MoveAction::Back, false, 1},     {">", MoveAction::StrafeRight, false, 0},
	};
	const float moveGap = 0.005f;
	const float moveW = (innerW - 2 * moveGap) / 3.0f;
	const float moveTop = kBelowBar0 + 0.013f;
	for (size_t i = 0; i < std::size(moves); ++i) {
		auto* btn = m_hudUi.Add<ui::Button>(gfx::Rect{innerX + (moveW + moveGap) * static_cast<float>(i % 3),
			 moveTop + (moveW + moveGap) * static_cast<float>(i / 3), moveW, moveW},
			moves[i].glyph,
			[this, action = moves[i].action] { onMoveAction(action); });
		btn->icon = moves[i].turn ? m_chevron2Tex.get() : m_chevronTex.get();
		btn->iconTurns = moves[i].quarters;
		below(btn);
	}

	// Hand pairs: 2×2 of left/right hand slots (window-fraction squares).
	const float setGap = 0.005f;
	const float setW = (innerW - setGap) / 2.0f;
	const float handGap = 0.0025f;
	const float handW = (setW - handGap) / 2.0f;
	const float setH = handW + 0.009f;
	const float handsTop = moveTop + 2 * (moveW + moveGap) + 0.016f;
	for (size_t i = 0; i < m_characters.size() && i < 4; ++i) {
		const float setX = innerX + (setW + setGap) * static_cast<float>(i % 2);
		const float setTop = handsTop + setH * static_cast<float>(i / 2);
		for (int hand = 0; hand < 2; ++hand) {
			below(m_hudUi.Add<HandSlot>(gfx::Rect{setX + (handW + handGap) * static_cast<float>(hand), setTop, handW,
				 handW},
				&m_characters, i, hand, m_itemIcons,
				[this, i, hand] { OnHandLeftClick(i, static_cast<size_t>(hand)); },
				[this, i, hand] { OnHandRightClick(i, static_cast<size_t>(hand)); }));
		}
	}
	const size_t handRows = (std::min<size_t>(m_characters.size(), 4) + 1) / 2;
	const float magicTop = handsTop + setH * static_cast<float>(handRows);

	// Magic / spellbook fills the rest of the control panel.
	below(m_hudUi.Add<ui::Label>(gfx::Rect{innerX, magicTop + 0.009f, innerW, 0.022f},
								 loc::Tr("hud.magic")));
	const float bookY = magicTop + 0.036f;
	const float bookH = panelBottom - kPad - bookY;
	below(m_hudUi.Add<ui::Panel>(gfx::Rect{innerX, bookY, innerW, bookH}));
	m_spellbook =
		m_hudUi.Add<SpellbookPanel>(gfx::Rect{innerX, bookY, innerW, bookH}, &m_characters,
									m_itemIcons);
	m_spellbook->onClick = [this] { Click(); };
	m_spellbook->castIcon = m_castIconTex.get();
	m_spellbook->clearIcon = m_clearIconTex.get();
	m_spellbook->spells = [this] {
		return spellDefs ? spellDefs()
						 : std::span<const std::unique_ptr<Spell>>{};
	};
	m_spellbook->onCast = [this](size_t member,
								 const std::vector<SpellSymbol>& seq) {
		Click();
		if (onCastSequence) onCastSequence(member, kBookHands, seq);
	};
	below(m_spellbook);

	// Message log: full window (sizes itself each frame from height fractions).
	m_log = m_hudUi.Add<MessageLog>();
	m_log->bounds = {0, 0, 1, 1};
	m_log->restoreLabel = loc::Tr("hud.log_show");

	m_handMenu = m_hudUi.Add<ui::ContextMenu>();
	m_inventory = m_hudUi.Add<InventoryWindow>(&m_characters, m_itemIcons, m_held);

	ApplyPartyBarScale();
}

void GameUI::OpenInventory() { if (m_inventory) m_inventory->Open(); }
void GameUI::CloseInventory() { if (m_inventory) m_inventory->Close(); }
bool GameUI::InventoryOpen() const { return m_inventory && m_inventory->IsOpen(); }

// Re-derives party-bar slot rects from the settings scale (window fractions)
// and shifts widgets under the bar so they stay clear of its bottom edge.
void GameUI::ApplyPartyBarScale() {
	if (m_partyPanels.empty()) return;
	const float s = m_settings.partyBarScale;
	// Width only shrinks when s < 1; height grows with s.
	const float ws = std::min(s, 1.0f);
	constexpr float kBarTop = 0.018f;
	constexpr float kBarH0 = 0.107f;
	constexpr float kMargin = 0.01f;
	constexpr float kGap0 = 0.006f;
	const float gap = kGap0 * ws;
	const float usable = 1.0f - 2 * kMargin - 3 * gap;
	const float slotW = (usable / 4.0f) * ws;
	const float barX = (1.0f - 4 * slotW - 3 * gap) * 0.5f;
	const float slotH = kBarH0 * s;
	for (size_t i = 0; i < m_partyPanels.size(); ++i)
		m_partyPanels[i]->bounds = {
			barX + static_cast<float>(i) * (slotW + gap), kBarTop, slotW, slotH};

	// below-bar widgets stored their scale-1 Y; shift by bar growth.
	const float shift = kBarH0 * (s - 1.0f);
	for (auto& [widget, y0] : m_belowBarWidgets)
		widget->bounds.y = y0 + shift;
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

// The HUD log widget exists only once BuildHud has run (the last game-load
// task), but world feedback can be raised before then — a dev-console command
// against the loading world, say. A null m_log drops the line instead of
// crashing on it.
void GameUI::AddLogLine(const std::string& line) {
	if (m_log) m_log->AddLine(line);
}

void GameUI::AddLogLine(const std::string& line, const Vec4& memberColor) {
	if (!m_log) return;
	// Identity colors are authored DARK (portrait fills, slot stripes); as
	// text ink on the dark footer they'd read as mud, so brighten toward
	// full — the hue carries the identity, the lift carries the legibility.
	const auto lift = [](float c) { return std::min(1.0f, c * 2.0f + 0.15f); };
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
	// Skinned: the button part frames the bar with the track inset as a dark
	// socket (the HandSlot treatment); flat mode keeps the bordered fill.
	const ui::Skin* skin = m_menuUi.GetSkin();
	if (skin && skin->button.texture) {
		ui::DrawNineSlice(m_spriteBatch, bar, skin->button, {1, 1, 1, 1});
		const float in = 3.0f;
		const gfx::Rect track{bar.x + in, bar.y + in, bar.w - 2 * in,
							  bar.h - 2 * in};
		m_spriteBatch.DrawRect(track, {0.0f, 0.0f, 0.0f, 1.0f});
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
		const float s = DeviceH() * 0.072f; // ~20% larger than a slot icon reads
		const gfx::Rect dst{m_hudMouseX - s * 0.5f, m_hudMouseY - s * 0.5f, s, s};
		m_spriteBatch.DrawSprite(dst, {0, 0, 1, 1}, *icon, {1, 1, 1, 1});
	}
}

void GameUI::RenderHud() {
	m_hudUi.Render(m_spriteBatch, DeviceW(), DeviceH());
	DrawHeldCursor();
}

} // namespace dungeon::game
