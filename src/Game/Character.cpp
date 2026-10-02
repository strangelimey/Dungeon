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

	// Portraits (portraits.cat ids), chosen to keep what the old baked busts
	// said about each: a helmeted knight, a green hood, a white hood with a
	// circlet, an old bearded mage.
	party[0].portraitId = "portrait018";
	party[1].portraitId = "portrait398";
	party[2].portraitId = "portrait1419";
	party[3].portraitId = "b045";
	// All four are human (their portraits are). Their stats stay as authored -
	// the eval suites measure exactly these four - so they are PREMADE members,
	// not race + points (docs/party-creation-plan.md).
	for (Character& member : party) member.raceId = "human";

	for (size_t i = 0; i < party.size(); ++i) {
		Character& member = party[i];
		// Seed with the inert defaults (aptitude x1, no practice term) so a
		// fresh party has sane bars before any catalog is read; the Game
		// re-derives against the real knobs as soon as Balance is loaded.
		member.RecomputeMaxima({});
		member.health = member.maxHealth;
		member.stamina = member.maxStamina;
		member.mana = member.maxMana;
		// The identity color DEFAULTS: the live value comes from GameSettings
		// (member_<n>= in the ini, edited on Settings → UI) via
		// Game::ApplyMemberColors — this seeds slots beyond its reach.
		if (i < kMemberColorCount) member.portraitColor = kDefaultMemberColors[i];
		// A member's first effect lands mid-fight; the list is made now, at its
		// ceiling (fx::kMaxEffects). Game::ResetRoster copy-assigns over these
		// members later, which keeps the storage they already hold.
		fx::ReserveEffects(member.effects);
	}

	// THE STARTING KIT (Michael, ui-updates): the two fighters each hold a
	// dagger, Brand in his RIGHT hand and Sera in her LEFT, the other hand bare.
	// (Brand's bare left is the hand the harness's `swing 0` uses by default, so
	// the eval suites' unarmed numbers are unchanged; nothing swings Sera's.)
	// The two casters hold one school rune and one form rune, school in the left
	// hand: Maren fire + project (a bolt), Tilo earth + protect (a ward). A rune
	// in a hand is memorized from its use menu, so the magic loop (memorize,
	// build in the spellbook, cast) is a click away in a fresh game; the other
	// schools and forms are found.
	party[0].inventory.Hand(1).typeId = "dagger";
	party[1].inventory.Hand(0).typeId = "dagger";
	// Sera carries the party's LIGHT in her free right hand (spell-updates): a
	// lit common torch, full. With nothing lit held, the party sees by the
	// level's ambient alone, so a new game without it would open in the dark.
	party[1].inventory.Hand(1).typeId = "torch_lit";
	party[2].inventory.Hand(0).typeId = RuneItemId(SpellSymbol::Fire);
	party[2].inventory.Hand(1).typeId = RuneItemId(SpellSymbol::Project);
	party[3].inventory.Hand(0).typeId = RuneItemId(SpellSymbol::Earth);
	party[3].inventory.Hand(1).typeId = RuneItemId(SpellSymbol::Protect);
	// And each caster carries the two MODIFIER runes, Ingwaz and Hagalaz, in the
	// backpack (Michael, spell-updates): the third tier is reachable from a fresh
	// game - memorize them from the sheet, then volley a bolt or burst a ward.
	// Each caster gets both, because a rune is memorized by ONE member and spent.
	for (const int caster : {2, 3}) {
		Inventory& inv = party[static_cast<size_t>(caster)].inventory;
		inv.Stow(std::string(RuneItemId(SpellSymbol::Multiple)));
		inv.Stow(std::string(RuneItemId(SpellSymbol::Explode)));
	}

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
