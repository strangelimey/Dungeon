// ============================================================================
// Game/GameUI_Items.cpp - what the mouse buttons do to an ITEM: the hand
// boxes' left-click action, the use menus (right-click used to open them;
// middle-click does now - docs/ui-updates-plan.md P2), and the handlers the
// menus run (memorize, eat/drink, cast, strike). Split out of GameUI.cpp,
// which had outgrown the file-size rule.
// ============================================================================
#include "Game/GameUI.h"

#include "Core/Loc.h"
#include "Game/Spell/Spell.h"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>

namespace dungeon::game {

namespace {

// Hand-use command ids that resolve to a melee swing: the verb IS the attack
// (Balance's closed attack table: damage type + numbers), the strike is the
// one shared PartyAttack path. A new weapon verb is data (items.cat
// `command`) + an AttackSpec row in Balance + a row here + a use.<verb> lang
// key.
constexpr std::string_view kMeleeUses[] = {
	"punch", "kick", "stab",  "slash", "chop",  "bash",
	"swing", "jab",  "thrust", "hack", "melee", "attack"};
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
// The uses that work wherever the item sits, not only in a hand: the middle-
// click menu on a backpack slot, a worn piece or the party inventory offers
// these and nothing else.
bool IsOffHandUse(std::string_view cmd) {
	return cmd == "memorize" || cmd == "eat" || cmd == "drink";
}
// True if ExecuteUse can dispatch this id — unknown ids (a catalog typo) get
// no menu entry rather than a dead one.
bool IsExecutableUse(std::string_view cmd) {
	return cmd == "eat" || cmd == "drink" || cmd == "memorize" || cmd == "throw" ||
		   IsMeleeUse(cmd) || IsCastUse(cmd);
}
// The useDefaults key a hand's contents map to ("unarmed" for a bare hand —
// item ids are catalog tokens, so the sentinel can never collide). A view, so
// a lookup builds no string.
std::string_view UseKey(std::string_view itemId) {
	return itemId.empty() ? std::string_view("unarmed") : itemId;
}
// What a bare hand (or an unwired itemCommands) offers.
const std::vector<std::string> kNoCommands;

// The hand menu's row ids (ui::ContextMenu rows carry an int, not a closure):
// the range names the kind of use, the offset indexes that kind's own list.
constexpr int kUseItemCmd = 0;    // + index into the item's commands
constexpr int kUseUnarmed = 1000; // + index into kUnarmedUses
constexpr int kUseSpell = 2000;   // + index into spellDefs()
constexpr int kUseClear = 3000;   // forget this hand's pick (checked FIRST)
constexpr int kUseThrow = 4000;   // the throw every held item offers (checked next)
// The most quick-cast spells the Magic group lists (spellMruCount's clamp).
constexpr size_t kMaxMenuSpells = 10;

} // namespace

void GameUI::OnHandLeftClick(size_t i, size_t hand) {
	if (i >= m_characters.size() || hand > 1) return;
	ItemSlot& slot = m_characters[i].inventory.Hand(static_cast<int>(hand));
	if (Holding()) {
		// Place the carried item in this hand, swapping any occupant onto the
		// cursor (a click never silently destroys an item) — but only holdable
		// items enter a hand; anything else stays on the cursor with a log line.
		if (!m_itemCategories || !m_itemCategories->Holdable(**m_held)) {
			AddLogLine(loc::FormatLine("log.cant_hold", loc::ViewKey("item.", **m_held)));
			return;
		}
		m_held->SwapWith(slot); // one exchange, no allocation (HeldItem)
		Click();
		return;
	}
	// Empty cursor: the control-bar hand is an ACTION button — it executes the
	// hand's default use. Picking the item UP is a press-and-hold (OnHandHold),
	// so a swing can't be fumbled into an accidental unequip mid-fight. An UNSET
	// hand performs the item's own
	// first command without recording it (DefaultUseFor); only a hand with
	// nothing to do at all (bare hand, rune, key) opens the use menu, so that
	// first click PICKS what future clicks will do.
	const std::string_view cmd = DefaultUseFor(m_characters[i], hand, slot.typeId);
	if (cmd.empty()) {
		if (m_handMenu) OpenHandUseMenu(i, hand, *m_handMenu);
		return;
	}
	ExecuteUse(i, hand, cmd);
}

// HELD on a HUD hand box: the item comes OUT of the hand - onto an empty cursor,
// or swapped with the cursor's (which the click already does; the hold does it
// too, so a long press with an item in hand never surprises). One SwapWith
// either way: no allocation.
void GameUI::OnHandHold(size_t i, size_t hand) {
	if (i >= m_characters.size() || hand > 1 || !m_held) return;
	ItemSlot& slot = m_characters[i].inventory.Hand(static_cast<int>(hand));
	if (Holding()) {
		OnHandLeftClick(i, hand); // place / swap, with the holdable check
		return;
	}
	if (slot.Empty()) return;
	m_held->SwapWith(slot);
	Click();
}

// RIGHT on a HUD hand box = its use menu, where the hand's default is SET. The
// item mouse map (right = details) was tried here too and taken back (Michael,
// 2026-09-30): the hand boxes are controls, and picking what a hand does is
// what right-click on them is for. The doll's hand cells on the sheet keep the
// item map.
void GameUI::OnHandRightClick(size_t i, size_t hand) {
	if (m_handMenu) OpenHandUseMenu(i, hand, *m_handMenu);
}

// MIDDLE opens the same menu, so the item map's use button works on a hand box
// as it does everywhere else.
void GameUI::OnHandMiddleClick(size_t i, size_t hand) {
	if (m_handMenu) OpenHandUseMenu(i, hand, *m_handMenu);
}

// RIGHT on an item in a member's inventory: what it is and what it weighs where
// it sits (a bag counting its contents). Opened on a member's item, it knows
// whose it is, so a rune that member can learn offers its Memorize button; the
// floor, the cursor and the console open it with no holder, and no button.
void GameUI::OpenItemDetails(size_t i, ItemPlace place) {
	const std::string* id = ItemAt(i, place);
	if (!id) return;
	ShowItemDetails(*id, ItemWeightAt(i, place));
	if (!ItemDetailsOpen() || !CanMemorize(i, *id)) return;
	m_detailsMember = i;
	m_detailsPlace = place;
	m_detailsItem.assign(*id);
	m_itemDetails->ShowMemorize(true);
}

void GameUI::MemorizeFromDetails() {
	// The slot must still hold what the dialog was opened on: the world runs
	// under it, and a slot that changed is not acted on (the use menu's rule).
	ItemSlot* slot = SlotAt(m_detailsMember, m_detailsPlace);
	if (slot && slot->typeId == m_detailsItem) MemorizeSlot(m_detailsMember, *slot);
	CloseItemDetails(); // the tablet is spent, so there is nothing left to show
}

void GameUI::ShowItemDetails(const std::string& typeId, float weightKg) {
	if (!m_itemDetails || !itemDetails) return;
	ItemDetails details;
	if (!itemDetails(typeId, details)) return;
	Vec3 fitMin{}, fitMax{};
	Mat4 pose{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	const size_t subs = itemPreview ? itemPreview(typeId, m_itemDetails->PreviewBuffer(),
												  fitMin, fitMax, pose)
									: 0;
	m_itemDetails->SetPreview(subs, fitMin, fitMax, pose);
	m_itemDetails->Open(details, weightKg);
	Click();
}

bool GameUI::DismissPopup() {
	if (PortraitPickerOpen()) {
		m_portraitPicker->Close();
		return true;
	}
	if (ItemDetailsOpen()) {
		m_itemDetails->Close();
		return true;
	}
	for (ui::ContextMenu* menu : {m_sheetMenu, m_handMenu})
		if (menu && menu->IsOpen()) {
			menu->Close();
			return true;
		}
	return false;
}

// Drawn over whichever page is up; Game blits the 3D image into the pane after.
void GameUI::RenderItemDetails() {
	if (ItemDetailsOpen())
		m_itemDetails->Render(m_spriteBatch, m_settings.theme, DeviceW(), DeviceH());
}

// The portrait picker, for one roster member: titled with their name, the
// current portrait outlined, a pick handed to onSetPortrait. An index past the
// roster does nothing.
void GameUI::OpenPortraitPicker(size_t member) {
	if (!m_portraitPicker || member >= m_characters.size()) return;
	CloseItemDetails(); // one dialog at a time
	const Character& c = m_characters[member];
	m_portraitPicker->Open(loc::FormatLine("portrait.pick.title", c.name).View(), c.portraitId,
						   [this, member](const std::string& id) {
							   if (onSetPortrait) onSetPortrait(member, id);
						   });
	Click();
}

void GameUI::RenderPortraitPicker() {
	if (PortraitPickerOpen()) m_portraitPicker->Render(m_spriteBatch, DeviceW(), DeviceH());
}

const std::string* GameUI::ItemAt(size_t i, ItemPlace place) const {
	if (i >= m_characters.size() || place.index < 0) return nullptr;
	const Inventory& inv = m_characters[i].inventory;
	const size_t n = static_cast<size_t>(place.index);
	const std::string* id = nullptr;
	switch (place.kind) {
	case ItemPlace::Kind::Doll:
		if (n < inv.equipment.size()) id = &inv.equipment[n].typeId;
		break;
	case ItemPlace::Kind::Pack:
		if (n < inv.SelectedContents().size()) id = &inv.SelectedContents()[n].typeId;
		break;
	case ItemPlace::Kind::Bag:
		if (n < inv.packs.size()) id = &inv.packs[n].typeId;
		break;
	}
	return id && !id->empty() ? id : nullptr;
}

ItemSlot* GameUI::SlotAt(size_t i, ItemPlace place) {
	if (i >= m_characters.size() || place.index < 0) return nullptr;
	Inventory& inv = m_characters[i].inventory;
	const size_t n = static_cast<size_t>(place.index);
	if (place.kind == ItemPlace::Kind::Doll && n < inv.equipment.size())
		return &inv.equipment[n];
	if (place.kind == ItemPlace::Kind::Pack && n < inv.SelectedContents().size())
		return &inv.SelectedContents()[n];
	return nullptr; // a bag is a Pack, not a slot
}

float GameUI::ItemWeightAt(size_t i, ItemPlace place) const {
	const std::string* id = ItemAt(i, place);
	if (!id || !m_itemWeights) return 0.0f;
	float kg = m_itemWeights->For(*id);
	if (place.kind == ItemPlace::Kind::Bag) // a bag weighs what it holds too
		for (const ItemSlot& s :
			 m_characters[i].inventory.packs[static_cast<size_t>(place.index)].contents)
			if (!s.Empty()) kg += m_itemWeights->For(s.typeId);
	return kg;
}

// MIDDLE on an item anywhere but a hand: its use menu, holding only the uses
// that make sense OFF the hand - memorize, eat, drink. A weapon's verbs need
// it in a hand, so a sword in the pack has none and says so. A hand place gets
// the full hand menu instead (the doll's hand cells are the HUD boxes' twins).
void GameUI::OpenItemUseMenu(size_t i, ItemPlace place, ui::ContextMenu& menu) {
	if (i >= m_characters.size()) return;
	if (place.kind == ItemPlace::Kind::Doll &&
		(place.index == static_cast<int>(EquipSlot::LeftHand) ||
		 place.index == static_cast<int>(EquipSlot::RightHand))) {
		const size_t hand = place.index == static_cast<int>(EquipSlot::LeftHand) ? 0 : 1;
		OpenHandUseMenu(i, hand, menu);
		return;
	}
	const std::string* id = ItemAt(i, place);
	if (!id) return;
	m_useMenuFor = UseMenuFor::Slot;
	m_handMenuMember = i;
	m_useMenuPlace = place;
	m_handMenuItem.assign(*id);
	menu.Begin(m_hudMouseX, m_hudMouseY);
	bool any = false;
	const std::vector<std::string>& cmds = CommandsFor(m_handMenuItem);
	for (size_t k = 0; k < cmds.size(); ++k) {
		if (!IsOffHandUse(cmds[k])) continue;
		if (cmds[k] == "memorize" && !CanMemorize(i, m_handMenuItem)) continue;
		menu.Add(loc::ViewKey("use.", cmds[k]), kUseItemCmd + static_cast<int>(k));
		any = true;
	}
	if (!any) {
		// Nothing to offer: no menu at all, and the member says why (Michael,
		// 2026-09-30) - an empty popup, or nothing happening, both read as broken.
		AddLogLine(loc::FormatLine("log.no_use", m_characters[i].name),
				   m_characters[i].portraitColor);
		return;
	}
	menu.Show();
}

// Both context menus (the HUD's and the sheet's) pick through here: what the
// open menu is FOR was recorded when it opened.
void GameUI::OnUseMenuPick(int id) {
	if (m_useMenuFor == UseMenuFor::Hand) {
		OnHandMenuPick(id);
		return;
	}
	const size_t i = m_handMenuMember;
	ItemSlot* slot = SlotAt(i, m_useMenuPlace);
	// The slot must still hold what the menu was opened on: the world runs under
	// an open menu, and acting on whatever has moved into the slot since would
	// eat the wrong thing.
	if (!slot || slot->typeId != m_handMenuItem) return;
	const std::vector<std::string>& cmds = CommandsFor(m_handMenuItem);
	const size_t k = static_cast<size_t>(id - kUseItemCmd);
	if (id < kUseItemCmd || k >= cmds.size()) return;
	const std::string_view cmd = cmds[k];
	if (cmd == "memorize") MemorizeSlot(i, *slot);
	else if (cmd == "eat" || cmd == "drink") EatSlot(i, *slot);
}

const std::vector<std::string>& GameUI::CommandsFor(const std::string& itemId) const {
	if (itemId.empty() || !itemCommands) return kNoCommands;
	return itemCommands(itemId);
}

// Builds the menu without allocating (ui::ContextMenu's contract): labels are
// loc views copied into the menu's inline rows, and each row's id says which
// use it is (the kUse* ranges), decoded by OnHandMenuPick against the member,
// hand and item recorded here. `menu` is the context the click landed in: the
// HUD's for a hand box, the sheet's for a doll hand cell.
void GameUI::OpenHandUseMenu(size_t i, size_t hand, ui::ContextMenu& menu) {
	if (i >= m_characters.size() || hand > 1) return;
	const Character& c = m_characters[i];
	m_useMenuFor = UseMenuFor::Hand;
	m_handMenuMember = i;
	m_handMenuHand = hand;
	m_handMenuItem.assign(c.inventory.Hand(static_cast<int>(hand)).typeId);
	menu.Begin(m_hudMouseX, m_hudMouseY);
	// The item's own command entries (ItemKind::commands, supplied by Game).
	// Labels come from the use.<cmd> lang keys; an id ExecuteUse can't dispatch
	// (catalog typo) gets no entry, so adding a verb is data + one case there.
	const std::vector<std::string>& cmds = CommandsFor(m_handMenuItem);
	bool anyItemCmd = false;
	for (size_t k = 0; k < cmds.size(); ++k) {
		if (!IsExecutableUse(cmds[k])) continue;
		// A rune the member already knows offers no Memorize; with nothing else
		// of its own, its hand falls through to the bare-hand pickers below.
		if (cmds[k] == "memorize" && !CanMemorize(i, m_handMenuItem)) continue;
		menu.Add(loc::ViewKey("use.", cmds[k]), kUseItemCmd + static_cast<int>(k));
		anyItemCmd = true;
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
	if (!anyItemCmd) {
		// THIS hand's recency list - each hand keeps its own repertoire. Resolved
		// to registry indices FIRST, since whether the verbs group under Combat
		// depends on whether any spell survives, and rows are added in order.
		std::array<size_t, kMaxMenuSpells> spells{};
		size_t spellCount = 0;
		const auto defs =
			spellDefs ? spellDefs() : std::span<const std::unique_ptr<Spell>>{};
		const size_t limit = std::min(
			kMaxMenuSpells, static_cast<size_t>(std::max(0, m_settings.spellMruCount)));
		const SpellIdList& mru = c.spellMru[hand];
		for (size_t k = 0; k < mru.Size(); ++k) {
			const std::string_view id = mru[k];
			if (spellCount >= limit) break;
			// Skip ids the registry no longer carries (the MRU is state,
			// the spell classes are code — they can drift across edits).
			for (size_t d = 0; d < defs.size(); ++d)
				if (defs[d]->Id() == id) {
					spells[spellCount++] = d;
					break;
				}
		}
		const bool hasMagic = spellCount > 0;
		const int combat = hasMagic ? menu.AddGroup(loc::View("menu.combat"))
									: ui::ContextMenu::kTopLevel;
		for (size_t k = 0; k < std::size(kUnarmedUses); ++k)
			menu.Add(loc::ViewKey("use.", kUnarmedUses[k]),
					 kUseUnarmed + static_cast<int>(k), combat);
		if (hasMagic) {
			const int magic = menu.AddGroup(loc::View("menu.magic"));
			for (size_t s = 0; s < spellCount; ++s)
				menu.Add(loc::View(defs[spells[s]]->NameKey()),
						 kUseSpell + static_cast<int>(spells[s]), magic);
		}
	}
	// THROW, for anything the hand holds (Michael, ui-updates: "add throw to
	// every holdable item"). Added here rather than to every item's commands,
	// so it is never an unset hand's click - a key or a rune in a hand still
	// opens this menu on a click instead of flying off - and it does not count
	// as the item's own command, so the Combat / Magic pickers stay. An item
	// that lists `command = throw` already has its row above.
	if (!m_handMenuItem.empty() && std::ranges::find(cmds, "throw") == cmds.end())
		menu.Add(loc::View("use.throw"), kUseThrow);
	// Clear, LAST: takes this hand back to unset. Offered only while the hand
	// is SET (Michael, 2026-09-28) - the item's own first command is not a pick,
	// and a stale pick already reads as unset, so neither gets the row.
	if (!SetUseFor(c, hand, m_handMenuItem).empty())
		menu.Add(loc::View("use.clear"), kUseClear);
	menu.Show(); // nothing actionable = no rows, so no empty menu pops
}

void GameUI::OnHandMenuPick(int id) {
	const size_t i = m_handMenuMember;
	const size_t hand = m_handMenuHand;
	if (id == kUseClear) {
		if (i >= m_characters.size() || hand > 1) return;
		m_characters[i].useDefaults[hand].Remove(UseKey(m_handMenuItem));
		Click();
	} else if (id == kUseThrow) {
		SelectUse(i, hand, m_handMenuItem, "throw");
	} else if (id >= kUseSpell) {
		if (!spellDefs) return;
		const auto defs = spellDefs();
		const size_t k = static_cast<size_t>(id - kUseSpell);
		if (k >= defs.size()) return;
		// "cast:<id>", assembled on the stack (the recorded default's format).
		char buf[64];
		const std::string_view spell = defs[k]->Id();
		const size_t n = std::min(spell.size(), sizeof(buf) - kCastPrefix.size());
		std::copy(kCastPrefix.begin(), kCastPrefix.end(), buf);
		std::copy_n(spell.data(), n, buf + kCastPrefix.size());
		SelectUse(i, hand, m_handMenuItem, {buf, kCastPrefix.size() + n});
	} else if (id >= kUseUnarmed) {
		const size_t k = static_cast<size_t>(id - kUseUnarmed);
		if (k < std::size(kUnarmedUses)) SelectUse(i, hand, m_handMenuItem, kUnarmedUses[k]);
	} else {
		const std::vector<std::string>& cmds = CommandsFor(m_handMenuItem);
		const size_t k = static_cast<size_t>(id - kUseItemCmd);
		if (k < cmds.size()) SelectUse(i, hand, m_handMenuItem, cmds[k]);
	}
}

void GameUI::SelectUse(size_t i, size_t hand, std::string_view itemId,
					   std::string_view cmd) {
	if (i >= m_characters.size() || hand > 1) return;
	const bool menuOnly = IsMenuOnlyUse(cmd);
	// The pick becomes this member's default for THIS HAND and the item TYPE
	// (so every khukri in that hand chops until they choose otherwise; a
	// bare-hand pick records under the "unarmed" key) — the other hand keeps
	// its own pick, so left can be one spell and right another. Menu-only
	// commands are deliberate one-shots — never recorded.
	// UseDefaults stores the text inline, so recording allocates nothing.
	if (!menuOnly) m_characters[i].useDefaults[hand].Set(UseKey(itemId), cmd);
	// Menu-only commands always perform; a defaultable pick performs per the
	// Controls setting (off = the menu only arms the default).
	if (menuOnly || m_settings.useMenuExecutes) ExecuteUse(i, hand, cmd);
}

void GameUI::ExecuteUse(size_t i, size_t hand, std::string_view cmd) {
	if (i >= m_characters.size() || hand > 1 || cmd.empty()) return;
	if (cmd == "memorize") {
		MemorizeFromHand(i, hand);
	} else if (cmd == "eat" || cmd == "drink") {
		// ONE handler for both verbs. An apple feeds AND waters a little, so
		// splitting by verb would have meant writing the other half twice; the
		// item's `nutrition`/`hydration` say what it does and the verb only says
		// how it reads in the menu.
		EatFromHand(i, hand);
	} else if (IsCastUse(cmd)) {
		// A "cast:<id>" default: the world's cast façade gates vocabulary and
		// mana and turns the outcome into log + sound; the firing hand's
		// quick-cast MRU is credited.
		if (onCastSpell) onCastSpell(i, cmd.substr(kCastPrefix.size()), hand);
	} else if (cmd == "throw") {
		// THIS member throws what THIS hand holds, through the same throw the
		// cursor makes (DungeonWorld::ThrowItem - the attack formula, the flight,
		// the payload). The hand empties only if the throw was made; a member
		// still recovering from the last one keeps it.
		ItemSlot& slot = m_characters[i].inventory.Hand(static_cast<int>(hand));
		if (!slot.Empty() && onHandThrow && onHandThrow(i, slot.typeId, slot.charge)) {
			slot.Clear();
			RefreshSheet(); // the carry load may be on screen
		}
	} else if (IsMeleeUse(cmd)) {
		// Every melee verb lands through the one strike path; the verb IS the
		// attack (damage type + numbers, Balance::FindAttack). Cooldown gating
		// and the alive-check live in DungeonWorld::PartyAttack.
		if (onHandAttack) onHandAttack(i, hand, cmd);
	}
	// Unknown id: a catalog typo — the menu never offered it; a stale saved
	// default falls through DefaultUseFor instead. Nothing to do.
}

std::string_view GameUI::SetUseFor(const Character& c, size_t hand,
								  const std::string& itemId) const {
	if (hand > 1) return {};
	// The pick must still be valid - the catalog may have changed since the save
	// was written, and a "cast:" default needs the member to know the spell (a
	// loaded save's defaults must not outrun its vocabulary). A stale pick reads
	// as unset.
	const std::string_view picked = c.useDefaults[hand].Find(UseKey(itemId));
	if (!picked.empty() && UseValidFor(c, CommandsFor(itemId), picked)) return picked;
	return {};
}

// Every frame, per hand box: views, a registry scan and an inline loc::Line -
// nothing on the heap.
HandSetUse GameUI::HandSetUseFor(size_t i, size_t hand) const {
	if (i >= m_characters.size() || hand > 1) return {};
	const Character& c = m_characters[i];
	const std::string_view set =
		SetUseFor(c, hand, c.inventory.Hand(static_cast<int>(hand)).typeId);
	if (set.empty()) return {};
	HandSetUse use;
	use.set = true;
	if (IsCastUse(set)) {
		// SetUseFor only returns a cast whose spell is in the registry.
		const std::string_view id = set.substr(kCastPrefix.size());
		if (spellDefs)
			for (const auto& def : spellDefs())
				if (def->Id() == id) {
					use.spell = def.get();
					use.label = loc::View(def->NameKey());
					break;
				}
	} else {
		// The same text the use menu's row showed (use.<verb>).
		use.label = loc::ViewKey("use.", set);
		// Whether it has a picture is the hand box's lookup (ui/use_<verb>.png).
		use.verb = set;
	}
	return use;
}

std::string_view GameUI::DefaultUseFor(const Character& c, size_t hand,
								  const std::string& itemId) const {
	if (hand > 1) return {};
	// SET and DEFAULT are two things (Michael, 2026-09-28). A hand is SET only
	// once the player picks a use from its menu, and only a set hand is shown
	// as set or can be cleared. An UNSET hand still does something on a left
	// click: the item's own first defaultable command, performed WITHOUT being
	// recorded, so the hand stays unset (a rune's only command is the
	// menu-only memorize, so it yields "" - a left-click can't eat a tablet).
	if (const std::string_view set = SetUseFor(c, hand, itemId); !set.empty()) return set;
	for (const std::string& cmd : CommandsFor(itemId))
		if (!IsMenuOnlyUse(cmd) && IsExecutableUse(cmd)) return cmd;
	return {}; // nothing to do - the left-click opens the use menu to pick one
}

bool GameUI::UseValidFor(const Character& c, const std::vector<std::string>& cmds,
						 std::string_view cmd) const {
	if (IsMenuOnlyUse(cmd) || !IsExecutableUse(cmd)) return false;
	if (IsCastUse(cmd)) {
		const std::string_view id = cmd.substr(kCastPrefix.size());
		if (!spellDefs) return false;
		for (const auto& def : spellDefs())
			if (def->Id() == id) return c.HasLearnedSpell(def->Id());
		return false; // spell gone from the registry
	}
	if (std::ranges::find(cmds, cmd) != cmds.end()) return true;
	// Anything held can be thrown (the menu offers it for every item; a bare
	// hand's throw does nothing - ExecuteUse finds the hand empty).
	if (cmd == "throw") return true;
	// The bare-hand combat verbs are pickable for any hand contents.
	return std::ranges::find(kUnarmedUses, cmd) != std::ranges::end(kUnarmedUses);
}

void GameUI::MemorizeFromHand(size_t i, size_t hand) {
	if (i >= m_characters.size() || hand > 1) return;
	MemorizeSlot(i, m_characters[i].inventory.Hand(static_cast<int>(hand)));
}

bool GameUI::CanMemorize(size_t i, std::string_view itemId) const {
	SpellSymbol sym;
	return i < m_characters.size() && RuneSymbolFromItemId(itemId, sym) &&
		   !m_characters[i].Knows(sym);
}

void GameUI::MemorizeSlot(size_t i, ItemSlot& slot) {
	// Never spends a tablet on a rune the member already knows.
	if (!CanMemorize(i, slot.typeId)) return;
	SpellSymbol sym;
	if (!RuneSymbolFromItemId(slot.typeId, sym)) return;
	m_characters[i].Learn(sym);
	slot.Clear(); // the tablet is consumed
	Click();
	AddLogLine(loc::FormatLine("log.memorize", m_characters[i].name,
							   loc::View(RuneNameKey(sym))),
			   m_characters[i].portraitColor);
	RefreshSheet(); // the sheet's known symbols may be on screen later
}

void GameUI::EatFromHand(size_t i, size_t hand) {
	if (i >= m_characters.size() || hand > 1) return;
	EatSlot(i, m_characters[i].inventory.Hand(static_cast<int>(hand)));
}

// The shared eat/drink, from WHEREVER the item sits (a hand, the pack, a worn
// slot) - the memorize pattern.
void GameUI::EatSlot(size_t i, ItemSlot& slot) {
	if (i >= m_characters.size() || slot.Empty()) return;
	Character& c = m_characters[i];
	// Localized name by the item.<id> convention (matches ItemKind::nameKey);
	// read BEFORE the slot is cleared, into an inline Line (it used to be a
	// std::string built by std::format - an allocation per bite).
	const loc::Line foodName = loc::ViewKey("item.", slot.typeId);
	// The world owns the catalogs and the meters, so it owns the arithmetic
	// (docs/health-and-healing.md). This used to restore a flat 25% of max
	// STAMINA — a placeholder from the items thread, kept only because nothing
	// consumed food. It is re-pointed here rather than replaced: an existing
	// seam, like SpendStamina's creep.
	const resource::Refill got =
		onConsume ? onConsume(i, slot.typeId) : resource::Refill{};
	if (!got.Any()) {
		// It fed nobody — either the item has no nutrition at all, or this
		// member is already full. Refuse rather than silently eating it: losing
		// the last apple to a full stomach is the kind of thing a player never
		// forgives, and never notices happening.
		AddLogLine(loc::FormatLine("log.eat_no_effect", c.name, foodName),
				   c.portraitColor);
		return;
	}
	// Consumed - or, for a container, stepped down a fill level (a waterskin
	// drunk from is a half-full one now, items.cat `drink_as`). Assigned into
	// the slot's own buffer: a shorter id never allocates.
	if (const std::string_view leaves = consumeLeaves ? consumeLeaves(slot.typeId)
													  : std::string_view{};
		!leaves.empty())
		slot.typeId.assign(leaves);
	else
		slot.Clear();
	Click();
	AddLogLine(loc::FormatLine("log.eat", c.name, foodName), c.portraitColor);
	RefreshSheet(); // the supply bars / carry load may be on screen
}

} // namespace dungeon::game
