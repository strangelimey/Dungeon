// ============================================================================
// Game/DungeonWorld_SpellLight.cpp - the Sowilo LIGHT spells in the world
// (lighting-updates Phase 6, docs/lighting-updates-plan.md).
//
// A light spell leaves a `light` effect on its caster (Spell/LightSpell.h); this
// file is what the world makes of it every frame:
//   - THE LIGHT: one per (member, school), from the school's lights.cat profile
//     (`spell_<school>`), above the party a little ahead of the eye, sized by
//     the cast's power against the effect kind's `scale_power` and dimming over
//     its last tenth like a torch (AppendSpellLights);
//   - THE FLARE (Hagalaz): a flash round the party that DAZZLES the monsters
//     within a few steps - a `dazzle` effect the monster tick reads, under which
//     a monster does nothing - and the school's light acting once (LightFlare).
// What each school's light does beyond its colour lands here step by step
// (6c-6f).
//
// Nothing here allocates: the effect lists are walked in place, the lights go
// through PushLight, the flash takes a fixed glow slot.
// ============================================================================
#include "Game/DungeonWorld.h"

#include "Core/Loc.h"
#include "Game/Effect/LightEffect.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace dungeon::game {

namespace {
// The share of a light's time over which it dims, and how dim it gets - the
// torch's rule, so a spell's light runs out the way a torch does.
constexpr float kDimShare = 0.1f;
constexpr float kDimFloor = 0.35f;
// How far round the party a flare reaches, in walking steps (WalkReach, so a
// wall or a shut door between keeps a monster or a fire out of it).
constexpr int kFlareSteps = 3;
// The most monsters one flare can dazzle: every square within kFlareSteps (a
// diamond of 2s(s+1)+1 squares) packed with the smallest monsters a square holds.
// A promise, not an estimate, so the list of what it dazzled never overflows.
constexpr size_t kFlareTargets =
	static_cast<size_t>(2 * kFlareSteps * (kFlareSteps + 1) + 1) *
	static_cast<size_t>(SlotsPerCell(SizeClass::Tiny));
// A stone's light sits a little above it, so it pools on the floor round the
// stone and climbs the walls from below.
constexpr float kStoneLightHeight = 0.15f;
// How often a mote rises off a stone, and how far from the party a stone may be
// and still shed them (the spark pool is shared with everything in flight).
constexpr float kStoneMoteEvery = 0.4f;
constexpr int kStoneMoteSquares = 8;
// TRACKS (6g): how often a stone shows the tracks in its reach, how long a
// track's mote lasts, how fast it drifts the way its maker went (m/s), and how
// bright a fresh one is.
constexpr float kTrackShowEvery = 0.3f;
constexpr float kTrackMoteLife = 1.3f;
constexpr float kTrackDrift = 0.12f;
constexpr float kTrackBright = 0.9f;

// How bright a light is with `timeLeft` of `duration` to go: full, then dimming
// to kDimFloor over its last tenth.
float DimFor(float timeLeft, float duration) {
	const float share = duration > 0.0f ? timeLeft / duration : 1.0f;
	return share >= kDimShare
			   ? 1.0f
			   : kDimFloor + (1.0f - kDimFloor) * std::max(share, 0.0f) / kDimShare;
}

std::string_view SpellLightId(SpellSymbol school) {
	switch (school) {
	case SpellSymbol::Fire: return "spell_fire";
	case SpellSymbol::Earth: return "spell_earth";
	case SpellSymbol::Air: return "spell_air";
	case SpellSymbol::Water: return "spell_water";
	default: return "spell_fire";
	}
}
} // namespace

const fx::LightEffect* DungeonWorld::SpellLightKind() const {
	return static_cast<const fx::LightEffect*>(m_effects.Find("light"));
}

float DungeonWorld::SpellLightScale(float power) const {
	const fx::LightEffect* kind = SpellLightKind();
	const float ref = kind ? kind->ScalePower() : 8.0f;
	return std::clamp(std::sqrt(std::max(power, 0.0f) / ref), 0.7f, 1.8f);
}

