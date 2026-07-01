// ============================================================================
// Game/Magic.h — the magic system: casting (recipe + mana resolution).
//
// This is the one runtime home for magic. Spells.h is the static DATA layer (the
// SpellSymbol alphabet a Character memorizes + the SpellBook recipe table);
// MagicSystem turns a memorized symbol sequence into a cast: it matches the
// recipe, checks/deducts mana, and EMITS a projectile spec (a spell bolt) for
// the caller to spawn. It does not fly or draw the bolt — that lives in the
// shared moving-item engine (Projectiles.h), which a party spell and a monster's
// ranged attack both feed. So MagicSystem depends on no map/monster/HUD/audio.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Game/Projectiles.h"
#include "Game/Spells.h"

#include <span>

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
		const SpellDef* spell = nullptr;  // the matched recipe (set on Cast)
		ProjectileSpec projectile{};      // the bolt to spawn (valid only on Cast)
	};

	// Resolves a cast for `caster` from the symbol `sequence`. The caster must
	// know every symbol, the sequence must match a recipe, and the caster must
	// have the mana. On success it deducts the caster's mana and returns {Cast,
	// spell, projectile} — a bolt spec flying `dir` from `origin` (the party eye)
	// for the owner to Spawn into the moving-item engine. Otherwise nothing is
	// deducted and the outcome explains why.
	CastReport Cast(Character& caster, std::span<const SpellSymbol> sequence,
					const Vec3& origin, const Vec3& dir);

private:
	SpellBook m_spellBook;
};

} // namespace dungeon::game
