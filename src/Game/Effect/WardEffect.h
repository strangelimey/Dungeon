// ============================================================================
// Game/Effect/WardEffect.h — the four Protect shields, one class each.
//
// A ward is what the Protect form rune leaves on its caster (on the whole
// party, cast with Ingwaz), and the SCHOOL picks which of four quite different
// guards you get. They live together in one file pair because they share their
// category, a name taken from their spell and the school-tinted Protect rune
// icon, and differ only in the pipeline hook each overrides (Effect.h):
//
//   Stoneskin  (earth) - HARDENS: contributes physical resist   -> ResistFor
//   Fireshield (fire)  - BURNS BACK: scorches a melee attacker  -> OnStruck
//   Waterveil  (water) - ABSORBS: a pool it spends soaking      -> OnAbsorb
//   Windward   (air)   - DEFLECTS: charges it spends on bolts   -> OnDeflect
//
// That hook IS the whole behaviour: no damage site names a ward. Wards stack
// ACROSS schools (all four at once) - that falls out of them being four kinds,
// each refreshing only itself.
// ============================================================================
#pragma once

#include "Game/Effect/Effect.h"

namespace dungeon::game::fx {

// The shared half: category, the "wears the Protect rune's face" icon, the
// name taken from the spell that casts it (so the HUD keeps reading "Stone
// Skin", and the sheet finds its existing spell.<id>.desc long form), the
// ward's fade line, and its SCHOOL - which a hand-applied ward (the `effect`
// command, a monster's) lands with, as DotEffect's kinds do. Unset, every ward
// fell back to fire and `effect stoneskin ahead` laid a fire-flavoured stone
// skin (code-review C279).
class WardEffect : public EffectKind {
public:
	WardEffect(std::string id, std::string nameKey, SpellSymbol school);
};

// Earth HARDENS: the ward's magnitude becomes PHYSICAL resist at the
// stoneskin_resist knob. Elemental bolts pass it by — earth guards against
// blades and clubs, not fire.
class StoneskinEffect : public WardEffect {
public:
	StoneskinEffect();
	float ResistFor(const Inst& inst, DamageType type,
					const Knobs& knobs) const override;
};

// Fire BURNS BACK: a monster that lands a MELEE blow on the bearer is scorched
// for the ward's magnitude. The blow itself is not reduced (earth is the
// school that hardens), and the ward outlives its bearer's last stand by
// exactly one burn — it fires even when the blow downs them.
class FireshieldEffect : public WardEffect {
public:
	FireshieldEffect();
	void OnStruck(Inst& inst, const DamageEvent& ev, ITarget& self,
				  ITarget* attacker, const ReactCtx& ctx) const override;
};

// Water ABSORBS: magnitude is a POOL, spent soaking damage before any reaches
// health, and the ward BURSTS when the pool runs out — unlike the timed wards
// it dies by spending. It sits in the absorb stage, so it soaks every source
// alike: melee, ranged, a wall bump, even a poison tick (silently — a
// per-frame tick must not spam the log).
class WaterveilEffect : public WardEffect {
public:
	WaterveilEffect();
	void OnAbsorb(Inst& inst, float& remaining, const DamageEvent& ev,
				  ITarget& self) const override;
};

// Air DEFLECTS: a bolt aimed at the bearer is turned aside outright — no
// strike roll — spending one of the ward's charges (its magnitude); the last
// deflection stills the wind. Blows are not deflected, and bolts aimed at
// unwarded neighbours fly true: the ward wraps its bearer alone.
class WindwardEffect : public WardEffect {
public:
	WindwardEffect();
	void OnDeflect(Inst& inst, DamageEvent& ev, ITarget& self) const override;
};

} // namespace dungeon::game::fx
