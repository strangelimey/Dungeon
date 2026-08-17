// ============================================================================
// Game/DungeonWorld_Combat.cpp — split out of DungeonWorld.cpp to keep files
// small (see DungeonWorld.h). Holds monster action execution + the combat
// model: formation-step helpers, wound/skill/stamina, the attack formula
// (party + monster melee/ranged), spell casting, and projectile-hit resolution.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Game/Blast.h"
#include "Game/Curve.h"
#include "Game/Defense.h"

#include "Core/Loc.h"

#include <algorithm>
#include <array>
#include <cmath>

using namespace DirectX;

namespace dungeon::game {

namespace {
// How long a hit-feedback splat stays over a struck member's portrait.
constexpr float kHitFlashSeconds = 0.7f;
} // namespace
void DungeonWorld::MemberMessage(const Character& member,
								 const std::string& line) const {
	if (onMemberMessage) onMemberMessage(line, member.portraitColor);
	else onMessage(line);
}

// The party half of the apply stage. Absorption happens BEFORE this now — the
// water veil is an absorb-stage hook (Effect/WardEffect.cpp), not a special
// case at the top of the one function that could hurt a member — so what is
// left here is purely "put the damage into hit points and deal with falling".
DungeonWorld::Fall DungeonWorld::WoundMember(Character& target, float damage,
											 bool quiet) {
	// Death needs deliberate OVERKILL (docs/combat.md Phase 5): a hit landing
	// on a member ALREADY down — a poison tick against the unconscious counts
	// — or a single blow past the overkill knob. Anything less leaves them
	// unconscious, stabilizing back up once the danger passes (the tick in
	// UpdateMonsters).
	const bool wasDown = !target.IsAlive();
	target.health -= damage;
	if (target.health < 0.0f) target.health = 0.0f;
	if (!quiet) {
		target.hitFlash = kHitFlashSeconds;
		target.hitSeverity = damage < 5.0f ? 0 : (damage < 10.0f ? 1 : 2);
		// A BLOW BREAKS REST, and `quiet` is exactly the right line to draw it
		// on: it is set for a Tick, so a poison or a burn does NOT wake the
		// party — you can rest through those, and they simply cost you — while
		// anything swung, shot or dropped on you does. Being hit while the world
		// runs at 60x is how a rest becomes a wipe with no input, which is the
		// one thing a state with no duration has to defend against.
		BreakRest("attacked", "log.rest_attacked");
	}
	if (target.IsAlive()) return Fall::None;
	target.stabilize = 0.0f; // the wound that downed them restarts the clock
	if (!target.dead &&
		(wasDown || damage >= m_balance.overkill * target.maxHealth)) {
		target.dead = true;
		return Fall::Dead;
	}
	return !target.dead && !wasDown ? Fall::Down : Fall::None;
}

// The one place skills grow (docs/skills.md). Levels derive from raw XP
// (floor(sqrt)), so a level-up is just "the derived number changed"; the
// SOURCE's associated stats creep behind at the creep_rate knob, the gain
// SPLIT EVENLY across them (docs/combat.md part 2 — a sword doesn't train
// stats twice as fast as a club), through the member's statProgress pools.
void DungeonWorld::GrantSkillXp(Character& member, std::string_view skillId,
								float xp, std::span<const std::string> stats) {
	if (skillId.empty() || xp <= 0.0f || !member.IsAlive()) return;

	// Look the skill up before inserting it. The subscript alone would build a
	// std::string on EVERY award, which was harmless while every award was an
	// event (a landed blow, a cast) and is not any more: the resource practices
	// train off regeneration, so constitution is awarded every frame a member is
	// healing. `skillXp` has a transparent comparator precisely so a string_view
	// can query it without allocating — only the first award of a given skill
	// ever reaches the insert. (docs/ARCHITECTURE.md "Memory strategy".)
	const auto it = member.skillXp.find(skillId);
	float& total = it != member.skillXp.end() ? it->second
											  : member.skillXp[std::string(skillId)];
	const int before = Character::LevelForXp(total);
	total += xp;
	const int after = Character::LevelForXp(total);
	if (after > before)
		MemberMessage(member,
					  loc::Format("log.skill_up", member.name,
								  loc::Tr("skill." + std::string(skillId)), after));

	if (stats.empty()) return; // an unclassed source trains no stat
	const float gain =
		xp * m_balance.creepRate / static_cast<float>(stats.size());
	for (const std::string& stat : stats) {
		float& pool = member.statProgress[stat];
		pool += gain;
		if (pool < 1.0f) continue;
		pool -= 1.0f;
		GrantStatPoint(member, stat);
	}
}

// THE DEFENSIVE FEEDBACK LOOPS (docs/damage-system.md). You learn what you
// actually survive, so the two loops train on opposite outcomes and never both
// fire for the same blow:
//
//   a MISS while unarmored  -> `avoid`, creeping DEX. Only unarmored: armor is
//                              the other answer to being swung at, and a
//                              member cannot be practising both.
//   a HIT that armor blunted -> that class's skill, creeping its stat (DEX for
//                              light and medium, STR for heavy). Trained by
//                              being struck, which is how you learn to live in
//                              a suit of plate.
//
// Only a ROLLED blow trains anything: a bump, a fall or a poison tick was never
// evaded and never turned, so counting them would train a skill that did no
// work. Call this once per resolved attack against a member.
void DungeonWorld::TrainDefense(Character& member, const fx::DamageEvent& ev) {
	if (!member.IsAlive()) return;
	// An early-out, not the rule: LessonFrom is still the authority on `rolled`,
	// but returning here saves an unrolled event (a DoT tick, a bump, a fall)
	// walking the equipment twice to be told it teaches nothing.
	if (!ev.rolled) return;
	const ArmorClass worn = WornArmorClass(member);
	constexpr float kXp = 1.0f; // the same unit a landed blow trains at

	// WHICH loop this blow feeds is defense::LessonFrom's decision — the two
	// train on opposite outcomes, and that rule is measured rather than reread
	// here. This function only pays out whatever it is told.
	static const std::vector<std::string> kDexStat{"dexterity"};
	static const std::vector<std::string> kStrStat{"strength"};
	switch (defense::LessonFrom(ev.rolled, ev.hit, worn,
								PartyTarget{*this, member}.Soak())) {
	case defense::Lesson::Nothing:
		return;
	case defense::Lesson::Avoid:
		GrantSkillXp(member, kAvoidSkill, kXp, kDexStat);
		return;
	case defense::Lesson::Armor:
		GrantSkillXp(member, ArmorSkillId(worn),
					 kXp * m_balance.Armor(worn).learn,
					 worn == ArmorClass::Heavy ? kStrStat : kDexStat);
		return;
	}
}

// The three resource practices (see the declaration). Throughput in, XP out,
// and NO stat creep — the empty stat span below is the entire mechanism that
// keeps the model from feeding itself, so it is deliberately not a defaulted
// parameter on GrantSkillXp: an omission would read as a defensible oversight,
// whereas passing `{}` here is a statement.
//
// A resource skill grows the pool it practises, so a level-up must re-derive
// the maxima exactly as a stat point does. GrantSkillXp only logs the level —
// it has no reason to know a skill might be a resource one — so the re-derive
// happens here, and only when the level actually moved.
void DungeonWorld::GrantResourceXp(Character& member, resource::Kind kind,
								   float points) {
	if (points <= 0.0f || !member.IsAlive()) return;
	float rate = 0.0f;
	switch (kind) {
	case resource::Kind::Health: rate = m_balance.constitutionXp; break;
	case resource::Kind::Stamina: rate = m_balance.conditioningXp; break;
	case resource::Kind::Mana: rate = m_balance.attunementXp; break;
	default: return;
	}
	if (rate <= 0.0f) return; // a zeroed knob switches the practice off entirely
	const char* skill = resource::SkillId(kind);
	const int before = member.SkillLevel(skill);
	GrantSkillXp(member, skill, points * rate, {});
	if (member.SkillLevel(skill) == before) return;
	// THE THIRD GROWTH ROUTE, and the one-pipeline check is what found it: a
	// resource PRACTICE levelling mid-fight grows the pool, and RecomputeMaxima
	// carries the growth onto the current value — so a member's health rises by
	// about a point in the middle of an exchange, with no DamageEvent anywhere
	// near it. It was reported as an unexplained +0.96 on Brand and Sera the
	// first time the pipeline suite ran, which is precisely the job: two of the
	// three growth routes had been enumerated by reading the code, and this one
	// had not.
	const ledger::Explained accounted{m_damageLedger, member.health,
									  ledger::Reason::Growth};
	member.RecomputeMaxima(m_balance.Resources());
	// CONDITIONING also drives the walking pace, and the party takes the
	// minimum — so a level here can change how fast everyone moves, and it has
	// to reach the Party now rather than at the next load.
	if (kind == resource::Kind::Stamina) ApplyPartyPace();
}

// --- rest (docs/health-and-healing.md "Rest is a STATE") ---------------------
// Entering rest does two things, and the second is the one that matters.
//
// THE AI GOES INTO LOCKSTEP, and this is not a convenience — it is what makes
// rest safe to build at all. `ai::AsyncDirector` paces its bucket workers in
// WALL-CLOCK milliseconds, so running the world 60x faster would give a monster
// 1/60th as many chances to think per simulated second. It would still MOVE and
// still SWING (execution is per-frame and cooldowns tick with dt) — it would
// simply be walking a stale path toward where the party used to be. That is the
// exact failure the eval harness already paid for once: a stale-but-plausible
// behaviour is far worse than a dead one, because nothing about it looks wrong.
//
// Lockstep runs each bucket's thinking inline off SIM time, so it thinks as
// often per simulated second at 60x as at 1x. The mode built to make the
// harness reproducible turns out to be the thing that makes a fast-forward
// honest — and the danger the design wants from "the world runs while you rest"
// is real rather than decorative.
//
// The PREVIOUS mode is remembered rather than assumed: an eval run may already
// be in lockstep, and rest must hand it back what it found.
void DungeonWorld::SetResting(bool on) {
	if (on == m_resting) return;
	if (on) {
		m_restLockstep = LockstepAI();
		SetLockstepAI(true);
	} else {
		SetLockstepAI(m_restLockstep);
	}
	m_resting = on;
	// "woken" is the default reason — a plain click on the button. A caller with
	// a better one (BreakRest) overwrites it immediately after.
	if (!on) m_restEndReason = "woken";
	onMessage(loc::Tr(on ? "log.rest_begin" : "log.rest_end"));
}

void DungeonWorld::BreakRest(const char* reason, const char* reasonKey) {
	if (!m_resting) return;
	SetResting(false);
	m_restEndReason = reason;
	onMessage(loc::Tr(reasonKey));
}

// The reasons rest ends WITHOUT the player saying so. There are only two, and
// both exist to stop the state quietly costing something.
void DungeonWorld::UpdateRest() {
	if (!m_resting || !m_roster) return;
	// 1. DEPRIVATION. Resting while a meter is empty spends health to pass time
	//    you are already losing health for — the one configuration where the
	//    state is purely harmful. You have to eat first.
	for (const Character& member : *m_roster)
		if (member.FindEffect("starving") || member.FindEffect("parched")) {
			BreakRest("hungry", "log.rest_hungry");
			return;
		}
	// 2. FULLY RECOVERED. Continuing past this point burns food and water for
	//    nothing at all, and the player cannot see the instant it stops paying.
	//
	//    "Recovered" has to include A MEMBER STILL ON THE FLOOR. An unconscious
	//    one is not full and never will be while they are down — they come round
	//    through the stabilize clock, which wants safe seconds — and waiting for
	//    that is one of the things resting is FOR. An earlier version counted
	//    only the standing, so a fight that left one member down and the others
	//    unhurt ended the rest after 0.02 seconds with everyone walking away and
	//    their friend face-down behind them. It reported "recovered", which is
	//    the part that would have made it hard to find.
	//
	//    The DEAD are excluded, and they are the reason this is not simply "is
	//    everyone full": death is permanent here, so a party carrying a corpse
	//    would rest until its supplies ran out.
	bool anyRecoverable = false, allFull = true;
	for (const Character& member : *m_roster) {
		if (member.dead) continue;
		anyRecoverable = true;
		if (!member.IsAlive() || member.health < member.maxHealth ||
			member.stamina < member.maxStamina || member.mana < member.maxMana)
			allFull = false;
	}
	if (anyRecoverable && allFull) BreakRest("recovered", "log.rest_done");
}

// --- supplies (docs/health-and-healing.md "Food and water") ------------------
// One frame of one member's two meters. THE EFFECTS DO THE DAMAGE, not this:
// all this does is decide whether the meter is empty and make the effect list
// agree with that. Everything downstream — resists, the ward stages, a Tick on
// a downed member being lethal under the overkill rule — is then the ordinary
// effects pipeline, unchanged and already tested.
//
// THE EFFECT IS THE METER'S SHADOW, and that is why `timeLeft` is topped up
// rather than the kind being given some notion of permanence. A permanent
// effect would need its own rule for what clears it, and the meter already IS
// that rule; keeping one truth means an eaten apple cannot leave a member
// starving, and a save cannot restore a starving member who is not hungry.
void DungeonWorld::TickSupplies(Character& member, float dt) {
	if (!member.IsAlive() || dt <= 0.0f) return;
	// The dead do not eat. The DOWNED do — they are unconscious, not gone, and
	// a party that leaves someone bleeding out on the floor for hours should
	// find them worse rather than perfectly preserved. (IsAlive already
	// excluded them above; this comment is here because the opposite reading is
	// the tempting one.)
	const float practice = member.PracticeLevel(resource::Kind::Stamina);
	for (const resource::Supply which :
		 {resource::Supply::Food, resource::Supply::Water}) {
		const resource::SupplyRules rules = m_balance.SupplyOf(which);
		float& level = member.SupplyLevel(which);
		level = std::clamp(level - resource::DrainPerSec(rules, practice) * dt,
						   0.0f, rules.max);

		const char* id = which == resource::Supply::Water ? "parched" : "starving";
		fx::Inst* held = member.FindEffect(id);
		if (level > 0.0f) {
			// Fed. The effect goes, and says so — a relief line, because the
			// moment you stop starving is worth as much of the player's
			// attention as the moment you start.
			if (held) {
				member.RemoveEffect(id);
				MemberMessage(member,
							  loc::Format(which == resource::Supply::Water
											  ? "log.no_longer_parched"
											  : "log.no_longer_starving",
										  member.name));
			}
			continue;
		}
		if (rules.starveDamage <= 0.0f) continue; // the meter is off
		const fx::EffectKind* kind = m_effects.Find(id);
		if (!kind) continue; // effects.cat dropped it; warned at load
		if (held) {
			// Hold it open. The duration is nominal — it exists so the ordinary
			// aging loop has something to age, and is refreshed faster than it
			// can ever run out.
			held->timeLeft = kDeprivationHold;
			held->magnitude = rules.starveDamage; // a live knob edit takes effect
			continue;
		}
		PartyTarget starved{*this, member};
		fx::Apply(member.effects, *kind, kind->DefaultSchool(),
				  rules.starveDamage, kDeprivationHold);
		starved.SayApplied(*kind);
	}
}

// Every exertion in the game pays this, because it hangs off SpendStamina — the
// one place a swing, a cast and a step all arrive. WATER COSTS MORE THAN FOOD
// (Michael's call): sweat is water, so a heavy fight in armour makes you
// thirsty rather than merely hungry, and the armour surcharge SpendStamina
// already applies is inside `points`, so a badly-fitted suit is paid for a
// third time here. That compounding is the intent.
void DungeonWorld::DrainSuppliesByExertion(Character& member, float points) {
	if (points <= 0.0f) return;
	for (const resource::Supply which :
		 {resource::Supply::Food, resource::Supply::Water}) {
		const resource::SupplyRules rules = m_balance.SupplyOf(which);
		float& level = member.SupplyLevel(which);
		level = std::clamp(level - points * rules.perExertion, 0.0f, rules.max);
	}
	// Deliberately NOT raising the starving/parched effect here even when this
	// empties a meter: TickSupplies runs every frame and owns that decision, so
	// there is exactly one place that can put the effect on. Two would drift.
}

// Eating and drinking are ONE operation with two verbs. An apple both feeds and
// waters a little, so a handler per verb would have had to duplicate the other
// half — and a stew or a waterskin is the same statement with the numbers moved.
// The verb is flavour; `nutrition`/`hydration` are the content.
resource::Refill DungeonWorld::ConsumeItem(Character& member,
										   const std::string& typeId) {
	resource::Refill got;
	const ItemKind& kind = ItemKindFor(typeId);
	const resource::SupplyRules foodRules = m_balance.SupplyOf(resource::Supply::Food);
	const resource::SupplyRules waterRules = m_balance.SupplyOf(resource::Supply::Water);
	// What it RESTORED, not what it was worth: a full member gains nothing from
	// an apple, and the caller needs to know that to refuse the action rather
	// than consume it for no effect.
	const float beforeFood = member.food, beforeWater = member.water;
	member.food = std::clamp(member.food + kind.nutrition, 0.0f, foodRules.max);
	member.water = std::clamp(member.water + kind.hydration, 0.0f, waterRules.max);
	got.food = member.food - beforeFood;
	got.water = member.water - beforeWater;
	// The starving/parched effects are NOT cleared here — TickSupplies sees the
	// refilled meter next frame and lifts them, with their relief lines, from
	// the one place that owns that transition.
	return got;
}

// A whole stat point lands: increment, log, and re-derive the resource maxima
// (the resource formula — a VIT point is FELT as a bigger health/stamina pool,
// and the growth carries the current value so it reads as growth, not damage).
void DungeonWorld::GrantStatPoint(Character& member, std::string_view stat) {
	int value = 0;
	if (stat == "strength") value = ++member.strength;
	else if (stat == "dexterity") value = ++member.dexterity;
	else if (stat == "vitality") value = ++member.vitality;
	else if (stat == "willpower") value = ++member.willpower;
	else if (stat == "intelligence") value = ++member.intelligence;
	else return; // unknown id — parse already warned
	// A stat point can land in the MIDDLE of a fight (vitality creeps off
	// exertion), and RecomputeMaxima carries the growth onto the current value —
	// so health genuinely ticks up here, outside the pipeline, while blows are
	// landing. It is the "odd number you will see and should not chase" from
	// docs/damage-system.md, now named instead of merely noted.
	const ledger::Explained accounted{m_damageLedger, member.health,
									  ledger::Reason::Growth};
	member.RecomputeMaxima(m_balance.Resources());
	MemberMessage(member, loc::Format("log.stat_up", member.name,
									  loc::Tr("stat." + std::string(stat)), value));
}

// Exertion (docs/combat.md part 3 + Phase 4): stamina is the exertion meter —
// every point spent feeds VIT's creep pool, holds regen off for a beat, and
// an emptied bar latches EXHAUSTED (the swing penalties; cleared with
// hysteresis in the regen tick). Swings and marching both arrive here.
// RETURNS THE SHORTFALL — how much of the bill the bar could not cover, AFTER
// the armor scaling below. Every ordinary caller ignores it and so keeps the old
// behaviour exactly (an emptied bar clamps at zero and latches EXHAUSTED, and
// that is all marching or an honest swing ever costs). Only over-exertion reads
// it, because only over-exertion may charge the remainder to health.
//
// The shortfall is deliberately measured HERE rather than split by the caller
// against the raw points: the armor scale is applied on the way out of the bar,
// so a caller-side split would drop the scaled excess on the floor instead of
// passing it on.
float DungeonWorld::SpendStamina(Character& member, float points) {
	if (points <= 0.0f || !member.IsAlive()) return 0.0f;
	// Armor you are too weak for is paid for twice: once on the defense roll
	// (ArmorPenalty) and again here, on every swing and every step. An
	// underpowered fighter in plate is easier to hit AND quickly spent.
	if (const ArmorClass worn = WornArmorClass(member); worn != ArmorClass::None) {
		const float short_ = m_balance.Armor(worn).strength -
							 static_cast<float>(member.strength);
		if (short_ > 0.0f)
			points *= 1.0f + short_ * m_balance.armorShortStamina;
	}
	member.stamina -= points;
	member.staminaHoldoff = m_balance.staminaHoldoff;
	float unpaid = 0.0f;
	if (member.stamina <= 0.0f) {
		unpaid = -member.stamina;
		member.stamina = 0.0f;
		if (!member.exhausted) {
			member.exhausted = true;
			MemberMessage(member, loc::Format("log.exhausted", member.name));
		}
	}
	// The WHOLE bill trains, the part paid in blood included — conditioning is
	// what the body did, not what the bar could afford.
	//
	// THIS REPLACED A VIT CREEP AND DID NOT JOIN IT (docs/health-and-healing.md).
	// The exertion used to drip vitality forward through statProgress, and
	// vitality drives max stamina — so spending stamina made the stamina pool
	// bigger, which is a loop feeding itself. Now the same throughput trains the
	// CONDITIONING skill, and that skill owns the pool instead. Leaving both in
	// would be precisely the double-dip the whole model exists to prevent, which
	// is why `vit_exertion` is gone rather than set to zero: a knob that must
	// stay at zero to keep the game correct is a trap with a dial on it.
	GrantResourceXp(member, resource::Kind::Stamina, points);
	// And it costs SUPPLIES — the same throughput, billed a third way. This is
	// where conditioning stops being free: a fitter member spends more stamina
	// over a day AND burns more food and water per point of it.
	DrainSuppliesByExertion(member, points);
	return unpaid;
}

// OVER-EXERTION'S BILL (docs/damage-system.md), charged once per swing or cast
// thrown from a stance past 1. `points` is what the over-exertion bought on the
// attack roll (defense::ExertionPoints); the cost is exert_cost times that, out
// of stamina first and out of HEALTH for whatever stamina could not cover.
//
// The health half goes through WoundMember like any other injury, so a fighter
// CAN put themselves down this way. But EXERTION SPENDS ONLY THE HEALTH YOU
// HAVE — the payment is capped at what is left in the bar, so it can empty it
// and never exceed it. That is what keeps it out of the overkill rule
// (docs/combat.md Phase 5): a shortfall past `overkill × maxHealth` would
// otherwise read as a definitive blow and kill outright, and at exert_max the
// bill genuinely can be several times a low-level member's whole health. You can
// collapse from over-exertion; you cannot burst.
//
// Narrated before the wound so the cause reads ahead of the effect it caused —
// the same ordering the reaction stage uses.
// ============================================================================
// Fumble consequences (docs/damage-system.md "When it goes wrong").
// ============================================================================

std::vector<mishap::Entry>
DungeonWorld::FumbleTable(const std::vector<mishap::Entry>& own,
						  bool severe) const {
	// An authored table REPLACES the default rather than adding to it — a table
	// you cannot turn off is not a table. So a weapon that authors only
	// `fumble` still gets the default SEVERE one, which is the common case: most
	// weapons want to say how they slip, not to redesign the disaster.
	if (!own.empty()) return own;
	return severe ? mishap::DefaultSevere()
				  : mishap::DefaultFumble(m_balance.fumbleRecover);
}

void DungeonWorld::DropItemInCell(const std::string& typeId, int cx, int cz) {
	ItemKind& kind = ItemKindFor(typeId);
	const Vec3 c = m_map.CellCenter(cx, cz);
	const int slot = FreeItemSlotNear(cx, cz, c.x, c.z, -1);
	// A RUNTIME drop (negative id), not an .ent record: a weapon knocked out of
	// a hand is dynamic state that rides the save, exactly like the cursor drop
	// beside it. Authoring a record would write it into the LEVEL.
	m_items.push_back({&kind, m_nextDropId--, cx, cz, false, slot});
	MarkSeen(cx, cz);
}

void DungeonWorld::PartyFumble(Character& attacker, size_t hand,
							   const ItemKind* weapon, const AttackProfile& atk,
							   int face) {
	if (!m_roster) return;
	const bool severe =
		mishap::Severe(face, static_cast<int>(m_balance.fumbleSevereFace + 0.5f));

	// The procs first — a blade that bites the hand holding it is an EFFECT, and
	// it lands on the attacker like any other. The striker is already an
	// fx::ITarget; nothing here is new machinery.
	PartyTarget self{*this, attacker};
	if (weapon && !weapon->onFumble.empty())
		fx::ApplyProcs(self, weapon->onFumble, std::nullopt,
					   /*source=*/-1, m_effects, m_combatRng);

	const auto run = [&](const std::vector<mishap::Entry>& table) {
		for (const mishap::Entry& e : table) switch (e.kind) {
			case mishap::Kind::Recover:
				// Off balance: the hand takes longer to come back. The one
				// consequence every fumble can always deliver.
				attacker.handCooldown[hand] *= std::max(1.0f, e.value);
				MemberMessage(attacker,
							  loc::Format("log.fumble_recover", attacker.name));
				break;
			case mishap::Kind::Stumble:
				// Billed as EXERTION, so it feeds VIT's creep and can reach
				// health on an empty bar like any other overspend.
				MemberMessage(attacker,
							  loc::Format("log.fumble_stumble", attacker.name));
				SpendExertion(attacker, e.value / std::max(0.01f, m_balance.exertCost));
				break;
			case mishap::Kind::Drop:
			case mishap::Kind::Fling: {
				// Nothing in the hand is nothing to lose — a bare fist fumbles
				// without disarming itself.
				ItemSlot& held = attacker.inventory.Hand(static_cast<int>(hand));
				if (held.Empty()) break;
				const std::string id = held.typeId;
				int cx = m_party.GridX(), cz = m_party.GridZ();
				if (e.kind == mishap::Kind::Fling) {
					// Somewhere adjacent and walkable, chosen from the cardinals
					// that qualify — never diagonally (the grid rule), and never
					// into stone, where it could not be picked up again.
					std::array<int, 4> dirs{0, 1, 2, 3};
					std::shuffle(dirs.begin(), dirs.end(), m_combatRng);
					for (const int d : dirs) {
						const int nx = cx + DirDX(static_cast<Direction>(d));
						const int nz = cz + DirDZ(static_cast<Direction>(d));
						if (m_map.IsWalkable(nx, nz)) { cx = nx; cz = nz; break; }
					}
				}
				held = ItemSlot{};
				DropItemInCell(id, cx, cz);
				MemberMessage(attacker,
							  loc::Format(e.kind == mishap::Kind::Fling
											  ? "log.fumble_fling"
											  : "log.fumble_drop",
										  attacker.name,
										  loc::Tr(ItemKindFor(id).nameKey)));
				break;
			}
			case mishap::Kind::SelfHit: {
				// The blow you just threw, landing on you at a fraction of its
				// force. Through the ONE pipeline like everything else, so your
				// own armour and resists answer it — and it is a Blow, so it can
				// itself be evaded, crit, or (yes) fumbled away.
				MemberMessage(attacker,
							  loc::Format("log.fumble_self", attacker.name));
				fx::DamageEvent ev = fx::DamageEvent::Blow(
					atk.type, atk.damage * std::max(0.0f, e.value),
					atk.attackBonus, -1);
				fx::Deal(ev, self, m_balance.Strike(), m_combatRng);
				self.NarrateFall();
				break;
			}
			case mishap::Kind::Wild: {
				// A wild swing catches whoever is standing beside you. The
				// QUADRANTS are the ranks (roster 0-1 front, 2-3 rear), so the
				// neighbour is the other member of your own rank — the one an
				// arm's length away, not the one behind you.
				const size_t me = static_cast<size_t>(&attacker - m_roster->data());
				const size_t beside = me ^ 1u; // 0<->1, 2<->3
				if (beside >= m_roster->size()) break;
				Character& victim = (*m_roster)[beside];
				if (!victim.IsAlive()) break;
				MemberMessage(attacker, loc::Format("log.fumble_wild",
													attacker.name, victim.name));
				PartyTarget hit{*this, victim};
				fx::DamageEvent ev = fx::DamageEvent::Blow(
					atk.type, atk.damage, atk.attackBonus, -1);
				fx::Deal(ev, hit, m_balance.Strike(), m_combatRng);
				hit.NarrateFall();
				break;
			}
			}
	};
	run(FumbleTable(weapon ? weapon->fumble : std::vector<mishap::Entry>{}, false));
	if (severe)
		run(FumbleTable(weapon ? weapon->fumbleSevere : std::vector<mishap::Entry>{},
						true));
}

void DungeonWorld::MonsterFumble(Monster& monster, const AttackProfile& atk,
								 int face) {
	const bool severe =
		mishap::Severe(face, static_cast<int>(m_balance.fumbleSevereFace + 0.5f));
	MonsterTarget self{*this, monster};
	if (!monster.kind->onFumble.empty())
		fx::ApplyProcs(self, monster.kind->onFumble, std::nullopt, -1, m_effects,
					   m_combatRng);

	const auto run = [&](const std::vector<mishap::Entry>& table) {
		for (const mishap::Entry& e : table) switch (e.kind) {
			case mishap::Kind::Recover:
				monster.attackCd *= std::max(1.0f, e.value);
				break;
			// A monster carries no inventory and no stamina bar, so three of the
			// six have nothing to act on. They are silent no-ops rather than
			// warnings: the DEFAULT severe table is `drop`, and every clawed
			// creature in the game shares it.
			case mishap::Kind::Stumble:
			case mishap::Kind::Drop:
			case mishap::Kind::Fling:
				break;
			case mishap::Kind::SelfHit: {
				fx::DamageEvent ev = fx::DamageEvent::Blow(
					atk.type, atk.damage * std::max(0.0f, e.value),
					atk.attackBonus, -1);
				fx::Deal(ev, self, m_balance.Strike(), m_combatRng);
				if (!monster.Alive())
					onMessage(loc::Format("log.monster_slain",
										  loc::Tr("monster." + monster.kind->name)));
				break;
			}
			case mishap::Kind::Wild: {
				// The nearest OTHER monster in the adjacent ring wears it. Same
				// mechanic as the party's, and it is the reason `wild` was worth
				// keeping on this side: a swarm hurting itself in a corridor is
				// the fumble a player most enjoys watching.
				Monster* beside = nullptr;
				for (Monster& m : m_monsters) {
					if (&m == &monster || !m.Alive()) continue;
					if (std::abs(m.x - monster.x) + std::abs(m.z - monster.z) != 1)
						continue;
					beside = &m;
					break;
				}
				if (!beside) break;
				MonsterTarget hit{*this, *beside};
				fx::DamageEvent ev = fx::DamageEvent::Blow(atk.type, atk.damage,
														   atk.attackBonus, -1);
				fx::Deal(ev, hit, m_balance.Strike(), m_combatRng);
				onMessage(loc::Format("log.fumble_wild_foe",
									  loc::Tr("monster." + monster.kind->name),
									  loc::Tr("monster." + beside->kind->name)));
				if (!beside->Alive())
					onMessage(loc::Format("log.monster_slain",
										  loc::Tr("monster." + beside->kind->name)));
				break;
			}
			}
	};
	run(FumbleTable(monster.kind->fumble, false));
	if (severe) run(FumbleTable(monster.kind->fumbleSevere, true));
}

void DungeonWorld::TickAutoAttack() {
	if (!m_harness.autoAttack || !m_roster) return;
	// NOTHING THERE, NOTHING SWUNG. A whiff at air costs the attack's pace and
	// its full stamina bill (PartyAttack pays both before it checks for a
	// target), so a party auto-swinging into an empty corridor would exhaust
	// itself before the fight and every number after it would be measuring an
	// exhausted party. A player clicking a hand slot at nothing gets that too —
	// but they stop, and this would not.
	const auto monsterAt = [&](int x, int z) {
		for (const Monster& mon : m_monsters)
			if (mon.Alive() && mon.x == x && mon.z == z) return true;
		return false;
	};
	const int px = m_party.GridX(), pz = m_party.GridZ();
	Direction faced = static_cast<Direction>(m_party.Facing());
	if (!monsterAt(px + DirDX(faced), pz + DirDZ(faced))) {
		// TURN TO THE THREAT. A player faces what is attacking them; a rung
		// should not have to PREDICT which side a monster will approach from,
		// and the first sweep measured twelve fights in which the party stared
		// at a wall while something chewed on them from behind. Cardinals only —
		// the grid rule (movement, reach and lanes are never diagonal).
		bool found = false;
		for (int d = 0; d < 4 && !found; ++d) {
			const auto dir = static_cast<Direction>(d);
			if (!monsterAt(px + DirDX(dir), pz + DirDZ(dir))) continue;
			faced = dir;
			m_party.SetFacing(static_cast<int>(dir));
			found = true;
		}
		// NOTHING ADJACENT, NOTHING SWUNG. A whiff at air costs the attack's
		// pace and its full stamina bill (PartyAttack pays both before it looks
		// for a target), so a party auto-swinging into an empty corridor would
		// arrive at the fight exhausted and every number after would describe an
		// exhausted party.
		if (!found) return;
	}

	// Every standing member, every hand that has come off cooldown. PartyAttack
	// owns the rules that decide whether a given swing is legal — the rear rank
	// needs a polearm, a hand still recovering does nothing — so this decides
	// only WHEN to ask, never whether.
	for (size_t m = 0; m < m_roster->size(); ++m) {
		const Character& member = (*m_roster)[m];
		if (!member.IsAlive()) continue;
		for (size_t hand = 0; hand < 2; ++hand)
			if (member.handCooldown[hand] <= 0.0f) PartyAttack(m, hand);
	}
}

void DungeonWorld::SpendExertion(Character& member, float points) {
	if (points <= 0.0f || !member.IsAlive()) return;
	const float unpaid = SpendStamina(member, points * m_balance.exertCost);
	if (unpaid <= 0.0f) return;
	MemberMessage(member, loc::Format("log.overexert", member.name));
	// A DECLARED EXCEPTION to the one-pipeline rule (Game/DamageLedger.h), and
	// Michael's call on 2026-08-15 when the check turned it up: this reaches
	// WoundMember directly rather than building a DamageEvent, so exhaustion is
	// not resisted, not soaked and not answered by a ward. Collapsing under your
	// own effort is not something armour turns. Named here rather than quietly
	// permitted, so the day it should become a Burst it is one edit and not a
	// discovery.
	const ledger::Explained accounted{m_damageLedger, member.health,
									  ledger::Reason::Exertion};
	if (WoundMember(member, std::min(unpaid, member.health)) == Fall::Down)
		MemberMessage(member, loc::Format("log.member_down", member.name));
}

void DungeonWorld::RecomputePartyMaxima() {
	if (!m_roster) return;
	const resource::PoolRules pools = m_balance.Resources();
	const float foodMax = m_balance.SupplyOf(resource::Supply::Food).max;
	const float waterMax = m_balance.SupplyOf(resource::Supply::Water).max;
	for (Character& member : *m_roster) {
		// Same growth rule as GrantStatPoint, reached the other way: a balance
		// apply or a load re-derives every maximum, and a raised ceiling carries
		// the current value up with it.
		const ledger::Explained accounted{m_damageLedger, member.health,
										  ledger::Reason::Growth};
		member.RecomputeMaxima(pools);
		// The supply ceilings are a knob, mirrored onto the member so the sheet
		// can draw a bar without reaching for Balance. Clamped with them, since
		// turning the knob DOWN in the editor must not leave someone over full.
		member.maxFood = foodMax;
		member.maxWater = waterMax;
		member.food = std::min(member.food, foodMax);
		member.water = std::min(member.water, waterMax);
	}
	// Conditioning feeds the PACE as well as the pool, and both are re-derived
	// from the same places (a load, a stat change, a balance apply), so they are
	// refreshed together rather than leaving one caller to remember the other.
	ApplyPartyPace();
}

// The party moves as fast as its SLOWEST member — the rule was already here;
// what is new is that the number it takes the minimum of is now a DERIVED pace
// rather than an authored one.
void DungeonWorld::ApplyPartyPace() {
	if (!m_roster || m_roster->empty()) return;
	const CurveRules pace = m_balance.PaceCurve();
	float slowest = (*m_roster)[0].MoveSpeed(pace);
	for (const Character& member : *m_roster)
		slowest = std::min(slowest, member.MoveSpeed(pace));
	m_party.SetSpeed(slowest);
}

// --- the pipeline's two faces (docs/effects.md) -------------------------------
// Everything fx::Deal needs to know about a combatant. The defender numbers are
// what PartyDefense/MonsterDefense used to assemble; the difference is that the
// EFFECT term is no longer a hard-coded Stone Skin branch but a sum over
// whatever effects the target happens to carry.

// The heaviest armor a member is wearing, which is the class that governs
// them: a plate cuirass over leather greaves is heavy armor with extra padding,
// not an average of the two.
ArmorClass DungeonWorld::WornArmorClass(const Character& member) {
	ArmorClass worst = ArmorClass::None;
	for (int i = 0; i < kEquipCount; ++i) {
		// HANDS DON'T COUNT. They are part of the equipment array, so a
		// cuirass carried in a fist would otherwise hand you heavy armor's
		// whole penalty for holding it. (Soak has always summed the hands
		// too, which is the same bug being quieter about it.)
		if (i == static_cast<int>(EquipSlot::LeftHand) ||
			i == static_cast<int>(EquipSlot::RightHand))
			continue;
		const ItemSlot& slot = member.inventory.equipment[static_cast<size_t>(i)];
		if (slot.Empty()) continue;
		const ArmorClass c = ItemKindFor(slot.typeId).armorClass;
		if (static_cast<int>(c) > static_cast<int>(worst)) worst = c;
	}
	return worst;
}

// What wearing `c` costs this member on the defense roll, training included.
// Returns 0 unarmored — where the `avoid` skill takes over instead.
// The ADAPTER: gather this member's inputs and hand them to the arithmetic in
// Game/Defense.h, which is where the floor rule lives and is measured.
float DungeonWorld::ArmorPenalty(const Character& member, ArmorClass c) const {
	if (c == ArmorClass::None) return 0.0f;
	const Balance& b = m_balance;
	const Balance::ArmorRules r = b.Armor(c);
	CurveRules offsetCurve = b.SkillCurve();
	offsetCurve.slope = b.armorOffsetSlope;
	return defense::ArmorPenalty(
		r.floor, r.Offsettable(), offsetCurve,
		static_cast<float>(member.SkillLevel(ArmorSkillId(c))), r.strength,
		static_cast<float>(member.strength), b.armorShortPenalty);
}

// The sheet's view of a member's defense. Built by ASKING the live path rather
// than re-deriving it: `total` is PartyTarget::Evasion itself, so a readout can
// never quietly disagree with the roll it claims to describe — which is the
// failure mode that makes a debug display worse than none at all.
DefenseReadout DungeonWorld::DefenseFor(const Character& member) {
	DefenseReadout r;
	const Balance& b = m_balance;
	r.armorClass = WornArmorClass(member);
	r.base = b.defenseBase;
	r.stat = CurveValue(static_cast<float>(member.dexterity), b.StatCurve());
	r.strength = member.strength;

	for (const ItemSlot& slot : member.inventory.equipment)
		if (!slot.Empty()) r.soak += ItemKindFor(slot.typeId).armor;

	if (r.armorClass == ArmorClass::None) {
		r.skillKey = std::string("skill.") + kAvoidSkill;
		r.skillLevel = member.SkillLevel(kAvoidSkill);
		r.skillBonus =
			CurveValue(static_cast<float>(r.skillLevel), b.AvoidCurve());
	} else {
		r.armorPenalty = ArmorPenalty(member, r.armorClass);
		r.strengthNeeded = static_cast<int>(b.Armor(r.armorClass).strength);
		r.skillKey = std::string("skill.") + ArmorSkillId(r.armorClass);
		r.skillLevel = member.SkillLevel(ArmorSkillId(r.armorClass));
		// Name the piece that decided the class, not merely the class.
		for (const ItemSlot& slot : member.inventory.equipment) {
			if (slot.Empty()) continue;
			const ItemKind& k = ItemKindFor(slot.typeId);
			if (k.armorClass == r.armorClass) {
				r.armorName = loc::Tr(k.nameKey);
				break;
			}
		}
	}
	// A PHYSICAL blow is the case worth showing: it is what the stance guards
	// with a weapon and what armor is for.
	r.total = PartyTarget{*this, const_cast<Character&>(member)}.Evasion(m_bashType);
	// Whatever the total is not otherwise accounted for IS the stance guard —
	// derived by subtraction so it cannot drift from the live formula if a term
	// is added there and forgotten here.
	r.stance = r.total - r.base - r.stat - r.skillBonus + r.armorPenalty;
	return r;
}

DefenseReadout DungeonWorld::DefenseWith(const Character& member,
										 const std::string& itemId) {
	// A COPY with the piece put on. The alternative — deriving "what would this
	// be worth" from its catalog fields — would be a second implementation of
	// the defense formula, and the two would drift the first time a term was
	// added to one of them.
	Character what = member;
	const WearSlot wear = ItemKindFor(itemId).wearSlot;
	for (int i = 0; i < kEquipCount; ++i) {
		if (!WearSlotFits(wear, static_cast<EquipSlot>(i))) continue;
		what.inventory.equipment[static_cast<size_t>(i)].typeId = itemId;
		break;
	}
	return DefenseFor(what);
}

// THE ADAPTER, and only that: resolve this member and this damage type into the
// numbers defense::Guard works on. Every RULE it used to carry now lives in
// Game/Defense.h where it is measured (docs/damage-system.md) —
//
//   * avoid is UNARMORED-ONLY, which makes going bare a build rather than the
//     poor man's option (and, since light/medium armor creep DEX just as avoid
//     does, means exactly one training loop ever runs);
//   * PHYSICAL is parried with a HAND, off its weapon class, the better of the
//     two answering; MAGICAL is warded with the skill in the INCOMING SCHOOL and
//     THE HANDS PLAY NO PART (the alternative rule — a guard belonging to a hand
//     and covering only the school that hand casts — is what this comment used to
//     describe and is NOT what happens); anything neither has nothing to parry
//     it with;
//   * the hands combine by MAX, not sum.
//
// The cooldown is deliberately NOT consulted: a stance is not an action, so a
// hand still guards while it recovers from a swing.
float DungeonWorld::PartyTarget::Evasion(DamageType type) const {
	const Balance& b = m_world.m_balance;
	defense::GuardInputs in;
	in.base = b.defenseBase;
	in.dexterity = static_cast<float>(m_member.dexterity);
	in.statCurve = b.StatCurve();

	in.worn = m_world.WornArmorClass(m_member);
	in.armorPenalty = m_world.ArmorPenalty(m_member, in.worn);
	in.avoidLevel = static_cast<float>(m_member.SkillLevel(kAvoidSkill));
	in.avoidCurve = b.AvoidCurve();

	in.held = 1.0f - m_member.offenseShare;
	in.skillCurve = b.SkillCurve();

	SpellSymbol school{};
	const bool hasSchool = m_world.m_damageTypes.SchoolOf(type, school);
	in.kind = defense::GuardKindFor(
		{hasSchool, m_world.m_damageTypes.IsPhysical(type)});
	if (hasSchool)
		in.schoolLevel = static_cast<float>(m_member.SkillLevel(SymbolId(school)));

	// A held item to the skill it parries off; an empty hand parries `unarmed`.
	// Resolved only for a PHYSICAL blow — Guard ignores these otherwise, so this
	// is purely about not paying for two catalog lookups per firebolt. The rule
	// still lives in Guard; this only declines to compute what it will not read.
	if (in.kind == defense::GuardKind::Physical) {
		const auto handLevel = [&](int hand) {
			const ItemSlot& slot = m_member.inventory.Hand(hand);
			const ItemKind* weapon =
				slot.Empty() ? nullptr : &m_world.ItemKindFor(slot.typeId);
			return static_cast<float>(m_member.SkillLevel(
				weapon ? std::string_view(weapon->skill)
					   : std::string_view("unarmed")));
		};
		in.leftLevel = handLevel(0);
		in.rightLevel = handLevel(1);
	}

	return defense::Guard(in);
}

ResistTable DungeonWorld::PartyPowers(const Character& member, int hand) {
	// Sums like a resist does: the wielded weapon plus every worn piece. A hand of
	// -1 means nothing is wielded — a spell — so only what is worn answers, which
	// is what lets a fire-attuned ring lend itself to a firebolt.
	ResistTable out;
	for (size_t i = 0; i < member.inventory.equipment.size(); ++i) {
		const ItemSlot& slot = member.inventory.equipment[i];
		if (slot.Empty()) continue;
		const bool isHand = i == static_cast<size_t>(EquipSlot::LeftHand) ||
							i == static_cast<size_t>(EquipSlot::RightHand);
		// The OTHER hand's weapon lends nothing to this swing: what is in your left
		// hand does not make your right hand's blade burn.
		if (isHand) {
			const bool thisHand =
				hand >= 0 && &slot == &member.inventory.Hand(hand);
			if (!thisHand) continue;
		}
		out.Add(ItemKindFor(slot.typeId).powers);
	}
	return out;
}

ResistTable DungeonWorld::AttackerPowers(int attacker, u32 shooter) {
	if (const Monster* m = MonsterByRuntimeId(shooter); m && m->kind)
		return m->kind->powers;
	if (m_roster && attacker >= 0 &&
		attacker < static_cast<int>(m_roster->size()))
		// WORN only: a bolt carries no hand, so what a caster happens to be holding
		// cannot be credited. Worn attunement is the honest half, and it is also the
		// half a robe-and-ring build is about.
		return PartyPowers((*m_roster)[static_cast<size_t>(attacker)], -1);
	return {};
}

float DungeonWorld::PartyTarget::Soak() const {
	float soak = 0.0f;
	for (const ItemSlot& slot : m_member.inventory.equipment)
		if (!slot.Empty()) soak += m_world.ItemKindFor(slot.typeId).armor;
	return soak;
}

float DungeonWorld::PartyTarget::Resist(DamageType type) const {
	float resist = m_member.natureResists[type]; // the race layer
	for (const ItemSlot& slot : m_member.inventory.equipment)
		if (!slot.Empty()) resist += m_world.ItemKindFor(slot.typeId).resists[type];
	resist += fx::EffectResist(m_member.effects, type, m_world.EffectKnobs());
	return m_world.m_balance.ClampResist(resist, m_member.natureResists[type]);
}

void DungeonWorld::PartyTarget::Wound(float amount, fx::DamageEvent& ev) {
	// THE RULE ITSELF (Game/DamageLedger.h): this is one of the six lines in the
	// game allowed to move a combatant's health, so it declares what it moved.
	// The scope MEASURES the change rather than being told `amount` — the clamp
	// at zero means those are not the same number.
	const ledger::Explained accounted{m_world.m_damageLedger, m_member.health,
									  ledger::Reason::Pipeline};
	m_fall = m_world.WoundMember(m_member, amount, ev.Quiet());
	ev.slew = m_fall != Fall::None;
	// The eval tally (docs/eval-harness.md). HERE rather than at the attack
	// sites, because everything that damages a member arrives through this one
	// line — a monster's blow, a blast, a DoT bite, a ward's reprisal. A tally
	// hung off the attack sites would have missed four of those five.
	m_world.m_harness.tally.taken += amount;
	// ONCE PER MEMBER, not once per fall — see Tally::downedMask. The roster
	// index is what identifies them; a slot past the mask's width is simply not
	// counted rather than aliasing onto somebody else's bit.
	if (m_fall != Fall::None) {
		const int slot = m_world.MemberIndex(m_member);
		if (slot >= 0 && slot < 8) {
			const u8 bit = static_cast<u8>(1u << slot);
			if ((m_world.m_harness.tally.downedMask & bit) == 0) {
				m_world.m_harness.tally.downedMask |= bit;
				++m_world.m_harness.tally.membersDowned;
			}
		}
	}
}

// Fed rather than hurt: a member whose nature DRINKS this element (a resist
// past 1). It cannot raise the dead — a corpse drinks nothing — but it will
// bring someone back from unconscious, which is the point of being made of the
// stuff that was just thrown at you.
void DungeonWorld::PartyTarget::Absorb(float amount, fx::DamageEvent& ev) {
	if (m_member.dead || amount <= 0.0f) return;
	const ledger::Explained accounted{m_world.m_damageLedger, m_member.health,
									  ledger::Reason::Pipeline};
	m_member.health = std::min(m_member.maxHealth, m_member.health + amount);
	// Quiet for a per-frame tick: a burn feeding something that drinks fire is
	// a steady trickle, not news forty times a second.
	if (!ev.Quiet())
		Say(loc::Format("log.member_absorbs", m_member.name,
						static_cast<int>(amount + 0.5f)));
}

void DungeonWorld::PartyTarget::NarrateFall() const {
	if (m_fall == Fall::Dead)
		m_world.MemberMessage(m_member,
							  loc::Format("log.member_dies", m_member.name));
	else if (m_fall == Fall::Down)
		m_world.MemberMessage(m_member,
							  loc::Format("log.member_down", m_member.name));
}

void DungeonWorld::PartyTarget::Say(const std::string& line) const {
	m_world.MemberMessage(m_member, line);
}

void DungeonWorld::PartyTarget::SayApplied(const fx::EffectKind& kind) const {
	const std::string& key = kind.ApplyLine(/*onMonster=*/false);
	if (!key.empty()) Say(loc::Format(key, m_member.name));
}

float DungeonWorld::MonsterTarget::Evasion(DamageType) const {
	// A monster's guard is its authored evasion plus whatever its STANCE held
	// back from its own competence — the mirror of a party member keeping part
	// of a hand's skill in reserve. The incoming type buys it nothing: a
	// monster has no hands to split and no schools to know, and a per-type
	// monster defense would be a monsters.cat column rather than a code change.
	const float held = std::max(0.0f, 1.0f - m_monster.kind->offense);
	return m_monster.kind->evasion + held * m_monster.kind->accuracy;
}

float DungeonWorld::MonsterTarget::Soak() const { return m_monster.kind->armor; }

float DungeonWorld::MonsterTarget::Resist(DamageType type) const {
	const float nature = m_monster.kind->resists[type];
	const float resist =
		nature + fx::EffectResist(m_monster.effects, type, m_world.EffectKnobs());
	return m_world.m_balance.ClampResist(resist, nature);
}

std::string DungeonWorld::MonsterTarget::Name() const {
	return loc::Tr("monster." + m_monster.kind->name);
}

void DungeonWorld::MonsterTarget::Say(const std::string& line) const {
	m_world.onMessage(line);
}

void DungeonWorld::MonsterTarget::SayApplied(const fx::EffectKind& kind) const {
	const std::string& key = kind.ApplyLine(/*onMonster=*/true);
	if (!key.empty()) Say(loc::Format(key, Name()));
}

// The monster mirror: a fire golem drinking a fire bolt. It still PROVOKES —
// you just made it stronger and it noticed — but earns its feeder no threat,
// since threat is a record of harm done.
void DungeonWorld::MonsterTarget::Absorb(float amount, fx::DamageEvent& ev) {
	if (!m_monster.Alive() || amount <= 0.0f) return;
	const ledger::Explained accounted{m_world.m_damageLedger, m_monster.hp,
									  ledger::Reason::Pipeline};
	m_monster.hp = std::min(m_monster.MaxHp(), m_monster.hp + amount);
	if (ev.Quiet()) return; // a tick feeding it is a trickle, not news
	m_world.ProvokeMonster(m_monster);
	m_world.onMessage(loc::Format("log.monster_absorbs", Name(),
								  static_cast<int>(amount + 0.5f)));
}

// The monster half of the apply stage: hit points, the grudge, the flinch, and
// the corpse — the five hand-written copies of this that used to sit at every
// site able to hurt a monster. The killing LINE stays with the caller (it says
// "slain" / "destroyed" / "burns away" depending on what did it, and has to
// come after the caller's own "hits for N").
void DungeonWorld::MonsterTarget::Wound(float amount, fx::DamageEvent& ev) {
	const ledger::Explained accounted{m_world.m_damageLedger, m_monster.hp,
									  ledger::Reason::Pipeline};
	m_monster.hp -= amount;
	// The eval tally's other half — see PartyTarget::Wound. Counted BEFORE the
	// death check below, so the blow that kills is still counted as damage
	// dealt rather than vanishing into the kill.
	m_world.m_harness.tally.dealt += amount;
	if (ev.source >= 0)
		m_world.AddThreat(m_monster, static_cast<size_t>(ev.source), amount);
	// A per-frame tick doesn't re-provoke or re-flinch every frame; anything
	// else wakes the monster and turns it on whoever struck.
	if (!ev.Quiet()) m_world.ProvokeMonster(m_monster);
	if (!m_monster.Alive()) {
		m_monster.hp = 0.0f; // a downed monster stays in the list (save restore)
		Extinguish(m_monster); // a corpse stops burning
		ev.slew = true;
		++m_world.m_harness.tally.monstersSlain;
	} else if (!ev.Quiet()) {
		m_monster.hitReq = true; // survivor flinches (a fatal blow plays Die)
	}
}

// --- burning bodies -----------------------------------------------------------
// What being ON FIRE looks like. The fire itself is an ordinary effect on the
// monster's list (Effect/DotEffect.h); these two answer "so where do the flames
// go, and what colour are they" for whichever effect happens to burn.

// How the flames read per school: the FireEffect palette is authored orange, so
// fire burns untinted and the other three recolour it (a water "burn" is the
// freezing kind — the plume runs cold blue). Multiplied over the flame/spark
// colours, so these are ratios against orange, not absolute colours.
Vec3 DungeonWorld::BurnTint(SpellSymbol school) {
	switch (school) {
	case SpellSymbol::Earth: return {0.55f, 1.25f, 0.45f}; // acrid green
	case SpellSymbol::Air:   return {0.85f, 1.05f, 1.35f}; // pale white-blue
	case SpellSymbol::Water: return {0.35f, 0.95f, 1.60f}; // cold blue
	default:                 return {1.0f, 1.0f, 1.0f};    // fire, as authored
	}
}

// The flame origin on a burning body: a third of a square up, so the plume
// rises off the torso rather than the feet (UNITS, like every other length).
Vec3 DungeonWorld::BurnOrigin(const Monster& monster) {
	return {monster.visualPos.x, monster.visualPos.y + 0.34f * kUnit,
			monster.visualPos.z};
}

void DungeonWorld::Extinguish(Monster& monster) {
	monster.effects.clear(); // a corpse carries nothing
	monster.plume.reset();
}

bool DungeonWorld::ApplyEffectAhead(std::string_view id, float magnitude,
									float seconds) {
	const fx::EffectKind* kind = m_effects.Find(id);
	if (!kind) return false;
	const Direction faced = static_cast<Direction>(m_party.Facing());
	const int tx = m_party.GridX() + DirDX(faced);
	const int tz = m_party.GridZ() + DirDZ(faced);
	for (Monster& m : m_monsters) {
		if (!m.Alive() || m.x != tx || m.z != tz) continue;
		// The kind's own school, so a hand-applied effect looks and behaves
		// exactly like one a weapon or a monster landed.
		fx::Apply(m.effects, *kind, kind->DefaultSchool(), magnitude, seconds);
		return true;
	}
	return false;
}

const fx::Inst* DungeonWorld::PlumeEffect(const Monster& monster) {
	for (const fx::Inst& e : monster.effects)
		if (e.kind && e.kind->Plume()) return &e;
	return nullptr;
}

int DungeonWorld::DotSource(const std::vector<fx::Inst>& effects) {
	for (const fx::Inst& e : effects)
		if (e.IsDot() && e.source >= 0) return e.source;
	return -1;
}

// Latch the party wipe exactly once when the last member falls (message + callback).
// Returns true the frame it latches. Shared by the melee/ranged/bump damage paths.
bool DungeonWorld::CheckPartyWipe() {
	if (m_partyWiped) return false;
	for (const Character& m : *m_roster)
		if (m.IsAlive()) return false; // someone still up
	m_partyWiped = true;
	onMessage(loc::Tr("log.party_wipe"));
	if (onPartyWipe) onPartyWipe();
	return true;
}

Vec3 DungeonWorld::PartyMemberSubPos(size_t member) const {
	// The facing-relative quadrant the portraits read (front-left/front-right/
	// rear-left/rear-right; Michael, 2026-07-10): the front pair stands a
	// quarter-cell toward the facing, the rear pair away, even indices in the
	// faced+3 column and odd in faced+1 (the projectile lane idiom — one home
	// for the handedness, shared by the lane test, the ranged aim, and the
	// melee near-row math).
	const Direction faced = static_cast<Direction>(m_party.Facing());
	const float q = kCellSize * 0.25f;
	const Vec3 center{(static_cast<float>(m_party.GridX()) + 0.5f) * kCellSize,
					  0.0f,
					  (static_cast<float>(m_party.GridZ()) + 0.5f) * kCellSize};
	const Direction lateral = static_cast<Direction>(
		(static_cast<int>(faced) + (member % 2 == 0 ? 3 : 1)) % 4);
	const float row = member < 2 ? q : -q; // front pair toward the facing
	return {center.x + static_cast<float>(DirDX(faced)) * row +
				static_cast<float>(DirDX(lateral)) * q,
			0.0f,
			center.z + static_cast<float>(DirDZ(faced)) * row +
				static_cast<float>(DirDZ(lateral)) * q};
}

int DungeonWorld::PickMeleeVictim(Monster& monster) {
	if (!m_roster) return -1;
	// The standing members (the old candidate list).
	std::array<size_t, 4> alive{};
	size_t n = 0;
	for (size_t i = 0; i < m_roster->size() && n < alive.size(); ++i)
		if ((*m_roster)[i].IsAlive()) alive[n++] = i;
	if (n == 0) return -1;

	// The PER-FILE blocking rule: relative to a reach-1 monster's approach the
	// party stands in two FILES (from ahead/behind the files are the columns,
	// each a front member backed by a rear one; from a flank they're the rows,
	// that side's member backing the other). In each file the FIRST STANDING
	// member is the one the monster can touch — a living near member shields
	// the far one, and a fallen near member OPENS the file: the monster steps
	// into the gap and reaches the far member directly (it does NOT get walled
	// by the other file's blocker). A pike (reach 2) skewers past blocking
	// entirely; ranged/casters never come through here. Approach is the
	// dominant-axis cardinal from the party cell toward the monster, relative
	// to the party facing (rel 0 = ahead ... 2 = behind); the file pairing
	// comes from the quadrant math's lateral idiom (even members hold the
	// faced+3 column, odd faced+1 — PartyMemberSubPos).
	const bool blocking = monster.kind->reach < 2;
	std::array<int, 2> nearPair{-1, -1};
	bool rowPair = true; // near pair is a rank (file mate = ^2) vs column (^1)
	if (blocking) {
		const int dx = monster.x - m_party.GridX();
		const int dz = monster.z - m_party.GridZ();
		Direction approach;
		if (std::abs(dx) >= std::abs(dz))
			approach = dx >= 0 ? Direction::East : Direction::West;
		else
			approach = dz > 0 ? Direction::South : Direction::North;
		const int rel =
			(static_cast<int>(approach) - m_party.Facing() + 4) % 4;
		switch (rel) {
		case 0: nearPair = {0, 1}; rowPair = true; break;  // the front line
		case 2: nearPair = {2, 3}; rowPair = true; break;  // from behind
		case 1: nearPair = {1, 3}; rowPair = false; break; // faced+1 column
		default: nearPair = {0, 2}; rowPair = false; break; // faced+3 column
		}
	}
	auto standing = [&](int i) {
		return i >= 0 && static_cast<size_t>(i) < m_roster->size() &&
			   (*m_roster)[i].IsAlive();
	};
	// The first standing member of each file (near, else the far one through
	// the gap); -1 = the whole file is down.
	std::array<int, 2> fileReach{-1, -1};
	if (blocking)
		for (size_t f = 0; f < 2; ++f) {
			const int nearI = nearPair[f];
			const int farI = nearI ^ (rowPair ? 2 : 1);
			fileReach[f] = standing(nearI) ? nearI
										   : (standing(farI) ? farI : -1);
		}
	auto reachable = [&](int i) {
		return !blocking || i == fileReach[0] || i == fileReach[1];
	};

	// The grudge first: the threat target when the monster can get at them —
	// in the near row, or through a fallen blocker's gap — else the file mate
	// standing directly in their way (the BLOCKER soaks the swing).
	if (const int t = ThreatTarget(monster); t >= 0) {
		if (reachable(t)) return t;
		return t ^ (rowPair ? 2 : 1); // unreachable ⇒ their file mate stands
	}

	// No grudge: uniform-random among the reachable members (the old pick,
	// narrowed to the first standing member of each file).
	std::array<size_t, 4> pool{};
	size_t pn = 0;
	for (size_t k = 0; k < n; ++k)
		if (reachable(static_cast<int>(alive[k]))) pool[pn++] = alive[k];
	if (pn == 0) { // can't happen (an alive member always heads their file) —
		pool = alive; // belt and braces against a future pairing change
		pn = n;
	}
	return static_cast<int>(pool[pn == 1 ? 0 : m_combatRng() % pn]);
}

void DungeonWorld::MonsterAttack(Monster& monster) {
	if (!m_roster || m_partyWiped) return;

	const int victim = PickMeleeVictim(monster);
	if (victim < 0) return;
	Character& target = (*m_roster)[victim];
	monster.attackCd = monster.kind->attackInterval;
	// Request the swing animation (one-shot; DriveMonsterAnim picks the variation
	// and times the hold, then the state machine returns to walk/idle). No attack
	// clip authored → DesiredState still yields Attack for a frame but PickClip is
	// empty, so nothing plays — the pre-clip look, as before.
	monster.attackReq = true;

	const std::string name = loc::Tr("monster." + monster.kind->name);
	// One blow, through the one pipeline. The wind ward can't deflect it (it
	// turns bolts), the water veil may soak it, and the fire shield answers it
	// — all of that is the stages' business now, not this function's.
	PartyTarget defender{*this, target};
	MonsterTarget striker{*this, monster};
	// The stance spends part of its competence on the swing; MonsterTarget's
	// guard keeps the rest (docs/damage-system.md).
	// Its POTENCY in what it deals (`powers`) scales the blow — a monster's whole
	// answer to the skill a character trains, since it has none.
	const AttackProfile atk{
		// Per-instance strength scales what it DEALS as well as what it can
		// take (Monster::MaxHp) — a scaled monster that hit like the authored
		// one would just be a longer fight, not a harder one.
		m_balance.Potent(monster.kind->damage * monster.strength,
						 monster.kind->powers, monster.kind->damageType),
		monster.kind->accuracy * monster.kind->offense,
		monster.kind->damageType, monster.kind->critPierce};
	fx::DamageEvent ev =
		fx::DamageEvent::Blow(atk.type, atk.damage, atk.attackBonus, victim);
	ev.pierceOnCrit = atk.pierceOnCrit;
	fx::Deal(ev, defender, m_balance.Strike(), m_combatRng);
	TrainDefense(target, ev); // avoid on a miss, armor on a blunted hit

	if (ev.fumble) {
		MemberMessage(target, loc::Format("log.foe_fumbles", name));
		// A monster's swing costs it too — the same tables, minus the three
		// consequences a creature with no hands and no stamina cannot pay.
		MonsterFumble(monster, atk, ev.fumbleFace);
	} else if (ev.crit && ev.hit)
		MemberMessage(target, loc::Format("log.foe_critical", name));
	else if (ev.defenderFumbled)
		MemberMessage(target, loc::Format("log.fumble_guard", target.name));

	if (!ev.hit) {
		MemberMessage(target, loc::Format("log.monster_misses", name, target.name));
		return;
	}
	// Nothing got through (immune) says so; drinking it says so through the
	// adapter, so only a real wound reports a number.
	if (ev.dealt >= 0.5f)
		MemberMessage(target, loc::Format("log.monster_hits", name, target.name,
										  static_cast<int>(ev.dealt + 0.5f)));
	else if (ev.dealt >= 0.0f)
		MemberMessage(target, loc::Format("log.member_unharmed", target.name));
	defender.NarrateFall();
	m_audio.Play(m_sounds.monster, 0.6f);
	// Whatever its blows leave behind — applied even if the blow itself downed
	// them (the ticks finish the job). A monster lends no element, so each
	// effect arrives in its own colours.
	fx::ApplyProcs(defender, monster.kind->onHit, std::nullopt, -1, m_effects,
				   m_combatRng);
	// A CRITICAL leaves more behind than an ordinary blow. Rolled after the
	// on_hit list, so a weapon that burns on every hit and bleeds on a crit
	// applies both rather than one instead of the other.
	if (ev.crit)
		fx::ApplyProcs(defender, monster.kind->onCrit, std::nullopt, -1,
					   m_effects, m_combatRng);
	// ...and now the blow has been reported, whatever guards them answers it
	// (the fire shield scorches its attacker). Its own death line comes from
	// the ward, since nothing else is narrating this reprisal.
	fx::React(ev, defender, &striker, Reaction());
	CheckPartyWipe();
}

// A blocked move has lurched the party into the obstacle. Every standing member
// is jarred for a small flat amount, with the smallest splat over each portrait
// and a single grunt — then we re-check for a wipe so a final stumble still ends
// the run cleanly.
// The world's own blows — a wall the party walked into, a shaft they dropped
// down. Neither has an attacker, so both are an Impact BLUDGEON through the one
// pipeline: a breastplate blunts it, Stone Skin turns it, a water veil drinks
// it, exactly as any other blow. Which means members do NOT all take the same
// amount; the caller's line reports the WORST of them, and says nothing at all
// when the party shrugged it off. Returns that worst dealt.
float DungeonWorld::CollideParty(float amount) {
	float worst = 0.0f;
	for (Character& member : *m_roster) {
		if (!member.IsAlive()) continue;
		PartyTarget jarred{*this, member};
		fx::DamageEvent ev = fx::DamageEvent::Impact(m_bashType, amount);
		fx::Deal(ev, jarred, m_balance.Strike(), m_combatRng);
		jarred.NarrateFall();
		// The collision answers itself: whatever guards a member scorches the
		// wall for nothing (no attacker), but a veil that burst soaking it has
		// to be dropped, and that is the react stage's business either way.
		fx::React(ev, jarred, nullptr, Reaction());
		worst = std::max(worst, ev.dealt);
	}
	return worst;
}

void DungeonWorld::OnBumpImpact() {
	if (!m_roster || m_partyWiped) return;
	const float worst = CollideParty(m_balance.bumpDamage);
	// Quiet when the party shrugged the wall off: the log speaks in whole
	// points, so a jar that rounds to nothing has nothing to report. (A heavily
	// resisted collision still chips a fraction — it just isn't news.)
	const int shown = static_cast<int>(worst + 0.5f);
	if (shown <= 0) return;

	onMessage(loc::Format("log.bump_hurt", shown));
	m_audio.Play(m_sounds.oof, 0.8f);
	CheckPartyWipe();
}

// The plunge landed. Same collision as the bump, one knob heavier — the pit's
// own "world.pitfall" line already said what happened, so this only reports the
// bruise, and stays silent when the party rode it out.
void DungeonWorld::OnFallImpact() {
	if (!m_roster || m_partyWiped) return;
	const float worst = CollideParty(m_balance.fallDamage);
	const int shown = static_cast<int>(worst + 0.5f);
	if (shown <= 0) return;

	onMessage(loc::Format("log.fall_hurt", shown));
	m_audio.Play(m_sounds.oof, 0.9f);
	CheckPartyWipe();
}

// The one place a monster's one-cell move is committed — the logical cell/slot
// snap the instant the step commits (so occupancy is atomic, like the party),
// while visualPos glides from where it stood over moveInterval.
void DungeonWorld::StepMonsterTo(Monster& monster, int x, int z, int slot) {
	monster.moveFrom = monster.visualPos;
	monster.x = x;
	monster.z = z;
	monster.slot = slot;
	monster.moving = true;
	monster.moveT = 0.0f;
	monster.moveCd = monster.kind->moveInterval;
}

// Greedy local step for the kite/flee executors: pick the lowest-scoring of the
// monster's own cell (the hold baseline) and its four free orthogonal neighbours,
// and step there. `score` is evaluated on candidate CELLS; only walkable, slot-
// free neighbours are considered (FreeSlotInCell also excludes the party cell).
void DungeonWorld::GreedyStep(Monster& monster, int selfIndex,
							  const std::function<int(int cx, int cz)>& score) {
	int bestScore = score(monster.x, monster.z); // own cell = hold baseline
	int bx = monster.x, bz = monster.z, bslot = monster.slot;
	static constexpr int kDX[4] = {1, -1, 0, 0}, kDZ[4] = {0, 0, 1, -1};
	for (int k = 0; k < 4; ++k) {
		const int nx = monster.x + kDX[k], nz = monster.z + kDZ[k];
		const int slot = FreeSlotInCell(nx, nz, monster.kind->size, selfIndex);
		if (slot < 0) continue; // unwalkable / occupied / the party cell
		const int s = score(nx, nz);
		if (s < bestScore) { bestScore = s; bx = nx; bz = nz; bslot = slot; }
	}
	if (bx != monster.x || bz != monster.z) StepMonsterTo(monster, bx, bz, bslot);
}

// ----------------------------------------------------------------------------
// Skirmisher (archetype = skirmisher): hold at range and shoot. The brain sets
// intent == Kite (no path); this executor drives movement + firing directly from
// live party position on the main thread, every frame at the monster's cadence.
// ----------------------------------------------------------------------------
void DungeonWorld::UpdateKiter(Monster& monster, int selfIndex) {
	const int px = m_party.GridX(), pz = m_party.GridZ();
	const int dist = std::max(std::abs(monster.x - px), std::abs(monster.z - pz));
	const bool los = CellHasLineOfSight(monster.x, monster.z, px, pz);

	// Announce once, like a brute, when it first has the party in reach.
	if (!monster.announced) {
		monster.announced = true;
		onMessage(loc::Format("log.monster_stirs", loc::Tr("monster." + monster.kind->name)));
		m_audio.Play(m_sounds.monster, 0.7f);
	}

	// Fire when it can see the party and is within its shooting reach (its perception
	// range), off cooldown. A blocked line holds fire (it repositions instead).
	if (los && static_cast<float>(dist) <= monster.kind->aggroRange &&
		monster.attackCd <= 0.0f)
		MonsterRangedAttack(monster);

	// Hold keepRange while lining up a shot: greedy 1-step to the free 4-neighbour
	// that best trades off distance-to-keepRange against being able to FIRE — i.e.
	// on a clear cardinal line to the party (orthogonal LoS) within reach. So it
	// backs off when crowded, closes when too far, and side-steps onto the party's
	// row/column to get the axis a bolt needs. Holds when its own cell scores best.
	// No BFS: kiting is a local decision the host makes each step against LIVE occupancy.
	if (!monster.moving && monster.moveCd <= 0.0f) {
		const int want = static_cast<int>(monster.KeepRange() + 0.5f);
		GreedyStep(monster, selfIndex, [&](int cx, int cz) {
			const int d = std::max(std::abs(cx - px), std::abs(cz - pz));
			int s = std::abs(d - want) * 2; // primary: distance error
			// Strongly prefer a cell it can actually shoot from (on-axis + in range);
			// getting onto the party's row/column is the point of a kiter.
			const bool canFire = static_cast<float>(d) <= monster.kind->aggroRange &&
								 CellHasLineOfSight(cx, cz, px, pz);
			if (!canFire) s += 4;
			return s;
		});
	}
}

// ----------------------------------------------------------------------------
// Flee (intent == Flee): a wounded monster below its fleeBelow threshold breaks
// off and runs. Greedy orthogonal 1-step that MAXIMISES distance from the party
// (the opposite of the brute chase); no attack. Holds if boxed in — a cornered
// monster has nowhere to run. Announce is left to whatever woke it.
// ----------------------------------------------------------------------------
void DungeonWorld::UpdateFleer(Monster& monster, int selfIndex) {
	if (monster.moving || monster.moveCd > 0.0f) return; // mid-step / on cooldown

	// Run away: score = NEGATED squared distance to the party, so GreedyStep (which
	// minimises) picks the FARTHEST free neighbour, and holds if none is farther.
	const int px = m_party.GridX(), pz = m_party.GridZ();
	GreedyStep(monster, selfIndex, [&](int cx, int cz) {
		const int dx = cx - px, dz = cz - pz;
		return -(dx * dx + dz * dz);
	});
}

void DungeonWorld::UpdateReturner(Monster& monster, int selfIndex) {
	if (monster.moving || monster.moveCd > 0.0f) return; // mid-step / on cooldown
	// Walk home: score = squared distance to the leash anchor, so GreedyStep steps
	// to the nearest free neighbour (and holds once it arrives).
	const int ax = monster.leashX, az = monster.leashZ;
	GreedyStep(monster, selfIndex, [&](int cx, int cz) {
		const int dx = cx - ax, dz = cz - az;
		return dx * dx + dz * dz;
	});
}

void DungeonWorld::UpdatePatroller(Monster& monster, int selfIndex) {
	if (monster.patrol.empty()) return;
	if (monster.moving || monster.moveCd > 0.0f) return; // mid-step / on cooldown
	// Advance to the next waypoint once standing on the current one (wrap the loop).
	monster.patrolIdx %= monster.patrol.size();
	const ai::Cell& cur = monster.patrol[monster.patrolIdx];
	if (monster.x == cur.x && monster.z == cur.z)
		monster.patrolIdx = (monster.patrolIdx + 1) % monster.patrol.size();
	const ai::Cell& wp = monster.patrol[monster.patrolIdx];
	// Greedy orthogonal step toward the waypoint (best for open/line routes; a wall
	// between can stall it — lay waypoints densely, or add BFS later).
	GreedyStep(monster, selfIndex, [&](int cx, int cz) {
		const int dx = cx - wp.x, dz = cz - wp.z;
		return dx * dx + dz * dz;
	});
}

void DungeonWorld::MonsterRangedAttack(Monster& monster) {
	monster.attackCd = monster.kind->attackInterval;
	monster.attackReq = true; // play the swing/cast gesture if the rig ships one

	// Launch a bolt down the CARDINAL axis it shares with the party (the caller only
	// fires when CellHasLineOfSight is true, which is orthogonal-only — so the party
	// is straight N/E/S/W). Aiming along the axis, like the party's own spell bolts,
	// keeps everything on the 4-directional grid; no diagonal shots. It flies through
	// the shared moving-item engine and strikes the party when it reaches their cell
	// (TargetSide::Party -> ResolveMonsterProjectileHit); a wall fizzles it.
	const int px = m_party.GridX(), pz = m_party.GridZ();
	Vec3 dir{0.0f, 0.0f, 0.0f};
	if (monster.z == pz && monster.x != px)
		dir.x = px > monster.x ? 1.0f : -1.0f; // same row: fire east/west
	else if (monster.x == px && monster.z != pz)
		dir.z = pz > monster.z ? 1.0f : -1.0f; // same column: fire north/south
	else
		return; // not axis-aligned (shouldn't happen — caller gates on orthogonal LoS)
	Vec3 origin = SlotCenter(monster.x, monster.z, monster.kind->size, monster.slot);
	origin.y += 0.6f;

	// AIM the lane at the threat target: slide the launch point's LATERAL
	// coordinate (the one LaneOffset measures — flight is straight, so the
	// lateral never changes) onto the target's quadrant. The shift is at most
	// a quarter-cell, and the shared row/column means the aligned coordinate
	// stays inside the monster's own cell. No target → the old slot lane.
	if (const int aimAt = ThreatTarget(monster); aimAt >= 0) {
		const Vec3 aim = PartyMemberSubPos(static_cast<size_t>(aimAt));
		if (dir.x != 0.0f)
			origin.z = aim.z;
		else
			origin.x = aim.x;
	}

	// A CASTER (archetype = caster with a monsters.cat `spell`) throws that
	// spell's bolt — Spell::MonsterBolt, the same class the party casts from,
	// at the monster's accuracy (no monster mana/vocab; it just shoots on
	// cooldown). A spell with no thrown form (a ward) yields nothing, and any
	// other ranged monster (a skirmisher), a plain ember bolt.
	const Spell* spell =
		monster.Spell().empty() ? nullptr : m_magic.FindSpell(monster.Spell());
	if (spell) {
		if (std::optional<ProjectileSpec> bolt =
				spell->MonsterBolt(origin, dir, monster.kind->accuracy)) {
			bolt->shooter = monster.runtimeId; // the impact reads its threat
			m_projectiles.Spawn(*bolt);
			m_audio.Play(m_sounds.spellCast, 0.6f); // the cast voice
			return;
		}
	}
	ProjectileSpec bolt;
	bolt.pos = origin;
	bolt.dir = dir;
	bolt.target = TargetSide::Party;
	bolt.speed = 6.0f;
	bolt.range = (monster.kind->aggroRange + 1.0f) * kCellSize; // a bit past aggro
	// The plain ember bolt types as the monster's melee (its dmgtype); a real
	// spell bolt above types by its school inside MakeBolt.
	bolt.atk = {monster.kind->damage, monster.kind->accuracy,
				monster.kind->damageType};
	bolt.color = {1.6f, 0.5f, 0.2f, 0.0f}; // ember-orange additive
	bolt.size = 0.18f;
	bolt.shooter = monster.runtimeId; // the impact reads its threat
	// A shot leaves what its melee leaves: `on_hit` is what this creature's
	// attacks carry, and a venomous thing's dart is venomous too. (A CASTER's
	// bolt above takes the SPELL's payload instead — the spell is the source
	// there, not the creature.)
	bolt.payload = PackPayload(monster.kind->onHit,
							   "monsters.cat [" + monster.kind->name + "]");
	m_projectiles.Spawn(bolt);
	m_audio.Play(m_sounds.monster, 0.5f); // soft launch cue (reuse the monster voice)
}

bool DungeonWorld::CellHasLineOfSight(int x0, int z0, int x1, int z1) const {
	// ORTHOGONAL-only over the LIVE map, mirroring ai::SnapshotView::HasLineOfSight:
	// a clear line exists only down a shared row or column (no diagonal sight/fire),
	// with every cell strictly between walkable; endpoints never block.
	if (x0 == x1 && z0 == z1) return true;
	if (x0 == x1) {
		const int s = z0 < z1 ? 1 : -1; // sight runs along Z (axis 1)
		for (int z = z0 + s; z != z1; z += s)
			if (!m_map.IsWalkable(x0, z) && !WallSeeThrough(x0, z, 1)) return false;
		return true;
	}
	if (z0 == z1) {
		const int s = x0 < x1 ? 1 : -1; // sight runs along X (axis 0)
		for (int x = x0 + s; x != x1; x += s)
			if (!m_map.IsWalkable(x, z0) && !WallSeeThrough(x, z0, 0)) return false;
		return true;
	}
	return false; // not axis-aligned — no orthogonal line
}

bool DungeonWorld::PartyAttack(size_t member, size_t hand, std::string_view verb) {
	if (!m_roster || member >= m_roster->size() || hand > 1) return false;
	Character& attacker = (*m_roster)[member];
	if (!attacker.IsAlive() || attacker.handCooldown[hand] > 0.0f) return false;

	// The cell directly ahead of the party.
	const Direction faced = static_cast<Direction>(m_party.Facing());
	const int tx = m_party.GridX() + DirDX(faced);
	const int tz = m_party.GridZ() + DirDZ(faced);

	Monster* target = nullptr;
	for (Monster& m : m_monsters)
		if (m.Alive() && m.x == tx && m.z == tz) { target = &m; break; }

	// The swinging hand's weapon: catalog damage/speed/stats feed the formula
	// below; its `skill` is the weapon class (docs/skills.md) — a bare hand
	// swings, and trains, unarmed. The class level scales the profile, the
	// landed blow below trains the class + creeps its associated stats.
	const ItemSlot& held = attacker.inventory.Hand(static_cast<int>(hand));
	const ItemKind* weapon = held.Empty() ? nullptr : &ItemKindFor(held.typeId);

	// The rear rank can't reach (Phase 7): roster slots 0-1 are the FRONT
	// line, 2-3 the REAR — a rear member swings only a polearm (`reach =
	// polearm`); everything else, bare hands included, whiffs on distance
	// alone. Ranged and spells ignore rank (CastSpell has no gate).
	if (member >= 2 && !(weapon && weapon->polearm)) {
		MemberMessage(attacker, loc::Tr("log.no_reach"));
		return true;
	}

	const std::string_view skillId =
		weapon ? std::string_view(weapon->skill) : std::string_view("unarmed");
	const int level = attacker.SkillLevel(skillId);
	// The attack formula (docs/combat.md part 5). The ATTACK (the executed
	// verb) supplies the damage type + its three numbers; the weapon supplies
	// the base damage/pace (unarmed knobs when bare/unstated); the associated
	// stats' average is the attack bonus; accuracy is ALWAYS DEX.
	const AttackSpec* spec = m_balance.FindAttack(verb);
	if (!spec) spec = &m_balance.Neutral();
	const std::span<const std::string> stats =
		weapon && !weapon->stats.empty() ? std::span<const std::string>(weapon->stats)
										 : std::span<const std::string>(UnarmedStats());
	const float statAvg = attacker.StatAvg(stats);
	const float base =
		weapon && weapon->damage > 0.0f ? weapon->damage : m_balance.unarmedBase;
	const float pace =
		weapon && weapon->speed > 0.0f ? weapon->speed : m_balance.unarmedSpeed;

	// Exhaustion penalties (Phase 4) read the state at swing time — the swing
	// that empties the bar lands whole; the NEXT one pays.
	const bool winded = attacker.exhausted;

	// A whiffed swing pays the attack's pace too — committing to a chop
	// costs the chop. And every swing, hit or miss, is EXERTION: it spends
	// (stamina_swing + stamina_weight × weapon kg) × the attack's stam.
	float interval =
		pace *
		(m_balance.speedBase -
		 m_balance.speedStat * static_cast<float>(attacker.dexterity)) *
		spec->pace;
	if (interval < m_balance.intervalMin) interval = m_balance.intervalMin;
	if (interval > m_balance.intervalMax) interval = m_balance.intervalMax;
	if (winded) interval *= m_balance.exhaustPace; // beyond the normal cap
	attacker.handCooldown[hand] = interval;
	SpendStamina(attacker,
				 (m_balance.staminaSwing +
				  m_balance.staminaWeight * (weapon ? weapon->weight : 0.0f)) *
					 spec->stam);

	// OVER-EXERTION is billed when the swing is DONE rather than when it begins,
	// through every exit — a whiff at air committed just as much as a landed
	// blow. The order is the reason for the lambda: the bill can put its own
	// owner down, and that line has to read AFTER the blow it paid for, not
	// before it. Zero unless the stance is past 1, in which case SpendExertion
	// is a no-op and this costs nothing.
	const float exertion = defense::ExertionPoints(
		attacker.offenseShare, static_cast<float>(level), m_balance.SkillCurve());
	const auto finish = [&] {
		SpendExertion(attacker, exertion);
		return true;
	};

	if (!target) {
		MemberMessage(attacker, loc::Tr("log.attack_air"));
		return finish();
	}

	const AttackProfile atk{
		(base + m_balance.statDamage * statAvg) * spec->dmg *
			(1.0f + m_balance.skillDamage * static_cast<float>(level)) *
			(winded ? m_balance.exhaustDamage : 1.0f),
		// THE ATTACK BONUS (docs/damage-system.md): skill is the main driver,
		// through its diminishing-returns curve; DEX shades it through its own,
		// much shallower one; the verb adds its authored points. All three are
		// in d100 points, against the ~41 the dice themselves deviate by.
		//
		// The STANCE scales the skill term and only the skill term — the same
		// points it takes off the guard are the ones it puts behind the swing, so
		// one number moves both sides (defense::StanceAttack). DEX is not skill
		// and rides at full weight whatever the stance.
		defense::StanceAttack(attacker.offenseShare, static_cast<float>(level),
							  m_balance.SkillCurve()) +
			CurveValue(static_cast<float>(attacker.dexterity),
					   m_balance.StatCurve()) +
			spec->acc,
		spec->type,
		// `crit = pierce`: this edge finds the gap between the plates.
		weapon && weapon->critPierce};
	const std::string name = loc::Tr("monster." + target->kind->name);
	PartyTarget striker{*this, attacker};
	MonsterTarget defender{*this, *target};
	// The attacker's type axis: potency summed from THIS hand's weapon and every
	// worn piece (PartyPowers). A character has no innate cell — their own axis is
	// skill — so this is entirely what they carry.
	fx::DamageEvent ev = fx::DamageEvent::Blow(
		atk.type,
		m_balance.Potent(atk.damage, PartyPowers(attacker, static_cast<int>(hand)),
						 atk.type),
		atk.attackBonus, static_cast<int>(member));
	ev.pierceOnCrit = atk.pierceOnCrit;
	fx::Deal(ev, defender, m_balance.Strike(), m_combatRng);
	// The dice half of the eval tally. Counted for the PARTY's swings only: a
	// hit rate that mixed both sides together would answer no question anyone
	// has, and the monsters' side is visible as `taken` anyway.
	if (ev.hit) ++m_harness.tally.hits; else ++m_harness.tally.misses;
	if (ev.crit) ++m_harness.tally.crits;
	if (ev.fumble) ++m_harness.tally.fumbles;

	// WHAT THE DICE DID, said before the outcome it caused. The open-ended roll
	// has been driving damage since P2 and was invisible: a critical arrived as
	// a big number with no explanation, and a fumble as an ordinary miss. The
	// line is the point — a mechanic nobody can see is a mechanic nobody has.
	if (ev.fumble) {
		MemberMessage(attacker, loc::Format("log.fumble", attacker.name));
		// ...and what that cost you, said after the line that announced it. The
		// swing is over — it cannot land — so the consequences are the rest of
		// this exchange, and `finish` below still bills any over-exertion.
		PartyFumble(attacker, hand, weapon, atk, ev.fumbleFace);
	} else if (ev.crit && ev.hit)
		MemberMessage(attacker, loc::Format("log.critical", attacker.name));
	else if (ev.defenderFumbled)
		MemberMessage(attacker, loc::Format("log.foe_fumbles", name));

	if (!ev.hit) {
		MemberMessage(attacker, loc::Format("log.party_misses", attacker.name, name));
		return finish();
	}
	// ENCHANTMENT: a landed blow with an elemental weapon carries its element
	// through as well — a SECOND event of that element, `element_bonus` of the
	// assembled damage. It rides the physical hit, so it is neither rolled nor
	// soaked (plate turns a blade, not a flame) but the target's resist for the
	// element still answers it.
	float elemental = 0.0f;
	if (weapon && weapon->enchanted && weapon->elementBonus > 0.0f) {
		// The enchantment gets the type axis too, and in ITS OWN element rather than
		// the blade's physical one — which is the case the whole feature is for: a
		// fire-attuned wielder's burning sword burns hotter.
		const DamageType elemType = m_damageTypes.ForSchool(weapon->element);
		fx::DamageEvent burst = fx::DamageEvent::Burst(
			elemType,
			m_balance.Potent(atk.damage * weapon->elementBonus,
							 PartyPowers(attacker, static_cast<int>(hand)), elemType),
			static_cast<int>(member));
		fx::Deal(burst, defender, m_balance.Strike(), m_combatRng);
		elemental = burst.dealt;
	}
	const float landed = ev.dealt + elemental;
	if (landed >= 0.5f)
		MemberMessage(attacker, loc::Format("log.party_hits", attacker.name, name,
											static_cast<int>(landed + 0.5f)));
	else if (landed >= 0.0f)
		MemberMessage(attacker, loc::Format("log.monster_unharmed", name));
	GrantSkillXp(attacker, skillId, 1.0f, stats); // a LANDED blow trains its class
	m_audio.Play(m_sounds.monster, 0.7f);

	fx::React(ev, defender, &striker, Reaction()); // whatever guards it answers
	if (!target->Alive()) {
		onMessage(loc::Format("log.monster_slain", name));
	} else if (weapon && !weapon->onHit.empty()) {
		// A survivor wears whatever the weapon leaves — burning from an
		// enchanted blade, bleeding from a serrated one. An enchanted weapon
		// lends its element as the flavour; a plain one lets each effect keep
		// its own.
		const std::optional<SpellSymbol> flavour =
			weapon->enchanted ? std::optional{weapon->element} : std::nullopt;
		fx::ApplyProcs(defender, weapon->onHit, flavour,
					   static_cast<int>(member), m_effects, m_combatRng);
		if (ev.crit)
			fx::ApplyProcs(defender, weapon->onCrit, flavour,
						   static_cast<int>(member), m_effects, m_combatRng);
	}
	return finish();
}

// ============================================================================
// Spell casting — a thin façade over the MagicSystem (Magic.h). This routes the
// party pose into the cast, spawns the resulting bolt into the moving-item engine
// (m_projectiles), and turns the cast outcome into HUD/audio feedback. The recipe
// lookup + mana live in the magic module; bolt flight, impacts, and sparks live in
// the shared engine (Projectiles.h) — its impact hook is ResolveSpellHit below.
// ============================================================================

bool DungeonWorld::CastSpell(size_t member, std::span<const SpellSymbol> sequence,
							 int hand) {
	if (!m_roster || member >= m_roster->size()) return false;
	Character& caster = (*m_roster)[member];
	if (!caster.IsAlive()) return false;

	// Bolt travels the party's faced cardinal direction (the grid facing, not the
	// free-look offset) — but down the CASTER'S QUADRANT lane, not the cell's
	// center line (Michael, 2026-07-10): the portraits read front-left,
	// front-right, rear-left, rear-right, so roster columns 0/2 are the
	// on-screen LEFT pair, 1/3 the right — each a quarter-cell off center.
	// (Repositioning members is a later feature.) The monster mirror already
	// holds (their bolts spawn at their sub-cell slot); a future ranged
	// weapon fires from the same lane.
	const Direction faced = static_cast<Direction>(m_party.Facing());
	const Direction lateral = static_cast<Direction>(
		(static_cast<int>(faced) + (member % 2 == 0 ? 3 : 1)) % 4);
	Vec3 origin = m_party.EyePosition();
	origin.x += static_cast<float>(DirDX(lateral)) * (kCellSize * 0.25f);
	origin.z += static_cast<float>(DirDZ(lateral)) * (kCellSize * 0.25f);
	const Vec3 dir{static_cast<float>(DirDX(faced)), 0.0f,
				   static_cast<float>(DirDZ(faced))};

	const MagicSystem::CastReport r =
		m_magic.Cast(caster, static_cast<int>(member), sequence, origin, dir,
					 m_combatRng);
	switch (r.outcome) {
	case MagicSystem::CastOutcome::Cast:
		// The spell's own Cast() override has already landed the effect
		// through the cast services (Spell/Spell.h) — a bolt is flying, a
		// ward settled. What remains is the COMMON aftermath every success
		// shares, whatever the spell did.
		MemberMessage(caster, loc::Format("log.cast", caster.name,
										  loc::Tr(r.spell->NameKey())));
		// A spell is LEARNED the first time it is successfully cast — the
		// failed outcomes below (a Fumble included) teach nothing.
		if (caster.learnedSpells.insert(r.spell->Id()).second)
			MemberMessage(caster, loc::Format("log.spell_learned", caster.name,
											  loc::Tr(r.spell->NameKey())));
		// The freshest cast leads the FIRING hand's quick list (each hand keeps
		// its own repertoire); a hand-less cast (dev console) touches neither,
		// and a spellbook cast (kBookHands — member-driven, not hand-fired)
		// credits BOTH so the discovery reaches either hand's menu.
		if (hand == 0 || hand == 1) {
			caster.TouchSpellMru(static_cast<size_t>(hand), r.spell->Id());
		} else if (hand == kBookHands) {
			caster.TouchSpellMru(0, r.spell->Id());
			caster.TouchSpellMru(1, r.spell->Id());
		}
		// The school skill grows with every SUCCESSFUL cast, in proportion to
		// the spell's mana (a dearer spell teaches more) — docs/skills.md.
		GrantSkillXp(caster, SymbolId(r.spell->School()), r.spell->Mana() * 0.25f,
					 SchoolStats(r.spell->School()));
		// ATTUNEMENT trains off the same throughput, and the two are NOT a
		// double-dip: the school skill is what you know about fire and creeps
		// INT/WIL for it, while attunement is what your body can pass — it
		// grows the mana POOL and creeps nothing. Two different lessons from
		// one act, which is the whole aptitude/practice split
		// (docs/health-and-healing.md).
		GrantResourceXp(caster, resource::Kind::Mana, r.spell->Mana());
		m_audio.Play(m_sounds.spellCast, 0.7f);
		// OVER-EXERTION's bill, last — like a swing's, so the collapse it can
		// cause reads after the cast that paid for it. Zero unless the caster's
		// stance is past 1.
		SpendExertion(caster, r.exertion);
		return true;
	case MagicSystem::CastOutcome::Fumble:
		// The skill roll failed: the mana is spent, nothing happens, and
		// nothing is learned — the recipe stays anonymous until a cast lands.
		MemberMessage(caster, loc::Format("log.cast_fumble", caster.name));
		m_audio.Play(m_sounds.spellFizzle, 0.6f);
		// A fumbled cast was still THROWN, so it is billed like any other.
		SpendExertion(caster, r.exertion);
		return false;
	case MagicSystem::CastOutcome::NoMana:
		MemberMessage(caster, loc::Format("log.cast_nomana", caster.name));
		return false;
	case MagicSystem::CastOutcome::Unknown:
		MemberMessage(caster, loc::Format("log.cast_unknown", caster.name));
		return false;
	case MagicSystem::CastOutcome::NoRecipe:
	default:
		onMessage(loc::Tr("log.spell_fizzles"));
		return false;
	}
}

bool DungeonWorld::CastSpellById(size_t member, std::string_view id, int hand) {
	const Spell* def = m_magic.FindSpell(id);
	if (!def) return false; // stale default / catalog typo — nothing to cast
	return CastSpell(member, def->Sequence(), hand);
}

namespace {
// The lane-hit window (Michael, 2026-07-10): the lateral distance between a
// bolt's travel line and a body's sub-cell position. 0 = the same side,
// cell/4 = a centered body (hit), ~cell/2+ = the opposite quadrant (flies
// past) — 0.35 splits hit from miss. Shared by both directions of fire.
constexpr float kLaneHalfWidth = kCellSize * 0.35f;

// Lateral offset of `pos` from a cardinal bolt's travel line.
float LaneOffset(const ProjectileImpact& impact, const Vec3& pos) {
	return std::abs(impact.dir.x) > 0.5f ? pos.z - impact.pos.z
										 : pos.x - impact.pos.x;
}
} // namespace

// The first live monster in this cell that is IN the bolt's lane, or -1. Split out
// so the single-target strike and an area carrier's detonation ask the same
// question — an area bolt flying past a wrong-lane body must fly on, exactly like
// a plain one, or a bomb would go off on contact with a monster it never touched.
int DungeonWorld::MonsterInLane(const ProjectileImpact& impact, int cx,
								int cz) const {
	for (size_t i = 0; i < m_monsters.size(); ++i) {
		const Monster& m = m_monsters[i];
		if (!m.Alive() || m.x != cx || m.z != cz) continue;
		const Vec3 mp = SlotCenter(m.x, m.z, m.kind->size, m.slot);
		if (std::abs(LaneOffset(impact, mp)) > kLaneHalfWidth)
			continue; // opposite side
		return static_cast<int>(i);
	}
	return -1;
}

bool DungeonWorld::ResolveSpellHit(const ProjectileImpact& impact) {
	const int cx = static_cast<int>(std::floor(impact.pos.x / kCellSize));
	const int cz = static_cast<int>(std::floor(impact.pos.z / kCellSize));
	// AN AREA CARRIER THAT CONNECTS STILL EXPLODES, and the blast is the whole of
	// what it does — no separate single-target strike, or the body it happened to
	// touch would take the blow twice. It goes off wherever it made contact, so a
	// bomb that hits the front rank catches everything around them.
	if (impact.payload.blast.Any()) {
		if (MonsterInLane(impact, cx, cz) < 0) return false; // flew past: not yet
		Detonate(cx, cz, impact.payload, impact.atk.type, impact.attacker);
		return true;
	}
	// A bolt flies down its LANE (the caster's quadrant line): it hits a
	// monster on the same side or in the middle of the cell, and flies
	// straight past one hugging the opposite side (Michael, 2026-07-10).
	// The window is the lateral distance between the bolt's line and the
	// monster's sub-cell slot: 0 same side, cell/4 for a centered (large)
	// body, ~cell/2+ for the opposite quadrant — 0.35 splits hit from miss.
	// (kLaneHalfWidth — shared with the party mirror below.)
	const int hitIndex = MonsterInLane(impact, cx, cz);
	if (hitIndex < 0) return false; // open air (or only wrong-lane bodies) — flies on
	Monster* hit = &m_monsters[static_cast<size_t>(hitIndex)];

	const std::string name = loc::Tr("monster." + hit->kind->name);
	MonsterTarget defender{*this, *hit};
	// The launcher's type axis (docs/damage-system.md "Two axes") — applied here
	// because this is the only place both the roster and the monster list are known.
	fx::DamageEvent ev = fx::DamageEvent::Bolt(
		impact.atk.type,
		m_balance.Potent(impact.atk.damage,
						 AttackerPowers(impact.attacker, impact.shooter),
						 impact.atk.type),
		impact.atk.attackBonus, impact.attacker);
	fx::Deal(ev, defender, m_balance.Strike(), m_combatRng);
	if (ev.hit) {
		if (ev.dealt >= 0.5f)
			onMessage(loc::Format("log.spell_hits", name,
								  static_cast<int>(ev.dealt + 0.5f)));
		else if (ev.dealt >= 0.0f)
			onMessage(loc::Format("log.monster_unharmed", name));
		m_audio.Play(m_sounds.spellImpact, 0.7f);
		fx::React(ev, defender, nullptr, Reaction()); // whatever guards it answers
		if (!hit->Alive()) {
			onMessage(loc::Format("log.spell_slain", name));
		} else {
			hit->hitReq = true; // survivor flinches (a fatal blow goes straight to Die)
			// A survivor wears whatever the carrier left — the payload, in its
			// source's element (see ProjectilePayload::flavour). The bolt's own
			// damage was resolved above; this is what it leaves BEHIND.
			if (!impact.payload.Empty())
				fx::ApplyProcs(defender, impact.payload.Procs(),
							   impact.payload.flavour, impact.attacker, m_effects,
							   m_combatRng);
			// Displacement (the air-school shove): a landed hit with `push` walks
			// the survivor up to that many cells along the bolt's travel, one
			// StepMonsterTo per cell so occupancy commits atomically; the first
			// blocked/occupied cell (FreeSlotInCell covers walls, closed doors,
			// packed cells, and the party's cell) stops it early. The final step
			// wins the visual glide, so the shove reads as one continuous slide.
			if (impact.push > 0) {
				const int dx = impact.dir.x > 0.5f ? 1 : (impact.dir.x < -0.5f ? -1 : 0);
				const int dz = impact.dir.z > 0.5f ? 1 : (impact.dir.z < -0.5f ? -1 : 0);
				int pushed = 0;
				for (int step = 0; step < impact.push; ++step) {
					const int nx = hit->x + dx, nz = hit->z + dz;
					const int slot = FreeSlotInCell(nx, nz, hit->kind->size, hitIndex);
					if (slot < 0) break; // wall / door / occupied — the shove stops
					StepMonsterTo(*hit, nx, nz, slot);
					++pushed;
				}
				if (pushed > 0)
					onMessage(loc::Format("log.spell_pushes", name));
			}
		}
	} else {
		onMessage(loc::Format("log.spell_misses", name));
	}
	return true; // a monster was here, so the bolt is consumed (hit or miss)
}

bool DungeonWorld::ResolveMonsterProjectileHit(const ProjectileImpact& impact) {
	if (!m_roster || m_partyWiped) return false;
	const int cx = static_cast<int>(std::floor(impact.pos.x / kCellSize));
	const int cz = static_cast<int>(std::floor(impact.pos.z / kCellSize));
	if (cx != m_party.GridX() || cz != m_party.GridZ()) return false; // not the party's cell yet

	// Reached the party: the LANE mirror of the party's shots (Michael,
	// 2026-07-10) — the bolt can only strike a standing member whose facing-
	// relative QUADRANT (portraits read front-left/front-right/rear-left/
	// rear-right; PartyMemberSubPos) sits in its lane; with nobody in the lane
	// it flies straight past the party. Consumed once it connects with anyone,
	// hit or miss (like a spell bolt).
	std::array<size_t, 4> inLane;
	size_t n = 0;
	for (size_t i = 0; i < m_roster->size() && i < 4; ++i) {
		if (!(*m_roster)[i].IsAlive()) continue;
		if (std::abs(LaneOffset(impact, PartyMemberSubPos(i))) <= kLaneHalfWidth)
			inLane[n++] = i;
	}
	if (n == 0) return false; // nobody in this lane — the bolt flies past

	// The shooter's grudge picks among the lane's bodies: its THREAT target
	// when the lane offers them, else the in-lane member it hates most, else
	// the old uniform-random pick (no threat, or a stale/slain shooter id).
	size_t pick = inLane[n == 1 ? 0 : m_combatRng() % n];
	if (const Monster* shooter = MonsterByRuntimeId(impact.shooter)) {
		const int want = ThreatTarget(*shooter);
		int best = -1;
		for (size_t k = 0; k < n; ++k) {
			const int i = static_cast<int>(inLane[k]);
			if (i == want) {
				best = i;
				break;
			}
			if (shooter->threat[i] > 0.0f &&
				(best < 0 || shooter->threat[i] > shooter->threat[best]))
				best = i;
		}
		if (best >= 0) pick = static_cast<size_t>(best);
	}
	Character& target = (*m_roster)[pick];
	// The bolt goes through the one pipeline: the Wind Ward turns it aside at
	// the deflect stage (spending a charge), the Water Veil may soak what gets
	// through, and the Fire Shield stays out of it — it burns back at blows,
	// not at bolts. All of that is the effects' business, not this resolver's.
	PartyTarget defender{*this, target};
	fx::DamageEvent ev = fx::DamageEvent::Bolt(
		impact.atk.type,
		m_balance.Potent(impact.atk.damage,
						 AttackerPowers(impact.attacker, impact.shooter),
						 impact.atk.type),
		impact.atk.attackBonus);
	fx::Deal(ev, defender, m_balance.Strike(), m_combatRng);
	TrainDefense(target, ev); // a dodged bolt teaches too
	if (ev.deflected) return true; // spent against the wind
	if (!ev.hit) {
		MemberMessage(target, loc::Format("log.monster_ranged_misses", target.name));
		return true;
	}
	if (ev.dealt >= 0.5f)
		MemberMessage(target, loc::Format("log.monster_ranged_hits", target.name,
										  static_cast<int>(ev.dealt + 0.5f)));
	else if (ev.dealt >= 0.0f)
		MemberMessage(target, loc::Format("log.member_unharmed", target.name));
	defender.NarrateFall();
	m_audio.Play(m_sounds.monster, 0.6f);
	// Whatever the carrier left — applied even if the bolt downed them, matching
	// the melee path: the ticks finish the job.
	if (!impact.payload.Empty())
		fx::ApplyProcs(defender, impact.payload.Procs(), impact.payload.flavour,
					   -1, m_effects, m_combatRng);
	// (a fire shield answers blows, not bolts)
	fx::React(ev, defender, nullptr, Reaction());
	CheckPartyWipe();
	return true;
}

// ============================================================================
// The dungeon as a target
// ============================================================================

float DungeonWorld::BreakableTarget::Resist(DamageType type) const {
	// Authored resists plus whatever is riding it, clamped by the shared rule. A
	// nature cell of 1.0 is immunity and past it the thing DRINKS that element —
	// an iron door fed by lightning — exactly as for a combatant, because it goes
	// through the same clamp.
	float resist = m_brk.resists[type];
	resist += fx::EffectResist(m_brk.effects, type, m_world.EffectKnobs());
	return m_world.m_balance.ClampResist(resist, m_brk.resists[type]);
}

void DungeonWorld::BreakableTarget::Wound(float amount, fx::DamageEvent& ev) {
	if (!m_brk.Alive()) return; // already broken, or never breakable
	const ledger::Explained accounted{m_world.m_damageLedger, m_brk.hp,
									  ledger::Reason::Pipeline};
	m_brk.hp -= amount;
	if (m_brk.hp > 0.0f) return;
	m_brk.hp = 0.0f;
	m_brk.broken = true;
	ev.slew = true; // this is the blow that finished it
	// The CONSEQUENCE is the owner's, not the pipeline's: a door's way opens for
	// good, a prop vanishes, a brazier goes dark.
	if (m_onBroken) m_onBroken();
}

void DungeonWorld::BreakableTarget::Absorb(float amount, fx::DamageEvent& ev) {
	if (!m_brk.Alive() || amount <= 0.0f) return;
	const ledger::Explained accounted{m_world.m_damageLedger, m_brk.hp,
									  ledger::Reason::Pipeline};
	m_brk.hp = std::min(m_brk.maxHp, m_brk.hp + amount);
	if (ev.Quiet()) return; // a tick feeding it is a trickle, not news
	Say(loc::Format("log.monster_absorbs", Name(),
					static_cast<int>(amount + 0.5f)));
}

std::string DungeonWorld::BreakableTarget::Name() const {
	return loc::Tr(m_nameKey);
}

void DungeonWorld::BreakableTarget::Say(const std::string& line) const {
	if (m_world.onMessage) m_world.onMessage(line);
}

void DungeonWorld::BreakableTarget::SayApplied(const fx::EffectKind& kind) const {
	// A door catches fire the way a monster does, not the way a party member
	// does — it is a thing in the world being described, not one of ours.
	if (m_world.onMessage)
		m_world.onMessage(loc::Format(kind.ApplyLine(/*onMonster=*/true), Name()));
}

void DungeonWorld::ForEachBreakableAt(
	int x, int z, const std::function<void(BreakableTarget&)>& fn) {
	// THE one place that knows what pieces of dungeon live in a cell and can be
	// hurt. Every source of damage goes through it, so adding a fourth breakable
	// kind reaches blasts, bolts and anything later, all at once.
	for (Door& d : m_doors) {
		if (d.x != x || d.z != z || !d.brk.Alive()) continue;
		BreakableTarget t{*this, d.brk, "door." + d.type, "log.door_broken", [&d] {
							  // The way is open FOR GOOD. Not `open = true` alone:
							  // a smashed door must not be closeable again, which is
							  // the whole difference from opening one. STATE only —
							  // the line is the caller's (see BrokenKey).
							  d.open = true;
							  d.openT = 1.0f;
						  }};
		fn(t);
	}
	for (Decoration& p : m_decorations) {
		if (p.x != x || p.z != z || !p.brk.Alive()) continue;
		// A smashed prop STAYS IN THE LIST, flagged broken, rather than being
		// erased. Two reasons, the second load-bearing: erasing while the adapter
		// still holds a reference into the vector would dangle it, and the SAVE has
		// to be able to name what was broken afterwards — an erased record cannot be
		// reported. Draw, collision and the map all skip a broken prop instead
		// (Decoration::Gone / ::Blocks), so nothing else has to know.
		BreakableTarget t{*this, p.brk, "decoration." + p.kind->id,
						  "log.prop_broken", nullptr};
		fn(t);
	}
	// FIXTURES, from the side-table rather than the map: their records are static,
	// so their damage state lives beside them (see FixtureBreak). Breaking one puts
	// its light out.
	for (FixtureBreak& fb : m_fixtureBreaks) {
		if (fb.x != x || fb.z != z || !fb.brk.Alive()) continue;
		BreakableTarget t{*this, fb.brk, "fixture." + fb.type, "log.fixture_broken",
						  [this, &fb] { DouseFixture(fb); }};
		fn(t);
	}
}

int DungeonWorld::SmashAt(int x, int z, float amount) {
	int struck = 0;
	// A BLOW, not a burst: rolled and soaked like any swing, so a door's `armor`
	// and its resists both answer it. Bash, because smashing is what this is.
	ForEachBreakableAt(x, z, [&](BreakableTarget& t) {
		++struck;
		fx::DamageEvent ev = fx::DamageEvent::Blow(m_bashType, amount, 500.0f, -1);
		fx::Deal(ev, t, m_balance.Strike(), m_combatRng);
		NarrateBreak(t, ev);
	});
	return struck;
}

void DungeonWorld::NarrateBreak(const BreakableTarget& t,
								const fx::DamageEvent& ev) {
	// The blow FIRST, then what it broke — the order the effects doc insists on and
	// the reason the break line is not said from inside the pipeline.
	if (!onMessage) return;
	if (ev.dealt >= 0.5f)
		onMessage(loc::Format("log.blast_hits", t.Name(),
							  static_cast<int>(ev.dealt + 0.5f)));
	if (ev.slew) onMessage(loc::Format(t.BrokenKey(), t.Name()));
}

void DungeonWorld::SeedFixtureBreakables() {
	// THE TABLE IS DERIVED FROM THE MAP, BUT THE BROKEN FLAGS ARE NOT. Rebuilding it
	// must therefore CARRY THEM OVER, because the rebuild can run after a save has
	// already been applied — which is exactly what happened: a restored broken
	// brazier came back whole, because re-seeding replaced its entry. Preserving
	// here rather than chasing the load-task order means it cannot break again if
	// that order changes.
	std::vector<FixtureBreak> prior = std::move(m_fixtureBreaks);
	m_fixtureBreaks.clear();
	const auto seed = [this, &prior](int x, int z, int wall,
									 const std::string& type) {
		const FixtureKind& k = FixtureKindFor(type);
		if (!k.destructible || k.hp <= 0.0f) return; // authored as scenery
		FixtureBreak fb;
		fb.x = x;
		fb.z = z;
		fb.wall = wall;
		fb.type = type;
		fb.brk.maxHp = k.hp;
		fb.brk.hp = k.hp;
		fb.brk.soak = k.soak;
		fb.brk.resists = k.resists;
		for (const FixtureBreak& old : prior)
			if (old.x == x && old.z == z && old.wall == wall && old.type == type) {
				fb.brk.broken = old.brk.broken; // whatever was already wrecked
				fb.brk.hp = old.brk.hp;         // ...and however battered
				fb.brk.effects = old.brk.effects;
				break;
			}
		m_fixtureBreaks.push_back(std::move(fb));
	};
	for (const WallSconce& s : m_map.Sconces())
		seed(s.x, s.z, static_cast<int>(s.wall), s.type);
	for (const FloorBrazier& b : m_map.Braziers()) seed(b.x, b.z, -1, b.type);
}

void DungeonWorld::DouseFixture(const FixtureBreak& fb) {
	// Breaking a light source means THE LIGHT GOES OUT, which is the whole point of
	// being able to break one — `lit` gates its point light, its flame particles and
	// its smoke together, so one flag does all three. The mesh stays: a wrecked
	// sconce is still bolted to the wall, just dark.
	if (fb.wall >= 0)
		m_map.SetSconceProps(fb.x, fb.z, static_cast<Direction>(fb.wall),
							 /*lit=*/false, kSconceBrightness, kSconceTurbidity);
	else
		m_map.SetBrazierProps(fb.x, fb.z, /*lit=*/false, kBrazierBrightness,
							  kBrazierTurbidity);
	// The haze it was feeding has to go with it, or a doused brazier leaves its own
	// god rays hanging in the air.
	BuildTurbidityMap();
}

void DungeonWorld::SeedBreakable(Breakable& brk, const DecorationKind& kind) {
	if (!kind.destructible || kind.hp <= 0.0f) return; // scenery: maxHp stays 0
	brk.maxHp = kind.hp;
	brk.hp = kind.hp;
	brk.soak = kind.soak;
	brk.resists = kind.resists;
}

// ============================================================================
// Area blasts
// ============================================================================

void DungeonWorld::Detonate(int cx, int cz, const ProjectilePayload& payload,
							DamageType type, int attacker) {
	const BlastSpec& spec = payload.blast;
	if (!spec.rules.Any()) return;

	// What a blast may enter: an open cell, no closed door. The SAME test that
	// stops a bolt, which is why a blast cannot leak into the corridor behind a
	// wall or through a shut door — Game/Blast.h does the geometry, this only says
	// what counts as open.
	ActiveBlast active;
	active.result = blast::Propagate(cx, cz, spec.rules, [this](int x, int z) {
		if (!m_map.IsWalkable(x, z)) return false;
		const Door* d = DoorAt(x, z);
		return !d || d->open;
	});
	if (active.result.clamped)
		log::Warn("a blast of force {} was clamped ({} squares / {} ticks max)",
				  spec.rules.force, blast::kMaxCells, blast::kMaxTicks);
	if (active.result.count == 0) return;

	// The whole propagation is computed at once and PLAYED OUT over time: each
	// tick's hits land `rate` seconds apart, which is the spell's expansion speed
	// (a fireball rushes, a gas cloud creeps). Tick 0 — the detonation square —
	// lands immediately, so a blast always does something on the frame it goes off.
	active.rate = std::max(0.0f, spec.rules.rate);
	active.type = type;
	active.attacker = attacker;
	active.payload = payload; // what every square it reaches is left burning with
	m_activeBlasts.push_back(std::move(active));
	m_audio.Play(m_sounds.spellImpact, 0.9f);
	UpdateBlasts(0.0f); // tick 0 now, not next frame
}

void DungeonWorld::UpdateBlasts(float dt) {
	for (size_t b = 0; b < m_activeBlasts.size();) {
		ActiveBlast& a = m_activeBlasts[b];
		a.elapsed += dt;
		// Everything whose tick has come due. A rate of 0 means the whole thing
		// resolves at once, which is what an instantaneous blast is.
		while (a.next < a.result.count) {
			const blast::Hit& h = a.result.hits[a.next];
			if (a.rate > 0.0f &&
				static_cast<float>(h.tick) * a.rate > a.elapsed + 1e-4f)
				break;
			++a.next;
			ApplyBlastHit(h, a);
		}
		if (a.next >= a.result.count)
			m_activeBlasts.erase(m_activeBlasts.begin() + static_cast<long>(b));
		else
			++b;
	}
}

void DungeonWorld::ApplyBlastHit(const blast::Hit& c,
								 const ActiveBlast& active) {
	// Not named `blast`: that is the namespace this Hit comes from.
	const DamageType type = active.type;
	const int attacker = active.attacker;
	if (c.damage <= 0.0f) return; // reached it, but the falloff spent it
	// The launcher's type axis scales every square the blast touches — one figure
	// for the whole detonation, so a fire-attuned caster's burst is hotter
	// everywhere rather than only where it went off.
	const float dmg = m_balance.Potent(c.damage, AttackerPowers(attacker, 0), type);
	{
		// A blast is MAGIC ARRIVING, so it goes through the pipeline as a Burst:
		// resisted but NOT soaked — plate turns a blade, not a blast
		// (docs/effects.md). It is also not rolled: an explosion filling your square
		// is not something you parry, so no opposed roll and no evasion. That is the
		// trade for the friendly fire below.

		// MONSTERS in the cell — all of them, every lane. A blast has no lane.
		for (Monster& m : m_monsters) {
			if (!m.Alive() || m.x != c.x || m.z != c.z) continue;
			const std::string name = loc::Tr("monster." + m.kind->name);
			MonsterTarget defender{*this, m};
			fx::DamageEvent ev = fx::DamageEvent::Burst(type, dmg, attacker);
			fx::Deal(ev, defender, m_balance.Strike(), m_combatRng);
			if (ev.dealt >= 0.5f)
				onMessage(loc::Format("log.blast_hits", name,
									  static_cast<int>(ev.dealt + 0.5f)));
			else if (ev.dealt >= 0.0f)
				onMessage(loc::Format("log.monster_unharmed", name));
			fx::React(ev, defender, nullptr, Reaction());
			if (!m.Alive()) {
				onMessage(loc::Format("log.spell_slain", name));
			} else {
				m.hitReq = true;
				// WHAT THE FRONT LEAVES: a transient blast passes on, but what it
				// set alight keeps burning through the effects pipeline on its own.
				// That is how "fire that catches" works without the blast itself
				// having to simulate persistence.
				if (!active.payload.Empty())
					fx::ApplyProcs(defender, active.payload.Procs(),
								   active.payload.flavour, attacker, m_effects,
								   m_combatRng);
			}
		}

		// THE PARTY, if this is their cell — FRIENDLY FIRE (Michael, 2026-08-11).
		// Every standing member, because they share the square; the blast does not
		// care who threw it. This is the one path where a party spell can hurt the
		// party, and it is why a blast wants room.
		if (m_roster && !m_partyWiped && c.x == m_party.GridX() &&
			c.z == m_party.GridZ()) {
			for (Character& member : *m_roster) {
				if (!member.IsAlive()) continue;
				PartyTarget defender{*this, member};
				fx::DamageEvent ev = fx::DamageEvent::Burst(type, dmg, -1);
				fx::Deal(ev, defender, m_balance.Strike(), m_combatRng);
				if (ev.dealt >= 0.5f)
					MemberMessage(member, loc::Format("log.blast_hits_member",
													  member.name,
													  static_cast<int>(ev.dealt + 0.5f)));
				else if (ev.dealt >= 0.0f)
					MemberMessage(member, loc::Format("log.member_unharmed",
													  member.name));
				defender.NarrateFall();
				fx::React(ev, defender, nullptr, Reaction());
				if (!active.payload.Empty())
					fx::ApplyProcs(defender, active.payload.Procs(),
								   active.payload.flavour, -1, m_effects,
								   m_combatRng);
			}
			CheckPartyWipe();
		}
	}
	// Whatever pieces of DUNGEON stand here take it too — a blast is the first
	// thing that reaches them, and it reaches them through the same pipeline.
	ForEachBreakableAt(c.x, c.z, [&](BreakableTarget& t) {
		fx::DamageEvent ev = fx::DamageEvent::Burst(type, dmg, attacker);
		fx::Deal(ev, t, m_balance.Strike(), m_combatRng);
		// A door the blast washed over is left ALIGHT, and burns down on its own.
		if (!active.payload.Empty())
			fx::ApplyProcs(t, active.payload.Procs(), active.payload.flavour,
						   attacker, m_effects, m_combatRng);
		NarrateBreak(t, ev);
	});
}

// ============================================================================
// Expiry — a carrier that stopped without striking anything
// ============================================================================

void DungeonWorld::ResolveProjectileExpiry(const ProjectileExpiry& expiry) {
	const int bx = static_cast<int>(std::floor(expiry.pos.x / kCellSize));
	const int bz = static_cast<int>(std::floor(expiry.pos.z / kCellSize));
	// AN AREA CARRIER GOES OFF WHERE IT STOPS, which is the whole point of a
	// thrown bomb: the wall it broke against is the centre. Detonate handles a
	// centre inside stone (nothing stands there, and the room beyond is one step
	// out), so a bolt that burst on a wall still fills the corridor it came down.
	if (expiry.payload.blast.Any()) {
		Detonate(bx, bz, expiry.payload, expiry.atk.type, expiry.attacker);
		return; // the blast IS the effect; no separate cell-wide proc pass
	}
	m_audio.Play(m_sounds.spellFizzle, 0.6f); // the soft fizzle, as before
	if (expiry.payload.Empty()) return;       // a plain bolt just goes out

	// AN EXPIRY IS CELL-WIDE WHERE A HIT IS LANE-WIDE (Michael, 2026-08-11 — he
	// chose "the cell it died in" as an expiry's target). That difference is the
	// mechanic, not an inconsistency: a bolt reaches only what stands in its lane,
	// but a bolt that BURSTS fills the square it burst in — so the shot that flew
	// past you down the far side of the corridor and broke on the wall behind you
	// still catches you. Nothing here reads kLaneHalfWidth, deliberately.
	//
	// A PROC burst stays on its TARGET SIDE: a plain carrier may only ever affect
	// the side it was flying against (TargetSide is that rule everywhere else in
	// the engine). An AREA blast is the deliberate exception and left above — it
	// has no side at all.
	//
	// `expiry.cause` distinguishes bursting on stone from running out of reach; it
	// is carried but not yet acted on — both burst identically today.
	const int cx = bx, cz = bz;

	switch (expiry.target) {
	case TargetSide::Monsters:
		for (Monster& m : m_monsters) {
			if (!m.Alive() || m.x != cx || m.z != cz) continue;
			MonsterTarget caught{*this, m};
			fx::ApplyProcs(caught, expiry.payload.Procs(), expiry.payload.flavour,
						   expiry.attacker, m_effects, m_combatRng);
		}
		break;
	case TargetSide::Party:
		if (!m_roster || m_partyWiped) break;
		if (cx != m_party.GridX() || cz != m_party.GridZ()) break;
		for (Character& member : *m_roster) {
			if (!member.IsAlive()) continue;
			PartyTarget caught{*this, member};
			fx::ApplyProcs(caught, expiry.payload.Procs(), expiry.payload.flavour,
						   -1, m_effects, m_combatRng);
		}
		break;
	}
}

} // namespace dungeon::game
