// ============================================================================
// Game/Spell/ModifiedSpell.cpp - see ModifiedSpell.h.
// ============================================================================
#include "Game/Spell/ModifiedSpell.h"

#include "Core/Loc.h"
#include "Game/Catalog.h"
#include "Game/Character.h"
#include "Game/Spell/BoltSpell.h"
#include "Game/Spell/LightSpell.h"
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
	if (dynamic_cast<const LightSpell*>(&base))
		return base.Id() + (modifier == SpellSymbol::Explode ? "_flare" : "_bright");
	if (modifier == SpellSymbol::Explode) return base.Id() + "_burst";
	return base.Id() + (ward ? "_party" : "_volley");
}

ModifiedSpell::ModifiedSpell(const Spell& base, SpellSymbol modifier)
	: Spell(IdFor(base, modifier), WithModifier(base.Sequence(), modifier), base.Power(),
			base.Mana() * 2.0f),
	  m_form(base), m_bolt(dynamic_cast<const BoltSpell*>(&base)),
	  m_ward(dynamic_cast<const WardSpell*>(&base)),
	  m_light(dynamic_cast<const LightSpell*>(&base)), m_modifier(modifier) {
	// A burst's first-cut rules, until spells.cat says otherwise: a small fire-
	// ball's shape. (A light's flare is no blast: it dazzles - LightSpell::Flare.)
	if (modifier == SpellSymbol::Explode && !m_light) {
		blast::Rules& r = m_payload.blast.rules;
		r.force = 3;
		r.falloff = 1.5f;
		r.rate = 0.06f;
	}
	if (m_ward && modifier == SpellSymbol::Multiple) m_share = 0.75f;
	// The rest - power, mana, the form's on-hit and shove, the burst's damage -
	// is the form's, so it waits for the form's own tuning (DeriveFromForm); taken
	// here as well, so a spell built outside SpellBook::Build is whole.
	DeriveFromForm();
}

void ModifiedSpell::DeriveFromForm() {
	m_power = m_form.Power();
	m_mana = m_form.Mana() * 2.0f;
	// What it leaves behind is its form's until its own entry says otherwise -
	// the burn a firebolt lands, a burst's blast rules kept as they stand.
	const BlastSpec blast = m_payload.blast;
	m_payload = m_form.MakePayload();
	m_payload.blast = blast;
	// A burst's damage is near its form's power.
	if (m_modifier == SpellSymbol::Explode && !m_light)
		m_payload.blast.rules.damage = m_form.Power() * 0.6f;
	if (m_bolt) m_push = m_bolt->Push();
}

void ModifiedSpell::Dress(ProjectileSpec& bolt, float power) const {
	LendLook(bolt);
	// THIS spell's on-hit effects, in its school (the form's bolt carried the
	// form's), and its shove.
	bolt.payload = m_payload;
	bolt.payload.flavour = School();
	bolt.push = m_push;
	if (m_modifier == SpellSymbol::Explode) bolt.payload.blast = ScaledBlast(power);
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
	if (m_light) {
		// Ingwaz: one bigger light for as long as the plain cast would last;
		// Hagalaz: the flare, and no light left.
		if (m_modifier == SpellSymbol::Multiple) m_light->LightOn(ctx, ctx.power * m_grow, ctx.power);
		else m_light->Flare(ctx, ctx.power);
		return;
	}
	if (m_bolt) {
		if (m_modifier == SpellSymbol::Explode) {
			ProjectileSpec bolt = m_bolt->PartyBolt(ctx, ctx.power);
			Dress(bolt, ctx.power);
			ctx.services.spawnBolt(bolt);
			return;
		}
		// A VOLLEY down the caster's own lane, a beat apart, each a little off
		// the lane's line (Michael) - alternately either side, never past it.
		const int n = VolleyCount(ctx.power);
		const Vec3 across{ctx.dir.z, 0.0f, -ctx.dir.x};
		for (int i = 0; i < n; ++i) {
			ProjectileSpec bolt = m_bolt->PartyBolt(ctx, ctx.power * m_share);
			Dress(bolt, ctx.power);
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
	Dress(*bolt, Power()); // a burst at the spell's own power
	if (m_modifier != SpellSymbol::Explode) bolt->atk.damage *= m_share;
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
	m_grow = e.GetFloat("grow", m_grow);
	// Only a BoltSpell read `push`, so `[airbolt_volley] push = 1` was dead data
	// and a volley shoved by its form's number (code-review C19).
	m_push = static_cast<int>(e.GetFloat("push", static_cast<float>(m_push)));
}

} // namespace dungeon::game
