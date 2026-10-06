// ============================================================================
// Game/Effect/LightEffect.cpp - see LightEffect.h.
// ============================================================================
#include "Game/Effect/LightEffect.h"

#include "Game/Catalog.h"

#include <algorithm>

namespace dungeon::game::fx {

LightEffect::LightEffect()
	: EffectKind("light", Category::Marker, "spell.firelight", Stacking::RefreshPerSchool),
	  m_schoolNames{"spell.firelight", "spell.stonelight", "spell.skylight",
					"spell.tidelight"} {
	m_iconItem = "rune_light"; // wears the Sowilo tablet's face
	// Its OWN fade line, naming the light ("Maren's Firelight fades."). As a
	// Marker it once took the Sight spell's, by category (code-review C9).
	m_fadeParty = "log.light_fades";
}

std::string_view LightEffect::NameKey(const Inst& inst) const {
	const size_t school = static_cast<size_t>(inst.school);
	return school < m_schoolNames.size() ? m_schoolNames[school] : m_nameKey;
}

void LightEffect::ApplyOverrides(const CatalogEntry& e, const DamageTypeBook& types) {
	EffectKind::ApplyOverrides(e, types);
	static constexpr const char* kSchoolKeys[] = {"name_fire", "name_earth", "name_air",
												  "name_water"};
	static_assert(std::size(kSchoolKeys) == kSchoolCount, "a name key per school");
	for (size_t i = 0; i < m_schoolNames.size(); ++i)
		m_schoolNames[i] = e.Get(kSchoolKeys[i], m_schoolNames[i]);
	m_scalePower = e.GetFloat("scale_power", m_scalePower);
	if (m_scalePower <= 0.0f) m_scalePower = 8.0f;
	m_kindleBrazierPower = e.GetFloat("kindle_brazier_power", m_kindleBrazierPower);
	m_scorchEvery = std::max(0.2f, e.GetFloat("scorch_every", m_scorchEvery));
	m_scorchDamage = e.GetFloat("scorch_damage", m_scorchDamage);
	m_soothe = std::max(0.0f, e.GetFloat("soothe", m_soothe));
	m_clearHaze = std::max(0.0f, e.GetFloat("clear_haze", m_clearHaze));
	m_crackleEvery = std::max(0.2f, e.GetFloat("crackle_every", m_crackleEvery));
	m_crackleDamage = e.GetFloat("crackle_damage", m_crackleDamage);
	m_warnRate = std::max(1.0f, e.GetFloat("warn_rate", m_warnRate));
	m_stoneItem = e.Get("stone_item", m_stoneItem);
}

DazzleEffect::DazzleEffect()
	: EffectKind("dazzle", Category::Marker, "effect.dazzle", Stacking::Refresh) {
	m_fadeMonster = "log.monster_dazzle_fades"; // it can see again
}

} // namespace dungeon::game::fx
