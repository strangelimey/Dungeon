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
										  const Vec3& origin, const Vec3& dir) {
	if (sequence.empty()) return {CastOutcome::NoRecipe, nullptr};

	// The caster must have memorized every symbol in the sequence.
	for (const SpellSymbol s : sequence)
		if (!caster.Knows(s)) return {CastOutcome::Unknown, nullptr};

	const SpellDef* spell = m_spellBook.Match(sequence);
	if (!spell) return {CastOutcome::NoRecipe, nullptr};
	if (caster.mana < spell->mana) return {CastOutcome::NoMana, spell};
	caster.mana -= spell->mana;

	// Add the bolt to the live set ("on the map"). Accuracy rides intelligence so
	// a trained caster lands reliably; power is the recipe's (caster-independent
	// for now — weapon foci scale it later).
	Projectile bolt;
	bolt.pos = origin;
	bolt.dir = dir;
	bolt.speed = spell->speed;
	bolt.rangeLeft = spell->range;
	bolt.atk = {spell->power, 0.70f + static_cast<float>(caster.intelligence) * 0.012f};
	const Vec4 g = ElementColor(spell->element);
	bolt.color = {g.x * 1.7f, g.y * 1.7f, g.z * 1.7f, 0.0f}; // bright additive
	bolt.size = 0.2f;
	m_projectiles.push_back(bolt);

	return {CastOutcome::Cast, spell};
}

void MagicSystem::SpawnSparkBurst(const Vec3& pos, const Vec4& color, int count) {
	for (int i = 0; i < count; ++i) {
		Spark s;
		s.pos = pos;
		auto r = [&] { return (static_cast<float>(m_rng() & 0xFFFF) / 32768.0f) - 1.0f; };
		s.vel = {r() * 2.2f, r() * 2.2f + 0.6f, r() * 2.2f};
		s.color = {color.x, color.y, color.z, 0.0f}; // additive
		s.age = 0.0f;
		s.life = 0.25f + (static_cast<float>(m_rng() & 0xFF) / 255.0f) * 0.2f;
		s.size = 0.1f;
		m_sparks.push_back(s);
	}
}

void MagicSystem::Update(float dt) {
	// Age the impact/fizzle sparks (drift out + slight gravity, then expire).
	for (Spark& s : m_sparks) {
		s.age += dt;
		s.pos = Add(s.pos, Scale(s.vel, dt));
		s.vel.y -= 3.5f * dt;
	}
	std::erase_if(m_sparks, [](const Spark& s) { return s.age >= s.life; });

	// Fly each bolt: a wall/out-of-range fizzles, a target in its cell takes a
	// strike. rangeLeft < 0 marks a bolt spent (erased below).
	for (Projectile& p : m_projectiles) {
		const float step = p.speed * dt;
		p.pos = Add(p.pos, Scale(p.dir, step));
		p.rangeLeft -= step;

		if (isBlocked && isBlocked(p.pos)) { // hit a wall (or left the map)
			SpawnSparkBurst(p.pos, p.color, 8);
			if (onFizzle) onFizzle(p.pos);
			p.rangeLeft = -1.0f;
			continue;
		}

		if (resolveHit && resolveHit(p.pos, p.atk)) { // struck a target
			SpawnSparkBurst(p.pos, p.color, 14);
			p.rangeLeft = -1.0f;
			continue;
		}

		if (p.rangeLeft <= 0.0f) { // ran out of reach in open air
			SpawnSparkBurst(p.pos, p.color, 6);
			if (onFizzle) onFizzle(p.pos);
		}
	}
	std::erase_if(m_projectiles, [](const Projectile& p) { return p.rangeLeft <= 0.0f; });
}

void MagicSystem::AppendBillboards(std::vector<gfx::ParticleInstance>& out) const {
	for (const Projectile& p : m_projectiles) out.push_back({p.pos, p.size, p.color});
	for (const Spark& s : m_sparks) {
		const float fade = 1.0f - s.age / s.life; // dim as it ages
		out.push_back(
			{s.pos, s.size, {s.color.x * fade, s.color.y * fade, s.color.z * fade, 0.0f}});
	}
}

} // namespace dungeon::game
