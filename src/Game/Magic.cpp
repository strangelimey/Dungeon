// ============================================================================
// Game/Magic.cpp — see Magic.h.
// ============================================================================
#include "Game/Magic.h"

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

	const SpellDef* spell = m_spellBook.Match(sequence);
	if (!spell) return {CastOutcome::NoRecipe, nullptr};
	if (caster.mana < spell->mana) return {CastOutcome::NoMana, spell};
	caster.mana -= spell->mana;

	// The skill roll (docs/skills.md "Skills → spells"): difficulty is the
	// recipe's rune count, opposed by the caster's SCHOOL skill level and a
	// touch of willpower. Tier-1 never fails; a fumbled cast has already
	// spent its mana — the energy slips away — and teaches nothing.
	const int level = caster.SkillLevel(SymbolId(spell->element));
	const float failChance =
		std::clamp(0.35f * static_cast<float>(sequence.size() - 1) -
					   0.10f * static_cast<float>(level) -
					   0.01f * static_cast<float>(caster.willpower),
				   0.0f, 0.9f);
	if (failChance > 0.0f &&
		std::uniform_real_distribution<float>(0.0f, 1.0f)(rng) < failChance)
		return {CastOutcome::Fumble, spell};

	// Effective power: the catalog number scaled by the caster's school skill
	// — the per-school caster POWER the growth forms scale by.
	const float power =
		spell->power * (1.0f + 0.10f * static_cast<float>(level));

	// A non-projectile effect carries no bolt — the owner dispatches on
	// spell->effect (a Shield lands on the caster in DungeonWorld::CastSpell).
	if (spell->effect != SpellEffect::Projectile)
		return {CastOutcome::Cast, spell, power};

	// Emit the bolt spec for the owner to spawn ("on the map"). Accuracy rides
	// intelligence plus school skill so a trained caster lands reliably. A
	// party spell strikes monsters.
	ProjectileSpec bolt;
	bolt.pos = origin;
	bolt.dir = dir;
	bolt.speed = spell->speed;
	bolt.range = spell->range;
	bolt.atk = {power, 0.70f + static_cast<float>(caster.intelligence) * 0.012f +
						   0.02f * static_cast<float>(level)};
	const Vec4 g = ElementColor(spell->element);
	bolt.color = {g.x * 1.7f, g.y * 1.7f, g.z * 1.7f, 0.0f}; // bright additive
	bolt.size = 0.2f;
	bolt.target = TargetSide::Monsters;
	bolt.push = spell->push; // displacement rides the bolt (the air-school shove)

	return {CastOutcome::Cast, spell, power, bolt};
}

} // namespace dungeon::game
