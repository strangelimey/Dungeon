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
#include <flat_map>
#include <functional>
#include <string>

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

class CharacterPanel : public ui::Widget {
public:
	// portraitFont draws the big placeholder initial (the Game passes its
	// title font, which tracks the window scale like everything else).
	CharacterPanel(const gfx::Rect& rect, const Character* character,
				   const ui::Font* portraitFont, const ResourceBarColors* barColors,
				   const HitSplatIcons* hitSplats, std::function<void()> onClick);

	void Update(ui::UIContext& ctx) override;
	void Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

	// Multiplier on the slot background alpha (Settings → UI → Party Bar);
	// the border, portrait, name, and bars stay fully opaque.
	float backgroundOpacity = 1.0f;

private:
	const Character* m_character;
	const ui::Font* m_portraitFont;
	const ResourceBarColors* m_barColors;
	const HitSplatIcons* m_hitSplats; // may be null (icons not loaded)
	std::function<void()> m_onClick;
	bool m_hot = false;
	bool m_held = false;
};

class HandSlot : public ui::Widget {
public:
	HandSlot(const gfx::Rect& rect, const Character* character,
			 std::function<void()> onClick);

	void Update(ui::UIContext& ctx) override;
	void Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	const Character* m_character;
	std::function<void()> m_onClick;
	bool m_hot = false;
	bool m_held = false;
};

class CharacterSheet : public ui::Widget {
public:
	CharacterSheet(const gfx::Rect& rect, const ui::Font* portraitFont,
				   const ResourceBarColors* barColors);

	// Re-points the sheet and caches the formatted strings.
	void SetCharacter(const Character& character);

	void Update(ui::UIContext& ctx) override;
	void Draw(ui::UIContext& ctx, gfx::SpriteBatch& batch) override;

private:
	struct AttributeLine {
		std::string label;
		std::string value;
	};

	const Character* m_character = nullptr;
	const ui::Font* m_portraitFont;
	const ResourceBarColors* m_barColors;
	std::string m_subtitle; // "Level 1 Rogue"
	std::string m_healthText, m_staminaText, m_manaText; // "42 / 42"
	std::array<AttributeLine, 5> m_attributes;
	// Static page text, localized once at construction (the sheet is rebuilt
	// on a language change) so Draw stays allocation-free.
	std::string m_healthLabel, m_staminaLabel, m_manaLabel;
	std::string m_attributesLabel, m_equipmentLabel, m_nothingCarried;
};

} // namespace dungeon::game
