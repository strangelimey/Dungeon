#include "Game/Character.h"

#include <iterator> // std::size

#include "Game/GameSettings.h" // kDefaultMemberColors — the one authored palette

namespace dungeon::game {

namespace {
// Parallel to WearSlot. "ring" covers both ring slots (see Inventory.h).
constexpr const char* kWearSlotIds[] = {"",      "head",   "body", "legs",
										"feet",  "cloak",  "amulet", "ring"};
} // namespace

const char* WearSlotId(WearSlot s) {
	const size_t i = static_cast<size_t>(s);
	return i < std::size(kWearSlotIds) ? kWearSlotIds[i] : "";
}

bool ParseWearSlot(std::string_view token, WearSlot& out) {
	for (size_t i = 1; i < std::size(kWearSlotIds); ++i)
		if (token == kWearSlotIds[i]) {
			out = static_cast<WearSlot>(i);
			return true;
		}
	return false;
}

bool WearSlotFits(WearSlot wear, EquipSlot slot) {
	switch (slot) {
	case EquipSlot::Head:   return wear == WearSlot::Head;
	case EquipSlot::Body:   return wear == WearSlot::Body;
	case EquipSlot::Legs:   return wear == WearSlot::Legs;
	case EquipSlot::Feet:   return wear == WearSlot::Feet;
	case EquipSlot::Cloak:  return wear == WearSlot::Cloak;
	case EquipSlot::Amulet: return wear == WearSlot::Amulet;
	case EquipSlot::Ring1:
	case EquipSlot::Ring2:  return wear == WearSlot::Ring;
	// The hands take anything HOLDABLE, which is a different question asked
	// elsewhere — never route them through here.
	case EquipSlot::LeftHand:
	case EquipSlot::RightHand:
	default: return false;
	}
}


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

	// Brand starts carrying one piece of each armor WEIGHT CLASS, for the same
	// reason the casters start with runes: the trade the armor system is built
	// around (harder to hit you, harder to hurt you — docs/damage-system.md) is
	// unreachable from a fresh game without first finding three specific items,
	// and it is the thing most worth feeling early. Stowed, not worn: putting
	// one ON is the interaction, and the sheet's Defense column is where the
	// difference shows.
	//
	// Brand is the front-line fighter and, at STR 10 against plate's 14, also
	// the demonstration of wearing something you cannot carry.
	for (const char* piece : {"padded_jack", "brigandine", "plate_cuirass"})
		party[0].inventory.Stow(piece);

	return party;
}

} // namespace dungeon::game
