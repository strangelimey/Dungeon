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
#include "Game/Balance.h"  // ParseResists
#include "Game/PartyBar.h" // kSlots
#include "Game/SaveGame.h" // EntityState::threat

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <sstream>
#include <tuple>

namespace dungeon::game {

// THE FOUR-SLOT TABLES hold the biggest party the rules allow. A monster's
// per-member threat is copied straight to and from the save's (so one check
// covers both), and the party bar reserves a slot per member.
static_assert(std::tuple_size_v<decltype(SaveData::EntityState::threat)> >= party::kMaxMembers);
static_assert(PartyBar::kSlots >= party::kMaxMembers);

namespace {

// What races.cat says a race does to the numbers PartyRules owns.
party::RaceStats RaceStatsOf(const CatalogEntry& race) {
	party::RaceStats rs;
	for (size_t i = 0; i < party::kStats; ++i)
		rs.mods[i] = std::atoi(race.Get(kStats[i].id, "0").c_str());
	rs.extraPoints = std::atoi(race.Get("extra_points", "0").c_str());
	return rs;
}

std::string HexColor(const Vec4& c) {
	const auto b = [](float f) { return static_cast<int>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
	return std::format("{:02x}{:02x}{:02x}", b(c.x), b(c.y), b(c.z));
}

// What a race does, in words, for the page: its bonuses then its weaknesses,
// extra free points, a pace that differs, and what it resists
// ("+2 Dexterity, +1 Intelligence, -2 Vitality, -1 Strength, quick").
std::string RaceTraits(const CatalogEntry& race, const party::RaceStats& rs) {
	std::vector<std::string> parts;
	for (int sign : {+1, -1})
		for (size_t i = 0; i < party::kStats; ++i)
			if (rs.mods[i] * sign > 0)
				parts.push_back(std::format("{:+} {}", rs.mods[i],
											loc::Tr(std::format("attr.{}", kStats[i].id))));
	if (rs.extraPoints > 0) parts.push_back(loc::Format("party.race.points", rs.extraPoints));
	const float pace = race.GetFloat("pace", 1.0f);
	if (pace > 1.001f) parts.push_back(loc::Tr("party.race.quick"));
	if (pace < 0.999f) parts.push_back(loc::Tr("party.race.slow"));
	// `resists` is damage type + fraction pairs ("earth 0.25, bash 0.1").
	std::string words = race.Get("resists", "");
	std::ranges::replace(words, ',', ' ');
	std::istringstream in(words);
	std::string type;
	float amount = 0.0f;
	while (in >> type >> amount)
		parts.push_back(loc::Format("party.race.resists", loc::Tr("dmg." + type),
									static_cast<int>(amount * 100.0f + 0.5f)));
	std::string out;
	for (const std::string& p : parts) out += (out.empty() ? "" : ", ") + p;
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

// --- the creation page (phase 3) ------------------------------------------------

void Game::OpenPartyCreation(const std::string& folder) {
	// Another world than the resident one: switch first (a frame later, as every
	// switch is), and the switch opens the page when it lands.
	if (!folder.empty() && (!m_world || folder != m_project.FolderName())) {
		if (!SwitchWorld(folder)) {
			log::Warn("party creation: world '{}' is gone", folder);
			return;
		}
		if (m_pendingWorld) {
			m_pendingWorld->partyPage = true;
			return;
		}
	}
	// The title screen with no world resident: the default one, as a plain new
	// game would open (Game_Wiring's onStartNewGame).
	if (!m_world && !LoadWorld(m_defaultWorld)) {
		log::Warn("party creation: the default world '{}' could not be opened", m_defaultWorld);
		return;
	}
	m_ui.OpenPartyPage(PartyCreationDataFor());
	log::Info("party creation: the page opens for '{}' ({} races, {} skills, {} starting items)",
			  m_project.FolderName(), m_project.races.Entries().size(),
			  m_world->TrainableSkills().size(), m_project.startItems.size());
}

PartyCreationData Game::PartyCreationDataFor() {
	PartyCreationData d;
	if (!m_world) return d;
	for (const CatalogEntry& e : m_project.races.Entries()) {
		PartyRaceInfo r;
		r.id = e.id;
		r.name = loc::Tr(e.Get("name", "race." + e.id));
		r.portraitTag = e.Get("portrait", "");
		r.stats = RaceStatsOf(e);
		r.traits = RaceTraits(e, r.stats);
		d.races.push_back(std::move(r));
	}
	for (const std::string& id : m_world->TrainableSkills())
		d.skills.push_back({id, loc::Tr("skill." + id)});
	for (const std::string& id : m_project.startItems)
		if (const CatalogEntry* e = m_project.FindItem(id))
			d.items.push_back({id, loc::Tr(e->Get("name", "item." + id))});
	// The default four, as they come today: their own names and faces, in the
	// Settings palette's colours.
	const std::vector<Character> four = CreateDefaultParty();
	for (size_t i = 0; i < four.size() && i < party::kMaxMembers; ++i) {
		party::MemberSpec m;
		m.premade = static_cast<int>(i);
		m.name = four[i].name;
		m.portrait = four[i].portraitId;
		const Vec4 c = i < kMemberColorCount ? m_settings.memberColors[i] : four[i].portraitColor;
		m.color = {c.x, c.y, c.z, 1.0f};
		m.colorSet = true;
		d.defaults.push_back(std::move(m));
	}
	for (size_t i = 0; i < party::kMaxMembers; ++i)
		d.newColors[i] = i < kMemberColorCount ? m_settings.memberColors[i] : Vec4{0.5f, 0.5f, 0.5f, 1};
	// The preview is the member the game will start with: BuildMember, then the
	// world's pool rules (ResetRoster re-derives the maxima the same way).
	d.build = [this](const party::MemberSpec& spec, std::string& why) {
		std::optional<Character> c = BuildMember(spec, why);
		if (c && m_world) c->RecomputeMaxima(m_world->GetBalance().Resources());
		return c;
	};
	return d;
}

bool Game::StartWithParty(const std::vector<party::MemberSpec>& specs, std::string& why) {
	std::vector<Character> built;
	if (!BuildParty(specs, built, why)) {
		log::Warn("party creation: refused - {}", why);
		return false;
	}
	log::Info("party creation: a new game with a party of {}", built.size());
	m_startParty = std::move(built);
	m_ui.onStartNewGame();
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
			// On the title screen no world is resident yet; open the default one,
			// as Start New Game would (Game_Wiring's onStartNewGame), so the spec
			// is checked against the catalogs the game will play.
			if (!m_world && !LoadWorld(m_defaultWorld)) {
				m_console.Refuse(
					std::format("the default world '{}' could not be opened", m_defaultWorld));
				return;
			}
			if (!m_ui.onStartNewGame) {
				m_console.Refuse("the game is not wired yet");
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
				std::string why;
				if (!party::ApplySpecField(specs.back(), std::string_view(word).substr(0, eq),
										   std::string_view(word).substr(eq + 1), why)) {
					m_console.Refuse(why);
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

	RegisterPartyPageCommands();
}

// The creation PAGE'S twin: every verb is one of the page's own edits (the
// methods its widgets call), so a scripted build is the page's code path, not
// a second one. The page builds its tree a frame after it opens; the edits
// apply from the moment it is asked for.
void Game::RegisterPartyPageCommands() {
	m_console.Register(
		{.name = "partypage",
		 .group = CmdGroup::Characters,
		 .params = "\n"
				   "open [world]\n"
				   "add | default | back | start | picker\n"
				   "select <member> | remove <member>\n"
				   "set <key=value> ...\n"
				   "spend <stat> <points>\n"
				   "skill <slot> <id|none> | item <slot> <id|none>",
		 .summary = "drive the party creation page (bare = its status)"},
		[this](const std::vector<std::string>& args) {
			const std::string verb = args.empty() ? "status" : args[0];
			if (verb == "open") {
				if (m_state != AppState::Menu) {
					m_console.Refuse("the party page opens from the title screen");
					return;
				}
				OpenPartyCreation(args.size() > 1 ? args[1] : std::string());
				m_console.Print(m_ui.PartyPageActive() ? "party page: open"
													   : "party page: opening (switching world)");
				return;
			}
			PartyCreationPage* page = m_ui.PartyPage();
			if (!page || !m_ui.PartyPageActive()) {
				if (verb == "status") m_console.Print("party page: closed");
				else m_console.Refuse("the party page is not open (partypage open)");
				return;
			}
			const auto index = [&](size_t at, size_t& out) {
				if (args.size() <= at) return false;
				out = static_cast<size_t>(std::atoi(args[at].c_str()));
				return true;
			};
			bool ok = true;
			std::string why;
			size_t i = 0;
			if (verb == "status") {
				for (const std::string& line : page->StatusLines()) m_console.Print(line);
				return;
			} else if (verb == "add") {
				ok = page->Add();
				why = "a party has at most four members";
			} else if (verb == "default") {
				page->FillDefault();
			} else if (verb == "back") {
				m_ui.LeavePartyPage();
			} else if (verb == "start") {
				ok = m_ui.StartPartyPage(why);
			} else if (verb == "picker") {
				const int r = page->RaceIndex(page->SelectedIndex());
				m_ui.OpenPartyPortraitPicker(
					page->SelectedIndex(),
					r >= 0 ? page->Data().races[static_cast<size_t>(r)].portraitTag : "");
				ok = m_ui.PortraitPickerOpen();
				why = "the face picker did not open";
			} else if (verb == "select" && index(1, i)) {
				ok = i < page->Count();
				if (ok) page->Select(i);
				why = "no such member";
			} else if (verb == "remove" && index(1, i)) {
				ok = page->Remove(i);
				why = "no such member, or the last one";
			} else if (verb == "set" && args.size() > 1) {
				for (size_t a = 1; a < args.size() && ok; ++a) {
					const size_t eq = args[a].find('=');
					if (eq == std::string::npos) {
						ok = false;
						why = std::format("'{}' is not key=value", args[a]);
						break;
					}
					ok = page->SetField(std::string_view(args[a]).substr(0, eq),
										std::string_view(args[a]).substr(eq + 1), why);
				}
			} else if (verb == "spend" && args.size() > 2) {
				const int stat = StatIndex(args[1]);
				const int n = std::atoi(args[2].c_str());
				ok = stat >= 0;
				why = "no stat '" + args[1] + "'";
				for (int k = 0; ok && k < std::abs(n); ++k) {
					ok = page->Spend(static_cast<size_t>(stat), n > 0 ? +1 : -1);
					why = n > 0 ? "no points left (or a premade member)" : "nothing spent there";
				}
			} else if ((verb == "skill" || verb == "item") && args.size() > 2 && index(1, i)) {
				const bool skill = verb == "skill";
				const std::vector<PartyPick>& from = skill ? page->Data().skills : page->Data().items;
				int choice = -1;
				for (size_t k = 0; k < from.size(); ++k)
					if (from[k].id == args[2]) choice = static_cast<int>(k);
				ok = args[2] == "none" || choice >= 0;
				why = std::format("'{}' is not offered", args[2]);
				if (ok) page->SetPick(skill, i, choice);
			} else {
				m_console.RefuseUsage();
				return;
			}
			if (!ok) {
				m_console.Refuse(why);
				return;
			}
			m_console.Print(std::format("party page: {} ok", verb));
		});
}

} // namespace dungeon::game
