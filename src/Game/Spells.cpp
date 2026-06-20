#include "Game/Spells.h"

#include "Core/Log.h"
#include "Game/Catalog.h"

#include <algorithm>

namespace dungeon::game {

namespace {
// Parallel to the SpellSymbol enum order.
constexpr const char* kIds[kSymbolCount] = {"fire", "earth", "air", "water"};
constexpr const char* kKeys[kSymbolCount] = {"symbol.fire", "symbol.earth",
											 "symbol.air", "symbol.water"};

// Parses a comma-separated symbol list ("fire,air") into a sequence. Returns
// false (and leaves `out` partial) on the first unknown token; an empty / blank
// field yields an empty sequence (caller treats that as malformed).
bool ParseSequence(std::string_view list, std::vector<SpellSymbol>& out) {
	out.clear();
	size_t start = 0;
	while (start <= list.size()) {
		size_t comma = list.find(',', start);
		std::string_view tok =
			list.substr(start, comma == std::string_view::npos ? std::string_view::npos
															   : comma - start);
		// Trim surrounding spaces so "fire, air" parses.
		while (!tok.empty() && tok.front() == ' ') tok.remove_prefix(1);
		while (!tok.empty() && tok.back() == ' ') tok.remove_suffix(1);
		if (!tok.empty()) {
			SpellSymbol s;
			if (!ParseSymbol(tok, s)) return false;
			out.push_back(s);
		}
		if (comma == std::string_view::npos) break;
		start = comma + 1;
	}
	return true;
}
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

std::string RuneItemId(SpellSymbol s) { return std::string("rune_") + SymbolId(s); }

bool RuneSymbolFromItemId(std::string_view typeId, SpellSymbol& out) {
	constexpr std::string_view kPrefix = "rune_";
	if (!typeId.starts_with(kPrefix)) return false;
	return ParseSymbol(typeId.substr(kPrefix.size()), out);
}

Vec4 ElementColor(SpellSymbol s) {
	switch (s) {
	case SpellSymbol::Fire:  return {1.00f, 0.13f, 0.08f, 0.0f}; // red
	case SpellSymbol::Earth: return {0.60f, 0.36f, 0.16f, 0.0f}; // brown
	case SpellSymbol::Air:   return {1.00f, 1.00f, 1.00f, 0.0f}; // white
	case SpellSymbol::Water: return {0.18f, 0.42f, 1.00f, 0.0f}; // blue
	default:                 return {1.0f, 1.0f, 1.0f, 0.0f};
	}
}

void SpellBook::Build(const Catalog& catalog) {
	m_defs.clear();
	for (const CatalogEntry& e : catalog.Entries()) {
		SpellDef def;
		def.id = e.id;
		def.nameKey = e.Get("name", "spell." + e.id);
		if (!ParseSequence(e.Get("symbols"), def.sequence) || def.sequence.empty()) {
			log::Warn("spell '{}' has no valid symbol sequence; skipped", e.id);
			continue;
		}
		// effect kind (only "projectile" exists in tier 1).
		const std::string effect = e.Get("effect", "projectile");
		if (effect != "projectile")
			log::Warn("spell '{}' has unknown effect '{}'; treating as projectile",
					  e.id, effect);
		def.effect = SpellEffect::Projectile;

		SpellSymbol elem;
		def.element = ParseSymbol(e.Get("element", "fire"), elem) ? elem : SpellSymbol::Fire;
		def.power = e.GetFloat("power", 8.0f);
		def.mana = e.GetFloat("mana", 4.0f);
		def.speed = e.GetFloat("speed", 7.0f);
		def.range = e.GetFloat("range", 8.0f);
		m_defs.push_back(std::move(def));
	}
	log::Info("Spellbook: {} recipes", m_defs.size());
}

const SpellDef* SpellBook::Match(std::span<const SpellSymbol> seq) const {
	for (const SpellDef& d : m_defs)
		if (std::ranges::equal(d.sequence, seq)) return &d;
	return nullptr;
}

} // namespace dungeon::game
