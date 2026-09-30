// ============================================================================
// Game/ItemDetails.h - what the item details dialog shows about one item type.
//
// Pure data, filled by the world (DungeonWorld::ItemDetailsFor, which knows the
// catalogs and the damage-type book) and read by the dialog, which knows
// neither. Every string is a VIEW into storage that outlives the dialog (the
// item kind's own strings, the damage-type table), so filling one allocates
// nothing - it happens on a click in a guarded frame.
// ============================================================================
#pragma once

#include "Game/Combat.h"    // ArmorClass, kMaxDamageTypes
#include "Game/Inventory.h" // WearSlot
#include "Game/Spells.h"    // SpellSymbol

#include <array>
#include <string_view>

namespace dungeon::game {

struct ItemDetails {
	std::string_view nameKey;  // "item.dagger"
	std::string_view category; // catalog token ("weapon"), "" = none
	// --- a weapon (damage > 0 marks one) ---
	float damage = 0.0f;
	float speed = 0.0f;       // seconds between swings
	std::string_view skill;   // weapon class it trains ("blade"), "" = none
	bool polearm = false;     // swings from the rear rank
	bool enchanted = false;   // carries `element` into every landed blow
	SpellSymbol element = SpellSymbol::Fire;
	float elementBonus = 0.0f;
	// --- worn protection ---
	float armor = 0.0f;       // flat soak
	ArmorClass armorClass = ArmorClass::None;
	WearSlot wear = WearSlot::None;
	// The non-zero resist cells, as (damage type name key, fraction) pairs.
	struct Resist {
		std::string_view nameKey;
		float value = 0.0f;
	};
	std::array<Resist, kMaxDamageTypes> resists{};
	size_t resistCount = 0;
	// --- what eating it restores ---
	float nutrition = 0.0f;
	float hydration = 0.0f;
};

} // namespace dungeon::game
