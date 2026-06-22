#include "Game/PartyHud.h"

#include "Core/Loc.h"

#include <algorithm>
#include <format>

namespace dungeon::game {

namespace {

void DrawStatBar(gfx::SpriteBatch& batch, const gfx::Rect& rect, float fraction,
				 const Vec4& color, const ui::Theme& theme) {
	batch.DrawRect(rect, theme.control);
	const float t = std::clamp(fraction, 0.0f, 1.0f);
	if (t > 0.0f) batch.DrawRect({rect.x, rect.y, rect.w * t, rect.h}, color);
	ui::DrawBorder(batch, rect, theme.panelBorder);
}

// Baked portrait texture when present; otherwise the tinted square with the
// character's initial as a fallback (fresh checkout before AssetBaker runs).
// The border is the character's identity color, matching the HandSlot
// stripe — doubled so the color coding actually reads at party-bar size.
void DrawIdentityBorder(gfx::SpriteBatch& batch, const gfx::Rect& rect,
						const Character& character) {
	ui::DrawBorder(batch, rect, character.portraitColor);
	ui::DrawBorder(batch, {rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 2},
				   character.portraitColor);
}

void DrawPortrait(gfx::SpriteBatch& batch, const gfx::Rect& rect,
				  const Character& character, const ui::Font& font,
				  const ui::Theme& theme) {
	if (character.portrait) {
		batch.DrawSprite(rect, {0, 0, 1, 1}, *character.portrait, {1, 1, 1, 1});
		DrawIdentityBorder(batch, rect, character);
		return;
	}
	batch.DrawRect(rect, character.portraitColor);
	DrawIdentityBorder(batch, rect, character);
	const std::string_view initial = std::string_view(character.name).substr(0, 1);
	const float initialW = font.MeasureWidth(initial);
	font.Draw(batch, initial, rect.x + (rect.w - initialW) * 0.5f,
			  rect.y + (rect.h - font.Height()) * 0.5f, theme.text);
}

} // namespace

// --- CharacterPanel ----------------------------------------------------------

CharacterPanel::CharacterPanel(const gfx::Rect& rect, const Character* character,
							   const ui::Font* portraitFont,
							   const ResourceBarColors* barColors,
							   const HitSplatIcons* hitSplats,
							   std::function<void()> onClick,
							   std::function<void()> onRight,
							   std::function<void()> onBars)
	: m_character(character), m_portraitFont(portraitFont), m_barColors(barColors),
	  m_hitSplats(hitSplats), m_onClick(std::move(onClick)),
	  m_onRight(std::move(onRight)), m_onBars(std::move(onBars)) {
	bounds = rect;
}

// The stat-bar strip: right of the portrait, below the name (mirrors Draw's
// layout so the click target matches what's drawn).
gfx::Rect CharacterPanel::BarsRect(ui::UIContext& ctx) const {
	const gfx::Rect& px = Pixel();
	const float pad = px.h * 0.08f;
	const float left = px.x + pad + (px.h - 2 * pad) + pad; // past the portrait
	const float right = px.x + px.w - pad;
	const float barsTop = px.y + pad + ctx.GetFont().LineAdvance() + 2.0f;
	const float barsBottom = px.y + px.h - pad;
	return {left, barsTop, right - left, barsBottom - barsTop};
}

void CharacterPanel::Update(ui::UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const float mx = input->MouseX(), my = input->MouseY();
	m_hot = !ctx.IsMouseConsumed() && Pixel().Contains(mx, my);
	if (m_hot) {
		if (input->WasMousePressed(MouseButton::Left)) m_held = true;
		if (input->WasMousePressed(MouseButton::Right)) m_heldRight = true;
		ctx.ConsumeMouse();
	}
	// A click over the stat bars (either button) opens the Stats tab; elsewhere on
	// the panel keeps the portrait actions (left = sheet/stow, right = backpack).
	if (m_held && input->WasMouseReleased(MouseButton::Left)) {
		if (m_hot) {
			if (BarsRect(ctx).Contains(mx, my) && m_onBars) m_onBars();
			else if (m_onClick) m_onClick();
		}
		m_held = false;
	}
	if (m_heldRight && input->WasMouseReleased(MouseButton::Right)) {
		if (m_hot) {
			if (BarsRect(ctx).Contains(mx, my) && m_onBars) m_onBars();
			else if (m_onRight) m_onRight();
		}
		m_heldRight = false;
	}
}

void CharacterPanel::Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const ui::Theme& theme = ctx.GetTheme();
	const gfx::Rect& px = Pixel();

