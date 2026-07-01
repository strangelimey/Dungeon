// ============================================================================
// Game/Magic.cpp — see Magic.h.
// ============================================================================
#include "Game/Magic.h"

#include "Game/Character.h"

namespace dungeon::game {

MagicSystem::MagicSystem() = default;

void MagicSystem::LoadSpells(const Catalog& spells) { m_spellBook.Build(spells); }

MagicSystem::CastReport MagicSystem::Cast(Character& caster,
										  std::span<const SpellSymbol> sequence,
										  const Vec3& origin, const Vec3& dir) {
	if (sequence.empty()) return {CastOutcome::NoRecipe, nullptr};

	// The caster must have memorized every symbol in the sequence.
	for (const SpellSymbol s : sequence)
		if (!caster.Knows(s)) return {CastOutcome::Unknown, nullptr};

	const SpellDef* spell = m_spellBook.Match(sequence);
	if (!spell) return {CastOutcome::NoRecipe, nullptr};
	if (caster.mana < spell->mana) return {CastOutcome::NoMana, spell};
	caster.mana -= spell->mana;

	// Emit the bolt spec for the owner to spawn ("on the map"). Accuracy rides
	// intelligence so a trained caster lands reliably; power is the recipe's
	// (caster-independent for now — weapon foci scale it later). A party spell
	// strikes monsters.
	ProjectileSpec bolt;
	bolt.pos = origin;
	bolt.dir = dir;
	bolt.speed = spell->speed;
	bolt.range = spell->range;
	bolt.atk = {spell->power, 0.70f + static_cast<float>(caster.intelligence) * 0.012f};
	const Vec4 g = ElementColor(spell->element);
	bolt.color = {g.x * 1.7f, g.y * 1.7f, g.z * 1.7f, 0.0f}; // bright additive
	bolt.size = 0.2f;
	bolt.target = TargetSide::Monsters;

	return {CastOutcome::Cast, spell, bolt};
}

} // namespace dungeon::game
