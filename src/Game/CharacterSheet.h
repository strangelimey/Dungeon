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

// The member's bust in the sheet's header band. Its own rect, no input.
class SheetPortrait : public ui::Widget {
public:
	SheetPortrait(const gfx::Rect& rect, const std::vector<Character>* roster,
				  const size_t* member, const ui::Font* font);

private:
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	const std::vector<Character>* m_roster;
	const size_t* m_member; // the sheet's live selection
	const ui::Font* m_font;
};

// One button of the mode strip: a hand-drawn glyph (grid / bars / star / gem /
// hourglass), owning its own hover, pressed while its mode is the active one.
class ModeButton : public ui::Widget {
public:
	ModeButton(const gfx::Rect& rect, int index, const int* activeIndex,
			   std::function<void(int)> onSelect);

private:
	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	int m_index;
	const int* m_active; // the sheet's live mode, as an index
	std::function<void(int)> m_onSelect;
	bool m_hot = false;
};

// The strip of mode buttons; splits itself into even columns.
class ModeSelector : public ui::Widget {
public:
	ModeSelector(const gfx::Rect& rect, int count, const int* activeIndex,
				 std::function<void(int)> onSelect);
};

// One row of a list tab. Generic: it holds its index and asks its owner to draw
// it, so all three tabs share the row/scroll machinery rather than each
// re-implementing it.
class SheetRow : public ui::Widget {
public:
	using DrawFn = std::function<void(size_t index, ui::UIContext&,
									  gfx::SpriteBatch&, const gfx::Rect&)>;

	SheetRow(size_t index, DrawFn draw) : m_index(index), m_draw(std::move(draw)) {
		debugName = "SheetRow";
	}

private:
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override {
		m_draw(m_index, ctx, batch, Pixel());
	}

	size_t m_index;
	DrawFn m_draw;
};

// A scrolling list tab body: a heading, then one SheetRow per item inside a
// ui::ScrollArea. The owner says how many rows there are, how tall each one is
// (rows wrap their descriptions, so height is measured, not authored) and how
// to draw one — everything else, including the scrollbar, comes from ScrollArea.
class SheetList : public ui::Widget {
public:
	using Counter = std::function<size_t()>;
	// Row height in pixels, given the font and the width available to it.
	using Measure =
		std::function<float(size_t index, const ui::Font& font, float widthPx)>;

	SheetList(const gfx::Rect& rect, std::string heading, std::string emptyText,
			  Counter count, Measure measure, SheetRow::DrawFn drawRow);

	void ScrollToTop();

	// Fractions of this widget: where the heading sits and where the scrolling
	// band starts/stops. Set by the sheet from its shared layout table.
	float headingY = 0.0f;
	float bandTop = 0.0f;
	float bandBottom = 1.0f;

private:
	// Measures every row and stacks them, so the repeater's placer (which runs
	// later in the same layout pass) has the offsets ready.
	void LayoutSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	// The scrolling band's view height in pixels, worked out from this widget's
	// own rect (the ScrollArea itself is laid out after this runs).
	float ViewHeight() const;

	std::string m_heading, m_empty;
	Counter m_count;
	Measure m_measure;
	ui::ScrollArea* m_scroll = nullptr;
	ui::Repeater* m_rows = nullptr;
	// Row tops and heights in pixels, rebuilt every layout, plus their total.
	// The repeater's own bounds carry that total (as a multiple of the view
	// height) — the scroll area measures overflow from its CHILDREN's bounds,
	// and the rows are its grandchildren, so the repeater has to report it.
	std::vector<float> m_rowTop, m_rowH;
	float m_contentH = 0.0f;
};

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

	void LayoutSelf(ui::UIContext& ctx) override;
	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

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
	// Builds the child widgets (portrait, mode strip, the three list tabs).
	void BuildParts();
	// The two bodies that neither scroll nor take a container of their own; they
	// fill the sheet and draw against it directly.
	void DrawInventory(ui::UIContext& ctx, gfx::SpriteBatch& batch,
					   const gfx::Rect& px);
	void DrawStats(ui::UIContext& ctx, gfx::SpriteBatch& batch, const gfx::Rect& px);
	// One row of each list tab, drawn into the rect the list gives it, plus the
	// height that row needs. Passed to the SheetLists as callbacks.
	float MeasureSkillRow(size_t i, const ui::Font& font, float widthPx) const;
	float MeasureSpellRow(size_t i, const ui::Font& font, float widthPx) const;
	float MeasureEffectRow(size_t i, const ui::Font& font, float widthPx) const;
	void DrawSkillRow(size_t i, ui::UIContext& ctx, gfx::SpriteBatch& batch,
					  const gfx::Rect& r);
	void DrawSpellRow(size_t i, ui::UIContext& ctx, gfx::SpriteBatch& batch,
					  const gfx::Rect& r);
	void DrawEffectRow(size_t i, ui::UIContext& ctx, gfx::SpriteBatch& batch,
					   const gfx::Rect& r);
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
	// The mode as a plain index, for the button strip to read live.
	int m_modeIndex = 0;
	// The three scrolling tabs, in Mode order after Stats (Skills, Spells,
	// Effects); only the active one is visible. Owned as children.
	std::array<SheetList*, 3> m_lists{nullptr, nullptr, nullptr};
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