	Vec4 background =
		m_held ? theme.controlActive : (m_hot ? theme.controlHot : theme.panel);
	background.w *= backgroundOpacity;
	batch.DrawRect(px, background);
	ui::DrawBorder(batch, px, m_hot ? theme.accent : theme.panelBorder);

	// Internals scale with the slot height (the slot itself is normalized).
	const float pad = px.h * 0.08f;
	const gfx::Rect portrait{px.x + pad, px.y + pad, px.h - 2 * pad,
							 px.h - 2 * pad};
	DrawPortrait(batch, portrait, *m_character, *m_portraitFont, theme);

	// Hit feedback: a transient splat over the portrait while hitFlash > 0,
	// fading out as the timer winds down (the world ticks it). No number — the
	// icon alone conveys the hit. Slightly oversized so the spatter overhangs.
	if (m_character->hitFlash > 0.0f && m_hitSplats) {
		if (const gfx::Texture* splat = m_hitSplats->For(m_character->hitSeverity)) {
			const float fade = std::clamp(m_character->hitFlash / 0.7f, 0.0f, 1.0f);
			const float grow = portrait.w * 0.14f;
			const gfx::Rect r{portrait.x - grow * 0.5f, portrait.y - grow * 0.5f,
							  portrait.w + grow, portrait.h + grow};
			batch.DrawSprite(r, {0, 0, 1, 1}, *splat, {1, 1, 1, fade});
		}
	}

	ui::Font& font = ctx.GetFont();
	const float left = portrait.x + portrait.w + pad;
	const float right = px.x + px.w - pad;
	font.Draw(batch, m_character->name, left, px.y + pad, theme.text);

	// Three resource bars filling the slot below the name.
	const float barsTop = px.y + pad + font.LineAdvance() + 2.0f;
	const float barsBottom = px.y + px.h - pad;
	const float barGap = 4.0f;
	const float barH = (barsBottom - barsTop - 2 * barGap) / 3.0f;
	const struct {
		float value, max;
		const Vec4& color;
	} bars[] = {
		{m_character->health, m_character->maxHealth, m_barColors->health},
		{m_character->stamina, m_character->maxStamina, m_barColors->stamina},
		{m_character->mana, m_character->maxMana, m_barColors->mana},
	};
	float y = barsTop;
	for (const auto& bar : bars) {
		DrawStatBar(batch, {left, y, right - left, barH},
					bar.value / std::max(bar.max, 1.0f), bar.color, theme);
		y += barH + barGap;
	}
}

// --- HandSlot ------------------------------------------------------------------

HandSlot::HandSlot(const gfx::Rect& rect, const Character* character, int hand,
				   const ItemIconBank* icons, std::function<void()> onLeft,
				   std::function<void()> onRight)
	: m_character(character), m_hand(hand), m_icons(icons),
	  m_onLeft(std::move(onLeft)), m_onRight(std::move(onRight)) {
	bounds = rect;
}

void HandSlot::Update(ui::UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	m_hot = !ctx.IsMouseConsumed() &&
			Pixel().Contains(input->MouseX(), input->MouseY());
	if (m_hot) {
		if (input->WasMousePressed(MouseButton::Left)) m_held = true;
		if (input->WasMousePressed(MouseButton::Right)) m_heldRight = true;
		ctx.ConsumeMouse();
	}
	if (m_held && input->WasMouseReleased(MouseButton::Left)) {
		if (m_hot && m_onLeft) m_onLeft();
		m_held = false;
	}
	if (m_heldRight && input->WasMouseReleased(MouseButton::Right)) {
		if (m_hot && m_onRight) m_onRight();
		m_heldRight = false;
	}
}

void HandSlot::Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const ui::Theme& theme = ctx.GetTheme();
	const gfx::Rect& px = Pixel();
	batch.DrawRect(px, m_held ? theme.controlActive
							  : (m_hot ? theme.controlHot : theme.control));
	// The item held in this hand, if any, drawn inset from the border.
	const ItemSlot& slot = m_character->inventory.Hand(m_hand);
	if (!slot.Empty() && m_icons) {
		if (const gfx::Texture* icon = m_icons->For(slot.typeId)) {
			const float pad = px.w * 0.12f;
			batch.DrawSprite({px.x + pad, px.y + pad, px.w - 2 * pad, px.h - 2 * pad},
							 {0, 0, 1, 1}, *icon, {1, 1, 1, 1});
		}
	}
	// Identity stripe along the bottom edge.
	batch.DrawRect({px.x + 1, px.y + px.h - 4, px.w - 2, 3},
				   m_character->portraitColor);
	ui::DrawBorder(batch, px, m_hot ? theme.accent : theme.panelBorder);
}

