// ============================================================================
// Game/CharacterSheet.h — the character details page (paper doll, packs,
// stats/skills/spells/effects tabs).
//
// Layout is parent-relative: every region is a fraction [0..1] of this
// widget's pixel rect (see Widget.h). No design-pixel artboard.
// ============================================================================
#pragma once

#include "Core/Loc.h"
#include "Game/PartyHudTypes.h"
#include "Game/Spells.h"
#include "UI/Controls.h"

#include <algorithm>
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
				  const size_t* member);

private:
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	const std::vector<Character>* m_roster;
	const size_t* m_member; // the sheet's live selection
};

// One button of the mode strip. A CUT STONE (more-ui-updates Phase 3): a
// ui::Button with its tab's symbol etched in (etch_tab_<mode>.png), acting on
// the press, and - while its mode is the one showing - held down with its gold
// lit. Without the skin it draws the old hand-drawn glyph (grid / bars / star /
// gem / hourglass) on a flat fill.
class ModeButton : public ui::Button {
public:
	ModeButton(const gfx::Rect& rect, int index, const int* activeIndex,
			   std::function<void(int)> onSelect);

private:
	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	int m_index;
	const int* m_active; // the sheet's live mode, as an index
};

// The strip of mode buttons; splits itself into even columns.
class ModeSelector : public ui::Widget {
public:
	ModeSelector(const gfx::Rect& rect, int count, const int* activeIndex,
				 std::function<void(int)> onSelect);
	// Each mode's etched symbol and its lit twin, in Mode order (null = none).
	void SetEtches(std::span<const gfx::Texture* const> etch,
				   std::span<const gfx::Texture* const> lit);
	// Stone `i`'s pixel rect as of the last layout (empty past the end) - for
	// a harness that clicks the tabs.
	gfx::Rect ButtonRect(size_t i) const {
		return i < m_buttons.size() ? m_buttons[i]->Pixel() : gfx::Rect{};
	}

private:
	std::vector<ModeButton*> m_buttons;
};

// One row of a list tab. Generic: it holds its index and asks its owner to draw
// it, so all three tabs share the row/scroll machinery rather than each
// re-implementing it.
class SheetRow : public ui::Widget {
public:
	using DrawFn = std::function<void(size_t index, ui::UIContext&,
									  gfx::SpriteBatch&, const gfx::Rect&)>;

	// `hover` is the owning list's hovered-row slot: a row under the pointer
	// writes its index there (the status bar reads it). It claims nothing, so
	// the wheel and the sheet behind it still see the pointer.
	SheetRow(size_t index, DrawFn draw, int* hover)
		: m_index(index), m_draw(std::move(draw)), m_hover(hover) {
		debugName = "SheetRow";
	}

private:
	// The scroll area's clip has already suppressed the pointer for a row that
	// is scrolled out of view (Widget::Update), so a hidden row is never hot.
	void UpdateSelf(ui::UIContext& ctx) override {
		const Input* input = ctx.CurrentInput();
		if (m_hover && input && !ctx.IsMouseConsumed() &&
			Pixel().Contains(input->MouseX(), input->MouseY()))
			*m_hover = static_cast<int>(m_index);
	}
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override {
		m_draw(m_index, ctx, batch, Pixel());
	}

	size_t m_index;
	DrawFn m_draw;
	int* m_hover;
};

// A scrolling list tab body: a heading, then one SheetRow per item inside a
// ui::ScrollArea. The owner says how many rows there are, how tall each one is
// (rows wrap their descriptions, so height is measured, not authored) and how
// to draw one — everything else, including the scrollbar, comes from ScrollArea.
class SheetList : public ui::Widget {
public:
	using Counter = std::function<size_t()>;
	// Row height in pixels. Takes the CONTEXT as well as the list's own font
	// because a row may set part of itself in another role (a spell's
	// description is Script) — and it has to MEASURE in the same face it will
	// DRAW in, or the wrap and the row height disagree.
	using Measure = std::function<float(size_t index, ui::UIContext& ctx,
										const ui::Font& font, float widthPx)>;

	SheetList(const gfx::Rect& rect, std::string heading, std::string emptyText,
			  Counter count, Measure measure, SheetRow::DrawFn drawRow);

	void ScrollToTop();
	// The row under the pointer this frame, or -1. Valid after the update walk
	// has visited the list (the sheet reads it in its own UpdateSelf, which runs
	// after its children).
	int HoveredRow() const { return m_hoverRow; }
	// Room for `n` rows before the list first shows - the row widgets and the
	// offset tables - so a tab opened mid-play builds nothing (the sheet's
	// frames are steady-state frames; see Game::SteadyStateFrame).
	void Warm(size_t n);

