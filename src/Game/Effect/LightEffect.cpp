// ============================================================================
// Game/Effect/LightEffect.cpp - see LightEffect.h.
// ============================================================================
#include "Game/Effect/LightEffect.h"

#include "Game/Catalog.h"

namespace dungeon::game::fx {

LightEffect::LightEffect()
	: EffectKind("light", Category::Marker, "spell.firelight", Stacking::RefreshPerSchool),
	  m_schoolNames{"spell.firelight", "spell.stonelight", "spell.skylight",
					"spell.tidelight"} {
	m_iconItem = "rune_light"; // wears the Sowilo tablet's face
}

std::string_view LightEffect::NameKey(const Inst& inst) const {
	const size_t school = static_cast<size_t>(inst.school);
	return school < m_schoolNames.size() ? m_schoolNames[school] : m_nameKey;
}

void LightEffect::ApplyOverrides(const CatalogEntry& e, const DamageTypeBook& types) {
	EffectKind::ApplyOverrides(e, types);
	static constexpr const char* kSchoolKeys[] = {"name_fire", "name_earth", "name_air",
												  "name_water"};
	for (size_t i = 0; i < m_schoolNames.size(); ++i)
		m_schoolNames[i] = e.Get(kSchoolKeys[i], m_schoolNames[i]);
	m_scalePower = e.GetFloat("scale_power", m_scalePower);
	if (m_scalePower <= 0.0f) m_scalePower = 8.0f;
}

DazzleEffect::DazzleEffect()
	: EffectKind("dazzle", Category::Marker, "effect.dazzle", Stacking::Refresh) {}

} // namespace dungeon::game::fx
