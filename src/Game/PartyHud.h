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
// Both widgets hold pointers into Game's character roster (stable for the
// session) and read the live values every draw; the sheet caches its
// formatted strings in SetCharacter so steady-state frames don't allocate.
// ============================================================================
#pragma once

#include "Game/Character.h"
#include "UI/Controls.h"

#include <array>
#include <flat_map>
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

class CharacterPanel : public ui::Widget {
public:
	// portraitFont draws the big placeholder initial (the Game passes its
	// title font, which tracks the window scale like everything else).
	// onClick fires on a left click on the PORTRAIT area (open the sheet / place a
	// held tablet); onRight on a right click there (open this member's inventory).
	// onBars fires on EITHER button over the stat-bar area (open the Stats tab).
	CharacterPanel(const gfx::Rect& rect, const Character* character,
				   const ui::Font* portraitFont, const ResourceBarColors* barColors,
				   const HitSplatIcons* hitSplats, std::function<void()> onClick,
				   std::function<void()> onRight, std::function<void()> onBars);

	void Update(ui::UIContext& ctx) override;
	void Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	// Multiplier on the slot background alpha (Settings → UI → Party Bar);
	// the border, portrait, name, and bars stay fully opaque.
	float backgroundOpacity = 1.0f;

private:
	// The stat-bar sub-region (right of the portrait, below the name) in pixels —
	// kept in sync with Draw's bar layout; a click here opens the Stats tab.
	gfx::Rect BarsRect(ui::UIContext& ctx) const;

	const Character* m_character;
	const ui::Font* m_portraitFont;
	const ResourceBarColors* m_barColors;
	const HitSplatIcons* m_hitSplats; // may be null (icons not loaded)
	std::function<void()> m_onClick;
	std::function<void()> m_onRight;
	std::function<void()> m_onBars;
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
	HandSlot(const gfx::Rect& rect, const Character* character, int hand,
			 const ItemIconBank* icons, std::function<void()> onLeft,
			 std::function<void()> onRight);

	void Update(ui::UIContext& ctx) override;
	void Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	const Character* m_character;
	int m_hand;
	const ItemIconBank* m_icons;
	std::function<void()> m_onLeft;
	std::function<void()> m_onRight;
	bool m_hot = false;
	bool m_held = false;       // left-button press latched on this slot
	bool m_heldRight = false;  // right-button press latched on this slot
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
	CharacterSheet(const gfx::Rect& rect, const ui::Font* portraitFont,
				   const ResourceBarColors* barColors, const ItemIconBank* icons,
				   const ItemWeightBank* weights, std::optional<std::string>* held);

	// Re-points the sheet (mutable, for inventory edits) and caches strings.
	void SetCharacter(Character& character);

	void Update(ui::UIContext& ctx) override;
	void Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	// Which body the sheet shows; the mode buttons under the portrait switch it.
	enum class Mode { Inventory, Stats, Skills };
	// Opens the sheet on a specific tab (the party bar uses this: portrait ->
	// Inventory, the stat bars -> Stats).
	void SetMode(Mode m) { m_mode = m; }

private:
	// Design-space rect of doll cell i (an index into the placed-cell table),
	// backpack slot i, and mode-toggle button i (0..2). Scaled by Draw/Update.
	gfx::Rect EquipRect(const gfx::Rect& px, float sx, float sy, int i) const;
	gfx::Rect PackRect(const gfx::Rect& px, float sx, float sy, int i) const;
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
	// Applies a held-aware click to a slot: place / swap / pick up.
	void ClickSlot(ItemSlot& slot);
	// Total carry weight (kg) of everything the member holds (equipment + pack).
	float CarryLoad() const;

	Character* m_character = nullptr;
	const ui::Font* m_portraitFont;
	const ResourceBarColors* m_barColors;
	const ItemIconBank* m_icons;
	const ItemWeightBank* m_weights;
	std::optional<std::string>* m_held;
	Mode m_mode = Mode::Inventory;
	int m_hotMode = -1; // mode button under the cursor (Update → Draw), -1 = none
	std::string m_healthText, m_staminaText, m_manaText; // "42 / 42"
	std::array<std::string, 5> m_attrValues;             // per-attribute numbers
	// Static page text, localized once at construction (the sheet is rebuilt
	// on a language change) so Draw stays allocation-free.
	std::string m_healthLabel, m_staminaLabel, m_manaLabel;
	std::string m_equipmentLabel, m_backpackLabel;
	std::string m_attributesLabel, m_skillsLabel, m_noSkills;
	std::array<std::string, 5> m_attrLabels;            // localized attribute names
	std::array<std::string, kEquipCount> m_equipLabels; // localized slot names
};

} // namespace dungeon::game