// --- InventoryWindow ---------------------------------------------------------

namespace {
constexpr float kInvPad = 16.0f;
constexpr float kInvHeader = 30.0f; // title band
constexpr float kInvGap = 8.0f;
constexpr int kInvCols = 2; // backpack slots laid out 2 wide
} // namespace

InventoryWindow::InventoryWindow(std::vector<Character>* roster,
								 const ItemIconBank* icons,
								 std::optional<std::string>* held)
	: m_roster(roster), m_icons(icons), m_held(held),
	  m_title(loc::Tr("ui.inv_all")) {}

int InventoryWindow::MemberCount() const {
	return static_cast<int>(std::min<size_t>(m_roster->size(), 4));
}

gfx::Rect InventoryWindow::PanelRect(const ui::UIContext& ctx) const {
	const float w = std::min(ctx.Width() * 0.72f, 960.0f);
	const float h = std::min(ctx.Height() * 0.54f, 560.0f);
	return {(ctx.Width() - w) * 0.5f, (ctx.Height() - h) * 0.5f, w, h};
}

gfx::Rect InventoryWindow::SlotRect(const gfx::Rect& panel, int member, int slot) const {
	const float colW = (panel.w - 2 * kInvPad) / static_cast<float>(MemberCount());
	const float colX = panel.x + kInvPad + static_cast<float>(member) * colW;
	const float slotsTop = panel.y + kInvPad + kInvHeader + 24.0f; // title + name
	const float innerW = colW - 12.0f;
	const float slotW = std::min((innerW - kInvGap) / static_cast<float>(kInvCols), 88.0f);
	const int sc = slot % kInvCols, row = slot / kInvCols;
	return {colX + 6.0f + static_cast<float>(sc) * (slotW + kInvGap),
			slotsTop + static_cast<float>(row) * (slotW + kInvGap), slotW, slotW};
}

void InventoryWindow::Update(ui::UIContext& ctx) {
	if (!m_open) return;
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	const gfx::Rect panel = PanelRect(ctx);
	const bool left = input->WasMousePressed(MouseButton::Left);
	const bool right = input->WasMousePressed(MouseButton::Right);
	const float mx = input->MouseX(), my = input->MouseY();

	if (left) {
		for (int m = 0; m < MemberCount(); ++m) {
			auto& pack = (*m_roster)[static_cast<size_t>(m)].inventory.backpack;
			for (int i = 0; i < static_cast<int>(pack.size()); ++i) {
				if (!SlotRect(panel, m, i).Contains(mx, my)) continue;
				ItemSlot& s = pack[static_cast<size_t>(i)];
				if (m_held && m_held->has_value()) {
					// Place held tablet; any occupant goes back onto the cursor.
					std::string incoming = **m_held;
					if (s.Empty()) m_held->reset();
					else *m_held = s.typeId;
					s.typeId = std::move(incoming);
				} else if (!s.Empty()) {
					*m_held = s.typeId; // pick the slot's item up onto the cursor
					s.Clear();
				}
				ctx.ConsumeMouse();
				return;
			}
		}
	}
	// A click off the panel closes it; clicks inside (but not a slot) are
	// swallowed. Either way the open window owns the mouse.
	if ((left || right) && !panel.Contains(mx, my)) m_open = false;
	ctx.ConsumeMouse();
}

void InventoryWindow::DrawOverlay(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	if (!m_open) return;
	const ui::Theme& theme = ctx.GetTheme();
	ui::Font& font = ctx.GetFont();
	batch.DrawRect({0, 0, ctx.Width(), ctx.Height()}, {0, 0, 0, 0.5f}); // dim wash
	const gfx::Rect panel = PanelRect(ctx);
	batch.DrawRect(panel, theme.panel);
	ui::DrawBorder(batch, panel, theme.panelBorder);
	font.Draw(batch, m_title, panel.x + kInvPad, panel.y + kInvPad, theme.accent);

	const float colW = (panel.w - 2 * kInvPad) / static_cast<float>(MemberCount());
	for (int m = 0; m < MemberCount(); ++m) {
		const auto& pack = (*m_roster)[static_cast<size_t>(m)].inventory.backpack;
		const float colX = panel.x + kInvPad + static_cast<float>(m) * colW;
		font.Draw(batch, (*m_roster)[static_cast<size_t>(m)].name, colX + 6.0f,
				  panel.y + kInvPad + kInvHeader, theme.text);
		for (int i = 0; i < static_cast<int>(pack.size()); ++i) {
			const gfx::Rect r = SlotRect(panel, m, i);
			batch.DrawRect(r, theme.control);
			ui::DrawBorder(batch, r, theme.panelBorder);
			const ItemSlot& s = pack[static_cast<size_t>(i)];
			if (!s.Empty() && m_icons) {
				if (const gfx::Texture* icon = m_icons->For(s.typeId)) {
					const float pad = r.w * 0.1f;
					batch.DrawSprite({r.x + pad, r.y + pad, r.w - 2 * pad, r.h - 2 * pad},
									 {0, 0, 1, 1}, *icon, {1, 1, 1, 1});
				}
			}
		}
	}
}

