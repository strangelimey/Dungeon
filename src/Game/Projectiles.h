// ============================================================================
// Game/Projectiles.h — the shared moving-item engine: flies projectiles, resolves
// their impacts, and draws them.
//
// This is the ONE runtime home for anything that flies through the dungeon and
// strikes on contact: a cast spell bolt today, a monster's ranged attack next,
// thrown items / traps later. A projectile is a generic MOVING ITEM whose
// properties (speed, range, visual, and a TARGET SIDE — who it may strike)
// determine how it moves and what it hits. Callers describe one with a
// ProjectileSpec and Spawn() it; the engine owns the live items + their impact
// sparks (purely transient — never saved) and draws them as additive billboards.
//
// Like MagicSystem, this engine deliberately knows nothing about the dungeon map,
// the monster list, the party, the HUD log, or audio. It reaches those modules
// through a small set of hooks the owner (DungeonWorld) wires up once:
//   - isBlocked   : does this world position stop an item (a wall / off-map)?
//   - resolveHit  : an item reached here — resolve a strike against whatever on
//                   its TARGET SIDE lives at it (combat + feedback); did it hit?
//   - onExpire    : an item died on a wall / at max range WITHOUT striking —
//                   the owner decides what that means (see ProjectileExpiry).
// So spawning "adds a moving item to the map" and that item later damages a
// monster (or the party) without this engine depending on the map or combat.
//
// A carrier delivers a PAYLOAD at one of two moments — it strikes something, or
// it expires. This engine never interprets a payload; it carries it and hands it
// back through the hooks, exactly as it already does with the AttackProfile.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Game/Blast.h"
#include "Game/Combat.h"
#include "Game/Effect/Effect.h"
#include "Graphics/ParticleBatch.h"

#include <array>
#include <functional>
#include <optional>
#include <random>
#include <span>
#include <string_view>
#include <vector>

namespace dungeon::game {

// Which side of the fight a moving item may strike. A party spell resolves
// against monsters; a monster's ranged attack resolves against the party.
enum class TargetSide { Party, Monsters };

// The most effects one carrier delivers. INLINE (a fixed array, not a vector)
// for two reasons, both worth keeping:
//   - a spawn happens mid-fight and an Item lives in a per-frame-simulated
//     vector, so a heap allocation per shot would violate the steady-state rule
//     (docs/ARCHITECTURE.md "Memory strategy" — the alloc guard asserts on it);
//   - the list is COPIED rather than borrowed from the spell that fired it, so a
//     bolt still in flight survives an editor catalog rebuild reseating the
//     registry underneath it.
// Effect ids are short enough to sit in std::string's small-buffer, so a
// realistic payload really does allocate nothing.
inline constexpr size_t kMaxPayloadProcs = 4;

// What a carrier DELIVERS, at either of its two moments. The effects are
// fx::Procs — the same "<id> <magnitude> <seconds> [chance]" list weapons and
// monsters already author as `on_hit`, so a bolt names an effect exactly the way
// a serrated blade does and the engine learns no new vocabulary.
//
// Michael's framing (2026-08-11): "when it hits something OR EXPIRES, it causes
// an effect on the target" — one payload, two moments. What differs is WHO is
// caught, and that is the host's rule, not this engine's (a hit is lane-wide, an
// expiry is cell-wide — see DungeonWorld::ResolveProjectileExpiry).
// An AREA burst the carrier sets off where it stops (Game/Blast.h). `force` is
// the blast's size in SQUARES and 0 — the default — means the carrier is not an
// area effect at all, which is what every bolt authored before this was.
//
// A BLAST IGNORES TargetSide: it catches EVERYONE in its squares, the party
// included (Michael, 2026-08-11). That is deliberately unlike the single-target
// paths, where the side is the whole rule — an explosion does not check whose
// side you are on, and positioning is the price of throwing one.
struct BlastSpec {
	blast::Rules rules; // Game/Blast.h — force, falloff, expansion rate, persistence
	bool Any() const { return rules.Any(); }
};

struct ProjectilePayload {
	std::array<fx::Proc, kMaxPayloadProcs> procs{};
	size_t count = 0;
	BlastSpec blast{};
	// The element these effects arrive in, when the source has one to lend — a
	// firebolt's burn is fire. Unset lets each effect keep its own colours, which
	// is what a monster's plain shot does (a creature lends no element, exactly as
	// its melee doesn't). The same rule as an enchanted weapon's `element`.
	std::optional<SpellSymbol> flavour;

