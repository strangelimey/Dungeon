// ============================================================================
// Game/Spell/Spell.h — the spell base class: THE home of spell behaviour.
//
// Every castable spell is a concrete class in this folder (one file pair per
// spell) deriving from Spell — directly or through the shared-form bases
// (BoltSpell, WardSpell). The base carries what every spell has: the catalog
// id, the display-name / description loc keys, the SYMBOL RECIPE (whose first
// rune IS the school, per the school rule), the mana cost, and the base
// power. What a cast DOES lives in the pure virtual Cast() override — plain
// C++, deliberately (see the content-stays-data-driven decision: typed data +
// C++ behaviour, no scripting), but ISOLATED here so each spell's effect has
// exactly one home. Numbers stay tunable without a rebuild: the project's
// spells.cat entries are numeric OVERRIDES laid over the class defaults
// (ApplyOverrides), applied by SpellBook::Build.
//
// Spells reach the world only through CastServices — a capability surface the
// host (DungeonWorld) wires once, like the MagicSystem's projectile hooks —
// so the module stays walled off from map/monsters/HUD.
// ============================================================================
#pragma once

#include "Game/Projectiles.h"
#include "Game/Spells.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dungeon::game {

struct Character;
struct ItemSlot;
struct CatalogEntry;

// Reads an entry's area-blast fields (blast_force / _damage / _falloff / _rate /
// _linger / _persist / _color) over `spec`, keeping what it does not author. A
// spell's overrides and a thrown item's own blast (a gas flask) both read it.
struct BlastSpec;
void ReadBlastRules(const CatalogEntry& e, BlastSpec& spec);

// What burns, or could, where the party is looking: the wall torch on the
// wall it faces from its own square, else the brazier in the square ahead
// (braziers block the way, so one is never IN the party's square). A spell
// asks this rather than learning what a fixture is.
struct FireAhead {
	enum class Kind : u8 { None, WallTorch, Brazier };
	Kind kind = Kind::None;
	bool lit = false;
	// False for a kind that can never hold a flame (fixtures.cat `flame = 0`,
	// the empty brazier): a puff of flame finds nothing to catch.
	bool canBurn = false;
	// A wall torch whose torch was taken: only the bare bracket is there.
	bool empty = false;
};

// What a Cast() may DO beyond touching the caster — wired by the host once
// (DungeonWorld ctor) and handed to every cast. A Cast() that only reads or
// edits the caster and the party (wards, held items: CastContext::party) needs
// none of the world ones.
struct CastServices {
	// Spawn a bolt into the moving-item engine ("onto the map").
	std::function<void(const ProjectileSpec&)> spawnBolt;
	// The same, `delay` seconds from now (a volley's later bolts). The host
	// holds it in a fixed queue - a cast lands in a guarded frame.
	std::function<void(const ProjectileSpec&, float delay)> spawnBoltAfter;
	// A log line ABOUT a member, tinted with their identity color. Borrows the
	// line, like every sink on the message path (docs/message-allocation.md).
	std::function<void(const Character&, std::string_view)> message;
	// Land a status effect on a character (docs/effects.md): the id names the
	// KIND, and the host resolves it through the registry and applies that
	// kind's stacking rule. A spell that leaves something behind — a ward, the
	// Sight mark — calls this instead of touching the effect list, so it knows
	// neither the effect classes nor the stacking policy.
	std::function<void(Character&, std::string_view id, SpellSymbol school,
					   float magnitude, float duration)>
		applyEffect;

