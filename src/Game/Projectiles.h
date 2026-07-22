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
//   - onFizzle    : an item died on a wall / at max range (for a sound).
// So spawning "adds a moving item to the map" and that item later damages a
// monster (or the party) without this engine depending on the map or combat.
// ============================================================================
#pragma once

#include "Core/MathTypes.h"
#include "Game/Combat.h"
#include "Graphics/ParticleBatch.h"

#include <functional>
#include <random>
#include <vector>

namespace dungeon::game {

// Which side of the fight a moving item may strike. A party spell resolves
// against monsters; a monster's ranged attack resolves against the party.
enum class TargetSide { Party, Monsters };

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
};

// Everything the owner needs to resolve one impact: where it landed, the strike
// profile, the item's travel direction + push so displacement effects (the
// air school's shove) know which way and how far to move the target, and who
// launched it (threat attribution/preference — see ProjectileSpec).
struct ProjectileImpact {
	Vec3 pos{};
	Vec3 dir{};
	AttackProfile atk{};
	int push = 0;
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
	// An item died on a wall / at max range at `p` (for a soft fizzle sound).
	std::function<void(const Vec3& p)> onFizzle;

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

	std::vector<Item> m_items;
	std::vector<Spark> m_sparks;
	u32 m_nextId = 1; // monotonic runtime-id source (0 = "none")
	std::mt19937 m_rng{0x5EED1234u}; // spark scatter (cosmetic; not the combat RNG)
};

} // namespace dungeon::game