void DungeonWorld::AppendSpellLights(float time) {
	if (!m_roster) return;
	const Vec3 eye = PartyEye();
	const Direction faced = static_cast<Direction>(m_party.Facing());
	// Over the party's heads and a little ahead, so it lights the way and the
	// walls beside rather than the backs of their necks.
	const Vec3 at{eye.x + static_cast<float>(DirDX(faced)) * kCellSize * 0.2f, eye.y + 0.35f,
				  eye.z + static_cast<float>(DirDZ(faced)) * kCellSize * 0.2f};
	for (size_t m = 0; m < m_roster->size(); ++m) {
		for (const fx::Inst& inst : (*m_roster)[m].effects) {
			// Earth's light is SET DOWN as a stone (6f), never carried.
			if (!inst.Is("light") || inst.school == SpellSymbol::Earth) continue;
			const light::Profile& profile = LightProfileFor(SpellLightId(inst.school));
			const float scale = SpellLightScale(inst.magnitude);
			const float dim = DimFor(inst.timeLeft, inst.duration);
			const u32 slot = static_cast<u32>(m * 4 + static_cast<size_t>(inst.school));
			const Vec4 c = ElementColor(inst.school);
			// AIR WARNS: its storm-flicker runs on a clock that speeds up while
			// the party is noticed (TickSpellLights).
			const float clock = inst.school == SpellSymbol::Air ? m_airPulseClock : time;
			PushLight(profile, "spell", LightKey(LightKind::Spell, slot), at, clock,
					  static_cast<float>(slot) * 2.1f, {c.x, c.y, c.z}, scale * dim,
					  profile.radius * kCellSize * scale * (0.6f + 0.4f * dim));
		}
	}
}

bool DungeonWorld::IsDazzled(const Monster& monster) {
	for (const fx::Inst& inst : monster.effects)
		if (inst.Is("dazzle")) return true;
	return false;
}

