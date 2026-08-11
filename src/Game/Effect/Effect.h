// ============================================================================
// Game/Effect/Effect.h — status effects: the KIND registry and the per-
// combatant INSTANCE.
//
// Everything transient riding a combatant — a ward, a poison, a burn, the
// see-through Sight mark — is one entry in that combatant's effect list. The
// split is the codebase's usual flyweight (ItemKind/Item, MonsterKind/
// Monster): the fat, behaviour-carrying half is ONE shared EffectKind loaded
// at startup, and what a combatant actually carries is a small POD Inst
// pointing at it. So a combatant with no effects costs nothing, applying one
// is a push_back, and the per-frame tick walks plain values.
//
// Identity and behaviour are C++, numbers and presentation are data — the
// settled spells-are-classes rule (docs/magic system.md). Each kind is a class
// in this folder, one file pair, hand-listed in AllEffects.cpp; the project's
// effects.cat only OVERRIDES their numbers and look. An effects.cat entry
// naming no class is a warning, never a new effect.
//
// P1 (docs/effects.md) builds the registry and moves Character's list onto it;
// behaviour still lives at the sites that always had it. P2 adds the pipeline
// hooks — deflect / mitigate / absorb / react — to this base, and the four
// wards' scattered implementations move into their kind classes.
// ============================================================================
#pragma once

#include "Core/Types.h"
#include "Game/Combat.h" // DamageType, StrikeRules, the strike resolver
#include "Game/Spells.h" // SpellSymbol (an effect's school: tint + flavour)

#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon::game {
class Catalog;
struct CatalogEntry;
} // namespace dungeon::game

namespace dungeon::game::fx {

// The FAMILY an effect belongs to. Behaviour is per class, not per category —
// this is for the queries that ask "any ward?" without naming one, and for the
// save/legacy token mapping.
enum class Category : u8 {
	Ward,   // the Protect shields — one kind per school, four behaviours
	Dot,    // damage over time (poison, bleed, burn)
	Marker, // no numbers of its own; the world reads its presence (Sight)
};

// What happens when an effect lands on someone already carrying its kind.
// Refresh is the house rule for both wards and DoTs (the old effect goes, the
// new one lands whole); a kind that genuinely wants to pile up opts in.
enum class Stacking : u8 {
	Refresh,          // replace any instance of this kind
	RefreshPerSchool, // replace only the instance carrying the same school
	Stack,            // pile up — every application is its own instance
};

struct Inst;
struct DamageEvent;
class ITarget;
class EffectKind;
class EffectBook;

// An ON-HIT PROC as content authors it: an effect id plus the numbers to land
// it with — "burn 3 6 0.5" is "burn at 3 a second for 6 seconds, half the
// time". A landed blow rolls each of its source's procs (ApplyProcs).
//
// This is how a weapon or a monster names an effect: by ID, so a serrated
// blade authors `on_hit = bleed` and a frost axe `element = water` +
// `on_hit = burn` with no engine change at all.
struct Proc {
	std::string id;         // effects.cat / class id
	float magnitude = 0.0f; // a DoT's damage per second
	float duration = 0.0f;  // seconds
	float chance = 1.0f;    // 0..1 roll on a landed blow
};

// Parse an authored proc list: entries separated by commas or semicolons, each
// "<id> <magnitude> <seconds> [chance]". `where` names the catalog entry in any
// warning. An empty spec adds nothing.
void ParseProcs(std::string_view spec, std::vector<Proc>& out,
				std::string_view where);

// The few balance.cat knobs an effect's own maths needs, handed in by the host
// so this module never includes Balance.h (and the knobs stay in the editor's
// Balance dialog, where they are tuned).
struct Knobs {
	float stoneskinResist = 1.0f; // ward magnitude -> physical resist
};

// What a REACTING effect needs to hit back with (stage 6). A reprisal is
// damage like any other, so it goes through Deal rather than doing its own
// arithmetic — which means the react hook has to carry the two things Deal
// takes. Bundled so the hook signature stays one parameter wide.
//
// No cascade: Deal never calls React, so a fire shield answering a fire shield
// stops after one exchange.
// (StrikeRules by VALUE — four floats, and Balance hands them out by value, so
// a call site can write the context inline without a dangling-reference trap.)
struct ReactCtx {
	StrikeRules rules;
	std::mt19937& rng;
};

// HOW damage arrives. The stages read it to decide which effects care: the wind
// ward turns aside bolts and not blows, the fire shield burns back at a swung
// blow and not a poison tick.
enum class Delivery : u8 {
	Melee,  // a swung blow, rolled against evasion
	Ranged, // a flying bolt, likewise
	Tick,   // a DoT's per-frame bite: unavoidable, and QUIET (a per-frame
			// event must not spam the log or flash a splat every frame)
	Impact, // unavoidable but not quiet — a wall bump, a ward's reprisal, the
			// elemental half of an enchanted blow
};

// ONE incoming hit, walked through the six stages (docs/effects.md): deflect,
// strike, mitigate, absorb, apply, react. Every source of damage — a swing, a
// bolt, a burn, a bump — builds one of these and hands it to Deal, so that
// resists, wards and knobs apply in exactly one place.
//
// The three booleans say which parts of the maths this kind of damage is
// subject to; the Delivery presets below set them the usual way. They are
// separate from Delivery because they genuinely vary independently — an
// enchanted weapon's elemental term is resisted by element but neither rolled
// nor soaked (plate turns a blade, not a flame).
struct DamageEvent {
	DamageType type{};
	float amount = 0.0f;   // the attacker's assembled damage, before defenses
	float accuracy = 0.0f; // 0..1, against the defender's evasion (if rolled)
	Delivery delivery = Delivery::Melee;
	// The roster index behind this damage (threat credit), or -1 for none —
	// a monster's own blow, a wall, an unattributed tick.
	int source = -1;
	bool rolled = true;   // run the accuracy-vs-evasion strike roll
	bool soaked = true;   // armor subtracts a flat amount
	bool resisted = true; // the defender's resist for `type` scales it

