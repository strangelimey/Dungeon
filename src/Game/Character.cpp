#include "Game/Character.h"

namespace dungeon::game {

// Four archetypes with distinct stat spreads so the HUD bars and the sheet
// read differently per slot. Values are placeholders until combat exists.
std::vector<Character> CreateDefaultParty() {
	std::vector<Character> party(4);

	party[0].name = "Brand";
	party[0].className = "Fighter";
	party[0].maxHealth = 42;
	party[0].maxStamina = 38;
	party[0].maxMana = 8;
	party[0].strength = 16;
	party[0].dexterity = 11;
	party[0].vitality = 15;
	party[0].willpower = 8;
	party[0].moveSpeed = 0.95f; // heavy gear, near baseline
	party[0].portraitColor = {0.42f, 0.20f, 0.14f, 1.0f}; // rust

	party[1].name = "Sera";
	party[1].className = "Rogue";
	party[1].maxHealth = 30;
	party[1].maxStamina = 44;
	party[1].maxMana = 12;
	party[1].strength = 10;
	party[1].dexterity = 17;
	party[1].vitality = 11;
	party[1].willpower = 10;
	party[1].moveSpeed = 1.2f; // fleet-footed
	party[1].portraitColor = {0.18f, 0.32f, 0.18f, 1.0f}; // moss

	party[2].name = "Maren";
	party[2].className = "Cleric";
	party[2].maxHealth = 34;
	party[2].maxStamina = 30;
	party[2].maxMana = 36;
	party[2].strength = 12;
	party[2].dexterity = 9;
	party[2].vitality = 13;
	party[2].willpower = 16;
	party[2].moveSpeed = 1.0f;
	party[2].portraitColor = {0.42f, 0.34f, 0.14f, 1.0f}; // gold

	party[3].name = "Tilo";
	party[3].className = "Mage";
	party[3].maxHealth = 24;
	party[3].maxStamina = 26;
	party[3].maxMana = 48;
	party[3].strength = 7;
	party[3].dexterity = 12;
	party[3].vitality = 9;
	party[3].willpower = 18;
	party[3].moveSpeed = 0.9f; // the party's anchor — sets the pace
	party[3].portraitColor = {0.22f, 0.22f, 0.44f, 1.0f}; // indigo

	for (Character& member : party) {
		member.health = member.maxHealth;
		member.stamina = member.maxStamina;
		member.mana = member.maxMana;
	}
	return party;
}

} // namespace dungeon::game
