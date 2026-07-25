// ============================================================================
// Game/Effect/WardEffect.cpp — see WardEffect.h.
//
// The four guards used to live at four hard-coded sites nowhere near each
// other: the air deflect inside the monster-projectile resolver, the water
// absorb at the top of WoundMember, the earth resist inside PartyDefense, and
// the fire retaliation halfway down MonsterAttack. Adding a fifth ward meant
// finding a sixth site. They are four hooks now, and each one names the stage
// it acts at.
// ============================================================================
#include "Game/Effect/WardEffect.h"

#include "Core/Loc.h"

#include <algorithm>

namespace dungeon::game::fx {

WardEffect::WardEffect(std::string id, std::string nameKey)
	: EffectKind(std::move(id), Category::Ward, std::move(nameKey),
				 Stacking::Refresh) {
	// A ward wears the Protect rune tablet's face in the HUD strip; the school
	// tint around it tells the four apart (effects.cat can override).
	m_iconItem = "rune_protect";
}

// --- earth: harden ------------------------------------------------------------

StoneskinEffect::StoneskinEffect() : WardEffect("stoneskin", "spell.stoneskin") {}

float StoneskinEffect::ResistFor(const Inst& inst, DamageType type,
								 const Knobs& knobs) const {
	return IsPhysical(type) ? inst.magnitude * knobs.stoneskinResist : 0.0f;
}

// --- fire: burn back ----------------------------------------------------------

FireshieldEffect::FireshieldEffect() : WardEffect("fireshield", "spell.fireshield") {}

void FireshieldEffect::OnStruck(Inst& inst, const DamageEvent& ev, ITarget& self,
								ITarget* attacker) const {
	// Only a swung blow gets burned — you cannot scorch a bolt or a poison.
	if (ev.delivery != Delivery::Melee || !attacker) return;
	const float burn = inst.magnitude;
	self.Say(loc::Format("log.shield_burns", attacker->Name(),
						 static_cast<int>(burn + 0.5f)));
	// The reprisal is the ward-BEARER's damage, so it feeds their threat: it
	// carries the event's own source (the member the blow landed on). It goes
	// straight to the apply stage — a reprisal is not itself deflectable or
	// soakable — which also means it cannot recurse into another reaction.
	DamageEvent back = DamageEvent::Impact(DamageType::Fire, burn, ev.source);
	attacker->Wound(burn, back);
	// Nothing else narrates a death here, so the ward does: through the
	// ATTACKER's own channel, which is the plain world log a monster's death
	// has always used.
	if (back.slew) attacker->Say(loc::Format("log.monster_slain", attacker->Name()));
}

// --- water: absorb ------------------------------------------------------------

WaterveilEffect::WaterveilEffect() : WardEffect("waterveil", "spell.waterveil") {}

void WaterveilEffect::OnAbsorb(Inst& inst, float& remaining,
							   const DamageEvent& ev, ITarget& self) const {
	if (remaining <= 0.0f || inst.magnitude <= 0.0f) return;
	const float soaked = std::min(inst.magnitude, remaining);
	inst.magnitude -= soaked;
	remaining -= soaked;
	if (!ev.Quiet()) self.Say(loc::Format("log.shield_soaks", self.Name()));
	if (inst.magnitude > 0.0f) return;
	// Spent — it BURSTS rather than fading, so it says its own line and zeroes
	// its timer; Deal drops it before the aging tick can add a fade line.
	self.Say(loc::Format("log.shield_bursts", self.Name()));
	inst.timeLeft = 0.0f;
}

// --- air: deflect -------------------------------------------------------------

WindwardEffect::WindwardEffect() : WardEffect("windward", "spell.windward") {}

void WindwardEffect::OnDeflect(Inst& inst, DamageEvent& ev, ITarget& self) const {
	if (ev.delivery != Delivery::Ranged) return; // it turns bolts, not blows
	inst.magnitude -= 1.0f;                      // one charge per deflection
	ev.deflected = true;
	self.Say(loc::Format("log.shield_deflects", self.Name()));
	if (inst.magnitude > 0.0f) return;
	self.Say(loc::Format("log.shield_stills", self.Name())); // the wind stills
	inst.timeLeft = 0.0f;
}

} // namespace dungeon::game::fx
