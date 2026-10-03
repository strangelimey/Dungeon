// ============================================================================
// Game/Spell/LightSpell.cpp - see LightSpell.h.
// ============================================================================
#include "Game/Spell/LightSpell.h"

#include "Core/Loc.h"
#include "Game/Catalog.h"
#include "Game/Character.h"

#include <algorithm>

namespace dungeon::game {

LightSpell::LightSpell(std::string id, std::vector<SpellSymbol> sequence, float power,
					   float mana, float duration)
	: Spell(std::move(id), std::move(sequence), power, mana), m_duration(duration) {}

float LightSpell::DurationFor(float power) const {
	const float base = std::max(Power(), 1.0f);
	return m_duration * std::max(0.5f, power / base);
}

void LightSpell::Cast(CastContext& ctx) const { LightOn(ctx, ctx.power, ctx.power); }

void LightSpell::LightOn(CastContext& ctx, float power, float durationPower) const {
	// One kind for all four, told apart by the school it lands with - which is
	// also how it names itself on the sheet.
	ctx.services.applyEffect(ctx.caster, "light", School(), power, DurationFor(durationPower));
	ctx.services.message(ctx.caster, loc::FormatLine("log.light_up", ctx.caster.name));
}

void LightSpell::Flare(CastContext& ctx, float power) const {
	if (ctx.services.lightFlare) ctx.services.lightFlare(School(), power, ctx.casterIndex);
	ctx.services.message(ctx.caster, loc::FormatLine("log.light_flare", ctx.caster.name));
}

void LightSpell::ApplyOverrides(const CatalogEntry& e) {
	Spell::ApplyOverrides(e);
	m_duration = e.GetFloat("duration", m_duration);
}

} // namespace dungeon::game
