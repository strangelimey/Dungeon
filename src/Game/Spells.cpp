#include "Game/Spells.h"

namespace dungeon::game {

namespace {
// Parallel to the SpellSymbol enum order.
constexpr const char* kIds[kSymbolCount] = {"fire", "earth", "air", "water"};
constexpr const char* kKeys[kSymbolCount] = {"symbol.fire", "symbol.earth",
											 "symbol.air", "symbol.water"};
} // namespace

const char* SymbolId(SpellSymbol s) { return kIds[static_cast<u32>(s)]; }
const char* SymbolKey(SpellSymbol s) { return kKeys[static_cast<u32>(s)]; }

bool ParseSymbol(std::string_view token, SpellSymbol& out) {
	for (u32 i = 0; i < kSymbolCount; ++i)
		if (token == kIds[i]) {
			out = static_cast<SpellSymbol>(i);
			return true;
		}
	return false;
}

} // namespace dungeon::game
