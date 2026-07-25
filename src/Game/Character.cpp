#include "Game/Character.h"

#include "Game/GameSettings.h" // kDefaultMemberColors — the one authored palette

namespace dungeon::game {

// (The status-effect kind tokens used to live here: an enum plus an id table
// indexed by it. An effect names ITSELF now — the kind's id, resolved through
// fx::EffectBook — so the table is gone and the save stores that id. Old
// tokens still load: EffectBook::FindLegacy maps them forward.)

// Four archetypes with distinct stat spreads so the HUD bars and the sheet
// read differently per slot. The resource BASES are authored (class identity);
// the maxima derive from them + the stats (the resource formula, docs/
// combat.md part 3) — seeded here at k=1, re-derived by the Game once the
// project's balance knobs are loaded. Bases were back-solved from the old
// authored maxima at k=1, so a default balance reproduces them exactly
// (Brand 42/38/8, Sera 30/44/12, Maren 34/30/36, Tilo 24/26/48).
std::vector<Character> CreateDefaultParty() {
	std::vector<Character> party(4);

	party[0].name = "Brand";
	party[0].baseHealth = 27;
	party[0].baseStamina = 22.5f;
	party[0].baseMana = 0;
	party[0].strength = 16;
	party[0].dexterity = 11;
	party[0].vitality = 15;
	party[0].willpower = 8;
	party[0].intelligence = 8; // fighter: slow mana
	party[0].moveSpeed = 0.95f; // heavy gear, near baseline

	party[1].name = "Sera";
	party[1].baseHealth = 19;
	party[1].baseStamina = 33.5f;
	party[1].baseMana = 1.5f;
	party[1].strength = 10;
	party[1].dexterity = 17;
	party[1].vitality = 11;
	party[1].willpower = 10;
	party[1].intelligence = 11; // rogue: middling
	party[1].moveSpeed = 1.2f; // fleet-footed

	party[2].name = "Maren";
	party[2].baseHealth = 21;
	party[2].baseStamina = 17.5f;
	party[2].baseMana = 21;
	party[2].strength = 12;
	party[2].dexterity = 9;
	party[2].vitality = 13;
	party[2].willpower = 16;
	party[2].intelligence = 14; // cleric: strong
	party[2].moveSpeed = 1.0f;

	party[3].name = "Tilo";
	party[3].baseHealth = 15;
	party[3].baseStamina = 18;
	party[3].baseMana = 30.5f;
	party[3].strength = 7;
	party[3].dexterity = 12;
	party[3].vitality = 9;
	party[3].willpower = 18;
	party[3].intelligence = 17; // mage: fast mana
	party[3].moveSpeed = 0.9f; // the party's anchor — sets the pace

	for (size_t i = 0; i < party.size(); ++i) {
		Character& member = party[i];
		member.RecomputeMaxima(1.0f, 1.0f, 1.0f); // seed; Game re-derives
		member.health = member.maxHealth;
		member.stamina = member.maxStamina;
		member.mana = member.maxMana;
		// The identity color DEFAULTS: the live value comes from GameSettings
		// (member_<n>= in the ini, edited on Settings → UI) via
		// Game::ApplyMemberColors — this seeds slots beyond its reach.
		if (i < kMemberColorCount) member.portraitColor = kDefaultMemberColors[i];
	}

	// Maren and Tilo — the party's casters — start with every rune tablet
	// (the four schools + the shared form runes) stowed in their backpacks,
	// so the magic loop (memorize from the hand menu, build in the spellbook,
	// cast) is reachable from a fresh game without scavenging the level first.
	for (Character* caster : {&party[2], &party[3]})
		for (u32 i = 0; i < kSymbolCount; ++i)
			caster->inventory.Stow(RuneItemId(static_cast<SpellSymbol>(i)));

	return party;
}

} // namespace dungeon::game
