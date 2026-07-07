// ============================================================================
// Game/PartyHud.h — party-facing UI widgets (Game layer: they know Character).
//
//   CharacterPanel  one slot of the top party bar: placeholder portrait,
//                   name, and health/stamina/mana bars. Hover highlights;
//                   clicking fires onClick (the Game opens the sheet).
//   HandSlot        one hand box of the HUD control panel (each member gets
//                   a left and a right hand, Dungeon Master style). Items
//                   don't exist yet, so it draws an empty framed box with
//                   the character's identity color along the bottom edge;
//                   clicking fires onClick (the HUD logs feedback).
//   CharacterSheet  the character details page: large portrait, name, the
//                   health/stamina/mana bars, and the inventory (a Dungeon
//                   Master-style equipment paper doll plus the backpack grid).
//                   Drawn over the frozen scene in AppState::CharacterSheet;
//                   the Game pairs it with prev/next/Back buttons in the same
//                   UIContext.
//
// The per-member widgets address Game's roster by (vector pointer, index)
// and RE-RESOLVE the member at the top of every Update/Draw — never caching
// a Character* across frames — so a roster whose SIZE changes (party
// creation builds 1..4 members) can never dangle them; a widget whose index
// fell off the end simply goes inert. The sheet still caches its formatted
// strings in SetCharacter so steady-state frames don't allocate.
// ============================================================================
#pragma once

#include "Game/Character.h"
#include "UI/Controls.h"

#include <array>
#include <flat_map>
#include <flat_set>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dungeon::game {

// Resource bar fill colors, shared by the party bar and the sheet. The
// master copy lives in Game (Settings → UI edits it, persisted to
// settings.ini as bar_<name>); both widgets point at it and read the live
// values every draw.
struct ResourceBarColors {
	Vec4 health{0.62f, 0.18f, 0.14f, 1.0f};
	Vec4 stamina{0.26f, 0.52f, 0.22f, 1.0f};
	Vec4 mana{0.22f, 0.36f, 0.68f, 1.0f};
};

// The three hit-feedback splat icons, indexed by severity (0 = small, 1 =
// medium, 2 = hard), drawn over a struck member's portrait. The textures are
// owned by Game (loaded from assets); the party panels point at this struct and
// read it live, so a null entry (icon missing) simply draws no splat.
// (Indexed rather than named because <windows.h> #defines `small` as char.)
struct HitSplatIcons {
	const gfx::Texture* icon[3] = {nullptr, nullptr, nullptr};
	const gfx::Texture* For(int severity) const {
		return icon[severity < 0 ? 0 : (severity > 2 ? 2 : severity)];
	}
};

// Item icons keyed by catalog id ("rune_fire" → its rune_icon_fire texture),
// for the held cursor, the hand slots, and the inventory window. Owned by Game
// (the textures live there); the HUD widgets read it live, so a missing id just
// draws no icon. The address handed to GameUI is stable.
struct ItemIconBank {
	std::flat_map<std::string, const gfx::Texture*> byType;
	const gfx::Texture* For(const std::string& typeId) const {
		const auto it = byType.find(typeId);
		return it == byType.end() ? nullptr : it->second;
	}
};

// Item carry weights (kg) keyed by catalog id, the data behind a member's carry
// load. Built once by Game from the items catalog; the sheet reads it live. A
// missing id weighs 0 (e.g. a typo or a weightless item).
struct ItemWeightBank {
	std::flat_map<std::string, float> byType;
	float For(const std::string& typeId) const {
		const auto it = byType.find(typeId);
		return it == byType.end() ? 0.0f : it->second;
	}
};