void DungeonWorld::LightFlare(SpellSymbol school, float power, int casterIndex) {
	const Vec3 eye = PartyEye();
	const Vec4 c = ElementColor(school);
	// THE FLASH: a burst of the school's light round the party, falling away
	// fast (a glow slot, `spell_flare`), and a cloud of bright motes.
	HandGlow* slot = &m_handGlows[0];
	for (HandGlow& g : m_handGlows)
		if (g.timeLeft < slot->timeLeft) slot = &g;
	*slot = {{eye.x, eye.y + 0.2f, eye.z}, {c.x, c.y, c.z}, 0.7f, 0.7f, 1.0f, true};
	m_projectiles.Puff({eye.x, eye.y - 0.1f, eye.z}, {c.x * 1.6f, c.y * 1.6f, c.z * 1.6f, 0.0f},
					   28, 1.4f, 0.6f, 0.05f * kUnit, 0.6f * kUnit);

	// WHAT IT REACHES: every square within a few walking steps of the party, so
	// a wall or a shut door keeps a monster (and a fire) out of it. ONE walk for
	// all of it - the dazzle, the scorch or shock, the kindling - where three
	// rules once disagreed (code-review C17).
	Reach reach;
	WalkReach(m_party.GridX(), m_party.GridZ(), kFlareSteps, reach);

	// THE DAZZLE: every monster it reaches does nothing for a time that grows
	// with the flare's power. The ones THIS flare dazzled are kept, by index, in
	// a fixed list: the school's light below acts on them and on nothing else - a
	// monster still dazzled by an earlier flare cast elsewhere is not struck.
	const fx::EffectKind* dazzle = m_effects.Find("dazzle");
	const float seconds = std::clamp(1.5f + 0.25f * power, 2.0f, 8.0f);
	std::array<u32, kFlareTargets> struck;
	size_t struckCount = 0;
	int dazzled = 0;
	for (size_t i = 0; i < m_monsters.size() && dazzle; ++i) {
		Monster& monster = m_monsters[i];
		if (!monster.Alive() || reach.StepsTo(monster.x, monster.z) < 0) continue;
		fx::Apply(monster.effects, *dazzle, school, power, seconds, casterIndex);
		++dazzled;
		if (struckCount < struck.size()) struck[struckCount++] = static_cast<u32>(i);
	}
	if (dazzled > 0 && onMessage) onMessage(loc::FormatLine("log.monsters_dazzled", dazzled));
	// Each monster this flare dazzled, still standing (a scorch may kill one, and
	// the list is re-checked against the vector each time it is read).
	const auto eachStruck = [&](auto&& act) {
		for (size_t k = 0; k < struckCount; ++k)
			if (struck[k] < m_monsters.size() && m_monsters[struck[k]].Alive())
				act(m_monsters[struck[k]]);
	};

	// THE SCHOOL'S LIGHT, ONCE, over the flare's reach.
	const fx::LightEffect* kind = SpellLightKind();
	switch (school) {
	case SpellSymbol::Fire: {
		// Every monster it dazzled is scorched, and every fire it reaches catches.
		const float damage = kind ? kind->ScorchDamage() * power / kind->ScalePower() : 0.0f;
		eachStruck([&](Monster& monster) { ScorchMonster(monster, damage, casterIndex); });
		KindleNear(reach, power);
		break;
	}
	case SpellSymbol::Air: {
		// A shock at every monster it dazzled.
		const float damage = kind ? kind->CrackleDamage() * power / kind->ScalePower() : 0.0f;
		eachStruck([&](Monster& monster) { CrackleMonster(monster, damage, casterIndex); });
		break;
	}
	case SpellSymbol::Earth:
		// What the light would show, mapped - every square it reaches - and the
		// tracks in them, all at once.
		MapStoneReach(m_party.GridX(), m_party.GridZ(), power);
		ShowTracks(m_party.GridX(), m_party.GridZ(), power, 1.0f);
		break;
	case SpellSymbol::Water: {
		// Every fire on the party out, and a draught of breath: stamina back.
		QuenchParty();
		if (m_roster)
			for (Character& member : *m_roster)
				if (member.IsAlive())
					member.stamina = std::min(member.maxStamina, member.stamina + power);
		break;
	}
	default: break;
	}
}

// --- what each school's light DOES ---------------------------------------------

float DungeonWorld::WaterLightPower() const {
	float power = 0.0f;
	if (m_roster)
		for (const Character& c : *m_roster)
			for (const fx::Inst& inst : c.effects)
				if (inst.Is("light") && inst.school == SpellSymbol::Water)
					power = std::max(power, inst.magnitude);
	return power;
}

float DungeonWorld::StaminaSoothe() const {
	const fx::LightEffect* kind = SpellLightKind();
	return kind && WaterLightPower() > 0.0f ? 1.0f + kind->Soothe() : 1.0f;
}

void DungeonWorld::AddClearBubble(gfx::Atmosphere& atmo) const {
	const float power = WaterLightPower();
	const fx::LightEffect* kind = SpellLightKind();
	if (power <= 0.0f || !kind || kind->ClearHaze() <= 0.0f) return;
	// As far as the light reaches, centred on the party: the weakest smoke puff
	// gives up its slot when all four are taken (the party's own air wins).
	const float radius =
		LightProfileFor("spell_water").radius * kCellSize * SpellLightScale(power);
	size_t slot = 0;
	for (size_t i = 0; i < gfx::kMaxDustPuffs; ++i) {
		if (atmo.dustPuffs[i].w <= 0.0f) {
			slot = i;
			break;
		}
		if (atmo.dustPuffs[i].w < atmo.dustPuffs[slot].w) slot = i;
	}
	const Vec3 eye = PartyEye();
	atmo.dustPuffs[slot] = {eye.x, eye.z, radius, -kind->ClearHaze()};
}