	bool Empty() const { return count == 0; }
	std::span<const fx::Proc> Procs() const { return {procs.data(), count}; }
	// Appends a proc if there is room; false when the payload is already full, so
	// the FILLING site can warn about the content it had to drop (this engine has
	// no log).
	bool Add(const fx::Proc& p) {
		if (count >= procs.size()) return false;
		procs[count++] = p;
		return true;
	}
};

// Pack an authored proc list into a carrier's payload. THE one place the inline
// capacity is enforced, so every kind of carrier reports dropped content the same
// way; `where` names the catalog entry in that warning.
ProjectilePayload PackPayload(std::span<const fx::Proc> procs,
							  std::string_view where);

// Why an item's flight ended without striking anything. The host reads it to
// decide what an expiry means: a bolt that burst against stone is a different
// event from one that simply ran out of reach in open air.
enum class ExpiryCause {
	Wall,  // stopped by geometry — a wall, or a closed door
	Range, // ran out of reach in open air
};

// A request to launch one moving item. The caller fills this and hands it to
// ProjectileSystem::Spawn; the engine copies out what it needs.
struct ProjectileSpec {
	Vec3 pos{};              // launch position (world)
	Vec3 dir{};              // unit travel direction (horizontal)
	float speed = 7.0f;      // m/s
	float range = 8.0f;      // metres before it fizzles in open air
	AttackProfile atk{};     // damage + accuracy applied on a hit
	Vec4 color{1, 1, 1, 1};  // glow (premultiplied additive)
	float size = 0.2f;       // billboard half-extent
	TargetSide target = TargetSide::Monsters;
	int push = 0;            // cells the struck target is shoved along `dir`
	// Who launched it, for the threat system (Monster::threat). At most one is
	// set: a party bolt carries its caster's roster index (threat accrual on
	// impact), a monster bolt its shooter's runtimeId (the impact reads the
	// shooter's threat table to prefer its target in the lane).
	int attacker = -1;       // party roster index, -1 = not a party shot
	u32 shooter = 0;         // monster runtimeId, 0 = not a monster shot
	ProjectilePayload payload{}; // what it leaves behind, on a hit or on expiry
};

// Everything the owner needs to resolve one impact: where it landed, the strike
// profile, the item's travel direction + push so displacement effects (the
// air school's shove) know which way and how far to move the target, who
// launched it (threat attribution/preference — see ProjectileSpec), and the
// payload the landed blow leaves on whatever it struck.
struct ProjectileImpact {
	Vec3 pos{};
	Vec3 dir{};
	AttackProfile atk{};
	int push = 0;
	int attacker = -1; // party roster index, -1 = not a party shot
	u32 shooter = 0;   // monster runtimeId, 0 = not a monster shot
	ProjectilePayload payload{};
};

// An item's flight ended without striking anything. Everything the owner needs
// to decide what that means: where and why it died, the payload it was carrying,
// which side it was flying against, and who launched it (an expiry's effects are
// credited like a hit's).
//
// This REPLACED a hook that passed only a position, for a sound. An expiry
// telling nobody anything was the hole P7 exists to close: a carrier is defined
// by causing something when it stops, and half of "when it stops" was dead.
struct ProjectileExpiry {
	Vec3 pos{};
	Vec3 dir{};
	ExpiryCause cause = ExpiryCause::Range;
	TargetSide target = TargetSide::Monsters;
	AttackProfile atk{}; // for the damage TYPE an expiry's effects arrive as
	ProjectilePayload payload{};
	int attacker = -1; // party roster index, -1 = not a party shot
	u32 shooter = 0;   // monster runtimeId, 0 = not a monster shot
};

// A read-only snapshot of one live item, for the editor's map marker + inspect
// dialog (projectiles are transient content the builder may want to freeze and
// examine). Keyed by a stable per-item runtime id, like monsters.
struct ProjectileInfo {
	u32 id = 0;
	Vec3 pos{};
	Vec3 dir{};
	float speed = 0.0f;
	float rangeLeft = 0.0f;
	AttackProfile atk{};
	TargetSide target = TargetSide::Monsters;
	ProjectilePayload payload{}; // what it will leave behind (inspector reads it)
};

class ProjectileSystem {
public:
	// Launches a moving item described by `spec` (adds it "to the map").
	void Spawn(const ProjectileSpec& spec);