	// --- filled in by the stages ---
	bool deflected = false; // an effect turned it aside outright
	bool hit = false;       // the strike landed (always true when unrolled)
	float dealt = 0.0f;     // what actually reached hit points
	// This event finished the target. The apply stage sets it; the CALLER says
	// the line, because only the caller knows what killed them ("slain" /
	// "destroyed" / "burns away") and where it belongs in the narration — the
	// blow's own "hits for 25" has to come first.
	bool slew = false;

	// A per-frame tick stays silent: no splat, no flinch, no soak line.
	bool Quiet() const { return delivery == Delivery::Tick; }

	// The presets, each naming a KIND of damage rather than a set of flags —
	// so a caller picks the thing that happened and the maths follows.
	// Everything is resisted; they differ in what else applies.
	//
	//   Blow / Bolt  a swing or a shot: rolled, soaked, resisted
	//   Impact       a COLLISION — a wall, a door, a falling rock. Not rolled
	//                (it simply happens), but bludgeon damage that armour
	//                genuinely blunts: soaked AND resisted.
	//   Burst        magical damage riding something else: an enchanted
	//                blade's element, a ward's reprisal. Resisted but NOT
	//                soaked — plate turns a blade, not a flame.
	//   Tick         a DoT's bite: resisted as it lands, so a ward raised
	//                mid-burn helps at once. Never soaked (armour does not
	//                help with poison already in the blood).
	static DamageEvent Blow(DamageType type, float amount, float accuracy,
							int source = -1);
	static DamageEvent Bolt(DamageType type, float amount, float accuracy,
							int source = -1);
	static DamageEvent Impact(DamageType type, float amount, int source = -1);
	static DamageEvent Burst(DamageType type, float amount, int source = -1);
	static DamageEvent Tick(DamageType type, float amount, int source = -1);
};

// What the stages need to know about — and do to — a combatant, whoever it is.
// DungeonWorld implements it twice, over Character and over Monster; nothing in
// this module knows either type exists. That wall is what makes the two sides
// genuinely share one pipeline instead of two that merely resemble each other.
class ITarget {
public:
	virtual ~ITarget() = default;

