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

namespace dungeon::game {

namespace {
// The share of a light's time over which it dims, and how dim it gets - the
// torch's rule, so a spell's light runs out the way a torch does.
constexpr float kDimShare = 0.1f;
constexpr float kDimFloor = 0.35f;
// How far round the party a flare dazzles, in walking steps (the light
// budget's reach map, so a wall between keeps a monster out of it).
constexpr u16 kFlareSteps = 3;

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
			const float share = inst.duration > 0.0f ? inst.timeLeft / inst.duration : 1.0f;
			const float dim =
				share >= kDimShare ? 1.0f
								   : kDimFloor + (1.0f - kDimFloor) * std::max(share, 0.0f) / kDimShare;
			const u32 slot = static_cast<u32>(m * 4 + static_cast<size_t>(inst.school));
			const Vec4 c = ElementColor(inst.school);
			PushLight(profile, "spell", LightKey(LightKind::Spell, slot), at, time,
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
	default: break;
	}
}

// --- what each school's light DOES ---------------------------------------------

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
}

} // namespace dungeon::game
