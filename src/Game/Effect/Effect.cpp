// ============================================================================
// Game/Effect/Effect.cpp — see Effect.h.
// ============================================================================
#include "Game/Effect/Effect.h"

#include "Core/Log.h"
#include "Game/Catalog.h"

#include <algorithm>

namespace dungeon::game::fx {

// --- EffectKind ---------------------------------------------------------------

EffectKind::EffectKind(std::string id, Category category, std::string nameKey,
					   Stacking stacking)
	: m_id(std::move(id)), m_nameKey(std::move(nameKey)),
	  m_category(category), m_stacking(stacking) {}

std::string_view EffectKind::NameKey(const Inst&) const { return m_nameKey; }

void EffectKind::ApplyOverrides(const CatalogEntry& e) {
	m_nameKey = e.Get("name", m_nameKey);
	m_iconItem = e.Get("icon", m_iconItem);
	const std::string stacking = e.Get("stacking", "");
	if (stacking == "refresh") m_stacking = Stacking::Refresh;
	else if (stacking == "school") m_stacking = Stacking::RefreshPerSchool;
	else if (stacking == "stack") m_stacking = Stacking::Stack;
	else if (!stacking.empty())
		log::Warn("effects.cat [{}]: unknown stacking '{}'", m_id, stacking);
}

float EffectKind::StatBonus(const Inst&, std::string_view) const { return 0.0f; }
float EffectKind::SpeedScale(const Inst&) const { return 1.0f; }

// --- landing one -------------------------------------------------------------

Inst& Apply(std::vector<Inst>& effects, const EffectKind& kind,
			SpellSymbol school, float magnitude, float duration, int source) {
	switch (kind.Stack()) {
	case Stacking::Refresh:
		std::erase_if(effects,
					  [&](const Inst& e) { return e.kind == &kind; });
		break;
	case Stacking::RefreshPerSchool:
		std::erase_if(effects, [&](const Inst& e) {
			return e.kind == &kind && e.school == school;
		});
		break;
	case Stacking::Stack:
		break; // every application is its own instance
	}
	effects.push_back({&kind, school, magnitude, duration, duration, source});
	return effects.back();
}

// --- EffectBook ---------------------------------------------------------------

void EffectBook::Build(const Catalog& catalog) {
	// The classes ARE the effect table (Effect/AllEffects.cpp); the catalog
	// gets the last word on numbers and look only.
	m_kinds = MakeAllEffects();
	for (const CatalogEntry& e : catalog.Entries()) {
		EffectKind* kind = nullptr;
		for (const auto& k : m_kinds)
			if (k->Id() == e.id) { kind = k.get(); break; }
		if (!kind) {
			log::Warn("effects.cat entry '{}' has no effect class; ignored", e.id);
			continue;
		}
		kind->ApplyOverrides(e);
	}
}

const EffectKind* EffectBook::Find(std::string_view id) const {
	for (const auto& k : m_kinds)
		if (k->Id() == id) return k.get();
	return nullptr;
}

const EffectKind* EffectBook::FindLegacy(std::string_view token,
										 SpellSymbol school) const {
	// Saves written before the effects system stored the CATEGORY as the kind
	// token, with the school telling the four wards apart. Map those forward
	// so an old save keeps its wards; the ids are the save format now.
	if (token != "ward") return Find(token); // poison / bleed / sight, unchanged
	switch (school) {
	case SpellSymbol::Fire:  return Find("fireshield");
	case SpellSymbol::Earth: return Find("stoneskin");
	case SpellSymbol::Air:   return Find("windward");
	case SpellSymbol::Water: return Find("waterveil");
	default: return nullptr;
	}
}

} // namespace dungeon::game::fx
