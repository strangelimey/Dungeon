// ============================================================================
// Game/Spell/ModifiedSpell.h - a third-tier spell: a form spell + a MODIFIER.
//
// The modifier runes (Ingwaz = Multiple, Hagalaz = Explode; spell-updates) are
// SHARED like the form runes, and they change how a form spell lands rather
// than what it is - so one class does it for every school, instead of sixteen:
//
//   Ingwaz  + a bolt (Project): a VOLLEY - several bolts down the caster's own
//           lane, a beat apart, each a little off the line; more with power
//           (`count`, `count_per_power`, `count_max`), each at `share` of the
//           power (Michael: each weaker, 60% by default).
//   Hagalaz + a bolt: the bolt BURSTS on impact (`blast_*`), its reach and its
//           damage growing with power (`blast_force_per_power`; the damage
//           scales as power against the spell's base power).
//   Ingwaz  + a ward (Protect): the ward on EVERY standing member, at `share`.
//   Hagalaz + a ward: the ward's power spent as a BURST of its element round
//           the caster, whose own square is spared - and no ward is left
//           (Michael: "burst instead").
//
// A modified spell is a whole spell in the registry (AllSpells.cpp makes one
// per form spell per modifier): its own id (`<base>_volley`, `_burst`,
// `_party`), its own name, its own spells.cat entry for the knobs above and its
// mana - so learning it, the spellbook, the hand menus and saves need nothing
// new. The FORM spell it modifies is borrowed (the registry owns both).
// ============================================================================
#pragma once

#include "Game/Spell/Spell.h"

namespace dungeon::game {

class BoltSpell;
class WardSpell;

class ModifiedSpell : public Spell {
public:
	// `base` must be a BoltSpell or a WardSpell (the forms a modifier changes);
	// `modifier` Multiple or Explode.
	ModifiedSpell(const Spell& base, SpellSymbol modifier);

	void Cast(CastContext& ctx) const override;
	std::optional<ProjectileSpec> MonsterBolt(const Vec3& origin, const Vec3& dir,
											  float accuracy) const override;
	int MonsterVolley() const override;
	void ApplyOverrides(const CatalogEntry& e) override;

	// The id a form spell gets with a modifier: firebolt + Explode =
	// "firebolt_burst" (the one place the convention lives).
	static std::string IdFor(const Spell& base, SpellSymbol modifier);

private:
	bool IsVolley() const { return m_bolt && m_modifier == SpellSymbol::Multiple; }
	// How many bolts a volley of `power` flies.
	int VolleyCount(float power) const;
	// The blast rules scaled to `power` (the authored rules at base power).
	BlastSpec ScaledBlast(float power) const;

	const BoltSpell* m_bolt = nullptr;
	const WardSpell* m_ward = nullptr;
	SpellSymbol m_modifier;
	// Ingwaz: bolts / members, and each one's share of the power.
	int m_count = 2;
	float m_countPerPower = 10.0f; // one more bolt per this much power
	int m_countMax = 5;
	float m_share = 0.6f;
	float m_gap = 0.18f;    // seconds between a volley's bolts
	float m_jitter = 0.12f; // metres a volley bolt may sit off the lane's line
	// Hagalaz: one more square of reach per this much power past the base.
	float m_forcePerPower = 12.0f;
};

} // namespace dungeon::game