int DungeonWorld::QuenchParty() {
	if (!m_roster) return 0;
	int quenched = 0;
	for (Character& c : *m_roster) {
		const size_t before = c.effects.size();
		std::erase_if(c.effects, [](const fx::Inst& e) { return e.kind && e.kind->Plume(); });
		if (c.effects.size() != before) {
			++quenched;
			MemberMessage(c, loc::FormatLine("log.light_quenches", c.name));
		}
	}
	return quenched;
}

void DungeonWorld::ScorchMonster(Monster& monster, float damage, int source) {
	if (damage <= 0.0f || !monster.Alive()) return;
	MonsterTarget target{*this, monster};
	fx::DamageEvent ev =
		fx::DamageEvent::Burst(m_damageTypes.ForSchool(SpellSymbol::Fire), damage, source);
	fx::Deal(ev, target, m_balance.Strike(), m_combatRng);
	const Vec3 at = BurnOrigin(monster);
	const Vec4 c = ElementColor(SpellSymbol::Fire);
	m_projectiles.Puff(at, {c.x * 1.6f, c.y * 3.0f, c.z * 2.0f, 0.0f}, 6, 0.3f, 0.4f,
					   0.04f * kUnit, 0.08f * kUnit);
	if (!onMessage) return;
	const loc::Line name = loc::ViewKey("monster.", monster.kind->name);
	if (ev.dealt >= 0.5f)
		onMessage(loc::FormatLine("log.light_scorches", name, static_cast<int>(ev.dealt + 0.5f)));
	if (!monster.Alive()) onMessage(loc::FormatLine("log.monster_slain", name));
}

int DungeonWorld::KindleNear(const Reach& reach, float power) {
	const fx::LightEffect* kind = SpellLightKind();
	const float brazierPower = kind ? kind->KindleBrazierPower() : 14.0f;
	int kindled = 0;
	for (Fire& fire : m_fires) {
		if (fire.lit || fire.empty || (fire.kind && fire.kind->flameless)) continue;
		// The fire's own square, reached: a wall torch's is the floor square it
		// hangs over, a brazier's the square it stands in (floor the party may not
		// enter, but open to the light - OpenSquare). By Manhattan distance it
		// lit a torch in the next corridor through the rock (code-review C17).
		if (reach.StepsTo(fire.x, fire.z) < 0) continue;
		if (fire.brazier && power < brazierPower) continue;
		if (SetFireBurning(fire.x, fire.z, fire.wall, true)) ++kindled;
	}
	if (kindled > 0 && onMessage) onMessage(loc::FormatLine("log.light_kindles", kindled));
	return kindled;
}

