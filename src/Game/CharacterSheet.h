// ============================================================================
// Game/CharacterSheet.h — the character details page (paper doll, packs,
// stats/skills/spells/effects tabs).
//
// Layout is parent-relative: every region is a fraction [0..1] of this
// widget's pixel rect (see Widget.h). No design-pixel artboard.
// ============================================================================
#pragma once

#include "Game/PartyHudTypes.h"
#include "Game/Spells.h"
#include "UI/Controls.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dungeon::game {

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
	// (Order == the mode-button strip order — Spells sits before Effects.)
	enum class Mode { Inventory, Stats, Skills, Spells, Effects };
	// Opens the sheet on a specific tab (the party bar uses this: portrait ->
	// Inventory, the stat bars -> Stats).
	void SetMode(Mode m) { m_mode = m; }

	// Fired when a held item is refused by the selected pack (item id, pack id) —
	// Game wires it to a "won't fit" log line + sound.
	std::function<void(const std::string&, const std::string&)> onRejectDrop;
	// Fired when a held non-holdable item is refused by a hand doll cell (item
	// id) — GameUI wires it to the shared "can't be held" log line + sound.
	std::function<void(const std::string&)> onRejectHold;
	// Fired by a RIGHT-click on a non-empty backpack slot (the slot's index in
	// the selected pack) — GameUI opens the item's use menu there (a rune's
	// Memorize works from the pack, not just a hand; Michael, 2026-07-10).
	std::function<void(int slot)> onSlotMenu;
	// The project's spell registry (wired to DungeonWorld::SpellDefs), so the
	// Spells tab can resolve a learned spell id -> its school, rune count, and
	// description. Null-safe: no registry, an empty Spells tab.
	std::function<std::span<const std::unique_ptr<Spell>>()> spells;

private:
	// Layout helpers: rects as fractions of the sheet's pixel rect `px`.
	// (Inventory.cpp / Lists.cpp / shell .cpp split the definitions.)
	gfx::Rect EquipRect(const gfx::Rect& px, int i) const;
	gfx::Rect PackRect(const gfx::Rect& px, int i) const;
	gfx::Rect PackRowRect(const gfx::Rect& px, int i) const;
	gfx::Rect ModeButtonRect(const gfx::Rect& px, int i) const;
	// Mode bodies + the shared mode-button strip (active button drawn pressed).
	void DrawModeButtons(ui::UIContext& ctx, gfx::SpriteBatch& batch,
						 const gfx::Rect& px);
	void DrawInventory(ui::UIContext& ctx, gfx::SpriteBatch& batch,
					   const gfx::Rect& px);
	void DrawStats(ui::UIContext& ctx, gfx::SpriteBatch& batch, const gfx::Rect& px);
	void DrawSkills(ui::UIContext& ctx, gfx::SpriteBatch& batch, const gfx::Rect& px);
	void DrawEffects(ui::UIContext& ctx, gfx::SpriteBatch& batch,
					 const gfx::Rect& px);
	void DrawSpells(ui::UIContext& ctx, gfx::SpriteBatch& batch, const gfx::Rect& px);
	// Shared vertical scroll for the list tabs (Skills / Spells / Effects).
	gfx::Rect ScrollViewRect(const gfx::Rect& px) const;
	gfx::Rect ScrollThumbRect(const gfx::Rect& px, const gfx::Rect& view,
							  float maxScroll) const;
	void UpdateScroll(ui::UIContext& ctx, const gfx::Rect& px);
	void DrawScrollbar(ui::UIContext& ctx, gfx::SpriteBatch& batch,
					   const gfx::Rect& px, const gfx::Rect& view);
	// Inventory hit-testing (left/right click on doll + packs).
	void UpdateInventory(ui::UIContext& ctx, const gfx::Rect& px, float mx, float my,
						 bool clicked);
	// SetCharacter bakes — each lives next to its Draw* file.
	void BakeStats();
	void BakeSkills();
	void BakeSpells();
	void BakeEffects();
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
	// List-tab scroll state (see the Scroll helpers above). Content/view heights
	// are cached by the active list Draw each frame; Update clamps against them.
	float m_scroll = 0.0f, m_scrollGrab = 0.0f;
	bool m_scrollDragging = false;
	float m_scrollContentH = 0.0f, m_scrollViewH = 0.0f;
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
	// Spells-tab rows, baked by SetCharacter: the member's LEARNED spells in
	// school -> rune-count order, each with a school-tinted name and a
	// description (the spell's <id>.desc, its base power formatted in).
	// Resolved through the `spells` registry callback.
	struct SpellRow {
		std::vector<SpellSymbol> symbols; // the recipe, drawn as rune icons first
		std::string name, desc;
		Vec4 tint{1, 1, 1, 1};
	};
	std::vector<SpellRow> m_spellRows;
	// Effects-tab rows, likewise baked by SetCharacter (the world is frozen
	// while the sheet is open, so effects can't change under it): the HUD
	// indicator's icon look (kind art + school tint + time sliver) plus the
	// long form — name, a magnitude-formatted description (loc key =
	// <nameKey>.desc), and the time left.
	struct EffectRow {
		// The effect's kind, for the icon art it borrows. Safe to hold: the
		// kinds live in the EffectBook for the app's lifetime (Effect.h).
		const fx::EffectKind* kind = nullptr;
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
	std::string m_spellsLabel, m_noSpells;
	std::array<std::string, 5> m_attrLabels;            // localized attribute names
};
} // namespace dungeon::game
