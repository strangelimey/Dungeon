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
#include "Game/Spells.h" // SpellSymbol (an effect's school: tint + flavour)

#include <memory>
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

	// Lay the project's effects.cat numbers over the class defaults, matched
	// by id (EffectBook::Build calls this once per load). The base takes the
	// name/icon/stacking; a derived kind adds its own fields.
	virtual void ApplyOverrides(const CatalogEntry& e);

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

protected:
	std::string m_id;
	std::string m_nameKey;
	std::string m_iconItem;
	Category m_category;
	Stacking m_stacking;
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

// The registry: every kind, constructed at its class defaults and then tuned
// by the project's effects.cat. Built once per load, and every Inst in the
// world points into it — so it must outlive them (DungeonWorld owns it).
class EffectBook {
public:
	void Build(const Catalog& catalog);
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