void DungeonWorld::TickSpellLights(float dt) {
	if (!m_roster) return;
	const fx::LightEffect* kind = SpellLightKind();
	if (!kind) return;
	// AIR WARNS: the flicker's clock, eased toward warn_rate while something
	// near has noticed the party and back to 1 when nothing has.
	const float warnTarget = PartyNoticed() ? kind->WarnRate() : 1.0f;
	m_airPulseRate += (warnTarget - m_airPulseRate) * std::min(1.0f, dt * 2.0f);
	m_airPulseClock += dt * m_airPulseRate;
	// WATER QUENCHES: while a Tidelight is up, nothing on the party burns.
	if (WaterLightPower() > 0.0f) QuenchParty();
	m_kindleClock -= dt;
	const bool kindleDue = m_kindleClock <= 0.0f;
	if (kindleDue) m_kindleClock = 0.25f;
	const int px = m_party.GridX(), pz = m_party.GridZ();
	// What a carried Firelight kindles: the party's square and a step from it,
	// walked like every light's reach (a torch through the wall stays dark).
	// (`beside`, not `near`: <windows.h> defines that one away.)
	Reach beside;
	if (kindleDue) WalkReach(px, pz, 1, beside);
	for (size_t m = 0; m < m_roster->size() && m < m_scorchClock.size(); ++m) {
		const fx::Inst* fire = nullptr;
		for (const fx::Inst& inst : (*m_roster)[m].effects)
			if (inst.Is("light") && inst.school == SpellSymbol::Fire) fire = &inst;
		if (!fire) {
			m_scorchClock[m] = 0.0f; // the next light scorches at once
			continue;
		}
		const float power = fire->magnitude;
		// KINDLES what it passes: a fire in the party's square or one beside it.
		if (kindleDue) KindleNear(beside, power);
		// SCORCHES what comes close: each monster in a square beside the party.
		m_scorchClock[m] -= dt;
		if (m_scorchClock[m] > 0.0f) continue;
		m_scorchClock[m] = kind->ScorchEvery();
		const float damage = kind->ScorchDamage() * power / kind->ScalePower();
		for (Monster& monster : m_monsters)
			if (monster.Alive() && std::abs(monster.x - px) + std::abs(monster.z - pz) == 1)
				ScorchMonster(monster, damage, static_cast<int>(m));
	}

	// AIR CRACKLES at foes: per member with a Skylight, a shock every
	// crackle_every seconds at the nearest monster in its reach and sight.
	for (size_t m = 0; m < m_roster->size() && m < m_crackleClock.size(); ++m) {
		const fx::Inst* air = nullptr;
		for (const fx::Inst& inst : (*m_roster)[m].effects)
			if (inst.Is("light") && inst.school == SpellSymbol::Air) air = &inst;
		if (!air) {
			m_crackleClock[m] = 0.0f;
			continue;
		}
		m_crackleClock[m] -= dt;
		if (m_crackleClock[m] > 0.0f) continue;
		m_crackleClock[m] = kind->CrackleEvery();
		const float reach = LightProfileFor("spell_air").radius * SpellLightScale(air->magnitude);
		CrackleNearest(reach, kind->CrackleDamage() * air->magnitude / kind->ScalePower(),
					   static_cast<int>(m));
	}
}

void DungeonWorld::CrackleMonster(Monster& monster, float damage, int source) {
	if (damage <= 0.0f || !monster.Alive()) return;
	MonsterTarget target{*this, monster};
	fx::DamageEvent ev =
		fx::DamageEvent::Burst(m_damageTypes.ForSchool(SpellSymbol::Air), damage, source);
	fx::Deal(ev, target, m_balance.Strike(), m_combatRng);
	// A spit of white sparks off the body.
	m_projectiles.Splash(BurnOrigin(monster), {0.0f, 0.0f, 0.0f}, {1.6f, 1.7f, 2.0f, 0.0f}, 10,
						 1.8f, 0.25f, 0.01f * kUnit);
	if (!onMessage) return;
	const loc::Line name = loc::ViewKey("monster.", monster.kind->name);
	if (ev.dealt >= 0.5f)
		onMessage(loc::FormatLine("log.light_crackles", name, static_cast<int>(ev.dealt + 0.5f)));
	if (!monster.Alive()) onMessage(loc::FormatLine("log.monster_slain", name));
}

bool DungeonWorld::CrackleNearest(float reachSquares, float damage, int source) {
	const int px = m_party.GridX(), pz = m_party.GridZ();
	Monster* nearest = nullptr;
	int best = 1 << 30;
	for (Monster& monster : m_monsters) {
		if (!monster.Alive()) continue;
		const int d = std::abs(monster.x - px) + std::abs(monster.z - pz);
		if (d < 1 || static_cast<float>(d) > reachSquares || d >= best) continue;
		// Only what the party could see: down a shared row or column, nothing solid
		// between (the grid's own line of sight).
		if (!CellHasLineOfSight(px, pz, monster.x, monster.z)) continue;
		best = d;
		nearest = &monster;
	}
	if (!nearest) return false;
	CrackleMonster(*nearest, damage, source);
	return true;
}

// --- EARTH: the stone set down (6f) ---------------------------------------------

float DungeonWorld::StoneReach(float power) const {
	return LightProfileFor("spell_earth").radius * SpellLightScale(power);
}

int DungeonWorld::StoneSteps(float power) const {
	return std::clamp(static_cast<int>(std::ceil(StoneReach(power))), 0, kStoneSteps);
}

namespace {
constexpr u8 kUnwalked = 0xFF;
} // namespace

