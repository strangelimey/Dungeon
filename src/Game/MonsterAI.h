// ============================================================================
// Game/MonsterAI.h — the monster brain: thinking is separate from acting.
//
// This is the single home for monster AI, walled off from the rest of the game
// exactly like MagicSystem: the brain knows nothing about DungeonWorld, the
// Monster struct, the map, the party, the HUD, or audio. It reaches the world
// through one small read-only interface (ai::IWorldView) and is fed a flat
// snapshot of the monster (ai::Agent). So the AI can grow without touching world
// internals, and it can be unit-tested in isolation — no global state to set up.
//
// THINKING vs ACTING are deliberately split, because they happen at different
// rates:
//   * Thinking (Brain::Think) decides the monster's STANDING ORDERS — its
//     ai::Intent: "stay idle" or "engage the party, chasing toward this cell".
//     It is cheap and happens INFREQUENTLY, gated by the monster's IQ bucket
//     (ai::Scheduler). A dim monster re-thinks rarely, so it reacts slowly —
//     it keeps lumbering toward where it last saw the party.
//   * Acting is the host EXECUTING those orders EVERY frame at the monster's own
//     move/attack cadence (moveInterval/attackInterval) — gliding a step,
//     swinging when adjacent. Execution calls Brain::NextStep for the path.
// So a low-IQ monster can still move fast and attack relentlessly; only its
// CHANGE OF MIND lags. Brain::Think never paths and never mutates; the host
// owns all motion, timers, sounds, and messages.
// ============================================================================
#pragma once

#include <vector>

namespace dungeon::ai {

// ----------------------------------------------------------------------------
// The world seam. The brain needs only two read-only spatial questions to path;
// the host implements them over its real map/party/monster state. `selfId` is
// the host's opaque key for the deciding monster (its index in the monster
// list) so occupancy checks can skip the monster itself.
// ----------------------------------------------------------------------------
class IWorldView {
public:
	virtual ~IWorldView() = default;
	// A cell a monster other than `selfId` may step into: in bounds, map-walkable,
	// not the party cell, and not held by another LIVE monster.
	virtual bool CellFreeForMonster(int x, int z, int selfId) const = 0;
	// A cell is map-walkable. The goal cell qualifies even if currently occupied,
	// so the BFS can still target a monster's last-known party cell.
	virtual bool IsWalkable(int x, int z) const = 0;
};

// ----------------------------------------------------------------------------
// The monster as the brain sees it — a flat snapshot the host fills from a
// Monster + its (shared) MonsterKind. Decoupling it from the live struct is what
// keeps this module independent of DungeonWorld.h. Only the few fields thinking
// and pathing need: everything else (timers, glide, facing) the host owns.
// ----------------------------------------------------------------------------
struct Agent {
	int id = -1;             // host self-key (monster index), to skip self in occupancy
	int x = 0, z = 0;        // logical grid cell
	float aggroRange = 6.0f; // Chebyshev cells of party distance to engage at
};

// What the agent can perceive when it thinks: where the party is right now.
struct Perception {
	int partyX = 0, partyZ = 0; // the party's grid cell
};

// ----------------------------------------------------------------------------
// The monster's standing orders, decided infrequently by Brain::Think and then
// EXECUTED every frame by the host. Re-thinking refreshes these; between thinks
// the monster keeps carrying them out — a dim monster relentlessly chases a
// STALE target (the party cell as of its last think).
// ----------------------------------------------------------------------------
struct Intent {
	enum class Mode { Idle, Engage }; // Idle: hold position; Engage: chase + attack
	Mode mode = Mode::Idle;
	int targetX = 0, targetZ = 0; // chase goal: the party cell at last think time
};

// ----------------------------------------------------------------------------
// IQ-driven think-rate bucketing. A monster has no reason to re-decide "I'll
// attack" 60 times a second — so monsters are sorted into THINK buckets and a
// bucket only re-thinks every so often. How often is set by the monster's IQ:
// smart monsters land in a fast bucket (think often, react crisply), dim ones in
// a slow bucket (think rarely, react sluggishly). Acting is NOT bucketed — only
// Brain::Think is, so movement/attack speed stays governed by the monster's own
// cooldowns, independent of how cleverly it re-plans.
//
// Bucket 0 is the smartest/fastest (kFastestHz); each step down halves the rate.
// The Scheduler owns one accumulator per bucket and, each frame, reports which
// buckets are due (a bitmask). The host maps each monster's IQ to a bucket and
// re-thinks only when that bucket's bit is set.
// ----------------------------------------------------------------------------
class Scheduler {
public:
	static constexpr int kBucketCount = 4;
	static constexpr float kFastestHz = 4.0f; // bucket 0 re-thinks 4x/second

	Scheduler();

	// Advance every bucket's timer by dt and return a bitmask (bit b = bucket b)
	// of the buckets that should re-think this frame.
	unsigned Tick(float dt);

	// Which bucket a monster with this IQ belongs to (0 = fastest). Pure mapping,
	// so the host can call it per monster every frame without storing the bucket.
	static int BucketForIq(float iq);

	// Seconds between re-thinks for bucket b (kFastestHz at b=0, halving down).
	static float BucketInterval(int b);

private:
	float m_acc[kBucketCount]; // per-bucket elapsed-time accumulator
};

// ----------------------------------------------------------------------------
// One monster's AI. Stateless between calls except the BFS pathfinding scratch
// it owns, so a chase step costs no per-call allocation. A single Brain serves
// every monster (the scratch is reused, not per-monster state).
// ----------------------------------------------------------------------------
class Brain {
public:
	// THINK (IQ-gated, infrequent, cheap): decide the monster's standing orders
	// from what it perceives. No pathfinding, no mutation — just intent.
	Intent Think(const Agent& a, const Perception& p);

	// ACT helper (per-frame, at the monster's move cadence): the next grid step
	// from the agent toward (targetX,targetZ) via 4-connected BFS over walkable,
	// monster-free cells. Writes the next cell to outX/outZ; returns false when
	// already at the target or no path exists. The goal cell is reachable even if
	// occupied (so a monster can still path to the party's last-known cell).
	bool NextStep(const Agent& a, int targetX, int targetZ, int mapW, int mapH,
				  const IWorldView& world, int& outX, int& outZ);

private:
	// BFS scratch reused across calls (sized to the map): the predecessor cell
	// index per cell, -1 = unvisited. Avoids a per-step allocation.
	std::vector<int> m_pathFrom;
};

} // namespace dungeon::ai
