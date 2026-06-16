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
// resolver.
// ============================================================================
#pragma once

#include "Core/Types.h"

#include <string_view>

namespace dungeon::game {

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

} // namespace dungeon::game