	// The defender's numbers. Resist is FINAL — nature/catalog + equipment +
	// this target's effects, summed and clamped by the host's rule.
	virtual float Evasion() const = 0;
	virtual float Soak() const = 0;
	virtual float Resist(DamageType type) const = 0;

	// The effects riding this combatant — the stages walk them for hooks.
	virtual std::vector<Inst>& Effects() = 0;

	// Stage 5: put `amount` into hit points and run this side's consequences
	// (a member's splat / unconscious / overkill / wipe latch; a monster's
	// threat credit / flinch / corpse). The one genuinely per-side stage. Sets
	// ev.slew when this is the damage that finished them.
	virtual void Wound(float amount, DamageEvent& ev) = 0;
	// The mirror: this target DRINKS the element (a nature resist past 1) and
	// is healed by `amount` instead of hurt. Capped at its maximum, and it
	// announces itself — being fed by a blow is worth saying out loud.
	virtual void Absorb(float amount, DamageEvent& ev) = 0;

	// For the lines effects write about their bearer.
	virtual std::string Name() const = 0;
	virtual void Say(const std::string& line) const = 0;
	// Announce that `kind` has just taken hold. The two sides word the same
	// affliction differently — "Sera is poisoned!" against "The blob catches
	// fire!" — so each effect supplies BOTH lines (effects.cat apply_party /
	// apply_monster) and the target picks the one that fits its grammar.
	virtual void SayApplied(const EffectKind& kind) const = 0;
};

// One effect's shared half: what it IS, what it looks like, how it stacks.
class EffectKind {
public:
	EffectKind(std::string id, Category category, std::string nameKey,
			   Stacking stacking = Stacking::Refresh);
	virtual ~EffectKind() = default;

	// The loc key naming this effect in the HUD strip and the sheet's Effects
	// tab; the sheet appends ".desc" for the long form. Virtual because one
	// kind can wear several names — the four Sight spells share a kind and
	// name themselves by school. Returns a view into storage the kind owns
	// (never a temporary: the HUD calls this every frame).
	virtual std::string_view NameKey(const Inst& inst) const;

	// What a DoT's bite is RESISTED AS. Per instance, because a burn is only
	// fire when it was lit by fire — the same kind carries a frost weapon's
	// chill. The tint convention does NOT decide this: bleeding rides fire red
	// in the HUD but wounds you as pierce.
	virtual DamageType DamageTypeOf(const Inst& inst) const;

	// Lay the project's effects.cat numbers over the class defaults, matched
	// by id (EffectBook::Build calls this once per load). The base takes the
	// name/icon/stacking; a derived kind adds its own fields.
	//
	// The type book comes in here because this is the FIRST moment a kind can
	// resolve one: the classes are constructed by MakeAllEffects() before any
	// project is loaded, so a DoT knows only that it burns as "fire" — which
	// index that is depends on the project.
	virtual void ApplyOverrides(const CatalogEntry& e, const DamageTypeBook& types);

	// --- the pipeline hooks (docs/effects.md) --------------------------------
	// One per stage an effect can intervene at. All default to doing nothing,
	// so a kind implements only the stage it actually cares about — and which
	// stage that is IS the effect's design:
	//   windward   deflect   turns a bolt aside, spending a charge
	//   stoneskin  mitigate  contributes physical resist
	//   waterveil  absorb    eats damage from a pool, and bursts when spent
	//   fireshield react     scorches whoever landed the blow
	// An effect that dies by SPENDING (rather than timing out) says its own
	// line and zeroes timeLeft; Deal erases it before the next stage, so the
	// world's aging tick never also prints the generic fade line.