// --- CharacterSheet ----------------------------------------------------------

// The sheet is authored in design pixels and scaled to the live rect every draw
// (x and y independently). Layout: a portrait + bars band across the top, then
// the inventory — a Dungeon Master-style equipment paper doll on the LEFT and
// the backpack grid on the RIGHT.
constexpr float kSheetDesignW = 780.0f;
constexpr float kSheetDesignH = 560.0f;
namespace {
constexpr float kSlotGap = 10.0f;
constexpr float kInvHeaderY = 182.0f; // "Equipment" / "Backpack" header row

// --- equipment paper doll ---
// The worn slots are arranged anatomically (Dungeon Master style) rather than
// in a plain grid: head on top, the torso flanked by neck/shoulder and hand
// accessories, feet at the bottom. Three columns (left/centre/right of the
// body) over four rows (head / torso / hands / feet); accessory cells use
// fractional col/row to sit between the main grid positions (the rings hang
// just below and outside their hand).
constexpr float kEquipSlot = 72.0f;
constexpr float kDollX = 56.0f;  // x of the LEFT column
constexpr float kDollY = 210.0f; // y of the HEAD row
constexpr float kDollStep = kEquipSlot + kSlotGap;
// One placed cell of the doll: which equipment slot it shows and where (col
// 0/1/2 = left/centre/right, row 0..3 = head..feet; fractions offset between).
struct DollCell {
	EquipSlot slot;
	float col, row;
};
constexpr DollCell kDollCells[] = {
	{EquipSlot::Head,      1.0f, 0.0f}, // top centre
	{EquipSlot::Amulet,    0.0f, 1.0f}, // left of the neck
	{EquipSlot::Body,      1.0f, 1.0f}, // torso centre
	{EquipSlot::Cloak,     2.0f, 1.0f}, // right shoulder
	{EquipSlot::LeftHand,  0.0f, 2.0f}, // left, arms row
	{EquipSlot::Legs,      1.0f, 2.0f}, // centre, between torso and feet
	{EquipSlot::RightHand, 2.0f, 2.0f}, // right, arms row
	{EquipSlot::Ring1,    -0.3f, 3.0f}, // left ring — below & outside the left hand
	{EquipSlot::Ring2,     2.3f, 3.0f}, // right ring — below & outside the right hand
	{EquipSlot::Feet,      1.0f, 3.0f}, // bottom centre
};
constexpr int kDollCellCount = static_cast<int>(sizeof(kDollCells) /
												sizeof(kDollCells[0]));

// --- pack row + backpack grid (right) ---
// The pack-row (containers) sits at kPackRowY, level with the doll's head row; a
// thin rule (<hr>) divides it from the SELECTED pack's contents grid just below.
constexpr float kPackSlot = 72.0f, kPackX = 430.0f;
constexpr int kPackCols = 4; // backpack laid out 4 wide
constexpr float kPackRowY = 210.0f; // the pack-row (one row of kPackRowSlots)
constexpr float kPackSepY = kPackRowY + kPackSlot + 9.0f;  // divider rule
constexpr float kPackY    = kPackRowY + kPackSlot + 18.0f; // contents grid

// --- mode toggle buttons (Inventory / Stats / Skills), a row under the portrait
constexpr int kModeCount = 3;
constexpr float kModeBtnSize = 30.0f, kModeBtnGap = 5.0f;
constexpr float kModeBtnX = 24.0f;  // aligns with the portrait's left edge
constexpr float kModeBtnY = 132.0f; // just below the 100px portrait (top at y20)
} // namespace