// Item categories + (for containers) content-slot capacities, keyed by catalog
// id. Built once by Game; the sheet reads it to tell whether a held item is a
// pack (container) and how many slots a freshly-equipped pack should have.
struct ItemCategoryBank {
	std::flat_map<std::string, std::string> byType;   // category
	std::flat_map<std::string, int> capacityByType;   // pack content slots
	// Categories a pack may HOLD (empty / contains "any" = unrestricted).
	std::flat_map<std::string, std::vector<std::string>> acceptsByType;
	// Ids whose catalog entry sets holdable=1 — the only items a HAND slot
	// (control bar or sheet doll) accepts. Everything else is refused.
	// (A set, not flat_map<...,bool>: vector<bool> can't back a flat_map.)
	std::flat_set<std::string> holdableTypes;
	bool Is(const std::string& typeId, std::string_view category) const {
		const auto it = byType.find(typeId);
		return it != byType.end() && it->second == category;
	}
	std::string CategoryOf(const std::string& typeId) const {
		const auto it = byType.find(typeId);
		return it == byType.end() ? std::string() : it->second;
	}
	// Content-slot capacity for a pack id, or 0 if unknown (caller defaults).
	int Capacity(const std::string& typeId) const {
		const auto it = capacityByType.find(typeId);
		return it == capacityByType.end() ? 0 : it->second;
	}
	// True if pack `packId` accepts an item of `category` in its contents.
	bool Accepts(const std::string& packId, const std::string& category) const {
		const auto it = acceptsByType.find(packId);
		if (it == acceptsByType.end() || it->second.empty()) return true; // unrestricted
		for (const std::string& a : it->second)
			if (a == "any" || a == category) return true;
		return false;
	}
	// True if pack `packId` may hold item `itemId`: the no-bag-in-a-bag rule plus
	// the accepts list. The one check shared by every place items enter a pack.
	bool PackAcceptsItem(const std::string& packId, const std::string& itemId) const {
		if (Is(itemId, "container")) return false; // no nesting bags
		return Accepts(packId, CategoryOf(itemId));
	}
	// True if a hand slot may hold this item (catalog holdable=1).
	bool Holdable(const std::string& typeId) const {
		return holdableTypes.contains(typeId);
	}
};

// Resolves roster slot `i` to the live member, or null when the roster is
// shorter than that (the party may have fewer than 4 members). The widgets
// call this at the top of Update/Draw instead of holding a Character*.
inline const Character* RosterMember(const std::vector<Character>* roster,
									 size_t i) {
	return roster && i < roster->size() ? &(*roster)[i] : nullptr;
}
inline Character* RosterMember(std::vector<Character>* roster, size_t i) {
	return roster && i < roster->size() ? &(*roster)[i] : nullptr;
}

class CharacterPanel : public ui::Widget {
public:
	// portraitFont draws the big placeholder initial (the Game passes its
	// title font, which tracks the window scale like everything else).
	// onClick fires on a left click on the PORTRAIT area (open the sheet / place a
	// held tablet); onRight on a right click there (open this member's inventory).
	// onBars fires on EITHER button over the stat-bar area (open the Stats tab).
	// `icons` (Game-owned, may be null) supplies the status-effect strip's
	// icon art (a ward draws the Protect rune tablet's icon). onEffects fires
	// on a left click on one of the strip's icons (open the sheet's Effects
	// tab — the icon's long form); it wins over onClick for that spot.
	CharacterPanel(const gfx::Rect& rect, const std::vector<Character>* roster,
				   size_t member,
				   const ui::Font* portraitFont, const ResourceBarColors* barColors,
				   const HitSplatIcons* hitSplats, const ItemIconBank* icons,
				   std::function<void()> onClick,
				   std::function<void()> onRight, std::function<void()> onBars,
				   std::function<void()> onEffects);

	void Update(ui::UIContext& ctx) override;
	void Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	// Multiplier on the slot background alpha (Settings → UI → Party Bar);
	// the border, portrait, name, and bars stay fully opaque.
	float backgroundOpacity = 1.0f;

private:
	// The stat-bar sub-region (right of the portrait, below the name) in pixels —
	// kept in sync with Draw's bar layout; a click here opens the Stats tab.
	gfx::Rect BarsRect(ui::UIContext& ctx) const;
	// The portrait square (left of the panel), and the Nth status-effect icon
	// — a row in the NAME band, right-aligned and growing right-to-left (many
	// effects stack; spill past the name is a later problem). One layout,
	// hit-tested by Update (hover names the effect) and drawn by Draw.
	gfx::Rect PortraitRect() const;
	gfx::Rect EffectIconRect(ui::UIContext& ctx, size_t index) const;

	const std::vector<Character>* m_roster;
	size_t m_member;
	// Resolved from (m_roster, m_member) at the top of every Update/Draw —
	// never valid across frames, so a roster resize can't dangle it.
	const Character* m_character = nullptr;
	const ui::Font* m_portraitFont;
	const ResourceBarColors* m_barColors;
	const HitSplatIcons* m_hitSplats; // may be null (icons not loaded)
	const ItemIconBank* m_icons;      // may be null (effect icons skip art)
	// Index into the member's effects list of the icon under the cursor
	// (size_t(-1) = none) — set by Update, read by Draw for the hover label.
	size_t m_hotEffect = static_cast<size_t>(-1);
	std::function<void()> m_onClick;
	std::function<void()> m_onRight;
	std::function<void()> m_onBars;
	std::function<void()> m_onEffects;
	bool m_hot = false;
	bool m_held = false;      // left-button press latched on this panel
	bool m_heldRight = false; // right-button press latched on this panel
};