	// Stage 1 — may cancel the event outright, before any roll.
	virtual void OnDeflect(Inst& inst, DamageEvent& ev, ITarget& self) const;
	// Stage 3 — this effect's contribution to its bearer's resist for `type`.
	virtual float ResistFor(const Inst& inst, DamageType type,
							const Knobs& knobs) const;
	// Stage 4 — eat what you can of `remaining` (a pool that spends itself).
	virtual void OnAbsorb(Inst& inst, float& remaining, const DamageEvent& ev,
						  ITarget& self) const;
	// Stage 6 — the blow landed on the bearer; react to whoever struck.
	// `attacker` is null when nobody did (a wall, an unattributed tick), and
	// `ctx` is what a reprisal deals its own damage through.
	virtual void OnStruck(Inst& inst, const DamageEvent& ev, ITarget& self,
						  ITarget* attacker, const ReactCtx& ctx) const;

	// --- the modifier query (docs/effects.md decision 4) ---------------------
	// Shipped unused: nothing implements these yet, and no read site asks. A
	// slow, a weaken, a blessing plugs in here without every stat reader
	// having to learn about effects after the fact.
	virtual float StatBonus(const Inst& inst, std::string_view stat) const;
	virtual float SpeedScale(const Inst& inst) const;

	const std::string& Id() const { return m_id; }
	Category Kind() const { return m_category; }
	Stacking Stack() const { return m_stacking; }
	// The item id whose baked icon this effect borrows in the HUD strip
	// ("rune_protect"); empty = the school-tinted square fallback.
	const std::string& IconItem() const { return m_iconItem; }
	// Whether a bearer of this effect visibly BURNS: a flame plume rising off
	// the body plus its own coloured light. Presentation only — the host reads
	// it to decide what to draw, and the effect list stays the single truth of
	// what is actually happening (effects.cat `plume`).
	bool Plume() const { return m_plume; }
	// The lines announcing that this effect took hold, one per side (either may
	// be empty for an effect that arrives silently). Both take the bearer's
	// name. effects.cat apply_party / apply_monster.
	const std::string& ApplyLine(bool onMonster) const {
		return onMonster ? m_applyMonster : m_applyParty;
	}
	// The school an instance takes when its source doesn't name one — a
	// monster's poison has no element behind it, but still wants earth green.
	SpellSymbol DefaultSchool() const { return m_school; }

protected:
	// The book this kind resolved against, borrowed for the lifetime of the
	// load (DungeonWorld owns both, and the book is built first). It is what
	// lets a hook answer a question about a type — "is this physical?", "what
	// does fire deal?" — without every hook signature growing a parameter.
	const DamageTypeBook* m_types = nullptr;

	std::string m_id;
	std::string m_nameKey;
	std::string m_iconItem;
	std::string m_applyParty, m_applyMonster;
	std::string m_damageTypeId; // resolved into m_damageType at ApplyOverrides
	Category m_category;
	Stacking m_stacking;
	DamageType m_damageType{};
	SpellSymbol m_school = SpellSymbol::Fire;
	bool m_plume = false;
};

// One live effect on one combatant. A POD by design — no string, no owning
// pointer — so a list of them is cheap to hold, copy, and walk per frame.
struct Inst {
	const EffectKind* kind = nullptr;
	// School flavour: the HUD tint, and for the school-keyed kinds (Sight, the
	// wards) which flavour landed. Not every effect has a meaningful one — a
	// monster's poison rides earth green by the palette convention.
	SpellSymbol school = SpellSymbol::Fire;
	// The kind-keyed number: a DoT's damage per second, a ward's armor / pool /
	// charges. RAW — a resist is applied where the effect BITES, not here, so a
	// defense that changes mid-effect changes what the effect does
	// (docs/effects.md decision 1).
	float magnitude = 0.0f;
	float timeLeft = 0.0f; // seconds; the world tick removes at <= 0
	float duration = 0.0f; // starting timeLeft (the HUD's depletion fraction)
	// Who applied it: a roster index for a party source, -1 for none/unknown.
	// Threat credit for a DoT tick reads it (P3); nothing else yet.
	int source = -1;

