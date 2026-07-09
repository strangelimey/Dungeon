// ============================================================================
// Game/Magic.cpp — see Magic.h.
// ============================================================================
#include "Game/Magic.h"

#include "Game/Balance.h"
#include "Game/Character.h"

#include <algorithm>

namespace dungeon::game {

MagicSystem::MagicSystem() = default;

void MagicSystem::LoadSpells(const Catalog& spells) { m_spellBook.Build(spells); }

MagicSystem::CastReport MagicSystem::Cast(Character& caster,
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
	const float failChance =
		std::clamp(0.35f * static_cast<float>(spell->Difficulty() - 1) -
					   0.10f * static_cast<float>(level) -
					   0.01f * static_cast<float>(caster.willpower),
				   0.0f, 0.9f);
	if (failChance > 0.0f &&
		std::uniform_real_distribution<float>(0.0f, 1.0f)(rng) < failChance)
		return {CastOutcome::Fumble, spell};

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
	CastContext ctx{caster, origin, dir, power, level, m_services};
	spell->Cast(ctx);

	return {CastOutcome::Cast, spell};
}

} // namespace dungeon::game