	// --- the world in front of the party (the hand spells) --------------------
	// The fire the party faces (FireAhead above).
	std::function<FireAhead()> fireAhead;
	// Lights it (true) or puts it out (false): its light, flame and haze, and -
	// going out - the smoke it leaves. True if it changed.
	std::function<bool(bool burning)> setFireAhead;
	// Fans it: a burning fire flares up for a moment. True if one did.
	std::function<bool()> flareAhead;
	// Lands `itemId` on the floor of the party's square - where a conjured item
	// goes when both of the caster's hands are full.
	std::function<void(std::string_view itemId)> dropAtFeet;
	// What a hand spell does to a HELD item, by the item's own catalog fields
	// (a spell never learns what an item kind is): light it (`lit_as`, an
	// unlit torch) or fill it a level (`fill_as`, an empty or half waterskin).
	// Each rewrites the slot in place and returns the name key of what it now
	// holds, or empty when the item does not take it.
	std::function<std::string_view(ItemSlot&)> lightItem;
	std::function<std::string_view(ItemSlot&)> fillItem;
	// True for a held item a spell's fire cannot light: a MAGICAL torch (its
	// lit kind has a `power_level`), lit only by its own Light command.
	std::function<bool(const ItemSlot&)> refusesFlame;
	// Shoves whatever monster stands in the square ahead `cells` squares further
	// away, along the party's facing (stopped early by a wall, a shut door or a
	// packed square). True if anything moved.
	std::function<bool(int cells)> shoveAhead;
	// A gust of `power` against every projectile flying AT the party in its own
	// square or the one ahead (ProjectileSystem::Repel): it weakens a stronger
	// shot and flings back one it outweighs, which then flies as the caster's
	// (credited to `casterIndex`, aimed at monsters).
	std::function<ProjectileSystem::Repelled(float power, int casterIndex)> repelAhead;
	// A blast of `payload` (its BlastSpec) as `school`'s damage, centred on the
	// party's square and spreading from it, the square itself untouched.
	std::function<void(const ProjectilePayload& payload, SpellSymbol school,
					   int casterIndex)>
		blastAroundParty;
};

// Everything a single cast knows: who, from where, at what strength. The
// gates (vocabulary, mana, the skill/fumble roll) have already passed by the
// time a Cast() sees this.
struct CastContext {
	Character& caster;
	Vec3 origin;      // the party eye — where a bolt is born
	Vec3 dir;         // the faced cardinal — where it flies
	float power;      // the spell's base power scaled by school skill
	int schoolLevel;  // the caster's level in the spell's school
	CastServices& services;
	// The caster's roster index (-1 when unknown) — rides a spawned bolt so
	// the impact can credit the hit to its caster (the threat system).
	int casterIndex = -1;
	// The caster's opposed-roll bonus in d100 POINTS, already assembled from
	// the school skill and stat curves by the host. Handed in rather than
	// computed here so a spell never learns what a Balance is. (New fields go
	// at the END, below this one: the one construction site initialises this
	// aggregate positionally, so a field inserted mid-struct silently
	// mis-assigns.)
	float attackBonus = 50.0f;
	// The hand the cast came from (0 / 1), or -1 when it came from no hand (the
	// spellbook, the console). A spell that acts on "the other hand" or puts
	// something IN a hand reads it; a hand-less cast picks a sensible hand.
	int hand = -1;
	// The whole roster, the caster among it - what a spell for the whole party
	// (a ward cast with Ingwaz) lands on. Empty when the host has none.
	std::span<Character> party{};
};

class Spell {
public:
	// `sequence` must obey the school rule (exactly one school rune, first) —
	// SpellBook::Build asserts it, so a malformed recipe fails loudly in the
	// registry, not silently at match time.
	Spell(std::string id, std::vector<SpellSymbol> sequence, float power,
		  float mana);
	virtual ~Spell() = default;

	// What happens when the cast lands — THE isolated hard-coding. Each
	// concrete spell's effect lives in its override (a bolt spawns, a ward
	// settles, a future wall rises...).
	virtual void Cast(CastContext& ctx) const = 0;

	// The spell thrown by a MONSTER caster (monsters.cat `spell = <id>`):
	// its bolt aimed at the party at the monster's accuracy, or nullopt for
	// a spell with no thrown form (a ward) — the caller falls back to its
	// plain shot. Monsters cast at base power (no school skill).
	virtual std::optional<ProjectileSpec> MonsterBolt(const Vec3& origin,
													  const Vec3& dir,
													  float accuracy) const;
	// How many of that bolt a monster caster throws per cast, a beat apart (a
	// volley with Ingwaz; one otherwise).
	virtual int MonsterVolley() const { return 1; }

