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
	// "Physical" is a FIELD on the damage type now, not the old `t <= Bash`
	// ordering trick — so a project can add a physical type and Stone Skin
	// hardens against it with no code change.
	const bool physical = m_types && m_types->IsPhysical(type);
	return physical ? inst.magnitude * knobs.stoneskinResist : 0.0f;
}

// --- fire: burn back ----------------------------------------------------------

FireshieldEffect::FireshieldEffect() : WardEffect("fireshield", "spell.fireshield") {}

void FireshieldEffect::OnStruck(Inst& inst, const DamageEvent& ev, ITarget& self,
								ITarget* attacker, const ReactCtx& ctx) const {
	// Only a swung blow gets burned — you cannot scorch a bolt or a poison.
	if (ev.delivery != Delivery::Melee || !attacker) return;
	// A BURST through the ORDINARY pipeline: fire that armour cannot blunt but
	// fire-resistance answers, so a salamander shrugs off the shield and a dry
	// mummy suffers for touching it. It goes through Deal like every other
	// source of damage rather than doing its own arithmetic — which is what
	// earns it the whole model for free: an attacker's own water veil eats the
	// reprisal, and one that DRINKS fire (a nature resist past 1) is fed by the
	// shield instead of hurt by it. Deal never calls React, so this cannot
	// cascade into a counter-reprisal.
	//
	// The reprisal is the ward-BEARER's damage, so it feeds their threat: it
	// carries the struck event's own source (the member the blow landed on).
	// The shield's own school decides what it burns AS — asked of the book, so
	// a project that retypes fire (or gives the fire school a different damage
	// type entirely) gets a shield that agrees with its own spells.
	const DamageType burnType =
		m_types ? m_types->ForSchool(SpellSymbol::Fire) : DamageType{};
	DamageEvent back = DamageEvent::Burst(burnType, inst.magnitude, ev.source);
	Deal(back, *attacker, ctx.rules, ctx.rng);
	// Nothing came of it (immune): no line. Being FED says so through the
	// attacker's own Absorb, so only a real burn reports a number here.
	if (back.dealt <= 0.0f) return;
	self.Say(loc::FormatLine("log.shield_burns", attacker->Name(),
							 static_cast<int>(back.dealt + 0.5f)));
	// Nothing else narrates a death here, so the ward does: through the
	// ATTACKER's own channel, which is the plain world log a monster's death
	// has always used.
	if (back.slew) attacker->Say(loc::FormatLine("log.monster_slain", attacker->Name()));
}

// --- water: absorb ------------------------------------------------------------

WaterveilEffect::WaterveilEffect() : WardEffect("waterveil", "spell.waterveil") {}

void WaterveilEffect::OnAbsorb(Inst& inst, float& remaining,
							   const DamageEvent& ev, ITarget& self) const {
	if (remaining <= 0.0f || inst.magnitude <= 0.0f) return;
	const float soaked = std::min(inst.magnitude, remaining);
	inst.magnitude -= soaked;
	remaining -= soaked;
	if (!ev.Quiet()) self.Say(loc::FormatLine("log.shield_soaks", self.Name()));
	if (inst.magnitude > 0.0f) return;
	// Spent — it BURSTS rather than fading, so it says its own line and zeroes
	// its timer; Deal drops it before the aging tick can add a fade line.
	self.Say(loc::FormatLine("log.shield_bursts", self.Name()));
	inst.timeLeft = 0.0f;
}

// --- air: deflect -------------------------------------------------------------

WindwardEffect::WindwardEffect() : WardEffect("windward", "spell.windward") {}

void WindwardEffect::OnDeflect(Inst& inst, DamageEvent& ev, ITarget& self) const {
	if (ev.delivery != Delivery::Ranged) return; // it turns bolts, not blows
	inst.magnitude -= 1.0f;                      // one charge per deflection
	ev.deflected = true;
	self.Say(loc::FormatLine("log.shield_deflects", self.Name()));
	if (inst.magnitude > 0.0f) return;
	self.Say(loc::FormatLine("log.shield_stills", self.Name())); // the wind stills
	inst.timeLeft = 0.0f;
}

} // namespace dungeon::game::fx