class HandSlot : public ui::Widget {
public:
	// `hand` is 0 = left / 1 = right (which inventory.Hand() this box shows).
	// `icons` (Game-owned, may be null) resolves the held item's icon to draw.
	// onLeft fires on a left click, onRight on a right click — GameUI decides
	// what each means given the held cursor (place / swap / pick up / attack /
	// context menu).
	HandSlot(const gfx::Rect& rect, const std::vector<Character>* roster,
			 size_t member, int hand,
			 const ItemIconBank* icons, std::function<void()> onLeft,
			 std::function<void()> onRight);

	void Update(ui::UIContext& ctx) override;
	void Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	const std::vector<Character>* m_roster;
	size_t m_member;
	// Re-resolved every Update/Draw (see CharacterPanel).
	const Character* m_character = nullptr;
	int m_hand;
	const ItemIconBank* m_icons;
	std::function<void()> m_onLeft;
	std::function<void()> m_onRight;
	bool m_hot = false;
	bool m_held = false;       // left-button press latched on this slot
	bool m_heldRight = false;  // right-button press latched on this slot
};

// The HUD Magic-area SPELLBOOK: opened from a hand's use menu (Magic »
// Spellbook), it fills the control panel's magic box with the owning member's
// KNOWN SYMBOLS as rune buttons, the sequence "spelled out" so far, the name
// of the spell that sequence resolves to (when a known recipe matches), and
// Cast / Clear. This is where the player BUILDS a spell: click symbols to
// append (an unavailable symbol draws a disabled overlay and stops responding
// — spent symbols never repeat, and the SCHOOL rule holds: the four element
// runes are mutually exclusive, one leads every spell, so the other three go
// dark once one is down and non-school symbols wait until one is), click a
// sequence slot to remove that symbol AND everything spelled after it, Cast
// fires onCast (the world gates vocabulary/mana) and clears the slate. The
// sequence row sits at the bottom, just above Cast / Clear. Closed, it
// draws the dim "no spells" placeholder line the label used to show. One
// persistent widget — no HUD rebuild on open/close; it re-resolves its member
// by roster index every frame (RosterMember) and drops sequence symbols the
// member no longer knows, so a roster reset can't dangle or desync it.
class SpellbookPanel : public ui::Widget {
public:
	SpellbookPanel(const gfx::Rect& rect, const std::vector<Character>* roster,
				   const ItemIconBank* icons);

	// Shows this member's book (fresh sequence). `hand` is the hand whose
	// menu opened it — a Cast from the book credits that hand's quick-cast
	// MRU (defaults/MRU are per member AND per hand).
	void OpenFor(size_t member, size_t hand);
	void Close();
	bool IsOpen() const { return m_member >= 0; }

	// Cast pressed: (member, opening hand, the built sequence) — wired to the
	// world's cast façade. Fired only with a non-empty sequence.
	std::function<void(size_t, size_t, const std::vector<SpellSymbol>&)> onCast;
	// The recipe table, for the live "= <spell>" match label (GameUI's
	// spellDefs source). Null-safe: no table, no label.
	std::function<std::span<const SpellDef>()> spells;
	std::function<void()> onClick; // UI click feedback

	void Update(ui::UIContext& ctx) override;
	void Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	// One cell of the rune-button grid: the four SCHOOL runes always hold the
	// TOP ROW (schools-table order, drawn as empty frames until memorized);
	// other runes appear in the rows below as the member learns them.
	struct RuneSlot {
		SpellSymbol symbol;
		bool known;
	};
	std::vector<RuneSlot> RuneSlots(const Character& c) const;
	// Layout inside the live box, shared by Update (hit-test) and Draw. The
	// symbol grid indexes RuneSlots (4 per row); the sequence row indexes
	// m_sequence.
	gfx::Rect SymbolRect(const gfx::Rect& px, size_t i) const;
	gfx::Rect SequenceRect(const gfx::Rect& px, size_t i) const;
	gfx::Rect CastRect(const gfx::Rect& px) const;
	gfx::Rect ClearRect(const gfx::Rect& px) const;
	// The recipe the sequence spells out, if any.
	const SpellDef* Match() const;
	// Draws one rune face: the rune-item icon when loaded, else an
	// element-tinted fallback square; element-coloured border. Disabled (the
	// symbol is already in the sequence) washes it out under a dark overlay.
	void DrawRune(gfx::SpriteBatch& batch, const gfx::Rect& r, SpellSymbol s,
				  bool hot, bool disabled = false) const;

