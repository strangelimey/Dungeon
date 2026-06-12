// ============================================================================
// Game/Character.h — party member data.
//
// One Character per party slot (up to four). Pure data for now — combat
// doesn't exist yet, so the numbers only feed the HUD party bar and the
// character sheet (see PartyHud.h). Portraits are placeholder tinted squares
// stamped with the character's initial until authored art exists.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"

#include <string>
#include <vector>

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

	// Placeholder portrait tint (see file banner).
	Vec4 portraitColor{0.3f, 0.3f, 0.3f, 1.0f};
};

// The default four-member starting party, fresh at full health.
std::vector<Character> CreateDefaultParty();

} // namespace dungeon::game