	// Fractions of this widget: where the heading sits and where the scrolling
	// band starts/stops. Set by the sheet from its shared layout table.
	float headingY = 0.0f;
	// The band's TOP is derived from the title's height in LayoutSelf (see
	// m_bandTop); only its bottom is authored.
	float bandBottom = 1.0f;

private:
	// Measures every row and stacks them, so the repeater's placer (which runs
	// later in the same layout pass) has the offsets ready.
	void LayoutSelf(ui::UIContext& ctx) override;
	// Clears the hovered row before the rows get their look at the pointer.
	void UpdateBeforeChildren(ui::UIContext&) override { m_hoverRow = -1; }
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
	// The repeater's resolved pixel height: the stacked content, or the band
	// when the content is shorter. Rows are placed as fractions of THIS, so a
	// short list does not stretch its rows to fill the band.
	float m_placeH = 0.0f;
	// Top of the scrolling band, resolved each layout from the title's line
	// advance so the rows can never be crowded by the heading above them.
	float m_bandTop = 0.0f;
	int m_hoverRow = -1; // written by the SheetRows (see HoveredRow)
};

class CharacterSheet : public ui::Widget {
public:
	// The panel's height as two WINDOW fractions: the tabs, at the height every
	// layout fraction was authored against, and the status bar under them.
	// GameUI sizes the panel as their sum; CharacterSheetLayout.h derives the
	// split from the same two numbers.
	static constexpr float kBodyH = 0.62f;
	static constexpr float kStatusH = 0.055f;

	// A CARD (more-ui-updates Phase 5, the party window): the same sheet, one
	// per member, showing only the TAB - no portrait, no tab stones, no status
	// band (the window has one of each) - under a band with the member's name.
	// The tab area keeps the sheet's proportions, so a card is the sheet's tab
	// at card size, and every fraction in CharacterSheetLayout.h still holds:
	// Body() is that area stretched back to a whole sheet body, its top above
	// the card. These are its measures in the card's own em; the window sets
	// the card's fontScale, which is what makes them small.
	static constexpr float kCardWEm = 47.0f;    // the sheet's width at 16:9
	static constexpr float kCardNameEm = 2.2f;  // the name band
	static constexpr float kCardTabEm = 17.1f;  // (1 - kHeaderY) of the sheet's body
	// A card's INVENTORY tab is its own layout (Phase 6, Michael: "keep the
	// backpack squares the same size as they are in the regular backpack"): no
	// paper doll, the carry load beside the name, then the pack row over the
	// selected bag's contents, six across - at the SHEET'S em (the window sets
	// the card's fontScale to it on this tab), so a square is the sheet's size.
	static constexpr float kCardInvPadEm = 0.8f;
	static constexpr float kCardSlotEm = 3.3f;     // the sheet's kPackW / kPackH
	static constexpr float kCardSlotGapEm = 0.47f; // its kPackGapX / kPackGapY
	static constexpr float kCardInvSepEm = 0.8f;   // pack row to contents
	static constexpr int kCardInvCols = 6;
	static constexpr float kCardInvWEm =
		2.0f * kCardInvPadEm + kCardInvCols * kCardSlotEm + (kCardInvCols - 1) * kCardSlotGapEm;
	// A card's height on the Inventory tab with `rows` rows of contents.
	static constexpr float CardInventoryHEm(int rows) {
		return kCardNameEm + kCardSlotGapEm + kCardSlotEm + kCardInvSepEm +
			   static_cast<float>(rows) * kCardSlotEm +
			   static_cast<float>(rows > 0 ? rows - 1 : 0) * kCardSlotGapEm + kCardInvPadEm;
	}
	CharacterSheet(const gfx::Rect& rect, std::vector<Character>* roster,
				   const ResourceBarStyle* barStyle, const ItemIconBank* icons,
				   const ItemWeightBank* weights, const ItemIconBank* slotIcons,
				   const ItemCategoryBank* categories,
				   HeldItem* held, bool card = false);
	bool IsCard() const { return m_card; }
	size_t Member() const { return m_member; }

	// Re-points the sheet at roster member `member` (mutable, for inventory
	// edits) and caches its strings. An out-of-range index leaves the sheet
	// showing nothing (Draw bails), never a stale member.
	void SetCharacter(size_t member);
	// The tab stones' etched symbols (and lit twins), in Mode order. Handed in
	// after construction: the sheet is built before the art load task runs.
	void SetModeEtches(std::span<const gfx::Texture* const> etch,
					   std::span<const gfx::Texture* const> lit) {
		if (m_modeStrip) m_modeStrip->SetEtches(etch, lit);
	}

