#include "Game/Character.h"

#include "Game/GameSettings.h" // kDefaultMemberColors — the one authored palette

namespace dungeon::game {

// --- status-effect kind tokens ----------------------------------------------
// Save-file / record names, indexed by StatusKind. Append with the enum.

namespace {
constexpr const char* kStatusKindIds[] = {"ward"};
} // namespace

const char* StatusKindId(StatusKind kind) {
	return kStatusKindIds[static_cast<size_t>(kind)];
}

bool ParseStatusKind(std::string_view token, StatusKind& out) {
	for (size_t i = 0; i < std::size(kStatusKindIds); ++i)
		if (token == kStatusKindIds[i]) {
			out = static_cast<StatusKind>(i);
			return true;
		}
	return false;
}

// --- skill → associated stat --------------------------------------------------
// The creep table from docs/skills.md. Data-shaped but tiny and stable, so a
// lookup beats a catalog field until skills themselves become authorable.

std::string_view SkillStat(std::string_view skillId) {
	if (skillId == "fire" || skillId == "blunt" || skillId == "unarmed")
		return "strength";
	if (skillId == "air" || skillId == "blade") return "dexterity";
	if (skillId == "earth") return "stamina";
	if (skillId == "water") return "health";
	return {};
}

// Four archetypes with distinct stat spreads so the HUD bars and the sheet
// read differently per slot. Values are placeholders until combat exists.
std::vector<Character> CreateDefaultParty() {
	std::vector<Character> party(4);

	party[0].name = "Brand";
	party[0].maxHealth = 42;
	party[0].maxStamina = 38;
	party[0].maxMana = 8;
	party[0].strength = 16;
	party[0].dexterity = 11;
	party[0].vitality = 15;
	party[0].willpower = 8;
	party[0].intelligence = 8; // fighter: slow mana
	party[0].moveSpeed = 0.95f; // heavy gear, near baseline

	party[1].name = "Sera";
	party[1].maxHealth = 30;
	party[1].maxStamina = 44;
	party[1].maxMana = 12;
	party[1].strength = 10;
	party[1].dexterity = 17;
	party[1].vitality = 11;
	party[1].willpower = 10;
	party[1].intelligence = 11; // rogue: middling
	party[1].moveSpeed = 1.2f; // fleet-footed

	party[2].name = "Maren";
	party[2].maxHealth = 34;
	party[2].maxStamina = 30;
	party[2].maxMana = 36;
	party[2].strength = 12;
	party[2].dexterity = 9;
	party[2].vitality = 13;
	party[2].willpower = 16;
	party[2].intelligence = 14; // cleric: strong
	party[2].moveSpeed = 1.0f;

	party[3].name = "Tilo";
	party[3].maxHealth = 24;
	party[3].maxStamina = 26;
	party[3].maxMana = 48;
	party[3].strength = 7;
	party[3].dexterity = 12;
	party[3].vitality = 9;
	party[3].willpower = 18;
	party[3].intelligence = 17; // mage: fast mana
	party[3].moveSpeed = 0.9f; // the party's anchor — sets the pace

	for (size_t i = 0; i < party.size(); ++i) {
		Character& member = party[i];
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
