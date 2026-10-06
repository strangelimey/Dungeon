// ============================================================================
// Game/Game_DevParty.cpp — the party's dev-console commands.
//
// Split out of Game_DevCommands.cpp by concern: what a member knows, carries
// and does (learn/rune/give/guard/swing/wear/equip/effect/grudges/cast/castsvc), their
// pools and supplies (party/regen/supplies/rest/consume/setsupply), and the
// sheet, the party inventory window, a hand box's use menu, and the dev setters
// that re-derive it (sheet/inventory/handmenu/setstat/setskill/heal/char).
// ============================================================================
#include "Game/Game.h"

#include "Core/Log.h"
#include "Game/DevCommandArgs.h"
#include "Game/PartyHudDraw.h" // HeartRateTarget (hudbars)
#include "Game/Spell/Spell.h"    // castsvc: a spell's blast payload
#include "Game/SpellbookPanel.h" // book spell|cast|status

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string>

namespace dungeon::game {

using devargs::Need;
using devargs::ParseSymbolArg;

void Game::RegisterPartyCommands() {
	m_console.Register({.name = "learn",
						.group = CmdGroup::Characters,
						.params = "<member> <symbol>",
						.summary = "grant a spell symbol to a member"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 2)) return;
						   const size_t m = static_cast<size_t>(std::atoi(args[0].c_str()));
						   SpellSymbol sym;
						   if (m >= m_characters.size()) {
							   m_console.Refuse("no such member");
							   return;
						   }
						   if (!ParseSymbolArg(m_console, args[1], sym)) return;
						   m_characters[m].Learn(sym);
						   m_ui.RefreshSheet();
						   m_console.Print(std::format("{} learned {}", m_characters[m].name,
													   SymbolId(sym)));
					   });
	m_console.Register({.name = "rune",
						.group = CmdGroup::Characters,
						.params = "<symbol>",
						.summary = "give a rune tablet to the lead member's pack"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   SpellSymbol sym;
						   if (!ParseSymbolArg(m_console, args[0], sym)) return;
						   const std::string typeId(RuneItemId(sym));
						   if (m_characters.empty() ||
							   !m_characters[0].inventory.Stow(typeId))
							   m_console.Print("pack full (or no party)");
						   else
							   m_console.Print(std::format("pack += {}", typeId));
					   });
	m_console.Register({.name = "give",
						.group = CmdGroup::Characters,
						.params = "<item> [member]",
						.summary = "stow an item in a member's pack, firing its found hooks"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   const size_t m = args.size() > 1
							   ? static_cast<size_t>(std::atoi(args[1].c_str())) : 0;
						   if (m >= m_characters.size()) {
							   m_console.Refuse("no such member");
							   return;
						   }
						   if (!m_project.HasItem(args[0])) {
							   m_console.Print(std::format("no item '{}' in items/weapons/armor", args[0]));
							   return;
						   }
						   if (!m_characters[m].inventory.Stow(args[0])) {
							   m_console.Print("pack full");
							   return;
						   }
						   // AND ITS HOOKS FIRE, exactly as if it had been lifted
						   // off a floor. A dev command that hands you a quest
						   // item without moving the quest would be a trap laid
						   // in the one tool you would use to test quest content.
						   OnItemFound(args[0]);
						   m_console.Print(std::format("{} pack += {}",
													   m_characters[m].name, args[0]));
					   });
	// The party leader (ui-updates Phase 9): bare reports who leads and who
	// could; a member index picks them, as a click on their name does (and is
	// refused the same way when they are down).
	m_console.Register({.name = "leader",
						.group = CmdGroup::Characters,
						.params = "[member]",
						.summary = "report or pick the party leader"},
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty()) {
							   const int m = std::atoi(args[0].c_str());
							   if (m < 0 || m >= static_cast<int>(m_characters.size())) {
								   m_console.Refuse("no such member");
								   return;
							   }
							   if (!m_world->SetLeader(m)) {
								   m_console.Refuse(std::format("{} cannot lead (down)",
																m_characters[static_cast<size_t>(m)].name));
								   return;
							   }
						   }
						   const int lead = m_world->Leader();
						   std::string line = std::format(
							   "leader {} ({}){}", lead, m_world->LeaderName(),
							   m_world->LeaderMember() ? "" : " - nobody standing");
						   for (size_t i = 0; i < m_characters.size(); ++i)
							   line += std::format(" | {}:{}", i,
												   m_characters[i].IsAlive() ? "up" : "down");
						   m_console.Print(line);
					   });
	// Portraits by id (portraits phase 2): bare lists every member's portrait
	// with its tags; a member and an id sets it, as the picker will (refused
	// for an id portraits.cat does not list). `picker` drives the picker (phase
	// 3) for a script: open it for a member, filter it by tag words, scroll it,
	// and read back what it shows - and the SRV gauge, since a picker that leaked
	// thumbnails would show there first.
	m_console.Register({.name = "portrait",
						.group = CmdGroup::Characters,
						.params = "[member] [id]\npicker [member|off|status]\n"
								  "picker filter <race|any> <sex|any> <age|any>\n"
								  "picker scroll <0..1>",
						.summary = "report or set a member's portrait, or drive the picker"},
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty() && args[0] == "picker") {
							   PortraitPicker* picker = m_ui.Portraits();
							   if (!picker) {
								   m_console.Refuse("no picker");
								   return;
							   }
							   const std::string sub = args.size() >= 2 ? args[1] : "0";
							   if (sub == "off") {
								   m_ui.ClosePortraitPicker();
							   } else if (sub == "filter") {
								   if (args.size() < 5) {
									   m_console.RefuseUsage();
									   return;
								   }
								   // A word's 1-based place in its list; 0 for "any".
								   const auto pick = [](std::span<const char* const> words,
														const std::string& w) {
									   if (w == "any") return 0;
									   for (size_t i = 0; i < words.size(); ++i)
										   if (w == words[i]) return static_cast<int>(i) + 1;
									   return -1;
								   };
								   const int r = pick(PortraitPicker::Races(), args[2]);
								   const int s = pick(PortraitPicker::Sexes(), args[3]);
								   const int a = pick(PortraitPicker::Ages(), args[4]);
								   if (r < 0 || s < 0 || a < 0) {
									   m_console.Refuse("unknown tag word");
									   return;
								   }
								   picker->SetFilter(r, s, a);
							   } else if (sub == "scroll") {
								   if (args.size() < 3) {
									   m_console.RefuseUsage();
									   return;
								   }
								   picker->ScrollTo(static_cast<float>(std::atof(args[2].c_str())));
							   } else if (sub != "status") {
								   const int m = std::atoi(sub.c_str());
								   if (m < 0 || m >= static_cast<int>(m_characters.size())) {
									   m_console.Refuse("no such member");
									   return;
								   }
								   OpenPortraitPicker(static_cast<size_t>(m));
							   }
							   const PortraitPicker::Status st = picker->GetStatus();
							   m_console.Print(std::format(
								   "picker {} shown={} of {} filter={},{},{} visible={}+{} "
								   "thumbs={} srv={} peak={}",
								   st.open ? "open" : "closed", st.shown, st.total, st.race,
								   st.sex, st.age, st.firstVisible, st.visible, st.thumbs,
								   m_device.SrvLive(), m_device.SrvHighWater()));
							   return;
						   }
						   size_t first = 0, last = m_characters.size();
						   if (!args.empty()) {
							   const int m = std::atoi(args[0].c_str());
							   if (m < 0 || m >= static_cast<int>(m_characters.size())) {
								   m_console.Refuse("no such member");
								   return;
							   }
							   first = static_cast<size_t>(m);
							   last = first + 1;
							   if (args.size() >= 2 && !SetPortrait(first, args[1])) {
								   m_console.Refuse(std::format("{} is not in portraits.cat", args[1]));
								   return;
							   }
						   }
						   for (size_t i = first; i < last; ++i) {
							   const Character& c = m_characters[i];
							   const CatalogEntry* e = m_portraitCatalog.Find(c.portraitId);
							   m_console.Print(std::format(
								   "portrait {} {} {} [{} {} {} {}]{}", i, c.name, c.portraitId,
								   CatalogGet(e, "source", "?"), CatalogGet(e, "race", "?"),
								   CatalogGet(e, "sex", "?"), CatalogGet(e, "age", "?"),
								   c.portrait ? "" : " - NOT LOADED"));
						   }
					   });
	// Throwing (ui-updates Phase 10): the leader throws the item on the cursor,
	// or a given catalog item from nowhere, straight ahead - what a click above
	// the floor does, without having to aim one.
	m_console.Register({.name = "throw",
						.group = CmdGroup::Characters,
						.params = "[item]",
						.summary = "the leader throws the held item (or a given one) ahead"},
					   [this](const std::vector<std::string>& args) {
						   if (m_state != AppState::Playing) {
							   m_console.Refuse("only over the level");
							   return;
						   }
						   if (!m_world->LeaderMember()) {
							   m_console.Refuse("nobody is standing to throw");
							   return;
						   }
						   if (!args.empty()) {
							   if (!m_project.HasItem(args[0])) {
								   m_console.Refuse(std::format("no item '{}'", args[0]));
								   return;
							   }
							   if (m_world->ThrowItem(args[0]))
								   m_console.Print(std::format("{} thrown", args[0]));
							   else
								   m_console.Refuse("not thrown (the leader is not ready)");
							   return;
						   }
						   if (!m_heldItem) {
							   m_console.Refuse("nothing held - give an item id");
							   return;
						   }
						   const std::string item = *m_heldItem;
						   if (m_world->ThrowItem(item)) {
							   m_heldItem.reset();
							   m_console.Print(std::format("{} thrown", item));
						   } else {
							   m_console.Refuse("not thrown (the leader is not ready)");
						   }
					   });
	// The cursor drop, aimed: a given catalog item laid on a square's floor by a
	// click at its centre (DropItemOnSquare), so a script can ask whether a thing
	// may rest there (code-review C74). A drop the world turns down is an ANSWER,
	// not a broken line - printed, not refused - since asking about a square that
	// must turn it down is the point; a real click there would throw instead.
	m_console.Register({.name = "drop",
						.group = CmdGroup::Characters,
						.params = "<item> <x> <z>",
						.summary = "drop an item on a square's floor as a click at its centre would"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 3)) return;
						   if (m_state != AppState::Playing) {
							   m_console.Refuse("only over the level");
							   return;
						   }
						   if (!m_project.HasItem(args[0])) {
							   m_console.Refuse(std::format("no item '{}'", args[0]));
							   return;
						   }
						   const int x = std::atoi(args[1].c_str()), z = std::atoi(args[2].c_str());
						   const bool laid = m_world->DropItemOnSquare(args[0], x, z);
						   m_console.Print(std::format("drop {} at {},{}: {}", args[0], x, z,
													   laid ? "laid" : "refused"));
					   });
	// The offense/defense split before its slider exists
	// (docs/damage-system.md). Worth keeping once the UI lands: setting an
	// exact share is how the split gets MEASURED, where dragging a slider is
	// how it gets FELT, and those are different questions.
	m_console.Register({.name = "guard",
						.group = CmdGroup::Characters,
						.params = "<share> [member]",
						.summary = "set a member's offense share and print the stance weights"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   const float share =
							   static_cast<float>(std::atof(args[0].c_str()));
						   const size_t m = args.size() > 1
							   ? static_cast<size_t>(std::atoi(args[1].c_str())) : 0;
						   if (m >= m_characters.size()) {
							   m_console.Refuse("no such member");
							   return;
						   }
						   // Deliberately NO upper clamp, unlike the slider (which
						   // stops at exert_max): this is how a stance past what
						   // the UI allows gets tried at all.
						   if (share < 0.0f) {
							   m_console.Refuse("share cannot be negative");
							   return;
						   }
						   Character& c = m_characters[m];
						   c.offenseShare = share;
						   // The two multiples the stance puts on the skill term
						   // (defense::AttackWeight / GuardWeight - curved at both
						   // ends, so the share alone no longer says it). The guard
						   // is reported UNCLAMPED, because a negative one is the
						   // whole point past 1 - it becomes a penalty, and a
						   // readout that floored it at 0 would say an over-exerted
						   // stance and an all-out one were the same thing.
						   const defense::StanceRules rules =
							   m_world ? m_world->GetBalance().Stance()
									   : defense::StanceRules{};
						   m_console.Print(std::format(
							   "{} offense {:.2f}: attack x{:.2f}, guard x{:.2f} of "
							   "skill{}",
							   c.name, share, defense::AttackWeight(share, rules),
							   defense::GuardWeight(share, rules),
							   share > 1.0f ? " - OVER-EXERTED" : ""));
					   });

	// THE PARTY'S SWING, from the console. `equip` and `wear` exist because the
	// armor system was untestable without them; this is the same gap one step
	// further on — every attack-side rule (the stance, crit pierce, and now the
	// whole fumble consequence table) could only be reached by clicking a hand
	// slot in the HUD, which no script drives reliably. A verb of "" takes the
	// neutral attack, exactly as the hand menu's default does.
	m_console.Register({.name = "swing",
						.group = CmdGroup::Combat,
						.params = "<member> [hand] [verb]",
						.summary = "attack with a member's hand"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   const size_t m =
							   static_cast<size_t>(std::atoi(args[0].c_str()));
						   if (m >= m_characters.size()) {
							   m_console.Refuse("no such member");
							   return;
						   }
						   const size_t hand = args.size() > 1
							   ? static_cast<size_t>(std::atoi(args[1].c_str())) : 0;
						   const std::string verb = args.size() > 2 ? args[2] : "";
						   // Reports what the hand did rather than staying silent:
						   // false means the swing never happened at all (down,
						   // still on cooldown, rear rank without a polearm), which
						   // is a different thing from a swing that missed and
						   // otherwise looks identical from a script.
						   m_console.Print(m_world->PartyAttack(m, hand, verb)
											   ? "swung"
											   : "that hand cannot swing now");
					   });

	// `equip` reaches a HAND; this reaches the doll — the only place worn armor
	// counts (DungeonWorld::WornArmorClass). Without it there is no scriptable
	// way to put armor ON a character, which made the whole armor system
	// untestable except by dragging things in the sheet.
	m_console.Register({.name = "wear",
						.group = CmdGroup::Characters,
						.params = "<item> [member]\n"
								  "none [member]",
						.summary = "put an item in its worn doll slot, or strip the doll"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   const size_t m = args.size() > 1
							   ? static_cast<size_t>(std::atoi(args[1].c_str())) : 0;
						   if (m >= m_characters.size()) {
							   m_console.Refuse("no such member");
							   return;
						   }
						   Character& c = m_characters[m];
						   if (args[0] == "none") {
							   // Strip the worn doll, hands untouched — the
							   // quickest way back to a bare-skinned baseline
							   // for comparing against armored numbers.
							   for (int i = 0; i < kEquipCount; ++i) {
								   if (i == static_cast<int>(EquipSlot::LeftHand) ||
									   i == static_cast<int>(EquipSlot::RightHand))
									   continue;
								   c.inventory.equipment[static_cast<size_t>(i)].typeId.clear();
							   }
							   m_console.Print(std::format("{} is unarmored", c.name));
							   return;
						   }
						   if (!m_project.HasItem(args[0])) {
							   m_console.Print(std::format(
								   "no item '{}' in items/weapons/armor", args[0]));
							   return;
						   }
						   // The item says where it goes — the same rule the
						   // paper doll enforces, so the console cannot put
						   // something somewhere the UI would refuse.
						   const WearSlot w = m_itemCategories.WornAt(args[0]);
						   if (w == WearSlot::None) {
							   m_console.Print(std::format(
								   "'{}' has no `wear` slot — hold it instead (equip)",
								   args[0]));
							   return;
						   }
						   for (int i = 0; i < kEquipCount; ++i) {
							   const EquipSlot slot = static_cast<EquipSlot>(i);
							   if (!WearSlotFits(w, slot)) continue;
							   c.inventory.equipment[static_cast<size_t>(i)].Clear(); // no charge left over
							   c.inventory.equipment[static_cast<size_t>(i)].typeId = args[0];
							   m_console.Print(std::format("{} wears {} ({})", c.name,
														   args[0], WearSlotId(w)));
							   return;
						   }
					   });

	// `give` fills the pack; this puts a weapon straight in a hand, which is
	// what a combat test actually needs (no cursor drag, no HUD clicking).
	m_console.Register({.name = "equip",
						.group = CmdGroup::Characters,
						.params = "<item|none> [member] [hand]",
						.summary = "put an item in a member's hand (none empties it)"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   const size_t m = args.size() > 1
							   ? static_cast<size_t>(std::atoi(args[1].c_str())) : 0;
						   const int hand = args.size() > 2
							   ? std::clamp(std::atoi(args[2].c_str()), 0, 1) : 0;
						   if (m >= m_characters.size()) {
							   m_console.Refuse("no such member");
							   return;
						   }
						   if (args[0] == "none") {
							   m_characters[m].inventory.Hand(hand).Clear();
							   m_console.Print(std::format("{} {} hand emptied", m_characters[m].name,
														   hand == 0 ? "left" : "right"));
							   return;
						   }
						   if (!m_project.HasItem(args[0])) {
							   m_console.Print(std::format("no item '{}' in items/weapons/armor", args[0]));
							   return;
						   }
						   m_characters[m].inventory.Hand(hand).Clear(); // no charge left over
						   m_characters[m].inventory.Hand(hand).typeId = args[0];
						   m_console.Print(std::format("{} {} hand = {}",
													   m_characters[m].name,
													   hand == 0 ? "left" : "right",
													   args[0]));
					   });
	// A weapon's on-hit procs, replaced IN MEMORY (DungeonWorld::SetItemOnHit) -
	// nothing is written to weapons.cat. tools\AllocTest.ps1 -OnHitTypo swings a
	// club whose proc names no effect, to see the warning that follows each
	// blow excuse its own allocations inside a guarded frame (code-review C215).
	m_console.Register({.name = "onhit",
						.group = CmdGroup::Combat,
						.params = "<item> [<effect> <magnitude> <seconds> [chance], ...]",
						.summary = "replace an item's swing on_hit in memory (none given clears it)"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   std::string spec;
						   for (size_t i = 1; i < args.size(); ++i)
							   spec += (i > 1 ? " " : "") + args[i];
						   if (!m_world->SetItemOnHit(args[0], spec)) {
							   m_console.Refuse(std::format("no item '{}' in items/weapons/armor",
															 args[0]));
							   return;
						   }
						   m_console.Print(std::format("onhit {}: {}", args[0],
													   spec.empty() ? "(none)" : spec));
					   });
	// Land a status effect directly, skipping the cast. Setting a ward up in a
	// live fight is otherwise a coin toss — vocabulary, mana, and the fumble
	// roll all have to go your way, and then a monster has to choose to hit
	// the bearer before you see the ward DO anything.
	//
	// MAGNITUDE IS PER SECOND for a DoT, and the default pair (8 for 60s) is
	// therefore 480 damage against a 42 hp member. Spelled out in the params
	// because the argument order reads as "10 damage over 20 seconds" and means
	// almost the opposite: `effect bleed 0 10 20` deals 200 and annihilates the
	// party (docs/eval-audit.md).
	m_console.Register({.name = "effect",
						.group = CmdGroup::Combat,
						.params = "<id> [member|ahead] [magnitude per sec for a DoT] [seconds]",
						.summary = "apply a status effect to a member or the monster ahead"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   const float mag = args.size() > 2
							   ? std::strtof(args[2].c_str(), nullptr) : 8.0f;
						   const float secs = args.size() > 3
							   ? std::strtof(args[3].c_str(), nullptr) : 60.0f;
						   // "ahead" targets the monster the party is facing —
						   // the only way to put an effect ON a monster by hand,
						   // and the way to watch one tick without a weapon that
						   // procs it (docs/effects.md P3).
						   if (args.size() > 1 && args[1] == "ahead") {
							   if (m_world->ApplyEffectAhead(args[0], mag, secs))
								   m_console.Print(std::format(
									   "monster ahead gains {}", args[0]));
							   else
								   m_console.Refuse(
									   "no monster ahead (or no such effect)");
							   return;
						   }
						   const size_t m = args.size() > 1
							   ? static_cast<size_t>(std::atoi(args[1].c_str())) : 0;
						   if (m >= m_characters.size()) {
							   m_console.Refuse("no such member");
							   return;
						   }
						   const fx::EffectKind* kind = m_world->Effects().Find(args[0]);
						   if (!kind) {
							   m_console.Print(std::format("no effect '{}'", args[0]));
							   return;
						   }
						   const float magnitude = args.size() > 2
							   ? std::strtof(args[2].c_str(), nullptr) : 8.0f;
						   const float seconds = args.size() > 3
							   ? std::strtof(args[3].c_str(), nullptr) : 60.0f;
						   // The school picks a ward's flavour and every effect's
						   // tint, so derive it from the kind — `effect fireshield`
						   // must land the FIRE ward, not an oddly tinted one.
						   SpellSymbol school = SpellSymbol::Fire;
						   if (args[0] == "stoneskin" || args[0] == "poison")
							   school = SpellSymbol::Earth;
						   else if (args[0] == "windward") school = SpellSymbol::Air;
						   else if (args[0] == "waterveil") school = SpellSymbol::Water;
						   fx::Apply(m_characters[m].effects, *kind, school, magnitude,
									 seconds);
						   m_ui.RefreshSheet();
						   m_console.Print(std::format("{} gains {} ({} for {}s)",
													   m_characters[m].name, args[0],
													   magnitude, seconds));
					   });
	// Named `grudges`, not `threat`: another file registers a different
	// `threat` first, so this one was unreachable under that name.
	m_console.Register({.name = "grudges",
						.group = CmdGroup::Combat,
						.summary = "list per-member threat for every monster holding a grudge"},
					   [this](const std::vector<std::string>&) {
						   const std::vector<std::string> lines = m_world->ThreatReport();
						   if (lines.empty()) {
							   m_console.Print("no threat anywhere");
							   return;
						   }
						   for (const std::string& l : lines) m_console.Print("  " + l);
					   });
	m_console.Register({.name = "cast",
						.group = CmdGroup::Combat,
						.params = "<member> [hand] <symbol>...",
						.summary = "cast a spell by symbol sequence"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 2)) return;
						   const size_t m = static_cast<size_t>(std::atoi(args[0].c_str()));
						   if (m >= m_characters.size()) {
							   m_console.Refuse("no such member");
							   return;
						   }
						   // Optional hand (credits that hand's quick-cast MRU) —
						   // symbol names are never "0"/"1", so the token is
						   // unambiguous.
						   int hand = -1;
						   size_t first = 1;
						   if (args.size() > 2 && (args[1] == "0" || args[1] == "1")) {
							   hand = std::atoi(args[1].c_str());
							   first = 2;
						   }
						   std::vector<SpellSymbol> seq;
						   for (size_t i = first; i < args.size(); ++i) {
							   SpellSymbol s;
							   if (!ParseSymbolArg(m_console, args[i], s)) return;
							   seq.push_back(s);
						   }
						   const bool ok = m_world->CastSpell(m, seq, hand);
						   m_console.Print(ok ? "cast away" : "no cast (fizzle / no mana / unknown)");
					   });

	// The party's LIGHT (DungeonWorld_Light.cpp): every held item with a charge,
	// how much is left, and the wall torch the party faces - taken off its
	// bracket or mounted back with no click.
	m_console.Register({.name = "torch",
						.group = CmdGroup::Party,
						.params = "\nstatus\ntake\nmount [item [charge]]\ncharge <member> <hand> <seconds>",
						.summary = "the held torches' charge; take / mount the wall torch ahead"},
					   [this](const std::vector<std::string>& args) {
						   const std::string what = args.empty() ? "status" : args[0];
						   int x = 0, z = 0, wall = -1;
						   if (what == "take" || what == "mount") {
							   if (!m_world->FireAheadCell(x, z, wall) || wall < 0) {
								   m_console.Refuse("no wall torch ahead");
								   return;
							   }
							   bool ok = false;
							   if (what == "take") {
								   ok = m_world->TakeTorchAt(x, z, wall, m_heldItem);
							   } else if (args.size() >= 2) { // a named torch, from nowhere
								   const float charge = args.size() >= 3
									   ? static_cast<float>(std::atof(args[2].c_str()))
									   : kNoCharge;
								   ok = m_world->MountTorchAt(x, z, wall, args[1], charge);
							   } else if (m_heldItem && m_world->MountTorchAt(x, z, wall, *m_heldItem,
																			  m_heldItem.Charge())) {
								   m_heldItem.reset(); // the cursor's torch goes in
								   ok = true;
							   }
							   m_console.Print(std::format("torch {}: {}", what, ok ? "done" : "refused"));
							   return;
						   }
						   if (what == "charge" && args.size() >= 4) {
							   const size_t m = static_cast<size_t>(std::atoi(args[1].c_str()));
							   const int hand = std::atoi(args[2].c_str());
							   if (m >= m_characters.size() || hand < 0 || hand > 1) {
								   m_console.Refuse("no such member or hand");
								   return;
							   }
							   m_characters[m].inventory.Hand(hand).charge =
								   static_cast<float>(std::atof(args[3].c_str()));
							   m_console.Print("torch charge set");
							   return;
						   }
						   for (size_t m = 0; m < m_characters.size(); ++m)
							   for (int h = 0; h < 2; ++h) {
								   const ItemSlot& s = m_characters[m].inventory.Hand(h);
								   if (s.Empty()) continue;
								   m_console.Print(std::format("  [{}] {} hand {}: {} charge {:.1f}", m,
															   m_characters[m].name, h, s.typeId, s.charge));
							   }
						   if (m_heldItem)
							   m_console.Print(std::format("  cursor: {} charge {:.1f}", *m_heldItem,
														   m_heldItem.Charge()));
						   const FireAhead f = m_world->FireAheadOfParty();
						   // ...and WHICH torch is in it (the bracket remembers).
						   std::string_view inIt;
						   if (int fx = 0, fz = 0, fw = -1; m_world->FireAheadCell(fx, fz, fw) && fw >= 0)
							   inIt = m_world->SconceTorch(fx, fz, fw);
						   m_console.Print(std::format("  wall torch ahead: {}{}{}",
													   f.kind != FireAhead::Kind::WallTorch ? "none"
													   : f.empty                            ? "empty bracket"
													   : f.lit                              ? "burning"
																							: "out",
													   inIt.empty() ? "" : " ", inIt));
					   });

	// The cast services one at a time, with no spell in between: what a spell
	// would see ahead of the party and what each world hook does to it, so a
	// hand spell's outcome can be pinned on the spell or on the world.
	m_console.Register({.name = "castsvc",
						.group = CmdGroup::Combat,
						.params = "fire\nlight\ndouse\nflare\nfloor [x z]\ndrop <item>\nshove [cells]\nrepel <power> [member]\nblast <spell>\npuff [school]",
						.summary = "drive one cast service directly (the world ahead of the party)"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   const std::string& what = args[0];
						   if (what == "fire") {
							   const FireAhead f = m_world->FireAheadOfParty();
							   const char* kind = f.kind == FireAhead::Kind::WallTorch ? "walltorch"
												: f.kind == FireAhead::Kind::Brazier ? "brazier"
																					   : "none";
							   m_console.Print(std::format(
								   "castsvc fire: kind={} lit={} canburn={} haze={:.2f} flare={:.2f}", kind,
								   f.lit ? 1 : 0, f.canBurn ? 1 : 0, m_world->FireAheadHaze(),
								   m_world->FireAheadFlare()));
						   } else if (what == "floor" && args.size() >= 3) {
							   // Any square's, for where a drop or a throw came down.
							   const int x = std::atoi(args[1].c_str()), z = std::atoi(args[2].c_str());
							   const std::string ids = m_world->ItemIdsAt(x, z);
							   m_console.Print(std::format("castsvc floor {},{}: {}", x, z,
														   ids.empty() ? "(none)" : ids));
						   } else if (what == "floor") {
							   const Party& p = m_world->GetParty();
							   const std::string ids = m_world->ItemIdsAt(p.GridX(), p.GridZ());
							   m_console.Print(std::format("castsvc floor: {}", ids.empty() ? "(none)" : ids));
						   } else if (what == "light" || what == "douse") {
							   m_console.Print(std::format("castsvc {}: changed={}", what,
														   m_world->SetFireAhead(what == "light") ? 1 : 0));
						   } else if (what == "flare") {
							   m_console.Print(std::format("castsvc flare: flared={}",
														   m_world->FlareFireAhead() ? 1 : 0));
						   } else if (what == "drop" && args.size() >= 2) {
							   m_world->DropAtPartyFeet(args[1]);
							   m_console.Print(std::format("castsvc drop: {} at the party's feet", args[1]));
						   } else if (what == "shove") {
							   const int cells = args.size() >= 2 ? std::atoi(args[1].c_str()) : 1;
							   m_console.Print(std::format("castsvc shove: moved={}",
														   m_world->ShoveAhead(cells) ? 1 : 0));
						   } else if (what == "repel" && args.size() >= 2) {
							   const float power = static_cast<float>(std::atof(args[1].c_str()));
							   const int member = args.size() >= 3 ? std::atoi(args[2].c_str()) : 0;
							   const auto r = m_world->RepelAhead(power, member);
							   m_console.Print(std::format("castsvc repel: weakened={} turned={}",
														   r.weakened, r.turned));
						   } else if (what == "blast" && args.size() >= 2) {
							   const Spell* spell = m_world->FindSpell(args[1]);
							   if (!spell || !spell->Blast().Any()) {
								   m_console.Refuse("no such spell, or it does not blast");
								   return;
							   }
							   m_world->BlastAroundParty(spell->MakePayload(), spell->School(), 0);
							   m_console.Print(std::format("castsvc blast: {} round the party", args[1]));
						   } else if (what == "puff") {
							   // From the eye down the cell's centre line (a real cast
							   // offsets it into the caster's lane).
							   SpellSymbol school = SpellSymbol::Fire;
							   if (args.size() >= 2 && !ParseSymbolArg(m_console, args[1], school)) return;
							   const Party& p = m_world->GetParty();
							   const Direction f = static_cast<Direction>(p.Facing());
							   m_world->HandPuff(school, p.EyePosition(),
												 {static_cast<float>(DirDX(f)), 0.0f,
												  static_cast<float>(DirDZ(f))});
							   m_console.Print(std::format("castsvc puff: {} ahead", SymbolId(school)));
						   } else {
							   m_console.RefuseUsage();
						   }
					   });

	// The party's side of an encounter, in one machine-readable block. `monsters`
	// has printed the other side for a while; without this a harness can watch a
	// fight and never learn what it COST, which is most of what a balance pass
	// is trying to find out.
	m_console.Register({.name = "party",
						.group = CmdGroup::Characters,
						.summary = "each member's hp/stamina/mana and stance"},
					   [this](const std::vector<std::string>&) {
						   for (size_t i = 0; i < m_characters.size(); ++i) {
							   const Character& c = m_characters[i];
							   m_console.Print(std::format(
								   "  [{}] {:<6} hp {:.1f}/{:.1f}  st {:.1f}/{:.1f}  "
								   "mp {:.1f}/{:.1f}  share {:.2f}{}{}",
								   i, c.name, c.health, c.maxHealth, c.stamina,
								   c.maxStamina, c.mana, c.maxMana, c.offenseShare,
								   c.dead ? "  DEAD" : (c.IsAlive() ? "" : "  DOWN"),
								   c.exhausted ? "  EXHAUSTED" : ""));
							   // What is on them: id, raw magnitude, seconds left - so
							   // a cure (potions.eval) can be seen to halve or lift one.
							   if (c.effects.empty()) continue;
							   std::string line = "      effects:";
							   for (const fx::Inst& e : c.effects)
								   line += std::format(" {} {:.2f}/{:.0f}s", e.Id(), e.magnitude,
													   e.timeLeft);
							   m_console.Print(line);
						   }
					   });

	// The three regeneration RATES, which nothing else can show: a bar's VALUE
	// is visible and its SLOPE is not, so the ordering the model asks for —
	// stamina/sec > mana/sec > health/sec at equal investment
	// (docs/health-and-healing.md) — was a claim no measurement could reach.
	//
	// Rates are printed AT FULL FLOW, before the state gate, because that is
	// what the knobs describe; the live gate is named at the end of each line so
	// a reading taken mid-swing is not mistaken for the tuning.
	//
	// THE ORDERING IS CHECKED ON A REFERENCE ROW, NOT PER MEMBER, and getting
	// that wrong the first time is worth recording: a per-member verdict called
	// Brand BROKEN, and Brand is right — he is a brute with INT 8, so his mana
	// crawls and ought to. The claim is about the KNOBS at EQUAL investment, and
	// a party of four deliberately unequal characters can never test it. So the
	// reference member has every aptitude at the stat curve's baseline (worth
	// exactly nothing, by construction) and one practice level shared by all
	// three pools — measured UNTRAINED and TRAINED, since a crossing can hide at
	// either end. Reporting whether an authored property holds is measurement;
	// what to do about it is Michael's.
	m_console.Register({.name = "regen",
						.group = CmdGroup::Characters,
						.summary = "health/stamina/mana regen per second, and the ordering"},
					   [this](const std::vector<std::string>&) {
						   const Balance& bal = m_world->GetBalance();
						   const resource::PoolRules pools = bal.Resources();
						   const CurveRules statCurve = bal.StatCurve();
						   const CurveRules paceCurve = bal.PaceCurve();
						   for (size_t i = 0; i < m_characters.size(); ++i) {
							   const Character& c = m_characters[i];
							   const auto rate = [&](resource::Kind k) {
								   return c.RegenPerSec(k, pools.For(k), statCurve);
							   };
							   m_console.Print(std::format(
								   "  [{}] {:<6} health {:.3f}/s  stamina {:.3f}/s  "
								   "mana {:.3f}/s  pace {:.2f}  {}",
								   i, c.name, rate(resource::Kind::Health),
								   rate(resource::Kind::Stamina),
								   rate(resource::Kind::Mana),
								   c.MoveSpeed(paceCurve),
								   c.staminaHoldoff > 0.0f ? "exerting" : "idle"));
						   }
						   // The PARTY's pace is the slowest member's, so it is
						   // its own line: the interesting case is a member
						   // training hard and the number not moving at all.
						   m_console.Print(std::format(
							   "  party pace {:.2f} (the slowest member's)",
							   m_world->GetParty().Speed()));
						   if (const float soothe = m_world->StaminaRegenScale(); soothe > 1.0f)
							   m_console.Print(std::format(
								   "  stamina x{:.2f} in a Tidelight (while not exerting)", soothe));
						   // The reference rows. Each pool is sized at the same
						   // investment it is being rated at, so the per-max term
						   // is honest rather than borrowed from someone else's
						   // body: max = aptitude + practice, with no authored base.
						   const float apt = statCurve.baseline;
						   for (const float lvl : {0.0f, 10.0f}) {
							   const auto rate = [&](resource::Kind k) {
								   const resource::Rules& r = pools.For(k);
								   return resource::RegenPerSec(
									   r, statCurve, apt,
									   resource::Maximum(r, 0.0f, apt, lvl), lvl);
							   };
							   const float hp = rate(resource::Kind::Health);
							   const float st = rate(resource::Kind::Stamina);
							   const float mp = rate(resource::Kind::Mana);
							   m_console.Print(std::format(
								   "  ref  practice {:<2.0f} health {:.3f}/s  "
								   "stamina {:.3f}/s  mana {:.3f}/s  order {}",
								   lvl, hp, st, mp,
								   st > mp && mp > hp ? "ok" : "BROKEN"));
						   }
					   });

	// Food and water, and how long they have left. The REMAINING TIME is the
	// point of the readout: a meter at 62 means nothing on its own, because the
	// drain rate depends on the member's conditioning — the fitter member burns
	// more, which is the brake the whole design rests on. Printed in hours,
	// because a supply run is a question about hours and not about seconds.
	m_console.Register({.name = "supplies",
						.group = CmdGroup::Characters,
						.summary = "each member's food and water, and hours left"},
					   [this](const std::vector<std::string>&) {
						   const Balance& bal = m_world->GetBalance();
						   const resource::SupplyRules food =
							   bal.SupplyOf(resource::Supply::Food);
						   const resource::SupplyRules water =
							   bal.SupplyOf(resource::Supply::Water);
						   for (size_t i = 0; i < m_characters.size(); ++i) {
							   const Character& c = m_characters[i];
							   const float cond =
								   c.PracticeLevel(resource::Kind::Stamina);
							   const auto hours = [&](const resource::SupplyRules& r,
													  float level) {
								   const float rate = resource::DrainPerSec(r, cond);
								   return rate > 0.0f ? level / rate / 3600.0f : 0.0f;
							   };
							   m_console.Print(std::format(
								   "  [{}] {:<6} food {:5.1f}/{:.0f} ({:4.1f}h)  "
								   "water {:5.1f}/{:.0f} ({:4.1f}h)  cond {:.0f}{}{}",
								   i, c.name, c.food, food.max, hours(food, c.food),
								   c.water, water.max, hours(water, c.water), cond,
								   c.FindEffect("starving") ? "  STARVING" : "",
								   c.FindEffect("parched") ? "  PARCHED" : ""));
						   }
					   });

	// The item details dialog on any item type, without a right-click - so a
	// script (and `uioverlap`) can reach it. Through ShowItemDetails, the one
	// opener every right-click uses.
	m_console.Register({.name = "itemdetails",
						.group = CmdGroup::Characters,
						.params = "<item> [kg]\n"
								  "pack <member> <slot>\n"
								  "memorize\n"
								  "off\n"
								  "status",
						.summary = "open, close or report the item details dialog"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   if (args[0] == "off") {
							   m_ui.CloseItemDetails();
							   m_console.Print("item details closed");
							   return;
						   }
						   if (args[0] == "status") {
							   const ItemDetailsDialog* dlg = m_ui.DetailsDialog();
							   m_console.Print(std::format(
								   "item details: {} ({} preview submeshes) opens={} memorize={}",
								   m_ui.ItemDetailsOpen() ? "open" : "closed",
								   dlg ? dlg->PreviewSubs().size() : 0,
								   dlg ? dlg->OpenCount() : 0u,
								   dlg && dlg->MemorizeShown() ? 1 : 0));
							   return;
						   }
						   if (args[0] == "memorize") {
							   m_console.Print(m_ui.PressDetailsMemorize()
												   ? "item details: memorized"
												   : "item details: no Memorize button up");
							   return;
						   }
						   if (m_state != AppState::Playing &&
							   m_state != AppState::CharacterSheet) {
							   m_console.Refuse("only over the level or the sheet");
							   return;
						   }
						   if (args[0] == "pack") {
							   if (!Need(m_console, args, 3)) return;
							   const size_t m = static_cast<size_t>(std::atoi(args[1].c_str()));
							   m_ui.OpenPackItemDetails(m, std::atoi(args[2].c_str()));
							   if (!m_ui.ItemDetailsOpen())
								   m_console.Refuse("nothing in that slot");
							   else
								   m_console.Print(std::format("item details: member {} pack slot {}",
															   args[1], args[2]));
							   return;
						   }
						   const float kg =
							   args.size() > 1 ? static_cast<float>(std::atof(args[1].c_str()))
											   : 0.0f;
						   m_ui.ShowItemDetails(args[0], kg);
						   if (!m_ui.ItemDetailsOpen())
							   m_console.Refuse("no such item");
						   else
							   m_console.Print(std::format("item details: {}", args[0]));
					   });

	// Open (or close) the character sheet. It exists because the sheet was
	// reachable ONLY by clicking a portrait, which is why `/check-ingame`
	// reports it as a screen it cannot sweep — so the one screen with the most
	// hand-laid-out content in the game was also the one screen `uioverlap`
	// never saw. `sheet <n>` then `uioverlap` closes half of that gap.
	// Shows the chrome in one UI material without touching the setting - the
	// Level dialog's preview, from the console - so every material can be
	// looked at in turn (more-ui-updates: the contrast pass). `off` hands the
	// chrome back to the setting and the place.
	m_console.Register({.name = "uimaterial",
						.group = CmdGroup::Settings,
						.params = "<material>\n"
								  "off\n"
								  "list",
						.summary = "preview a UI material (nothing saved)"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   if (args[0] == "off") {
							   m_ui.EndStonePreview();
							   m_console.Print(std::format("uimaterial: back to {}", m_ui.ShownStone()));
							   return;
						   }
						   const std::vector<std::string> order = m_ui.StoneOrder();
						   if (args[0] == "list") {
							   std::string line = "uimaterial:";
							   for (const std::string& s : order) line += " " + s;
							   m_console.Print(line);
							   return;
						   }
						   if (std::ranges::find(order, args[0]) == order.end()) {
							   m_console.Refuse("no such material - `uimaterial list`");
							   return;
						   }
						   m_ui.PreviewStone(args[0]);
						   m_console.Print(std::format("uimaterial: showing {}", args[0]));
					   });

	// The framed resource bars (docs/icon-updates-plan.md): report each member's
	// heartbeat, sweep every bar so the dimming and the leading edge can be
	// judged without a fight, or pin the heart rate to judge the beat.
	m_console.Register({.name = "hudbars",
						.group = CmdGroup::Characters,
						.params = "[status]\n"
								  "demo on|off\n"
								  "rate <bpm|auto>",
						.summary = "report, sweep or pin the party bar's resource bars"},
					   [this](const std::vector<std::string>& args) {
						   ResourceBarStyle& style = m_ui.BarStyle();
						   if (!args.empty() && args[0] == "demo") {
							   if (!Need(m_console, args, 2)) return;
							   style.demo = args[1] == "on";
							   m_console.Print(std::format("hudbars demo {}",
														   style.demo ? "on" : "off"));
							   return;
						   }
						   if (!args.empty() && args[0] == "rate") {
							   if (!Need(m_console, args, 2)) return;
							   style.pinnedBpm =
								   args[1] == "auto"
									   ? -1.0f
									   : std::max(0.0f, static_cast<float>(
															std::atof(args[1].c_str())));
							   m_console.Print(style.pinnedBpm < 0.0f
												   ? std::string("hudbars rate auto")
												   : std::format("hudbars rate {:.0f} bpm",
																 style.pinnedBpm));
							   return;
						   }
						   if (!args.empty() && args[0] != "status") {
							   m_console.RefuseUsage();
							   return;
						   }
						   const bool noticed = m_world && m_world->PartyNoticed();
						   m_console.Print(std::format(
							   "hudbars: {} | noticed {} | demo {} | rate {}",
							   !style.frame     ? "no frame texture (flat)"
							   : !style.framed ? "flat (uiskin off)"
											   : "framed",
							   noticed ? "yes" : "no", style.demo ? "on" : "off",
							   style.pinnedBpm < 0.0f ? std::string("auto")
													  : std::format("{:.0f}", style.pinnedBpm)));
						   for (size_t i = 0; i < m_characters.size(); ++i) {
							   const Character& c = m_characters[i];
							   auto pct = [](float v, float m) {
								   return m > 0.0f ? 100.0f * v / m : 0.0f;
							   };
							   m_console.Print(std::format(
								   "  {} {:<6} hp {:3.0f}% st {:3.0f}% mp {:3.0f}% | "
								   "heart {:5.1f} bpm -> {:3.0f}{}",
								   i, c.name, pct(c.health, c.maxHealth),
								   pct(c.stamina, c.maxStamina), pct(c.mana, c.maxMana),
								   style.PulseOf(i).bpm, HeartRateTarget(c, noticed),
								   c.IsAlive() ? "" : " (down)"));
						   }
					   });

	m_console.Register({.name = "sheet",
						.group = CmdGroup::Characters,
						.params = "[member]\n"
								  "off\n"
								  "status",
						.summary = "open, close or report the character sheet"},
					   [this](const std::vector<std::string>& args) {
						   // What the sheet shows - for a harness driving it with
						   // keys (the strafe keys page members, Tab the tabs).
						   if (!args.empty() && args[0] == "status") {
							   static constexpr const char* kModes[] = {
								   "inventory", "stats", "skills", "spells", "effects"};
							   const size_t m = m_ui.SheetIndex();
							   m_console.Print(std::format(
								   "sheet: {} member {} ({}) tab {}",
								   m_state == AppState::CharacterSheet ? "open" : "closed",
								   m, m < m_characters.size() ? m_characters[m].name : "?",
								   kModes[static_cast<int>(m_ui.SheetMode())]));
							   // The status bar, so a script that parks the pointer
							   // on something can read what it says.
							   const std::string_view name = m_ui.SheetStatusName();
							   m_console.Print(
								   name.empty()
									   ? std::string("sheet bar: (empty)")
									   : std::format("sheet bar: {} | {}", name,
													 m_ui.SheetStatusText()));
							   // The shown member's pack row ('-' = no bag), which
							   // one is selected, how many slots it has, and the
							   // equips counted so far.
							   if (m < m_characters.size()) {
								   const Inventory& inv = m_characters[m].inventory;
								   std::string row;
								   for (const Pack& p : inv.packs)
									   row += " " + (p.Empty() ? std::string("-") : p.typeId);
								   m_console.Print(std::format(
									   "sheet packs:{} selected={} slots={} equips={}", row,
									   inv.selectedPack, inv.SelectedContents().size(),
									   m_ui.SheetPackEquips()));
							   }
							   // Where the "All" button is (AllocTest -All clicks it).
							   const gfx::Rect all = m_ui.SheetAllRect();
							   m_console.Print(std::format("sheet all: {},{}",
														   static_cast<int>(all.x + all.w * 0.5f),
														   static_cast<int>(all.y + all.h * 0.5f)));
							   return;
						   }
						   if (!args.empty() && args[0] == "off") {
							   // The close box's own path (GameUI's onResume), so this
							   // closes the sheet exactly as a player does - back to
							   // the level OR the world map it was opened from. It
							   // used to set Playing outright, which could not see the
							   // bug where paging with < > broke the close box.
							   if (m_state == AppState::CharacterSheet && m_ui.onResume)
								   m_ui.onResume();
							   m_console.Print("sheet closed");
							   return;
						   }
						   const size_t m =
							   args.empty()
								   ? 0
								   : static_cast<size_t>(std::atoi(args[0].c_str()));
						   if (m >= m_characters.size()) {
							   m_console.Refuse("no such member");
							   return;
						   }
						   // Through the same entry point the portrait click uses,
						   // so this cannot drift from what a player sees.
						   OpenCharacterSheet(m);
						   m_console.Print(
							   std::format("sheet open: {}", m_characters[m].name));
					   });

	// The floating HUD panels (ui-panels P3a, UI/FloatingPanel.h). A harness
	// moves and scales them through here rather than scripting drags at a
	// layout that moves under it; bare (or `list`) prints where each one is.
	m_console.Register(
		{.name = "hudpanel",
		 .group = CmdGroup::Settings,
		 .params = "\n"
				   "list\n"
				   "<id> <x> <y> [scale]\n"
				   "hide|show <id>\n"
				   "reset\n"
				   "lock on|off\n"
				   "layout standard|minimal",
		 .summary = "list, place, minimize, reset or lock the floating HUD panels"},
		[this](const std::vector<std::string>& args) {
			if (args.empty() || args[0] == "list") {
				// The arranging buttons' side, so a harness can aim at the
				// top-right pair (minimize in the corner, reset beside it), and
				// the clicks that minimized or restored a panel since launch.
				const ui::FloatingPanel* move = m_ui.HudPanel(kHudMove);
				m_console.Print(std::format(
					"hud layout {}, {}, grip {:.0f}px, minimizes {}, restores {}",
					m_settings.hudLayout == 1 ? "minimal" : "standard",
					m_settings.hudLocked ? "locked" : "unlocked", move ? move->GripSide() : 0.0f,
					m_ui.PanelMinimizes(), m_ui.PanelRestores()));
				for (size_t i = 0; i < std::size(kHudPanelFields); ++i) {
					const HudPanelLook& look = m_settings.*(kHudPanelFields[i].look);
					const ui::FloatingPanel* panel = m_ui.HudPanel(i);
					const gfx::Rect r = panel ? panel->Pixel() : gfx::Rect{};
					m_console.Print(std::format(
						"  {:<8} {}  px {:.0f},{:.0f} {:.0f}x{:.0f}  saved {}  scale {:.2f}  opacity {:.2f}{}",
						kHudPanelFields[i].id,
						!panel ? "unbuilt" : (panel->visible ? "shown " : "hidden"), r.x, r.y,
						r.w, r.h,
						look.x < 0.0f ? std::string("default")
									  : std::format("{:.3f},{:.3f}", look.x, look.y),
						look.scale, look.opacity, look.hidden ? "  minimized" : ""));
				}
				return;
			}
			if (args[0] == "hide" || args[0] == "show") {
				const bool hide = args[0] == "hide";
				for (const HudPanelField& field : kHudPanelFields) {
					if (args.size() < 2 || args[1] != field.id) continue;
					if (!field.glyph) {
						m_console.Refuse(std::format("{} does not minimize", field.id));
						return;
					}
					(m_settings.*(field.look)).hidden = hide;
					m_settings.Save();
					m_console.Print(std::format("{} {}", field.id, hide ? "minimized" : "restored"));
					return;
				}
				m_console.Refuse("usage: hudpanel hide|show <id> - party, status, move, hands, magic, cards");
				return;
			}
			if (args[0] == "reset") {
				m_ui.ResetHudLayout();
				m_console.Print("hud layout reset");
				return;
			}
			if (args[0] == "layout") {
				if (args.size() < 2 || (args[1] != "standard" && args[1] != "minimal")) {
					m_console.Refuse("usage: hudpanel layout standard|minimal");
					return;
				}
				m_ui.SetHudLayout(args[1] == "minimal" ? 1 : 0);
				m_console.Print(std::format("hud layout {}", args[1]));
				return;
			}
			if (args[0] == "lock") {
				m_settings.hudLocked = args.size() < 2 || args[1] != "off";
				m_settings.Save();
				m_console.Print(m_settings.hudLocked ? "hud layout locked" : "hud layout unlocked");
				return;
			}
			for (const HudPanelField& field : kHudPanelFields) {
				if (args[0] != field.id) continue;
				if (args.size() < 3) {
					m_console.Refuse("usage: hudpanel <id> <x> <y> [scale] (window fractions)");
					return;
				}
				HudPanelLook& look = m_settings.*(field.look);
				look.x = std::clamp(static_cast<float>(std::atof(args[1].c_str())), 0.0f, 1.0f);
				look.y = std::clamp(static_cast<float>(std::atof(args[2].c_str())), 0.0f, 1.0f);
				if (args.size() > 3)
					look.scale = std::clamp(static_cast<float>(std::atof(args[3].c_str())), 0.5f, 1.5f);
				m_settings.Save();
				m_console.Print(std::format("{} at {:.3f},{:.3f} scale {:.2f}", field.id, look.x,
											look.y, look.scale));
				return;
			}
			m_console.Refuse("no such panel - party, status, options, move, hands, magic, cards, inventory, tray, sheet");
		});

	// The party window (a card per member, on one tab - Game/PartyWindow.h),
	// otherwise reachable only through the sheet's "All" button. It exists for
	// tools\AllocTest.ps1 -Items, which picks an item out of a pack in this
	// window inside a guarded frame; `status` is how it checks where the item
	// went, since a missed click and a clean move look alike to the guard, and
	// `slot` is where it aims (the window lays itself out in em, so a harness
	// cannot work the slots out from window fractions).
	m_console.Register({.name = "inventory",
						.group = CmdGroup::Characters,
						.params = "[inventory|stats|skills|spells|effects]\n"
								  "off\n"
								  "status\n"
								  "slot <member> <slot>\n"
								  "stone <tab>",
						.summary = "open, close or report the party window"},
					   [this](const std::vector<std::string>& args) {
						   static constexpr const char* kTabs[] = {
							   "inventory", "stats", "skills", "spells", "effects"};
						   if (!args.empty() && args[0] == "status") {
							   std::string line = std::format(
								   "inventory: {} tab {} held={} opens={}",
								   m_ui.InventoryOpen() ? "open" : "closed",
								   kTabs[static_cast<int>(m_ui.InventoryMode())],
								   m_heldItem ? *m_heldItem : std::string("none"),
								   m_ui.InventoryOpens());
							   for (size_t m = 0; m < m_characters.size(); ++m) {
								   line += std::format(" | {}:", m);
								   for (const ItemSlot& s :
										m_characters[m].inventory.SelectedContents())
									   line += " " + (s.Empty() ? std::string("-") : s.typeId);
							   }
							   m_console.Print(line);
							   // Its status line, so a script that parks the pointer on
							   // something can read what it says.
							   const std::string_view name = m_ui.InventoryStatusName();
							   m_console.Print(name.empty()
												   ? std::string("inventory bar: (empty)")
												   : std::format("inventory bar: {} | {}", name,
																 m_ui.InventoryStatusText()));
							   return;
						   }
						   if (!args.empty() && args[0] == "slot") {
							   if (!Need(m_console, args, 3)) return;
							   const size_t m = static_cast<size_t>(std::atoi(args[1].c_str()));
							   const int i = std::atoi(args[2].c_str());
							   gfx::Rect r;
							   if (!m_ui.InventoryOpen() || !m_ui.InventorySlotRect(m, i, r)) {
								   m_console.Refuse("the window is closed, or no such member");
								   return;
							   }
							   m_console.Print(std::format("inventory slot {} {}: {},{} {}x{}", m, i,
														   static_cast<int>(r.x + r.w * 0.5f),
														   static_cast<int>(r.y + r.h * 0.5f),
														   static_cast<int>(r.w),
														   static_cast<int>(r.h)));
							   return;
						   }
						   if (!args.empty() && args[0] == "stone") {
							   if (!Need(m_console, args, 2)) return;
							   const gfx::Rect r = m_ui.InventoryStoneRect(
								   static_cast<size_t>(std::atoi(args[1].c_str())));
							   if (!m_ui.InventoryOpen() || r.w <= 0.0f) {
								   m_console.Refuse("the window is closed, or no such stone");
								   return;
							   }
							   m_console.Print(std::format("inventory stone {}: {},{}", args[1],
														   static_cast<int>(r.x + r.w * 0.5f),
														   static_cast<int>(r.y + r.h * 0.5f)));
							   return;
						   }
						   if (!args.empty() && args[0] == "off") {
							   m_ui.CloseInventory();
							   m_console.Print("inventory closed");
							   return;
						   }
						   int tab = 0;
						   if (!args.empty()) {
							   tab = -1;
							   for (int t = 0; t < 5; ++t)
								   if (args[0] == kTabs[t]) tab = t;
							   if (tab < 0) {
								   m_console.RefuseUsage();
								   return;
							   }
						   }
						   if (m_state != AppState::Playing) {
							   m_console.Refuse("only over the level");
							   return;
						   }
						   m_ui.OpenInventory(static_cast<CharacterSheet::Mode>(tab));
						   m_console.Print(std::format("inventory open on {}", kTabs[tab]));
					   });

	// Open (or close) a member's spellbook in the Magic area - the selector
	// button's own path. It exists for tools\AllocTest.ps1 -Cast: an open book
	// redraws its rune grid every frame, and that grid allocated per frame for
	// as long as no harness could reach it.
	m_console.Register({.name = "book",
						.group = CmdGroup::Characters,
						.params = "[member]\n"
								  "off\n"
								  "spell <symbol>...\n"
								  "cast\n"
								  "status",
						.summary = "open or close a member's spellbook, build and cast in it"},
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty() && args[0] == "off") {
							   m_ui.CloseSpellbook();
							   m_console.Print("book closed");
							   return;
						   }
						   // The open book's slate, worked as its rune grid and Cast
						   // button work it (SpellbookPanel::SetSequence / PressCast).
						   SpellbookPanel* book = m_ui.Spellbook();
						   if (!args.empty() && (args[0] == "spell" || args[0] == "cast" ||
												 args[0] == "status")) {
							   if (!book || !book->IsOpen()) {
								   m_console.Refuse("no book is open (`book <member>` first)");
								   return;
							   }
							   if (args[0] == "spell") {
								   std::array<SpellSymbol, kMaxRecipe> seq{};
								   size_t n = 0;
								   for (size_t i = 1; i < args.size(); ++i) {
									   if (n >= seq.size()) break;
									   if (!ParseSymbolArg(m_console, args[i], seq[n])) return;
									   ++n;
								   }
								   if (!book->SetSequence({seq.data(), n})) {
									   m_console.Refuse("not laid: an unknown rune, or too many");
									   return;
								   }
							   } else if (args[0] == "cast") {
								   book->PressCast();
							   }
							   m_console.Print(std::format("book slate: {} rune(s)", book->SequenceLength()));
							   return;
						   }
						   const size_t m =
							   args.empty()
								   ? 0
								   : static_cast<size_t>(std::atoi(args[0].c_str()));
						   if (m >= m_characters.size()) {
							   m_console.Refuse("no such member");
							   return;
						   }
						   if (!m_ui.OpenSpellbook(m)) {
							   m_console.Refuse(std::format(
								   "{} has no book to open (down, or knows no symbols)",
								   m_characters[m].name));
							   return;
						   }
						   m_console.Print(std::format("book open: {}", m_characters[m].name));
					   });

	// A HUD hand box's use menu, opened as a right-click on the box opens it
	// (code-review C382). It exists for tools\InGameTest.ps1's hand menu sweep:
	// the bare-hand menu's Combat / Magic group rows are what a sub-18 px rem cut,
	// and a scripted click on the HUD is what a sweep must not do. Spells named
	// after the hand are credited to its quick-cast list first, as a cast from
	// that hand credits them - the Magic group without a cast, whose fumble roll
	// would make the sweep a coin toss. Every report also goes to dungeon.log,
	// where the sweep's verdict reads it. The box reads 0x0 until the HUD has
	// laid the menu out, which it does only while the console and the map are
	// SHUT.
	m_console.Register({.name = "handmenu",
						.group = CmdGroup::Characters,
						.params = "<member> <hand> [spell]...\n"
								  "group <n>\n"
								  "status\n"
								  "off",
						.summary = "open a HUD hand box's use menu, as a right-click on it does"},
					   [this](const std::vector<std::string>& args) {
						   ui::ContextMenu* menu = m_ui.HandMenu();
						   const auto report = [&] {
							   const ui::UIContext* hud = m_ui.UiTree("hud");
							   const float rem = hud ? hud->GetFont().Height() : 0.0f;
							   std::string line;
							   if (menu && menu->IsOpen()) {
								   const gfx::Rect box = menu->InkRect();
								   line = std::format(
									   "handmenu: open - {} rows, {} groups, submenu {} rows; box "
									   "[{:.0f},{:.0f} {:.0f}x{:.0f}]; HUD rem {:.1f} px, window {}x{}",
									   menu->TopRows(), menu->Groups(), menu->SubmenuRows(), box.x,
									   box.y, box.w, box.h, rem, m_window.Width(), m_window.Height());
							   } else {
								   line = std::format("handmenu: closed; HUD rem {:.1f} px, window {}x{}",
													  rem, m_window.Width(), m_window.Height());
							   }
							   m_console.Print(line);
							   log::Info("{}", line);
						   };
						   const std::string what = args.empty() ? "status" : args[0];
						   if (what == "status") {
							   report();
							   return;
						   }
						   if (what == "off") {
							   if (menu) menu->Close();
							   report();
							   return;
						   }
						   if (what == "group") {
							   if (!Need(m_console, args, 2)) return;
							   const int n = std::atoi(args[1].c_str());
							   if (!menu || !menu->IsOpen() || n < 0 ||
								   !menu->OpenGroup(static_cast<size_t>(n))) {
								   m_console.Refuse("no open hand menu has that group");
								   return;
							   }
							   report();
							   return;
						   }
						   if (!Need(m_console, args, 2)) return;
						   const size_t m = static_cast<size_t>(std::atoi(args[0].c_str()));
						   if (m >= m_characters.size() || (args[1] != "0" && args[1] != "1")) {
							   m_console.Refuse("no such member or hand");
							   return;
						   }
						   // Under the map overlay the HUD takes no input, so no click
						   // reaches a hand box - and a menu opened anyway is never
						   // laid out (`editor off` leaves the map open in Player mode).
						   if (m_mapView.IsOpen()) {
							   m_console.Refuse("the map is open - shut it first (`mappage close`)");
							   return;
						   }
						   const size_t hand = args[1] == "1" ? 1 : 0;
						   const auto defs = m_world->SpellDefs();
						   for (size_t k = 2; k < args.size(); ++k)
							   if (std::ranges::none_of(defs, [&](const std::unique_ptr<Spell>& d) {
									   return d->Id() == args[k];
								   })) {
								   m_console.Refuse(std::format("no spell '{}'", args[k]));
								   return;
							   }
						   for (size_t k = 2; k < args.size(); ++k)
							   m_characters[m].TouchSpellMru(hand, args[k]);
						   if (!m_ui.OpenHandMenuAtBox(m, hand)) {
							   m_console.Refuse("no shown hand box is that hand, or it offers nothing");
							   return;
						   }
						   report();
					   });

	// The rest STATE, for a script and for a quick look. It reports the world
	// speed too, since that is the whole mechanism and the number a reader needs
	// to interpret how much simulated time a `step` just covered.
	// BARE `rest` REPORTS AND DOES NOT TOGGLE. It was a toggle for about ten
	// minutes, and the eval script written against it read `rest` as a status
	// query at four places — each of which silently turned the state back on and
	// made the auto-stop rules look broken when they were working. A query that
	// mutates is a trap, and this one caught its own author.
	m_console.Register({.name = "rest",
						.group = CmdGroup::Characters,
						.params = "[on|off]\n"
								  "until [secs]\n"
								  "button",
						.summary = "report or set the rest state, or rest until it ends"},
					   [this](const std::vector<std::string>& args) {
						   // `rest button` - where the HUD's Rest button is, so a
						   // harness can start a rest the way a player does: a click
						   // in an ordinary frame, which the allocation guard arms,
						   // rather than a command in the console's, which it never
						   // does (AllocTest -Rest).
						   if (!args.empty() && args[0] == "button") {
							   const gfx::Rect r = m_ui.RestButtonRect();
							   m_console.Print(std::format("rest button: {},{}",
														   static_cast<int>(r.x + r.w * 0.5f),
														   static_cast<int>(r.y + r.h * 0.5f)));
							   return;
						   }
						   // `rest until` — enter rest AND run the world until it
						   // ends. This is the form a script wants, and the reason
						   // it exists is a trap worth recording: `rest on` followed
						   // by `step N` does NOT measure a rest. UpdateRest does not
						   // depend on dt, so it fires on the very next ORDINARY
						   // frame — and with timescale 0 that frame happens between
						   // the two console commands. If there was nothing to
						   // recover, rest was already over before the step began,
						   // and the step then ran its FULL budget of dungeon time
						   // with no rest in progress: supplies drained for fifteen
						   // minutes and the table read as "resting is expensive".
						   //
						   // Stepping from inside the same command leaves no frame
						   // in between, so the state cannot end before the clock
						   // starts. StepWorld already stops the moment rest ends.
						   if (!args.empty() && args[0] == "until") {
							   const float cap =
								   args.size() > 1
									   ? static_cast<float>(std::atof(args[1].c_str()))
									   : 3600.0f;
							   m_world->SetResting(true);
							   // A cap is an upper BOUND, not a claim about elapsed time, so
								   // unlike `step` this does not refuse when it is
								   // wider than one call can run.
								   StepStop stop{};
								   const int ran = StepWorld(cap, stop);
							   m_console.Print(std::format(
								   "rested {:.2f}s — {}",
								   static_cast<float>(ran) / 60.0f,
								   m_world->Resting() ? "still resting (hit the cap)"
													 : m_world->RestEndReason()));
							   return;
						   }
						   if (!args.empty()) m_world->SetResting(args[0] != "off");
						   const char* why = m_world->RestEndReason();
						   m_console.Print(std::format(
							   "rest {} (world x{:.0f}){}{}",
							   m_world->Resting() ? "on" : "off",
							   m_world->RestTimeScale(),
							   *why && !m_world->Resting() ? "  last ended: " : "",
							   *why && !m_world->Resting() ? why : ""));
					   });

	// Eat or drink a catalog item outright — no inventory, no hand slot. The UI
	// path (a hand-menu `eat`/`drink`) runs the very same DungeonWorld::
	// ConsumeItem, so this exercises the arithmetic and the effect-lifting that
	// a script cannot reach by clicking (see [[hands-on-visual-testing]]).
	m_console.Register({.name = "consume",
						.group = CmdGroup::Characters,
						.params = "<item> [member]",
						.summary = "eat or drink an item outright"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   const size_t m =
							   args.size() > 1
								   ? static_cast<size_t>(std::atoi(args[1].c_str()))
								   : 0;
						   if (m >= m_characters.size()) {
							   m_console.Refuse("no such member");
							   return;
						   }
						   const resource::Refill got =
							   m_world->ConsumeItem(m_characters[m], args[0]);
						   if (got.downed) {
							   m_console.Print(std::format("{} is down and drinks nothing",
														   m_characters[m].name));
							   return;
						   }
						   m_console.Print(
							   got.Any()
								   ? std::format("{} consumes {}: food +{:.1f} water +{:.1f} "
												 "health +{:.1f} stamina +{:.1f} mana +{:.1f} "
												 "cured {}",
												 m_characters[m].name, args[0], got.food,
												 got.water, got.health, got.stamina, got.mana,
												 got.cured)
								   : std::format("{} gains nothing from {} "
												 "(not consumable, or already full)",
												 m_characters[m].name, args[0]));
					   });

	// Seeding a supply state, so a script can start a rung hungry instead of
	// stepping eight hours to get there.
	m_console.Register({.name = "setsupply",
						.group = CmdGroup::Characters,
						.params = "<member|all> <food|water> <n>",
						.summary = "set a member's or the party's food or water"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 3)) return;
						   const bool all = args[0] == "all";
						   const size_t one =
							   static_cast<size_t>(std::atoi(args[0].c_str()));
						   if (!all && one >= m_characters.size()) {
							   m_console.Refuse("no such member");
							   return;
						   }
						   resource::Supply which{};
						   if (args[1] == "food") which = resource::Supply::Food;
						   else if (args[1] == "water") which = resource::Supply::Water;
						   else {
							   m_console.Print("expected food or water");
							   return;
						   }
						   const float max =
							   m_world->GetBalance().SupplyOf(which).max;
						   const float v = std::clamp(
							   static_cast<float>(std::atof(args[2].c_str())), 0.0f,
							   max);
						   for (size_t i = 0; i < m_characters.size(); ++i) {
							   if (!all && i != one) continue;
							   m_characters[i].SupplyLevel(which) = v;
						   }
						   m_console.Print(std::format("{} {} = {:.1f}",
													   all ? "party" : m_characters[one].name,
													   args[1], v));
					   });

	// Seeding a POOL, setsupply's twin for health, stamina and mana: a value set
	// outright, clamped to the member's maximum. It exists for the state no other
	// command reaches cleanly - a member at 0 health, UNCONSCIOUS but not dead,
	// which AllocTest -Rest needs: the downed wait out the stabilize clock, a
	// monster in aggro resets it, so a rest beside one cannot end by itself.
	// (A bleed downs a member only to finish them on its next tick; over-exertion
	// needs a monster to swing at.) A health write is a harness fiat, not a game
	// rule, so it REBASES the damage ledger, as `heal` does. The dead are left
	// alone: a pool does not raise them, `heal` does.
	m_console.Register({.name = "setpool",
						.group = CmdGroup::Characters,
						.params = "<member|all> <health|stamina|mana> <n>",
						.summary = "set a member's or the party's health, stamina or mana"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 3)) return;
						   const bool all = args[0] == "all";
						   const size_t one =
							   static_cast<size_t>(std::atoi(args[0].c_str()));
						   if (!all && one >= m_characters.size()) {
							   m_console.Refuse("no such member");
							   return;
						   }
						   const std::string& pool = args[1];
						   if (pool != "health" && pool != "stamina" && pool != "mana") {
							   m_console.Refuse("expected health, stamina or mana");
							   return;
						   }
						   const float want =
							   std::max(0.0f, static_cast<float>(std::atof(args[2].c_str())));
						   int set = 0;
						   for (size_t i = 0; i < m_characters.size(); ++i) {
							   if (!all && i != one) continue;
							   Character& c = m_characters[i];
							   if (c.dead) continue;
							   if (pool == "health") c.health = std::min(want, c.maxHealth);
							   else if (pool == "stamina") c.stamina = std::min(want, c.maxStamina);
							   else c.mana = std::min(want, c.maxMana);
							   ++set;
						   }
						   if (set == 0) {
							   m_console.Refuse("setpool: nobody to set (the dead keep no "
												"pools; `heal` raises them)");
							   return;
						   }
						   if (pool == "health") m_world->RebaseDamageLedger();
						   m_console.Print(std::format("{} {} = {:.1f}",
													   all ? "party" : m_characters[one].name,
													   pool, want));
					   });

	// --- seeding a rung (docs/eval-harness.md) ------------------------------
	// A single eval run cannot play from fresh characters to end-game, so a rung
	// has to START where it wants to measure. These two put a member wherever on
	// the curve the test needs.
	m_console.Register({.name = "setstat",
						.group = CmdGroup::Characters,
						.params = "<member> <str|dex|vit|wil|int> <n>",
						.summary = "set a member's stat and re-derive the pools"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 3)) return;
						   const size_t m =
							   static_cast<size_t>(std::atoi(args[0].c_str()));
						   if (m >= m_characters.size()) {
							   m_console.Refuse("no such member");
							   return;
						   }
						   Character& c = m_characters[m];
						   const int n = std::atoi(args[2].c_str());
						   const std::string& s = args[1];
						   if (s.starts_with("str")) c.strength = n;
						   else if (s.starts_with("dex")) c.dexterity = n;
						   else if (s.starts_with("vit")) c.vitality = n;
						   else if (s.starts_with("wil")) c.willpower = n;
						   else if (s.starts_with("int")) c.intelligence = n;
						   else {
							   m_console.Refuse("unknown stat: " + s);
							   return;
						   }
						   // Health/stamina/mana maxima DERIVE from stats
						   // (Character::RecomputeMaxima), so a stat set without
						   // this leaves a level-20 fighter with a novice's hit
						   // points and every number after it measured wrong.
						   m_world->RecomputePartyMaxima();
						   m_console.Print(std::format("{} {} = {}", c.name, s, n));
					   });

	// Levels DERIVE from raw xp (floor(sqrt)), so this sets the xp that yields
	// the level asked for — squaring is the honest inverse, and it means a
	// seeded skill trains onward from exactly where a played one would have.
	// Without it, giving a caster a usable fire skill for a test means casting
	// thirty times and hoping the mana holds out.
	m_console.Register({.name = "setskill",
						.group = CmdGroup::Characters,
						.params = "<member> <skill> <level[.fraction]>",
						.summary = "set a member's skill level and re-derive the pools"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 3)) return;
						   const size_t m =
							   static_cast<size_t>(std::atoi(args[0].c_str()));
						   if (m >= m_characters.size()) {
							   m_console.Refuse("no such member");
							   return;
						   }
						   // A FRACTION is the way to the next level: 2.5 sits
						   // halfway from 2 to 3 (what the sheet's skill bar shows).
						   const float wanted = static_cast<float>(std::atof(args[2].c_str()));
						   if (wanted < 0.0f) {
							   m_console.Refuse("level cannot be negative");
							   return;
						   }
						   Character& c = m_characters[m];
						   const float level = std::floor(wanted);
						   const float xp = level * level + (wanted - level) * (2.0f * level + 1.0f);
						   c.skillXp[args[1]] = xp;
						   // RE-DERIVE, for exactly the reason `setstat` already
						   // had to: a RESOURCE practice feeds the pool maxima
						   // and the walking pace now, so a skill set without
						   // this leaves a conditioned member carrying a novice's
						   // stamina bar and the party walking at the old speed —
						   // and everything measured afterwards is quietly wrong.
						   m_world->RecomputePartyMaxima();
						   m_console.Print(std::format("{} {} = level {} ({:.0f} xp)",
													   c.name, args[1],
													   c.SkillLevel(args[1]), xp));
					   });

	// RESET THE PARTY BETWEEN RUNGS. A ladder runs many encounters in one
	// process, and the first one that wipes ends the run: a wipe returns to the
	// TITLE SCREEN (Game_Wiring's onPartyWipe), after which every `step` is
	// correctly refused and every rung after it measures nothing. Found exactly
	// that way — rung 2 of the first two-tier script never ran.
	//
	// `newgame` would also fix it and costs a full staged reload per rung; this
	// restores in place. It deliberately does NOT touch stats, skills, gear or
	// stance: those are what a preset SEEDED, and a heal that undid the seeding
	// would make the second rung measure the first one's party.
	m_console.Register({.name = "heal",
						.group = CmdGroup::Characters,
						.params = "[member]",
						.summary = "restore the party (or one member) to full"},
					   [this](const std::vector<std::string>& args) {
						   const auto restore = [this](Character& c) {
							   c.dead = false;
							   c.health = c.maxHealth;
							   c.stamina = c.maxStamina;
							   c.mana = c.maxMana;
							   c.exhausted = false;
							   c.staminaHoldoff = 0.0f;
							   c.stabilize = 0.0f;
							   c.hitFlash = 0.0f;
							   c.handCooldown[0] = c.handCooldown[1] = 0.0f;
							   // A burn carried over from the previous rung
							   // would tick into the next one's numbers.
							   c.effects.clear();
						   };
						   if (!args.empty()) {
							   const size_t m =
								   static_cast<size_t>(std::atoi(args[0].c_str()));
							   if (m >= m_characters.size()) {
								   m_console.Refuse("no such member");
								   return;
							   }
							   restore(m_characters[m]);
							   // A harness fiat, not a game rule: nothing a player
							   // can do heals like this, so it is REBASED rather
							   // than sanctioned — there is no route worth naming
							   // (Game/DamageLedger.h).
							   m_world->RebaseDamageLedger();
							   m_console.Print(
								   std::format("healed {}", m_characters[m].name));
							   return;
						   }
						   for (Character& c : m_characters) restore(c);
						   m_world->RebaseDamageLedger();
						   // A wipe left the app on the title screen; put it back
						   // in play, or the heal fixes the party and the next
						   // `step` still refuses.
						   m_ui.ResetHudStatus();
						   // ...and clear the wipe LATCH, which gates every
						   // monster attack. Without it the party stands up and
						   // nothing ever swings at them again — a whole sweep
						   // of rungs reporting forty-five seconds of nothing.
						   m_world->ClearWipeLatch();
						   ResumeAfterHeal();
						   m_console.Print(std::format("healed the party ({})",
													   StateName()));
					   });

	// The seeding half of `party`: what a member IS, rather than how they are
	// doing. A rung that seeded nothing (a typo'd skill id, a member index past
	// the roster) would otherwise run and report a perfectly plausible number
	// for the wrong character.
	m_console.Register({.name = "char",
						.group = CmdGroup::Characters,
						.params = "<member>",
						.summary = "a member's stats, skills, creep pools and gear"},
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1)) return;
						   const size_t m =
							   static_cast<size_t>(std::atoi(args[0].c_str()));
						   if (m >= m_characters.size()) {
							   m_console.Refuse("no such member");
							   return;
						   }
						   const Character& c = m_characters[m];
						   m_console.Print(std::format(
							   "  {}  str {} dex {} vit {} wil {} int {}", c.name,
							   c.strength, c.dexterity, c.vitality, c.willpower,
							   c.intelligence));
						   m_console.Print(std::format(
							   "    hp {:.1f}/{:.1f}  st {:.1f}/{:.1f}  mp {:.1f}/{:.1f}",
							   c.health, c.maxHealth, c.stamina, c.maxStamina, c.mana,
							   c.maxMana));
						   // Two places: a lesson can pay under half a point (heavy
						   // armour's 0.45), and tools\CombatTest.py counts lessons
						   // out of the xp.
						   for (const auto& [id, xp] : c.skillXp)
							   if (xp > 0.0f) // the untrained rest is the seed, not news
								   m_console.Print(std::format("    skill {:<12} level {} ({:.2f} xp)",
															   id, Character::LevelForXp(xp), xp));
						   // THE CREEP POOLS, and they are here for one reason:
						   // the resource practices must creep NOTHING
						   // (docs/health-and-healing.md). Without this line the
						   // only evidence is that a stat has not moved yet — and
						   // a slow leak reads exactly like no leak until the pool
						   // crosses 1.0, which is the "absent and correct report
						   // identically" trap this project has already paid for.
						   // A non-zero pool beside a resource skill IS the bug.
						   // statProgress is an ARRAY that the kStats table sizes,
						   // not the name-keyed dictionary it was — that subscript
						   // allocated during play (Character.h). The row names it.
						   for (int s = 0; s < kStatCount; ++s)
							   if (c.statProgress[static_cast<size_t>(s)] > 0.0f)
								   m_console.Print(std::format(
									   "    creep {:<12} {:.3f} toward the next point",
									   kStats[static_cast<size_t>(s)].id,
									   c.statProgress[static_cast<size_t>(s)]));
						   for (int h = 0; h < 2; ++h) {
							   const ItemSlot& slot = c.inventory.Hand(h);
							   m_console.Print(std::format(
								   "    hand{} {}", h,
								   slot.Empty() ? "(empty)" : slot.typeId));
						   }
						   for (int e = 0; e < kEquipCount; ++e) {
							   // The HANDS are equipment slots too (Inventory::
							   // Hand indexes this same array), so listing every
							   // slot printed each weapon twice and read as a
							   // member wearing their own sword.
							   const auto id = static_cast<EquipSlot>(e);
							   if (id == EquipSlot::LeftHand ||
								   id == EquipSlot::RightHand)
								   continue;
							   const ItemSlot& slot = c.inventory.equipment[
								   static_cast<size_t>(e)];
							   if (!slot.Empty())
								   m_console.Print(std::format("    worn  {}", slot.typeId));
						   }
						   // THE DEFENSE AS EACH READER SEES IT, side by side: the
						   // class (WornArmorClass), the pipeline's soak
						   // (PartyTarget::Soak) beside the sheet's (DefenseFor),
						   // and every resist the pipeline would apply. They once
						   // disagreed about the hands (code-review C12), and
						   // nothing but a line like this could show it.
						   if (m_world) {
							   const DamageTypeBook& types = m_world->DamageTypes();
							   std::string resists;
							   for (size_t t = 0; t < types.Count(); ++t) {
								   const DamageType type{static_cast<u8>(t)};
								   const float r = m_world->PipelineResist(c, type);
								   if (r != 0.0f)
									   resists += std::format(" {}={:.2f}", types.Id(type), r);
							   }
							   m_console.Print(std::format(
								   "    defense class {} soak {:.1f} sheet {:.1f} resist{}",
								   ArmorClassId(m_world->WornArmorClass(c)),
								   m_world->PipelineSoak(c), m_world->DefenseFor(c).soak,
								   resists.empty() ? std::string(" -") : resists));
						   }
						   // What is on the member: a ward cast on the whole party
						   // has nowhere else to be seen from the console.
						   for (const fx::Inst& e : c.effects)
							   m_console.Print(std::format("    effect {} {:.1f} {:.1f}s", e.Id(),
														   e.magnitude, e.timeLeft));
					   });
}

} // namespace dungeon::game
