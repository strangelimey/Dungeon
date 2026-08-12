// ============================================================================
// Game/DungeonWorld_Combat.cpp — split out of DungeonWorld.cpp to keep files
// small (see DungeonWorld.h). Holds monster action execution + the combat
// model: formation-step helpers, wound/skill/stamina, the attack formula
// (party + monster melee/ranged), spell casting, and projectile-hit resolution.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Game/Curve.h"

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

	float& total = member.skillXp[std::string(skillId)];
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
	if (!ev.rolled || !member.IsAlive()) return;
	const ArmorClass worn = WornArmorClass(member);
	constexpr float kXp = 1.0f; // the same unit a landed blow trains at

	if (!ev.hit) {
		if (worn != ArmorClass::None) return; // armor took the swing, not skill
		static const std::vector<std::string> kDex{"dexterity"};
		GrantSkillXp(member, kAvoidSkill, kXp, kDex);
		return;
	}
	// A landed blow only teaches the armor something if the armor was there to
	// blunt it.
	if (worn == ArmorClass::None) return;
	const float soak = PartyTarget{*this, member}.Soak();
	if (soak <= 0.0f) return;
	static const std::vector<std::string> kDexStat{"dexterity"};
	static const std::vector<std::string> kStrStat{"strength"};
	GrantSkillXp(member, ArmorSkillId(worn), kXp * m_balance.Armor(worn).learn,
				 worn == ArmorClass::Heavy ? kStrStat : kDexStat);
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
	member.RecomputeMaxima(m_balance.kHealth, m_balance.kStamina, m_balance.kMana);
	MemberMessage(member, loc::Format("log.stat_up", member.name,
									  loc::Tr("stat." + std::string(stat)), value));
}

// Exertion (docs/combat.md part 3 + Phase 4): stamina is the exertion meter —
// every point spent feeds VIT's creep pool, holds regen off for a beat, and
// an emptied bar latches EXHAUSTED (the swing penalties; cleared with
// hysteresis in the regen tick). Swings and marching both arrive here.
void DungeonWorld::SpendStamina(Character& member, float points) {
	if (points <= 0.0f || !member.IsAlive()) return;
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
	if (member.stamina <= 0.0f) {
		member.stamina = 0.0f;
		if (!member.exhausted) {
			member.exhausted = true;
			MemberMessage(member, loc::Format("log.exhausted", member.name));
		}
	}
	float& pool = member.statProgress["vitality"];
	pool += points * m_balance.vitExertion;
	if (pool < 1.0f) return;
	pool -= 1.0f;
	GrantStatPoint(member, "vitality");
}

