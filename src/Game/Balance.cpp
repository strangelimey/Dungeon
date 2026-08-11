// ============================================================================
// Game/Balance.cpp — see Balance.h.
// ============================================================================
#include "Game/Balance.h"

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
	{"acc_base", &Balance::accBase},
	{"acc_stat", &Balance::accStat},
	{"acc_skill", &Balance::accSkill},
	{"hit_floor", &Balance::hitFloor},
	{"hit_ceil", &Balance::hitCeil},
	{"roll_scale", &Balance::rollScale},
	{"crit_threshold", &Balance::critThreshold},
	{"fumble_threshold", &Balance::fumbleThreshold},
	{"margin_damage", &Balance::marginDamage},
	{"margin_cap", &Balance::marginCap},
	{"resist_clamp", &Balance::resistClamp},
	{"wound_floor", &Balance::woundFloor},
	{"speed_base", &Balance::speedBase},
	{"speed_stat", &Balance::speedStat},
	{"interval_min", &Balance::intervalMin},
	{"interval_max", &Balance::intervalMax},
	{"spell_stat", &Balance::spellStat},
	{"stoneskin_resist", &Balance::stoneskinResist},
	{"creep_rate", &Balance::creepRate},
	{"vit_exertion", &Balance::vitExertion},
	{"k_health", &Balance::kHealth},
	{"k_stamina", &Balance::kStamina},
	{"k_mana", &Balance::kMana},
	{"stamina_swing", &Balance::staminaSwing},
	{"stamina_weight", &Balance::staminaWeight},
	{"stamina_step", &Balance::staminaStep},
	{"stamina_regen", &Balance::staminaRegen},
	{"stamina_regen_max", &Balance::staminaRegenMax},
	{"stamina_holdoff", &Balance::staminaHoldoff},
	{"exhaust_damage", &Balance::exhaustDamage},
	{"exhaust_pace", &Balance::exhaustPace},
	{"exhaust_recover", &Balance::exhaustRecover},
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
		{"stab", "pierce", {}, 0.8f, 0.05f, 0.8f, 0.8f},
		{"jab", "pierce", {}, 0.7f, 0.05f, 0.7f, 0.7f},
		{"thrust", "pierce", {}, 1.2f, 0.0f, 1.15f, 1.3f},
		{"slash", "slash", {}, 1.0f, 0.0f, 1.0f, 1.0f},
		{"hack", "slash", {}, 1.15f, -0.03f, 1.15f, 1.3f},
		{"chop", "slash", {}, 1.3f, -0.05f, 1.25f, 1.5f},
		{"bash", "bash", {}, 1.15f, -0.05f, 1.2f, 1.5f},
		{"swing", "bash", {}, 1.0f, 0.0f, 1.0f, 1.2f},
		{"punch", "bash", {}, 1.0f, 0.0f, 1.0f, 0.8f},
		{"kick", "bash", {}, 1.15f, 0.0f, 1.15f, 1.2f},
	};
	m_neutral = {"", "bash", {}, 1.0f, 0.0f, 1.0f, 1.0f};
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