CharacterSheet::CharacterSheet(const gfx::Rect& rect, const ui::Font* portraitFont,
							   const ResourceBarColors* barColors,
							   const ItemIconBank* icons,
							   const ItemWeightBank* weights,
							   const ItemIconBank* slotIcons,
							   std::optional<std::string>* held)
	: m_portraitFont(portraitFont), m_barColors(barColors), m_icons(icons),
	  m_weights(weights), m_slotIcons(slotIcons), m_held(held),
	  m_healthLabel(loc::Tr("bar.health")),
	  m_staminaLabel(loc::Tr("bar.stamina")), m_manaLabel(loc::Tr("bar.mana")),
	  m_attributesLabel(loc::Tr("sheet.attributes")),
	  m_skillsLabel(loc::Tr("sheet.skills")), m_noSkills(loc::Tr("sheet.no_skills")) {
	bounds = rect;
	m_attrLabels = {loc::Tr("attr.strength"), loc::Tr("attr.dexterity"),
					loc::Tr("attr.vitality"), loc::Tr("attr.willpower"),
					loc::Tr("attr.intelligence")};
}

void CharacterSheet::SetCharacter(Character& character) {
	m_character = &character;
	m_healthText = std::format("{} / {}", static_cast<int>(character.health),
							   static_cast<int>(character.maxHealth));
	m_staminaText = std::format("{} / {}", static_cast<int>(character.stamina),
								static_cast<int>(character.maxStamina));
	m_manaText = std::format("{} / {}", static_cast<int>(character.mana),
							 static_cast<int>(character.maxMana));
	m_attrValues = {std::to_string(character.strength),
					std::to_string(character.dexterity),
					std::to_string(character.vitality),
					std::to_string(character.willpower),
					std::to_string(character.intelligence)};
}

gfx::Rect CharacterSheet::EquipRect(const gfx::Rect& px, float sx, float sy,
									int i) const {
	const DollCell c = kDollCells[i];
	const float dx = kDollX + c.col * kDollStep;
	const float dy = kDollY + c.row * kDollStep;
	return {px.x + dx * sx, px.y + dy * sy, kEquipSlot * sx, kEquipSlot * sy};
}

gfx::Rect CharacterSheet::PackRect(const gfx::Rect& px, float sx, float sy,
								   int i) const {
	const float dx = kPackX + static_cast<float>(i % kPackCols) * (kPackSlot + kSlotGap);
	const float dy = kPackY + static_cast<float>(i / kPackCols) * (kPackSlot + kSlotGap);
	return {px.x + dx * sx, px.y + dy * sy, kPackSlot * sx, kPackSlot * sy};
}

// Pack-row slot i (the container row above the backpack contents): one row,
// aligned to the backpack columns.
gfx::Rect CharacterSheet::PackRowRect(const gfx::Rect& px, float sx, float sy,
									  int i) const {
	const float dx = kPackX + static_cast<float>(i) * (kPackSlot + kSlotGap);
	return {px.x + dx * sx, px.y + kPackRowY * sy, kPackSlot * sx, kPackSlot * sy};
}

gfx::Rect CharacterSheet::ModeButtonRect(const gfx::Rect& px, float sx, float sy,
										 int i) const {
	const float dx = kModeBtnX + static_cast<float>(i) * (kModeBtnSize + kModeBtnGap);
	return {px.x + dx * sx, px.y + kModeBtnY * sy, kModeBtnSize * sx,
			kModeBtnSize * sy};
}

void CharacterSheet::ClickSlot(ItemSlot& slot) {
	if (!m_held) return;
	if (m_held->has_value()) {
		std::string incoming = **m_held; // place; any occupant returns to cursor
		if (slot.Empty()) m_held->reset();
		else *m_held = slot.typeId;
		slot.typeId = std::move(incoming);
	} else if (!slot.Empty()) {
		*m_held = slot.typeId; // pick the slot's item up onto the cursor
		slot.Clear();
	}
}

