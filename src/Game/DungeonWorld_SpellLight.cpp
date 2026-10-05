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
// How far round the party a flare dazzles, in walking steps (the light
// budget's reach map, so a wall between keeps a monster out of it).
constexpr u16 kFlareSteps = 3;
// A stone's light sits a little above it, so it pools on the floor round the
// stone and climbs the walls from below.
constexpr float kStoneLightHeight = 0.15f;
// How often a mote rises off a stone, and how far from the party a stone may be
// and still shed them (the spark pool is shared with everything in flight).
constexpr float kStoneMoteEvery = 0.4f;
constexpr int kStoneMoteSquares = 8;

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

	// THE DAZZLE: every monster within a few steps of the party - by walking,
	// so a wall keeps one out of it - does nothing for a time that grows with
	// the flare's power.
	const fx::EffectKind* dazzle = m_effects.Find("dazzle");
	RefreshReach();
	const int w = m_map.Width(), h = m_map.Height();
	const float seconds = std::clamp(1.5f + 0.25f * power, 2.0f, 8.0f);
	int dazzled = 0;
	for (Monster& monster : m_monsters) {
		if (!monster.Alive() || !dazzle) continue;
		if (monster.x < 0 || monster.z < 0 || monster.x >= w || monster.z >= h) continue;
		const size_t cell = static_cast<size_t>(monster.z) * static_cast<size_t>(w) +
							static_cast<size_t>(monster.x);
		if (cell >= m_reach.size() || m_reach[cell] > kFlareSteps) continue;
		fx::Apply(monster.effects, *dazzle, school, power, seconds, casterIndex);
		++dazzled;
	}
	if (dazzled > 0 && onMessage) onMessage(loc::FormatLine("log.monsters_dazzled", dazzled));

	// THE SCHOOL'S LIGHT, ONCE, over the flare's reach.
	const fx::LightEffect* kind = SpellLightKind();
	switch (school) {
	case SpellSymbol::Fire: {
		// Every monster it dazzled is scorched, and every fire in reach catches.
		const float damage = kind ? kind->ScorchDamage() * power / kind->ScalePower() : 0.0f;
		for (Monster& monster : m_monsters)
			if (monster.Alive() && IsDazzled(monster)) ScorchMonster(monster, damage, casterIndex);
		KindleNear(kFlareSteps, power);
		break;
	}
	case SpellSymbol::Air: {
		// A shock at every monster it dazzled.
		const float damage = kind ? kind->CrackleDamage() * power / kind->ScalePower() : 0.0f;
		for (Monster& monster : m_monsters)
			if (monster.Alive() && IsDazzled(monster))
				CrackleMonster(monster, damage, casterIndex);
		break;
	}
	case SpellSymbol::Earth:
		// What the light would show, mapped: every square it reaches.
		MapAroundParty(static_cast<int>(std::ceil(StoneReach(power))));
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

int DungeonWorld::KindleNear(int steps, float power) {
	const fx::LightEffect* kind = SpellLightKind();
	const float brazierPower = kind ? kind->KindleBrazierPower() : 14.0f;
	const int px = m_party.GridX(), pz = m_party.GridZ();
	int kindled = 0;
	for (Fire& fire : m_fires) {
		if (fire.lit || fire.empty || (fire.kind && fire.kind->flameless)) continue;
		if (std::abs(fire.x - px) + std::abs(fire.z - pz) > steps) continue;
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
		if (kindleDue) KindleNear(1, power);
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

int DungeonWorld::MapAroundParty(int steps) {
	RefreshReach();
	const int w = m_map.Width(), h = m_map.Height();
	if (m_reach.size() != static_cast<size_t>(w) * static_cast<size_t>(h)) return 0;
	int mapped = 0;
	for (int z = 0; z < h; ++z)
		for (int x = 0; x < w; ++x)
			if (m_reach[static_cast<size_t>(z) * w + x] <= steps) {
				MarkSeen(x, z); // the square and the walls round it
				++mapped;
			}
	return mapped;
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
	*slot = {px, pz, power, seconds, seconds, 0.0f};
	// MAPS what it shows: every square its light reaches.
	MapAroundParty(static_cast<int>(std::ceil(StoneReach(power))));
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
		if (s.timeLeft <= 0.0f) continue; // spent: the slot is free
		// The odd mote rising off it, while the party is near enough to see it.
		s.moteClock -= dt;
		if (s.moteClock > 0.0f) continue;
		s.moteClock = kStoneMoteEvery;
		if (std::abs(s.x - px) + std::abs(s.z - pz) > kStoneMoteSquares) continue;
		const float dim = DimFor(s.timeLeft, s.duration);
		m_projectiles.Puff(m_map.CellCenter(s.x, s.z, 0.08f * kUnit),
						   {profile.color.x * dim, profile.color.y * dim, profile.color.z * dim, 0.0f},
						   1, 0.05f, 1.8f, 0.015f * kUnit, 0.12f * kUnit);
	}
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
