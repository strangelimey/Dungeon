// ============================================================================
// Game/Magic.h — the magic system: casting + live spell effects.
//
// This is the one runtime home for magic. Spells.h is the static DATA layer (the
// SpellSymbol alphabet a Character memorizes + the SpellBook recipe table);
// MagicSystem is the RUNTIME that turns a cast into a travelling bolt, flies it,
// and renders it. It owns the live projectiles and their impact sparks (purely
// transient — never saved) and draws them as additive billboards.
//
// MagicSystem deliberately knows nothing about the dungeon map, the monster
// list, the HUD log, or audio. It reaches those other modules through a small
// set of hooks the owner (DungeonWorld) wires up once:
//   - isBlocked   : does this world position stop a bolt (a wall / off-map)?
//   - resolveHit  : a bolt reached here — resolve a strike against whatever
//                   lives at it (combat + feedback); did it hit a target?
//   - onFizzle    : a bolt died on a wall / at max range (for a sound).
// So a cast "adds a projectile to the map" and that bolt later damages a monster
// without MagicSystem ever depending on the map or combat internals.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Game/Combat.h"
#include "Game/Spells.h"
#include "Graphics/ParticleBatch.h"

#include <functional>
#include <random>
#include <span>
#include <vector>

namespace dungeon::game {

struct Character;
class Catalog;

class MagicSystem {
public:
	MagicSystem();

	// (Re)builds the recipe table from the project's spells catalog. Call once.
	void LoadSpells(const Catalog& spells);
	bool HasRecipes() const { return !m_spellBook.Empty(); }

	// The outcome of a cast attempt; the owner turns it into a log line.
	enum class CastOutcome { Cast, Unknown, NoRecipe, NoMana };
	struct CastReport {
		CastOutcome outcome = CastOutcome::NoRecipe;
		const SpellDef* spell = nullptr; // the matched recipe (set on Cast)
	};

	// Resolves a cast for `caster` from the symbol `sequence`. The caster must
	// know every symbol, the sequence must match a recipe, and the caster must
	// have the mana. On success it deducts the caster's mana and spawns a bolt
	// flying `dir` from `origin` (the party eye), and returns {Cast, spell}.
	// Otherwise nothing is spawned and the outcome explains why.
	CastReport Cast(Character& caster, std::span<const SpellSymbol> sequence,
					const Vec3& origin, const Vec3& dir);

	// Advances live projectiles (flight + impact/fizzle via the hooks) and ages
	// the impact sparks. Call once per frame.
	void Update(float dt);

	// Appends the live bolt + spark billboards (premultiplied additive) to the
	// particle list the renderer draws after the opaque scene.
	void AppendBillboards(std::vector<gfx::ParticleInstance>& out) const;

	// Drops all live effects (new game / level change).
	void Clear() {
		m_projectiles.clear();
		m_sparks.clear();
	}

	// --- world seam (wired once by the owner) -------------------------------
	// True if a bolt is stopped by the cell at world position `p` (wall / off-map).
	std::function<bool(const Vec3& p)> isBlocked;
	// A bolt reached `p`; resolve a strike there with `atk`. Return true if it
	// struck a target (the bolt is consumed). The owner does combat + feedback.
	std::function<bool(const Vec3& p, const AttackProfile& atk)> resolveHit;
	// A bolt died on a wall / at max range at `p` (for a soft fizzle sound).
	std::function<void(const Vec3& p)> onFizzle;

private:
	// A travelling spell bolt — a cast's projectile effect. Flies its direction
	// at `speed`, carries the strike profile applied on a monster hit, and draws
	// as a glowing element-coloured billboard. Transient: never saved.
	struct Projectile {
		Vec3 pos{};
		Vec3 dir{};             // unit travel direction (horizontal)
		float speed = 7.0f;     // m/s
		float rangeLeft = 8.0f; // metres remaining before it fizzles
		AttackProfile atk{};    // damage + accuracy applied on a hit
		Vec4 color{1, 1, 1, 1}; // element glow (premultiplied additive)
		float size = 0.2f;      // billboard half-extent
	};
	// A short-lived impact/fizzle spark (a burst of these sells a hit). Flies
	// out, fades over its life, additive.
	struct Spark {
		Vec3 pos{};
		Vec3 vel{};
		Vec4 color{1, 1, 1, 1};
		float age = 0.0f;
		float life = 0.35f;
		float size = 0.1f;
	};

	void SpawnSparkBurst(const Vec3& pos, const Vec4& color, int count);

	SpellBook m_spellBook;
	std::vector<Projectile> m_projectiles;
	std::vector<Spark> m_sparks;
	std::mt19937 m_rng{0x5EED1234u}; // spark scatter (cosmetic; not the combat RNG)
};

} // namespace dungeon::game
