// ============================================================================
// Game/Magic.h — the magic system: casting (recipe + mana + skill resolution).
//
// This is the one runtime home for magic. Spells.h is the SYMBOL alphabet +
// the SpellBook registry; the Spell classes (Game/Spell/, one file pair per
// spell) carry each spell's recipe, numbers, and — in their Cast() override —
// its behaviour. MagicSystem runs the COMMON gates a cast passes through
// (vocabulary, mana, the skill/fumble roll, power scaling) and then hands the
// landing to the matched spell: spell->Cast(ctx), where ctx carries the
// CastServices the owner wired once (spawn a bolt, log a member line). So
// MagicSystem depends on no map/monster/HUD/audio — like its projectile
// hooks, the services keep the module walled off.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Game/Spell/Spell.h"
#include "Game/Spells.h"

#include <random>
#include <span>

namespace dungeon::game {

struct Balance;
struct Character;
class Catalog;

class MagicSystem {
public:
	MagicSystem();

	// (Re)builds the spell registry (classes + the catalog's numeric
	// overrides). Call once.
	void LoadSpells(const Catalog& spells);
	bool HasRecipes() const { return !m_spellBook.Empty(); }

	// What a successful Cast() may DO to the world — wired once by the owner
	// (DungeonWorld ctor), handed to every spell's Cast().
	void SetCastServices(CastServices services) {
		m_services = std::move(services);
	}
	// The attack-formula tuning (Balance.h) — Cast reads the spell_stat knob
	// (school-stat power bonus). Wired by the owner; null = no bonus.
	void SetBalance(const Balance* balance) { m_balance = balance; }

	// The spell with catalog id `id`, or null. Lets the host reference a spell
	// for a monster CASTER (archetype = caster) by name — its bolt comes from
	// Spell::MonsterBolt — without the party cast path (no caster/mana/vocab).
	const Spell* FindSpell(std::string_view id) const {
		return m_spellBook.Find(id);
	}
	// Read access to the whole registry (enumeration for casting UIs).
	const SpellBook& Book() const { return m_spellBook; }

	// The outcome of a cast attempt; the owner turns it into a log line.
	// Fumble = the skill-gated failure (docs/skills.md): the recipe matched and
	// the mana is SPENT, but the casting slipped — no effect, no learning.
	enum class CastOutcome { Cast, Unknown, NoRecipe, NoMana, Fumble };
	struct CastReport {
		CastOutcome outcome = CastOutcome::NoRecipe;
		const Spell* spell = nullptr; // the matched spell (set on Cast)
	};

	// Resolves a cast for `caster` from the symbol `sequence`. The caster must
	// know every symbol, the sequence must match a spell, and the caster must
	// have the mana; then the skill roll (school level + willpower vs the
	// recipe's rune count, rolled on `rng`) may still Fumble it — mana spent,
	// nothing else. On success the spell's own Cast() lands the effect through
	// the wired services (a bolt spawns, a ward settles, ...) at skill-scaled
	// power. On the non-Fumble failures nothing is deducted and the outcome
	// explains why. `casterIndex` is the caster's roster index (-1 unknown) —
	// it rides any spawned bolt so the impact credits its caster (threat).
	CastReport Cast(Character& caster, int casterIndex,
					std::span<const SpellSymbol> sequence, const Vec3& origin,
					const Vec3& dir, std::mt19937& rng);

private:
	SpellBook m_spellBook;
	CastServices m_services;
	const Balance* m_balance = nullptr; // owner-wired tuning (spell_stat)
};

} // namespace dungeon::game
