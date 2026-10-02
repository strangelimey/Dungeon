// ============================================================================
// Game/Spell/ModifiedSpell.cpp - see ModifiedSpell.h.
// ============================================================================
#include "Game/Spell/ModifiedSpell.h"

#include "Core/Loc.h"
#include "Game/Catalog.h"
#include "Game/Character.h"
#include "Game/Spell/BoltSpell.h"
#include "Game/Spell/WardSpell.h"

#include <algorithm>
#include <cmath>

namespace dungeon::game {

namespace {
std::vector<SpellSymbol> WithModifier(std::span<const SpellSymbol> base, SpellSymbol m) {
	std::vector<SpellSymbol> seq(base.begin(), base.end());
	seq.push_back(m);
	return seq;
}
} // namespace

std::string ModifiedSpell::IdFor(const Spell& base, SpellSymbol modifier) {
	const bool ward = dynamic_cast<const WardSpell*>(&base) != nullptr;
	if (modifier == SpellSymbol::Explode) return base.Id() + "_burst";
	return base.Id() + (ward ? "_party" : "_volley");
}

ModifiedSpell::ModifiedSpell(const Spell& base, SpellSymbol modifier)
	: Spell(IdFor(base, modifier), WithModifier(base.Sequence(), modifier), base.Power(),
			base.Mana() * 2.0f),
	  m_bolt(dynamic_cast<const BoltSpell*>(&base)),
	  m_ward(dynamic_cast<const WardSpell*>(&base)), m_modifier(modifier) {
	// A burst's first-cut rules, until spells.cat says otherwise: a small fire-
	// ball's shape, damage near the base spell's power.
	if (modifier == SpellSymbol::Explode) {
		blast::Rules& r = m_payload.blast.rules;
		r.force = 3;
		r.damage = base.Power() * 0.6f;
		r.falloff = 1.5f;
		r.rate = 0.06f;
	}
	if (m_ward && modifier == SpellSymbol::Multiple) m_share = 0.75f;
}

int ModifiedSpell::VolleyCount(float power) const {
	const int extra = m_countPerPower > 0.0f
						  ? static_cast<int>(std::floor(std::max(0.0f, power - Power()) / m_countPerPower))
						  : 0;
	return std::clamp(m_count + extra, 1, std::max(m_count, m_countMax));
}

BlastSpec ModifiedSpell::ScaledBlast(float power) const {
	BlastSpec s = m_payload.blast;
	const float base = std::max(Power(), 1.0f);
	// Harder hits as the caster grows (the ratio of this cast's power to the
	// spell's own), and a further reach for every m_forcePerPower past it.
	s.rules.damage *= power / base;
	if (m_forcePerPower > 0.0f)
		s.rules.force += static_cast<int>(std::floor(std::max(0.0f, power - base) / m_forcePerPower));
	return s;
}

void ModifiedSpell::Cast(CastContext& ctx) const {
	if (m_bolt) {
		if (m_modifier == SpellSymbol::Explode) {
			ProjectileSpec bolt = m_bolt->PartyBolt(ctx, ctx.power);
			bolt.payload.blast = ScaledBlast(ctx.power);
			ctx.services.spawnBolt(bolt);
			return;
		}
		// A VOLLEY down the caster's own lane, a beat apart, each a little off
		// the lane's line (Michael) - alternately either side, never past it.
		const int n = VolleyCount(ctx.power);
		const Vec3 across{ctx.dir.z, 0.0f, -ctx.dir.x};
		for (int i = 0; i < n; ++i) {
			ProjectileSpec bolt = m_bolt->PartyBolt(ctx, ctx.power * m_share);
			const float side = i == 0 ? 0.0f : ((i % 2) ? 1.0f : -1.0f) * m_jitter /
														   static_cast<float>((i + 1) / 2);
			bolt.pos = Add(bolt.pos, Scale(across, side));
			if (i == 0) ctx.services.spawnBolt(bolt);
			else ctx.services.spawnBoltAfter(bolt, m_gap * static_cast<float>(i));
		}
		return;
	}
	if (!m_ward) return;
	if (m_modifier == SpellSymbol::Multiple) {
		// The ward on every member still standing, the caster among them.
		for (Character& member : ctx.party)
			if (member.IsAlive()) m_ward->WardOn(ctx, member, ctx.power * m_share);
		return;
	}
	// The ward's power spent as a BURST of its element round the caster - their
	// own square spared - and no ward left behind.
	ProjectilePayload payload = MakePayload();
	payload.blast = ScaledBlast(ctx.power);
	payload.flavour = School();
	ctx.services.blastAroundParty(payload, School(), ctx.casterIndex);
	if (ctx.services.message)
		ctx.services.message(ctx.caster, loc::FormatLine("log.ward_burst", ctx.caster.name));
}

std::optional<ProjectileSpec> ModifiedSpell::MonsterBolt(const Vec3& origin, const Vec3& dir,
														 float accuracy) const {
	if (!m_bolt) return std::nullopt;
	std::optional<ProjectileSpec> bolt = m_bolt->MonsterBolt(origin, dir, accuracy);
	if (!bolt) return bolt;
	if (m_modifier == SpellSymbol::Explode) bolt->payload.blast = ScaledBlast(Power());
	else bolt->atk.damage *= m_share;
	return bolt;
}

int ModifiedSpell::MonsterVolley() const { return IsVolley() ? VolleyCount(Power()) : 1; }

void ModifiedSpell::ApplyOverrides(const CatalogEntry& e) {
	Spell::ApplyOverrides(e);
	m_count = static_cast<int>(e.GetFloat("count", static_cast<float>(m_count)));
	m_countPerPower = e.GetFloat("count_per_power", m_countPerPower);
	m_countMax = static_cast<int>(e.GetFloat("count_max", static_cast<float>(m_countMax)));
	m_share = e.GetFloat("share", m_share);
	m_gap = e.GetFloat("gap", m_gap);
	m_jitter = e.GetFloat("jitter", m_jitter);
	m_forcePerPower = e.GetFloat("blast_force_per_power", m_forcePerPower);
}

} // namespace dungeon::game
