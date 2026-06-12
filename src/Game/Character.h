// ============================================================================
// Game/Character.h — party member data.
//
// One Character per party slot (up to four). Pure data for now — combat
// doesn't exist yet, so the numbers only feed the HUD party bar and the
// character sheet (see PartyHud.h). Portraits are baked by AssetBaker
// (portrait_<name>.png); the tinted square stamped with the character's
// initial remains as the fallback when the texture is missing.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"

#include <string>
#include <vector>

namespace dungeon::gfx {
class Texture;
}

namespace dungeon::game {

struct Character {
	std::string name;
	std::string className;
	int level = 1;

	float health = 1.0f, maxHealth = 1.0f;
	float stamina = 1.0f, maxStamina = 1.0f;
	float mana = 1.0f, maxMana = 1.0f;

	int strength = 10;
	int dexterity = 10;
	int vitality = 10;
	int willpower = 10;

	// Movement-pace multiplier (1 = baseline, lower = slower). The party
	// moves at the pace of its slowest member: the Game feeds the roster
	// minimum into Party::SetSpeed, which scales step and turn rates.
	float moveSpeed = 1.0f;

	// Baked portrait (portrait_<name>.png), wired by the Game after the
	// texture loads; null draws the tinted-initial fallback instead.
	const gfx::Texture* portrait = nullptr;
	// Fallback portrait tint, also the slot's identity color.
	Vec4 portraitColor{0.3f, 0.3f, 0.3f, 1.0f};
};

// The default four-member starting party, fresh at full health.
std::vector<Character> CreateDefaultParty();

} // namespace dungeon::game
