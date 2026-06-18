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
							   std::function<void()> onRight)
	: m_character(character), m_portraitFont(portraitFont), m_barColors(barColors),
	  m_hitSplats(hitSplats), m_onClick(std::move(onClick)),
	  m_onRight(std::move(onRight)) {
	bounds = rect;
}

void CharacterPanel::Update(ui::UIContext& ctx) {
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
		if (m_hot && m_onClick) m_onClick();
		m_held = false;
	}
	if (m_heldRight && input->WasMouseReleased(MouseButton::Right)) {
		if (m_hot && m_onRight) m_onRight();
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
// body) over four rows (head / torso / hands / feet).
constexpr float kEquipSlot = 72.0f;
constexpr float kDollX = 56.0f;  // x of the LEFT column
constexpr float kDollY = 210.0f; // y of the HEAD row
constexpr float kDollStep = kEquipSlot + kSlotGap;
// One placed cell of the doll: which equipment slot it shows and where (col
// 0/1/2 = left/centre/right, row 0..3 = head..feet). Not every EquipSlot is
// placed yet — the two rings exist in storage but get screen cells later.
struct DollCell {
	EquipSlot slot;
	int col, row;
};
constexpr DollCell kDollCells[] = {
	{EquipSlot::Head,      1, 0}, // top centre
	{EquipSlot::Amulet,    0, 1}, // left of the neck
	{EquipSlot::Body,      1, 1}, // torso centre
	{EquipSlot::Cloak,     2, 1}, // right shoulder
	{EquipSlot::LeftHand,  0, 2}, // left, arms row
	{EquipSlot::Legs,      1, 2}, // centre, between torso and feet
	{EquipSlot::RightHand, 2, 2}, // right, arms row
	{EquipSlot::Feet,      1, 3}, // bottom centre
};
constexpr int kDollCellCount = static_cast<int>(sizeof(kDollCells) /
												sizeof(kDollCells[0]));

// --- backpack grid (right) ---
constexpr float kPackSlot = 72.0f, kPackX = 430.0f, kPackY = 210.0f;
constexpr int kPackCols = 4; // backpack laid out 4 wide
} // namespace

CharacterSheet::CharacterSheet(const gfx::Rect& rect, const ui::Font* portraitFont,
							   const ResourceBarColors* barColors,
							   const ItemIconBank* icons,
							   std::optional<std::string>* held)
	: m_portraitFont(portraitFont), m_barColors(barColors), m_icons(icons),
	  m_held(held), m_healthLabel(loc::Tr("bar.health")),
	  m_staminaLabel(loc::Tr("bar.stamina")), m_manaLabel(loc::Tr("bar.mana")),
	  m_equipmentLabel(loc::Tr("sheet.equipment")),
	  m_backpackLabel(loc::Tr("sheet.backpack")) {
	bounds = rect;
	for (int i = 0; i < kEquipCount; ++i) m_equipLabels[i] = loc::Tr(kEquipLabels[i]);
}

void CharacterSheet::SetCharacter(Character& character) {
	m_character = &character;
	m_healthText = std::format("{} / {}", static_cast<int>(character.health),
							   static_cast<int>(character.maxHealth));
	m_staminaText = std::format("{} / {}", static_cast<int>(character.stamina),
								static_cast<int>(character.maxStamina));
	m_manaText = std::format("{} / {}", static_cast<int>(character.mana),
							 static_cast<int>(character.maxMana));
}

gfx::Rect CharacterSheet::EquipRect(const gfx::Rect& px, float sx, float sy,
									int i) const {
	const DollCell c = kDollCells[i];
	const float dx = kDollX + static_cast<float>(c.col) * kDollStep;
	const float dy = kDollY + static_cast<float>(c.row) * kDollStep;
	return {px.x + dx * sx, px.y + dy * sy, kEquipSlot * sx, kEquipSlot * sy};
}

