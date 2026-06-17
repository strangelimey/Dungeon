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
	const ItemSlot& slot = m_character->inventory.hands[m_hand];
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
	  m_title(loc::Tr("ui.inventory")) {}

int InventoryWindow::DisplayCount() const {
	if (m_solo >= 0) return 1;
	return static_cast<int>(std::min<size_t>(m_roster->size(), 4));
}

size_t InventoryWindow::MemberAtColumn(int col) const {
	return m_solo >= 0 ? static_cast<size_t>(m_solo) : static_cast<size_t>(col);
}

gfx::Rect InventoryWindow::PanelRect(const ui::UIContext& ctx) const {
	// A solo backpack is a slim panel; the whole party is a wide one.
	const float w = m_solo >= 0 ? std::min(ctx.Width() * 0.22f, 320.0f)
								 : std::min(ctx.Width() * 0.72f, 960.0f);
	const float h = std::min(ctx.Height() * 0.54f, 560.0f);
	return {(ctx.Width() - w) * 0.5f, (ctx.Height() - h) * 0.5f, w, h};
}

gfx::Rect InventoryWindow::SlotRect(const gfx::Rect& panel, int col, int slot) const {
	const float colW = (panel.w - 2 * kInvPad) / static_cast<float>(DisplayCount());
	const float colX = panel.x + kInvPad + static_cast<float>(col) * colW;
	const float slotsTop = panel.y + kInvPad + kInvHeader + 24.0f; // title + name
	const float innerW = colW - 12.0f;
	// Cap the slot size so a wide (solo) column doesn't blow them up, and so all
	// four backpack rows fit the panel height.
	const float slotW = std::min((innerW - kInvGap) / static_cast<float>(kInvCols), 96.0f);
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
		for (int c = 0; c < DisplayCount(); ++c) {
			const size_t member = MemberAtColumn(c);
			for (int i = 0; i < kBackpackSlots; ++i) {
				if (!SlotRect(panel, c, i).Contains(mx, my)) continue;
				ItemSlot& s = (*m_roster)[member].inventory.backpack[i];
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

	const float colW = (panel.w - 2 * kInvPad) / static_cast<float>(DisplayCount());
	for (int c = 0; c < DisplayCount(); ++c) {
		const size_t member = MemberAtColumn(c);
		const float colX = panel.x + kInvPad + static_cast<float>(c) * colW;
		font.Draw(batch, (*m_roster)[member].name, colX + 6.0f,
				  panel.y + kInvPad + kInvHeader, theme.text);
		for (int i = 0; i < kBackpackSlots; ++i) {
			const gfx::Rect r = SlotRect(panel, c, i);
			batch.DrawRect(r, theme.control);
			ui::DrawBorder(batch, r, theme.panelBorder);
			const ItemSlot& s = (*m_roster)[member].inventory.backpack[i];
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

// The sheet is authored at 620x520 design pixels and scaled to the live rect
// every draw (x and y independently, so it stretches with the window aspect
// like the rest of the normalized UI).
constexpr float kSheetDesignW = 620.0f;
constexpr float kSheetDesignH = 520.0f;

CharacterSheet::CharacterSheet(const gfx::Rect& rect, const ui::Font* portraitFont,
							   const ResourceBarColors* barColors)
	: m_portraitFont(portraitFont), m_barColors(barColors),
	  m_healthLabel(loc::Tr("bar.health")), m_staminaLabel(loc::Tr("bar.stamina")),
	  m_manaLabel(loc::Tr("bar.mana")),
	  m_attributesLabel(loc::Tr("sheet.attributes")),
	  m_equipmentLabel(loc::Tr("sheet.equipment")),
	  m_nothingCarried(loc::Tr("sheet.nothing_carried")) {
	bounds = rect;
}

void CharacterSheet::SetCharacter(const Character& character) {
	m_character = &character;
	m_subtitle = loc::Format("sheet.subtitle", character.level,
							 loc::Tr(character.classKey));
	m_healthText = std::format("{} / {}", static_cast<int>(character.health),
							   static_cast<int>(character.maxHealth));
	m_staminaText = std::format("{} / {}", static_cast<int>(character.stamina),
								static_cast<int>(character.maxStamina));
	m_manaText = std::format("{} / {}", static_cast<int>(character.mana),
							 static_cast<int>(character.maxMana));
	m_attributes = {{{loc::Tr("attr.strength"), std::to_string(character.strength)},
					 {loc::Tr("attr.dexterity"), std::to_string(character.dexterity)},
					 {loc::Tr("attr.vitality"), std::to_string(character.vitality)},
					 {loc::Tr("attr.willpower"), std::to_string(character.willpower)},
					 {loc::Tr("attr.intelligence"),
					  std::to_string(character.intelligence)}}};
}

void CharacterSheet::Update(ui::UIContext& ctx) {
	// Swallow clicks over the page so nothing beneath reacts; the sheet's
	// own buttons live later in the context and update first.
	const Input* input = ctx.CurrentInput();
	if (!input || ctx.IsMouseConsumed()) return;
	if (Pixel().Contains(input->MouseX(), input->MouseY())) ctx.ConsumeMouse();
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

	DrawPortrait(batch, R(24, 24, 128, 128), *m_character, *m_portraitFont, theme);
	m_portraitFont->Draw(batch, m_character->name, px.x + 176 * sx,
						 px.y + 24 * sy, theme.accent);
	font.Draw(batch, m_subtitle, px.x + 176 * sx, px.y + 100 * sy, theme.textDim);

	// Resource bars with centered value text.
	const struct {
		const std::string& label;
		float value, max;
		const Vec4& color;
		const std::string& text;
	} rows[] = {
		{m_healthLabel, m_character->health, m_character->maxHealth,
		 m_barColors->health, m_healthText},
		{m_staminaLabel, m_character->stamina, m_character->maxStamina,
		 m_barColors->stamina, m_staminaText},
		{m_manaLabel, m_character->mana, m_character->maxMana, m_barColors->mana,
		 m_manaText},
	};
	float y = 150.0f;
	for (const auto& row : rows) {
		const gfx::Rect bar = R(290, y, 306, 26);
		font.Draw(batch, row.label, px.x + 176 * sx,
				  bar.y + (bar.h - font.Height()) * 0.5f, theme.textDim);
		DrawStatBar(batch, bar, row.value / std::max(row.max, 1.0f), row.color,
					theme);
		const float textW = font.MeasureWidth(row.text);
		font.Draw(batch, row.text, bar.x + (bar.w - textW) * 0.5f,
				  bar.y + (bar.h - font.Height()) * 0.5f, theme.text);
		y += 38.0f;
	}

	// Attributes (left) and an equipment placeholder (right).
	font.Draw(batch, m_attributesLabel, px.x + 24 * sx, px.y + 280 * sy, theme.accent);
	float ay = 320.0f;
	for (const AttributeLine& attr : m_attributes) {
		font.Draw(batch, attr.label, px.x + 24 * sx, px.y + ay * sy, theme.textDim);
		font.Draw(batch, attr.value, px.x + 180 * sx, px.y + ay * sy, theme.text);
		ay += 36.0f;
	}
	font.Draw(batch, m_equipmentLabel, px.x + 290 * sx, px.y + 280 * sy, theme.accent);
	font.Draw(batch, m_nothingCarried, px.x + 290 * sx, px.y + 320 * sy,
			  theme.textDim);
}

} // namespace dungeon::game
