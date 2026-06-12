// ============================================================================
// Game/PartyHud.h — party-facing UI widgets (Game layer: they know Character).
//
//   CharacterPanel  one slot of the top party bar: placeholder portrait,
//                   name, and health/stamina/mana bars. Hover highlights;
//                   clicking fires onClick (the Game opens the sheet).
//   CharacterSheet  the character details page: large portrait, name, class,
//                   stat bars with values, and attributes. Drawn over the
//                   frozen scene in AppState::CharacterSheet; the Game pairs
//                   it with prev/next/Back buttons in the same UIContext.
//
// Both widgets hold pointers into Game's character roster (stable for the
// session) and read the live values every draw; the sheet caches its
// formatted strings in SetCharacter so steady-state frames don't allocate.
// ============================================================================
#pragma once

#include "Game/Character.h"
#include "UI/Controls.h"

#include <array>
#include <functional>
#include <string>

namespace dungeon::game {

class CharacterPanel : public ui::Widget {
public:
	// portraitFont draws the big placeholder initial (the Game passes its
	// title font, which tracks the window scale like everything else).
	CharacterPanel(const gfx::Rect& rect, const Character* character,
				   const ui::Font* portraitFont, std::function<void()> onClick);

	void Update(ui::UIContext& ctx) override;
	void Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	// Multiplier on the slot background alpha (Settings → UI → Party Bar);
	// the border, portrait, name, and bars stay fully opaque.
	float backgroundOpacity = 1.0f;

private:
	const Character* m_character;
	const ui::Font* m_portraitFont;
	std::function<void()> m_onClick;
	bool m_hot = false;
	bool m_held = false;
};

class CharacterSheet : public ui::Widget {
public:
	CharacterSheet(const gfx::Rect& rect, const ui::Font* portraitFont);

	// Re-points the sheet and caches the formatted strings.
	void SetCharacter(const Character& character);

	void Update(ui::UIContext& ctx) override;
	void Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	struct AttributeLine {
		const char* label = "";
		std::string value;
	};

	const Character* m_character = nullptr;
	const ui::Font* m_portraitFont;
	std::string m_subtitle; // "Level 1 Rogue"
	std::string m_healthText, m_staminaText, m_manaText; // "42 / 42"
	std::array<AttributeLine, 4> m_attributes;
};

} // namespace dungeon::game