	void LayoutSelf(ui::UIContext& ctx) override;
	void UpdateSelf(ui::UIContext& ctx) override;
	void DrawSelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;
	// The children (portrait, mode strip, lists, close box) lay out against the
	// BODY, not the whole panel, so the status bar beneath is nobody's but the
	// sheet's.
	gfx::Rect ContentRect() const override { return Body(); }

	// What the status bar says this frame (empty = nothing hovered). Exposed for
	// the dev console's `sheet status`, which is how a script reads it.
	std::string_view StatusName() const { return m_statusName.View(); }
	std::string_view StatusText() const { return m_statusText.View(); }
	const Vec4& StatusColor() const { return m_statusColor; }
	// Where pack slot `i` of the shown member's selected bag is, in pixels, as
	// of the last layout (Inventory tab). For the dev readout a harness aims by.
	gfx::Rect PackSlotRect(int i) const { return PackRect(Body(), i); }
	// Containers equipped into the pack row so far (see m_packEquips).
	unsigned PackEquips() const { return m_packEquips; }

	// Which body the sheet shows; the mode buttons under the portrait switch it.
	// (Order == the mode-button strip order — Spells sits before Effects.)
	enum class Mode { Inventory, Stats, Skills, Spells, Effects };
	// Opens the sheet on a specific tab (the party bar uses this: portrait ->
	// Inventory, the stat bars -> Stats).
	void SetMode(Mode m) { m_mode = m; }
	// The next (+1) or previous (-1) tab, wrapping at either end - Tab and
	// Shift+Tab (play-test #5). Goes through the same path as a mode button.
	void StepMode(int delta);
	Mode CurrentMode() const { return m_mode; }
	// Switches to tab `i` (a Mode as an index), scrolling the lists to the top
	// when it changes. The mode strip's click and StepMode both land here, and
	// the party window's tab stones, for all four of its cards.
	void SelectMode(int i);

	// Fired when a held item is refused by the selected pack (item id, pack id) —
	// Game wires it to a "won't fit" log line + sound.
	std::function<void(const std::string&, const std::string&)> onRejectDrop;
	// Fired when a held non-holdable item is refused by a hand doll cell (item
	// id) — GameUI wires it to the shared "can't be held" log line + sound.
	std::function<void(const std::string&)> onRejectHold;
	// The defense breakdown for the shown member, assembled by whoever knows
	// the world (DungeonWorld::DefenseFor). Null = the section is skipped, so
	// the sheet still builds in a context with no world behind it.
	std::function<DefenseReadout(const Character&)> defenseFor;
	// The same AS IF an item were worn — what the pack tooltip compares to.
	std::function<DefenseReadout(const Character&, const std::string&)> defenseWith;
	// The item mouse buttons (docs/ui-updates-plan.md P2), fired on a non-empty
	// doll cell, backpack slot or bag: RIGHT opens the item's details, MIDDLE its
	// use menu (a rune's Memorize works from the pack, not just a hand; Michael,
	// 2026-07-10). LEFT stays pick up / put down / swap. The place is in the
	// shown member's inventory; GameUI resolves it.
	std::function<void(ItemPlace)> onItemDetails;
	std::function<void(ItemPlace)> onItemUse;
	// The "Change portrait" button under the name (docs/portraits-plan.md P4):
	// GameUI opens the portrait picker for the shown member. A card has no
	// portrait and so no button.
	std::function<void()> onChangePortrait;
	// The card's background opacity, read live (Settings -> UI; the sheet is a
	// floating window, ui-panels P3b). Null = opaque.
	const float* opacity = nullptr;
	// The project's spell registry (wired to DungeonWorld::SpellDefs), so the
	// Spells tab can resolve a learned spell id -> its school, rune count, and
	// description. Null-safe: no registry, an empty Spells tab.
	std::function<std::span<const std::unique_ptr<Spell>>()> spells;

private:
	// The tabs' rect: the panel minus the status band at its foot. Every layout
	// fraction resolves against this, never against Pixel().
	gfx::Rect Body() const;
	// The status band itself.
	gfx::Rect StatusRect() const;
	// Sets the status bar from whatever the pointer is over on the active tab
	// (called every Update; clears it when nothing is). `pointerFree` = nothing
	// in front of the sheet has claimed the pointer. CharacterSheet_Status.cpp.
	void UpdateStatus(ui::UIContext& ctx, bool pointerFree);
	void SetStatus(std::string_view name, std::string_view text,
				   const Vec4& nameColor);
	// An item's status: its name and its weight - a bag's with its contents
	// (`contents`, empty for anything that is not an equipped pack).
	void SetItemStatus(const std::string& itemId, std::span<const ItemSlot> contents);
	void DrawStatus(ui::UIContext& ctx, gfx::SpriteBatch& batch);
	// The Stats tab's row geometry, shared by its draw and its hover test.
	struct StatRows {
		float top = 0.0f, step = 0.0f;
	};
	StatRows StatRowsFor(ui::UIContext& ctx, const gfx::Rect& px) const;