gfx::Rect CharacterSheet::PackRect(const gfx::Rect& px, float sx, float sy,
								   int i) const {
	const float dx = kPackX + static_cast<float>(i % kPackCols) * (kPackSlot + kSlotGap);
	const float dy = kPackY + static_cast<float>(i / kPackCols) * (kPackSlot + kSlotGap);
	return {px.x + dx * sx, px.y + dy * sy, kPackSlot * sx, kPackSlot * sy};
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
	const Input* input = ctx.CurrentInput();
	if (!input || ctx.IsMouseConsumed()) return; // sheet buttons update first
	const gfx::Rect& px = Pixel();
	const float mx = input->MouseX(), my = input->MouseY();
	if (!px.Contains(mx, my)) return;
	const float sx = px.w / kSheetDesignW, sy = px.h / kSheetDesignH;
	if (m_character && input->WasMousePressed(MouseButton::Left)) {
		for (int i = 0; i < kDollCellCount; ++i)
			if (EquipRect(px, sx, sy, i).Contains(mx, my)) {
				const size_t s = static_cast<size_t>(kDollCells[i].slot);
				ClickSlot(m_character->inventory.equipment[s]);
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
	auto R = [&](float x, float y, float w, float h) {
		return gfx::Rect{px.x + x * sx, px.y + y * sy, w * sx, h * sy};
	};
	ui::Font& font = ctx.GetFont();

	// --- header band (portrait, name, bars) ---------------------------------
	DrawPortrait(batch, R(24, 20, 100, 100), *m_character, *m_portraitFont, theme);
	m_portraitFont->Draw(batch, m_character->name, px.x + 140 * sx, px.y + 30 * sy,
						 theme.accent);

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
	float by = 92.0f;
	for (const auto& b : bars) {
		const gfx::Rect bar = R(255, by, 200, 22);
		font.Draw(batch, b.label, px.x + 140 * sx,
				  bar.y + (bar.h - font.Height()) * 0.5f, theme.textDim);
		DrawStatBar(batch, bar, b.value / std::max(b.max, 1.0f), b.color, theme);
		const float tw = font.MeasureWidth(b.text);
		font.Draw(batch, b.text, bar.x + (bar.w - tw) * 0.5f,
				  bar.y + (bar.h - font.Height()) * 0.5f, theme.text);
		by += 30.0f;
	}

	// --- equipment paper doll (left) ----------------------------------------
	font.Draw(batch, m_equipmentLabel, px.x + kDollX * sx, px.y + kInvHeaderY * sy,
			  theme.accent);
	for (int i = 0; i < kDollCellCount; ++i) {
		const size_t slot = static_cast<size_t>(kDollCells[i].slot);
		const gfx::Rect r = EquipRect(px, sx, sy, i);
		batch.DrawRect(r, theme.control);
		ui::DrawBorder(batch, r, theme.panelBorder);
		const ItemSlot& s = m_character->inventory.equipment[slot];
		if (s.Empty()) {
			// Empty slot shows its name so the doll reads even with no gear.
			const std::string& label = m_equipLabels[slot];
			const float lw = font.MeasureWidth(label);
			font.Draw(batch, label, r.x + (r.w - lw) * 0.5f,
					  r.y + (r.h - font.Height()) * 0.5f, theme.textDim);
		} else if (m_icons) {
			if (const gfx::Texture* icon = m_icons->For(s.typeId)) {
				const float p = r.w * 0.1f;
				batch.DrawSprite({r.x + p, r.y + p, r.w - 2 * p, r.h - 2 * p}, {0, 0, 1, 1},
								 *icon, {1, 1, 1, 1});
			}
		}
	}

	// --- backpack (right, dynamic) ------------------------------------------
	font.Draw(batch, m_backpackLabel, px.x + kPackX * sx, px.y + kInvHeaderY * sy,
			  theme.accent);
	const auto& pack = m_character->inventory.backpack;
	for (int i = 0; i < static_cast<int>(pack.size()); ++i) {
		const gfx::Rect r = PackRect(px, sx, sy, i);
		batch.DrawRect(r, theme.control);
		ui::DrawBorder(batch, r, theme.panelBorder);
		const ItemSlot& s = pack[static_cast<size_t>(i)];
		if (!s.Empty() && m_icons) {
			if (const gfx::Texture* icon = m_icons->For(s.typeId)) {
				const float p = r.w * 0.1f;
				batch.DrawSprite({r.x + p, r.y + p, r.w - 2 * p, r.h - 2 * p}, {0, 0, 1, 1},
								 *icon, {1, 1, 1, 1});
			}
		}
	}
}

} // namespace dungeon::game
