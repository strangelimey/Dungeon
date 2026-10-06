// ============================================================================
// Game/Effect/SightEffect.cpp — see SightEffect.h.
// ============================================================================
#include "Game/Effect/SightEffect.h"

#include "Game/Catalog.h"

namespace dungeon::game::fx {

SightEffect::SightEffect()
	: EffectKind("sight", Category::Marker, "spell.embersight",
				 Stacking::RefreshPerSchool),
	  m_schoolNames{"spell.embersight", "spell.stonesight", "spell.farsight",
					"spell.scrying"} {
	m_iconRune = SpellSymbol::Sight; // wears the Sight rune's glyph
	m_fadeParty = "log.sight_fades"; // the stone closes again
}

std::string_view SightEffect::NameKey(const Inst& inst) const {
	const size_t school = static_cast<size_t>(inst.school);
	return school < m_schoolNames.size() ? m_schoolNames[school] : m_nameKey;
}

void SightEffect::ApplyOverrides(const CatalogEntry& e, const DamageTypeBook& types) {
	EffectKind::ApplyOverrides(e, types);
	// Per-school display names, should a project rename its sight spells:
	// name_fire = ... / name_earth = ... / name_air = ... / name_water = ...
	static constexpr const char* kSchoolKeys[] = {"name_fire", "name_earth",
												  "name_air", "name_water"};
	static_assert(std::size(kSchoolKeys) == kSchoolCount, "a name key per school");
	for (size_t i = 0; i < m_schoolNames.size(); ++i)
		m_schoolNames[i] = e.Get(kSchoolKeys[i], m_schoolNames[i]);
}

} // namespace dungeon::game::fx
