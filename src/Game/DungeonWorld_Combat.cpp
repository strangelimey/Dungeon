// ============================================================================
// Game/DungeonWorld_Combat.cpp — split out of DungeonWorld.cpp to keep files
// small (see DungeonWorld.h). Holds monster action execution + the combat
// model: formation-step helpers, wound/skill/stamina, the attack formula
// (party + monster melee/ranged), spell casting, and projectile-hit resolution.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Assets/File.h"
#include "Assets/Image.h"
#include "Core/Loc.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Game/AssetUtil.h"
#include "Game/DungeonMeshBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>

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

void DungeonWorld::WoundMember(Character& target, float damage, bool quiet) {
	// Water Veil: water guards by ABSORBING — the ward soaks damage into its
	// pool (the ward's magnitude) before any reaches health, and BURSTS when
	// the pool is spent (unlike the timed wards, it dies by spending). Sitting
	// in the one place a member takes damage, it soaks every source alike —
	// melee, ranged, a wall bump, even a poison tick (silently — quiet mode
	// is per-frame). A partial soak lets the remainder through to the normal
	// wound path below.
	if (StatusEffect* ward = target.FindWard(SpellSymbol::Water);
		ward && damage > 0.0f) {
		const float soaked = std::min(ward->magnitude, damage);
		ward->magnitude -= soaked;
		damage -= soaked;
		if (!quiet)
			MemberMessage(target, loc::Format("log.shield_soaks", target.name));
		if (ward->magnitude <= 0.0f) {
			target.RemoveWard(SpellSymbol::Water); // spent — burst, not fade
			MemberMessage(target, loc::Format("log.shield_bursts", target.name));
		}
		if (damage <= 0.0f) return; // fully absorbed — no wound, no splat
	}
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
	if (target.IsAlive()) return;
	target.stabilize = 0.0f; // the wound that downed them restarts the clock
	if (!target.dead &&
		(wasDown || damage >= m_balance.overkill * target.maxHealth)) {
		target.dead = true;
		MemberMessage(target, loc::Format("log.member_dies", target.name));
	} else if (!target.dead && !wasDown) {
		MemberMessage(target, loc::Format("log.member_down", target.name));
	}
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

// --- the defender side (docs/combat.md part 4) --------------------------------
// One resist table per side, summed from its sources and clamped by the
// balance rule (±resist_clamp; an authored nature cell at 1.0 = immunity).

DefenseProfile DungeonWorld::PartyDefense(const Character& member,
										  DamageType type) {
	float resist = member.natureResists[type]; // the race layer
	float soak = 0.0f;
	for (const ItemSlot& slot : member.inventory.equipment) {
		if (slot.Empty()) continue;
		const ItemKind& kind = ItemKindFor(slot.typeId);
		resist += kind.resists[type];
		soak += kind.armor;
	}
	// Stone Skin: earth hardens — the ward's magnitude converts to PHYSICAL
	// resist at the stoneskin_resist knob (elemental bolts pass it by).
	if (IsPhysical(type))
		if (const StatusEffect* ward = member.FindWard(SpellSymbol::Earth))
			resist += ward->magnitude * m_balance.stoneskinResist;
	return {member.Evasion(), soak,
			m_balance.ClampResist(resist, member.natureResists[type])};
}

DefenseProfile DungeonWorld::MonsterDefense(const MonsterKind& kind,
											DamageType type) const {
	return {kind.evasion, kind.armor,
			m_balance.ClampResist(kind.resists[type], kind.resists[type])};
}

void DungeonWorld::ApplyHitEffect(Character& target, StatusKind kind,
								  const HitEffect& fx) {
	if (fx.dps <= 0.0f || fx.duration <= 0.0f) return;
	std::uniform_real_distribution<float> roll(0.0f, 1.0f);
	if (roll(m_combatRng) > fx.chance) return;
	// Reapply REFRESHES (the ward-recast rule): the old effect goes, the new
	// one lands whole. School carries only the HUD tint — poison rides earth
	// green, bleed fire red (the palette convention until richer art exists).
	const bool poison = kind == StatusKind::Poison;
	target.RemoveEffect(kind);
	target.effects.push_back({kind,
							  poison ? SpellSymbol::Earth : SpellSymbol::Fire,
							  poison ? "effect.poison" : "effect.bleed",
							  fx.duration, fx.duration, fx.dps});
	MemberMessage(target, loc::Format(poison ? "log.poisoned" : "log.bleeding",
									  target.name));
}

// --- burning monsters (the enchanted-weapon proc) -----------------------------
// The mirror of ApplyHitEffect above, for the other side of the fight. A
// Character carries a LIST of status effects; a monster carries this one slot,
// which is all the content needs so far — see Monster::burnDps.

// How the flames read per school: the FireEffect palette is authored orange, so
// fire burns untinted and the other three recolour it (a water "burn" is the
// freezing kind — the plume runs cold blue). Multiplied over the flame/spark
// colours, so these are ratios against orange, not absolute colours.
static Vec3 BurnTint(SpellSymbol school) {
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

void DungeonWorld::IgniteMonster(Monster& monster, SpellSymbol school,
								 const HitEffect& fx, int source) {
	if (fx.dps <= 0.0f || fx.duration <= 0.0f) return;
	std::uniform_real_distribution<float> roll(0.0f, 1.0f);
	if (roll(m_combatRng) > fx.chance) return;
	// The RESIST applies once, here: a fire-natured monster catches poorly (and
	// an authored immunity, a cell of 1.0, never catches at all), rather than
	// paying the resist every tick. Same clamp rule as any other damage.
	const float dps = fx.dps * (1.0f - MonsterDefense(*monster.kind,
													  SchoolDamageType(school)).resist);
	if (dps <= 0.0f) return;
	const bool relit = monster.burnLeft > 0.0f; // reapply REFRESHES (the ward rule)
	monster.burnDps = dps;
	monster.burnLeft = fx.duration;
	monster.burnSchool = school;
	monster.burnSource = source;
	if (!monster.burnFx)
		monster.burnFx = std::make_unique<FireEffect>(BurnOrigin(monster), 1.1f,
													  monster.runtimeId * 2654435761u);
	monster.burnFx->SetTint(BurnTint(school)); // a relight may change school
	if (!relit)
		onMessage(loc::Format("log.monster_ignites",
							  loc::Tr("monster." + monster.kind->name)));
}

void DungeonWorld::Extinguish(Monster& monster) {
	monster.burnDps = 0.0f;
	monster.burnLeft = 0.0f;
	monster.burnSource = -1;
	monster.burnFx.reset();
}

void DungeonWorld::TickBurn(Monster& monster, float dt) {
	monster.burnLeft -= dt;
	monster.hp -= monster.burnDps * dt;
	// The fire keeps the grudge alive: whoever lit it goes on earning threat
	// for as long as it burns, so a hit-and-run torch still holds attention.
	if (monster.burnSource >= 0)
		AddThreat(monster, static_cast<size_t>(monster.burnSource),
				  monster.burnDps * dt);
	const std::string name = loc::Tr("monster." + monster.kind->name);
	if (!monster.Alive()) {
		monster.hp = 0.0f; // the ordinary downed state — the death clip plays
		Extinguish(monster);
		onMessage(loc::Format("log.monster_burns_away", name));
	} else if (monster.burnLeft <= 0.0f) {
		Extinguish(monster);
		onMessage(loc::Format("log.monster_burns_out", name));
	}
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

	const AttackProfile atk{monster.kind->damage, monster.kind->accuracy,
							monster.kind->damageType};
	const DefenseProfile def = PartyDefense(target, atk.type);
	const AttackResult r = ResolveAttack(atk, def, m_balance.Strike(), m_combatRng);
	const std::string name = loc::Tr("monster." + monster.kind->name);

	if (!r.hit) {
		MemberMessage(target, loc::Format("log.monster_misses", name, target.name));
		return;
	}
	MemberMessage(target, loc::Format("log.monster_hits", name, target.name,
									  static_cast<int>(r.damage + 0.5f)));
	m_audio.Play(m_sounds.monster, 0.6f);
	WoundMember(target, r.damage);
	// On-hit DoTs (Phase 6): the landed blow may envenom/open a wound —
	// applied even if the blow itself downed them (the ticks finish the job).
	ApplyHitEffect(target, StatusKind::Poison, monster.kind->poison);
	ApplyHitEffect(target, StatusKind::Bleed, monster.kind->bleed);
	// Fire Shield retaliation: fire guards by burning back — a monster that
	// LANDS a melee blow on a fire-warded member is scorched for the ward's
	// power (the hit itself is not reduced; earth is the school that hardens).
	// Fires even if the blow downs the member — the ward outlives its bearer's
	// last stand by exactly one burn.
	if (const StatusEffect* ward = target.FindWard(SpellSymbol::Fire)) {
		monster.hp -= ward->magnitude;
		onMessage(loc::Format("log.shield_burns", name,
							  static_cast<int>(ward->magnitude + 0.5f)));
		if (!monster.Alive()) {
			monster.hp = 0.0f; // downed monster stays in the list (save restore)
			onMessage(loc::Format("log.monster_slain", name));
		} else {
			monster.hitReq = true; // the burn stings — flinch like any hit
			// The burn is the ward-bearer's damage — it feeds their threat.
			AddThreat(monster, static_cast<size_t>(victim), ward->magnitude);
		}
	}
	CheckPartyWipe();
}

// A blocked move has lurched the party into the obstacle. Every standing member
// is jarred for a small flat amount, with the smallest splat over each portrait
// and a single grunt — then we re-check for a wipe so a final stumble still ends
// the run cleanly.
void DungeonWorld::OnBumpImpact() {
	if (!m_roster || m_partyWiped) return;

	constexpr float kBumpDamage = 2.0f; // small flat jar, regardless of armor
	bool anyHurt = false;
	for (Character& member : *m_roster) {
		if (!member.IsAlive()) continue;
		WoundMember(member, kBumpDamage); // severity 0 at this damage; logs any downing
		anyHurt = true;
	}
	if (!anyHurt) return;

	onMessage(loc::Format("log.bump_hurt", static_cast<int>(kBumpDamage + 0.5f)));
	m_audio.Play(m_sounds.oof, 0.8f);
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
	if (!spec) spec = &Balance::Neutral();
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
		m_balance.accBase +
			m_balance.accStat * static_cast<float>(attacker.dexterity) +
			m_balance.accSkill * static_cast<float>(level) + spec->acc,
		spec->type};
	const DefenseProfile def = MonsterDefense(*target->kind, atk.type);
	const AttackResult r = ResolveAttack(atk, def, m_balance.Strike(), m_combatRng);
	const std::string name = loc::Tr("monster." + target->kind->name);

	if (!r.hit) {
		MemberMessage(attacker, loc::Format("log.party_misses", attacker.name, name));
		return true;
	}
	// ENCHANTMENT: a landed blow with an elemental weapon carries its element
	// through as well — `element_bonus` of the assembled damage, resisted as
	// that element (not soaked; plate turns a blade, not a flame). It rides the
	// physical hit, so it gets no accuracy roll of its own.
	float elemental = 0.0f;
	if (weapon && weapon->enchanted && weapon->elementBonus > 0.0f) {
		const DamageType type = SchoolDamageType(weapon->element);
		elemental = atk.damage * weapon->elementBonus *
					(1.0f - MonsterDefense(*target->kind, type).resist);
		if (elemental < 0.0f) elemental = 0.0f;
	}
	target->hp -= r.damage + elemental;
	ProvokeMonster(*target); // the struck monster alone notices + turns to the party
	AddThreat(*target, member, r.damage + elemental);
	int dmg = static_cast<int>(r.damage + elemental + 0.5f);
	MemberMessage(attacker, loc::Format("log.party_hits", attacker.name, name, dmg));
	GrantSkillXp(attacker, skillId, 1.0f, stats); // a LANDED blow trains its class
	m_audio.Play(m_sounds.monster, 0.7f);

	if (!target->Alive()) {
		target->hp = 0.0f; // a downed monster stays in the list (so a new game /
		// save can restore it) but renders, blocks, and acts as dead.
		Extinguish(*target); // a corpse stops burning
		onMessage(loc::Format("log.monster_slain", name));
	} else {
		target->hitReq = true; // survivor flinches (a fatal blow goes straight to Die)
		// ...and a survivor may be left alight by the weapon's proc.
		if (weapon && weapon->enchanted)
			IgniteMonster(*target, weapon->element, weapon->elementDot,
						  static_cast<int>(member));
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

	const DefenseProfile def = MonsterDefense(*hit->kind, impact.atk.type);
	const AttackResult r =
		ResolveAttack(impact.atk, def, m_balance.Strike(), m_combatRng);
	const std::string name = loc::Tr("monster." + hit->kind->name);
	if (r.hit) {
		hit->hp -= r.damage;
		ProvokeMonster(*hit); // a spell strike also wakes its target
		if (impact.attacker >= 0)
			AddThreat(*hit, static_cast<size_t>(impact.attacker), r.damage);
		onMessage(loc::Format("log.spell_hits", name, static_cast<int>(r.damage + 0.5f)));
		m_audio.Play(m_sounds.spellImpact, 0.7f);
		if (!hit->Alive()) {
			hit->hp = 0.0f; // downed monster stays in the list (save can restore it)
			Extinguish(*hit); // a corpse stops burning
			onMessage(loc::Format("log.spell_slain", name));
		} else {
			hit->hitReq = true; // survivor flinches (a fatal blow goes straight to Die)
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
	// Wind Ward: air guards by DEFLECTING — a bolt aimed at the warded member
	// is turned aside outright (no strike roll), spending one of the ward's
	// charges (its magnitude); the last deflection stills the wind. Bolts
	// aimed at unwarded neighbours fly true — the ward wraps its caster alone.
	if (StatusEffect* ward = target.FindWard(SpellSymbol::Air)) {
		ward->magnitude -= 1.0f;
		MemberMessage(target, loc::Format("log.shield_deflects", target.name));
		if (ward->magnitude <= 0.0f) {
			target.RemoveWard(SpellSymbol::Air); // spent — stills, not fade
			MemberMessage(target, loc::Format("log.shield_stills", target.name));
		}
		return true; // the bolt is spent against the wind
	}
	const DefenseProfile def = PartyDefense(target, impact.atk.type);
	const AttackResult r =
		ResolveAttack(impact.atk, def, m_balance.Strike(), m_combatRng);
	if (!r.hit) {
		MemberMessage(target, loc::Format("log.monster_ranged_misses", target.name));
		return true;
	}
	MemberMessage(target, loc::Format("log.monster_ranged_hits", target.name,
									  static_cast<int>(r.damage + 0.5f)));
	m_audio.Play(m_sounds.monster, 0.6f);
	WoundMember(target, r.damage);
	CheckPartyWipe();
	return true;
}


} // namespace dungeon::game
