// ============================================================================
// Game/Balance.cpp — see Balance.h.
// ============================================================================
#include "Game/Balance.h"

#include "Game/Defense.h" // the attacker's type axis, kept pure for the harness

#include "Core/Log.h"
#include "Game/Catalog.h"

#include <charconv>
#include <format>

namespace dungeon::game {

namespace {

// The knob sheet (docs/combat.md part 5). Order = the editor dialog's rows.
constexpr BalanceField kBalanceFields[] = {
	{"unarmed_base", &Balance::unarmedBase},
	{"unarmed_speed", &Balance::unarmedSpeed},
	{"stat_damage", &Balance::statDamage},
	{"skill_damage", &Balance::skillDamage},
	{"damage_jitter", &Balance::damageJitter},
	{"crit_threshold", &Balance::critThreshold},
	{"fumble_threshold", &Balance::fumbleThreshold},
	{"margin_damage", &Balance::marginDamage},
	{"margin_cap", &Balance::marginCap},
	{"skill_curve", &Balance::skillCurve},
	{"skill_bonus", &Balance::skillBonus},
	{"skill_cap", &Balance::skillCap},
	{"stat_curve", &Balance::statCurve},
	{"stat_bonus", &Balance::statBonus},
	{"stat_cap", &Balance::statCap},
	{"stat_baseline", &Balance::statBaseline},
	{"defense_base", &Balance::defenseBase},
	{"avoid_slope", &Balance::avoidSlope},
	{"avoid_cap", &Balance::avoidCap},
	{"armor_light_penalty", &Balance::armorLightPenalty},
	{"armor_light_floor", &Balance::armorLightFloor},
	{"armor_light_str", &Balance::armorLightStr},
	{"armor_light_learn", &Balance::armorLightLearn},
	{"armor_medium_penalty", &Balance::armorMediumPenalty},
	{"armor_medium_floor", &Balance::armorMediumFloor},
	{"armor_medium_str", &Balance::armorMediumStr},
	{"armor_medium_learn", &Balance::armorMediumLearn},
	{"armor_heavy_penalty", &Balance::armorHeavyPenalty},
	{"armor_heavy_floor", &Balance::armorHeavyFloor},
	{"armor_heavy_str", &Balance::armorHeavyStr},
	{"armor_heavy_learn", &Balance::armorHeavyLearn},
	{"armor_offset_slope", &Balance::armorOffsetSlope},
	{"armor_short_penalty", &Balance::armorShortPenalty},
	{"armor_short_stamina", &Balance::armorShortStamina},
	{"resist_clamp", &Balance::resistClamp},
	{"potency_clamp", &Balance::potencyClamp},
	{"wound_floor", &Balance::woundFloor},
	{"speed_base", &Balance::speedBase},
	{"speed_stat", &Balance::speedStat},
	{"interval_min", &Balance::intervalMin},
	{"interval_max", &Balance::intervalMax},
	{"spell_stat", &Balance::spellStat},
	{"stoneskin_resist", &Balance::stoneskinResist},
	{"creep_rate", &Balance::creepRate},
	// The three resources (docs/health-and-healing.md). Grouped per pool rather
	// than per term so a row in the Balance dialog sits beside the ones it
	// trades against — health's whole story reads top to bottom, then stamina's.
	{"k_health", &Balance::kHealth},
	{"health_skill_slope", &Balance::healthSkillSlope},
	{"health_skill_cap", &Balance::healthSkillCap},
	{"health_regen", &Balance::healthRegen},
	{"health_regen_stat", &Balance::healthRegenStat},
	{"health_regen_max", &Balance::healthRegenMax},
	{"health_regen_slope", &Balance::healthRegenSlope},
	{"health_regen_cap", &Balance::healthRegenCap},
	{"k_stamina", &Balance::kStamina},
	{"stamina_skill_slope", &Balance::staminaSkillSlope},
	{"stamina_skill_cap", &Balance::staminaSkillCap},
	{"k_mana", &Balance::kMana},
	{"mana_skill_slope", &Balance::manaSkillSlope},
	{"mana_skill_cap", &Balance::manaSkillCap},
	{"mana_regen", &Balance::manaRegen},
	{"mana_regen_stat", &Balance::manaRegenStat},
	{"mana_regen_max", &Balance::manaRegenMax},
	{"mana_regen_slope", &Balance::manaRegenSlope},
	{"mana_regen_cap", &Balance::manaRegenCap},
	{"mana_exert", &Balance::manaExert},
	{"conditioning_xp", &Balance::conditioningXp},
	{"attunement_xp", &Balance::attunementXp},
	{"constitution_xp", &Balance::constitutionXp},
	// Supplies — grouped per meter, like the pools.
	{"food_max", &Balance::foodMax},
	{"food_rate", &Balance::foodRate},
	{"food_cond_slope", &Balance::foodCondSlope},
	{"food_cond_cap", &Balance::foodCondCap},
	{"food_exertion", &Balance::foodExertion},
	{"hunger_damage", &Balance::hungerDamage},
	{"water_max", &Balance::waterMax},
	{"water_rate", &Balance::waterRate},
	{"water_cond_slope", &Balance::waterCondSlope},
	{"water_cond_cap", &Balance::waterCondCap},
	{"water_exertion", &Balance::waterExertion},
	{"thirst_damage", &Balance::thirstDamage},
	{"rest_scale", &Balance::restScale},
	{"pace_slope", &Balance::paceSlope},
	{"pace_cap", &Balance::paceCap},
	{"stamina_swing", &Balance::staminaSwing},
	{"stamina_weight", &Balance::staminaWeight},
	{"stamina_step", &Balance::staminaStep},
	{"stamina_regen", &Balance::staminaRegen},
	{"stamina_regen_stat", &Balance::staminaRegenStat},
	{"stamina_regen_max", &Balance::staminaRegenMax},
	{"stamina_regen_slope", &Balance::staminaRegenSlope},
	{"stamina_regen_cap", &Balance::staminaRegenCap},
	{"stamina_holdoff", &Balance::staminaHoldoff},
	{"exhaust_damage", &Balance::exhaustDamage},
	{"exhaust_pace", &Balance::exhaustPace},
	{"exhaust_recover", &Balance::exhaustRecover},
	{"exert_cost", &Balance::exertCost},
	{"exert_max", &Balance::exertMax},
	{"fumble_severe_face", &Balance::fumbleSevereFace},
	{"fumble_recover", &Balance::fumbleRecover},
	{"stabilize_time", &Balance::stabilizeTime},
	{"stabilize_health", &Balance::stabilizeHealth},
	{"overkill", &Balance::overkill},
	{"bump_damage", &Balance::bumpDamage},
	{"fall_damage", &Balance::fallDamage},
	{"threat_scale", &Balance::threatScale},
	{"threat_threshold", &Balance::threatThreshold},
	{"threat_switch", &Balance::threatSwitch},
	{"threat_decay", &Balance::threatDecay},
};

// Whitespace/comma tokens (the catalog list convention).
std::vector<std::string> Tokens(std::string_view s) {
	std::vector<std::string> out;
	std::string cur;
	for (const char c : s) {
		if (c == ' ' || c == '\t' || c == ',') {
			if (!cur.empty()) out.push_back(std::move(cur));
			cur.clear();
		} else {
			cur += c;
		}
	}
	if (!cur.empty()) out.push_back(std::move(cur));
	return out;
}

float FloatOf(const std::string& tok, float fallback) {
	float v = fallback;
	std::from_chars(tok.data(), tok.data() + tok.size(), v);
	return v;
}

// Short/full stat-name tokens → the Character stat id ("" = unknown).
std::string_view NormalizeStat(std::string_view tok) {
	if (tok == "str" || tok == "strength") return "strength";
	if (tok == "dex" || tok == "dexterity") return "dexterity";
	if (tok == "vit" || tok == "vitality") return "vitality";
	if (tok == "wil" || tok == "will" || tok == "willpower") return "willpower";
	if (tok == "int" || tok == "intelligence") return "intelligence";
	return {};
}

} // namespace

Balance::Balance() {
	// The attack identity table (docs/combat.md part 1): id + damage type is
	// C++ — the closed list — with first-cut numbers attacks.cat overrides.
	// The type is an ID here; Load resolves it against the project's
	// damagetypes.cat. A verb naming a type the project does not define is a
	// warning at load, not a silent retype.
	attacks = {
		{"stab", "pierce", {}, 0.8f, 5.0f, 0.8f, 0.8f},
		{"jab", "pierce", {}, 0.7f, 5.0f, 0.7f, 0.7f},
		{"thrust", "pierce", {}, 1.2f, 0.0f, 1.15f, 1.3f},
		{"slash", "slash", {}, 1.0f, 0.0f, 1.0f, 1.0f},
		{"hack", "slash", {}, 1.15f, -3.0f, 1.15f, 1.3f},
		{"chop", "slash", {}, 1.3f, -5.0f, 1.25f, 1.5f},
		{"bash", "bash", {}, 1.15f, -5.0f, 1.2f, 1.5f},
		{"swing", "bash", {}, 1.0f, 0.0f, 1.0f, 1.2f},
		{"punch", "bash", {}, 1.0f, 0.0f, 1.0f, 0.8f},
		{"kick", "bash", {}, 1.15f, 0.0f, 1.15f, 1.2f},
	};
	m_neutral = {"", "bash", {}, 1.0f, 0.0f, 1.0f, 1.0f};
}

Balance::ArmorRules Balance::Armor(ArmorClass c) const {
	switch (c) {
	case ArmorClass::Light:
		return {armorLightPenalty, armorLightFloor, armorLightStr, armorLightLearn};
	case ArmorClass::Medium:
		return {armorMediumPenalty, armorMediumFloor, armorMediumStr,
				armorMediumLearn};
	case ArmorClass::Heavy:
		return {armorHeavyPenalty, armorHeavyFloor, armorHeavyStr, armorHeavyLearn};
	default:
		return {}; // unarmored: no penalty, no floor, nothing to ask of STR
	}
}

// The knob sheet's flat floats, gathered into the shape the pure arithmetic
// wants. THE CURVE FORM IS SHARED (skillCurve) and only the slope and cap
// differ per resource — the same bargain AvoidCurve makes, and for the same
// reason: which SHAPE feels right is one judgement made once against the
// Balance dialog's graph, while how far each particular term may reach is a
// per-resource decision.
//
// The BASELINE stays 0 on both skill curves: an untrained practice is simply no
// help, never a handicap. Only stats get a baseline of 10, and that curve is
// passed separately (RegenPerSec takes it) precisely because it is the STAT's
// curve and not this resource's.
resource::Rules Balance::Resource(resource::Kind kind) const {
	const auto form = static_cast<CurveForm>(static_cast<int>(skillCurve));
	resource::Rules r;
	switch (kind) {
	case resource::Kind::Health:
		r.perAptitude = kHealth;
		r.skillMax = {form, healthSkillSlope, healthSkillCap, 0.0f};
		r.regenBase = healthRegen;
		r.regenPerAptitude = healthRegenStat;
		r.regenPerMax = healthRegenMax;
		r.skillRegen = {form, healthRegenSlope, healthRegenCap, 0.0f};
		return r;
	case resource::Kind::Stamina:
		r.perAptitude = kStamina;
		r.skillMax = {form, staminaSkillSlope, staminaSkillCap, 0.0f};
		r.regenBase = staminaRegen;
		r.regenPerAptitude = staminaRegenStat;
		r.regenPerMax = staminaRegenMax;
		r.skillRegen = {form, staminaRegenSlope, staminaRegenCap, 0.0f};
		return r;
	case resource::Kind::Mana:
		r.perAptitude = kMana;
		r.skillMax = {form, manaSkillSlope, manaSkillCap, 0.0f};
		r.regenBase = manaRegen;
		r.regenPerAptitude = manaRegenStat;
		r.regenPerMax = manaRegenMax;
		r.skillRegen = {form, manaRegenSlope, manaRegenCap, 0.0f};
		return r;
	default:
		return r; // an out-of-range kind gets the inert defaults, not a guess
	}
}

resource::PoolRules Balance::Resources() const {
	return {Resource(resource::Kind::Health), Resource(resource::Kind::Stamina),
			Resource(resource::Kind::Mana)};
}

resource::SupplyRules Balance::SupplyOf(resource::Supply which) const {
	const auto form = static_cast<CurveForm>(static_cast<int>(skillCurve));
	if (which == resource::Supply::Water)
		return {waterMax, waterRate,
				{form, waterCondSlope, waterCondCap, 0.0f}, waterExertion,
				thirstDamage};
	return {foodMax, foodRate, {form, foodCondSlope, foodCondCap, 0.0f},
			foodExertion, hungerDamage};
}

const AttackSpec* Balance::FindAttack(std::string_view id) const {
	for (const AttackSpec& a : attacks)
		if (a.id == id) return &a;
	return nullptr;
}

float Balance::ClampResist(float sum, float natureCell) const {
	// An authored nature cell of 1.0 is IMMUNITY, and beyond it ABSORPTION: 1.5
	// means the target drinks half again of what it is dealt, as healing. Both
	// escape the clamp — they say what a thing IS (a fire golem, a water
	// elemental), rather than stacking mitigation the clamp exists to cap.
	if (natureCell >= 1.0f) return natureCell;
	if (sum > resistClamp) return resistClamp;
	if (sum < -resistClamp) return -resistClamp;
	return sum;
}

float Balance::Potent(float amount, const ResistTable& potency,
					  DamageType type) const {
	// The adapter: the arithmetic and its reasoning live in Game/Defense.h, which is
	// pure and therefore measurable; this only hands it the knob.
	return defense::Potent(amount, potency, type, potencyClamp);
}

void Balance::Load(const Catalog& balanceCat, const Catalog& attacksCat,
				   const DamageTypeBook& types) {
	if (const CatalogEntry* e = balanceCat.Find("formula"))
		for (const BalanceField& f : kBalanceFields)
			this->*(f.value) = e->GetFloat(f.key, this->*(f.value));

	// Resolve every verb's damage type against the loaded book. This is the
	// moment the C++ identity table meets the project's vocabulary.
	auto resolve = [&types](AttackSpec& a) {
		if (!types.Find(a.typeId, a.type))
			log::Warn("attack '{}' deals damage type '{}', which this project "
					  "does not define (damagetypes.cat)",
					  a.id.empty() ? "unarmed" : a.id, a.typeId);
	};
	for (AttackSpec& a : attacks) resolve(a);
	resolve(m_neutral);

	for (AttackSpec& a : attacks)
		if (const CatalogEntry* e = attacksCat.Find(a.id)) {
			a.dmg = e->GetFloat("damage", a.dmg);
			a.acc = e->GetFloat("accuracy", a.acc);
			a.pace = e->GetFloat("speed", a.pace);
			a.stam = e->GetFloat("stamina", a.stam);
		}
}

void Balance::Save(Catalog& balanceCat, Catalog& attacksCat) const {
	CatalogEntry formula;
	formula.id = "formula";
	if (const CatalogEntry* e = balanceCat.Find("formula"))
		formula = *e; // keep unknown fields (forward compat)
	for (const BalanceField& f : kBalanceFields)
		formula.Set(f.key, std::format("{:g}", this->*(f.value)));
	balanceCat.Add(std::move(formula));

	for (const AttackSpec& a : attacks) {
		CatalogEntry e;
		e.id = a.id;
		if (const CatalogEntry* prev = attacksCat.Find(a.id)) e = *prev;
		e.Set("damage", std::format("{:g}", a.dmg));
		e.Set("accuracy", std::format("{:g}", a.acc));
		e.Set("speed", std::format("{:g}", a.pace));
		e.Set("stamina", std::format("{:g}", a.stam));
		attacksCat.Add(std::move(e));
	}
}

std::span<const BalanceField> BalanceFields() { return kBalanceFields; }

const std::vector<std::string>& SchoolStats(SpellSymbol school) {
	// docs/combat.md part 2: earth + fire → INT, air + water → WIL.
	static const std::vector<std::string> kInt{"intelligence"};
	static const std::vector<std::string> kWil{"willpower"};
	switch (school) {
	case SpellSymbol::Air:
	case SpellSymbol::Water: return kWil;
	default: return kInt;
	}
}

const std::vector<std::string>& UnarmedStats() {
	static const std::vector<std::string> kStr{"strength"};
	return kStr;
}

std::vector<std::string> ParseStatList(std::string_view spec,
									   std::string_view owner) {
	std::vector<std::string> out;
	for (const std::string& tok : Tokens(spec)) {
		const std::string_view stat = NormalizeStat(tok);
		if (stat.empty())
			log::Warn("{}: unknown stat '{}' in stats=", owner, tok);
		else
			out.emplace_back(stat);
	}
	return out;
}

void ParseResists(std::string_view spec, ResistTable& out,
				  std::string_view owner, const DamageTypeBook& types) {
	const std::vector<std::string> toks = Tokens(spec);
	for (size_t i = 0; i + 1 < toks.size(); i += 2) {
		DamageType type;
		if (!types.Find(toks[i], type)) {
			log::Warn("{}: unknown damage type '{}' in resists=", owner, toks[i]);
			continue;
		}
		out[type] = FloatOf(toks[i + 1], 0.0f);
	}
	if (toks.size() % 2 != 0)
		log::Warn("{}: resists= has a dangling token '{}'", owner, toks.back());
}

} // namespace dungeon::game
