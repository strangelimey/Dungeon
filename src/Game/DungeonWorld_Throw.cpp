// ============================================================================
// Game/DungeonWorld_Throw.cpp - throwing (ui-updates Phase 10).
//
// Anything the party holds can be thrown, by the party LEADER (Phase 9): a click
// with an item on the cursor that meets no reachable floor - above the floor's
// horizon, on a wall, past reach - throws it straight ahead (Grimrock's
// screen-height rule; DropItemAt decides, Game calls ThrowItem when it says no).
//
// A THROW IS AN ATTACK (Michael: "throwing an item feeds through the same attack
// pipeline as the hand controls"). Its profile is the swing's formula
// (PartyAttackProfile) with the `throwing` skill; the ATTACK it flies as - the
// item's `throw`, a weapon's first command, else `throw` (bash) - types it and
// shades its numbers, so a stone bludgeons and a throwing star (`throw = stab`)
// pierces; it is potent with what the thrower wears and what the thing itself
// is; and its blow goes through fx::Deal with the swing's crit, fumble and
// enchantment rules. Its SPEED is the thrower's skill against the thing's
// weight. What it LEAVES is the item's: its `on_hit` effects on what it strikes,
// or a `throw_spell`'s whole payload - a fire flask bursts into firebolt_burst's
// blast.
//
// The flight is an ordinary moving item in the shared engine (Projectiles.h)
// carrying the item's kind as CARGO, so it flies, walls stop it and monsters in
// its lane take it exactly as they take a bolt, down the thrower's quadrant
// lane as a cast does. It draws as the item itself, tumbling (DungeonWorld_
// Render). And it is NEVER LOST: it comes down where its flight ends - in the
// struck monster's square, in front of the wall it hit, or where it ran out of
// reach - unless it is a thing that shatters (`throw_breaks`), and anything
// that would clear flights mid-air (a save, a level change, the inspector's
// Remove) lands it first (LandCargo).
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Core/Loc.h"
#include "Game/Blast.h"
#include "Game/Defense.h"
#include "Game/Facing.h"

#include <algorithm>
#include <cmath>

