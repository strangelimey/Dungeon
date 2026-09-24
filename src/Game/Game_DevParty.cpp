// ============================================================================
// Game/Game_DevParty.cpp — the party's dev-console commands.
//
// Split out of Game_DevCommands.cpp by concern: what a member knows, carries
// and does (learn/rune/give/guard/swing/wear/equip/effect/threat/cast), their
// pools and supplies (party/regen/supplies/rest/consume/setsupply), and the
// sheet and the dev setters that re-derive it (sheet/setstat/setskill/heal/
// char).
// ============================================================================
#include "Game/Game.h"

#include "Core/Log.h"
#include "Game/DevCommandArgs.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <string>

namespace dungeon::game {

using devargs::Need;
using devargs::ParseSymbolArg;

void Game::RegisterPartyCommands() {
	m_console.Register("learn", "grant a spell symbol to a member (dev)",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 2,
									 "usage: learn <member 0-3> <fire|earth|air|water>"))
							   return;
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
	m_console.Register("rune", "give a rune tablet to the lead member's pack (dev)",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1,
									 "usage: rune <fire|earth|air|water>"))
							   return;
						   SpellSymbol sym;
						   if (!ParseSymbolArg(m_console, args[0], sym)) return;
						   const std::string typeId = RuneItemId(sym);
						   if (m_characters.empty() ||
							   !m_characters[0].inventory.Stow(typeId))
							   m_console.Print("pack full (or no party)");
						   else
							   m_console.Print(std::format("pack += {}", typeId));
					   });
	m_console.Register("give", "stow an items.cat item in a member's pack (dev)",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1,
									 "usage: give <item id> [member 0-3]"))
							   return;
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
	// The offense/defense split before its slider exists
	// (docs/damage-system.md). Worth keeping once the UI lands: setting an
	// exact share is how the split gets MEASURED, where dragging a slider is
	// how it gets FELT, and those are different questions.
	m_console.Register("guard", "set a hand's offense share 0..N (dev)",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1,
									 "usage: guard <share 0..N> [member 0-3]"))
							   return;
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
						   // The held-back share is reported UNCLAMPED, because a
						   // negative one is the whole point past 1 — the guard
						   // becomes a penalty, and a readout that floored it at
						   // 0% would say an over-exerted stance and an all-out
						   // one were the same thing.
						   const float held = (1.0f - share) * 100.0f;
						   m_console.Print(std::format(
							   "{} offense {:.2f} (guarding with {:.0f}% of hand "
							   "skill{})",
							   c.name, share, held,
							   share > 1.0f ? " — OVER-EXERTED" : ""));
					   });

	// THE PARTY'S SWING, from the console. `equip` and `wear` exist because the
	// armor system was untestable without them; this is the same gap one step
	// further on — every attack-side rule (the stance, crit pierce, and now the
	// whole fumble consequence table) could only be reached by clicking a hand
	// slot in the HUD, which no script drives reliably. A verb of "" takes the
	// neutral attack, exactly as the hand menu's default does.
	m_console.Register("swing", "attack with a member's hand (dev): swing <member> [hand] [verb]",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1,
									 "usage: swing <member 0-3> [hand 0/1] [verb]"))
							   return;
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
	m_console.Register("wear", "put an item in its worn doll slot (dev)",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1,
									 "usage: wear <item id|none> [member 0-3]"))
							   return;
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
							   c.inventory.equipment[static_cast<size_t>(i)].typeId = args[0];
							   m_console.Print(std::format("{} wears {} ({})", c.name,
														   args[0], WearSlotId(w)));
							   return;
						   }
					   });

	// `give` fills the pack; this puts a weapon straight in a hand, which is
	// what a combat test actually needs (no cursor drag, no HUD clicking).
	m_console.Register("equip", "put an item in a member's hand (dev)",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1,
									 "usage: equip <item id> [member 0-3] [hand 0/1]"))
							   return;
						   const size_t m = args.size() > 1
							   ? static_cast<size_t>(std::atoi(args[1].c_str())) : 0;
						   const int hand = args.size() > 2
							   ? std::clamp(std::atoi(args[2].c_str()), 0, 1) : 0;
						   if (m >= m_characters.size()) {
							   m_console.Refuse("no such member");
							   return;
						   }
						   if (!m_project.HasItem(args[0])) {
							   m_console.Print(std::format("no item '{}' in items/weapons/armor", args[0]));
							   return;
						   }
						   m_characters[m].inventory.Hand(hand).typeId = args[0];
						   m_console.Print(std::format("{} {} hand = {}",
													   m_characters[m].name,
													   hand == 0 ? "left" : "right",
													   args[0]));
					   });
	// Land a status effect directly, skipping the cast. Setting a ward up in a
	// live fight is otherwise a coin toss — vocabulary, mana, and the fumble
	// roll all have to go your way, and then a monster has to choose to hit
	// the bearer before you see the ward DO anything.
	m_console.Register("effect", "apply a status effect to a member or the monster ahead (dev)",
					   [this](const std::vector<std::string>& args) {
						   // MAGNITUDE IS PER SECOND for a DoT, and the default
						   // pair (8 for 60s) is therefore 480 damage against a
						   // 42 hp member. Spelled out in the usage because the
						   // argument order reads as "10 damage over 20 seconds"
						   // and means almost the opposite: `effect bleed 0 10
						   // 20` deals 200 and annihilates the party
						   // (docs/eval-audit.md).
						   if (!Need(m_console, args, 1,
									 "usage: effect <id> [member 0-3 | ahead] "
									 "[magnitude, PER SECOND for a DoT] [seconds]"))
							   return;
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
	m_console.Register("threat",
					   "list per-member threat for every monster holding a grudge (dev)",
					   [this](const std::vector<std::string>&) {
						   const std::vector<std::string> lines = m_world->ThreatReport();
						   if (lines.empty()) {
							   m_console.Print("no threat anywhere");
							   return;
						   }
						   for (const std::string& l : lines) m_console.Print("  " + l);
					   });
	m_console.Register("cast", "cast a spell by symbol sequence (dev): cast <member> [hand 0/1] <sym>...",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 2,
									 "usage: cast <member 0-3> [hand 0/1] <sym> [sym...]"))
							   return;
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

	// The party's side of an encounter, in one machine-readable block. `monsters`
	// has printed the other side for a while; without this a harness can watch a
	// fight and never learn what it COST, which is most of what a balance pass
	// is trying to find out.
	m_console.Register("party", "each member's hp/stamina/mana + stance (dev)",
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
	m_console.Register("regen", "health/stamina/mana per second, and the ordering (dev)",
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
	m_console.Register("supplies", "each member's food and water, and hours left (dev)",
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

	// Open (or close) the character sheet. It exists because the sheet was
	// reachable ONLY by clicking a portrait, which is why `/check-ingame`
	// reports it as a screen it cannot sweep — so the one screen with the most
	// hand-laid-out content in the game was also the one screen `uioverlap`
	// never saw. `sheet <n>` then `uioverlap` closes half of that gap.
	m_console.Register("sheet", "open the character sheet (dev): sheet <member|off>",
					   [this](const std::vector<std::string>& args) {
						   if (!args.empty() && args[0] == "off") {
							   if (m_state == AppState::CharacterSheet)
								   m_state = AppState::Playing;
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

	// The rest STATE, for a script and for a quick look. It reports the world
	// speed too, since that is the whole mechanism and the number a reader needs
	// to interpret how much simulated time a `step` just covered.
	// BARE `rest` REPORTS AND DOES NOT TOGGLE. It was a toggle for about ten
	// minutes, and the eval script written against it read `rest` as a status
	// query at four places — each of which silently turned the state back on and
	// made the auto-stop rules look broken when they were working. A query that
	// mutates is a trap, and this one caught its own author.
	m_console.Register("rest",
					   "the rest state (dev): rest [on|off|until [secs]], bare = report",
					   [this](const std::vector<std::string>& args) {
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
	m_console.Register("consume", "eat or drink an item (dev): consume <item> [member]",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1,
									 "usage: consume <item id> [member 0-3]"))
							   return;
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
						   m_console.Print(
							   got.Any()
								   ? std::format("{} consumes {}: food +{:.1f} water +{:.1f}",
												 m_characters[m].name, args[0],
												 got.food, got.water)
								   : std::format("{} gains nothing from {} "
												 "(not consumable, or already full)",
												 m_characters[m].name, args[0]));
					   });

	// Seeding a supply state, so a script can start a rung hungry instead of
	// stepping eight hours to get there.
	m_console.Register("setsupply",
					   "set food/water (dev): setsupply <member|all> <food|water> <n>",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 3,
									 "usage: setsupply <member 0-3|all> "
									 "<food|water> <n>"))
							   return;
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

	// --- seeding a rung (docs/eval-harness.md) ------------------------------
	// A single eval run cannot play from fresh characters to end-game, so a rung
	// has to START where it wants to measure. These two put a member wherever on
	// the curve the test needs.
	m_console.Register("setstat", "set a stat (dev): setstat <member> <stat> <n>",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 3,
									 "usage: setstat <member 0-3> "
									 "<str|dex|vit|wil|int> <n>"))
							   return;
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
	m_console.Register("setskill", "set a skill level (dev): setskill <member> <skill> <level>",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 3,
									 "usage: setskill <member 0-3> <skill id> <level>"))
							   return;
						   const size_t m =
							   static_cast<size_t>(std::atoi(args[0].c_str()));
						   if (m >= m_characters.size()) {
							   m_console.Refuse("no such member");
							   return;
						   }
						   const int level = std::atoi(args[2].c_str());
						   if (level < 0) {
							   m_console.Refuse("level cannot be negative");
							   return;
						   }
						   Character& c = m_characters[m];
						   const float xp = static_cast<float>(level) * level;
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
	m_console.Register("heal", "restore the party to full (dev): heal [member]",
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
	m_console.Register("char", "a member's stats, skills and gear (dev): char <member>",
					   [this](const std::vector<std::string>& args) {
						   if (!Need(m_console, args, 1, "usage: char <member 0-3>")) return;
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
						   for (const auto& [id, xp] : c.skillXp)
							   if (xp > 0.0f) // the untrained rest is the seed, not news
								   m_console.Print(std::format("    skill {:<12} level {} ({:.1f} xp)",
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
					   });
}

} // namespace dungeon::game