void CharacterSheet::Update(ui::UIContext& ctx) {
	m_hotMode = -1;
	const Input* input = ctx.CurrentInput();
	if (!input || ctx.IsMouseConsumed()) return; // sheet buttons update first
	const gfx::Rect& px = Pixel();
	const float mx = input->MouseX(), my = input->MouseY();
	if (!px.Contains(mx, my)) return;
	const float sx = px.w / kSheetDesignW, sy = px.h / kSheetDesignH;
	const bool clicked = m_character && input->WasMousePressed(MouseButton::Left);

	// Mode toggle buttons (always present, every mode).
	for (int i = 0; i < kModeCount; ++i)
		if (ModeButtonRect(px, sx, sy, i).Contains(mx, my)) {
			m_hotMode = i;
			if (clicked) {
				m_mode = static_cast<Mode>(i);
				ctx.ConsumeMouse();
			}
			break;
		}

	// Item slots are only live (and only hit-tested) in Inventory mode.
	if (clicked && m_mode == Mode::Inventory && !ctx.IsMouseConsumed()) {
		for (int i = 0; i < kDollCellCount; ++i)
			if (EquipRect(px, sx, sy, i).Contains(mx, my)) {
				const size_t s = static_cast<size_t>(kDollCells[i].slot);
				ClickSlot(m_character->inventory.equipment[s]);
				ctx.ConsumeMouse();
				return;
			}
		// Pack row: click a non-empty pack to SELECT it (its contents fill the
		// grid). Packs aren't drop targets yet — only the starting backpack exists.
		for (int i = 0; i < kPackRowSlots; ++i)
			if (PackRowRect(px, sx, sy, i).Contains(mx, my)) {
				if (!m_character->inventory.packs[static_cast<size_t>(i)].Empty())
					m_character->inventory.selectedPack = i;
				ctx.ConsumeMouse();
				return;
			}
		auto& pack = m_character->inventory.backpack;
		for (int i = 0; i < static_cast<int>(pack.size()); ++i)
			if (PackRect(px, sx, sy, i).Contains(mx, my)) {
				ClickSlot(pack[static_cast<size_t>(i)]);
				ctx.ConsumeMouse();
				return;
			}
	}
	ctx.ConsumeMouse(); // swallow other clicks over the sheet
}

void CharacterSheet::Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const ui::Theme& theme = ctx.GetTheme();
	const gfx::Rect& px = Pixel();

	batch.DrawRect(px, theme.panel);
	ui::DrawBorder(batch, px, theme.panelBorder);
	if (!m_character) return;

	const float sx = px.w / kSheetDesignW;
	const float sy = px.h / kSheetDesignH;

	// --- header band (portrait, name) — the bars live on the Stats page ------
	DrawPortrait(batch, {px.x + 24 * sx, px.y + 20 * sy, 100 * sx, 100 * sy},
				 *m_character, *m_portraitFont, theme);
	m_portraitFont->Draw(batch, m_character->name, px.x + 140 * sx, px.y + 30 * sy,
						 theme.accent);

	// --- mode toggle + the active mode's body -------------------------------
	DrawModeButtons(ctx, batch, px, sx, sy);
	switch (m_mode) {
	case Mode::Inventory: DrawInventory(ctx, batch, px, sx, sy); break;
	case Mode::Stats:     DrawStats(ctx, batch, px, sx, sy); break;
	case Mode::Skills:    DrawSkills(ctx, batch, px, sx, sy); break;
	}
}

// The three little icon buttons under the portrait. The active mode's button is
// drawn "pressed" (active fill + accent border); hovered buttons lighten. Icons
// are primitive-drawn (no atlas): a 2x2 grid (Inventory), ascending bars
// (Stats), a six-point star (Skills).
void CharacterSheet::DrawModeButtons(ui::UIContext& ctx, gfx::SpriteBatch& batch,
									 const gfx::Rect& px, float sx, float sy) {
	const ui::Theme& theme = ctx.GetTheme();
	for (int i = 0; i < kModeCount; ++i) {
		const gfx::Rect r = ModeButtonRect(px, sx, sy, i);
		const bool active = static_cast<int>(m_mode) == i;
		batch.DrawRect(r, active ? theme.controlActive
								 : (m_hotMode == i ? theme.controlHot : theme.control));
		ui::DrawBorder(batch, r, active ? theme.accent : theme.panelBorder);
		const Vec4 ink = active ? theme.text : theme.textDim;
		const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
		if (i == 0) { // Inventory: 2x2 grid of squares
			const float sq = r.w * 0.17f, g = r.w * 0.09f;
			const float x0 = cx - sq - g * 0.5f, y0 = cy - sq - g * 0.5f;
			for (int gx = 0; gx < 2; ++gx)
				for (int gy = 0; gy < 2; ++gy)
					batch.DrawRect({x0 + gx * (sq + g), y0 + gy * (sq + g), sq, sq}, ink);
		} else if (i == 1) { // Stats: three ascending bars
			const float bw = r.w * 0.13f, g = r.w * 0.07f;
			const float x0 = cx - (3 * bw + 2 * g) * 0.5f, baseY = cy + r.h * 0.24f;
			const float h[3] = {r.h * 0.22f, r.h * 0.34f, r.h * 0.46f};
			for (int k = 0; k < 3; ++k)
				batch.DrawRect({x0 + k * (bw + g), baseY - h[k], bw, h[k]}, ink);
		} else { // Skills: a six-point star (two overlaid triangles)
			const float rad = r.w * 0.26f, dx = rad * 0.866f, dy = rad * 0.5f;
			batch.DrawTriangle({cx, cy - rad}, {cx - dx, cy + dy}, {cx + dx, cy + dy}, ink);
			batch.DrawTriangle({cx, cy + rad}, {cx - dx, cy - dy}, {cx + dx, cy - dy}, ink);
		}
	}
}

