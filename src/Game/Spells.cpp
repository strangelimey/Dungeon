#include "Game/Spells.h"

#include "Core/Log.h"
#include "Game/Catalog.h"
#include "Game/Spell/Spell.h"

#include <algorithm>

namespace dungeon::game {

namespace {
// Parallel to the SpellSymbol enum order.
constexpr const char* kIds[kSymbolCount] = {"fire", "earth", "air", "water",
											"project", "protect", "sight"};
constexpr const char* kKeys[kSymbolCount] = {"symbol.fire", "symbol.earth",
											 "symbol.air", "symbol.water",
											 "symbol.project", "symbol.protect",
											 "symbol.sight"};

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
	// The shared form runes are school-less: a neutral arcane gold, distinct
	// from all four school accents (a cast spell never shows this — bolts tint
	// by Spell::School(), the first rune).
	case SpellSymbol::Project:
	case SpellSymbol::Protect:
	case SpellSymbol::Sight:   return {0.92f, 0.76f, 0.30f, 0.0f}; // gold
	default:                 return {1.0f, 1.0f, 1.0f, 0.0f};
	}
}

SpellBook::SpellBook() = default;
SpellBook::~SpellBook() = default;

void SpellBook::Build(const Catalog& catalog) {
	// The concrete classes ARE the recipe table (Spell/AllSpells.cpp); the
	// catalog gets the last word on NUMBERS only. Guard the class-authored
	// recipes anyway — a broken one should fail at load, loudly.
	m_spells = MakeAllSpells();
	for (const auto& spell : m_spells) {
		const auto seq = spell->Sequence();
		const auto schools = std::ranges::count_if(seq, IsSchoolSymbol);
		if (seq.empty() || schools != 1 || !IsSchoolSymbol(seq.front()))
			log::Warn("spell class '{}' breaks the one-school rule (exactly "
					  "one element rune, first)",
					  spell->Id());
	}

	// Lay the project's numeric overrides on top, matched by entry id. A
	// stale `symbols` field (the recipe is class identity now) and an entry
	// naming no class are warned about — data can tune, never redefine.
	for (const CatalogEntry& e : catalog.Entries()) {
		Spell* spell = nullptr;
		for (const auto& s : m_spells)
			if (s->Id() == e.id) { spell = s.get(); break; }
		if (!spell) {
			log::Warn("spells.cat entry '{}' has no spell class; ignored", e.id);
			continue;
		}
		if (const std::string symbols = e.Get("symbols", ""); !symbols.empty()) {
			std::vector<SpellSymbol> seq;
			if (!ParseSequence(symbols, seq) ||
				!std::ranges::equal(seq, spell->Sequence()))
				log::Warn("spells.cat entry '{}' symbols disagree with the "
						  "class recipe; the class wins",
						  e.id);
		}
		spell->ApplyOverrides(e);
	}
	log::Info("Spellbook: {} spells", m_spells.size());
}

const Spell* SpellBook::Match(std::span<const SpellSymbol> seq) const {
	for (const auto& s : m_spells)
		if (std::ranges::equal(s->Sequence(), seq)) return s.get();
	return nullptr;
}

const Spell* SpellBook::Find(std::string_view id) const {
	for (const auto& s : m_spells)
		if (s->Id() == id) return s.get();
	return nullptr;
}

} // namespace dungeon::game
