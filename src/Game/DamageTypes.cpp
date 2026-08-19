// ============================================================================
// Game/DamageTypes.cpp — DamageTypeBook (declared in Combat.h).
//
// Its own translation unit ON PURPOSE. Combat.h promises that the strike
// resolver is pure — numbers in, numbers out — and tools/RollTest holds it to
// that by compiling Combat.cpp straight in and linking Core ALONE. Building
// the book needs the catalog reader, which needs the file layer, which needs
// Assets; leaving it beside ResolveAttack dragged all of that into a harness
// that only wanted to roll dice. Splitting the file is what keeps the harness
// measuring the shipping resolver instead of a copy of it.
// ============================================================================
#include "Core/Log.h"
#include "Game/Catalog.h"
#include "Game/Combat.h"

namespace dungeon::game {

namespace {

// The seven the game shipped with, seeded ONLY when a project defines no types
// of its own (see the class comment: a compatibility floor, not a default).
// Order matches the retired enum, so a project that predates damagetypes.cat
// keeps the exact indices its content was authored against.
struct Seed {
	const char* id;
	bool physical;
	bool hasSchool;
	SpellSymbol school;
};
constexpr Seed kSeeds[] = {
	{"slash", true, false, SpellSymbol::Fire},
	{"pierce", true, false, SpellSymbol::Fire},
	{"bash", true, false, SpellSymbol::Fire},
	{"fire", false, true, SpellSymbol::Fire},
	{"earth", false, true, SpellSymbol::Earth},
	{"air", false, true, SpellSymbol::Air},
	{"water", false, true, SpellSymbol::Water},
};

const std::string kNoId; // returned for an out-of-range index

} // namespace

void DamageTypeBook::Build(const Catalog& catalog) {
	m_entries.clear();
	m_hasSchool.fill(false);
	m_bySchool.fill(DamageType{});

	for (const CatalogEntry& e : catalog.Entries()) {
		if (m_entries.size() >= kMaxDamageTypes) {
			log::Warn("damagetypes.cat: more than {} types; '{}' and any after "
					  "it are ignored (kMaxDamageTypes sizes every ResistTable)",
					  kMaxDamageTypes, e.id);
			break;
		}
		Entry entry;
		entry.id = e.id;
		entry.nameKey = e.Get("name", "dmg." + e.id);
		entry.physical = e.GetBool("physical", false);
		if (const std::string school = e.Get("school", ""); !school.empty()) {
			if (ParseSymbol(school, entry.school)) {
				entry.hasSchool = true;
			} else {
				log::Warn("damagetypes.cat [{}]: unknown school '{}'", e.id, school);
			}
		}
		m_entries.push_back(std::move(entry));
	}

	if (m_entries.empty()) {
		log::Warn("damagetypes.cat is missing or empty — seeding the seven "
				  "built-in damage types");
		for (const Seed& s : kSeeds) {
			Entry entry;
			entry.id = s.id;
			entry.nameKey = std::string("dmg.") + s.id;
			entry.physical = s.physical;
			entry.hasSchool = s.hasSchool;
			entry.school = s.school;
			m_entries.push_back(std::move(entry));
		}
	}

	// Resolve school -> type ONCE. A per-tick lookup is then an array index,
	// which matters because every DoT bite and every enchanted blow asks.
	for (size_t i = 0; i < m_entries.size(); ++i) {
		const Entry& e = m_entries[i];
		if (!e.hasSchool) continue;
		const size_t s = static_cast<size_t>(e.school);
		if (m_hasSchool[s]) {
			log::Warn("damagetypes.cat [{}]: school '{}' is already claimed by "
					  "'{}'; the first one wins",
					  e.id, SymbolId(e.school), m_entries[m_bySchool[s].index].id);
			continue;
		}
		m_hasSchool[s] = true;
		m_bySchool[s] = DamageType{static_cast<u8>(i)};
	}

	log::Info("damage types: {} loaded", m_entries.size());
}

bool DamageTypeBook::Find(std::string_view id, DamageType& out) const {
	for (size_t i = 0; i < m_entries.size(); ++i)
		if (m_entries[i].id == id) {
			out = DamageType{static_cast<u8>(i)};
			return true;
		}
	return false;
}

DamageType DamageTypeBook::FindOr(std::string_view id, DamageType fallback) const {
	DamageType t;
	return Find(id, t) ? t : fallback;
}

const std::string& DamageTypeBook::Id(DamageType t) const {
	return t.index < m_entries.size() ? m_entries[t.index].id : kNoId;
}

const std::string& DamageTypeBook::NameKey(DamageType t) const {
	return t.index < m_entries.size() ? m_entries[t.index].nameKey : kNoId;
}

bool DamageTypeBook::IsPhysical(DamageType t) const {
	return t.index < m_entries.size() && m_entries[t.index].physical;
}

bool DamageTypeBook::SchoolOf(DamageType t, SpellSymbol& out) const {
	if (t.index >= m_entries.size() || !m_entries[t.index].hasSchool) return false;
	out = m_entries[t.index].school;
	return true;
}

DamageType DamageTypeBook::ForSchool(SpellSymbol school) const {
	const size_t s = static_cast<size_t>(school);
	// The form runes (Project/Protect/Sight) claim no type — they never deal
	// damage on their own, and index 0 is the honest "no element" answer.
	return s < m_hasSchool.size() && m_hasSchool[s] ? m_bySchool[s] : DamageType{};
}

} // namespace dungeon::game