	// Lay the project's spells.cat NUMBERS over the class defaults (matching
	// entry by id; SpellBook::Build calls this once per load). The base takes
	// name/power/mana/on_hit; derived classes add their own fields. The RECIPE is
	// class-only — identity never comes from data.
	virtual void ApplyOverrides(const CatalogEntry& e);

	// The payload a cast hands its carrier: this spell's on-hit effects and blast,
	// PACKED ONCE AT LOAD (ApplyOverrides) and copied from there. A cast happens
	// in a frame the steady-state allocation guard watches, and packing per shot
	// built a "spells.cat [<id>]" warning label every time whether or not it
	// warned. Flavour is left unset; the carrier decides it.
	const ProjectilePayload& MakePayload() const { return m_payload; }

	// The damage-type book this spell resolved against (SpellBook::Build), so a
	// bolt can ask what its school deals. Borrowed; DungeonWorld owns it and it
	// outlives the registry.
	void SetTypes(const DamageTypeBook* types) { m_types = types; }

	const std::string& Id() const { return m_id; }
	const std::string& NameKey() const { return m_nameKey; } // "spell.<id>"
	const std::string& DescKey() const { return m_descKey; } // "spell.<id>.desc"
	std::span<const SpellSymbol> Sequence() const { return m_sequence; }
	// The school rule makes the first rune the spell's school (and colour).
	SpellSymbol School() const { return m_sequence.front(); }
	// The skill roll's difficulty: the recipe's rune count (docs/skills.md).
	int Difficulty() const { return static_cast<int>(m_sequence.size()); }
	float Power() const { return m_power; }
	float Mana() const { return m_mana; }
	// What this spell BURSTS as, and what it leaves behind — for the eval
	// harness's `blast` command, which detonates a spell's AUTHORED numbers at a
	// chosen cell. Exposed rather than letting the harness invent its own rules:
	// a geometry measurement is only worth something if it describes the content
	// that actually ships (docs/eval-harness.md).
	const BlastSpec& Blast() const { return m_payload.blast; }
	std::span<const fx::Proc> Procs() const { return m_payload.Procs(); }
	// What its bolt lights and sheds in flight (spells.cat `light` / `trail`,
	// lighting-updates Phase 4): a lights.cat / trails.cat id, empty = the
	// school's default (`bolt_<school>`, `trail_<school>`), which the world picks.
	const std::string& LightId() const { return m_light; }
	const std::string& TrailId() const { return m_trail; }

protected:
	// Lays this spell's own `light` / `trail`, where it names them, over a bolt
	// another spell built (a modifier wrapping a bolt spell).
	void LendLook(ProjectileSpec& bolt) const {
		if (!m_light.empty()) bolt.lightId = m_light;
		if (!m_trail.empty()) bolt.trailId = m_trail;
	}
	const DamageTypeBook* m_types = nullptr;

	std::string m_id;
	std::string m_nameKey;
	std::string m_descKey;
	std::vector<SpellSymbol> m_sequence;
	float m_power;
	float m_mana;
	// What a landed cast leaves behind (spells.cat `on_hit`, packed into the
	// carrier's inline array at load - empty for most spells, a ward's business
	// is the ward it applies) and the area burst it sets off, if any
	// (`blast_force` and friends; zero force for a plain single-target bolt).
	ProjectilePayload m_payload;
	std::string m_light; // spells.cat `light` (see LightId)
	std::string m_trail; // spells.cat `trail` (see TrailId)
};

// Every concrete spell, freshly constructed at class defaults — the registry
// SpellBook::Build starts from (AllSpells.cpp includes each spell's header;
// adding a spell = its file pair + one line there + a CMakeLists entry).
std::vector<std::unique_ptr<Spell>> MakeAllSpells();

} // namespace dungeon::game
