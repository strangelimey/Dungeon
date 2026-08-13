// ============================================================================
// Game/Magic.cpp — see Magic.h.
// ============================================================================
#include "Game/Magic.h"

#include "Game/Curve.h"
#include "Game/Defense.h" // StanceAttack / ExertionPoints — the stance arithmetic

#include "Game/Balance.h"
#include "Game/Character.h"

#include <algorithm>

namespace dungeon::game {

MagicSystem::MagicSystem() = default;

void MagicSystem::LoadSpells(const Catalog& spells, const DamageTypeBook& types) {
	m_spellBook.Build(spells, types);
}

MagicSystem::CastReport MagicSystem::Cast(Character& caster, int casterIndex,
										  std::span<const SpellSymbol> sequence,
										  const Vec3& origin, const Vec3& dir,
										  std::mt19937& rng) {
	if (sequence.empty()) return {CastOutcome::NoRecipe, nullptr};

	// The caster must have memorized every symbol in the sequence.
	for (const SpellSymbol s : sequence)
		if (!caster.Knows(s)) return {CastOutcome::Unknown, nullptr};

	const Spell* spell = m_spellBook.Match(sequence);
	if (!spell) return {CastOutcome::NoRecipe, nullptr};
	if (caster.mana < spell->Mana()) return {CastOutcome::NoMana, spell};
	caster.mana -= spell->Mana();

	// The skill roll (docs/skills.md "Skills → spells"): difficulty is the
	// recipe's rune count, opposed by the caster's SCHOOL skill level and a
	// touch of willpower. Tier-1 never fails; a fumbled cast has already
	// spent its mana — the energy slips away — and teaches nothing.
	const int level = caster.SkillLevel(SymbolId(spell->School()));
	// What a stance past 1 buys this cast, for the OWNER to bill (CastReport
	// carries it out; this module never touches stamina or health). Computed
	// above the fumble gate on purpose — a fumbled cast was still thrown, which
	// is the same reason its mana is already gone.
	const float exertion =
		m_balance ? defense::ExertionPoints(caster.offenseShare,
											static_cast<float>(level),
											m_balance->SkillCurve())
				  : 0.0f;
	const float failChance =
		std::clamp(0.35f * static_cast<float>(spell->Difficulty() - 1) -
					   0.10f * static_cast<float>(level) -
					   0.01f * static_cast<float>(caster.willpower),
				   0.0f, 0.9f);
	if (failChance > 0.0f &&
		std::uniform_real_distribution<float>(0.0f, 1.0f)(rng) < failChance)
		return {CastOutcome::Fumble, spell, exertion};

	// The gates have passed — the spell's own Cast() lands the effect (a bolt
	// spawns, a ward settles, ...) through the owner-wired services, at
	// EFFECTIVE power: the class/catalog number scaled by school skill (the
	// per-school caster POWER the growth forms scale by) plus the school's
	// associated-stat bonus (docs/combat.md part 2: earth/fire ride INT,
	// air/water WIL, at the spell_stat knob).
	float power = spell->Power() * (1.0f + 0.10f * static_cast<float>(level));
	if (m_balance)
		power *= 1.0f + m_balance->spellStat *
							caster.StatAvg(SchoolStats(spell->School()));
	// The caster's side of the opposed roll, assembled HERE because this is
	// where Balance is: the school skill through the skill curve, the school's
	// associated stat through the stat curve. The spell class receives the
	// finished number and stays ignorant of both.
	//
	// The STANCE scales the school-skill term and only that — the same points it
	// takes off the caster's guard are the ones it puts behind the cast, exactly
	// as a swing's do (defense::StanceAttack). The school's stat is not skill and
	// rides at full weight.
	float attackBonus = 50.0f;
	if (m_balance) {
		attackBonus =
			defense::StanceAttack(caster.offenseShare, static_cast<float>(level),
								  m_balance->SkillCurve()) +
			CurveValue(caster.StatAvg(SchoolStats(spell->School())),
					   m_balance->StatCurve());
	}
	CastContext ctx{caster,    origin,      dir,         power,
					level,     m_services,  casterIndex, attackBonus};
	spell->Cast(ctx);

	return {CastOutcome::Cast, spell, exertion};
}

} // namespace dungeon::game