	// Layout helpers: rects as fractions of the sheet's pixel rect `px`.
	// (Inventory.cpp / Lists.cpp / shell .cpp split the definitions.)
	gfx::Rect EquipRect(const gfx::Rect& px, int i) const;
	gfx::Rect PackRect(const gfx::Rect& px, int i) const;
	gfx::Rect PackRowRect(const gfx::Rect& px, int i) const;
	// Builds the child widgets (portrait, mode strip, the three list tabs).
	void BuildParts();
	// The two bodies that neither scroll nor take a container of their own; they
	// fill the sheet and draw against it directly.
	// The armor tooltip (docs/damage-system.md). Hovering a WORN piece explains
	// the defense it gives; hovering one in the PACK compares it against what
	// is worn, so the two can be judged without swapping and swapping back.
	// Drawn in the overlay pass so no slot can paint over it.
	void DrawArmorTip(ui::UIContext& ctx, gfx::SpriteBatch& batch,
					  const gfx::Rect& px) const;
	void DrawOverlaySelf(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	void DrawInventory(ui::UIContext& ctx, gfx::SpriteBatch& batch,
					   const gfx::Rect& px);
	void DrawStats(ui::UIContext& ctx, gfx::SpriteBatch& batch, const gfx::Rect& px);
	// One row of each list tab, drawn into the rect the list gives it, plus the
	// height that row needs. Passed to the SheetLists as callbacks.
	// An effect row's symbol is square and spans exactly its NAME line, so the
	// three line up: symbol top = name top, symbol bottom = description top.
	// Both the size and the text column that follows it are shared by
	// MeasureEffectRow and DrawEffectRow, which must agree or the row's height
	// will not match what is drawn into it.
	float EffectIconSize(const ui::Font& nameFont) const;
	float EffectTextInset(const ui::Font& nameFont) const; // from the sheet's left

	float MeasureSkillRow(size_t i, ui::UIContext& ctx, const ui::Font& font,
					  float widthPx) const;
	float MeasureSpellRow(size_t i, ui::UIContext& ctx, const ui::Font& font,
					  float widthPx) const;
	float MeasureEffectRow(size_t i, ui::UIContext& ctx, const ui::Font& font,
					  float widthPx) const;
	void DrawSkillRow(size_t i, ui::UIContext& ctx, gfx::SpriteBatch& batch,
					  const gfx::Rect& r);
	void DrawSpellRow(size_t i, ui::UIContext& ctx, gfx::SpriteBatch& batch,
					  const gfx::Rect& r);
	void DrawEffectRow(size_t i, ui::UIContext& ctx, gfx::SpriteBatch& batch,
					   const gfx::Rect& r);
	// Which doll cell / pack slot / bag the pointer is over (m_hover*), every
	// frame; all -1 off the Inventory tab or when the pointer is claimed.
	void TrackInventoryHover(ui::UIContext& ctx, const gfx::Rect& px, bool pointerFree);
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
	bool m_card = false; // a party-window card (see kCardWEm)
	// Re-resolved from (m_roster, m_member) at the top of every Update/Draw
	// (see CharacterPanel); the body helpers null-check it.
	Character* m_character = nullptr;
	const ResourceBarStyle* m_barStyle;
	const ItemIconBank* m_icons;
	const ItemWeightBank* m_weights;
	const ItemIconBank* m_slotIcons; // equipment-slot outline silhouettes
	const ItemCategoryBank* m_categories; // item id → category (pack = container)
	HeldItem* m_held;
	// What the pointer is over, refreshed every Update: the doll cell index, or
	// the pack slot index, or neither. Only ever one of them.
	int m_hoverDoll = -1;
	int m_hoverPack = -1;
	// Containers equipped into the pack row this run, swaps included - how
	// tools\AllocTest.ps1 -Packs shows its clicks actually equipped something
	// (`sheet status`).
	unsigned m_packEquips = 0;
	int m_hoverPackRow = -1; // the bag row above the grid (status bar only)
	// The status bar's two halves, held inline (no heap: this is set every
	// frame the sheet is up) plus the colour the name draws in.
	loc::Line m_statusName, m_statusText;
	Vec4 m_statusColor{1, 1, 1, 1};
	Vec4 m_accent{1, 1, 1, 1}; // the theme accent, captured by UpdateStatus
	Mode m_mode = Mode::Inventory;
	// The mode as a plain index, for the button strip to read live.
	int m_modeIndex = 0;
	// The three scrolling tabs, in Mode order after Stats (Skills, Spells,
	// Effects); only the active one is visible. Owned as children.
	std::array<SheetList*, 3> m_lists{nullptr, nullptr, nullptr};
	ModeSelector* m_modeStrip = nullptr; // the tab stones (a child)
	// The Stats tab's numbers ("42 / 42", the attribute values) are formatted
	// at DRAW time into stack buffers, not baked: the world keeps running under
	// the sheet, so a value baked at open went stale while it was on screen.

	// A tab's rows, REUSED across bakes rather than rebuilt. Opening the sheet
	// is a click in a settled frame, and clearing a vector of strings then
	// pushing new ones allocated every row again (53 allocations a reopen).
	// Reset() only rewinds the count: a row keeps its strings' capacity, so a
	// re-bake assign()s into memory it already owns. Warm() builds the rows up
	// front (with each row's Reserve()) so even the first open finds them;
	// a member with more rows than that grows the pool once. Next() hands back
	// a used row, so a bake must set EVERY field of it.
	template <class Row> struct RowPool {
		std::vector<Row> rows;
		size_t count = 0;
		void Reset() { count = 0; }
		Row& Next() {
			if (count == rows.size()) rows.emplace_back().Reserve();
			return rows[count++];
		}
		void Truncate(size_t n) { count = std::min(count, n); }
		void Warm(size_t n) {
			while (rows.size() < n) rows.emplace_back().Reserve();
		}
		size_t size() const { return count; }
		const Row& operator[](size_t i) const { return rows[i]; }
	};
	// Skills-tab rows, baked by SetCharacter: the
	// localized skill name, the level number, the progress fraction toward
	// the next level, and the bar tint (school colour; weapon classes use
	// the theme accent via alpha 0 as the "no tint" flag).
	struct SkillRow {
		std::string id; // the skill id, for its status-bar hint (skill.<id>.hint)
		std::string label;
		std::string level;
		float frac = 0.0f;
		Vec4 tint{0, 0, 0, 0};
		// A GROUP HEADING rather than a skill: label only, no number, no bar.
		// The resource practices are shown in their own group
		// (docs/health-and-healing.md) because the trained skills are things you
		// CHOSE to practise and those are things your body did. One flag rather
		// than a second row type — the list walks one vector, and a heading is
		// simply a row that draws less.
		bool header = false;
		void Reserve() {
			id.reserve(31);
			label.reserve(63);
		}
	};
	RowPool<SkillRow> m_skillRows;
	// Spells-tab rows, baked by SetCharacter: the member's LEARNED spells in
	// school -> rune-count order, each with a school-tinted name and a
	// description (the spell's <id>.desc, its base power formatted in).
	// Resolved through the `spells` registry callback.
	struct SpellRow {
		std::vector<SpellSymbol> symbols; // the recipe, drawn as rune icons first
		std::string name, desc;
		Vec4 tint{1, 1, 1, 1};
		// A description is a loc::Line at most (loc::kCapacity), so it fits.
		void Reserve() {
			symbols.reserve(8);
			name.reserve(63);
			desc.reserve(loc::Line::kCapacity);
		}
	};
	RowPool<SpellRow> m_spellRows;
	std::vector<const Spell*> m_spellOrder; // BakeSpells' sort scratch
	// Effects-tab rows, likewise baked by SetCharacter and then again every
	// frame in UpdateSelf (the world runs under the sheet): the HUD
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
		void Reserve() {
			name.reserve(63);
			desc.reserve(loc::Line::kCapacity);
			time.reserve(63);
		}
	};
	RowPool<EffectRow> m_effectRows;
	// Static page text, localized once at construction (the sheet is rebuilt
	// on a language change) so Draw stays allocation-free.
	std::string m_healthLabel, m_staminaLabel, m_manaLabel;
	std::string m_foodLabel, m_waterLabel; // the two supply meters
	std::string m_attributesLabel, m_skillsLabel, m_noSkills;
	std::string m_effectsLabel, m_noEffects;
	std::string m_spellsLabel, m_noSpells;
	std::array<std::string, 5> m_attrLabels;            // localized attribute names
};
} // namespace dungeon::game
