#include "Game/PartyHud.h"

#include <algorithm>
#include <format>

namespace dungeon::game {

namespace {

// Resource bar colors, shared by the party bar and the sheet.
constexpr Vec4 kHealthColor{0.62f, 0.18f, 0.14f, 1.0f};
constexpr Vec4 kStaminaColor{0.26f, 0.52f, 0.22f, 1.0f};
constexpr Vec4 kManaColor{0.22f, 0.36f, 0.68f, 1.0f};

void DrawStatBar(gfx::SpriteBatch& batch, const gfx::Rect& rect, float fraction,
				 const Vec4& color, const ui::Theme& theme) {
	batch.DrawRect(rect, theme.control);
	const float t = std::clamp(fraction, 0.0f, 1.0f);
	if (t > 0.0f) batch.DrawRect({rect.x, rect.y, rect.w * t, rect.h}, color);
	ui::DrawBorder(batch, rect, theme.panelBorder);
}

// Baked portrait texture when present; otherwise the tinted square with the
// character's initial as a fallback (fresh checkout before AssetBaker runs).
void DrawPortrait(gfx::SpriteBatch& batch, const gfx::Rect& rect,
				  const Character& character, const ui::Font& font,
				  const ui::Theme& theme) {
	if (character.portrait) {
		batch.DrawSprite(rect, {0, 0, 1, 1}, *character.portrait, {1, 1, 1, 1});
		ui::DrawBorder(batch, rect, theme.panelBorder);
		return;
	}
	batch.DrawRect(rect, character.portraitColor);
	ui::DrawBorder(batch, rect, theme.panelBorder);
	const std::string_view initial = std::string_view(character.name).substr(0, 1);
	const float initialW = font.MeasureWidth(initial);
	font.Draw(batch, initial, rect.x + (rect.w - initialW) * 0.5f,
			  rect.y + (rect.h - font.Height()) * 0.5f, theme.text);
}

} // namespace

// --- CharacterPanel ----------------------------------------------------------

CharacterPanel::CharacterPanel(const gfx::Rect& rect, const Character* character,
							   const ui::Font* portraitFont,
							   std::function<void()> onClick)
	: m_character(character), m_portraitFont(portraitFont),
	  m_onClick(std::move(onClick)) {
	bounds = rect;
}

void CharacterPanel::Update(ui::UIContext& ctx) {
	const Input* input = ctx.CurrentInput();
	if (!input) return;
	m_hot = !ctx.IsMouseConsumed() &&
			Pixel().Contains(input->MouseX(), input->MouseY());
	if (m_hot) {
		if (input->WasMousePressed(MouseButton::Left)) m_held = true;
		ctx.ConsumeMouse();
	}
	if (m_held && input->WasMouseReleased(MouseButton::Left)) {
		if (m_hot && m_onClick) m_onClick();
		m_held = false;
	}
}

void CharacterPanel::Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) {
	const ui::Theme& theme = ctx.GetTheme();
	const gfx::Rect& px = Pixel();

	batch.DrawRect(px, m_held ? theme.controlActive
							  : (m_hot ? theme.controlHot : theme.panel));
	ui::DrawBorder(batch, px, m_hot ? theme.accent : theme.panelBorder);

	// Internals scale with the slot height (the slot itself is normalized).
	const float pad = px.h * 0.08f;
	const gfx::Rect portrait{px.x + pad, px.y + pad, px.h - 2 * pad,
							 px.h - 2 * pad};
	DrawPortrait(batch, portrait, *m_character, *m_portraitFont, theme);

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
		{m_character->health, m_character->maxHealth, kHealthColor},
		{m_character->stamina, m_character->maxStamina, kStaminaColor},
		{m_character->mana, m_character->maxMana, kManaColor},
	};
	float y = barsTop;
	for (const auto& bar : bars) {
		DrawStatBar(batch, {left, y, right - left, barH},
					bar.value / std::max(bar.max, 1.0f), bar.color, theme);
		y += barH + barGap;
	}
}

// --- CharacterSheet ----------------------------------------------------------

// The sheet is authored at 620x520 design pixels and scaled to the live rect
// every draw (x and y independently, so it stretches with the window aspect
// like the rest of the normalized UI).
constexpr float kSheetDesignW = 620.0f;
constexpr float kSheetDesignH = 520.0f;

CharacterSheet::CharacterSheet(const gfx::Rect& rect, const ui::Font* portraitFont)
	: m_portraitFont(portraitFont) {
	bounds = rect;
}

void CharacterSheet::SetCharacter(const Character& character) {
	m_character = &character;
	m_subtitle = std::format("Level {} {}", character.level, character.className);
	m_healthText = std::format("{} / {}", static_cast<int>(character.health),
							   static_cast<int>(character.maxHealth));
	m_staminaText = std::format("{} / {}", static_cast<int>(character.stamina),
								static_cast<int>(character.maxStamina));
	m_manaText = std::format("{} / {}", static_cast<int>(character.mana),
							 static_cast<int>(character.maxMana));
	m_attributes = {{{"Strength", std::to_string(character.strength)},
					 {"Dexterity", std::to_string(character.dexterity)},
					 {"Vitality", std::to_string(character.vitality)},
					 {"Willpower", std::to_string(character.willpower)}}};
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
		const char* label;
		float value, max;
		const Vec4& color;
		const std::string& text;
	} rows[] = {
		{"Health", m_character->health, m_character->maxHealth, kHealthColor,
		 m_healthText},
		{"Stamina", m_character->stamina, m_character->maxStamina, kStaminaColor,
		 m_staminaText},
		{"Mana", m_character->mana, m_character->maxMana, kManaColor, m_manaText},
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
	font.Draw(batch, "Attributes", px.x + 24 * sx, px.y + 280 * sy, theme.accent);
	float ay = 320.0f;
	for (const AttributeLine& attr : m_attributes) {
		font.Draw(batch, attr.label, px.x + 24 * sx, px.y + ay * sy, theme.textDim);
		font.Draw(batch, attr.value, px.x + 180 * sx, px.y + ay * sy, theme.text);
		ay += 36.0f;
	}
	font.Draw(batch, "Equipment", px.x + 290 * sx, px.y + 280 * sy, theme.accent);
	font.Draw(batch, "Nothing carried.", px.x + 290 * sx, px.y + 320 * sy,
			  theme.textDim);
}

} // namespace dungeon::game