int DungeonWorld::Reach::StepsTo(int cx, int cz) const {
	const int lx = cx - x + kStoneSteps, lz = cz - z + kStoneSteps;
	if (count == 0 || lx < 0 || lz < 0 || lx >= kStoneWindow || lz >= kStoneWindow) return -1;
	const u8 d = steps[static_cast<size_t>(lz * kStoneWindow + lx)];
	return d == kUnwalked ? -1 : d;
}

void DungeonWorld::WalkReach(int x, int z, int steps, Reach& out) const {
	out.x = x;
	out.z = z;
	out.count = 0;
	out.steps.fill(kUnwalked);
	const int w = m_map.Width(), h = m_map.Height();
	if (x < 0 || z < 0 || x >= w || z >= h) return;
	steps = std::clamp(steps, 0, kStoneSteps);
	// A breadth-first walk in a window round (x, z): `cells` doubles as the
	// queue, the window's step counts say where the walk has been. The origin
	// counts whatever stands there (the party may be in an open doorway).
	const auto local = [&](int cx, int cz) {
		return static_cast<size_t>((cz - z + kStoneSteps) * kStoneWindow + (cx - x + kStoneSteps));
	};
	out.steps[local(x, z)] = 0;
	out.cells[static_cast<size_t>(out.count++)] = z * w + x;
	for (int head = 0; head < out.count; ++head) {
		const int c = out.cells[static_cast<size_t>(head)];
		const int cx = c % w, cz = c / w;
		const u8 d = out.steps[local(cx, cz)];
		if (d >= steps) continue;
		constexpr int kDX[4] = {0, 1, 0, -1}, kDZ[4] = {-1, 0, 1, 0};
		for (int k = 0; k < 4; ++k) {
			const int nx = cx + kDX[k], nz = cz + kDZ[k];
			// Detonate's own test: floor, and no shut door (also false off the
			// map). It was IsWalkable, which a closed door passes.
			if (!OpenSquare(nx, nz)) continue;
			u8& slot = out.steps[local(nx, nz)];
			if (slot != kUnwalked) continue;
			slot = static_cast<u8>(d + 1);
			out.cells[static_cast<size_t>(out.count++)] = nz * w + nx;
		}
	}
}

int DungeonWorld::MapStoneReach(int x, int z, float power) {
	Reach reach;
	WalkReach(x, z, StoneSteps(power), reach);
	const int w = m_map.Width();
	for (int i = 0; i < reach.count; ++i)
		MarkSeen(reach.cells[static_cast<size_t>(i)] % w,
				 reach.cells[static_cast<size_t>(i)] / w); // and the walls round it
	return reach.count;
}

void DungeonWorld::PlaceLightStone(float power, float seconds) {
	const int px = m_party.GridX(), pz = m_party.GridZ();
	// A stone already in this square gives way to the new one; else a free slot,
	// else the stone nearest its end.
	LightStone* slot = nullptr;
	for (LightStone& s : m_lightStones)
		if (s.timeLeft > 0.0f && s.x == px && s.z == pz) slot = &s;
	if (!slot) {
		slot = &m_lightStones[0];
		for (LightStone& s : m_lightStones)
			if (s.timeLeft < slot->timeLeft) slot = &s;
	}
	// The stone it replaces leaves the cubes; the new one joins them (C178).
	if (slot->timeLeft > 0.0f) NoteCellCaster(slot->x, slot->z);
	*slot = {px, pz, power, seconds, seconds, 0.0f, 0.0f};
	NoteCellCaster(px, pz);
	// MAPS what it shows: every square its light reaches.
	MapStoneReach(px, pz, power);
	// Set down with a breath of amber dust.
	const light::Profile& profile = LightProfileFor("spell_earth");
	const Vec3 at = m_map.CellCenter(px, pz, 0.1f * kUnit);
	m_projectiles.Puff(at, {profile.color.x * 1.4f, profile.color.y * 1.4f, profile.color.z * 1.4f, 0.0f},
					   14, 0.5f, 0.9f, 0.03f * kUnit, 0.15f * kUnit);
}