float CharacterSheet::CarryLoad() const {
	if (!m_character || !m_weights) return 0.0f;
	float total = 0.0f;
	for (const ItemSlot& s : m_character->inventory.equipment)
		if (!s.Empty()) total += m_weights->For(s.typeId);
	for (const ItemSlot& s : m_character->inventory.packs) // the bags themselves
		if (!s.Empty()) total += m_weights->For(s.typeId);
	for (const ItemSlot& s : m_character->inventory.backpack)
		if (!s.Empty()) total += m_weights->For(s.typeId);
	return total;
}

void CharacterSheet::DrawInventory(ui::UIContext& ctx, gfx::SpriteBatch& batch,
								   const gfx::Rect& px, float sx, float sy) {
	const ui::Theme& theme = ctx.GetTheme();
	ui::Font& font = ctx.GetFont();

	// --- equipment paper doll (left) ----------------------------------------
	// No header — the anatomical doll layout reads as "equipment" on its own.
	for (int i = 0; i < kDollCellCount; ++i) {
		const size_t slot = static_cast<size_t>(kDollCells[i].slot);
		const gfx::Rect r = EquipRect(px, sx, sy, i);
		batch.DrawRect(r, theme.control);
		ui::DrawBorder(batch, r, theme.panelBorder);
		const ItemSlot& s = m_character->inventory.equipment[slot];
		if (s.Empty()) {
			// Empty slot shows a faint outline silhouette of its type (head/body/
			// …) so the doll reads at a glance; an equipped item covers it.
			if (m_slotIcons) {
				if (const gfx::Texture* o = m_slotIcons->For(kEquipIcon[slot])) {
					const float p = r.w * 0.12f;
					batch.DrawSprite({r.x + p, r.y + p, r.w - 2 * p, r.h - 2 * p},
									 {0, 0, 1, 1}, *o, {1, 1, 1, 0.5f});
				}
			}
		} else if (m_icons) {
			if (const gfx::Texture* icon = m_icons->For(s.typeId)) {
				const float p = r.w * 0.1f;
				batch.DrawSprite({r.x + p, r.y + p, r.w - 2 * p, r.h - 2 * p}, {0, 0, 1, 1},
								 *icon, {1, 1, 1, 1});
			}
		}
	}

	// --- backpack (right, dynamic) ------------------------------------------
	// The carry load stands in for a "Backpack" header (the slot grid is self-
	// evident); it turns red once over the strength-derived capacity. Display
	// only — no gameplay penalty yet.
	const float load = CarryLoad();
	const float maxLoad = m_character->MaxCarryLoad();
	const std::string loadText = loc::Format(
		"sheet.load", std::format("{:.1f}", load), std::format("{:.0f}", maxLoad));
	const Vec4 loadColor = load > maxLoad ? Vec4{0.85f, 0.25f, 0.2f, 1.0f} : theme.accent;
	font.Draw(batch, loadText, px.x + kPackX * sx, px.y + kInvHeaderY * sy, loadColor);

	// Pack row: the member's containers (slot 0 = the starting backpack). The
	// selected pack is highlighted; its contents fill the grid below.
	const Inventory& inv = m_character->inventory;
	auto drawIcon = [&](const gfx::Rect& r, const ItemSlot& s) {
		if (s.Empty() || !m_icons) return;
		if (const gfx::Texture* icon = m_icons->For(s.typeId)) {
			const float p = r.w * 0.1f;
			batch.DrawSprite({r.x + p, r.y + p, r.w - 2 * p, r.h - 2 * p}, {0, 0, 1, 1},
							 *icon, {1, 1, 1, 1});
		}
	};
	for (int i = 0; i < kPackRowSlots; ++i) {
		const gfx::Rect r = PackRowRect(px, sx, sy, i);
		const bool sel = i == inv.selectedPack;
		batch.DrawRect(r, sel ? theme.controlActive : theme.control);
		ui::DrawBorder(batch, r, sel ? theme.accent : theme.panelBorder);
		drawIcon(r, inv.packs[static_cast<size_t>(i)]);
	}

	// A divider rule (<hr>) between the pack row and its contents, spanning the
	// grid width.
	const float gridW = kPackCols * kPackSlot + (kPackCols - 1) * kSlotGap;
	batch.DrawRect({px.x + kPackX * sx, px.y + kPackSepY * sy, gridW * sx,
					std::max(1.0f, sy)},
				   theme.panelBorder);

	// Backpack contents (the selected pack's items). For now only pack 0 (the
	// starting backpack) carries contents — the `backpack` vector — so that is
	// what shows; per-pack contents storage is a later step.
	const auto& pack = inv.backpack;
	for (int i = 0; i < static_cast<int>(pack.size()); ++i) {
		const gfx::Rect r = PackRect(px, sx, sy, i);
		batch.DrawRect(r, theme.control);
		ui::DrawBorder(batch, r, theme.panelBorder);
		drawIcon(r, pack[static_cast<size_t>(i)]);
	}
}