	const std::vector<Character>* m_roster;
	const ItemIconBank* m_icons;
	int m_member = -1;  // roster slot whose book is open (-1 = closed)
	size_t m_hand = 0;  // the hand whose menu opened the book (MRU credit)
	std::vector<SpellSymbol> m_sequence;
	int m_hotSymbol = -1, m_hotSeq = -1;
	bool m_hotCast = false, m_hotClear = false;
	std::string m_placeholder, m_castLabel, m_clearLabel; // localized once
};

// The COMBINED party inventory: a centered panel with one backpack column per
// member, for swapping items between characters at a glance. Opened by the
// sheet's "All" button or by right-clicking the world while carrying a tablet;
// non-modal (the world keeps running) but claims the mouse like the map overlay.
// Click a slot to drop the held tablet in (swapping any occupant onto the
// cursor) or, empty-handed, to pick the slot's item up; click off the panel —
// or Esc (handled by Game) — closes it. Overlay-drawn so it floats above the
// HUD; the held-cursor icon (drawn last) stays on top.
class InventoryWindow : public ui::Widget {
public:
	InventoryWindow(std::vector<Character>* roster, const ItemIconBank* icons,
					std::optional<std::string>* held);

	void Open() { m_open = true; }
	void Close() { m_open = false; }
	bool IsOpen() const { return m_open; }

	void Update(ui::UIContext& ctx) override;
	void Draw(ui::UIContext&, gfx::SpriteBatch&) override {} // overlay-only
	void DrawOverlay(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	int MemberCount() const;
	gfx::Rect PanelRect(const ui::UIContext& ctx) const;
	gfx::Rect SlotRect(const gfx::Rect& panel, int member, int slot) const;

	std::vector<Character>* m_roster;
	const ItemIconBank* m_icons;
	std::optional<std::string>* m_held;
	bool m_open = false;
	std::string m_title; // localized once at construction
};

// The character sheet. A fixed header (portrait, name, the health/stamina/mana
// bars) tops three switchable modes, toggled by the small icon buttons under
// the portrait (the active mode's button draws "pressed"):
//   * Inventory — the worn-equipment paper doll + the (dynamic) backpack grid.
//     Held-aware: a tablet carried on the cursor drops into an equipment or
//     backpack slot (swapping any occupant onto the cursor); empty-handed, a
//     click picks the slot's item up.
//   * Stats     — the member's attributes.
//   * Skills    — placeholder until a skill system exists.
// icons resolves item art; held is Game's cursor item (the overlay cursor is
// drawn by GameUI). The sheet is frozen-state, so it edits its member live
// (mutable pointer).
class CharacterSheet : public ui::Widget {
public:
	CharacterSheet(const gfx::Rect& rect, std::vector<Character>* roster,
				   const ui::Font* portraitFont,
				   const ResourceBarColors* barColors, const ItemIconBank* icons,
				   const ItemWeightBank* weights, const ItemIconBank* slotIcons,
				   const ItemCategoryBank* categories,
				   std::optional<std::string>* held);

	// Re-points the sheet at roster member `member` (mutable, for inventory
	// edits) and caches its strings. An out-of-range index leaves the sheet
	// showing nothing (Draw bails), never a stale member.
	void SetCharacter(size_t member);

	void Update(ui::UIContext& ctx) override;
	void Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	// Which body the sheet shows; the mode buttons under the portrait switch it.
	enum class Mode { Inventory, Stats, Skills, Effects };
	// Opens the sheet on a specific tab (the party bar uses this: portrait ->
	// Inventory, the stat bars -> Stats).
	void SetMode(Mode m) { m_mode = m; }