void DungeonWorld::TickLightStones(float dt) {
	const light::Profile& profile = LightProfileFor("spell_earth");
	const int px = m_party.GridX(), pz = m_party.GridZ();
	for (LightStone& s : m_lightStones) {
		if (s.timeLeft <= 0.0f) continue;
		s.timeLeft = std::max(0.0f, s.timeLeft - dt);
		if (s.timeLeft <= 0.0f) { // spent: the slot is free, the stone gone from the cubes
			NoteCellCaster(s.x, s.z);
			continue;
		}
		// Only while the party is near enough to see it: the odd mote rising off
		// it, and the tracks within its reach (6g).
		s.moteClock -= dt;
		s.trackClock -= dt;
		if (std::abs(s.x - px) + std::abs(s.z - pz) > kStoneMoteSquares) continue;
		const float dim = DimFor(s.timeLeft, s.duration);
		if (s.trackClock <= 0.0f) {
			s.trackClock = kTrackShowEvery;
			ShowTracks(s.x, s.z, s.power, dim);
		}
		if (s.moteClock > 0.0f) continue;
		s.moteClock = kStoneMoteEvery;
		m_projectiles.Puff(m_map.CellCenter(s.x, s.z, 0.08f * kUnit),
						   {profile.color.x * dim, profile.color.y * dim, profile.color.z * dim, 0.0f},
						   1, 0.05f, 1.8f, 0.015f * kUnit, 0.12f * kUnit);
	}
}

// --- MONSTER TRACKS (6g) ----------------------------------------------------------

void DungeonWorld::FitTracksToMap() {
	// Sized with the fog mask, at a load or an edit - never in a settled frame.
	m_tracks.assign(static_cast<size_t>(m_map.Width()) * static_cast<size_t>(m_map.Height()),
					Track{});
}

void DungeonWorld::ClearTracks() { std::fill(m_tracks.begin(), m_tracks.end(), Track{}); }

void DungeonWorld::RecordTrack(int x, int z, Direction dir, TrackMaker maker) {
	const int w = m_map.Width();
	if (x < 0 || z < 0 || x >= w || z >= m_map.Height()) return;
	const size_t cell = static_cast<size_t>(z) * static_cast<size_t>(w) + static_cast<size_t>(x);
	if (cell >= m_tracks.size()) return; // not fitted yet: nothing to write into
	m_tracks[cell] = {m_trackClock, dir, maker};
}

float DungeonWorld::TrackAge(const Track& t) const {
	if (t.maker == TrackMaker::None) return -1.0f;
	const float age = static_cast<float>(m_trackClock - t.stamp);
	return age < m_balance.trackLife ? std::max(age, 0.0f) : -1.0f;
}

void DungeonWorld::ShowTracks(int x, int z, float power, float strength) {
	if (m_tracks.empty() || m_balance.trackLife <= 0.0f) return;
	Reach reach;
	WalkReach(x, z, StoneSteps(power), reach);
	const int count = reach.count;
	const auto& cells = reach.cells;
	const light::Profile& profile = LightProfileFor("spell_earth");
	const int w = m_map.Width();
	// A cheap hash for which tracks show this time and where on the square.
	const auto next = [this] {
		m_trackSeed = m_trackSeed * 1664525u + 1013904223u;
		return static_cast<float>(m_trackSeed >> 8) / 16777216.0f; // 0..1
	};
	for (int i = 0; i < count; ++i) {
		const size_t cell = static_cast<size_t>(cells[i]);
		if (cell >= m_tracks.size()) continue;
		const Track& t = m_tracks[cell];
		if (t.maker != TrackMaker::Monster) continue; // the party's own are for later
		const float age = TrackAge(t);
		if (age < 0.0f) continue;
		// Fresher shows more often and brighter; an old one is a rare faint glint.
		const float fresh = 1.0f - age / m_balance.trackLife;
		if (next() > strength * (0.3f + 0.7f * fresh)) continue;
		// A footprint: along the line it walked, a little to one side or the
		// other, drifting slowly on the way it went.
		const float fx = static_cast<float>(DirDX(t.dir)), fz = static_cast<float>(DirDZ(t.dir));
		const float along = (next() - 0.5f) * 0.7f * kCellSize;
		const float side = (next() < 0.5f ? -0.1f : 0.1f) * kCellSize;
		const Vec3 c = m_map.CellCenter(static_cast<int>(cell % static_cast<size_t>(w)),
										static_cast<int>(cell / static_cast<size_t>(w)),
										0.02f * kUnit);
		const Vec3 at{c.x + fx * along - fz * side, c.y, c.z + fz * along + fx * side};
		const float b = kTrackBright * (0.3f + 0.7f * fresh) * strength;
		if (!m_projectiles.Mote(at, {fx * kTrackDrift, 0.02f, fz * kTrackDrift},
								{profile.color.x * b, profile.color.y * b, profile.color.z * b, 0.0f},
								kTrackMoteLife, 0.012f * kUnit))
			return; // the pool is full: the rest wait for the next showing
	}
}

