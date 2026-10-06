#include "Game/Spells.h"

#include "Core/Log.h"
#include "Game/Catalog.h"
#include "Game/Spell/Spell.h"
#include "Game/SpellIdList.h"

#include <algorithm>
#include <iterator>

namespace dungeon::game {

namespace {
// Parallel to the SpellSymbol enum order. Unsized, so a symbol appended to the
// enum without its row here fails the asserts below instead of reading a null.
constexpr const char* kIds[] = {"fire",  "earth", "air",      "water",   "project",
								"protect", "sight", "multiple", "explode", "light"};
// The rune tablets' item ids, spelled out rather than composed so RuneItemId
// can hand back a view (see Spells.h).
constexpr std::string_view kRuneIds[] = {
	"rune_fire",  "rune_earth", "rune_air",      "rune_water",  "rune_project",
	"rune_protect", "rune_sight", "rune_multiple", "rune_explode", "rune_light"};
constexpr const char* kKeys[] = {"symbol.fire",    "symbol.earth",   "symbol.air",
								 "symbol.water",   "symbol.project", "symbol.protect",
								 "symbol.sight",   "symbol.multiple", "symbol.explode",
								 "symbol.light"};
// The Futhark names: Kenaz, Berkano, Ansuz, Laguz, Tiwaz, Algiz, Dagaz,
// Ingwaz, Hagalaz, Sowilo.
constexpr const char* kRuneNameKeys[] = {
	"rune.fire",    "rune.earth", "rune.air",      "rune.water",   "rune.project",
	"rune.protect", "rune.sight", "rune.multiple", "rune.explode", "rune.light"};
static_assert(std::size(kIds) == kSymbolCount && std::size(kRuneIds) == kSymbolCount &&
			  std::size(kKeys) == kSymbolCount && std::size(kRuneNameKeys) == kSymbolCount);

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
const char* RuneNameKey(SpellSymbol s) { return kRuneNameKeys[static_cast<u32>(s)]; }

bool SymbolMayFollow(SpellSymbol s, std::span<const SpellSymbol> sequence) {
	// Each tier may appear once, and only straight after the one before it:
	// nothing -> school -> form -> modifier. That also rules out a repeat.
	const SymbolTier tier = TierOf(s);
	if (sequence.empty()) return tier == SymbolTier::School;
	const SymbolTier last = TierOf(sequence.back());
	if (last == SymbolTier::School) return tier == SymbolTier::Form;
	if (last == SymbolTier::Form) return tier == SymbolTier::Modifier;
	return false; // nothing follows a modifier
}

bool WellFormedRecipe(std::span<const SpellSymbol> sequence) {
	if (sequence.empty()) return false;
	for (size_t i = 0; i < sequence.size(); ++i)
		if (!SymbolMayFollow(sequence[i], sequence.first(i))) return false;
	return true;
}

bool ParseSymbol(std::string_view token, SpellSymbol& out) {
	for (u32 i = 0; i < kSymbolCount; ++i)
		if (token == kIds[i]) {
			out = static_cast<SpellSymbol>(i);
			return true;
		}
	return false;
}

std::string_view RuneItemId(SpellSymbol s) { return kRuneIds[static_cast<u32>(s)]; }

// The two tables above spell the same names; a mismatch would bake (and look up)
// one symbol's carving under another's.
static_assert([] {
	for (u32 i = 0; i < kSymbolCount; ++i)
		if (kRuneIds[i].substr(5) != std::string_view(kIds[i])) return false;
	return true;
}());

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
	case SpellSymbol::Sight:
	case SpellSymbol::Multiple:
	case SpellSymbol::Explode:
	case SpellSymbol::Light:   return {0.92f, 0.76f, 0.30f, 0.0f}; // gold
	default:                 return {1.0f, 1.0f, 1.0f, 0.0f};
	}
}

SpellBook::SpellBook() = default;
SpellBook::~SpellBook() = default;

void SpellBook::Build(const Catalog& catalog, const DamageTypeBook& types) {
	// The concrete classes ARE the recipe table (Spell/AllSpells.cpp); the
	// catalog gets the last word on NUMBERS only. Guard the class-authored
	// recipes anyway — a broken one should fail at load, loudly.
	m_spells = MakeAllSpells();
	// Hand every spell the damage-type vocabulary before anything asks a bolt
	// what it deals — the classes were constructed before a project existed.
	for (const auto& spell : m_spells) spell->SetTypes(&types);
	for (const auto& spell : m_spells)
		if (!WellFormedRecipe(spell->Sequence()))
			log::Warn("spell class '{}' breaks the recipe grammar (a school "
					  "rune, then at most one form, then at most one modifier)",
					  spell->Id());

	// Lay the project's numeric overrides on top, matched by entry id. A
	// stale `symbols` field (the recipe is class identity now) and an entry
	// naming no class are warned about — data can tune, never redefine.
	//
	// IN TWO PASSES: every spell that stands alone first; then each spell built
	// on another (Spell::Form - a modifier on a form) takes its defaults from
	// that form AS TUNED (DeriveFromForm), and only then its own entry. In one
	// pass a modified spell's defaults were its form's CLASS numbers, and its
	// bolts carried its form's on-hit whatever its own entry said (code-review
	// C19).
	const auto spellFor = [&](const CatalogEntry& e) -> Spell* {
		for (const auto& s : m_spells)
			if (s->Id() == e.id) return s.get();
		return nullptr;
	};
	for (const CatalogEntry& e : catalog.Entries()) {
		Spell* spell = spellFor(e);
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
		if (!spell->Form()) spell->ApplyOverrides(e);
	}
	for (const auto& spell : m_spells)
		if (spell->Form()) spell->DeriveFromForm();
	for (const CatalogEntry& e : catalog.Entries())
		if (Spell* spell = spellFor(e); spell && spell->Form()) spell->ApplyOverrides(e);
	log::Info("Spellbook: {} spells", m_spells.size());
	// A member's learned list and quick-cast lists hold at most this many: past
	// it, a newly cast spell is silently not learned.
	if (m_spells.size() > SpellIdList::kSlots)
		log::Warn("the spellbook holds {} spells but a member can learn only {} "
				  "(SpellIdList::kSlots) - raise it",
				  m_spells.size(), SpellIdList::kSlots);
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