	std::string_view Id() const { return kind ? std::string_view(kind->Id()) : std::string_view(); }
	std::string_view NameKey() const { return kind ? kind->NameKey(*this) : std::string_view(); }
	bool Is(std::string_view id) const { return kind && kind->Id() == id; }
	bool IsWard() const { return kind && kind->Kind() == Category::Ward; }
	bool IsDot() const { return kind && kind->Kind() == Category::Dot; }
};

// Land `kind` on an effect list, honouring the kind's stacking policy — THE
// one place the refresh rule lives (it used to be a RemoveWard/RemoveEffect
// call open-coded at each cast and proc site). Returns the landed instance.
Inst& Apply(std::vector<Inst>& effects, const EffectKind& kind,
			SpellSymbol school, float magnitude, float duration,
			int source = -1);

// What a list of effects adds to their bearer's resist for `type` — the
// mitigate stage's effect term. Both target adapters call it, so "effects can
// harden you" is written once rather than once per side.
float EffectResist(const std::vector<Inst>& effects, DamageType type,
				   const Knobs& knobs);

// Roll a landed blow's on-hit procs against `target` and land the ones that
// take, announcing each through the target. THE one place a proc becomes an
// effect — a monster's venom, an enchanted blade's fire, a serrated edge's
// bleed all arrive here.
//
// `school` flavours them when the source has an element to lend (a weapon's);
// without one each effect falls back to its own default. A target IMMUNE to
// what a DoT deals never catches it at all. `source` is the roster index to
// credit the ticks to, or -1.
void ApplyProcs(ITarget& target, std::span<const Proc> procs,
				std::optional<SpellSymbol> school, int source,
				const EffectBook& book, std::mt19937& rng);

// THE path damage takes: walk `ev` through stages 1-5 against `target` —
// deflect, strike, mitigate, absorb, apply. Everything is reported back in
// `ev` (deflected / hit / dealt / slew); the caller narrates from that.
//
// (Whoever DEALT it matters only to the react stage, which is React's job, so
// it is not a parameter here; the target's own adapter applies the knobs when
// it resolves a resist, so those are not either.)
void Deal(DamageEvent& ev, ITarget& target, const StrikeRules& rules,
		  std::mt19937& rng);

// Stage 6, split out so the CALLER can narrate first: a reaction writes its own
// line ("the blob is scorched by the fire shield") and that has to read after
// the blow it answers, not before it. Call it once the blow itself has been
// reported. Harmless when the target has no reacting effects, which is why
// every attack site calls it rather than only the ones that might matter.
void React(const DamageEvent& ev, ITarget& target, ITarget* attacker,
		   const ReactCtx& ctx);

// The registry: every kind, constructed at its class defaults and then tuned
// by the project's effects.cat. Built once per load, and every Inst in the
// world points into it — so it must outlive them (DungeonWorld owns it).
class EffectBook {
public:
	void Build(const Catalog& catalog, const DamageTypeBook& types);
	// The kind with this id, or null. Also resolves the LEGACY save tokens
	// ("ward" + school, "poison", "bleed", "sight") so an old save loads.
	const EffectKind* Find(std::string_view id) const;
	const EffectKind* FindLegacy(std::string_view token, SpellSymbol school) const;
	bool Empty() const { return m_kinds.empty(); }

private:
	std::vector<std::unique_ptr<EffectKind>> m_kinds;
};

// The kind table (Effect/AllEffects.cpp) — every class, at its defaults.
std::vector<std::unique_ptr<EffectKind>> MakeAllEffects();

} // namespace dungeon::game::fx