std::vector<std::string> DungeonWorld::DescribeTracks() const {
	// A readout, not a frame: it may allocate.
	std::vector<std::pair<float, size_t>> live;
	for (size_t i = 0; i < m_tracks.size(); ++i)
		if (const float age = TrackAge(m_tracks[i]); age >= 0.0f) live.emplace_back(age, i);
	std::sort(live.begin(), live.end());
	static constexpr const char* kDirNames[] = {"north", "east", "south", "west"};
	std::vector<std::string> out;
	out.push_back(std::format("tracks: {} on {} (fading over {:.0f} s, clock {:.1f})",
							  live.size(), m_currentLevel, m_balance.trackLife, m_trackClock));
	const size_t w = static_cast<size_t>(std::max(m_map.Width(), 1));
	for (size_t i = 0; i < live.size() && i < 8; ++i) {
		const Track& t = m_tracks[live[i].second];
		out.push_back(std::format("  {},{} going {}  {:.1f} s old  ({})", live[i].second % w,
								  live[i].second / w, kDirNames[static_cast<int>(t.dir) & 3],
								  live[i].first,
								  t.maker == TrackMaker::Party ? "party" : "monster"));
	}
	return out;
}

void DungeonWorld::AppendStoneLights(float time) {
	const light::Profile& profile = LightProfileFor("spell_earth");
	for (size_t i = 0; i < m_lightStones.size(); ++i) {
		const LightStone& s = m_lightStones[i];
		if (s.timeLeft <= 0.0f) continue;
		const float scale = SpellLightScale(s.power);
		const float dim = DimFor(s.timeLeft, s.duration);
		PushLight(profile, "stone", LightKey(LightKind::Stone, static_cast<u32>(i)),
				  m_map.CellCenter(s.x, s.z, kStoneLightHeight * kUnit), time,
				  static_cast<float>(i) * 1.7f, profile.color, scale * dim,
				  profile.radius * kCellSize * scale * (0.6f + 0.4f * dim));
	}
}

std::vector<std::string> DungeonWorld::DescribeLightStones() const {
	std::vector<std::string> out;
	for (size_t i = 0; i < m_lightStones.size(); ++i) {
		const LightStone& s = m_lightStones[i];
		if (s.timeLeft <= 0.0f) continue;
		out.push_back(std::format("  stone {}: square {},{}  power {:.1f}  {:.0f} of {:.0f} s  "
								  "reach {:.1f} sq",
								  i, s.x, s.z, s.power, s.timeLeft, s.duration,
								  StoneReach(s.power)));
	}
	// With how much of the level is mapped, so a stone's mapping can be read off.
	const size_t seen = static_cast<size_t>(std::count(m_seen.begin(), m_seen.end(), 1));
	out.insert(out.begin(), std::format("lightstones: {} on {} (of {}); {} squares mapped",
										out.size(), m_currentLevel, kLightStones, seen));
	return out;
}

} // namespace dungeon::game
