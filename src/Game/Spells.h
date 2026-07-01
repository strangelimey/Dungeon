// ============================================================================
// Game/Spells.h — spell symbols (the casting alphabet).
//
// Dungeon Master style: each SYMBOL is a power, and a spell is a SEQUENCE of
// symbols (Fire + Air = fire bolt). Tier 1 is the four elements; more tiers
// come later. A character learns a symbol by memorizing a Rune item (the rune
// is consumed), so vocabulary is per character — stored as a bitmask on
// Character (knownSymbols). The recipe table that maps a sequence to an effect
// lives in the project's spells catalog (Spell.* / spells.cat); this header is
// just the symbol primitive shared by Character, the casting UI, and the
// resolver, plus the recipe table (SpellBook) that maps a sequence to an effect.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon::game {

class Catalog; // Catalog.h — SpellBook::Build reads the spells catalog.

// Tier-1 elemental symbols. The enum order is the serialization order (save +
// catalog tokens) — APPEND new symbols, never reorder.
enum class SpellSymbol : u8 { Fire, Earth, Air, Water, Count };

inline constexpr u32 kSymbolCount = static_cast<u32>(SpellSymbol::Count);

// The bit for a symbol within a known-symbols mask (Character::knownSymbols).
inline constexpr u32 SymbolBit(SpellSymbol s) { return 1u << static_cast<u32>(s); }

// Lowercase id token ("fire") for catalog/save/console text.
const char* SymbolId(SpellSymbol s);
// loc:: key for the display name ("symbol.fire"); pass through loc::Tr.
const char* SymbolKey(SpellSymbol s);
// Parses an id token ("fire") into `out`; false on anything unknown.
bool ParseSymbol(std::string_view token, SpellSymbol& out);

// The catalog/item id of a symbol's rune tablet ("fire" -> "rune_fire"). The one
// place the rune-item naming convention lives — used by the icon loader, the dev
// `rune` command, and the rune model binding.
std::string RuneItemId(SpellSymbol s);
// The inverse: "rune_fire" -> SpellSymbol::Fire. False for any non-rune id.
bool RuneSymbolFromItemId(std::string_view typeId, SpellSymbol& out);

// Premultiplied-additive accent colour for an element (rgb = emissive, alpha 0,
// per docs/magic system.md: Fire=red, Earth=brown, Air=white, Water=blue). The
// single source shared by the rune-tablet glow and the spell-bolt billboard.
Vec4 ElementColor(SpellSymbol s);

// What a cast does once a recipe matches. A typed enum dispatched from ONE point
// (DungeonWorld::CastSpell) — NOT scripting (see the content-stays-data-driven
// decision). Tier 1 is projectiles only; heal/conjure/AoE append here later.
enum class SpellEffect : u8 { Projectile, Count };

// One resolved recipe — a spells.cat entry (Spells.cpp parses the fields).
struct SpellDef {
	std::string id;                    // catalog id ("flame")
	std::string nameKey;               // loc key for the display name ("spell.flame")
	std::vector<SpellSymbol> sequence; // the symbol sequence that casts it
	SpellEffect effect = SpellEffect::Projectile;
	SpellSymbol element = SpellSymbol::Fire; // elemental flavour (bolt colour)
	float power = 8.0f;                 // base damage on a clean hit
	float mana = 4.0f;                  // mana points the cast costs
	float speed = 7.0f;                 // bolt travel speed (m/s)
	float range = 8.0f;                 // bolt reach (m) before it fizzles
};

// The project's recipe table: built once from the spells catalog, matched by an
// EXACT symbol sequence (order matters — Fire,Air differs from Air,Fire).
class SpellBook {
public:
	// (Re)builds the table from a spells catalog (each entry's `symbols` field is
	// the sequence). Malformed entries — unknown symbols, empty sequence — are
	// skipped with a warning.
	void Build(const Catalog& catalog);
	// The recipe whose sequence exactly equals `seq`, or null if none matches.
	const SpellDef* Match(std::span<const SpellSymbol> seq) const;
	// The recipe with catalog id `id`, or null if none. Lets a non-party caster
	// (a monster) reference a spell by name rather than reproduce its sequence.
	const SpellDef* Find(std::string_view id) const;
	bool Empty() const { return m_defs.empty(); }

private:
	std::vector<SpellDef> m_defs;
};

} // namespace dungeon::game