	// Advances live items (flight + impact/fizzle via the hooks) and ages the
	// impact sparks. Call once per frame.
	void Update(float dt);

	// Appends the live item + spark billboards (premultiplied additive) to the
	// particle list the renderer draws after the opaque scene.
	void AppendBillboards(std::vector<gfx::ParticleInstance>& out) const;

	// Drops all live items + sparks (new game / level change).
	void Clear() {
		m_items.clear();
		m_sparks.clear();
	}

	// --- editor introspection (transient content, shown on the map) ----------
	// A snapshot of every live item, for the editor map markers.
	std::vector<ProjectileInfo> Live() const;
	// One live item by its stable runtime id (false if it already landed/died).
	bool Find(u32 id, ProjectileInfo& out) const;
	// Removes a live item by id (the inspector's "dismiss" action). False if gone.
	bool Remove(u32 id);

	// --- world seam (wired once by the owner) -------------------------------
	// True if an item flying `dir` is stopped by the cell at world position `p`
	// (a wall / off-map — but a bore along the travel axis lets it fly through).
	std::function<bool(const Vec3& p, const Vec3& dir)> isBlocked;
	// An item reached `impact.pos`; resolve a strike there on `side`. Return true
	// if it struck a target (the item is consumed). The owner does combat + feedback.
	std::function<bool(TargetSide side, const ProjectileImpact& impact)> resolveHit;
	// An item's flight ended without striking anything (a wall, or out of reach).
	// The owner decides what that means — the fizzle sound, and whatever the
	// payload does to the cell it died in.
	std::function<void(const ProjectileExpiry& expiry)> onExpire;

private:
	// A live moving item in flight. Flies its direction at `speed`, carries the
	// strike profile applied on a hit against its `target` side, and draws as a
	// glowing billboard. Transient: never saved.
	struct Item {
		u32 id = 0;             // stable runtime id (editor inspect); never reused
		Vec3 pos{};
		Vec3 dir{};             // unit travel direction (horizontal)
		float speed = 7.0f;     // m/s
		float rangeLeft = 8.0f; // metres remaining before it fizzles
		AttackProfile atk{};    // damage + accuracy applied on a hit
		Vec4 color{1, 1, 1, 1}; // glow (premultiplied additive)
		float size = 0.2f;      // billboard half-extent
		TargetSide target = TargetSide::Monsters;
		int push = 0;           // cells the struck target is shoved along `dir`
		int attacker = -1;      // party roster index (threat; see ProjectileSpec)
		u32 shooter = 0;        // monster runtimeId (threat; see ProjectileSpec)
		ProjectilePayload payload{}; // delivered on a hit, or on expiry
	};
	// A short-lived impact/fizzle spark (a burst of these sells a hit). Flies out,
	// fades over its life, additive.
	struct Spark {
		Vec3 pos{};
		Vec3 vel{};
		Vec4 color{1, 1, 1, 1};
		float age = 0.0f;
		float life = 0.35f;
		float size = 0.1f;
	};

	void SpawnSparkBurst(const Vec3& pos, const Vec4& color, int count);
	// Report a flight that ended without a strike, through onExpire.
	void Expire(const Item& it, ExpiryCause cause);

	std::vector<Item> m_items;
	std::vector<Spark> m_sparks;
	u32 m_nextId = 1; // monotonic runtime-id source (0 = "none")
	std::mt19937 m_rng{0x5EED1234u}; // spark scatter (cosmetic; not the combat RNG)
};

} // namespace dungeon::game