namespace dungeon::game {

namespace {
// The skill a throw trains. The stats a non-weapon throw averages (and creeps on
// a landed hit) are Balance's ThrowStats: a throw is arm and eye.
constexpr std::string_view kThrowSkill = "throwing";
} // namespace

bool DungeonWorld::ThrowItem(const std::string& typeId, int member, float charge) {
	if (member < 0) member = m_leader;
	if (!m_roster || member < 0 || member >= static_cast<int>(m_roster->size()))
		return false;
	Character& thrower = (*m_roster)[static_cast<size_t>(member)];
	if (!thrower.IsAlive()) return false; // nobody standing (Game gates the leader too)
	const size_t who = static_cast<size_t>(member);
	if (who < m_throwCooldown.size() && m_throwCooldown[who] > 0.0f) {
		MemberMessage(thrower, loc::FormatLine("log.throw_wait", thrower.name));
		return false;
	}

	const ItemKind& kind = ItemKindFor(typeId);
	const bool weapon = kind.damage > 0.0f;
	// The ATTACK it flies as, which types it: its own `throw`, else a weapon's
	// first command, else the plain `throw` (bash).
	std::string_view attackId = "throw";
	if (!kind.throwAttack.empty()) attackId = kind.throwAttack;
	else if (weapon && !kind.commands.empty()) attackId = kind.commands.front();
	const AttackSpec* spec = m_balance.FindAttack(attackId);
	if (!spec) spec = &m_balance.Neutral();
	const std::span<const std::string> stats =
		weapon && !kind.stats.empty() ? std::span<const std::string>(kind.stats)
									  : std::span<const std::string>(ThrowStats());
	const int level = thrower.SkillLevel(kThrowSkill);
	const float base =
		weapon ? kind.damage : m_balance.throwBase + m_balance.throwWeight * kind.weight;
	// The swing's own formula, so a throw and a blow can never drift apart.
	AttackProfile atk =
		PartyAttackProfile(thrower, &kind, *spec, level, thrower.StatAvg(stats), base);
	// Potent with what the thrower WEARS (no hand: nothing is wielded) and with
	// what the thrown thing itself is.
	ResistTable powers = PartyPowers(thrower, -1);
	powers.Add(kind.powers);
	atk.damage = m_balance.Potent(atk.damage, powers, atk.type);

	// Down the THROWER's quadrant lane along the grid facing - the cast's rule
	// (CastSpell), so a throw from the left pair flies the left side.
	const Direction faced = static_cast<Direction>(m_party.Facing());
	const Direction lateral =
		static_cast<Direction>(facing::SlotSide(static_cast<int>(faced), who));
	Vec3 origin = m_party.EyePosition();
	origin.x += static_cast<float>(DirDX(lateral)) * (kCellSize * 0.25f);
	origin.z += static_cast<float>(DirDZ(lateral)) * (kCellSize * 0.25f);
	origin.y -= 0.15f * kUnit; // from the hand, a little under the eye

	ProjectileSpec flight;
	flight.pos = origin;
	flight.dir = {static_cast<float>(DirDX(faced)), 0.0f, static_cast<float>(DirDZ(faced))};
	// SPEED: the thrower's skill against the thing's weight (Michael).
	flight.speed = std::max(m_balance.throwSpeedMin,
							m_balance.throwSpeed +
								m_balance.throwSpeedSkill * static_cast<float>(level) -
								m_balance.throwSpeedWeight * kind.weight);
	flight.range = m_balance.throwRange * kCellSize;
	flight.atk = atk;
	flight.color = {0.35f, 0.32f, 0.28f, 0.0f}; // its sparks: a puff of grit
	flight.size = 0.0f;
	flight.target = TargetSide::Monsters;
	flight.attacker = member;
	flight.payload = kind.throwPayload;
	flight.cargo = &kind; // the kinds are stable (PreloadItemKinds)
	flight.cargoCharge = charge; // and it lands with what it had
	// It lights if its kind does (a lit torch keeps lighting the corridor it
	// flies down) and sheds its kind's `trail` - Launch dresses it.
	Launch(flight);

	// What a throw costs, as a swing does: the wait (paced by the attack), the
	// stamina (by weight, shaded by the attack) and any over-exertion.
	if (who < m_throwCooldown.size())
		m_throwCooldown[who] = m_balance.throwInterval * spec->pace;
	SpendStamina(thrower, (m_balance.throwStamina + m_balance.staminaWeight * kind.weight) *
							  spec->stam);
	SpendExertion(thrower,
				  defense::ExertionPoints(thrower.offenseShare, static_cast<float>(level),
										  m_balance.SkillCurve(), m_balance.Stance()));
	++m_harness.tally.throws;
	m_audio.Play(m_sounds.click, 0.5f); // placeholder whoosh
	MemberMessage(thrower, loc::FormatLine("log.throw", thrower.name, loc::View(kind.nameKey)));
	return true;
}

bool DungeonWorld::ResolveThrowHit(const ProjectileImpact& impact) {
	const int cx = static_cast<int>(std::floor(impact.pos.x / kCellSize));
	const int cz = static_cast<int>(std::floor(impact.pos.z / kCellSize));
	const int index = MonsterInLane(impact, cx, cz);
	if (index < 0) return false; // open air, or only a body on the far side
	const ItemKind& kind = *static_cast<const ItemKind*>(impact.cargo);
	++m_harness.tally.throwStrikes;
	// Whatever happens, the thing comes down here - or shatters here.
	const auto comeDown = [&] {
		if (!kind.throwBreaks) DropItemInCell(kind.id, cx, cz, impact.cargoCharge);
	};
	// A thing that BURSTS (a flask carrying a blast) bursts on contact, and the
	// blast is the whole of what it does - the area-carrier rule.
	if (impact.payload.blast.Any()) {
		Detonate(cx, cz, impact.payload, kind.throwBlastType, impact.attacker);
		comeDown();
		return true;
	}

	Monster& hit = m_monsters[static_cast<size_t>(index)];
	Character* thrower =
		m_roster && impact.attacker >= 0 && impact.attacker < static_cast<int>(m_roster->size())
			? &(*m_roster)[static_cast<size_t>(impact.attacker)]
			: nullptr;
	const std::string_view who = thrower ? std::string_view(thrower->name) : std::string_view();
	const loc::Line name = loc::ViewKey("monster.", hit.kind->name);
	MonsterTarget defender{*this, hit};

	// THE BLOW, as a swing's: rolled, soaked, resisted, with its crit and the
	// over-exertion fumble band.
	fx::DamageEvent ev = fx::DamageEvent::Blow(impact.atk.type, impact.atk.damage,
											   impact.atk.attackBonus, impact.attacker);
	ev.pierceOnCrit = impact.atk.pierceOnCrit;
	ev.fumbleExtra = impact.atk.fumbleExtra;
	fx::Deal(ev, defender, m_balance.Strike(), m_combatRng);
	if (thrower) {
		if (ev.fumble) MemberMessage(*thrower, loc::FormatLine("log.fumble", thrower->name));
		else if (ev.crit && ev.hit) MemberMessage(*thrower, loc::FormatLine("log.critical", thrower->name));
	}
	if (!ev.hit) {
		onMessage(loc::FormatLine("log.throw_misses", who, loc::View(kind.nameKey), name));
		comeDown();
		return true;
	}
	// An ENCHANTED weapon carries its element through on a landed throw too - but
	// not into a monster the blow just killed (code-review C5), as at the door.
	float elemental = 0.0f;
	if (!ev.slew && kind.enchanted && kind.elementBonus > 0.0f) {
		const DamageType elemType = m_damageTypes.ForSchool(kind.element);
		fx::DamageEvent burst = fx::DamageEvent::Burst(elemType,
													   impact.atk.damage * kind.elementBonus,
													   impact.attacker);
		fx::Deal(burst, defender, m_balance.Strike(), m_combatRng);
		elemental = burst.dealt;
	}
	const float landed = ev.dealt + elemental;
	if (landed >= 0.5f)
		onMessage(loc::FormatLine("log.throw_hits", who, loc::View(kind.nameKey), name,
								  static_cast<int>(landed + 0.5f)));
	else
		onMessage(loc::FormatLine("log.monster_unharmed", name));
	m_audio.Play(m_sounds.monster, 0.7f);
	// A landed throw trains the thrower, and creeps the stats it used.
	if (thrower) {
		const std::span<const std::string> stats =
			kind.damage > 0.0f && !kind.stats.empty() ? std::span<const std::string>(kind.stats)
													  : std::span<const std::string>(ThrowStats());
		GrantSkillXp(*thrower, kThrowSkill, 1.0f, stats);
	}
	fx::React(ev, defender, nullptr, Reaction()); // no reprisal reaches across the room
	if (!hit.Alive()) {
		onMessage(loc::FormatLine("log.monster_slain", name));
	} else {
		hit.hitReq = true;
		// A survivor wears what the thing leaves - its on_hit effects.
		if (!impact.payload.Empty())
			fx::ApplyProcs(defender, impact.payload.Procs(), impact.payload.flavour,
						   impact.attacker, m_effects, m_combatRng, impact.payload.Tint());
	}
	comeDown();
	return true;
}

bool DungeonWorld::StrikeDoorWithThrow(int cx, int cz, const ProjectileExpiry& expiry) {
	// A thrown thing that meets a SHUT DOOR hits it, as a bolt does
	// (StrikeDoorWithBolt) - and on the same terms: only a door doors.cat made
	// `breakable` is a target at all, and its armour and resists decide the
	// rest, so a rock batters a wooden door and does nothing to a stone one.
	Door* d = DoorAt(cx, cz);
	if (!d || d->open || !d->brk.Alive()) return false;
	const ItemKind& kind = *static_cast<const ItemKind*>(expiry.cargo);
	BreakableTarget t = DoorTarget(*d);
	Character* thrower =
		m_roster && expiry.attacker >= 0 && expiry.attacker < static_cast<int>(m_roster->size())
			? &(*m_roster)[static_cast<size_t>(expiry.attacker)]
			: nullptr;
	const std::string_view who = thrower ? std::string_view(thrower->name) : std::string_view();
	// THE BLOW the throw carries (its potency already applied at the throw),
	// soaked and resisted - but NOT ROLLED: it has already met the panel, so there
	// is nothing to miss, and an inert door would otherwise "win" the opposed roll
	// about half the time. No skill is trained: a door is not an opponent.
	fx::DamageEvent ev = fx::DamageEvent::Blow(expiry.atk.type, expiry.atk.damage,
											   expiry.atk.attackBonus, expiry.attacker);
	ev.rolled = false;
	fx::Deal(ev, t, m_balance.Strike(), m_combatRng);
	// An ENCHANTED weapon carries its element through, as on a monster.
	float landed = ev.dealt;
	if (!ev.slew && kind.enchanted && kind.elementBonus > 0.0f) {
		fx::DamageEvent burst = fx::DamageEvent::Burst(m_damageTypes.ForSchool(kind.element),
													   expiry.atk.damage * kind.elementBonus,
													   expiry.attacker);
		fx::Deal(burst, t, m_balance.Strike(), m_combatRng);
		landed += burst.dealt;
		ev.slew = ev.slew || burst.slew;
	}
	m_audio.Play(m_sounds.click, 0.6f); // placeholder thud
	if (onMessage) {
		if (landed >= 0.5f)
			onMessage(loc::FormatLine("log.throw_hits", who, loc::View(kind.nameKey), t.Name(),
									  static_cast<int>(landed + 0.5f)));
		else
			onMessage(loc::FormatLine("log.monster_unharmed", t.Name()));
		if (ev.slew) onMessage(loc::FormatLine(t.BrokenKey(), t.Name()));
	}
	// What the thing leaves on what it strikes - its on_hit effects.
	if (!ev.slew && !expiry.payload.Empty())
		fx::ApplyProcs(t, expiry.payload.Procs(), expiry.payload.flavour, expiry.attacker,
					   m_effects, m_combatRng, expiry.payload.Tint());
	return true;
}

void DungeonWorld::LandThrown(const ProjectileExpiry& expiry) {
	const ItemKind& kind = *static_cast<const ItemKind*>(expiry.cargo);
	// It stopped AGAINST a shut door: the door takes the blow first. Not a thing
	// carrying a blast - a fire flask bursts in front of the door below, and that
	// blast reaches the door's face itself (the area-carrier rule: the blast is
	// the whole of what it does, as when one connects with a monster).
	if (expiry.cause == ExpiryCause::Wall && !expiry.payload.blast.Any())
		StrikeDoorWithThrow(static_cast<int>(std::floor(expiry.pos.x / kCellSize)),
							static_cast<int>(std::floor(expiry.pos.z / kCellSize)), expiry);
	// Where it comes down: the last OPEN square along the flight
	// (blast::LastOpenCell, the walk-back a bolt's end shares through FlightEnd) -
	// a wall's square (or a shut door's) is never one, the flight has already
	// stepped into it when it stops. A thing that SHATTERS bursts in any open
	// square (OpenSquare, a bolt's rule), over a pit as over floor, and is not
	// pulled back toward the thrower. A thing that LANDS must also be able to
	// REST there (ItemCanRest, the drop's own rule), so it comes down short of a
	// pit or a stairwell rather than hang over the shaft (code-review C74). And
	// never past the PARTY'S OWN square, whatever is under it: the party can
	// stand on a hole (a stairwell it has just come up, a pit barred by a flag),
	// and backing off past it would land the thing behind the thrower. With
	// nothing open within reach it comes down in the party's square.
	const int px = m_party.GridX(), pz = m_party.GridZ();
	int cx = px, cz = pz;
	blast::LastOpenCell(expiry.pos.x, expiry.pos.z, expiry.dir.x, expiry.dir.z, kCellSize,
						[&](int x, int z) {
							if (x == px && z == pz) return true;
							return kind.throwBreaks ? OpenSquare(x, z) : ItemCanRest(x, z);
						},
						cx, cz);
	++m_harness.tally.throwLandings;
	m_harness.tally.landX = cx;
	m_harness.tally.landZ = cz;
	// A thing that SHATTERS lets go of what it carries where it stops - a fire
	// flask's blast, a poison flask's cloud - as a spent bolt does.
	if (kind.throwBreaks) {
		if (!expiry.payload.Empty() || expiry.payload.blast.Any()) {
			ProjectileExpiry burst = expiry;
			burst.pos = m_map.CellCenter(cx, cz);
			burst.atk.type = kind.throwBlastType; // fire burns as fire, gas as poison
			ResolveProjectileExpiry(burst);
		}
		return;
	}
	DropItemInCell(kind.id, cx, cz, expiry.cargoCharge);
	m_audio.Play(m_sounds.click, 0.4f); // placeholder thud
}

} // namespace dungeon::game