void CharacterSheet::DrawStats(ui::UIContext& ctx, gfx::SpriteBatch& batch,
							   const gfx::Rect& px, float sx, float sy) {
	const ui::Theme& theme = ctx.GetTheme();
	ui::Font& font = ctx.GetFont();

	constexpr float kFirstRowY = 218.0f, kRowH = 40.0f;

	// --- attributes (left column) -------------------------------------------
	// Each attribute on its own row: name on the left, value right-aligned at a
	// fixed column (so the numbers line up).
	font.Draw(batch, m_attributesLabel, px.x + kDollX * sx, px.y + kInvHeaderY * sy,
			  theme.accent);
	constexpr float kLabelX = 56.0f, kValueRight = 300.0f;
	for (size_t i = 0; i < m_attrLabels.size(); ++i) {
		const float y = kFirstRowY + static_cast<float>(i) * kRowH;
		font.Draw(batch, m_attrLabels[i], px.x + kLabelX * sx, px.y + y * sy, theme.textDim);
		const float vw = font.MeasureWidth(m_attrValues[i]);
		font.Draw(batch, m_attrValues[i], px.x + kValueRight * sx - vw, px.y + y * sy,
				  theme.text);
	}

	// --- health / stamina / mana bars (right column, beside the attributes) -
	const struct {
		const std::string& label;
		float value, max;
		const Vec4& color;
		const std::string& text;
	} bars[] = {
		{m_healthLabel, m_character->health, m_character->maxHealth,
		 m_barColors->health, m_healthText},
		{m_staminaLabel, m_character->stamina, m_character->maxStamina,
		 m_barColors->stamina, m_staminaText},
		{m_manaLabel, m_character->mana, m_character->maxMana, m_barColors->mana,
		 m_manaText},
	};
	constexpr float kBarLabelX = 360.0f, kBarX = 470.0f, kBarW = 240.0f, kBarH = 22.0f;
	for (size_t i = 0; i < std::size(bars); ++i) {
		const auto& b = bars[i];
		const float by = kFirstRowY + static_cast<float>(i) * kRowH;
		const gfx::Rect bar{px.x + kBarX * sx, px.y + by * sy, kBarW * sx, kBarH * sy};
		font.Draw(batch, b.label, px.x + kBarLabelX * sx,
				  bar.y + (bar.h - font.Height()) * 0.5f, theme.textDim);
		DrawStatBar(batch, bar, b.value / std::max(b.max, 1.0f), b.color, theme);
		const float tw = font.MeasureWidth(b.text);
		font.Draw(batch, b.text, bar.x + (bar.w - tw) * 0.5f,
				  bar.y + (bar.h - font.Height()) * 0.5f, theme.text);
	}
}

void CharacterSheet::DrawSkills(ui::UIContext& ctx, gfx::SpriteBatch& batch,
								const gfx::Rect& px, float sx, float sy) {
	const ui::Theme& theme = ctx.GetTheme();
	ui::Font& font = ctx.GetFont();
	font.Draw(batch, m_skillsLabel, px.x + kDollX * sx, px.y + kInvHeaderY * sy,
			  theme.accent);
	font.Draw(batch, m_noSkills, px.x + kDollX * sx, px.y + 230.0f * sy, theme.textDim);
}

} // namespace dungeon::game
