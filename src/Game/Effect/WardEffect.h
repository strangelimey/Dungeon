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

class StoneskinEffect : public WardEffect {
public:
	StoneskinEffect();
};

class FireshieldEffect : public WardEffect {
public:
	FireshieldEffect();
};

class WaterveilEffect : public WardEffect {
public:
	WaterveilEffect();
};

class WindwardEffect : public WardEffect {
public:
	WindwardEffect();
};

} // namespace dungeon::game::fx