void DungeonWorld::RecomputePartyMaxima() {
	if (!m_roster) return;
	for (Character& member : *m_roster)
		member.RecomputeMaxima(m_balance.kHealth, m_balance.kStamina,
							   m_balance.kMana);
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
float DungeonWorld::ArmorPenalty(const Character& member, ArmorClass c) const {
	if (c == ArmorClass::None) return 0.0f;
	const Balance& b = m_balance;
	const Balance::ArmorRules r = b.Armor(c);

	// THE FLOOR IS THE CURVE'S CAP, not a clamp bolted on afterwards: a
	// hyperbolic curve approaches its ceiling and never arrives, so "no matter
	// how much you practise, plate still makes you easier to hit" is a property
	// of the maths rather than a rule enforced beside it.
	CurveRules offsetCurve = b.SkillCurve();
	offsetCurve.slope = b.armorOffsetSlope;
	offsetCurve.cap = r.Offsettable();
	const float level =
		static_cast<float>(member.SkillLevel(ArmorSkillId(c)));
	float penalty = r.floor + (r.Offsettable() - CurveValue(level, offsetCurve));

	// Too weak for it: easier to hit AND quickly spent (the stamina half is in
	// SpendStamina). The same story told twice, which is the point.
	const float short_ = r.strength - static_cast<float>(member.strength);
	if (short_ > 0.0f) penalty += short_ * b.armorShortPenalty;
	return penalty;
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

float DungeonWorld::PartyTarget::Evasion(DamageType type) const {
	const Balance& b = m_world.m_balance;
	// The innate floor plus DEX.
	float guard = b.defenseBase +
				  CurveValue(static_cast<float>(m_member.dexterity), b.StatCurve());

	// ARMOR (docs/damage-system.md). Wearing anything costs you the roll and
	// pays you back in soak; wearing NOTHING is the only way the `avoid` skill
	// applies at all, which is what makes going bare a real build rather than
	// simply the poor man's option.
	const ArmorClass worn = m_world.WornArmorClass(m_member);
	if (worn == ArmorClass::None)
		guard += CurveValue(static_cast<float>(m_member.SkillLevel(kAvoidSkill)),
							b.AvoidCurve());
	else
		guard -= m_world.ArmorPenalty(m_member, worn);

	// THE HELD-BACK SKILL (docs/damage-system.md). The character keeps
	// (1 - offenseShare) of their skill back to defend with — ONE stance, not
	// one per hand. What that guard is worth depends on what is arriving:
	//
	//   PHYSICAL   parried with a HAND, off its weapon class — so the two
	//              hands can differ, and the better of them answers the blow.
	//              An empty hand parries `unarmed`: bare-handed, but not
	//              nothing.
	//   MAGICAL    warded with the SKILL IN THE INCOMING SCHOOL. Knowing fire
	//              is what lets you turn fire aside, so a fire specialist
	//              shrugs off a firebolt and is no better than anyone else
	//              against frost. The HANDS ARE IRRELEVANT here — which is
	//              worth stating plainly, because the alternative rule (the
	//              guard belongs to a hand and covers only the school that
	//              hand is set to cast) is the one this comment used to
	//              describe, and it is NOT what happens.
	//
	// THE HANDS COMBINE BY MAX, NOT SUM. Summing would make holding both
	// hands back strictly better than one and turn the slider into a free
	// defense button; taking the best guard keeps it an honest trade.
	//
	// The cooldown is deliberately NOT consulted: a stance is not an action,
	// so a hand still guards while it recovers from a swing.
	const float held = 1.0f - m_member.offenseShare;
	if (held <= 0.0f) return guard; // all-out attack guards with nothing

	const auto skillGuard = [&](std::string_view skillId) {
		return held * CurveValue(static_cast<float>(m_member.SkillLevel(skillId)),
								 b.SkillCurve());
	};

	// Magic: one lookup, no hands. (It sat inside the hand loop once, which
	// computed the same number twice and took the max of it with itself.)
	if (SpellSymbol school{}; m_world.m_damageTypes.SchoolOf(type, school))
		return guard + skillGuard(SymbolId(school));

	if (!m_world.m_damageTypes.IsPhysical(type))
		return guard; // neither physical nor a school: nothing to parry it with

	float best = 0.0f;
	for (int hand = 0; hand < 2; ++hand) {
		const ItemSlot& slot = m_member.inventory.Hand(hand);
		const ItemKind* weapon =
			slot.Empty() ? nullptr : &m_world.ItemKindFor(slot.typeId);
		best = std::max(best, skillGuard(weapon ? std::string_view(weapon->skill)
												: std::string_view("unarmed")));
	}
	return guard + best;
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
	m_fall = m_world.WoundMember(m_member, amount, ev.Quiet());
	ev.slew = m_fall != Fall::None;
}

// Fed rather than hurt: a member whose nature DRINKS this element (a resist
// past 1). It cannot raise the dead — a corpse drinks nothing — but it will
// bring someone back from unconscious, which is the point of being made of the
// stuff that was just thrown at you.
void DungeonWorld::PartyTarget::Absorb(float amount, fx::DamageEvent& ev) {
	if (m_member.dead || amount <= 0.0f) return;
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
	m_monster.hp -= amount;
	if (ev.source >= 0)
		m_world.AddThreat(m_monster, static_cast<size_t>(ev.source), amount);
	// A per-frame tick doesn't re-provoke or re-flinch every frame; anything
	// else wakes the monster and turns it on whoever struck.
	if (!ev.Quiet()) m_world.ProvokeMonster(m_monster);
	if (!m_monster.Alive()) {
		m_monster.hp = 0.0f; // a downed monster stays in the list (save restore)
		Extinguish(m_monster); // a corpse stops burning
		ev.slew = true;
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
	fx::DamageEvent ev = fx::DamageEvent::Blow(
		monster.kind->damageType, monster.kind->damage,
		monster.kind->accuracy * monster.kind->offense, victim);
	fx::Deal(ev, defender, m_balance.Strike(), m_combatRng);
	TrainDefense(target, ev); // avoid on a miss, armor on a blunted hit

	if (ev.fumble)
		MemberMessage(target, loc::Format("log.foe_fumbles", name));
	else if (ev.crit && ev.hit)
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
	if (!target) {
		MemberMessage(attacker, loc::Tr("log.attack_air"));
		return true;
	}

	const AttackProfile atk{
		(base + m_balance.statDamage * statAvg) * spec->dmg *
			(1.0f + m_balance.skillDamage * static_cast<float>(level)) *
			(winded ? m_balance.exhaustDamage : 1.0f),
		// THE ATTACK BONUS (docs/damage-system.md): skill is the main driver,
		// through its diminishing-returns curve; DEX shades it through its own,
		// much shallower one; the verb adds its authored points. All three are
		// in d100 points, against the ~41 the dice themselves deviate by.
		CurveValue(static_cast<float>(level), m_balance.SkillCurve()) +
			CurveValue(static_cast<float>(attacker.dexterity),
					   m_balance.StatCurve()) +
			spec->acc,
		spec->type};
	const std::string name = loc::Tr("monster." + target->kind->name);
	PartyTarget striker{*this, attacker};
	MonsterTarget defender{*this, *target};
	fx::DamageEvent ev = fx::DamageEvent::Blow(atk.type, atk.damage, atk.attackBonus,
											   static_cast<int>(member));
	fx::Deal(ev, defender, m_balance.Strike(), m_combatRng);

	// WHAT THE DICE DID, said before the outcome it caused. The open-ended roll
	// has been driving damage since P2 and was invisible: a critical arrived as
	// a big number with no explanation, and a fumble as an ordinary miss. The
	// line is the point — a mechanic nobody can see is a mechanic nobody has.
	if (ev.fumble)
		MemberMessage(attacker, loc::Format("log.fumble", attacker.name));
	else if (ev.crit && ev.hit)
		MemberMessage(attacker, loc::Format("log.critical", attacker.name));
	else if (ev.defenderFumbled)
		MemberMessage(attacker, loc::Format("log.foe_fumbles", name));

	if (!ev.hit) {
		MemberMessage(attacker, loc::Format("log.party_misses", attacker.name, name));
		return true;
	}
	// ENCHANTMENT: a landed blow with an elemental weapon carries its element
	// through as well — a SECOND event of that element, `element_bonus` of the
	// assembled damage. It rides the physical hit, so it is neither rolled nor
	// soaked (plate turns a blade, not a flame) but the target's resist for the
	// element still answers it.
	float elemental = 0.0f;
	if (weapon && weapon->enchanted && weapon->elementBonus > 0.0f) {
		fx::DamageEvent burst = fx::DamageEvent::Burst(
			m_damageTypes.ForSchool(weapon->element), atk.damage * weapon->elementBonus,
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
	return true;
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
		m_audio.Play(m_sounds.spellCast, 0.7f);
		return true;
	case MagicSystem::CastOutcome::Fumble:
		// The skill roll failed: the mana is spent, nothing happens, and
		// nothing is learned — the recipe stays anonymous until a cast lands.
		MemberMessage(caster, loc::Format("log.cast_fumble", caster.name));
		m_audio.Play(m_sounds.spellFizzle, 0.6f);
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

bool DungeonWorld::ResolveSpellHit(const ProjectileImpact& impact) {
	const int cx = static_cast<int>(std::floor(impact.pos.x / kCellSize));
	const int cz = static_cast<int>(std::floor(impact.pos.z / kCellSize));
	// A bolt flies down its LANE (the caster's quadrant line): it hits a
	// monster on the same side or in the middle of the cell, and flies
	// straight past one hugging the opposite side (Michael, 2026-07-10).
	// The window is the lateral distance between the bolt's line and the
	// monster's sub-cell slot: 0 same side, cell/4 for a centered (large)
	// body, ~cell/2+ for the opposite quadrant — 0.35 splits hit from miss.
	// (kLaneHalfWidth — shared with the party mirror below.)
	Monster* hit = nullptr;
	int hitIndex = -1;
	for (size_t i = 0; i < m_monsters.size(); ++i) {
		Monster& m = m_monsters[i];
		if (!m.Alive() || m.x != cx || m.z != cz) continue;
		const Vec3 mp = SlotCenter(m.x, m.z, m.kind->size, m.slot);
		if (std::abs(LaneOffset(impact, mp)) > kLaneHalfWidth)
			continue; // opposite side
		hit = &m;
		hitIndex = static_cast<int>(i);
		break;
	}
	if (!hit) return false; // open air (or only wrong-lane bodies) — flies on

	const std::string name = loc::Tr("monster." + hit->kind->name);
	MonsterTarget defender{*this, *hit};
	fx::DamageEvent ev = fx::DamageEvent::Bolt(impact.atk.type, impact.atk.damage,
											   impact.atk.attackBonus,
											   impact.attacker);
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
	fx::DamageEvent ev = fx::DamageEvent::Bolt(impact.atk.type, impact.atk.damage,
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
// Expiry — a carrier that stopped without striking anything
// ============================================================================

void DungeonWorld::ResolveProjectileExpiry(const ProjectileExpiry& expiry) {
	m_audio.Play(m_sounds.spellFizzle, 0.6f); // the soft fizzle, as before
	if (expiry.payload.Empty()) return;       // a plain bolt just goes out

	// AN EXPIRY IS CELL-WIDE WHERE A HIT IS LANE-WIDE (Michael, 2026-08-11 — he
	// chose "the cell it died in" as an expiry's target). That difference is the
	// mechanic, not an inconsistency: a bolt reaches only what stands in its lane,
	// but a bolt that BURSTS fills the square it burst in — so the shot that flew
	// past you down the far side of the corridor and broke on the wall behind you
	// still catches you. Nothing here reads kLaneHalfWidth, deliberately.
	//
	// It stays on its TARGET SIDE: a carrier may only ever affect the side it was
	// flying against (TargetSide is that rule everywhere else in the engine), so a
	// burst cannot friendly-fire. Whether it SHOULD is a live design question and
	// is deliberately not answered here.
	//
	// `expiry.cause` distinguishes bursting on stone from running out of reach; it
	// is carried but not yet acted on — both burst identically today.
	const int cx = static_cast<int>(std::floor(expiry.pos.x / kCellSize));
	const int cz = static_cast<int>(std::floor(expiry.pos.z / kCellSize));

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
