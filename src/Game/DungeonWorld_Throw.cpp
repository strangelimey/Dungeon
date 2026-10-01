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
// or a `throw_spell`'s whole payload - a fire flask bursts into fireburst's
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
#include "Game/Defense.h"

#include <algorithm>
#include <cmath>

namespace dungeon::game {

namespace {
// The skill a throw trains, and the stats a non-weapon throw averages (and
// creeps on a landed hit): a throw is arm and eye. Built at start-up, not on the
// first throw - that lands in a frame the allocation guard watches.
constexpr std::string_view kThrowSkill = "throwing";
const std::vector<std::string> kThrowStats{"str", "dex"};
} // namespace

bool DungeonWorld::ThrowItem(const std::string& typeId) {
	if (!m_roster || m_leader < 0 || m_leader >= static_cast<int>(m_roster->size()))
		return false;
	Character& thrower = (*m_roster)[static_cast<size_t>(m_leader)];
	if (!thrower.IsAlive()) return false; // nobody standing leads (Game gates too)
	const size_t who = static_cast<size_t>(m_leader);
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
									  : std::span<const std::string>(kThrowStats);
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
	const Direction lateral = static_cast<Direction>(
		(static_cast<int>(faced) + (who % 2 == 0 ? 3 : 1)) % 4);
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
	flight.attacker = m_leader;
	flight.payload = kind.throwPayload;
	flight.cargo = &kind; // the kinds are stable (PreloadItemKinds)
	m_projectiles.Spawn(flight);

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
		if (!kind.throwBreaks) DropItemInCell(kind.id, cx, cz);
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
	// An ENCHANTED weapon carries its element through on a landed throw too.
	float elemental = 0.0f;
	if (kind.enchanted && kind.elementBonus > 0.0f) {
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
													  : std::span<const std::string>(kThrowStats);
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
						   impact.attacker, m_effects, m_combatRng);
	}
	comeDown();
	return true;
}

void DungeonWorld::LandThrown(const ProjectileExpiry& expiry) {
	const ItemKind& kind = *static_cast<const ItemKind*>(expiry.cargo);
	// The last OPEN square along the flight: a wall's square (or a shut door's)
	// is never one - the flight has already stepped into it when it stops - so
	// back off along it, half a square at a time, to the party's own at worst.
	Vec3 p = expiry.pos;
	int cx = m_party.GridX(), cz = m_party.GridZ();
	for (int step = 0; step < 16; ++step) {
		const int x = static_cast<int>(std::floor(p.x / kCellSize));
		const int z = static_cast<int>(std::floor(p.z / kCellSize));
		const Door* door = DoorAt(x, z);
		if (m_map.IsWalkable(x, z) && !(door && !door->open)) {
			cx = x;
			cz = z;
			break;
		}
		p.x -= expiry.dir.x * kCellSize * 0.5f;
		p.z -= expiry.dir.z * kCellSize * 0.5f;
	}
	++m_harness.tally.throwLandings;
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
	DropItemInCell(kind.id, cx, cz);
	m_audio.Play(m_sounds.click, 0.4f); // placeholder thud
}

} // namespace dungeon::game
