// ============================================================================
// Game/Effect/WardEffect.h — the four Protect shields, one class each.
//
// A ward is what the Protect form rune leaves on its caster, and the SCHOOL
// picks which of four quite different guards you get. They live together in
// one file pair because they share everything P1 cares about (category, name
// from their spell, the school-tinted Protect rune icon) and differ only in
// the hook each will override in P2:
//
//   Stoneskin  (earth) — HARDENS: contributes physical resist   -> mitigate
//   Fireshield (fire)  — BURNS BACK: scorches a melee attacker  -> react
//   Waterveil  (water) — ABSORBS: a pool it spends soaking      -> absorb
//   Windward   (air)   — DEFLECTS: charges it spends on bolts   -> deflect
//
// Until P2 those behaviours still live at their old sites; what these classes
// carry today is identity. Wards stack ACROSS schools (all four at once) —
// that falls out of them being four kinds, each refreshing only itself.
// ============================================================================
#pragma once

#include "Game/Effect/Effect.h"

namespace dungeon::game::fx {

// The shared half: category, the "wears the Protect rune's face" icon, and the
// name taken from the spell that casts it (so the HUD keeps reading "Stone
// Skin", and the sheet finds its existing spell.<id>.desc long form).
class WardEffect : public EffectKind {
public:
	WardEffect(std::string id, std::string nameKey);
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
