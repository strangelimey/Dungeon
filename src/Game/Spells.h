// ============================================================================
// Game/Spells.h — spell symbols (the casting alphabet).
//
// Dungeon Master style: each SYMBOL is a power, and a spell is a SEQUENCE of
// symbols (Fire + Air = fire bolt). Tier 1 is the four elements; more tiers
// come later. A character learns a symbol by memorizing a Rune item (the rune
// is consumed), so vocabulary is per character — stored as a bitmask on
// Character (knownSymbols). Recipes and behaviour live in the Spell classes
// (Game/Spell/, one file pair per spell); the project's spells.cat only
// OVERRIDES their numbers. This header is the symbol primitive shared by
// Character, the casting UI, and the resolver, plus the registry (SpellBook)
// that maps a sequence to its Spell.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon::game {

class Catalog; // Catalog.h — SpellBook::Build reads the spells catalog.

// The spell symbols: the four tier-1 SCHOOL elements first, then the shared
// tier-2 FORM symbols (Project = "throw it ahead", Protect = "guard the
// caster"; more forms follow — see docs/magic system.md). The enum order is
// the serialization order (save + catalog tokens) — APPEND new symbols,
// never reorder.
enum class SpellSymbol : u8 { Fire, Earth, Air, Water, Project, Protect, Count };

inline constexpr u32 kSymbolCount = static_cast<u32>(SpellSymbol::Count);

// The `hand` values a cast carries (DungeonWorld::CastSpell): 0/1 credit that
// hand's quick-cast MRU, -1 credits neither (the dev console), and kBookHands
// credits BOTH — the spellbook panel is member-driven (its selector row), not
// hand-fired, so a discovered spell reaches either hand's menu.
inline constexpr int kBookHands = 2;

// The bit for a symbol within a known-symbols mask (Character::knownSymbols).
inline constexpr u32 SymbolBit(SpellSymbol s) { return 1u << static_cast<u32>(s); }

// True for the four SCHOOL symbols — the base element runes (Fire/Earth/Air/
// Water, per docs/magic system.md's schools table). School runes are MUTUALLY
// EXCLUSIVE: a spell's sequence carries exactly ONE, in FIRST position (the
// first rune picks the school). The tier-2 FORM symbols are SHARED across
// schools — they append after the enum's first four and return false here.
inline constexpr bool IsSchoolSymbol(SpellSymbol s) {
	return static_cast<u32>(s) < 4;
}

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

// Premultiplied-additive accent colour for a symbol (rgb = emissive, alpha 0,
// per docs/magic system.md: Fire=red, Earth=brown, Air=white, Water=blue). The
// single source shared by the rune-tablet glow and the spell-bolt billboard.
// A SPELL always tints by its SCHOOL (Spell::School() — the first rune); the
// shared form runes carry a neutral gold of their own for tablet/UI ink.
Vec4 ElementColor(SpellSymbol s);

class Spell; // Spell/Spell.h — the spell base class (behaviour + defaults)

// The spell registry: every concrete Spell class (Spell/AllSpells.cpp), with
// the project's spells.cat NUMBERS laid over the class defaults, matched by
// an EXACT symbol sequence (order matters — Fire,Air differs from Air,Fire).
// Recipes and behaviour live in the classes; the catalog only tunes.
class SpellBook {
public:
	SpellBook();
	~SpellBook(); // out of line — Spell is incomplete here

	// (Re)builds the registry from the concrete classes, then applies the
	// spells catalog's numeric overrides (matched by entry id). Catalog
	// entries with no matching class — or stale `symbols` fields that no
	// longer agree with the class recipe — are warned about, never trusted.
	void Build(const Catalog& catalog);
	// The spell whose recipe exactly equals `seq`, or null if none matches.
	const Spell* Match(std::span<const SpellSymbol> seq) const;
	// The spell with id `id`, or null if none. Lets a non-party caster (a
	// monster) reference a spell by name rather than reproduce its sequence.
	const Spell* Find(std::string_view id) const;
	// Every spell, for UIs that enumerate castable spells (the hand-slot Magic
	// menu filters these by the member's known symbols).
	std::span<const std::unique_ptr<Spell>> Defs() const { return m_spells; }
	bool Empty() const { return m_spells.empty(); }

private:
	std::vector<std::unique_ptr<Spell>> m_spells;
};

} // namespace dungeon::game
