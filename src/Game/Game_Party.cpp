// ============================================================================
// Game/Game_Party.cpp - making a party (docs/party-creation-plan.md).
//
// One function turns a member as CHOSEN (party::MemberSpec) into a Character:
// BuildMember. The creation page, the `newparty` dev command and the eval suite
// all go through it, so a party made by a script is the party a player would
// have made with the same choices. The arithmetic of stats and points is the
// pure Game/PartyRules.h; what lives here is the part that needs the world's
// catalogs - the race's bases, pace and resists, the skills this world can
// train, and where a starting item goes.
// ============================================================================
#include "Game/Game.h"

#include "Core/Log.h"
#include "Game/Balance.h" // ParseResists

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>

namespace dungeon::game {

namespace {

// What races.cat says a race does to the numbers PartyRules owns.
party::RaceStats RaceStatsOf(const CatalogEntry& race) {
	party::RaceStats rs;
	for (size_t i = 0; i < party::kStats; ++i)
		rs.mods[i] = std::atoi(race.Get(kStats[i].id, "0").c_str());
	rs.extraPoints = std::atoi(race.Get("extra_points", "0").c_str());
	return rs;
}

// "c04040" / "#c04040" -> an opaque colour; false for anything else.
bool ParseHexColor(std::string_view s, std::array<float, 4>& out) {
	if (!s.empty() && s[0] == '#') s.remove_prefix(1);
	if (s.size() != 6) return false;
	unsigned v = 0;
	for (char ch : s) {
		const int d = ch >= '0' && ch <= '9'   ? ch - '0'
					  : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
					  : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
											   : -1;
		if (d < 0) return false;
		v = v * 16 + static_cast<unsigned>(d);
	}
	out = {((v >> 16) & 0xff) / 255.0f, ((v >> 8) & 0xff) / 255.0f, (v & 0xff) / 255.0f, 1.0f};
	return true;
}

std::string HexColor(const Vec4& c) {
	const auto b = [](float f) { return static_cast<int>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
	return std::format("{:02x}{:02x}{:02x}", b(c.x), b(c.y), b(c.z));
}

std::vector<std::string> SplitList(std::string_view s) {
	std::vector<std::string> out;
	size_t start = 0;
	while (start <= s.size()) {
		const size_t comma = s.find(',', start);
		const std::string_view part =
			s.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
		if (!part.empty()) out.emplace_back(part);
		if (comma == std::string_view::npos) break;
		start = comma + 1;
	}
	return out;
}

} // namespace

void Game::ApplyRaceResists(Character& member) const {
	member.natureResists = ResistTable{};
	if (!m_world) return;
	const CatalogEntry* race = m_project.races.Find(member.raceId);
	if (!race) return;
	const std::string spec = race->Get("resists", "");
	if (!spec.empty())
		ParseResists(spec, member.natureResists, "races.cat " + member.raceId,
					 m_world->DamageTypes());
}

std::optional<Character> Game::BuildMember(const party::MemberSpec& spec,
										   std::string& why) const {
	if (!m_world) {
		why = "no world is loaded";
		return std::nullopt;
	}
	const bool keepName = spec.premade >= 0 && spec.name.empty();
	if (!keepName && !party::NameValid(spec.name)) {
		why = std::format("'{}' is not a usable name (1-{} characters, no underscore)",
						  spec.name, party::kMaxNameLength);
		return std::nullopt;
	}
	if (!spec.portrait.empty() && !m_portraitCatalog.Find(spec.portrait)) {
		why = std::format("portrait {} is not in portraits.cat", spec.portrait);
		return std::nullopt;
	}

	Character c;
	if (spec.premade >= 0) {
		// One of the default four, exactly as authored (the eval suites measure
		// them): only who they are, their face and their colour can change.
		std::vector<Character> defaults = CreateDefaultParty();
		if (spec.premade >= static_cast<int>(defaults.size())) {
			why = std::format("there is no premade member {}", spec.premade);
			return std::nullopt;
		}
		c = std::move(defaults[static_cast<size_t>(spec.premade)]);
	} else {
		const CatalogEntry* race = m_project.races.Find(spec.race);
		if (!race) {
			why = std::format("race '{}' is not in races.cat", spec.race);
			return std::nullopt;
		}
		const party::RaceStats rs = RaceStatsOf(*race);
		if (!party::SpendingValid(rs, spec.spent)) {
			why = std::format("{} points spent of {}", party::PointsSpent(spec.spent),
							  party::PointBudget(rs));
			return std::nullopt;
		}
		const party::StatArray stats = party::Stats(rs, spec.spent);
		for (size_t i = 0; i < party::kStats; ++i) c.*(kStats[i].value) = stats[i];
		c.raceId = spec.race;
		c.baseHealth = race->GetFloat("base_health", 20.0f);
		c.baseStamina = race->GetFloat("base_stamina", 20.0f);
		c.baseMana = race->GetFloat("base_mana", 10.0f);
		c.moveSpeed = race->GetFloat("pace", 1.0f);

		// Two skills, each boosted to kSkillBoostLevel - only ones this world
		// can train, or the boost would sit on a skill nothing ever reads.
		if (!party::SkillPicksValid(spec.skills)) {
			why = std::format("at most {} different skills", party::kSkillPicks);
			return std::nullopt;
		}
		const std::vector<std::string> trainable = m_world->TrainableSkills();
		for (const std::string& skill : spec.skills) {
			if (std::ranges::find(trainable, skill) == trainable.end()) {
				why = std::format("'{}' is not a skill this world trains", skill);
				return std::nullopt;
			}
			c.skillXp[skill] = party::kSkillBoostXp;
		}

		// Two starting items, from the world's list, each where it belongs: a
		// weapon in the first empty hand, armour where it is worn, anything else
		// in the pack.
		if (spec.items.size() > static_cast<size_t>(party::kItemPicks)) {
			why = std::format("at most {} starting items", party::kItemPicks);
			return std::nullopt;
		}
		for (const std::string& item : spec.items) {
			const CatalogEntry* e = m_project.FindItem(item);
			if (!e || std::ranges::find(m_project.startItems, item) == m_project.startItems.end()) {
				why = std::format("'{}' is not on this world's starting list", item);
				return std::nullopt;
			}
			bool placed = false;
			if (m_project.weapons.Find(item)) {
				for (int h = 0; h < 2 && !placed; ++h)
					if (c.inventory.Hand(h).Empty()) {
						c.inventory.Hand(h).typeId = item;
						placed = true;
					}
			}
			WearSlot wear = WearSlot::None;
			if (!placed && ParseWearSlot(e->Get("wear", ""), wear)) {
				for (int s = 0; s < kEquipCount && !placed; ++s) {
					ItemSlot& slot = c.inventory.equipment[static_cast<size_t>(s)];
					if (slot.Empty() && WearSlotFits(wear, static_cast<EquipSlot>(s))) {
						slot.typeId = item;
						placed = true;
					}
				}
			}
			if (!placed && !c.inventory.Stow(item)) {
				why = std::format("no room for {}", item);
				return std::nullopt;
			}
		}
		// What CreateDefaultParty does for each of its four.
		c.RecomputeMaxima({});
		c.health = c.maxHealth;
		c.stamina = c.maxStamina;
		c.mana = c.maxMana;
		fx::ReserveEffects(c.effects);
	}
	if (!keepName) c.name = spec.name;
	if (!spec.portrait.empty()) c.portraitId = spec.portrait;
	if (spec.premade < 0 || spec.colorSet)
		c.portraitColor = {spec.color[0], spec.color[1], spec.color[2], spec.color[3]};
	ApplyRaceResists(c);
	return c;
}

bool Game::BuildParty(const std::vector<party::MemberSpec>& specs, std::vector<Character>& out,
					  std::string& why) const {
	if (specs.size() < party::kMinMembers || specs.size() > party::kMaxMembers) {
		why = std::format("a party is {} to {} members, not {}", party::kMinMembers,
						  party::kMaxMembers, specs.size());
		return false;
	}
	out.clear();
	for (size_t i = 0; i < specs.size(); ++i) {
		std::optional<Character> c = BuildMember(specs[i], why);
		if (!c) {
			why = std::format("member {}: {}", i, why);
			return false;
		}
		out.push_back(std::move(*c));
	}
	return true;
}

void Game::RegisterPartyCreationCommands() {
	// A party from one line - the harness's way in, and the creation page's twin.
	// Members are split by '|'; each is key=value words:
	//   name=Old_Tom race=dwarf portrait=portrait020 color=c08040
	//   points=2,0,3,0,0 skills=blunt,conditioning items=club,padded_jack
	// or premade=<0..3> (one of the default four; name/portrait/color may still
	// be given). A name's underscores are spaces, as in a save. `default` starts
	// with the default four. Either way a new game starts at once, as `newgame`.
	m_console.Register(
		{.name = "newparty",
		 .group = CmdGroup::Characters,
		 .params = "default\n<member> [| <member> ...]",
		 .summary = "start a new game with a party built from a spec (party creation)"},
		[this](const std::vector<std::string>& args) {
			if (args.empty()) {
				m_console.RefuseUsage();
				return;
			}
			if (!m_world || !m_ui.onStartNewGame) {
				m_console.Refuse("load a world first");
				return;
			}
			if (args.size() == 1 && args[0] == "default") {
				m_startParty.reset();
				m_ui.onStartNewGame();
				m_console.Print("new game with the default party");
				return;
			}
			std::vector<party::MemberSpec> specs(1);
			for (const std::string& word : args) {
				if (word == "|") {
					specs.emplace_back();
					continue;
				}
				const size_t eq = word.find('=');
				if (eq == std::string::npos) {
					m_console.Refuse(std::format("'{}' is not key=value", word));
					return;
				}
				const std::string key = word.substr(0, eq), val = word.substr(eq + 1);
				party::MemberSpec& m = specs.back();
				if (key == "name") {
					m.name = val;
					std::ranges::replace(m.name, '_', ' ');
				} else if (key == "race") {
					m.race = val;
				} else if (key == "portrait") {
					m.portrait = val;
				} else if (key == "color") {
					if (!ParseHexColor(val, m.color)) {
						m_console.Refuse(std::format("'{}' is not a colour (rrggbb)", val));
						return;
					}
					m.colorSet = true;
				} else if (key == "points") {
					const std::vector<std::string> p = SplitList(val);
					if (p.size() != party::kStats) {
						m_console.Refuse("points= takes five numbers (str,dex,vit,wil,int)");
						return;
					}
					for (size_t i = 0; i < party::kStats; ++i) m.spent[i] = std::atoi(p[i].c_str());
				} else if (key == "skills") {
					m.skills = SplitList(val);
				} else if (key == "items") {
					m.items = SplitList(val);
				} else if (key == "premade") {
					m.premade = std::atoi(val.c_str());
				} else {
					m_console.Refuse(std::format("unknown key '{}'", key));
					return;
				}
			}
			std::vector<Character> built;
			std::string why;
			if (!BuildParty(specs, built, why)) {
				m_console.Refuse(why);
				return;
			}
			m_startParty = std::move(built);
			m_ui.onStartNewGame();
			m_console.Print(std::format("new game with a party of {}", specs.size()));
		});

	// Everything a member was MADE from, one line each - what `party` (the
	// pools) does not show and a round-trip test needs to compare.
	m_console.Register(
		{.name = "roster",
		 .group = CmdGroup::Characters,
		 .summary = "each member's name, race, face, colour, stats, skills and kit"},
		[this](const std::vector<std::string>&) {
			for (size_t i = 0; i < m_characters.size(); ++i) {
				const Character& c = m_characters[i];
				std::string skills;
				for (const auto& [id, xp] : c.skillXp)
					if (xp > 0.0f)
						skills += std::format("{}{}:{}", skills.empty() ? "" : ",", id,
											  Character::LevelForXp(xp));
				std::string kit;
				for (const ItemSlot& s : c.inventory.equipment)
					if (!s.Empty()) kit += (kit.empty() ? "" : ",") + s.typeId;
				for (const Pack& p : c.inventory.packs)
					for (const ItemSlot& s : p.contents)
						if (!s.Empty()) kit += (kit.empty() ? "" : ",") + ("pack:" + s.typeId);
				m_console.Print(std::format(
					"roster {} {} race={} portrait={} color={} pace={:.2f} "
					"stats={},{},{},{},{} bases={:.1f},{:.1f},{:.1f} skills={} kit={}",
					i, c.name, c.raceId.empty() ? "-" : c.raceId, c.portraitId,
					HexColor(c.portraitColor), c.moveSpeed, c.strength, c.dexterity,
					c.vitality, c.willpower, c.intelligence, c.baseHealth, c.baseStamina,
					c.baseMana, skills.empty() ? "-" : skills, kit.empty() ? "-" : kit));
			}
		});
}

} // namespace dungeon::game
