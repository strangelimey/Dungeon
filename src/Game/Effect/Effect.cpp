// ============================================================================
// Game/Effect/Effect.cpp — see Effect.h.
// ============================================================================
#include "Game/Effect/Effect.h"

#include "Core/Log.h"
#include "Game/Catalog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>

namespace dungeon::game::fx {

// --- EffectKind ---------------------------------------------------------------

EffectKind::EffectKind(std::string id, Category category, std::string nameKey,
					   Stacking stacking)
	: m_id(std::move(id)), m_nameKey(std::move(nameKey)),
	  m_category(category), m_stacking(stacking) {}

std::string_view EffectKind::NameKey(const Inst&) const { return m_nameKey; }

DamageType EffectKind::DamageTypeOf(const Inst&) const { return m_damageType; }

void EffectKind::ApplyOverrides(const CatalogEntry& e) {
	m_nameKey = e.Get("name", m_nameKey);
	m_iconItem = e.Get("icon", m_iconItem);
	m_applyParty = e.Get("apply_party", m_applyParty);
	m_applyMonster = e.Get("apply_monster", m_applyMonster);
	m_plume = e.GetBool("plume", m_plume);
	if (const std::string type = e.Get("damage_type", ""); !type.empty())
		if (!ParseDamageType(type, m_damageType))
			log::Warn("effects.cat [{}]: unknown damage_type '{}'", m_id, type);
	if (const std::string school = e.Get("school", ""); !school.empty())
		if (!ParseSymbol(school, m_school))
			log::Warn("effects.cat [{}]: unknown school '{}'", m_id, school);
	const std::string stacking = e.Get("stacking", "");
	if (stacking == "refresh") m_stacking = Stacking::Refresh;
	else if (stacking == "school") m_stacking = Stacking::RefreshPerSchool;
	else if (stacking == "stack") m_stacking = Stacking::Stack;
	else if (!stacking.empty())
		log::Warn("effects.cat [{}]: unknown stacking '{}'", m_id, stacking);
}

// The hooks all default to "this effect doesn't care about that stage".
void EffectKind::OnDeflect(Inst&, DamageEvent&, ITarget&) const {}
float EffectKind::ResistFor(const Inst&, DamageType, const Knobs&) const {
	return 0.0f;
}
void EffectKind::OnAbsorb(Inst&, float&, const DamageEvent&, ITarget&) const {}
void EffectKind::OnStruck(Inst&, const DamageEvent&, ITarget&, ITarget*) const {}

float EffectKind::StatBonus(const Inst&, std::string_view) const { return 0.0f; }
float EffectKind::SpeedScale(const Inst&) const { return 1.0f; }

// --- the event presets --------------------------------------------------------

DamageEvent DamageEvent::Blow(DamageType type, float amount, float accuracy,
							  int source) {
	return {.type = type, .amount = amount, .accuracy = accuracy,
			.delivery = Delivery::Melee, .source = source};
}

DamageEvent DamageEvent::Bolt(DamageType type, float amount, float accuracy,
							  int source) {
	return {.type = type, .amount = amount, .accuracy = accuracy,
			.delivery = Delivery::Ranged, .source = source};
}

DamageEvent DamageEvent::Tick(DamageType type, float amount, int source) {
	return {.type = type, .amount = amount, .delivery = Delivery::Tick,
			.source = source, .rolled = false, .soaked = false,
			.resisted = false};
}

DamageEvent DamageEvent::Impact(DamageType type, float amount, int source) {
	return {.type = type, .amount = amount, .delivery = Delivery::Impact,
			.source = source, .rolled = false, .soaked = false,
			.resisted = false};
}

// --- authored procs -----------------------------------------------------------

void ParseProcs(std::string_view spec, std::vector<Proc>& out,
				std::string_view where) {
	size_t start = 0;
	while (start < spec.size()) {
		size_t end = spec.find_first_of(",;", start);
		if (end == std::string_view::npos) end = spec.size();
		const std::string_view entry = spec.substr(start, end - start);
		start = end + 1;
		// Split the entry on whitespace: <id> <magnitude> <seconds> [chance].
		std::array<std::string_view, 4> token{};
		size_t n = 0, i = 0;
		while (i < entry.size() && n < token.size()) {
			while (i < entry.size() && std::isspace(static_cast<unsigned char>(entry[i]))) ++i;
			const size_t from = i;
			while (i < entry.size() && !std::isspace(static_cast<unsigned char>(entry[i]))) ++i;
			if (i > from) token[n++] = entry.substr(from, i - from);
		}
		if (n == 0) continue; // blank entry (trailing comma) — not an error
		if (n < 3) {
			log::Warn("{}: on-hit '{}' needs <id> <magnitude> <seconds> [chance]",
					  where, entry);
			continue;
		}
		Proc proc;
		proc.id = std::string(token[0]);
		proc.magnitude = std::strtof(std::string(token[1]).c_str(), nullptr);
		proc.duration = std::strtof(std::string(token[2]).c_str(), nullptr);
		if (n >= 4) proc.chance = std::strtof(std::string(token[3]).c_str(), nullptr);
		out.push_back(std::move(proc));
	}
}

void ApplyProcs(ITarget& target, std::span<const Proc> procs,
				std::optional<SpellSymbol> school, int source,
				const EffectBook& book, std::mt19937& rng) {
	std::uniform_real_distribution<float> roll(0.0f, 1.0f);
	for (const Proc& proc : procs) {
		if (proc.magnitude <= 0.0f || proc.duration <= 0.0f) continue;
		if (roll(rng) > proc.chance) continue;
		const EffectKind* kind = book.Find(proc.id);
		if (!kind) {
			log::Warn("on-hit proc names effect '{}', which has no kind", proc.id);
			continue;
		}
		const SpellSymbol flavour = school.value_or(kind->DefaultSchool());
		// IMMUNITY refuses outright: flames find no purchase on a fire
		// elemental, and it would be odd to watch one burn for nothing.
		if (kind->Kind() == Category::Dot) {
			const Inst probe{kind, flavour};
			if (target.Resist(kind->DamageTypeOf(probe)) >= 1.0f) continue;
		}
		// Announce only what's NEW: a refresh is the same affliction lasting
		// longer, not a fresh alarm.
		const bool already =
			std::ranges::any_of(target.Effects(), [&](const Inst& e) {
				return e.kind == kind;
			});
		Apply(target.Effects(), *kind, flavour, proc.magnitude, proc.duration,
			  source);
		if (!already) target.SayApplied(*kind);
	}
}

// --- the pipeline -------------------------------------------------------------

float EffectResist(const std::vector<Inst>& effects, DamageType type,
				   const Knobs& knobs) {
	float sum = 0.0f;
	for (const Inst& e : effects)
		if (e.kind) sum += e.kind->ResistFor(e, type, knobs);
	return sum;
}

namespace {
// Drop the effects that spent themselves during a stage. They have already
// said their own line (a veil bursting, a ward stilling), which is exactly why
// they are removed HERE and not left to the aging tick's generic fade line.
void DropSpent(std::vector<Inst>& effects) {
	std::erase_if(effects, [](const Inst& e) { return e.timeLeft <= 0.0f; });
}
} // namespace

void Deal(DamageEvent& ev, ITarget& target, const StrikeRules& rules,
		  std::mt19937& rng) {
	// --- 1. deflect: an effect may turn it aside before anything is rolled ---
	for (Inst& e : target.Effects()) {
		if (e.kind) e.kind->OnDeflect(e, ev, target);
		if (ev.deflected) break;
	}
	DropSpent(target.Effects());
	if (ev.deflected) return;

	// --- 2/3. strike and mitigate -------------------------------------------
	// A rolled event goes through the shared resolver, which does the hit roll,
	// the damage jitter, soak and resist in one — the same maths every attack
	// has always used. An unrolled one (a tick, a bump, an enchanted blow's
	// elemental half) lands for what it says, subject only to the parts its
	// flags admit.
	float damage = 0.0f;
	if (ev.rolled) {
		const DefenseProfile def{target.Evasion(), ev.soaked ? target.Soak() : 0.0f,
								 ev.resisted ? target.Resist(ev.type) : 0.0f};
		const AttackResult r = ResolveAttack({ev.amount, ev.accuracy, ev.type},
											 def, rules, rng);
		ev.hit = r.hit;
		damage = r.damage;
	} else {
		ev.hit = true;
		damage = ev.amount;
		if (ev.soaked) damage -= target.Soak();
		if (ev.resisted) damage *= 1.0f - target.Resist(ev.type);
		if (damage < 0.0f) damage = 0.0f;
	}
	if (!ev.hit) return;

	// --- 4. absorb: pooled effects eat what they can -------------------------
	for (Inst& e : target.Effects()) {
		if (e.kind) e.kind->OnAbsorb(e, damage, ev, target);
		if (damage <= 0.0f) break;
	}
	DropSpent(target.Effects());

	// --- 5. apply: the one per-side stage ------------------------------------
	ev.dealt = damage;
	if (damage > 0.0f) target.Wound(damage, ev);
	// (stage 6 is React, below — the caller runs it once it has said its piece)
}

void React(const DamageEvent& ev, ITarget& target, ITarget* attacker) {
	if (!ev.hit || ev.deflected) return; // nothing landed to answer
	// By INDEX, re-reading the list each step: a reaction reaches back into the
	// world (a reprisal deals damage of its own), so the list must not be held
	// across the call. The standing rule for a reaction is that it may damage
	// the ATTACKER but must not add effects to its own bearer.
	for (size_t i = 0; i < target.Effects().size(); ++i) {
		Inst& e = target.Effects()[i];
		if (e.kind) e.kind->OnStruck(e, ev, target, attacker);
	}
	DropSpent(target.Effects());
}

// --- landing one -------------------------------------------------------------

Inst& Apply(std::vector<Inst>& effects, const EffectKind& kind,
			SpellSymbol school, float magnitude, float duration, int source) {
	switch (kind.Stack()) {
	case Stacking::Refresh:
		std::erase_if(effects,
					  [&](const Inst& e) { return e.kind == &kind; });
		break;
	case Stacking::RefreshPerSchool:
		std::erase_if(effects, [&](const Inst& e) {
			return e.kind == &kind && e.school == school;
		});
		break;
	case Stacking::Stack:
		break; // every application is its own instance
	}
	effects.push_back({&kind, school, magnitude, duration, duration, source});
	return effects.back();
}

// --- EffectBook ---------------------------------------------------------------

void EffectBook::Build(const Catalog& catalog) {
	// The classes ARE the effect table (Effect/AllEffects.cpp); the catalog
	// gets the last word on numbers and look only.
	m_kinds = MakeAllEffects();
	for (const CatalogEntry& e : catalog.Entries()) {
		EffectKind* kind = nullptr;
		for (const auto& k : m_kinds)
			if (k->Id() == e.id) { kind = k.get(); break; }
		if (!kind) {
			log::Warn("effects.cat entry '{}' has no effect class; ignored", e.id);
			continue;
		}
		kind->ApplyOverrides(e);
	}
}

const EffectKind* EffectBook::Find(std::string_view id) const {
	for (const auto& k : m_kinds)
		if (k->Id() == id) return k.get();
	return nullptr;
}

const EffectKind* EffectBook::FindLegacy(std::string_view token,
										 SpellSymbol school) const {
	// Saves written before the effects system stored the CATEGORY as the kind
	// token, with the school telling the four wards apart. Map those forward
	// so an old save keeps its wards; the ids are the save format now.
	if (token != "ward") return Find(token); // poison / bleed / sight, unchanged
	switch (school) {
	case SpellSymbol::Fire:  return Find("fireshield");
	case SpellSymbol::Earth: return Find("stoneskin");
	case SpellSymbol::Air:   return Find("windward");
	case SpellSymbol::Water: return Find("waterveil");
	default: return nullptr;
	}
}

} // namespace dungeon::game::fx