	// Fired when a held item is refused by the selected pack (item id, pack id) —
	// Game wires it to a "won't fit" log line + sound.
	std::function<void(const std::string&, const std::string&)> onRejectDrop;
	// Fired when a held non-holdable item is refused by a hand doll cell (item
	// id) — GameUI wires it to the shared "can't be held" log line + sound.
	std::function<void(const std::string&)> onRejectHold;

private:
	// Design-space rect of doll cell i (an index into the placed-cell table),
	// backpack slot i, and mode-toggle button i (0..2). Scaled by Draw/Update.
	gfx::Rect EquipRect(const gfx::Rect& px, float sx, float sy, int i) const;
	gfx::Rect PackRect(const gfx::Rect& px, float sx, float sy, int i) const;
	gfx::Rect PackRowRect(const gfx::Rect& px, float sx, float sy, int i) const;
	gfx::Rect ModeButtonRect(const gfx::Rect& px, float sx, float sy, int i) const;
	// Mode bodies + the shared mode-button strip (active button drawn pressed).
	void DrawModeButtons(ui::UIContext& ctx, gfx::SpriteBatch& batch,
						 const gfx::Rect& px, float sx, float sy);
	void DrawInventory(ui::UIContext& ctx, gfx::SpriteBatch& batch,
					   const gfx::Rect& px, float sx, float sy);
	void DrawStats(ui::UIContext& ctx, gfx::SpriteBatch& batch, const gfx::Rect& px,
				   float sx, float sy);
	void DrawSkills(ui::UIContext& ctx, gfx::SpriteBatch& batch, const gfx::Rect& px,
					float sx, float sy);
	void DrawEffects(ui::UIContext& ctx, gfx::SpriteBatch& batch,
					 const gfx::Rect& px, float sx, float sy);
	// Applies a held-aware click to a slot: place / swap / pick up.
	void ClickSlot(ItemSlot& slot);
	// Pack-row slot i was clicked: equip a held container into it, else select it.
	void EquipOrSelectPack(int i);
	// True if the SELECTED pack may hold `itemId` (its accepts list + the no-bag-
	// in-a-bag rule) — gates dropping a held item into the contents grid.
	bool PackAccepts(const std::string& itemId) const;
	// Total carry weight (kg) of everything the member holds (equipment + pack).
	float CarryLoad() const;

	std::vector<Character>* m_roster;
	size_t m_member = 0;
	// Re-resolved from (m_roster, m_member) at the top of every Update/Draw
	// (see CharacterPanel); the body helpers null-check it.
	Character* m_character = nullptr;
	const ui::Font* m_portraitFont;
	const ResourceBarColors* m_barColors;
	const ItemIconBank* m_icons;
	const ItemWeightBank* m_weights;
	const ItemIconBank* m_slotIcons; // equipment-slot outline silhouettes
	const ItemCategoryBank* m_categories; // item id → category (pack = container)
	std::optional<std::string>* m_held;
	Mode m_mode = Mode::Inventory;
	int m_hotMode = -1; // mode button under the cursor (Update → Draw), -1 = none
	std::string m_healthText, m_staminaText, m_manaText; // "42 / 42"
	std::array<std::string, 5> m_attrValues;             // per-attribute numbers
	// Skills-tab rows, baked by SetCharacter like the attribute values: the
	// localized skill name, the level number, the progress fraction toward
	// the next level, and the bar tint (school colour; weapon classes use
	// the theme accent via alpha 0 as the "no tint" flag).
	struct SkillRow {
		std::string label;
		std::string level;
		float frac = 0.0f;
		Vec4 tint{0, 0, 0, 0};
	};
	std::vector<SkillRow> m_skillRows;
	// Effects-tab rows, likewise baked by SetCharacter (the world is frozen
	// while the sheet is open, so effects can't change under it): the HUD
	// indicator's icon look (kind art + school tint + time sliver) plus the
	// long form — name, a magnitude-formatted description (loc key =
	// <nameKey>.desc), and the time left.
	struct EffectRow {
		StatusKind kind = StatusKind::Ward;
		Vec4 tint{1, 1, 1, 1};
		float frac = 0.0f; // timeLeft / duration, the icon's sliver
		std::string name, desc, time;
	};
	std::vector<EffectRow> m_effectRows;
	// Static page text, localized once at construction (the sheet is rebuilt
	// on a language change) so Draw stays allocation-free.
	std::string m_healthLabel, m_staminaLabel, m_manaLabel;
	std::string m_attributesLabel, m_skillsLabel, m_noSkills;
	std::string m_effectsLabel, m_noEffects;
	std::array<std::string, 5> m_attrLabels;            // localized attribute names
};

} // namespace dungeon::game
